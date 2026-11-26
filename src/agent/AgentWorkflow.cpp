/**
 * @file AgentWorkflow.cpp
 * @brief 工作流状态机与计划校验（见 .hpp）。
 */

#include "AgentWorkflow.hpp"

#include "AgentRepairTransaction.hpp"


#include <QCoreApplication>
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
           step.tool == QStringLiteral("mods.list") ||
           step.tool == QStringLiteral("instance.testLaunch") ||
           step.tool == QStringLiteral("repair.verify");
}

bool AgentWorkflow::submitPlan(const QString &goal, const QList<AgentPlanStep> &steps,
                               QString *problem) {
    const auto fail = [problem](const QString &message) {
        if (problem != nullptr) *problem = message;
        return false;
    };
    if (!active()) return fail(QCoreApplication::translate("AgentWorkflow", "自由对话模式无需提交计划。"));
    if (!requiresPlan()) return fail(QCoreApplication::translate("AgentWorkflow", "当前模式无需提交计划，可直接调用工具。"));
    if (goal.trimmed().isEmpty()) return fail(QCoreApplication::translate("AgentWorkflow", "计划缺少目标（goal）。"));
    if (goal.size() > kMaxGoalLength) return fail(QCoreApplication::translate("AgentWorkflow", "计划目标过长（上限 500 字）。"));
    if (steps.isEmpty()) return fail(QCoreApplication::translate("AgentWorkflow", "计划未包含任何步骤。"));
    if (steps.size() > m_skill.maxSteps) {
        return fail(QCoreApplication::translate("AgentWorkflow", "计划步数超过上限（%1）。").arg(m_skill.maxSteps));
    }
    bool hasVerify = false;
    bool hasRepair = false;
    for (int i = 0; i < steps.size(); ++i) {
        const AgentPlanStep &step = steps.at(i);
        const QString label = QCoreApplication::translate("AgentWorkflow", "第 %1 步").arg(i + 1);
        const AgentToolSpec toolSpec = AgentToolRegistry::find(step.tool);
        if (toolSpec.name.isEmpty()) return fail(QCoreApplication::translate("AgentWorkflow", "%1 的工具 %2 不存在。").arg(label, step.tool));
        if (step.tool.startsWith(QStringLiteral("workflow."))) {
            return fail(QCoreApplication::translate("AgentWorkflow", "%1 不能为工作流工具本身。").arg(label));
        }
        if (!m_skill.allows(step.tool)) {
            return fail(QCoreApplication::translate("AgentWorkflow", "%1 的工具 %2 不在当前模式白名单内。").arg(label, step.tool));
        }
        if (m_skill.readOnly && isWriteRisk(toolSpec.risk)) {
            return fail(QCoreApplication::translate("AgentWorkflow", "%1 为写操作，当前模式仅限只读。").arg(label));
        }
        QString schemaProblem;
        if (!AgentToolRegistry::validateArguments(toolSpec.schema, step.arguments, &schemaProblem)) {
            return fail(QCoreApplication::translate("AgentWorkflow", "%1 的参数无效：%2").arg(label, schemaProblem));
        }
        if (step.reason.size() > kMaxReasonLength) {
            return fail(QCoreApplication::translate("AgentWorkflow", "%1 的说明过长。").arg(label));
        }
        if (isRepairStep(step)) hasRepair = true;
        if (step.verify) {
            // 测试启动会起进程（risk=Command），但不写原实例；它是 S5.1 的验证步。
            const bool executionVerify = step.tool == QStringLiteral("instance.testLaunch") ||
                                         step.tool == QStringLiteral("repair.verify");
            if (isWriteRisk(toolSpec.risk) && !executionVerify) {
                return fail(QCoreApplication::translate("AgentWorkflow", "%1 被标记为验证步，但其为写操作。").arg(label));
            }
            hasVerify = true;
        }
    }
    if (!hasRepair) return fail(QCoreApplication::translate("AgentWorkflow", "计划未包含任何修复步骤，不能仅包含只读步骤。"));
    if (!hasVerify) return fail(QCoreApplication::translate("AgentWorkflow", "计划须至少包含一个验证步（verify=true）。"));

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
    QString text = QCoreApplication::translate("AgentWorkflow", "目标：%1\n").arg(m_goal);
    int index = 1;
    for (const AgentPlanStep &step : m_steps) {
        text += QStringLiteral("%1. %2 — %3").arg(index).arg(step.tool, step.reason);
        if (step.verify) text += QCoreApplication::translate("AgentWorkflow", "（验证步）");
        text += QLatin1Char('\n');
        if (!step.arguments.isEmpty()) {
            text += QString::fromUtf8(
                QJsonDocument(step.arguments).toJson(QJsonDocument::Compact));
            text += QLatin1Char('\n');
        }
        ++index;
    }
    text += QCoreApplication::translate("AgentWorkflow", "注意：批准计划不等于批准所有写入，每个写步骤仍需单独审批。");
    return text;
}

void AgentWorkflow::approvePlan(bool approved) {
    if (m_stage != AgentWorkflowStage::AwaitPlanApproval) return;
    m_planApproved = approved;
    if (!approved) {
        reset(QCoreApplication::translate("AgentWorkflow", "用户拒绝了修复计划。"));
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
            m_failureReason = QCoreApplication::translate("AgentWorkflow", "第 %1 步（%2）失败：%3")
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
        return fail(m_failureReason.isEmpty() ? QCoreApplication::translate("AgentWorkflow", "工作流已失败。") : m_failureReason);
    }
    if (!active()) return true;  // 自由对话没有完成闸
    if (m_skill.requiresPlan) {
        if (!m_planApproved) return fail(QCoreApplication::translate("AgentWorkflow", "修复计划尚未通过审批。"));
        if (m_executedSteps < m_steps.size()) {
            return fail(QCoreApplication::translate("AgentWorkflow", "计划仍有 %1 步未执行。").arg(m_steps.size() - m_executedSteps));
        }
        if (!m_verified) return fail(QCoreApplication::translate("AgentWorkflow", "未执行验证步，不能标记完成。"));
    }
    for (const QString &tool : m_skill.requiredTools) {
        if (!m_touchedTools.contains(tool)) {
            return fail(QCoreApplication::translate("AgentWorkflow", "缺少必需的工具调用：%1。").arg(tool));
        }
    }
    if (m_evidenceIds.isEmpty()) return fail(QCoreApplication::translate("AgentWorkflow", "缺少证据 id，不能标记完成。"));
    if (!m_instanceId.isEmpty()) {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord active;
        if (transactions.activeForInstance(m_instanceId, &active, nullptr)) {
            return fail(QCoreApplication::translate("AgentWorkflow", "仍有未收尾的修复事务（%1）；需先通过 repair.finish 提交或回滚。")
                            .arg(active.id));
        }
    }
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
    QString line = QCoreApplication::translate("AgentWorkflow", "模式：%1 · 阶段：%2").arg(m_skill.name, stageName());
    if (!m_steps.isEmpty()) {
        line += QCoreApplication::translate("AgentWorkflow", " · 计划 %1/%2 步").arg(m_executedSteps).arg(m_steps.size());
    }
    if (!m_evidenceIds.isEmpty()) {
        line += QCoreApplication::translate("AgentWorkflow", " · 证据 %1 条").arg(m_evidenceIds.size());
    }
    return line;
}
