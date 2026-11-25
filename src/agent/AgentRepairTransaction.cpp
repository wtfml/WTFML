/**
 * @file AgentRepairTransaction.cpp
 * @brief 事务日志与文件差异（见 .hpp）。
 */

#include "AgentRepairTransaction.hpp"

#include "AgentPaths.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace {

QString relativePath(const QString &root, const QString &path) {
    const QString cleanRoot = QDir::cleanPath(root);
    const QString clean = QDir::cleanPath(path);
    if (clean.startsWith(cleanRoot + QLatin1Char('/'))) return clean.mid(cleanRoot.size() + 1);
    return QFileInfo(path).fileName();
}

QString sha256OfFile(const QString &path, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("读不动文件：%1").arg(QFileInfo(path).fileName());  // i18n-ignore
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (problem != nullptr) *problem = QStringLiteral("读文件哈希失败：%1").arg(QFileInfo(path).fileName());  // i18n-ignore
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool scanFiles(const QString &root, const QStringList &scopes,
               QList<QPair<QString, QString>> *files, QString *problem) {
    if (files == nullptr) return false;
    files->clear();
    for (const QString &scope : scopes) {
        if (scope == QStringLiteral("settings")) continue;
        if (scope == QStringLiteral("options")) {
            const QString path = root + QStringLiteral("/options.txt");
            if (QFileInfo(path).isFile()) files->append({QStringLiteral("options.txt"), path});
            continue;
        }
        if (scope != QStringLiteral("mods") && scope != QStringLiteral("config") &&
            scope != QStringLiteral("defaultconfigs")) {
            continue;
        }
        const QString dirPath = root + QLatin1Char('/') + scope;
        if (!QFileInfo(dirPath).isDir()) continue;
        QDirIterator it(dirPath, QDir::Files | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            files->append({relativePath(root, it.fileInfo().absoluteFilePath()),
                           it.fileInfo().absoluteFilePath()});
            if (files->size() > 20000) {
                if (problem != nullptr) *problem = QStringLiteral("事务范围文件超过 20000 个。");  // i18n-ignore
                return false;
            }
        }
    }
    return true;
}

}  // namespace

QJsonObject AgentRepairFileChange::toJson() const {
    QString kindText = QStringLiteral("modified");
    if (kind == Kind::Added) kindText = QStringLiteral("added");
    if (kind == Kind::Deleted) kindText = QStringLiteral("deleted");
    return QJsonObject{
        {QStringLiteral("kind"), kindText},
        {QStringLiteral("path"), relativePath},
        {QStringLiteral("beforeSha256"), beforeSha256},
        {QStringLiteral("afterSha256"), afterSha256},
        {QStringLiteral("beforeBytes"), beforeBytes},
        {QStringLiteral("afterBytes"), afterBytes},
    };
}

QJsonObject AgentRepairTransactionRecord::toJson() const {
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("instanceId"), instanceId},
        {QStringLiteral("workspaceId"), workspaceId},
        {QStringLiteral("state"), state},
        {QStringLiteral("scopes"), QJsonArray::fromStringList(scopes)},
        {QStringLiteral("baseline"), baseline},
        {QStringLiteral("settings"), settings},
        {QStringLiteral("verification"), verification},
        {QStringLiteral("symptomKey"), symptomKey},
        {QStringLiteral("actions"), QJsonArray::fromStringList(actions)},
        {QStringLiteral("problem"), problem},
        {QStringLiteral("createdAt"), createdAt.toString(Qt::ISODate)},
        {QStringLiteral("updatedAt"), updatedAt.toString(Qt::ISODate)},
    };
}

AgentRepairTransactionRecord AgentRepairTransactionRecord::fromJson(const QJsonObject &object) {
    AgentRepairTransactionRecord record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.instanceId = object.value(QStringLiteral("instanceId")).toString();
    record.workspaceId = object.value(QStringLiteral("workspaceId")).toString();
    record.state = object.value(QStringLiteral("state")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("scopes")).toArray()) {
        if (value.isString()) record.scopes.append(value.toString());
    }
    record.baseline = object.value(QStringLiteral("baseline")).toObject();
    record.settings = object.value(QStringLiteral("settings")).toObject();
    record.verification = object.value(QStringLiteral("verification")).toObject();
    record.symptomKey = object.value(QStringLiteral("symptomKey")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("actions")).toArray()) {
        if (value.isString()) record.actions.append(value.toString());
    }
    record.problem = object.value(QStringLiteral("problem")).toString();
    record.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(),
                                             Qt::ISODate);
    record.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(),
                                             Qt::ISODate);
    return record;
}

