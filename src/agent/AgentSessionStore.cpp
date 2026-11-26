/**
 * @file AgentSessionStore.cpp
 * @brief 会话落盘实现（见 .hpp 的稳定点/顺序约束）。
 */

#include "AgentSessionStore.hpp"

#include "AgentPaths.hpp"
#include "AgentSessionCrypto.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

namespace {

QString nowStamp() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

QString recoveryStamp() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz"));
}

QString sha256Hex(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

int snapshotCount() { return 3; }

}  // namespace

AgentSessionStore::AgentSessionStore(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::dataDir() : QDir::cleanPath(root)) {}

void AgentSessionStore::setRoot(const QString &root) {
    m_root = root.isEmpty() ? AgentPaths::dataDir() : QDir::cleanPath(root);
}

QString AgentSessionStore::sessionsDir() const { return m_root + QStringLiteral("/sessions"); }
int AgentSessionStore::maxBodyBytes() { return 8 * 1024 * 1024; }
int AgentSessionStore::maxMessages() { return 2000; }

QString AgentSessionStore::metaPath(const QString &id) const {
    return sessionsDir() + QLatin1Char('/') + id + QStringLiteral(".meta.json");
}
QString AgentSessionStore::plainBodyPath(const QString &id) const {
    return sessionsDir() + QLatin1Char('/') + id + QStringLiteral(".json");
}
QString AgentSessionStore::encryptedBodyPath(const QString &id) const {
    return sessionsDir() + QLatin1Char('/') + id + QStringLiteral(".enc");
}
QString AgentSessionStore::bodyPath(const AgentSessionMeta &meta) const {
    return meta.encrypted ? encryptedBodyPath(meta.id) : plainBodyPath(meta.id);
}
QString AgentSessionStore::recoveryDir(const QString &id) const {
    return sessionsDir() + QStringLiteral("/.recovery/") + id;
}
QString AgentSessionStore::artifactDir(const QString &id) const {
    return m_root + QStringLiteral("/artifacts/") + id;
}

bool AgentSessionStore::writeAtomic(const QString &path, const QByteArray &bytes,
                                    QString *problem) const {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法创建目录：%1").arg(info.absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法写入：%1").arg(path);
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "写盘失败：%1").arg(path);
        return false;
    }
    return true;
}

bool AgentSessionStore::readMeta(const QString &path, AgentSessionMeta *meta,
                                 QString *problem) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法读取元数据：%1").arg(path);
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "元数据损坏：%1").arg(path);
        return false;
    }
    const AgentSessionMeta parsed = AgentSessionMeta::fromJson(document.object());
    if (parsed.schema > 1) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSessionStore", "会话版本过新（schema %1），当前版本不覆盖原文件。")
                           .arg(parsed.schema);
        }
        return false;
    }
    if (parsed.id.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "元数据缺少 id：%1").arg(path);
        return false;
    }
    *meta = parsed;
    return true;
}

bool AgentSessionStore::writeMeta(const AgentSessionMeta &meta, QString *problem) const {
    return writeAtomic(metaPath(meta.id),
                       QJsonDocument(meta.toJson()).toJson(QJsonDocument::Indented), problem);
}

bool AgentSessionStore::create(AgentSession *session, QString *problem) {
    if (session == nullptr) {
        return false;
    }
    AgentSession created;
    created.meta.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    created.meta.title = AgentSessionTitle::defaultTitle();
    created.meta.createdAt = QDateTime::currentDateTimeUtc();
    created.meta.updatedAt = created.meta.createdAt;
    created.meta.encrypted = false;
    created.meta.skill = QStringLiteral("general");
    if (!save(created, false, problem)) {
        return false;
    }
    *session = created;
    return true;
}

