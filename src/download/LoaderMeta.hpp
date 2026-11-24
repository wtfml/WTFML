/**
 * @file LoaderMeta.hpp
 * @brief 模组加载器元数据接口的**纯函数**部分：地址怎么拼、列表回复怎么挑版本。
 * @note 这里不联网、不发信号，只做"字符串进、字符串出"。真正发请求的是
 * VersionManager::installLoaderVersion()（它才是 QObject），这样这几条规则能在
 * 验证工程里离线钉住 —— 联网的那半边没法在沙箱里跑（要真下几个 GB 的资源）。
 *
 * 【Forge / NeoForge】它们不走"拉一份 json 再下库"那条路，而是**跑它们自己的安装器 jar**
 * （`java -jar xxx-installer.jar --installClient <游戏目录>`，见下面的注释）。这里只放
 * "版本从哪来、挑哪一个、安装器 jar 在哪个地址、装出来的目录叫什么"这四件事。
 *
 * Fabric 的接口形状（ 实测）：
 * GET /v2/versions/loader/<游戏版本>
 * -> 数组，每个元素 {loader:{version,stable,build,maven}, intermediary:{...},
 * launcherMeta:{version,libraries,mainClass,min_java_version}}
 * **按新到旧排序**，而且只有**最新那个**的 stable 是 true（0.19.5 是 true，
 * 0.19.4 及更早全是 false）—— 实测 253 条里 stable 恰好一条。
 * 回复约 589 KB（253 条 × 每条自带一整份 launcherMeta.libraries，占 99%）。
 * GET /v2/versions/loader/<游戏版本>/<加载器版本>/profile/json
 * -> 就是那份"薄 json"，原样写进 versions/<id>/<id>.json 即可（约 2.8 KB）。
 */
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QLatin1Char>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <limits>

namespace LoaderMeta {

/// Fabric 元数据的根地址（没有镜像源：BMCLAPI 不代理 Fabric 的 meta）。
inline QString fabricMetaBase() {
    return QStringLiteral("https://meta.fabricmc.net");
}

/// 某个游戏版本可用的加载器版本列表。
inline QString fabricLoaderListUrl(const QString &gameVersion) {
    return fabricMetaBase() + QStringLiteral("/v2/versions/loader/") + gameVersion;
}

/// 某个加载器版本的 profile json —— 内容就是落盘的那份版本 json。
inline QString fabricProfileUrl(const QString &gameVersion, const QString &loaderVersion) {
    return fabricLoaderListUrl(gameVersion) + QLatin1Char('/') + loaderVersion +
           QStringLiteral("/profile/json");
}

/**
 * @brief Fabric 装出来的版本 id。
 *
 * 协议是 `fabric-loader-<加载器版本>-<游戏版本>`，**不是我们发明**的：profile json 里的
 * `id` 字段就是这个，而它同时也是版本目录名 —— 别的启动器、加载器自己的安装器都按这个名字
 * 找目录，改一个字就等于"装好了但谁都不认识它"。
 */
inline QString fabricLoaderId(const QString &gameVersion, const QString &loaderVersion) {
    return QStringLiteral("fabric-loader-%1-%2").arg(loaderVersion, gameVersion);
}

/**
 * @brief 一个版本 id 能不能直接当目录名用（`versions/<id>/`）。
 *
 * 白名单和 LibraryRules::mavenPathForName() 那条是同一个（`[A-Za-z0-9._+-]`），理由也一样：
 * **内容来自网络、用途是拼路径**。加载器 profile 里的 `id` 是我们唯一一处"把网上的字符串
 * 直接当目录名"的地方（原版的版本 id 来自我们自己的 manifest，从来没从外面进来过），
 * 所以这里必须自己挡一道 —— `../` 那种 id 会让整个版本装到数据目录外面去。
 * 另外单独点名 `.` 和 `..`：它们能过白名单，但显然不是版本名。
 */
inline bool isSafeVersionId(const QString &id) {
    if (id.isEmpty() || id == QLatin1String(".") || id == QLatin1String("..")) {
        return false;
    }
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9._+-]+$"));
    return safe.match(id).hasMatch();
}

/// profile 检查没过的**哪一种**。
///
/// NOTE: 这里刻意只回一个枚举、不回已经拼好的话 —— 给用户看的那句话在
/// VersionManager 里（那里才有 tr()），两边分开才不会出现"文案在纯函数里、
/// 翻译表却扫不到"这种事。验证工程也断言这个枚举（比断言一句话稳）。
enum class ProfileProblem {
    None,
    BadId,        ///< profile 里的 id 空着或者不能当目录名（见 isSafeVersionId）
    WrongParent,  ///< profile 的 inheritsFrom 不是我们要装在它上面的那个版本
};

/// `checkFabricProfile()` 的结果。
struct ProfileCheck {
    ProfileProblem problem = ProfileProblem::None;
    QString id;      ///< profile 里的版本 id（版本目录名，也是最终 versionDownloadFinished 的 id）
    QString parent;  ///< profile 里的 inheritsFrom（出问题时报给用户看）

    bool ok() const { return problem == ProfileProblem::None; }
};

/**
 * @brief 检查一份 Fabric profile 能不能照这样装下去。
 * @param profile 接口返回的 profile（原样）
 * @param gameVersion 我们打算装在这个原版版本之上
 *
 * 两条规则，都是"宁可不装"：这个 json 会**原样**落盘成 `versions/<id>/<id>.json`，
 * 而它的 id 就是目录名、它的 inheritsFrom 决定启动时去哪找主类/客户端 jar。
 *
 * 1. `id` 必须是能当目录名的（见 isSafeVersionId）—— 不然是往数据目录外面写。
 * 2. `inheritsFrom` 必须就是 `gameVersion` —— 对不上就会装出一个"目录名叫 A、
 * 内容基于 B"的版本：启动时它去找 B，而用户以为自己装的是基于 A 的。
 */
inline ProfileCheck checkFabricProfile(const QJsonObject &profile, const QString &gameVersion) {
    ProfileCheck result;
    result.id = profile.value(QStringLiteral("id")).toString();
    result.parent = profile.value(QStringLiteral("inheritsFrom")).toString();
    if (!isSafeVersionId(result.id)) {
        result.problem = ProfileProblem::BadId;
        return result;
    }
    if (result.parent != gameVersion) {
        result.problem = ProfileProblem::WrongParent;
        return result;
    }
    return result;
}

/**
 * @brief 核对外部安装器装完之后留下的那份版本 json。
 * @param versionJson `versions/<id>/<id>.json` 的内容。
 * @param gameVersion 我们打算装在这个原版版本之上。
 * @param expectedId 我们按约定算出来的版本 id（= 目录名）。
 * @return 复用 ProfileCheck 那两个字段；problem 的含义和 checkFabricProfile() 一样。
 *
 * 为什么退出码 0 还不够：安装器的退出码是它自己说了算的，而**真正被写进磁盘的那份 json
 * 才是别的启动器和我们自己的启动路径要读的东西**。这里查两条：
 *
 * 1. json 里的 `id` 必须**等于我们算出来的那个目录名**。目录名是我们拼的（`forgeLoaderId`),
 * 而 json 是安装器写的；两者对不上意味着"Forge 换了命名约定"，我们接下来要么把
 * wtfml.meta 写到一个不存在的 id 上、要么把一个名字对不上的版本记成"装好了"。
 * 2. `inheritsFrom` 必须就是 `gameVersion`。对不上就是"装是装上了，但基于的是另一个原版"
 * —— 启动时它去找那个原版，而用户以为自己装的是基于这个的。
 *
 * NOTE: 和 checkFabricProfile() 用同一个结构体（ProfileCheck）和同一套 problem 枚举是
 * 刻意的：两处的失败模式一模一样（ID 不可用 / 父版本不对），文案由调用方按
 * problem 分派，多一套枚举只会多一处要同步的东西。
 */
inline ProfileCheck checkInstallerResult(const QJsonObject &versionJson, const QString &gameVersion,
                                         const QString &expectedId) {
    ProfileCheck result;
    result.id = versionJson.value(QStringLiteral("id")).toString();
    result.parent = versionJson.value(QStringLiteral("inheritsFrom")).toString();
    if (result.id != expectedId || !isSafeVersionId(result.id)) {
        result.problem = ProfileProblem::BadId;
        return result;
    }
    if (result.parent != gameVersion) {
        result.problem = ProfileProblem::WrongParent;
        return result;
    }
    return result;
}

/// 从加载器列表里挑出来的那一个。
struct LoaderPick {
    QString version;    ///< 加载器版本号；空 = 列表里没有能用的
    bool stable = false;  ///< 挑中的那个是不是被标了 stable

    bool ok() const { return !version.isEmpty(); }
};

/// 不钉具体版本号时按哪一档挑 —— 界面上那两行「下载最新版」/「下载稳定版」。
///
/// 四个加载器各有各的对应物，判据都取**上游自己的**（不是我们编的）：Fabric 用接口的 `stable`
/// 字段、NeoForge 用"版本号里没有 `-`/`+` 后缀"、Forge 用 promotions 的 `-recommended` /
/// `-latest`、OptiFine 用"是不是 `preview_` 预览版"。
enum class LoaderChannel {
    Stable,  ///< 稳定档：正式版（Forge 是它标着 recommended 的那一个）
    Newest,  ///< 最新档：连 alpha/beta/预览版一起算，挑上游最新发出来的那个
};

