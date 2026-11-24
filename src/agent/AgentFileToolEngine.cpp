/**
 * @file AgentFileToolEngine.cpp
 * @brief 文件工具的执行/审批（见 .hpp）。
 */

#include "AgentFileToolEngine.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentInstanceLock.hpp"
#include "AgentRedaction.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {
AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message) {
    AgentToolResult result;
    result.status = kind == Agent::ErrorKind::EgressBlocked ? AgentToolResult::Status::Blocked
                                                            : AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}
}  // namespace

AgentFileToolEngine::AgentFileToolEngine(AgentFileTools *files, AgentArtifactStore *artifacts,
                                         ContextProvider provider)
    : m_files(files), m_artifacts(artifacts), m_provider(std::move(provider)) {}

QJsonArray AgentFileToolEngine::schemas() const {
    QJsonArray result;
    for (const AgentToolSpec &spec : AgentToolRegistry::fileBuiltins()) {
        result.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                  {QStringLiteral("function"),
                                   QJsonObject{{QStringLiteral("name"), AgentToolRegistry::wireName(spec.name)},
                                               {QStringLiteral("description"), spec.description},
                                               {QStringLiteral("parameters"), spec.schema}}}});
    }
    return result;
}

AgentToolResult AgentFileToolEngine::invoke(const Agent::ToolCall &call) {
    const QString internalName = AgentToolRegistry::internalName(call.name);
    const AgentToolSpec spec = AgentToolRegistry::find(internalName);
    if (spec.name.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("未知工具。"));
    QString problem;
    if (!AgentToolRegistry::validateArguments(spec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    Agent::ToolCall routed = call;
    routed.name = internalName;
    QString contextProblem;
    const AgentToolContext context = m_provider(&contextProblem);
    if (!contextProblem.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, contextProblem);
    }
    if (internalName == QStringLiteral("fs.list")) return handleList(routed, context);
    if (internalName == QStringLiteral("fs.listNames")) return handleListNames(routed, context);
    if (internalName == QStringLiteral("fs.read")) return handleRead(routed, context);
    if (internalName == QStringLiteral("fs.write")) return handleWrite(routed, context);
    if (internalName == QStringLiteral("artifact.read")) return handleArtifactRead(routed, context);
    return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("工具未实现。"));
}

QJsonObject AgentFileToolEngine::egressJson(const AgentEgressPolicy::Result &result,
                                            const AgentEgressPolicy::Kind kind) const {
    return QJsonObject{{QStringLiteral("kind"), AgentEgressPolicy::kindKey(kind)},
                       {QStringLiteral("decision"), AgentEgressPolicy::decisionKey(result.decision)},
                       {QStringLiteral("reason"), result.reason},
                       {QStringLiteral("redactionHits"), result.redactionHits}};
}

bool AgentFileToolEngine::autoNotice(const AgentEgressPolicy::Result &result) const {
    return result.decision == AgentEgressPolicy::Decision::Auto && result.needsNotice;
}

