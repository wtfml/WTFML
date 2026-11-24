#include "manage/ManualRecovery.hpp"
#include <QCoreApplication>

#include "manage/InstanceFiles.hpp"
#include "manage/RecoveryReview.hpp"
#include "manage/RestoreJournal.hpp"
#include "util/DirectorySessionLock.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QObject>
#include <QSaveFile>
#include <QUuid>

namespace ManualRecovery {
namespace {

/// 事务记录**原样**写回（改写失败、或者失败时要撤回这一趟写下的记录）。
/// 和 `RestoreJournal::write()` 分开：那一口收的是对象、写的是紧凑 JSON；这里收回来的
/// 是盘上原来那串字节，多解一次再序列化一次等于拿"我们认识的字段"去覆盖"别人的记录"。
bool writeRawJournal(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QByteArray readJournalBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        return {};
    }
    return file.readAll();
}

}  // namespace

Result swapDirectories(const Request &request) {
    Result result;
    const auto refuse = [&result](const QString &text) {
        result.ok = false;
        result.problem = text;
        return result;
    };

    // ---------- ① 判路径：都在这个游戏文件夹里、同一个父目录、路径上没有链接 ----------
    const QString root = InstanceFiles::canonicalPath(request.gameRoot);
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return refuse(QObject::tr("无法识别游戏目录，未替换任何文件。"));
    }
    const QString target = InstanceFiles::canonicalPath(request.targetDir);
    const QString replacement = InstanceFiles::canonicalPath(request.replacementDir);
    if (target.isEmpty() || replacement.isEmpty()
        || !InstanceFiles::pathIsSameOrInside(root, target)
        || !InstanceFiles::pathIsSameOrInside(root, replacement)) {
        return refuse(QObject::tr("两个目录必须都在这一个游戏目录里面，一个目录都没动。"));
    }
    if (target == root || replacement == root || target == replacement) {
        return refuse(QObject::tr("无法交换这两个目录，原目录已保留。"));
    }
    // `.wtfml-` 开头的是我们自己的暂存目录；`cache/` 底下是记录与暂存，都不该被拿来
    // 当目标。旧目录那一侧**允许** `.wtfml-restore-previous-` —— 那正是还原事务留下的
    // 保留目录，也就是这里"旧目录"通常指的那一份；别的 `.wtfml-` 名字一概不认。
    const QString cacheDir = QDir(root).filePath(QStringLiteral("cache"));
    const QString replacementName = QFileInfo(replacement).fileName();
    const bool isRetainedName =
        replacementName.startsWith(QStringLiteral(".wtfml-restore-previous-"))
        && !QUuid(replacementName.mid(QStringLiteral(".wtfml-restore-previous-").size())).isNull();
    if (QFileInfo(target).fileName().startsWith(QStringLiteral(".wtfml-"))
        || (replacementName.startsWith(QStringLiteral(".wtfml-")) && !isRetainedName)
        || InstanceFiles::pathIsSameOrInside(cacheDir, target)
        || InstanceFiles::pathIsSameOrInside(cacheDir, replacement)) {
        return refuse(QObject::tr("所选目录中有启动器的临时文件或操作记录，已取消交换。原有目录保留。"));
    }
    // 事务记录（`RestoreJournal::valid()`）与启动恢复都按"同父目录的两段改名"判；
    // 跨父目录、跨卷的改名本来也做不到，这里提前说清楚。
    if (QFileInfo(target).absolutePath() != QFileInfo(replacement).absolutePath()) {
        return refuse(QObject::tr("两个目录不在同一个父目录下，无法通过重命名交换。原目录已保留。"));
    }
    if (!RestoreJournal::ordinaryPath(root, target)
        || !RestoreJournal::ordinaryPath(root, replacement)) {
        return refuse(QObject::tr("路径上有链接或无法确认的层级，未换任何目录。"));
    }
    if (QFileInfo(target).exists() && !QFileInfo(target).isDir()) {
        return refuse(QObject::tr("「%1」不是一个目录，未换任何目录。")
                          .arg(QDir::toNativeSeparators(target)));
    }
    if (!QFileInfo(replacement).isDir()) {
        return refuse(QObject::tr("找不到所选旧目录：%1")
                          .arg(QDir::toNativeSeparators(replacement)));
    }

    // ---------- ② 运行状态：调用方填的那一位 + 数据目录会话锁 ----------
    if (request.inUse) {
        return refuse(QObject::tr("这一份正在运行，或者正被复制 / 备份 / 还原 / 安装占用。等它停下来再来换，一个目录都没动。"));
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(root, &lockProblem)) {
        return refuse(lockProblem);
    }

    // ---------- ③ 事务记录：同一个目标只留一条 ----------
    const QString journalDir = RestoreJournal::directory(root);
    if (!RestoreJournal::ordinaryPath(root, journalDir) || !QDir().mkpath(journalDir)) {
        return refuse(QObject::tr("事务记录目录不可用，未换任何目录。"));
    }
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString journalPath;
    bool reusingRecord = false;
    int recordsForTarget = 0;
    const auto names = QDir(journalDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &name : names.mid(0, 128)) {
        const QString path = QDir(journalDir).filePath(name);
        if (QUuid(QFileInfo(name).completeBaseName()).isNull()) {
            continue;
        }
        const RecoveryReview::Item item = RecoveryReview::read(root, path);
        // 读不懂 / 越界的记录：启动恢复同样一个字节都不动，不会和这一趟抢同一个目标。
        if (!item.problem.isEmpty()) {
            continue;
        }
        const QString itemTarget = InstanceFiles::canonicalPath(item.target);
        const QString itemRetained = InstanceFiles::canonicalPath(item.retained);
        if (itemTarget != target) {
            // 另一条记录把这一份旧目录当成自己的 `retained`：换走它会让那一条在启动时
            // 变成"目标和旧目录均缺失"。先让人把那条处理掉。
            if (itemRetained == replacement) {
                return refuse(QObject::tr("旧目录已用于另一条恢复记录（目标为 %1）。\n请先完成该恢复操作，再替换目录。")
                                  .arg(QDir::toNativeSeparators(itemTarget)));
            }
            continue;
        }
        ++recordsForTarget;
        if (itemRetained != replacement) {
            return refuse(QObject::tr("此目录已有一条未处理的恢复记录。记录中的旧目录为：\n%1\n\n请先处理该记录或将其标记为已检查，再替换目录。")
                              .arg(QDir::toNativeSeparators(item.retained)));
        }
        journalPath = path;
        reusingRecord = true;
    }
    if (recordsForTarget > 1) {
        return refuse(QObject::tr("同一目录有多条恢复记录。请在「中断恢复检查」中逐条核对，再确定恢复顺序。原有目录保留。"));
    }
    if (journalPath.isEmpty()) {
        journalPath = QDir(journalDir).filePath(token + QStringLiteral(".json"));
    }
    const QString journalLockPath = journalPath + QStringLiteral(".lock");
    if (!RestoreJournal::ordinaryPath(root, journalPath)
        || !RestoreJournal::ordinaryPath(root, journalLockPath)) {
        return refuse(QObject::tr("事务记录的位置不可安全访问，未换任何目录。"));
    }
    QLockFile transactionLock(journalLockPath);
    transactionLock.setStaleLockTime(0);
    if (!transactionLock.tryLock(0)) {
        return refuse(QObject::tr("这一条恢复记录正被另一项操作使用，未换任何目录。"));
    }
    // 改写别人的记录时，先留一份原样字节：这一趟在动盘之前就失败的话，要能原样写回去。
    const QByteArray previousJournal = readJournalBytes(journalPath);
    if (reusingRecord && previousJournal.isEmpty()) {
        return refuse(QObject::tr("无法读取恢复记录，未替换目录。"));
    }    const auto undoJournal = [&journalPath, &previousJournal] {
        if (previousJournal.isEmpty()) {
            QFile::remove(journalPath);
        } else {
            writeRawJournal(journalPath, previousJournal);
        }
    };

    const bool hadTarget = QFileInfo(target).isDir();
    const QString retained = QDir(QFileInfo(target).absolutePath())
                                 .filePath(QStringLiteral(".wtfml-restore-previous-") + token);
    auto journal = RestoreJournal::record(target, retained, hadTarget);
    if (!RestoreJournal::valid(root, journal) || !RestoreJournal::write(journalPath, journal)) {
        undoJournal();
        return refuse(QObject::tr("无法安全地写入下一次的恢复记录，未替换任何目录。"));
    }
    result.journalPath = journalPath;

    // 记录落盘之后再确认一次现场：这中间如果有别的程序删了 / 换了两份目录，宁可什么都不做。
    if (!QFileInfo(replacement).isDir() || QFileInfo(target).exists() != hadTarget
        || QFileInfo(target).isDir() != hadTarget) {
        undoJournal();
        return refuse(QObject::tr("动盘之前目录发生了变化，未换任何目录。"));
    }

    // ---------- ④ 两段改名（这一段不接受取消，也不做任何删除） ----------
    // 第一段：现在这一份挪到旁边。失败 ⇒ 盘上还是原来那样，把记录撤回。
    if (hadTarget && !QDir().rename(target, retained)) {
        undoJournal();
        return refuse(QObject::tr("当前目录无法移动（可能正被其他程序占用），没有改动任何文件。"));
    }
    // 第二段：旧目录换上来。失败 ⇒ 把第一段倒回去；倒不回去就两份都留在原处、
    // 记录留着，让启动恢复和人工检查接手（**不删任何一边**）。
    if (!QDir().rename(replacement, target)) {
        const bool putBack = !hadTarget || QDir().rename(retained, target);
        if (putBack) {
            undoJournal();
            return refuse(QObject::tr("选中的旧目录替换失败，当前目录已放回原位；两份均未删除。"));
        }
        result.retainedPath = retained;
        return refuse(QObject::tr("选中的旧目录替换失败，当前目录也未能放回原位。\n\n两份均完整保留：当前目录在 %1，选中的旧目录在 %2。\n恢复记录保留在 %3，请人工检查。")
                          .arg(QDir::toNativeSeparators(retained),
                               QDir::toNativeSeparators(replacement),
                               QDir::toNativeSeparators(journalPath)));
    }

    // ---------- ⑤ 收尾：记录改成「已发布」，两份目录都留着 ----------
    result.ok = true;
    result.retainedPath = hadTarget ? retained : QString();
    journal.insert(QStringLiteral("state"), QStringLiteral("published"));
    if (!RestoreJournal::write(journalPath, journal)) {
        // 数据已经换好了，只是记录还停在「prepared」。启动恢复碰到"目标在、旧目录也在"
        // 只会提示人工检查，不会删任何一边 —— 这句话要让用户看见，但不该把成功说成失败。
        result.notes.append(QObject::tr("未能将恢复记录标记为「已发布」。两个目录均已保留，下次启动时可检查此事务。"));
    }
    return result;
}

}  // namespace ManualRecovery
