/**
 * @file BackupFiles.cpp
 * @brief 世界与实例备份的实现。设计与为什么这么设计见头文件。
 */

#include "manage/BackupFiles.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>

#include "manage/FolderManager.hpp"
#include "manage/RestoreJournal.hpp"
#include "manage/OwnedStaging.hpp"
#include "util/DirectorySessionLock.hpp"
#include "util/AppPaths.hpp"
#include "util/ZipUtil.hpp"

namespace BackupFiles {

namespace {

// 盘上那两个字面量。写成一眼能读懂、**和界面无关**的词（`kindKey` 出现在清单里，
// 别的工具照着这个字段判"这是哪种备份"，所以它不能跟着界面上那句中文走）。
const char *kFormatTag = "wtfml-backup/1";
const char *kKindWorld = "world";
const char *kKindInstance = "instance";

/// 「还原之前自动做的那一份」在文件名里那段记号（`backupFileName(…, safety=true)` 把它
/// 插在前缀和时间戳中间）。**一处定义**：`nameIsSafety()`（认人）与
/// `sortBackupsNewestFirst()`（排序）都得认它 —— 两边各写一份的话，哪天这个记号变了，
/// 会变成"排在前面"和"认成安全的"两件事。
const QString kSafetyMark = QCoreApplication::translate("BackupFiles", "恢复前-");

/// 还原之前对着空闲空间判"放不放得下"时，在清单里那些字节之外再留的**固定**余量。
/// 宁可留大一点：留多了什么也没浪费（还原完就还回去了），留少了就是"判够了、解到一半
/// 盘满"——那正是加这一眼要避免的那件事（文件的元数据、簇对齐、临时文件都吃空间）。
constexpr qint64 kSpaceHeadroomBytes = 16LL * 1024 * 1024;

/// 除了上面那点固定余量，再按总量多留这么一份（百分比）。大存档按比例留才够用，
/// 小存档靠那 16 MB 兜着。
constexpr qint64 kSpaceHeadroomPercent = 5;

bool isLinkToElsewhere(const QFileInfo &info) {
    return info.isJunction() || info.isSymLink();
}

/// 这棵树里第一条链接的**绝对路径**；一个链接都没有就回空串。**只认链接本身、不跟进去**
/// （理由和 `isLinkToElsewhere()` 那段一样：跟进去就可能转不完、或者跑到树外面去）。
///
/// 调用方是 `restoreBackup()`：还原会把链接**本身**摘掉、再按包里那份普通目录解出来，
/// 而包里与安全快照里都**从来没有**链接这一条 —— 所以见到链接必须停下来问清楚之前
/// 什么都不做（`collectFiles()` 跳过链接是备份那一侧有意为之，不是这里能补回来的）。
QString firstLinkInside(const QString &rootDir) {
    const QFileInfo rootInfo(rootDir);
    if (!rootInfo.exists()) {
        return QString();
    }
    if (isLinkToElsewhere(rootInfo)) {
        return rootInfo.absoluteFilePath();  // 根本身就是个链接
    }
    QStringList stack{rootInfo.absoluteFilePath()};
    QSet<QString> seen;
    while (!stack.isEmpty()) {
        const QString dir = stack.takeLast();
        const QString key = dir.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        // 文件那一边也要看：指向文件的符号链接在还原时同样会被换成普通文件
        // （`removeTreeWithoutFollowingLinks()` 摘的是链接本身）。
        const QFileInfoList entries = QDir(dir).entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (isLinkToElsewhere(entry)) {
                return entry.absoluteFilePath();
            }
            if (entry.isDir()) {
                stack.append(entry.absoluteFilePath());
            }
        }
    }
    return QString();
}

/// 一份文件的 sha1（小写十六进制）。读不动时回空串 —— **调用方必须把空串当失败**，
/// 不能当成"这个文件的哈希是空的"（那样这一份备份就永远对得了账了）。
///
/// NOTE: 走 `addData(QIODevice*)` 是**流式**的，不是 `readAll()`。一个存档的 region 文件
/// 几十 MB、一个实例快照里有几百 MB 的 jar —— 整份读到内存里只是为了让哈希函数看
/// 一遍，是不必要的峰值。
QString sha1OfFile(const QString &path, const std::function<bool()> &cancelled = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd()) {
        if (cancelled && cancelled()) return {};
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty()) return {};
        hash.addData(bytes);
    }
    if (file.error() != QFile::NoError || (cancelled && cancelled())) return {};
    return QString::fromLatin1(hash.result().toHex());
}

/// 文件名里那个词能不能当文件名用（Windows 不认 `\/:*?"<>|` 和控制字符，
/// 结尾的点和空格也会被悄悄吃掉 —— 吃掉之后两次备份可能撞成同一个名字）。
QString safeLabel(const QString &label) {
    QString out;
    out.reserve(label.size());
    for (const QChar ch : label) {
        const bool bad = ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')
                         || ch == QLatin1Char('*') || ch == QLatin1Char('?') || ch == QLatin1Char('"')
                         || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('|')
                         || ch < QChar(0x20);
        out.append(bad ? QLatin1Char('-') : ch);
    }
    out = out.trimmed();
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        out = QStringLiteral("backup");
    }
    if (out.size() > 80) {
        out = out.left(80);
    }
    return out;
}

QString kindKey(Kind kind) {
    return QString::fromLatin1(kind == Kind::Instance ? kKindInstance : kKindWorld);
}

/// 字符串数组那一小段（范围 / 排除表），空的项一律不记 —— 空串进清单只会让读回来的人
/// 多一条"这一条是什么意思"的疑问。
QStringList stringListOf(const QJsonValue &value) {
    QStringList out;
    const QJsonArray array = value.toArray();
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        const QString text = item.toString();
        if (!text.isEmpty()) {
            out.append(text);
        }
    }
    return out;
}

