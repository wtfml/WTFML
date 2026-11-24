/**
 * @file VersionManagerExport.cpp
 * @brief `VersionManager` 的"认这个实例是什么"与"把它打成一个整合包"那一摊。
 *
 * 【为什么这两件事在一个文件里】
 *
 * 导出要写的每一行清单（游戏版本、加载器键、加载器版本号）**都来自**前面那几个"从盘上
 * 认出来"的询问（`instanceLoaderInfo()` / `instanceGameVersion()` /
 * `instanceModLoader()`）；认错一个字，写出去的包用户装回来就是"模组都在、游戏起不来"。
 * 把它们摆在一起，"认"与"写"照着同一个判据走、改一处能看见另一处。
 *
 * 同一个文件里还有那两个**在线反查**（Modrinth 按 sha1 批量查、CurseForge 按指纹查）：
 * 导出时清单里那些文件要落到"project/version"上，只有这两条路能问出来。
 *
 * 【不在这里的】
 *
 * zip 怎么写、清单长什么样，是 `ModpackExport` 那一层的纯逻辑（那个文件不碰网络、也不碰
 * 用户的游戏目录）；这里只负责取数、发请求、把失败翻译成人话。
 */

#include "VersionManager.hpp"
#include "VersionManagerInternals.hpp"

#include <QByteArray>
#include <QChar>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include "../download/LoaderMeta.hpp"
#include "../launcher/VersionArguments.hpp"
#include "../launcher/VersionInheritance.hpp"
#include "../net/HttpClient.hpp"
#include "../util/AppPaths.hpp"
#include "../util/ZipUtil.hpp"
#include "FolderManager.hpp"
#include "InstanceFiles.hpp"
#include "ModpackExport.hpp"
#include "ModpackImport.hpp"

using namespace VersionManagerInternals;

// ---------------------------------------------------------------------------
// 导出整合包（要联网 / 要读盘的那几件；清单和 zip 的规则都在 ModpackExport 那一层）
// ---------------------------------------------------------------------------

namespace {

/// 加载器 → 整合包清单里那一行的键（`ModpackImport` 那套词）。认不出（OptiFine）回空串。
///
/// NOTE: 写成逐条列出的 switch 而不是一张查表，是为了**将来多一个加载器时编译器会替你
/// 盯一眼**（loaderKindForDependencyKey 那一条也是这个理由，方向正好相反）。
QString dependencyKeyForLoaderKind(VersionManager::LoaderKind kind) {
    switch (kind) {
    case VersionManager::LoaderKind::Fabric:
        return ModpackImport::fabricLoaderKey();
    case VersionManager::LoaderKind::Forge:
        return ModpackImport::forgeLoaderKey();
    case VersionManager::LoaderKind::NeoForge:
        return ModpackImport::neoforgeLoaderKey();
    case VersionManager::LoaderKind::Quilt:
        return ModpackImport::quiltLoaderKey();
    case VersionManager::LoaderKind::OptiFine:
        // 两种清单格式里都没有它那一格。**这一档由调用方在进这个函数之前就拦掉了**
        // （见 optifineExportProblem），这里回空串只是"没有可以写进清单的键"这个事实。
        return QString();
    }
    return QString();
}

/// 加载器版本号长得像不像一个版本号：**点分数字**（`47.4.10` / `10.13.4.1614`）。
///
/// 判据抄的是 LoaderMeta::listForgeVersions 里那条"只收点分数字的版本号"：Forge 自己的
/// maven 上带字母后缀的那几个（`10.13.3.1401-1710ls`）连**排序**都没有定义，我们那份列表
/// 本来就不会把它们交出来，所以"点分数字"对真值是充分的。
static bool looksLikeLoaderVersion(const QString &text) {
    if (text.isEmpty()) {
        return false;
    }
    bool sawDigit = false;
    for (const QChar ch : text) {
        if (ch.isDigit()) {
            sawDigit = true;
        } else if (ch != QLatin1Char('.')) {
            return false;
        }
    }
    return sawDigit;
}

/// 一个版本 id 是不是某个**我们装得了的**加载器装出来的，是的话把键和版本号拆出来。
///
/// 三种拼法都有唯一出处（LoaderMeta 里那三个函数），这里只是把它们**反过来读**：
/// `fabric-loader-0.16.14-1.20.1` / `1.20.1-forge-47.4.10` / `neoforge-21.1.215`
/// 外加 Quilt 的 `quilt-loader-0.20.0-1.20.1`（我们装不了它，但**认不出来就等于把它当
/// 纯原版导**，而那种包用户装回来会发现模组都在、游戏起不来，见 .hpp 里那段）。
///
/// NOTE: Fabric / Quilt 那两条**从左边切第一刀**（加载器版本到第一个 `-` 为止，剩下的
/// 都算游戏版本）：游戏版本里真能带 `-`（`1.21-pre1`），加载器版本不会。反过来切的话
/// `0.16.14` 会被拆成 `0.16.14` 和空。
///
/// @param knownVersions 在线清单里那些版本（没取到就是空表）。**只有 Forge 那一条用得上**：
/// 它的分隔符是一个**不带头锚点**的子串（见下面那段 NOTE），光看长相不足以判断
/// 左边那半是不是游戏版本。
bool loaderIdParts(const QString &id, const QList<MinecraftVersion> &knownVersions,
                   QString *keyOut, QString *versionOut) {
    const auto fill = [keyOut, versionOut](const QString &key, const QString &version) {
        if (keyOut != nullptr) {
            *keyOut = key;
        }
        if (versionOut != nullptr) {
            *versionOut = version;
        }
        return true;
    };
    // 两条 `<加载器>-loader-<加载器版本>-<游戏版本>` 形状的路：只有键不一样。
    const auto loaderLoaderForm = [&](const QString &prefix, const QString &key) {
        if (!id.startsWith(prefix)) {
            return false;
        }
        const QString rest = id.mid(prefix.size());
        const int dash = rest.indexOf(QLatin1Char('-'));
        if (dash <= 0) {
            return false;
        }
        return fill(key, rest.left(dash));
    };
    static const QString kFabric = QStringLiteral("fabric-loader-");
    static const QString kQuilt = QStringLiteral("quilt-loader-");
    if (loaderLoaderForm(kFabric, ModpackImport::fabricLoaderKey())) {
        return true;
    }
    if (loaderLoaderForm(kQuilt, ModpackImport::quiltLoaderKey())) {
        return true;
    }
    // Forge 这条和上面三条**不一样**：它的分隔符 `-forge-` 是个**不带头锚点**
    // 的子串（`<游戏版本>-forge-<加载器版本>` 只能从中间认），而这一条路要读的 id 里
    // 混着**用户自己起的名字**（PCL 那类合并实例的 `id` 就是目录名，见 instanceGameVersion
    // 里那段），于是「1.20.1-forge-版」「26.2hyperland-forge-47.4.10」这种名字会被当成
    // 一个真的 Forge 实例：清单里写上一个不存在的 Forge 版本号，收包的人那边整包装不上，
    // 而包看上去是好的（正是 .hpp 里那段最怕的失败形态）。
    // 两道判据一起上，左边那半走**全项目同一套** isKnownGameVersion（清单里有它才算数，
    // 清单没取到时它自己退回"以数字开头 + 没有空白"的长相判据），右边那半必须是点分数字。
    static const QString kForge = QStringLiteral("-forge-");
    const int forge = id.indexOf(kForge);
    if (forge > 0 && forge + kForge.size() < id.size()) {
        const QString gamePart = id.left(forge);
        const QString loaderPart = id.mid(forge + kForge.size());
        if (VersionManager::isKnownGameVersion(gamePart, knownVersions) &&
            looksLikeLoaderVersion(loaderPart)) {
            return fill(ModpackImport::forgeLoaderKey(), loaderPart);
        }
        return false;
    }
    static const QString kNeoForge = QStringLiteral("neoforge-");
    if (id.startsWith(kNeoForge) && id.size() > kNeoForge.size()) {
        return fill(ModpackImport::neoforgeLoaderKey(), id.mid(kNeoForge.size()));
    }
    return false;
}

/// OptiFine 那句"认得出来、写不下去"的说明。
///
/// NOTE: 它是**故意不导出**的，不是没做：两种规范里都没有 OptiFine 那一格（Modrinth 的
/// 依赖表只认 minecraft / forge / neoforge / fabric-loader / quilt-loader），照原版
/// 导出去的话用户装回来的包看着是好的、进去是一堆没有光影的模组。
QString optifineExportProblem(const QString &version) {
    const QString shown =
        version.trimmed().isEmpty() ? QString() : QStringLiteral(" %1").arg(version.trimmed());
    return QObject::tr(
               "此实例使用 OptiFine%1。当前整合包清单格式无法记录 OptiFine 依赖，导出已取消。")
        .arg(shown);
}

/// 认出来的加载器键能不能写进清单。空串 = 能写（**包括"压根没有加载器"这一档**，
/// 那是正常的：纯原版实例导出来就是纯原版包）。
///
/// NOTE: 传进来的键**不会是空串以外的 OptiFine**（那一档在进这里之前就拦了）；
/// Quilt 现在有合法的清单键（quilt-loader），可以照写。
QString loaderKeyExportProblem(const QString &key, const QString &version) {
    if (key.isEmpty() || key == ModpackImport::fabricLoaderKey() ||
        key == ModpackImport::forgeLoaderKey() || key == ModpackImport::neoforgeLoaderKey()
        || key == ModpackImport::quiltLoaderKey()) {
        // 2.1.4 起 Quilt 也能装了：清单里的 quilt-loader 键可以照写，导回来能装。
        return QString();
    }
    const QString shown =
        version.trimmed().isEmpty() ? QString() : QStringLiteral(" %1").arg(version.trimmed());
    return QObject::tr("不支持导出使用「%1」（%2）加载器的整合包。")
        .arg(key, shown.trimmed().isEmpty() ? QObject::tr("没有版本号") : shown.trimmed());
}

/// Modrinth 那一发没成时那句话。
QString modrinthLookupFailureText(const HttpClient::Reply &reply) {
    if (!reply.networkError.isEmpty()) {
        return QObject::tr("问 Modrinth 要文件信息时没连上：%1").arg(reply.networkError);
    }
    if (reply.status == HttpClient::kStatusTooManyRequests) {
        // 限流那一句的唯一出处（见 HttpClient::throttledText）。
        return HttpClient::throttledText(QObject::tr("问 Modrinth 要文件信息"));
    }
    return QObject::tr("Modrinth 的接口回了 HTTP %1。").arg(reply.status);
}

/// 一发问多少条。两条路都定 100：正文小、重来代价小，而一个大包（几百条）也就三四发。
constexpr int kLookupChunk = 100;

/// 长得像不像一个原版版本号。
///
/// 两条判据：**以数字开头**（Mojang 的版本号全都以数字开头 —— `1.20.1` / `24w14a` /
/// `26.2-snapshot-1`）＋ **一个空白都没有**。
///
/// 第二条是给 PCL 那种"目录名就是用户起的名字"的实例兜底的：`1.20.2-Fabric 0.17.2`
/// 以数字开头，可它**不是一个游戏版本** —— 写进清单之后收包的人那边会去找一个没有的版本，
/// 而包看上去是好的（这就是为什么不能只看第一个字符）。
static bool looksLikeGameVersion(const QString &text) {
    if (text.isEmpty() || !text.at(0).isDigit()) {
        return false;
    }
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            return false;
        }
    }
    return true;
}

