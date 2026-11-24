/**
 * @file ServerInstallRun.hpp
 * @brief 「拿一份安装计划，真的把这一档装出来」这条路上的**判据层**
 * （纯判据：不下载、不写盘、不起进程）。
 *
 * 【它和 `download/ServerInstall.hpp` 差在哪】
 *
 * `ServerInstall` 回答的是「装这一档要下什么、跑不跑安装器、装完该看到哪个文件」——那是一份
 * **计划**，而且是一份**尽量写死了的计划**：能拼出地址的那几档（Purpur / Fabric / Forge /
 * NeoForge / Quilt / Spigot）它自己就拼好了。
 *
 * 这一层回答的是计划里**留了两处口子**之后怎么办：
 *
 * ① 「地址还没取回来」的那两档（原版 / Paper）。原版的地址在版本 json 的 `downloads.server`
 * 里，Paper 的地址里带着对象 sha256（`…/v1/objects/<sha256>/paper-1.21.4-232.jar`）——
 * 两者都不是"游戏版本 + 构建号"两个字符串拼得出来的，所以计划里 `downloads` 是**空的**，
 * 得先把那份元数据取回来。**取哪一份**（`needFor`）和**取回来怎么读**（`fillDownloads`）
 * 是两件纯事，在这一层。
 * ② 「跑它那个工具的那条命令行长什么样」「这一件事在任务中心里该分成几步」——也都是纯的。
 *
 * 【这一层里没有 QObject、没有网络、没有进程、不看时钟】
 *
 * 全是有进有出的纯函数：喂一份 Plan 和一份字节进来，回一个结论出去。所以 `verify` 能直接钉
 * （见 tests 的 **ZY** 一节）——不用真联网、不用真起 java。真的下、真的起进程、真的收尾留给
 * `ServerInstallController`（那一个才带 QObject / QNetwork / QProcess）。
 *
 * 【为什么"哪一档这一轮不做"也记在这儿】
 *
 * 用户定的：Spigot 那一档**界面上照旧列出来**（用户得知道有这一档），但选中它时
 * 明说这一轮还没接、并把「创建」收起来 —— **不留一颗按下去什么都不发生的按钮**。
 * 这个判据只有一个出处，就在下面的 `flavorSupported()` / `unsupportedReason()`。
 */

#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "download/ServerInstall.hpp"

