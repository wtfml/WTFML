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
        if (problem != nullptr) *problem = QStringLiteral("找不到内嵌知识文件：%1").arg(path);  // i18n-ignore
        return false;
    }
    *bytes = file.read(kMaxKnowledgeBytes + 1);
    if (bytes->size() > kMaxKnowledgeBytes) {
        if (problem != nullptr) *problem = QStringLiteral("内嵌知识文件超过 %1 字节：%2").arg(kMaxKnowledgeBytes).arg(path);  // i18n-ignore
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
            problems->append(QStringLiteral("%1：%2 必须是数组").arg(ruleId, field));  // i18n-ignore
        }
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > limit) {
        if (problems != nullptr) {
            problems->append(QStringLiteral("%1：%2 超过 %3 条").arg(ruleId, field).arg(limit));  // i18n-ignore
        }
        return false;
    }
    for (const QJsonValue &item : array) {
        if (!item.isString()) {
            if (problems != nullptr) problems->append(QStringLiteral("%1：%2 里有非字符串项").arg(ruleId, field));  // i18n-ignore
            return false;
        }
        const QString text = item.toString().trimmed();
        if (text.isEmpty() || text.size() > kMaxStringLength) {
            if (problems != nullptr) {
                problems->append(QStringLiteral("%1：%2 里有空串或超长项").arg(ruleId, field));  // i18n-ignore
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
        result.resourceProblem = QStringLiteral("崩溃规则不是有效 JSON：%1").arg(error.errorString());  // i18n-ignore
        return result;
    }
    const QJsonObject root = document.object();
    result.version = root.value(QStringLiteral("version")).toString();
    if (result.version.isEmpty()) result.version = QStringLiteral("unknown");
    const QJsonValue rulesValue = root.value(QStringLiteral("rules"));
    if (!rulesValue.isArray()) {
        result.resourceProblem = QStringLiteral("崩溃规则缺少 rules 数组");  // i18n-ignore
        return result;
    }
    const QStringList allowed = phases();
    QHash<QString, bool> seen;
    for (const QJsonValue &value : rulesValue.toArray()) {
        if (!value.isObject()) {
            result.problems.append(QStringLiteral("规则不是对象，已跳过"));  // i18n-ignore
            continue;
        }
        const QJsonObject object = value.toObject();
        CrashRule rule;
        rule.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (!validRuleId(rule.id)) {
            result.problems.append(QStringLiteral("规则 id 不合法，已跳过：%1").arg(rule.id));  // i18n-ignore
            continue;
        }
        if (seen.contains(rule.id)) {
            result.problems.append(QStringLiteral("规则 id 重复，已跳过：%1").arg(rule.id));  // i18n-ignore
            continue;
        }
        const QJsonValue priorityValue = object.value(QStringLiteral("priority"));
        if (!priorityValue.isDouble()) {
            result.problems.append(QStringLiteral("%1：缺少 priority").arg(rule.id));  // i18n-ignore
            continue;
        }
        rule.priority = priorityValue.toInt();
        if (rule.priority < 1 || rule.priority > 100) {
            result.problems.append(QStringLiteral("%1：priority 超出 1–100").arg(rule.id));  // i18n-ignore
            continue;
        }
        rule.phase = object.value(QStringLiteral("phase")).toString();
        if (!allowed.contains(rule.phase)) {
            result.problems.append(QStringLiteral("%1：phase 不认识：%2").arg(rule.id, rule.phase));  // i18n-ignore
            continue;
        }
        QStringList problems;
        bool ok = readStrings(object.value(QStringLiteral("match")), QStringLiteral("match"),
                              rule.id, kMaxMatchStrings, &rule.match, &problems);
        if (ok && rule.match.isEmpty()) {
            problems.append(QStringLiteral("%1：match 不能为空").arg(rule.id));  // i18n-ignore
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
            problems.append(QStringLiteral("%1：confidence 必须在 0–1 之间").arg(rule.id));  // i18n-ignore
            ok = false;
        } else if (ok) {
            rule.confidence = confidenceValue.toDouble();
        }
        rule.confidenceBasis =
            object.value(QStringLiteral("confidenceBasis")).toString().trimmed();
        if (ok && (rule.confidenceBasis.isEmpty() ||
                   rule.confidenceBasis.size() > kMaxBasisLength)) {
            problems.append(QStringLiteral("%1：confidenceBasis 为空或超长").arg(rule.id));  // i18n-ignore
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
        result.resourceProblem = QStringLiteral("里程碑规则不是有效 JSON：%1").arg(error.errorString());  // i18n-ignore
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
            result.problems.append(QStringLiteral("里程碑 %1 没有标记").arg(key));  // i18n-ignore
            continue;
        }
        result.milestones.insert(key, markers);
    }
    if (!result.ok()) result.problems.append(QStringLiteral("里程碑缺少 main_menu/world_loaded/crash 之一"));  // i18n-ignore
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