/// 从 maven 坐标那一格（`<原版>-<加载器版本>`）里切出加载器版本。
///
/// NOTE: 切的是**第一个**横杠，不是最后一个：`1.12.2-14.23.5.2860` 要 `14.23.5.2860`，
/// 而 GTNH 那种 `1.7.10-10.13.4.1614-1.7.10` 后面还挂着一个游戏版本 —— 按最后一个
/// 横杠切会切出 `1.7.10`，正好是反的。没有横杠时整串就是版本号。
static QString loaderVersionFromMaven(const QString &mavenVersion) {
    const QString text = mavenVersion.trimmed();
    const int dash = text.indexOf(QLatin1Char('-'));
    return dash > 0 ? text.mid(dash + 1) : text;
}

/// `loaderEvidenceFromJson()` 认出 OptiFine 时往 `keyOut` 里填的那一格。
///
/// 它**不是**整合包依赖表里的键（Modrinth / CurseForge 都不认 OptiFine，所以
/// `ModpackImport` 那边没有 `optifineLoaderKey()`，也不该有 —— 清单里根本没这一格）。
/// 它唯一的作用是给"认出来了、是哪一个"一个统一的出口，读它的是
/// `loaderDisplayNameForEvidenceKey()` 和 `instanceModLoader()`。
static QString optifineEvidenceKey() { return QStringLiteral("optifine"); }

