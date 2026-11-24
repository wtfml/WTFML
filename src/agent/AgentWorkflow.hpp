/**
 * @file AgentWorkflow.hpp
 * @brief Stage 2 工作流状态机：Collect → Analyze → Plan → AwaitPlanApproval → Execute*
 * → Verify → Report（docs/features/AGENT.md §11.3）。
 *
 * 这一层只记事实：当前技能、提交的计划、每步结果、证据 id。工具分派与审批在
 * `AgentCompositeToolEngine`/`AgentWorkflowEngine`；提示词在 `AgentSkill`。
 *
 * 硬约束：计划必须由结构化工具提交并本地校验（自然语言计划不算）；写类工具只能在已批准
 * 的计划里执行；验证失败或用户拒绝某一步，阶段直接转 `Failed`，`workflow.finish` 会拒绝。
 */

#pragma once

#include "AgentSkill.hpp"
#include "AgentTool.hpp"

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

struct AgentPlanStep {
    QString tool;          ///< 工具内部名
    QJsonObject arguments;
    QString reason;
    bool verify = false;   ///< 验证步：必须是只读工具，执行后检查关键项是否恢复

    QJsonObject toJson() const;
};

enum class AgentWorkflowStage { Collect, AwaitPlanApproval, Execute, Verify, Report, Failed };

class AgentWorkflow {
public:
    void setSkill(const AgentSkill &skill);
    AgentSkill skill() const { return m_skill; }
    /// 换了技能/实例：旧计划作废，证据保留（旧工具结果仍是事实）。
    void reset(const QString &reason = QString());

    AgentWorkflowStage stage() const { return m_stage; }
    QString stageName() const;
    bool active() const { return m_skill.id != QStringLiteral("general"); }
    bool readOnly() const { return m_skill.readOnly; }
    bool requiresPlan() const { return m_skill.requiresPlan; }

    /// 提交计划：字段/工具/风险/步数全部本地校验，未通过不改状态。
    bool submitPlan(const QString &goal, const QList<AgentPlanStep> &steps, QString *problem);
    QString planGoal() const { return m_goal; }
    QList<AgentPlanStep> planSteps() const { return m_steps; }
    QJsonObject planJson() const;
    QString planPreview() const;
    /// 用户对计划卡的决定。拒绝 = 计划作废、阶段回到 Collect。
    void approvePlan(bool approved);

    bool planApproved() const { return m_planApproved; }
    /// Execute 阶段：这个写类工具是不是**当前这一**步（计划按顺序执行，不能跳步）。
    bool stepAllows(const QString &tool) const;
    /// 下一步在计划里的下标（== 已执行步数）。
    int nextStepIndex() const;
    void recordResult(const Agent::ToolCall &call, const AgentToolResult &result);

    /// 完成闸：缺哪一条就把原因写进 @p problem。
    bool canFinish(QString *problem) const;
    void markFinished();
    void markFailed(const QString &reason);

    QStringList evidenceIds() const { return m_evidenceIds; }
    QJsonObject statusJson() const;
    QString statusLine() const;

private:
    bool isRepairStep(const AgentPlanStep &step) const;
    bool isVerifyStep(const AgentPlanStep &step) const;

    AgentSkill m_skill;
    AgentWorkflowStage m_stage = AgentWorkflowStage::Collect;
    QString m_goal;
    QList<AgentPlanStep> m_steps;
    bool m_planApproved = false;
    QStringList m_evidenceIds;
    QSet<QString> m_touchedTools;
    int m_executedSteps = 0;
    int m_failedSteps = 0;
    QString m_failureReason;
    bool m_verified = false;
};
