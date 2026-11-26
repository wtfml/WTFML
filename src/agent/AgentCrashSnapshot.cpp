/**
 * @file AgentCrashSnapshot.cpp
 * @brief 崩溃文本解析与快照组装（见 .hpp）。
 */

#include "AgentCrashSnapshot.hpp"

#include "AgentMixinOwnership.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

constexpr int kMaxCauseChain = 8;
constexpr int kMaxStackClasses = 40;
constexpr int kMaxSuspects = 8;
constexpr int kMaxRulesInReport = 8;
constexpr int kMaxRecommendations = 8;
constexpr int kMaxFailureRegionChars = 8000;
constexpr int kMaxExcerptChars = 400;
constexpr int kMaxModList = 2000;

/// Minecraft 日志行头：`[12:00:00] [main/ERROR]: `（第三段 `[Mixin]` 之类可有可无）。
QString logPayload(const QString &line) {
    static const QRegularExpression prefix(
        QStringLiteral(R"(^\[\d{2}:\d{2}:\d{2}(?:\.\d+)?\]\s*\[[^\]]+\](?:\s*\[[^\]]*\])?\s*:?\s?)"));
    const auto match = prefix.match(line);
    if (match.hasMatch()) return line.mid(match.capturedLength());
    return line;
}

/// 全限定名形态的异常类型；裸词不认（崩溃报告里到处是普通单词）。
QString exceptionType(const QString &text) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^([A-Za-z_$][\w$]*(?:\.[\w$]+)+(?:Exception|Error|Throwable|Failure))(?=[\s:;(]|$))"));
    const auto match = pattern.match(text.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

bool looksLikeReportHeader(const QString &trimmed) {
    return trimmed.startsWith(QStringLiteral("----")) || trimmed.startsWith(QStringLiteral("//")) ||
           trimmed.startsWith(QStringLiteral("Time:")) ||
           trimmed.startsWith(QStringLiteral("Description:")) ||
           trimmed.startsWith(QStringLiteral("--")) || trimmed.isEmpty();
}

bool looksLikeSectionBreak(const QString &trimmed) {
    return trimmed.startsWith(QStringLiteral("--")) || trimmed.startsWith(QStringLiteral("Details:"));
}

QString firstToken(const QString &text) {
    const QStringList parts = text.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString() : parts.constFirst();
}

QString versionFromFabricEntry(const QString &rest) {
    const QStringList parts = rest.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QString();
    const QString last = parts.constLast();
    if (!last.isEmpty() && last.at(0).isDigit()) return last;
    return QString();
}

QString nameFromFabricEntry(const QString &rest) {
    const QStringList parts = rest.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QString();
    const QString last = parts.constLast();
    if (last.at(0).isDigit()) {
        QStringList kept = parts;
        kept.removeLast();
        return kept.join(QLatin1Char(' '));
    }
    return rest.trimmed();
}

void addUnique(QStringList *list, const QString &value, int limit) {
    if (value.isEmpty() || list->size() >= limit) return;
    if (list->contains(value)) return;
    list->append(value);
}

/// `Suspected Mods: a (A 1.0), b (B 2.0)` → `a`、`b`。
QStringList splitSuspectedMods(const QString &value) {
    QStringList result;
    for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        QString id = part.trimmed();
        const int paren = id.indexOf(QStringLiteral(" ("));
        if (paren > 0) id = id.left(paren);
        id = id.trimmed();
        if (!id.isEmpty()) result.append(id);
    }
    return result;
}

bool isWordChar(QChar ch) {
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-') ||
           ch == QLatin1Char('.');
}

/// 按单词边界找；`core` 不该命中 `net.minecraft.core.BlockPos` 里的那一段。
bool wordContains(const QString &text, const QString &word) {
    if (word.isEmpty() || text.isEmpty()) return false;
    int index = text.indexOf(word, 0, Qt::CaseInsensitive);
    while (index >= 0) {
        const bool leftOk = index == 0 || !isWordChar(text.at(index - 1));
        const int end = index + word.size();
        const bool rightOk = end >= text.size() || !isWordChar(text.at(end));
        if (leftOk && rightOk) return true;
        index = text.indexOf(word, index + 1, Qt::CaseInsensitive);
    }
    return false;
}

