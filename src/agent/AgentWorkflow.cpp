/**
 * @file AgentWorkflow.cpp
 * @brief 工作流状态机与计划校验（见 .hpp）。
 */

#include "AgentWorkflow.hpp"


#include <QJsonArray>
#include <QJsonDocument>

namespace {

constexpr int kMaxGoalLength = 500;
constexpr int kMaxReasonLength = 300;

bool isWriteRisk(AgentRisk risk) {
    return risk == AgentRisk::Write || risk == AgentRisk::Destructive ||
           risk == AgentRisk::Command || risk == AgentRisk::ExternalMcp;
}

}  // namespace

QJsonObject AgentPlanStep::toJson() const {
    return QJsonObject{{QStringLiteral("tool"), tool},
                       {QStringLiteral("arguments"), arguments},
                       {QStringLiteral("reason"), reason},
                       {QStringLiteral("verify"), verify}};
}

void AgentWorkflow::setSkill(const AgentSkill &skill) {
    if (m_skill.id == skill.id) {
        m_skill = skill;
        return;
    }
    m_skill = skill;
    reset();
}

QString AgentWorkflow::stageName() const {
    switch (m_stage) {
    case AgentWorkflowStage::Collect: return QStringLiteral("collect");
    case AgentWorkflowStage::AwaitPlanApproval: return QStringLiteral("await-plan-approval");
    case AgentWorkflowStage::Execute: return QStringLiteral("execute");
    case AgentWorkflowStage::Verify: return QStringLiteral("verify");
    case AgentWorkflowStage::Report: return QStringLiteral("report");
    case AgentWorkflowStage::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("collect");
}

void AgentWorkflow::reset(const QString &reason) {
    m_stage = AgentWorkflowStage::Collect;
    m_goal.clear();
    m_steps.clear();
    m_planApproved = false;
    m_evidenceIds.clear();
    m_touchedTools.clear();
    m_executedSteps = 0;
    m_failedSteps = 0;
    m_verified = false;
    m_failureReason = reason;
}

bool AgentWorkflow::isRepairStep(const AgentPlanStep &step) const {
    const AgentToolSpec toolSpec = AgentToolRegistry::find(step.tool);
    return isWriteRisk(toolSpec.risk);
}

bool AgentWorkflow::isVerifyStep(const AgentPlanStep &step) const {
    if (step.verify) return true;
    const AgentToolSpec toolSpec = AgentToolRegistry::find(step.tool);
    return step.tool.startsWith(QStringLiteral("diagnostics.")) ||
           step.tool == QStringLiteral("instance.integrity") ||
           step.tool == QStringLiteral("instance.context") ||
           step.tool == QStringLiteral("settings.get") ||
           step.tool == QStringLiteral("mods.list");
}

bool AgentWorkflow::submitPlan(const QString &goal, const QList<AgentPlanStep> &steps,
                               QString *problem) {
    const auto fail = [problem](const QString &message) {
        if (problem != nullptr) *problem = message;
        return false;
    };
    if (!active()) return fail(QStringLiteral("自由对话模式不需要提交计划。"));
    if (!requiresPlan()) return fail(QStringLiteral("这个模式不需要计划，直接调用工具即可。"));
    if (goal.trimmed().isEmpty()) return fail(QStringLiteral("计划缺少目标（goal）。"));
    if (goal.size() > kMaxGoalLength) return fail(QStringLiteral("计划目标太长（上限 500 字）。"));
    if (steps.isEmpty()) return fail(QStringLiteral("计划里没有任何步骤。"));
    if (steps.size() > m_skill.maxSteps) {
        return fail(QStringLiteral("计划步数超过上限（%1）。").arg(m_skill.maxSteps));
    }
    bool hasVerify = false;
    bool hasRepair = false;
    for (int i = 0; i < steps.size(); ++i) {
        const AgentPlanStep &step = steps.at(i);
        const QString label = QStringLiteral("第 %1 步").arg(i + 1);
        const AgentToolSpec toolSpec = AgentToolRegistry::find(step.tool);
        if (toolSpec.name.isEmpty()) return fail(QStringLiteral("%1 的工具 %2 不存在。").arg(label, step.tool));
        if (step.tool.startsWith(QStringLiteral("workflow."))) {
            return fail(QStringLiteral("%1 不能是工作流工具本身。").arg(label));
        }
        if (!m_skill.allows(step.tool)) {
            return fail(QStringLiteral("%1 的工具 %2 不在当前模式白名单里。").arg(label, step.tool));
        }
        if (m_skill.readOnly && isWriteRisk(toolSpec.risk)) {
            return fail(QStringLiteral("%1 是写操作，当前模式只读。").arg(label));
        }
        QString schemaProblem;
        if (!AgentToolRegistry::validateArguments(toolSpec.schema, step.arguments, &schemaProblem)) {
            return fail(QStringLiteral("%1 的参数不合法：%2").arg(label, schemaProblem));
        }
        if (step.reason.size() > kMaxReasonLength) {
            return fail(QStringLiteral("%1 的理由太长。").arg(label));
        }
        if (isRepairStep(step)) hasRepair = true;
        if (step.verify) {
            if (isWriteRisk(toolSpec.risk)) {
                return fail(QStringLiteral("%1 被标成验证步，但它是写操作。").arg(label));
            }
            hasVerify = true;
        }
    }
    if (!hasRepair) return fail(QStringLiteral("计划里没有任何修复步骤，只能是纯只读。"));
    if (!hasVerify) return fail(QStringLiteral("计划至少要有一个验证步（verify=true）。"));

    m_goal = goal.trimmed();
    m_steps = steps;
    m_planApproved = false;
    m_executedSteps = 0;
    m_failedSteps = 0;
    m_failureReason.clear();
    m_verified = false;
    m_stage = AgentWorkflowStage::AwaitPlanApproval;
    return true;
}

QJsonObject AgentWorkflow::planJson() const {
    QJsonArray steps;
    for (const AgentPlanStep &step : m_steps) steps.append(step.toJson());
    return QJsonObject{{QStringLiteral("goal"), m_goal},
                       {QStringLiteral("steps"), steps},
                       {QStringLiteral("approved"), m_planApproved},
                       {QStringLiteral("stage"), stageName()}};
}

QString AgentWorkflow::planPreview() const {
    QString text = QStringLiteral("目标：%1\n").arg(m_goal);
    int index = 1;
    for (const AgentPlanStep &step : m_steps) {
        text += QStringLiteral("%1. %2 — %3").arg(index).arg(step.tool, step.reason);
        if (step.verify) text += QStringLiteral("（验证步）");
        text += QLatin1Char('\n');
        if (!step.arguments.isEmpty()) {
            text += QString::fromUtf8(
                QJsonDocument(step.arguments).toJson(QJsonDocument::Compact));
            text += QLatin1Char('\n');
        }
        ++index;
    }
    text += QStringLiteral("注意：批准计划不等于批准所有写入 —— 每个写步骤还会单独弹审批。");
    return text;
}

void AgentWorkflow::approvePlan(bool approved) {
    if (m_stage != AgentWorkflowStage::AwaitPlanApproval) return;
    m_planApproved = approved;
    if (!approved) {
        reset(QStringLiteral("用户拒绝了修复计划。"));
        m_stage = AgentWorkflowStage::Collect;
        return;
    }
    m_stage = AgentWorkflowStage::Execute;
}

bool AgentWorkflow::stepAllows(const QString &tool) const {
    if (!requiresPlan()) return true;
    if (!m_planApproved) return false;
    // 只放行**当前这一**步：修复和验证是有顺序的，跳过验证直接再写一遍不算数。
    if (m_executedSteps >= m_steps.size()) return false;
    return m_steps.at(m_executedSteps).tool == tool;
}

int AgentWorkflow::nextStepIndex() const { return m_executedSteps; }

void AgentWorkflow::recordResult(const Agent::ToolCall &call, const AgentToolResult &result) {
    m_touchedTools.insert(call.name);
    const QJsonArray evidenceIds = result.data.value(QStringLiteral("evidenceIds")).toArray();
    for (const QJsonValue &value : evidenceIds) {
        const QString id = value.toString();
        if (!id.isEmpty() && !m_evidenceIds.contains(id)) m_evidenceIds.append(id);
    }
    for (const QJsonValue &value : result.data.value(QStringLiteral("evidence")).toArray()) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && !m_evidenceIds.contains(id)) m_evidenceIds.append(id);
    }
    if (m_stage != AgentWorkflowStage::Execute && m_stage != AgentWorkflowStage::Verify) return;
    if (m_steps.isEmpty()) return;
    // 计划里的一步：按顺序推进；失败即停（不让模型跳过失败继续“成功”）。
    if (m_steps.size() > m_executedSteps) {
        const AgentPlanStep &step = m_steps.at(m_executedSteps);
        if (step.tool != call.name) return;
        if (result.status != AgentToolResult::Status::Ok) {
            ++m_failedSteps;
            m_failureReason = QStringLiteral("第 %1 步（%2）失败：%3")
                                  .arg(m_executedSteps + 1)
                                  .arg(step.tool, result.summary);
            m_stage = AgentWorkflowStage::Failed;
            return;
        }
        ++m_executedSteps;
        if (step.verify) {
            m_verified = true;
            m_stage = AgentWorkflowStage::Verify;
        }
    }
}

