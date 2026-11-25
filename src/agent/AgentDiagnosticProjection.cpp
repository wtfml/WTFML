/**
 * @file AgentDiagnosticProjection.cpp
 * @brief 诊断字段白名单投影（见 .hpp）。
 */

#include "AgentDiagnosticProjection.hpp"

#include "AgentRedaction.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace {

/// 本机路径的几种固定形态。比"看有没有反斜杠"更窄：证据里允许出现 `logs/latest.log`
/// 这种相对路径（它本来就是给人看的定位线索），拒绝的是带盘符、UNC、用户目录的绝对路径。
const QRegularExpression &localPathPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"((?:[A-Za-z]:[\\/])|(?:\\\\[^\\\s]+[\\/])|(?:file://)|(?:/(?:Users|home)/))"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

QJsonObject findingJson(const StartupDiagnosis::Finding &finding, int index) {
    return QJsonObject{
        {QStringLiteral("code"), QStringLiteral("startup-%1").arg(index + 1)},
        {QStringLiteral("severity"), StartupDiagnosis::severityName(finding.severity)},
        {QStringLiteral("title"), AgentRedaction::redact(finding.title).text},
        {QStringLiteral("hint"), AgentRedaction::redact(finding.hint).text},
    };
}

QString severityCountKey(StartupDiagnosis::Severity severity) {
    switch (severity) {
    case StartupDiagnosis::Severity::Problem: return QStringLiteral("problems");
    case StartupDiagnosis::Severity::Warning: return QStringLiteral("warnings");
    case StartupDiagnosis::Severity::Info: return QStringLiteral("infos");
    case StartupDiagnosis::Severity::Ok: return QStringLiteral("oks");
    }
    return QStringLiteral("infos");
}

QString evidenceId(int index) { return QStringLiteral("E%1").arg(index + 1); }

}  // namespace

namespace AgentDiagnosticProjection {

QJsonObject InstanceFacts::toJson() const {
    QJsonObject object{{QStringLiteral("instanceId"), instanceId},
                       {QStringLiteral("instanceName"), instanceName},
                       {QStringLiteral("gameVersion"), gameVersion},
                       {QStringLiteral("loaderName"), loaderName},
                       {QStringLiteral("loaderVersion"), loaderVersion},
                       {QStringLiteral("xmxMb"), xmxMb},
                       {QStringLiteral("xmsMb"), xmsMb},
                       {QStringLiteral("autoMemory"), autoMemory},
                       {QStringLiteral("modCount"), modCount},
                       {QStringLiteral("disabledModCount"), disabledModCount},
                       {QStringLiteral("running"), running}};
    if (requiredJavaMajor > 0) object.insert(QStringLiteral("requiredJavaMajor"), requiredJavaMajor);
    if (usedJavaMajor > 0) object.insert(QStringLiteral("usedJavaMajor"), usedJavaMajor);
    return object;
}

QJsonObject Evidence::toJson() const {
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("capturedAt"), capturedAt}};
    if (!sha256.isEmpty()) object.insert(QStringLiteral("sha256"), sha256);
    if (!excerpt.isEmpty()) object.insert(QStringLiteral("excerpt"), excerpt);
    return object;
}

bool containsLocalPath(const QString &text) {
    return localPathPattern().match(text).hasMatch();
}

QString sanitizeEvidence(const QString &text) {
    if (text.isEmpty()) return QString();
    QStringList kept;
    const QStringList sentences = text.split(QRegularExpression(QStringLiteral("[。\\n]")),
                                             Qt::SkipEmptyParts);
    for (const QString &sentence : sentences) {
        if (containsLocalPath(sentence)) continue;
        const QString trimmed = sentence.trimmed();
        if (trimmed.isEmpty()) continue;
        kept.append(AgentRedaction::redact(trimmed).text);
    }
    if (kept.isEmpty()) return QString();
    QString result = kept.join(QStringLiteral("。"));
    // 证据片段只做定位线索，不把整段报告搬进上下文。
    if (result.size() > 400) result = result.left(400) + QStringLiteral("…");
    return result;
}

