/**
 * @file AgentAutoSafe.cpp
 * @brief Auto-safe 授权判定与审计（见 .hpp）。
 */

#include "AgentAutoSafe.hpp"

#include "AgentPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("AgentAutoSafe", text);
}

QString argumentText(const Agent::ToolCall &call, const char *key) {
    return call.arguments.value(QLatin1String(key)).toString().trimmed();
}

/// 目标摘要（审计里只看相对路径/键名/文件名，不含本机目录）。
QString targetOf(const Agent::ToolCall &call) {
    const QString name = AgentToolRegistry::internalName(call.name);
    if (name == QStringLiteral("fs.write")) return argumentText(call, "path");
    if (name == QStringLiteral("settings.set")) return argumentText(call, "key");
    if (name == QStringLiteral("mods.setEnabled")) return argumentText(call, "fileName");
    if (name == QStringLiteral("mods.installFix")) return argumentText(call, "projectId");
    if (name == QStringLiteral("instance.testLaunch") || name == QStringLiteral("repair.verify") ||
        name == QStringLiteral("mods.bisect")) {
        return QStringLiteral("launch");
    }
    if (name.startsWith(QStringLiteral("repair."))) {
        return argumentText(call, "transactionId");
    }
    return QString();
}

bool isWorldPath(const QString &path) {
    const QString normalized = path.trimmed().toLower();
    return normalized == QStringLiteral("saves") || normalized.startsWith(QStringLiteral("saves/")) ||
           normalized.startsWith(QStringLiteral("saves\\"));
}

int launchCountOf(const Agent::ToolCall &call) {
    const QString name = AgentToolRegistry::internalName(call.name);
    if (name == QStringLiteral("mods.bisect")) {
        int value = call.arguments.value(QStringLiteral("maxLaunches")).toInt(8);
        if (value <= 0) value = 8;
        return qBound(1, value, 20);
    }
    if (name == QStringLiteral("instance.testLaunch") || name == QStringLiteral("repair.verify")) {
        int value = call.arguments.value(QStringLiteral("repeat")).toInt(1);
        if (value <= 0) value = 1;
        return qBound(1, value, 3);
    }
    return 0;
}

}  // namespace

