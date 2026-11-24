/**
 * @file ServerInstall.hpp
 * @brief 服务端安装链路的**纯函数**部分：世界上有哪几档服务端、每一档"要下什么、
 * 要不要跑安装器、装完必须看到哪个文件、启动它发什么参数"。
 * @note 和 LoaderMeta.hpp 一个形状：header-only、全 inline、**不联网、不发信号、不读盘**，
 * 只做"参数进、计划出"。发请求和起进程留给以后接（用户 ：「下载里面准备
 * 一下服务端（原版端，spigot，（neo）forge，fabric服务器等）看看安装链路有没有打通，
 * 研究一下怎么安装，不急着具体做入口，先准备好功能到时候和ui做链接」）。
 *
 * 【为什么单独一个文件，而不是塞进 VersionManager 那条客户端安装的路】
 *
 * 客户端那一套（LoaderMeta + VersionManager::runInstaller）解决的是"往 versions/ 里装一份
 * **能在启动器里点启动**的客户端"：要写 launcher_profiles.json、要核对 versions/<id>/ 目录
 * 真的存在、要生成 profile。服务端**一样都不要**：它不注册到启动器里，产物是一个能直接
 * `java -jar` 跑的目录 —— Forge / NeoForge 那两档连 jar 都没有，入口是一张参数文件
 * （`libraries/.../win_args.txt`）。
 *
 * 唯一的交集是"都是跑别人的安装器 jar"，但连地址都不是同一条（`--installClient` vs
 * `--installServer`，见 VersionManager::forgeInstallerArguments）。硬塞在一起的话，
 * 那个函数会长出一个 bool 参数来分岔，而两边的"装完怎么核对"整段都不一样。
 *
 * Paper GET https://fill.papermc.io/v3/projects/paper/versions/1.21.4/builds
 * -> 数组，**按构建号升序**（实测 224 条，头一条 id=1）。每条：
 * {id:232, time, channel:"STABLE"|"ALPHA",
 * downloads:{"server:default":{name, checksums:{sha256}, size, url}}, commits:[...]}
 * 实测 channel 只有 STABLE / ALPHA 两个值。
 * 旧接口 api.papermc.io/v2 已经 410 下线，别照老教程写。
 * Paper GET https://fill.papermc.io/v3/projects/paper
 * -> {project:{id,name}, versions:{"1.21":["1.21.11","1.21.11-rc3",…], "1.20":[…]}}
 * 按大版本分组，组里**新到旧**。
 * Purpur GET https://api.purpurmc.org/v2/purpur
 * -> {project, metadata:{current}, versions:[…]}（**平铺**一维数组，不是分组的）
 * Purpur GET https://api.purpurmc.org/v2/purpur/1.21.4/latest
 * -> {project, version, build:"2416", result:"SUCCESS", timestamp, duration, commits}
 * `build` 是**字符串**不是数字；`result` 不是 SUCCESS 的话这条构建是坏的。
 * Fabric GET https://meta.fabricmc.net/v2/versions/loader/<mc>/<loader>/<installer>/server/jar
 * -> 200，application/java-archive，168437 字节（1.21.4 + loader 0.16.9 + installer 1.0.1）。
 * **三段缺一不可**：少了 loader 段或 installer 段都是 404（两种都实测过）。
 * 这条是 `Transfer-Encoding: chunked`：**没有 Content-Length**（HEAD 也不报），
 * 所以下载进度只能是"已下多少"、算不出百分比；没有 ETag，有 Last-Modified。
 * 同一条地址连下两次字节完全一样。
 * `/v2/versions/loader/<mc>/server/json` 是 **404**（没有这个端点，别信），
 * Fabric 服务端这条路上没有"先拿一份 json"那一步。
 * Fabric GET https://meta.fabricmc.net/v2/versions/installer
 * -> [{url, maven:"net.fabricmc:fabric-installer:1.1.2", version:"1.1.2", stable:true}, …]
 * 新到旧，实测只有最新那条 stable。
 * Quilt GET https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/maven-metadata.xml
 * -> 常规 maven 元数据（实测 latest/release 都是 0.15.1）
 * Quilt 没有 `server/jar` 那种端点（404）。走它 installer 的命令行：
 * java -jar quilt-installer.jar install server <mc> [loader] \
 * --install-dir=<目录> --download-server
 * 那个 `--install-dir` **必须写成 `--install-dir=<值>`**，分段写它不认 ——
 * 实测原话是 `Option "--install-dir" must specify a value`（而且**退出码 0**，
 * 参数写错也不报失败；这条命令**不能拿退出码当判据**）。
 * `--download-server` 是它 usage 里写明的服务端安装选项（"Downloads the
 * corresponding Minecraft server for the specified install version, naming it
 * server.jar"）。不给它的话，装完的目录里**没有 server.jar**，启动器直接退出
 * （见下面那段实测）。
 * Spigot 官方**不发成品**，没有直链。只有 BuildTools：
 * https://hub.spigotmc.org/jenkins/job/BuildTools/lastSuccessfulBuild/artifact/target/BuildTools.jar
 * 现场编（`java -jar BuildTools.jar --rev <版本>`，要 JDK 21+，产物 `spigot-<版本>-R0.1-SNAPSHOT.jar`）。
 * Forge / NeoForge 的安装器地址复用 LoaderMeta（`forgeInstallerUrl` / `neoforgeInstallerUrl`），
 * 装法是把 `--installClient` 换成 `--installServer <目录>`。
 * Vanilla 没有独立端点：地址在版本 json 的 `downloads.server` 里（见 vanillaServerDownload）。
 *
 * Fabric 下下来那份 `fabric-server-launch.jar`（168437 字节）**本身就是启动器**
 * （栈里是 `net.fabricmc.installer.ServerLauncher`），落地名没被改过、字节也没变。
 * 第一次跑：自己联网下 8 个 loader 库，原版那部分是从**它下下来的 bundler jar
 * 本地解开**的；第二次跑一条下载都没有 ⇒ `firstLaunchNeedsNetwork` 那一格是真的。
 * Quilt 装出来那份 `quilt-server-launch.jar` 只有 **487 字节**：一张**只有 manifest** 的
 * 壳（Main-Class + Class-Path），**不是**带代码的启动器。loader 的 11 个库是
 * **安装器**下的，而且它每次跑都重下一遍（没有本地缓存）。第一次跑**不联网**，
 * 只把本地那份原版 bundler jar 解开（实测 44 条 Unpacking、0 条下载）。
 * 缺 `server.jar` 时它直接退出：`The Minecraft server .JAR is missing (…\server.jar)!`，
 * 并写下 `quilt-server-launcher.properties`（里面 `serverJar=server.jar`）。
 * ⇒ **Quilt 那格 `firstLaunchNeedsNetwork` 是 false**，前提是装的时候带了
 * `--download-server`（或者驱动层自己把原版 server.jar 放进去）。
 * 两档共有的第一道坎：第一次跑都会写 `eula.txt`（`eula=false`）然后**拒绝启动** ——
 * 要有人把那个 false 改成 true。这八档全是原版血统的服务端，这条对它们一视同仁，
 * 接 UI 的时候得有个地方问用户（本文件只管"怎么装"，不管这件事）。
 */

