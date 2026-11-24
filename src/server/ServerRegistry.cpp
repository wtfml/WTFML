/**
 * @file ServerRegistry.cpp
 * @brief `ServerRegistry` 的实现 —— 扫 `<数据目录>/servers`、读/写 `.wtfml-server` 与那两个
 * 配置文件。
 *
 * 这里**是**那一层碰盘的代码（见头文件的分工）：扫目录、读文件、写文件、建目录、删目录。
 * 判据（`eula.txt` 到底算不算接受了、`server.properties` 怎么解、这一档装好了没有）全在
 * `ServerModel` / `ServerInstall` 里，这一层一个判据都不重新发明。
 */

#include "server/ServerRegistry.hpp"
#include <QCoreApplication>
#include "server/ServerWriteGuard.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "manage/FolderManager.hpp"
#include "util/AppPaths.hpp"
#include "util/DirectorySessionLock.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/RestoreJournal.hpp"

namespace ServerRegistry {

namespace {

bool writableServer(const QString &id, QString *problem) {
    return ServerWriteGuard::acquire(id, problem);
}

/// 相对服务端目录、`/` 分隔的一条路径（判据层认的形状，见 `ServerModel::isInstalled()`）。
QString relPath(const QString &a, const QString &b, const QString &c = QString()) {
    QString path = a + QLatin1Char('/') + b;
    if (!c.isEmpty()) {
        path += QLatin1Char('/') + c;
    }
    return path;
}

/// 根目录下的文件名（**不含**子目录）。认 flavor、判"装好了没有"都用它。
QStringList rootFileNames(const QString &dir) {
    return QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
}

/**
 * @brief 扫出 Forge / NeoForge 那两张 `win_args.txt` 的**相对路径**。
 *
 * 只探两个**已知的**位置（安装器自己定的，见 `ServerInstall::planFor()` 里那段），
 * **不**去 walk 整个 `libraries/`：一个装好的 Forge 服务端的 libraries 底下有几千个文件，
 * 而这张参数文件的位置从第一个版本起就没变过。
 */
QStringList forgeArgsPaths(const QString &dir) {
    QStringList found;
    const QStringList probes{
        QStringLiteral("libraries/net/minecraftforge/forge"),
        QStringLiteral("libraries/net/neoforged/neoforge"),
    };
    for (const QString &probe : probes) {
        const QString probeDir = dir + QLatin1Char('/') + probe;
        const QStringList versions =
            QDir(probeDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &version : versions) {
            if (QFileInfo::exists(probeDir + QLatin1Char('/') + version
                                  + QStringLiteral("/win_args.txt"))) {
                found.append(relPath(probe, version, QStringLiteral("win_args.txt")));
            }
        }
    }
    return found;
}

/// 每一档"装好了没有"的判据。**和 `ServerInstall::Plan::installedMarkers` 同一条**
/// （那一位是按"要装的那一档"给的，这一位是照"盘上现在这个样子"认的 —— 用户从外面拖进来
/// 的服务端没有 Plan，只有文件）。@p missingOut 里回缺的那一样（没装好时才有）。
bool installedFor(ServerInstall::Flavor flavor, const QStringList &rootFiles,
                  const QStringList &forgeArgs, QString *missingOut) {
    const auto has = [&rootFiles](const QString &name) { return rootFiles.contains(name); };
    switch (flavor) {
    case ServerInstall::Flavor::Fabric:
        if (has(QStringLiteral("fabric-server-launch.jar"))) {
            return true;
        }
        if (missingOut != nullptr) {
            *missingOut = QStringLiteral("fabric-server-launch.jar");
        }
        return false;
    case ServerInstall::Flavor::Quilt:
        if (!has(QStringLiteral("quilt-server-launch.jar"))) {
            if (missingOut != nullptr) {
                *missingOut = QStringLiteral("quilt-server-launch.jar");
            }
            return false;
        }
        if (!has(QStringLiteral("server.jar"))) {
            if (missingOut != nullptr) {
                *missingOut = QStringLiteral("server.jar");
            }
            return false;
        }
        return true;
    case ServerInstall::Flavor::Forge:
    case ServerInstall::Flavor::NeoForge:
        if (forgeArgs.isEmpty()) {
            if (missingOut != nullptr) {
                *missingOut = QStringLiteral("libraries/…/win_args.txt");
            }
            return false;
        }
        if (!has(QStringLiteral("user_jvm_args.txt"))) {
            if (missingOut != nullptr) {
                *missingOut = QStringLiteral("user_jvm_args.txt");
            }
            return false;
        }
        return true;
    case ServerInstall::Flavor::Spigot:
        // 产物名带它自己的后缀（`spigot-<版本>-R0.1-SNAPSHOT.jar`），从游戏版本号拼不出来
        // ⇒ 只能扫（见 `ServerInstall::pickSpigotJar()`）。
        if (!ServerInstall::pickSpigotJar(rootFiles).isEmpty()) {
            return true;
        }
        if (missingOut != nullptr) {
            *missingOut = QStringLiteral("spigot-*.jar");
        }
        return false;
    default:
        // 原版 / Paper / Purpur 三档都叫 server.jar（`ServerInstall::planFor()` 里那三条）。
        if (has(QStringLiteral("server.jar"))) {
            return true;
        }
        if (missingOut != nullptr) {
            *missingOut = QStringLiteral("server.jar");
        }
        return false;
    }
}

/// 把一条路径里的 `/` 都当分隔符，回它是不是"严格在根底下"（删目录前那道闸用的）。
bool isStrictlyInside(const QString &rootPath, const QString &candidate) {
    const QString root = QDir::cleanPath(rootPath);
    const QString path = QDir::cleanPath(candidate);
    return path.size() > root.size() && path.startsWith(root + QLatin1Char('/'));
}

}  // namespace

// ---------------------------------------------------------------------------
// 根与列表
// ---------------------------------------------------------------------------

QString rootDir() { return AppPaths::serversDir(); }

bool ensureRoot(QString *problemOut) {
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), problemOut)) return false;
    const QString root = rootDir();
    if (QDir(root).exists()) {
        return true;
    }
    if (QDir().mkpath(root)) {
        return true;
    }
    if (problemOut != nullptr) {
        *problemOut = QCoreApplication::translate("ServerRegistry", "无法创建服务端目录：%1").arg(QDir::toNativeSeparators(root));
    }
    return false;
}

