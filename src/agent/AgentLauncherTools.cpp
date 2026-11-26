/**
 * @file AgentLauncherTools.cpp
 * @brief 启动器工具的执行/审批/投影（见 .hpp）。
 */

#include "AgentLauncherTools.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentArtifactStore.hpp"
#include "AgentRepairCases.hpp"
#include "AgentCrashSnapshot.hpp"
#include "AgentDiagnosticProjection.hpp"
#include "AgentKnowledgeRules.hpp"
#include "AgentLogIndex.hpp"
#include "AgentMixinOwnership.hpp"
#include "AgentRedaction.hpp"

#include "../manage/InstanceFiles.hpp"
#include "AgentRepairTransaction.hpp"
#include "AgentWorkspaceStore.hpp"
#include "../settings/GlobalJavaSettings.hpp"
#include "../util/SystemMemory.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

constexpr int kLogTailLinesDefault = 120;
constexpr int kLogTailLinesMax = 500;
constexpr int kModNameLimit = 2000;
constexpr int kEvidenceExcerpt = 400;
/// 崩溃快照/全文的读盘上限（docs/dev/AGENT_AUTONOMY.md S5.3：报告 8 MB）。
constexpr int kSnapshotReadLimit = 8 * 1024 * 1024;
/// 没有崩溃报告时用的是日志尾，不能把 100 MB 全读进来。
constexpr int kSnapshotLogTailBytes = 512 * 1024;
constexpr int kMixinScanMaxJars = 300;
constexpr int kSegmentLines = 2000;
constexpr int kMaxSegments = 24;
constexpr int kSummaryLineChars = 200;

QJsonObject integerField(int maximum) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("maximum"), maximum}};
}

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
                       {QStringLiteral("reason"), QCoreApplication::translate("AgentLauncherTools", "写操作审批，不发送文件内容。")},
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

QString scrubLine(const QString &text, int *hits) {
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    if (hits != nullptr) *hits += redacted.hits;
    // 路径之外还有账户与硬件痕迹：启动参数、MAC、SID、本机用户名。
    return AgentDiagnosticProjection::scrubLogText(redacted.text);
}

/// 读文件前 @p maxBytes 字节；@p truncated 说明原文件更长（不是读失败）。
bool readCapped(const QString &path, int maxBytes, QString *text, qint64 *bytes, bool *truncated) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (bytes != nullptr) *bytes = file.size();
    QByteArray data = file.read(qint64(maxBytes) + 1);
    if (truncated != nullptr) *truncated = data.size() > maxBytes;
    if (data.size() > maxBytes) data.truncate(maxBytes);
    if (text != nullptr) *text = QString::fromUtf8(data);
    return true;
}

/// 读文件末尾 @p maxBytes 字节（日志可以是几百 MB，不能全读）。
bool readTailCapped(const QString &path, int maxBytes, QString *text, qint64 *bytes,
                    bool *truncated) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const qint64 size = file.size();
    if (bytes != nullptr) *bytes = size;
    if (truncated != nullptr) *truncated = size > maxBytes;
    if (size > maxBytes) file.seek(size - maxBytes);
    if (text != nullptr) *text = QString::fromUtf8(file.readAll());
    return true;
}

const AgentHostLogSource *pickLogSource(const QList<AgentHostLogSource> &sources,
                                       const QString &key) {
    if (sources.isEmpty()) return nullptr;
    if (key.isEmpty() || key == QStringLiteral("auto")) return &sources.constFirst();
    for (const AgentHostLogSource &source : sources) {
        if (source.key == key) return &source;
    }
    return nullptr;
}

/// @p reportId 为空 = 最新一份；非空时只认真实存在的文件名（不做路径拼接）。
bool pickCrashReport(const QList<AgentHostCrashReport> &reports, const QString &reportId,
                     AgentHostCrashReport *out) {
    if (reports.isEmpty()) return false;
    if (reportId.isEmpty()) {
        if (out != nullptr) *out = reports.constFirst();
        return true;
    }
    for (const AgentHostCrashReport &report : reports) {
        if (report.fileName == reportId) {
            if (out != nullptr) *out = report;
            return true;
        }
    }
    return false;
}

}  // namespace

