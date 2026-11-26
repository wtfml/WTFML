/**
 * @file AgentKnowledgeRules.cpp
 * @brief 崩溃规则/里程碑标记的加载与匹配（见 .hpp）。
 */

#include "AgentKnowledgeRules.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QResource>

#include <algorithm>

/// qrc 编进静态库时没人引用这个符号就会被链接器丢掉；和 AgentPromptLibrary 同一个办法。
/// Q_INIT_RESOURCE 不能写在命名空间里。
static void initAgentKnowledgeResources() {
    Q_INIT_RESOURCE(agent);
}

namespace {

constexpr int kMaxKnowledgeBytes = 256 * 1024;
constexpr int kMaxMatchStrings = 6;
constexpr int kMaxRequireStrings = 4;
constexpr int kMaxSuspects = 8;
constexpr int kMaxRecommendations = 6;
constexpr int kMaxActions = 4;
constexpr int kMaxStringLength = 200;
constexpr int kMaxBasisLength = 120;
constexpr int kMaxMilestoneMarkers = 8;

void ensureResources() {
    static const bool initialized = [] {
        initAgentKnowledgeResources();
        return true;
    }();
    Q_UNUSED(initialized);
}

bool validRuleId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Z][A-Z0-9_]{2,31}$"));
    return pattern.match(id).hasMatch();
}

bool readResource(const QString &path, QByteArray *bytes, QString *problem) {
    ensureResources();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("Embedded knowledge file not found: %1").arg(path);
        return false;
    }
    *bytes = file.read(kMaxKnowledgeBytes + 1);
    if (bytes->size() > kMaxKnowledgeBytes) {
        if (problem != nullptr) *problem = QStringLiteral("Embedded knowledge file exceeds %1 bytes: %2").arg(kMaxKnowledgeBytes).arg(path);
        return false;
    }
    return true;
}

/// 读一组字面量；越界或空串时回 false 并记一条问题。
bool readStrings(const QJsonValue &value, const QString &field, const QString &ruleId, int limit,
                 QStringList *out, QStringList *problems) {
    if (value.isUndefined() || value.isNull()) return true;
    if (!value.isArray()) {
        if (problems != nullptr) {
            problems->append(QStringLiteral("%1: %2 must be an array").arg(ruleId, field));
        }
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > limit) {
        if (problems != nullptr) {
            problems->append(QStringLiteral("%1: %2 exceeds %3 items").arg(ruleId, field).arg(limit));
        }
        return false;
    }
    for (const QJsonValue &item : array) {
        if (!item.isString()) {
            if (problems != nullptr) problems->append(QStringLiteral("%1: %2 contains non-string item").arg(ruleId, field));
            return false;
        }
        const QString text = item.toString().trimmed();
        if (text.isEmpty() || text.size() > kMaxStringLength) {
            if (problems != nullptr) {
                problems->append(QStringLiteral("%1: %2 contains empty or oversized item").arg(ruleId, field));
            }
            return false;
        }
        out->append(text);
    }
    return true;
}

bool containsLiteral(const QString &text, const QStringList &needles) {
    for (const QString &needle : needles) {
        if (text.contains(needle, Qt::CaseInsensitive)) return true;
    }
    return false;
}

}  // namespace