/// 这一串能不能原样当一个**目录名**用（还原的落点是 `targetParentDir + "/" + root` 拼出来的）。
///
/// 只挡 `/`、`\`、`.`、`..` 是不够的：Windows 在打开路径时会把每一段
/// **末尾的空格和点折掉** —— `".. "`、`". "`、`"..."`、`"a. "` 全都解析回上一层 / 本层
/// 目录。于是 `target = 父目录/".. "` 指向的就是父目录本身：安全快照照做（把父目录整个
/// 收进去）、`isDir()` 为真、整棵父目录被删掉，再把包里那一份解回来，最后报「还原完成」。
/// 所以末尾的空格 / 点一律不认；保留设备名（CON / NUL / COM1 …）同样不认。
bool isSafeComponent(const QString &name) {
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return false;
    }
    for (const QChar &character : name) {
        const ushort code = character.unicode();
        if (code < 0x20 || character == QLatin1Char('/') || character == QLatin1Char('\\') ||
            character == QLatin1Char(':') || character == QLatin1Char('<') ||
            character == QLatin1Char('>') || character == QLatin1Char('"') ||
            character == QLatin1Char('|') || character == QLatin1Char('?') ||
            character == QLatin1Char('*')) {
            return false;
        }
    }
    // 保留设备名：`CON`、`NUL`、`COM1`…，带扩展名也一样（`CON.txt` 仍是设备）。
    const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QStringList reserved{
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return !reserved.contains(stem);
}

}  // namespace

bool Manifest::isValid() const {
    if (root.isEmpty() || !createdAt.isValid()) {
        return false;
    }
    // 前缀只能是**一个目录名**。还原时落点是 `targetParentDir + "/" + root` 拼出来的，
    // 里面带上 `/`、`..` 或者盘符就能把文件写到别的地方去 —— 我们自己写的包不会这样，
    // 但"拿一个别人的包来还原"是用户能做的事，所以读回来的那一刻就得挡住。
    // 判据收在 `isSafeComponent()` 一处（末尾空格/点、保留设备名那几类
    // Windows 会自己折掉 / 换掉的写法也在那儿挡）。
    return isSafeComponent(root);
}

QString manifestEntryName() {
    return QStringLiteral("wtfml-backup.json");
}

QString formatTag() {
    return QString::fromLatin1(kFormatTag);
}

QByteArray manifestToJson(const Manifest &manifest) {
    QJsonObject root;
    root.insert(QStringLiteral("format"), formatTag());
    root.insert(QStringLiteral("kind"), kindKey(manifest.kind));
    root.insert(QStringLiteral("root"), manifest.root);
    root.insert(QStringLiteral("label"), manifest.label);
    // 要求的四样：来源实例 id、时间、游戏版本、包含范围。校验值在下面 `files` 里。
    root.insert(QStringLiteral("sourceInstanceId"), manifest.sourceInstanceId);
    root.insert(QStringLiteral("sourceInstanceName"), manifest.sourceInstanceName);
    root.insert(QStringLiteral("gameVersion"), manifest.gameVersion);
    root.insert(QStringLiteral("createdAt"), manifest.createdAt.toString(Qt::ISODate));
    root.insert(QStringLiteral("scope"), QJsonArray::fromStringList(manifest.scope));
    if (!manifest.worldName.isEmpty()) {
        root.insert(QStringLiteral("worldName"), manifest.worldName);
    }
    // `complete` / `excluded` 只有实例快照有意义：世界备份无从"排除"起，写一个
    // 恒为 false 的字段只会让读的人以为它是个开关。
    if (manifest.kind == Kind::Instance) {
        root.insert(QStringLiteral("complete"), manifest.complete);
        root.insert(QStringLiteral("excluded"), QJsonArray::fromStringList(manifest.excluded));
    }
    if (manifest.safety) {
        root.insert(QStringLiteral("safety"), true);
    }
    root.insert(QStringLiteral("fileCount"), manifest.fileCount());
    root.insert(QStringLiteral("totalBytes"), manifest.totalBytes);

    QJsonArray files;
    for (const FileRecord &record : manifest.files) {
        QJsonObject item;
        item.insert(QStringLiteral("path"), record.path);
        item.insert(QStringLiteral("size"), record.size);
        item.insert(QStringLiteral("sha1"), record.sha1);
        files.append(item);
    }
    root.insert(QStringLiteral("files"), files);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

Manifest manifestFromJson(const QByteArray &bytes, bool *ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return Manifest();
    }
    const QJsonObject root = doc.object();
    // 第一道判据是那个**写死的**格式标记，而不是"有没有哪个字段"。以后改格式时，
    // 旧包会在这里被挡住（"不是我们这一版做的"），而不是半信半疑地往里解。
    if (root.value(QStringLiteral("format")).toString() != formatTag()) {
        return Manifest();
    }

    Manifest manifest;
    const QString kind = root.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String(kKindWorld)) {
        manifest.kind = Kind::World;
    } else if (kind == QLatin1String(kKindInstance)) {
        manifest.kind = Kind::Instance;
    } else {
        return Manifest();  // 认不出的档 → 整份不认（宁可说"读不动"，也不猜）
    }

    manifest.root = root.value(QStringLiteral("root")).toString();
    manifest.label = root.value(QStringLiteral("label")).toString();
    manifest.sourceInstanceId = root.value(QStringLiteral("sourceInstanceId")).toString();
    manifest.sourceInstanceName = root.value(QStringLiteral("sourceInstanceName")).toString();
    manifest.gameVersion = root.value(QStringLiteral("gameVersion")).toString();
    manifest.worldName = root.value(QStringLiteral("worldName")).toString();
    manifest.complete = root.value(QStringLiteral("complete")).toBool(false);
    manifest.safety = root.value(QStringLiteral("safety")).toBool(false);
    manifest.createdAt = QDateTime::fromString(root.value(QStringLiteral("createdAt")).toString(),
                                               Qt::ISODate);
    manifest.scope = stringListOf(root.value(QStringLiteral("scope")));
    manifest.excluded = stringListOf(root.value(QStringLiteral("excluded")));

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    manifest.files.reserve(files.size());
    for (const QJsonValue &value : files) {
        const QJsonObject item = value.toObject();
        FileRecord record;
        record.path = item.value(QStringLiteral("path")).toString();
        if (record.path.isEmpty()) {
            continue;  // 没名字的条目落不了地，直接当它不存在
        }
        record.size = item.value(QStringLiteral("size")).toInteger(0);
        record.sha1 = item.value(QStringLiteral("sha1")).toString();
        manifest.files.append(record);
    }

    // `totalBytes` 以数出来的为准：清单是"记录"，不是"凭据" —— 有人手改过这个数的话，
    // 界面上的"共 N MB"不该跟着错。`files` 空的时候保留清单里那个数（这时它是唯一的说法）。
    if (!manifest.files.isEmpty()) {
        qint64 total = 0;
        for (const FileRecord &record : manifest.files) {
            total += record.size;
        }
        manifest.totalBytes = total;
    } else {
        manifest.totalBytes = root.value(QStringLiteral("totalBytes")).toInteger(0);
    }

    if (!manifest.isValid()) {
        return Manifest();
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return manifest;
}