QStringList serverIds() {
    const QString root = rootDir();
    if (!QDir(root).exists()) {
        return QStringList();
    }
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

bool serverExists(const QString &serverId) {
    return !serverId.isEmpty() && QDir(AppPaths::serverDir(serverId)).exists();
}

// ---------------------------------------------------------------------------
// 扫一行 / 扫全部
// ---------------------------------------------------------------------------

Entry describe(const QString &serverId, QString *problemOut) {
    Entry entry;
    entry.serverId = serverId;
    entry.dirPath = AppPaths::serverDir(serverId);

    if (!QDir(entry.dirPath).exists()) {
        entry.name = serverId;
        entry.note = QCoreApplication::translate("ServerRegistry", "目录不在了");
        if (problemOut != nullptr) {
            *problemOut = entry.note;
        }
        return entry;
    }

    QStringList notes;

    // 1) 我们记的那一份（用户挑的档、版本、Java…）。
    QString metaProblem;
    const ServerModel::Info info = readMeta(serverId, &metaProblem);
    if (!metaProblem.isEmpty()) {
        notes.append(metaProblem);
    }
    entry.name = info.name.isEmpty() ? serverId : info.name;
    entry.gameVersion = info.gameVersion;
    entry.loaderVersion = info.loaderVersion;

    // 2) 盘上现在有哪些文件（认 flavor 和判"装好了没有"都用它）。
    const QStringList rootFiles = rootFileNames(entry.dirPath);
    const QStringList forgeArgs = forgeArgsPaths(entry.dirPath);
    QStringList relPaths = rootFiles;
    relPaths += forgeArgs;

    if (info.flavorKnown) {
        entry.flavor = info.flavor;
        entry.flavorKnown = true;
    } else {
        // 自己记的永远优先；没有才从文件反推（用户可能手动换了加载器版本，那个事实只有
        // `.wtfml-server` 说得清，见 `ServerModel::inferFlavorFromPaths()` 那段）。
        entry.flavor = ServerModel::inferFlavorFromPaths(relPaths);
        entry.flavorKnown = false;
        notes.append(QCoreApplication::translate("ServerRegistry", "这一档是从目录里的文件反推的（%1），点一下确认")
                         .arg(ServerInstall::flavorName(entry.flavor)));
    }

    // 3) 装好了没有 —— 先按"我们记的那一档 + 版本"精确判（那一档的 markers 才拼得出来），
    // 拼不出来就退回按文件认。
    QString missing;
    entry.installed =
        installedFor(entry.flavor, rootFiles, forgeArgs, &missing);
    if (!entry.installed) {
        notes.append(QCoreApplication::translate("ServerRegistry", "安装不完整：缺少 %1").arg(missing));
    }

    // 4) 那两个配置文件（都是可选的，缺了不算毛病）。
    const ServerModel::Properties props = readProperties(serverId, nullptr);
    entry.port = ServerModel::serverPortOf(props);
    entry.eulaAccepted = readEula(serverId);

    entry.note = notes.join(QCoreApplication::translate("ServerRegistry", "；"));
    if (problemOut != nullptr) {
        *problemOut = entry.note;
    }
    return entry;
}

QList<Entry> scanAll(QString *problemOut) {
    QList<Entry> entries;
    const QStringList ids = serverIds();
    for (const QString &id : ids) {
        entries.append(describe(id, nullptr));
    }
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    return entries;
}

// ---------------------------------------------------------------------------
// 我们记的那一份
// ---------------------------------------------------------------------------

ServerModel::Info readMeta(const QString &serverId, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QString path = AppPaths::serverMetaPath(serverId);
    QFile file(path);
    if (!file.exists()) {
        return ServerModel::Info{};  // 没有这一份不是错误（用户从外面拖进来的就是这样）
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "读不了 %1").arg(ServerModel::formatTag());
        }
        return ServerModel::Info{};
    }
    // 大小上限：`wtfml.meta` 那边是 16 MiB（`InstanceMetaFile::kSizeLimit`），这里一份
    // 服务端记录只有几格，4 MiB 已经宽得离谱。超过就当"读不懂"——**不要**先 readAll 再判：
    // 那正是"把几百 MB 的东西读进内存才发现太大"的形状。
    if (file.size() > 4 * 1024 * 1024) {
        file.close();
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "%1 大得不像一份记录（%2 字节），已按读不懂处理")
                              .arg(ServerModel::formatTag())
                              .arg(file.size());
        }
        return ServerModel::Info{};
    }
    const QByteArray bytes = file.readAll();
    file.close();
    bool ok = false;
    const ServerModel::Info info = ServerModel::fromJson(bytes, &ok);
    if (!ok && problemOut != nullptr) {
        // "文件在、但读不懂"这件事**必须说出来**（别当成"这个服务端什么设置都没有"）——
        // 和 `UpdatePolicy::load()` 里那句同源。
        *problemOut = QCoreApplication::translate("ServerRegistry", "无法解析服务端记录（%1）。请重新安装，或修正记录文件后重试。")
                          .arg(ServerModel::formatTag());
    }
    return info;
}