#pragma once

#include "LoaderMeta.hpp"
#include <QCoreApplication>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace ServerInstall {

// LoaderMeta 那边现成的几样，这里直接用 —— 不另写一套（两个"版本号怎么比大小"迟早会漂开）。
// 写成 using 声明而不是在每个调用点上加 `LoaderMeta::`：下面出现的次数多，而且这几样
// 本来就是"同一个概念、同一份实现"，读的时候不该每一行都要提醒一次它住在隔壁。
using LoaderMeta::forgeInstallerUrl;
using LoaderMeta::isStableLoaderVersion;
using LoaderMeta::LoaderVersionEntry;
using LoaderMeta::LoaderVersionList;
using LoaderMeta::loaderVersionStrings;
using LoaderMeta::neoforgeInstallerUrl;
using LoaderMeta::sortLoaderVersionsNewestFirst;

// ---------------------------------------------------------------------------
// 有哪几档
// ---------------------------------------------------------------------------

/**
 * @brief 服务端的八档。
 *
 * NOTE: **顺序就是界面上列表的顺序**（原版在最前，Spigot 在最后 —— 从"最省事"到"最麻烦"）。
 * 往中间插东西要连着 `allFlavors()` 一起改，别只改一边。
 */
enum class Flavor {
    Vanilla,    ///< 原版端（Mojang 官方那份 server.jar）
    Paper,      ///< 原版端的高性能分支，插件服务端里用得最多的那一档
    Purpur,     ///< Paper 的下游，玩法开关比 Paper 多一些
    Fabric,     ///< 轻量加载器，模组偏原版扩展
    Forge,      ///< 老牌模组加载器，1.20.1 及更早的服务端模组大多在它上面
    NeoForge,   ///< Forge 分家出来的那一支，新版本的服务端模组主流
    Quilt,      ///< Fabric 的分支，模组基本通用，服务端侧用得少
    Spigot,     ///< 插件服务端的老底子，官方不发成品
};

/// 全部八档，按界面上的顺序。
inline QList<Flavor> allFlavors() {
    return {Flavor::Vanilla, Flavor::Paper,  Flavor::Purpur, Flavor::Fabric,
            Flavor::Forge,   Flavor::NeoForge, Flavor::Quilt,  Flavor::Spigot};
}

/// 落盘用的代号。**这是持久化格式的一部分**（以后 `wtfml.meta` / 服务端清单里存的就是它），
/// 改一个字等于已经把服务端装好的用户下次被认成"不认识的类型"。
inline QString flavorToken(Flavor flavor) {
    switch (flavor) {
    case Flavor::Vanilla:
        return QStringLiteral("vanilla");
    case Flavor::Paper:
        return QStringLiteral("paper");
    case Flavor::Purpur:
        return QStringLiteral("purpur");
    case Flavor::Fabric:
        return QStringLiteral("fabric");
    case Flavor::Forge:
        return QStringLiteral("forge");
    case Flavor::NeoForge:
        return QStringLiteral("neoforge");
    case Flavor::Quilt:
        return QStringLiteral("quilt");
    case Flavor::Spigot:
        return QStringLiteral("spigot");
    }
    return QString();
}

/// flavorToken() 的反函数。认不出来回 false 且**不碰 out**（调用方可能正拿它当"上一次
/// 认出来的那一档"用，写一半反而把好数据弄坏 —— 和 GameWindowSettings 那边一个规矩）。
inline bool flavorFromToken(const QString &token, Flavor *out) {
    static const struct {
        const char *token;
        Flavor flavor;
    } table[] = {
        {"vanilla", Flavor::Vanilla}, {"paper", Flavor::Paper},   {"purpur", Flavor::Purpur},
        {"fabric", Flavor::Fabric},   {"forge", Flavor::Forge},   {"neoforge", Flavor::NeoForge},
        {"quilt", Flavor::Quilt},     {"spigot", Flavor::Spigot},
    };
    for (const auto &row : table) {
        if (token == QLatin1String(row.token)) {
            if (out != nullptr) {
                *out = row.flavor;
            }
            return true;
        }
    }
    return false;
}