QString scrubPaths(const QString &text) {
    if (text.isEmpty()) return text;
    QString result = text;
    result.replace(localPathPattern(), QStringLiteral("<path>"));
    return result;
}

namespace {

const QRegularExpression &launchArgPattern() {
    // 只遮“启动参数名 + 值”，参数名保留，方便判断是谁在启动。
    static const QRegularExpression pattern(QStringLiteral(
        R"((?i)(--(?:accesstoken|uuid|username|xuid|clientid|userproperties|auth\w*)\s+)(\S+))"));
    return pattern;
}

const QRegularExpression &macPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)"));
    return pattern;
}

const QRegularExpression &sidPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(\bS-1-5-21(?:-\d{5,}){3,4}\b)"));
    return pattern;
}

const QRegularExpression &hardwareFieldPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"((?i)\b(hwid|machineguid|machine_id|deviceid|device_id|boardid|serial(?:number)?)(\s*[:=]\s*)([^\s,;]+))"));
    return pattern;
}

}  // namespace

QStringList localAccountNames() {
    QStringList names;
    for (const char *key : {"USERNAME", "USER", "USERPROFILE"}) {
        QString value = qEnvironmentVariable(key).trimmed();
        if (value.isEmpty()) continue;
        if (QLatin1String(key) == QLatin1String("USERPROFILE")) {
            value = QFileInfo(value).fileName();
        }
        if (!value.isEmpty() && !names.contains(value, Qt::CaseInsensitive)) names.append(value);
    }
    return names;
}

QString scrubIdentifiers(const QString &text, const QStringList &knownNames) {
    if (text.isEmpty()) return text;
    QString result = text;
    result.replace(launchArgPattern(), QStringLiteral("\\1<redacted>"));
    result.replace(macPattern(), QStringLiteral("<mac>"));
    result.replace(sidPattern(), QStringLiteral("<sid>"));
    result.replace(hardwareFieldPattern(), QStringLiteral("\\1\\2<redacted>"));
    for (const QString &name : knownNames) {
        if (name.size() < 4) continue;  // 太短的名字会把正常单词打掉
        const QRegularExpression pattern(
            QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(name)),
            QRegularExpression::CaseInsensitiveOption);
        result.replace(pattern, QStringLiteral("<user>"));
    }
    return result;
}

QString scrubLogText(const QString &text) {
    return scrubIdentifiers(scrubPaths(text), localAccountNames());
}

QJsonObject startup(const StartupDiagnosis::Report &report, const InstanceFacts &facts,
                    QList<Evidence> *evidence) {
    QJsonObject counts;
    QJsonArray findings;
    int index = 0;
    for (const StartupDiagnosis::Finding &finding : report.findings) {
        const QString key = severityCountKey(finding.severity);
        counts.insert(key, counts.value(key).toInt() + 1);
        findings.append(findingJson(finding, index));
        const QString excerpt = sanitizeEvidence(finding.detail);
        if (evidence != nullptr) {
            Evidence item;
            item.id = evidenceId(evidence->size());
            item.kind = QStringLiteral("diagnostic");
            item.source = QStringLiteral("startup-diagnosis#%1").arg(index + 1);
            item.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            item.excerpt = excerpt;
            evidence->append(item);
        }
        index += 1;
    }
    QJsonObject object{{QStringLiteral("kind"), QStringLiteral("startup")},
                       {QStringLiteral("instance"), facts.toJson()},
                       {QStringLiteral("counts"), counts},
                       {QStringLiteral("problems"), report.problems},
                       {QStringLiteral("warnings"), report.warnings},
                       {QStringLiteral("findings"), findings}};
    if (evidence != nullptr) {
        QJsonArray ids;
        for (const Evidence &item : *evidence) ids.append(item.id);
        object.insert(QStringLiteral("evidenceIds"), ids);
    }
    return object;
}