/// 列表里的一个加载器版本（"能列出来给用户挑"的那一份，见文件尾部那一节）。
struct LoaderVersionEntry {
    QString version;      ///< 加载器版本号（就是会写进 wtfml.meta 的 `loaderVersion`）
    bool stable = false;  ///< 是不是**稳定档**（正式版）。false = alpha/beta/快照/预览版 ——
                          /// < "能不能用"是另一件事（ 用户拍板："beta 版虽然不稳定，
                          /// < 但是也能使用"，所以现在照列，只在界面上标出「测试版」）。
                          /// < 判据按加载器不同：Fabric 用接口的 `stable` 字段；NeoForge 是"版本号
                          /// < 里没有 `-`/`+` 后缀"；OptiFine 是"不是 `preview_` 预览版"；Forge 那份
                          /// < maven 清单里**全**是正式版，所以一律 true（见 listForgeVersions）。
};

using LoaderVersionList = QList<LoaderVersionEntry>;

/// 频道在界面上那一格数据里的键（ 起：用户挑的可能是"具体版本号"，也可能是这两档）。
///
/// 写成 `@latest` / `@stable` 而不是空串或 `latest`：那一格（`Qt::UserRole`）平时放的是
/// **具体版本号**，而真版本号得过 isSafeVersionId() 那个白名单（`[A-Za-z0-9._+-]`）——
/// **`@` 不在里面**，所以这两个键不可能和真版本号撞车。
inline QString loaderChannelKey(LoaderChannel channel) {
    return channel == LoaderChannel::Newest ? QStringLiteral("@latest")
                                            : QStringLiteral("@stable");
}

/// loaderChannelKey() 的反函数。不是那两个键时回 false（调用方才知道这是"用户钉的具体版本号"）。
inline bool loaderChannelFromKey(const QString &key, LoaderChannel *channel) {
    if (channel == nullptr) {
        return false;
    }
    if (key == QStringLiteral("@latest")) {
        *channel = LoaderChannel::Newest;
        return true;
    }
    if (key == QStringLiteral("@stable")) {
        *channel = LoaderChannel::Stable;
        return true;
    }
    return false;
}

/// 界面上那一格（频道键 / 具体版本号 / 空串）-> 安装那条路怎么装。
///
/// 空串 = 没进过那棵树、也没挑过（比如版本列表根本没取到）：按**稳定档**算 —— 和改动前
/// "不钉版本"的行为一模一样（那时也是挑推荐/正式版），所以这条路不会因为多两个频道而变化。
struct LoaderRequest {
    QString pinnedVersion;  ///< 非空 = 用户钉死了这个版本号，`channel` 就不起作用了
    LoaderChannel channel = LoaderChannel::Stable;  ///< 没钉版本时按这一档挑

    bool pinned() const { return !pinnedVersion.isEmpty(); }
};

inline LoaderRequest loaderRequestFromChoice(const QString &choice) {
    LoaderRequest request;
    if (loaderChannelFromKey(choice, &request.channel)) {
        return request;
    }
    request.pinnedVersion = choice;
    return request;
}

/// 把条目列表抽成版本号列表（`stable` 那份信息在这里丢掉 —— 只有"要标出测试版"的地方
/// 才需要它，见 InstallPageController::fillLoaderVersionItems）。
inline QStringList loaderVersionStrings(const LoaderVersionList &entries) {
    QStringList versions;
    versions.reserve(entries.size());
    for (const LoaderVersionEntry &entry : entries) {
        versions.append(entry.version);
    }
    return versions;
}

/**
 * @brief `/v2/versions/loader/<mc>` 的回复 -> 能列给用户挑的全部加载器版本。
 * @param list 接口返回的数组（原样）。
 *
 * **顺序原样保留**（接口本来就是新到旧），不排序：这份列表里 stable 只有最新那条是 true，
 * 而版本号里带 `+build.N` 那种后缀，自己排序只会更容易排错（理由和 pickFabricLoader 一样）。
 */
inline LoaderVersionList listFabricLoaderVersions(const QJsonArray &list) {
    LoaderVersionList entries;
    entries.reserve(list.size());
    for (const QJsonValue &value : list) {
        const QJsonObject loader = value.toObject().value(QStringLiteral("loader")).toObject();
        const QString version = loader.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) {
            continue;
        }
        entries.append(LoaderVersionEntry{version, loader.value(QStringLiteral("stable")).toBool()});
    }
    return entries;
}

/**
 * @brief 从 `/v2/versions/loader/<mc>` 的回复里挑一个加载器版本。
 * @param list 接口返回的数组（原样）。
 * @return 挑中的版本；列表为空或没有 usable 条目时 `ok()` 为 false。
 *
 * 规则（@param channel 选哪一档，见 LoaderChannel）：
 * 稳定档 = **第一个 stable 的**（没有 stable 就退回第一个带版本号的）；
 * 最新档 = 接口给的**第一条**（这个接口保证按新到旧排，所以第一条就是最新的那个，
 * 它多半也是 stable 的那一条 —— 只有 Fabric 刚发了新加载器、还没标 stable 时不同）。
 *
 * 为什么是"第一个"而不是"比大小"：接口保证按新到旧排序（实测 253 条严格递减），
 * 而 stable 只有最新那个是 true。自己写一个版本号比较器（Fabric 的版本号里有
 * `0.1.0.48` 这种四段、也可能带 `+build.N`）反而更容易挑错 —— 而且"最新"本来就是
 * 这个接口的语义，不是我们要判断的事情。
 *
 * NOTE: 挑法只有这一处 —— "默认选中哪个"（这里的挑法）和"能挑哪些"（listFabricLoaderVersions）
 * 共用同一个解析，少一处抄歪的机会。
 */
inline LoaderPick pickFabricLoader(const QJsonArray &list,
                                   LoaderChannel channel = LoaderChannel::Stable) {
    const LoaderVersionList entries = listFabricLoaderVersions(list);
    if (entries.isEmpty()) {
        return LoaderPick{};
    }
    for (const LoaderVersionEntry &entry : entries) {
        if (channel == LoaderChannel::Newest || entry.stable) {
            return LoaderPick{entry.version, entry.stable};
        }
    }
    return LoaderPick{entries.first().version, false};
}

// ---------------------------------------------------------------------------
// Forge / NeoForge：跑它们自己的安装器 jar
// ---------------------------------------------------------------------------
//
// 【为什么这一段和上面 Fabric 那段完全不是一回事】
//
// Fabric 那边我们能自己拼出结果：一份 2.8 KB 的 profile json 落盘就是版本 json，剩下的
// 就是下 8 个 maven 库。Forge / NeoForge 不行 —— 它们装出来的东西里有**二进制补丁**
// （拿 vanilla 的 client jar 打出一份 srg 名 jar，再打出一份 client jar），那不是靠下载
// 能凑出来的。所以只能把它们的安装器请下来、**另起一个 Java 进程**跑。
//
// java -jar <安装器>.jar --installClient <游戏目录>
//
// * 目录里**必须已经有 launcher_profiles.json**。没有的话它直接拒绝：
// `There is no minecraft launcher profile in "...", you need to run the launcher first!`
// + `There was an error during installation`，退出码 1 —— 一个字节的库都不会下。
// 这是它唯一的前置条件（我们拿一个只有 launcher_profiles.json 的空目录试过，它照样装成）。
// * 成功时最后两行是 `Successfully installed client into launcher.` /
// `You can delete this installer file now if you wish`，退出码 0。
// * **自动的安装器是拿不到的**：真实下载页在广告后面。见下面 URL 那段的说明。
//
// Forge 47.4.10 → `versions/1.20.1-forge-47.4.10/1.20.1-forge-47.4.10.json`
// （inheritsFrom = 1.20.1，mainClass = cpw.mods.bootstraplauncher
// .BootstrapLauncher，29 个库，**没有 downloads 字段**）
// 外加 `versions/1.20.1/1.20.1.jar`（vanilla 的客户端 jar，sha1 与官方
// 逐字节相同）—— **但没有 1.20.1.json**。
// NeoForge 21.1.215 → `versions/neoforge-21.1.215/neoforge-21.1.215.json`
// 外加 `versions/1.21.1/` 的 json **和** jar。
// * 两个都**不下 assets**。所以"装完加载器"之后必须自己把原版那一套补全（resources
// 索引 + 5000 个对象），否则用户拿到的是一个启动就崩的版本 —— 补全复用现有的
// downloadVersion()，不是另写一份。

