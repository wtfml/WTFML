/**
 * @file AgentTestLaunchRunner.hpp
 * @brief 智能体测试启动的产品实现：建工作区、挑 Java、起 QProcess、等里程碑、收进程树。
 *
 * 只在 `src/controllers/` 用，认识 MainWindow / VersionManager / GlobalJavaSettings；
 * `src/agent/` 只认 `AgentLaunchHost` 接口。复制与 Java 探测在后台线程，进程和日志轮询
 * 在主线程；不登记用户会话、不弹日志窗、不写最近启动。
 */

#pragma once

#include "agent/AgentLaunchHost.hpp"
#include "agent/AgentTestLaunch.hpp"
#include "agent/AgentWorkspaceStore.hpp"
#include "settings/EffectiveJavaSettings.hpp"

#include <QJsonObject>
#include <QList>
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

class MainWindow;
class QProcess;
class QTimer;

class AgentTestLaunchRunner : public QObject {
public:
    explicit AgentTestLaunchRunner(MainWindow *owner, QObject *parent = nullptr);
    ~AgentTestLaunchRunner() override;

    bool start(const QString &instanceId, const QString &workspaceId,
               const AgentTestLaunchOptions &options, const AgentLaunchHost::Completion &done,
               QString *problem);
    void cancel();

private:
    struct Prepared {
        bool ok = false;
        bool cancelled = false;
        QString problem;
        EffectiveJavaSettings java;
    };

    void onPrepared(const Prepared &prepared);
    void beginRun();
    void onProcessStarted();
    void onProcessOutput(bool error);
    void poll();
    void consumeLatestLog();
    void checkCrashReports();
    void evaluate();
    void finishRun(bool ok, const QString &phase, const QString &detail);
    void finishAll(const QString &problem);
    void killProcessTree();
    void closeJob();
    void appendLog(const QString &text);
    void feedLine(const QString &line);
    QString workspaceVersionDir() const;
    bool targetReached() const;
    bool unsafeJvmArgs() const;
    QStringList crashReportNames() const;

    MainWindow *m_owner = nullptr;
    AgentWorkspaceStore m_store;
    bool m_active = false;
    bool m_starting = false;
    bool m_ephemeral = false;
    bool m_cancelled = false;
    QString m_instanceId;
    QString m_workspaceId;
    QString m_versionId;
    QString m_sourceDir;
    QString m_sourceJar;
    QString m_gameRoot;
    QString m_librariesDir;
    QString m_assetsDir;
    QString m_workspaceRoot;
    QString m_launchId;
    AgentTestLaunchOptions m_options;
    AgentLaunchHost::Completion m_done;
    QList<AgentTestLaunchRunResult> m_runs;
    QJsonObject m_versionJson;
    EffectiveJavaSettings m_javaSettings;
    QStringList m_versionJvmArgs;
    QStringList m_args;
    QString m_javaPath;
    QString m_logPath;
    qint64 m_logOffset = 0;
    bool m_processStarted = false;
    bool m_crashed = false;
    bool m_newCrashReport = false;
    bool m_mainMenu = false;
    bool m_worldLoaded = false;
    bool m_settled = false;
    QString m_phase = QStringLiteral("not_started");
    QString m_detail;
    QStringList m_logRefs;
    qint64 m_runStartMs = 0;
    qint64 m_reachedAtMs = 0;
    QProcess *m_process = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QTimer *m_startTimer = nullptr;
    void *m_jobHandle = nullptr;
    QSet<QString> m_seenCrashReports;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    bool m_runFinished = false;
    QString m_logPartial;
    QFutureWatcher<Prepared> *m_prepareWatcher = nullptr;
};
