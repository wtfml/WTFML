/**
 * @file AgentRepairToolEngine.cpp
 * @brief 事务工具的参数、审批与异步交付（见 .hpp）。
 */

#include "AgentRepairToolEngine.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentRepairCases.hpp"
#include "AgentTestLaunchToolEngine.hpp"
#include "AgentWorkspaceStore.hpp"

#include "../util/AppPaths.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

QJsonObject stringField(const QString &type, int maxLength) {
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("maxLength"), maxLength}};
}

QJsonObject integerField(int maximum) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("maximum"), maximum}};
}

QJsonObject enumField(const QStringList &values) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("enum"), QJsonArray::fromStringList(values)}};
}

QJsonObject objectSchema(const QString &required, const QJsonObject &properties) {
    QJsonArray requiredArray;
    if (!required.isEmpty()) requiredArray.append(required);
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), requiredArray},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

AgentToolSpec spec(const QString &name, const QString &displayName, const QString &description,
                   AgentRisk risk, const QJsonObject &schema) {
    return AgentToolSpec{name, displayName, description, risk, schema};
}

QJsonObject localEgress() {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("local")},
                       {QStringLiteral("decision"), QStringLiteral("auto")},
                       {QStringLiteral("reason"), QStringLiteral("Transactions stay inside the local workspace and never send file contents")},
                       {QStringLiteral("redactionHits"), 0}};
}

QStringList parseScopes(const QJsonValue &value, QString *problem) {
    QStringList scopes;
    QStringList parts;
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) {
            if (item.isString()) parts.append(item.toString());
        }
    } else {
        parts = value.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    for (const QString &raw : parts) {
        const QString scope = raw.trimmed();
        if (!AgentRepairTransactionStore::validScope(scope)) {
            if (problem != nullptr) *problem = QStringLiteral("Unknown repair scope: %1").arg(scope);
            return {};
        }
        if (!scopes.contains(scope)) scopes.append(scope);
    }
    if (scopes.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("Select at least one repair scope.");
        return {};
    }
    return scopes;
}

/// 病例库的动作标签有界词表：范围、设置键、改动文件的目录与类别。故意不含自由文本，
/// 相对路径明细只留在本地 JSON 里。
QString casePathClass(const QString &relativePath) {
    if (relativePath.startsWith(QStringLiteral("mods/"))) return QStringLiteral("mods");
    if (relativePath.startsWith(QStringLiteral("config/")) ||
        relativePath.startsWith(QStringLiteral("defaultconfigs/"))) {
        return QStringLiteral("config");
    }
    if (relativePath.contains(QLatin1Char('/'))) return QStringLiteral("sub");
    return QStringLiteral("root");
}

QString caseChangeClass(const AgentRepairFileChange &change) {
    const QString suffix = QStringLiteral(":") + casePathClass(change.relativePath);
    switch (change.kind) {
    case AgentRepairFileChange::Kind::Added:
        return QStringLiteral("added") + suffix;
    case AgentRepairFileChange::Kind::Deleted:
        return QStringLiteral("deleted") + suffix;
    case AgentRepairFileChange::Kind::Modified:
        break;
    }
    return (change.relativePath.endsWith(QStringLiteral(".jar.disabled"))
                ? QStringLiteral("disabled")
                : QStringLiteral("changed")) +
           suffix;
}

QString textPreview(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.read(8192);
    return QString::fromUtf8(bytes);
}

}  // namespace

AgentRepairToolEngine::AgentRepairToolEngine(AgentRepairHost *repairHost,
                                             AgentLaunchHost *launchHost,
                                             ContextProvider provider)
    : m_repairHost(repairHost), m_launchHost(launchHost), m_provider(std::move(provider)) {}

void AgentRepairToolEngine::setHosts(AgentRepairHost *repairHost, AgentLaunchHost *launchHost) {
    m_repairHost = repairHost;
    m_launchHost = launchHost;
}