bool writeMeta(const QString &serverId, const ServerModel::Info &info, QString *problemOut) {
    if (!ServerWriteGuard::acquireFile(serverId, AppPaths::serverMetaFileName(), problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QString dir = AppPaths::serverDir(serverId);
    if (!QDir().mkpath(dir)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法创建服务端目录：%1")
                              .arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }
    QSaveFile file(AppPaths::serverMetaPath(serverId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "写不了 %1：%2")
                              .arg(ServerModel::formatTag(), file.errorString());
        }
        return false;
    }
    // 已知字段**盖在盘上那份上面**，不是照着结构体重建整份：`ServerModel::toJson()` 只写
    // 它认识的那几格，直接写回去会把「更新的启动器写的」「用户手写的」键一起抹掉
    // （`wtfml.meta` 那边早就改成"读—合并—写"了，这里对齐）。
    // 盘上那份读不懂时不当底：那种情况下写回去本来就是一次覆盖，而调用方（例如
    // `setRestartOnCrash`）会先拒绝；这里不额外拦，免得"修一份坏记录"也写不了。
    QJsonObject merged;
    {
        const QString path = AppPaths::serverMetaPath(serverId);
        QFile existing(path);
        if (!QFileInfo(path).isSymLink() && existing.exists() && existing.open(QIODevice::ReadOnly)) {
            // 4 MiB 足够一份记录；超了就是别的东西，不拿它当底。
            const QJsonObject object =
                QJsonDocument::fromJson(existing.read(4 * 1024 * 1024)).object();
            if (!object.isEmpty()) {
                merged = object;
            }
            existing.close();
        }
    }
    // **先清后盖**：模型自己拥有的键（`ownedMetaKeys()`）先删掉，再把这次要写的盖上去。
    // 少了"先清"这一步，"恢复默认值"就永远落不了盘 —— `toJson()` 对默认值是**省略**的，
    // 只盖不删时盘上那格旧值（例如 `restartOnCrash: true`）会一直活着，回读仍是旧值。
    // 不认识的那些键不在表里，照样原样留着。
    const QJsonObject known = QJsonDocument::fromJson(ServerModel::toJson(info)).object();
    for (const QString &key : ServerModel::ownedMetaKeys()) {
        merged.remove(key);
    }
    for (auto it = known.begin(); it != known.end(); ++it) {
        merged.insert(it.key(), it.value());
    }
    const QByteArray bytes = QJsonDocument(merged).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法保存 %1：%2")
                              .arg(ServerModel::formatTag(), file.errorString());
        }
        return false;
    }
    return true;
}

