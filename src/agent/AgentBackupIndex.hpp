/**
 * @file AgentBackupIndex.hpp
 * @brief 文件维护备份的元数据/内容：独立于会话生命周期，保留 owner/source/sourceSessionId。
 *
 * Stage 1B 只做 `fs.write` 的写前备份：内容 DPAPI 加密，元数据明文（不含内容/密钥）。
 * 删除会话**不删除**这些备份；创建备份属于写操作，必须随写审批一起确认。
 *
 * #129 起新备份写进启动器备份目录（`BackupFiles::Kind::Files`，见下方 `AgentBackups`），
 * 这一库**冻结**：只读（列出、读回），不再写入，也不删除。
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include "manage/BackupFiles.hpp"

struct AgentBackupEntry {
    QString backupId;
    QString owner = QStringLiteral("agent");
    QString source = QStringLiteral("agent-session");
    QString storage = QStringLiteral("agent-maintenance");
    QString sourceSessionId;
    QString instanceId;
    QString reason;
    QStringList originalPaths;
    QString sha256;
    QString restoreHint;
    qint64 size = 0;
    QDateTime createdAt;

    QJsonObject toJson() const;
    static AgentBackupEntry fromJson(const QJsonObject &object);
};

class AgentBackupIndex {
public:
    explicit AgentBackupIndex(const QString &root = QString());

    void setRoot(const QString &root);
    QString root() const { return m_root; }
    QString dirFor(const QString &backupId) const;

    bool createFromBytes(const QString &sourceSessionId, const QString &instanceId,
                         const QString &relativePath, const QByteArray &original,
                         const QString &reason, QString *backupId, QString *problem = nullptr);
    /// 一份备份收多个文件（Stage 2 的 `backup.create` 一次备 config/mods/options）。
    /// 内容仍是**一个** DPAPI 密文：容器是一段 JSON（路径 + base64），因此单文件备份
    /// 与多文件备份在盘上是同一种形状，`readFiles()` 两种都读得回来。
    bool createFromFiles(const QString &sourceSessionId, const QString &instanceId,
                         const QList<QPair<QString, QByteArray>> &files, const QString &reason,
                         QString *backupId, QString *problem = nullptr);
    /// 一条桶里的原始文件被改回旧版本前，先把它当前内容备份一份（恢复前的快照）。
    bool list(QList<AgentBackupEntry> *entries, QString *problem = nullptr);
    bool readOriginal(const QString &backupId, QByteArray *bytes, QString *problem = nullptr);
    /// 读回一份备份里的全部文件（单文件备份也走这里）。
    bool readFiles(const QString &backupId, QList<QPair<QString, QByteArray>> *files,
                   QString *problem = nullptr);

    /// 只清 Agent 自有备份；launcher/user/shared 不应出现在这里（这里也不会误删它们）。
    bool removeOwnedBackup(const QString &backupId, QString *problem = nullptr);

private:
    bool writeMeta(const AgentBackupEntry &entry, QString *problem);

    QString m_root;
};

/// 智能体备份的统一视图：新格式是当前游戏文件夹 `cache/backups/files` 下的 Files 备份，
/// 旧格式是上面冻结的维护备份库（只认 owner=agent 的条目）。
namespace AgentBackups {

inline constexpr QLatin1StringView kTriggerCreate{"backup.create"};
inline constexpr QLatin1StringView kTriggerWrite{"fs.write"};
inline constexpr QLatin1StringView kTriggerBeforeRestore{"before-restore"};

struct Item {
    QString backupId;
    QString instanceId;
    QString sessionId;
    QString sessionTitle;
    QString trigger;      ///< kTriggerCreate / kTriggerWrite / kTriggerBeforeRestore
    QString scope;        ///< backup.create 的范围（config+mods 这种），其余为空
    QStringList paths;    ///< 相对实例目录
    qint64 bytes = 0;
    QDateTime createdAt;
    QString path;         ///< 新格式 = 备份目录；旧格式 = 空

    bool legacy() const { return path.isEmpty(); }
};

/// 新旧两库里 @p instanceId 的备份（为空 = 全部），从新到旧。@p legacy 为空则不读旧库。
QList<Item> list(const QString &instanceId, AgentBackupIndex *legacy);
/// 读回一份备份的全部文件（新格式逐个核对 sha1，旧格式解密）。
bool readFiles(const Item &item, AgentBackupIndex *legacy,
               QList<BackupFiles::FilePayload> *files, QString *problem = nullptr);
/// 写一份新格式备份，成功后按实例执行保留策略（这一会话的备份不清）。
bool write(const QString &trigger, const QString &instanceId, const QString &sessionId,
           const QString &sessionTitle, const QString &scope,
           const QList<BackupFiles::FilePayload> &files, QString *backupId,
           QString *backupDir = nullptr, QString *problem = nullptr);
/// 智能体工具结果里的标签（「写入文件前的备份」这类）。
QString label(const Item &item);

}  // namespace AgentBackups
