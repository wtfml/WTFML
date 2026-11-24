/**
 * @file AgentWorkspaceStore.cpp
 * @brief 工作区登记与清理（见 .hpp）。
 */

#include "AgentWorkspaceStore.hpp"

#include "AgentPaths.hpp"
#include "../manage/FolderManager.hpp"
#include "../util/AppPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace {

QString cleanPath(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString &a, const QString &b) {
    return QString::compare(cleanPath(a), cleanPath(b), Qt::CaseInsensitive) == 0;
}

}  // namespace

QJsonObject AgentWorkspaceRecord::toJson() const {
    return QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("sourceInstanceId"), sourceInstanceId},
        {QStringLiteral("versionId"), versionId},
        {QStringLiteral("rootPath"), rootPath},
        {QStringLiteral("status"), status},
        {QStringLiteral("createdAt"), createdAt.toString(Qt::ISODate)},
        {QStringLiteral("updatedAt"), updatedAt.toString(Qt::ISODate)},
    };
}

AgentWorkspaceRecord AgentWorkspaceRecord::fromJson(const QJsonObject &object) {
    AgentWorkspaceRecord record;
    record.id = object.value(QStringLiteral("id")).toString();
    record.sourceInstanceId = object.value(QStringLiteral("sourceInstanceId")).toString();
    record.versionId = object.value(QStringLiteral("versionId")).toString();
    record.rootPath = object.value(QStringLiteral("rootPath")).toString();
    record.status = object.value(QStringLiteral("status")).toString();
    record.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(),
                                             Qt::ISODate);
    record.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(),
                                             Qt::ISODate);
    return record;
}

AgentWorkspaceStore::AgentWorkspaceStore(const QString &recordsRoot,
                                         const QString &workspacesRoot)
    : m_recordsRoot(recordsRoot.isEmpty() ? AgentPaths::workspaceRecordsDir()
                                          : QDir::cleanPath(recordsRoot)),
      m_workspacesRoot(workspacesRoot.isEmpty() ? AppPaths::agentWorkspacesDir()
                                                : QDir::cleanPath(workspacesRoot)) {}

