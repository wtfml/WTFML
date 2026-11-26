/**
 * @file AgentController.hpp
 * @brief 智能体 UI 与核心层的唯一接线点：会话、API 配置、流式聊天、停止/重试、加密、实例关联、导出。
 *
 * 文件工具只作用于本会话锁定的实例（Stage 1B，写入逐次确认）；不启动 MCP。所有网络只走 `AgentModelClient`；
 * 所有凭据只走 `AgentSecretStore`；所有会话落盘只走 `AgentSessionStore`。
 */

#pragma once

#include "agent/AgentArtifactStore.hpp"
#include "agent/AgentBackupIndex.hpp"
#include "agent/AgentCompositeToolEngine.hpp"
#include "agent/AgentFileToolEngine.hpp"
#include "agent/AgentLauncherHost.hpp"
#include "agent/AgentLauncherTools.hpp"
#include "agent/AgentMcpToolEngine.hpp"
#include "agent/AgentWorkflow.hpp"
#include "agent/AgentWorkflowEngine.hpp"
#include "agent/AgentCommandTools.hpp"
#include "agent/AgentWebTools.hpp"
#include "agent/McpManager.hpp"
#include "agent/McpTrustStore.hpp"
#include "agent/QtAgentWebTransport.hpp"
#include "agent/AgentFileTools.hpp"
#include "agent/AgentInstanceLock.hpp"
#include "agent/AgentLoop.hpp"
#include "agent/AgentSessionStore.hpp"
#include "agent/AgentSettings.hpp"
#include "agent/AgentTypes.hpp"
#include "controllers/AgentTaskLedger.hpp"

#include "AgentViewTypes.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class AgentMcpPage;
class AgentLaunchHost;
class AgentTestLaunchToolEngine;
class AgentRepairToolEngine;
class AgentModToolEngine;
class AgentPage;
class AgentSettingsPage;
class AgentModelClient;
class OverlayHost;
class QTimer;

class AgentController : public QObject {
    Q_OBJECT

public:
    AgentController(AgentPage *page, OverlayHost *overlay, QObject *parent = nullptr);
    ~AgentController() override;

    /// 关窗/析构：取消在飞请求、停节流保存、把会话刷盘。必须有界。
    void shutdown();

    AgentPage *page() const { return m_page; }
    AgentSessionStore *sessionStore() { return &m_store; }
    AgentLoop *loop() { return m_loop; }
    /// #130：智能体操作记进任务中心。任务中心晚于本控制器建，由 MainWindow 事后接上。
    void setTaskManager(Tasks::TaskManager *tasks) { m_taskLedger->setTaskManager(tasks); }
    AgentTaskLedger *taskLedger() { return m_taskLedger; }
    bool ownsTask(Tasks::TaskId id) const { return m_taskLedger->owns(id); }
    bool canCancelTask(Tasks::TaskId id) const { return m_taskLedger->canCancel(id); }
    /// 任务中心「取消」：收掉这一项并停下当前回复；计划任务另把工作流标为失败。
    bool cancelTask(Tasks::TaskId id);
    /// 任务中心「查看会话」：会话还在 → 打开它并回 true；已删除 → 弹「会话不存在」并回 false。
    bool openTaskSession(Tasks::TaskId id);
    AgentFileToolEngine *toolEngine() { return m_toolEngine; }
    AgentCompositeToolEngine *compositeEngine() { return m_compositeEngine; }
    AgentWorkflow *workflow() { return &m_workflow; }
    McpManager *mcpManager() { return m_mcpManager; }
    /// 打开 MCP 服务器页（API 设置里的「MCP 服务器…」和测试入口都走它）。
    void openMcpSettings();
    /// 主窗口建好的启动器能力宿主（不转移所有权）。没有宿主时启动器工具一律拒绝。
    void setLauncherHost(AgentLauncherHost *host);

