/**
 * @file AgentModToolEngine.cpp
 * @brief S5.4 模组工具的审批、异步编排与交付（见 .hpp）。
 */

#include "AgentModToolEngine.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentPaths.hpp"
#include "AgentDiagnosticProjection.hpp"
#include "AgentRepairTransaction.hpp"

#include "../operations/ModDeclarations.hpp"
#include "../util/AppPaths.hpp"
#include "../util/ZipUtil.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

constexpr qint64 kMaxHashBytes = 512ll * 1024 * 1024;  ///< 单文件哈希上限（S5.4：512 MB）
constexpr int kMaxPlatformHashes = 2000;
constexpr int kMaxInstallFiles = 10;
constexpr qint64 kMaxInstallBytes = 500ll * 1024 * 1024;
constexpr qint64 kMaxSingleDownloadBytes = 100ll * 1024 * 1024;
constexpr int kMaxIssueOutput = 200;
constexpr int kMaxModFiles = 2000;

QJsonObject stringField(const QString &type, int maxLength) {
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("maxLength"), maxLength}};
}

QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject boolField() {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
}

AgentToolSpec spec(const QString &name, const QString &displayName, const QString &description,
                   AgentRisk risk, const QJsonObject &schema) {
    return AgentToolSpec{name, displayName, description, risk, schema};
}

QString sha1OfFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool safeFileName(const QString &fileName) {
    return !fileName.isEmpty() && fileName.size() <= 200 &&
           !fileName.contains(QLatin1Char('/')) && !fileName.contains(QLatin1Char('\\')) &&
           fileName != QStringLiteral(".") && fileName != QStringLiteral("..") &&
           fileName.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive);
}

QStringList sha1List(const QList<AgentModCompat::InstalledFile> &files) {
    QStringList sha1s;
    for (const AgentModCompat::InstalledFile &file : files) {
        const QString sha1 = file.sha1.trimmed().toLower();
        if (sha1.isEmpty() || sha1s.contains(sha1)) continue;
        sha1s.append(sha1);
        if (sha1s.size() >= kMaxPlatformHashes) break;
    }
    return sha1s;
}

QJsonObject issueJson(const AgentModCompat::Issue &issue) {
    QJsonObject object{{QStringLiteral("type"), issue.type},
                       {QStringLiteral("severity"), issue.severity}};
    if (!issue.modId.isEmpty()) object.insert(QStringLiteral("modId"), issue.modId);
    if (!issue.fileName.isEmpty()) object.insert(QStringLiteral("fileName"), issue.fileName);
    if (!issue.detail.isEmpty()) object.insert(QStringLiteral("detail"), issue.detail);
    if (!issue.evidence.isEmpty()) {
        object.insert(QStringLiteral("evidenceBasis"), QJsonArray::fromStringList(issue.evidence));
    }
    return object;
}

QJsonObject countsJson(const AgentModCompat::Analysis &analysis) {
    QJsonObject counts{{QStringLiteral("problem"), 0},
                       {QStringLiteral("warning"), 0},
                       {QStringLiteral("info"), 0}};
    for (const AgentModCompat::Issue &issue : analysis.issues) {
        counts.insert(issue.severity, counts.value(issue.severity).toInt() + 1);
    }
    return counts;
}

}  // namespace

QList<AgentToolSpec> AgentModToolEngine::specs() {
    QList<AgentToolSpec> list;
    list.append(spec(
        QStringLiteral("mods.analyzeCompatibility"),
        QCoreApplication::translate("AgentModToolEngine", "模组兼容性分析"),
        QStringLiteral("读每个模组 JAR 的本地声明（加载器/游戏版本/Java/前置），对照实例判断"  // i18n-ignore
                       "加载器不匹配、版本不兼容、缺前置、重复、文件损坏；offline=false 时"  // i18n-ignore
                       "经确认后把 SHA-1 发给 Modrinth 补平台结论。"),  // i18n-ignore
        AgentRisk::NetworkRead,
        objectSchema({}, {{QStringLiteral("offline"), boolField()}})));
    list.append(spec(
        QStringLiteral("mods.verifyFiles"),
        QCoreApplication::translate("AgentModToolEngine", "校验模组文件"),
        QStringLiteral("逐个检查 mods/ 下的 JAR：zip 结构、大小、SHA-1；platform=true 时经确认"  // i18n-ignore
                       "后按哈希在 Modrinth 找回候选版本。"),  // i18n-ignore
        AgentRisk::NetworkRead,
        objectSchema({}, {{QStringLiteral("platform"), boolField()}})));
    list.append(spec(
        QStringLiteral("mods.installFix"),
        QCoreApplication::translate("AgentModToolEngine", "修复模组文件"),
        QStringLiteral("只接受 projectId/versionId/expectedSha1：本地解析下载地址，下载到暂存目录，"  // i18n-ignore
                       "校验大小和 SHA-1 后写进事务工作区（可补齐必需前置）；没有活动事务时拒绝。"),  // i18n-ignore
        AgentRisk::Write,
        objectSchema({QStringLiteral("projectId"), QStringLiteral("workspaceId")},
                     {{QStringLiteral("projectId"), stringField(QStringLiteral("string"), 64)},
                      {QStringLiteral("versionId"), stringField(QStringLiteral("string"), 64)},
                      {QStringLiteral("expectedSha1"), stringField(QStringLiteral("string"), 40)},
                      {QStringLiteral("workspaceId"), stringField(QStringLiteral("string"), 80)},
                      {QStringLiteral("installDependencies"), boolField()}})));
    list.append(spec(
        QStringLiteral("mods.bisect"),
        QCoreApplication::translate("AgentModToolEngine", "依赖感知二分"),
        QStringLiteral("在事务工作区里反复启停模组并启动游戏，用 delta debugging 找出"  // i18n-ignore
                       "哪些模组一起启用才崩；必需前置始终启用，结束后工作区恢复基线。"),  // i18n-ignore
        AgentRisk::Command,
        objectSchema({QStringLiteral("workspaceId")},
                     {{QStringLiteral("workspaceId"), stringField(QStringLiteral("string"), 80)},
                      {QStringLiteral("prioritizedCandidates"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                   {QStringLiteral("maxItems"), 50}}},
                      {QStringLiteral("maxLaunches"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), AgentModBisect::Bisect::maxLaunchesLimit()}}},
                      {QStringLiteral("repeatPerCandidate"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), AgentModBisect::Bisect::maxRepeatLimit()}}},
                      {QStringLiteral("timeoutSeconds"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 600}}},
                      {QStringLiteral("respectDependencies"), boolField()}})));
    list.append(spec(
        QStringLiteral("instance.repairCore"),
        QCoreApplication::translate("AgentModToolEngine", "修复游戏核心文件"),
        QStringLiteral("按实例声明的官方大小与 SHA-1 重下客户端 jar、库和资源索引；"  // i18n-ignore
                       "每次审批，事务活动期间拒绝。"),  // i18n-ignore
        AgentRisk::Destructive, objectSchema({}, {})));
    return list;
}

AgentModToolEngine::AgentModToolEngine(AgentLauncherHost *launcherHost, AgentModHost *modHost,
                                       ContextProvider provider, QObject *dispatcher,
                                       AgentLaunchHost *launchHost)
    : m_launcherHost(launcherHost),
      m_modHost(modHost),
      m_launchHost(launchHost),
      m_provider(std::move(provider)),
      m_dispatcher(dispatcher) {}