QString AgentWorkspaceStore::newId() {
    return QStringLiteral("ws-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString AgentWorkspaceStore::newVersionId(const QString &workspaceId) {
    QString suffix = workspaceId;
    if (suffix.startsWith(QStringLiteral("ws-"))) suffix.remove(0, 3);
    suffix.remove(QLatin1Char('-'));
    return QStringLiteral("agent-test-") + suffix.left(12).toLower();
}

bool AgentWorkspaceStore::validId(const QString &id) {
    if (!id.startsWith(QStringLiteral("ws-")) || id.size() > 80) return false;
    for (const QChar ch : id) {
        const ushort code = ch.unicode();
        const bool allowed = (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
                             (code >= '0' && code <= '9') || code == '_' || code == '-';
        if (!allowed) return false;
    }
    return true;
}

QString AgentWorkspaceStore::recordPath(const QString &id) const {
    return m_recordsRoot + QLatin1Char('/') + id + QStringLiteral(".json");
}

QString AgentWorkspaceStore::recordDir(const QString &id) const {
    return m_recordsRoot + QLatin1Char('/') + id;
}

QString AgentWorkspaceStore::logsDir(const QString &id) const {
    return recordDir(id) + QStringLiteral("/logs");
}

QString AgentWorkspaceStore::workspaceRoot(const QString &id) const {
    return m_workspacesRoot + QLatin1Char('/') + id;
}

bool AgentWorkspaceStore::writeRecord(const AgentWorkspaceRecord &record,
                                      QString *problem) const {
    if (!QDir().mkpath(m_recordsRoot)) {
        if (problem != nullptr) *problem = QStringLiteral("无法创建工作区登记目录。");  // i18n-ignore
        return false;
    }
    QSaveFile file(recordPath(record.id));
    if (!file.open(QIODevice::WriteOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("无法写入工作区登记文件。");  // i18n-ignore
        return false;
    }
    file.write(QJsonDocument(record.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (problem != nullptr) *problem = QStringLiteral("工作区登记文件保存失败。");  // i18n-ignore
        return false;
    }
    return true;
}

bool AgentWorkspaceStore::create(const AgentWorkspaceRecord &record, QString *problem) {
    if (!validId(record.id) || record.sourceInstanceId.isEmpty() || record.versionId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("工作区登记信息不完整。");  // i18n-ignore
        return false;
    }
    if (!samePath(record.rootPath, workspaceRoot(record.id))) {
        if (problem != nullptr) *problem = QStringLiteral("工作区路径与登记 id 不匹配。");  // i18n-ignore
        return false;
    }
    if (QFileInfo::exists(recordPath(record.id))) {
        if (problem != nullptr) *problem = QStringLiteral("工作区 id 已存在。");  // i18n-ignore
        return false;
    }
    if (!QDir().mkpath(workspaceRoot(record.id)) || !QDir().mkpath(logsDir(record.id))) {
        if (problem != nullptr) *problem = QStringLiteral("无法创建工作区目录。");  // i18n-ignore
        return false;
    }
    AgentWorkspaceRecord stored = record;
    if (stored.status.isEmpty()) stored.status = QStringLiteral("preparing");
    if (!stored.createdAt.isValid()) stored.createdAt = QDateTime::currentDateTimeUtc();
    stored.updatedAt = QDateTime::currentDateTimeUtc();
    if (!writeRecord(stored, problem)) return false;
    return true;
}

bool AgentWorkspaceStore::load(const QString &id, AgentWorkspaceRecord *record,
                               QString *problem) const {
    if (!validId(id)) {
        if (problem != nullptr) *problem = QStringLiteral("工作区 id 不合法。");  // i18n-ignore
        return false;
    }
    QFile file(recordPath(id));
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QStringLiteral("找不到工作区登记。");  // i18n-ignore
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (problem != nullptr) *problem = QStringLiteral("工作区登记不是有效 JSON。");  // i18n-ignore
        return false;
    }
    AgentWorkspaceRecord loaded = AgentWorkspaceRecord::fromJson(doc.object());
    if (loaded.id != id || !samePath(loaded.rootPath, workspaceRoot(id))) {
        if (problem != nullptr) *problem = QStringLiteral("工作区登记与 id 不匹配。");  // i18n-ignore
        return false;
    }
    if (record != nullptr) *record = loaded;
    return true;
}

bool AgentWorkspaceStore::list(QList<AgentWorkspaceRecord> *records,
                               QString *problem) const {
    if (records == nullptr) return false;
    records->clear();
    const QDir dir(m_recordsRoot);
    if (!dir.exists()) return true;
    const QStringList names = dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : names) {
        const QString id = name.left(name.size() - 5);
        AgentWorkspaceRecord record;
        if (load(id, &record, problem)) records->append(record);
    }
    return true;
}

bool AgentWorkspaceStore::updateStatus(const QString &id, const QString &status,
                                       QString *problem) {
    AgentWorkspaceRecord record;
    if (!load(id, &record, problem)) return false;
    record.status = status;
    record.updatedAt = QDateTime::currentDateTimeUtc();
    return writeRecord(record, problem);
}

bool AgentWorkspaceStore::remove(const QString &id, bool removeWorkspace, QString *problem) {
    AgentWorkspaceRecord record;
    if (!load(id, &record, problem)) return false;
    if (removeWorkspace && QFileInfo::exists(record.rootPath)) {
        if (!samePath(record.rootPath, workspaceRoot(id))) {
            if (problem != nullptr) *problem = QStringLiteral("拒绝删除路径不匹配的工作区。");  // i18n-ignore
            return false;
        }
        if (!removeTreeWithoutFollowingLinks(record.rootPath)) {
            if (problem != nullptr) *problem = QStringLiteral("工作区目录删不掉，可能仍被占用。");  // i18n-ignore
            return false;
        }
    }
    const QString dir = recordDir(id);
    if (QFileInfo::exists(dir) && !removeTreeWithoutFollowingLinks(dir)) {
        if (problem != nullptr) *problem = QStringLiteral("工作区日志目录删不掉。");  // i18n-ignore
        return false;
    }
    QFile::remove(recordPath(id));
    return true;
}
