/**
 * @file AgentRepairTransaction.hpp
 * @brief 修复事务的日志与差异：基线哈希、设置增量、验证记录、状态与恢复。
 *
 * 事务只记录事实，不做文件复制/进程启动。工作区由 `AgentWorkspaceStore` 登记，
 * 复制与提交在 `AgentRepairHost` 实现（controllers 层）；这一层负责：
 * - 基线（源实例选定范围内的文件 SHA-256）与当前工作区的差异；
 * - 事务 JSON（`%LOCALAPPDATA%/WTFML/agent/transactions/<id>.json`）；
 * - 事务内 `settings.set` 的增量（提交时才落盘）；
 * - 验证结果和状态机。
 */

#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct AgentRepairFileChange {
    enum class Kind { Added, Modified, Deleted };
    Kind kind = Kind::Modified;
    QString relativePath;
    QString beforeSha256;
    QString afterSha256;
    qint64 beforeBytes = 0;
    qint64 afterBytes = 0;

    QJsonObject toJson() const;
};

struct AgentRepairTransactionRecord {
    QString id;
    QString instanceId;
    QString workspaceId;
    QString state;        ///< preparing / ready / verifying / ready_to_commit / committing / committed / rolling_back / rolled_back / failed
    QStringList scopes;
    QJsonObject baseline; ///< 相对路径 -> SHA-256
    QJsonObject settings; ///< key -> {old, new}
    QJsonObject verification;
    /// 本地修复病例库用的症状键与动作标签（P2）：验证时写入，提交/回滚时只改结果。
    QString symptomKey;
    QStringList actions;
    QString problem;
    QDateTime createdAt;
    QDateTime updatedAt;

    QJsonObject toJson() const;
    static AgentRepairTransactionRecord fromJson(const QJsonObject &object);
    bool active() const;
};

class AgentRepairTransactionStore {
public:
    explicit AgentRepairTransactionStore(const QString &root = QString());

    static QString newId();
    static bool validId(const QString &id);
    static QStringList validScopes();
    static bool validScope(const QString &scope);

    QString root() const { return m_root; }
    QString recordPath(const QString &id) const;

    bool create(const AgentRepairTransactionRecord &record, QString *problem = nullptr);
    bool load(const QString &id, AgentRepairTransactionRecord *record,
              QString *problem = nullptr) const;
    bool list(QList<AgentRepairTransactionRecord> *records, QString *problem = nullptr) const;
    bool save(const AgentRepairTransactionRecord &record, QString *problem = nullptr);
    bool updateState(const QString &id, const QString &state, const QString &problem,
                     QString *outProblem = nullptr);
    bool recordSetting(const QString &id, const QString &key, const QJsonValue &oldValue,
                       const QJsonValue &newValue, QString *problem = nullptr);
    bool setVerification(const QString &id, const QJsonObject &verification,
                         QString *problem = nullptr);
    bool loadByWorkspace(const QString &workspaceId, AgentRepairTransactionRecord *record,
                         QString *problem = nullptr) const;
    bool activeForInstance(const QString &instanceId, AgentRepairTransactionRecord *record = nullptr,
                           QString *problem = nullptr) const;

    /// 源实例选定范围的基线；`instanceRoot` 是源 `versions/<id>`。
    static QJsonObject captureBaseline(const QString &instanceRoot, const QStringList &scopes,
                                       QString *problem = nullptr);
    /// 工作区当前文件与基线比较。
    static QList<AgentRepairFileChange> diff(const QString &workspaceRoot,
                                             const QJsonObject &baseline,
                                             const QStringList &scopes,
                                             QString *problem = nullptr);

private:
    bool writeRecord(const AgentRepairTransactionRecord &record, QString *problem) const;

    QString m_root;
};