QJsonArray AgentModToolEngine::schemas() const {
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

bool AgentModToolEngine::handles(const QString &internalName) const {
    for (const AgentToolSpec &item : specs()) {
        if (item.name == internalName) return true;
    }
    return false;
}

int AgentModToolEngine::platformHashLimit() { return kMaxPlatformHashes; }

AgentToolResult AgentModToolEngine::errorResult(Agent::ErrorKind kind,
                                                const QString &message) const {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

QJsonObject AgentModToolEngine::externalSendEgress(const QString &reason, int fileCount) const {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("platform-query")},
                       {QStringLiteral("decision"), QStringLiteral("confirm")},
                       {QStringLiteral("reason"), reason},
                       {QStringLiteral("fileCount"), fileCount},
                       {QStringLiteral("redactionHits"), 0}};
}

void AgentModToolEngine::post(const std::function<void()> &work) {
    if (m_dispatcher.isNull()) {
        work();
        return;
    }
    QMetaObject::invokeMethod(m_dispatcher.data(), work, Qt::QueuedConnection);
}

bool AgentModToolEngine::runAlive(const Run &run) const {
    return m_active && run.generation == m_generation;
}

void AgentModToolEngine::cleanupStaging(const QString &stagingDir) {
    if (!stagingDir.isEmpty()) QDir(stagingDir).removeRecursively();
}

QString AgentModToolEngine::workspaceModsDir(const AgentWorkspaceRecord &workspace) const {
    return workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId +
           QStringLiteral("/mods");
}

void AgentModToolEngine::deliver(const Agent::ToolCall &call, const AgentToolResult &result) {
    if (m_sink) m_sink(call, result);
}

void AgentModToolEngine::finishWithData(Run run, const QJsonObject &data,
                                        const QString &summary) {
    if (!runAlive(run)) return;
    m_active = false;
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = summary;
    result.data = data;
    deliver(run.pending.call, result);
}

void AgentModToolEngine::finishWithError(Run run, Agent::ErrorKind kind,
                                         const QString &message) {
    cleanupStaging(run.stagingDir);
    if (!runAlive(run)) return;
    m_active = false;
    deliver(run.pending.call, errorResult(kind, message));
}