namespace ServerInstallRun {

// ---------------------------------------------------------------------------
// 这一档这一轮支不支持
// ---------------------------------------------------------------------------

/**
 * @brief 这一档的安装（从"点创建"到"目录里真的站着一个能启动的服务端"）接好了没有。
 *
 * **唯一出处**：`NewServerPage` 那颗「创建」的禁用、摘要里那句"暂时不支持"、以及驱动层
 * 入口那道闸，三处读的都是这里。
 *
 * Spigot 为什么不做：官方**不发成品**，得下它的 BuildTools 现编（要几分钟、要联网、而且要
 * 用户本机装着 git）。这条路单独一档的量比另外七档加起来还大，而它失败的姿势（"本机没有
 * git"）又不是我们能修的那种。用户 选的是"列出但明说暂不支持"。
 */
inline bool flavorSupported(ServerInstall::Flavor flavor) {
    return flavor != ServerInstall::Flavor::Spigot;
}

/// 为什么没接。**空 = 接好了**（给用户看的那句话，界面照抄，别自己另写）。
inline QString unsupportedReason(ServerInstall::Flavor flavor) {
    if (flavor == ServerInstall::Flavor::Spigot) {
        return QCoreApplication::translate("ServerInstallRun", "Spigot 官方提供 BuildTools，需联网并使用本机的 Git 构建。当前版本暂不支持此安装方式，请选择其他服务端类型。");
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 还差哪一份元数据
// ---------------------------------------------------------------------------

/// 计划里那两处"地址还取不回来"的口子，各要一份什么。
enum class Metadata {
    None,         ///< 计划里已经齐了，一个请求都不用发
    VersionJson,  ///< 原版：地址在版本 json 的 `downloads.server` 里
    PaperBuilds,  ///< Paper：地址里的对象名带 sha256，得先取构建列表
};

/**
 * @brief 要取的那一份元数据。
 *
 * NOTE: `VersionJson` 特意**不给 url** —— 那一份**先看本地**（`versions/<版本>/<版本>.json`，
 * 装服务端的人多半已经把客户端那个版本装过了，那份 json 里就有 `downloads.server`），
 * 本地没有才去问版本清单。所以"去哪儿取"是驱动层的事（它手上有版本清单 / 版本管理器），
 * 这一层只负责说"还差这一份"。
 */
struct Need {
    Metadata kind = Metadata::None;
    QString url;    ///< 要发出去的那条地址。`VersionJson` 是空的（先看本地）
    QString label;  ///< 任务中心里那一步叫什么

    bool ok() const { return kind != Metadata::None; }
};

/// 这一档还差哪一份元数据。
inline Need needFor(const ServerInstall::Request &request, const ServerInstall::Plan &plan) {
    Need need;
    if (!plan.ok) {
        return need;
    }
    switch (request.flavor) {
    case ServerInstall::Flavor::Vanilla:
        // 「先看本地、本地没有再去要」这一句就是这一档的 label。
        need.kind = Metadata::VersionJson;
        need.label = QCoreApplication::translate("ServerInstallRun", "取原版版本信息");
        return need;
    case ServerInstall::Flavor::Paper:
        need.kind = Metadata::PaperBuilds;
        need.url = ServerInstall::paperBuildsUrl(request.gameVersion);
        need.label = QCoreApplication::translate("ServerInstallRun", "取 Paper 构建列表");
        return need;
    default:
        // 其余五档的地址在 `planFor()` 里就拼好了（Purpur / Fabric / Forge / NeoForge / Quilt），
        // Spigot 那一档根本走不到这儿（`flavorSupported()` 就把入口挡了）。
        return need;
    }
}

// ---------------------------------------------------------------------------
// 取回来的那份怎么读
// ---------------------------------------------------------------------------

/**
 * @brief `fillDownloads()` 的结论。
 *
 * NOTE: `plan` 只在 `ok == true` 时是完整的（和 `ServerInstall::Plan` 那条一样：不行的时候
 * 只有 `problem` 有意义）。
 */
struct Fill {
    bool ok = false;
    QString problem;                  ///< 给用户看的那句话
    ServerInstall::Plan plan;         ///< 填好 downloads 的那一份计划
    QString detail;                   ///< 取元数据那一步收尾时写在那一行上的小字（可空）
};

/**
 * @brief 把取回来的那份元数据填成**具体的下载**。
 *
 * @param payload `Need::kind` 对应的字节。`Metadata::None` 时**不读它**（传空的就行）。
 *
 * 判据一律用 `ServerInstall` 自己那一套解析（`vanillaServerDownload` / `paperBuildsFromJson`
 * / `pickPaperBuild`），这一层一个字段都不自己抠 —— 上游改了哪里、退回哪一条，两边必须是
 * 同一个说法，否则会出现"页面上写着构建 #232、真下的却是别的那一条"。
 */
inline Fill fillDownloads(const ServerInstall::Request &request, const ServerInstall::Plan &plan,
                          const QByteArray &payload) {
    Fill fill;
    fill.plan = plan;

    switch (request.flavor) {
    case ServerInstall::Flavor::Vanilla: {
        // 本地那份版本 json 和网上取回来的那份，到这儿是**同一个入参**（都是那份 json 的字节）。
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fill.problem = QCoreApplication::translate("ServerInstallRun", "拿到的原版版本信息无法解析（不是一份 JSON）。");
            return fill;
        }
        const ServerInstall::Download jar = ServerInstall::vanillaServerDownload(document.object());
        if (!jar.ok()) {
            // 远古版本（1.2.5 之前）官方就没发过服务端 jar —— 这不是"下载失败"，是"这一档
            // 在这个版本上不存在"，所以这句得说清楚，别让用户以为是网络的事。
            fill.problem = QCoreApplication::translate("ServerInstallRun", "官方没给这个版本发过服务端（原版服务端从 1.2.5 才有）。换一个版本，或者挑 Paper / Fabric 那几档。");
            return fill;
        }
        fill.plan.downloads = QList<ServerInstall::Download>{jar};
        fill.ok = true;
        return fill;
    }
    case ServerInstall::Flavor::Paper: {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()) {
            fill.problem = QCoreApplication::translate("ServerInstallRun", "拿到的 Paper 构建列表无法解析（不是一份 JSON 数组）。");
            return fill;
        }
        const QList<ServerInstall::Build> builds =
            ServerInstall::paperBuildsFromJson(document.array());
        if (builds.isEmpty()) {
            fill.problem = QCoreApplication::translate("ServerInstallRun", "Paper 没给这个游戏版本发过构建。换一个版本试试。");
            return fill;
        }
        // 稳定档优先，但没有稳定档时退回最新那条（见 `pickPaperBuild`）—— 退回这件事要
        // **说出来**，否则用户会以为手上这个是稳定版。
        const ServerInstall::Build build = ServerInstall::pickPaperBuild(builds, true);
        ServerInstall::Download jar;
        jar.fileName = QStringLiteral("server.jar");
        jar.url = build.url;
        jar.sha256 = build.sha256;  // Paper 只给 sha256（没有 sha1）
        jar.size = build.size;
        fill.plan.downloads = QList<ServerInstall::Download>{jar};
        fill.ok = true;
        fill.detail = ServerInstall::paperBuildIsStable(build)
                          ? QCoreApplication::translate("ServerInstallRun", "构建 #%1（稳定档）").arg(build.number)
                          : QCoreApplication::translate("ServerInstallRun", "构建 #%1（上游没标稳定档，这就是最新那条）")
                                .arg(build.number);
        return fill;
    }
    default:
        // 其余那几档地址已经拼好了，一个字节都不用读。
        fill.ok = true;
        return fill;
    }
}

/**
 * @brief 跑安装器时那一条命令行（**不含 java 可执行文件本身**）。
 *
 * `ServerInstall::Plan::toolArguments` 明说了不含 `-jar`、不含工具 jar 的路径（"由驱动层
 * 补上"）—— 这个函数就是那一处补的地方，顺序是 `-jar <jar> <args…>`。
 */
inline QStringList toolCommand(const ServerInstall::Plan &plan, const QString &toolJarPath) {
    if (toolJarPath.isEmpty()) {
        return QStringList();
    }
    QStringList command{QStringLiteral("-jar"), toolJarPath};
    command.append(plan.toolArguments);
    return command;
}

/// 跑安装器时那个 jar 叫什么（相对服务端目录）。空 = 这一档不跑安装器。
///
/// 判据是"计划里有没有给工具的参数"而不是"是哪一档"：`planFor()` 只给要跑工具的那三档
/// （Forge / NeoForge / Quilt / Spigot）填 `toolArguments`，所以这一条和它是同一件事，
/// 但它**不看 flavor**——以后哪一档换了装法，这里跟着走，不用改。
inline QString installerJarName(const ServerInstall::Plan &plan) {
    if (plan.toolArguments.isEmpty() || plan.downloads.isEmpty()) {
        return QString();
    }
    // 工具 jar 就是计划里下下来的**第一份**（`planFor()` 那几档的 downloads 都只有它一个）。
    return plan.downloads.first().fileName;
}

// ---------------------------------------------------------------------------
// 这一件事在任务中心里分成几步
// ---------------------------------------------------------------------------

/**
 * @brief 任务中心那几步，按做的先后。
 *
 * 分成"取要下什么 / 逐个下载 / 跑安装器 / 核对装好了没有"这四类。**不给百分比**：一共要下
 * 多少字节得等元数据取回来才知道，而在那之前报任何百分数都是编的（和 `TaskSnapshot::percentOf`
 * 里"没有总量就不显示百分比"同一条规矩）。
 *
 * NOTE: 「核对装好了没有」那一步**不省**：它是"失败之后不留一个被当成成品的半成品"这句话的
 * 落点 —— 判据是 `ServerModel::isInstalled()`（计划里那几条相对路径都在不在），
 * 不是"最后那个进程退出码是 0"。
 */
inline QStringList stepNames(const ServerInstall::Request &request,
                             const ServerInstall::Plan &plan) {
    QStringList steps;
    const Need need = needFor(request, plan);
    if (need.ok()) {
        steps.append(need.label);
    }
    for (const ServerInstall::Download &download : plan.downloads) {
        steps.append(QCoreApplication::translate("ServerInstallRun", "下载 %1").arg(download.fileName));
    }
    if (!installerJarName(plan).isEmpty()) {
        steps.append(QCoreApplication::translate("ServerInstallRun", "跑安装器"));
    }
    steps.append(QCoreApplication::translate("ServerInstallRun", "核对安装结果"));
    return steps;
}

}  // namespace ServerInstallRun
