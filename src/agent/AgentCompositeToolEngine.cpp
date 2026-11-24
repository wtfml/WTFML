/**
 * @file AgentCompositeToolEngine.cpp
 * @brief 子引擎分派、技能闸与结果记账（见 .hpp）。
 */

#include "AgentCompositeToolEngine.hpp"

#include <QCoreApplication>
#include <QDateTime>


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
                                         AgentToolResult result) {
            observeResult(call, &result);
            if (m_workflow != nullptr) m_workflow->recordResult(call, result);
            if (m_sink) m_sink(call, result);
        });
    }
}

void AgentCompositeToolEngine::observeResult(const Agent::ToolCall &call,
                                             AgentToolResult *result) {
    if (m_autoSafe == nullptr || result == nullptr) return;
    if (!m_autoSafe->enabled() && m_autoSafe->audit().value(QStringLiteral("state")).toString() !=
                                      QStringLiteral("active")) {
        return;
    }
    const QString tool = AgentToolRegistry::internalName(call.name);
    if (tool == QStringLiteral("repair.begin") &&
        result->status == AgentToolResult::Status::Ok) {
        const QString transactionId = result->data.value(QStringLiteral("transactionId")).toString();
        const QString workspaceId = result->data.value(QStringLiteral("workspaceId")).toString();
        if (!transactionId.isEmpty()) m_autoSafe->bindTransaction(transactionId, workspaceId);
        return;
    }
    if (tool == QStringLiteral("repair.verify")) {
        const QJsonObject verification = result->data.value(QStringLiteral("verification")).toObject();
        if (!verification.isEmpty()) m_autoSafe->noteVerification(verification);
        return;
    }
    if (tool == QStringLiteral("repair.finish")) {
        const bool committed = result->data.value(QStringLiteral("committed")).toBool();
        const QString state = committed ? QStringLiteral("committed")
                             : result->status == AgentToolResult::Status::Ok
                                 ? QStringLiteral("rolled_back")
                                 : QStringLiteral("failed");
        const QJsonObject audit =
            m_autoSafe->finish(state, QString(), QDateTime::currentDateTimeUtc());
        result->data.insert(QStringLiteral("autoSafeAudit"), audit);
        result->data.insert(QStringLiteral("autoSafe"),
                            QJsonObject{{QStringLiteral("state"), state},
                                        {QStringLiteral("launches"), m_autoSafe->launchesUsed()}});
    }
}

AgentToolResult AgentCompositeToolEngine::applyAutoSafe(const Agent::ToolCall &call,
                                                        const AgentToolResult &result,
                                                        AgentToolEngine *engine) {
    if (m_autoSafe == nullptr || engine == nullptr || !m_autoSafe->enabled()) return result;
    const AgentToolSpec spec = AgentToolRegistry::find(call.name);
    const bool approvable = spec.risk == AgentRisk::Write || spec.risk == AgentRisk::Destructive ||
                            spec.risk == AgentRisk::Command;
    if (!approvable) return result;  // 外发确认（NetworkRead）不归 Auto-safe 管
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QString reason;
    int launches = 0;
    const AgentAutoSafe::Decision decision = m_autoSafe->authorize(call, now, &reason, &launches);
    if (decision == AgentAutoSafe::Decision::RequireApproval) {
        AgentToolResult pending = result;
        pending.approval.insert(QStringLiteral("autoSafeReason"), reason);
        return pending;
    }
    if (decision == AgentAutoSafe::Decision::Deny) {
        AgentToolResult denied;
        denied.status = AgentToolResult::Status::Error;
        denied.summary = QCoreApplication::translate("AgentCompositeToolEngine",
                                                     "Auto-safe 拒绝：%1")  // i18n-ignore
                             .arg(reason);
        denied.error.kind = Agent::ErrorKind::ToolError;
        denied.error.message = denied.summary;
        denied.data = QJsonObject{{QStringLiteral("autoSafeDenied"), true},
                                  {QStringLiteral("autoSafeReason"), reason}};
        m_autoSafe->record(call, denied, 0, now);
        return denied;
    }
    const QString approvalId = result.approval.value(QStringLiteral("approvalId")).toString();
    AgentToolResult resolved = engine->resolveApproval(approvalId, true);
    m_autoSafe->record(call, resolved, launches, now);
    observeResult(call, &resolved);
    return resolved;
}

bool AgentCompositeToolEngine::handles(const QString &internalName) const {
    return findEngine(internalName) != nullptr;
}

QJsonArray AgentCompositeToolEngine::schemas() const {
    QJsonArray result;
    const AgentSkill skill = m_workflow != nullptr ? m_workflow->skill() : AgentSkill{};
    for (const AgentToolEngine *engine : m_engines) {
        for (const QJsonValue &value : engine->schemas()) {
            const QString wire = value.toObject()
                                     .value(QStringLiteral("function"))
                                     .toObject()
                                     .value(QStringLiteral("name"))
                                     .toString();
            const QString internal = AgentToolRegistry::internalName(wire);
            if (!skill.allows(internal)) continue;
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
    // 白名单在每个模式都生效（自由对话也有一份显式清单）；计划/步进闸只对有工作流的模式。
    const AgentSkill skill = m_workflow != nullptr ? m_workflow->skill() : AgentSkill{};
    if (!skill.allows(internalName)) {
        return riskBlockedMessage(skill.name, internalName);
    }
    if (m_workflow == nullptr || !m_workflow->active()) return QString();
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internalName);
    if (skill.readOnly && isWriteRisk(toolSpec.risk)) {
        return QStringLiteral("当前模式（%1）是只读模式，不能执行写操作。").arg(skill.name);
    }
    // rollback 是失败后的收尾动作；commit 仍由工具自己要求和验证证据与逐次审批。
    if (internalName == QStringLiteral("repair.finish")) return QString();
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
    const AgentToolResult raw = engine->invoke(routed);
    if (raw.status == AgentToolResult::Status::Pending) return raw;  // 结果从 sink 回来
    if (raw.status == AgentToolResult::Status::ApprovalRequired) {
        const QString approvalId = raw.approval.value(QStringLiteral("approvalId")).toString();
        if (!approvalId.isEmpty()) {
            m_approvalOwner.insert(approvalId, engine);
            m_approvalCalls.insert(approvalId, routed);
        }
        // Auto-safe 授权的写/启动工具在这里替用户点"允许"；范围外的仍旧交给 UI。
        const AgentToolResult result = applyAutoSafe(routed, raw, engine);
        if (result.status == AgentToolResult::Status::ApprovalRequired) return result;
        if (result.data.value(QStringLiteral("autoSafeDenied")).toBool()) return result;
        if (m_workflow != nullptr && !routed.name.startsWith(QStringLiteral("workflow."))) {
            m_workflow->recordResult(routed, result);
        }
        return result;
    }
    AgentToolResult result = raw;
    observeResult(routed, &result);
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
    AgentToolResult result = engine->resolveApproval(approvalId, approved);
    if (result.status == AgentToolResult::Status::Pending) return result;
    // 计划批准 = Auto-safe 的授权范围（写哪些文件/键/模组）。
    if (m_autoSafe != nullptr && m_autoSafe->enabled() && m_workflow != nullptr &&
        AgentToolRegistry::internalName(call.name) == QStringLiteral("workflow.submitPlan") &&
        result.status == AgentToolResult::Status::Ok) {
        m_autoSafe->bindPlan(m_workflow->planSteps());
    }
    observeResult(call, &result);
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