QList<AgentToolSpec> AgentLauncherTools::specs() {
    QList<AgentToolSpec> list;
    list.append(spec(QStringLiteral("instance.list"),
                     QCoreApplication::translate("AgentLauncherTools", "列出实例"),
                     QStringLiteral("默认只返回实例总数；detail=full 返回名称/版本/加载器，需要逐次确认，"
                                    "不返回绝对路径与账户信息。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("instance.context"),
                     QCoreApplication::translate("AgentLauncherTools", "查看锁定实例"),
                     QStringLiteral("返回本会话锁定实例的元数据摘要；未锁定实例时拒绝。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("diagnostics.startup"),
                     QCoreApplication::translate("AgentLauncherTools", "启动诊断"),
                     QStringLiteral("跑一遍启动诊断；结果按字段白名单投影，不含路径与账户。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("diagnostics.crash"),
                     QCoreApplication::translate("AgentLauncherTools", "崩溃诊断"),
                     QStringLiteral("读最近一次异常退出事实与规则提示；不猜具体模组。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("logs.tail"),
                     QCoreApplication::translate("AgentLauncherTools", "看日志结尾"),
                     QStringLiteral("读 latest.log 末尾若干行（最多 500）；已脱敏，不含本机路径。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("lines"),
                                        QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                                    {QStringLiteral("maximum"), kLogTailLinesMax}}}})));
    list.append(spec(QStringLiteral("crash.list"),
                     QCoreApplication::translate("AgentLauncherTools", "列崩溃报告"),
                     QStringLiteral("默认只给条数与最新一份的概况；detail=full 列文件名，需要逐次确认。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("diagnostics.crashSnapshot"),
                     QCoreApplication::translate("AgentLauncherTools", "崩溃快照"),
                     QStringLiteral("解析最新一份崩溃报告（或日志尾）：环境、阶段、异常链、"  // i18n-ignore
                                    "模组嫌疑与命中规则；报告头部提到的模组只算线索。"),  // i18n-ignore
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("reportId"),
                                        stringField(QStringLiteral("string"), 128)}})));
    list.append(spec(QStringLiteral("logs.search"),
                     QCoreApplication::translate("AgentLauncherTools", "搜索日志"),
                     QStringLiteral("按字面量（大小写不敏感）搜日志，带行号与上下文；"  // i18n-ignore
                                    "最多 200 条，路径已脱敏。"),  // i18n-ignore
                     AgentRisk::ReadOnly,
                     objectSchema({QStringLiteral("query")},
                                  {{QStringLiteral("query"), stringField(QStringLiteral("string"), 400)},
                                   {QStringLiteral("source"),
                                    enumField({QStringLiteral("auto"), QStringLiteral("instance"),
                                               QStringLiteral("shared"),
                                               QStringLiteral("launcher")})},
                                   {QStringLiteral("before"), integerField(10)},
                                   {QStringLiteral("after"), integerField(10)},
                                   {QStringLiteral("maxMatches"),
                                    integerField(AgentLogIndex::maxMatchesLimit())}})));
    list.append(spec(QStringLiteral("logs.readRange"),
                     QCoreApplication::translate("AgentLauncherTools", "按行读日志"),
                     QStringLiteral("从第 startLine 行开始读一段日志，最多 500 行；已脱敏。"),  // i18n-ignore
                     AgentRisk::ReadOnly,
                     objectSchema({QStringLiteral("startLine")},
                                  {{QStringLiteral("source"),
                                    enumField({QStringLiteral("auto"), QStringLiteral("instance"),
                                               QStringLiteral("shared"),
                                               QStringLiteral("launcher")})},
                                   {QStringLiteral("startLine"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
                                   {QStringLiteral("lineCount"),
                                    integerField(AgentLogIndex::maxLinesLimit())}})));
    list.append(spec(QStringLiteral("crash.readFull"),
                     QCoreApplication::translate("AgentLauncherTools", "读崩溃报告全文"),
                     QStringLiteral("把整份崩溃报告脱敏后存成 artifact，只返回分段目录与摘要；"  // i18n-ignore
                                    "原文用 artifact.read 按偏移读。"),  // i18n-ignore
                     AgentRisk::ReadOnly,
                     objectSchema({QStringLiteral("reportId")},
                                  {{QStringLiteral("reportId"),
                                    stringField(QStringLiteral("string"), 128)}})));
    list.append(spec(QStringLiteral("instance.integrity"),
                     QCoreApplication::translate("AgentLauncherTools", "完整性检查"),
                     QStringLiteral("按现有完整性判据扫一遍；只给分组计数，不给路径；可能耗时数秒。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("mods.list"),
                     QCoreApplication::translate("AgentLauncherTools", "列模组"),
                     QStringLiteral("默认只返回数量与启停统计；detail=full 返回完整文件名，逐次确认。"),
                     AgentRisk::ReadOnly,
                     objectSchema({}, {{QStringLiteral("detail"),
                                        enumField({QStringLiteral("summary"),
                                                   QStringLiteral("full")})}})));
    list.append(spec(QStringLiteral("mods.setEnabled"),
                     QCoreApplication::translate("AgentLauncherTools", "启停模组"),
                     QStringLiteral("只改扩展名（.jar ↔ .jar.disabled），可原路改回；每次审批。"),
                     AgentRisk::Write,
                     objectSchema({QStringLiteral("fileName"), QStringLiteral("enabled")},
                                  {{QStringLiteral("fileName"),
                                    stringField(QStringLiteral("string"), 260)},
                                   {QStringLiteral("enabled"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
                                   {QStringLiteral("workspaceId"),
                                    stringField(QStringLiteral("string"), 80)}})));
    list.append(spec(QStringLiteral("settings.get"),
                     QCoreApplication::translate("AgentLauncherTools", "读启动器设置"),
                     QStringLiteral("只返回白名单键的非路径字段。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("settings.set"),
                     QCoreApplication::translate("AgentLauncherTools", "改启动器设置"),
                     QStringLiteral("只允许白名单键；每次审批，记录旧值与新值。"), AgentRisk::Write,
                     objectSchema({QStringLiteral("key"), QStringLiteral("value")},
                                  {{QStringLiteral("key"), enumField(settingsKeys())},
                                   {QStringLiteral("value"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                {QStringLiteral("maxLength"), 2048}}},
                                   {QStringLiteral("workspaceId"),
                                    stringField(QStringLiteral("string"), 80)}})));
    list.append(spec(QStringLiteral("java.list"),
                     QCoreApplication::translate("AgentLauncherTools", "列本机 Java"),
                     QStringLiteral("只返回主版本等非路径字段。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("memory.suggest"),
                     QCoreApplication::translate("AgentLauncherTools", "内存建议"),
                     QStringLiteral("按现有纯函数给推荐 Xmx/Xms；只说不写。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("backup.list"),
                     QCoreApplication::translate("AgentLauncherTools", "列备份"),
                     QStringLiteral("列维护备份的 owner/source/时间/大小，来源会话删除也列得出。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("backup.create"),
                     QCoreApplication::translate("AgentLauncherTools", "创建备份"),
                     QStringLiteral("备份所选范围；每次审批，owner=agent 写入 maintenance。"),
                     AgentRisk::Write,
                     objectSchema({QStringLiteral("reason")},
                                  {{QStringLiteral("reason"),
                                    stringField(QStringLiteral("string"), 200)},
                                   {QStringLiteral("scopes"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                {QStringLiteral("maxItems"), 4}}}})));
    list.append(spec(QStringLiteral("backup.restore"),
                     QCoreApplication::translate("AgentLauncherTools", "恢复备份"),
                     QStringLiteral("恢复前先做一份快照；每次审批；共享/外部备份只引用不改写。"),
                     AgentRisk::Destructive,
                     objectSchema({QStringLiteral("backupId")},
                                  {{QStringLiteral("backupId"),
                                    stringField(QStringLiteral("string"), 64)}})));
    list.append(spec(QStringLiteral("upgrade.inspect"),
                     QCoreApplication::translate("AgentLauncherTools", "升级检查"),
                     QStringLiteral("只读：列出可升级到的目标与兼容性结论，不执行升级。"),
                     AgentRisk::ReadOnly, objectSchema({}, {})));
    list.append(spec(QStringLiteral("upgrade.openWizard"),
                     QCoreApplication::translate("AgentLauncherTools", "打开升级向导"),
                     QStringLiteral("打开现有升级页，由用户在原界面里选择与确认。"), AgentRisk::ReadOnly,
                     objectSchema({}, {})));
    list.append(spec(QStringLiteral("launcher.openPage"),
                     QCoreApplication::translate("AgentLauncherTools", "打开启动器页面"),
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
                            QCoreApplication::translate("AgentLauncherTools", "不提供本会话放行，每次均需重新确认。"), egressObject);
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
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "未知工具。"));
    }
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (m_host == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "启动器工具宿主未接入。"));
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
    if (internal == QStringLiteral("logs.search")) return handleLogsSearch(routed, context);
    if (internal == QStringLiteral("logs.readRange")) return handleLogsReadRange(routed, context);
    if (internal == QStringLiteral("crash.list")) return handleCrashList(routed, context);
    if (internal == QStringLiteral("diagnostics.crashSnapshot")) {
        return handleCrashSnapshot(routed, context);
    }
    if (internal == QStringLiteral("crash.readFull")) return handleCrashReadFull(routed, context);
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
                       QCoreApplication::translate("AgentLauncherTools", "工具未实现。"));
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
                           QString(), QCoreApplication::translate("AgentLauncherTools", "实例总数"),
                           QCoreApplication::translate("AgentLauncherTools", "当前游戏目录共有 %1 个实例。").arg(items.size()),
                           QString(), data);
    }
    QJsonArray array;
    for (const AgentDiagnosticProjection::InstanceFacts &facts : items) {
        array.append(facts.toJson());
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    QString preview;
    for (const AgentDiagnosticProjection::InstanceFacts &facts : items) {
        preview += QCoreApplication::translate("AgentLauncherTools", "%1（%2 · %3）")
                       .arg(facts.instanceName, facts.gameVersion,
                            facts.loaderName.isEmpty()
                                ? QCoreApplication::translate("AgentLauncherTools", "原版")
                                : facts.loaderName) +
                   QLatin1Char('\n');
    }
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("instance.list"),
                QStringLiteral("%1 instance(s)").arg(items.size()));
    const QJsonObject data{{QStringLiteral("count"), int(items.size())},
                           {QStringLiteral("instances"), array},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString content = QString::fromUtf8(
        QJsonDocument(array).toJson(QJsonDocument::Compact));
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceListFull, QString(), content,
                       QCoreApplication::translate("AgentLauncherTools", "完整实例清单"),
                       QCoreApplication::translate("AgentLauncherTools", "完整清单：%1 项").arg(items.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleInstanceContext(const Agent::ToolCall &call,
                                                          const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        AgentToolResult result =
            errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                QStringLiteral("Instance %1").arg(facts.instanceName));
    QJsonObject data = facts.toJson();
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "%1（%2 · %3），Xmx %4 MB")
                                .arg(facts.instanceName, facts.gameVersion,
                                     facts.loaderName.isEmpty() ? QCoreApplication::translate("AgentLauncherTools", "原版")
                                                                : facts.loaderName)
                                .arg(facts.xmxMb);
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QCoreApplication::translate("AgentLauncherTools", "锁定实例"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleStartupDiagnosis(const Agent::ToolCall &call,
                                                           const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                           QCoreApplication::translate("AgentLauncherTools", "诊断未得出可用结论，建议打开实例设置自行排查。"));
    }
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "启动诊断：%1 个问题、%2 条提醒")
                                .arg(report.problems)
                                .arg(report.warnings);
    const QString preview = report.toText();
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QCoreApplication::translate("AgentLauncherTools", "启动诊断"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleCrashDiagnosis(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                           QCoreApplication::translate("AgentLauncherTools", "最近一次启动正常退出，没有可诊断的异常。"));
    }
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = GameExitDiagnosis::summary(facts);
    QString preview = summary;
    for (const QString &hint : hints) preview += QLatin1Char('\n') + hint;
    return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, QString(),
                       preview, QCoreApplication::translate("AgentLauncherTools", "崩溃诊断"), summary, preview, data);
}

AgentToolResult AgentLauncherTools::handleLogsTail(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                QStringLiteral("Last %2 line(s) of %1").arg(source).arg(bounded),
                contentDigest(redacted.text));
    const QJsonObject data{{QStringLiteral("source"), source},
                           {QStringLiteral("lines"), bounded},
                           {QStringLiteral("content"), redacted.text},
                           {QStringLiteral("redactionHits"), redacted.hits},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "日志结尾：%1（%2 行）").arg(source).arg(bounded);
    return deliverRead(context, call, AgentEgressPolicy::Kind::Log, source, redacted.text,
                       QCoreApplication::translate("AgentLauncherTools", "日志结尾"), summary,
                       AgentRedaction::preview(redacted.text, 4000), data);
}

