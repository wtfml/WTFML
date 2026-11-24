/**
 * @file InstanceFiles.cpp
 * @brief 实现实例目录判据与 jar/json 名称解析。
 * @note 判据见 .hpp；这里只保留实现。
 */

#include "InstanceFiles.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLatin1Char>
#include <QSet>
#include <iterator>

#include "../launcher/VersionInheritance.hpp"
#include "../util/AppPaths.hpp"
#include "../util/ZipUtil.hpp"

#if defined(Q_OS_WIN)
// 放在所有 Qt 头之后（windows.h 定义的东西太多，先于 Qt 包含容易撞名 —— 和 FolderManager.cpp
// 同一个写法）。这里只用 CreateFileW / GetFinalPathNameByHandleW / CloseHandle 三个。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace InstanceFiles {
namespace {

/// 在清单里找一个名字，**不区分大小写**，返回它在磁盘上的**真实**拼写（找不到返回空串）。
///
/// 为什么不区分大小写：Windows 的文件系统本来就不区分。用户把目录从 `MyPack` 改成 `mypack`
/// 之后，里面的 `MyPack.jar` 是**同一个文件**，用区分大小写的比较会认不出来。返回值必须是
/// 磁盘上那个真实拼写 —— 它要拿去拼路径。
QString findNamed(const QStringList &names, const QString &wanted) {
    for (const QString &name : names) {
        if (name.compare(wanted, Qt::CaseInsensitive) == 0) {
            return name;
        }
    }
    return QString();
}

/// 从**文件名**上认版本 JSON：顶层 json 多于一个、又没有精确匹配时用这一条。
///
/// 判据是"版本 json 和自己的 jar 摆在一起、名字也对得上"：`<X>.json` 旁边有 `<X>.jar`
/// 就算候选。这一条救的是真机上这个目录：
/// versions/魔改版ATT2-BC&A&IS(多周目)/
/// 魔改版ATT2-BC&A&IS(多周目).jar ← 目录名改过，jar 也跟着叫这个
/// 魔改版ATT2-[V1.8]BC&A&IS(多周目).jar ← 老名字那一份，json 还叫这个
/// 魔改版ATT2-[V1.8]BC&A&IS(多周目).json
/// patchouli_data.json ← **模组留下的杂物**（这个实例目录同时是
/// 游戏目录，saves/config/options.txt 都在里面）
/// 精确匹配 `<目录名>.json` 落空、"恰好一个 json"也被 `patchouli_data.json` 顶掉，
/// 于是 jsonName 退成 `<目录名>.json`（不存在）⇒ 启动前弹"版本信息缺失"。
/// 按"名字对得上 jar"挑，候选只剩一个，正是那份真 json。
///
/// 只吃文件名，不读盘 —— resolve() 因此仍然是纯函数（真值表在验证工程里钉得住）。
QString pickJsonByJarStem(const DirListing &listing) {
    QStringList jarStems;
    for (const QString &jar : listing.jarNames) {
        jarStems << QFileInfo(jar).completeBaseName();
    }
    QString found;
    for (const QString &json : listing.jsonNames) {
        const QString stem = QFileInfo(json).completeBaseName();
        bool matches = false;
        for (const QString &jarStem : jarStems) {
            if (stem.compare(jarStem, Qt::CaseInsensitive) == 0) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        if (!found.isEmpty()) {
            return QString();  // 不止一个，认不出 —— 交给调用方（内容那一层还能再挑一次）
        }
        found = json;
    }
    return found;
}

/// 读一个实例目录里的版本 JSON，取它声明的父版本（`inheritsFrom`）。
///
/// 读不到、读不懂、字段不是字符串，一律返回空串 —— 也就是"这不是一个继承型实例"。
/// **刻意不告警**：这条路径只在"一个 jar 都认不出来"时才会走到，而那种目录里
/// 绝大多数本来就是下载残骸（真机上还有 0 字节的空目录），一句告警换不来任何有用信息，
/// 只会把"认不出来的目录"这件事从安静变成刷屏。真有版本的目录在别处有它自己的说法
/// （installedVersions 的告警、启动时的"版本信息缺失"）。
QString readInheritsFrom(const QString &versionId, const QString &jsonName) {
    const QString name = jsonName.isEmpty() ? versionId + QStringLiteral(".json") : jsonName;
    QFile file(AppPaths::versionDir(versionId) + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return QString();
    }
    return VersionInheritance::parentIdOf(doc.object());
}

/// Windows 保留设备名（CON / PRN / AUX / NUL / CONIN$ / CONOUT$ / COM1-9 / LPT1-9）。
/// 判的是**主名**：`CON` 建不出来，`CON.txt` 一样建不出来。大小写不敏感。
bool isReservedDeviceName(const QString &stem) {
    const QString upper = stem.toUpper();
    if (upper == QLatin1String("CON") || upper == QLatin1String("PRN") ||
        upper == QLatin1String("AUX") || upper == QLatin1String("NUL") ||
        upper == QLatin1String("CONIN$") || upper == QLatin1String("CONOUT$")) {
        return true;
    }
    if (upper.size() != 4 ||
        !(upper.startsWith(QLatin1String("COM")) || upper.startsWith(QLatin1String("LPT")))) {
        return false;
    }
    // 末位是 1-9，或者上面说的那三个上标（它们是**一个** QChar，不是 '1' 折出来的两倍宽）。
    const QChar last = upper.at(3);
    return (last >= QLatin1Char('1') && last <= QLatin1Char('9')) || last == QChar(0x00B9) ||
           last == QChar(0x00B2) || last == QChar(0x00B3);
}

}  // namespace

QString validatePathSegment(const QString &segment) {
    if (segment.isEmpty()) {
        return QCoreApplication::translate("InstanceFiles", "名字不能是空的。");
    }

    // `/` `\` 排在最前面判，理由写在 .hpp 里：它们同时是路径分隔符，放过去就是
    // 路径穿越 —— `../别的目录` 这种名字会让后面的 QDir::rename 跑到 versions/ 外面。
    // 其余几个是 Windows 文件名的既定禁区，不挡的话会在 rename 那一刻才失败。
    // 0x00-0x1F（控制字符）单列一条：名字里夹一个换行或制表符，界面上看起来是空的，
    // 但这种名字在盘上确实建得出来，之后就再也说不清那是哪个实例了。
    for (const QChar ch : segment) {
        if (ch.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(ch)) {
            return QCoreApplication::translate("InstanceFiles", "名字里不能有 < > : \" / \\ | ? * 这些字符，也不能有制表符、换行这类控制字符。");
        }
    }

    if (segment == QLatin1String(".") || segment == QLatin1String("..")) {
        return QCoreApplication::translate("InstanceFiles", "「.」和「..」是保留名称，请使用其他目录名。");
    }

    // Windows 会**静默**删掉结尾的空格和点：输入 `abc ` 建出来就是 `abc`。静默是关键 ——
    // 用户看到的目录名和他刚输的不一样，然后就在列表里找不到自己的实例了。
    //
    // NOTE: 空格和点**这里都判**。只判点不行："尾随空格已经被调用方 trim 吃掉了"
    // 这句话只对"实例名"那条路成立（validateDirName() 开头就 trim），对**整合包清单
    // 里那一条**不成立：ModpackImport::isSafeRelativePath() 是逐段原样喂进来的，
    // 中间不经过任何 trim，于是 `mods/x.jar ` 这一条能过判据、Windows 建出来却是
    // `mods/x.jar` —— 名字和清单里写的对不上。两种字符被吃掉的后果一模一样，判据也就该一样。
    if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' '))) {
        return QCoreApplication::translate("InstanceFiles", "名称不能以句点或空格结尾。");
    }

    // 主名 = 第一个点之前的那一段。用 section 而不是 QFileInfo::baseName()：
    // 后者对 `CON.txt` 给 `CON`、对没有点的 `CON` 也给 `CON`，两者都对这里合适，
    // 但它还会做别的规整（比如把 `..x` 当成扩展名），没必要。
    if (isReservedDeviceName(segment.section(QLatin1Char('.'), 0, 0))) {
        return QCoreApplication::translate("InstanceFiles", "「%1」是 Windows 保留设备名，无法创建目录。")
            .arg(segment);
    }

    return QString();
}