namespace AgentKnowledge {

bool CrashRule::matches(const QString &text) const {
    if (text.isEmpty() || match.isEmpty()) return false;
    if (!containsLiteral(text, match)) return false;
    for (const QString &needle : required) {
        if (!text.contains(needle, Qt::CaseInsensitive)) return false;
    }
    return true;
}

QList<CrashRule> CrashRuleSet::match(const QString &text) const {
    QList<CrashRule> hits;
    if (text.isEmpty()) return hits;
    // 报告可以有 8 MB：整段只降一次大小写，再逐条比小写字面量。
    const QString lowered = text.toLower();
    for (const CrashRule &rule : rules) {
        bool hit = false;
        for (const QString &needle : rule.matchLower) {
            if (lowered.contains(needle)) {
                hit = true;
                break;
            }
        }
        if (!hit) continue;
        for (const QString &needle : rule.requiredLower) {
            if (!lowered.contains(needle)) {
                hit = false;
                break;
            }
        }
        if (hit) hits.append(rule);
    }
    return hits;
}

QStringList phases() {
    return {QStringLiteral("startup"),
            QStringLiteral("mod_initialization"),
            QStringLiteral("config_load"),
            QStringLiteral("world_load"),
            QStringLiteral("runtime"),
            QStringLiteral("native"),
            QStringLiteral("shutdown")};
}

CrashRuleSet parseCrashRules(const QByteArray &json) {
    CrashRuleSet result;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.resourceProblem = QStringLiteral("Crash rules are not valid JSON: %1").arg(error.errorString());
        return result;
    }
    const QJsonObject root = document.object();
    result.version = root.value(QStringLiteral("version")).toString();
    if (result.version.isEmpty()) result.version = QStringLiteral("unknown");
    const QJsonValue rulesValue = root.value(QStringLiteral("rules"));
    if (!rulesValue.isArray()) {
        result.resourceProblem = QStringLiteral("Crash rules are missing the rules array");
        return result;
    }
    const QStringList allowed = phases();
    QHash<QString, bool> seen;
    for (const QJsonValue &value : rulesValue.toArray()) {
        if (!value.isObject()) {
            result.problems.append(QStringLiteral("Rule is not an object; skipped"));
            continue;
        }
        const QJsonObject object = value.toObject();
        CrashRule rule;
        rule.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (!validRuleId(rule.id)) {
            result.problems.append(QStringLiteral("Invalid rule ID; skipped: %1").arg(rule.id));
            continue;
        }
        if (seen.contains(rule.id)) {
            result.problems.append(QStringLiteral("Duplicate rule ID; skipped: %1").arg(rule.id));
            continue;
        }
        const QJsonValue priorityValue = object.value(QStringLiteral("priority"));
        if (!priorityValue.isDouble()) {
            result.problems.append(QStringLiteral("%1: missing priority").arg(rule.id));
            continue;
        }
        rule.priority = priorityValue.toInt();
        if (rule.priority < 1 || rule.priority > 100) {
            result.problems.append(QStringLiteral("%1: priority out of range (1-100)").arg(rule.id));
            continue;
        }
        rule.phase = object.value(QStringLiteral("phase")).toString();
        if (!allowed.contains(rule.phase)) {
            result.problems.append(QStringLiteral("%1: unrecognized phase: %2").arg(rule.id, rule.phase));
            continue;
        }
        QStringList problems;
        bool ok = readStrings(object.value(QStringLiteral("match")), QStringLiteral("match"),
                              rule.id, kMaxMatchStrings, &rule.match, &problems);
        if (ok && rule.match.isEmpty()) {
            problems.append(QStringLiteral("%1: match cannot be empty").arg(rule.id));
            ok = false;
        }
        if (ok) {
            ok = readStrings(object.value(QStringLiteral("requires")), QStringLiteral("requires"),
                             rule.id, kMaxRequireStrings, &rule.required, &problems);
        }
        if (ok) {
            ok = readStrings(object.value(QStringLiteral("suspects")), QStringLiteral("suspects"),
                             rule.id, kMaxSuspects, &rule.suspects, &problems);
        }
        if (ok) {
            ok = readStrings(object.value(QStringLiteral("recommendedTests")),
                             QStringLiteral("recommendedTests"), rule.id, kMaxRecommendations,
                             &rule.recommendedTests, &problems);
        }
        if (ok) {
            ok = readStrings(object.value(QStringLiteral("suggestedActions")),
                             QStringLiteral("suggestedActions"), rule.id, kMaxActions,
                             &rule.suggestedActions, &problems);
        }
        const QJsonValue confidenceValue = object.value(QStringLiteral("confidence"));
        if (ok && (!confidenceValue.isDouble() || confidenceValue.toDouble() < 0.0 ||
                   confidenceValue.toDouble() > 1.0)) {
            problems.append(QStringLiteral("%1: confidence must be between 0 and 1").arg(rule.id));
            ok = false;
        } else if (ok) {
            rule.confidence = confidenceValue.toDouble();
        }
        rule.confidenceBasis =
            object.value(QStringLiteral("confidenceBasis")).toString().trimmed();
        if (ok && (rule.confidenceBasis.isEmpty() ||
                   rule.confidenceBasis.size() > kMaxBasisLength)) {
            problems.append(QStringLiteral("%1: confidenceBasis is empty or too long").arg(rule.id));
            ok = false;
        }
        if (!ok) {
            result.problems.append(problems);
            continue;
        }
        rule.matchLower.clear();
        for (const QString &needle : rule.match) rule.matchLower.append(needle.toLower());
        rule.requiredLower.clear();
        for (const QString &needle : rule.required) rule.requiredLower.append(needle.toLower());
        seen.insert(rule.id, true);
        result.rules.append(rule);
    }
    // priority 大的先命中；同优先级保持文件里的顺序（stable_sort）。
    std::stable_sort(result.rules.begin(), result.rules.end(),
                     [](const CrashRule &a, const CrashRule &b) { return a.priority > b.priority; });
    return result;
}