AgentToolResult AgentModToolEngine::approvalRequired(const Pending &pending,
                                                     const QString &displayName,
                                                     const QString &summary,
                                                     const QString &preview,
                                                     const QString &backupHint,
                                                     const QJsonObject &egress) {
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    AgentToolSpec toolSpec = AgentToolRegistry::find(pending.call.name);
    QJsonObject request = AgentApprovalPolicy::request(
        pending.call.name, displayName.isEmpty() ? toolSpec.displayName : displayName,
        pending.context.instanceName, summary, preview, QString(), backupHint, egress);
    request.insert(QStringLiteral("approvalId"), approvalId);
    m_pending.insert(approvalId, pending);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

bool AgentModToolEngine::needsPlatformApproval(const Pending &pending) const {
    if (!pending.platform || m_modHost == nullptr) return false;
    if (pending.context.strictEgress) return true;
    return !m_platformApproved.contains(pending.context.instanceId);
}

AgentToolResult AgentModToolEngine::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    if (!handles(internal)) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "未知工具。"));
    }
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internal);
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (m_launcherHost == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "启动器工具宿主未接入。"));
    }
    QString contextProblem;
    const AgentToolContext context =
        m_provider != nullptr ? m_provider(&contextProblem) : AgentToolContext{};
    if (!contextProblem.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, contextProblem);
    }
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "会话还没有锁定实例。"));
    }

    Pending pending;
    pending.call = call;
    pending.context = context;

    if (internal == QStringLiteral("mods.analyzeCompatibility") ||
        internal == QStringLiteral("mods.verifyFiles")) {
        AgentDiagnosticProjection::InstanceFacts facts;
        if (!m_launcherHost->instanceFacts(context.instanceId, &facts, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        QList<AgentHostModEntry> entries;
        if (!m_launcherHost->mods(context.instanceId, &entries, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError, problem);
        }
        QString modsDir;
        QString modsProblem;
        m_launcherHost->modsDirectory(context.instanceId, &modsDir, &modsProblem);
        for (const AgentHostModEntry &entry : entries) {
            if (pending.mods.size() >= kMaxModFiles) {
                pending.modsTruncated = true;
                break;
            }
            AgentHostModFile file;
            file.fileName = entry.fileName;
            file.enabled = entry.enabled;
            file.bytes = entry.bytes;
            file.path = entry.path;
            if (file.path.isEmpty() && !modsDir.isEmpty()) {
                file.path = modsDir + QLatin1Char('/') + entry.fileName;
            }
            pending.mods.append(file);
        }
        pending.modsDir = modsDir;
        pending.target.gameVersion = facts.gameVersion;
        pending.target.loader = ModDeclarations::loaderKey(
            ModDeclarations::loaderFromKey(facts.loaderName));
        pending.target.loaderVersion = facts.loaderVersion;
        pending.target.javaMajor = facts.usedJavaMajor;
        pending.job = internal == QStringLiteral("mods.analyzeCompatibility") ? Job::Analyze
                                                                            : Job::Verify;
        const bool wantsPlatform = internal == QStringLiteral("mods.analyzeCompatibility")
                                       ? !call.arguments.value(QStringLiteral("offline")).toBool(false)
                                       : call.arguments.value(QStringLiteral("platform")).toBool(false);
        pending.platform = wantsPlatform && m_modHost != nullptr;
        if (needsPlatformApproval(pending)) {
            const QString reason =
                QCoreApplication::translate("AgentModToolEngine", "把 %1 个模组文件的 SHA-1 发给 Modrinth，不发文件名").arg(pending.mods.size());
            const QString summary =
                QCoreApplication::translate("AgentModToolEngine", "分析模组兼容性：%1 个文件（%2 %3）")
                    .arg(pending.mods.size())
                    .arg(facts.gameVersion.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "版本未知") : facts.gameVersion,
                         facts.loaderName.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "原版") : facts.loaderName);
            const QString preview =
                QCoreApplication::translate("AgentModToolEngine", "本地先读每个 JAR 的声明（不联网）；平台查询只发 SHA-1，不发文件名，"
                      "结果按哈希缓存。加 offline=true / platform=false 可只做本地分析。");  // i18n-ignore
            return approvalRequired(pending,
                           QCoreApplication::translate("AgentModToolEngine", "模组兼容性分析"),
                           summary, preview,
                                    QCoreApplication::translate("AgentModToolEngine", "平台查询只发 SHA-1；本地分析不写盘。"),
                                    externalSendEgress(reason, pending.mods.size()));
        }
        return startAnalyzeOrVerify(pending);
    }

    if (internal == QStringLiteral("mods.bisect")) {
        const QString workspaceId = call.arguments.value(QStringLiteral("workspaceId")).toString();
        AgentWorkspaceStore workspaces;
        AgentWorkspaceRecord workspace;
        if (!workspaces.load(workspaceId, &workspace, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "没有这个工作区。") : problem);  // i18n-ignore
        }
        if (workspace.sourceInstanceId != context.instanceId) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "工作区不属于当前锁定的实例。"));  // i18n-ignore
        }
        const QString root = QDir(workspaces.workspacesRoot()).absolutePath();
        if (!QDir::cleanPath(workspace.rootPath).startsWith(root + QLatin1Char('/'))) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "工作区路径不在登记目录下，拒绝执行。"));  // i18n-ignore
        }
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord record;
        if (!transactions.loadByWorkspace(workspaceId, &record, &problem) || !record.active()) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "二分只从事务工作区：先 repair.begin，再带 workspaceId 调这里。"));  // i18n-ignore
        }
        if (m_launchHost == nullptr) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "测试启动宿主未接入。"));  // i18n-ignore
        }
        pending.job = Job::Bisect;
        pending.workspace = workspace;
        pending.transactionId = record.id;
        pending.respectDependencies = call.arguments.value(QStringLiteral("respectDependencies")).toBool(true);
        pending.maxLaunches = call.arguments.value(QStringLiteral("maxLaunches")).toInt(8);
        pending.maxLaunches = qBound(1, pending.maxLaunches, AgentModBisect::Bisect::maxLaunchesLimit());
        pending.repeatPerCandidate =
            call.arguments.value(QStringLiteral("repeatPerCandidate")).toInt(2);
        pending.repeatPerCandidate =
            qBound(1, pending.repeatPerCandidate, AgentModBisect::Bisect::maxRepeatLimit());
        pending.timeoutSeconds = call.arguments.value(QStringLiteral("timeoutSeconds")).toInt(120);
        pending.timeoutSeconds = qBound(10, pending.timeoutSeconds, 600);
        for (const QJsonValue &value :
             call.arguments.value(QStringLiteral("prioritizedCandidates")).toArray()) {
            if (!value.isString()) continue;
            const QString token = value.toString().trimmed();
            if (token.isEmpty() || token.size() > 200) continue;
            if (pending.prioritizedCandidates.size() >= 50) break;
            pending.prioritizedCandidates.append(token);
        }
        const QString summary =
            QCoreApplication::translate("AgentModToolEngine",
                                        "在事务工作区 %1 里二分：最多 %2 次启动、每次最多 %3 秒。")  // i18n-ignore
                .arg(workspaceId)
                .arg(pending.maxLaunches)
                .arg(pending.timeoutSeconds);
        const QString preview =
            QCoreApplication::translate("AgentModToolEngine",
                                        "反复启用/停用工作区 mods/ 并启动游戏，每轮把 config/ 与 options.txt "  // i18n-ignore
                                        "重置到二分开始时的快照；不写原实例、不下载、不改世界。结束后把 "  // i18n-ignore
                                        "mods/、config/、options.txt 恢复成二分前的样子。");  // i18n-ignore
        return approvalRequired(pending,
                                QCoreApplication::translate("AgentModToolEngine", "依赖感知二分"),
                                summary, preview,
                                QCoreApplication::translate("AgentModToolEngine", "只启动隔离工作区里的游戏；进程用 Job Object 清理。"),  // i18n-ignore
                                QJsonObject{{QStringLiteral("kind"), QStringLiteral("local")},
                                            {QStringLiteral("decision"), QStringLiteral("auto")},
                                            {QStringLiteral("reason"), QCoreApplication::translate("AgentModToolEngine", "只在本机工作区启动")},  // i18n-ignore
                                            {QStringLiteral("redactionHits"), 0}});
    }

    if (internal == QStringLiteral("mods.installFix")) {
        const QString workspaceId = call.arguments.value(QStringLiteral("workspaceId")).toString();
        AgentWorkspaceStore workspaces;
        AgentWorkspaceRecord workspace;
        if (!workspaces.load(workspaceId, &workspace, &problem)) {
            return errorResult(Agent::ErrorKind::ToolError,
                               problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "没有这个工作区。") : problem);
        }
        if (workspace.sourceInstanceId != context.instanceId) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "工作区不属于当前锁定的实例。"));
        }
        const QString root = QDir(workspaces.workspacesRoot()).absolutePath();
        if (!QDir::cleanPath(workspace.rootPath).startsWith(root + QLatin1Char('/'))) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "工作区路径不在登记目录下，拒绝写入。"));
        }
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord record;
        if (!transactions.loadByWorkspace(workspaceId, &record, &problem) || !record.active()) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentModToolEngine", "模组修复只写事务工作区：先 repair.begin，再带 workspaceId 调这里。"));
        }
        pending.job = Job::Install;
        pending.workspace = workspace;
        pending.transactionId = record.id;
        pending.projectId = call.arguments.value(QStringLiteral("projectId")).toString().trimmed();
        pending.versionId = call.arguments.value(QStringLiteral("versionId")).toString().trimmed();
        pending.expectedSha1 =
            call.arguments.value(QStringLiteral("expectedSha1")).toString().trimmed().toLower();
        pending.installDependencies =
            call.arguments.value(QStringLiteral("installDependencies")).toBool(true);
        const QString summary =
            QCoreApplication::translate("AgentModToolEngine", "修复模组：%1%2 → 事务工作区 %3")
                .arg(pending.projectId,
                     pending.versionId.isEmpty() ? QString()
                                                 : QStringLiteral(" @ ") + pending.versionId,
                     workspaceId);
        const QString preview =
            pending.installDependencies
                ? QCoreApplication::translate("AgentModToolEngine", "先把文件下载到暂存目录并校验大小与 SHA-1，全部通过后才复制进工作区 mods/；会补齐版本声明的必需前置；任何一步失败都不改工作区，也不碰原实例。")
                : QCoreApplication::translate("AgentModToolEngine", "先把文件下载到暂存目录并校验大小与 SHA-1，全部通过后才复制进工作区 mods/；任何一步失败都不改工作区，也不碰原实例。");
        return approvalRequired(pending,
                           QCoreApplication::translate("AgentModToolEngine", "修复模组文件"),
                           summary, preview,
                                QCoreApplication::translate("AgentModToolEngine", "只写事务工作区；提交/回滚由 repair.finish 决定。"),
                                QJsonObject{{QStringLiteral("kind"), QStringLiteral("local")},
                                            {QStringLiteral("decision"), QStringLiteral("auto")},
                                            {QStringLiteral("reason"), QCoreApplication::translate("AgentModToolEngine", "下载与写盘都在本机")},
                                            {QStringLiteral("redactionHits"), 0}});
    }

    // instance.repairCore
    {
        AgentRepairTransactionStore transactions;
        AgentRepairTransactionRecord active;
        if (transactions.activeForInstance(context.instanceId, &active, &problem)) {
            return errorResult(
                Agent::ErrorKind::ToolError,
                QCoreApplication::translate("AgentModToolEngine", "事务活动期间不能改共享库/资源；先提交或回滚事务。"));
        }
        if (m_modHost == nullptr) {
            return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "模组宿主未接入。"));
        }
        pending.job = Job::RepairCore;
        const QString summary = QCoreApplication::translate("AgentModToolEngine", "重下实例 %1 的客户端 jar、库和资源索引").arg(context.instanceName);
        const QString preview =
            QCoreApplication::translate("AgentModToolEngine", "按实例声明的官方大小与 SHA-1 校验并重下；会动全体版本共用的 libraries/ 与 assets/。");
        return approvalRequired(pending,
                           QCoreApplication::translate("AgentModToolEngine", "修复游戏核心文件"),
                           summary, preview,
                                QCoreApplication::translate("AgentModToolEngine", "共享库/资源不保留旧副本；失败时按官方哈希重下。"),
                                QJsonObject{{QStringLiteral("kind"), QStringLiteral("local")},
                                            {QStringLiteral("decision"), QStringLiteral("auto")},
                                            {QStringLiteral("reason"), QCoreApplication::translate("AgentModToolEngine", "只从官方源重下")},
                                            {QStringLiteral("redactionHits"), 0}});
    }
}