bool AgentSessionStore::save(const AgentSession &session, bool snapshot, QString *problem) {
    if (session.meta.id.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "会话缺少 id。");
        return false;
    }
    if (session.messages.size() > maxMessages()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSessionStore", "会话超过 %1 条消息，已停止追加，可导出或新建会话。")
                           .arg(maxMessages());
        }
        return false;
    }

    QJsonArray messages;
    for (const Agent::Message &message : session.messages) {
        messages.append(message.toJson());
    }
    QJsonObject body{{QStringLiteral("schema"), 1},
                     {QStringLiteral("messages"), messages},
                     {QStringLiteral("summary"), session.summary},
                     {QStringLiteral("usage"), session.usage.toJson()},
                     {QStringLiteral("pendingApproval"), session.pendingApproval}};
    QByteArray plain = QJsonDocument(body).toJson(QJsonDocument::Indented);
    if (plain.size() > maxBodyBytes()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSessionStore", "会话正文超过 %1 MB，已停止追加，可导出或新建会话。")
                           .arg(maxBodyBytes() / (1024 * 1024));
        }
        return false;
    }

    AgentSessionMeta meta = session.meta;
    meta.messageCount = session.messages.size();
    meta.updatedAt = QDateTime::currentDateTimeUtc();
    if (!session.messages.isEmpty() && session.messages.constLast().createdAt.isValid()) {
        meta.updatedAt = std::max(meta.updatedAt, session.messages.constLast().createdAt.toUTC());
    }
    if (!session.messages.isEmpty()) {
        const Agent::Message &last = session.messages.constLast();
        meta.lastTurnState = Agent::statusKey(last.status);
    }

    QByteArray finalBytes = plain;
    if (meta.encrypted) {
        finalBytes = AgentSessionCrypto::encrypt(plain);
        if (finalBytes.isEmpty()) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("AgentSessionStore", "整会话加密失败，已保留原正文。");
            }
            return false;
        }
    }
    meta.bodySha256 = sha256Hex(finalBytes);

    if (!writeAtomic(bodyPath(meta), finalBytes, problem)) {
        return false;
    }
    // 切换加密状态后，另一种格式的旧正文必须删掉；删不掉不算保存成功（否则明文还在）。
    const QString stale = meta.encrypted ? plainBodyPath(meta.id) : encryptedBodyPath(meta.id);
    if (QFileInfo::exists(stale) && !QFile::remove(stale)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法删除旧正文：%1").arg(stale);
        return false;
    }
    if (!writeMeta(meta, problem)) {
        return false;
    }
    if (snapshot) {
        snapshotAfterSave(meta.id, nullptr);
    }
    return true;
}

bool AgentSessionStore::load(const QString &sessionId, AgentSession *session, QString *problem) {
    if (session == nullptr || sessionId.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "会话 id 为空。");
        return false;
    }
    AgentSessionMeta meta;
    if (!readMeta(metaPath(sessionId), &meta, problem)) {
        return false;
    }
    const QString path = bodyPath(meta);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = meta.encrypted ? QCoreApplication::translate("AgentSessionStore", "无法解密：加密正文不存在或不可读。")
                                      : QCoreApplication::translate("AgentSessionStore", "无法读取会话正文：%1").arg(path);
        }
        return false;
    }
    if (file.size() > maxBodyBytes()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "会话正文超过大小上限，拒绝载入。");
        return false;
    }
    QByteArray stored = file.readAll();
    file.close();

    QByteArray plain = stored;
    if (meta.encrypted) {
        plain = AgentSessionCrypto::decrypt(stored);
        if (plain.isEmpty()) {
            // 明确不回退明文：换用户/换机器/值损坏都只能报错。
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法解密该会话（账户变更、设备变更或内容损坏）。");
            return false;
        }
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(plain, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        quarantine(path, nullptr);
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSessionStore", "会话正文损坏，已隔离原文件，可从恢复快照手动恢复。");
        }
        return false;
    }
    const QJsonObject root = document.object();
    const int schema = root.value(QStringLiteral("schema")).toInt(1);
    if (schema > 1) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSessionStore", "会话版本过新（schema %1），不覆盖原文件。").arg(schema);
        }
        return false;
    }

    AgentSession loaded;
    loaded.meta = meta;
    for (const QJsonValue &value : root.value(QStringLiteral("messages")).toArray()) {
        Agent::Message message = Agent::Message::fromJson(value.toObject());
        if (message.status == Agent::MessageStatus::Streaming) {
            // 上次退出时还在流式：恢复成“已中断”，不自动重发。
            message.status = Agent::MessageStatus::Interrupted;
        }
        loaded.messages.append(message);
    }
    loaded.summary = root.value(QStringLiteral("summary")).toObject();
    loaded.usage = Agent::Usage::fromJson(root.value(QStringLiteral("usage")).toObject());
    loaded.pendingApproval = root.value(QStringLiteral("pendingApproval")).toObject();

    // 两次提交之间崩溃：正文是真相，元数据按正文重算（不删原文件）。
    const QString actualSha = sha256Hex(stored);
    if (meta.bodySha256 != actualSha || meta.messageCount != loaded.messages.size()) {
        loaded.meta.bodySha256 = actualSha;
        loaded.meta.messageCount = loaded.messages.size();
        loaded.meta.lastTurnState = loaded.messages.isEmpty()
                                        ? QStringLiteral("complete")
                                        : Agent::statusKey(loaded.messages.constLast().status);
        writeMeta(loaded.meta, nullptr);
    }
    // 流式恢复成 interrupted 之后，元数据也跟着改（只在盘上那一份需要时）。
    const QString lastState = loaded.messages.isEmpty()
                                  ? QStringLiteral("complete")
                                  : Agent::statusKey(loaded.messages.constLast().status);
    if (loaded.meta.lastTurnState != lastState) {
        loaded.meta.lastTurnState = lastState;
        writeMeta(loaded.meta, nullptr);
    }
    *session = loaded;
    return true;
}