QString validateFileName(const QString &name) {
    const QString problem = validatePathSegment(name);
    if (!problem.isEmpty()) {
        return problem;
    }
    if (name.length() > kMaxFileNameLength) {
        return QCoreApplication::translate("InstanceFiles", "文件名长度为 %2 个字符，上限为 %1 个字符。")
            .arg(kMaxFileNameLength)
            .arg(name.length());
    }
    return QString();
}

QString validateDirName(const QString &rawName) {
    const QString name = rawName.trimmed();

    // 逐条判据全在 validatePathSegment() 里（整合包那条路也用它）。这里只多一条长度 ——
    // 长度是**实例目录名**独有的规矩，不是"一段路径"的规矩，所以它留在这一层。
    const QString problem = validatePathSegment(name);
    if (!problem.isEmpty()) {
        return problem;
    }

    if (name.length() > kMaxDirNameLength) {
        return QCoreApplication::translate("InstanceFiles", "名称长度为 %2 个字符，上限为 %1 个字符。")
            .arg(kMaxDirNameLength)
            .arg(name.length());
    }

    return QString();
}

bool isUnfinishedImport(const QString &id) {
    if (!QFileInfo(AppPaths::versionDir(id)).isDir()) {
        return false;
    }
    // 标记是最后提交点：即使 meta 已写入，标记尚在就不能放行半成品。
    if (QFileInfo::exists(importMarkerPath(id))) return true;
    // ① 没有导入标记时，meta 表示这一份已经由启动器完成过。
    if (QFileInfo::exists(AppPaths::versionMetaPath(id))) {
        return false;
    }
    // ②③ 交给既有的判据：resolveInstance() 在没有 jar 的时候会去读那份 json 的
    // inheritsFrom（见 DirListing::inheritsFrom 的 NOTE），正是这两条要问的东西。
    const Resolved resolved = resolveInstance(id);
    if (!resolved.jarName.isEmpty()) {
        return false;
    }
    if (resolved.inheritsFrom.isEmpty()) {
        return false;
    }
    // ④ 光有 inheritsFrom 不够：别的启动器装出来的加载器版本也长这样（见 .hpp 那段）。
    // 没有标记的（老版本启动器留下的半成品）只能看那份 json 的形状：导入第一步写的
    // 只有 `id` / `inheritsFrom` / `type` 三格。第三方 profile 一律带 `mainClass` /
    // `libraries` 之类，不会这么薄 —— 那些是**正常实例**，认成半成品会让它们从列表里
    // 消失、点启动还被拦一句"还没装完"。
    const QString jsonName =
        resolved.jsonName.isEmpty() ? id + QStringLiteral(".json") : resolved.jsonName;
    QFile file(AppPaths::versionDir(id) + QLatin1Char('/') + jsonName);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject object = doc.object();
    for (const QString &key : object.keys()) {
        if (key != QStringLiteral("id") && key != QStringLiteral("inheritsFrom")
            && key != QStringLiteral("type")) {
            return false;
        }
    }
    return true;
}

