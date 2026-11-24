/**
 * @file AgentLauncherTools.cpp
 * @brief 启动器工具的执行/审批/投影（见 .hpp）。
 */

#include "AgentLauncherTools.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentDiagnosticProjection.hpp"
#include "AgentRedaction.hpp"

#include "../manage/InstanceFiles.hpp"
#include "../settings/GlobalJavaSettings.hpp"
#include "../util/SystemMemory.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

constexpr int kLogTailLinesDefault = 120;
constexpr int kLogTailLinesMax = 500;
constexpr int kModNameLimit = 2000;
constexpr int kEvidenceExcerpt = 400;

QJsonObject stringField(const QString &type, int maxLength) {
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("maxLength"), maxLength}};
}

QJsonObject enumField(const QStringList &values) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("enum"), QJsonArray::fromStringList(values)}};
}

QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

AgentToolSpec spec(const QString &name, const QString &displayName, const QString &description,
                   AgentRisk risk, const QJsonObject &schema) {
    return AgentToolSpec{name, displayName, description, risk, schema};
}

QJsonArray evidenceJson(const QList<AgentDiagnosticProjection::Evidence> &evidence) {
    QJsonArray array;
    for (const AgentDiagnosticProjection::Evidence &item : evidence) array.append(item.toJson());
    return array;
}

void addEvidence(QList<AgentDiagnosticProjection::Evidence> *evidence, const QString &kind,
                 const QString &source, const QString &excerpt, const QString &sha256 = QString()) {
    AgentDiagnosticProjection::Evidence item;
    item.id = QStringLiteral("E%1").arg(evidence->size() + 1);
    item.kind = kind;
    item.source = source;
    item.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    item.excerpt = excerpt.left(kEvidenceExcerpt);
    item.sha256 = sha256;
    evidence->append(item);
}

/// 写类工具也带一个 egress 字段：审批卡统一显示“外发”那一行，这里说明这次不发文件内容。
QJsonObject writeEgress(const QString &kindKey) {
    return QJsonObject{{QStringLiteral("kind"), kindKey},
                       {QStringLiteral("decision"), QStringLiteral("auto")},
                       {QStringLiteral("reason"), QStringLiteral("写操作审批，不发送文件内容")},
                       {QStringLiteral("redactionHits"), 0}};
}

QString contentDigest(const QString &text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QJsonObject modCounts(int total, int enabled, int disabled, bool truncated) {
    return QJsonObject{{QStringLiteral("mods"), total},
                       {QStringLiteral("enabled"), enabled},
                       {QStringLiteral("disabled"), disabled},
                       {QStringLiteral("truncated"), truncated}};
}

/// 参数 `detail` 只认 summary（默认）/ full。
bool wantsFull(const Agent::ToolCall &call) {
    return call.arguments.value(QStringLiteral("detail")).toString() == QStringLiteral("full");
}

}  // namespace