/// 加载器/自带的“模组”：它们出现在栈里不代表用户装的东西有问题。
bool isPlatformModId(const QString &modId) {
    static const QStringList ids{
        QStringLiteral("minecraft"),     QStringLiteral("forge"),
        QStringLiteral("neoforge"),      QStringLiteral("fabricloader"),
        QStringLiteral("fabric-loader"), QStringLiteral("fabric-api"),
        QStringLiteral("quilt_loader"),  QStringLiteral("mixinextras"),
        QStringLiteral("java"),          QStringLiteral("mcp")};
    return ids.contains(modId.toLower());
}

struct SuspectDraft {
    QString modId;
    int tier = 3;  ///< 1 = stack frame，2 = mixin config，3 = 仅提及
    QString detail;
};

void addSuspect(QHash<QString, SuspectDraft> *drafts, QList<QString> *order, const QString &modId,
                int tier, const QString &detail) {
    const QString key = modId.toLower();
    if (key.isEmpty()) return;
    auto it = drafts->find(key);
    if (it != drafts->end()) {
        if (tier < it->tier) {
            it->tier = tier;
            it->detail = detail;
        }
        return;
    }
    SuspectDraft draft;
    draft.modId = modId;
    draft.tier = tier;
    draft.detail = detail;
    drafts->insert(key, draft);
    order->append(key);
}

double rounded(double value) {
    return qRound(value * 100.0) / 100.0;
}

}  // namespace