/// Forge 的"推广版本"表（`<游戏版本>-recommended` / `<游戏版本>-latest` 两两成对）。
///
/// 只有 4 KB，一次把**所有**游戏版本的 Forge 推荐版本都拿回来了。
inline QString forgePromotionsUrl() {
    return QStringLiteral(
        "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
}

/// 从 promotions_slim.json 里挑一个 Forge 版本。
/// @param promotions 接口返回的对象（原样，顶层有 `promos`）。
/// @param gameVersion 原版版本号（就是 `promos` 里键的前半段）。
/// @return 挑中的版本；这个游戏版本一个都没有时 `ok()` 为 false。
///
/// 规则：**优先 recommended，没有才用 latest**。
///
/// 为什么不是 latest：这两个键的语义是 Forge 自己给的 —— recommended 是"我们觉得这个
/// 版本可以日常用"，latest 是"这个游戏版本上最新发出来的"。实测 1.20.1 那一对是
/// 47.4.10（recommended）和 47.4.23（latest），而真机上装着的正是 47.4.10。
/// 还**必须**兜底到 latest：真表里有一大批游戏版本只有 latest 一个键（实测 1.20 只有
/// `46.0.14`、1.19.1 只有 `42.0.9`、1.21.6 只有 `56.0.9`），一个不兜底它们就都装不了。
/// NOTE: 别拿 1.7.10 当"只有 latest"的例子 —— 我一开始就是这么写的，而真表里它**两个键
/// 都有**（`10.13.4.1614` 各写一遍）。看着像常识的版本号也得回真数据里核一遍。
/// NOTE: 这里不自己比版本号大小 —— 键的语义是"Forge 推荐的"，不是"数字最大的"，
/// latest 也可能比 recommended 小（真发生过，见上）。
inline LoaderPick pickForgeVersion(const QJsonObject &promotions, const QString &gameVersion,
                                   LoaderChannel channel = LoaderChannel::Stable) {
    const QJsonObject promos = promotions.value(QStringLiteral("promos")).toObject();
    const QString recommended =
        promos.value(gameVersion + QStringLiteral("-recommended")).toString();
    const QString latest = promos.value(gameVersion + QStringLiteral("-latest")).toString();
    // 两个频道就是这两个键的两个次序（界面那两行「下载稳定版（release）」/「下载最新版
    // （alpha/beta）」）。**两个都要兜底**：真表里有一大批游戏版本只有一个键。
    if (channel == LoaderChannel::Newest) {
        if (!latest.isEmpty()) {
            return LoaderPick{latest, false};
        }
        if (!recommended.isEmpty()) {
            return LoaderPick{recommended, true};
        }
    } else {
        if (!recommended.isEmpty()) {
            return LoaderPick{recommended, true};
        }
        if (!latest.isEmpty()) {
            return LoaderPick{latest, false};
        }
    }
    return LoaderPick{};
}

/// Forge 安装器 jar 的地址（maven 上的坐标就是 `<游戏版本>-<Forge 版本>`）。
///
/// NOTE: 这是 maven 直链，不是下载页那个地址。安装器 jar 自己开头写着一段
/// `_comment_`：`Please do not automate the download and installation of Forge.
/// Our efforts are supported by ads from the download page.` —— 也就是说
/// **Forge 明确不希望别人自动装**。我们走 maven 直链是刻意的选择：下载页那条路
/// 要先过广告页拿 token，做成"替用户点广告"才是真的不礼貌；maven 上的 jar 是同一个
/// 文件、由 Forge 自己发布，走它至少没绕过任何东西。
inline QString forgeInstallerUrl(const QString &gameVersion, const QString &loaderVersion) {
    const QString full = gameVersion + QLatin1Char('-') + loaderVersion;
    return QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/%1/forge-%1-installer.jar")
        .arg(full);
}

/// Forge 安装器 jar 的**另一种坐标**：`<游戏>-<加载器>-<游戏>`。
///
/// NOTE: 老的游戏版本只发布了
/// 这一种坐标，常规那种（forgeInstallerUrl）**直接 404**。一个一个 curl 出来的表：
///
/// 坐标 1.7.10 1.8.9 1.9.4 1.11.2 1.12.2
/// <游戏>-<加载器> 404 404 404 200 200
/// <游戏>-<加载器>-<游戏> 200 200 200 404 404
///
/// 1.8.9 那两个都是官方推广表里会挑中的版本（11.15.1.1722 / 11.15.1.2318），所以这不是
/// "某个野生版本"，是这条路的正常输入。判断"该用哪一种"没有便宜的离线判据（两种都有
/// 先例、也不按版本号单调），所以**不猜**：按常规坐标下一遍，404 了就换成这一种再下
/// （见 VersionManager::startInstallerDownload / retryInstallerDownloadAlternate()）。
inline QString forgeInstallerUrlAlternate(const QString &gameVersion,
                                          const QString &loaderVersion) {
    const QString full = gameVersion + QLatin1Char('-') + loaderVersion;
    return QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/%1-%2/"
                          "forge-%1-%2-installer.jar")
        .arg(full, gameVersion);
}

/// 安装器自己那条 maven 下载路的**官方**根（喂给 `--mirror`）。
inline QString forgeMavenRoot() { return QStringLiteral("https://maven.minecraftforge.net/"); }
inline QString neoForgeMavenRoot() {
    return QStringLiteral("https://maven.neoforged.net/releases/");
}

/// Forge 装出来的版本目录名。
inline QString forgeLoaderId(const QString &gameVersion, const QString &loaderVersion) {
    return QStringLiteral("%1-forge-%2").arg(gameVersion, loaderVersion);
}

/// NeoForge 的 maven 元数据（那 63 KB 的 XML 里就是**全部**版本号）。
inline QString neoforgeMavenMetadataUrl() {
    return QStringLiteral(
        "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml");
}

