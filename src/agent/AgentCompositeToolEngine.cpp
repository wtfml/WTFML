/**
 * @file AgentCompositeToolEngine.cpp
 * @brief 子引擎分派、技能闸与结果记账（见 .hpp）。
 */

#include "AgentCompositeToolEngine.hpp"


namespace {

bool isWriteRisk(AgentRisk risk) {
    return risk == AgentRisk::Write || risk == AgentRisk::Destructive ||
           risk == AgentRisk::Command || risk == AgentRisk::ExternalMcp;
}

}  // namespace

void AgentCompositeToolEngine::addEngine(AgentToolEngine *engine) {
    if (engine != nullptr && !m_engines.contains(engine)) m_engines.append(engine);
}

AgentToolEngine *AgentCompositeToolEngine::findEngine(const QString &internalName) const {
    for (AgentToolEngine *engine : m_engines) {
        if (engine->handles(internalName)) return engine;
    }
    return nullptr;
}

void AgentCompositeToolEngine::setCompletionSink(AgentToolSink sink) {
    m_sink = std::move(sink);
    for (AgentToolEngine *engine : m_engines) {
        engine->setCompletionSink([this](const Agent::ToolCall &call,
                                         const AgentToolResult &result) {
            if (m_workflow != nullptr) m_workflow->recordResult(call, result);
            if (m_sink) m_sink(call, result);
        });
    }
}

bool AgentCompositeToolEngine::handles(const QString &internalName) const {
    return findEngine(internalName) != nullptr;
}

QJsonArray AgentCompositeToolEngine::schemas() const {
    QJsonArray result;
    const AgentSkill skill = m_workflow != nullptr ? m_workflow->skill() : AgentSkill{};
    for (const AgentToolEngine *engine : m_engines) {
        for (const QJsonValue &value : engine->schemas()) {
            if (m_workflow != nullptr && m_workflow->active()) {
                const QString wire = value.toObject()
                                         .value(QStringLiteral("function"))
                                         .toObject()
                                         .value(QStringLiteral("name"))
                                         .toString();
                const QString internal = AgentToolRegistry::internalName(wire);
                if (!skill.allows(internal)) continue;
            }
            result.append(value);
        }
    }
    return result;
}

QString AgentCompositeToolEngine::riskBlockedMessage(const QString &skillName,
                                                     const QString &toolName) {
    return QStringLiteral("当前模式（%1）不允许调用 %2。").arg(skillName, toolName);
}

QString AgentCompositeToolEngine::gate(const QString &internalName) const {
    if (m_workflow == nullptr || !m_workflow->active()) return QString();
    const AgentSkill skill = m_workflow->skill();
    if (!skill.allows(internalName)) {
        return riskBlockedMessage(skill.name, internalName);
    }
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internalName);
    if (skill.readOnly && isWriteRisk(toolSpec.risk)) {
        return QStringLiteral("当前模式（%1）是只读模式，不能执行写操作。").arg(skill.name);
    }
    if (m_workflow->stage() == AgentWorkflowStage::Failed &&
        internalName != QStringLiteral("workflow.finish") && isWriteRisk(toolSpec.risk)) {
        return QStringLiteral("工作流已经失败，不能继续写入；请先说明失败原因并重新提交计划。");
    }
    if (skill.requiresPlan && isWriteRisk(toolSpec.risk)) {
        if (!m_workflow->planApproved()) {
            return QStringLiteral("修复计划还没批准；请先用 workflow.submitPlan 提交计划。");
        }
        if (!m_workflow->stepAllows(internalName)) {
            const QList<AgentPlanStep> steps = m_workflow->planSteps();
            const int index = m_workflow->nextStepIndex();
            if (index >= steps.size()) {
                return QStringLiteral("计划里的步骤都执行完了，不能再写；请先验证并提交报告。");
            }
            return QStringLiteral("计划按顺序执行：当前第 %1 步是 %2，不是 %3。")
                .arg(index + 1)
                .arg(steps.at(index).tool, internalName);
        }
    }
    return QString();
}

AgentToolResult AgentCompositeToolEngine::invoke(const Agent::ToolCall &call) {
    Agent::ToolCall routed = call;
    routed.name = AgentToolRegistry::internalName(call.name);
    AgentToolEngine *engine = findEngine(routed.name);
    if (engine == nullptr) {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Error;
        result.summary = QStringLiteral("未知工具。");
        result.error.kind = Agent::ErrorKind::ToolError;
        result.error.message = result.summary;
        return result;
    }
    const QString blocked = gate(routed.name);
    if (!blocked.isEmpty()) {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Error;
        result.summary = blocked;
        result.error.kind = Agent::ErrorKind::ToolError;
        result.error.message = blocked;
        result.data = QJsonObject{{QStringLiteral("blockedByWorkflow"), true}};
        if (m_workflow != nullptr) m_workflow->recordResult(routed, result);
        return result;
    }
    const AgentToolResult result = engine->invoke(routed);
    if (result.status == AgentToolResult::Status::Pending) return result;  // 结果从 sink 回来
    if (result.status == AgentToolResult::Status::ApprovalRequired) {
        const QString approvalId =
            result.approval.value(QStringLiteral("approvalId")).toString();
        if (!approvalId.isEmpty()) {
            m_approvalOwner.insert(approvalId, engine);
            m_approvalCalls.insert(approvalId, routed);
        }
        return result;
    }
    if (m_workflow != nullptr) m_workflow->recordResult(routed, result);
    return result;
}

AgentToolResult AgentCompositeToolEngine::resolveApproval(const QString &approvalId, bool approved) {
    AgentToolEngine *engine = m_approvalOwner.take(approvalId);
    if (engine == nullptr) {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Error;
        result.summary = QStringLiteral("审批请求已失效。");
        result.error.kind = Agent::ErrorKind::ToolError;
        result.error.message = result.summary;
        return result;
    }
    const Agent::ToolCall call = m_approvalCalls.take(approvalId);
    const AgentToolResult result = engine->resolveApproval(approvalId, approved);
    if (result.status == AgentToolResult::Status::Pending) return result;
    // 审批通过后的只读结果也带证据：照样记账，否则完成闸会漏掉这一条。
    if (m_workflow != nullptr && !call.name.isEmpty() && !call.name.startsWith(QStringLiteral("workflow."))) {
        m_workflow->recordResult(call, result);
    }
    return result;
}

void AgentCompositeToolEngine::cancelPending() {
    m_approvalOwner.clear();
    m_approvalCalls.clear();
    for (AgentToolEngine *engine : m_engines) engine->cancelPending();
}

bool AgentCompositeToolEngine::committing() const {
    for (const AgentToolEngine *engine : m_engines) {
        if (engine->committing()) return true;
    }
    return false;
}