/// 上述那一串证据键 → **给界面看的加载器名**。
///
/// 前四条走 `loaderDisplayName()`（**必须逐字一致**：实例行前面那个标志就是拿这个名字去
/// 反查下标的，见 `defaultInstanceMark()`）；Quilt 是唯一一处例外 —— 我们装不出 Quilt
/// （见 `instanceLoaderInfo()` 那段），`loaderDisplayName()` 里没有它，可"有没有模组
/// 加载器"这个问题上它和别人一样算数，所以这里单独拼一个名字。它只喂两件事：实例设置里
/// 「模组」那一页开不开、以及实例行画什么（认不出来的名字一律画草方块）。
///
/// NOTE: 写成一张**逐条列出**的表而不是遍历比较（同 `loaderKindForDependencyKey()` 那条
/// 的规矩）：键那一边的唯一出处是 `ModpackImport` 那几个函数，漏一个的话症状是
/// "这个实例明明有加载器、界面上就是不认"，所以宁可让这一张表短到一眼看得完。
static QString loaderDisplayNameForEvidenceKey(const QString &key) {
    using Kind = VersionManager::LoaderKind;
    if (key == ModpackImport::fabricLoaderKey()) {
        return VersionManager::loaderDisplayName(Kind::Fabric);
    }
    if (key == ModpackImport::forgeLoaderKey()) {
        return VersionManager::loaderDisplayName(Kind::Forge);
    }
    if (key == ModpackImport::neoforgeLoaderKey()) {
        return VersionManager::loaderDisplayName(Kind::NeoForge);
    }
    if (key == optifineEvidenceKey()) {
        return VersionManager::loaderDisplayName(Kind::OptiFine);
    }
    if (key == ModpackImport::quiltLoaderKey()) {
        return VersionManager::loaderDisplayName(Kind::Quilt);
    }
    return QString();
}

/**
 * @brief 从一份**合并过的**版本 json 自己身上认加载器。
 *
 * 【这条路为什么必须有】PCL / HMCL 那一类启动器把整份 json 合进实例目录（真机上绝大多数
 * 实例都是这样）：它们的 `inheritsFrom` 是空的，`id` 就是用户给实例起的名字（`海波篮子`、
 * `1.21.11-Fabric 0.19.3`），于是按 id 认的那条路一步都走不出去 —— 走不出去的结果是
 * 「纯原版」，于是一个装着 133 个模组的 Fabric 实例会被导成一个**原版包**：收包的人那边
 * 模组都在、游戏起不来，而包看上去是好的。
 *
 * 【`mainClass` 不能当判据】`cpw.mods.bootstraplauncher.BootstrapLauncher` 是 Forge 与
 * NeoForge **共用**的，而 `net.minecraft.launchwrapper.Launch` 连纯原版 1.12.2 都是它。
 *
 * @param json 一份**没合并过**的版本 json（`rawVersionJson` 读回来的那份）。
 * @return 这份 json 有没有说出加载器（含"是 OptiFine，导不了"那一档 —— 那也算说出来了）。
 * 说出来时 `problemOut` 是"能不能导出"那一句：**空 = 能**。
 *
 * NOTE: 说到加载器时 `keyOut` / `versionOut` **一定会填**（含 OptiFine / Quilt 那两档，
 * 它们的 `problemOut` 非空 —— 也就是"说得出来、但导不了"）。导出那一路只在
 * `problemOut` 为空时读这两个出参，多填一份不影响它；而 `instanceModLoader()`
 * 两个都要（它问的是"是哪一个、哪一版"，不是"导不导得出去"）。
 */
bool loaderEvidenceFromJson(const QJsonObject &json, QString *keyOut, QString *versionOut,
                            QString *problemOut) {
    *problemOut = QString();

    QString fabricVersion;
    QString legacyForgeVersion;  ///< net.minecraftforge:forge:<原版>-<版本>（1.12 及以前）
    QString modernForgeVersion;  ///< net.minecraftforge:fmlloader:<原版>-<版本>（1.13 以后）
    QString optifineVersion;
    QString quiltVersion;
    bool neoForgeSeen = false;
    for (const QJsonValue &entry : json.value(QStringLiteral("libraries")).toArray()) {
        const QStringList parts =
            entry.toObject().value(QStringLiteral("name")).toString().split(QLatin1Char(':'));
        if (parts.size() < 3) {
            continue;
        }
        const QString group = parts.at(0);
        const QString artifact = parts.at(1);
        const QString version = parts.at(2);
        if (group == QLatin1String("net.fabricmc") && artifact == QLatin1String("fabric-loader")) {
            fabricVersion = version;
        } else if (group == QLatin1String("net.minecraftforge") &&
                   artifact == QLatin1String("forge")) {
            legacyForgeVersion = version;
        } else if (group == QLatin1String("net.minecraftforge") &&
                   artifact == QLatin1String("fmlloader")) {
            modernForgeVersion = version;
        } else if (group == QLatin1String("net.neoforged.fancymodloader") &&
                   artifact == QLatin1String("loader")) {
            neoForgeSeen = true;
        } else if (group == QLatin1String("optifine") && artifact == QLatin1String("OptiFine")) {
            optifineVersion = version;
        } else if (group == QLatin1String("org.quiltmc") &&
                   artifact == QLatin1String("quilt-loader")) {
            quiltVersion = version;
        }
    }

    // OptiFine 排在最前面：装了加载器**又**装了 OptiFine 的实例两种都会命中，而按规范它
    // 是"导不了"的那一档（id 那条路也是先看 OptiFine 再看别的加载器，顺序保持一致）。
    if (!optifineVersion.isEmpty()) {
        *problemOut = optifineExportProblem(optifineVersion);
        // 这两个出参照填（见函数头那条 NOTE）。键写成 optifineEvidenceKey()，理由在那一处。
        if (keyOut != nullptr) {
            *keyOut = optifineEvidenceKey();
        }
        if (versionOut != nullptr) {
            *versionOut = optifineVersion;
        }
        return true;
    }
    if (!quiltVersion.isEmpty()) {
        // 那句"清单写得下、我们装不了"的说明只有一份（loaderKeyExportProblem 里）。
        *problemOut = loaderKeyExportProblem(ModpackImport::quiltLoaderKey(), quiltVersion);
        if (keyOut != nullptr) {
            *keyOut = ModpackImport::quiltLoaderKey();
        }
        if (versionOut != nullptr) {
            *versionOut = quiltVersion;
        }
        return true;
    }

    QStringList declared;
    const QJsonValue arguments = json.value(QStringLiteral("arguments"));
    if (arguments.isObject()) {
        declared = VersionArguments::declaredArguments(
            arguments.toObject().value(QStringLiteral("game")).toArray());
    }
    const auto declaredValue = [&declared](const QString &flag) {
        const int at = declared.indexOf(flag);
        return (at >= 0 && at + 1 < declared.size()) ? declared.at(at + 1) : QString();
    };

    const auto fill = [keyOut, versionOut](const QString &key, const QString &version) {
        *keyOut = key;
        *versionOut = version;
        return true;
    };

    if (!fabricVersion.isEmpty()) {
        return fill(ModpackImport::fabricLoaderKey(), fabricVersion);
    }

    if (neoForgeSeen) {
        const QString neoForgeVersion = declaredValue(QStringLiteral("--fml.neoForgeVersion"));
        if (neoForgeVersion.isEmpty()) {
            // 宁可明说导不了，也不要拿 FML 那个 4.x 顶上去：清单里那一格是"别人照着装的
            // 版本号"，写错的话收包的人装出来的是另一个 NeoForge。
            *problemOut = QObject::tr(
                "无法确定实例的 NeoForge 版本。版本文件缺少 --fml.neoForgeVersion，不能用 FML 版本号替代。");
            return true;
        }
        return fill(ModpackImport::neoforgeLoaderKey(), neoForgeVersion);
    }

    const QString forgeFromMaven =
        loaderVersionFromMaven(modernForgeVersion.isEmpty() ? legacyForgeVersion
                                                           : modernForgeVersion);
    if (!forgeFromMaven.isEmpty()) {
        return fill(ModpackImport::forgeLoaderKey(), forgeFromMaven);
    }
    const QString forgeDeclared = declaredValue(QStringLiteral("--fml.forgeVersion"));
    if (!forgeDeclared.isEmpty()) {
        return fill(ModpackImport::forgeLoaderKey(), forgeDeclared);
    }

    return false;
}

}  // namespace