    /// 主窗口提供“当前主页选中的实例 + 游戏根身份”；首次实例工具调用时锁定。
    using InstanceProvider = std::function<AgentInstanceInfo()>;
    void setInstanceContextProvider(InstanceProvider provider);
    /// 当前游戏目录下的全部实例（「实例」下拉的选项；锁定的实例是否还在也按它判）。
    using InstanceListProvider = std::function<QList<AgentInstanceInfo>()>;
    void setInstanceListProvider(InstanceListProvider provider);

    /// 测试专用：固定实例根目录与实例信息，不经过主页。
    void setInstanceContextForTest(const QString &rootPath, const AgentInstanceInfo &info);
    /// 测试专用：走审批卡
    void approveToolForTest(bool approved);
    /// 测试专用：取消在飞请求后直接删除当前会话（跳过确认弹层）。
    bool deleteSessionForTest(const QString &sessionId);

    /// 测试专用：换掉真实网络客户端（不转移所有权；调用方负责活得比控制器久）。
    void setModelClientForTest(AgentModelClient *client);
    /// 崩溃卡片上的「使用智能体询问」：另起一条会话、切到崩溃急救、锁住崩的那个实例，
    /// 把崩溃摘要带进去直接开工，最后问一句要不要打开 Auto-safe。
    /// 没配置模型时走正常的设置页提示，不在这里偷偷替用户配一个。
    /// @param instanceId 崩的那个实例（可为空：空则按主页当前实例、第一次实例工具调用时锁定）。
    bool askAboutCrash(const QString &context, const QString &instanceId = QString());

    /// 测试专用：走 askAboutCrash 的会话/模式/实例那一段，但不弹 Auto-safe 问话。
    void askAboutCrashForTest(const QString &context, const QString &instanceId) {
        m_suppressAutoSafeOffer = true;
        askAboutCrash(context, instanceId);
        m_suppressAutoSafeOffer = false;
    }

    /// 测试专用：不经过设置页直接摆一份可用的 profile/key。
    void applyProfileForTest(const AgentProviderProfile &profile, const QString &apiKey);
    void cancelForTest() { onStop(); }
    void sendForTest(const QString &text) { onSend(text); }
    /// 测试专用：直接切模式（等价于点模式卡片）。
    void chooseSkillForTest(const QString &skillId) { onSkillChosen(skillId); }
    QString currentSessionId() const { return m_session.meta.id; }

public slots:
    /// 切到别的选项卡时调用：Stage 1A 选择取消不可见的后台请求，避免悄悄花钱。
    void onPageHidden();
    /// 切回智能体页：「跟随主页」的胶囊按主页此刻的实例重算（主页实例可能在别的页换过）。
    void onPageShown();

private:
    void wirePage();
    void loadInitialSession();
    void refreshSessions();
    void selectSession(const QString &sessionId);
    void discardBlankCurrent(const QString &nextId);
    void updateHeader();
    void refreshApiState();
    void updateConfiguredState();
    void scheduleSave();
    void flushSave(bool snapshot);
    void upsertAssistant(const Agent::Message &message);
    void removeTrailingFailedAssistant();
    QString currentApiKey() const;
    QString configurationProblem() const;
    void openSettings();
    void onSettingsSave();
    void onSettingsTest();
    void onSettingsDeleteProfile();
    void onNewSession();
    /// 切到一条没内容的会话（当前这条空着就继续用它）；在飞请求先停、内容保留。
    bool ensureBlankSession();
    /// 这次崩的实例：锁成会话实例（不在当前游戏目录就不锁，留给首次实例工具调用）。
    void lockCrashInstance(const QString &instanceId);
    QString crashInstanceLabel(const QString &instanceId) const;
    /// 崩溃转发发出去之后，问一句要不要打开 Auto-safe（默认落在「逐步确认」）。
    void offerAutoSafeForCrash();
    /// 按当前模式/已锁实例打开 Auto-safe；不具备条件时复位开关并提示。
    bool enableAutoSafeForCurrentSession();
    void onSend(const QString &text);
    void onStop();
    void onRetry(const QString &messageId);
    void onRename(const QString &sessionId, const QString &title);
    void onPin(const QString &sessionId, bool pinned);
    void onDelete(const QString &sessionId);
    void onEncryptToggled(bool encrypted);
    void onInstanceMenuRequested();
    void onInstanceChosen(const QString &instanceId);
    void onExport(const QString &sessionId);
    AgentInstanceInfo homeInstance() const;
    QList<AgentInstanceInfo> instances() const;
    AgentInstanceInfo lockedInstance() const;
    void onApproval(bool approved);
    void onSessionApproval();
    void onSkillChosen(const QString &skillId);
    void onAutoSafeToggled(bool enabled);
    void disableAutoSafe(const QString &reason);
    void refreshAutoSafeUi();
    void refreshMcpPage();
    void onMcpConnect(const QString &serverId);
    void onMcpDisconnect(const QString &serverId);
    void onMcpAdd();
    void onMcpEdit(const QString &serverId);
    void onMcpRemove(const QString &serverId);
    /// 起服务器前的那一次确认：确认了就写确认记录并启动。
    bool confirmAndStart(const QString &serverId, bool reconnect);
    AgentView::McpServerDraft draftFrom(const McpServerConfig &server) const;
    McpServerConfig serverFrom(const AgentView::McpServerDraft &draft) const;
    void stopMcpServers();
    /// 把当前模式的系统提示与工具闸同步给 Loop（切模式、换会话、发消息前都走它）。
    void applySkillToLoop();
    void refreshSkillUi();
    AgentToolContext toolContext(QString *problem);
    void updateInstanceChip();
    void showToolCardsFromHistory();
    QList<AgentView::ToolCard> toolCardsFromHistory() const;

