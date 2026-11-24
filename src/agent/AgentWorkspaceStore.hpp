/**
 * @file AgentWorkspaceStore.hpp
 * @brief 智能体测试启动的隔离工作区登记：id、物理根、日志目录、状态与清理。
 *
 * 物理副本在 `<游戏根>/cache/agent-workspaces/<id>`，登记和日志在
 * `%LOCALAPPDATA%/WTFML/agent/workspaces/<id>.json` 与 `<id>/logs`。这一层只负责
 * 目录/记录，不复制实例、不启动进程；复制由 `AgentTestLaunchRunner` 经
 * `VersionManager::copyInstanceFrom()` 做。
 *
 * 安全约束：id 只认 `ws-` 开头的安全字符；清理时只删登记过的、路径正好等于
 * `workspacesRoot()/<id>` 的目录，不跟随链接，不删未知目录。
 */

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

struct AgentWorkspaceRecord {
    QString id;
    QString sourceInstanceId;
    QString versionId;      ///< 工作区里那份实例目录名（ASCII，`agent-test-…`）
    QString rootPath;       ///< 物理工作区根（绝对路径，只在本机用）
    QString status;         ///< preparing / copying / ready / failed / cleaned
    QDateTime createdAt;
    QDateTime updatedAt;

    QJsonObject toJson() const;
    static AgentWorkspaceRecord fromJson(const QJsonObject &object);
};

class AgentWorkspaceStore {
public:
    /// 空参数 = 用 AgentPaths / AppPaths 的产品目录；测试传临时目录隔离。
    explicit AgentWorkspaceStore(const QString &recordsRoot = QString(),
                                 const QString &workspacesRoot = QString());

    static QString newId();
    static QString newVersionId(const QString &workspaceId);
    static bool validId(const QString &id);

    QString recordsRoot() const { return m_recordsRoot; }
    QString workspacesRoot() const { return m_workspacesRoot; }
    QString recordPath(const QString &id) const;
    QString recordDir(const QString &id) const;
    QString logsDir(const QString &id) const;
    QString workspaceRoot(const QString &id) const;

    bool create(const AgentWorkspaceRecord &record, QString *problem = nullptr);
    bool load(const QString &id, AgentWorkspaceRecord *record,
              QString *problem = nullptr) const;
    bool list(QList<AgentWorkspaceRecord> *records, QString *problem = nullptr) const;
    bool updateStatus(const QString &id, const QString &status, QString *problem = nullptr);

    /// 删登记；@p removeWorkspace 为真时同时删物理工作区（只删登记过、路径匹配的那一份）。
    bool remove(const QString &id, bool removeWorkspace, QString *problem = nullptr);
    bool cleanup(const QString &id, QString *problem = nullptr) { return remove(id, true, problem); }

private:
    bool writeRecord(const AgentWorkspaceRecord &record, QString *problem) const;

    QString m_recordsRoot;
    QString m_workspacesRoot;
};