AgentToolResult AgentLauncherTools::handleCrashList(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                    QStringLiteral("%1 crash report(s)").arg(reports.size()));
        const QJsonObject data{{QStringLiteral("count"), int(reports.size())},
                               {QStringLiteral("latest"), newest},
                               {QStringLiteral("evidence"), evidenceJson(evidence)}};
        return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, QString(),
                           newest.value(QStringLiteral("head")).toString(),
                           QCoreApplication::translate("AgentLauncherTools", "崩溃报告概况"),
                           QCoreApplication::translate("AgentLauncherTools", "崩溃报告：%1 份").arg(reports.size()),
                           QString(), data);
    }
    QString preview;
    for (const AgentHostCrashReport &report : reports) {
        preview += QCoreApplication::translate("AgentLauncherTools", "%1 · %2 字节 · %3")
                       .arg(report.fileName)
                       .arg(report.bytes)
                       .arg(report.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"))) +
                   QLatin1Char('\n');
    }
    QJsonArray names;
    for (const AgentHostCrashReport &report : reports) names.append(report.fileName);
    const QJsonObject data{{QStringLiteral("count"), int(reports.size())},
                           {QStringLiteral("names"), names}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::FileNameList,
                       QStringLiteral("crash-reports"), preview,
                       QCoreApplication::translate("AgentLauncherTools", "崩溃报告清单"),
                       QCoreApplication::translate("AgentLauncherTools", "崩溃报告清单：%1 份").arg(reports.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleLogsSearch(const Agent::ToolCall &call,
                                                     const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "会话还没有锁定实例。"));
    }
    QList<AgentHostLogSource> sources;
    QString problem;
    if (!m_host->logSources(context.instanceId, &sources, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    const AgentHostLogSource *source =
        pickLogSource(sources, call.arguments.value(QStringLiteral("source")).toString());
    if (source == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "没有这份日志，或实例还没有日志文件。"));
    }
    AgentLogIndex::SearchOptions options;
    options.query = call.arguments.value(QStringLiteral("query")).toString();
    options.before = call.arguments.value(QStringLiteral("before")).toInt(0);
    options.after = call.arguments.value(QStringLiteral("after")).toInt(0);
    options.maxMatches = call.arguments.value(QStringLiteral("maxMatches")).toInt(50);
    AgentLogIndex::SearchResult found;
    if (!AgentLogIndex::search(source->path, options, &found, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    int redactionHits = 0;
    QJsonArray matches;
    QString preview;
    for (const AgentLogIndex::Match &match : found.matches) {
        QJsonArray contextLines;
        for (const AgentLogIndex::Line &line : match.context) {
            contextLines.append(QJsonObject{
                {QStringLiteral("line"), line.number},
                {QStringLiteral("text"), scrubLine(line.text, &redactionHits)}});
        }
        const QString text = scrubLine(match.text, &redactionHits);
        matches.append(QJsonObject{{QStringLiteral("line"), match.line},
                                   {QStringLiteral("text"), text},
                                   {QStringLiteral("context"), contextLines}});
        preview += QStringLiteral("%1: %2\n").arg(match.line).arg(text);
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("log"), source->label,
                QStringLiteral("%1 search match(es)").arg(found.totalMatches),
                contentDigest(preview));
    const QJsonObject data{
        {QStringLiteral("source"), source->label},
        {QStringLiteral("query"), options.query},
        {QStringLiteral("totalMatches"), found.totalMatches},
        {QStringLiteral("returned"), int(found.matches.size())},
        {QStringLiteral("maxMatches"), options.maxMatches},
        {QStringLiteral("bytes"), double(found.bytes)},
        {QStringLiteral("scannedLines"), found.scannedLines},
        {QStringLiteral("truncated"), found.truncated},
        {QStringLiteral("budgetExceeded"), found.budgetExceeded},
        {QStringLiteral("redactionHits"), redactionHits},
        {QStringLiteral("matches"), matches},
        {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "日志搜索：命中 %1 处（返回 %2 处）")
                                .arg(found.totalMatches)
                                .arg(found.matches.size());
    return deliverRead(context, call, AgentEgressPolicy::Kind::Log, source->label, preview,
                       QCoreApplication::translate("AgentLauncherTools", "搜索日志"), summary,
                       AgentRedaction::preview(preview, 4000), data);
}

AgentToolResult AgentLauncherTools::handleLogsReadRange(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "会话还没有锁定实例。"));
    }
    QList<AgentHostLogSource> sources;
    QString problem;
    if (!m_host->logSources(context.instanceId, &sources, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    const AgentHostLogSource *source =
        pickLogSource(sources, call.arguments.value(QStringLiteral("source")).toString());
    if (source == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "没有这份日志，或实例还没有日志文件。"));
    }
    const int startLine = call.arguments.value(QStringLiteral("startLine")).toInt();
    if (startLine < 1) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "startLine 从 1 开始。"));
    }
    const int lineCount = call.arguments.value(QStringLiteral("lineCount")).toInt(200);
    AgentLogIndex::LineRange range;
    if (!AgentLogIndex::readRange(source->path, startLine, lineCount,
                                  AgentLogIndex::maxCharsLimit(), &range, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    int redactionHits = 0;
    QJsonArray lines;
    QString preview;
    for (const AgentLogIndex::Line &line : range.lines) {
        const QString text = scrubLine(line.text, &redactionHits);
        lines.append(QJsonObject{{QStringLiteral("line"), line.number},
                                 {QStringLiteral("text"), text}});
        preview += text;
        preview += QLatin1Char('\n');
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("log"), source->label,
                QStringLiteral("Lines %1-%2 (total %3)")
                    .arg(range.firstLine)
                    .arg(range.lastLine)
                    .arg(range.totalLines),
                contentDigest(preview));
    const QJsonObject data{
        {QStringLiteral("source"), source->label},
        {QStringLiteral("firstLine"), range.firstLine},
        {QStringLiteral("lastLine"), range.lastLine},
        {QStringLiteral("totalLines"), range.totalLines},
        {QStringLiteral("truncated"), range.truncated},
        {QStringLiteral("redactionHits"), redactionHits},
        {QStringLiteral("lines"), lines},
        {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "日志第 %1–%2 行（共 %3 行）")
                                .arg(range.firstLine)
                                .arg(range.lastLine)
                                .arg(range.totalLines);
    return deliverRead(context, call, AgentEgressPolicy::Kind::Log, source->label, preview,
                       QCoreApplication::translate("AgentLauncherTools", "按行读日志"), summary,
                       AgentRedaction::preview(preview, 4000), data);
}

AgentToolResult AgentLauncherTools::handleCrashSnapshot(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "会话还没有锁定实例。"));
    }
    QString problem;
    AgentDiagnosticProjection::InstanceFacts facts;
    if (!m_host->instanceFacts(context.instanceId, &facts, &problem)) {
        facts.instanceId = context.instanceId;
    }
    facts.instanceId = context.instanceId;

    const QString requestedId = call.arguments.value(QStringLiteral("reportId")).toString();
    QList<AgentHostCrashReport> reports;
    m_host->crashReports(context.instanceId, &reports, &problem);
    AgentHostCrashReport report;
    QString text;
    QString label;
    QString sourceKind;
    QString reportId;
    qint64 bytes = 0;
    bool truncated = false;
    if (pickCrashReport(reports, requestedId, &report)) {
        if (!readCapped(report.path, kSnapshotReadLimit, &text, &bytes, &truncated)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "崩溃报告读不动。"));
        }
        label = report.label;
        sourceKind = QStringLiteral("crash-report");
        reportId = report.fileName;
    } else if (!requestedId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "找不到这份崩溃报告；先用 crash.list 看有哪些。"));
    } else {
        // 没有崩溃报告也可能崩过（进程直接死掉）：退回日志尾，并标明来源是日志。
        QList<AgentHostLogSource> sources;
        if (!m_host->logSources(context.instanceId, &sources, &problem) || sources.isEmpty()) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "没有崩溃报告，也没有可读的游戏日志。"));
        }
        const AgentHostLogSource &source = sources.constFirst();
        if (!readTailCapped(source.path, kSnapshotLogTailBytes, &text, &bytes, &truncated)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "日志读不动。"));
        }
        label = source.label;
        sourceKind = QStringLiteral("game-log");
    }

    AgentCrashSnapshot::SnapshotInput input;
    input.parsed = AgentCrashSnapshot::parse(text, truncated);
    input.rules = AgentKnowledge::loadCrashRules();
    input.fallback.minecraftVersion = facts.gameVersion;
    input.fallback.loader = facts.loaderName;
    input.fallback.loaderVersion = facts.loaderVersion;
    input.fallback.javaMajor = facts.usedJavaMajor;
    input.sourceKind = sourceKind;
    input.sourceLabel = label;
    input.reportId = reportId;
    input.bytes = bytes;
    for (const AgentCrashSnapshot::ModEntry &entry : input.parsed.modList) {
        input.modVersions.insert(entry.modId.toLower(), entry.version);
    }
    // 只在文本真的点名了 mixin 配置时才扫 JAR：那是这个工具里唯一要起外部进程的一步。
    if (!input.parsed.mixinConfigs.isEmpty()) {
        QString modsPath;
        QString mixinProblem;
        if (m_host->modsDirectory(context.instanceId, &modsPath, &mixinProblem)) {
            int skipped = 0;
            const QList<AgentMixinOwnership::Entry> owners =
                AgentMixinOwnership::scan(modsPath, kMixinScanMaxJars, &skipped, &mixinProblem);
            for (const AgentMixinOwnership::Entry &entry : owners) {
                if (!entry.modId.isEmpty()) input.mixinOwners.insert(entry.config, entry.modId);
            }
        }
    }
    const AgentCrashSnapshot::Snapshot snapshot = AgentCrashSnapshot::build(input);
    QJsonObject data = snapshot.json;
    const QJsonObject crash = data.value(QStringLiteral("crash")).toObject();
    const QString exception = crash.value(QStringLiteral("exception")).toString();
    const QString phase = crash.value(QStringLiteral("phase")).toString();
    // 症状键：结构字段拼出来的本地键，喂给 repair.begin 后能把结果记进病例库。
    const QString signature = AgentRepairCase::crashSignature(exception, phase,
                                                             snapshot.matchedRuleIds);
    if (!signature.isEmpty()) data.insert(QStringLiteral("signature"), signature);
    // 病例库默认关闭；开着时只给排在前面的几条动作标签与结果统计，不给原文和路径。
    AgentRepairCase::Store cases;
    QJsonArray priorCases;
    for (const AgentRepairCase::Entry &entry : cases.ranked(signature, 3)) {
        priorCases.append(entry.toModelJson());
    }
    if (!priorCases.isEmpty()) data.insert(QStringLiteral("priorCases"), priorCases);
    const QString summary =
        QCoreApplication::translate("AgentLauncherTools", "崩溃快照：%1，阶段 %2，命中 %3 条规则，嫌疑 %4 个")
            .arg(exception.isEmpty()
                     ? QCoreApplication::translate("AgentLauncherTools", "未识别异常")
                     : exception,
                 phase)
            .arg(snapshot.matchedRuleIds.size())
            .arg(data.value(QStringLiteral("suspects")).toArray().size());
    const QString content = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, label, content,
                       QCoreApplication::translate("AgentLauncherTools", "崩溃快照"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleCrashReadFull(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "会话还没有锁定实例。"));
    }
    const QString reportId = call.arguments.value(QStringLiteral("reportId")).toString();
    QList<AgentHostCrashReport> reports;
    QString problem;
    if (!m_host->crashReports(context.instanceId, &reports, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    AgentHostCrashReport report;
    if (reportId.isEmpty() || !pickCrashReport(reports, reportId, &report)) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "找不到这份崩溃报告；先用 crash.list 看有哪些。"));
    }
    QString raw;
    qint64 bytes = 0;
    bool truncated = false;
    if (!readCapped(report.path, kSnapshotReadLimit, &raw, &bytes, &truncated)) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "崩溃报告读不动。"));
    }
    // 本机路径、账户与硬件痕迹先换掉再进 artifact：artifact.read 只做凭据遮挡，不管这些。
    const QString text = AgentDiagnosticProjection::scrubLogText(raw);
    AgentArtifactStore artifacts;
    QString artifactId;
    QString artifactProblem;
    if (!artifacts.putModelReadable(context.sessionId, text, &artifactId, &artifactProblem)) {
        return errorResult(Agent::ErrorKind::ToolError, artifactProblem);
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    QJsonArray segments;
    QString firstSummary;
    int offset = 0;
    int segmentIndex = 0;
    for (int start = 0; start < lines.size() && segmentIndex < kMaxSegments;) {
        const int end = qMin(start + kSegmentLines, int(lines.size()));
        QStringList picked;
        for (int i = start; i < end && picked.size() < 2; ++i) {
            const QString trimmed = lines.at(i).trimmed();
            if (trimmed.isEmpty()) continue;
            const bool heading = trimmed.startsWith(QStringLiteral("--")) ||
                                 trimmed.startsWith(QStringLiteral("Caused by:")) ||
                                 trimmed.startsWith(QStringLiteral("Description:")) ||
                                 trimmed.contains(QStringLiteral("Exception")) ||
                                 trimmed.contains(QStringLiteral("Error"));
            if (heading) {
                picked.append(AgentRedaction::redact(trimmed).text.left(kSummaryLineChars));
            }
        }
        for (int i = start; i < end && picked.size() < 2; ++i) {
            const QString trimmed = lines.at(i).trimmed();
            if (!trimmed.isEmpty()) {
                picked.append(AgentRedaction::redact(trimmed).text.left(kSummaryLineChars));
            }
        }
        const QString segmentSummary = picked.join(QStringLiteral(" / "));
        if (firstSummary.isEmpty()) firstSummary = segmentSummary;
        int segmentChars = 0;
        for (int i = start; i < end; ++i) segmentChars += lines.at(i).size() + 1;
        segments.append(QJsonObject{{QStringLiteral("index"), segmentIndex + 1},
                                    {QStringLiteral("firstLine"), start + 1},
                                    {QStringLiteral("lastLine"), end},
                                    {QStringLiteral("artifactOffset"), offset},
                                    {QStringLiteral("summary"), segmentSummary}});
        offset += segmentChars;
        start = end;
        ++segmentIndex;
    }
    const bool segmentsTruncated =
        segmentIndex >= kMaxSegments && segmentIndex * kSegmentLines < lines.size();
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("crash-report"), report.label, firstSummary,
                contentDigest(text));
    const QJsonObject data{{QStringLiteral("reportId"), report.fileName},
                           {QStringLiteral("source"), report.label},
                           {QStringLiteral("bytes"), double(bytes)},
                           {QStringLiteral("totalLines"), int(lines.size())},
                           {QStringLiteral("truncated"), truncated || segmentsTruncated},
                           {QStringLiteral("artifactId"), artifactId},
                           {QStringLiteral("segments"), segments},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    const QString summary =
        QCoreApplication::translate("AgentLauncherTools", "崩溃报告全文：%1 行、%2 段（原文已脱敏进 artifact）")
                                .arg(lines.size())
                                .arg(segments.size());
    return deliverRead(context, call, AgentEgressPolicy::Kind::CrashReport, report.label,
                       firstSummary, QCoreApplication::translate("AgentLauncherTools", "崩溃报告全文"), summary,
        summary, data);
}