QString importMarkerPath(const QString &id) {
    return AppPaths::versionDir(id) + QStringLiteral("/wtfml.importing");
}

QString importManifestFingerprint(const QByteArray &manifestJson) {
    if (manifestJson.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(manifestJson, QCryptographicHash::Sha256).toHex());
}

QString importMarkerFingerprint(const QString &id) {
    QFile marker(importMarkerPath(id));
    if (!marker.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // 老版本留下的标记是一行 ISO 时间（不是 JSON）：解析失败 = 没有指纹。别把它当成
    // "指纹对不上" —— 那会把升级前没导完的那一份锁死，用户只能删掉重下几百 MB。
    const QJsonDocument doc = QJsonDocument::fromJson(marker.readAll());
    if (!doc.isObject()) {
        return QString();
    }
    return doc.object().value(QStringLiteral("manifestSha256")).toString();
}

ImportNameVerdict checkImportTargetName(const QString &rawName, QString *problemOut,
                                        const QString &manifestFingerprint) {
    const QString problem = validateRenameTarget(rawName, QString());
    if (problem.isEmpty()) {
        if (problemOut != nullptr) {
            problemOut->clear();
        }
        return ImportNameVerdict::Ok;
    }
    // 名字本身就不合法（有分隔符、是保留设备名、太长……）时**不查盘**：那种名字拼出来的
    // 路径本身就不可信，拿它去盘上问"是不是半成品"没有意义。
    if (!validateDirName(rawName).isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = problem;
        }
        return ImportNameVerdict::Taken;
    }
    const QString name = rawName.trimmed();
    if (isUnfinishedImport(name)) {
        // 同名还不够，得是**同一个包**。上一次那趟把清单指纹写在了标记里；
        // 这一趟的指纹和它对不上时不能接着下 —— 两个包的文件会混进一个目录（见 .hpp）。
        // 任何一侧认不出指纹（老标记 / 调用方没给）就照旧放行，宁可少拦一次也不锁死
        // 用户已经下到一半的那一份。
        const QString recorded = importMarkerFingerprint(name);
        if (!recorded.isEmpty() && !manifestFingerprint.isEmpty()
            && recorded.compare(manifestFingerprint, Qt::CaseInsensitive) != 0) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("InstanceFiles", "「%1」中有另一个整合包的未完成导入记录。\n\n使用其他实例名称可保留原有进度。若要继续上次的导入，请重新选择上次的整合包。")
                    .arg(name);
            }
            return ImportNameVerdict::Taken;
        }
        if (problemOut != nullptr) {
            problemOut->clear();
        }
        return ImportNameVerdict::OkResume;
    }
    if (problemOut != nullptr) {
        *problemOut = problem;
    }
    return ImportNameVerdict::Taken;
}

QString validateRenameTarget(const QString &rawName, const QString &currentId) {
    const QString shapeProblem = validateDirName(rawName);
    if (!shapeProblem.isEmpty()) {
        return shapeProblem;
    }

    const QString name = rawName.trimmed();
    if (name == currentId) {
        return QCoreApplication::translate("InstanceFiles", "新名字和现在一样，没有改动的必要。");
    }

    // 重名比较不区分大小写，理由见 .hpp。`entry` 用磁盘上的真实拼写，
    // 那句提示要指得出到底撞上了哪一个。
    // 带上 QDir::Hidden：不带的话，盘上有个 `.foo`（用户自己建的、
    // 或者别的工具留下的）时这里**看不见它**，于是"重命名成 .foo"和"整合包叫 .foo"
    // 都判成"不重名"—— 而下一步是往那个目录里搬东西（整合包那条路是解压合并、不是
    // 建新目录），几秒钟后用户的账上就多了一个半新半旧、而且在实例列表里根本看不见的
    // 目录。看不见的东西更要在这里挡住：这一句的用途就是"告诉用户这个名字有主了"。
    const QStringList entries =
        QDir(AppPaths::versionsDir()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        if (entry.compare(name, Qt::CaseInsensitive) == 0 &&
            entry.compare(currentId, Qt::CaseInsensitive) != 0) {
            return QCoreApplication::translate("InstanceFiles", "已经有一个叫“%1”的实例了，换一个名字。").arg(entry);
        }
    }

    return QString();
}