/// 界面上给用户看的名字。
inline QString flavorName(Flavor flavor) {
    switch (flavor) {
    case Flavor::Vanilla:
        return QCoreApplication::translate("ServerInstall", "原版（Vanilla）");
    case Flavor::Paper:
        return QStringLiteral("Paper");
    case Flavor::Purpur:
        return QStringLiteral("Purpur");
    case Flavor::Fabric:
        return QStringLiteral("Fabric");
    case Flavor::Forge:
        return QStringLiteral("Forge");
    case Flavor::NeoForge:
        return QStringLiteral("NeoForge");
    case Flavor::Quilt:
        return QStringLiteral("Quilt");
    case Flavor::Spigot:
        return QStringLiteral("Spigot");
    }
    return QString();
}

/// 界面上那一行的小字：这一档是什么。**唯一出处** —— 界面别自己另写一套。
inline QString flavorNote(Flavor flavor) {
    switch (flavor) {
    case Flavor::Vanilla:
        return QCoreApplication::translate("ServerInstall", "Mojang 官方服务端，什么都不改");
    case Flavor::Paper:
        return QCoreApplication::translate("ServerInstall", "支持插件的高性能服务端，基于原版服务端。");
    case Flavor::Purpur:
        return QCoreApplication::translate("ServerInstall", "Paper 的下游，玩法开关比 Paper 多");
    case Flavor::Fabric:
        return QCoreApplication::translate("ServerInstall", "轻量加载器，模组偏原版风格的小改");
    case Flavor::Forge:
        return QCoreApplication::translate("ServerInstall", "老牌模组加载器，1.20.1 及更早的服务端模组大多在它上面");
    case Flavor::NeoForge:
        return QCoreApplication::translate("ServerInstall", "Forge 分家出来的那支，1.20.2 起的服务端模组主流");
    case Flavor::Quilt:
        return QCoreApplication::translate("ServerInstall", "Fabric 的分支，模组基本通用，服务端侧用得少");
    case Flavor::Spigot:
        return QCoreApplication::translate("ServerInstall", "插件服务端的老底子，官方不发成品，得现编一份");
    }
    return QString();
}

/**
 * @brief "这一档在这个游戏版本上到底有没有" —— **不在这里判**，别在这层写死一张版本表。
 *
 * NOTE: 没有便宜的离线判据（NeoForge 只从 1.20.2 起有、Paper 不是每个版本都发、Spigot 每个
 * 版本都得现编），而写死一张表就是给自己埋一个"上游又发了新版但我们的表还是旧的"。
 * 真实答案在上游的清单里，**取回来是空的 = 这个版本没有这一档**：
 *
 * 原版 -> 版本 json 里有没有 `downloads.server`（见 vanillaServerDownload）
 * Paper -> paperBuildsUrl() 取回来是空数组
 * Purpur -> purpurVersionsUrl() 里有没有这个版本号
 * Fabric -> LoaderMeta::fabricLoaderListUrl()
 * Forge -> LoaderMeta::forgePromotionsUrl()
 * NeoForge-> LoaderMeta::listNeoForgeVersions()
 * Quilt -> quiltInstallerMetadataUrl() ★ 这条只说明**安装器**有，不代表这个游戏
 * 版本有对应的 Quilt 版本（那个只有装的时候才知道）
 * Spigot -> 编了才知道（BuildTools 认不认这个 `--rev`）
 */

// ---------------------------------------------------------------------------
// 装法是哪三种
// ---------------------------------------------------------------------------

/// 这一档"装"这个动作到底是什么。
enum class InstallMethod {
    DirectJar,     ///< 下下来那个 jar 就是服务端，直接能跑
    RunInstaller,  ///< 下下来的是**安装器**，要在安装目录里跑一次才算装好
    BuildRequired, ///< 上游不发成品，得用它的构建工具现编一份
};

inline InstallMethod installMethod(Flavor flavor) {
    switch (flavor) {
    case Flavor::Vanilla:
    case Flavor::Paper:
    case Flavor::Purpur:
    case Flavor::Fabric:
        return InstallMethod::DirectJar;
    case Flavor::Forge:
    case Flavor::NeoForge:
    case Flavor::Quilt:
        return InstallMethod::RunInstaller;
    case Flavor::Spigot:
        return InstallMethod::BuildRequired;
    }
    return InstallMethod::DirectJar;
}

