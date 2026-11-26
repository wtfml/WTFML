/**
 * @file AgentArtifactStore.cpp
 * @brief artifact 落盘/读取（见 .hpp 的模型可见性边界）。
 */

#include "AgentArtifactStore.hpp"

#include "AgentPaths.hpp"
#include "AgentRedaction.hpp"
#include "util/SecretProtector.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace {
const QByteArray &artifactMagic() {
    static const QByteArray magic("WTFML-AGENT-ARTIFACT-1\n");
    return magic;
}
constexpr qint64 kDefaultSessionQuotaBytes = 64 * 1024 * 1024;

QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}  // namespace

AgentArtifactStore::AgentArtifactStore(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::artifactsRootDir() : QDir::cleanPath(root)) {}

void AgentArtifactStore::setRoot(const QString &root) {
    m_root = root.isEmpty() ? AgentPaths::artifactsRootDir() : QDir::cleanPath(root);
}

qint64 AgentArtifactStore::defaultSessionQuotaBytes() { return kDefaultSessionQuotaBytes; }

qint64 AgentArtifactStore::sessionBytes(const QString &sessionId) const {
    const QDir dir(sessionDir(sessionId));
    if (!dir.exists()) return 0;
    qint64 total = 0;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.enc")}, QDir::Files);
    for (const QFileInfo &info : files) total += std::max<qint64>(0, info.size());
    return total;
}

QString AgentArtifactStore::sessionDir(const QString &sessionId) const {
    return m_root + QStringLiteral("/") + sessionId;
}

bool AgentArtifactStore::putModelReadable(const QString &sessionId, const QString &content,
                                          QString *artifactId, QString *problem) {
    return put(sessionId, content, true, artifactId, problem);
}

bool AgentArtifactStore::putLocalRaw(const QString &sessionId, const QString &content,
                                     QString *artifactId, QString *problem) {
    return put(sessionId, content, false, artifactId, problem);
}

bool AgentArtifactStore::put(const QString &sessionId, const QString &content, bool modelReadable,
                             QString *artifactId, QString *problem) {
    if (sessionId.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 缺少会话 ID。");
        return false;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString storedContent = modelReadable ? AgentRedaction::redact(content).text : content;
    const QByteArray plain = storedContent.toUtf8();
    const QByteArray encrypted = SecretProtector::protect(plain, artifactMagic());
    if (encrypted.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 加密失败，未写入磁盘。");
        return false;
    }
    if (m_sessionQuotaBytes > 0 &&
        sessionBytes(sessionId) + encrypted.size() > m_sessionQuotaBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentArtifactStore", "本会话的 artifact 已达上限（%1 MB），不再写入新内容。")
                           .arg(double(m_sessionQuotaBytes) / (1024.0 * 1024.0), 0, 'f', 1);
        }
        return false;
    }
    const QString dir = sessionDir(sessionId);
    if (!QDir().mkpath(dir)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "无法创建 artifact 目录。");
        return false;
    }
    const QString dataPath = dir + QStringLiteral("/") + id + QStringLiteral(".enc");
    const QString metaPath = dir + QStringLiteral("/") + id + QStringLiteral(".meta.json");
    {
        QSaveFile file(dataPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() ||
            !file.commit()) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 写入失败。");
            return false;
        }
    }
    const QJsonObject meta{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("modelReadable"), modelReadable},
        {QStringLiteral("size"), plain.size()},
        {QStringLiteral("sha256"), sha256(plain)},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QSaveFile file(metaPath);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(meta).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        QFile::remove(dataPath);
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 元数据写入失败。");
        return false;
    }
    if (artifactId != nullptr) *artifactId = id;
    return true;
}

bool AgentArtifactStore::readModel(const QString &sessionId, const QString &artifactId, int offset,
                                   int length, QString *content, QString *problem) {
    QString plain;
    if (!readEncrypted(sessionId, artifactId, true, &plain, problem)) return false;
    if (offset < 0 || length <= 0 || offset >= plain.size()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 读取范围无效。");
        return false;
    }
    const QString slice = plain.mid(offset, qMin(length, 1024 * 1024));
    // 每次读取都重新过脱敏；之前审批过不豁免。
    const AgentRedaction::Result redacted = AgentRedaction::redact(slice);
    if (content != nullptr) *content = redacted.text;
    return true;
}

bool AgentArtifactStore::readLocalRaw(const QString &sessionId, const QString &artifactId,
                                      QString *content, QString *problem) {
    QString plain;
    if (!readEncrypted(sessionId, artifactId, false, &plain, problem)) return false;
    if (content != nullptr) *content = plain;
    return true;
}

bool AgentArtifactStore::readEncrypted(const QString &sessionId, const QString &artifactId,
                                       bool requireModelReadable, QString *content,
                                       QString *problem) {
    const QString dir = sessionDir(sessionId);
    QFile metaFile(dir + QStringLiteral("/") + artifactId + QStringLiteral(".meta.json"));
    if (!metaFile.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 不存在或不可读。");
        return false;
    }
    const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
    if (requireModelReadable && !meta.value(QStringLiteral("modelReadable")).toBool(false)) {
        // 不区分“不存在”和“本地原始存档”：不泄露存在性。
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 不存在或不可读。");
        return false;
    }
    QFile data(dir + QStringLiteral("/") + artifactId + QStringLiteral(".enc"));
    if (!data.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 不存在或不可读。");
        return false;
    }
    const QByteArray plain = SecretProtector::unprotect(data.readAll(), artifactMagic());
    if (plain.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentArtifactStore", "artifact 无法解密：可能已更换 Windows 账户或设备，或内容已损坏。");
        return false;
    }
    if (content != nullptr) *content = QString::fromUtf8(plain);
    return true;
}

bool AgentArtifactStore::removeSession(const QString &sessionId) {
    QDir dir(sessionDir(sessionId));
    return !dir.exists() || dir.removeRecursively();
}