Resolved resolve(const DirListing &listing) {
    Resolved out;
    out.jarCandidates = listing.jarNames;

    // ① 精确匹配（<目录名>.jar）优先。绝大多数实例走这条，而且它不依赖扫描顺序。
    // ② 否则顶层恰好一个 jar 就是它 —— 这一条正是本轮放松判据要救的那类实例：
    // 用户自己在资源管理器里改了目录名，没改里面的 jar 名。
    // ③ 其余（一个都没有 / 两个以上且没有精确匹配）都不算实例。
    out.jarName = findNamed(listing.jarNames, listing.dirName + QStringLiteral(".jar"));
    if (out.jarName.isEmpty() && listing.jarNames.size() == 1) {
        out.jarName = listing.jarNames.first();
    }

    // json 按同样的 ①/② 解析，再加一条 ②'，但**解析不到不算认不出来**：
    // 1. installedVersions() 的语义始终是"客户端 jar 在不在"（半个版本、只有 json 的下载
    // 残骸都不是实例），把 json 纳入判据会把"下到一半"的目录从"看得见、点了会告诉你
    // 为什么"变成"看不见" —— 正是本轮要消灭的那种静默。
    // 2. json 缺席有它自己现成的错误路径：启动时会弹"版本信息缺失，请先重新下载"。
    out.jsonName = findNamed(listing.jsonNames, listing.dirName + QStringLiteral(".json"));
    if (out.jsonName.isEmpty() && listing.jsonNames.size() == 1) {
        out.jsonName = listing.jsonNames.first();
    }
    // ②' 顶层 json 多于一个：挑名字对得上这份目录里某个 jar 的那一个。理由和那个真机
    // 目录见 pickJsonByJarStem()。挑不出来就留着空 —— resolveInstance() 会读内容再挑
    // 一次（那一层碰磁盘，纯函数不碰）。
    if (out.jsonName.isEmpty()) {
        out.jsonName = pickJsonByJarStem(listing);
    }

    // ④ 一个 jar 都没有、但 json 声明了父版本 → 认得出。带模组加载器的版本就长这样
    // （Fabric 的目录里只有一份 json，主 jar 在原版那边）。
    //
    // NOTE: 这条**不吃上面那两条的失败**：顶层有两个 jar、又没同名的那一个时，
    // jarNames 非空、认不出 jar，但 json 声明了父版本的话也算数 —— 那两个 jar
    // 是垃圾（用户丢进来的、装别的加载器剩下的），客户端 jar 反正在父版本那边，
    // 按 ①/② 挑只会挑错。判据是"声明了父版本"，不是"有没有多余的 jar"。
    out.inheritsFrom = listing.inheritsFrom;

    return out;
}

DirListing scanDir(const QString &versionId) {
    DirListing out;
    out.dirName = versionId;

    const QDir dir(AppPaths::versionDir(versionId));

    // NOTE: 必须**非递归** —— QDir::Files 不能配上 QDir::Subdirectories。
    // 实例目录底下还有 libraries/：下载时把整个版本目录当目的地，库 jar 全写在
    // versions/<id>/libraries/ 里，真机上一个整合包随手就是几百个。递归扫会把它们
    // 统统当成客户端 jar 的候选，于是"顶层恰好一个 jar"这条判据立刻失效、
    // 所有实例反而认不出来了。别顺手加上 Subdirectories。
    //
    // NOTE: **一趟 readdir 分两类**，不是两趟 `entryList()`（一次 `*.jar`、一次
    // `*.json`）：两趟看的是同一批顶层条目，而每趟都是一次完整的目录读取。真机上
    // 200 个实例一轮刷新就是 400 趟，这一趟能省掉一半。顺序不变：带名字过滤和
    // 不带名字过滤的 `entryList()` 都按文件名排，所以两个表里每一条的先后和改之前
    // 逐字相同。
    //
    // 后缀比较**不区分大小写**：`QDir` 的名字过滤器（原来那两个 `*.jar` / `*.json`）
    // 默认就是不分大小写的（它的 `CaseSensitive` 过滤器位没有置），所以 `A.JAR` 改之前
    // 就是候选 jar，这里必须照旧。
    //
    // 目录不存在时 `entryList()` 给空表，和原来那句 `dir.exists()` 早退一个结果，
    // 所以那道闸不需要了（它自己也要一次 stat）。
    const QStringList names = dir.entryList(QDir::Files);
    for (const QString &name : names) {
        if (name.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)) {
            out.jarNames.append(name);
        } else if (name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            out.jsonNames.append(name);
        }
    }
    return out;
}