QList<AgentToolSpec> AgentRepairToolEngine::specs() {
    const QJsonObject beginSchema = objectSchema(
        QStringLiteral("scopes"),
        {{QStringLiteral("scopes"),
          QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                      {QStringLiteral("maxItems"), 5},
                      {QStringLiteral("items"), enumField(AgentRepairTransactionStore::validScopes())}}},
         {QStringLiteral("testWorlds"),
          QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                      {QStringLiteral("maxItems"), 20},
                      {QStringLiteral("items"), stringField(QStringLiteral("string"), 128)}}},
         {QStringLiteral("diskBudgetMb"), integerField(32768)},
         {QStringLiteral("symptomKey"),
          stringField(QStringLiteral("string"), AgentRepairCase::maxSignatureChars())}});
    const QJsonObject txSchema = objectSchema(
        QStringLiteral("transactionId"),
        {{QStringLiteral("transactionId"), stringField(QStringLiteral("string"), 80)}});
    const QJsonObject verifySchema = objectSchema(
        QStringLiteral("transactionId"),
        {{QStringLiteral("transactionId"), stringField(QStringLiteral("string"), 80)},
         {QStringLiteral("timeoutSeconds"), integerField(600)},
         {QStringLiteral("settleSeconds"), integerField(60)},
         {QStringLiteral("milestone"),
          enumField({QStringLiteral("process_started"), QStringLiteral("main_menu"),
                     QStringLiteral("world_loaded")})},
         {QStringLiteral("repeat"), integerField(3)}});
    const QJsonObject finishSchema = objectSchema(
        QStringLiteral("transactionId"),
        {{QStringLiteral("transactionId"), stringField(QStringLiteral("string"), 80)},
         {QStringLiteral("action"),
          enumField({QStringLiteral("commit"), QStringLiteral("rollback")})}});
    return {
        spec(QStringLiteral("repair.begin"), QCoreApplication::translate("AgentRepairToolEngine", "开始修复事务"),
             QStringLiteral("Copy an isolated workspace and record its baseline; subsequent write tools target the workspace via workspaceId."),
             AgentRisk::Write, beginSchema),
        spec(QStringLiteral("repair.diff"), QCoreApplication::translate("AgentRepairToolEngine", "查看事务差异"),
             QStringLiteral("List modified files, hashes, and config previews in the workspace relative to baseline."),
             AgentRisk::ReadOnly, txSchema),
        spec(QStringLiteral("repair.verify"), QCoreApplication::translate("AgentRepairToolEngine", "验证修复事务"),
             QStringLiteral("Run a test launch in the transaction workspace and record milestones, crashes, or timeouts to the transaction log."),
             AgentRisk::Command, verifySchema),
        spec(QStringLiteral("repair.finish"), QCoreApplication::translate("AgentRepairToolEngine", "提交或回滚事务"),
             QStringLiteral("commit creates a backup before replaying diffs; rollback discards the workspace."),
             AgentRisk::Write, finishSchema),
        spec(QStringLiteral("repair.status"), QCoreApplication::translate("AgentRepairToolEngine", "查看修复事务"),
             QStringLiteral("List active/interrupted transactions, status, and verification outcomes; never returns local paths."),
             AgentRisk::ReadOnly, objectSchema(QString(), {})),
    };
}

QJsonArray AgentRepairToolEngine::schemas() const {
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

bool AgentRepairToolEngine::handles(const QString &internalName) const {
    return internalName == QStringLiteral("repair.begin") ||
           internalName == QStringLiteral("repair.diff") ||
           internalName == QStringLiteral("repair.verify") ||
           internalName == QStringLiteral("repair.finish") ||
           internalName == QStringLiteral("repair.status");
}

AgentToolResult AgentRepairToolEngine::errorResult(Agent::ErrorKind kind,
                                                   const QString &message) const {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

void AgentRepairToolEngine::deliver(const Agent::ToolCall &call, const AgentToolResult &result) {
    m_active = false;
    m_activeCall = {};
    if (m_sink) m_sink(call, result);
}

AgentToolResult AgentRepairToolEngine::approvalRequired(const Pending &pending) {
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pending.insert(approvalId, pending);
    QJsonObject request = AgentApprovalPolicy::request(
        pending.call.name, pending.displayName, pending.context.instanceName, pending.summary,
        pending.preview, QString(), QStringLiteral("事务只写隔离工作区；提交前会再核对源文件哈希。"),  // i18n-ignore
        localEgress());
    request.insert(QStringLiteral("approvalId"), approvalId);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = pending.summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentRepairToolEngine::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internal);
    if (!handles(internal) || toolSpec.name.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("未知工具。"));  // i18n-ignore
    }
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QString contextProblem;
    const AgentToolContext context =
        m_provider != nullptr ? m_provider(&contextProblem) : AgentToolContext{};
    if (!contextProblem.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, contextProblem);
    if (internal == QStringLiteral("repair.begin")) return handleBegin(call, context);
    if (internal == QStringLiteral("repair.diff")) return handleDiff(call, context);
    if (internal == QStringLiteral("repair.verify")) return handleVerify(call, context);
    if (internal == QStringLiteral("repair.finish")) return handleFinish(call, context);
    if (internal == QStringLiteral("repair.status")) return handleStatus(call, context);
    return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("工具未实现。"));  // i18n-ignore
}

