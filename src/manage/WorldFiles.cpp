/**
 * @file WorldFiles.cpp
 * @brief 实现 WorldFiles.hpp 里那四个函数。
 * @note 见 WorldFiles.hpp 的说明。这里只留实现上的坑。
 */

#include "WorldFiles.hpp"
#include <QCoreApplication>
#include "util/WriteDirectoryGuard.hpp"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QStorageInfo>

#include <algorithm>
#include <limits>

#include "FolderManager.hpp"
#include "InstanceFiles.hpp"
#include "util/ZipUtil.hpp"

namespace {

/// 一个世界目录里必须有的那一个文件。游戏靠它认"这是不是一个世界"。
constexpr char kLevelDatName[] = "level.dat";

/// gzip 的两个魔数字节（`1f 8b`）。盘上那份没有它，就不是 gzip 包的（见下面那段）。
constexpr quint8 kGzipMagic0 = 0x1F;
constexpr quint8 kGzipMagic1 = 0x8B;

/// 临时文件的落点：**系统临时目录**（读和写都是，写那一路 改的 —— 落盘现在
/// 走 QSaveFile，它自己在 level.dat 那个目录里开临时文件，见 writeLevelData 里的 ★★）。
/// 名字带随机串，同一个世界里两份同时跑也不会撞（正常路径上不会有，但撞上一次的代价是
/// "覆盖了别人的中间产物" —— 随机串比一个计数器便宜）。
QString tempPathFor(const QString &baseDir, const QString &tag) {
    return baseDir + QStringLiteral("/.wtfml-") + tag + QLatin1Char('-') +
           QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
}

/// 读一格 Byte 出来（`descend()` 找不到、或者那一格不是整数族，都算"没有这一项"）。
bool readByteField(const NbtTree::Node &root, const QString &path, bool *present, qint64 *out) {
    *present = false;
    const NbtTree::Node *node = NbtTree::descend(root, path);
    if (node == nullptr || NbtTree::integerBits(node->type) == 0) {
        return false;
    }
    *present = true;
    *out = node->number;
    return true;
}

/// 读一格 String 出来（同上，不是文字就算"没有"）。
bool readStringField(const NbtTree::Node &root, const QString &path, bool *present, QString *out) {
    *present = false;
    const NbtTree::Node *node = NbtTree::descend(root, path);
    if (node == nullptr || node->type != NbtTree::Type::String) {
        return false;
    }
    *present = true;
    *out = node->text;
    return true;
}

/**
 * @brief 改一格的值；那一格在盘上根本没有时，按 @p whenMissing 决定拒绝还是**建一格**。
 *
 * （见头文件里 `MissingField` 那一段）：老存档可能没有 `Data/allowCommands`
 * （1.8 才加的）或者 `Data/hardcore`。这类档在界面上那几格一律可动，
 * 所以"盘上没有"这件事必须有个着落。`Create` 那一档就是它。
 *
 * @param type 建的时候用什么类型 —— 世界名是 `String`，允许作弊 / 极限模式 / 难度是 `Byte`
 * （和 `readWorldInfo()` 读它们时用的 `readStringField` / `readByteField` 对得上）。
 */
bool writeFieldOrCreate(NbtTree::Node *root, const QString &path, const QString &text,
                        NbtTree::Type type, WorldFiles::MissingField whenMissing) {
    if (NbtTree::Node *node = NbtTree::descend(root, path)) {
        if (node->type != type) {
            if (NbtTree::integerBits(type) == 0 || NbtTree::integerBits(node->type) == 0) {
                return false;
            }
            node->type = type;
            node->number = 0;
        }
        return NbtTree::setValueText(node, text);
    }
    if (whenMissing != WorldFiles::MissingField::Create) {
        return false;
    }

    // 最后一段是字段名，前面那截是父节点的路径。父节点不在 / 不是复合标签就建不了 ——
    // 连 `Data` 都不是复合标签的话，这份 `level.dat` 的形状我们看不懂，一个字节都不动
    // （和 `NbtTree::parse()` 那条规矩一致）。
    const int cut = path.lastIndexOf(QLatin1Char('/'));
    const QString parentPath = cut < 0 ? QString() : path.left(cut);
    const QString fieldName = cut < 0 ? path : path.mid(cut + 1);
    NbtTree::Node *parent = parentPath.isEmpty() ? root : NbtTree::descend(root, parentPath);
    if (parent == nullptr || parent->type != NbtTree::Type::Compound) {
        return false;
    }

    NbtTree::Field fresh;
    fresh.first = fieldName;
    fresh.second.type = type;
    parent->fields.append(fresh);
    // `append()` 之后必须**重新取一次**（`QList` 可能把那一块内存搬走）。
    return NbtTree::setValueText(&parent->fields.last().second, text);
}

}  // namespace