AgentModToolEngine::LocalScan AgentModToolEngine::scanMods(const QString &modsDir,
                                                          const QList<AgentHostModFile> &mods,
                                                          const AgentModCompat::Target &target,
                                                          int hashBudgetMs) {
    LocalScan scan;
    if (mods.isEmpty()) return scan;
    QList<AgentHostModFile> resolved;
    QStringList archives;
    for (const AgentHostModFile &mod : mods) {
        AgentHostModFile file = mod;
        if (file.path.isEmpty() && !modsDir.isEmpty()) {
            file.path = modsDir + QLatin1Char('/') + mod.fileName;
        }
        resolved.append(file);
        if (!file.path.isEmpty() && QFileInfo(file.path).isFile()) archives.append(file.path);
    }
    QString batchProblem;
    const QHash<QString, ZipUtil::ArchiveMetadata> metadata =
        archives.isEmpty()
            ? QHash<QString, ZipUtil::ArchiveMetadata>{}
            : ZipUtil::readMetadataBatch(archives, ModDeclarations::metadataEntries(),
                                         &batchProblem);
    QElapsedTimer hashClock;
    hashClock.start();
    for (const AgentHostModFile &mod : resolved) {
        AgentModCompat::InstalledFile file;
        file.fileName = mod.fileName;
        file.enabled = mod.enabled;
        file.path = mod.path;
        const QFileInfo info(mod.path);
        file.bytes = info.size();
        if (mod.path.isEmpty() || !info.isFile()) {
            file.readProblem = QCoreApplication::translate("AgentModToolEngine", "文件不存在或读不动");
            scan.files.append(file);
            continue;
        }
        const auto record = metadata.constFind(mod.path);
        if (record == metadata.constEnd()) {
            file.readProblem = batchProblem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "未检查：本轮未返回声明结果")
                                                      : batchProblem;
        } else if (!record->problem.isEmpty()) {
            file.readProblem = record->problem;
        } else {
            ModDeclarations::parse(mod.fileName, record->entries, record->entryProblems,
                                   &file.declaration);
        }
        if (file.readProblem.isEmpty() && file.bytes > 0) {
            if (file.bytes > kMaxHashBytes) {
                scan.truncated = true;  // 超大文件不算损坏，只是没算哈希
            } else if (hashClock.elapsed() >= hashBudgetMs) {
                scan.truncated = true;
            } else {
                file.sha1 = sha1OfFile(mod.path);
                if (file.sha1.isEmpty()) {
                    file.readProblem = QCoreApplication::translate("AgentModToolEngine", "算不出 SHA-1（文件读不动）");
                } else {
                    ++scan.hashed;
                }
            }
        }
        scan.files.append(file);
    }
    scan.analysis = AgentModCompat::analyzeLocal(scan.files, target);
    return scan;
}

AgentToolResult AgentModToolEngine::startAnalyzeOrVerify(const Pending &pending) {
    if (m_active) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "已有一个模组任务在运行。"));
    }
    Run run;
    run.pending = pending;
    run.generation = ++m_generation;
    m_run = run;
    m_active = true;
    const Run captured = run;
    const QList<AgentHostModFile> mods = pending.mods;
    const QString modsDir = pending.modsDir;
    const AgentModCompat::Target target = pending.target;
    (void)QtConcurrent::run([this, captured, mods, modsDir, target] {
        const LocalScan scan = scanMods(modsDir, mods, target, 60000);
        post([this, captured, scan] {
            if (!runAlive(captured)) return;
            if (captured.pending.job == Job::Analyze) {
                if (!captured.pending.platform || m_modHost == nullptr) {
                    finishAnalyze(captured, scan, {}, false, QString());
                    return;
                }
                beginPlatformLookup(captured, scan);
                return;
            }
            if (!captured.pending.platform || m_modHost == nullptr) {
                finishVerify(captured, scan, {}, false, QString());
                return;
            }
            beginPlatformLookup(captured, scan);
        });
    });
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QCoreApplication::translate("AgentModToolEngine", "正在读模组声明…");
    return result;
}

void AgentModToolEngine::beginPlatformLookup(Run run, const LocalScan &scan) {
    const QStringList sha1s = sha1List(scan.files);
    if (sha1s.isEmpty()) {
        if (run.pending.job == Job::Analyze) {
            finishAnalyze(run, scan, {}, false, QCoreApplication::translate("AgentModToolEngine", "没有算成 SHA-1 的文件，跳过平台查询"));
        } else {
            finishVerify(run, scan, {}, false, QCoreApplication::translate("AgentModToolEngine", "没有算成 SHA-1 的文件，跳过平台查询"));
        }
        return;
    }
    QString problem;
    const bool started = m_modHost->lookupModPlatform(
        run.pending.context.instanceId, run.pending.target.gameVersion,
        run.pending.target.loaderVersion, run.pending.target.loader, sha1s,
        [this, run, scan](const QList<AgentModCompat::PlatformEntry> &entries,
                          const QString &problem) {
            post([this, run, scan, entries, problem] {
                if (!runAlive(run)) return;
                if (run.pending.job == Job::Analyze) {
                    finishAnalyze(run, scan, entries, problem.isEmpty(), problem);
                } else {
                    finishVerify(run, scan, entries, problem.isEmpty(), problem);
                }
            });
        },
        &problem);
    if (!started) {
        if (run.pending.job == Job::Analyze) {
            finishAnalyze(run, scan, {}, false, problem);
        } else {
            finishVerify(run, scan, {}, false, problem);
        }
    }
}

void AgentModToolEngine::finishAnalyze(Run run, const LocalScan &scan,
                                       const QList<AgentModCompat::PlatformEntry> &entries,
                                       bool platformComplete, const QString &platformProblem) {
    AgentModCompat::Analysis analysis = scan.analysis;
    if (run.pending.platform && (platformComplete || !entries.isEmpty())) {
        AgentModCompat::applyPlatform(&analysis, scan.files, entries, run.pending.target,
                                      platformComplete);
    }
    QJsonArray issues;
    for (const AgentModCompat::Issue &issue : analysis.issues) {
        if (issues.size() >= kMaxIssueOutput) break;
        issues.append(issueJson(issue));
    }
    QJsonArray files;
    for (const AgentModCompat::InstalledFile &file : scan.files) {
        files.append(QJsonObject{{QStringLiteral("fileName"), file.fileName},
                                 {QStringLiteral("enabled"), file.enabled},
                                 {QStringLiteral("bytes"), double(file.bytes)},
                                 {QStringLiteral("sha1"), file.sha1},
                                 {QStringLiteral("modId"), file.declaration.modId},
                                 {QStringLiteral("version"), file.declaration.version}});
    }
    QJsonObject platform{{QStringLiteral("queried"), run.pending.platform},
                         {QStringLiteral("complete"), platformComplete},
                         {QStringLiteral("entries"), int(entries.size())}};
    if (!platformProblem.isEmpty()) platform.insert(QStringLiteral("problem"), platformProblem);
    const QJsonObject data{{QStringLiteral("issues"), issues},
                           {QStringLiteral("counts"), countsJson(analysis)},
                           {QStringLiteral("unknown"), analysis.unknownCount},
                           {QStringLiteral("scanned"), analysis.scanned},
                           {QStringLiteral("hashed"), scan.hashed},
                           {QStringLiteral("truncated"),
                            scan.truncated || analysis.truncated || run.pending.modsTruncated},
                           {QStringLiteral("files"), files},
                           {QStringLiteral("platform"), platform},
                           {QStringLiteral("target"),
                            QJsonObject{{QStringLiteral("gameVersion"), run.pending.target.gameVersion},
                                        {QStringLiteral("loader"), run.pending.target.loader},
                                        {QStringLiteral("loaderVersion"), run.pending.target.loaderVersion},
                                        {QStringLiteral("javaMajor"), run.pending.target.javaMajor}}}};
    const QString summary = QCoreApplication::translate("AgentModToolEngine", "模组兼容性：%1 个文件，%2 个问题、%3 个提醒、%4 个未知")
                                .arg(analysis.scanned)
                                .arg(data.value(QStringLiteral("counts")).toObject().value(QStringLiteral("problem")).toInt())
                                .arg(data.value(QStringLiteral("counts")).toObject().value(QStringLiteral("warning")).toInt())
                                .arg(analysis.unknownCount);
    finishWithData(run, data, summary);
}