namespace AgentAutoSafe {

QString Authorizer::decisionName(Decision decision) {
    switch (decision) {
    case Decision::Allow: return QStringLiteral("allow");
    case Decision::RequireApproval: return QStringLiteral("require");
    case Decision::Deny: return QStringLiteral("deny");
    }
    return QStringLiteral("require");
}

void Authorizer::enable(const QString &instanceId, const QString &sessionId,
                        const QDateTime &now, const Limits &limits) {
    m_enabled = true;
    m_instanceId = instanceId;
    m_sessionId = sessionId;
    m_transactionId.clear();
    m_workspaceId.clear();
    m_tools.clear();
    m_paths.clear();
    m_keys.clear();
    m_modFiles.clear();
    m_projectIds.clear();
    m_scopes.clear();
    m_launches = 0;
    m_operations = QJsonArray{};
    m_verification = QJsonObject{};
    m_notes.clear();
    m_finishedAudit = QJsonObject{};
    m_limits.maxMinutes = qMax(0, limits.maxMinutes);
    m_limits.maxLaunches = qMax(0, limits.maxLaunches);
    m_startedAt = now;
    m_expiresAt = QDateTime();  // 不再有时效
}

void Authorizer::disable(const QDateTime &now, const QString &reason) {
    Q_UNUSED(now);
    if (!reason.isEmpty()) note(reason);
    m_enabled = false;
}

bool Authorizer::expired(const QDateTime &now) const {
    Q_UNUSED(now);
    // 开多久都算有效（2026-11-27 产品确认：去掉 60 分钟上限）。
    return false;
}

bool Authorizer::revalidate(const QString &sessionId, const QString &instanceId) {
    if (!m_enabled) return false;
    if (!m_sessionId.isEmpty() && m_sessionId != sessionId) {
        disable(QDateTime::currentDateTimeUtc(), QStringLiteral("session-changed"));  // i18n-ignore
        return false;
    }
    if (!m_instanceId.isEmpty() && m_instanceId != instanceId) {
        disable(QDateTime::currentDateTimeUtc(), QStringLiteral("instance-changed"));  // i18n-ignore
        return false;
    }
    return true;
}

bool Authorizer::matchesInstance(const QString &instanceId) const {
    return m_enabled && !m_instanceId.isEmpty() && m_instanceId == instanceId;
}

void Authorizer::note(const QString &text) {
    if (text.isEmpty() || m_notes.contains(text)) return;
    m_notes.append(text);
}

void Authorizer::bindPlan(const QList<AgentPlanStep> &steps) {
    m_tools.clear();
    m_paths.clear();
    m_keys.clear();
    m_modFiles.clear();
    m_projectIds.clear();
    for (const AgentPlanStep &step : steps) {
        const QString tool = AgentToolRegistry::internalName(step.tool);
        if (!m_tools.contains(tool)) m_tools.append(tool);
        const QJsonObject args = step.arguments;
        const auto add = [](QStringList *list, const QString &value) {
            const QString trimmed = value.trimmed();
            if (!trimmed.isEmpty() && !list->contains(trimmed)) list->append(trimmed);
        };
        if (tool == QStringLiteral("fs.write")) add(&m_paths, args.value(QStringLiteral("path")).toString());
        if (tool == QStringLiteral("settings.set")) {
            add(&m_keys, args.value(QStringLiteral("key")).toString());
        }
        if (tool == QStringLiteral("mods.setEnabled")) {
            add(&m_modFiles, args.value(QStringLiteral("fileName")).toString());
        }
        if (tool == QStringLiteral("mods.installFix")) {
            add(&m_projectIds, args.value(QStringLiteral("projectId")).toString());
        }
    }
}

void Authorizer::bindTransaction(const QString &transactionId, const QString &workspaceId) {
    m_transactionId = transactionId;
    m_workspaceId = workspaceId;
}

void Authorizer::setRepairScopes(const QStringList &scopes) {
    m_scopes = scopes;
}

void Authorizer::noteVerification(const QJsonObject &verification) {
    m_verification = verification;
}

Decision Authorizer::authorize(const Agent::ToolCall &call, const QDateTime &now,
                               QString *reason, int *launchCount) const {
    Q_UNUSED(now);  // 不再按时效判定；保留这个参数是为了以后要加别的时效条件时不改签名
    if (launchCount != nullptr) *launchCount = 0;
    const auto require = [reason](const QString &text) {
        if (reason != nullptr) *reason = text;
        return Decision::RequireApproval;
    };
    const auto deny = [reason](const QString &text) {
        if (reason != nullptr) *reason = text;
        return Decision::Deny;
    };
    if (!m_enabled) return require(tr("Auto-safe 没有开启"));
    const QString tool = AgentToolRegistry::internalName(call.name);
    const QString workspaceId = argumentText(call, "workspaceId");

    // 硬禁止：能改变可执行代码来源、动世界、装回旧备份、改共享库。
    if (tool == QStringLiteral("settings.set")) {
        const QString key = argumentText(call, "key");
        if (key == QStringLiteral("java.path") || key == QStringLiteral("args.extraJvm")) {
            return deny(tr("这个设置键能改变游戏用哪段代码，Auto-safe 不放行：%1").arg(key));
        }
        if (!m_keys.contains(key)) {
            return require(tr("设置键 %1 不在已批准的计划里").arg(key));
        }
        return Decision::Allow;
    }
    if (tool == QStringLiteral("fs.write")) {
        const QString path = argumentText(call, "path");
        if (isWorldPath(path)) return deny(tr("世界/存档不在事务授权范围"));
        if (path.contains(QStringLiteral(".."))) return deny(tr("路径里带 .. 不在授权范围"));
        if (!m_paths.contains(path)) {
            return require(tr("文件 %1 不在已批准的计划里").arg(path));
        }
        if (m_workspaceId.isEmpty() || workspaceId != m_workspaceId) {
            return deny(tr("写入必须落在本事务的工作区里"));
        }
        return Decision::Allow;
    }
    if (tool == QStringLiteral("mods.setEnabled")) {
        const QString fileName = argumentText(call, "fileName");
        if (!m_modFiles.contains(fileName)) {
            return require(tr("模组 %1 不在已批准的计划里").arg(fileName));
        }
        if (m_workspaceId.isEmpty() || workspaceId != m_workspaceId) {
            return deny(tr("模组启停必须落在本事务的工作区里"));
        }
        return Decision::Allow;
    }
    if (tool == QStringLiteral("mods.installFix")) {
        if (m_workspaceId.isEmpty() || workspaceId != m_workspaceId) {
            return deny(tr("模组修复必须落在本事务的工作区里"));
        }
        const QString projectId = argumentText(call, "projectId");
        if (!m_projectIds.contains(projectId)) {
            return require(tr("项目 %1 不在已批准的计划里").arg(projectId));
        }
        return Decision::Allow;
    }
    if (tool == QStringLiteral("backup.create")) {
        return m_tools.contains(tool) ? Decision::Allow
                                      : require(tr("备份不在已批准的计划里"));
    }
    if (tool == QStringLiteral("backup.restore") || tool == QStringLiteral("instance.repairCore")) {
        return deny(tr("这个操作不在 Auto-safe 范围（会动原实例或共享库）"));
    }
    if (tool == QStringLiteral("instance.testLaunch") || tool == QStringLiteral("repair.verify") ||
        tool == QStringLiteral("mods.bisect")) {
        // 不再限启动次数（2026-11-27 产品确认）；次数只拿去写审计。
        if (launchCount != nullptr) *launchCount = launchCountOf(call);
        return Decision::Allow;
    }
    if (tool == QStringLiteral("repair.begin") || tool == QStringLiteral("repair.finish") ||
        tool == QStringLiteral("repair.diff") || tool == QStringLiteral("repair.status")) {
        return Decision::Allow;
    }
    return require(tr("工具 %1 不在 Auto-safe 的授权类型里").arg(tool));
}

void Authorizer::record(const Agent::ToolCall &call, const AgentToolResult &result, int launches,
                        const QDateTime &now) {
    if (launches > 0) m_launches += launches;
    QJsonObject operation{{QStringLiteral("at"), now.toString(Qt::ISODate)},
                          {QStringLiteral("tool"), AgentToolRegistry::internalName(call.name)},
                          {QStringLiteral("target"), targetOf(call)},
                          {QStringLiteral("status"),
                           result.status == AgentToolResult::Status::Ok ? QStringLiteral("ok")
                           : result.status == AgentToolResult::Status::Pending
                               ? QStringLiteral("pending")
                               : QStringLiteral("error")},
                          {QStringLiteral("summary"), result.summary.left(300)}};
    if (launches > 0) operation.insert(QStringLiteral("launches"), launches);
    m_operations.append(operation);
}

QJsonObject Authorizer::scopeJson() const {
    return QJsonObject{
        {QStringLiteral("instanceId"), m_instanceId},
        {QStringLiteral("transactionId"), m_transactionId},
        {QStringLiteral("workspaceId"), m_workspaceId},
        {QStringLiteral("tools"), QJsonArray::fromStringList(m_tools)},
        {QStringLiteral("paths"), QJsonArray::fromStringList(m_paths)},
        {QStringLiteral("settingsKeys"), QJsonArray::fromStringList(m_keys)},
        {QStringLiteral("modFiles"), QJsonArray::fromStringList(m_modFiles)},
        {QStringLiteral("projectIds"), QJsonArray::fromStringList(m_projectIds)},
        {QStringLiteral("repairScopes"), QJsonArray::fromStringList(m_scopes)},
        {QStringLiteral("maxMinutes"), m_limits.maxMinutes},
        {QStringLiteral("maxLaunches"), m_limits.maxLaunches},
        {QStringLiteral("startedAt"), m_startedAt.toString(Qt::ISODate)}};
}

QJsonObject Authorizer::audit() const {
    if (!m_finishedAudit.isEmpty()) return m_finishedAudit;
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("sessionId"), m_sessionId},
        {QStringLiteral("scope"), scopeJson()},
        {QStringLiteral("launches"), m_launches},
        {QStringLiteral("operations"), m_operations},
        {QStringLiteral("verification"), m_verification},
        {QStringLiteral("notes"), QJsonArray::fromStringList(m_notes)},
        {QStringLiteral("state"), m_transactionId.isEmpty() ? QStringLiteral("no-transaction")
                                                            : QStringLiteral("active")}};
}