/// 把一个点分版本号解析成数字列表。空列表 = 不是版本号。
///
/// NOTE: 逐段走正则，不用 QString::toInt() —— 后者接受前导空白、正负号（`"+5"`、`" 5"`
/// 都能过），而这里进来的是 maven 上别人写的东西、出去的用途是拼 URL 和比大小。
inline QList<int> parseDottedVersion(const QString &text) {
    QList<int> parts;
    if (text.isEmpty()) {
        return parts;
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    const QStringList segments = text.split(QLatin1Char('.'));
    for (const QString &segment : segments) {
        if (!digits.match(segment).hasMatch()) {
            return QList<int>();
        }
        // 溢出**也得当"不是版本号"**：`^[0-9]+$` 过得了、toInt() 过不了的
        // 那一段（maven 上别人写的东西，`26.1.2.99999999999` 这种）原来是静默变成 **0**，
        // 于是这个版本比谁都小 —— "有新版本"永远消不掉、版本榜上排到最后，而且一声不响。
        // 这个函数的契约本来就是"空列表 = 认不出来"，认不出来有的是地方处理它。
        bool ok = false;
        const qlonglong value = segment.toLongLong(&ok);
        if (!ok || value > std::numeric_limits<int>::max()) {
            return QList<int>();
        }
        parts.append(static_cast<int>(value));
    }
    return parts;
}

/// 比两个点分版本号。**短的按补零算**（所以 `26.1.2` < `26.1.2.71`）。
/// @return 负数 / 0 / 正数和 strcmp 一个意思。
inline int compareDottedVersions(const QList<int> &left, const QList<int> &right) {
    const int length = qMax(left.size(), right.size());
    for (int i = 0; i < length; ++i) {
        const int a = i < left.size() ? left.at(i) : 0;
        const int b = i < right.size() ? right.at(i) : 0;
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return 0;
}

/// `prefix` 是不是 `full` 的**逐段**前缀（`[21,1]` 是 `[21,1,251]` 的前缀，
/// 但**不是** `[21,10,64]` 的前缀）。
///
/// NOTE: 这里必须是逐段比，不能拿字符串前缀比 —— NeoForge 真有 `21.1.x` 和 `21.10.x`
/// 两族（分别对应 MC 1.21.1 和 1.21.10），按字符串 `"21.1"` 前缀匹配会把
/// `21.10.64` 也算进来，然后在比大小时把 `21.10.64` 挑成 1.21.1 的加载器。
inline bool isDottedPrefix(const QList<int> &prefix, const QList<int> &full) {
    if (prefix.isEmpty() || prefix.size() > full.size()) {
        return false;
    }
    for (int i = 0; i < prefix.size(); ++i) {
        if (prefix.at(i) != full.at(i)) {
            return false;
        }
    }
    return true;
}

/// NeoForge 版本号的"游戏版本前缀"。
///
/// NeoForge 的版本号刻意跟着游戏版本走，但**不是**同一串数字：
/// 游戏 1.21.1 → NeoForge 21.1.x （去掉开头的 `1.`）
/// 游戏 1.20.2 → NeoForge 20.2.x
/// 游戏 26.2 → NeoForge 26.2.x （游戏版本自己就是 26.2，没有 `1.` 可去）
/// 游戏 26.1.2 → NeoForge 26.1.2.x
/// 所以规则是：去掉开头的 `1.`（如果有），再取前两段；只有一段时补一个 `.0`。
/// 补 `.0` 那一手是给 `1.21` 这种两段式游戏版本用的 —— 它对应 NeoForge 的 `21.0.x`，
/// 不补的话前缀会退化成 `[21]`，于是把 `21.1.x`、`21.11.x` 全都算进来。
inline QList<int> neoForgeVersionPrefix(const QString &gameVersion) {
    QString text = gameVersion;
    if (text.startsWith(QLatin1String("1."))) {
        text.remove(0, 2);
    }
    QList<int> parts = parseDottedVersion(text);
    if (parts.isEmpty()) {
        return parts;
    }
    while (parts.size() < 2) {
        parts.append(0);
    }
    parts = parts.mid(0, 2);
    return parts;
}

/// 版本号里那段"点分数字"：`26.3.0.10-beta` → `26.3.0.10`、`26.1.0.0-alpha.1+snapshot-1`
/// → `26.1.0.0`。
///
/// 为什么要有这一刀：maven 上的版本号**带后缀**（NeoForge 的 `-beta`、`-alpha.N+snapshot-N`），
/// 而"谁比谁新"看的只是数字那段 —— `26.3.0.10-beta` 比 `26.3.0.9-beta` 新，可按字符串比
/// `"26.3.0.10-beta" < "26.3.0.9-beta"`（`1` < `9`）刚好反过来。所以比大小、认前缀之前
/// 都先切这一刀。
///
/// 切在**第一个** `-` 或 `+` 上（后缀自己还带 `-` 的，`-alpha.1+snapshot-1` 那种，一样切得对）。
inline QString dottedVersionKey(const QString &version) {
    for (int i = 0; i < version.size(); ++i) {
        const QChar c = version.at(i);
        if (c == QLatin1Char('-') || c == QLatin1Char('+')) {
            return version.left(i);
        }
    }
    return version;
}

/// 这个版本号是不是**稳定档**（正式版）。判据：**没有后缀**（既没有 `-` 也没有 `+`）。
///
/// NOTE: 这是"上游标没标它稳定"的**近似**，不是"它一定好用"：NeoForge 的 `26.3.0.10-beta`
/// 和 `26.1.0.0-alpha.1+snapshot-1` 都算不稳定，而它们**照样能装**（用户
/// 拍板要列出来）。
inline bool isStableLoaderVersion(const QString &version) {
    return !version.contains(QLatin1Char('-')) && !version.contains(QLatin1Char('+'));
}

/// 两个版本号字符串的**自然序**比较：数字段按数值比，其余字符按码点比。
///
/// 为什么不能用 `a < b`（ 改的）：下面的 sortLoaderVersionsNewestFirst 最后一级
/// tie-break 原来是字符串升序，而它接手的恰恰是"切掉后缀之后数字一模一样"的那些 ——
/// NeoForge 的 `26.1.0.0-alpha.1+snapshot-1` 和 `26.1.0.0-alpha.2+snapshot-1` 过完
/// dottedVersionKey() 都是 `26.1.0.0`，于是次序退化成按字符串比：`alpha.1` 排在
/// `alpha.2` **前面**。列表是"新到旧"的，所以「最新档」会挑中一个更旧的快照。
/// （26.1 那一族正是这种形状；26.3 那族的 `26.3.0.0-beta`…`26.3.0.10-beta` 数字段本来就
/// 不同，走不到这一级。）
///
/// NOTE: 结果是个**全序**（数字段比数值、数值相同再比位数，字符比码点，前缀短的算小），
/// 所以顺带把"std::sort 不是稳定排序、次序要钉死"那件事一起办了。
/// @return 小于 0 表示 left 更小（更旧）。
inline int compareVersionStrings(const QString &left, const QString &right) {
    const auto digitRun = [](const QString &text, int from) {
        int end = from;
        while (end < text.size() && text.at(end).isDigit()) {
            ++end;
        }
        return end;
    };
    const auto withoutLeadingZeros = [](const QString &digits) {
        int first = 0;
        while (first + 1 < digits.size() && digits.at(first) == QLatin1Char('0')) {
            ++first;
        }
        return digits.mid(first);
    };

    int i = 0;
    int j = 0;
    while (i < left.size() && j < right.size()) {
        const QChar lc = left.at(i);
        const QChar rc = right.at(j);
        if (lc.isDigit() && rc.isDigit()) {
            const int leftEnd = digitRun(left, i);
            const int rightEnd = digitRun(right, j);
            const QString leftDigits = left.mid(i, leftEnd - i);
            const QString rightDigits = right.mid(j, rightEnd - j);
            const QString leftNumber = withoutLeadingZeros(leftDigits);
            const QString rightNumber = withoutLeadingZeros(rightDigits);
            if (leftNumber.size() != rightNumber.size()) {
                return leftNumber.size() < rightNumber.size() ? -1 : 1;
            }
            const int order = leftNumber.compare(rightNumber);
            if (order != 0) {
                return order < 0 ? -1 : 1;
            }
            // 数值相同（`01` 与 `1`）：位数少的定死算小，别让它变成"相等"。
            if (leftDigits.size() != rightDigits.size()) {
                return leftDigits.size() < rightDigits.size() ? -1 : 1;
            }
            i = leftEnd;
            j = rightEnd;
            continue;
        }
        if (lc != rc) {
            return lc < rc ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < left.size()) {
        return 1;
    }
    if (j < right.size()) {
        return -1;
    }
    return 0;
}

/// 把一份加载器版本列表按版本号**从新到旧**排。
///
/// maven 那两份清单（Forge 5048 条、NeoForge 63 KB）都是**按发布时间排的，不是按版本号排的**，
/// 所以"列出能挑哪些版本"这条路必须自己比大小。实测两份都能证明这一点：Forge 那份的末尾是
/// `... 26.1.2-64.1.3, 1.19.4-45.4.5, 26.3-66.0.0, 26.3-66.0.2`（一个 1.19.4 的补发夹在
/// 一堆 26.x 中间），NeoForge 那份的末尾同理。
///
/// NOTE: 比的是**点分数字**（compareDottedVersions），所以先过 dottedVersionKey() 把
/// `-beta` 那类后缀切掉。比不了的那些（Forge 的 `10.13.3.1401-1710ls` 之类）在各自的
/// 分堆里就被挡掉了，所以这里不会拿到空 key。
inline void sortLoaderVersionsNewestFirst(LoaderVersionList &entries) {
    struct Row {
        QList<int> parsed;          ///< parseDottedVersion(dottedVersionKey(version))
        LoaderVersionEntry entry;
    };
    QList<Row> rows;
    rows.reserve(entries.size());
    for (const LoaderVersionEntry &entry : entries) {
        rows.append(Row{parseDottedVersion(dottedVersionKey(entry.version)), entry});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &left, const Row &right) {
        const int order = compareDottedVersions(left.parsed, right.parsed);
        if (order != 0) {
            return order > 0;
        }
        // 数字一模一样时（同一版的正式版和它的 `-beta`）：**正式版在前**。
        // 差一位就排错的话，用户第一眼看到的那条是测试版。
        if (left.entry.stable != right.entry.stable) {
            return left.entry.stable;
        }
        // 再一样就按**自然序**（数字段按数值）定死一个次序 —— 这一级接手的正是
        // "后缀不同、数字相同"的那些（`26.1.0.0-alpha.N+snapshot-N` 那一族），
        // 按字符串比会把 alpha.10 排到 alpha.2 后面，见 compareVersionStrings 上面
        // 那段。顺带把"std::sort **不是**稳定排序、次序必须钉死"那件事一起办了。
        return compareVersionStrings(left.entry.version, right.entry.version) > 0;
    });
    for (int i = 0; i < rows.size(); ++i) {
        entries[i] = rows.at(i).entry;
    }
}

/// neoforge 的 maven-metadata.xml -> 这个游戏版本能用的**全部** NeoForge 版本（新到旧）。
/// @param mavenMetadata XML 原文（原样，这里只扫 `<version>...</version>`）。
/// @param gameVersion 原版版本号。
///
/// NOTE: 这一条改动之前这里是"带 `-` 的一律跳过"，理由是"宁可列少一点也不给用户一个半成品"。
/// 那个理由被用户否掉了（beta 能用），但没有被"全收"否掉的地方是：**稳定档的挑法一点没变**
/// （见 pickNeoForgeVersion），只有"用户自己点一条"时才会碰到 beta。
inline LoaderVersionList listNeoForgeVersions(const QByteArray &mavenMetadata,
                                              const QString &gameVersion) {
    LoaderVersionList entries;
    const QList<int> prefix = neoForgeVersionPrefix(gameVersion);
    if (prefix.isEmpty()) {
        return entries;
    }

    QSet<QString> seen;
    static const QRegularExpression tag(QStringLiteral("<version>([^<]+)</version>"));
    QRegularExpressionMatchIterator it = tag.globalMatch(QString::fromUtf8(mavenMetadata));
    while (it.hasNext()) {
        const QString version = it.next().captured(1).trimmed();
        const QList<int> parsed = parseDottedVersion(dottedVersionKey(version));
        if (parsed.isEmpty() || !isDottedPrefix(prefix, parsed) || seen.contains(version)) {
            continue;
        }
        seen.insert(version);
        entries.append(LoaderVersionEntry{version, isStableLoaderVersion(version)});
    }
    sortLoaderVersionsNewestFirst(entries);
    return entries;
}

/// 从 neoforge 的 maven-metadata.xml 里挑一个 NeoForge 版本。
/// @param mavenMetadata XML 原文（原样，这里只扫 `<version>...</version>`）。
/// @param gameVersion 原版版本号。
/// @param channel 挑稳定档还是最新档（见 LoaderChannel）。
/// @return 挑中的版本；**稳定档**在这个游戏版本上只有 beta 时 `ok()` 为 false（那时只有
/// 「最新档」挑得出来 —— 界面上「下载稳定版」那一行也会是灰的）。
///
/// 规则：两个频道都取 listNeoForgeVersions 排完之后的**第一条符合条件的**：
/// 稳定档 = 第一条 stable 的；最新档 = 第一条（就是数字最大的，beta 也算）。
/// 为什么是"比大小"而不是像 Fabric 那样"取第一条"：见 listNeoForgeVersions 上面那段
/// （maven 清单是按发布时间排的）。
///
/// NOTE: 只走这一处解析，"默认选中哪个"和"能挑哪些"不会慢慢漂开。
inline LoaderPick pickNeoForgeVersion(const QByteArray &mavenMetadata, const QString &gameVersion,
                                      LoaderChannel channel = LoaderChannel::Stable) {
    const LoaderVersionList entries = listNeoForgeVersions(mavenMetadata, gameVersion);
    if (entries.isEmpty()) {
        return LoaderPick{};
    }
    for (const LoaderVersionEntry &entry : entries) {
        if (channel == LoaderChannel::Newest || entry.stable) {
            return LoaderPick{entry.version, entry.stable};
        }
    }
    return LoaderPick{};
}

inline QString neoforgeInstallerUrl(const QString &loaderVersion) {
    return QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/neoforge/%1/neoforge-%1-installer.jar")
        .arg(loaderVersion);
}

// ---------------------------------------------------------------------------
// Quilt：meta 的形状和 Fabric 很像（loader / hashed / intermediary / launcherMeta），
// 但**没有 stable 字段**，而且列表不是按版本号排的。所以这里自己按点分数字排，
// 稳定档按"版本号没有 - / + 后缀"判（和 NeoForge 同一条口径）。
// ---------------------------------------------------------------------------

inline QString quiltMetaBase() {
    return QStringLiteral("https://meta.quiltmc.org");
}

inline QString quiltLoaderListUrl(const QString &gameVersion) {
    return quiltMetaBase() + QStringLiteral("/v3/versions/loader/") + gameVersion;
}

inline QString quiltProfileUrl(const QString &gameVersion, const QString &loaderVersion) {
    return quiltLoaderListUrl(gameVersion) + QLatin1Char('/') + loaderVersion +
           QStringLiteral("/profile/json");
}

/// Quilt 装出来的版本 id（profile 里的 id 才是权威；这个用于钉版本时的预填与日志）。
inline QString quiltLoaderId(const QString &gameVersion, const QString &loaderVersion) {
    return QStringLiteral("quilt-loader-%1-%2").arg(loaderVersion, gameVersion);
}

/// `/v3/versions/loader/<mc>` 的回复 -> 能列给用户挑的全部 Quilt 加载器版本（新到旧）。
inline LoaderVersionList listQuiltLoaderVersions(const QJsonArray &list) {
    LoaderVersionList entries;
    entries.reserve(list.size());
    QSet<QString> seen;
    for (const QJsonValue &value : list) {
        const QJsonObject loader = value.toObject().value(QStringLiteral("loader")).toObject();
        const QString version = loader.value(QStringLiteral("version")).toString();
        if (version.isEmpty() || seen.contains(version)) {
            continue;
        }
        seen.insert(version);
        entries.append(LoaderVersionEntry{version, isStableLoaderVersion(version)});
    }
    sortLoaderVersionsNewestFirst(entries);
    return entries;
}

/// 从 Quilt 列表里挑一个版本：稳定档 = 第一条 stable（没有就回空，让调用方说清
/// “这个游戏版本只有测试版”）；最新档 = 排序后的第一条。
inline LoaderPick pickQuiltLoader(const QJsonArray &list, LoaderChannel channel) {
    const LoaderVersionList entries = listQuiltLoaderVersions(list);
    if (entries.isEmpty()) {
        return LoaderPick{};
    }
    for (const LoaderVersionEntry &entry : entries) {
        if (channel == LoaderChannel::Newest || entry.stable) {
            return LoaderPick{entry.version, entry.stable};
        }
    }
    return LoaderPick{};
}

/// 判断 Fabric / Quilt `/versions/loader/<mc>` 的回复是不是“该游戏版本不被支持”
/// （而不是网络/服务故障）：
/// - Fabric 对不支持的版本（如 1.12.2）返回 HTTP 400 + `[]`；
/// - Quilt 对不支持的版本（如 1.12.2 / 1.14）返回 HTTP 404 + `{"code":"not_found",...}`。
inline bool isUnsupportedGameVersionMetaReply(bool isQuilt, int httpStatus,
                                              const QByteArray &body) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return false;
    }
    if (doc.isArray()) {
        return doc.array().isEmpty() &&
               (httpStatus == 0 || httpStatus == 200 || httpStatus == 400 || httpStatus == 404);
    }
    if (isQuilt && httpStatus == 404 && doc.isObject()) {
        return doc.object().value(QStringLiteral("code")).toString() == QLatin1String("not_found");
    }
    return false;
}