Resolved resolveInstance(const QString &versionId) {
    const DirListing listing = scanDir(versionId);
    Resolved out = resolve(listing);

    // 上面按**文件名**挑不出版本 json（顶层好几个 json、名字又都对不上 jar）时，读内容
    // 再挑一次：版本 json 是个对象，至少得有几样版本 json 该有的东西（mainClass /
    // libraries / id / inheritsFrom），模组留下的杂物 json 一样都没有。
    //
    // 只在"挑不出来"时才走到这里（真机上 61 个实例是 0 个走这条），所以热路径不变。
    // 读不懂的、不是对象的，一律不算候选 —— 和 readInheritsFrom() 一样不告警：这一层的
    // 职责只是"尽量挑对"，挑不对时启动前那条"版本信息缺失"会说话。
    if (out.jsonName.isEmpty() && listing.jsonNames.size() > 1) {
        QString found;
        for (const QString &json : listing.jsonNames) {
            QFile file(AppPaths::versionDir(versionId) + QLatin1Char('/') + json);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (!doc.isObject()) {
                continue;
            }
            const QJsonObject object = doc.object();
            const bool looksLikeVersion =
                object.contains(QStringLiteral("mainClass")) ||
                object.contains(QStringLiteral("libraries")) ||
                object.contains(QStringLiteral("id")) ||
                object.contains(QStringLiteral("inheritsFrom"));
            if (!looksLikeVersion) {
                continue;
            }
            if (!found.isEmpty()) {
                found.clear();  // 不止一个"像版本 json"的：不猜
                break;
            }
            found = json;
        }
        out.jsonName = found;
    }

    if (!out.recognized()) {
        // 只有"一个 jar 都认不出来"时才去读 json —— 这一条是热路径（每个已安装版本、
        // 每次刷新界面都要过一遍，真机上 61 个实例），绝大多数实例在自己目录里就认出了
        // jar，于是这里一次磁盘读取都不会发生，行为与改动前逐字节相同。
        out.inheritsFrom = readInheritsFrom(versionId, out.jsonName);
    }
    return out;
}

QString jarOwnerId(const QString &versionId) {
    Resolved resolved = resolveInstance(versionId);
    if (!resolved.jarName.isEmpty()) {
        return versionId;
    }

    // 自己没有 jar：顺着 inheritsFrom 链往上找第一个"自己有 jar"的那一层。
    //
    // 两道闸都是必须的：手写/写错的 json 可以首尾相接成环（A 继承 B、B 继承 A），
    // 也可以串成一条很长的链。这里不报错，只退化 —— 报错的地方在别处（启动前检查会说
    // "缺少客户端 JAR"，VersionManager 会说"父版本没装"），这一层的职责只是给一个
    // 说得通的路径，让那些报错能指着正确的地方。
    QSet<QString> seen;
    seen.insert(versionId);
    QString last = versionId;
    for (int depth = 0; depth < VersionInheritance::maxDepth(); ++depth) {
        const QString parent = resolved.inheritsFrom;
        if (parent.isEmpty() || seen.contains(parent)) {
            break;
        }
        seen.insert(parent);
        last = parent;
        resolved = resolveInstance(parent);
        if (!resolved.jarName.isEmpty()) {
            return parent;
        }
    }
    // 整条链上都没有 jar：指向链上最后一层，报错时说的就是"1.20.1 的 jar 不在"，
    // 而不是把责任推给加载器那个目录（它的 json 本来就是不长 jar 的）。
    return last;
}

QString jarName(const QString &versionId) {
    const QString owner = jarOwnerId(versionId);
    const Resolved resolved = resolveInstance(owner);
    // 退回老名字。见 .hpp 的 NOTE —— 这不是"错误处理"，是让只想拿个名字去看看在不在的
    // 调用方在正常目录上逐字节不变。
    return resolved.recognized() && !resolved.jarName.isEmpty()
               ? resolved.jarName
               : owner + QStringLiteral(".jar");
}

QString jsonName(const QString &versionId) {
    const Resolved resolved = resolveInstance(versionId);
    return resolved.jsonName.isEmpty() ? versionId + QStringLiteral(".json") : resolved.jsonName;
}

QString jarPath(const QString &versionId) {
    return AppPaths::versionDir(jarOwnerId(versionId)) + QLatin1Char('/') + jarName(versionId);
}

QString jarRelativePath(const QString &versionId) {
    return QStringLiteral("versions/%1/%2").arg(jarOwnerId(versionId), jarName(versionId));
}

QString jsonPath(const QString &versionId) {
    return AppPaths::versionDir(versionId) + QLatin1Char('/') + jsonName(versionId);
}