QString backupRoot() {
    return AppPaths::backupDir();
}

QString backupDir(Kind kind) {
    return backupRoot() + (kind == Kind::Instance ? QStringLiteral("/instances")
                                                  : QStringLiteral("/worlds"));
}

QString backupFilePrefix(const QString &label) {
    return safeLabel(label) + QLatin1Char('-');
}

QString backupFileName(const QString &label, const QDateTime &at, bool safety) {
    QString name = backupFilePrefix(label);
    if (safety) {
        // 「恢复前」夹在中间而不是放在尾巴上：这一份是**自动**做出来的，摆前面一点是为了
        // 在按名字排序的列表里一眼看得见（它的时间戳和用户点的那一份几乎一样，只看时间
        // 排不出先后）。
        name += QCoreApplication::translate("BackupFiles", "恢复前-");
    }
    name += at.toString(QStringLiteral("yyyy-MM-dd-HHmmss"));
    name += QStringLiteral(".zip");
    return name;
}

QStringList sortBackupsNewestFirst(const QStringList &paths, const QString &prefix) {
    const QString &safetyMark = kSafetyMark;
    struct Key {
        QString stamp;   // 名字里那串时间戳（不含「恢复前-」和 `.zip`）
        bool stamped = false;
        bool safety = false;
    };
    const auto keyOf = [&prefix, &safetyMark](const QString &path) {
        Key key;
        // 前缀长度是定死的（label + '-'），大小写不敏感匹配也不影响截取位置。
        QString rest = QFileInfo(path).fileName().mid(prefix.size());
        key.safety = rest.startsWith(safetyMark);
        if (key.safety) rest = rest.mid(safetyMark.size());
        key.stamped = !rest.isEmpty() && rest.at(0).isDigit();
        key.stamp = rest;
        return key;
    };
    QStringList sorted = paths;
    std::sort(sorted.begin(), sorted.end(), [&keyOf](const QString &a, const QString &b) {
        const Key aKey = keyOf(a);
        const Key bKey = keyOf(b);
        // 认得出时间戳的排前面；两边都认得出才按时间戳从新到旧。
        if (aKey.stamped != bKey.stamped) return aKey.stamped;
        if (aKey.stamped && aKey.stamp != bKey.stamp) return aKey.stamp > bKey.stamp;
        // 同一秒（或者都认不出）：自动留下的那一份在前，剩下的按名字倒序。
        if (aKey.safety != bKey.safety) return aKey.safety;
        return a > b;
    });
    return sorted;
}

QString sizeText(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(
            QString::number(double(bytes) / (1024.0 * 1024 * 1024), 'f', 2));
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(QString::number(double(bytes) / (1024.0 * 1024), 'f', 1));
    }
    return QStringLiteral("%1 KB").arg(QString::number(double(bytes) / 1024.0, 'f', 1));
}

bool nameIsSafety(const QString &fileName, const QString &prefix) {
    return fileName.mid(prefix.size()).startsWith(kSafetyMark);
}

QList<FileRecord> collectFiles(const QString &rootDir, const QStringList &excludeTop,
                               QString *problemOut, const std::function<bool()> &cancelled) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (!QFileInfo(rootDir).isDir()) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("找不到这个目录：%1").arg(QDir::toNativeSeparators(rootDir));
        }
        return QList<FileRecord>();
    }

    QList<FileRecord> files;
    struct Frame {
        QString abs;
        QString prefix;  ///< 相对根的那一截，带尾斜杠；根自己是空串
    };
    QList<Frame> stack;
    const QString cleanRoot = QDir::cleanPath(rootDir);
    stack.append({cleanRoot, QString()});
    QSet<QString> seenDirs;  // 同一个目录（大小写/`.` 这类差别）只走一遍

    while (!stack.isEmpty()) {
        if (cancelled && cancelled()) {
            if (problemOut) *problemOut = QObject::tr("备份扫描已取消或超时。");
            return {};
        }
        const Frame frame = stack.takeLast();
        const QString key = frame.abs.toLower();
        if (seenDirs.contains(key)) {
            continue;
        }
        seenDirs.insert(key);

        const QDir dir(frame.abs);
        // 和 `ModpackExport::collectInstanceFiles()` 同一组 flags：隐藏文件**要收**
        // （存档里 `.gitignore`、加载器写的状态文件都带点，"名字带不带点"不该决定
        // 它进不进备份），目录**不跟链接**（见 isLinkToElsewhere）—— 一个指回祖先的
        // junction 能让这圈枚举转不完，指向实例外面的会把别人的东西打进包。文件那一边
        // 跟着链接（一个指向文件的符号链接就是一份文件，ModpackExport 那边同一取舍）。
        const QFileInfoList plainFiles =
            dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
        for (const QFileInfo &entry : plainFiles) {
            FileRecord record;
            record.path = frame.prefix + entry.fileName();
            record.size = entry.size();
            record.sha1 = sha1OfFile(entry.absoluteFilePath(), cancelled);
            if (record.sha1.isEmpty()) {
                // 读不动就是读不动 —— 不能记一条没有校验值的记录假装它在，那样还原之后的
                // 对账反而会**放过**这个文件（"空校验值 = 不查"）。
                if (problemOut != nullptr) {
                    *problemOut = cancelled && cancelled()
                        ? QObject::tr("备份扫描已取消或超时。")
                        : QObject::tr("读取文件失败：%1")
                                      .arg(QDir::toNativeSeparators(entry.absoluteFilePath()));
                }
                return QList<FileRecord>();
            }
            files.append(record);
        }

        const QFileInfoList subdirs = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden, QDir::Name);
        for (const QFileInfo &entry : subdirs) {
            if (isLinkToElsewhere(entry)) {
                continue;
            }
            const QString name = entry.fileName();
            // 排除**只看顶层**：`assets` / `libraries` 是实例根下面那两份，别处的同名目录
            // （某个模组自己 data 里的 assets）是**要**备份的 —— 按名字全局排除会把它们
            // 悄悄漏掉，而"少了一个模组的资源"是那种几年之后才会有人发现的事。
            if (frame.prefix.isEmpty() && excludeTop.contains(name, Qt::CaseInsensitive)) {
                continue;
            }
            stack.append({entry.absoluteFilePath(), frame.prefix + name + QLatin1Char('/')});
        }
    }

    // 按路径排序：清单要**可复现**（同一份内容打两次包，清单逐字一样）——
    // 目录枚举顺序是文件系统的自由，不排的话连"两次录屏逐帧比"这种事都做不了。
    std::sort(files.begin(), files.end(),
              [](const FileRecord &a, const FileRecord &b) { return a.path < b.path; });
    return files;
}