void AgentModToolEngine::finishVerify(Run run, const LocalScan &scan,
                                      const QList<AgentModCompat::PlatformEntry> &entries,
                                      bool platformComplete, const QString &platformProblem) {
    QHash<QString, AgentModCompat::PlatformEntry> bySha1;
    for (const AgentModCompat::PlatformEntry &entry : entries) {
        bySha1.insert(entry.sha1.trimmed().toLower(), entry);
    }
    QJsonArray files;
    QJsonArray candidates;
    int corrupt = 0;
    int unknown = 0;
    int platformUnknown = 0;
    for (const AgentModCompat::InstalledFile &file : scan.files) {
        QString status = QStringLiteral("ok");
        QString detail;
        if (!file.readProblem.isEmpty() || file.bytes == 0) {
            status = QStringLiteral("corrupt");
            detail = file.readProblem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "文件大小为 0") : file.readProblem;
            ++corrupt;
        } else if (file.declaration.declarations.isEmpty()) {
            status = QStringLiteral("unknown");
            detail = QCoreApplication::translate("AgentModToolEngine", "JAR 里没有可识别的模组声明");
            ++unknown;
        }
        files.append(QJsonObject{{QStringLiteral("fileName"), file.fileName},
                                 {QStringLiteral("enabled"), file.enabled},
                                 {QStringLiteral("bytes"), double(file.bytes)},
                                 {QStringLiteral("sha1"), file.sha1},
                                 {QStringLiteral("status"), status},
                                 {QStringLiteral("detail"), detail}});
        if (!run.pending.platform || !platformComplete || file.sha1.isEmpty()) continue;
        const auto entry = bySha1.constFind(file.sha1);
        if (entry == bySha1.constEnd()) {
            ++platformUnknown;
            continue;
        }
        candidates.append(QJsonObject{{QStringLiteral("fileName"), file.fileName},
                                      {QStringLiteral("projectId"), entry->projectId},
                                      {QStringLiteral("versionId"), entry->versionId},
                                      {QStringLiteral("versionNumber"), entry->versionNumber},
                                      {QStringLiteral("sha1"), file.sha1}});
    }
    QJsonObject platform{{QStringLiteral("queried"), run.pending.platform},
                         {QStringLiteral("complete"), platformComplete},
                         {QStringLiteral("entries"), int(entries.size())}};
    if (!platformProblem.isEmpty()) platform.insert(QStringLiteral("problem"), platformProblem);
    const QJsonObject data{{QStringLiteral("files"), files},
                           {QStringLiteral("candidates"), candidates},
                           {QStringLiteral("scanned"), int(scan.files.size())},
                           {QStringLiteral("hashed"), scan.hashed},
                           {QStringLiteral("corrupt"), corrupt},
                           {QStringLiteral("unknown"), unknown},
                           {QStringLiteral("platformUnknown"), platformUnknown},
                           {QStringLiteral("truncated"),
                            scan.truncated || run.pending.modsTruncated},
                           {QStringLiteral("platform"), platform}};
    const QString summary = QCoreApplication::translate("AgentModToolEngine", "模组校验：%1 个文件，%2 个损坏、%3 个无法确认")
                                .arg(scan.files.size())
                                .arg(corrupt)
                                .arg(unknown);
    finishWithData(run, data, summary);
}

void AgentModToolEngine::startInstall(const Pending &pending) {
    if (m_active) {
        return;  // 审批后目标已被别的作业占用：不交付（调用方已收到 Pending 之外的结果）
    }
    Run run;
    run.pending = pending;
    run.generation = ++m_generation;
    m_run = run;
    m_active = true;
    run.stagingDir = AgentPaths::modStagingDir() + QLatin1Char('/') + run.pending.transactionId +
                     QLatin1Char('-') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(run.stagingDir)) {
        finishWithError(run, Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "建不出暂存目录，未下载任何文件。"));
        return;
    }
    resolveNext(run);
}

void AgentModToolEngine::resolveNext(Run run) {
    if (!runAlive(run)) return;
    const Pending &pending = run.pending;
    QString projectId;
    QString expectedSha1;
    if (run.resolveIndex == 0) {
        projectId = pending.projectId;
        expectedSha1 = pending.expectedSha1;
    } else {
        if (!pending.installDependencies) {
            downloadNext(run);
            return;
        }
        const QStringList dependencies = run.targets.constFirst().requiredProjectIds;
        const int index = run.resolveIndex - 1;
        if (index >= dependencies.size() || run.targets.size() >= kMaxInstallFiles) {
            downloadNext(run);
            return;
        }
        projectId = dependencies.at(index);
    }
    QString problem;
    const bool started = m_modHost->resolveModTarget(
        pending.context.instanceId, projectId,
        run.resolveIndex == 0 ? pending.versionId : QString(), expectedSha1,
        [this, run](const AgentHostModTarget &target, const QString &problem) {
            post([this, run, target, problem] { onResolved(run, target, problem); });
        },
        &problem);
    if (!started) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "解析下载地址失败，未下载任何文件。") : problem);
    }
}

void AgentModToolEngine::onResolved(Run run, const AgentHostModTarget &target,
                                    const QString &problem) {
    if (!runAlive(run)) return;
    if (!problem.isEmpty()) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "解析下载地址失败：%1（未下载任何文件）").arg(problem));
        return;
    }
    if (!safeFileName(target.fileName)) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "平台给的文件名不合法，拒绝写入。"));
        return;
    }
    if (!run.pending.expectedSha1.isEmpty() && run.resolveIndex == 0 &&
        target.sha1.compare(run.pending.expectedSha1, Qt::CaseInsensitive) != 0) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "平台记录的哈希与 expectedSha1 不一致，拒绝下载。"));
        return;
    }
    if (target.size > 0 && target.size > kMaxSingleDownloadBytes) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "文件超过 100 MB，不在自动修复范围内。"));
        return;
    }
    run.targets.append(target);
    ++run.resolveIndex;
    resolveNext(run);
}

void AgentModToolEngine::downloadNext(Run run) {
    if (!runAlive(run)) return;
    if (run.downloadIndex >= run.targets.size()) {
        qint64 total = 0;
        for (const AgentHostModTarget &target : run.targets) {
            if (target.size > 0) total += target.size;
        }
        if (total > kMaxInstallBytes) {
            finishWithError(run, Agent::ErrorKind::ToolError,
                            QCoreApplication::translate("AgentModToolEngine", "这一批超过 500 MB，整批拒绝，工作区未改动。"));
            return;
        }
        copyStagedIntoWorkspace(run);
        return;
    }
    const AgentHostModTarget target = run.targets.at(run.downloadIndex);
    QString problem;
    const bool started = m_modHost->downloadModFile(
        target, run.stagingDir,
        [this, run, target](const AgentModHost::DownloadOutcome &outcome) {
            post([this, run, target, outcome] {
                if (!runAlive(run)) return;
                if (!outcome.ok) {
                    finishWithError(run, Agent::ErrorKind::ToolError,
                                    QCoreApplication::translate("AgentModToolEngine", "下载失败：%1（整批停止，工作区未改动）")
                                        .arg(outcome.problem));
                    return;
                }
                if (!target.sha1.isEmpty() &&
                    outcome.sha1.compare(target.sha1, Qt::CaseInsensitive) != 0) {
                    finishWithError(run, Agent::ErrorKind::ToolError,
                                    QCoreApplication::translate("AgentModToolEngine", "下载回来的哈希与平台记录不一致（%1），整批停止。")
                                        .arg(target.fileName));
                    return;
                }
                Run next = run;
                next.staged.append(qMakePair(target, outcome.stagedPath));
                ++next.downloadIndex;
                downloadNext(next);
            });
        },
        &problem);
    if (!started) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "下载没有开始，未写任何文件。") : problem);
    }
}