AgentToolResult AgentLauncherTools::handleIntegrity(const Agent::ToolCall &call,
                                                    const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                       QCoreApplication::translate("AgentLauncherTools", "完整性检查"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleModsList(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                    QStringLiteral("%1 mod(s) (%2 enabled, %3 disabled)")
                        .arg(mods.size())
                        .arg(enabled)
                        .arg(disabled));
        QJsonObject data = modCounts(mods.size(), enabled, disabled, truncated);
        data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
        return deliverRead(context, call, AgentEgressPolicy::Kind::DirectoryMetadata,
                           QStringLiteral("mods"), QString(), QCoreApplication::translate("AgentLauncherTools", "模组概况"),
                           QCoreApplication::translate("AgentLauncherTools", "模组 %1 个：已启用 %2 个，已禁用 %3 个")
                               .arg(mods.size())
                               .arg(enabled)
                               .arg(disabled),
                           QString(), data);
    }
    QString preview;
    int count = 0;
    for (const AgentHostModEntry &entry : mods) {
        if (count >= kModNameLimit) break;
        preview += entry.enabled ? entry.fileName
                                 : QCoreApplication::translate("AgentLauncherTools", "%1（已禁用）").arg(entry.fileName);
        preview += QLatin1Char('\n');
        ++count;
    }
    QJsonObject data = modCounts(mods.size(), enabled, disabled, truncated);
    data.insert(QStringLiteral("names"), preview);
    return deliverRead(context, call, AgentEgressPolicy::Kind::FileNameList,
                       QStringLiteral("mods"), preview, QCoreApplication::translate("AgentLauncherTools", "模组清单"),
                       (truncated ? QCoreApplication::translate("AgentLauncherTools", "模组清单：%1 项（已截断）")
                                  : QCoreApplication::translate("AgentLauncherTools", "模组清单：%1 项"))
                           .arg(qMin(mods.size(), kModNameLimit)),
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
            values.insert(it.key(), QStringLiteral("<local path hidden>"));
        } else {
            values.insert(it.key(), it.value());
        }
        preview += QStringLiteral("%1 = %2\n")
                       .arg(it.key(), it.value().toVariant().toString());
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("settings.get"),
                QStringLiteral("%1 launcher setting(s)").arg(values.size()));
    const QJsonObject data{{QStringLiteral("values"), values},
                           {QStringLiteral("writableKeys"),
                            QJsonArray::fromStringList(settings.writableKeys)},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact)),
                       QCoreApplication::translate("AgentLauncherTools", "启动器设置"),
                       QCoreApplication::translate("AgentLauncherTools", "启动器设置当前值"), preview,
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
                QStringLiteral("%1 local Java major version(s)").arg(majors.size()));
    const QJsonObject data{{QStringLiteral("majors"), array},
                           {QStringLiteral("count"), int(majors.size())},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(), QString(),
                       QCoreApplication::translate("AgentLauncherTools", "本机 Java"),
                       QCoreApplication::translate("AgentLauncherTools", "本机 Java 主版本：%1 个").arg(majors.size()),
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
                QStringLiteral("System memory %1 MB total, %2 MB available, %3 mod(s)")
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
                       QCoreApplication::translate("AgentLauncherTools", "内存建议"),
                       QCoreApplication::translate("AgentLauncherTools", "建议 Xmx %1 MB、Xms %2 MB").arg(xmx).arg(xms), QString(),
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
                                 {QStringLiteral("instanceId"), backup.instanceId},
                                 {QStringLiteral("restorable"), backup.restorable}});
        preview += QStringLiteral("%1 · %2 · %3 · %4\n")
                       .arg(backup.backupId, backup.owner, backup.label,
                            backup.createdAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("backup.list"),
                QStringLiteral("%1 backup(s)").arg(backups.size()));
    const QJsonObject data{{QStringLiteral("count"), int(backups.size())},
                           {QStringLiteral("backups"), array},
                           {QStringLiteral("evidence"), evidenceJson(evidence)}};
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(), QString(),
                       QCoreApplication::translate("AgentLauncherTools", "备份列表"),
                       QCoreApplication::translate("AgentLauncherTools", "维护备份：%1 份").arg(backups.size()), preview, data);
}