/// NeoForge 装出来的版本目录名。
///
/// 和 Forge **不一样**：它不带游戏版本前缀，直接就是 `neoforge-<版本>`
/// （实测 21.1.215 → `versions/neoforge-21.1.215/`）。这不是笔误，也不能"统一一下"——
/// 别的启动器按这个名字找目录。
inline QString neoforgeLoaderId(const QString &loaderVersion) {
    return QStringLiteral("neoforge-%1").arg(loaderVersion);
}

/// Forge / NeoForge 的安装器认的那个"启动器档案"文件名。
///
/// 它不是一个可选的东西：**没有它安装器直接拒绝干活**（见上面那段实测）。我们的启动器
/// 自己不需要它，但它旁边的原版启动器、以及这些第三方安装器都按它判断"这是一个
/// Minecraft 数据目录"。
inline QString launcherProfilesFileName() { return QStringLiteral("launcher_profiles.json"); }

/// 一份**最小可用**的 launcher_profiles.json。
///
/// 只在目标目录里没有这个文件时才写（见 VersionManager::ensureLauncherProfiles）——
/// 装过任何一个原版启动器/第三方启动器的目录里都有一份内容更全的，覆盖它是把用户的
/// 东西弄丢。结构照抄安装器认的那三个键：原版启动器写出来就是这个形状。
inline QByteArray minimalLauncherProfiles() {
    return QByteArrayLiteral("{\n  \"profiles\": {},\n  \"settings\": {},\n  \"version\": 3\n}\n");
}

// ---------------------------------------------------------------------------
// OptiFine：跑它自己的安装器（无界面入口 + 换 APPDATA）
// ---------------------------------------------------------------------------
//
// 同样是"请下来别人的安装器、另起一个 Java 进程跑它"，但 OptiFine 的安装器有三处要命的
// 不同：
//
// 1. **命令行参数它整个丢掉**：没有 `--installClient` 那种东西可给，游戏目录只从
// `%APPDATA%/.minecraft` 算（`Utils.getWorkingDirectory()`）。所以它往哪个目录装
// 完全由子进程的 APPDATA 决定 —— 我们给它换一个，让它对着一个临时目录干活。
// 2. **它是 GUI 程序**：jar 的 `Main-Class` 是 `optifine.InstallerFrame`（Swing 窗口，
// headless 下当场 HeadlessException）。真正的无界面入口是 **`optifine.Installer`**：
// `java -cp <安装器>.jar optifine.Installer`，**不需要任何参数** —— 版本号是它从自己
// 那份 jar 里读的（把它改名成「随便什么名字.jar」，它照样报 OptiFine_1.18.2_HD_U_H9）。
// 3. **出错时它弹一个 Swing 对话框等着人点**：不点就永不退出（实测跑满 120 秒被杀）。
// 所以子进程必须带 `-Djava.awt.headless=true` —— 那些框当场变成异常，进程以退出码 1
// 结束。**成功那条路一个框都不弹、退出码 0**（实测 headless 下 2 秒装完）。
//
// versions/1.18.2-OptiFine_HD_U_H9/1.18.2-OptiFine_HD_U_H9.json 463 B，见下
// versions/1.18.2-OptiFine_HD_U_H9/1.18.2-OptiFine_HD_U_H9.jar **和原版客户端 jar
// 逐字节相同**（sha1 一样）。这一条反直觉但很关键：OptiFine **不往这份 jar 里写
// 补丁**，它只是把原版那份原样放进新目录当"这个版本的客户端 jar"。它自己的代码在
// 下面那个库里，靠 launchwrapper + `--tweakClass optifine.OptiFineTweaker` 挂进去。
// libraries/optifine/OptiFine/1.18.2_HD_U_H9/OptiFine-1.18.2_HD_U_H9.jar 6.2 MB
// libraries/optifine/launchwrapper-of/2.3/launchwrapper-of-2.3.jar
// 外加它往 launcher_profiles.json 里加的一条档案（还顺手把 selectedProfile 改成了它）
//
// 那份 json 的 `libraries` 只有两个 `name`，**既没有 `url` 也没有 `downloads`** —— 只能
// 从本机那份 `libraries/` 里满足，没有"靠下载补齐"这条路。所以搬产物时**必须连带
// `libraries/optifine/` 一起搬**，只搬版本目录会得到一个启动就 NoClassDefFound 的版本。
//
// 【和 Forge 正好相反的两处顺序】
//
// * Forge 是"下安装器 → 跑 → 装完再补原版"。OptiFine 的安装器**要拿原版的客户端 jar
// 当输入**（上面第二样产物就是它），所以原版必须**先**在盘上。
// * Forge 缺 `launcher_profiles.json` 是第一件事就拒绝（一个字节的库都不下）。OptiFine
// 把它排在**最后一步**：前面整套东西都装完了，它才去更新那个文件，缺了当场弹框报错
// —— headless 下就是退出码 1，而**产物其实已经躺在盘上了**。我们的临时目录里自己补
// 一份最小的，所以走不到那一步。
//
// 【版本目录名不是我们算得出来的】
//
// Forge/NeoForge 的名字（`1.20.1-forge-47.4.10` / `neoforge-21.1.215`）我们能按约定拼，
// OptiFine 的不行 —— 它是安装器按 jar 里的版本号自己拼的（`<原版>-OptiFine_<版本>`）。
// 所以这一条**装完去盘上认**：我们给它那个临时目录是全新的，`versions/` 底下除了自己放
// 进去的原版之外多出来的那一份就是它装的。见 VersionManager::adoptOptifineProducts()。