/// 界面上那句"这一步要做什么"。**唯一出处**。
inline QString installMethodText(InstallMethod method) {
    switch (method) {
    case InstallMethod::DirectJar:
        return QCoreApplication::translate("ServerInstall", "下载后即为服务端，可直接运行");
    case InstallMethod::RunInstaller:
        return QCoreApplication::translate("ServerInstall", "下载的是安装器，需在安装目录中运行一次");
    case InstallMethod::BuildRequired:
        return QCoreApplication::translate("ServerInstall", "官方不提供成品，需用其构建工具现场编译（需要几分钟并联网）");
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 地址怎么拼
// ---------------------------------------------------------------------------

// ---- Paper ----

/// Paper 某个游戏版本的构建列表。取回来是**升序**的，读的时候用 paperBuildsFromJson 排。
inline QString paperBuildsUrl(const QString &gameVersion) {
    return QStringLiteral("https://fill.papermc.io/v3/projects/paper/versions/%1/builds")
        .arg(gameVersion);
}

/// Paper 的工程信息（里面有"哪些游戏版本有 Paper"，按大版本分组）。
inline QString paperProjectUrl() {
    return QStringLiteral("https://fill.papermc.io/v3/projects/paper");
}

// ---- Purpur ----

/// Purpur 的工程信息（`versions` 是**平铺**的一维版本号数组）。
inline QString purpurVersionsUrl() {
    return QStringLiteral("https://api.purpurmc.org/v2/purpur");
}

/// Purpur 某个版本的**最新一条**构建。
inline QString purpurLatestUrl(const QString &gameVersion) {
    return QStringLiteral("https://api.purpurmc.org/v2/purpur/%1/latest").arg(gameVersion);
}

inline QString purpurDownloadUrl(const QString &gameVersion, const QString &build) {
    return QStringLiteral("https://api.purpurmc.org/v2/purpur/%1/%2/download").arg(gameVersion, build);
}

// ---- Fabric ----

/// Fabric 的安装器列表。
inline QString fabricInstallerListUrl() {
    return QStringLiteral("https://meta.fabricmc.net/v2/versions/installer");
}

/// Fabric 服务端那个 jar 的地址。
///
/// **三段一个都不能少**：少 loader 段或少 installer 段都是 404（两种都实测过）。
/// 也是因为这一段，`pickFabricInstaller` 那边"挑一个安装器版本"不是可有可无的：
/// 没挑出来就拼不出这条地址。
///
/// NOTE: 下下来那 168 KB 的 jar **不是服务端本身，是个启动器**：第一次跑的时候它自己会去
/// maven 把库拉下来（所以 `Plan::firstLaunchNeedsNetwork` 那一档是 true）。
inline QString fabricServerJarUrl(const QString &gameVersion, const QString &loaderVersion,
                                  const QString &installerVersion) {
    return QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/%1/%2/%3/server/jar")
        .arg(gameVersion, loaderVersion, installerVersion);
}

// ---- Quilt ----

/// Quilt 安装器的 maven 元数据（里面是**全部**安装器版本号）。
inline QString quiltInstallerMetadataUrl() {
    return QStringLiteral(
        "https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/maven-metadata.xml");
}

/// Quilt 安装器 jar 的地址。
inline QString quiltInstallerUrl(const QString &installerVersion) {
    return QStringLiteral("https://maven.quiltmc.org/repository/release/org/quiltmc/quilt-installer/%1/quilt-installer-%1.jar")
        .arg(installerVersion);
}

// ---- Spigot ----

/// BuildTools 的地址（jenkins 上"最后一次成功的构建"，没有版本号那种稳定直链 ——
/// 这也正是 Spigot 这一档"装法不可复现"的地方：今天编和下周编出来的不是同一个文件）。
inline QString spigotBuildToolsUrl() {
    return QStringLiteral("https://hub.spigotmc.org/jenkins/job/BuildTools/lastSuccessfulBuild/artifact/target/BuildTools.jar");
}

// ---------------------------------------------------------------------------
// 回复怎么读
// ---------------------------------------------------------------------------

/// 能下的一"条"东西（Paper 的一个构建 / 原版那一个服务端 / Fabric 那个启动器 jar）。
struct Build {
    int number = 0;        ///< 构建号（Paper / Purpur 才有；别的恒 0）。比大小就按它
    QString channel;       ///< 上游标的频道（Paper 实测只有 `STABLE` / `ALPHA`；别的恒空）
    QString fileName;      ///< 上游给这个文件起的名字（**我们落地时会改名**，见 Plan::downloads）
    QString url;
    QString sha1;          ///< 空 = 上游没给
    QString sha256;        ///< 空 = 上游没给
    qint64 size = -1;      ///< -1 = 上游没报（Fabric 那条是 chunked，**没有 Content-Length**）

    bool ok() const { return !url.isEmpty(); }
};

/// Paper 的构建列表 -> 能下的那些条，**新到旧**。
inline QList<Build> paperBuildsFromJson(const QJsonArray &builds) {
    QList<Build> entries;
    for (const QJsonValue &value : builds) {
        const QJsonObject object = value.toObject();
        const QJsonObject downloads = object.value(QStringLiteral("downloads")).toObject();
        QJsonObject file = downloads.value(QStringLiteral("server:default")).toObject();
        if (file.isEmpty()) {
            const QStringList keys = downloads.keys();
            for (const QString &key : keys) {
                const QJsonObject candidate = downloads.value(key).toObject();
                if (!candidate.value(QStringLiteral("url")).toString().isEmpty()) {
                    file = candidate;
                    break;
                }
            }
        }
        const QString url = file.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            continue;
        }
        Build entry;
        entry.number = object.value(QStringLiteral("id")).toInt();
        entry.channel = object.value(QStringLiteral("channel")).toString();
        entry.fileName = file.value(QStringLiteral("name")).toString();
        entry.url = url;
        entry.sha256 = file.value(QStringLiteral("checksums"))
                           .toObject()
                           .value(QStringLiteral("sha256"))
                           .toString();
        entry.size = static_cast<qint64>(file.value(QStringLiteral("size")).toDouble(-1.0));
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Build &left, const Build &right) { return left.number > right.number; });
    return entries;
}

/// 这一条构建是不是上游标的稳定档。
inline bool paperBuildIsStable(const Build &build) {
    return build.channel.compare(QLatin1String("STABLE"), Qt::CaseInsensitive) == 0;
}

/// 挑一条 Paper 构建：稳定档 = 第一条 stable 的（全没有就退回最新那条，
/// 否则"这个版本只有 ALPHA"时会变成一条都下不了）。
inline Build pickPaperBuild(const QList<Build> &builds, bool stableOnly) {
    if (!stableOnly) {
        return builds.isEmpty() ? Build{} : builds.first();
    }
    for (const Build &build : builds) {
        if (paperBuildIsStable(build)) {
            return build;
        }
    }
    return builds.isEmpty() ? Build{} : builds.first();
}