AgentToolResult AgentLauncherTools::handleUpgradeInspect(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
    }
    QJsonObject data;
    QString problem;
    if (!m_host->upgradeInspect(context.instanceId, &data, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    QList<AgentDiagnosticProjection::Evidence> evidence;
    addEvidence(&evidence, QStringLiteral("tool"), QStringLiteral("upgrade.inspect"),
                QStringLiteral("%1 upgrade candidate target(s)")
                    .arg(data.value(QStringLiteral("targets")).toArray().size()));
    data.insert(QStringLiteral("evidence"), evidenceJson(evidence));
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "升级检查：%1 个候选目标")
                                .arg(data.value(QStringLiteral("targets")).toArray().size());
    return deliverRead(context, call, AgentEgressPolicy::Kind::InstanceMetadata, QString(),
                       QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)),
                       QCoreApplication::translate("AgentLauncherTools", "升级检查"), summary, summary, data);
}

AgentToolResult AgentLauncherTools::handleOpenPage(const Agent::ToolCall &call,
                                                   const AgentToolContext &context) {
    const QString page = call.arguments.value(QStringLiteral("page")).toString();
    QString problem;
    if (!m_host->openPage(page, context.instanceId, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    return autoResult(QJsonObject{{QStringLiteral("page"), page}},
                      QCoreApplication::translate("AgentLauncherTools", "已打开：%1").arg(page), QJsonObject(), false);
}

// ---------------------------------------------------------------- 写工具

AgentToolResult AgentLauncherTools::handleSettingsSet(const Agent::ToolCall &call,
                                                      const AgentToolContext &context) {
    const QString key = call.arguments.value(QStringLiteral("key")).toString();
    const QString raw = call.arguments.value(QStringLiteral("value")).toString();
    if (!settingsKeys().contains(key)) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "该设置键不在白名单中。"));
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
                               QCoreApplication::translate("AgentLauncherTools", "该设置键只接受 true 或 false。"));
        }
        parsed = lowered == QStringLiteral("true");
    } else if (key == QStringLiteral("memory.xmx") || key == QStringLiteral("memory.xms")) {
        bool ok = false;
        const int number = raw.toInt(&ok);
        if (!ok || number < (key == QStringLiteral("memory.xmx") ? 512 : 256) || number > 65536) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "内存取值超出范围（Xmx 512–65536 MB，Xms 256–65536 MB）。"));
        }
        parsed = number;
    }
    const QString workspaceId = call.arguments.value(QStringLiteral("workspaceId")).toString();
    QString transactionId;
    if (!workspaceId.isEmpty()) {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord record;
        if (!transactions.loadByWorkspace(workspaceId, &record, &problem) ||
            record.instanceId != context.instanceId) {
            return errorResult(Agent::ErrorKind::ToolError,
                               problem.isEmpty() ? QCoreApplication::translate("AgentLauncherTools", "该工作区没有对应的修复事务。") : problem);
        }
        transactionId = record.id;
    } else {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord active;
        if (transactions.activeForInstance(context.instanceId, &active, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "修复事务进行中，不能直接修改启动器设置。请带 workspaceId 调用。"));
        }
    }
    Pending pending;
    pending.kind = Pending::Kind::SettingsWrite;
    pending.call = call;
    pending.context = context;
    pending.key = key;
    pending.value = parsed;
    pending.oldValue = oldValue;
    pending.workspaceId = workspaceId;
    pending.transactionId = transactionId;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "修改设置 %1：%2 → %3")
                                .arg(key, oldValue.toVariant().toString(), raw);
    return approvalRequired(pending, QCoreApplication::translate("AgentLauncherTools", "修改启动器设置"), summary, summary, QString(),
                            QCoreApplication::translate("AgentLauncherTools", "只修改白名单中的设置键；写入失败不会覆盖已保存的设置。"), pending.egress);
}