bool VersionManager::isKnownGameVersion(const QString &id,
                                        const QList<MinecraftVersion> &knownVersions) {
    const QString text = id.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    // ① 清单在手（启动时取过一次，见 MainWindow::onVersionsFetched 那句日志）：**以它为准**。
    //
    // 这一条治的是两件**相反**的事，两件都在真机上量到过（只读探针
    // `mw_check instance-scan <数据目录>`， 拿用户的盘跑出来的）：
    // · **放行不以数字开头的老版本号**：`rd-132211` / `c0.30_01c` / `inf-20100618`
    // 都是清单里真有、也装得出来的版本，光看长相会把这一档整个漏掉；
    // · **拦住"长得像版本号、但 Mojang 根本没这个 id"的东西**：真机上有 `20w14∞`
    // （它的正式 id 是 `20w14infinite`）和 `26.2hyperland`（Hyperlands 那个私服客户端）
    // —— 写进清单收包的人那边没有这个版本可装，和"目录名不像版本号"是同一个病，
    // 只是长得更像、光看长相拦不住。
    //
    // 严格到这一步的代价只有"清单比实例旧"这一种情形（刚出的快照不在缓存里），而清单是
    // 启动时取的、能装出来的版本必然在里面，所以那种情形实际上要"用户手工丢进来一个
    // 比清单还新的目录"才会撞上 —— 撞上了也只是**报一句、不写假版本号**，重启就恢复。
    if (!knownVersions.isEmpty()) {
        for (const MinecraftVersion &version : knownVersions) {
            if (version.id == text) {
                return true;
            }
        }
        return false;
    }
    // ② 清单没取到（离线第一趟）：只能看长相，**这里的放宽是有意的** —— 宁可放行一个可疑的
    // 版本号，也不要"离线时一个实例都导不出去"。挡着的还是 `原版ATT2地图（无模组）` /
    // `1.20.2-Fabric 0.17.2` 这类最明显的假版本号。
    return looksLikeGameVersion(text);
}