QJsonObject Authorizer::finish(const QString &finalState, const QString &rollbackResult,
                               const QDateTime &now) {
    if (!m_finishedAudit.isEmpty()) return m_finishedAudit;
    QJsonObject audit = this->audit();
    audit.insert(QStringLiteral("state"), finalState);
    audit.insert(QStringLiteral("endedAt"), now.toString(Qt::ISODate));
    audit.insert(QStringLiteral("launches"), m_launches);
    if (!rollbackResult.isEmpty()) audit.insert(QStringLiteral("rollback"), rollbackResult);
    if (!m_notes.isEmpty()) audit.insert(QStringLiteral("notes"), QJsonArray::fromStringList(m_notes));
    if (!m_transactionId.isEmpty()) {
        const QString dir = AgentPaths::transactionsDir() + QLatin1Char('/') + m_transactionId;
        if (QDir().mkpath(dir)) {
            const QString path = dir + QStringLiteral("/audit.json");
            QSaveFile file(path);
            if (file.open(QIODevice::WriteOnly) &&
                file.write(QJsonDocument(audit).toJson(QJsonDocument::Indented)) >= 0 &&
                file.commit()) {
                audit.insert(QStringLiteral("path"),
                             QStringLiteral("transactions/%1/audit.json").arg(m_transactionId));
            }
        }
    }
    m_finishedAudit = audit;
    m_enabled = false;
    return audit;
}

}  // namespace AgentAutoSafe