/// OptiFine 的官方下载页：每个游戏版本一节（`<h2>Minecraft <版本></h2>`），整页约 203 KB。
inline QString optifineDownloadsPageUrl() {
    return QStringLiteral("https://optifine.net/downloads");
}

/// 镜像源（BMCLAPI）上某个游戏版本的 OptiFine 列表（JSON 数组，一个游戏版本一页）。
///
/// NOTE: 它和官方下载页**不是同一份数据**：这里每条只有文件名和 type/patch，
/// 页面上那套"哪一条是主推的"（`downloadLineMain`）在这里没有对应物。
inline QString optifineMirrorListUrl(const QString &gameVersion) {
    return QStringLiteral("https://bmclapi2.bangbang93.com/optifine/") + gameVersion;
}

/// NOTE: 最后两段是 `type` / `patch` 两个**字段**，不是文件名 —— 官方页面上那条
/// `OptiFine_1.18.2_HD_U_H9.jar` 到这里拆成了 `HD_U` 和 `H9`。
inline QString optifineMirrorFileUrl(const QString &gameVersion, const QString &type,
                                     const QString &patch) {
    return optifineMirrorListUrl(gameVersion) + QLatin1Char('/') + type + QLatin1Char('/') + patch;
}

/// 官方那条路上的广告页：真文件在它后面（页面里那条 `downloadx?...&x=<32 位十六进制>`）。
inline QString optifineAdPageUrl(const QString &fileName) {
    return QStringLiteral("https://optifine.net/adloadx?f=") + fileName;
}

/// 安装器 jar 的文件名能不能直接拿来拼 URL 和拼路径。
///
/// 白名单和 isSafeVersionId() 那条是同一个（`[A-Za-z0-9._+-]`），理由也一样：**内容来自
/// 网络、用途是拼路径**。这里多要求 `.jar` 结尾，因为这个名字会落成
/// `cache/installers/` 下面的一个文件。
inline bool isSafeJarFileName(const QString &name) {
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9._+-]+\\.jar$"));
    return safe.match(name).hasMatch();
}

/// 预览版的文件名长这样（`preview_OptiFine_1.18.2_HD_U_H9_pre2.jar`）。
inline bool isOptifinePreviewFile(const QString &fileName) {
    return fileName.startsWith(QLatin1String("preview_"));
}

/// 挑出来的那一个 OptiFine 安装器。
struct OptiFinePick {
    QString fileName;  ///< 安装器 jar 的名字（`OptiFine_1.18.2_HD_U_H9.jar`）
    QString type;      ///< 镜像那条路才有：`HD_U`（预览版是 `HD_U_H9`）
    QString patch;     ///< 镜像那条路才有：`H9`（预览版是 `pre2`）

    bool ok() const { return !fileName.isEmpty(); }
};

/// 安装器文件名 → OptiFine 的"版本号"（就是写进 wtfml.meta 的 loaderVersion）。
///
/// `OptiFine_1.18.2_HD_U_H9.jar` → `HD_U_H9`；
/// 预览版 `preview_OptiFine_1.18.2_HD_U_H9_pre2.jar` → `HD_U_H9_pre2`。
///
/// NOTE: **刻意不拿镜像那份 `type`/`patch` 拼**。那两个字段的切法和文件名不一样
/// （预览版那几条的 type 是 `HD_U_H9`、patch 是 `pre2`），而"这个版本叫什么"在
/// 官方/镜像两条路上必须是**同一个值** —— 只有这一处切法才不会两边对不上。
inline QString optifineEditionFromFile(const QString &gameVersion, const QString &fileName) {
    QString text = fileName;
    const QString kPreview = QStringLiteral("preview_");
    const QString kPrefix = QStringLiteral("OptiFine_");
    if (text.startsWith(kPreview)) {
        text.remove(0, kPreview.size());
    }
    if (text.startsWith(kPrefix)) {
        text.remove(0, kPrefix.size());
    }
    if (text.endsWith(QLatin1String(".jar"))) {
        text.chop(4);
    }
    const QString versionPrefix = gameVersion + QLatin1Char('_');
    if (text.startsWith(versionPrefix)) {
        text.remove(0, versionPrefix.size());
    }
    return text;
}

/// `optifineEditionFromFile()` 的**反函数**：版本号 -> 安装器 jar 的文件名。
///
/// `HD_U_H9` → `OptiFine_1.18.2_HD_U_H9.jar`；预览版 `HD_U_H9_pre2` →
/// `preview_OptiFine_1.18.2_HD_U_H9_pre2.jar`。
///
/// 为什么要反着算一遍：界面上用户挑的是一个**版本号**（`HD_U_H9`，那才是会写进 wtfml.meta 的
/// 值），而"下哪个文件"要的是文件名。没有这一手的话，"钉死一个 OptiFine 版本"就落不了地
/// （见 VersionManager::adoptOptifineEdition）。
///
/// NOTE: 预览版的判据写成正则 `_pre<数字>$` 而不是 `contains("_pre")` —— 正式版的版本号将来
/// 万一含 `_pre` 这三个字母（`HD_U_PRE...`），按 contains 判就会给一个正式版套上
/// `preview_` 前缀，下回来一个不存在的文件。
inline QString optifineInstallerFileName(const QString &gameVersion, const QString &edition) {
    static const QRegularExpression previewSuffix(QStringLiteral("_pre[0-9]+$"));
    const QString base =
        QStringLiteral("OptiFine_%1_%2.jar").arg(gameVersion, edition);
    if (previewSuffix.match(edition).hasMatch()) {
        return QStringLiteral("preview_") + base;
    }
    return base;
}

/// 安装器文件名/版本号 -> OptiFine **镜像**那条路要的两个路径段（`type` / `patch`）。
///
/// 镜像的地址是 `/optifine/<游戏版本>/<type>/<patch>`（见 optifineMirrorFileUrl），两段的切法
/// 和文件名不一样：`HD_U_H9` → `HD_U` / `H9`，预览版 `HD_U_H9_pre2` → `HD_U_H9` / `pre2`
/// （实测那 11 条镜像数据逐条对过）。所以**不能**切成 `[0], [1]` 两段了事。
struct OptiFineMirrorPath {
    QString type;
    QString patch;

    bool ok() const { return !type.isEmpty() && !patch.isEmpty(); }
};

inline OptiFineMirrorPath optifineMirrorPathFromEdition(const QString &edition) {
    const int cut = edition.lastIndexOf(QLatin1Char('_'));
    if (cut <= 0 || cut + 1 >= edition.size()) {
        return OptiFineMirrorPath{};
    }
    return OptiFineMirrorPath{edition.left(cut), edition.mid(cut + 1)};
}

