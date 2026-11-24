/**
 * @file AgentLauncherHostImpl.hpp
 * @brief `AgentLauncherHost` 的产品实现：把智能体工具接到现有的 VersionManager /
 * GlobalJavaSettings / BackupFiles / VersionIntegrity 上。
 *
 * 依赖规则：这一层在 `src/controllers/`，可以 include `MainWindow` 与业务层；`src/agent/`
 * 只认 `AgentLauncherHost` 那张纯虚表。
 *
 * 约束：所有方法同步执行、不弹嵌套事件循环。完整性体检按时间预算跑，超时回部分结果。
 */

#pragma once

#include "agent/AgentBackupIndex.hpp"
#include "agent/AgentLaunchHost.hpp"
#include "agent/AgentRepairHost.hpp"
#include "agent/AgentLauncherHost.hpp"

#include <memory>

class MainWindow;
class AgentTestLaunchRunner;
class AgentRepairRunner;

class AgentLauncherHostImpl final : public AgentLauncherHost, public AgentLaunchHost, public AgentRepairHost {
public:
    explicit AgentLauncherHostImpl(MainWindow *owner);
    ~AgentLauncherHostImpl() override;

    bool instanceList(QList<AgentDiagnosticProjection::InstanceFacts> *items,
                      QString *problem) override;
    bool instanceFacts(const QString &instanceId,
                       AgentDiagnosticProjection::InstanceFacts *facts,
                       QString *problem) override;
    bool startupDiagnosis(const QString &instanceId, StartupDiagnosis::Report *report,
                          QString *problem) override;
    bool crashFacts(const QString &instanceId, GameExitDiagnosis::Facts *facts,
                    QStringList *hints, QString *problem) override;
    bool logTail(const QString &instanceId, int lines, QString *text, QString *relativeSource,
                 QString *problem) override;
    bool logSources(const QString &instanceId, QList<AgentHostLogSource> *items,
                    QString *problem) override;
    bool crashReports(const QString &instanceId, QList<AgentHostCrashReport> *items,
                      QString *problem) override;
    bool modsDirectory(const QString &instanceId, QString *path, QString *problem) override;
    bool integrity(const QString &instanceId, VersionIntegrity::Report *report,
                   QString *problem) override;
    bool mods(const QString &instanceId, QList<AgentHostModEntry> *items, QString *problem) override;
    bool setModEnabled(const QString &instanceId, const QString &fileName, bool enabled,
                       QString *previous, QString *problem) override;
    bool settings(AgentHostSettings *settings, QString *problem) override;
    bool applySetting(const QString &key, const QJsonValue &value, QJsonObject *applied,
                      QString *problem) override;
    bool javaVersions(QList<int> *majors, QString *problem) override;
    bool backups(QList<AgentHostBackupRef> *items, QString *problem) override;
    bool planBackup(const QString &instanceId, const QStringList &scopes,
                    AgentHostBackupPlan *plan, QString *problem) override;
    bool createBackup(const QString &sessionId, const QString &instanceId,
                      const AgentHostBackupPlan &plan, QString *backupId, QString *problem) override;
    bool restoreBackup(const QString &sessionId, const QString &backupId,
                       QString *preRestoreBackupId, QString *problem) override;
    bool upgradeInspect(const QString &instanceId, QJsonObject *data, QString *problem) override;
    bool openPage(const QString &page, const QString &instanceId, QString *problem) override;

    // ---------- 测试启动（异步） ----------
    bool startTestLaunch(const QString &instanceId, const QString &workspaceId,
                         const AgentTestLaunchOptions &options, const Completion &done,
                         QString *problem) override;
    void cancelTestLaunch() override;

    // ---------- 修复事务（异步） ----------
    bool beginTransaction(const QString &instanceId, const QStringList &scopes,
                          const BeginCompletion &done, QString *problem) override;
    bool finishTransaction(const QString &transactionId, bool commit,
                           const FinishCompletion &done, QString *problem) override;

private:
    AgentDiagnosticProjection::InstanceFacts factsFor(const QString &instanceId) const;
    QString modsDir(const QString &instanceId) const;
    QString instanceDir(const QString &instanceId) const;

    MainWindow *m_owner = nullptr;
    AgentBackupIndex m_maintenance;
    std::unique_ptr<AgentTestLaunchRunner> m_testLaunchRunner;
    std::unique_ptr<AgentRepairRunner> m_repairRunner;
};