int mainClassJavaMajor(const QString &versionId, const QString &mainClass) {
    if (mainClass.isEmpty()) {
        return 0;
    }
    // zip 里的条目名是 `/` 分隔的（`a.b.C` → `a/b/C.class`）—— 和 LauncherManager 里那段
    // 找主类的判据拼的是同一个字符串。
    QString entryName = mainClass;
    entryName.replace(QLatin1Char('.'), QLatin1Char('/'));
    entryName += QStringLiteral(".class");

    // 找的范围和 LauncherManager 里"主类是不是装在别的 jar 里"完全一致：客户端 jar 那个
    // 目录（继承型实例在主版本那边）+ 实例自己那个目录的顶层 jar。**顺序也一样**：先
    // 主人那一份，再实例自己那些 —— 主类绝大多数就在客户端 jar 里，第一发就命中。
    QStringList tried;
    for (const QString &ownerId : {jarOwnerId(versionId), versionId}) {
        const DirListing listing = scanDir(ownerId);
        for (const QString &jarName : listing.jarNames) {
            if (tried.contains(jarName, Qt::CaseInsensitive)) {
                continue;
            }
            tried << jarName;
            const QString jar = AppPaths::versionDir(ownerId) + QLatin1Char('/') + jarName;
            QByteArray bytes;
            // 只要头 8 个字节，但 readEntry() 一次给的是整个条目 —— 上限压到 1 MiB
            // （R11）：class 文件再大也不会到这个数，而一个同名的巨无霸条目不该在每次
            // 启动扫描时把内存和临时目录一起撑起来。
            if (ZipUtil::readEntry(jar, entryName, &bytes, nullptr,
                                   QCoreApplication::translate("InstanceFiles", "读客户端 JAR 里的主类"), 1024 * 1024) !=
                ZipUtil::EntryStatus::Ok) {
                continue;  // 这个 jar 里没有这个类（或者读不动）—— 换下一个
            }
            // class 文件的头 8 个字节：魔数(4) + 次版本(2) + 主版本(2)，都是大端。
            if (bytes.size() < 8) {
                continue;
            }
            const auto *raw = reinterpret_cast<const unsigned char *>(bytes.constData());
            if (raw[0] != 0xCA || raw[1] != 0xFE || raw[2] != 0xBA || raw[3] != 0xBE) {
                continue;  // 不是 class 文件（同名条目？）—— 不猜
            }
            const int classFileMajor = (static_cast<int>(raw[6]) << 8) | static_cast<int>(raw[7]);
            if (classFileMajor < 45) {
                continue;  // 比 Java 1.1 还小：不是我们认得的东西
            }
            return classFileMajor - 44;
        }
    }
    return 0;
}

// 为什么不能只靠 QFileInfo::canonicalFilePath()：它在 Windows 上是
// `slowCanonicalized()`（qfilesystemengine.cpp:38），逐段走的时候判据是
// `fi.isSymLink()` —— 而 `isSymLink()` 只认 `LinkType`，junction 在 Qt 里走的是
// **`JunctionType`**（qfilesystemmetadata_p.h:365-370：`IO_REPARSE_TAG_SYMLINK` → LinkType，
// `IO_REPARSE_TAG_MOUNT_POINT` → JunctionType）。所以**canonicalFilePath 不解 junction**：
// 拿一个指着实例目录的 junction 当"游戏文件夹"喂进来，它就原样把那串字面量还给你。
//
// NOTE: 用的是**公开 API**（`QFileInfo::isJunction()` / `junctionTarget()`，Qt 6.2+），
// 所以这里不需要 windows.h，非 Windows 上这两个函数一个永远 false、一个回空串。
// NOTE: 解出来之后再递归一次 —— 目标那一头可能还压着别的 junction。`guard` 就是防
// 互相指的环（A 指着 B、B 指着 A）：撞上限就返回当时那串，判据**退化成字面量比较**，
// 不会死循环（这一层宁可漏判也不能挂住界面）。
// NOTE: `static` 是有意的 —— 它只是这一条判据的实现细节，不进 .hpp、不给外面用。
#if defined(Q_OS_WIN)
/**
 * @brief Windows 上**唯一**可靠的"这条路径最终指向哪儿"：开一个句柄，让文件系统自己回答。
 *
 * 为什么不继续用 `QFileInfo::junctionTarget()`：它解析的是重解析点里存的那串 substitute
 * name，而那串字面量可以是相对的、也可能带 `\??\Global\` 这类前缀 —— Qt 于是把相对的那一段
 * 拼到 junction 自己的父目录上。本机实测（2026-11-04）：`cmd /c mklink /J` 造出来的目标是
 * `\??\Global\D:\...`，`junctionTarget()` 回的是
 * `.../jt-link2/Global/D:/Desktop/.../源实例`，而 `pathIsSameOrInside()` 拿它去比当然对不上
 * —— "不能把实例复制到它自己里面"那道闸就被绕过了（ZJ3/ZJ5/T27.3 三条断言在本机红、
 * 在 CI 绿，正是这个差别）。
 *
 * 句柄这一路走的是文件系统自己的解析结果，写坏了也骗不过它；`FILE_FLAG_BACKUP_SEMANTICS`
 * 是"要打开的是目录而不是文件"必须带的。失败回空串，调用方退回 Qt 那条实现。
 */