/// 官方下载页里某个游戏版本那一节的原文（`<h2>Minecraft <版本></h2>` 到下一个 `<h2>` 之间）。
///
/// **找不到这一节时回一个空（null）的 QString** —— 调用方靠 isNull() 区分"页面上压根没有这个
/// 游戏版本"和"这一节在、但里面一条都没有"。后者是见过的（只有预览、还没发正式版的那些）。
///
/// NOTE: 那一节的边界靠**下一个 `<h2>`** 划。页面上 `<h2>` 只用来分游戏版本，所以这是稳的；
/// 也正因为如此，找不到小节时要如实返回空，而不是"从整页里随便挑一个"。
inline QString optifineDownloadsSection(const QString &html, const QString &gameVersion) {
    static const QRegularExpression heading(
        QStringLiteral("<h2>\\s*Minecraft\\s+([^<]+?)\\s*</h2>"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator headings = heading.globalMatch(html);
    while (headings.hasNext()) {
        const QRegularExpressionMatch match = headings.next();
        if (match.captured(1).trimmed() != gameVersion) {
            continue;
        }
        const int start = match.capturedEnd();
        const int end = headings.hasNext() ? headings.peekNext().capturedStart() : html.size();
        return html.mid(start, end - start);
    }
    return QString();
}

/**
 * @brief 那一节里标着某一类 class 的行，把安装器文件名抠出来（按页面顺序）。
 * @param section optifineDownloadsSection() 的产物。
 * @param classToken 认哪一类行（`downloadLineMain` / `downloadLineMore`，见调用方）。
 *
 * NOTE: 这里**不按引号形态挑** —— 页面上那个 class 用的是**单引号**
 * （`class='downloadLine downloadLineMain'`），写正则时拿双引号去配会一条都匹配不上
 * （我第一遍就是这么读那份夹具的，白读了一轮）。
 */
inline QStringList optifineRowFileNames(const QString &section, const QString &classToken) {
    static const QRegularExpression row(QStringLiteral("<tr\\b([^>]*)>(.*?)</tr>"),
                                        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression link(QStringLiteral("adloadx\\?f=([^\"'&<>\\s]+)"));
    QStringList names;
    QRegularExpressionMatchIterator rows = row.globalMatch(section);
    while (rows.hasNext()) {
        const QRegularExpressionMatch match = rows.next();
        if (!match.captured(1).contains(classToken)) {
            continue;
        }
        const QRegularExpressionMatch found = link.match(match.captured(2));
        if (!found.hasMatch()) {
            continue;
        }
        const QString fileName = found.captured(1);
        if (!isSafeJarFileName(fileName)) {
            continue;
        }
        names.append(fileName);
    }
    return names;
}

/// 一个源上的一行 OptiFine 安装器（去重、排好序之后的一行）。
///
/// 两个源都先化成这个形状，**列表和挑法再共用同一份** —— 不然"列表里排第一的那条"和
/// "挑出来的那条"就会各算各的，迟早对不上（用户看到列表第一条是 `pre2`、点下去装的是 `J3`
/// 那种）。
struct OptiFineRow {
    QString fileName;  ///< 安装器 jar 的名字，下这个文件、也是落盘用的名字
    QString edition;   ///< 版本号，写进 wtfml.meta 的那个值（`optifineEditionFromFile`）
    bool stable = false;  ///< 正式版（不是 `preview_` 预览版）就是 true
    QString type;     ///< 镜像那条路才有：`HD_U`（预览版是 `HD_U_H9`）
    QString patch;    ///< 镜像那条路才有：`H9`（预览版是 `pre2`）
    qint64 publishedAt = 0;  ///< 镜像那条路才有：发布时间（`_id` 前 4 字节），页面那条路是 0
};

/// 这个名字能不能当成"属于这个游戏版本、我们能装"的安装器文件名。
///
/// 判据是**两个函数互为反函数**：`optifineEditionFromFile()` 削掉版本号得到 edition，
/// `optifineInstallerFileName()` 再反算回来还得是同一个名字。反算不回来就说明这个文件不属于
/// 这个游戏版本（或者版本号的形状我们认不出来），拿它拼出来的下载地址一定 404。
inline bool isOptifineFileForVersion(const QString &fileName, const QString &gameVersion) {
    if (!isSafeJarFileName(fileName)) {
        return false;
    }
    const QString edition = optifineEditionFromFile(gameVersion, fileName);
    if (edition.isEmpty()) {
        return false;
    }
    return optifineInstallerFileName(gameVersion, edition) == fileName;
}

/// 镜像那条 JSON 里每条的 `_id` 是 MongoDB 的 ObjectId，**前 4 个字节 = 发布时间的 Unix 秒**。
///
/// 拿它当排序键，而不是"把数组倒过来"：那份数组实际是按 **type/patch 字符串字典序**排的
/// （实测 1.21.4 那条数组的次序是 `pre1, pre10, pre11…, pre2, pre9`），只有 patch 都是单数字
/// 时才碰巧和时间同序 —— 一旦有 `pre10` 就露馅，"倒过来"会把 `pre9` 排到 `pre2` 前面。
///
/// @return 拿不到（字段缺了 / 不是十六进制）时回 0，排序时落到最后。
inline qint64 optifineEntryTimestamp(const QJsonObject &entry) {
    const QString id = entry.value(QStringLiteral("_id")).toString();
    if (id.size() < 8) {
        return 0;
    }
    bool ok = false;
    const uint seconds = id.left(8).toUInt(&ok, 16);
    return ok ? qint64(seconds) : 0;
}

/**
 * @brief 官方下载页那一节 -> 按**页面次序**排好的原始行（挑法和列表都走这一份）。
 * @param section optifineDownloadsSection() 的产物。
 * @param gameVersion 原版版本号。
 *
 * 取预览版行 + main 行 + More 行三类（`downloadLineMore` 是页面上折叠在「+ More」里的旧版本），
 * **预览版也列** —— 用户 的决定：「beta 版虽然不稳定，但是也能使用，要列出来」。
 * 一行标着 `downloadLineMain` 的是正式版，标着 `downloadLinePreview` 的不是。
 */
inline QList<OptiFineRow> optifinePageRows(const QString &section, const QString &gameVersion) {
    QStringList names = optifineRowFileNames(section, QStringLiteral("downloadLinePreview"));
    names += optifineRowFileNames(section, QStringLiteral("downloadLineMain"));
    names += optifineRowFileNames(section, QStringLiteral("downloadLineMore"));

    QList<OptiFineRow> rows;
    QSet<QString> seen;
    for (const QString &fileName : names) {
        if (!isOptifineFileForVersion(fileName, gameVersion)) {
            continue;
        }
        const QString edition = optifineEditionFromFile(gameVersion, fileName);
        if (seen.contains(edition)) {  // 同一个版本号只在第一次出现时留一条
            continue;
        }
        seen.insert(edition);
        rows.append(OptiFineRow{fileName, edition, !isOptifinePreviewFile(fileName), QString(),
                                QString()});
    }
    return rows;
}

/**
 * @brief 镜像那份 JSON -> 按**发布时间从新到旧**排好的原始行（挑法和列表都走这一份）。
 * @param list 接口返回的数组（原样）。
 * @param gameVersion 原版版本号。
 *
 * NOTE: `type` / `patch` 是**镜像自己的字段原样留着** —— 拼下载地址要用它。虽然
 * `optifineMirrorPathFromEdition()` 切出来的和它们逐条一致（有断言盯着），但拼地址这件事
 * 上宁可取原始数据。
 */
inline QList<OptiFineRow> optifineMirrorRows(const QJsonArray &list, const QString &gameVersion) {
    QList<OptiFineRow> rows;
    QSet<QString> seen;
    for (const QJsonValue &value : list) {
        const QJsonObject entry = value.toObject();
        const QString fileName = entry.value(QStringLiteral("filename")).toString();
        const QString type = entry.value(QStringLiteral("type")).toString();
        const QString patch = entry.value(QStringLiteral("patch")).toString();
        // 少一样就用不了：文件名是给用户看的和落盘用的，type/patch 是拼下载地址用的。
        if (type.isEmpty() || patch.isEmpty() || !isOptifineFileForVersion(fileName, gameVersion)) {
            continue;
        }
        const QString edition = optifineEditionFromFile(gameVersion, fileName);
        if (seen.contains(edition)) {
            continue;
        }
        seen.insert(edition);
        rows.append(OptiFineRow{fileName, edition, !isOptifinePreviewFile(fileName), type, patch,
                                optifineEntryTimestamp(entry)});
    }
    // 新到旧。同一秒发的几条（`_id` 前 8 位一样）拿版本号字符串定死一个次序，免得列表看着乱跳。
    std::sort(rows.begin(), rows.end(), [](const OptiFineRow &left, const OptiFineRow &right) {
        if (left.publishedAt != right.publishedAt) {
            return left.publishedAt > right.publishedAt;
        }
        return left.edition > right.edition;
    });
    return rows;
}

/// 把排好序的原始行化成界面要的那份清单（`stable` 直接透传）。
inline LoaderVersionList optifineVersionList(const QList<OptiFineRow> &rows) {
    LoaderVersionList entries;
    entries.reserve(rows.size());
    for (const OptiFineRow &row : rows) {
        entries.append(LoaderVersionEntry{row.edition, row.stable});
    }
    return entries;
}

/// 在排好序的原始行里挑一条：`Newest` 就是第一条，`Stable` 是第一条正式版（没有就回空）。
inline OptiFinePick pickOptiFineFromRows(const QList<OptiFineRow> &rows, LoaderChannel channel) {
    for (const OptiFineRow &row : rows) {
        if (channel == LoaderChannel::Newest || row.stable) {
            return OptiFinePick{row.fileName, row.type, row.patch};
        }
    }
    return OptiFinePick{};
}

/**
 * @brief 官方下载页 -> 这个游戏版本能挑的**全部** OptiFine 版本（新到旧）。
 */
inline LoaderVersionList listOptifineVersionsFromDownloadsPage(const QString &html,
                                                              const QString &gameVersion) {
    const QString section = optifineDownloadsSection(html, gameVersion);
    if (section.isNull()) {
        return LoaderVersionList();
    }
    return optifineVersionList(optifinePageRows(section, gameVersion));
}

/**
 * @brief 镜像源那份 JSON -> 这个游戏版本能挑的**全部** OptiFine 版本（新到旧）。
 * @param list 接口返回的数组（原样）。
 */
inline LoaderVersionList listOptifineVersionsFromMirror(const QJsonArray &list,
                                                        const QString &gameVersion) {
    return optifineVersionList(optifineMirrorRows(list, gameVersion));
}

/**
 * @brief 从官方下载页的原文里挑一个 OptiFine 版本。
 * @param html `https://optifine.net/downloads` 的原文。
 * @param gameVersion 原版版本号（页面上那一节 `<h2>Minecraft <版本></h2>`）。
 * @param channel 稳定档取 main 那条（OptiFine 自己标出来的"这个游戏版本就用这个"），
 * 最新档取那一节最上面那条（预览版排在 main 前面，所以就是最新的预览版）。
 *
 * NOTE: 那一节的边界靠**下一个 `<h2>`** 划（见 optifineDownloadsSection）。页面上 `<h2>`
 * 只用来分游戏版本，所以这是稳的；但也正因为如此，找不到小节时要如实返回空，
 * 而不是"从整页里随便挑一个"。
 */
inline OptiFinePick pickOptifineFromDownloadsPage(const QString &html, const QString &gameVersion,
                                                  LoaderChannel channel = LoaderChannel::Stable) {
    const QString section = optifineDownloadsSection(html, gameVersion);
    if (section.isNull()) {
        return OptiFinePick{};
    }
    return pickOptiFineFromRows(optifinePageRows(section, gameVersion), channel);
}

/// 从镜像源 `/optifine/<原版版本>` 的回复里挑一个 OptiFine 版本（新到旧里的第一条 / 第一条正式版）。
///
/// NOTE: 官方页面上那个"官方主推"的概念（`downloadLineMain`）镜像这边没有，所以这条路的
/// **稳定档**只能取"最新的正式版"，两条路挑出不同版本是可能的（官方 lag 一次就会）。
///
/// @param list 那个接口返回的数组（原样）。
/// @param gameVersion 原版版本号。
/// @param channel 见 pickOptifineFromDownloadsPage()。
inline OptiFinePick pickOptifineFromMirror(const QJsonArray &list, const QString &gameVersion,
                                           LoaderChannel channel = LoaderChannel::Stable) {
    return pickOptiFineFromRows(optifineMirrorRows(list, gameVersion), channel);
}

/// 从广告页的原文里抠出真正的下载地址（`https://optifine.net/downloadx?f=…&x=…`）。
/// @param html `optifineAdPageUrl()` 那个地址的回复原文。
/// @param fileName 我们请求的那个文件名（用来在页面里有多条链接时挑对）。
/// @return 可以直接 GET 的绝对地址；抠不出来时空串。
///
/// 页面上那条链接是**相对**的，所以这里补上 host。挑法：优先挑 `f=` 和我们请求的一致的那
/// 条；页面里只有一条时就用那一条 —— 实测那个 token **不跟文件名绑定**（拿 H7 的 token 去
/// 请求 H9，下下来的照样是 H9 的 jar，6.7 MB、`application/java-archive`）。有好几条又
/// 没有一条对得上时**不猜**：宁可报"拿不到下载地址"，也不给用户下一个不知道是什么的文件。
inline QString optifineJarUrlFromAdPage(const QString &html, const QString &fileName) {
    static const QRegularExpression link(
        QStringLiteral("(downloadx\\?f=([^\"'&<>\\s]+)&x=[0-9a-fA-F]{8,})"));
    QStringList links;
    QStringList names;
    QRegularExpressionMatchIterator it = link.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        links.append(match.captured(1));
        names.append(match.captured(2));
    }
    if (links.isEmpty()) {
        return QString();
    }
    int index = names.indexOf(fileName);
    if (index < 0) {
        if (links.size() != 1) {
            return QString();
        }
        index = 0;
    }
    return QStringLiteral("https://optifine.net/") + links.at(index);
}

// ---------------------------------------------------------------------------
// Forge：把 maven 清单列成"能挑哪些版本"
// ---------------------------------------------------------------------------
//
// 【为什么 Forge 要新开一个源】原来 Forge 只有 promotions_slim.json（4 KB，"官方推荐哪个"），
// 它**只回答了"默认装哪个"**，里面根本没有"这个游戏版本总共有哪些 Forge" —— 1.20.1 那对键是
// 47.4.10（recommended）和 47.4.23（latest），而实际上 1.20.1 下有 132 个版本。要列出全部
// 版本就得去它的 maven 清单（5048 条 / 211 KB，实测）。
//
// 【坐标的两种形态，以及为什么不能按字符串前缀切】
//
// 1.20.1-47.4.10 常规：<游戏>-<加载器>
// 1.8.9-11.15.1.2318-1.8.9 老版本：<游戏>-<加载器>-<游戏>
//
// 后一种是 forgeInstallerUrlAlternate() 那个坐标（老游戏版本只发布了这一种）。所以"去掉
// `<游戏>-` 前缀"之后还得把**尾巴上那个游戏版本**再剥掉，否则列表里会出现
// `11.15.1.2318-1.8.9` 这种带重复游戏号的"版本号"。
//
// 尾巴怎么认：不能按字符串比（`1.10` 的尾巴实测是 `1.10.0`，字符串 `"1.10"` 对不上
// `"1.10.0"`），得按**逐段版本前缀**比（isDottedPrefix）。这一条是实测踩出来的：
// 1.10 那 18 个版本如果不这么剥，全都会带着 `-1.10.0` 混进列表。

/// Forge 的 maven 清单（**全部** 5048 个版本，211 KB）。
///
/// NOTE: 和 promotions_slim.json 的分工是清楚的：那张表回答"默认装哪个"（推荐/最新），
/// 这一份回答"总共有哪些"。两个都要。
inline QString forgeMavenMetadataUrl() {
    return QStringLiteral(
        "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml");
}

/// Forge 的 maven 坐标 -> 加载器版本号。不是这个游戏版本的坐标时回空串。
///
/// `1.20.1-47.4.10` → `47.4.10`；`1.8.9-11.15.1.2318-1.8.9` → `11.15.1.2318`。
inline QString forgeLoaderVersionFromCoordinate(const QString &coordinate,
                                                const QString &gameVersion) {
    const QString prefix = gameVersion + QLatin1Char('-');
    if (gameVersion.isEmpty() || !coordinate.startsWith(prefix)) {
        return QString();
    }
    QString rest = coordinate.mid(prefix.size());
    const int dash = rest.lastIndexOf(QLatin1Char('-'));
    if (dash > 0) {
        const QList<int> game = parseDottedVersion(gameVersion);
        const QList<int> tail = parseDottedVersion(rest.mid(dash + 1));
        if (!game.isEmpty() && !tail.isEmpty() && isDottedPrefix(game, tail)) {
            rest = rest.left(dash);
        }
    }
    return rest;
}

/// Forge 的 maven 清单 -> 这个游戏版本能用的**全部** Forge 版本（新到旧）。
///
/// stable 一律 true：这份清单里**全**是正式版（Forge 的 maven 上没有 alpha/beta 那种后缀；
/// 带字母尾巴那几个 `10.13.3.1401-1710ls` 已经被上面第一条筛掉了）。NOTE: 这里**不是**
/// "哪个是 Forge 推荐的"那个意思 —— 推荐/最新是 promotions 的活（pickForgeVersion）。
inline LoaderVersionList listForgeVersions(const QByteArray &mavenMetadata,
                                           const QString &gameVersion) {
    LoaderVersionList entries;
    if (gameVersion.isEmpty()) {
        return entries;
    }

    QSet<QString> seen;
    static const QRegularExpression tag(QStringLiteral("<version>([^<]+)</version>"));
    QRegularExpressionMatchIterator it = tag.globalMatch(QString::fromUtf8(mavenMetadata));
    while (it.hasNext()) {
        const QString coordinate = it.next().captured(1).trimmed();
        const QString version = forgeLoaderVersionFromCoordinate(coordinate, gameVersion);
        if (version.isEmpty() || parseDottedVersion(version).isEmpty() || seen.contains(version)) {
            continue;
        }
        seen.insert(version);
        entries.append(LoaderVersionEntry{version, true});
    }
    sortLoaderVersionsNewestFirst(entries);
    return entries;
}

}  // namespace LoaderMeta

// 这两个类型要**过信号**（loaderVersionsFetched 带一整份"版本号 + 是不是测试版"回来），
// 所以得像 DownloadManager 那几份元数据一样声明成 metatype。声明放在**命名空间外面**：
// 放里面声明出来的名字是裸的 `LoaderVersionEntry`，而 Q_ARG 那侧写的名字是
// `LoaderMeta::LoaderVersionList` —— 对不上就等于没注册（Qt 只在运行时报一句 no such type）。
Q_DECLARE_METATYPE(LoaderMeta::LoaderVersionEntry)
Q_DECLARE_METATYPE(LoaderMeta::LoaderVersionList)
