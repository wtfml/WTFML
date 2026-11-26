/**
 * @file AgentMcpToolEngine.cpp
 * @brief MCP 工具的审批与异步交付（见 .hpp）。
 */

#include "AgentMcpToolEngine.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentRedaction.hpp"
#include "AgentTool.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

constexpr int kInlineResultBytes = 16 * 1024;

QJsonObject mcpEgress(const QString &serverName) {
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("external-mcp")},
        {QStringLiteral("decision"), QStringLiteral("confirm")},
        {QStringLiteral("reason"),
         QStringLiteral("结果来自外部 MCP 服务器「%1」，按不可信数据处理").arg(serverName)},
        {QStringLiteral("redactionHits"), 0}};
}

}  // namespace

AgentMcpToolEngine::AgentMcpToolEngine(McpManager *manager, ContextProvider provider,
                                       QObject *parent)
    : QObject(parent), m_manager(manager), m_provider(std::move(provider)) {
    if (m_manager != nullptr) {
        connect(m_manager, &McpManager::toolFinished, this, &AgentMcpToolEngine::onToolFinished);
        connect(m_manager, &McpManager::toolFailed, this, &AgentMcpToolEngine::onToolFailed);
        connect(m_manager, &McpManager::toolsChanged, this, [this] {
            // 服务器掉线/工具清单变化：以前的审批请求不能再被批准成一次调用。
            m_approvals.clear();
        });
    }
}

void AgentMcpToolEngine::setManager(McpManager *manager) {
    if (m_manager == manager) return;
    if (m_manager != nullptr) disconnect(m_manager, nullptr, this, nullptr);
    m_manager = manager;
    m_approvals.clear();
    if (m_manager != nullptr) {
        connect(m_manager, &McpManager::toolFinished, this, &AgentMcpToolEngine::onToolFinished);
        connect(m_manager, &McpManager::toolFailed, this, &AgentMcpToolEngine::onToolFailed);
    }
}

AgentToolResult AgentMcpToolEngine::errorResult(Agent::ErrorKind kind,
                                                const QString &message) const {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

bool AgentMcpToolEngine::handles(const QString &internalName) const {
    if (m_manager == nullptr) return false;
    if (!internalName.startsWith(QStringLiteral("mcp__"))) return false;
    return !m_manager->toolForWireName(internalName).name.isEmpty() ||
           !m_manager->conflictReason(internalName).isEmpty();
}

QJsonArray AgentMcpToolEngine::schemas() const {
    QJsonArray array;
    if (m_manager == nullptr) return array;
    for (const McpToolDescriptor &tool : m_manager->tools()) {
        QJsonObject parameters = tool.inputSchema;
        if (parameters.isEmpty()) {
            parameters = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                     {QStringLiteral("properties"), QJsonObject{}}};
        }
        array.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{
                 {QStringLiteral("name"), tool.wireName()},
                 {QStringLiteral("description"),
                  QStringLiteral("[MCP · %1] %2").arg(tool.serverName, tool.description)},
                 {QStringLiteral("parameters"), parameters}}}});
    }
    return array;
}

AgentToolResult AgentMcpToolEngine::invoke(const Agent::ToolCall &call) {
    if (m_manager == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("MCP 未接入。"));
    }
    const QString conflict = m_manager->conflictReason(call.name);
    if (!conflict.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, conflict);
    const McpToolDescriptor tool = m_manager->toolForWireName(call.name);
    if (tool.name.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("MCP 工具不存在或服务器未连接。"));
    }
    QString schemaProblem;
    const QJsonObject schema = tool.inputSchema.isEmpty()
                                   ? QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}
                                   : tool.inputSchema;
    if (!AgentToolRegistry::validateArguments(schema, call.arguments, &schemaProblem)) {
        return errorResult(Agent::ErrorKind::ToolError, schemaProblem);
    }
    AgentToolContext context;
    if (m_provider != nullptr) {
        QString problem;
        context = m_provider(&problem);
    }
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ApprovalPending pending;
    pending.tool = tool;
    pending.arguments = call.arguments;
    pending.call = call;
    m_approvals.insert(approvalId, pending);

    const QString argumentsText =
        QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact));
    const QString summary = QStringLiteral("调用 MCP 工具：%1（服务器「%2」）")
                                .arg(tool.name, tool.serverName);
    QJsonObject request = AgentApprovalPolicy::request(
        call.name, QStringLiteral("外部 MCP 工具"), tool.serverName, summary,
        AgentRedaction::preview(argumentsText, 2000), QString(),
        QStringLiteral("服务器以当前 Windows 用户权限运行；结果按不可信数据处理。"),
        mcpEgress(tool.serverName));
    request.insert(QStringLiteral("approvalId"), approvalId);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentMcpToolEngine::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_approvals.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));
    }
    const ApprovalPending pending = m_approvals.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(
            Agent::ErrorKind::ApprovalDenied, QStringLiteral("用户拒绝了这次 MCP 调用。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    const QString requestId =
        m_manager->callTool(pending.tool.serverId, pending.tool.name, pending.arguments);
    if (requestId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("MCP 服务器现在不可调用（未连接或待处理请求已满）。"));
    }
    m_calls.insert(requestId, pending.call);
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("已发送到 MCP 服务器，等结果…");
    return result;
}