bool setRestartOnCrash(const QString &serverId, bool on, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    // 先看"读不读得懂"：不是"文件在不在"（不在 = 从外面拖进来的，照写）。
    QString readProblem;
    ServerModel::Info info = readMeta(serverId, &readProblem);
    if (!readProblem.isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "%1。为保留原有记录，已取消写入。")
                              .arg(readProblem);
        }
        return false;
    }
    info.restartOnCrash = on;
    return writeMeta(serverId, info, problemOut);
}

// ---------------------------------------------------------------------------
// 建 / 删
// ---------------------------------------------------------------------------

QString sanitizeId(const QString &desired) {
    QString out;
    out.reserve(desired.size());
    for (const QChar c : desired) {
        if (c.unicode() < 0x20) {
            out.append(QLatin1Char('_'));
            continue;
        }
        // Windows 文件名里不许出现的那几个 + 路径分隔符。
        // 这个集合里**一个空格都不能有** —— 写成 `"< > : …"` 那种带空格的形式，空格本身
        // 也在这个集合里，症状是"服务端的名字里每一个空格都被换成下划线"（而用户真机上
        // 的名字就有带空格的，`Server1.20.2(modded)` 旁边那种）。
        if (QStringLiteral("<>:\"/\\|?*").contains(c)) {
            out.append(QLatin1Char('_'));
            continue;
        }
        out.append(c);
    }
    out = out.trimmed();
    // 首尾的点在 Windows 上会被吃掉（`Server.` 存下去叫 `Server`），自己也别留。
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    while (out.startsWith(QLatin1Char('.'))) {
        out.remove(0, 1);
    }
    // 保留名（Windows 的 CON / PRN / AUX / NUL / COM1… / LPT1…）—— 一个合法的服务端名字
    // 撞上它们基本不可能，但撞上了目录建不出来，不如现在就换掉。
    static const QStringList reserved{
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"),
    };
    if (reserved.contains(out, Qt::CaseInsensitive)) {
        out.prepend(QLatin1Char('_'));
    }
    return out;
}

QString uniqueServerId(const QString &desired) {
    if (desired.isEmpty()) {
        return QString();
    }
    if (!serverExists(desired)) {
        return desired;
    }
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1-%2").arg(desired).arg(i);
        if (!serverExists(candidate)) {
            return candidate;
        }
    }
    return desired;  // 到这儿基本不可能（一万个同名）
}

QString defaultServerName(ServerInstall::Flavor flavor, const QString &gameVersion) {
    Q_UNUSED(flavor);
    if (gameVersion.isEmpty()) {
        return QStringLiteral("Server");
    }
    return QStringLiteral("Server%1").arg(gameVersion);
}

bool createServer(const QString &serverId, const ServerModel::Info &info, QString *problemOut) {
    if (!writableServer(serverId, problemOut)) return false;
    if (serverId.isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "要给这个服务端起个名字");
        }
        return false;
    }
    if (!ensureRoot(problemOut)) {
        return false;
    }
    ServerModel::Info toWrite = info;
    if (toWrite.serverId.isEmpty()) {
        toWrite.serverId = serverId;
    }
    return writeMeta(serverId, toWrite, problemOut);
}

