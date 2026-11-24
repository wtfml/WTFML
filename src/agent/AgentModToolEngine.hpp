/**
 * @file AgentModToolEngine.hpp
 * @brief S5.4 的四个工具：`mods.analyzeCompatibility`、`mods.verifyFiles`、
 *        `mods.installFix`、`instance.repairCore`。
 *
 * 分工：参数/审批/证据/异步交付在这一层；纯判断在 `AgentModCompat`，本地声明解析在
 * `ModDeclarations`；联网、下载、核心文件修复经 `AgentModHost`（controllers 层实现）。
 *
 * 平台查询（把 SHA-1 发给 Modrinth）**必须**先拿到确认：非严格模式下同一实例只确认一次，
 * 严格模式下每次；`offline=true` 完全不发。`mods.installFix` 只在活动事务的工作区里写文件，
 * 没有事务就拒绝，原实例一个字节不改。
 */

#pragma once

#include "AgentLaunchHost.hpp"
#include "AgentLauncherHost.hpp"
#include "AgentModBisect.hpp"
#include "AgentModHost.hpp"
#include "AgentTool.hpp"
#include "AgentWorkspaceStore.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class AgentModToolEngine : public AgentToolEngine {
public:
    using ContextProvider = std::function<AgentToolContext(QString *problem)>;

    /// @param dispatcher 回到主线程用的上下文对象（`AgentController`）；测试可传 `nullptr`，
    ///        那时回调在调用线程上直接执行。
    AgentModToolEngine(AgentLauncherHost *launcherHost, AgentModHost *modHost,
                       ContextProvider provider, QObject *dispatcher = nullptr,
                       AgentLaunchHost *launchHost = nullptr);

    static QList<AgentToolSpec> specs();

    QJsonArray schemas() const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    void cancelPending() override;
    bool handles(const QString &internalName) const override;
    void setCompletionSink(AgentToolSink sink) override { m_sink = std::move(sink); }

    void setHosts(AgentLauncherHost *launcherHost, AgentModHost *modHost,
                  AgentLaunchHost *launchHost = nullptr) {
        m_launcherHost = launcherHost;
        m_modHost = modHost;
        m_launchHost = launchHost;
    }

    /// 本地扫描结果：测试直接喂临时 `mods/` 目录。
    struct LocalScan {
        QList<AgentModCompat::InstalledFile> files;
        AgentModCompat::Analysis analysis;
        QString problem;
        int hashed = 0;
        bool truncated = false;
    };
    /// @param hashBudgetMs 算 SHA-1 的时间上限；超过后剩下的文件不带哈希（平台查询跳过它们）。
    static LocalScan scanMods(const QString &modsDir, const QList<AgentHostModFile> &mods,
                              const AgentModCompat::Target &target, int hashBudgetMs = 60000);

    /// 平台查询一次最多多少个哈希（和 `Modrinth` 的分批上限一致）。
    static int platformHashLimit();

private:
    enum class Job { Analyze, Verify, Install, RepairCore, Bisect };

    struct Pending {
        Job job = Job::Analyze;
        Agent::ToolCall call;
        AgentToolContext context;
        AgentModCompat::Target target;
        QStringList sha1s;
        bool platform = false;
        bool installDependencies = true;
        AgentWorkspaceRecord workspace;
        QString transactionId;
        QList<AgentHostModFile> mods;
        QString modsDir;
        bool modsTruncated = false;
        QString projectId;
        QString versionId;
        QString expectedSha1;
        QStringList flaggedPaths;
        // mods.bisect
        QStringList prioritizedCandidates;
        int maxLaunches = 8;
        int repeatPerCandidate = 2;
        int timeoutSeconds = 120;
        bool respectDependencies = true;
    };

    /// 二分的现场：工作区路径、快照、初始启停状态与每一步的证据。
    struct BisectState {
        QString versionDir;
        QString modsDir;
        QString configDir;
        QString optionsPath;
        QString stagingDir;
        bool optionsExisted = false;
        QHash<QString, bool> initialEnabled;  ///< 文件基名 → 初始是否启用
        QStringList installedBaseNames;
        QElapsedTimer timer;
        QJsonArray evidence;
        int launchIndex = 0;
        QString problem;
    };

    /// 一次已开始的作业（同时最多一个）。
    struct Run {
        Pending pending;
        QString stagingDir;
        QList<AgentHostModTarget> targets;   ///< 根模组 + 前置
        QList<QPair<AgentHostModTarget, QString>> staged;  ///< 已下载并校验过的目标与暂存文件
        QStringList copied;                  ///< 已写进工作区的相对文件名
        int resolveIndex = 0;
        int downloadIndex = 0;
        bool platformComplete = false;
        QString platformProblem;
        int generation = 0;
    };

    AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message) const;
    AgentToolResult approvalRequired(const Pending &pending, const QString &displayName,
                                     const QString &summary, const QString &preview,
                                     const QString &backupHint, const QJsonObject &egress);
    QJsonObject externalSendEgress(const QString &reason, int fileCount) const;
    bool needsPlatformApproval(const Pending &pending) const;

    AgentToolResult startAnalyzeOrVerify(const Pending &pending);
    void startInstall(const Pending &pending);
    void startRepairCore(const Pending &pending);
    void startBisect(const Pending &pending);
    void bisectNext();
    void onBisectLaunchDone(const AgentTestLaunchSummary &summary, const QString &problem);
    void finishBisect();
    bool applyBisectEnabledSet(const QStringList &enabledFiles);
    bool resetBisectWorkspace();
    void restoreBisectBaseline();
    void clearBisectLogs();

    void beginPlatformLookup(Run run, const LocalScan &scan);
    void finishAnalyze(Run run, const LocalScan &scan,
                       const QList<AgentModCompat::PlatformEntry> &entries, bool platformComplete,
                       const QString &platformProblem);
    void finishVerify(Run run, const LocalScan &scan,
                      const QList<AgentModCompat::PlatformEntry> &entries, bool platformComplete,
                      const QString &platformProblem);

    void resolveNext(Run run);
    void onResolved(Run run, const AgentHostModTarget &target, const QString &problem);
    void downloadNext(Run run);
    void onDownloaded(Run run, const AgentModHost::DownloadOutcome &outcome);
    void copyStagedIntoWorkspace(Run run);

    void deliver(const Agent::ToolCall &call, const AgentToolResult &result);
    void finishWithError(Run run, Agent::ErrorKind kind, const QString &message);
    void finishWithData(Run run, const QJsonObject &data, const QString &summary);
    void post(const std::function<void()> &work);
    bool runAlive(const Run &run) const;
    void cleanupStaging(const QString &stagingDir);
    QString workspaceModsDir(const AgentWorkspaceRecord &workspace) const;

    AgentLauncherHost *m_launcherHost = nullptr;
    AgentModHost *m_modHost = nullptr;
    AgentLaunchHost *m_launchHost = nullptr;
    std::unique_ptr<AgentModBisect::Bisect> m_bisect;
    BisectState m_bisectState;
    ContextProvider m_provider;
    QPointer<QObject> m_dispatcher;
    AgentToolSink m_sink;
    QHash<QString, Pending> m_pending;
    QSet<QString> m_platformApproved;  ///< 非严格模式：已经确认过"把哈希发给 Modrinth"的实例
    bool m_active = false;
    Run m_run;
    int m_generation = 0;
};