AgentToolResult AgentMcpToolEngine::resultFromContent(const Agent::ToolCall &call,
                                                      const QJsonObject &result) {
    const bool isError = result.value(QStringLiteral("isError")).toBool(false);
    QString text;
    QJsonArray images;
    for (const QJsonValue &value : result.value(QStringLiteral("content")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("text")) {
            text += item.value(QStringLiteral("text")).toString();
            text += QLatin1Char('\n');
        } else if (type == QStringLiteral("image")) {
            images.append(item.value(QStringLiteral("mimeType")).toString());
        } else {
            text += QStringLiteral("[%1 content not inlined]").arg(type);
            text += QLatin1Char('\n');
        }
    }
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    QString inlineText = redacted.text;
    const QString egressKind = QStringLiteral("external-mcp");
    const QString serverName = [this, &call] {
        const McpToolDescriptor tool = m_manager->toolForWireName(call.name);
        return tool.serverName.isEmpty() ? QStringLiteral("MCP") : tool.serverName;
    }();
    QJsonObject data{{QStringLiteral("server"), serverName},
                     {QStringLiteral("tool"), call.name},
                     {QStringLiteral("isError"), isError},
                     {QStringLiteral("content"), QString()},
                     {QStringLiteral("redactionHits"), redacted.hits},
                     {QStringLiteral("egress"), mcpEgress(serverName)},
                     {QStringLiteral("egressKind"), egressKind},
                     {QStringLiteral("untrusted"), true}};
    if (!images.isEmpty()) data.insert(QStringLiteral("images"), images);
    bool truncated = false;
    if (inlineText.toUtf8().size() > kInlineResultBytes) {
        inlineText = QString::fromUtf8(inlineText.toUtf8().left(kInlineResultBytes)) +
                     QStringLiteral("\n…(MCP result truncated)");
        truncated = true;
    }
    data.insert(QStringLiteral("content"), inlineText);
    data.insert(QStringLiteral("truncated"), truncated);
    AgentToolResult out;
    out.status = isError ? AgentToolResult::Status::Error : AgentToolResult::Status::Ok;
    out.summary = isError ? QStringLiteral("MCP 工具返回错误（%1）").arg(serverName)
                          : QStringLiteral("MCP 工具完成：%1").arg(call.name);
    if (isError) {
        out.error.kind = Agent::ErrorKind::ToolError;
        out.error.message = out.summary;
    }
    out.data = data;
    return out;
}

void AgentMcpToolEngine::onToolFinished(const QString &requestId, const QJsonObject &result) {
    if (!m_calls.contains(requestId)) return;
    const Agent::ToolCall call = m_calls.take(requestId);
    const AgentToolResult out = resultFromContent(call, result);
    if (m_sink) m_sink(call, out);
}

void AgentMcpToolEngine::onToolFailed(const QString &requestId, const Agent::Error &error) {
    if (!m_calls.contains(requestId)) return;
    const Agent::ToolCall call = m_calls.take(requestId);
    AgentToolResult out = errorResult(error.kind, error.message);
    out.data = QJsonObject{{QStringLiteral("server"), QStringLiteral("MCP")},
                           {QStringLiteral("untrusted"), true}};
    if (m_sink) m_sink(call, out);
}

void AgentMcpToolEngine::cancelPending() {
    m_approvals.clear();
    const QStringList ids = m_calls.keys();
    for (const QString &id : ids) {
        if (m_manager != nullptr) m_manager->cancelCall(id);
    }
    for (const QString &id : ids) {
        const Agent::ToolCall call = m_calls.take(id);
        AgentToolResult out = errorResult(Agent::ErrorKind::Cancelled,
                                          QStringLiteral("这次 MCP 调用已取消。"));
        if (m_sink) m_sink(call, out);
    }
}
