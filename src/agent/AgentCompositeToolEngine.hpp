/**
 * @file AgentCompositeToolEngine.hpp
 * @brief 把文件工具、启动器工具、工作流工具（以及后续 MCP）合成一个给 `AgentLoop` 用的引擎。
 *
 * 分派按内部工具名走 `handles()`，不试错调用；审批请求记住是哪个子引擎发出来的，决定时
 * 只交回给那一个。技能白名单与“写步骤必须在已批准计划里”这两道闸也在这里——子引擎不知道
 * 当前是哪个模式，也不该知道。
 */

#pragma once

#include "AgentAutoSafe.hpp"
#include "AgentTool.hpp"
#include "AgentWorkflow.hpp"

#include <QHash>
#include <QList>
#include <QString>

class AgentCompositeToolEngine : public AgentToolEngine {
public:
    AgentCompositeToolEngine() = default;

    void addEngine(AgentToolEngine *engine);
    QList<AgentToolEngine *> engines() const { return m_engines; }
    void setWorkflow(AgentWorkflow *workflow) { m_workflow = workflow; }
    /// Auto-safe 事务授权（D12）：授权范围内的写/启动工具不再逐次弹审批。
    /// 每次工具调用前会拿这三个值调一次 `Authorizer::revalidate()` —— 会话/实例被换掉就
    /// 当场关掉，不会拿着旧授权继续跑。
    void setAutoSafe(AgentAutoSafe::Authorizer *authorizer) { m_autoSafe = authorizer; }
    void setAutoSafeContext(const QString &sessionId, const QString &instanceId) {
        m_autoSafeSessionId = sessionId;
        m_autoSafeInstanceId = instanceId;
    }

    QJsonArray schemas() const override;
    bool allowsResultReuse(const Agent::ToolCall &call) const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    AgentToolResult resolveSessionApproval(const QString &approvalId) override;
    void cancelPending() override;
    bool committing() const override;
    bool handles(const QString &internalName) const override;
    /// 异步子引擎（MCP）的结果先记进工作流账，再交给 Loop。
    void setCompletionSink(AgentToolSink sink) override;

    /// 当前错误/拒绝对话用的一句人话（给 UI 的提示用）。
    static QString riskBlockedMessage(const QString &skillName, const QString &toolName);

private:
    AgentToolEngine *findEngine(const QString &internalName) const;
    /// 技能白名单 + 只读 + 已批准计划三道闸；空串表示放行。
    QString gate(const QString &internalName) const;

    /// ApprovalRequired 时按 Auto-safe 判定：放行就替用户点"允许"，否则原样交回 UI。
    AgentToolResult applyAutoSafe(const Agent::ToolCall &call, const AgentToolResult &result,
                                  AgentToolEngine *engine);
    AgentToolResult resolveApprovalInternal(const QString &approvalId, bool approved,
                                            bool sessionScope);
    /// 记审计需要的节点：repair.begin 绑事务、repair.verify 记验证、repair.finish 收尾写审计。
    void observeResult(const Agent::ToolCall &call, AgentToolResult *result);

    QList<AgentToolEngine *> m_engines;
    AgentWorkflow *m_workflow = nullptr;
    AgentAutoSafe::Authorizer *m_autoSafe = nullptr;
    QString m_autoSafeSessionId;
    QString m_autoSafeInstanceId;
    QHash<QString, AgentToolEngine *> m_approvalOwner;
    QHash<QString, Agent::ToolCall> m_approvalCalls;
    AgentToolSink m_sink;
};