namespace WorldFiles {

bool looksLikeWorld(const QString &worldDir) {
    return QFileInfo::exists(levelDataPath(worldDir));
}

QString nameProblem(const QString &rawName) {
    return InstanceFiles::validateDirName(rawName);
}

namespace {

/// 把源世界逐个文件复制到暂存候选里，遇到中断 / 取消立刻停。
/// 已存在且内容一致的候选文件会被跳过（续做复用），其它情况一律重写目标文件。
struct CopyTreeOutcome {
    bool ok = false;
    bool cancelled = false;
    int totalFiles = 0;
    int copiedFiles = 0;
    int skippedFiles = 0;
    int failedFiles = 0;
    qint64 totalBytes = 0;
    qint64 copiedBytes = 0;
    QStringList failures;
    QString problem;
};

bool sameFileContent(const QString &left, const QString &right,
                     const std::function<bool()> &cancelled) {
    QFile a(left), b(right);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hashA(QCryptographicHash::Sha1), hashB(QCryptographicHash::Sha1);
    while (!a.atEnd()) {
        if (cancelled && cancelled()) {
            return false;
        }
        const QByteArray chunk = a.read(1 << 20);
        if (chunk.isEmpty()) {
            break;
        }
        hashA.addData(chunk);
    }
    while (!b.atEnd()) {
        if (cancelled && cancelled()) {
            return false;
        }
        const QByteArray chunk = b.read(1 << 20);
        if (chunk.isEmpty()) {
            break;
        }
        hashB.addData(chunk);
    }
    return hashA.result() == hashB.result();
}

/// 单个文件：QSaveFile 临时文件 + 原子改名。失败时目标那一份保持原样或不存在。
bool copyOneFile(const QString &source, const QString &target,
                 const std::function<bool()> &cancelled, QString *problem) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("WorldFiles", "读不了 %1（%2）")
                           .arg(QDir::toNativeSeparators(source), in.errorString());
        }
        return false;
    }
    const QFileInfo targetInfo(target);
    if (!QDir().mkpath(targetInfo.absolutePath())) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("WorldFiles", "无法创建目录 %1").arg(targetInfo.absolutePath());
        }
        return false;
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("WorldFiles", "无法写入 %1（%2）")
                           .arg(QDir::toNativeSeparators(target), out.errorString());
        }
        return false;
    }
    while (!in.atEnd()) {
        if (cancelled && cancelled()) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("WorldFiles", "已取消");
            }
            return false;  // 析构扔掉临时文件
        }
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty()) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("WorldFiles", "读不了 %1（%2）")
                               .arg(QDir::toNativeSeparators(source), in.errorString());
            }
            return false;
        }
        if (out.write(chunk) != chunk.size()) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("WorldFiles", "无法写入 %1（%2）")
                               .arg(QDir::toNativeSeparators(target), out.errorString());
            }
            return false;
        }
    }
    if (!out.commit()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("WorldFiles", "发布不了 %1（%2）")
                           .arg(QDir::toNativeSeparators(target), out.errorString());
        }
        return false;
    }
    return true;
}

CopyTreeOutcome copyWorldTree(const QString &source, const QString &candidate,
                              const std::function<bool()> &cancelled,
                              const ImportProgress &progress) {
    CopyTreeOutcome outcome;
    QSet<QString> failedPaths;
    // 先收一份清单（相对路径 + 大小）：空间预检、进度总数和续做核对都读它。
    QList<QPair<QString, qint64>> files;
    QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelled && cancelled()) {
            outcome.cancelled = true;
            outcome.problem = QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。");
            return outcome;
        }
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isSymLink() || info.isJunction()) {
            outcome.problem = QCoreApplication::translate("WorldFiles", "世界包含链接，未开始导入。");
            return outcome;
        }
        if (info.isDir()) {
            continue;
        }
        if (!info.isFile()) {
            outcome.problem = QCoreApplication::translate("WorldFiles", "世界里有不是普通文件的内容：%1")
                                  .arg(QDir::toNativeSeparators(info.absoluteFilePath()));
            return outcome;
        }
        const QString relative = QDir(source).relativeFilePath(info.absoluteFilePath());
        files.append({relative, info.size()});
        outcome.totalFiles += 1;
        outcome.totalBytes += info.size();
    }
    for (const auto &file : files) {
        if (cancelled && cancelled()) {
            outcome.cancelled = true;
            outcome.problem = QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。");
            return outcome;
        }
        const QString sourceFile = QDir(source).filePath(file.first);
        const QString targetFile = QDir(candidate).filePath(file.first);
        const QFileInfo existing(targetFile);
        if (existing.isFile() && existing.size() == file.second
            && sameFileContent(sourceFile, targetFile, cancelled)) {
            ++outcome.skippedFiles;
            outcome.copiedBytes += file.second;
            if (progress) {
                progress(outcome.skippedFiles + outcome.copiedFiles, outcome.totalFiles,
                         outcome.copiedBytes, outcome.totalBytes);
            }
            continue;
        }
        QString problem;
        if (!copyOneFile(sourceFile, targetFile, cancelled, &problem)) {
            if (cancelled && cancelled()) {
                outcome.cancelled = true;
                outcome.problem = QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。");
                return outcome;
            }
            ++outcome.failedFiles;
            failedPaths.insert(file.first);
            if (outcome.failures.size() < 20) {
                outcome.failures.append(QCoreApplication::translate("WorldFiles", "%1：%2").arg(file.first, problem));
            }
            outcome.problem = QCoreApplication::translate("WorldFiles", "文件复制失败：%1（原因：%2）")
                                  .arg(file.first, problem);
            continue;
        }
        ++outcome.copiedFiles;
        outcome.copiedBytes += file.second;
        if (progress) {
            progress(outcome.skippedFiles + outcome.copiedFiles, outcome.totalFiles,
                     outcome.copiedBytes, outcome.totalBytes);
        }
    }
    // 复制完后按清单再核一遍（路径 + 大小）；续做跳过的文件已经在上面比过内容。
    // 已经在复制那一步记过失败的项不再重复计数（否则“失败 1 个”会变成 2 个）。
    for (const auto &file : files) {
        if (failedPaths.contains(file.first)) {
            continue;
        }
        const QFileInfo target(QDir(candidate).filePath(file.first));
        if (!target.isFile() || target.size() != file.second) {
            ++outcome.failedFiles;
            failedPaths.insert(file.first);
            if (outcome.failures.size() < 20) {
                outcome.failures.append(QCoreApplication::translate("WorldFiles", "%1：暂存副本不完整").arg(file.first));
            }
        }
    }
    outcome.ok = outcome.failedFiles == 0;
    if (!outcome.ok && outcome.problem.isEmpty()) {
        outcome.problem = QCoreApplication::translate("WorldFiles", "部分文件无法复制到暂存目录。");
    }
    return outcome;
}

}  // namespace