bool removeServer(const QString &serverId, QString *problemOut) {
    if (!writableServer(serverId, problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    // 闸一：id 不能空、不能带路径分隔符（也不能是 `.` / `..`）。
    if (serverId.isEmpty() || serverId.contains(QLatin1Char('/'))
        || serverId.contains(QLatin1Char('\\')) || serverId == QLatin1String(".")
        || serverId == QLatin1String("..")) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "这个名字不能删");
        }
        return false;
    }
    const QString dir = AppPaths::serverDir(serverId);
    if (!QDir(dir).exists()) {
        return true;  // 本来就不在 = 已经达成目的
    }
    // 闸二：解出来的路径必须**严格在根目录底下**（防止 `serverId` 里塞了 `..` 之类）。
    if (!isStrictlyInside(rootDir(), dir)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "这个路径不在服务端目录底下，没动它");
        }
        return false;
    }
    // 进**回收站**（用户几个月的心血，点错一次得有地方捞回来）。服务端里的世界或内容
    // 目录可能是 junction；`moveToRecycleBin()` 只摘链接本身，绝不删除链接目标。
    QString recycleProblem;
    if (!moveToRecycleBin(dir, &recycleProblem)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法删除：%1\n%2")
                              .arg(QDir::toNativeSeparators(dir), recycleProblem);
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 那两个配置文件
// ---------------------------------------------------------------------------

QString eulaPath(const QString &serverId) {
    return AppPaths::serverDir(serverId) + QStringLiteral("/eula.txt");
}

QString propertiesPath(const QString &serverId) {
    return AppPaths::serverDir(serverId) + QStringLiteral("/server.properties");
}

bool readEula(const QString &serverId) {
    QFile file(eulaPath(serverId));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;  // 文件不在 / 读不了 ⇒ 还没接受
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    return ServerModel::eulaAccepted(text);
}

bool acceptEula(const QString &serverId, QString *problemOut) {
    if (!ServerWriteGuard::acquireFile(serverId, QStringLiteral("eula.txt"), problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QString dir = AppPaths::serverDir(serverId);
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法创建服务端目录：%1")
                              .arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }
    QSaveFile file(eulaPath(serverId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "写不了 eula.txt：%1").arg(file.errorString());
        }
        return false;
    }
    const auto bytes = ServerModel::eulaFileText(true).toUtf8();
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (problemOut) *problemOut = QCoreApplication::translate("ServerRegistry", "eula.txt 写入不完整，原文件未替换。");
        return false;
    }
    if (!file.commit()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法保存 eula.txt：%1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

ServerModel::Properties readProperties(const QString &serverId, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    QFile file(propertiesPath(serverId));
    if (!file.exists()) {
        return ServerModel::Properties{};  // 还没跑过一次 = 还没这个文件，不是错误
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "读不了 server.properties：%1").arg(file.errorString());
        }
        return ServerModel::Properties{};
    }
    // 按 UTF-8 解字节只是"文件本身怎么存"，**值的转义在 parseProperties() 里还原**
    // （`\uXXXX` / `\\` / `\t` 这些，见 ServerModel.hpp 的 escapePropertyText 那段）——
    // Minecraft 服务端按 Properties 那套读，值里的非 ASCII 早就被转义成 ASCII 了。
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    return ServerModel::parseProperties(text);
}

bool writeProperties(const QString &serverId, const ServerModel::Properties &properties,
                     QString *problemOut) {
    if (!ServerWriteGuard::acquireFile(serverId, QStringLiteral("server.properties"), problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QString dir = AppPaths::serverDir(serverId);
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法创建服务端目录：%1")
                              .arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }
    QSaveFile file(propertiesPath(serverId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "写不了 server.properties：%1").arg(file.errorString());
        }
        return false;
    }
    // UTF-8 是**文件容器**的编码（注释行里可能有中文，那部分原样保留）；键和值里的
    // 非 ASCII 由 `Properties::toString()` 转义成 `\uXXXX`，所以真正被游戏读的那部分全是
    // ASCII —— `Properties.load(InputStream)` 那层按 ISO-8859-1 解也不会有问题（R9）。
    const auto bytes = properties.toString().toUtf8();
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (problemOut) *problemOut = QCoreApplication::translate("ServerRegistry", "server.properties 写入不完整，原文件未替换。");
        return false;
    }
    if (!file.commit()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerRegistry", "无法保存 server.properties：%1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace ServerRegistry