static QString finalPathByHandle(const QString &path) {
    QString native = QDir::toNativeSeparators(path);
    if (!native.startsWith(QStringLiteral("\\\\?\\"))) {
        native = native.startsWith(QStringLiteral("\\\\"))
                     ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2)
                     : QStringLiteral("\\\\?\\") + native;
    }
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return QString();
    }
    // 32 KiB 是 Windows 支持的路径上限量级（32767 个字符）；不够就当场判失败，
    // 不猜一个截断过的路径（截断过的路径指到别处比指不到更危险）。
    wchar_t buffer[32768];
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer, DWORD(std::size(buffer)), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (length == 0 || length >= DWORD(std::size(buffer))) {
        return QString();
    }
    QString resolved = QString::fromWCharArray(buffer, int(length));
    if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
        resolved = QStringLiteral("\\\\") + resolved.mid(8);
    } else if (resolved.startsWith(QStringLiteral("\\\\?\\"))) {
        resolved = resolved.mid(4);
    }
    return QDir::fromNativeSeparators(resolved);
}
#endif

static QString resolveJunctions(const QString &absPath, int guard) {
    if (guard <= 0 || absPath.isEmpty()) {
        return absPath;
    }
    // 只处理盘符式/UNC 式绝对路径；`from` 是第一个可以当"真目录"去看的位置。
    int from = 0;
    if (absPath.size() >= 2 && absPath.at(1) == QLatin1Char(':')) {
        from = 2;  // "C:" 这一截不可能是 junction
    }
    while (true) {
        const int sep = absPath.indexOf(QLatin1Char('/'), from);
        const QString prefix = (sep < 0) ? absPath : absPath.left(sep);
        const QFileInfo info(prefix);
        if (info.isJunction()) {
            QString target;
#if defined(Q_OS_WIN)
            // 先问文件系统（见 finalPathByHandle 的块注释）；拿不到才退回 Qt 那一条。
            target = finalPathByHandle(prefix);
#endif
            if (target.isEmpty()) {
                target = info.junctionTarget();  // Qt 已经补成绝对路径
                // 但"补成绝对路径"这个前提是会破的：拿到的若不是绝对路径，就不要拿它去
                // 拼判据（拼出来的是一个既不是 A 也不是 B 的路径，反而把闸放过去了）。
                if (!target.isEmpty() && !QDir::isAbsolutePath(target)) {
                    target.clear();
                }
            }
            if (!target.isEmpty()) {
                target.replace(QLatin1Char('\\'), QLatin1Char('/'));
                if (sep >= 0) {
                    target += absPath.mid(sep);  // 把 junction 底下那一截接回去
                }
                return resolveJunctions(QDir::cleanPath(target), guard - 1);
            }
        }
        if (sep < 0) {
            break;  // 走到最后一段了
        }
        from = sep + 1;
    }
    return absPath;
}

QString resolveLinkTarget(const QString &path) {
    if (path.isEmpty()) {
        return QString();
    }
#if defined(Q_OS_WIN)
    // 句柄这一路对普通目录也会成功（回它自己的路径），所以先确认"它是个链接"，
    // 否则调用方拿到"目标 = 自己"会以为链接指着自己（死循环那种判据）。
    const QFileInfo info(path);
    if (!info.isJunction() && !info.isSymLink()) {
        return QString();
    }
    QString resolved = finalPathByHandle(path);
    if (resolved.isEmpty()) {
        // 退回 Qt：至少符号链接那一档它是准的；junction 那一档可能拿到畸形的字面量，
        // 所以再校验一次"是不是绝对路径"，不合格就当"读不出来"（宁可让调用方重做别名，
        // 也不要拿一个既不是 A 也不是 B 的路径去比）。
        resolved = info.isSymLink() ? info.symLinkTarget() : info.junctionTarget();
        if (!resolved.isEmpty() && !QDir::isAbsolutePath(resolved)) {
            resolved.clear();
        }
    }
    return QDir::fromNativeSeparators(resolved);
#else
    const QFileInfo info(path);
    return info.isSymLink() ? info.symLinkTarget() : QString();
#endif
}

QString canonicalPath(const QString &p) {    // 规范化：能解成真路径就解（解掉符号链接 / 8.3 短名 / 大小写那些别名），**再加一轮
    // junction**（canonicalFilePath 不管 junction，见上面那段）；解不了就退到 cleanPath +
    // absolutePath —— 见 .hpp 上那段 NOTE（判据不消失）。
        if (p.isEmpty()) {
            return QString();
        }
        QString canonical = QFileInfo(p).canonicalFilePath();
        if (canonical.isEmpty()) {
            canonical = QDir::cleanPath(QDir(p).absolutePath());
        }
        const QString resolved = resolveJunctions(canonical, 32);
        // **解完 junction 还要再规范一遍**。`resolveJunctions()` 给回的
        // 是重解析点里**存着的那个写法**（盘符/目录大小写、8.3 短名都可能和盘上真实的
        // 写法不同），而下面那句比较是**大小写敏感**的 —— 不补这一轮，
        // `versions/<id>` 的真实路径和"经 junction 走回来的同一条路径"会被判成不包含，
        // `InstanceTransferController` 拿它挡的"目标不能在自己里面"就在 junction 存在时
        // 漏判（复制/移动可以把实例塞进它自己）。canonicalFilePath() 不解 junction，
        // 所以顺序必须是"先解 junction、再规范化"：这时路径里已经没有 junction 成分了，
        // 它只剩下把剩下的别名收干净这一件事。
        const QString normalized = QFileInfo(resolved).canonicalFilePath();
        return normalized.isEmpty() ? resolved : normalized;
}