bool AgentWorkflow::canFinish(QString *problem) const {
    const auto fail = [problem](const QString &message) {
        if (problem != nullptr) *problem = message;
        return false;
    };
    if (m_stage == AgentWorkflowStage::Failed) {
        return fail(m_failureReason.isEmpty() ? QStringLiteral("工作流已经失败。") : m_failureReason);
    }
    if (!active()) return true;  // 自由对话没有完成闸
    if (m_skill.requiresPlan) {
        if (!m_planApproved) return fail(QStringLiteral("修复计划还没有通过审批。"));
        if (m_executedSteps < m_steps.size()) {
            return fail(QStringLiteral("计划还有 %1 步没执行。").arg(m_steps.size() - m_executedSteps));
        }
        if (!m_verified) return fail(QStringLiteral("没有执行验证步，不能报完成。"));
    }
    for (const QString &tool : m_skill.requiredTools) {
        if (!m_touchedTools.contains(tool)) {
            return fail(QStringLiteral("缺少必需的工具调用：%1。").arg(tool));
        }
    }
    if (m_evidenceIds.isEmpty()) return fail(QStringLiteral("没有任何证据 id，不能报完成。"));
    return true;
}

void AgentWorkflow::markFinished() {
    if (m_stage != AgentWorkflowStage::Failed) m_stage = AgentWorkflowStage::Report;
}