bool AgentSessionStore::list(QList<AgentSessionMeta> *metas, QString *problem) {
    if (metas == nullptr) {
        return false;
    }
    metas->clear();
    QDir dir(sessionsDir());
    if (!dir.exists()) {
        return true;
    }
    QStringList problems;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.meta.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        AgentSessionMeta meta;
        QString error;
        if (readMeta(file.absoluteFilePath(), &meta, &error)) {
            metas->append(meta);
            continue;
        }
        problems.append(error);
        // 损坏的元数据不当作空会话，也不从列表里悄悄消失。
        AgentSessionMeta broken;
        broken.id = file.fileName().chopped(QStringLiteral(".meta.json").size());
        broken.title = QCoreApplication::translate("AgentSessionStore", "损坏，已隔离");
        broken.updatedAt = file.lastModified().toUTC();
        broken.corrupt = true;
        if (!broken.id.isEmpty()) {
            metas->append(broken);
        }
    }
    std::sort(metas->begin(), metas->end(), [](const AgentSessionMeta &a, const AgentSessionMeta &b) {
        if (a.pinned != b.pinned) return a.pinned;
        if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
        return a.id < b.id;
    });
    if (!problems.isEmpty() && problem != nullptr) {
        *problem = problems.join(QLatin1Char('\n'));
    }
    return true;
}

bool AgentSessionStore::rename(const QString &sessionId, const QString &title, QString *problem) {
    AgentSessionMeta meta;
    if (!readMeta(metaPath(sessionId), &meta, problem)) return false;
    QString cleaned = title.simplified();
    if (cleaned.isEmpty()) cleaned = AgentSessionTitle::defaultTitle();
    meta.title = cleaned.left(80);
    meta.updatedAt = QDateTime::currentDateTimeUtc();
    return writeMeta(meta, problem);
}

bool AgentSessionStore::setPinned(const QString &sessionId, bool pinned, QString *problem) {
    AgentSessionMeta meta;
    if (!readMeta(metaPath(sessionId), &meta, problem)) return false;
    meta.pinned = pinned;
    meta.updatedAt = QDateTime::currentDateTimeUtc();
    return writeMeta(meta, problem);
}

bool AgentSessionStore::setEncrypted(const QString &sessionId, bool encrypted, QString *problem) {
    AgentSession session;
    if (!load(sessionId, &session, problem)) return false;
    if (session.meta.encrypted == encrypted) return true;
    session.meta.encrypted = encrypted;
    // save() 先写新格式，成功后才删旧格式；失败时旧明文/密文原样保留。
    return save(session, false, problem);
}