ImportResult importWorld(const QString &worldDir, const QString &savesDir, const QString &name,
                         const ImportOptions &options, const std::function<bool()> &cancelled,
                         const ImportProgress &progress) {
    ImportResult result;
    const auto fail = [&result](const QString &reason) {
        result.problem = reason;
        return result;
    };
    if (!WriteDirectoryGuard::acquire(savesDir, &result.problem)) {
        return result;
    }
    const QString source = worldDir.trimmed();
    const QString target = savesDir.trimmed();
    if (source.isEmpty()) {
        return fail(QCoreApplication::translate("WorldFiles", "没有指定要导入哪一个世界。"));
    }
    if (target.isEmpty()) {
        return fail(QCoreApplication::translate("WorldFiles", "不知道要导入到哪个实例里去。"));
    }
    const QString nameProblemText = nameProblem(name);
    if (!nameProblemText.isEmpty()) {
        return fail(nameProblemText);
    }
    const QString dirName = name.trimmed();
    if (!QFileInfo(source).isDir()) {
        return fail(QCoreApplication::translate("WorldFiles", "「%1」不是一个目录。").arg(source));
    }
    if (!looksLikeWorld(source)) {
        return fail(QCoreApplication::translate("WorldFiles", "「%1」缺少 level.dat，无法识别为世界存档。请选择存档所在目录。")
                        .arg(QDir(source).dirName()));
    }
    if (!QFileInfo(target).isDir()) {
        return fail(QCoreApplication::translate("WorldFiles", "此实例中没有 saves 目录。请先启动一次游戏，或手动创建该目录。"));
    }
    const QString destDir = target + QLatin1Char('/') + dirName;
    if (QFileInfo::exists(destDir)) {
        result.conflict = true;
        switch (options.conflictPolicy) {
        case OperationReport::ConflictPolicy::SkipExisting:
            result.ok = true;
            result.skipped = true;
            result.problem = QCoreApplication::translate("WorldFiles", "已经有一个叫「%1」的世界了，按选择跳过了这一份。")
                                 .arg(dirName);
            return result;
        case OperationReport::ConflictPolicy::Overwrite:
        case OperationReport::ConflictPolicy::Cancel:
            // 世界存档**从不覆盖**：覆盖一个世界等于把用户的进度静默换掉。
            return fail(QCoreApplication::translate("WorldFiles", "这个实例里已经有一个叫「%1」的世界了，换个名字再导入。")
                            .arg(dirName));
        }
    }
    if (InstanceFiles::pathIsSameOrInside(source, target)) {
        return fail(QCoreApplication::translate("WorldFiles", "不能把世界导入到它自己或其子目录中。"));
    }
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return fail(QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。"));
    }

    // 暂存：调用方给了可续目录就复用它（里面已有的文件按内容跳过），否则用系统临时目录。
    QTemporaryDir temporary;
    QString stagingRoot;
    bool keepStaging = false;
    if (!options.stagingDir.isEmpty()) {
        stagingRoot = options.stagingDir;
        if (!QDir().mkpath(stagingRoot)) {
            return fail(QCoreApplication::translate("WorldFiles", "无法创建世界导入暂存目录：%1").arg(stagingRoot));
        }
        keepStaging = true;
    } else {
        if (!temporary.isValid()) {
            return fail(QCoreApplication::translate("WorldFiles", "无法创建世界导入暂存目录。"));
        }
        stagingRoot = temporary.path();
    }
    result.stagingDir = keepStaging ? stagingRoot : QString();
    const QString candidate = QDir(stagingRoot).filePath(QStringLiteral("world"));

    // 空间：按整份世界算（续做时已有的文件也算进去 —— 少算是会真写满盘的）。
    constexpr qint64 margin = 32 * 1024 * 1024;
    qint64 required = margin;
    QDirIterator sizes(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                  | QDir::System, QDirIterator::Subdirectories);
    while (sizes.hasNext()) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return fail(QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。"));
        }
        sizes.next();
        const auto info = sizes.fileInfo();
        if (info.isSymLink() || info.isJunction()) {
            return fail(QCoreApplication::translate("WorldFiles", "世界包含链接，未开始导入。"));
        }
        if (!info.isFile()) {
            continue;
        }
        const qint64 size = info.size();
        result.totalBytes += size;
        result.totalFiles += 1;
        if (size < 0 || size > std::numeric_limits<qint64>::max() - required) {
            return fail(QCoreApplication::translate("WorldFiles", "无法安全计算世界大小，未开始导入。"));
        }
        required += size;
    }
    const QStorageInfo storage(target);
    const qint64 available = options.spaceBudgetBytes >= 0
        ? qMin(storage.bytesAvailable(), options.spaceBudgetBytes) : storage.bytesAvailable();
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly() || available < required) {
        return fail(QCoreApplication::translate("WorldFiles", "目标磁盘不可写或可用空间不足，未开始导入世界（需要至少 %1 字节，含安全余量）。")
                        .arg(required));
    }

    const CopyTreeOutcome copied = copyWorldTree(source, candidate, cancelled, progress);
    result.copiedFiles = copied.copiedFiles;
    result.skippedFiles = copied.skippedFiles;
    result.failedFiles = copied.failedFiles;
    result.copiedBytes = copied.copiedBytes;
    result.failures = copied.failures;
    if (copied.cancelled || (cancelled && cancelled())) {
        result.cancelled = true;
        result.problem = QCoreApplication::translate("WorldFiles", "导入世界已取消或超时。");
        if (!keepStaging) {
            removeTreeWithoutFollowingLinks(stagingRoot);
        }
        return result;
    }
    if (!copied.ok) {
        result.problem = copied.problem.isEmpty()
            ? QCoreApplication::translate("WorldFiles", "导入「%1」的时候有些文件没拷成。").arg(dirName)
            : copied.problem;
        if (!keepStaging) {
            removeTreeWithoutFollowingLinks(stagingRoot);
        }
        return result;
    }
    if (!looksLikeWorld(candidate)) {
        result.problem = QCoreApplication::translate("WorldFiles", "导入已取消、超时或源世界在复制期间发生变化，未发布存档。");
        if (!keepStaging) {
            removeTreeWithoutFollowingLinks(stagingRoot);
        }
        return result;
    }
    if (!QDir().rename(candidate, destDir)) {
        result.problem = QCoreApplication::translate("WorldFiles", "无法发布导入的世界，目标可能已被其它程序创建，未覆盖。");
        if (!keepStaging) {
            removeTreeWithoutFollowingLinks(stagingRoot);
        }
        return result;
    }
    // 发布成功：可续暂存到此没有价值（目标已经在了），连同那份报告一起收掉。
    if (keepStaging) {
        removeTreeWithoutFollowingLinks(stagingRoot);
    }
    result.ok = true;
    result.stagingDir.clear();
    return result;
}

