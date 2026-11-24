/**
 * @file AgentModHost.hpp
 * @brief 模组兼容与文件修复的接入口（docs/dev/AGENT_AUTONOMY.md S5.4）。
 *
 * 平台查询、版本地址解析、下载和核心文件修复都要么联网、要么动业务管理器，这些都在
 * controllers 层实现；`src/agent/` 只认这张纯虚表，于是 `verify_agent` 能用假宿主逐格验
 * 工具行为（审批、参数、失败回滚），不需要网络或真实例。
 *
 * 约束：
 * - 平台查询只接 Modrinth；发给它的是 SHA-1，不发文件名；调用方负责在发之前拿到确认。
 * - 回调在实现方的线程上回来（产品实现回到主线程）；引擎不阻塞 UI、不开嵌套事件循环。
 * - 下载只落在调用方给的暂存目录；写工作区是引擎的事，宿主不碰原实例。
 */

#pragma once

#include "AgentModCompat.hpp"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

/// 一份模组文件的本地位置（`logs` 那套一样：绝对路径只在本地用，不进模型输出）。
struct AgentHostModFile {
    QString fileName;
    QString path;
    bool enabled = true;
    qint64 bytes = 0;
};

/// 解出来的下载目标：地址、大小、哈希都是平台记录里的那一份。
struct AgentHostModTarget {
    QString projectId;
    QString versionId;
    QString versionNumber;
    QString fileName;
    QString url;
    QString sha1;
    qint64 size = -1;
    QStringList requiredProjectIds;
};

class AgentModHost {
public:
    virtual ~AgentModHost() = default;

    /// 按 SHA-1 批量问 Modrinth（每批 ≤100），回每个哈希的身份与版本元数据。
    using PlatformCompletion =
        std::function<void(const QList<AgentModCompat::PlatformEntry> &entries,
                           const QString &problem)>;
    virtual bool lookupModPlatform(const QString &instanceId, const QString &gameVersion,
                                   const QString &loaderVersion, const QString &loader,
                                   const QStringList &sha1s, const PlatformCompletion &done,
                                   QString *problem) = 0;

    /// 按项目号/版本号找下载目标（本地解析出地址、大小、哈希与必需前置）。
    using ResolveCompletion =
        std::function<void(const AgentHostModTarget &target, const QString &problem)>;
    virtual bool resolveModTarget(const QString &instanceId, const QString &projectId,
                                  const QString &versionId, const QString &expectedSha1,
                                  const ResolveCompletion &done, QString *problem) = 0;

    struct DownloadOutcome {
        bool ok = false;
        QString stagedPath;
        QString sha1;
        qint64 bytes = 0;
        QString problem;
    };
    using DownloadCompletion = std::function<void(const DownloadOutcome &outcome)>;
    /// 下载到 @p stagingDir 下，**按目标的大小和 SHA-1 校验**后才回 ok。
    virtual bool downloadModFile(const AgentHostModTarget &target, const QString &stagingDir,
                                 const DownloadCompletion &done, QString *problem) = 0;

    struct RepairOutcome {
        bool ok = false;
        QString problem;
    };
    using RepairCompletion = std::function<void(const RepairOutcome &outcome)>;
    /// 复用 `VersionManager::repairVersionFiles()`：客户端 jar、库、资源索引。
    virtual bool repairCoreFiles(const QString &instanceId, const QStringList &flaggedPaths,
                                 const RepairCompletion &done, QString *problem) = 0;
};
