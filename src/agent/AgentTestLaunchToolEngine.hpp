/**
 * @file AgentTestLaunchToolEngine.hpp
 * @brief `instance.testLaunch`：审批后在隔离工作区启动一次游戏，等里程碑/崩溃/超时。
 *
 * 工具只负责参数校验、审批和异步交付；建工作区、起进程、读日志、杀进程树在
 * `AgentLaunchHost` 的实现里（controllers 层）。每次独立调用都审批；计划批准不等于
 * 批准这一次启动。
 */

#pragma once

#include "AgentLaunchHost.hpp"
#include "AgentTool.hpp"

#include <QHash>
#include <QJsonObject>
#include <QString>

class AgentTestLaunchToolEngine : public AgentToolEngine {
public:
    using ContextProvider = std::function<AgentToolContext(QString *problem)>;

    AgentTestLaunchToolEngine(AgentLaunchHost *host, ContextProvider provider);

    static QList<AgentToolSpec> specs();
    static bool parseOptions(const QJsonObject &arguments, AgentTestLaunchOptions *options,
                             QString *problem);
    static QJsonObject summaryJson(const AgentTestLaunchSummary &summary);

    QJsonArray schemas() const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    void cancelPending() override;
    bool handles(const QString &internalName) const override;
    void setCompletionSink(AgentToolSink sink) override { m_sink = std::move(sink); }
    void setHost(AgentLaunchHost *host) { m_host = host; }

private:
    struct Pending {
        Agent::ToolCall call;
        AgentToolContext context;
        AgentTestLaunchOptions options;
        QString workspaceId;
    };

    void startApproved(const Pending &pending);
    AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message) const;

    AgentLaunchHost *m_host = nullptr;
    ContextProvider m_provider;
    AgentToolSink m_sink;
    QHash<QString, Pending> m_pending;
    Agent::ToolCall m_activeCall;
    bool m_active = false;
};