bool copyWorldInto(const QString &worldDir, const QString &savesDir, const QString &name,
                   QString *problemOut, const std::function<bool()> &cancelled, qint64 spaceBudgetBytes) {
    ImportOptions options;
    options.conflictPolicy = OperationReport::ConflictPolicy::Cancel;
    options.spaceBudgetBytes = spaceBudgetBytes;
    const ImportResult result = importWorld(worldDir, savesDir, name, options, cancelled);
    if (problemOut != nullptr) {
        *problemOut = result.problem;
    }
    return result.ok && !result.skipped;
}

bool renameWorld(const QString &worldDir, const QString &newName, QString *problemOut) {
    if (!WriteDirectoryGuard::acquire(worldDir, problemOut)) return false;
    const auto fail = [problemOut](const QString &reason) {
        if (problemOut != nullptr) {
            *problemOut = reason;
        }
        return false;
    };
    if (problemOut != nullptr) {
        problemOut->clear();
    }

    const QString source = worldDir.trimmed();
    if (source.isEmpty()) {
        return fail(QCoreApplication::translate("WorldFiles", "没有指定要改哪一个世界。"));
    }
    const QString problem = nameProblem(newName);
    if (!problem.isEmpty()) {
        return fail(problem);
    }
    const QString dirName = newName.trimmed();

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isDir()) {
        return fail(QCoreApplication::translate("WorldFiles", "存档「%1」已丢失。").arg(source));
    }

    // 只差大小写（`Foo` → `foo`）是**合法**的改名：它改的就是磁盘上那个名字的大小写。
    // 所以恰好和现名相等的那一项不参与"重名"比较（和 VersionManager::renameInstance()
    // 那边同一条规矩）。逐字相等则什么都不用做，直接算成功。
    const QString current = sourceInfo.fileName();
    if (current == dirName) {
        return true;
    }

    const QString parent = sourceInfo.absolutePath();
    const QString destDir = parent + QLatin1Char('/') + dirName;
    if (current.compare(dirName, Qt::CaseInsensitive) != 0 && QFileInfo::exists(destDir)) {
        return fail(QCoreApplication::translate("WorldFiles", "这个存档文件夹里已经有一个叫「%1」的世界了。").arg(dirName));
    }

    // QDir::rename() 的第二个参数是**目标全路径**（不是"新名字"），写成新名字的话会在
    // 当前工作目录下改出一个同名目录来 —— 这是那个重载最容易看错的一处。
    if (!QDir().rename(source, destDir)) {
        return fail(QCoreApplication::translate("WorldFiles", "重命名失败，存档文件可能正被其他程序占用。"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 世界设置：读 / 改 / 写 level.dat
// ---------------------------------------------------------------------------

QString levelDataPath(const QString &worldDir) {
    return worldDir + QLatin1Char('/') + QLatin1String(kLevelDatName);
}

QString difficultyName(int difficulty) {
    switch (difficulty) {
    case 0:
        return QCoreApplication::translate("WorldFiles", "和平");
    case 1:
        return QCoreApplication::translate("WorldFiles", "简单");
    case 2:
        return QCoreApplication::translate("WorldFiles", "普通");
    case 3:
        return QCoreApplication::translate("WorldFiles", "困难");
    default:
        // 越界**不给默认名**：盘上写着一个 7 的时候，界面上打「普通」就是在撒谎
        // （用户会以为它是好的，然后被我们改成一个真的普通）。空串让调用方自己说
        // "这个值我们不认识"。
        return QString();
    }
}

// ---------------------------------------------------------------------------
// 规则全集
// ---------------------------------------------------------------------------
//
// 来龙去脉、以及"为什么这一张表不分游戏版本"写在头文件那一段里。这里只留实现上的两条：
//
// * 表里的名字**不分大小写地排好序**（界面上那一列就是照这个顺序摆的，`allGameRules()`
// 还会再排一遍兜底）；
// * 默认值**逐字**写在这儿（`"true"` / `"false"` / `"65536"`）—— 形状是从这串字推出来的
// （`NbtTree::ruleKind()`），所以多一个类型列只会有两个说法打架的机会。

namespace {

const QList<WorldFiles::GameRuleDefault> &ruleCatalog() {
    static const QList<WorldFiles::GameRuleDefault> table = {
        {QStringLiteral("announceAdvancements"), QStringLiteral("true")},
        {QStringLiteral("blockExplosionDropDecay"), QStringLiteral("true")},
        {QStringLiteral("commandBlockOutput"), QStringLiteral("true")},
        {QStringLiteral("disableElytraMovementCheck"), QStringLiteral("false")},
        {QStringLiteral("disableRaids"), QStringLiteral("false")},
        {QStringLiteral("doDaylightCycle"), QStringLiteral("true")},
        {QStringLiteral("doEntityDrops"), QStringLiteral("true")},
        {QStringLiteral("doFireTick"), QStringLiteral("true")},
        {QStringLiteral("doImmediateRespawn"), QStringLiteral("false")},
        {QStringLiteral("doInsomnia"), QStringLiteral("true")},
        {QStringLiteral("doLimitedCrafting"), QStringLiteral("false")},
        {QStringLiteral("doMobLoot"), QStringLiteral("true")},
        {QStringLiteral("doMobSpawning"), QStringLiteral("true")},
        {QStringLiteral("doPatrolSpawning"), QStringLiteral("true")},
        {QStringLiteral("doTileDrops"), QStringLiteral("true")},
        {QStringLiteral("doTraderSpawning"), QStringLiteral("true")},
        {QStringLiteral("doVinesSpread"), QStringLiteral("true")},
        {QStringLiteral("doWardenSpawning"), QStringLiteral("true")},
        {QStringLiteral("doWeatherCycle"), QStringLiteral("true")},
        {QStringLiteral("drowningDamage"), QStringLiteral("true")},
        {QStringLiteral("elytraMovementCheck"), QStringLiteral("true")},
        {QStringLiteral("fallDamage"), QStringLiteral("true")},
        {QStringLiteral("fireDamage"), QStringLiteral("true")},
        {QStringLiteral("forgiveDeadPlayers"), QStringLiteral("true")},
        {QStringLiteral("freezeDamage"), QStringLiteral("true")},
        {QStringLiteral("keepInventory"), QStringLiteral("false")},
        {QStringLiteral("logAdminCommands"), QStringLiteral("true")},
        {QStringLiteral("maxCommandChainLength"), QStringLiteral("65536")},
        {QStringLiteral("maxEntityCramming"), QStringLiteral("24")},
        {QStringLiteral("mobExplosionDropDecay"), QStringLiteral("true")},
        {QStringLiteral("mobGriefing"), QStringLiteral("true")},
        {QStringLiteral("naturalRegeneration"), QStringLiteral("true")},
        {QStringLiteral("playersSleepingPercentage"), QStringLiteral("100")},
        {QStringLiteral("randomTickSpeed"), QStringLiteral("3")},
        {QStringLiteral("reducedDebugInfo"), QStringLiteral("false")},
        {QStringLiteral("respawnRadius"), QStringLiteral("10")},
        {QStringLiteral("sendCommandFeedback"), QStringLiteral("true")},
        {QStringLiteral("showDeathMessages"), QStringLiteral("true")},
        {QStringLiteral("snowAccumulationHeight"), QStringLiteral("1")},
        {QStringLiteral("spawnMonsters"), QStringLiteral("3")},
        {QStringLiteral("spawnRadius"), QStringLiteral("10")},
        {QStringLiteral("spectatorsGenerateChunks"), QStringLiteral("true")},
        {QStringLiteral("tntExplosionDropDecay"), QStringLiteral("false")},
        {QStringLiteral("universalAnger"), QStringLiteral("false")},
    };
    return table;
}

}  // namespace

const QList<GameRuleDefault> &knownGameRules() {
    return ruleCatalog();
}

QList<NbtTree::GameRule> allGameRules(const QList<NbtTree::GameRule> &onDisk) {
    QHash<QString, QString> diskValues;
    for (const NbtTree::GameRule &rule : onDisk) {
        diskValues.insert(rule.name, rule.value);
    }

    QList<NbtTree::GameRule> out;
    QSet<QString> already;
    for (const GameRuleDefault &known : ruleCatalog()) {
        NbtTree::GameRule rule;
        rule.name = known.name;
        // 盘上有就用盘上那个值（那是这个世界的真值），没有才拿默认值顶着。
        rule.value = diskValues.contains(known.name) ? diskValues.value(known.name) : known.value;
        out.append(rule);
        already.insert(known.name);
    }
    // 盘上有、全集里没有的（模组写的、比这张表还新的规则）**原样列出来**：这一页说的是
    // "这个世界现在是什么样"，少一条就是我们瞒着用户。
    for (const NbtTree::GameRule &rule : onDisk) {
        if (!already.contains(rule.name)) {
            out.append(rule);
        }
    }

    // 排一遍（大小写不敏感）：全集的顺序本来就是排好的，但盘上捡回来那几条得插到该在的
    // 位置上去（`moddedRule` 该落在 m 那一段里，不是排到所有小写名字后面）。
    std::sort(out.begin(), out.end(),
              [](const NbtTree::GameRule &a, const NbtTree::GameRule &b) {
                  const int cmp = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                  return cmp != 0 ? cmp < 0 : a.name < b.name;
              });
    return out;
}

bool applyChangedGameRules(NbtTree::Document *document, const QList<NbtTree::GameRule> &rules,
                           QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (document == nullptr) {
        return false;
    }
    for (const NbtTree::GameRule &rule : rules) {
        QString why;
        // `MissingRule::Create`：名字不在表里就加一条，`Data/GameRules` 那一格不在就连
        // 那一格一起建（用户点名要的那一档，见头文件里那一段）。
        if (!NbtTree::setGameRule(&document->root, rule.name, rule.value, &why,
                                  NbtTree::MissingRule::Create)) {
            if (problemOut != nullptr) {
                *problemOut = why.isEmpty() ? QCoreApplication::translate("WorldFiles", "无法保存游戏规则「%1」。").arg(rule.name)
                                            : why;
            }
            return false;
        }
    }
    return true;
}

void readWorldInfo(const NbtTree::Document &document, bool gzipped, WorldInfo *out) {
    if (out == nullptr) {
        return;
    }
    *out = WorldInfo();
    out->gzipped = gzipped;

    readStringField(document.root, QStringLiteral("Data/LevelName"), &out->hasLevelName,
                    &out->levelName);
    readStringField(document.root, QStringLiteral("Data/Version/Name"), &out->hasVersionName,
                    &out->versionName);
    readByteField(document.root, QStringLiteral("Data/LastPlayed"), &out->hasLastPlayed,
                  &out->lastPlayedMs);
    readByteField(document.root, QStringLiteral("Data/DataVersion"), &out->hasDataVersion,
                  &out->dataVersion);

    qint64 number = 0;
    if (readByteField(document.root, QStringLiteral("Data/allowCommands"), &out->hasAllowCommands,
                      &number)) {
        // **只认 0 和非 0 两种**，不把 2 也画成"开"：这个字段游戏自己只写 0/1，
        // 别的值说明盘上那份不是游戏写的，界面上当"没有这一项"处理（灰色的），
        // 总好过一个看不出来的"我点了一下它从 1 变成了 0"。
        if (number == 0 || number == 1) {
            out->allowCommands = (number != 0);
        } else {
            out->hasAllowCommands = false;
        }
    }
    if (readByteField(document.root, QStringLiteral("Data/hardcore"), &out->hasHardcore, &number)) {
        if (number == 0 || number == 1) {
            out->hardcore = (number != 0);
        } else {
            out->hasHardcore = false;
        }
    }
    if (readByteField(document.root, QStringLiteral("Data/Difficulty"), &out->hasDifficulty,
                      &number)) {
        if (number >= 0 && number <= 3) {
            out->difficulty = int(number);
        } else {
            out->hasDifficulty = false;
        }
    }

    const NbtTree::Node *rulesNode =
        NbtTree::descend(document.root, QStringLiteral("Data/GameRules"));
    out->hasRules = (rulesNode != nullptr && rulesNode->type == NbtTree::Type::Compound);
    // 注意 `rules` 这一份和 `hasRules` 是**两个问题**：`hasRules` = "有没有一张能编辑的规则表"，
    // `rules` = "现在读得出几条"。空表和"没有那一格"在 `rules` 上都是零条（读起来一样），
    // 但在 `hasRules` 上分得开 —— 界面要的那一句「这个世界还没有任何游戏规则」只对**前者**说。
    QString ignored;
    NbtTree::gameRulesOf(document.root, &out->rules, &ignored);
}

bool applyWorldInfo(NbtTree::Document *document, const WorldInfo &info, QString *problemOut,
                    MissingField whenMissing) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (document == nullptr) {
        return false;
    }
    const auto fail = [problemOut](const QString &why) {
        if (problemOut != nullptr) {
            *problemOut = why;
        }
        return false;
    };
    NbtTree::Node *root = &document->root;

    // 每一项都**先看 `hasXxx`**：界面上那一格没显示（盘上本来就没有、用户也没碰），这里
    // 就不写。三行一个模样，故意不抽成一个循环 —— 每一项的判据不一样（文字 / 开关 / 数值），
    // 抽出来的那个"通用项"会把这三种差别藏进参数里。
    if (info.hasLevelName &&
        !writeFieldOrCreate(root, QStringLiteral("Data/LevelName"), info.levelName,
                            NbtTree::Type::String, whenMissing)) {
        return fail(QCoreApplication::translate("WorldFiles", "存档中的世界名称字段类型无效，未保存修改。"));
    }
    if (info.hasAllowCommands &&
        !writeFieldOrCreate(root, QStringLiteral("Data/allowCommands"),
                            info.allowCommands ? QStringLiteral("1") : QStringLiteral("0"),
                            NbtTree::Type::Byte, whenMissing)) {
        return fail(QCoreApplication::translate("WorldFiles", "存档中的「允许作弊」不是有效布尔值（0 或 1），未保存修改。"));
    }
    if (info.hasHardcore &&
        !writeFieldOrCreate(root, QStringLiteral("Data/hardcore"),
                            info.hardcore ? QStringLiteral("1") : QStringLiteral("0"),
                            NbtTree::Type::Byte, whenMissing)) {
        return fail(QCoreApplication::translate("WorldFiles", "存档中的「极限模式」不是有效布尔值（0 或 1），未保存修改。"));
    }
    if (info.hasDifficulty &&
        !writeFieldOrCreate(root, QStringLiteral("Data/Difficulty"),
                            QString::number(info.difficulty), NbtTree::Type::Byte, whenMissing)) {
        return fail(QCoreApplication::translate("WorldFiles", "存档中的「难度」值无效（有效值为 0 至 3），未保存修改。"));
    }

    // 游戏规则：逐条走 `setGameRule()`。它**只改已有的**（不发明），名字对不上就是"没写
    // 进去"，报出来。
    //
    // 这一条**不跟着 `whenMissing` 走**，要新增规则请走 `applyChangedGameRules()`
    // —— 那一档（`MissingRule::Create`）要求调用方保证"写进去的名字是游戏真有的那一条"，
    // 而 `WorldInfo::rules` 是"盘上读出来那几条"，这里保证不了。两件事分开之后，
    // 这一句仍然是"只把我们读到的那些写回去"。
    for (const NbtTree::GameRule &rule : info.rules) {
        QString why;
        if (!NbtTree::setGameRule(root, rule.name, rule.value, &why)) {
            return fail(why.isEmpty()
                            ? QCoreApplication::translate("WorldFiles", "无法保存游戏规则「%1」。").arg(rule.name)
                            : why);
        }
    }
    return true;
}

bool readLevelData(const QString &worldDir, LevelData *out, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (out == nullptr) {
        return false;
    }
    const auto fail = [problemOut](const QString &why) {
        if (problemOut != nullptr) {
            *problemOut = why;
        }
        return false;
    };

    const QString source = worldDir.trimmed();
    if (source.isEmpty()) {
        return fail(QCoreApplication::translate("WorldFiles", "没有指定是哪一个世界。"));
    }
    const QString path = levelDataPath(source);
    QFile file(path);
    if (!file.exists()) {
        return fail(QCoreApplication::translate("WorldFiles", "这个存档里没有 level.dat，它不是一个世界（或已损坏）。"));
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QCoreApplication::translate("WorldFiles", "无法读取 level.dat：%1").arg(file.errorString()));
    }
    const QByteArray raw = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return fail(QCoreApplication::translate("WorldFiles", "level.dat 读取不完整：%1").arg(file.errorString()));
    }
    file.close();

    // 先闻一下头两字节，**不是**一律当 gzip 解：`level.dat` 里那份 NBT 通常是 gzip
    // 包着的，但**被工具改过的、或者某些模组导出的**那份是裸的。一律丢给 gunzip 的话
    // 后者会得到一句"这不是 gzip"，而它对游戏来说是能读的（游戏两种都认）——
    // 我们就会把一个好存档报成坏档，还劝用户去备份。闻到 `1f 8b` 才当 gzip。
    const bool gzipped = raw.size() >= 2 && static_cast<quint8>(raw.at(0)) == kGzipMagic0 &&
                         static_cast<quint8>(raw.at(1)) == kGzipMagic1;

    QByteArray body;
    if (gzipped) {
        // 解压走 PowerShell → 落到一个临时文件，再读回来（ZipUtil 那边不往 stdout 打
        // 二进制的理由见它自己的头文件）。临时文件在**系统临时目录**：世界目录里多出来
        // 的东西会被游戏扫到。
        const QString temp = tempPathFor(QDir::tempPath(), QStringLiteral("read-level"));
        QString why;
        if (!ZipUtil::gunzipTo(path, temp, &why)) {
            QFile::remove(temp);
            return fail(QCoreApplication::translate("WorldFiles", "无法解压 level.dat（它可能已损坏）：%1").arg(why));
        }
        QFile bodyFile(temp);
        if (!bodyFile.open(QIODevice::ReadOnly)) {
            QFile::remove(temp);
            return fail(QCoreApplication::translate("WorldFiles", "无法读取解压后的临时文件：%1").arg(bodyFile.errorString()));
        }
        body = bodyFile.readAll();
        bodyFile.close();
        QFile::remove(temp);
    } else {
        body = raw;
    }

    NbtTree::Document document;
    QString why;
    if (!NbtTree::parse(body, &document, &why)) {
        return fail(QCoreApplication::translate("WorldFiles", "level.dat 里的内容无法解析（这个存档可能已损坏）：%1").arg(why));
    }
    if (document.root.type != NbtTree::Type::Compound) {
        // `parse()` 已经保证根是一整格，但**没保证它是复合标签**（NBT 规范允许根是任何
        // 类型）—— 而游戏把根当复合读，不是复合的它自己也读不动。这里和游戏站一边。
        return fail(QCoreApplication::translate("WorldFiles", "level.dat 的根不是一张表（这个存档可能已损坏）。"));
    }

    QFile current(path);
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != raw ||
        current.error() != QFileDevice::NoError) {
        return fail(QCoreApplication::translate("WorldFiles", "读取期间 level.dat 已变化或无法重读，请重新打开世界设置。"));
    }
    out->sourceSha256 = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
    out->document = document;
    readWorldInfo(document, gzipped, &out->info);
    return true;
}