void AgentModToolEngine::copyStagedIntoWorkspace(Run run) {
    if (!runAlive(run)) return;
    const QString modsDir = workspaceModsDir(run.pending.workspace);
    if (!QDir().mkpath(modsDir)) {
        finishWithError(run, Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "建不出工作区 mods 目录。"));
        return;
    }
    QStringList copied;
    for (const auto &staged : run.staged) {
        const AgentHostModTarget &target = staged.first;
        const QString source = staged.second;
        const QString destination = modsDir + QLatin1Char('/') + target.fileName;
        if (QFileInfo::exists(destination)) {
            for (const QString &path : copied) QFile::remove(path);
            finishWithError(run, Agent::ErrorKind::ToolError,
                            QCoreApplication::translate("AgentModToolEngine", "工作区里已经有 %1，拒绝覆盖（已撤销本次复制的文件）。")
                                .arg(target.fileName));
            return;
        }
        const QString temp =
            destination + QStringLiteral(".wtfml-tmp-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::copy(source, temp)) {
            QFile::remove(temp);
            for (const QString &path : copied) QFile::remove(path);
            finishWithError(run, Agent::ErrorKind::ToolError,
                            QCoreApplication::translate("AgentModToolEngine", "复制 %1 失败（已撤销本次复制的文件）。").arg(target.fileName));
            return;
        }
        const QString copiedSha1 = sha1OfFile(temp);
        if (!target.sha1.isEmpty() &&
            copiedSha1.compare(target.sha1, Qt::CaseInsensitive) != 0) {
            QFile::remove(temp);
            for (const QString &path : copied) QFile::remove(path);
            finishWithError(run, Agent::ErrorKind::ToolError,
                            QCoreApplication::translate("AgentModToolEngine", "复制后校验 %1 失败（已撤销本次复制的文件）。")
                                .arg(target.fileName));
            return;
        }
        if (!QFile::rename(temp, destination)) {
            QFile::remove(temp);
            for (const QString &path : copied) QFile::remove(path);
            finishWithError(run, Agent::ErrorKind::ToolError,
                            QCoreApplication::translate("AgentModToolEngine", "落盘 %1 失败（已撤销本次复制的文件）。").arg(target.fileName));
            return;
        }
        copied.append(destination);
    }
    cleanupStaging(run.stagingDir);
    QJsonArray installed;
    for (const auto &staged : run.staged) {
        const AgentHostModTarget &target = staged.first;
        installed.append(QJsonObject{{QStringLiteral("fileName"), target.fileName},
                                     {QStringLiteral("projectId"), target.projectId},
                                     {QStringLiteral("versionId"), target.versionId},
                                     {QStringLiteral("versionNumber"), target.versionNumber},
                                     {QStringLiteral("sha1"), target.sha1},
                                     {QStringLiteral("bytes"), double(target.size)}});
    }
    const QJsonObject data{{QStringLiteral("transactionId"), run.pending.transactionId},
                           {QStringLiteral("workspaceId"), run.pending.workspace.id},
                           {QStringLiteral("installed"), installed},
                           {QStringLiteral("dependenciesFetched"), int(run.targets.size()) - 1},
                           {QStringLiteral("note"),
                            QCoreApplication::translate("AgentModToolEngine", "只改了事务工作区；用 repair.diff 看改动，repair.verify 验证，"
                                  "repair.finish 提交或回滚。")}};  // i18n-ignore
    const QString summary = QCoreApplication::translate("AgentModToolEngine", "模组修复：%1 个文件已写进工作区（事务 %2）")
                                .arg(run.staged.size())
                                .arg(run.pending.transactionId);
    finishWithData(run, data, summary);
}

void AgentModToolEngine::startRepairCore(const Pending &pending) {
    if (m_active) return;
    Run run;
    run.pending = pending;
    run.generation = ++m_generation;
    m_run = run;
    m_active = true;
    QString problem;
    const bool started = m_modHost->repairCoreFiles(
        pending.context.instanceId, pending.flaggedPaths,
        [this, run](const AgentModHost::RepairOutcome &outcome) {
            post([this, run, outcome] {
                if (!runAlive(run)) return;
                if (!outcome.ok) {
                    finishWithError(run, Agent::ErrorKind::ToolError,
                                    outcome.problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "核心文件修复失败。")
                                                              : outcome.problem);
                    return;
                }
                const QJsonObject data{{QStringLiteral("repaired"), true},
                                       {QStringLiteral("note"),
                                        QCoreApplication::translate("AgentModToolEngine", "已把缺文件/哈希不符的文件排进下载并等它收尾；"
                                              "再跑 instance.integrity 复核分组计数。")}};  // i18n-ignore
                finishWithData(run, data,
                               QCoreApplication::translate("AgentModToolEngine", "核心文件已按官方大小与 SHA-1 修复完成。"));
            });
        },
        &problem);
    if (!started) {
        finishWithError(run, Agent::ErrorKind::ToolError,
                        problem.isEmpty() ? QCoreApplication::translate("AgentModToolEngine", "核心文件修复没有开始。") : problem);
    }
}

// ---------------------------------------------------------------- 依赖感知二分

namespace {

/// 有界递归复制（config/ 这种小目录）；超出上限就失败，不半截继续。
bool copyTree(const QString &from, const QString &to, int *filesLeft, qint64 *bytesLeft,
              QString *problem) {
    QDir source(from);
    if (!source.exists()) return true;
    if (!QDir().mkpath(to)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentModToolEngine", "建不出快照目录。");  // i18n-ignore
        return false;
    }
    const QFileInfoList entries =
        source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || entry.isJunction()) continue;
        const QString target = to + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            if (!copyTree(entry.absoluteFilePath(), target, filesLeft, bytesLeft, problem)) {
                return false;
            }
            continue;
        }
        if (*filesLeft <= 0 || entry.size() > *bytesLeft) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("AgentModToolEngine", "config/ 超过快照上限（5000 个文件 / 200 MB）。");  // i18n-ignore
            }
            return false;
        }
        --(*filesLeft);
        *bytesLeft -= entry.size();
        if (!QFile::copy(entry.absoluteFilePath(), target)) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentModToolEngine", "复制 config/ 失败。");  // i18n-ignore
            return false;
        }
    }
    return true;
}

}  // namespace