/**
 * @brief Paper 的工程信息 -> 有 Paper 的游戏版本号，**新到旧**。
 *
 * **必须自己排**，两边都靠不住：
 *
 * ① 上游那份是按大版本**分组**的一个对象（`{"1.21":[…], "1.20":[…]}`），而这一层读到的是
 * QJsonObject —— 它内部按键的**字典序**存，也就是说"上游 JSON 里那个书写顺序"到这里
 * 已经不存在了。直接遍历 keys() 摊平出来是 `1.20` 在前、`1.21` 在后，正好是反的；
 * 真实数据更难看：`1.21` 排在 `26.1` 前面（字典序 `1` < `2`），而 26.x 才是新的。
 * ② 组**内**才是新到旧，不能推广到组与组之间。
 *
 * 排序沿用加载器那几档同一套（LoaderMeta 的版本号比较 + 正式版排在它的 rc 前面），
 * 所以不会出现"同一个版本号在两个列表里排得不一样"。
 */
inline QStringList paperVersionsFromJson(const QJsonObject &project) {
    LoaderVersionList entries;
    QSet<QString> seen;
    const QJsonObject groups = project.value(QStringLiteral("versions")).toObject();
    const QStringList keys = groups.keys();
    for (const QString &key : keys) {
        const QJsonArray list = groups.value(key).toArray();
        for (const QJsonValue &value : list) {
            const QString version = value.toString();
            if (version.isEmpty() || seen.contains(version)) {
                continue;
            }
            seen.insert(version);
            entries.append(LoaderVersionEntry{version, isStableLoaderVersion(version)});
        }
    }
    sortLoaderVersionsNewestFirst(entries);
    return loaderVersionStrings(entries);
}

/**
 * @brief Purpur 的工程信息 -> 有 Purpur 的游戏版本号，**新到旧**。
 *
 * NOTE: 上游给的是**平铺**的一维数组（不像 Paper 那样按大版本分组），看着像能直接用原序 ——
 * 不能。那个数组是**按发布时间升序**的（实测尾部是 `… "1.21.11", "26.1.2", "26.2",
 * "26.3"`），拿原序当"新到旧"用，界面上第一个版本号会是它最老的那个。
 * 所以和 Paper 那条一样自己排。
 */
inline QStringList purpurVersionsFromJson(const QJsonObject &project) {
    LoaderVersionList entries;
    QSet<QString> seen;
    const QJsonArray list = project.value(QStringLiteral("versions")).toArray();
    for (const QJsonValue &value : list) {
        const QString version = value.toString();
        if (version.isEmpty() || seen.contains(version)) {
            continue;
        }
        seen.insert(version);
        entries.append(LoaderVersionEntry{version, isStableLoaderVersion(version)});
    }
    sortLoaderVersionsNewestFirst(entries);
    return loaderVersionStrings(entries);
}

/// Purpur 的 `/<版本>/latest` 回复 -> 构建号；**空 = 这条构建是坏的、不能下**。
///
/// 判据 `result == "SUCCESS"`：上游会连着失败那几条一起报出来，坏的那个直接别下。
inline QString purpurBuildFromJson(const QJsonObject &latest) {
    if (latest.value(QStringLiteral("result")).toString() != QLatin1String("SUCCESS")) {
        return QString();
    }
    const QString build = latest.value(QStringLiteral("build")).toString();
    return build.isEmpty() ? QString() : build;
}

/// Fabric 的安装器列表 -> 能挑的安装器版本，**原样顺序**（上游就是新到旧；实测只有最新那条
/// stable 是 true）。不自己排序的理由和 listFabricLoaderVersions 一样。
inline LoaderVersionList listFabricInstallerVersions(const QJsonArray &list) {
    LoaderVersionList entries;
    for (const QJsonValue &value : list) {
        const QJsonObject object = value.toObject();
        const QString version = object.value(QStringLiteral("version")).toString();
        if (version.isEmpty()) {
            continue;
        }
        entries.append(
            LoaderVersionEntry{version, object.value(QStringLiteral("stable")).toBool()});
    }
    return entries;
}

/// 挑一个 Fabric 安装器版本：稳定档 = 第一条 stable 的；没有 stable 的就退回最新那条
/// （这条路上"挑不出来"比"挑了个不 stable 的"更糟 —— 挑不出来拼不出地址，一整档就废了）。
inline QString pickFabricInstaller(const QJsonArray &list, bool stableOnly) {
    const LoaderVersionList entries = listFabricInstallerVersions(list);
    if (entries.isEmpty()) {
        return QString();
    }
    if (stableOnly) {
        for (const LoaderVersionEntry &entry : entries) {
            if (entry.stable) {
                return entry.version;
            }
        }
    }
    return entries.first().version;
}

/// Quilt 安装器那几个版本 -> 能挑的版本，**新到旧**。
///
/// NOTE: 和 NeoForge 那份 maven 元数据同一形状，所以直接用 NeoForge 那边现成的判据
/// （parseDottedVersion / isStableLoaderVersion / sortLoaderVersionsNewestFirst），
/// 不另写一套"版本号怎么比大小" —— 两套迟早会不一致。
inline LoaderVersionList listQuiltInstallerVersions(const QByteArray &mavenMetadata) {
    LoaderVersionList entries;
    QSet<QString> seen;
    static const QRegularExpression tag(QStringLiteral("<version>([^<]+)</version>"));
    QRegularExpressionMatchIterator it = tag.globalMatch(QString::fromUtf8(mavenMetadata));
    while (it.hasNext()) {
        const QString version = it.next().captured(1).trimmed();
        if (version.isEmpty() || seen.contains(version)) {
            continue;
        }
        seen.insert(version);
        entries.append(LoaderVersionEntry{version, isStableLoaderVersion(version)});
    }
    sortLoaderVersionsNewestFirst(entries);
    return entries;
}