AgentToolResult AgentRepairToolEngine::handleBegin(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    if (m_active) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("已有事务操作在运行。"));  // i18n-ignore
    if (m_repairHost == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("修复事务宿主未接入。"));  // i18n-ignore
    }
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));  // i18n-ignore
    }
    QString problem;
    QStringList scopes = parseScopes(call.arguments.value(QStringLiteral("scopes")), &problem);
    if (scopes.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, problem);
    if (m_store.activeForInstance(context.instanceId, nullptr, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("这个实例已有活动事务。"));  // i18n-ignore
    }
    const QString symptomKey =
        call.arguments.value(QStringLiteral("symptomKey")).toString().trimmed();
    if (!symptomKey.isEmpty() && !AgentRepairCase::validSignature(symptomKey)) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("symptomKey 只允许字母、数字和 _.:+|-，最多 %1 个字符。")  // i18n-ignore
                               .arg(AgentRepairCase::maxSignatureChars()));
    }
    Pending pending;
    pending.kind = Pending::Kind::Begin;
    pending.call = call;
    pending.context = context;
    pending.scopes = scopes;
    pending.symptomKey = symptomKey;
    pending.displayName = QCoreApplication::translate("AgentRepairToolEngine", "创建修复事务");
    pending.summary = QStringLiteral("为「%1」创建隔离工作区，范围：%2。")  // i18n-ignore
                          .arg(context.instanceName.isEmpty() ? context.instanceId : context.instanceName,
                               scopes.join(QStringLiteral("、")));  // i18n-ignore
    pending.preview = QStringLiteral("提交前需要用 repair.verify 验证；commit 会先备份原文件并核对基线。");  // i18n-ignore
    return approvalRequired(pending);
}