void AgentModToolEngine::startBisect(const Pending &pending) {
    if (m_active) return;
    if (m_launchHost == nullptr) {
        m_run.pending = pending;
        m_active = true;
        finishWithError(m_run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "测试启动宿主未接入。"));  // i18n-ignore
        return;
    }
    m_active = true;
    m_generation += 1;
    m_run = Run{};
    m_run.pending = pending;
    m_run.generation = m_generation;
    m_bisect.reset();
    m_bisectState = BisectState{};
    m_bisectState.timer.start();
    const AgentWorkspaceRecord &workspace = pending.workspace;
    m_bisectState.versionDir = workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId;
    m_bisectState.modsDir = m_bisectState.versionDir + QStringLiteral("/mods");
    m_bisectState.configDir = m_bisectState.versionDir + QStringLiteral("/config");
    m_bisectState.optionsPath = m_bisectState.versionDir + QStringLiteral("/options.txt");
    m_bisectState.stagingDir = AgentPaths::modStagingDir() + QStringLiteral("/bisect-") +
                               pending.transactionId + QLatin1Char('-') +
                               QUuid::createUuid().toString(QUuid::WithoutBraces);

    // 1) 清点工作区的模组（同一基名只认启用的那份）。
    QList<AgentModBisect::Mod> mods;
    QList<QPair<int, QString>> archives;  // mod 下标 → 本地路径
    QDir modsDir(m_bisectState.modsDir);
    const QFileInfoList files = modsDir.entryInfoList(
        {QStringLiteral("*.jar"), QStringLiteral("*.jar.disabled")}, QDir::Files, QDir::Name);
    QSet<QString> seenBase;
    QStringList archivePaths;
    for (const QFileInfo &file : files) {
        QString base = file.fileName();
        const bool enabled = base.endsWith(QStringLiteral(".jar"));
        if (!enabled) base.chop(QStringLiteral(".disabled").size());
        if (seenBase.contains(base)) continue;  // 同名两份：先到的是启用那份
        seenBase.insert(base);
        AgentModBisect::Mod mod;
        mod.fileName = base;
        mod.initiallyEnabled = enabled;
        mod.modifiedMs = file.lastModified().toMSecsSinceEpoch();
        mods.append(mod);
        archives.append({int(mods.size()) - 1, file.absoluteFilePath()});
        archivePaths.append(file.absoluteFilePath());
    }
    if (!archivePaths.isEmpty()) {
        QString batchProblem;
        const QHash<QString, ZipUtil::ArchiveMetadata> metadata = ZipUtil::readMetadataBatch(
            archivePaths, ModDeclarations::metadataEntries(), &batchProblem);
        for (const auto &item : archives) {
            const auto record = metadata.constFind(item.second);
            if (record == metadata.constEnd()) continue;
            ModDeclarations::Mod parsed;
            ModDeclarations::parse(QFileInfo(item.second).fileName(), record->entries,
                                   record->entryProblems, &parsed);
            mods[item.first].modId = parsed.modId;
            mods[item.first].requiredModIds = parsed.requiredModIds;
        }
    }
    for (const AgentModBisect::Mod &mod : mods) {
        m_bisectState.initialEnabled.insert(mod.fileName, mod.initiallyEnabled);
        m_bisectState.installedBaseNames.append(mod.fileName);
    }

    // 2) config/ 与 options.txt 的快照（每轮启动前恢复它）。
    int filesLeft = 5000;
    qint64 bytesLeft = 200ll * 1024 * 1024;
    QString snapshotProblem;
    const bool snapshotOk =
        QDir().mkpath(m_bisectState.stagingDir) &&
        copyTree(m_bisectState.configDir, m_bisectState.stagingDir + QStringLiteral("/config"),
                 &filesLeft, &bytesLeft, &snapshotProblem);
    m_bisectState.optionsExisted = QFileInfo(m_bisectState.optionsPath).isFile();
    if (snapshotOk && m_bisectState.optionsExisted &&
        !QFile::copy(m_bisectState.optionsPath,
                     m_bisectState.stagingDir + QStringLiteral("/options.txt"))) {
        finishWithError(m_run, Agent::ErrorKind::ToolError,
                        QCoreApplication::translate("AgentModToolEngine", "复制 options.txt 快照失败，未开始二分。"));  // i18n-ignore
        return;
    }
    if (!snapshotOk) {
        finishWithError(m_run, Agent::ErrorKind::ToolError,
                        snapshotProblem.isEmpty()
                            ? QCoreApplication::translate("AgentModToolEngine", "准备快照目录失败，未开始二分。")  // i18n-ignore
                            : snapshotProblem);
        return;
    }

    // 3) 规划器。
    AgentModBisect::Options options;
    options.maxLaunches = pending.maxLaunches;
    options.repeatPerCandidate = pending.repeatPerCandidate;
    options.maxMinutes = AgentModBisect::Bisect::maxMinutesLimit();
    options.respectDependencies = pending.respectDependencies;
    options.prioritizedCandidates = pending.prioritizedCandidates;
    m_bisect = std::make_unique<AgentModBisect::Bisect>(mods, options);
    bisectNext();
}

void AgentModToolEngine::clearBisectLogs() {
    QDir(m_bisectState.versionDir + QStringLiteral("/logs")).removeRecursively();
    QDir(m_bisectState.versionDir + QStringLiteral("/crash-reports")).removeRecursively();
    const QFileInfoList crashes = QDir(m_bisectState.versionDir).entryInfoList(
        {QStringLiteral("hs_err_pid*")}, QDir::Files);
    for (const QFileInfo &file : crashes) QFile::remove(file.absoluteFilePath());
}

bool AgentModToolEngine::applyBisectEnabledSet(const QStringList &enabledFiles) {
    QDir modsDir(m_bisectState.modsDir);
    const QSet<QString> desired(enabledFiles.cbegin(), enabledFiles.cend());
    const QFileInfoList files = modsDir.entryInfoList(
        {QStringLiteral("*.jar"), QStringLiteral("*.jar.disabled")}, QDir::Files, QDir::Name);
    QSet<QString> seen;
    for (const QFileInfo &file : files) {
        QString base = file.fileName();
        if (!base.endsWith(QStringLiteral(".jar"))) base.chop(QStringLiteral(".disabled").size());
        if (seen.contains(base)) continue;
        seen.insert(base);
        // base 是**启用形态**（`x.jar`）；停用形态是在后面加 `.disabled`。
        const QString target = desired.contains(base) ? base
                                                      : base + QStringLiteral(".disabled");
        if (file.fileName() == target) continue;
        const QString targetPath = m_bisectState.modsDir + QLatin1Char('/') + target;
        if (QFileInfo::exists(targetPath)) {
            m_bisectState.problem = QCoreApplication::translate("AgentModToolEngine", "工作区里有重名文件：%1").arg(target);  // i18n-ignore
            return false;
        }
        if (!QFile::rename(file.absoluteFilePath(), targetPath)) {
            m_bisectState.problem = QCoreApplication::translate("AgentModToolEngine", "改不了模组文件名：%1").arg(base);  // i18n-ignore
            return false;
        }
    }
    for (const QString &base : desired) {
        if (!seen.contains(base)) {
            m_bisectState.problem = QCoreApplication::translate("AgentModToolEngine", "工作区里找不到模组：%1").arg(base);  // i18n-ignore
            return false;
        }
    }
    return true;
}

bool AgentModToolEngine::resetBisectWorkspace() {
    int filesLeft = 5000;
    qint64 bytesLeft = 200ll * 1024 * 1024;
    QString problem;
    QDir(m_bisectState.configDir).removeRecursively();
    if (!copyTree(m_bisectState.stagingDir + QStringLiteral("/config"), m_bisectState.configDir,
                  &filesLeft, &bytesLeft, &problem)) {
        m_bisectState.problem = problem;
        return false;
    }
    QFile::remove(m_bisectState.optionsPath);
    if (m_bisectState.optionsExisted &&
        !QFile::copy(m_bisectState.stagingDir + QStringLiteral("/options.txt"),
                     m_bisectState.optionsPath)) {
        m_bisectState.problem = QCoreApplication::translate("AgentModToolEngine", "恢复 options.txt 失败。");  // i18n-ignore
        return false;
    }
    clearBisectLogs();
    return true;
}