CrashRuleSet loadCrashRules() {
    static QMutex mutex;
    static bool loaded = false;
    static CrashRuleSet cached;
    QMutexLocker locker(&mutex);
    if (loaded) return cached;
    loaded = true;
    QByteArray bytes;
    QString problem;
    if (!readResource(QStringLiteral(":/agent/knowledge/crash_rules.json"), &bytes, &problem)) {
        cached.resourceProblem = problem;
        return cached;
    }
    cached = parseCrashRules(bytes);
    return cached;
}

bool MilestoneRuleSet::ok() const {
    return milestones.contains(QStringLiteral("main_menu")) &&
           milestones.contains(QStringLiteral("world_loaded")) &&
           milestones.contains(QStringLiteral("crash"));
}

MilestoneRuleSet parseMilestones(const QByteArray &json) {
    MilestoneRuleSet result;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.resourceProblem = QStringLiteral("Milestone rules are not valid JSON: %1").arg(error.errorString());
        return result;
    }
    const QJsonObject root = document.object();
    result.version = root.value(QStringLiteral("version")).toString();
    if (result.version.isEmpty()) result.version = QStringLiteral("unknown");
    const QJsonObject milestones = root.value(QStringLiteral("milestones")).toObject();
    const QStringList required{QStringLiteral("main_menu"), QStringLiteral("world_loaded"),
                               QStringLiteral("crash")};
    for (const QString &key : required) {
        QStringList markers;
        if (!readStrings(milestones.value(key), key, key, kMaxMilestoneMarkers, &markers,
                         &result.problems)) {
            continue;
        }
        if (markers.isEmpty()) {
            result.problems.append(QStringLiteral("Milestone %1 has no markers").arg(key));
            continue;
        }
        result.milestones.insert(key, markers);
    }
    if (!result.ok()) result.problems.append(QStringLiteral("Milestones are missing one of main_menu/world_loaded/crash"));
    return result;
}

MilestoneRuleSet loadMilestones() {
    static QMutex mutex;
    static bool loaded = false;
    static MilestoneRuleSet cached;
    QMutexLocker locker(&mutex);
    if (loaded) return cached;
    loaded = true;
    QByteArray bytes;
    QString problem;
    if (!readResource(QStringLiteral(":/agent/knowledge/launch_milestones.json"), &bytes,
                      &problem)) {
        cached.resourceProblem = problem;
        return cached;
    }
    cached = parseMilestones(bytes);
    return cached;
}

bool milestoneMatches(const QString &milestone, const QString &line) {
    if (line.isEmpty()) return false;
    const MilestoneRuleSet rules = loadMilestones();
    for (const QString &marker : rules.markers(milestone)) {
        if (line.contains(marker, Qt::CaseSensitive)) return true;
    }
    return false;
}

}  // namespace AgentKnowledge