AgentToolResult AgentRepairToolEngine::handleDiff(const Agent::ToolCall &call,
                                                  const AgentToolContext &context) {
    Q_UNUSED(context);
    AgentRepairTransactionRecord record;
    QString problem;
    if (!m_store.load(call.arguments.value(QStringLiteral("transactionId")).toString(), &record,
                      &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (record.state == QStringLiteral("preparing") || record.state == QStringLiteral("copying")) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("事务还在准备工作区。"));  // i18n-ignore
    }
    AgentWorkspaceStore workspaceStore;
    AgentWorkspaceRecord workspace;
    if (!workspaceStore.load(record.workspaceId, &workspace, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    const QString workspaceRoot = workspace.rootPath + QStringLiteral("/versions/") +
                                  workspace.versionId;
    const QList<AgentRepairFileChange> changes =
        AgentRepairTransactionStore::diff(workspaceRoot, record.baseline, record.scopes, &problem);
    if (!problem.isEmpty()) return errorResult(Agent::ErrorKind::ToolError, problem);

    QJsonArray changeArray;
    QJsonArray settings;
    const int limit = 200;
    int previewBudget = 16000;
    for (int i = 0; i < changes.size() && i < limit; ++i) {
        const AgentRepairFileChange &change = changes.at(i);
        QJsonObject item = change.toJson();
        if (previewBudget > 0 &&
            (change.relativePath.endsWith(QStringLiteral(".json")) ||
             change.relativePath.endsWith(QStringLiteral(".txt")) ||
             change.relativePath.endsWith(QStringLiteral(".toml")) ||
             change.relativePath.endsWith(QStringLiteral(".cfg")))) {
            const QString sourcePath = QDir(AppPaths::versionDir(record.instanceId))
                                           .absoluteFilePath(change.relativePath);
            const QString targetPath = QDir(workspaceRoot).absoluteFilePath(change.relativePath);
            if (QFileInfo(sourcePath).size() <= 8192 || !QFileInfo(sourcePath).exists()) {
                const QString before = textPreview(sourcePath).left(2000);
                item.insert(QStringLiteral("beforePreview"), before);
                previewBudget -= before.size();
            }
            if (previewBudget > 0) {
                const QString after = textPreview(targetPath).left(2000);
                item.insert(QStringLiteral("afterPreview"), after);
                previewBudget -= after.size();
            }
        }
        changeArray.append(item);
    }
    for (auto it = record.settings.begin(); it != record.settings.end(); ++it) {
        settings.append(QJsonObject{{QStringLiteral("key"), it.key()},
                                    {QStringLiteral("entry"), it.value()}});
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("事务 %1：%2 个文件改动，%3 个设置改动。")  // i18n-ignore
                         .arg(record.id)
                         .arg(changes.size())
                         .arg(record.settings.size());
    result.data = QJsonObject{
        {QStringLiteral("transactionId"), record.id},
        {QStringLiteral("state"), record.state},
        {QStringLiteral("changes"), changeArray},
        {QStringLiteral("truncated"), changes.size() > limit},
        {QStringLiteral("settings"), settings},
        {QStringLiteral("verification"), record.verification},
        {QStringLiteral("evidence"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("E1")},
                                {QStringLiteral("kind"), QStringLiteral("transaction-diff")},
                                {QStringLiteral("source"), record.id},
                                {QStringLiteral("excerpt"), result.summary}}}},
    };
    return result;
}

AgentToolResult AgentRepairToolEngine::handleVerify(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    Q_UNUSED(context);
    if (m_active) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("已有事务操作在运行。"));  // i18n-ignore
    if (m_launchHost == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("测试启动宿主未接入。"));  // i18n-ignore
    }
    AgentRepairTransactionRecord record;
    QString problem;
    if (!m_store.load(call.arguments.value(QStringLiteral("transactionId")).toString(), &record,
                      &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!record.active()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("事务已经结束，不能重新验证。"));  // i18n-ignore
    }
    AgentTestLaunchOptions options;
    if (!AgentTestLaunchToolEngine::parseOptions(call.arguments, &options, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    m_store.updateState(record.id, QStringLiteral("verifying"), QString(), nullptr);
    m_active = true;
    m_activeCall = call;
    const Agent::ToolCall activeCall = call;
    const QString transactionId = record.id;
    const bool started = m_launchHost->startTestLaunch(
        record.instanceId, record.workspaceId, options,
        [this, activeCall, transactionId](const AgentTestLaunchSummary &summary) {
            if (!m_active) return;
            const QJsonObject verification = AgentTestLaunchToolEngine::summaryJson(summary);
            m_store.setVerification(transactionId, verification, nullptr);
            m_store.updateState(transactionId, summary.ok ? QStringLiteral("ready_to_commit")
                                                          : QStringLiteral("failed"),
                                summary.problem, nullptr);
            recordCaseFor(transactionId, summary.ok,
                          summary.ok ? QStringLiteral("verified")
                                     : QStringLiteral("not_verified"));
            AgentToolResult result;
            result.status = summary.ok ? AgentToolResult::Status::Ok
                                       : AgentToolResult::Status::Error;
            result.summary = summary.ok ? QStringLiteral("事务验证通过。")  // i18n-ignore
                                        : (summary.problem.isEmpty()
                                               ? QStringLiteral("事务验证没有达到目标。")  // i18n-ignore
                                               : summary.problem);
            result.data = QJsonObject{
                {QStringLiteral("transactionId"), transactionId},
                {QStringLiteral("verification"), verification},
                {QStringLiteral("launchId"), summary.launchId},
                {QStringLiteral("workspaceId"), summary.workspaceId},
                {QStringLiteral("evidence"), verification.value(QStringLiteral("evidence"))},
            };
            if (!summary.ok) {
                result.error.kind = Agent::ErrorKind::ToolError;
                result.error.message = result.summary;
                // Auto-safe（D12）：验证失败自动回滚，把回滚结果并进验证结果再交付。
                if (m_autoSafe && m_repairHost != nullptr) {
                    m_active = false;
                    m_autoRollbackBase = result;
                    m_autoRollbackCall = activeCall;
                    QString rollbackProblem;
                    const bool started = m_repairHost->finishTransaction(
                        transactionId, false,
                        [this](const AgentRepairFinishResult &finish) {
                            AgentToolResult merged = m_autoRollbackBase;
                            merged.data.insert(
                                QStringLiteral("autoRollback"),
                                QJsonObject{{QStringLiteral("ok"), finish.ok},
                                            {QStringLiteral("transactionId"), finish.transactionId},
                                            {QStringLiteral("problem"), finish.problem}});
                            merged.data.insert(
                                QStringLiteral("transactionState"),
                                finish.ok ? QStringLiteral("rolled_back") : QStringLiteral("failed"));
                            merged.summary +=
                                finish.ok ? QStringLiteral("已自动回滚事务工作区。")  // i18n-ignore
                                          : QStringLiteral("自动回滚失败，保留现场。");  // i18n-ignore
                            m_autoRollbackBase = {};
                            const Agent::ToolCall call = m_autoRollbackCall;
                            m_autoRollbackCall = {};
                            deliver(call, merged);
                        },
                        &rollbackProblem);
                    if (!started) {
                        AgentToolResult merged = m_autoRollbackBase;
                        merged.data.insert(
                            QStringLiteral("autoRollback"),
                            QJsonObject{{QStringLiteral("ok"), false},
                                        {QStringLiteral("problem"), rollbackProblem}});
                        merged.summary +=
                            QCoreApplication::translate("AgentRepairToolEngine",
                                                        "自动回滚没有开始：%1")  // i18n-ignore
                                .arg(rollbackProblem);
                        m_autoRollbackBase = {};
                        const Agent::ToolCall call = m_autoRollbackCall;
                        m_autoRollbackCall = {};
                        deliver(call, merged);
                    }
                    return;
                }
            }
            deliver(activeCall, result);
        },
        &problem);
    if (!started) {
        m_active = false;
        m_activeCall = {};
        m_store.updateState(transactionId, QStringLiteral("failed"), problem, nullptr);
        return errorResult(Agent::ErrorKind::ToolError,
                           problem.isEmpty() ? QStringLiteral("测试启动没有开始。") : problem);  // i18n-ignore
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("事务验证已开始，等待里程碑或超时…");  // i18n-ignore
    return result;
}

AgentToolResult AgentRepairToolEngine::handleFinish(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    if (m_active) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("已有事务操作在运行。"));  // i18n-ignore
    if (m_repairHost == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("修复事务宿主未接入。"));  // i18n-ignore
    }
    AgentRepairTransactionRecord record;
    QString problem;
    if (!m_store.load(call.arguments.value(QStringLiteral("transactionId")).toString(), &record,
                      &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!record.active()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("事务已经结束。"));  // i18n-ignore
    }
    const QString action = call.arguments.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("commit")) {
        const bool verified = record.verification.value(QStringLiteral("ok")).toBool();
        if (!verified) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QStringLiteral("事务还没有通过的验证；不能提交。"));  // i18n-ignore
        }
        Pending pending;
        pending.kind = Pending::Kind::FinishCommit;
        pending.call = call;
        pending.context = context;
        pending.transactionId = record.id;
        pending.displayName = QCoreApplication::translate("AgentRepairToolEngine", "提交修复事务");
        pending.summary = QStringLiteral("把事务 %1 的差异提交到原实例；提交前逐个核对基线哈希。")  // i18n-ignore
                              .arg(record.id);
        pending.preview = QStringLiteral("先备份原文件，再原子替换；失败自动回滚。");  // i18n-ignore
        return approvalRequired(pending);
    }
    if (action != QStringLiteral("rollback")) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("action 只支持 commit/rollback。"));  // i18n-ignore
    }

    m_active = true;
    m_activeCall = call;
    const Agent::ToolCall activeCall = call;
    const QString transactionId = record.id;
    const bool started = m_repairHost->finishTransaction(
        transactionId, false,
        [this, activeCall, transactionId](const AgentRepairFinishResult &finish) {
            if (!m_active) return;
            AgentToolResult result;
            result.status = finish.ok ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
            result.summary = finish.ok ? QStringLiteral("事务已回滚。")  // i18n-ignore
                                       : (finish.problem.isEmpty() ? QStringLiteral("事务回滚失败。")  // i18n-ignore
                                                                   : finish.problem);
            result.data = QJsonObject{{QStringLiteral("transactionId"), transactionId},
                                      {QStringLiteral("committed"), finish.committed},
                                      {QStringLiteral("changedFiles"), QJsonArray::fromStringList(finish.changedFiles)},
                                      {QStringLiteral("backupIds"), QJsonArray::fromStringList(finish.backupIds)}};
            if (!finish.ok) {
                result.error.kind = Agent::ErrorKind::ToolError;
                result.error.message = result.summary;
            }
            recordCaseFor(transactionId, finish.ok, QStringLiteral("rolled_back"));
            deliver(activeCall, result);
        },
        &problem);
    if (!started) {
        m_active = false;
        m_activeCall = {};
        return errorResult(Agent::ErrorKind::ToolError,
                           problem.isEmpty() ? QStringLiteral("事务回滚没有开始。") : problem);  // i18n-ignore
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("事务回滚已开始…");  // i18n-ignore
    return result;
}