bool AgentSessionStore::requestDeletion(const QString &sessionId, QString *problem) {
    AgentSessionMeta meta;
    if (!readMeta(metaPath(sessionId), &meta, problem)) return false;
    meta.deletionState = QStringLiteral("pending");
    return writeMeta(meta, problem);
}

bool AgentSessionStore::completeDeletion(const QString &sessionId, QString *problem,
                                         QString *stageOut) {
    AgentSessionMeta meta;
    if (!readMeta(metaPath(sessionId), &meta, problem)) return false;
    if (meta.deletionState != QLatin1String("pending") &&
        meta.deletionState != QLatin1String("deleting")) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "会话没有待删除标记。");
        return false;
    }
    meta.deletionState = QStringLiteral("deleting");
    if (!writeMeta(meta, problem)) return false;

    for (const QString &path : {plainBodyPath(sessionId), encryptedBodyPath(sessionId)}) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "正文删除失败：%1").arg(path);
            if (stageOut != nullptr) *stageOut = QStringLiteral("body");
            return false;
        }
    }
    for (const QString &path : {recoveryDir(sessionId), artifactDir(sessionId)}) {
        QDir dir(path);
        if (dir.exists() && !dir.removeRecursively()) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "会话附属文件删除失败：%1").arg(path);
            if (stageOut != nullptr) *stageOut = path.endsWith(QStringLiteral("artifacts"))
                                                    ? QStringLiteral("artifacts")
                                                    : QStringLiteral("snapshots");
            return false;
        }
    }
    if (QFileInfo::exists(metaPath(sessionId)) && !QFile::remove(metaPath(sessionId))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "元数据删除失败：%1").arg(metaPath(sessionId));
        if (stageOut != nullptr) *stageOut = QStringLiteral("metadata");
        return false;
    }
    return true;
}

bool AgentSessionStore::finalizePendingDeletions(QStringList *problems) {
    QDir dir(sessionsDir());
    if (!dir.exists()) {
        return true;
    }
    bool ok = true;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.meta.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        AgentSessionMeta meta;
        if (!readMeta(file.absoluteFilePath(), &meta, nullptr)) {
            continue;
        }
        if (meta.deletionState != QLatin1String("pending") &&
            meta.deletionState != QLatin1String("deleting")) {
            continue;
        }
        QString error;
        QString stage;
        if (!completeDeletion(meta.id, &error, &stage)) {
            ok = false;
            if (problems != nullptr) {
                problems->append(QCoreApplication::translate("AgentSessionStore", "%1（%2）：%3").arg(meta.id, stage, error));
            }
        }
    }
    return ok;
}

void AgentSessionStore::snapshotAfterSave(const QString &id, QString *problem) const {
    const QString directory = recoveryDir(id) + QLatin1Char('/') + recoveryStamp();
    if (!QDir().mkpath(directory)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "无法创建恢复快照目录。");
        return;
    }
    const QString metaSource = metaPath(id);
    if (!QFile::copy(metaSource, directory + QStringLiteral("/meta.json"))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "恢复快照复制元数据失败。");
    }
    QString bodySource = plainBodyPath(id);
    if (!QFileInfo::exists(bodySource)) {
        bodySource = encryptedBodyPath(id);
    }
    if (QFileInfo::exists(bodySource) &&
        !QFile::copy(bodySource, directory + QLatin1Char('/') + QFileInfo(bodySource).fileName())) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentSessionStore", "恢复快照复制正文失败。");
    }

    QDir root(recoveryDir(id));
    const QFileInfoList snapshots =
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (int i = 0; i < snapshots.size() - snapshotCount(); ++i) {
        QDir(snapshots.at(i).absoluteFilePath()).removeRecursively();
    }
}

void AgentSessionStore::quarantine(const QString &path, QString *problem) const {
    const QString target = path + QStringLiteral(".corrupt-") + nowStamp();
    if (!QFile::rename(path, target) && problem != nullptr) {
        *problem = QCoreApplication::translate("AgentSessionStore", "损坏文件隔离失败：%1").arg(path);
    }
}