AgentToolResult AgentLauncherTools::handleModsSetEnabled(const Agent::ToolCall &call,
                                                         const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                           QCoreApplication::translate("AgentLauncherTools", "只能处理 .jar 和 .jar.disabled 文件。"));
    }
    const QString workspaceId = call.arguments.value(QStringLiteral("workspaceId")).toString();
    if (!workspaceId.isEmpty()) {
        AgentWorkspaceStore workspaces;
        AgentWorkspaceRecord workspace;
        if (!workspaces.load(workspaceId, &workspace, &problem) ||
            workspace.sourceInstanceId != context.instanceId) {
            return errorResult(Agent::ErrorKind::ToolError,
                               problem.isEmpty() ? QCoreApplication::translate("AgentLauncherTools", "工作区与当前实例不匹配。") : problem);
        }
        const QString path = workspace.rootPath + QStringLiteral("/versions/") +
                             workspace.versionId + QStringLiteral("/mods/") + fileName;
        if (!QFileInfo(path).isFile()) {
            return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "工作区的 mods 目录中没有该文件。"));
        }
        if (fileName.endsWith(QStringLiteral(".jar")) == enable) {
            return errorResult(Agent::ErrorKind::ToolError,
                               enable ? QCoreApplication::translate("AgentLauncherTools", "该模组已启用。")
                                      : QCoreApplication::translate("AgentLauncherTools", "该模组已禁用。"));
        }
    } else {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord active;
        if (transactions.activeForInstance(context.instanceId, &active, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "修复事务进行中，不能直接修改原实例。请带 workspaceId 调用。"));
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
                                       enable ? QCoreApplication::translate("AgentLauncherTools", "该模组已启用。")
                                              : QCoreApplication::translate("AgentLauncherTools", "该模组已禁用。"));
                }
            }
        }
        if (!found) {
            return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "mods 目录中没有该文件。"));
        }
    }
    Pending pending;
    pending.kind = Pending::Kind::ModToggle;
    pending.call = call;
    pending.context = context;
    pending.fileName = fileName;
    pending.enable = enable;
    pending.workspaceId = workspaceId;
    pending.egress = writeEgress(QStringLiteral("file-name-list"));
    const QString summary = (enable ? QCoreApplication::translate("AgentLauncherTools", "启用模组：%1")
                                 : QCoreApplication::translate("AgentLauncherTools", "禁用模组：%1"))
                                .arg(fileName);
    return approvalRequired(pending, QCoreApplication::translate("AgentLauncherTools", "启停模组"), summary, summary, QString(),
                            QCoreApplication::translate("AgentLauncherTools", "只修改扩展名（.jar ↔ .jar.disabled），可随时改回。"),
                            pending.egress);
}

