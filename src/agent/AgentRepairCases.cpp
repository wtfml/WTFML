/**
 * @file AgentRepairCases.cpp
 * @brief 本地修复病例库（见 .hpp 的存什么 / 不存什么）。
 */

#include "AgentRepairCases.hpp"

#include "AgentPaths.hpp"
#include "AgentSettings.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace AgentRepairCase {
namespace {

constexpr int kMaxCases = 200;
constexpr int kMaxActions = 12;
constexpr int kMaxFiles = 12;
constexpr int kMaxSignatureChars = 160;
constexpr int kMaxActionChars = 96;
constexpr int kMaxFileChars = 160;

QString sanitizeLabel(const QString &text, int maxChars) {
    QString clean;
    clean.reserve(text.size());
    for (const QChar character : text) {
        if (character == QLatin1Char('\n') || character == QLatin1Char('\r') ||
            character == QLatin1Char('\t')) {
            clean.append(QLatin1Char(' '));
            continue;
        }
        if (character.isPrint()) clean.append(character);
    }
    clean = clean.simplified();
    if (clean.size() > maxChars) clean = clean.left(maxChars);
    return clean;
}

QStringList sanitizeLabels(const QStringList &items, int maxItems, int maxChars) {
    QStringList result;
    for (const QString &item : items) {
        const QString clean = sanitizeLabel(item, maxChars);
        if (clean.isEmpty() || result.contains(clean)) continue;
        result.append(clean);
        if (result.size() >= maxItems) break;
    }
    return result;
}

QStringList sortedKeys(const QStringList &items) {
    QStringList copy = items;
    copy.sort();
    return copy;
}

bool sameCase(const Case &left, const Case &right) {
    return left.signature == right.signature &&
           sortedKeys(left.actions) == sortedKeys(right.actions);
}

QString fileNameFor(const QString &id) { return id + QStringLiteral(".json"); }

}  // namespace

QJsonObject Case::toJson() const {
    QJsonArray actionArray;
    for (const QString &action : actions) actionArray.append(action);
    QJsonArray fileArray;
    for (const QString &file : files) fileArray.append(file);
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("signature"), signature},
        {QStringLiteral("actions"), actionArray},
        {QStringLiteral("files"), fileArray},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("verified"), verified},
        {QStringLiteral("gameVersion"), gameVersion},
        {QStringLiteral("loader"), loader},
        {QStringLiteral("attempts"), attempts},
        {QStringLiteral("successes"), successes},
        {QStringLiteral("updatedAt"),
         QDateTime::fromMSecsSinceEpoch(updatedMs).toUTC().toString(Qt::ISODateWithMs)},
    };
}

Case Case::fromJson(const QJsonObject &object) {
    Case item;
    item.id = object.value(QStringLiteral("id")).toString();
    item.signature = object.value(QStringLiteral("signature")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("actions")).toArray()) {
        item.actions.append(value.toString());
    }
    for (const QJsonValue &value : object.value(QStringLiteral("files")).toArray()) {
        item.files.append(value.toString());
    }
    item.outcome = object.value(QStringLiteral("outcome")).toString();
    item.verified = object.value(QStringLiteral("verified")).toBool(false);
    item.gameVersion = object.value(QStringLiteral("gameVersion")).toString();
    item.loader = object.value(QStringLiteral("loader")).toString();
    item.attempts = qMax(0, object.value(QStringLiteral("attempts")).toInt(1));
    item.successes = qMax(0, object.value(QStringLiteral("successes")).toInt(0));
    const QDateTime updated = QDateTime::fromString(
        object.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
    item.updatedMs = updated.isValid() ? updated.toMSecsSinceEpoch() : 0;
    return item;
}

QString crashSignature(const QString &exception, const QString &phase,
                       const QStringList &ruleIds) {
    QStringList parts;
    if (!exception.isEmpty()) parts.append(QStringLiteral("crash:") + sanitizeLabel(exception, 80));
    if (!phase.isEmpty()) parts.append(QStringLiteral("phase:") + sanitizeLabel(phase, 40));
    QStringList rules = sanitizeLabels(ruleIds, 8, 60);
    rules.sort();
    for (const QString &rule : rules) parts.append(QStringLiteral("rule:") + rule);
    if (parts.isEmpty()) return QString();
    QString signature = parts.join(QLatin1Char('|'));
    if (signature.size() > kMaxSignatureChars) signature = signature.left(kMaxSignatureChars);
    return validSignature(signature) ? signature : QString();
}

QString repairSignature(const QStringList &scopes) {
    QStringList names;
    for (const QString &scope : sortedKeys(sanitizeLabels(scopes, 8, 24))) {
        names.append(scope);
    }
    if (names.isEmpty()) return QString();
    return QStringLiteral("repair:") + names.join(QLatin1Char('+'));
}

bool validSignature(const QString &signature) {
    if (signature.isEmpty() || signature.size() > kMaxSignatureChars) return false;
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.:+|-]+$"));
    return pattern.match(signature).hasMatch();
}

QJsonObject Entry::toModelJson() const {
    QJsonArray actionArray;
    for (const QString &action : item.actions) actionArray.append(action);
    return QJsonObject{
        {QStringLiteral("outcome"), item.outcome},
        {QStringLiteral("attempts"), item.attempts},
        {QStringLiteral("successes"), item.successes},
        {QStringLiteral("actions"), actionArray},
    };
}

double rankScore(const Case &item, qint64 nowMs) {
    const double rate = double(item.successes + 1) / double(item.attempts + 2);
    double recency = 0.0;
    if (item.updatedMs > 0 && nowMs >= item.updatedMs) {
        const double ageDays = double(nowMs - item.updatedMs) / (24.0 * 60.0 * 60.0 * 1000.0);
        recency = 0.05 / (1.0 + ageDays / 365.0);
    }
    return rate + recency;
}