namespace AgentCrashSnapshot {

Parsed parse(const QString &text, bool truncated) {
    Parsed parsed;
    parsed.rawText = text;
    parsed.truncated = truncated;
    parsed.totalBytes = text.toUtf8().size();
    const QStringList lines = text.split(QLatin1Char('\n'));
    parsed.totalLines = int(lines.size());

    const QString lower = text.toLower();
    if (lower.contains(QStringLiteral("---- minecraft crash report ----"))) {
        parsed.kind = QStringLiteral("crash-report");
    } else if (lower.contains(
                   QStringLiteral("a fatal error has been detected by the java runtime environment")) ||
               lower.contains(QStringLiteral("hs_err_pid")) ||
               lower.contains(QStringLiteral("exception_access_violation"))) {
        parsed.kind = QStringLiteral("native-log");
    } else {
        parsed.kind = QStringLiteral("game-log");
    }

    // 加载器：先认 NeoForge/Quilt，再认 Fabric，最后认 Forge（NeoForge 报告里也有 forge 字样）。
    if (lower.contains(QStringLiteral("neoforge"))) {
        parsed.environment.loader = QStringLiteral("neoforge");
    } else if (lower.contains(QStringLiteral("quilt_loader")) ||
               lower.contains(QStringLiteral("quilt-loader"))) {
        parsed.environment.loader = QStringLiteral("quilt");
    } else if (lower.contains(QStringLiteral("fabric-loader")) ||
               lower.contains(QStringLiteral("fabric mods:")) ||
               lower.contains(QStringLiteral("net.fabricmc"))) {
        parsed.environment.loader = QStringLiteral("fabric");
    } else if (lower.contains(QStringLiteral("net.minecraftforge")) ||
               lower.contains(QStringLiteral("cpw.mods")) ||
               lower.contains(QStringLiteral("mod list:"))) {
        parsed.environment.loader = QStringLiteral("forge");
    }

    bool inModList = false;
    bool inFabricMods = false;
    bool inLogModList = false;
    bool failureStarted = false;
    bool failureClosed = false;
    QString failure;
    QSet<QString> stackSeen;

    for (const QString &raw : lines) {
        QString line = raw;
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        const QString payload = logPayload(line);
        const QString trimmed = payload.trimmed();
        if (trimmed.isEmpty()) continue;

        // 失败区只到下一个 "-- " 段之前：报告尾部的模组清单不算"在这段失败里提到过"。
        if (failureStarted && !failureClosed) {
            if (trimmed.startsWith(QStringLiteral("--"))) {
                failureClosed = true;
            } else if (failure.size() < kMaxFailureRegionChars) {
                failure += trimmed;
                failure += QLatin1Char('\n');
            }
        }

        // ---- 环境 ----
        if (trimmed.startsWith(QStringLiteral("Minecraft Version:"))) {
            parsed.environment.minecraftVersion = firstToken(trimmed.mid(18));
        } else if (trimmed.startsWith(QStringLiteral("Java Version:"))) {
            const QString value = trimmed.mid(13).trimmed();
            const QString number = firstToken(value);
            const QStringList parts = number.split(QRegularExpression(QStringLiteral("[._]")));
            if (!parts.isEmpty()) {
                if (parts.constFirst() == QStringLiteral("1") && parts.size() > 1) {
                    parsed.environment.javaMajor = parts.at(1).toInt();
                } else {
                    parsed.environment.javaMajor = parts.constFirst().toInt();
                }
            }
        } else if (trimmed.startsWith(QStringLiteral("Description:")) && parsed.description.isEmpty()) {
            parsed.description = trimmed.mid(12).trimmed();
        }

        // ---- 模组清单 ----
        if (trimmed == QStringLiteral("Mod List:") || trimmed.startsWith(QStringLiteral("Mod List:"))) {
            inModList = true;
            inFabricMods = false;
            continue;
        }
        if (trimmed == QStringLiteral("Fabric Mods:") || trimmed.startsWith(QStringLiteral("Fabric Mods:"))) {
            inFabricMods = true;
            inModList = false;
            continue;
        }
        if (looksLikeSectionBreak(trimmed)) {
            inModList = false;
            inFabricMods = false;
            continue;
        }
        const bool indented = line.startsWith(QLatin1Char('\t')) || line.startsWith(QStringLiteral("    "));
        // 日志里的模组清单（Fabric/Quilt 启动时打的 `Loading N mods:` + `- modid version`）。
        static const QRegularExpression loadingMods(
            QStringLiteral(R"(^Loading \d+ mods:)"));
        if (!inLogModList && loadingMods.match(trimmed).hasMatch()) {
            inLogModList = true;
            inModList = false;
            inFabricMods = false;
            continue;
        }
        if (inLogModList) {
            if (trimmed.startsWith(QStringLiteral("- "))) {
                const QStringList parts = trimmed.mid(2).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (!parts.isEmpty() && parsed.modList.size() < kMaxModList) {
                    ModEntry entry;
                    entry.modId = parts.constFirst();
                    if (parts.size() > 1) entry.version = parts.at(1);
                    parsed.modList.append(entry);
                }
                continue;
            }
            inLogModList = false;
        }
        if (inModList) {
            if (trimmed.contains(QLatin1Char('|'))) {
                const QStringList parts = trimmed.split(QLatin1Char('|'));
                if (parts.size() >= 4 && parsed.modList.size() < kMaxModList) {
                    ModEntry entry;
                    entry.fileName = parts.at(0).trimmed();
                    entry.name = parts.at(1).trimmed();
                    entry.modId = parts.at(2).trimmed();
                    entry.version = parts.at(3).trimmed();
                    if (!entry.modId.isEmpty()) parsed.modList.append(entry);
                }
                continue;
            }
            if (!indented) inModList = false;
        }
        if (inFabricMods) {
            const int colon = trimmed.indexOf(QLatin1Char(':'));
            if (indented && colon > 0 && parsed.modList.size() < kMaxModList) {
                ModEntry entry;
                entry.modId = trimmed.left(colon).trimmed();
                const QString rest = trimmed.mid(colon + 1).trimmed();
                entry.name = nameFromFabricEntry(rest);
                entry.version = versionFromFabricEntry(rest);
                parsed.modList.append(entry);
                continue;
            }
            if (!indented) inFabricMods = false;
        }

        // ---- 头部 Suspected Mods ----
        if (trimmed.startsWith(QStringLiteral("Suspected Mods:"))) {
            for (const QString &id : splitSuspectedMods(trimmed.mid(15))) {
                addUnique(&parsed.suspectedMods, id, kMaxSuspects);
            }
        }

        // ---- 异常链 ----
        if (trimmed.startsWith(QStringLiteral("Caused by:"))) {
            const QString type = exceptionType(trimmed.mid(10));
            if (!type.isEmpty()) {
                addUnique(&parsed.causeChain, type, kMaxCauseChain);
                failureStarted = true;
            }
        } else if (!failureStarted && !looksLikeReportHeader(trimmed) &&
                   !trimmed.startsWith(QStringLiteral("at "))) {
            const QString type = exceptionType(trimmed);
            if (!type.isEmpty() && parsed.exception.isEmpty()) {
                parsed.exception = type;
                parsed.causeChain.prepend(type);
                failureStarted = true;
            }
        }

        // ---- 栈帧 ----
        if (trimmed.startsWith(QStringLiteral("at "))) {
            const int paren = trimmed.indexOf(QLatin1Char('('));
            const QString frame = paren > 3 ? trimmed.mid(3, paren - 3).trimmed()
                                            : trimmed.mid(3).trimmed();
            if (!frame.isEmpty() && stackSeen.size() < kMaxStackClasses &&
                !stackSeen.contains(frame)) {
                stackSeen.insert(frame);
                parsed.stackClasses.append(frame);
            }
        }

        // ---- mixin ----
        static const QRegularExpression mixinModPattern(
            QStringLiteral(R"(Mixin apply for mod ([A-Za-z0-9_.\-]+) failed)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto mixinMod = mixinModPattern.match(trimmed);
        if (mixinMod.hasMatch()) {
            addUnique(&parsed.mixinMods, mixinMod.captured(1), kMaxSuspects);
        }
        static const QRegularExpression mixinConfigPattern(
            QStringLiteral(R"(([A-Za-z0-9_.\-]+\.mixins?\.json))"),
            QRegularExpression::CaseInsensitiveOption);
        auto configIt = mixinConfigPattern.globalMatch(trimmed);
        while (configIt.hasNext() && parsed.mixinConfigs.size() < kMaxSuspects) {
            const auto match = configIt.next();
            addUnique(&parsed.mixinConfigs, match.captured(1), kMaxSuspects);
        }
    }

    parsed.failureRegion = failure.left(kMaxFailureRegionChars);

    // 加载器版本只在能确定归属时取；Forge 从 `forge-1.20.1-47.3.0-universal.jar` 里取第二段。
    static const QRegularExpression fabricVersion(
        QStringLiteral(R"(fabric-loader-([\d.]+))"), QRegularExpression::CaseInsensitiveOption);
    const auto fabricMatch = fabricVersion.match(text);
    if (fabricMatch.hasMatch()) parsed.environment.loaderVersion = fabricMatch.captured(1);
    if (parsed.environment.loader == QStringLiteral("neoforge")) {
        static const QRegularExpression neoVersion(QStringLiteral(R"(neoforge-([\d.]+))"),
                                                  QRegularExpression::CaseInsensitiveOption);
        const auto neoMatch = neoVersion.match(text);
        if (neoMatch.hasMatch()) parsed.environment.loaderVersion = neoMatch.captured(1);
    }
    if (parsed.environment.loader == QStringLiteral("forge")) {
        static const QRegularExpression forgeJar(
            QStringLiteral(R"(forge-\d[\d.]*-([\d.]+)(?:-universal)?\.jar)"),
            QRegularExpression::CaseInsensitiveOption);
        for (const ModEntry &entry : parsed.modList) {
            const auto match = forgeJar.match(entry.fileName);
            if (match.hasMatch()) {
                parsed.environment.loaderVersion = match.captured(1);
                break;
            }
        }
    }
    return parsed;
}

Snapshot build(const SnapshotInput &input) {
    const Parsed &parsed = input.parsed;
    Snapshot snapshot;

    Environment environment = parsed.environment;
    if (environment.minecraftVersion.isEmpty()) {
        environment.minecraftVersion = input.fallback.minecraftVersion;
    }
    if (environment.loader.isEmpty()) environment.loader = input.fallback.loader.toLower();
    if (environment.loaderVersion.isEmpty()) environment.loaderVersion = input.fallback.loaderVersion;
    if (environment.javaMajor <= 0 && input.fallback.javaMajor > 0) {
        environment.javaMajor = input.fallback.javaMajor;
    }

    const QList<AgentKnowledge::CrashRule> hits = input.rules.match(parsed.rawText);
    QString phase = QStringLiteral("unknown");
    QString suggestedAction;
    QString primaryRule;
    double baseConfidence = 0.5;
    QStringList recommendedTests;
    QJsonArray ruleIds;
    for (const AgentKnowledge::CrashRule &rule : hits) {
        if (ruleIds.size() < kMaxRulesInReport) ruleIds.append(rule.id);
        if (primaryRule.isEmpty()) {
            primaryRule = rule.id;
            phase = rule.phase;
            baseConfidence = rule.confidence;
            if (!rule.suggestedActions.isEmpty()) suggestedAction = rule.suggestedActions.constFirst();
        }
        for (const QString &test : rule.recommendedTests) {
            if (recommendedTests.size() >= kMaxRecommendations) break;
            if (!recommendedTests.contains(test)) recommendedTests.append(test);
        }
    }
    snapshot.matchedRuleIds = [&ruleIds] {
        QStringList ids;
        for (const QJsonValue &value : ruleIds) ids.append(value.toString());
        return ids;
    }();

    // ---- 嫌疑模组 ----
    QHash<QString, SuspectDraft> drafts;
    QList<QString> order;
    const QString stackText = [&parsed] {
        QString text;
        for (const QString &frame : parsed.stackClasses) {
            text += frame;
            text += QLatin1Char('\n');
        }
        return text;
    }();
    for (const QString &modId : parsed.mixinMods) {
        addSuspect(&drafts, &order, modId, 1, QStringLiteral("Mixin apply for mod statement"));
    }
    for (const QString &config : parsed.mixinConfigs) {
        const QString normalized = AgentMixinOwnership::normalizeConfig(config);
        const QString owner = input.mixinOwners.value(normalized);
        if (!owner.isEmpty()) {
            addSuspect(&drafts, &order, owner, 2, QStringLiteral("mixin config %1").arg(config));
            continue;
        }
        // 读不到归属就标 unknown：不按文件名猜模组。
        addSuspect(&drafts, &order, QStringLiteral("unknown"), 3,
                   QStringLiteral("mixin config %1 has no owner declaration").arg(config));
    }
    // 失败区文本/栈帧里出现的已装模组：算“被提到/栈帧”，不算根因。
    for (const ModEntry &entry : parsed.modList) {
        if (isPlatformModId(entry.modId)) continue;
        if (!wordContains(parsed.failureRegion, entry.modId)) continue;
        const bool inFrame = wordContains(stackText, entry.modId);
        addSuspect(&drafts, &order, entry.modId, inFrame ? 1 : 3,
                   inFrame ? QStringLiteral("stack frame includes mod")
                           : QStringLiteral("mentioned in failure section"));
    }
    for (const QString &modId : parsed.suspectedMods) {
        if (isPlatformModId(modId)) continue;
        addSuspect(&drafts, &order, modId, 3, QStringLiteral("Suspected Mods header in report"));
    }
    for (const AgentKnowledge::CrashRule &rule : hits) {
        for (const QString &modId : rule.suspects) {
            if (isPlatformModId(modId)) continue;
            addSuspect(&drafts, &order, modId, 3, QStringLiteral("clue from rule %1").arg(rule.id));
        }
    }

    struct Row {
        QString modId;
        QString version;
        QString role;
        double confidence = 0.0;
        QString basis;
        int tier = 3;
    };
    QList<Row> rows;
    for (const QString &key : order) {
        const SuspectDraft &draft = drafts.value(key);
        double confidence = baseConfidence;
        QString tierLabel;
        switch (draft.tier) {
        case 1:
            confidence = qMax(0.72, baseConfidence + 0.12);
            tierLabel = QStringLiteral("stack frame");
            break;
        case 2:
            confidence = qMax(0.55, baseConfidence + 0.05);
            tierLabel = QStringLiteral("mixin config");
            break;
        default:
            confidence = qMin(0.4, baseConfidence);
            tierLabel = QStringLiteral("mention only");
            break;
        }
        Row row;
        row.modId = draft.modId;
        row.version = input.modVersions.value(draft.modId.toLower());
        if (row.version.isEmpty() && draft.modId != QStringLiteral("unknown")) {
            for (const ModEntry &entry : parsed.modList) {
                if (entry.modId.compare(draft.modId, Qt::CaseInsensitive) == 0) {
                    row.version = entry.version;
                    break;
                }
            }
        }
        row.role = draft.tier <= 2 ? QStringLiteral("candidate") : QStringLiteral("related");
        row.confidence = rounded(qMin(0.95, confidence));
        row.basis = (primaryRule.isEmpty() ? QStringLiteral("no-rule") : primaryRule) +
                    QStringLiteral(" + ") + tierLabel;
        if (!draft.detail.isEmpty()) row.basis += QStringLiteral(" (%1)").arg(draft.detail);
        row.tier = draft.tier;
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.modId < b.modId;
    });
    if (rows.size() > kMaxSuspects) rows = rows.mid(0, kMaxSuspects);

    // ---- 证据 ----
    QJsonArray evidence;
    const QString excerpt = parsed.failureRegion.isEmpty()
                                ? QString()
                                : parsed.failureRegion.left(kMaxExcerptChars);
    const QString sha256 = QString::fromLatin1(
        QCryptographicHash::hash(input.parsed.failureRegion.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
    int nextEvidence = input.firstEvidenceIndex + 1;
    const QString sourceEvidenceId = QStringLiteral("E%1").arg(nextEvidence++);
    evidence.append(QJsonObject{
        {QStringLiteral("id"), sourceEvidenceId},
        {QStringLiteral("kind"), parsed.kind},
        {QStringLiteral("source"), input.sourceLabel},
        {QStringLiteral("capturedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("excerpt"), excerpt}});
    QString mappingEvidenceId;
    if (!input.mixinOwners.isEmpty()) {
        mappingEvidenceId = QStringLiteral("E%1").arg(nextEvidence++);
        evidence.append(QJsonObject{
            {QStringLiteral("id"), mappingEvidenceId},
            {QStringLiteral("kind"), QStringLiteral("jar-declaration")},
            {QStringLiteral("source"), QStringLiteral("mods")},
            {QStringLiteral("capturedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("excerpt"),
             QStringLiteral("%1 mixin config(s) attributed to a mod via JAR declarations")
                 .arg(input.mixinOwners.size())}});
    }

    QJsonArray suspects;
    for (const Row &row : rows) {
        QJsonArray refs{sourceEvidenceId};
        if (row.tier == 2 && !mappingEvidenceId.isEmpty()) refs.append(mappingEvidenceId);
        suspects.append(QJsonObject{
            {QStringLiteral("modId"), row.modId},
            {QStringLiteral("version"), row.version},
            {QStringLiteral("role"), row.role},
            {QStringLiteral("confidence"), row.confidence},
            {QStringLiteral("confidenceBasis"), row.basis},
            {QStringLiteral("evidence"), refs}});
    }

    QJsonArray causeChain;
    for (const QString &type : parsed.causeChain) causeChain.append(type);
    QJsonArray recommended;
    for (const QString &test : recommendedTests) recommended.append(test);

    QJsonObject json{
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("kind"),
                      input.sourceKind.isEmpty() ? parsed.kind : input.sourceKind},  // i18n-ignore
                     {QStringLiteral("label"), input.sourceLabel},
                     {QStringLiteral("reportId"), input.reportId},
                     {QStringLiteral("bytes"), input.bytes > 0 ? input.bytes : parsed.totalBytes},
                     {QStringLiteral("totalLines"), parsed.totalLines}}},
        {QStringLiteral("environment"),
         QJsonObject{{QStringLiteral("minecraftVersion"), environment.minecraftVersion},
                     {QStringLiteral("loader"), environment.loader},
                     {QStringLiteral("loaderVersion"), environment.loaderVersion},
                     {QStringLiteral("javaMajor"), environment.javaMajor}}},
        {QStringLiteral("crash"),
         QJsonObject{{QStringLiteral("phase"), phase},
                     {QStringLiteral("description"), parsed.description},
                     {QStringLiteral("exception"), parsed.exception},
                     {QStringLiteral("causeChain"), causeChain},
                     {QStringLiteral("stackFrameCount"), int(parsed.stackClasses.size())}}},
        {QStringLiteral("suspects"), suspects},
        {QStringLiteral("matchedRules"), ruleIds},
        {QStringLiteral("recommendedTests"), recommended},
        {QStringLiteral("suggestedAction"), suggestedAction},
        {QStringLiteral("knowledgeVersion"), input.rules.version},
        {QStringLiteral("truncated"), parsed.truncated},
        {QStringLiteral("evidence"), evidence}};
    if (!input.rules.problems.isEmpty() || !input.rules.resourceProblem.isEmpty()) {
        QStringList problems = input.rules.problems;
        if (!input.rules.resourceProblem.isEmpty()) problems.prepend(input.rules.resourceProblem);
        if (problems.size() > 5) problems = problems.mid(0, 5);
        json.insert(QStringLiteral("knowledgeProblems"), QJsonArray::fromStringList(problems));
    }
    snapshot.json = json;
    return snapshot;
}

}  // namespace AgentCrashSnapshot