void AgentModToolEngine::bisectNext() {
    if (!m_active || !m_bisect) return;
    const AgentModBisect::Step step = m_bisect->step();
    if (step.kind == AgentModBisect::Step::Kind::Finished) {
        finishBisect();
        return;
    }
    m_bisectState.problem.clear();
    bool prepared = applyBisectEnabledSet(step.enabledFiles);
    if (prepared) prepared = resetBisectWorkspace();
    if (!prepared) {
        AgentModBisect::LaunchOutcome failure;
        failure.completed = false;
        failure.detail = m_bisectState.problem;
        m_bisect->report(failure, m_bisectState.timer.elapsed());
        bisectNext();
        return;
    }
    QJsonObject record{{QStringLiteral("index"), ++m_bisectState.launchIndex},
                       {QStringLiteral("purpose"), step.purpose},
                       {QStringLiteral("enabled"), int(step.enabledFiles.size())}};
    m_bisectState.evidence.append(record);

    AgentTestLaunchOptions options;
    options.timeoutSeconds = m_run.pending.timeoutSeconds;
    options.settleSeconds = 5;
    options.milestone = QStringLiteral("main_menu");
    options.repeat = 1;
    options.captureLogs = true;
    options.expectCrash = false;
    const int generation = m_generation;
    QString startProblem;
    const bool started = m_launchHost->startTestLaunch(
        m_run.pending.context.instanceId, m_run.pending.workspace.id, options,
        [this, generation](const AgentTestLaunchSummary &summary) {
            post([this, generation, summary] {
                if (!m_active || generation != m_generation) return;
                onBisectLaunchDone(summary, QString());
            });
        },
        &startProblem);
    if (!started) {
        onBisectLaunchDone(AgentTestLaunchSummary{}, startProblem);
    }
}

void AgentModToolEngine::onBisectLaunchDone(const AgentTestLaunchSummary &summary,
                                            const QString &problem) {
    if (!m_active || !m_bisect) return;
    AgentModBisect::LaunchOutcome outcome;
    QString detail;
    if (!problem.isEmpty() || summary.runs.isEmpty()) {
        outcome.completed = false;
        detail = problem.isEmpty()
                     ? QCoreApplication::translate("AgentModToolEngine", "测试启动没有开始")  // i18n-ignore
                     : problem;
    } else {
        const AgentTestLaunchRunResult &run = summary.runs.constFirst();
        outcome.completed = run.processStarted;
        outcome.crashed = run.crashed || run.newCrashReport;
        detail = run.detail;
        if (!outcome.completed) {
            detail = run.detail.isEmpty()
                         ? QCoreApplication::translate("AgentModToolEngine", "进程没有起来")  // i18n-ignore
                         : run.detail;
        }
    }
    outcome.detail = detail.left(200);
    if (!m_bisectState.evidence.isEmpty()) {
        QJsonObject record = m_bisectState.evidence.last().toObject();
        record.insert(QStringLiteral("completed"), outcome.completed);
        record.insert(QStringLiteral("crashed"), outcome.crashed);
        if (!outcome.detail.isEmpty()) record.insert(QStringLiteral("detail"), outcome.detail);
        m_bisectState.evidence.replace(m_bisectState.evidence.size() - 1, record);
    }
    m_bisect->report(outcome, m_bisectState.timer.elapsed());
    bisectNext();
}

void AgentModToolEngine::restoreBisectBaseline() {
    QStringList enabled;
    for (auto it = m_bisectState.initialEnabled.constBegin();
         it != m_bisectState.initialEnabled.constEnd(); ++it) {
        if (it.value()) enabled.append(it.key());
    }
    applyBisectEnabledSet(enabled);
    resetBisectWorkspace();
}

void AgentModToolEngine::finishBisect() {
    if (!m_bisect) return;
    const AgentModBisect::Result result = m_bisect->result();
    const bool restored = applyBisectEnabledSet([this] {
        QStringList enabled;
        for (auto it = m_bisectState.initialEnabled.constBegin();
             it != m_bisectState.initialEnabled.constEnd(); ++it) {
            if (it.value()) enabled.append(it.key());
        }
        return enabled;
    }());
    const bool reset = resetBisectWorkspace();
    const bool workspaceRestored = restored && reset;
    if (!m_bisectState.stagingDir.isEmpty()) QDir(m_bisectState.stagingDir).removeRecursively();
    QJsonArray culprits;
    for (const QString &culprit : result.culprits) culprits.append(culprit);
    QJsonArray minimal;
    for (const QString &file : result.minimalSet) minimal.append(file);
    QJsonArray notes;
    for (const QString &note : result.notes) notes.append(note);
    const QJsonObject data{
        {QStringLiteral("result"), result.result},
        {QStringLiteral("culprits"), culprits},
        {QStringLiteral("minimalSet"), minimal},
        {QStringLiteral("launches"), result.launches},
        {QStringLiteral("budgetExhausted"), result.budgetExhausted},
        {QStringLiteral("flaky"), result.flaky},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("evidence"), m_bisectState.evidence},
        {QStringLiteral("workspaceRestored"), workspaceRestored},
        {QStringLiteral("transactionId"), m_run.pending.transactionId},
        {QStringLiteral("workspaceId"), m_run.pending.workspace.id},
        {QStringLiteral("limits"),
         QJsonObject{{QStringLiteral("maxLaunches"), m_run.pending.maxLaunches},
                     {QStringLiteral("repeatPerCandidate"), m_run.pending.repeatPerCandidate},
                     {QStringLiteral("timeoutSeconds"), m_run.pending.timeoutSeconds}}}};
    const QString summary =
        QCoreApplication::translate("AgentModToolEngine", "二分结果：%1（%2 次启动）")  // i18n-ignore
            .arg(result.result)
            .arg(result.launches);
    m_bisect.reset();
    m_bisectState = BisectState{};
    finishWithData(m_run, data, summary);
}

AgentToolResult AgentModToolEngine::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentModToolEngine", "审批请求已失效。"));
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(
            Agent::ErrorKind::ApprovalDenied,
            QCoreApplication::translate("AgentModToolEngine", "用户拒绝了这次操作；没有发平台请求，也没有读写任何文件。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    if (pending.job == Job::Analyze || pending.job == Job::Verify) {
        if (pending.platform && !pending.context.strictEgress) {
            m_platformApproved.insert(pending.context.instanceId);
        }
        return startAnalyzeOrVerify(pending);
    }
    if (pending.job == Job::Bisect) {
        startBisect(pending);
        AgentToolResult result;
        result.status = AgentToolResult::Status::Pending;
        result.summary = QCoreApplication::translate("AgentModToolEngine", "正在准备二分…");  // i18n-ignore
        return result;
    }
    if (pending.job == Job::Install) {
        startInstall(pending);
        AgentToolResult result;
        result.status = AgentToolResult::Status::Pending;
        result.summary = QCoreApplication::translate("AgentModToolEngine", "正在解析下载地址…");
        return result;
    }
    startRepairCore(pending);
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QCoreApplication::translate("AgentModToolEngine", "正在修复核心文件…");
    return result;
}

void AgentModToolEngine::cancelPending() {
    m_pending.clear();
    const bool wasActive = m_active;
    const Agent::ToolCall call = m_run.pending.call;
    const bool wasBisect = m_bisect != nullptr;
    m_active = false;
    ++m_generation;
    if (wasBisect) {
        if (m_launchHost != nullptr) m_launchHost->cancelTestLaunch();
        restoreBisectBaseline();
        if (!m_bisectState.stagingDir.isEmpty()) {
            QDir(m_bisectState.stagingDir).removeRecursively();
        }
        m_bisect.reset();
        m_bisectState = BisectState{};
    }
    if (wasActive && m_sink && !call.name.isEmpty()) {
        cleanupStaging(m_run.stagingDir);
        m_sink(call, errorResult(Agent::ErrorKind::Cancelled, QCoreApplication::translate("AgentModToolEngine", "这次模组任务已取消。")));
    }
}