/// 装完之后在目录里找那个 Spigot 的产物。
///
/// 为什么不能用"装完必须有 `<游戏版本>.jar`"来判：BuildTools 的产物名是
/// `spigot-<版本>-R0.1-SNAPSHOT.jar` —— **后缀是它自己那套版本号规则加上去的**，从游戏版本号
/// 拼不出来。所以只能扫目录。回空 = 没编出来。
inline QString pickSpigotJar(const QStringList &fileNames) {
    for (const QString &name : fileNames) {
        if (name.startsWith(QLatin1String("spigot-")) && name.endsWith(QLatin1String(".jar"))) {
            return name;
        }
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 计划
// ---------------------------------------------------------------------------

/// 要下下来的一个文件。
struct Download {
    /// 落在**安装目录根**下叫什么。NOTE: 工具 jar（`Forge`/`NeoForge`/`Quilt`/`Spigot` 那几档）
    /// 也先落在根下，**由驱动层按 `Plan::toolRunDirectory` 挪进子目录再跑**。
    QString fileName;
    QString url;
    QString sha1;      ///< 空 = 上游没给
    QString sha256;    ///< 空 = 上游没给
    qint64 size = -1;  ///< -1 = 上游没报（这条的进度只能报"已下多少"，算不出百分比）

    bool ok() const { return !url.isEmpty(); }
};

/**
 * @brief 原版服务端 —— 它**不在这一层拼地址**，地址是版本 json 的一部分。
 *
 * 原版服务端跟着当前游戏版本的元数据走（`downloads.server`），所以这一档的"要下什么"
 * 得拿那份 json 来问，而不是拿两个字符串拼。取不到 = 这个版本没有官方服务端
 * （远古版本确实没有）。
 */
inline Download vanillaServerDownload(const QJsonObject &versionJson) {
    const QJsonObject server = versionJson.value(QStringLiteral("downloads"))
                                   .toObject()
                                   .value(QStringLiteral("server"))
                                   .toObject();
    const QString url = server.value(QStringLiteral("url")).toString();
    if (url.isEmpty()) {
        return Download{};
    }
    Download jar;
    jar.fileName = QStringLiteral("server.jar");
    jar.url = url;
    jar.sha1 = server.value(QStringLiteral("sha1")).toString();
    // 原版这里**有** size（版本 json 里写着），所以它那条下载进度是算得出来的百分比 ——
    // 和 Fabric 那条 chunked 的正好相反。
    jar.size = static_cast<qint64>(server.value(QStringLiteral("size")).toDouble(-1.0));
    return jar;
}

/**
 * @brief "给我装这一档"需要的全部输入。
 *
 * 都是**用户在界面上挑的字符串**，这一层一个都不去核实（核实是发请求那一层的事）。
 */
struct Request {
    Flavor flavor = Flavor::Vanilla;
    QString gameVersion;       ///< 原版版本号。**八档全都要**（原版那份服务端也在版本 json 里）
    QString loaderVersion;     ///< 加载器版本（Forge / NeoForge / Fabric / Quilt 才用）
    QString installerVersion;  ///< 安装器版本（Fabric / Quilt 才用）
    QString buildId;           ///< 构建号（Paper / Purpur 才用；空 = 取最新那条）
};

/**
 * @brief 这一档怎么装 —— 界面读这一个结构就能把"要下什么、要不要跑安装器、装完是什么样"
 * 全画出来；真正干活的那半边（下、跑进程、核对产物）照这个结构做就行。
 *
 * NOTE: `ok == false` 时**只有 `problem` 是有意义的**，别的字段一个都别读（这一层不保证它们
 * 是空的还是半填的）。
 */
struct Plan {
    bool ok = false;
    QString problem;  ///< ok == false 时**给用户看的那句话**

    Flavor flavor = Flavor::Vanilla;
    InstallMethod method = InstallMethod::DirectJar;

    /// 按顺序下这些。**可能是空的**：原版（地址在版本 json 里）和 Paper（地址里的对象名带
    /// sha256、拼不出来，见 note）这两档的地址都不是这一层能给的。
    QList<Download> downloads;

    /// 跑那个工具时的参数（**不含** java 可执行文件、不含 `-jar`；工具 jar 的路径由驱动层补上）。
    QStringList toolArguments;

    /// 跑工具时的**工作目录**（相对安装目录；空 = 就在安装目录里跑）。
    ///
    /// 非空时驱动层要做两件 extra 的事：① 把那个工具 jar **挪进**这个子目录再在那儿跑；
    /// ② 装完之后**把产物搬回安装目录根**（产物会落在跑它的地方，不在根下）。
    QString toolRunDirectory;

    /// 装完必须**存在**的这些相对路径（"装好了没有"的判据）。
    /// 空 = 这一档没法用文件名判（见 note）。
    QStringList installedMarkers;

    /// 装完之后启动它发什么（**不含** java、不含内存/额外 JVM 参数、不含工作目录 ——
    /// 那几样跟服务端是哪一档没关系，由调用方按实例设置补上；拼那一下的唯一出处是
    /// `ServerRun::launchArguments()`）。
    /// 空 = 得先看目录里产物叫什么（见 note）。
    QStringList launchTokens;

    /// 调用方补的那几项 JVM 参数（内存…）该插在 `launchTokens` 的**第几位**。
    ///
    /// 默认 0 = 接在最前面。**Forge / NeoForge 是 1** —— 必须夹在两张 `@` 参数文件中间：
    /// 排在 `@user_jvm_args.txt` **之后**（那张文件里也写着 `-Xmx`，同一个参数以最后出现的
    /// 为准，接在前面会被它盖掉、界面上那根内存滑杆成了摆设），又排在 `@win_args.txt`
    /// **之前**（那张文件**自带主类**，它之后的一切都会被 java 当成给服务端的程序参数 ⇒
    /// `-Xmx` 一个字都不生效）。
    /// 这里原来那两段 NOTE 说的是"接在这几个 token **后面**" —— 照它实现会
    /// 拼出 `… win_args.txt nogui -Xmx4G`，而 `-Xmx` 落在主类后面**不生效**。那一版说法是
    /// 错的，改用这一格表达"插在第几位"。
    int launchJvmArgsIndex = 0;

    /// 第一次启动时它自己还要联网拉东西（下下来的那个 jar 是个启动器）。
    bool firstLaunchNeedsNetwork = false;

    /// 给用户看的一句补充（可空）。**唯一出处** —— 界面照抄，别自己另写。
    QString note;
};

/// 拼一份安装计划。
///
/// 缺参数时 `ok = false` + 一句能直接给用户看的 `problem`（不是"参数不合法"这种话）。
inline Plan planFor(const Request &request) {
    Plan plan;
    plan.flavor = request.flavor;
    plan.method = installMethod(request.flavor);

    if (request.gameVersion.isEmpty()) {
        plan.problem = QCoreApplication::translate("ServerInstall", "请选择游戏版本");
        return plan;
    }

    switch (request.flavor) {
    case Flavor::Vanilla:
        // 地址不在这层：原版服务端是版本 json 的一部分（downloads.server）。
        plan.ok = true;
        plan.installedMarkers = QStringList{QStringLiteral("server.jar")};
        plan.launchTokens = QStringList{QStringLiteral("-jar"), QStringLiteral("server.jar"),
                                        QStringLiteral("nogui")};
        plan.note = QCoreApplication::translate("ServerInstall", "它是版本 json 里的一部分，地址跟着当前游戏版本走（见 vanillaServerDownload）");
        return plan;

    case Flavor::Paper: {
        // downloads 空是**故意**的：那条地址里的对象名带 sha256
        // （fill-data.papermc.io/v1/objects/<sha256>/paper-1.21.4-232.jar），从"游戏版本 +
        // 构建号"两个字符串拼不出来 —— 必须先把构建列表取回来。
        plan.ok = true;
        plan.installedMarkers = QStringList{QStringLiteral("server.jar")};
        plan.launchTokens = QStringList{QStringLiteral("-jar"), QStringLiteral("server.jar"),
                                        QStringLiteral("nogui")};
        plan.note = QCoreApplication::translate("ServerInstall", "下载地址要先取构建列表（见 paperBuildsUrl / paperBuildsFromJson），取回来那条里才有地址和校验值");
        return plan;
    }

    case Flavor::Purpur: {
        const QString build =
            request.buildId.isEmpty() ? QStringLiteral("latest") : request.buildId;
        Download jar;
        jar.fileName = QStringLiteral("server.jar");
        jar.url = purpurDownloadUrl(request.gameVersion, build);
        // NOTE: Purpur 只在 `/<版本>/latest` 那条回复里给一个 **md5**（没有 sha1、没有 sha256）
        // ⇒ 这一条的两个校验字段都是空的。下载层要能接受"这一条没校验值"。
        plan.downloads = QList<Download>{jar};
        plan.ok = true;
        plan.installedMarkers = QStringList{QStringLiteral("server.jar")};
        plan.launchTokens = QStringList{QStringLiteral("-jar"), QStringLiteral("server.jar"),
                                        QStringLiteral("nogui")};
        if (request.buildId.isEmpty()) {
            plan.note = QCoreApplication::translate("ServerInstall", "没给构建号 ⇒ 下的是最新那条");
        }
        return plan;
    }

    case Flavor::Fabric: {
        if (request.loaderVersion.isEmpty()) {
            plan.problem = QCoreApplication::translate("ServerInstall", "请选择 Fabric 加载器版本");
            return plan;
        }
        if (request.installerVersion.isEmpty()) {
            plan.problem = QCoreApplication::translate("ServerInstall", "这一档的地址里必须带安装器版本（少一段就是 404）");
            return plan;
        }
        Download jar;
        // 落地就叫这个名字：下下来那份 jar 是**启动器**，第一次跑它会在旁边把这个名字
        // 那份真正的启动 jar 生出来。改成别的名字它照样能跑，但"盘上到底哪个是入口"
        // 就变成两套说法了 —— 照它自己的约定来最省事。
        jar.fileName = QStringLiteral("fabric-server-launch.jar");
        jar.url = fabricServerJarUrl(request.gameVersion, request.loaderVersion,
                                     request.installerVersion);
        plan.downloads = QList<Download>{jar};
        plan.ok = true;
        plan.installedMarkers = QStringList{QStringLiteral("fabric-server-launch.jar")};
        plan.launchTokens = QStringList{QStringLiteral("-jar"),
                                        QStringLiteral("fabric-server-launch.jar"),
                                        QStringLiteral("nogui")};
        plan.firstLaunchNeedsNetwork = true;
        plan.note = QCoreApplication::translate("ServerInstall", "下载的是启动器而非服务端：首次运行会自动下载依赖库（需要联网），之后为纯本地运行");
        return plan;
    }

    case Flavor::Forge:
    case Flavor::NeoForge: {
        const bool neo = request.flavor == Flavor::NeoForge;
        if (request.loaderVersion.isEmpty()) {
            plan.problem = neo ? QCoreApplication::translate("ServerInstall", "请选择 NeoForge 版本")
                               : QCoreApplication::translate("ServerInstall", "请选择 Forge 版本");
            return plan;
        }
        // 两张参数文件的路径都是**安装器自己定的**，我们只是照着算出来（LoaderMeta 里
        // forgeLoaderId / neoforgeLoaderId 那两段注释说的就是这个道理）。改一个字就等于
        // "装好了但启动不起来"，而且报出来的是"找不到文件"而不是"我们算错了"。
        const QString argsPath =
            neo ? QStringLiteral("libraries/net/neoforged/neoforge/%1/win_args.txt")
                      .arg(request.loaderVersion)
                : QStringLiteral("libraries/net/minecraftforge/forge/%1-%2/win_args.txt")
                      .arg(request.gameVersion, request.loaderVersion);

        Download installer;
        installer.fileName = QStringLiteral("installer.jar");
        installer.url = neo ? neoforgeInstallerUrl(request.loaderVersion)
                            : forgeInstallerUrl(request.gameVersion, request.loaderVersion);
        plan.downloads = QList<Download>{installer};

        // `.` 而不是绝对路径：工具就是**在安装目录里**跑的（toolRunDirectory 空），
        // 这样这一层不用知道任何一个盘的路径 —— 它是纯函数。
        plan.toolArguments = QStringList{QStringLiteral("--installServer"), QStringLiteral(".")};

        plan.ok = true;
        plan.installedMarkers = QStringList{argsPath, QStringLiteral("user_jvm_args.txt")};
        // 入口不是 jar，是这两张 `@` 参数文件。**我们不跑安装器生成的 run.bat**：那是一个
        // cmd 脚本，它自己再叫一次 java，内存从 user_jvm_args.txt 里读 —— 我们想让实例设置
        // 里那几个内存旋钮说了算，就得自己拼这条命令。
        // NOTE: 顺序有讲究，**而且不是"接在前面还是接在后面"一句话说得清的**：我们的
        // `-Xmx` 要夹在两张文件**中间** —— 见下面 `launchJvmArgsIndex` 那一格和
        // `ServerRun.hpp` 文件头那段（`@win_args.txt` 自带主类，接在它后面等于没发）。
        plan.launchTokens = QStringList{QStringLiteral("@user_jvm_args.txt"),
                                        QStringLiteral("@") + argsPath,
                                        QStringLiteral("nogui")};
        // 我们的内存那几项夹在两张参数文件**中间**（理由与那条被改掉的旧说法见
        // `launchJvmArgsIndex` 上面那段）。
        plan.launchJvmArgsIndex = 1;
        plan.note = QCoreApplication::translate("ServerInstall", "装完的入口不是 jar，是它生成的两张参数文件；启动时我们不跑它的 run.bat，直接自己拼这条命令");
        return plan;
    }

    case Flavor::Quilt: {
        if (request.installerVersion.isEmpty()) {
            plan.problem = QCoreApplication::translate("ServerInstall", "请选择 Quilt 安装器版本");
            return plan;
        }
        Download installer;
        installer.fileName = QStringLiteral("quilt-installer.jar");
        installer.url = quiltInstallerUrl(request.installerVersion);
        plan.downloads = QList<Download>{installer};

        QStringList args{QStringLiteral("install"), QStringLiteral("server"),
                         request.gameVersion};
        // 加载器版本可以不给（不给就是它自己挑最新那个）—— 所以这里不是必填。
        if (!request.loaderVersion.isEmpty()) {
            args.append(request.loaderVersion);
        }
        // 必须写成 `--install-dir=<值>` 连在一起的一段：分段写它不认（实测原话
        // `Option "--install-dir" must specify a value`）。
        args.append(QStringLiteral("--install-dir=."));
        args.append(QStringLiteral("--download-server"));
        plan.toolArguments = args;

        plan.ok = true;
        // 两张都要：光有 quilt-server-launch.jar 只说明安装器跑完了，**还不能启动** ——
        // 缺 server.jar 时启动器一句话不说就退出（实测），所以它也算"装好了没有"。
        plan.installedMarkers = QStringList{QStringLiteral("quilt-server-launch.jar"),
                                            QStringLiteral("server.jar")};
        plan.launchTokens = QStringList{QStringLiteral("-jar"),
                                        QStringLiteral("quilt-server-launch.jar"),
                                        QStringLiteral("nogui")};
        plan.firstLaunchNeedsNetwork = false;
        plan.note = QCoreApplication::translate("ServerInstall", "安装时使用 --download-server 下载原版 server.jar。Quilt 的 quilt-server-launch.jar 可在安装后离线启动");
        return plan;
    }

    case Flavor::Spigot: {
        Download tool;
        tool.fileName = QStringLiteral("BuildTools.jar");
        tool.url = spigotBuildToolsUrl();
        plan.downloads = QList<Download>{tool};

        plan.toolArguments = QStringList{QStringLiteral("--rev"), request.gameVersion};
        // 在子目录里编：BuildTools 会在当前目录铺一整套 git 仓库和中间产物（几百 MB），
        // 直接铺在服务端目录里的话那个目录就废了。代价是**产物也落在那个子目录里**，
        // 驱动层编完要把它搬上来。
        plan.toolRunDirectory = QStringLiteral("BuildTools");

        plan.ok = true;
        plan.firstLaunchNeedsNetwork = true;  // 编的过程中要 git clone + maven 拉依赖
        // 产物名带 `-R0.1-SNAPSHOT` 后缀，从游戏版本号拼不出来 ⇒ 只能扫目录（见 pickSpigotJar）。
        plan.note = QCoreApplication::translate("ServerInstall", "BuildTools 在子目录中生成 spigot-<版本>-R0.1-SNAPSHOT.jar。安装时将选中的 JAR 移到服务端目录，并据此生成启动参数。");
        return plan;
    }
    }

    plan.problem = QCoreApplication::translate("ServerInstall", "未知的服务端类型");
    return plan;
}

}  // namespace ServerInstall
