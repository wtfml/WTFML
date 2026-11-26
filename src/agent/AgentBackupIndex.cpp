/**
 * @file AgentBackupIndex.cpp
 * @brief 维护备份落盘（见 .hpp）。
 */

#include "AgentBackupIndex.hpp"

#include "AgentPaths.hpp"
#include "util/SecretProtector.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace {
const QByteArray &backupMagic() {
    static const QByteArray magic("WTFML-AGENT-BACKUP-1\n");
    return magic;
}
}  // namespace

QJsonObject AgentBackupEntry::toJson() const {
    QJsonArray paths;
    for (const QString &path : originalPaths) paths.append(path);
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("backupId"), backupId},
                       {QStringLiteral("owner"), owner},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("storage"), storage},
                       {QStringLiteral("sourceSessionId"), sourceSessionId},
                       {QStringLiteral("instanceId"), instanceId},
                       {QStringLiteral("reason"), reason},
                       {QStringLiteral("originalPaths"), paths},
                       {QStringLiteral("sha256"), sha256},
                       {QStringLiteral("restoreHint"), restoreHint},
                       {QStringLiteral("size"), size},
                       {QStringLiteral("createdAt"),
                        createdAt.isValid() ? createdAt.toUTC().toString(Qt::ISODateWithMs)
                                            : QString()}};
}

AgentBackupEntry AgentBackupEntry::fromJson(const QJsonObject &object) {
    AgentBackupEntry entry;
    entry.backupId = object.value(QStringLiteral("backupId")).toString();
    entry.owner = object.value(QStringLiteral("owner")).toString(QStringLiteral("agent"));
    entry.source = object.value(QStringLiteral("source")).toString(QStringLiteral("agent-session"));
    entry.storage = object.value(QStringLiteral("storage")).toString(QStringLiteral("agent-maintenance"));
    entry.sourceSessionId = object.value(QStringLiteral("sourceSessionId")).toString();
    entry.instanceId = object.value(QStringLiteral("instanceId")).toString();
    entry.reason = object.value(QStringLiteral("reason")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("originalPaths")).toArray()) {
        entry.originalPaths.append(value.toString());
    }
    entry.sha256 = object.value(QStringLiteral("sha256")).toString();
    entry.restoreHint = object.value(QStringLiteral("restoreHint")).toString();
    entry.size = object.value(QStringLiteral("size")).toInteger(0);
    entry.createdAt = QDateTime::fromString(
        object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
    return entry;
}

AgentBackupIndex::AgentBackupIndex(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::maintenanceBackupDir() : QDir::cleanPath(root)) {}

void AgentBackupIndex::setRoot(const QString &root) {
    m_root = root.isEmpty() ? AgentPaths::maintenanceBackupDir() : QDir::cleanPath(root);
}

QString AgentBackupIndex::dirFor(const QString &backupId) const {
    return m_root + QStringLiteral("/") + backupId;
}

bool AgentBackupIndex::createFromBytes(const QString &sourceSessionId, const QString &instanceId,
                                       const QString &relativePath, const QByteArray &original,
                                       const QString &reason, QString *backupId,
                                       QString *problem) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString dir = dirFor(id);
    if (!QDir().mkpath(dir)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "无法创建维护备份目录。");
        return false;
    }
    const QByteArray encrypted = SecretProtector::protect(original, backupMagic());
    if (encrypted.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份加密失败，未保存。");
        return false;
    }
    {
        QSaveFile file(dir + QStringLiteral("/original.enc"));
        if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() ||
            !file.commit()) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份内容写入失败。");
            return false;
        }
    }
    AgentBackupEntry entry;
    entry.backupId = id;
    entry.sourceSessionId = sourceSessionId;
    entry.instanceId = instanceId;
    entry.reason = reason;
    entry.originalPaths = {relativePath};
    entry.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
    entry.restoreHint = QStringLiteral("Restorable by the current Windows user on this device via the agent backup metadata.");  // i18n-ignore: stored metadata, never shown
    entry.size = original.size();
    entry.createdAt = QDateTime::currentDateTimeUtc();
    if (!writeMeta(entry, problem)) {
        QDir(dir).removeRecursively();
        return false;
    }
    if (backupId != nullptr) *backupId = id;
    return true;
}