QList<AgentToolSpec> AgentLauncherTools::specs() {
    QList<AgentToolSpec> list;
    list.append(spec(QStringLiteral("instance.list"), QStringLiteral("列出实例"),
                     QStringLiteral("默认只返回实例总数；detail=full 返回名称/版本/加载器，需要逐次确认，"
                                    "不返回绝对路径与账户信息。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("instance.context"), QStringLiteral("查看锁定实例"),
                     QStringLiteral("返回本会话锁定实例的元数据摘要；未锁定实例时拒绝。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("diagnostics.startup"), QStringLiteral("启动诊断"),
                     QStringLiteral("跑一遍启动诊断；结果按字段白名单投影，不含路径与账户。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("diagnostics.crash"), QStringLiteral("崩溃诊断"),
                     QStringLiteral("读最近一次异常退出事实与规则提示；不猜具体模组。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("logs.tail"), QStringLiteral("看日志结尾"),
                     QStringLiteral("读 latest.log 末尾若干行（最多 500）；已脱敏，不含本机路径。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("lines"),
                                        QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                                    {QStringLiteral("maximum"), kLogTailLinesMax}}}})));
    list.append(spec(QStringLiteral("crash.list"), QStringLiteral("列崩溃报告"),
                     QStringLiteral("默认只给条数与最新一份的概况；detail=full 列文件名，需要逐次确认。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("instance.integrity"), QStringLiteral("完整性体检"),
                     QStringLiteral("按现有完整性判据扫一遍；只给分组计数，不给路径；可能耗时数秒。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("mods.list"), QStringLiteral("列模组"),
                     QStringLiteral("默认只返回数量与启停统计；detail=full 返回完整文件名，逐次确认。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("mods.setEnabled"), QStringLiteral("启停模组"),
                     QStringLiteral("只改扩展名（.jar ↔ .jar.disabled），可原路改回；每次审批。"),
                     AgentRisk::Write,
                     objectSchema({QStringLiteral("fileName"), QStringLiteral("enabled")},
                                  {{QStringLiteral("fileName"),
                                    stringField(QStringLiteral("string"), 260)},
                                   {QStringLiteral("enabled"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}})));
    list.append(spec(QStringLiteral("settings.get"), QStringLiteral("读启动器设置"),
                     QStringLiteral("只返回白名单键的非路径字段。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("settings.set"), QStringLiteral("改启动器设置"),
                     QStringLiteral("只允许白名单键；每次审批，记录旧值与新值。"), AgentRisk::Write,
                     objectSchema({QStringLiteral("key"), QStringLiteral("value")},
                                  {{QStringLiteral("key"), enumField(settingsKeys())},
                                   {QStringLiteral("value"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                {QStringLiteral("maxLength"), 2048}}}})));
    list.append(spec(QStringLiteral("java.list"), QStringLiteral("列本机 Java"),
                     QStringLiteral("只返回主版本等非路径字段。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("memory.suggest"), QStringLiteral("内存建议"),
                     QStringLiteral("按现有纯函数给推荐 Xmx/Xms；只说不写。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("backup.list"), QStringLiteral("列备份"),
                     QStringLiteral("列维护备份的 owner/source/时间/大小，来源会话删除也列得出。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("backup.create"), QStringLiteral("创建备份"),
                     QStringLiteral("备份所选范围；每次审批，owner=agent 写入 maintenance。"),
                     AgentRisk::Write,
                     objectSchema({QStringLiteral("reason")},
                                  {{QStringLiteral("reason"),
                                    stringField(QStringLiteral("string"), 200)},
                                   {QStringLiteral("scopes"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                {QStringLiteral("maxItems"), 4}}}})));
    list.append(spec(QStringLiteral("backup.restore"), QStringLiteral("恢复备份"),
                     QStringLiteral("恢复前先做一份快照；每次审批；共享/外部备份只引用不改写。"),
                     AgentRisk::Destructive,
                     objectSchema({QStringLiteral("backupId")},
                                  {{QStringLiteral("backupId"),
                                    stringField(QStringLiteral("string"), 64)}})));
    list.append(spec(QStringLiteral("upgrade.inspect"), QStringLiteral("升级检查"),
                     QStringLiteral("只读：列出可升级到的目标与兼容性结论，不执行升级。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("upgrade.openWizard"), QStringLiteral("打开升级向导"),
                     QStringLiteral("打开现有升级页，由用户在原界面里选择与确认。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("launcher.openPage"), QStringLiteral("打开启动器页面"),
                     QStringLiteral("只打开已有入口（实例设置/日志/备份/完整性），不执行写操作。"),
                     AgentRisk::ReadOnly,
                     objectSchema({QStringLiteral("page")},
                                  {{QStringLiteral("page"),
                                    enumField({QStringLiteral("instance-settings"),
                                               QStringLiteral("logs"),
                                               QStringLiteral("backups"),
                                               QStringLiteral("integrity")})}})));
    return list;
}

QStringList AgentLauncherTools::settingsKeys() {
    return {QStringLiteral("memory.auto"),
            QStringLiteral("memory.xmx"),
            QStringLiteral("memory.xms"),
            QStringLiteral("java.autoSelect"),
            QStringLiteral("java.path"),
            QStringLiteral("args.extraJvm"),
            QStringLiteral("launch.afterLaunch"),
            QStringLiteral("launch.showGameLog"),
            QStringLiteral("launch.processPriority")};
}

bool AgentLauncherTools::isLauncherTool(const QString &internalName) {
    for (const AgentToolSpec &item : specs()) {
        if (item.name == internalName) return true;
    }
    return false;
}

AgentLauncherTools::AgentLauncherTools(AgentLauncherHost *host, ContextProvider provider)
    : m_host(host), m_provider(std::move(provider)) {}

QJsonArray AgentLauncherTools::schemas() const {
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

bool AgentLauncherTools::handles(const QString &internalName) const {
    return isLauncherTool(internalName);
}

void AgentLauncherTools::cancelPending() { m_pending.clear(); }

QJsonObject AgentLauncherTools::egressJson(const AgentEgressPolicy::Result &result,
                                           const AgentEgressPolicy::Kind kind) const {
    return QJsonObject{{QStringLiteral("kind"), AgentEgressPolicy::kindKey(kind)},
                       {QStringLiteral("decision"), AgentEgressPolicy::decisionKey(result.decision)},
                       {QStringLiteral("reason"), result.reason},
                       {QStringLiteral("redactionHits"), result.redactionHits}};
}

bool AgentLauncherTools::autoNotice(const AgentEgressPolicy::Result &result) const {
    return result.decision == AgentEgressPolicy::Decision::Auto && result.needsNotice;
}

AgentToolResult AgentLauncherTools::errorResult(Agent::ErrorKind kind,
                                                const QString &message) const {
    AgentToolResult result;
    result.status = kind == Agent::ErrorKind::EgressBlocked ? AgentToolResult::Status::Blocked
                                                            : AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

AgentToolResult AgentLauncherTools::autoResult(const QJsonObject &data, const QString &summary,
                                               const QJsonObject &egress, bool notice) {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = summary;
    result.data = data;
    result.data.insert(QStringLiteral("egress"), egress);
    result.data.insert(QStringLiteral("autoReadNotice"), notice);
    return result;
}

AgentToolResult AgentLauncherTools::deliverRead(const AgentToolContext &context,
                                                const Agent::ToolCall &call,
                                                AgentEgressPolicy::Kind kind,
                                                const QString &relative, const QString &content,
                                                const QString &displayName, const QString &summary,
                                                const QString &preview, const QJsonObject &data) {
    AgentEgressPolicy::Request request;
    request.kind = kind;
    request.relativePath = relative;
    request.content = content;
    request.strictMode = context.strictEgress || m_strictEgress;
    const AgentEgressPolicy::Result egress = AgentEgressPolicy::evaluate(request);
    if (egress.decision == AgentEgressPolicy::Decision::Block) {
        return errorResult(Agent::ErrorKind::EgressBlocked, egress.reason);
    }
    const QJsonObject egressObject = egressJson(egress, kind);
    if (egress.decision != AgentEgressPolicy::Decision::Confirm) {
        return autoResult(data, summary, egressObject, autoNotice(egress));
    }
    Pending pending;
    pending.kind = Pending::Kind::Generic;
    pending.call = call;
    pending.context = context;
    pending.egress = egressObject;
    pending.data = data;
    pending.displayName = displayName;
    pending.summary = summary;
    pending.preview = preview;
    return approvalRequired(pending, displayName, summary, preview, QString(),
                            QStringLiteral("不提供会话级放行；每次都要重新确认。"), egressObject);
}

AgentToolResult AgentLauncherTools::approvalRequired(const Pending &pending,
                                                     const QString &displayName,
                                                     const QString &summary,
                                                     const QString &preview, const QString &diff,
                                                     const QString &backupHint,
                                                     const QJsonObject &egress) {
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const AgentToolSpec toolSpec = AgentToolRegistry::find(pending.call.name);
    QJsonObject request = AgentApprovalPolicy::request(
        pending.call.name,
        displayName.isEmpty() ? toolSpec.displayName : displayName,
        pending.call.arguments.value(QStringLiteral("path")).toString().isEmpty()
            ? pending.context.instanceName
            : pending.call.arguments.value(QStringLiteral("path")).toString(),
        summary, preview, diff, backupHint, egress);
    request.insert(QStringLiteral("approvalId"), approvalId);
    m_pending.insert(approvalId, pending);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

// ---------------------------------------------------------------- 只读工具

AgentToolResult AgentLauncherTools::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internal);
    if (!isLauncherTool(internal)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("未知工具。"));
    }
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (m_host == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("启动器工具宿主未接入。"));
    }
    QString contextProblem;
    const AgentToolContext context =
        m_provider != nullptr ? m_provider(&contextProblem) : AgentToolContext{};
    if (!contextProblem.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, contextProblem);
    }
    Agent::ToolCall routed = call;
    routed.name = internal;
    if (internal == QStringLiteral("instance.list")) return handleInstanceList(routed, context);
    if (internal == QStringLiteral("instance.context")) return handleInstanceContext(routed, context);
    if (internal == QStringLiteral("diagnostics.startup")) {
        return handleStartupDiagnosis(routed, context);
    }
    if (internal == QStringLiteral("diagnostics.crash")) return handleCrashDiagnosis(routed, context);
    if (internal == QStringLiteral("logs.tail")) return handleLogsTail(routed, context);
    if (internal == QStringLiteral("crash.list")) return handleCrashList(routed, context);
    if (internal == QStringLiteral("instance.integrity")) return handleIntegrity(routed, context);
    if (internal == QStringLiteral("mods.list")) return handleModsList(routed, context);
    if (internal == QStringLiteral("mods.setEnabled")) return handleModsSetEnabled(routed, context);
    if (internal == QStringLiteral("settings.get")) return handleSettingsGet(routed, context);
    if (internal == QStringLiteral("settings.set")) return handleSettingsSet(routed, context);
    if (internal == QStringLiteral("java.list")) return handleJavaList(routed, context);
    if (internal == QStringLiteral("memory.suggest")) return handleMemorySuggest(routed, context);
    if (internal == QStringLiteral("backup.list")) return handleBackupList(routed, context);
    if (internal == QStringLiteral("backup.create")) return handleBackupCreate(routed, context);
    if (internal == QStringLiteral("backup.restore")) return handleBackupRestore(routed, context);
    if (internal == QStringLiteral("upgrade.inspect")) return handleUpgradeInspect(routed, context);
    if (internal == QStringLiteral("launcher.openPage")) return handleOpenPage(routed, context);
    return errorResult(Agent::ErrorKind::ToolError,
                       QStringLiteral("工具未实现。"));
}

AgentToolResult AgentLauncherTools::handleInstanceList(const Agent::ToolCall &call,
                                                       const AgentToolContext &context) {
    QList<AgentDiagnosticProjection::InstanceFacts> items;
    QString problem;
    if (!m_host->instanceList(&items, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!wantsFull(call)) {
        QJsonObject data{{QStringLiteral("count"), int(items.size())}};
        return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceListSummary, QString(),
                           QString(), QStringLiteral("实例总数"),
                           QStringLiteral("当前游戏文件夹共有 %1 个实例。").arg(items.size()),
                           QString(), data);
    }
    QJsonArray array;
    for (const AgentDiagnosticProjection::InstanceFacts &facts : items) {
        array.append(facts.toJson());
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    QString preview;
    for (const AgentDiagnosticProjection::InstanceFacts &facts : items) {
        preview += QStringLiteral("%1（%2 · %3）\n")
                       .arg(facts.instanceName, facts.gameVersion,
                            facts.loaderName.isEmpty() ? QStringLiteral("原版") : facts.loaderName);
    }
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("instance.list"),
                QStringLiteral("实例清单 %1 项").arg(items.size()));
    const QJsonObject data{{QStringLiteral("count"), int(items.size())},
                           {QStringLiteral("instances"), array},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString content = QString::fromUtf8(
        QJsonDocument(array).toJson(QJsonDocument::Compact));
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceListFull, QString(), content,
                       QStringLiteral("完整实例清单"),
                       QStringLiteral("完整清单：%1 项").arg(items.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleInstanceContext(const Agent::ToolCall &call,
                                                          const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        AgentToolResult result =
            errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
        result.data = QJsonObject{{QStringLiteral("needsInstanceLock"), true}};
        return result;
    }
    AgentDiagnosticProjection::InstanceFacts facts;
    QString problem;
    if (!m_host->instanceFacts(context.instanceId, &facts, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    facts.instanceId = context.instanceId;
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("instance.context"),
                QStringLiteral("实例 %1").arg(facts.instanceName));
    QJsonObject data = facts.toJson();
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QStringLiteral("%1（%2 · %3），Xmx %4 MB")
                                .arg(facts.instanceName, facts.gameVersion,
                                     facts.loaderName.isEmpty() ? QStringLiteral("原版")
                                                                : facts.loaderName)
                                .arg(facts.xmxMb);
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QStringLiteral("锁定实例"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleStartupDiagnosis(const Agent::ToolCall &call,
                                                           const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    StartupDiagnosis::Report report;
    QString problem;
    if (!m_host->startupDiagnosis(context.instanceId, &report, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    AgentDiagnosticProjection::InstanceFacts facts;
    if (!m_host->instanceFacts(context.instanceId, &facts, &problem)) {
        facts.instanceId = context.instanceId;
    }
    facts.instanceId = context.instanceId;
    QList<AgentDiagnosticProjection::Evidence> evidence;
    QJsonObject data =
        AgentDiagnosticProjection::startup(report, facts, &evidence);
    if (!AgentDiagnosticProjection::hasContent(data) && evidence.isEmpty()) {
        // 空结果不许当成功（§9.4.1）：告诉模型“这次没拿到东西”。
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("诊断没有拿到任何可用结论，请让用户打开实例设置自行排查。"));
    }
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QStringLiteral("启动诊断：%1 个问题、%2 条提醒")
                                .arg(report.problems)
                                .arg(report.warnings);
    const QString preview = report.toText();
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QStringLiteral("启动诊断"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleCrashDiagnosis(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    GameExitDiagnosis::Facts facts;
    QStringList hints;
    QString problem;
    if (!m_host->crashFacts(context.instanceId, &facts, &hints, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    AgentDiagnosticProjection::InstanceFacts instance;
    if (!m_host->instanceFacts(context.instanceId, &instance, &problem)) {
        instance.instanceId = context.instanceId;
    }
    instance.instanceId = context.instanceId;
    QList<AgentDiagnosticProjection::Evidence> evidence;
    QJsonObject data = AgentDiagnosticProjection::crash(facts, hints, instance, &evidence);
    if (!GameExitDiagnosis::isAbnormal(facts) && hints.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("最近一次启动是正常退出，没有可诊断的异常。"));
    }
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = GameExitDiagnosis::summary(facts);
    QString preview = summary;
    for (const QString &hint : hints) preview += QLatin1Char('\n') + hint;
    return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, QString(),
                       preview, QStringLiteral("崩溃诊断"), summary, preview, data);
}

AgentToolResult AgentLauncherTools::handleLogsTail(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    const int lines = call.arguments.value(QStringLiteral("lines")).toInt(kLogTailLinesDefault);
    const int bounded = qBound(1, lines, kLogTailLinesMax);
    QString text;
    QString source;
    QString problem;
    if (!m_host->logTail(context.instanceId, bounded, &text, &source, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("log"), source,
                QStringLiteral("%1 末尾 %2 行").arg(source).arg(bounded),
                contentDigest(redacted.text));
    const QJsonObject data{{QStringLiteral("source"), source},
                           {QStringLiteral("lines"), bounded},
                           {QStringLiteral("content"), redacted.text},
                           {QStringLiteral("redactionHits"), redacted.hits},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString summary = QStringLiteral("日志结尾：%1（%2 行）").arg(source).arg(bounded);
    return deliverRead(context, call, AgentEgressPolicy::Kind::Log, source, redacted.text,
                       QStringLiteral("日志结尾"), summary,
                       AgentRedaction::preview(redacted.text, 4000), data);
}

AgentToolResult AgentLauncherTools::handleCrashList(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    QList<AgentHostCrashReport> reports;
    QString problem;
    if (!m_host->crashReports(context.instanceId, &reports, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!wantsFull(call)) {
        QJsonObject newest;
        if (!reports.isEmpty()) {
            const AgentHostCrashReport &latest = reports.first();
            newest = QJsonObject{{QStringLiteral("modifiedAt"),
                                  latest.modifiedAt.toString(Qt::ISODate)},
                                 {QStringLiteral("bytes"), latest.bytes},
                                 {QStringLiteral("head"),
                                  AgentDiagnosticProjection::sanitizeEvidence(latest.headText)}};
        }
        QList<AgentDiagnosticProjection::Evidence> evidence;
        addEvidence(&evidence, QStringLiteral("file"), QStringLiteral("crash-reports"),
                    QStringLiteral("崩溃报告 %1 份").arg(reports.size()));
        const QJsonObject data{{QStringLiteral("count"), int(reports.size())},
                               {QStringLiteral("latest"), newest},
                               {QStringLiteral("evidence"), evidenceJson(evidence)}};
        return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, QString(),
                           newest.value(QStringLiteral("head")).toString(),
                           QStringLiteral("崩溃报告概况"),
                           QStringLiteral("崩溃报告：%1 份").arg(reports.size()),
                           QString(), data);
    }
    QString preview;
    for (const AgentHostCrashReport &report : reports) {
        preview += QStringLiteral("%1 · %2 字节 · %3\n")
                       .arg(report.fileName)
                       .arg(report.bytes)
                       .arg(report.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    QJsonArray names;
    for (const AgentHostCrashReport &report : reports) names.append(report.fileName);
    const QJsonObject data{{QStringLiteral("count"), int(reports.size())},
                           {QStringLiteral("names"), names}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::FileNameList,
                       QStringLiteral("crash-reports"), preview,
                       QStringLiteral("崩溃报告清单"),
                       QStringLiteral("崩溃报告清单：%1 份").arg(reports.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleIntegrity(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    VersionIntegrity::Report report;
    QString problem;
    if (!m_host->integrity(context.instanceId, &report, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    AgentDiagnosticProjection::InstanceFacts facts;
    if (!m_host->instanceFacts(context.instanceId, &facts, &problem)) {
        facts.instanceId = context.instanceId;
    }
    facts.instanceId = context.instanceId;
    QList<AgentDiagnosticProjection::Evidence> evidence;
    QJsonObject data = AgentDiagnosticProjection::integrity(report, facts, &evidence);
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = report.summary();
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QStringLiteral("完整性体检"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleModsList(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    QList<AgentHostModEntry> mods;
    QString problem;
    if (!m_host->mods(context.instanceId, &mods, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    int enabled = 0;
    int disabled = 0;
    for (const AgentHostModEntry &entry : mods) {
        if (entry.enabled) {
            ++enabled;
        } else {
            ++disabled;
        }
    }
    const bool truncated = mods.size() > kModNameLimit;
    if (!wantsFull(call)) {
        QList<AgentDiagnosticProjection::Evidence> evidence;
        addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("mods.list"),
                    QStringLiteral("模组 %1 个（启用 %2、停用 %3）")
                        .arg(mods.size())
                        .arg(enabled)
                        .arg(disabled));
        QJsonObject data = modCounts(mods.size(), enabled, disabled, truncated);
        data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
        return deliverRead(context, call, AgentEgressPolicy::Kind::DirectoryMetadata,
                           QStringLiteral("mods"), QString(), QStringLiteral("模组概况"),
                           QStringLiteral("模组 %1 个：启用 %2、停用 %3")
                               .arg(mods.size())
                               .arg(enabled)
                               .arg(disabled),
                           QString(), data);
    }
    QString preview;
    int count = 0;
    for (const AgentHostModEntry &entry : mods) {
        if (count >= kModNameLimit) break;
        preview += entry.fileName + (entry.enabled ? QString() : QStringLiteral("（已停用）"));
        preview += QLatin1Char('\n');
        ++count;
    }
    QJsonObject data = modCounts(mods.size(), enabled, disabled, truncated);
    data.insert(QStringLiteral("names"), preview);
    return deliverRead(context, call, AgentEgressPolicy::Kind::FileNameList,
                       QStringLiteral("mods"), preview, QStringLiteral("模组清单"),
                       QStringLiteral("模组清单：%1 项%2")
                           .arg(qMin(mods.size(), kModNameLimit))
                           .arg(truncated ? QStringLiteral("（已截断）") : QString()),
                       preview, data);
}

AgentToolResult AgentLauncherTools::handleSettingsGet(const Agent::ToolCall &call,
                                                       const AgentToolContext &context) {
    AgentHostSettings settings;
    QString problem;
    if (!m_host->settings(&settings, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QJsonObject values;
    QString preview;
    for (auto it = settings.values.begin(); it != settings.values.end(); ++it) {
        // 路径类字段一律不外发：`java.path` 只写不读。
        if (it.key() == QStringLiteral("java.path")) continue;
        if (it.value().isString() &&
            AgentDiagnosticProjection::containsLocalPath(it.value().toString())) {
            values.insert(it.key(), QStringLiteral("<本机路径已隐藏>"));
        } else {
            values.insert(it.key(), it.value());
        }
        preview += QStringLiteral("%1 = %2\n")
                       .arg(it.key(), it.value().toVariant().toString());
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("settings.get"),
                QStringLiteral("启动器设置 %1 项").arg(values.size()));
    const QJsonObject data{{QStringLiteral("values"), values},
                           {QStringLiteral("writableKeys"),
                            QJsonArray::fromStringList(settings.writableKeys)},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact)),
                       QStringLiteral("启动器设置"), QStringLiteral("启动器设置当前值"), preview,
                       data);
}

AgentToolResult AgentLauncherTools::handleJavaList(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    QList<int> majors;
    QString problem;
    if (!m_host->javaVersions(&majors, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QJsonArray array;
    for (int major : majors) array.append(major);
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("java.list"),
                QStringLiteral("本机 Java 主版本 %1 个").arg(majors.size()));
    const QJsonObject data{{QStringLiteral("majors"), array},
                           {QStringLiteral("count"), int(majors.size())},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(), QString(),
                       QStringLiteral("本机 Java"),
                       QStringLiteral("本机 Java 主版本：%1 个").arg(majors.size()),
                       QString(), data);
}

AgentToolResult AgentLauncherTools::handleMemorySuggest(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    const SystemMemory::Usage usage = SystemMemory::read();
    int modCount = 0;
    if (!context.instanceId.isEmpty()) {
        AgentDiagnosticProjection::InstanceFacts facts;
        QString problem;
        if (m_host->instanceFacts(context.instanceId, &facts, &problem)) {
            modCount = facts.modCount;
        }
    }
    const int xmx = GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb, modCount);
    const int xms = GlobalJavaSettings::suggestXmsMb(xmx);
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("memory.suggest"),
                QStringLiteral("系统内存 %1 MB、可用 %2 MB，模组 %3 个")
                    .arg(usage.totalMb)
                    .arg(usage.availableMb)
                    .arg(modCount));
    const QJsonObject data{{QStringLiteral("totalMb"), qint64(usage.totalMb)},
                           {QStringLiteral("availableMb"), qint64(usage.availableMb)},
                           {QStringLiteral("suggestedXmxMb"), xmx},
                           {QStringLiteral("suggestedXmsMb"), xms},
                           {QStringLiteral("modCount"), modCount},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(), QString(),
                       QStringLiteral("内存建议"),
                       QStringLiteral("建议 Xmx %1 MB、Xms %2 MB").arg(xmx).arg(xms), QString(),
                       data);
}

AgentToolResult AgentLauncherTools::handleBackupList(const Agent::ToolCall &call,
                                                     const AgentToolContext &context) {
    QList<AgentHostBackupRef> backups;
    QString problem;
    if (!m_host->backups(&backups, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QJsonArray array;
    QString preview;
    for (const AgentHostBackupRef &backup : backups) {
        array.append(QJsonObject{{QStringLiteral("backupId"), backup.backupId},
                                 {QStringLiteral("owner"), backup.owner},
                                 {QStringLiteral("source"), backup.source},
                                 {QStringLiteral("label"), backup.label},
                                 {QStringLiteral("createdAt"), backup.createdAt.toString(Qt::ISODate)},
                                 {QStringLiteral("bytes"), backup.bytes},
                                 {QStringLiteral("sourceSessionId"), backup.sourceSessionId},
                                 {QStringLiteral("restorable"), backup.restorable}});
        preview += QStringLiteral("%1 · %2 · %3 · %4\n")
                       .arg(backup.backupId, backup.owner, backup.label,
                            backup.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("backup.list"),
                QStringLiteral("备份 %1 份").arg(backups.size()));
    const QJsonObject data{{QStringLiteral("count"), int(backups.size())},
                           {QStringLiteral("backups"), array},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(), QString(),
                       QStringLiteral("备份列表"),
                       QStringLiteral("维护备份：%1 份").arg(backups.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleUpgradeInspect(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    QJsonObject data;
    QString problem;
    if (!m_host->upgradeInspect(context.instanceId, &data, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("upgrade.inspect"),
                QStringLiteral("升级目标 %1 个")
                    .arg(data.value(QStringLiteral("targets")).toArray().size()));
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QStringLiteral("升级检查：%1 个候选目标")
                                .arg(data.value(QStringLiteral("targets")).toArray().size());
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QStringLiteral("升级检查"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleOpenPage(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    const QString page = call.arguments.value(QStringLiteral("page")).toString();
    QString problem;
    if (!m_host->openPage(page, context.instanceId, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    return autoResult(QJsonObject{{QStringLiteral("page"), page}},
                      QStringLiteral("已打开：%1").arg(page), QJsonObject(), false);
}

// ---------------------------------------------------------------- 写工具

AgentToolResult AgentLauncherTools::handleSettingsSet(const Agent::ToolCall &call,
                                                      const AgentToolContext &context) {
    const QString key = call.arguments.value(QStringLiteral("key")).toString();
    const QString raw = call.arguments.value(QStringLiteral("value")).toString();
    if (!settingsKeys().contains(key)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("这个设置键不在白名单里。"));
    }
    AgentHostSettings settings;
    QString problem;
    if (!m_host->settings(&settings, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    const QJsonValue oldValue = settings.values.value(key);
    // 值统一按字符串传，类型按当前值（或已知键）决定：布尔/整数/枚举各一档。
    QJsonValue parsed = raw;
    const bool booleanKey = key == QStringLiteral("memory.auto") ||
                            key == QStringLiteral("java.autoSelect") ||
                            key == QStringLiteral("launch.showGameLog");
    if (booleanKey) {
        const QString lowered = raw.trimmed().toLower();
        if (lowered != QStringLiteral("true") && lowered != QStringLiteral("false")) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QStringLiteral("这个键只接受 true/false。"));
        }
        parsed = lowered == QStringLiteral("true");
    } else if (key == QStringLiteral("memory.xmx") || key == QStringLiteral("memory.xms")) {
        bool ok = false;
        const int number = raw.toInt(&ok);
        if (!ok || number < (key == QStringLiteral("memory.xmx") ? 512 : 256) || number > 65536) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QStringLiteral("内存取值不合理（Xmx 512–65536 MB，Xms 256–65536 MB）。"));
        }
        parsed = number;
    }
    Pending pending;
    pending.kind = Pending::Kind::SettingsWrite;
    pending.call = call;
    pending.context = context;
    pending.key = key;
    pending.value = parsed;
    pending.oldValue = oldValue;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    const QString summary = QStringLiteral("修改设置 %1：%2 → %3")
                                .arg(key, oldValue.toVariant().toString(), raw);
    return approvalRequired(pending, QStringLiteral("修改启动器设置"), summary, summary, QString(),
                            QStringLiteral("只改白名单键；失败不会推进“已保存”基线。"), pending.egress);
}

AgentToolResult AgentLauncherTools::handleModsSetEnabled(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    const QString fileName = call.arguments.value(QStringLiteral("fileName")).toString();
    const bool enable = call.arguments.value(QStringLiteral("enabled")).toBool();
    QString problem = InstanceFiles::validateFileName(fileName);
    if (!problem.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!fileName.endsWith(QStringLiteral(".jar")) &&
        !fileName.endsWith(QStringLiteral(".jar.disabled"))) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("只处理 .jar / .jar.disabled 文件。"));
    }
    QList<AgentHostModEntry> mods;
    if (!m_host->mods(context.instanceId, &mods, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    bool found = false;
    for (const AgentHostModEntry &entry : mods) {
        if (entry.fileName == fileName) {
            found = true;
            if (entry.enabled == enable) {
                return errorResult(Agent::ErrorKind::ToolError,
                                   enable ? QStringLiteral("这个模组已经是启用状态。")
                                          : QStringLiteral("这个模组已经是停用状态。"));
            }
        }
    }
    if (!found) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("mods 里没有这个文件。"));
    }
    Pending pending;
    pending.kind = Pending::Kind::ModToggle;
    pending.call = call;
    pending.context = context;
    pending.fileName = fileName;
    pending.enable = enable;
    pending.egress = writeEgress(QStringLiteral("file-name-list"));
    const QString summary = QStringLiteral("%1 模组：%2")
                                .arg(enable ? QStringLiteral("启用") : QStringLiteral("停用"),
                                     fileName);
    return approvalRequired(pending, QStringLiteral("启停模组"), summary, summary, QString(),
                            QStringLiteral("只改扩展名（.jar ↔ .jar.disabled），可原路改回。"),
                            pending.egress);
}

AgentToolResult AgentLauncherTools::handleBackupCreate(const Agent::ToolCall &call,
                                                       const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));
    }
    QStringList scopes;
    for (const QJsonValue &value : call.arguments.value(QStringLiteral("scopes")).toArray()) {
        const QString scope = value.toString();
        if (scope == QStringLiteral("config") || scope == QStringLiteral("mods") ||
            scope == QStringLiteral("options") || scope == QStringLiteral("servers")) {
            scopes.append(scope);
        }
    }
    if (scopes.isEmpty()) scopes = {QStringLiteral("config"), QStringLiteral("options")};
    const QString reason = call.arguments.value(QStringLiteral("reason")).toString();
    AgentHostBackupPlan plan;
    QString problem;
    if (!m_host->planBackup(context.instanceId, scopes, &plan, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (plan.relativePaths.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("所选范围里没有可备份的文件。"));
    }
    Pending pending;
    pending.kind = Pending::Kind::BackupCreate;
    pending.call = call;
    pending.context = context;
    pending.plan = plan;
    pending.reason = reason;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    QString preview = QStringLiteral("范围：%1\n文件：%2 个\n大小：%3 字节\n保存：%4\n")
                          .arg(plan.scope)
                          .arg(plan.relativePaths.size())
                          .arg(plan.bytes)
                          .arg(plan.saveLocation);
    for (const QString &path : plan.relativePaths) preview += QStringLiteral("  %1\n").arg(path);
    const QString summary = QStringLiteral("创建备份：%1（%2 个文件）")
                                .arg(plan.scope)
                                .arg(plan.relativePaths.size());
    return approvalRequired(pending, QStringLiteral("创建维护备份"), summary, preview, QString(),
                            QStringLiteral("owner=agent，写入 repairs 的 maintenance 目录；"
                                           "删除会话不会删掉它。"),
                            pending.egress);
}

AgentToolResult AgentLauncherTools::handleBackupRestore(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    const QString backupId = call.arguments.value(QStringLiteral("backupId")).toString();
    QList<AgentHostBackupRef> backups;
    QString problem;
    if (!m_host->backups(&backups, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    bool found = false;
    AgentHostBackupRef target;
    for (const AgentHostBackupRef &backup : backups) {
        if (backup.backupId == backupId) {
            found = true;
            target = backup;
        }
    }
    if (!found) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("找不到这份备份。"));
    Pending pending;
    pending.kind = Pending::Kind::BackupRestore;
    pending.call = call;
    pending.context = context;
    pending.backup = target;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    const QString summary = QStringLiteral("恢复备份：%1（%2）").arg(target.label, target.backupId);
    return approvalRequired(pending, QStringLiteral("恢复备份"), summary, summary, QString(),
                            QStringLiteral("恢复前会先做一份快照；来源会话已删除也能恢复。"),
                            pending.egress);
}

// ---------------------------------------------------------------- 审批

AgentToolResult AgentLauncherTools::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(
            Agent::ErrorKind::ApprovalDenied, QStringLiteral("用户拒绝了本次操作；未改动任何内容。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    switch (pending.kind) {
    case Pending::Kind::Generic:
        return autoResult(pending.data, pending.summary, pending.egress, false);
    case Pending::Kind::SettingsWrite: {
        QJsonObject applied;
        QString problem;
        if (!m_host->applySetting(pending.key, pending.value, &applied, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        QJsonObject data{{QStringLiteral("key"), pending.key},
                         {QStringLiteral("oldValue"), pending.oldValue},
                         {QStringLiteral("newValue"), pending.value},
                         {QStringLiteral("applied"), applied},
                         {QStringLiteral("egress"), pending.egress}};
        const QString summary =
            QStringLiteral("已写入设置 %1（旧值 %2）。")
                .arg(pending.key, pending.oldValue.toVariant().toString());
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = summary;
        result.data = data;
        return result;
    }
    case Pending::Kind::ModToggle: {
        QString previous;
        QString problem;
        if (!m_host->setModEnabled(pending.context.instanceId, pending.fileName, pending.enable,
                                   &previous, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = QStringLiteral("%1：%2（原文件名 %3）")
                             .arg(pending.enable ? QStringLiteral("已启用")
                                                 : QStringLiteral("已停用"),
                                  pending.fileName, previous);
        result.data = QJsonObject{{QStringLiteral("fileName"), pending.fileName},
                                  {QStringLiteral("enabled"), pending.enable},
                                  {QStringLiteral("previous"), previous},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    case Pending::Kind::BackupCreate: {
        QString backupId;
        QString problem;
        if (!m_host->createBackup(pending.context.sessionId, pending.context.instanceId,
                                  pending.plan, &backupId, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = QStringLiteral("备份完成：%1（%2 个文件）")
                             .arg(backupId)
                             .arg(pending.plan.relativePaths.size());
        result.data = QJsonObject{{QStringLiteral("backupId"), backupId},
                                  {QStringLiteral("scope"), pending.plan.scope},
                                  {QStringLiteral("files"), pending.plan.relativePaths.size()},
                                  {QStringLiteral("bytes"), pending.plan.bytes},
                                  {QStringLiteral("owner"), QStringLiteral("agent")},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    case Pending::Kind::BackupRestore: {
        QString snapshotId;
        QString problem;
        if (!m_host->restoreBackup(pending.context.sessionId, pending.backup.backupId, &snapshotId,
                                   &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = QStringLiteral("已恢复备份 %1（恢复前快照 %2）")
                             .arg(pending.backup.backupId, snapshotId);
        result.data = QJsonObject{{QStringLiteral("backupId"), pending.backup.backupId},
                                  {QStringLiteral("preRestoreBackupId"), snapshotId},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    }
    return errorResult(Agent::ErrorKind::Internal, QStringLiteral("未知审批类型。"));
}