WriteResult writeBackup(const Manifest &manifest, const QString &sourceDir,
                        const QString &targetZip, const std::function<bool()> &cancelled) {
    WriteResult result;
    result.zipPath = targetZip;
    result.manifest = manifest;
    const QString gameRoot = InstanceFiles::canonicalPath(AppPaths::minecraftDir());
    if (!DirectorySessionLock::acquire(gameRoot, &result.problem)) return result;

    if (manifest.root.isEmpty()) {
        result.problem = QObject::tr("这一份没说清自己是什么（缺少顶层前缀），没有打包。");
        return result;
    }

    QString problem;
    result.manifest.files = collectFiles(sourceDir, manifest.excluded, &problem, cancelled);
    if (!problem.isEmpty()) {
        result.problem = problem;
        result.manifest.files.clear();
        return result;
    }
    if (result.manifest.files.isEmpty()) {
        result.problem = QObject::tr("这个目录是空的，没什么可备份的：%1")
                             .arg(QDir::toNativeSeparators(sourceDir));
        return result;
    }

    qint64 total = 0;
    for (const FileRecord &record : result.manifest.files) {
        total += record.size;
    }
    result.manifest.totalBytes = total;
    if (!result.manifest.createdAt.isValid()) {
        result.manifest.createdAt = QDateTime::currentDateTime();
    }

    // Freeze the bytes before invoking the external archiver. Hashing the live source
    // and then archiving it directly can publish a backup that cannot pass restore.
    const QString outputParent = QFileInfo(targetZip).absolutePath();
    if (!QDir().mkpath(outputParent)) {
        result.problem = QObject::tr("无法创建备份目录：%1").arg(outputParent);
        return result;
    }
    OwnedStaging::Lease stagingLease;
    QTemporaryDir staged(QDir(outputParent).filePath(QStringLiteral(".wtfml-backup-XXXXXX")));
    if (!staged.isValid()) {
        result.problem = QObject::tr("无法创建备份暂存目录，请检查磁盘空间和写入权限。");
        return result;
    }
    if (!stagingLease.attach(gameRoot, InstanceFiles::canonicalPath(staged.path()), QStringLiteral("backup"))) {
        result.problem = QObject::tr("无法登记备份暂存的所有权，未继续打包。");
        return result;
    }
    const QStorageInfo storage(outputParent);
    if (!hasRoomFor(storage.isValid() && storage.isReady() ? storage.bytesAvailable() : -1,
                    restoreSpaceNeeded(result.manifest) * 2)) {
        result.problem = QObject::tr("备份磁盘空间不足，需要同时容纳暂存副本和压缩包。");
        return result;
    }
    const QDir snapshot(QDir(staged.path()).filePath(QStringLiteral("data")));
    for (const FileRecord &record : result.manifest.files) {
        const QString dest = snapshot.filePath(record.path);
        QFile input(QDir(sourceDir).filePath(record.path));
        QFile output(dest);
        if (!QDir().mkpath(QFileInfo(dest).absolutePath()) || !input.open(QIODevice::ReadOnly)
            || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            result.problem = QObject::tr("无法暂存备份文件：%1；请检查文件占用、权限和磁盘空间。")
                                 .arg(record.path);
            return result;
        }
        QCryptographicHash hash(QCryptographicHash::Sha1);
        qint64 copied = 0;
        while (!input.atEnd()) {
            if (cancelled && cancelled()) {
                result.problem = QObject::tr("备份暂存已取消或超时。");
                return result;
            }
            const QByteArray bytes = input.read(1024 * 1024);
            if (bytes.isEmpty() || output.write(bytes) != bytes.size()) {
                result.problem = QObject::tr("备份文件读取或写入不完整：%1；请检查磁盘空间和文件占用。")
                                     .arg(record.path);
                return result;
            }
            copied += bytes.size();
            hash.addData(bytes);
        }
        if (input.error() != QFile::NoError || !output.flush()) {
            result.problem = QObject::tr("备份文件读写失败：%1").arg(record.path);
            return result;
        }
        if (copied != record.size || QString::fromLatin1(hash.result().toHex()) != record.sha1) {
            result.problem = QObject::tr("备份期间文件发生变化：%1。请暂停游戏或同步后重试，未发布备份。")
                                 .arg(record.path);
            return result;
        }
    }
    // Also catch added/deleted files and edits to files already copied. The archive
    // subsequently reads only the private snapshot, never the changing source.
    const auto after = collectFiles(sourceDir, manifest.excluded, &problem, cancelled);
    bool unchanged = problem.isEmpty() && after.size() == result.manifest.files.size();
    for (qsizetype i = 0; unchanged && i < after.size(); ++i) {
        const auto &before = result.manifest.files.at(i);
        unchanged = after.at(i).path == before.path && after.at(i).size == before.size
                    && after.at(i).sha1 == before.sha1;
    }
    if (!unchanged || (cancelled && cancelled())) {
        result.problem = !problem.isEmpty() ? problem
            : QObject::tr("备份期间目录发生变化或操作已取消，请暂停游戏或同步后重试，未发布备份。");
        return result;
    }

    // 清单先落一份**临时文件**：`ZipUtil::createZip` 的条目表只能指向盘上真实的文件
    // （整张表走临时文件递进 PowerShell，理由见 ZipUtil.hpp 里 createZip 那段 ——
    // Windows 一条命令行只有 32767 个字符）。
    //
    // 这里**不能用 `QTemporaryFile`**（产品级的错）：
    // 它建出来的文件在**那个对象还活着的时候别的进程读不了** —— 连 `close()` 过之后
    // 也一样，.NET 那边拿到的是「The process cannot access the file ... because it is
    // being used by another process」。而打包这件事**正是**"另一个进程（PowerShell）
    // 去读这个文件"，所以只要走 QTemporaryFile，这一趟必然整份失败：清单加不进去 ⇒
    // zip 一个条目都没有 ⇒ 用户在「立即备份」上看到的是一句和自己的盘毫无关系的话。
    // 用普通 QFile + 一个不会撞的名字（和 `ZipUtil::createZip` 递条目表那张临时文件
    // 同一个写法），写进一个作用域里让它在打包之前就关掉，成功失败都删掉那一份。
    const QString manifestPath = QDir(staged.path()).filePath(QStringLiteral("manifest.json"));
    const QByteArray manifestBytes = manifestToJson(result.manifest);
    {
        QFile manifestFile(manifestPath);
        if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            result.problem = QObject::tr("无法创建临时文件以保存备份清单：%1")
                                 .arg(QDir::toNativeSeparators(manifestPath));
            return result;
        }
        // 短写（盘刚好在半路满）也算失败：半截 JSON 装进包里，读回来时是"清单读不动"，
        // 而那句话说不到这个真因上。
        if (manifestFile.write(manifestBytes) != manifestBytes.size() || !manifestFile.flush()) {
            result.problem = QObject::tr("备份清单写入不完整：%1")
                                 .arg(QDir::toNativeSeparators(manifestPath));
            manifestFile.close();
            QFile::remove(manifestPath);
            return result;
        }
    }

    QList<ZipUtil::ZipSource> entries;
    ZipUtil::ZipSource manifestEntry;
    manifestEntry.entryName = manifestEntryName();
    manifestEntry.sourcePath = manifestPath;
    entries.append(manifestEntry);

    const QDir source(snapshot);
    for (const FileRecord &record : result.manifest.files) {
        ZipUtil::ZipSource entry;
        entry.entryName = result.manifest.root + QLatin1Char('/') + record.path;
        entry.sourcePath = source.filePath(record.path);
        entries.append(entry);
    }

    QString error;
    // **同一秒里打两份的时候名字是一样的**（`backupFileName()` 只精确到秒），而发布那一步
    // 是 `QFile::rename(pending, target)` —— 目标已经在了就发不出去。连着点两下「立即备份」，
    // 或者"还原前的安全快照"正好和用户点的那一份同秒，不先找名字就会撞在这一步上（用户看到
    // 的是"无法发布"，而前一份其实好好的）。所以落盘之前先找一个**没人占**的名字，并把实际
    // 路径写回 `result.zipPath`（调用方弹窗和日志读的都是它，不是原来那个）。
    QString target = targetZip;
    if (QFileInfo::exists(target)) {
        const QFileInfo info(target);
        const QString suffix = info.suffix();
        const QString stem = suffix.isEmpty() ? info.fileName()
                                              : info.fileName().chopped(suffix.size() + 1);
        for (int n = 2; QFileInfo::exists(target) && n < 1000; ++n) {
            target = QDir(info.absolutePath())
                         .filePath(suffix.isEmpty() ? QStringLiteral("%1-%2").arg(stem).arg(n)
                                                    : QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(suffix));
        }
        if (QFileInfo::exists(target)) {
            result.problem = QObject::tr("同名的备份太多了，换一个时间再试：%1")
                                 .arg(QDir::toNativeSeparators(targetZip));
            QFile::remove(manifestPath);
            return result;
        }
    }
    result.zipPath = target;
    const QString pending = QDir(staged.path()).filePath(QStringLiteral("archive.pending"));
    const bool made = ZipUtil::createZip(pending, entries, &error, cancelled);
    // 清单也放在已登记暂存内，崩溃后不在系统临时目录遗留文件清单。
    QFile::remove(manifestPath);
    if (!made) {
        result.problem = error.isEmpty() ? QObject::tr("打包没有成功。") : error;
        return result;
    }

    if ((cancelled && cancelled()) || !QFile::rename(pending, target)) {
        result.problem = QObject::tr("备份已取消，或无法发布备份文件（目标被占用、已存在或磁盘写入失败），未覆盖已有备份。");
        return result;
    }

    result.ok = true;
    return result;
}