AgentToolResult AgentRepairToolEngine::handleStatus(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    Q_UNUSED(call);
    Q_UNUSED(context);
    QList<AgentRepairTransactionRecord> records;
    QString problem;
    if (!m_store.list(&records, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QJsonArray array;
    int active = 0;
    for (int i = 0; i < records.size() && i < 200; ++i) {
        const auto &record = records.at(i);
        if (record.active()) ++active;
        array.append(QJsonObject{
            {QStringLiteral("transactionId"), record.id},
            {QStringLiteral("instanceId"), record.instanceId},
            {QStringLiteral("workspaceId"), record.workspaceId},
            {QStringLiteral("state"), record.state},
            {QStringLiteral("scopes"), QJsonArray::fromStringList(record.scopes)},
            {QStringLiteral("verificationOk"), record.verification.value(QStringLiteral("ok")).toBool()},
            {QStringLiteral("problem"), record.problem},
            {QStringLiteral("updatedAt"), record.updatedAt.toString(Qt::ISODate)},
        });
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("事务 %1 个，其中活动 %2 个。").arg(records.size()).arg(active);  // i18n-ignore
    result.data = QJsonObject{{QStringLiteral("count"), records.size()},
                              {QStringLiteral("active"), active},
                              {QStringLiteral("transactions"), array}};
    return result;
}

AgentToolResult AgentRepairToolEngine::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));  // i18n-ignore
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(Agent::ErrorKind::ApprovalDenied,
                                             QStringLiteral("用户拒绝了本次事务操作。"));  // i18n-ignore
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    if (pending.kind == Pending::Kind::Begin) {
        m_active = true;
        m_activeCall = pending.call;
        const Agent::ToolCall activeCall = pending.call;
        QString problem;
        const QString symptomKey = pending.symptomKey;
        const bool started = m_repairHost->beginTransaction(
            pending.context.instanceId, pending.scopes,
            [this, activeCall, symptomKey](const AgentRepairBeginResult &begin) {
                if (!m_active) return;
                if (begin.ok && !symptomKey.isEmpty()) {
                    AgentRepairTransactionRecord created;
                    if (m_store.load(begin.transactionId, &created, nullptr)) {
                        created.symptomKey = symptomKey;
                        m_store.save(created, nullptr);
                    }
                }
                AgentToolResult result;
                result.status = begin.ok ? AgentToolResult::Status::Ok
                                         : AgentToolResult::Status::Error;
                result.summary = begin.ok
                                     ? QStringLiteral("事务已创建：%1（工作区 %2）。")  // i18n-ignore
                                           .arg(begin.transactionId, begin.workspaceId)
                                     : (begin.problem.isEmpty() ? QStringLiteral("事务没有创建。")  // i18n-ignore
                                                                : begin.problem);
                result.data = QJsonObject{{QStringLiteral("transactionId"), begin.transactionId},
                                          {QStringLiteral("workspaceId"), begin.workspaceId},
                                          {QStringLiteral("baselineBytes"), begin.baselineBytes}};
                if (!begin.ok) {
                    result.error.kind = Agent::ErrorKind::ToolError;
                    result.error.message = result.summary;
                }
                deliver(activeCall, result);
            },
            &problem);
        if (!started) {
            m_active = false;
            m_activeCall = {};
            return errorResult(Agent::ErrorKind::ToolError,
                               problem.isEmpty() ? QStringLiteral("事务没有开始。") : problem);  // i18n-ignore
        }
        AgentToolResult result;
        result.status = AgentToolResult::Status::Pending;
        result.summary = QStringLiteral("正在复制工作区、记录基线…");  // i18n-ignore
        return result;
    }

    m_active = true;
    m_activeCall = pending.call;
    const Agent::ToolCall activeCall = pending.call;
    const QString transactionId = pending.transactionId;
    QString problem;
    const bool started = m_repairHost->finishTransaction(
        transactionId, true,
        [this, activeCall, transactionId](const AgentRepairFinishResult &finish) {
            if (!m_active) return;
            AgentToolResult result;
            result.status = finish.ok ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
            result.summary = finish.ok ? QStringLiteral("事务已提交。")  // i18n-ignore
                                       : (finish.problem.isEmpty() ? QStringLiteral("事务提交失败。")  // i18n-ignore
                                                                   : finish.problem);
            result.data = QJsonObject{{QStringLiteral("transactionId"), transactionId},
                                      {QStringLiteral("committed"), finish.committed},
                                      {QStringLiteral("changedFiles"), QJsonArray::fromStringList(finish.changedFiles)},
                                      {QStringLiteral("backupIds"), QJsonArray::fromStringList(finish.backupIds)}};
            if (!finish.ok) {
                result.error.kind = Agent::ErrorKind::ToolError;
                result.error.message = result.summary;
            }
            recordCaseFor(transactionId, finish.ok && finish.committed,
                          finish.ok ? QStringLiteral("committed")
                                    : QStringLiteral("commit_failed"));
            deliver(activeCall, result);
        },
        &problem);
    if (!started) {
        m_active = false;
        m_activeCall = {};
        return errorResult(Agent::ErrorKind::ToolError,
                           problem.isEmpty() ? QStringLiteral("事务提交没有开始。") : problem);  // i18n-ignore
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("正在提交事务、备份原文件…");  // i18n-ignore
    return result;
}

void AgentRepairToolEngine::recordCaseFor(const QString &transactionId, bool verified,
                                          const QString &outcome) {
    AgentRepairCase::Store cases;
    if (!cases.enabled()) return;  // 默认关闭：不落盘、不读盘
    AgentRepairTransactionRecord record;
    if (!m_store.load(transactionId, &record, nullptr)) return;
    QString signature = record.symptomKey;
    if (!AgentRepairCase::validSignature(signature)) {
        signature = AgentRepairCase::repairSignature(record.scopes);
    }
    if (!AgentRepairCase::validSignature(signature)) return;

    // 提交/回滚只改结果；动作标签在验证那一步已经记进事务日志。
    if (outcome != QStringLiteral("verified") && outcome != QStringLiteral("not_verified")) {
        cases.markOutcome(signature, record.actions, outcome, nullptr);
        return;
    }

    QStringList files;
    QStringList actions;
    for (const QString &scope : record.scopes) actions.append(QStringLiteral("scope:") + scope);
    for (const QString &key : record.settings.keys()) {
        actions.append(QStringLiteral("setting:") + key);
    }
    AgentWorkspaceStore workspaceStore;
    AgentWorkspaceRecord workspace;
    QString problem;
    if (workspaceStore.load(record.workspaceId, &workspace, &problem)) {
        const QString workspaceRoot =
            workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId;
        const QList<AgentRepairFileChange> changes = AgentRepairTransactionStore::diff(
            workspaceRoot, record.baseline, record.scopes, &problem);
        for (const AgentRepairFileChange &change : changes) {
            files.append(change.relativePath);
            actions.append(QStringLiteral("file:") + caseChangeClass(change));
        }
    }
    actions.removeDuplicates();

    AgentRepairTransactionRecord updated;
    if (m_store.load(transactionId, &updated, nullptr)) {
        updated.symptomKey = signature;
        updated.actions = actions.mid(0, AgentRepairCase::maxActions());
        m_store.save(updated, nullptr);
    }

    AgentRepairCase::Case item;
    item.signature = signature;
    item.actions = updated.actions.isEmpty() ? actions : updated.actions;
    item.files = files;
    item.outcome = outcome;
    item.verified = verified;
    item.attempts = 1;
    item.successes = verified ? 1 : 0;
    cases.record(item, nullptr);
}

void AgentRepairToolEngine::cancelPending() {
    m_pending.clear();
    const Agent::ToolCall call = m_activeCall;
    m_active = false;
    m_activeCall = {};
    if (m_sink && !call.name.isEmpty()) {
        AgentToolResult result = errorResult(Agent::ErrorKind::Cancelled,
                                             QStringLiteral("这次事务操作已取消。"));  // i18n-ignore
        m_sink(call, result);
    }
}
