/**
 * @file AgentWorkflowEngine.cpp
 * @brief 计划卡与完成闸（见 .hpp）。
 */

#include "AgentWorkflowEngine.hpp"

#include "AgentApprovalPolicy.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

/// 计划/报告都是模型自己写的结构化数据，透传不进 egress 判定；这里只填一个说明字段。
QJsonObject planEgress() {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("workflow-plan")},
                       {QStringLiteral("decision"), QStringLiteral("confirm")},
                       {QStringLiteral("reason"), QCoreApplication::translate("AgentWorkflowEngine", "计划不含文件内容")}};
}

}  // namespace

QList<AgentToolSpec> AgentWorkflowEngine::specs() {
    QList<AgentToolSpec> list;
    list.append(AgentToolSpec{
        QStringLiteral("workflow.submitPlan"), QCoreApplication::translate("AgentWorkflowEngine", "提交修复计划"),
        QStringLiteral("结构化提交修复计划；每个写步骤都会在执行时单独审批，批准计划不等于批准写入。"),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("goal"), QStringLiteral("steps")},
                     {{QStringLiteral("goal"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 500}}},
                      {QStringLiteral("steps"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                   {QStringLiteral("maxItems"), 20}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("workflow.finish"), QCoreApplication::translate("AgentWorkflowEngine", "提交完成报告"),
        QStringLiteral("提交报告前本地核对：计划是否批准、有没有验证步、有没有证据。缺一项就拒绝。"),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("report")},
                     {{QStringLiteral("report"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 8000}}},
                      {QStringLiteral("unresolved"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 2000}}}})});
    return list;
}

bool AgentWorkflowEngine::isWorkflowTool(const QString &internalName) {
    return internalName == QStringLiteral("workflow.submitPlan") ||
           internalName == QStringLiteral("workflow.finish");
}

AgentWorkflowEngine::AgentWorkflowEngine(AgentWorkflow *workflow) : m_workflow(workflow) {}

QJsonArray AgentWorkflowEngine::schemas() const {
    QJsonArray result;
    for (const AgentToolSpec &item : specs()) {
        result.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), AgentToolRegistry::wireName(item.name)},
                         {QStringLiteral("description"), item.description},
                         {QStringLiteral("parameters"), item.schema}}}});
    }
    return result;
}

bool AgentWorkflowEngine::handles(const QString &internalName) const {
    return isWorkflowTool(internalName);
}

void AgentWorkflowEngine::cancelPending() { m_pending.clear(); }

AgentToolResult AgentWorkflowEngine::errorResult(Agent::ErrorKind kind,
                                                 const QString &message) const {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

AgentToolResult AgentWorkflowEngine::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    if (m_workflow == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentWorkflowEngine", "工作流未接入。"));
    }
    if (internal == QStringLiteral("workflow.submitPlan")) return handleSubmitPlan(call);
    if (internal == QStringLiteral("workflow.finish")) return handleFinish(call);
    return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentWorkflowEngine", "未知工具。"));
}

AgentToolResult AgentWorkflowEngine::handleSubmitPlan(const Agent::ToolCall &call) {
    const QString goal = call.arguments.value(QStringLiteral("goal")).toString();
    QList<AgentPlanStep> steps;
    for (const QJsonValue &value : call.arguments.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject object = value.toObject();
        AgentPlanStep step;
        step.tool = AgentToolRegistry::internalName(
            object.value(QStringLiteral("tool")).toString());
        step.arguments = object.value(QStringLiteral("arguments")).toObject();
        step.reason = object.value(QStringLiteral("reason")).toString();
        step.verify = object.value(QStringLiteral("verify")).toBool(false);
        steps.append(step);
    }
    QString problem;
    if (!m_workflow->submitPlan(goal, steps, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    Pending pending;
    pending.goal = goal;
    pending.steps = steps;
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject request = AgentApprovalPolicy::request(
        call.name, QCoreApplication::translate("AgentWorkflowEngine", "修复计划"), m_workflow->skill().name, goal,
        m_workflow->planPreview(), QString(),
        QCoreApplication::translate("AgentWorkflowEngine", "批准计划仅表示同意该方案；每个写步骤执行时仍需单独确认。"),
        planEgress());
    request.insert(QStringLiteral("approvalId"), approvalId);
    request.insert(QStringLiteral("plan"), m_workflow->planJson());
    m_pending.insert(approvalId, pending);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = QCoreApplication::translate("AgentWorkflowEngine", "待批准计划：%1（%2 步）").arg(goal).arg(steps.size());
    result.approval = request;
    return result;
}

AgentToolResult AgentWorkflowEngine::handleFinish(const Agent::ToolCall &call) {
    const QString report = call.arguments.value(QStringLiteral("report")).toString();
    const QString unresolved = call.arguments.value(QStringLiteral("unresolved")).toString();
    if (report.trimmed().isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentWorkflowEngine", "报告不能为空。"));
    }
    QString problem;
    if (!m_workflow->canFinish(&problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    m_workflow->markFinished();
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QCoreApplication::translate("AgentWorkflowEngine", "已提交完成报告。");
    result.data = QJsonObject{{QStringLiteral("report"), report},
                              {QStringLiteral("unresolved"), unresolved},
                              {QStringLiteral("evidenceIds"),
                               QJsonArray::fromStringList(m_workflow->evidenceIds())},
                              {QStringLiteral("workflow"), m_workflow->statusJson()}};
    return result;
}

AgentToolResult AgentWorkflowEngine::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentWorkflowEngine", "审批请求已失效。"));
    }
    m_pending.remove(approvalId);
    m_workflow->approvePlan(approved);
    if (!approved) {
        return errorResult(Agent::ErrorKind::ApprovalDenied,
                           QCoreApplication::translate("AgentWorkflowEngine", "用户拒绝了修复计划；计划已作废，停止执行写操作。"));
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QCoreApplication::translate("AgentWorkflowEngine", "计划已批准（%1 步），按顺序执行；每个写步骤仍需单独确认。")
                         .arg(m_workflow->planSteps().size());
    result.data = QJsonObject{{QStringLiteral("plan"), m_workflow->planJson()},
                              {QStringLiteral("stage"), m_workflow->stageName()}};
    return result;
}