AgentToolResult AgentLauncherTools::handleBackupCreate(const Agent::ToolCall &call,
                                                       const AgentToolContext &context) {
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "会话尚未锁定实例。"));
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
                           QCoreApplication::translate("AgentLauncherTools", "所选范围内没有可备份的文件。"));
    }
    Pending pending;
    pending.kind = Pending::Kind::BackupCreate;
    pending.call = call;
    pending.context = context;
    pending.plan = plan;
    pending.reason = reason;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    QString preview = QCoreApplication::translate("AgentLauncherTools", "范围：%1\n文件：%2 个\n大小：%3 字节\n保存位置：%4\n")
                          .arg(plan.scope)
                          .arg(plan.relativePaths.size())
                          .arg(plan.bytes)
                          .arg(plan.saveLocation);
    for (const QString &path : plan.relativePaths) preview += QStringLiteral("  %1\n").arg(path);
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "创建备份：%1（%2 个文件）")
                                .arg(plan.scope)
                                .arg(plan.relativePaths.size());
    return approvalRequired(pending, QCoreApplication::translate("AgentLauncherTools", "创建维护备份"), summary, preview, QString(),
                            QCoreApplication::translate("AgentLauncherTools", "备份保存在启动器备份目录，删除会话不影响备份。"),
                            pending.egress);
}

