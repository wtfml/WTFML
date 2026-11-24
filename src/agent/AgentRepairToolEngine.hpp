/**
 * @file AgentRepairToolEngine.hpp
 * @brief `repair.begin/diff/verify/finish/status`：事务工具层。
 *
 * 事务日志与差异在 `AgentRepairTransaction`；建工作区/提交在 `AgentRepairHost`；
 * 验证复用 `instance.testLaunch` 的 `AgentLaunchHost`。工具只做参数校验、审批、
 * 状态检查和异步交付。
 */

#pragma once

#include "AgentLaunchHost.hpp"
#include "AgentRepairHost.hpp"
#include "AgentRepairTransaction.hpp"
#include "AgentTestLaunch.hpp"
#include "AgentTool.hpp"

#include <QHash>
#include <QJsonObject>
#include <QString>

class AgentRepairToolEngine : public AgentToolEngine {
public:
    using ContextProvider = std::function<AgentToolContext(QString *problem)>;

    AgentRepairToolEngine(AgentRepairHost *repairHost, AgentLaunchHost *launchHost,
                          ContextProvider provider);

    static QList<AgentToolSpec> specs();
    QJsonArray schemas() const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    void cancelPending() override;
    bool handles(const QString &internalName) const override;
    void setCompletionSink(AgentToolSink sink) override { m_sink = std::move(sink); }
    void setHosts(AgentRepairHost *repairHost, AgentLaunchHost *launchHost);

private:
    struct Pending {
        enum class Kind { Begin, FinishCommit };
        Kind kind = Kind::Begin;
        Agent::ToolCall call;
        AgentToolContext context;
        QStringList scopes;
        QString transactionId;
        QString displayName;
        QString summary;
        QString preview;
    };

    AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message) const;
    AgentToolResult handleBegin(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleDiff(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleVerify(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleFinish(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleStatus(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult approvalRequired(const Pending &pending);
    void finishVerification(const Agent::ToolCall &call, const AgentTestLaunchSummary &summary);
    void deliver(const Agent::ToolCall &call, const AgentToolResult &result);

    AgentRepairHost *m_repairHost = nullptr;
    AgentLaunchHost *m_launchHost = nullptr;
    ContextProvider m_provider;
    AgentToolSink m_sink;
    AgentRepairTransactionStore m_store;
    QHash<QString, Pending> m_pending;
    bool m_active = false;
    Agent::ToolCall m_activeCall;
};
