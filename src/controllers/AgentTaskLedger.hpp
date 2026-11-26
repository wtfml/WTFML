/**
 * @file AgentTaskLedger.hpp
 * @brief 智能体操作进入任务中心（#130）：把会改实例、要等确认或耗时的工具调用记成任务。
 *
 * 只认三样东西：任务账本（`Tasks::TaskManager`）、智能体循环的信号、工作流的状态。
 * 每次事件都从工作流和循环的现状重新推一遍，不另存一份会和它们走散的状态机。
 *
 * 口径（docs/features/AGENT.md「任务中心」一节）：
 * - 只读工具不记；校验/分析/备份/下载/测试启动这类耗时工具开始就记一项；
 * - 快写类（`fs.write`、`mods.setEnabled`、`settings.set`）只有真的弹了确认才记；
 * - 已批准计划里的每一步（含 Auto-safe 自动放行的）不另记，算计划任务的一步；
 * - 计划任务在 `workflow.submitPlan` 获批时建，`workflow.finish` 成功、工作流失败、
 *   停止/离页、计划作废时收尾。
 * - 「查看会话」只在本次运行内有效：任务号 → 会话号的表只在内存里。
 */

#pragma once

#include "agent/AgentLoop.hpp"
#include "tasks/TaskSnapshot.hpp"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class AgentWorkflow;
namespace Tasks { class TaskManager; }

class AgentTaskLedger : public QObject {
    Q_OBJECT

public:
    struct Context {
        QString sessionId;
        QString instanceId;
        QString instanceName;
    };

    explicit AgentTaskLedger(AgentWorkflow *workflow, QObject *parent = nullptr);

    /// 账本晚于控制器建（MainWindow 里任务中心后建）；没接上之前什么都不记。
    void setTaskManager(Tasks::TaskManager *tasks) { m_tasks = tasks; }
    void setContextProvider(std::function<Context()> provider) { m_context = std::move(provider); }
    void attach(AgentLoop *loop);

    // ---- 事件（attach() 接到循环信号上；测试直接调） ----
    void onToolStarted(const Agent::ToolCall &call);
    void onApprovalRequested(const Agent::ToolCall &call);
    /// 控制器在把用户的决定交给循环**之前**调：拒绝要和「执行失败」分开说。
    void onApprovalResolved(bool approved);
    void onToolFinished(const Agent::ToolCall &call, const AgentToolResult &result);
    void onLoopState(AgentLoop::State state);
    void onTurnFailed(const Agent::Error &error);
    /// 换会话 / 换技能 / 换实例后调：工作流的计划没了，计划任务作废。
    void onWorkflowReset();

    // ---- 任务中心 ----
    bool owns(Tasks::TaskId id) const { return m_sessions.contains(id); }
    bool isActive(Tasks::TaskId id) const;
    bool isPlanTask(Tasks::TaskId id) const { return id != Tasks::invalidTaskId() && id == m_planTask; }
    /// 「取消」此刻点得动吗（`repair.finish` 写回实例文件那一段点不动）。
    bool canCancel(Tasks::TaskId id) const;
    /// 收掉这一项（计划任务另由调用方把工作流标失败、停掉循环）。不回滚未提交的修复事务。
    bool cancelFromTaskCenter(Tasks::TaskId id);
    QString sessionFor(Tasks::TaskId id) const { return m_sessions.value(id); }
    Tasks::TaskId planTask() const { return m_planTask; }
    Tasks::TaskId standaloneTask() const { return m_standalone; }

private:
    enum class Policy { None, Long, QuickWrite, PlanOnly };
    static Policy policyFor(const QString &tool);
    bool isPlanStep(const QString &tool) const;
    Tasks::TaskKind kindFor(const QString &tool) const;
    QString titleFor(const Agent::ToolCall &call, const QString &tool) const;
    Tasks::TaskId beginTask(Tasks::TaskKind kind, const QString &title, const QList<Tasks::TaskStep> &steps);
    void ensurePlanTask();
    void syncPlan();
    void setWaiting(Tasks::TaskId id, bool waiting, const QString &detail);
    void setCancelBlocked(Tasks::TaskId id, const QString &reason);
    void cancelWith(Tasks::TaskId *id, const QString &detail);
    Context context() const { return m_context ? m_context() : Context{}; }

    AgentWorkflow *m_workflow = nullptr;
    Tasks::TaskManager *m_tasks = nullptr;
    std::function<Context()> m_context;
    QHash<Tasks::TaskId, QString> m_sessions;
    Tasks::TaskId m_planTask = Tasks::invalidTaskId();
    Tasks::TaskId m_standalone = Tasks::invalidTaskId();
    QString m_standaloneCall;      ///< 独立任务对应的工具调用 id
    bool m_awaitingApproval = false;
    bool m_rejected = false;       ///< 刚拒绝：随后那条失败结果不再改判
    bool m_committing = false;     ///< repair.finish 正在写回
};