QJsonObject crash(const GameExitDiagnosis::Facts &facts, const QStringList &hints,
                  const InstanceFacts &instance, QList<Evidence> *evidence) {
    QJsonArray hintArray;
    for (const QString &hint : hints) {
        const QString sanitized = sanitizeEvidence(hint);
        if (!sanitized.isEmpty()) hintArray.append(sanitized);
    }
    QJsonObject object{{QStringLiteral("kind"), QStringLiteral("crash")},
                       {QStringLiteral("instance"), instance.toJson()},
                       {QStringLiteral("abnormal"), GameExitDiagnosis::isAbnormal(facts)},
                       {QStringLiteral("exitCode"), facts.exitCode},
                       {QStringLiteral("crashed"), facts.crashed},
                       {QStringLiteral("exitText"),
                        sanitizeEvidence(GameExitDiagnosis::exitDescription(facts.exitCode))},
                       {QStringLiteral("errorLineCount"), facts.errorLineCount},
                       {QStringLiteral("sawOutOfMemory"), facts.sawOutOfMemory},
                       {QStringLiteral("sawMissingDependency"), facts.sawMissingDependency},
                       {QStringLiteral("logCaptured"), facts.logCaptured},
                       {QStringLiteral("hints"), hintArray}};
    if (facts.requiredJavaMajor > 0) {
        object.insert(QStringLiteral("requiredJavaMajor"), facts.requiredJavaMajor);
    }
    if (facts.usedJavaMajor > 0) {
        object.insert(QStringLiteral("usedJavaMajor"), facts.usedJavaMajor);
    }
    if (evidence != nullptr) {
        Evidence item;
        item.id = evidenceId(evidence->size());
        item.kind = QStringLiteral("diagnostic");
        item.source = QStringLiteral("game-exit-diagnosis");
        item.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        item.excerpt = sanitizeEvidence(GameExitDiagnosis::summary(facts));
        evidence->append(item);
        object.insert(QStringLiteral("evidenceIds"), QJsonArray{item.id});
    }
    return object;
}

QJsonObject integrity(const VersionIntegrity::Report &report, const InstanceFacts &facts,
                      QList<Evidence> *evidence) {
    QJsonArray groups;
    for (const VersionIntegrity::Group group : VersionIntegrity::allGroups()) {
        groups.append(QJsonObject{
            {QStringLiteral("group"), VersionIntegrity::groupName(group)},
            {QStringLiteral("missing"), report.missingIn(group)},
            {QStringLiteral("corrupt"), report.corruptIn(group)}});
    }
    // 每个分组只给计数；具体是哪几个文件属于"路径"，留在本地证据卡里。
    QJsonObject object{{QStringLiteral("kind"), QStringLiteral("integrity")},
                       {QStringLiteral("instance"), facts.toJson()},
                       {QStringLiteral("checked"), report.checked},
                       {QStringLiteral("missing"), report.missing()},
                       {QStringLiteral("corrupt"), report.corrupt()},
                       {QStringLiteral("ok"), report.ok()},
                       {QStringLiteral("groups"), groups}};
    if (!report.error.isEmpty()) {
        object.insert(QStringLiteral("error"), sanitizeEvidence(report.error));
    }
    if (evidence != nullptr) {
        Evidence item;
        item.id = evidenceId(evidence->size());
        item.kind = QStringLiteral("diagnostic");
        item.source = QStringLiteral("version-integrity");
        item.capturedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        item.excerpt = sanitizeEvidence(report.summary());
        evidence->append(item);
        object.insert(QStringLiteral("evidenceIds"), QJsonArray{item.id});
    }
    return object;
}

bool hasContent(const QJsonObject &projected) {
    if (projected.isEmpty()) return false;
    if (!projected.value(QStringLiteral("error")).toString().isEmpty()) return true;
    if (projected.contains(QStringLiteral("findings"))) {
        return !projected.value(QStringLiteral("findings")).toArray().isEmpty() ||
               projected.value(QStringLiteral("problems")).toInt() > 0;
    }
    if (projected.contains(QStringLiteral("hints"))) return true;
    if (projected.contains(QStringLiteral("checked"))) {
        return projected.value(QStringLiteral("checked")).toInt() > 0;
    }
    return true;
}

}  // namespace AgentDiagnosticProjection