AgentToolResult AgentLauncherTools::handleBackupRestore(const Agent::ToolCall &call,
                                                        const AgentToolContext &context) {
    {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord active;
        QString problem;
        if (transactions.activeForInstance(context.instanceId, &active, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentLauncherTools", "修复事务进行中，不能直接恢复备份。请先提交或回滚事务。"));
        }
    }
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
    if (!found) return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "找不到该备份。"));
    if (!target.restorable) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "该备份由启动器管理，请在实例设置「备份与操作」页的「从快照恢复…」中还原。"));
    }
    // 只准写回会话锁定的实例（D8）：备份记的是别的实例就拒绝，不跟着元数据写过去。
    if (target.instanceId.isEmpty() || target.instanceId != context.instanceId) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QCoreApplication::translate("AgentLauncherTools", "该备份不属于当前锁定的实例，已拒绝还原。"));
    }
    Pending pending;
    pending.kind = Pending::Kind::BackupRestore;
    pending.call = call;
    pending.context = context;
    pending.backup = target;
    pending.egress = writeEgress(QStringLiteral("instance-metadata"));
    const QString summary = QCoreApplication::translate("AgentLauncherTools", "恢复备份：%1（%2）").arg(target.label, target.backupId);
    return approvalRequired(pending, QCoreApplication::translate("AgentLauncherTools", "恢复备份"), summary, summary, QString(),
                            QCoreApplication::translate("AgentLauncherTools", "恢复前会先创建快照；来源会话已删除时也能恢复。"),
                            pending.egress);
}

// ---------------------------------------------------------------- 审批

AgentToolResult AgentLauncherTools::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentLauncherTools", "审批请求已失效。"));
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(
            Agent::ErrorKind::ApprovalDenied, QCoreApplication::translate("AgentLauncherTools", "用户已拒绝本次操作，未修改任何内容。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    switch (pending.kind) {
    case Pending::Kind::Generic:
        return autoResult(pending.data, pending.summary, pending.egress, false);
    case Pending::Kind::SettingsWrite: {
        QJsonObject applied;
        QString problem;
        if (!pending.workspaceId.isEmpty()) {
            AgentRepairTransactionStore transactions;
            if (!transactions.recordSetting(pending.transactionId, pending.key, pending.oldValue,
                                            pending.value, &problem)) {
                return errorResult(Agent::ErrorKind::ToolError, problem);
            }
            applied = QJsonObject{{QStringLiteral("transaction"), true}};
        } else if (!m_host->applySetting(pending.key, pending.value, &applied, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        QJsonObject data{{QStringLiteral("key"), pending.key},
                         {QStringLiteral("oldValue"), pending.oldValue},
                         {QStringLiteral("newValue"), pending.value},
                         {QStringLiteral("applied"), applied},
                         {QStringLiteral("egress"), pending.egress}};
        const QString summary =
            QCoreApplication::translate("AgentLauncherTools", "已写入设置 %1（旧值 %2）。")
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
        if (!pending.workspaceId.isEmpty()) {
            AgentWorkspaceStore workspaces;
            AgentWorkspaceRecord workspace;
            if (!workspaces.load(pending.workspaceId, &workspace, &problem)) {
                return errorResult(Agent::ErrorKind::ToolError, problem);
            }
            const QString mods = workspace.rootPath + QStringLiteral("/versions/") +
                                 workspace.versionId + QStringLiteral("/mods");
            const QString from = mods + QLatin1Char('/') + pending.fileName;
            previous = pending.fileName;
            QString toName = pending.fileName;
            if (pending.enable) {
                if (toName.endsWith(QStringLiteral(".jar.disabled"))) toName.chop(5);
            } else if (!toName.endsWith(QStringLiteral(".jar.disabled"))) {
                toName += QStringLiteral(".disabled");
            }
            const QString to = mods + QLatin1Char('/') + toName;
            if (QFileInfo::exists(to) || !QFile::rename(from, to)) {
                return errorResult(Agent::ErrorKind::ToolError,
                                   QCoreApplication::translate("AgentLauncherTools", "工作区中的模组重命名失败。"));
            }
        } else if (!m_host->setModEnabled(pending.context.instanceId, pending.fileName,
                                          pending.enable, &previous, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = (pending.enable
                              ? QCoreApplication::translate("AgentLauncherTools", "已启用：%1（原文件名 %2）")
                              : QCoreApplication::translate("AgentLauncherTools", "已禁用：%1（原文件名 %2）"))
                             .arg(pending.fileName, previous);
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
        result.summary = QCoreApplication::translate("AgentLauncherTools", "备份完成：%1（%2 个文件）")
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
        result.summary = QCoreApplication::translate("AgentLauncherTools", "已恢复备份 %1（恢复前快照 %2）")
                             .arg(pending.backup.backupId, snapshotId);
        result.data = QJsonObject{{QStringLiteral("backupId"), pending.backup.backupId},
                                  {QStringLiteral("preRestoreBackupId"), snapshotId},
                                  {QStringLiteral("egress"), pending.egress}};
        return result;
    }
    }
    return errorResult(Agent::ErrorKind::Internal, QCoreApplication::translate("AgentLauncherTools", "未知审批类型。"));
}
