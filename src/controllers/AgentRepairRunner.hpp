/**
 * @file AgentRepairRunner.hpp
 * @brief 修复事务的产品实现：复制工作区、计算基线、提交差异或回滚。
 *
 * 只认识 MainWindow / VersionManager 与本地文件；事务日志在 `AgentRepairTransactionStore`，
 * 工作区登记在 `AgentWorkspaceStore`。`repair.begin/finish` 是异步的：复制、提交和回滚
 * 都在后台线程，结果回主线程。设置项通过注入的回调读写，避免在这里复制一份白名单。
 */

#pragma once

#include "agent/AgentRepairHost.hpp"
#include "agent/AgentRepairTransaction.hpp"
#include "agent/AgentWorkspaceStore.hpp"

#include <QFutureWatcher>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class MainWindow;

class AgentRepairRunner : public QObject {
public:
    using ApplySetting = std::function<bool(const QString &key, const QJsonValue &value,
                                            QString *problem)>;
    using ReadSetting = std::function<QJsonValue(const QString &key)>;

    AgentRepairRunner(MainWindow *owner, ApplySetting applySetting, ReadSetting readSetting,
                      QObject *parent = nullptr);
    ~AgentRepairRunner() override;

    bool begin(const QString &instanceId, const QStringList &scopes,
               const AgentRepairHost::BeginCompletion &done, QString *problem);
    bool finish(const QString &transactionId, bool commit,
                const AgentRepairHost::FinishCompletion &done, QString *problem);

private:
    struct BeginPrepared {
        bool ok = false;
        QString problem;
        QJsonObject baseline;
        qint64 bytes = 0;
    };
    struct FinishPrepared {
        bool ok = false;
        bool committed = false;
        QString problem;
        QStringList changed;
        QStringList backups;
    };

    MainWindow *m_owner = nullptr;
    ApplySetting m_applySetting;
    ReadSetting m_readSetting;
    AgentWorkspaceStore m_workspaces;
    AgentRepairTransactionStore m_transactions;
    QString m_beginTransactionId;
    QString m_beginWorkspaceId;
    QString m_beginSourceRoot;
    QString m_beginWorkspaceRoot;
    QString m_beginVersionId;
    QStringList m_beginScopes;
    AgentRepairHost::BeginCompletion m_beginDone;
    AgentRepairHost::FinishCompletion m_finishDone;
    QString m_finishTransactionId;
    bool m_finishCommit = false;
    bool m_active = false;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<BeginPrepared> *m_beginWatcher = nullptr;
    QFutureWatcher<FinishPrepared> *m_finishWatcher = nullptr;
};