AgentToolResult AgentFileToolEngine::handleList(const Agent::ToolCall &call,
                                                const AgentToolContext &context) {
    const QString relative = call.arguments.value(QStringLiteral("path")).toString();
    QString problem;
    const AgentDirMetadata metadata = m_files->list(context.rootPath, relative, &problem);
    if (!problem.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, problem);
    AgentEgressPolicy::Request egressRequest;
    egressRequest.kind = AgentEgressPolicy::Kind::DirectoryMetadata;
    egressRequest.relativePath = relative;
    egressRequest.strictMode = context.strictEgress || m_strictEgress;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(egressRequest);
    if (egress.decision == AgentEgressPolicy::Decision::Block) {
        return errorResult(Agent::ErrorKind::EgressBlocked, egress.reason);
    }
    Pending pending;
    pending.kind = Pending::Kind::DirectoryMetadata;
    pending.call = call;
    pending.context = context;
    pending.egress = egressJson(egress, egressRequest.kind);
    pending.directory = metadata;
    pending.summary = metadata.summary;
    if (egress.decision == AgentEgressPolicy::Decision::Confirm) {
        return approvalRequired(pending, metadata.summary, QString(), QString(),
                                QStringLiteral("只返回目录元数据，不含文件名。"), pending.egress);
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = metadata.summary;
    result.data = QJsonObject{{QStringLiteral("entries"), metadata.entryCount},
                              {QStringLiteral("files"), metadata.fileCount},
                              {QStringLiteral("directories"), metadata.directoryCount},
                              {QStringLiteral("bytes"), metadata.totalSize},
                              {QStringLiteral("truncated"), metadata.truncated},
                              {QStringLiteral("egress"), pending.egress},
                              {QStringLiteral("autoReadNotice"), autoNotice(egress)}};
    return result;
}

AgentToolResult AgentFileToolEngine::handleListNames(const Agent::ToolCall &call,
                                                     const AgentToolContext &context) {
    const QString relative = call.arguments.value(QStringLiteral("path")).toString();
    const int limit = call.arguments.value(QStringLiteral("limit")).toInt(2000);
    bool truncated = false;
    QString problem;
    const QList<AgentFileNameEntry> names =
        m_files->listNames(context.rootPath, relative, limit, &truncated, &problem);
    if (!problem.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, problem);
    QString text;
    for (const AgentFileNameEntry &entry : names) {
        text += entry.name + (entry.directory ? QStringLiteral("/") : QString()) + QLatin1Char('\n');
    }
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    AgentEgressPolicy::Request request;
    request.kind = AgentEgressPolicy::Kind::FileNameList;
    request.relativePath = relative;
    request.content = text;
    request.strictMode = context.strictEgress || m_strictEgress;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(request);

    Pending pending;
    pending.kind = Pending::Kind::FileNames;
    pending.call = call;
    pending.context = context;
    pending.egress = egressJson(egress, request.kind);
    pending.content = redacted.text;
    pending.names = names;
    pending.namesTruncated = truncated;
    pending.summary = QStringLiteral("完整文件名清单：%1 项%2")
                          .arg(names.size())
                          .arg(truncated ? QStringLiteral("（已截断）") : QString());
    return approvalRequired(pending, pending.summary,
                            AgentRedaction::preview(redacted.text, 4000), QString(),
                            QStringLiteral("不提供“本会话允许此目录”；每次都要重新确认。"),
                            pending.egress);
}

AgentToolResult AgentFileToolEngine::handleRead(const Agent::ToolCall &call,
                                                const AgentToolContext &context) {
    const QString relative = call.arguments.value(QStringLiteral("path")).toString();
    const AgentEgressPolicy::Kind kind = AgentEgressPolicy::kindForPath(relative);
    if (kind == AgentEgressPolicy::Kind::CredentialFile) {
        return errorResult(Agent::ErrorKind::EgressBlocked, QStringLiteral("凭据文件禁止读取。"));
    }
    const int maxBytes = call.arguments.value(QStringLiteral("maxBytes")).toInt(0);
    const AgentFileReadResult read = m_files->read(context.rootPath, relative, maxBytes);
    if (!read.ok) return errorResult(read.error.kind, read.error.message);

    AgentEgressPolicy::Request request;
    request.kind = kind;
    request.relativePath = relative;
    request.content = read.redactedText;
    request.strictMode = context.strictEgress || m_strictEgress;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(request);
    if (egress.decision == AgentEgressPolicy::Decision::Block) {
        return errorResult(Agent::ErrorKind::EgressBlocked, egress.reason);
    }
    Pending pending;
    pending.kind = Pending::Kind::Read;
    pending.call = call;
    pending.context = context;
    pending.egress = egressJson(egress, kind);
    pending.read = read;
    pending.content = egress.redactedContent;
    pending.summary = QStringLiteral("读取文本：%1（%2 字节%3）")
                          .arg(relative)
                          .arg(read.size)
                          .arg(read.truncated ? QStringLiteral("，已截断") : QString());
    if (egress.decision == AgentEgressPolicy::Decision::Confirm) {
        return approvalRequired(pending, pending.summary,
                                AgentRedaction::preview(egress.redactedContent, 4000), QString(),
                                QStringLiteral("确认后只发送脱敏片段；不提供会话级目录放行。"),
                                pending.egress);
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = pending.summary;
    QString content = egress.redactedContent;
    QString modelArtifact;
    if (content.toUtf8().size() > 16 * 1024) {
        QString artifactProblem;
        if (m_artifacts != nullptr &&
            m_artifacts->putModelReadable(context.sessionId, content, &modelArtifact,
                                          &artifactProblem)) {
            content = content.left(16 * 1024) + QChar(10) +
                      QStringLiteral("…（本次读取内容见模型可读 artifact，同样已脱敏）");
        }
    }
    result.data = QJsonObject{{QStringLiteral("path"), relative},
                              {QStringLiteral("content"), content},
                              {QStringLiteral("modelArtifact"), modelArtifact},
                              {QStringLiteral("sha256"), read.sha256},
                              {QStringLiteral("fileIdentityToken"), read.identityToken},
                              {QStringLiteral("size"), read.size},
                              {QStringLiteral("truncated"), read.truncated},
                              {QStringLiteral("egress"), pending.egress},
                              {QStringLiteral("autoReadNotice"), autoNotice(egress)}};
    return result;
}

AgentToolResult AgentFileToolEngine::handleWrite(const Agent::ToolCall &call,
                                                 const AgentToolContext &context) {
    const QString relative = call.arguments.value(QStringLiteral("path")).toString();
    const QString content = call.arguments.value(QStringLiteral("content")).toString();
    const QString expected = call.arguments.value(QStringLiteral("expectedSha256")).toString();
    const QString token = call.arguments.value(QStringLiteral("fileIdentityToken")).toString();
    const AgentWritePlan plan = m_files->prepareWrite(context.rootPath, relative, content,
                                                      expected, token);
    if (!plan.ok) return errorResult(plan.error.kind, plan.error.message);
    AgentEgressPolicy::Request request;
    request.kind = AgentEgressPolicy::Kind::ConfigFile;
    request.relativePath = relative;
    request.content = AgentRedaction::redact(plan.newContent).text;
    request.strictMode = true;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(request);
    Pending pending;
    pending.kind = Pending::Kind::Write;
    pending.call = call;
    pending.context = context;
    pending.egress = egressJson(egress, request.kind);
    pending.writePlan = plan;
    pending.content = request.content;
    pending.summary = QStringLiteral("写入 %1（%2 → %3 字节）")
                          .arg(relative)
                          .arg(plan.oldSize)
                          .arg(plan.newContent.toUtf8().size());
    return approvalRequired(pending, pending.summary,
                            AgentRedaction::preview(plan.newContent, 4000),
                            AgentRedaction::redact(plan.diff).text,
                            QStringLiteral("写前备份到维护备份目录（owner=agent，含 sourceSessionId）。"),
                            pending.egress);
}

AgentToolResult AgentFileToolEngine::handleArtifactRead(const Agent::ToolCall &call,
                                                       const AgentToolContext &context) {
    const QString artifactId = call.arguments.value(QStringLiteral("artifactId")).toString();
    const int offset = call.arguments.value(QStringLiteral("offset")).toInt(0);
    const int length = call.arguments.value(QStringLiteral("length")).toInt(65536);
    QString content;
    QString problem;
    if (m_artifacts == nullptr ||
        !m_artifacts->readModel(context.sessionId, artifactId, offset, length, &content,
                                &problem)) {
        return errorResult(Agent::ErrorKind::EgressBlocked,
                           problem.isEmpty() ? QStringLiteral("artifact 不存在或不可读。") : problem);
    }
    AgentEgressPolicy::Request request;
    request.kind = AgentEgressPolicy::Kind::ArtifactRead;
    request.content = content;
    request.strictMode = context.strictEgress || m_strictEgress;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(request);
    if (egress.decision == AgentEgressPolicy::Decision::Block) {
        return errorResult(Agent::ErrorKind::EgressBlocked, egress.reason);
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("读取模型 artifact（已重新脱敏）。");
    result.data = QJsonObject{{QStringLiteral("artifactId"), artifactId},
                              {QStringLiteral("content"), egress.redactedContent},
                              {QStringLiteral("offset"), offset},
                              {QStringLiteral("egress"), egressJson(egress, request.kind)},
                              {QStringLiteral("autoReadNotice"), autoNotice(egress)}};
    return result;
}

AgentToolResult AgentFileToolEngine::approvalRequired(const Pending &pending,
                                                      const QString &summary,
                                                      const QString &preview,
                                                      const QString &diff,
                                                      const QString &backupHint,
                                                      const QJsonObject &egress) {
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const AgentToolSpec spec = AgentToolRegistry::find(pending.call.name);
    QJsonObject request = AgentApprovalPolicy::request(
        pending.call.name, spec.displayName,
        pending.call.arguments.value(QStringLiteral("path")).toString(), summary, preview, diff,
        backupHint, egress);
    request.insert(QStringLiteral("approvalId"), approvalId);
    m_pending.insert(approvalId, pending);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentFileToolEngine::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Error;
        result.summary = QStringLiteral("用户拒绝了本次操作；未写入任何内容。");
        result.error.kind = Agent::ErrorKind::ApprovalDenied;
        result.error.message = result.summary;
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    switch (pending.kind) {
    case Pending::Kind::DirectoryMetadata: {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = pending.directory.summary;
        result.data = QJsonObject{{QStringLiteral("entries"), pending.directory.entryCount},
                                  {QStringLiteral("files"), pending.directory.fileCount},
                                  {QStringLiteral("directories"), pending.directory.directoryCount},
                                  {QStringLiteral("bytes"), pending.directory.totalSize},
                                  {QStringLiteral("truncated"), pending.directory.truncated},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    case Pending::Kind::FileNames: {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = pending.summary;
        result.data = QJsonObject{{QStringLiteral("names"), pending.content},
                                  {QStringLiteral("truncated"), pending.namesTruncated},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    case Pending::Kind::Read: {
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = pending.summary;
        QString content = pending.content;
        QString modelArtifact;
        if (content.toUtf8().size() > 16 * 1024) {
            QString artifactProblem;
            if (m_artifacts != nullptr &&
                m_artifacts->putModelReadable(pending.context.sessionId, content, &modelArtifact,
                                              &artifactProblem)) {
                content = content.left(16 * 1024) + QChar(10) +
                          QStringLiteral("…（本次读取内容见模型可读 artifact，同样已脱敏）");
            }
        }
        result.data = QJsonObject{{QStringLiteral("content"), content},
                                  {QStringLiteral("modelArtifact"), modelArtifact},
                                  {QStringLiteral("sha256"), pending.read.sha256},
                                  {QStringLiteral("fileIdentityToken"), pending.read.identityToken},
                                  {QStringLiteral("size"), pending.read.size},
                                  {QStringLiteral("truncated"), pending.read.truncated},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    case Pending::Kind::Write: {
        m_committing = true;
        const AgentWriteResult write = m_files->commitWrite(
            pending.context.rootPath, pending.writePlan, pending.context.sessionId,
            pending.context.instanceId);
        m_committing = false;
        if (!write.ok) return errorResult(write.error.kind, write.error.message);
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary =
            write.unchanged
                ? QStringLiteral("文件内容未变化，未写入。")
                : QStringLiteral("写入成功：%1 → %2 字节，已备份并校验。")
                      .arg(write.bytesBefore)
                      .arg(write.bytesAfter);
        result.data = QJsonObject{{QStringLiteral("sha256"), write.sha256},
                                  {QStringLiteral("fileIdentityToken"), write.identityToken},
                                  {QStringLiteral("bytesBefore"), write.bytesBefore},
                                  {QStringLiteral("bytesAfter"), write.bytesAfter},
                                  {QStringLiteral("backupId"), write.backupId},
                                  {QStringLiteral("backupDir"), write.backupDir},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    }
    return errorResult(Agent::ErrorKind::Internal, QStringLiteral("未知审批类型。"));
}

void AgentFileToolEngine::cancelPending() { m_pending.clear(); }

bool AgentFileToolEngine::handles(const QString &internalName) const {
    for (const AgentToolSpec &item : AgentToolRegistry::fileBuiltins()) {
        if (item.name == internalName) return true;
    }
    return false;
}
