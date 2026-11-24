/**
 * @file AgentRepairHost.hpp
 * @brief 修复事务的异步宿主：建工作区/基线、提交差异或回滚。
 *
 * `repair.begin` 与 `repair.finish` 可能复制/写大量文件，走这条异步接口；`repair.diff`
 * 与 `repair.status` 只读事务日志，不需要宿主。实现放在 controllers 层。
 */

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

struct AgentRepairBeginResult {
    bool ok = false;
    QString transactionId;
    QString workspaceId;
    qint64 baselineBytes = 0;
    QString problem;
};

struct AgentRepairFinishResult {
    bool ok = false;
    bool committed = false;
    QString transactionId;
    QStringList changedFiles;
    QStringList backupIds;
    QString problem;
};

class AgentRepairHost {
public:
    using BeginCompletion = std::function<void(const AgentRepairBeginResult &)>;
    using FinishCompletion = std::function<void(const AgentRepairFinishResult &)>;

    virtual ~AgentRepairHost() = default;

    virtual bool beginTransaction(const QString &instanceId, const QStringList &scopes,
                                  const BeginCompletion &done, QString *problem) = 0;
    /// @param commit true = 提交到原实例；false = 回滚/丢弃工作区。
    virtual bool finishTransaction(const QString &transactionId, bool commit,
                                   const FinishCompletion &done, QString *problem) = 0;
};