Manifest readManifest(const QString &zipPath, bool *ok, QString *problemOut) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (problemOut != nullptr) {
        problemOut->clear();
    }

    QByteArray bytes;
    QString error;
    const ZipUtil::EntryStatus status =
        ZipUtil::readEntry(zipPath, manifestEntryName(), &bytes, &error,
                           QObject::tr("读取备份清单"));
    // 「不是我们做的包」和「读不动」在这个 switch 里是**两句不同的话**：
    // 前者是用户拿错了文件（或者那个包是别的启动器做的），后者是这个文件本身坏了。
    // 合成一句的话，用户会去怀疑一个其实好好的文件。
    switch (status) {
    case ZipUtil::EntryStatus::NotFound:
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("未找到备份元数据（%1），可能非本启动器生成；请手动解压到实例目录。")
                              .arg(manifestEntryName());
        }
        return Manifest();
    case ZipUtil::EntryStatus::NotAnArchive:
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("文件无法作为压缩包打开。请检查格式及完整性。");
        }
        return Manifest();
    case ZipUtil::EntryStatus::Failed:
        if (problemOut != nullptr) {
            *problemOut = error.isEmpty() ? QObject::tr("读取备份清单失败。") : error;
        }
        return Manifest();
    case ZipUtil::EntryStatus::Ok:
        break;
    }

    bool parsed = false;
    const Manifest manifest = manifestFromJson(bytes, &parsed);
    if (!parsed) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("无法读取备份清单。请确认备份格式受当前版本支持。");
        }
        return Manifest();
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return manifest;
}

qint64 restoreSpaceNeeded(const Manifest &manifest) {
    qint64 sum = 0;
    for (const FileRecord &record : manifest.files) {
        if (record.size > 0) {
            sum += record.size;
        }
    }
    return sum + qMax(kSpaceHeadroomBytes, sum * kSpaceHeadroomPercent / 100);
}

bool hasRoomFor(qint64 freeBytes, qint64 neededBytes) {
    if (freeBytes < 0) {
        return true;  // 问不出来 ⇒ 不拦（见头文件那一句）
    }
    return freeBytes >= neededBytes;
}