bool pathIsSameOrInside(const QString &parent, const QString &candidate) {
    QString a = canonicalPath(parent);
    QString b = canonicalPath(candidate);
    if (a.isEmpty() || b.isEmpty()) {
        return false;  // 空串不比（"没给路径"不是"包含"）
    }
    // 分隔符统一 + 尾随分隔符削掉：Qt 自己一律用 `/`，但调用方可能从别处拿到 `\`。
    a.replace(QLatin1Char('\\'), QLatin1Char('/'));
    b.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (a.endsWith(QLatin1Char('/')) && a.size() > 1) {
        a.chop(1);
    }
    while (b.endsWith(QLatin1Char('/')) && b.size() > 1) {
        b.chop(1);
    }
    if (b == a) {
        return true;
    }
    // `a + '/'` 这一截是必须的：不然 `D:/ab` 会被判成在 `D:/a` 里面。
    //
    // NOTE: 大小写**不**放宽。canonicalFilePath() 给回来的就是盘上真实的那个写法，两边
    // 都经过它，正常情况下写法一定一致；放宽反而会在"盘上真有两个只差大小写的目录"
    // 那种情况下把不相干的路径判成包含（Windows 上那种目录同源目录里是建不出来的，
    // 但网络盘/挂载点上未必）。
    return b.startsWith(a + QLatin1Char('/'));
}

QStringList migrateInstanceCacheDirs(const QString &oldId, const QString &newId,
                                     const QStringList &roots, int *movedOut) {
    if (movedOut != nullptr) {
        *movedOut = 0;
    }
    QStringList problems;
    const auto badName = [](const QString &id) {
        return id.isEmpty() || id == QStringLiteral(".") || id == QStringLiteral("..")
            || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\'));
    };
    // 名字都是先过 validateDirName()／validateRenameTarget() 的；这里再挡一次纯属自卫 ——
    // 下面拿它拼路径，空串拼出来是**根目录本身**，那会把整个 cache 根当成一个格子搬走。
    if (badName(oldId) || badName(newId)) {
        problems.append(QCoreApplication::translate("InstanceFiles", "缓存备份没有搬：%1 → %2 里有一个不能当目录名用。")
                            .arg(oldId, newId));
        return problems;
    }
    if (oldId == newId) {
        return problems;
    }

    int moved = 0;
    for (const QString &rawRoot : roots) {
        const QString root = rawRoot.trimmed();
        if (root.isEmpty()) {
            continue;
        }
        const QDir dir(root);
        const QString from = dir.filePath(oldId);
        const QString to = dir.filePath(newId);
        // 只差大小写：Windows 上两边是同一个目录，新名字本来就能直接看见它，搬是空操作。
        // （真正把目录名大小写改过来的是实例目录那一步，cache 这里不掺和。）
        if (from.compare(to, Qt::CaseInsensitive) == 0) {
            continue;
        }
        const QFileInfo fromInfo(from);
        if (!fromInfo.exists()) {
            continue;  // 没备份过（绝大多数实例都是这样）
        }
        if (!fromInfo.isDir() || fromInfo.isSymLink() || fromInfo.isJunction()) {
            problems.append(QCoreApplication::translate("InstanceFiles", "缓存备份没有搬：%1 不是一个普通目录。")
                                .arg(QDir::toNativeSeparators(from)));
            continue;
        }
        // 目标格子已经有东西：不合并（见头文件那条 —— 两边的"第 N 代"没法排先后）。
        if (QFileInfo::exists(to)) {
            problems.append(QCoreApplication::translate("InstanceFiles", "%1 中已存在名为「%2」的备份，旧备份保留在原位置。")
                                .arg(QDir::toNativeSeparators(root), newId));
            continue;
        }
        if (!QDir().rename(from, to)) {
            problems.append(QCoreApplication::translate("InstanceFiles", "无法将缓存备份从 %1 移至 %2。请检查文件是否被占用。")
                                .arg(QDir::toNativeSeparators(from),
                                     QDir::toNativeSeparators(to)));
            continue;
        }
        ++moved;
    }
    if (movedOut != nullptr) {
        *movedOut = moved;
    }
    return problems;
}

bool pathHasLinkComponent(const QString &root, const QString &relativePath) {
    if (root.trimmed().isEmpty() || relativePath.isEmpty()) {
        return false;
    }
    QString path = QDir::cleanPath(root);
    QString relative = relativePath;
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        path += QLatin1Char('/');
        path += segment;
        const QFileInfo info(path);
        if (info.isJunction() || info.isSymLink()) {
            return true;
        }
    }
    return false;
}

}  // namespace InstanceFiles