void AgentWorkflow::markFailed(const QString &reason) {
    m_stage = AgentWorkflowStage::Failed;
    m_failureReason = reason;
}

QJsonObject AgentWorkflow::statusJson() const {
    const bool needsPlan = m_skill.requiresPlan;
    return QJsonObject{{QStringLiteral("skill"), m_skill.id},
                       {QStringLiteral("skillName"), m_skill.name},
                       {QStringLiteral("stage"), stageName()},
                       {QStringLiteral("readOnly"), m_skill.readOnly},
                       {QStringLiteral("requiresPlan"), needsPlan},
                       {QStringLiteral("planApproved"), m_planApproved},
                       {QStringLiteral("steps"), m_steps.size()},
                       {QStringLiteral("executedSteps"), m_executedSteps},
                       {QStringLiteral("evidenceIds"), QJsonArray::fromStringList(m_evidenceIds)}};
}

QString AgentWorkflow::statusLine() const {
    if (!active()) return QString();
    QString line = QStringLiteral("模式：%1 · 阶段：%2").arg(m_skill.name, stageName());
    if (!m_steps.isEmpty()) {
        line += QStringLiteral(" · 计划 %1/%2 步").arg(m_executedSteps).arg(m_steps.size());
    }
    if (!m_evidenceIds.isEmpty()) {
        line += QStringLiteral(" · 证据 %1 条").arg(m_evidenceIds.size());
    }
    return line;
}