RestoreResult restoreBackup(const QString &zipPath, const QString &targetParentDir,
                            const std::function<bool()> &cancelled,
                            const RestoreObserver &observer) {
    RestoreResult result;
    const QString gameRoot = InstanceFiles::canonicalPath(AppPaths::minecraftDir());
    QString directoryProblem;
    if (!DirectorySessionLock::acquire(gameRoot, &directoryProblem)) {
        result.stage = RestoreResult::Stage::ReadManifest;
        result.problem = directoryProblem;
        return result;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const auto stopped = [&] { return elapsed.elapsed() >= 900000 || (cancelled && cancelled()); };
    const QString stoppedText =
        QObject::tr("还原已取消或超时，原有文件已保留。");

    if (stopped()) {
        result.stage = RestoreResult::Stage::ReadManifest;
        result.problem = stoppedText;
        return result;
    }

    bool readable = false;
    QString problem;
    const Manifest manifest = readManifest(zipPath, &readable, &problem);
    if (!readable) {
        result.stage = RestoreResult::Stage::ReadManifest;
        result.problem = problem;
        return result;
    }

    const QString target = QDir(targetParentDir).filePath(manifest.root);
    // 双保险：落点必须正好是父目录底下的**那一层**。`Manifest::isValid()` 那一头已经
    // 把奇怪的名字挡住了，这里再问一遍拼出来的路径最后一段还是不是 `root` —— 下面几步
    // 会替换这个路径，拼错一层就会动到别人的东西。
    if (QFileInfo(target).fileName().compare(manifest.root, Qt::CaseInsensitive) != 0) {
        result.stage = RestoreResult::Stage::ReadManifest;
        result.problem = QObject::tr("找不到备份记录中的目标目录（%1），未解压备份。")
                             .arg(manifest.root);
        return result;
    }
    result.targetPath = target;

    // ② 新旧目录会同时存在，因此即使原地还原也必须预留解压空间。
    // 预检在安全快照之前，不通过时不改动目标内容。
    if (stopped()) {
        result.stage = RestoreResult::Stage::Space;
        result.problem = stoppedText;
        return result;
    }
    const QFileInfo existing(target);
    {
        const qint64 free = QStorageInfo(targetParentDir).bytesAvailable();
        const qint64 needed = restoreSpaceNeeded(manifest);
        if (!hasRoomFor(free, needed)) {
            result.stage = RestoreResult::Stage::Space;
            result.problem = QObject::tr("目标磁盘空间不足。解压备份预计需要 %1，可用空间为 %2。\n请释放空间，或将备份还原到其他磁盘。\n原有文件已保留。")
                                 .arg(QLocale().formattedDataSize(needed),
                                      QLocale().formattedDataSize(free));
            return result;
        }
    }

    if (existing.exists() && !existing.isDir()) {
        result.stage = RestoreResult::Stage::RemoveOld;
        result.problem = QObject::tr("还原位置存在同名文件，未覆盖或删除：%1").arg(target);
        return result;
    }

    // 落点树里**有链接**（junction / 符号链接）时拒绝还原。
    //
    // `collectFiles()` 不跟链接 —— 备份包里因此**从来没有**链接这一条，还原前的安全
    // 快照同样不含它。而下面 `removeTreeWithoutFollowingLinks()` 会把链接**本身**摘掉，
    // 再按包里那份普通目录解出来：链接指向的内容不会丢（它不在这个树里），但
    // "这个目录是个链接"这件事就再也回不去了，安全快照也退不回来。
    //
    // 两条改法里选了"拒绝并说清"（另一条要在清单里多记一类链接条目，还得定义链接目标
    // 谁来保证存在、跨盘链接怎么处理）。这里停在**动盘之前**：一个字节都没动。
    if (existing.isDir()) {
        const QString link = firstLinkInside(target);
        if (!link.isEmpty()) {
            result.stage = RestoreResult::Stage::SafetySnapshot;
            result.problem = QObject::tr("目标中包含链接：%1\n\n还原会用普通目录替换链接，安全快照无法恢复链接本身。已取消还原，原有内容已保留。\n\n若要继续，请先移走链接，再重试。")
                                 .arg(QDir::toNativeSeparators(link));
            return result;
        }
    }

    // ③ 现在那儿已经有一份 → **先给它做一份安全快照**，再整个挪走。
    //
    // 顺序不能反，而且安全快照**做不成就不往下走**：先删掉旧的、再回来告诉用户
    // "刚才想给你留一份但是没留成"，是这一整套里唯一不可接受的结果。
    QList<FileRecord> originalFiles;
    if (existing.exists()) {
        if (existing.isDir()) {
            Manifest safety;
            safety.kind = manifest.kind;
            safety.root = manifest.root;
            safety.label = manifest.label;
            safety.sourceInstanceId = manifest.sourceInstanceId;
            safety.sourceInstanceName = manifest.sourceInstanceName;
            safety.gameVersion = manifest.gameVersion;
            safety.worldName = manifest.worldName;
            // 安全快照**必须完整**，不能把被还原那一份的 `excluded` /
            // `complete` 原样抄过来。发布会替换整个 target，不分 assets / libraries。
            // 照抄的话，用户还原一份普通快照
            // （excluded = assets、libraries）时，那两棵目录被删了、安全快照里也没有，
            // 而我们上一条还在跟用户说"这一步是能退回来的"。
            safety.complete = true;
            safety.excluded.clear();
            safety.safety = true;
            safety.createdAt = QDateTime::currentDateTime();
            safety.scope = QStringList{
                QObject::tr("还原之前自动给「现在这一份」做的完整安全快照")};

            const QString dir = backupDir(safety.kind);
            QDir().mkpath(dir);
            const QString safetyZip =
                QDir(dir).filePath(backupFileName(safety.label, safety.createdAt, /*safety=*/true));

            // 取消传进安全快照那一趟：它本身支持协作式停止（扫描 / 分块读取 / 打包）。
            const WriteResult snapshot = writeBackup(safety, target, safetyZip, stopped);
            if (!snapshot.ok) {
                // 落点是个**空目录**（上一次解压被杀在第一份文件之前、或者
                // 游戏 / 用户自己建了一个空存档目录）时没有"现在这一份"可留 ——
                // `writeBackup()` 会以「这个目录是空的，没什么可备份的」失败，而这里
                // 又要求"快照不成就不往下走" ⇒ 之后**每一次**还原都卡死在这一档，
                // 界面上只说"目录没被改动"，用户没有任何出口。
                // 空目录删掉本来就没有损失，所以这一种失败放行（别种失败照旧拦）。
                QString probeProblem;
                const QList<FileRecord> present = collectFiles(target, QStringList(), &probeProblem);
                if (!probeProblem.isEmpty() || !present.isEmpty()) {
                    result.stage = RestoreResult::Stage::SafetySnapshot;
                    result.problem = QObject::tr("未能为当前目录创建安全快照：%1\n还原已取消，原有内容已保留。")
                                         .arg(snapshot.problem);
                    return result;
                }
                // 空目录：不留安全快照，接着往下（`result.safetyZip` 保持空）。
            } else {
                // `zipPath` 才是**真正**落盘的那一个（同秒撞名时 `writeBackup()` 会换一个
                // 名字，见那里），不是我们上面按时间戳拼出来的候选名。
                result.safetyZip = snapshot.zipPath;
                originalFiles = snapshot.manifest.files;
            }
            // 安全快照做完了、旧目录**还没**动：这里仍然可以安全地停下。刚做出来的
            // 那份快照是这一趟自己的产物、还原并没有发生 —— 删掉它，别在备份列表里
            // 留下一份用户没要过的东西。盘上原有的东西一个字节都没动。
            if (stopped()) {
                if (!result.safetyZip.isEmpty()) {
                    QFile::remove(result.safetyZip);
                    result.safetyZip.clear();
                }
                result.stage = RestoreResult::Stage::SafetySnapshot;
                result.problem = stoppedText;
                return result;
            }
        }
    }

    // 解压前最后一个安全停止点；原目录仍在原位。
    if (!existing.exists() && stopped()) {
        result.stage = RestoreResult::Stage::Extract;
        result.problem = stoppedText;
        return result;
    }

    // ④ 解压。走 `extractSubtrees()` 是**故意的**：它把前缀剥掉，而且**不属于那个前缀的
    // 条目一个都不碰** —— zip 根上躺着的那份清单因此不会落到用户的世界目录里。
    //
    // 前缀剥掉后只写入私有 candidate，绝不直接向用户目录解压。
    QString error;
    if (!QDir().mkpath(targetParentDir)) {
        result.stage = RestoreResult::Stage::Extract;
        result.problem = QObject::tr("无法创建还原父目录，原数据未改变。");
        return result;
    }
    OwnedStaging::Lease stagingLease;
    QTemporaryDir staging(QDir(targetParentDir).filePath(QStringLiteral(".wtfml-restore-XXXXXX")));
    if (!staging.isValid()) {
        result.stage = RestoreResult::Stage::Extract;
        result.problem = QObject::tr("无法创建同卷还原暂存目录，原目录未改变。");
        return result;
    }
    const QString candidate = QDir(staging.path()).filePath(QStringLiteral("payload"));
    if (!stagingLease.attach(gameRoot, InstanceFiles::canonicalPath(staging.path()), QStringLiteral("restore"))) {
        result.stage = RestoreResult::Stage::Extract;
        result.problem = QObject::tr("无法登记还原暂存的所有权，原目录未替换。");
        return result;
    }
    if (!ZipUtil::extractSubtrees(QStringList{zipPath}, candidate,
                                  QStringList{manifest.root + QLatin1Char('/')}, nullptr, &error,
                                  stopped)) {
        result.stage = RestoreResult::Stage::Extract;
        QString message = QObject::tr("未完成解压：%1").arg(error);
        if (!result.safetyZip.isEmpty()) {
            message += QObject::tr("\n还原之前的「现在这一份」在：%1")
                           .arg(QDir::toNativeSeparators(result.safetyZip));
        }
        result.problem = message;
        return result;
    }

    // ⑤ 逐文件对账。**"文件在那儿"和"文件是对的"是两件事** —— 解压到一半被打断、盘写满，
    // 留下的都是"在，但不对"的文件，只有这一趟认得出。
    result.checked = manifest.fileCount();
    result.mismatched = verifyTree(candidate, manifest.files, stopped);
    if (stopped()) {
        result.stage = RestoreResult::Stage::Verify;
        result.problem = stoppedText;
        return result;
    }
    result.ok = result.mismatched == 0;
    if (!result.ok) {
        result.stage = RestoreResult::Stage::Verify;
        QString message = QObject::tr("解压结果与备份清单不一致（%1 / %2 个文件）。")
                              .arg(result.mismatched)
                              .arg(result.checked);
        if (!result.safetyZip.isEmpty()) {
            message += QObject::tr("\n还原之前的「现在这一份」在：%1")
                           .arg(QDir::toNativeSeparators(result.safetyZip));
        }
        result.problem = message;
        return result;
    }
    result.ok = false; // Validation alone is not a successful publication.
    if (stopped()) {
        result.stage = RestoreResult::Stage::Verify;
        result.problem = stoppedText;
        return result;
    }
    // The safety snapshot must still describe the live target before replacement.
    // Never discard changes made by a game or sync client during extraction.
    const QFileInfo current(target);
    if (current.exists() != existing.exists() || current.isSymLink() || current.isJunction()
        || (!firstLinkInside(target).isEmpty())) {
        result.stage = RestoreResult::Stage::RemoveOld;
        result.problem = QObject::tr("还原期间目标目录或链接发生变化，原目录未替换，请暂停游戏或同步后重试。");
        return result;
    }
    if (existing.exists()) {
        QString probeProblem;
        const auto live = collectFiles(target, {}, &probeProblem, stopped);
        bool same = probeProblem.isEmpty() && live.size() == originalFiles.size();
        for (qsizetype i = 0; same && i < live.size(); ++i) {
            same = live.at(i).path == originalFiles.at(i).path
                   && live.at(i).size == originalFiles.at(i).size
                   && live.at(i).sha1 == originalFiles.at(i).sha1;
        }
        if (!same || stopped()) {
            result.stage = RestoreResult::Stage::RemoveOld;
            result.problem = QObject::tr("还原期间原文件发生变化或操作取消，原目录未替换，请重试。");
            return result;
        }
    }
    // No cancellation between these renames: either publish, or put the original
    // back. The retained tree is OUTSIDE auto-removing staging, also on a crash.
    const QString normalizedTarget = InstanceFiles::canonicalPath(target);
    const QString retained = QDir(QFileInfo(normalizedTarget).absolutePath()).filePath(QStringLiteral(".wtfml-restore-previous-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString journalDir = RestoreJournal::directory(gameRoot);
    auto journal = RestoreJournal::record(normalizedTarget, retained, existing.exists());
    // Outside managed data roots, retain rename safety but do not authorize startup
    // recovery to touch arbitrary external folders using a persisted path.
    const bool managed = RestoreJournal::valid(gameRoot, journal);
    if (!managed && InstanceFiles::pathIsSameOrInside(gameRoot, normalizedTarget)) {
        result.stage = RestoreResult::Stage::RemoveOld;
        result.problem = QObject::tr("还原路径不适合安全登记事务，原目录未替换。");
        return result;
    }
    const QString journalPath = QDir(journalDir).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    QLockFile transactionLock(journalPath + QStringLiteral(".lock"));
    transactionLock.setStaleLockTime(0);
    if (managed && (!RestoreJournal::ordinaryPath(gameRoot, journalDir)
                    || !RestoreJournal::ordinaryPath(gameRoot, journalPath)
                    || !RestoreJournal::ordinaryPath(gameRoot, journalPath + QStringLiteral(".lock"))
                    || !QDir().mkpath(journalDir) || !transactionLock.tryLock(0)
                    || !RestoreJournal::write(journalPath, journal))) {
        result.stage = RestoreResult::Stage::RemoveOld;
        result.problem = QObject::tr("无法安全写入还原事务日志，原目录未替换。");
        return result;
    }
    if (observer) observer(RestorePhase::Prepared);
    if (existing.exists() && !QDir().rename(target, retained)) {
        if (managed) QFile::remove(journalPath);
        result.stage = RestoreResult::Stage::RemoveOld;
        result.problem = QObject::tr("无法挪开原目录（可能正被游戏占用），原文件未删除。");
        return result;
    }
    if (observer) observer(RestorePhase::OriginalRetained);
    if (!QDir().rename(candidate, target)) {
        result.stage = RestoreResult::Stage::RemoveOld;
        const bool recovered = !existing.exists() || QDir().rename(retained, target);
        result.recoveryFailed = !recovered;
        if (managed && recovered) QFile::remove(journalPath);
        result.problem = recovered ? QObject::tr("无法发布还原目录，已保留或恢复原目录。")
            : QObject::tr("无法发布且原目录未能自动归位；原文件完整保留在：%1").arg(retained);
        return result;
    }
    if (observer) observer(RestorePhase::Published);
    result.ok = true;
    journal.insert(QStringLiteral("state"), QStringLiteral("published"));
    if (managed && !RestoreJournal::write(journalPath, journal)) {
        // Do not delete the original without a durable publication marker.
        result.retainedPath = existing.exists() ? retained : QString();
        return result;
    }
    if (existing.exists() && !removeTreeWithoutFollowingLinks(retained)) {
        result.retainedPath = retained;
    }
    if (managed && result.retainedPath.isEmpty()) QFile::remove(journalPath);
    if (observer) observer(RestorePhase::Committed);
    return result;
}

QStringList recoverInterruptedRestores(const QString &gameDir) {
    QStringList messages;
    QString problem;
    const QString root = InstanceFiles::canonicalPath(gameDir);
    if (root.isEmpty() || !QFileInfo(root).isDir()) return messages;
    if (!DirectorySessionLock::acquire(root, &problem)) return {problem};
    const QString directory = RestoreJournal::directory(root);
    if (!RestoreJournal::ordinaryPath(root, directory))
        return {QObject::tr("还原事务目录含链接，未自动恢复。")};
    const QDir dir(directory);
    const auto names = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto &name : names.mid(0, 128)) {
        if (QUuid(QFileInfo(name).completeBaseName()).isNull()) continue;
        const QString path = dir.filePath(name);
        if (!RestoreJournal::ordinaryPath(root, path)
            || !RestoreJournal::ordinaryPath(root, path + QStringLiteral(".lock"))) continue;
        QLockFile lock(path + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) continue;
        const auto json = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (!RestoreJournal::valid(root, json)) {
            messages.append(QObject::tr("还原日志无效或路径不安全，未改动文件：%1").arg(path));
            continue;
        }
        const QString target = json.value(QStringLiteral("target")).toString();
        const QString retained = json.value(QStringLiteral("retained")).toString();
        const bool hasTarget = QFileInfo::exists(target);
        const bool hasOriginal = QFileInfo::exists(retained);
        if (json.value(QStringLiteral("state")).toString() == QLatin1String("prepared")
            && json.value(QStringLiteral("hadOriginal")).toBool()
            && !hasTarget && hasOriginal && QFileInfo(retained).isDir()) {
            if (QDir().rename(retained, target)) {
                QFile::remove(path);
                messages.append(QObject::tr("中断的还原已恢复原目录：%1").arg(target));
            } else messages.append(QObject::tr("原目录未能归位，仍完整保留在：%1").arg(retained));
        } else if (hasOriginal) {
            messages.append(QObject::tr("还原事务保留了旧目录，未覆盖现有内容，请检查：%1").arg(retained));
        } else if (hasTarget || !json.value(QStringLiteral("hadOriginal")).toBool()) {
            QFile::remove(path); // Own journal only; no directory deletion.
        } else messages.append(QObject::tr("还原事务的目标和旧目录均缺失，请检查日志：%1").arg(path));
    }
    messages += OwnedStaging::cleanup(root);
    return messages;
}

int verifyTree(const QString &dir, const QList<FileRecord> &files,
               const std::function<bool()> &cancelled) {
    int mismatched = 0;
    const QDir root(dir);
    for (const FileRecord &record : files) {
        if (cancelled && cancelled()) return qMax(1, int(files.size()));
        const QFileInfo info(root.filePath(record.path));
        if (!info.isFile()) {
            ++mismatched;  // 缺了
            continue;
        }
        if (info.size() != record.size) {
            ++mismatched;  // 长短就不对，不用算哈希了
            continue;
        }
        if (record.sha1.isEmpty()) {
            continue;  // 清单里本来就没记校验值（手改过的清单）—— 查不了，不算它错
        }
        if (sha1OfFile(info.absoluteFilePath(), cancelled) != record.sha1) {
            ++mismatched;  // 长度一样但内容不一样
        }
    }
    return mismatched;
}

QString scopeLine(const QString &what, int fileCount) {
    return QObject::tr("%1（%2 个文件）").arg(what).arg(fileCount);
}

}  // namespace BackupFiles