    AgentPage *m_page = nullptr;
    OverlayHost *m_overlay = nullptr;
    AgentSessionStore m_store;
    AgentArtifactStore m_artifacts;
    AgentFileTools m_files;
    AgentFileToolEngine *m_toolEngine = nullptr;
    AgentLauncherHost *m_launcherHost = nullptr;
    AgentWorkflow m_workflow;
    AgentWorkflowEngine *m_workflowEngine = nullptr;
    AgentLauncherTools *m_launcherTools = nullptr;
    AgentTestLaunchToolEngine *m_testLaunchTools = nullptr;
    AgentRepairToolEngine *m_repairTools = nullptr;
    AgentModToolEngine *m_modTools = nullptr;
    AgentAutoSafe::Authorizer m_autoSafe;
    bool m_autoSafeUserOn = false;
    AgentCompositeToolEngine *m_compositeEngine = nullptr;
    McpTrustStore m_mcpStore;
    McpManager *m_mcpManager = nullptr;
    AgentMcpToolEngine *m_mcpTools = nullptr;
    QtAgentWebTransport *m_webTransport = nullptr;
    AgentWebTools *m_webTools = nullptr;
    AgentCommandTools *m_commandTools = nullptr;
    QPointer<AgentMcpPage> m_mcpPage;
    AgentSession m_session;
    InstanceProvider m_instanceProvider;
    InstanceListProvider m_instanceListProvider;
    AgentInstanceInfo m_testInstance;
    QString m_testRootPath;
    bool m_useTestInstance = false;
    AgentProviderProfile m_profile;
    QString m_testApiKey;
    AgentModelClient *m_client = nullptr;
    AgentModelClient *m_ownedClient = nullptr;
    AgentLoop *m_loop = nullptr;
    AgentTaskLedger *m_taskLedger = nullptr;
    QPointer<AgentSettingsPage> m_settingsPage;
    QTimer *m_saveTimer = nullptr;
    bool m_configured = false;
    bool m_savePending = false;
    bool m_settingsOpenQueued = false;  // 让位后排了下一轮重开 API 设置页
    bool m_shuttingDown = false;
    QString m_autoNoticeShownFor;
    /// 测试专用：askAboutCrash 不弹 Auto-safe 问话（mw_check 在模态框里会卡住）。
    bool m_suppressAutoSafeOffer = false;
};