bool AgentRepairTransactionRecord::active() const {
    return state != QStringLiteral("committed") && state != QStringLiteral("rolled_back") &&
           state != QStringLiteral("cancelled");
}

AgentRepairTransactionStore::AgentRepairTransactionStore(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::transactionsDir() : QDir::cleanPath(root)) {}

QString AgentRepairTransactionStore::newId() {
    return QStringLiteral("tx-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool AgentRepairTransactionStore::validId(const QString &id) {
    if (!id.startsWith(QStringLiteral("tx-")) || id.size() > 80) return false;
    for (const QChar ch : id) {
        const ushort code = ch.unicode();
        const bool allowed = (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
                             (code >= '0' && code <= '9') || code == '_' || code == '-';
        if (!allowed) return false;
    }
    return true;
}

QStringList AgentRepairTransactionStore::validScopes() {
    return {QStringLiteral("mods"), QStringLiteral("config"), QStringLiteral("defaultconfigs"),
            QStringLiteral("options"), QStringLiteral("settings")};
}

bool AgentRepairTransactionStore::validScope(const QString &scope) {
    return validScopes().contains(scope);
}

QString AgentRepairTransactionStore::recordPath(const QString &id) const {
    return m_root + QLatin1Char('/') + id + QStringLiteral(".json");
}

bool AgentRepairTransactionStore::writeRecord(const AgentRepairTransactionRecord &record,
                                              QString *problem) const {
    if (!QDir().mkpath(m_root)) {
        if (problem != nullptr) *problem = QStringLiteral("无法创建事务目录。");  // i18n-ignore
        return false;
    }
    QSaveFile file(recordPath(record.id));
    if (!file.open(QIODevice::WriteOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("无法写入事务日志。");  // i18n-ignore
        return false;
    }
    file.write(QJsonDocument(record.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (problem != nullptr) *problem = QStringLiteral("事务日志保存失败。");  // i18n-ignore
        return false;
    }
    return true;
}

bool AgentRepairTransactionStore::create(const AgentRepairTransactionRecord &record,
                                         QString *problem) {
    if (!validId(record.id) || record.instanceId.isEmpty() || record.workspaceId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("事务登记信息不完整。");  // i18n-ignore
        return false;
    }
    if (QFileInfo::exists(recordPath(record.id))) {
        if (problem != nullptr) *problem = QStringLiteral("事务 id 已存在。");  // i18n-ignore
        return false;
    }
    AgentRepairTransactionRecord stored = record;
    if (stored.state.isEmpty()) stored.state = QStringLiteral("preparing");
    if (!stored.createdAt.isValid()) stored.createdAt = QDateTime::currentDateTimeUtc();
    stored.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(stored, problem);
}

bool AgentRepairTransactionStore::load(const QString &id, AgentRepairTransactionRecord *record,
                                       QString *problem) const {
    if (!validId(id)) {
        if (problem != nullptr) *problem = QStringLiteral("事务 id 不合法。");  // i18n-ignore
        return false;
    }
    QFile file(recordPath(id));
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("找不到事务日志。");  // i18n-ignore
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (problem != nullptr) *problem = QStringLiteral("事务日志不是有效 JSON。");  // i18n-ignore
        return false;
    }
    AgentRepairTransactionRecord loaded = AgentRepairTransactionRecord::fromJson(doc.object());
    if (loaded.id != id || loaded.instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("事务日志与 id 不匹配。");  // i18n-ignore
        return false;
    }
    if (record != nullptr) *record = loaded;
    return true;
}

bool AgentRepairTransactionStore::list(QList<AgentRepairTransactionRecord> *records,
                                       QString *problem) const {
    if (records == nullptr) return false;
    records->clear();
    const QDir dir(m_root);
    if (!dir.exists()) return true;
    const QStringList names = dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : names) {
        AgentRepairTransactionRecord record;
        if (load(name.left(name.size() - 5), &record, problem)) records->append(record);
    }
    return true;
}

bool AgentRepairTransactionStore::save(const AgentRepairTransactionRecord &record,
                                       QString *problem) {
    AgentRepairTransactionRecord stored = record;
    stored.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(stored, problem);
}

bool AgentRepairTransactionStore::updateState(const QString &id, const QString &state,
                                              const QString &problem, QString *outProblem) {
    AgentRepairTransactionRecord record;
    if (!load(id, &record, outProblem)) return false;
    record.state = state;
    record.problem = problem;
    record.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(record, outProblem);
}

bool AgentRepairTransactionStore::recordSetting(const QString &id, const QString &key,
                                                const QJsonValue &oldValue,
                                                const QJsonValue &newValue, QString *problem) {
    AgentRepairTransactionRecord record;
    if (!load(id, &record, problem)) return false;
    QJsonObject entry = record.settings.value(key).toObject();
    if (entry.isEmpty()) entry.insert(QStringLiteral("old"), oldValue);
    entry.insert(QStringLiteral("new"), newValue);
    record.settings.insert(key, entry);
    record.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(record, problem);
}

bool AgentRepairTransactionStore::setVerification(const QString &id,
                                                  const QJsonObject &verification,
                                                  QString *problem) {
    AgentRepairTransactionRecord record;
    if (!load(id, &record, problem)) return false;
    record.verification = verification;
    record.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(record, problem);
}

bool AgentRepairTransactionStore::loadByWorkspace(const QString &workspaceId,
                                                  AgentRepairTransactionRecord *record,
                                                  QString *problem) const {
    QList<AgentRepairTransactionRecord> records;
    if (!list(&records, problem)) return false;
    for (const auto &item : records) {
        if (item.workspaceId == workspaceId) {
            if (record != nullptr) *record = item;
            return true;
        }
    }
    if (problem != nullptr) *problem = QStringLiteral("找不到这个工作区对应的事务。");  // i18n-ignore
    return false;
}

bool AgentRepairTransactionStore::activeForInstance(const QString &instanceId,
                                                    AgentRepairTransactionRecord *record,
                                                    QString *problem) const {
    QList<AgentRepairTransactionRecord> records;
    if (!list(&records, problem)) return false;
    for (const auto &item : records) {
        if (item.instanceId == instanceId && item.active()) {
            if (record != nullptr) *record = item;
            return true;
        }
    }
    return false;
}

QJsonObject AgentRepairTransactionStore::captureBaseline(const QString &instanceRoot,
                                                         const QStringList &scopes,
                                                         QString *problem) {
    QList<QPair<QString, QString>> files;
    if (!scanFiles(instanceRoot, scopes, &files, problem)) return {};
    QJsonObject baseline;
    qint64 totalBytes = 0;
    for (const auto &entry : files) {
        QString hashProblem;
        const QString hash = sha256OfFile(entry.second, &hashProblem);
        if (hash.isEmpty()) {
            if (problem != nullptr && problem->isEmpty()) *problem = hashProblem;
            return {};
        }
        baseline.insert(entry.first, hash);
        totalBytes += QFileInfo(entry.second).size();
        if (totalBytes > 512LL * 1024 * 1024) {
            if (problem != nullptr) *problem = QStringLiteral("事务范围超过 512 MB。");  // i18n-ignore
            return {};
        }
    }
    return baseline;
}

QList<AgentRepairFileChange> AgentRepairTransactionStore::diff(const QString &workspaceRoot,
                                                               const QJsonObject &baseline,
                                                               const QStringList &scopes,
                                                               QString *problem) {
    QList<QPair<QString, QString>> files;
    if (!scanFiles(workspaceRoot, scopes, &files, problem)) return {};
    QHash<QString, QString> current;
    QHash<QString, qint64> currentBytes;
    for (const auto &entry : files) {
        QString hashProblem;
        const QString hash = sha256OfFile(entry.second, &hashProblem);
        if (hash.isEmpty()) {
            if (problem != nullptr && problem->isEmpty()) *problem = hashProblem;
            return {};
        }
        current.insert(entry.first, hash);
        currentBytes.insert(entry.first, QFileInfo(entry.second).size());
    }
    QList<AgentRepairFileChange> changes;
    QSet<QString> seen;
    for (auto it = current.begin(); it != current.end(); ++it) {
        seen.insert(it.key());
        const QString before = baseline.value(it.key()).toString();
        if (before == it.value()) continue;
        AgentRepairFileChange change;
        change.relativePath = it.key();
        change.beforeSha256 = before;
        change.afterSha256 = it.value();
        change.kind = before.isEmpty() ? AgentRepairFileChange::Kind::Added
                                       : AgentRepairFileChange::Kind::Modified;
        change.beforeBytes = -1;
        change.afterBytes = currentBytes.value(it.key());
        changes.append(change);
    }
    for (auto it = baseline.begin(); it != baseline.end(); ++it) {
        if (seen.contains(it.key())) continue;
        AgentRepairFileChange change;
        change.relativePath = it.key();
        change.beforeSha256 = it.value().toString();
        change.kind = AgentRepairFileChange::Kind::Deleted;
        change.beforeBytes = -1;
        changes.append(change);
    }
    return changes;
}