bool AgentBackupIndex::createFromFiles(const QString &sourceSessionId, const QString &instanceId,
                                      const QList<QPair<QString, QByteArray>> &files,
                                      const QString &reason, QString *backupId, QString *problem) {
    if (files.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "没有可备份的文件。");
        return false;
    }
    QJsonArray array;
    QStringList paths;
    for (const auto &file : files) {
        // base64：配置与 mods 里都有二进制（servers.dat、jar 里的字节），不能按文本拼。
        array.append(QJsonObject{{QStringLiteral("path"), file.first},
                                 {QStringLiteral("data"),
                                  QString::fromLatin1(file.second.toBase64())}});
        paths.append(file.first);
    }
    const QByteArray payload = QJsonDocument(
        QJsonObject{{QStringLiteral("format"), QStringLiteral("agent-maintenance-files")},
                    {QStringLiteral("files"), array}}).toJson(QJsonDocument::Compact);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString dir = dirFor(id);
    if (!QDir().mkpath(dir)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "无法创建维护备份目录。");
        return false;
    }
    const QByteArray encrypted = SecretProtector::protect(payload, backupMagic());
    if (encrypted.isEmpty()) {
        QDir(dir).removeRecursively();
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份加密失败，未保存。");
        return false;
    }
    {
        QSaveFile file(dir + QStringLiteral("/original.enc"));
        if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() ||
            !file.commit()) {
            QDir(dir).removeRecursively();
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份内容写入失败。");
            return false;
        }
    }
    AgentBackupEntry entry;
    entry.backupId = id;
    entry.sourceSessionId = sourceSessionId;
    entry.instanceId = instanceId;
    entry.reason = reason;
    entry.originalPaths = paths;
    entry.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    entry.restoreHint = QStringLiteral("Restorable by the current Windows user on this device via the agent backup metadata.");  // i18n-ignore: stored metadata, never shown
    entry.size = payload.size();
    entry.createdAt = QDateTime::currentDateTimeUtc();
    if (!writeMeta(entry, problem)) {
        QDir(dir).removeRecursively();
        return false;
    }
    if (backupId != nullptr) *backupId = id;
    return true;
}

bool AgentBackupIndex::readFiles(const QString &backupId,
                                 QList<QPair<QString, QByteArray>> *files, QString *problem) {
    QByteArray payload;
    if (!readOriginal(backupId, &payload, problem)) return false;
    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    QList<QPair<QString, QByteArray>> result;
    if (root.value(QStringLiteral("format")).toString() ==
        QStringLiteral("agent-maintenance-files")) {
        for (const QJsonValue &value : root.value(QStringLiteral("files")).toArray()) {
            const QJsonObject object = value.toObject();
            const QString path = object.value(QStringLiteral("path")).toString();
            const QByteArray data = QByteArray::fromBase64(
                object.value(QStringLiteral("data")).toString().toLatin1());
            if (path.isEmpty()) continue;
            result.append({path, data});
        }
    } else {
        // 单文件备份（`createFromBytes`）：原样回那一个文件，路径取元数据里那一条。
        QList<AgentBackupEntry> entries;
        list(&entries);
        for (const AgentBackupEntry &entry : entries) {
            if (entry.backupId != backupId) continue;
            if (entry.originalPaths.isEmpty()) break;
            result.append({entry.originalPaths.constFirst(), payload});
            break;
        }
    }
    if (result.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份内容为空或格式无法识别。");
        return false;
    }
    if (files != nullptr) *files = result;
    return true;
}

bool AgentBackupIndex::writeMeta(const AgentBackupEntry &entry, QString *problem) {
    const QString path = dirFor(entry.backupId) + QStringLiteral("/meta.json");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(entry.toJson()).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份元数据写入失败。");
        return false;
    }
    return true;
}

bool AgentBackupIndex::list(QList<AgentBackupEntry> *entries, QString *problem) {
    Q_UNUSED(problem);
    if (entries == nullptr) return false;
    entries->clear();
    QDir root(m_root);
    if (!root.exists()) return true;
    const QFileInfoList dirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &dir : dirs) {
        QFile meta(dir.absoluteFilePath() + QStringLiteral("/meta.json"));
        if (!meta.open(QIODevice::ReadOnly)) continue;
        const AgentBackupEntry entry =
            AgentBackupEntry::fromJson(QJsonDocument::fromJson(meta.readAll()).object());
        if (!entry.backupId.isEmpty()) entries->append(entry);
    }
    std::sort(entries->begin(), entries->end(), [](const auto &a, const auto &b) {
        return a.createdAt > b.createdAt;
    });
    return true;
}

bool AgentBackupIndex::readOriginal(const QString &backupId, QByteArray *bytes,
                                    QString *problem) {
    QFile file(dirFor(backupId) + QStringLiteral("/original.enc"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份不存在。");
        return false;
    }
    const QByteArray plain = SecretProtector::unprotect(file.readAll(), backupMagic());
    if (plain.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份无法解密。");
        return false;
    }
    if (bytes != nullptr) *bytes = plain;
    return true;
}

bool AgentBackupIndex::removeOwnedBackup(const QString &backupId, QString *problem) {
    QFile meta(dirFor(backupId) + QStringLiteral("/meta.json"));
    if (!meta.open(QIODevice::ReadOnly)) return true;
    const AgentBackupEntry entry =
        AgentBackupEntry::fromJson(QJsonDocument::fromJson(meta.readAll()).object());
    meta.close();
    if (entry.owner != QStringLiteral("agent")) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "只能清理智能体创建的备份（owner=agent）。");
        return false;
    }
    QDir dir(dirFor(backupId));
    if (!dir.exists()) return true;
    bool ok = true;
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) ok = QFile::remove(file.absoluteFilePath()) && ok;
    const QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &child : dirs) ok = QDir(child.absoluteFilePath()).removeRecursively() && ok;
    ok = QDir().rmdir(dir.absolutePath()) && ok;
    if (!ok && problem != nullptr) *problem = QCoreApplication::translate("AgentBackupIndex", "备份目录未能完全删除。");
    return ok;
}