bool writeLevelData(const QString &worldDir, const LevelData &data, QString *problemOut) {
    if (!WriteDirectoryGuard::acquire(worldDir, problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const auto fail = [problemOut](const QString &why) {
        if (problemOut != nullptr) {
            *problemOut = why;
        }
        return false;
    };

    const QString dir = worldDir.trimmed();
    if (dir.isEmpty()) {
        return fail(QCoreApplication::translate("WorldFiles", "没有指定要写哪一个世界。"));
    }
    if (!looksLikeWorld(dir)) {
        return fail(QCoreApplication::translate("WorldFiles", "存档缺少 level.dat，未保存修改。"));
    }

    const QByteArray body = NbtTree::serialize(data.document);
    const QString target = levelDataPath(dir);

    // 中间产物一律放**系统临时目录**：最后那一步交给 QSaveFile（见下面 ★★），它在
    // level.dat 所在的那个目录里自己开临时文件、自己换 —— 我们不再往用户的世界目录里
    // 摆任何东西，也就没有"留一个 `.wtfml-xxx.tmp` 在他的存档里"这回事。
    QString tempGz;

    // 写回去的**形状跟着读进来时那一份**：原来是 gzip 就还是 gzip，原来是裸的就还是裸的。
    // 我们有能力"顺手帮你压一下"，但那不是用户要的 —— 他要的是改一条游戏规则，
    // 而"启动器碰过之后这个文件的形状变了"会让以后对不上账（比如他自己拿工具比对）。
    if (data.info.gzipped) {
        const QString tempPlain = tempPathFor(QDir::tempPath(), QStringLiteral("write-level"));
        QFile plain(tempPlain);
        if (!plain.open(QIODevice::WriteOnly)) {
            return fail(QCoreApplication::translate("WorldFiles", "无法创建临时文件：%1").arg(plain.errorString()));
        }
        const qint64 written = plain.write(body);
        plain.close();
        if (written != body.size()) {
            QFile::remove(tempPlain);
            return fail(QCoreApplication::translate("WorldFiles", "临时文件没写全（盘满了？）"));
        }
        tempGz = tempPathFor(QDir::tempPath(), QStringLiteral("level"));
        QString why;
        const bool compressed = ZipUtil::gzipFrom(tempPlain, tempGz, &why);
        QFile::remove(tempPlain);
        if (!compressed) {
            QFile::remove(tempGz);
            return fail(QCoreApplication::translate("WorldFiles", "压缩 level.dat 失败了：%1").arg(why));
        }
    }

    // 落盘这一步**不能用 `QFile::rename()`**：Qt 6.10 里它走的是
    // `QFileSystemEngine::renameFile()` → `::MoveFile`，**不带** REPLACE_EXISTING
    // （qfilesystemengine_win.cpp:1761-1770），目标已存在时就是**失败**。
    // 带 REPLACE_EXISTING 的是隔壁那个私有函数 `renameOverwriteFile()`（1778-1790），
    // 而它只有一条路走得出去：`QSaveFile::commit()`。
    //
    // `level.dat` 到这一步**永远**已经存在（这整件事就是"改一个已有世界的设置"），
    // 所以用 `QFile::rename()` 每次都会失败，而界面上只看到一句"换掉 level.dat 失败了" +
    // 一个问号 —— 看着像"游戏正开着这个世界"的占用问题，其实是改名盖不住。
    //
    // 所以用 QSaveFile：它在**目标所在的目录**里开自己的临时文件，`commit()` 用
    // `renameOverwriteFile()` 一步换掉 —— 真·原子替换（中途断电，盘上要么还是原来那份、
    // 要么已经是新的那份，不会留半截）。代价是压好的字节要多抄一遍：level.dat 量级是
    // 几 MB，值。
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        QFile::remove(tempGz);
        return fail(QCoreApplication::translate("WorldFiles", "无法写入 level.dat：%1").arg(out.errorString()));
    }

    bool wroteOk = false;
    if (data.info.gzipped) {
        QFile gz(tempGz);
        if (gz.open(QIODevice::ReadOnly)) {
            wroteOk = true;
            char buffer[64 * 1024];
            while (true) {
                const qint64 got = gz.read(buffer, static_cast<qint64>(sizeof(buffer)));
                // **读到尾（0）和读出错（-1）不是一回事**：写成
                // `got <= 0` 一 break 会把两者并成同一个出口 —— 于是"读到一半失败"
                // 会被当成"抄完了"，wroteOk 还是 true，底下 commit() 就把一份**截断的**
                // gzip 原子地盖到 level.dat 上，界面还报成功。
                //
                // QIODevice::read() 出错回 **-1**（qfiledevice.cpp:463-486
                // 的 readData() 在 `!isOpen()` 等情况下 `return -1`），只有真到尾才回 0。
                // 这一趟是**逐字节抄**（不经过解压），所以除这两种没有第三种结局 ——
                // 写侧那两处已经由底下 `out.write(...) != got` 管着。
                //
                // NOTE: 到这一步用户的盘上已经有闸 ④ 留的那份备份（见上面那段），所以
                // 这不是"会丢数据"的洞，是"会拿一份半截 level.dat 换掉好文件、而且
                // 不说"的洞。
                if (got < 0) {
                    wroteOk = false;
                    break;
                }
                if (got == 0) {
                    break;
                }
                if (out.write(buffer, got) != got) {
                    wroteOk = false;
                    break;
                }
            }
            gz.close();
        }
        QFile::remove(tempGz);
        if (!wroteOk) {
            out.cancelWriting();
            return fail(QCoreApplication::translate("WorldFiles", "无法替换压缩后的 level.dat。请检查磁盘空间和写入权限。"));
        }
    } else if (out.write(body) != body.size()) {
        out.cancelWriting();
        return fail(QCoreApplication::translate("WorldFiles", "临时文件没写全（盘满了？）"));
    }

    // 压缩/写临时文件期间外部程序仍可能修改原文件，发布前再检查，失败不覆盖。
    // 这是乐观并发保护，不能替代游戏运行门禁，也不是跨程序文件系统事务。
    if (!data.sourceSha256.isEmpty()) {
        QFile original(target);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!original.open(QIODevice::ReadOnly) || !hash.addData(&original) ||
            hash.result() != data.sourceSha256) {
            out.cancelWriting();
            return fail(QCoreApplication::translate("WorldFiles", "level.dat 已被其它程序修改或无法读取，未覆盖；请重新打开世界设置。"));
        }
    }
    if (!out.commit()) {
        return fail(QCoreApplication::translate("WorldFiles", "换掉 level.dat 失败了：%1").arg(out.errorString()));
    }
    return true;
}

}  // namespace WorldFiles