Store::Store(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::repairCasesDir() : QDir::cleanPath(root)) {}

bool Store::enabled() const { return AgentSettings::repairCasesEnabled(); }

int maxCases() { return kMaxCases; }
int maxActions() { return kMaxActions; }
int maxFiles() { return kMaxFiles; }
int maxSignatureChars() { return kMaxSignatureChars; }

QList<Case> Store::all() const {
    QList<Case> items;
    const QDir dir(m_root);
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &info : files) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        Case item = Case::fromJson(object);
        if (item.id.isEmpty() || !validSignature(item.signature)) continue;
        items.append(item);
    }
    return items;
}

bool Store::record(const Case &input, QString *problem) {
    if (!enabled()) return true;
    if (!validSignature(input.signature)) {
        if (problem != nullptr) *problem = QStringLiteral("病例症状键无效。");  // i18n-ignore
        return false;
    }
    Case item = input;
    item.actions = sanitizeLabels(item.actions, kMaxActions, kMaxActionChars);
    item.files = sanitizeLabels(item.files, kMaxFiles, kMaxFileChars);
    item.attempts = qMax(1, item.attempts);
    // 验证通过就算一次成功（调用方不用同时填两个字段）。
    if (item.verified && item.successes <= 0) item.successes = item.attempts;
    item.successes = qBound(0, item.successes, item.attempts);
    item.updatedMs = item.updatedMs > 0 ? item.updatedMs : QDateTime::currentMSecsSinceEpoch();
    QList<Case> items = all();

    bool merged = false;
    for (Case &existing : items) {
        if (!sameCase(existing, item)) continue;
        existing.attempts += item.attempts;
        existing.successes = qBound(0, existing.successes + item.successes, existing.attempts);
        existing.outcome = item.outcome;
        existing.verified = item.verified;
        existing.updatedMs = item.updatedMs;
        if (!item.gameVersion.isEmpty()) existing.gameVersion = item.gameVersion;
        if (!item.loader.isEmpty()) existing.loader = item.loader;
        existing.actions = sanitizeLabels(existing.actions + item.actions, kMaxActions,
                                          kMaxActionChars);
        existing.files = sanitizeLabels(existing.files + item.files, kMaxFiles, kMaxFileChars);
        item = existing;
        merged = true;
        break;
    }
    if (!merged) {
        if (item.id.isEmpty()) item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        items.append(item);
    }

    // 超出上限就按更新时间从旧到新删；不做“按分数删”，排序是读的时候的事。
    std::sort(items.begin(), items.end(), [](const Case &left, const Case &right) {
        return left.updatedMs < right.updatedMs;
    });
    while (items.size() > kMaxCases) {
        const Case dropped = items.takeFirst();
        QFile::remove(m_root + QLatin1Char('/') + fileNameFor(dropped.id));
    }

    if (!QDir().mkpath(m_root)) {
        if (problem != nullptr) *problem = QStringLiteral("无法创建病例库目录。");  // i18n-ignore
        return false;
    }
    QSaveFile save(m_root + QLatin1Char('/') + fileNameFor(item.id));
    if (!save.open(QIODevice::WriteOnly) ||
        save.write(QJsonDocument(item.toJson()).toJson(QJsonDocument::Indented)) < 0 ||
        !save.commit()) {
        if (problem != nullptr) *problem = QStringLiteral("病例写盘失败。");  // i18n-ignore
        return false;
    }
    return true;
}

bool Store::markOutcome(const QString &signature, const QStringList &actions,
                        const QString &outcome, QString *problem) {
    if (!enabled()) return true;
    Case probe;
    probe.signature = signature;
    probe.actions = sanitizeLabels(actions, kMaxActions, kMaxActionChars);
    if (!validSignature(probe.signature)) return true;
    QList<Case> items = all();
    for (Case &existing : items) {
        if (!sameCase(existing, probe)) continue;
        existing.outcome = outcome;
        existing.verified = outcome == QStringLiteral("committed") ||
                            outcome.startsWith(QStringLiteral("verified"));
        existing.updatedMs = QDateTime::currentMSecsSinceEpoch();
        QSaveFile save(m_root + QLatin1Char('/') + fileNameFor(existing.id));
        if (!save.open(QIODevice::WriteOnly) ||
            save.write(QJsonDocument(existing.toJson()).toJson(QJsonDocument::Indented)) < 0 ||
            !save.commit()) {
            if (problem != nullptr) *problem = QStringLiteral("病例写盘失败。");  // i18n-ignore
            return false;
        }
        return true;
    }
    return true;
}

QList<Entry> Store::ranked(const QString &signature, int max) const {
    QList<Entry> entries;
    if (!enabled() || !validSignature(signature) || max <= 0) return entries;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const Case &item : all()) {
        if (item.signature != signature) continue;
        Entry entry;
        entry.item = item;
        entry.score = rankScore(item, nowMs);
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        if (std::abs(left.score - right.score) < 1e-9) {
            if (left.item.updatedMs != right.item.updatedMs) {
                return left.item.updatedMs > right.item.updatedMs;
            }
            return left.item.id < right.item.id;
        }
        return left.score > right.score;
    });
    if (entries.size() > max) entries = entries.mid(0, max);
    return entries;
}

bool Store::clear(QString *problem) {
    QDir dir(m_root);
    if (!dir.exists()) return true;
    if (!dir.removeRecursively()) {
        if (problem != nullptr) *problem = QStringLiteral("病例库删不掉。");  // i18n-ignore
        return false;
    }
    return true;
}

}  // namespace AgentRepairCase