QString VersionManager::instanceLoaderInfo(const QString &instanceId, QString *keyOut,
                                           QString *versionOut) {
    if (keyOut != nullptr) {
        keyOut->clear();
    }
    if (versionOut != nullptr) {
        versionOut->clear();
    }
    const QString start = instanceId.trimmed();
    if (start.isEmpty()) {
        return tr("没有给出实例名，不知道要导出哪一个实例。");
    }

    // 在线清单那一份（没取到就是空表），只给 loaderIdParts 里 Forge 那一条用：那条的判据
    // 要问"左边那半是不是一个真游戏版本"，出处是 isKnownGameVersion()。取法和
    // instanceGameVersion() 里那句逐字相同（它也是**现取**，不缓存 —— 清单是启动时那一份）。
    const QList<MinecraftVersion> knownVersions = downloadManager->getAvailableVersions();

    // ---- 第一条路：顺着版本 json 的继承链看那一串 id ----
    //
    // 这一条排在前面，因为它是**装在盘上的东西自己说的**：装加载器出来的目录名就是那个
    // id，导入整合包出来的薄 json 的 inheritsFrom 也指着它。
    QSet<QString> seen;
    QString id = start;
    // 链上每一份**没合并过**的 json，从实例自己那份开始。第三条路要用（见下），顺手存着
    // 免得再读一遍盘。注意：认出来了就 return 了，所以攒下来的那些一定是"还没认出来"时
    // 走过的整条链。
    QList<QJsonObject> chainJsons;
    for (int depth = 0; depth < VersionInheritance::maxDepth() && !id.isEmpty(); ++depth) {
        if (seen.contains(id)) {
            // 环（手写的 json 可以是 A→B→A）。上面几层里该认的已经认过了，往下没有信息
            // 可挖 —— 这不是错误。
            break;
        }
        seen.insert(id);

        // OptiFine 单独认一次：它那个 id 是安装器自己拼的（`1.18.2-OptiFine_HD_U_H9`），
        // 而且在两种清单里都没有对应的键，loaderIdParts 里根本没有它那一档。
        //
        // 同一个**不带头锚点**的毛病（和 loaderIdParts 里 Forge 那条一样，那
        // 边有完整来路）—— 左边那半也得是一个真游戏版本。不收这一下的话，用户把实例改名
        // 成「我的-OptiFine_版」就会被当成"OptiFine 实例"，而这个分支的出路是**拒绝导出**：
        // 一个好好的实例，因为名字里带了几个字，就一个包都导不出去，报的还是"两种清单里
        // 都没有 OptiFine 那一格"。右边那半不另判长相 —— OptiFine 的版本名
        // （`HD_U_H9`）是它自己一套写法，没有能便宜判出来的形状。
        static const QString kOptiFine = QStringLiteral("-OptiFine_");
        const int optifine = id.indexOf(kOptiFine);
        if (optifine > 0 &&
            VersionManager::isKnownGameVersion(id.left(optifine), knownVersions)) {
            return optifineExportProblem(id.mid(optifine + kOptiFine.size()));
        }

        QString key;
        QString version;
        if (loaderIdParts(id, knownVersions, &key, &version)) {
            const QString problem = loaderKeyExportProblem(key, version);
            if (problem.isEmpty()) {
                if (keyOut != nullptr) {
                    *keyOut = key;
                }
                if (versionOut != nullptr) {
                    *versionOut = version;
                }
            }
            return problem;
        }
        const QJsonObject json = rawVersionJson(id);
        chainJsons.append(json);
        id = VersionInheritance::parentIdOf(json);
    }

    // ---- 第二条路：从**那份 json 自己身上**认（libraries + arguments.game）----
    //
    // 罩的是 PCL / HMCL 那一类"整份 json 都合进实例目录"的实例 —— 真机上绝大多数都是。
    // 它们的链上一步都走不出去（`inheritsFrom` 是空的，`id` 是用户给实例起的名字），
    // 所以第一条路问不出任何东西；而它们**照样**把加载器明明白白写在 libraries 里。
    // 少了这一条，一个装着 133 个模组的 Fabric 实例会被下面那条 meta 路当成**纯原版**
    // 导出去。
    //
    // 顺序是**从实例自己那份开始**（链越靠前越贴着这个实例），一份一份往下看。
    for (const QJsonObject &json : chainJsons) {
        QString key;
        QString version;
        QString problem;
        if (!loaderEvidenceFromJson(json, &key, &version, &problem)) {
            continue;
        }
        if (problem.isEmpty()) {
            if (keyOut != nullptr) {
                *keyOut = key;
            }
            if (versionOut != nullptr) {
                *versionOut = version;
            }
        }
        return problem;
    }

    // ---- 第三条路：wtfml.meta 里我们自己记的那一段 ----
    //
    // 罩的是"链上没有、但 meta 里记着"的那一类（用户手写过一个 meta）。这张反查表是照着
    // loaderDisplayName() 拼出来的，所以不可能和写进去的那一份漂。
    const QJsonObject meta = readVersionMeta(start);
    const QString metaName = meta.value(QStringLiteral("loaderName")).toString().trimmed();
    const QString metaVersion = meta.value(QStringLiteral("loaderVersion")).toString().trimmed();
    if (metaName.isEmpty()) {
        // 链上没有任何加载器的痕迹、meta 里也没记 —— 这就是**纯原版实例**。这是正常结果
        // （不是"认不出来"）：盘上没有任何一处说它装了加载器。
        return QString();
    }
    // 大小写不敏感：这一段是给界面看的显示名，用户手写过的 meta 里写 "fabric" 也认。
    if (metaName.compare(loaderDisplayName(LoaderKind::OptiFine), Qt::CaseInsensitive) == 0) {
        return optifineExportProblem(metaVersion);
    }
    if (metaName.compare(loaderDisplayName(LoaderKind::Quilt), Qt::CaseInsensitive) == 0) {
        // Quilt 不在安装页那张 0..3 的索引表里（见 loaderKindCount），但导出时它
        // 有完整的清单键 —— 不能因为"索引表里没有"就把它当认不出来。
        if (keyOut != nullptr) *keyOut = ModpackImport::quiltLoaderKey();
        if (versionOut != nullptr) *versionOut = metaVersion;
        return QString();
    }
    for (int index = 0; index < loaderKindCount(); ++index) {
        LoaderKind kind = LoaderKind::Fabric;
        if (!loaderKindFromIndex(index, &kind)) {
            break;
        }
        if (metaName.compare(loaderDisplayName(kind), Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString key = dependencyKeyForLoaderKind(kind);
        if (key.isEmpty()) {
            // 上面已经把 OptiFine 那一档拦掉了，走到这里说明那张表里多了一个还没有键的
            // 加载器（新加 LoaderKind 忘了接这里）—— 如实报，别当成纯原版往下导。
            return tr("当前导出格式不支持加载器「%1」，导出已取消。")
                .arg(metaName);
        }
        if (keyOut != nullptr) {
            *keyOut = key;
        }
        if (versionOut != nullptr) {
            *versionOut = metaVersion;
        }
        return QString();
    }
    return tr("实例的 wtfml.meta 声明了未知加载器「%1」，导出已取消。若实例实际使用 Fabric、Forge 或 NeoForge，请将 loaderName 改为相应名称后重试。")
        .arg(metaName);
}

QString VersionManager::instanceGameVersion(const QString &instanceId) {
    const QString start = instanceId.trimmed();
    if (start.isEmpty()) {
        return QString();
    }
    // 在线清单那一份（没取到就是空表）。判据里那一半"清单里真有这个 id"要用它。
    const QList<MinecraftVersion> knownVersions = downloadManager->getAvailableVersions();
    // `usable()` 在这个函数里至少被调 4 次（① ③ 的循环 ④ ⑤），而
    // `isKnownGameVersion()` 对**非空**清单是线性扫整份（900+ 条）—— 先把 id 收进一个
    // QSet，查一次是 O(1)。清单为空（离线第一趟）时照旧交给 `isKnownGameVersion()`，
    // 那一支是"看长相"的放宽判据，不能省。
    QSet<QString> knownVersionIds;
    knownVersionIds.reserve(knownVersions.size());
    for (const MinecraftVersion &version : knownVersions) {
        knownVersionIds.insert(version.id);
    }
    const auto usable = [&knownVersions, &knownVersionIds](const QString &text) {
        return knownVersions.isEmpty() ? isKnownGameVersion(text, knownVersions)
                                       : knownVersionIds.contains(text);
    };

    // ① 我们自己记的那一格。它是**最准的**：装的时候就是从清单里抄下来的，显示名用的也是它。
    const QJsonObject meta = readVersionMeta(start);
    const QString realVersion = meta.value(QStringLiteral("realVersion")).toString().trimmed();
    if (usable(realVersion)) {
        return realVersion;
    }

    // ② 顺继承链走到末端，顺手把每一份 json 存下来给 ③ 用。链上每一层的 id 都得从
    // **没合并过**的那份 json 里读（getVersionJson() 会把 inheritsFrom 吃掉，
    // 走一步就到底了，见它自己的说明）。防环 + 深度上限和别处同一对常量
    // （VersionInheritance 里那两个）。
    QSet<QString> seen;
    QString id = start;
    QString last;
    QList<QJsonObject> chainJsons;
    for (int depth = 0; depth < VersionInheritance::maxDepth() && !id.isEmpty(); ++depth) {
        if (seen.contains(id)) {
            break;
        }
        seen.insert(id);
        last = id;
        const QJsonObject json = rawVersionJson(id);
        chainJsons.append(json);
        id = VersionInheritance::parentIdOf(json);
    }

    // ③ 链上某一份 json 自己写着它基于哪个原版 —— `clientVersion` 那一格。
    //
    // 这是 PCL / HMCL 那类**合并过的**实例唯一说得出口的地方（真机上绝大多数实例都是）：
    // 它们的 `id` 就是用户给实例起的名字（`海波篮子`、`1.21.11-Fabric 0.19.3`）、
    // `inheritsFrom` 是空的，所以 ① 没有、② 走不动，而 `clientVersion` 是真值。
    // 少了这一条，这些实例一个都导不出去 —— 报出来是「导出失败：读不出 海波篮子 基于哪个原版」。
    //
    // 从**末端**往前找：链越靠末越接近原版那一份。Mojang 自己那份 json 里没有这一格，
    // 所以薄 json（我们自己的整合包导入、加载器安装）不受影响，照旧落到 ④。
    for (auto it = chainJsons.crbegin(); it != chainJsons.crend(); ++it) {
        const QString clientVersion =
            it->value(QStringLiteral("clientVersion")).toString().trimmed();
        if (usable(clientVersion)) {
            return clientVersion;
        }
    }

    // ③b 链上某一份 json 自己写着 `id`，且那个 id 认得出是游戏版本。
    //
    // 薄 json（目录名 `1.20.1-T35`，文件里 `id` 写 `1.20.1`）只有这一格说得出口：
    // 目录名不是版本号、`inheritsFrom` 也没有。只在 `usable()` 认它时才采用 ——
    // 合并过的实例 json 里 `id` 往往是用户起的名字，不能当版本号。
    for (const QJsonObject &json : chainJsons) {
        const QString declared = json.value(QStringLiteral("id")).toString().trimmed();
        if (usable(declared)) {
            return declared;
        }
    }

    // ④ 再兜一层：链的末端那个 **id**。薄 json 走的正是这里 —— 它的 inheritsFrom 最后
    // 指到原版那一份，而原版那份目录名就是版本号。
    if (usable(last)) {
        return last;
    }

    // ⑤ 最后一层：实例自己的目录名。这是几条路里最不可信的一个（真机上有
    // `原版ATT2地图（无模组）`、`1.20.2-Fabric 0.17.2` 这种），所以除了"在清单里"
    // 就只有"清单里有"或者"长得像版本号"（以数字开头 + 没有空白）才敢用 ——
    // 判据的出处是 isKnownGameVersion()。
    return usable(start) ? start : QString();
}

bool VersionManager::gameVersionSupportsDatapacks(const QString &versionId,
                                                  const QString &releaseTime) {
    // 数据包是快照 17w43a引进的。判据见 .hpp 那段说明。
    //
    // ① 盘上那份 json 自己写着什么时候发布的 ⇒ 按日期比。**离线也准**，快照和远古版
    // 全都分得对（它们的 releaseTime 就是真日期）。
    const QString stamp = releaseTime.trimmed();
    if (!stamp.isEmpty()) {
        const QDateTime released = QDateTime::fromString(stamp, Qt::ISODate);
        if (released.isValid()) {
            // 只比**日期**（`releaseTime` 带的是 `+00:00` 偏移，换算到 UTC 再取日期，
            // 免得"离那一天只差几个钟头"把结果翻过去）。卡在 2017-11-01 而不是 17w43a
            // 那一天本身：17w43a 是 10-25 发的，但它之后还有 17w44a…17w47a 一串快照，
            // 全都带数据包。留几天富裕比卡在当天稳 —— **多给一页**是可以修的，
            // 静默藏掉一页不行（见 .hpp 里那条 NOTE）。
            return released.toUTC().date() >= QDate(2017, 11, 1);
        }
    }

    // ② 没读到日期（薄 json、手写的 json）⇒ 看版本号本身。以数字开头的按「主.次」比。
    // 不是数字开头的（`a1.2.6` / `b1.7.3` / 用户起的名字）走到这儿 ⇒ 给（下面那条
    // "问不出来就给"的规矩）。
    const QString text = versionId.trimmed();
    if (text.isEmpty() || !text.at(0).isDigit()) {
        return true;
    }
    // 到第一个不是数字也不是点的字符为止：`1.20.1` → `1.20.1`、`1.13-pre1` → `1.13`、
    // `26.2-snapshot` → `26.2`。`20w14∞` 那种也是数字开头，取出来是 `20` ⇒ 判成"有"
    // （它是 2020 年的快照，本来就有 —— 取成 `20` 正好判对）。
    int end = 0;
    while (end < text.size() && (text.at(end).isDigit() || text.at(end) == QLatin1Char('.'))) {
        ++end;
    }
    const QStringList parts = text.left(end).split(QLatin1Char('.'));
    const int major = parts.value(0).toInt();
    const int minor = parts.value(1).toInt();
    return major > 1 || (major == 1 && minor >= 13);
}

bool VersionManager::instanceSupportsDatapacks(const QString &instanceId) {
    const QString start = instanceId.trimmed();
    if (start.isEmpty()) {
        return true;  // 问不出来就给（同 gameVersionSupportsDatapacks 的那条规矩）
    }
    // 合并过的那一份 json：`releaseTime` 会从父那层继承上来（VersionInheritance::merge
    // 里那一串"子非空用子、否则用父"），所以薄 json 的加载器实例照样问得出来。
    const QJsonObject merged = getVersionJson(start);
    const QString releaseTime = merged.value(QStringLiteral("releaseTime")).toString();
    // 版本号用 instanceGameVersion()：它认得合并实例的 clientVersion / 链末端 / 目录名，
    // 而且每一格都过了 isKnownGameVersion()。
    return gameVersionSupportsDatapacks(instanceGameVersion(start), releaseTime);
}

bool VersionManager::instanceModLoader(const QString &instanceId, QString *nameOut,
                                       QString *versionOut) const {
    const InstanceTraits traits = instanceTraits(instanceId);
    if (nameOut != nullptr) {
        *nameOut = traits.loaderName;
    }
    if (versionOut != nullptr) {
        *versionOut = traits.loaderVersion;
    }
    return !traits.loaderName.isEmpty();
}

VersionManager::InstanceTraits VersionManager::instanceTraits(const QString &instanceId) const {
    InstanceTraits traits;
    const QString start = instanceId.trimmed();
    if (start.isEmpty()) {
        return traits;
    }

    // ---- meta 那半：两个答案都先看这里 ----
    //
    // 一次读，供两条判据用（见头文件里"为什么合并成一次"）。
    const QJsonObject meta = versionMeta(start);
    const QString metaName = meta.value(QStringLiteral("loaderName")).toString().trimmed();
    if (!metaName.isEmpty()) {
        traits.loaderName = metaName;
        traits.loaderVersion = meta.value(QStringLiteral("loaderVersion")).toString().trimmed();
    }
    traits.type = meta.value(QStringLiteral("type")).toString();
    if (!traits.loaderName.isEmpty() && !traits.type.isEmpty()) {
        return traits;  // 两个答案都齐了，版本 json 那半一个字节都不用读
    }

    // ---- json 那半：只在缺答案时才去问 ----
    //
    // 那道"缓存里有没有"是必须的：`mergedJsonEntry()` 走到 getVersionJson() 时，文件不在
    // 会 qWarning，而这个函数每次刷新实例列表都对每个实例跑一遍（同 versionChannelOf()
    // 那条闸的理由）。缓存里已经有这一格 = 之前读成功过，可以直接问。
    if (!mergedVersionJsons.contains(start) && !QFile::exists(InstanceFiles::jsonPath(start))) {
        return traits;
    }
    const MergedJsonEntry *entry = mergedJsonEntry(start);
    if (entry == nullptr) {
        return traits;
    }
    if (!entry->factsScanned) {
        // 一份 json 只认一遍：`libraries` 扫一遍、`arguments.game` 展开一遍都是实打实的
        // 钱，而它们跟着这份 json 变，所以在同一个缓存格里记着就够了（见 MergedJsonEntry）。
        QString key;
        QString version;
        QString problem;  // "认得出来但导不了"那一句 —— 这里只问"是哪个"，丢掉。
        entry->loaderFound = loaderEvidenceFromJson(entry->json, &key, &version, &problem);
        entry->loaderKey = key;
        entry->loaderVersion = version;
        entry->type = entry->json.value(QStringLiteral("type")).toString();
        entry->factsScanned = true;
    }
    if (traits.loaderName.isEmpty() && entry->loaderFound) {
        const QString name = loaderDisplayNameForEvidenceKey(entry->loaderKey);
        if (!name.isEmpty()) {
            traits.loaderName = name;
            traits.loaderVersion = entry->loaderVersion;
        } else {
            // 认出来了、但那张表里没有它的名字：**当成认不出来**（名字是给界面用的）。
            // 这句话现在一个实例一生只说一次（以前是每刷新一次说一遍）。
            qWarning() << "Loader evidence key not in the display table:" << entry->loaderKey
                       << "for" << start;
        }
    }
    if (traits.type.isEmpty()) {
        traits.type = entry->type;
    }
    return traits;
}

QString VersionManager::lookupModrinthVersionFiles(
    const QStringList &sha1s, QHash<QString, ModpackExport::ModrinthHit> *out) {
    if (out == nullptr) {
        return tr("内部错误：没有给出放结果的地方。");
    }
    out->clear();
    if (sha1s.isEmpty()) {
        return QString();  // 空表那一发不该发出去（回复必然是空 map）
    }
    // 先按哈希去重再分批：内容相同的两个 jar（比如同一个文件换个名字）只问一次。
    // 只靠请求体那一层去重仍会把它们分到两批里 —— 第二批发出去只剩几条，白等一个 RTT。
    const QStringList wanted = ModpackExport::uniqueSha1s(sha1s);

    // User-Agent **必须带**：Modrinth 的接口规矩里写着要能认出是谁在问（它按这个封滥用）。
    // 和 ResourceStore 那两处是同一个串，但**不能共用一份**（一个在 download/、一个在
    // manage/，为一行字建一个头文件不划算）；改的时候记得两边一起看。
    const QList<QPair<QByteArray, QByteArray>> headers{
        {QByteArrayLiteral("User-Agent"), QByteArrayLiteral("WTFML")}};

    for (int offset = 0; offset < wanted.size(); offset += kLookupChunk) {
        const QStringList chunk = wanted.mid(offset, kLookupChunk);
        const QByteArray body = ModpackExport::modrinthVersionFilesRequestBody(chunk);
        if (body.isEmpty()) {
            continue;
        }
        // 根地址：验证工程的口子优先（见 setModrinthApiBaseOverride），否则官方那一份
        // （唯一出处仍是 ResourceCatalog 里那个 apiRoot()）。同一套算法在
        // `modrinthApiRoot()` 里，预设兼容检查也问那里。
        const QString root = modrinthApiRoot();
        const HttpClient::Reply reply = HttpClient::send(
            loaderNetworkManager(), QUrl(root + QStringLiteral("/version_files")),
            HttpClient::Verb::Post, body, QByteArrayLiteral("application/json"), QByteArray(),
            HttpClient::kDefaultTimeoutMs, headers);
        if (!reply.ok()) {
            // 整件事停下（调用点那边是"要不要继续"由用户决定的那一档）：一半的批次认出来、
            // 一半没认出来，交给用户的报告就成了半真半假的东西。
            return modrinthLookupFailureText(reply);
        }
        QString problem;
        const QHash<QString, ModpackExport::ModrinthHit> part =
            ModpackExport::parseModrinthVersionFiles(reply.raw, &problem);
        if (!problem.isEmpty()) {
            return problem;
        }
        for (auto it = part.constBegin(); it != part.constEnd(); ++it) {
            out->insert(it.key(), it.value());
        }
    }
    return QString();
}

QString VersionManager::lookupCurseForgeFingerprints(
    const QList<quint32> &fingerprints, QHash<quint32, ModpackExport::CurseForgeHit> *out) {
    if (out == nullptr) {
        return tr("内部错误：没有给出放结果的地方。");
    }
    out->clear();
    if (fingerprints.isEmpty()) {
        return QString();
    }
    // 同 Modrinth 那一半：内容相同的两个文件指纹一样，先去掉重复再分批。
    const QList<quint32> wanted = ModpackExport::uniqueFingerprints(fingerprints);

    // 和 resolveCurseForgeFiles 同一个形状：key 是用户自己填的才带（并且**只有**带了它才走
    // 官方那台），空串走那台不要 key 的社区镜像。base 自带 `/v1`（见 curseForgeApiBase）。
    const QString base =
        ModpackImport::curseForgeApiBase(m_curseForgeApiKey, m_curseForgeApiBaseOverride);
    const bool hasKey = !m_curseForgeApiKey.isEmpty();
    QList<QPair<QByteArray, QByteArray>> headers;
    if (hasKey) {
        headers.append({QByteArrayLiteral("x-api-key"), m_curseForgeApiKey.toUtf8()});
    }

    for (int offset = 0; offset < wanted.size(); offset += kLookupChunk) {
        const QList<quint32> chunk = wanted.mid(offset, kLookupChunk);
        const QByteArray body = ModpackExport::curseForgeFingerprintsRequestBody(chunk);
        if (body.isEmpty()) {
            continue;
        }
        const HttpClient::Reply reply = HttpClient::send(
            loaderNetworkManager(), QUrl(base + QStringLiteral("/fingerprints")),
            HttpClient::Verb::Post, body, QByteArrayLiteral("application/json"), QByteArray(),
            HttpClient::kDefaultTimeoutMs, headers);
        if (!reply.ok()) {
            if (!reply.networkError.isEmpty()) {
                return tr("问 CurseForge 要文件信息时没连上：%1\n\n%2")
                    .arg(reply.networkError, curseForgeKeyHint(hasKey));
            }
            if (reply.status == HttpClient::kStatusTooManyRequests) {
                return HttpClient::throttledText(tr("问 CurseForge 要文件信息"));
            }
            const QString apiError = reply.json.value(QStringLiteral("error")).toString().trimmed();
            return tr("CurseForge 的接口回了 HTTP %1%2\n\n%3")
                .arg(reply.status)
                .arg(apiError.isEmpty() ? QString() : tr("（%1）").arg(apiError), curseForgeKeyHint(hasKey));
        }
        QString problem;
        const QHash<quint32, ModpackExport::CurseForgeHit> part =
            ModpackExport::parseCurseForgeFingerprints(reply.raw, &problem);
        if (!problem.isEmpty()) {
            return problem;
        }
        for (auto it = part.constBegin(); it != part.constEnd(); ++it) {
            out->insert(it.key(), it.value());
        }
    }
    return QString();
}

ModpackExport::ExportPlan VersionManager::buildExportPlan(
    const ModpackExport::ExportRequest &request, const ModpackExport::ExportProgress &progress,
    const ModpackExport::ExportCancel &cancel) {
    // 两条反查路就地接上这一层的两个方法。走一层 std::function 是**有意的**：那一层
    // （ModpackExport）因此一个字节的网络都不碰，验证工程能喂假接口把整条路跑完。
    const ModpackExport::ModrinthQuery queryModrinth =
        [this](const QStringList &sha1s, QHash<QString, ModpackExport::ModrinthHit> *out,
               QString *problemOut) {
            const QString problem = lookupModrinthVersionFiles(sha1s, out);
            if (problemOut != nullptr) {
                *problemOut = problem;
            }
            return problem.isEmpty();
        };
    const ModpackExport::CurseForgeQuery queryCurseForge =
        [this](const QList<quint32> &fingerprints,
               QHash<quint32, ModpackExport::CurseForgeHit> *out, QString *problemOut) {
            const QString problem = lookupCurseForgeFingerprints(fingerprints, out);
            if (problemOut != nullptr) {
                *problemOut = problem;
            }
            return problem.isEmpty();
        };
    return ModpackExport::buildExportPlan(request, queryModrinth, queryCurseForge, progress, cancel);
}

QString VersionManager::writePackArchive(const ModpackExport::ExportPlan &plan,
                                         const QString &targetZip,
                                         const std::function<bool()> &cancelled) {
    if (plan.cancelled) {
        // 用户叫停：整份 plan 都作废（overrides 里可能有半截的东西），什么都不写。
        return tr("导出已经取消了。");
    }
    if (!plan.problem.isEmpty()) {
        return plan.problem;
    }
    if (targetZip.trimmed().isEmpty()) {
        return tr("没有指定导出到哪个文件。");
    }
    if (plan.instanceDir.trimmed().isEmpty()) {
        // 认不出来的那些文件只有相对路径，取不出盘上那一份 —— 硬打出来的包会缺东西，而
        // zip 本身是好的。这是调用方的编程错误，如实报（别打出一个看着像整合包的残包）。
        return tr("导出计划缺少实例目录，无法读取来源未知的文件。");
    }

    // 清单先写进一个临时目录 —— 打包那一步只认盘上的文件（ZipUtil::createZip 就是照
    // "东西已经在盘上"设计的）。
    QTemporaryDir staging;
    if (!staging.isValid()) {
        return tr("无法创建临时目录以准备整合包：%1").arg(staging.errorString());
    }
    const QString manifestPath =
        QDir(staging.path()).absoluteFilePath(ModpackExport::indexFileName(plan.spec.format));
    QSaveFile manifest(manifestPath);
    if (!manifest.open(QIODevice::WriteOnly)) {
        return tr("写不了整合包的清单文件 %1。").arg(QDir::toNativeSeparators(manifestPath));
    }
    // QSaveFile 的 commit() 在失败时**不会**把目标文件留在盘上（和 writeVersionJson 同一个
    // 理由：这里坏掉的后果是打出一个清单半截的包）。
    manifest.write(ModpackExport::buildIndexFile(plan.spec));
    if (!manifest.commit()) {
        return tr("写不了整合包的清单文件 %1。").arg(QDir::toNativeSeparators(manifestPath));
    }

    // 「先写 .part、成功了再改名」：用户中途关掉启动器、或者磁盘满了，盘上留下的是一个
    // `xxx.mrpack.part`，而不是一个看着像整合包、双击却打不开的文件。
    //
    // **不删**已经在那儿的 `<目标>.part` / `<目标>.bak`，换一个没人用的
    // 名字（`.part-2`、`.bak-2`…）。这两个后缀太常见了 —— 别的下载器留下的半截、用户
    // 自己手打的一份备份，都可能正好叫这个名字。若把那两行 `QFile::remove()` 加回来、
    // 把它们当"我们上一趟的残骸"直接删掉，删的就不是我们的文件（那个 `.bak` 尤其像用户
    // 自己的备份：我们自己的 `.bak` 只在成功换名之后存在几毫秒，崩溃留下的那种又只会在
    // 目标已经不存在时才出现，而那种情况这一趟根本不会走到这儿）。
    const auto freshSidecar = [](const QString &base, const QString &suffix) {
        QString candidate = base + suffix;
        for (int i = 2; QFile::exists(candidate) && i < 1000; ++i) {
            candidate = base + suffix + QLatin1Char('-') + QString::number(i);
        }
        return candidate;
    };
    const QString partPath = freshSidecar(targetZip, QStringLiteral(".part"));
    QString error;
    if (!ZipUtil::createZip(partPath, ModpackExport::archiveEntries(plan, manifestPath), &error,
                            cancelled)) {
        QFile::remove(partPath);
        if (cancelled && cancelled()) return tr("导出已经取消了。");
        return error.isEmpty() ? tr("打包失败。") : error;
    }
    // 收尾不能直接用 `QFile::rename(partPath, targetZip)`：Qt 6.10 的 QFile::rename 走的是
    // `::MoveFile`（不带 REPLACE_EXISTING，见 qfilesystemengine_win.cpp 的 renameFile()），
    // 目标已存在就是失败 —— 而"目标本来就在那儿"（用户在 QFileDialog 里选了一个已存在的
    // 包名、对话框问过"要不要替换"之后）正是最常见的收尾。
    //
    // 所以这个顺序：先把原来那份挪到 `<目标>.bak`，成了再删它；中途任何一步失败就把它放回
    // 去。整段里用户原来那份文件**要么还在原地、要么还在 .bak 上** —— 不会出现"包没换成功、
    // 原来那份也没了"。目标本来不存在时（第一次导出）就是一条干净的改名。
    const bool hadTarget = QFile::exists(targetZip);
    const QString backupPath = hadTarget ? freshSidecar(targetZip, QStringLiteral(".bak"))
                                         : QString();
    if (hadTarget) {
        if (!QFile::rename(targetZip, backupPath)) {
            // 挪不开就**一个字节都不动**：新包留在 .part 上，把它的位置说出来。
            return tr("无法移走原文件 %1，已保留原文件。\n新整合包位于 %2。")
                .arg(QDir::toNativeSeparators(targetZip),
                     QDir::toNativeSeparators(partPath));
        }
    }
    if (!QFile::rename(partPath, targetZip)) {
        if (hadTarget && !QFile::rename(backupPath, targetZip)) {
            // 放不回去是**真的坏消息**（原来那份在 .bak 上），所以这句话必须说清它还在。
            qWarning() << "Cannot restore the replaced pack archive:" << backupPath << targetZip;
            return tr("整合包已生成，但无法替换 %1，也无法恢复原文件。原文件位于 %2。")
                .arg(QDir::toNativeSeparators(targetZip),
                     QDir::toNativeSeparators(backupPath));
        }
        return tr("整合包已生成，但无法写入 %1。请检查目标是否被占用。\n新整合包位于 %2。")
            .arg(QDir::toNativeSeparators(targetZip), QDir::toNativeSeparators(partPath));
    }
    if (hadTarget) {
        QFile::remove(backupPath);
    }
    return QString();
}
