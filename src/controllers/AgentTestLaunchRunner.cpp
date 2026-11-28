/**
 * @file AgentTestLaunchRunner.cpp
 * @brief 智能体测试启动的实现（见 .hpp）。
 */

#include "AgentTestLaunchRunner.hpp"

#include "MainWindow.hpp"

#include "agent/AgentPaths.hpp"
#include "launcher/LauncherManager.hpp"
#include "launcher/QuickPlay.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GameWindowSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>
#include <QScopeGuard>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

constexpr int kPollMs = 500;
constexpr int kStartTimeoutMs = 5000;
constexpr qint64 kWorkspaceLimitBytes = 16LL * 1024 * 1024 * 1024;
constexpr qint64 kWorkspaceHeadroomBytes = 1024LL * 1024 * 1024;

QString newLaunchId() {
    return QStringLiteral("launch-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString assetIndexFor(const QJsonObject &json) {
    QString index = json.value(QStringLiteral("assetIndex")).toObject()
                        .value(QStringLiteral("id")).toString();
    if (index.isEmpty()) index = QStringLiteral("29");
    return index;
}

int javaMajorFor(const QJsonObject &json) {
    int major = json.value(QStringLiteral("javaVersion")).toObject()
                    .value(QStringLiteral("majorVersion")).toInt();
    if (major <= 0) major = 21;
    return major;
}

QStringList unsafeJvmOptions(const QString &text) {
    QStringList found;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString lower = part.toLower();
        if (lower.startsWith(QStringLiteral("-javaagent")) ||
            lower.startsWith(QStringLiteral("-agentlib")) ||
            lower.startsWith(QStringLiteral("-agentpath")) ||
            lower.startsWith(QStringLiteral("-xbootclasspath"))) {
            found.append(part);
        }
    }
    return found;
}

QStringList linesOf(const QString &text) {
    return text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

}  // namespace

AgentTestLaunchRunner::AgentTestLaunchRunner(MainWindow *owner, QObject *parent)
    : QObject(parent), m_owner(owner), m_store() {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &AgentTestLaunchRunner::poll);

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this] {
        if (!m_active || m_runFinished) return;
        evaluate();
        if (m_active && !m_runFinished && m_phase != QStringLiteral("crashed")) {
            m_detail = QStringLiteral("测试启动超时。");  // i18n-ignore
            finishRun(false, QStringLiteral("timed_out"), m_detail);
        }
    });

    m_startTimer = new QTimer(this);
    m_startTimer->setSingleShot(true);
    m_startTimer->setInterval(kStartTimeoutMs);
    connect(m_startTimer, &QTimer::timeout, this, [this] {
        if (!m_active || m_runFinished || m_processStarted) return;
        m_detail = QStringLiteral("java 进程没有在 %1 秒内启动。").arg(kStartTimeoutMs / 1000);  // i18n-ignore
        finishRun(false, QStringLiteral("not_started"), m_detail);
    });
}

AgentTestLaunchRunner::~AgentTestLaunchRunner() {
    cancel();
    for (QFutureWatcher<Prepared> *watcher : m_prepareWatchers) {
        watcher->waitForFinished();
    }
}

QString AgentTestLaunchRunner::workspaceVersionDir() const {
    return m_workspaceRoot + QStringLiteral("/versions/") + m_versionId;
}

QStringList AgentTestLaunchRunner::crashReportNames() const {
    QStringList names;
    const QStringList dirs{workspaceVersionDir() + QStringLiteral("/crash-reports"),
                           m_workspaceRoot + QStringLiteral("/crash-reports")};
    for (const QString &dir : dirs) {
        const QStringList files =
            QDir(dir).entryList(QStringList{QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
        for (const QString &file : files) names.append(dir + QLatin1Char('/') + file);
    }
    return names;
}

bool AgentTestLaunchRunner::unsafeJvmArgs() const {
    return !unsafeJvmOptions(m_javaSettings.extraJvmArgs).isEmpty() ||
           !unsafeJvmOptions(m_versionJvmArgs.join(QLatin1Char(' '))).isEmpty();
}

void AgentTestLaunchRunner::appendLog(const QString &text) {
    if (text.isEmpty()) return;
    const QString dir = m_store.logsDir(m_workspaceId);
    QDir().mkpath(dir);
    QFile file(dir + QLatin1Char('/') + m_launchId + QStringLiteral(".log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(text.toUtf8());
    }
}

void AgentTestLaunchRunner::feedLine(const QString &raw) {
    const QString line = raw.trimmed();
    if (line.isEmpty()) return;
    if (AgentTestLaunchMilestones::isCrashLine(line)) {
        m_crashed = true;
        if (m_detail.isEmpty()) m_detail = line.left(400);
    }
    if (AgentTestLaunchMilestones::isMainMenuLine(line)) m_mainMenu = true;
    if (AgentTestLaunchMilestones::isWorldLoadedLine(line)) m_worldLoaded = true;
}

void AgentTestLaunchRunner::consumeLatestLog() {
    QFile file(m_logPath);
    if (!file.open(QIODevice::ReadOnly)) return;
    const qint64 size = file.size();
    if (size < m_logOffset) {
        m_logOffset = 0;
        m_logPartial.clear();
    }
    if (size == m_logOffset) return;
    file.seek(m_logOffset);
    const QByteArray bytes = file.read(size - m_logOffset);
    m_logOffset = size;
    QString text = QString::fromUtf8(bytes);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_logPartial += text;
    const int last = m_logPartial.lastIndexOf(QLatin1Char('\n'));
    if (last < 0) return;
    const QString complete = m_logPartial.left(last);
    m_logPartial = m_logPartial.mid(last + 1);
    for (const QString &line : linesOf(complete)) feedLine(line);
}

void AgentTestLaunchRunner::checkCrashReports() {
    const QStringList names = crashReportNames();
    for (const QString &name : names) {
        if (m_seenCrashReports.contains(name)) continue;
        m_seenCrashReports.insert(name);
        m_newCrashReport = true;
        m_crashed = true;
        if (m_detail.isEmpty()) m_detail = QStringLiteral("出现新的崩溃报告。");  // i18n-ignore
    }
    const QStringList hsErr = QDir(workspaceVersionDir())
                                  .entryList(QStringList{QStringLiteral("hs_err_pid*.log")},
                                             QDir::Files);
    if (!hsErr.isEmpty()) {
        m_crashed = true;
        if (m_detail.isEmpty()) m_detail = QStringLiteral("出现 hs_err_pid 日志。");  // i18n-ignore
    }
}

bool AgentTestLaunchRunner::targetReached() const {
    if (m_options.milestone == QStringLiteral("process_started")) return m_processStarted;
    if (m_options.milestone == QStringLiteral("world_loaded")) return m_worldLoaded;
    return m_mainMenu;
}

void AgentTestLaunchRunner::onProcessOutput(bool error) {
    if (m_process == nullptr) return;
    const QByteArray bytes = error ? m_process->readAllStandardError()
                                   : m_process->readAllStandardOutput();
    if (bytes.isEmpty()) return;
    const QString text = QString::fromUtf8(bytes);
    appendLog((error ? QStringLiteral("[stderr] ") : QStringLiteral("[stdout] ")) + text);
    for (const QString &line : linesOf(text)) feedLine(line);
}

void AgentTestLaunchRunner::onProcessStarted() {
    if (!m_active || m_runFinished) return;
    m_startTimer->stop();
    m_processStarted = true;
    m_phase = QStringLiteral("starting");
    appendLog(QStringLiteral("java 进程已启动（PID %1）。\n").arg(m_process->processId()));  // i18n-ignore
#ifdef Q_OS_WIN
    if (m_jobHandle != nullptr && m_process != nullptr) {
        AssignProcessToJobObject(HANDLE(m_jobHandle), HANDLE(m_process->processId()));
    }
#endif
}

void AgentTestLaunchRunner::beginRun() {
    if (!m_active) return;
    m_runFinished = false;
    m_processStarted = false;
    m_crashed = false;
    m_newCrashReport = false;
    m_mainMenu = false;
    m_worldLoaded = false;
    m_settled = false;
    m_reachedAtMs = 0;
    m_phase = QStringLiteral("starting");
    m_detail.clear();
    m_logPartial.clear();
    m_seenCrashReports.clear();
    m_logRefs.clear();
    m_runStartMs = QDateTime::currentMSecsSinceEpoch();

    m_javaPath = m_javaSettings.javaPath;
    if (m_javaPath.isEmpty()) {
        m_javaPath = QStandardPaths::findExecutable(QStringLiteral("java"));
    }
    if (m_javaPath.isEmpty()) {
        finishRun(false, QStringLiteral("not_started"), QStringLiteral("找不到 Java 可执行文件。"));  // i18n-ignore
        return;
    }

    m_logPath = workspaceVersionDir() + QStringLiteral("/logs/latest.log");
    m_logOffset = QFileInfo(m_logPath).size();
    const QStringList before = crashReportNames();
    for (const QString &name : before) m_seenCrashReports.insert(name);

#ifdef Q_OS_WIN
    m_jobHandle = CreateJobObjectW(nullptr, nullptr);
    if (m_jobHandle != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(HANDLE(m_jobHandle), JobObjectExtendedLimitInformation, &info,
                                sizeof(info));
    }
#endif

    m_process = new QProcess(this);
    m_process->setProgram(m_javaPath);
    m_process->setArguments(m_args);
    m_process->setWorkingDirectory(workspaceVersionDir());
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::started, this, &AgentTestLaunchRunner::onProcessStarted);
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            [this] { onProcessOutput(false); });
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this] { onProcessOutput(true); });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (m_runFinished) return;
                const bool abnormal = status == QProcess::CrashExit || exitCode != 0;
                if (abnormal) {
                    m_crashed = true;
                    if (m_detail.isEmpty()) {
                        m_detail = QStringLiteral("java 进程异常退出（退出码 %1）。").arg(exitCode);  // i18n-ignore
                    }
                }
                m_processStarted = true;
                consumeLatestLog();
                checkCrashReports();
                if (m_options.expectCrash && m_crashed) {
                    finishRun(true, QStringLiteral("crashed"), m_detail);
                } else {
                    const bool reached = targetReached();
                    const bool settled = reached && m_reachedAtMs > 0 &&
                                         QDateTime::currentMSecsSinceEpoch() - m_reachedAtMs >=
                                             qint64(m_options.settleSeconds) * 1000;
                    finishRun(settled && !m_crashed, settled ? QStringLiteral("stable")
                                                             : QStringLiteral("exited"),
                              settled ? QString() : QStringLiteral("进程在达到观察窗口前退出。"));  // i18n-ignore
                }
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_runFinished) return;
        m_detail = QStringLiteral("启动游戏进程失败：%1").arg(m_process->errorString());  // i18n-ignore
        finishRun(false, QStringLiteral("not_started"), m_detail);
    });

    appendLog(QStringLiteral("启动测试进程：%1\n参数：%2\n").arg(m_javaPath,  // i18n-ignore
                                                              m_args.join(QLatin1Char(' '))));
    m_startTimer->start();
    m_pollTimer->start();
    m_timeoutTimer->start(m_options.timeoutSeconds * 1000);
    m_process->start();
}

void AgentTestLaunchRunner::poll() {
    if (!m_active || m_runFinished) return;
    consumeLatestLog();
    checkCrashReports();
    evaluate();
}

void AgentTestLaunchRunner::evaluate() {
    if (!m_active || m_runFinished) return;

    if (m_options.expectCrash && m_crashed) {
        finishRun(true, QStringLiteral("crashed"), m_detail);
        return;
    }

    if (targetReached()) {
        if (m_reachedAtMs == 0) {
            m_reachedAtMs = QDateTime::currentMSecsSinceEpoch();
            m_phase = m_options.milestone;
        }
        const qint64 observed = QDateTime::currentMSecsSinceEpoch() - m_reachedAtMs;
        if (!m_options.expectCrash && observed >= qint64(m_options.settleSeconds) * 1000) {
            m_settled = true;
            finishRun(true, m_options.milestone, QString());
            return;
        }
    }

    if (m_runStartMs > 0 &&
        QDateTime::currentMSecsSinceEpoch() - m_runStartMs >=
            qint64(m_options.timeoutSeconds) * 1000) {
        m_detail = QStringLiteral("测试启动超时。");  // i18n-ignore
        finishRun(false, QStringLiteral("timed_out"), m_detail);
    }
}

void AgentTestLaunchRunner::stopProcess(const std::function<void()> &done) {
    QProcess *process = m_process;
    m_process = nullptr;
    if (process != nullptr) process->disconnect(this);
#ifdef Q_OS_WIN
    if (m_jobHandle != nullptr) {
        TerminateJobObject(HANDLE(m_jobHandle), 1);
        CloseHandle(HANDLE(m_jobHandle));
        m_jobHandle = nullptr;
    }
#endif
    if (process == nullptr || process->state() == QProcess::NotRunning) {
        if (process != nullptr) process->deleteLater();
        if (done) done();
        return;
    }
    // 等系统报告退出再销毁，避免 QProcess 析构在界面线程同步等进程。
    connect(process, &QProcess::stateChanged, this, [process, done](QProcess::ProcessState state) {
        if (state != QProcess::NotRunning) return;
        process->deleteLater();
        if (done) done();
    });
    connect(process, &QProcess::started, process, &QProcess::kill);
    process->kill();
}

void AgentTestLaunchRunner::finishRun(bool ok, const QString &phase, const QString &detail) {
    Q_UNUSED(ok);
    if (!m_active || m_runFinished) return;
    m_runFinished = true;
    m_pollTimer->stop();
    m_timeoutTimer->stop();
    m_startTimer->stop();
    if (m_process != nullptr) {
        onProcessOutput(false);
        onProcessOutput(true);
    }
    consumeLatestLog();
    checkCrashReports();

    AgentTestLaunchRunResult run;
    run.processStarted = m_processStarted;
    run.exitCode = m_process != nullptr ? m_process->exitCode() : -1;
    run.crashed = m_crashed;
    run.timedOut = phase == QStringLiteral("timed_out");
    run.milestoneReached = targetReached();
    run.newCrashReport = m_newCrashReport;
    run.stoppedAfterSettle = m_settled;
    run.startupPhase = phase;
    run.durationMs = m_runStartMs > 0
                         ? QDateTime::currentMSecsSinceEpoch() - m_runStartMs
                         : 0;
    run.detail = detail.isEmpty() ? m_detail : detail;
    run.logRefs = {m_launchId + QStringLiteral("/latest.log"),
                   m_launchId + QStringLiteral(".log")};
    m_runs.append(run);

    stopProcess([this, cancelFlag = m_cancelFlag] {
        if (cancelFlag->load()) return;
        if (m_runs.size() >= m_options.repeat) {
            finishAll(QString());
            return;
        }
        QTimer::singleShot(250, this, [this, cancelFlag] {
            if (cancelFlag->load()) return;
            m_launchId = newLaunchId();
            beginRun();
        });
    });
}

void AgentTestLaunchRunner::finishAll(const QString &problem) {
    if (!m_active) return;
    if (m_starting) {
        QTimer::singleShot(0, this, [this, problem, cancelFlag = m_cancelFlag] {
            if (!cancelFlag->load()) finishAll(problem);
        });
        return;
    }
    AgentTestLaunchSummary summary;
    summary.launchId = m_launchId;
    summary.workspaceId = m_workspaceId;
    summary.workspaceKept = true;
    summary.runs = m_runs;
    summary.problem = problem;
    summary.ok = problem.isEmpty() && !m_runs.isEmpty();
    for (const AgentTestLaunchRunResult &run : m_runs) {
        const bool runOk = m_options.expectCrash
                               ? (run.crashed && !run.timedOut)
                               : (run.milestoneReached && !run.crashed && !run.timedOut &&
                                  run.stoppedAfterSettle);
        if (!runOk) summary.ok = false;
    }
    m_store.updateStatus(m_workspaceId, summary.ok ? QStringLiteral("done")
                                                   : QStringLiteral("failed"));
    m_active = false;
    const AgentLaunchHost::Completion done = m_done;
    m_done = {};
    if (done) done(summary);
}

bool AgentTestLaunchRunner::start(const QString &instanceId, const QString &workspaceId,
                                  const AgentTestLaunchOptions &options,
                                  const AgentLaunchHost::Completion &done, QString *problem) {
    if (m_active) {
        if (problem != nullptr) *problem = QStringLiteral("已有一个测试启动在运行。");  // i18n-ignore
        return false;
    }
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("主窗口未接入。");  // i18n-ignore
        return false;
    }
    if (instanceId.isEmpty() || !QDir(AppPaths::versionDir(instanceId)).exists()) {
        if (problem != nullptr) *problem = QStringLiteral("实例目录不存在。");  // i18n-ignore
        return false;
    }
    if (m_owner->m_launcherManager->isVersionRunning(instanceId)) {
        if (problem != nullptr) *problem = QStringLiteral("这个实例正在运行，不能同时做测试启动。");  // i18n-ignore
        return false;
    }

    m_active = true;
    m_starting = true;
    const auto startingGuard = qScopeGuard([this] { m_starting = false; });
    m_runs.clear();
    m_instanceId = instanceId;
    m_workspaceId.clear();
    m_options = options;
    m_done = done;
    m_launchId = newLaunchId();
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);

    m_versionJson = m_owner->m_versionManager->getVersionJson(instanceId);
    if (m_versionJson.isEmpty()) {
        finishAll(QStringLiteral("读不到实例的 version.json。"));  // i18n-ignore
        return true;
    }
    const JavaSettingsOverride override =
        javaOverrideFromJson(m_owner->m_versionManager->versionMeta(instanceId));
    m_javaSettings = resolveJavaSettings(m_owner->m_javaSettings->effective(), override);
    m_owner->resolveMemoryForLaunch(instanceId, m_javaSettings, override);
    m_versionJvmArgs = m_owner->m_versionManager->versionJvmArgs(instanceId);
    if (unsafeJvmArgs()) {
        finishAll(QStringLiteral("实例的 JVM 参数包含能加载本地代码的选项（-javaagent/-agentlib 等），"  // i18n-ignore
                                 "测试启动拒绝执行。"));  // i18n-ignore
        return true;
    }

    m_ephemeral = workspaceId.isEmpty();
    if (m_ephemeral) {
        m_workspaceId = AgentWorkspaceStore::newId();
        m_versionId = AgentWorkspaceStore::newVersionId(m_workspaceId);
        AgentWorkspaceRecord record;
        record.id = m_workspaceId;
        record.sourceInstanceId = instanceId;
        record.versionId = m_versionId;
        record.rootPath = m_store.workspaceRoot(m_workspaceId);
        record.status = QStringLiteral("copying");
        QString storeProblem;
        if (!m_store.create(record, &storeProblem)) {
            finishAll(storeProblem);
            return true;
        }
        m_workspaceRoot = record.rootPath;
    } else {
        AgentWorkspaceRecord record;
        QString storeProblem;
        if (!m_store.load(workspaceId, &record, &storeProblem) ||
            record.sourceInstanceId != instanceId) {
            finishAll(storeProblem.isEmpty() ? QStringLiteral("工作区与当前实例不匹配。")  // i18n-ignore
                                             : storeProblem);
            return true;
        }
        m_workspaceId = workspaceId;
        m_versionId = record.versionId;
        m_workspaceRoot = record.rootPath;
    }

    m_sourceDir = AppPaths::versionDir(instanceId);
    m_sourceJar = InstanceFiles::jarPath(instanceId);
    m_gameRoot = AppPaths::minecraftDir();
    m_librariesDir = AppPaths::libraryDir();
    m_assetsDir = AppPaths::assetsDir();
    const QJsonObject versionJson = m_versionJson;
    const QString sourceDir = m_sourceDir;
    const QString sourceInstance = instanceId;
    const QString workspaceRoot = m_workspaceRoot;
    const QString versionId = m_versionId;
    const bool copyNeeded = m_ephemeral;
    const int javaMajor = javaMajorFor(versionJson);
    const QString component = versionJson.value(QStringLiteral("javaVersion"))
                                  .toObject()
                                  .value(QStringLiteral("component"))
                                  .toString();
    VersionManager *versions = m_owner->m_versionManager;
    GlobalJavaSettings *javaStore = m_owner->m_javaSettings;
    const EffectiveJavaSettings initialJava = m_javaSettings;
    const std::shared_ptr<std::atomic_bool> cancelFlag = m_cancelFlag;

    auto *watcher = new QFutureWatcher<Prepared>(this);
    m_prepareWatchers.append(watcher);
    connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, watcher, cancelFlag] {
        m_prepareWatchers.removeOne(watcher);
        watcher->deleteLater();
        if (!cancelFlag->load()) onPrepared(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([=] {
        Prepared prepared;
        prepared.java = initialJava;
        if (cancelFlag->load()) return prepared;
        QString prepareProblem;
        if (copyNeeded) {
            qint64 sourceBytes = 0;
            QDirIterator it(sourceDir, QDir::Files | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                sourceBytes += it.fileInfo().size();
                if (sourceBytes > kWorkspaceLimitBytes) break;
            }
            if (sourceBytes > kWorkspaceLimitBytes) {
                prepared.problem = QStringLiteral("实例超过 16 GB 工作区上限，改用只读诊断。");  // i18n-ignore
                return prepared;
            }
            const QStorageInfo storage(workspaceRoot);
            if (storage.isValid() && storage.bytesAvailable() <
                                          sourceBytes + kWorkspaceHeadroomBytes) {
                prepared.problem = QStringLiteral("工作区所在磁盘空间不足。");  // i18n-ignore
                return prepared;
            }
            if (!versions->copyInstanceFrom(sourceDir, sourceInstance, workspaceRoot, versionId,
                                            &prepareProblem)) {
                prepared.problem = prepareProblem;
                return prepared;
            }
        }
        if (!QDir(workspaceRoot + QStringLiteral("/versions/") + versionId).exists()) {
            prepared.problem = QStringLiteral("工作区实例目录没有建出来。");  // i18n-ignore
            return prepared;
        }
        if (cancelFlag->load()) return prepared;
        QString javaProblem;
        if (!javaStore->ensureJavaForVersion(prepared.java, javaMajor, component, true,
                                             &javaProblem, false, false, {}, cancelFlag.get())) {
            prepared.problem = javaProblem.isEmpty() ? QStringLiteral("找不到可用的 Java。")  // i18n-ignore
                                                     : javaProblem;
            return prepared;
        }
        if (cancelFlag->load()) return prepared;
        prepared.ok = true;
        return prepared;
    }));
    return true;
}

void AgentTestLaunchRunner::onPrepared(const Prepared &prepared) {
    if (!m_active) return;
    if (!prepared.ok) {
        m_store.updateStatus(m_workspaceId, QStringLiteral("failed"));
        finishAll(prepared.problem);
        return;
    }
    m_javaSettings = prepared.java;
    m_store.updateStatus(m_workspaceId, QStringLiteral("ready"));

    LaunchRoots roots;
    roots.gameRoot = m_gameRoot;
    roots.versionDir = workspaceVersionDir();
    roots.librariesDir = m_librariesDir;
    roots.assetsDir = m_assetsDir;
    roots.nativesDir = roots.versionDir + QStringLiteral("/natives");
    roots.jarPath = m_sourceJar;
    roots.siblingJarDir = roots.versionDir;

    QuickPlay::Target quickPlay;
    if (!m_options.quickPlayWorld.isEmpty()) {
        const QString worldDir =
            roots.versionDir + QStringLiteral("/saves/") + m_options.quickPlayWorld;
        if (!QDir(worldDir).exists()) {
            finishAll(QStringLiteral("工作区里没有世界 %1。").arg(m_options.quickPlayWorld));  // i18n-ignore
            return;
        }
        if (!QuickPlay::supports(m_versionJson, QuickPlay::Kind::Singleplayer)) {
            finishAll(QStringLiteral("这个版本不支持 --quickPlaySingleplayer。"));  // i18n-ignore
            return;
        }
        quickPlay.kind = QuickPlay::Kind::Singleplayer;
        quickPlay.value = m_options.quickPlayWorld;
    }

    GameWindowSettings window = m_owner->m_settings->gameWindowSettings();
    window = resolveGameWindow(
        window, gameWindowOverrideFromJson(
                    m_owner->m_versionManager->versionMeta(m_instanceId)));
    ScreenWorkArea screen;
    if (const QScreen *primary = QGuiApplication::primaryScreen()) {
        const QRect available = primary->availableGeometry();
        screen.width = available.width();
        screen.height = available.height();
    }
    m_args = m_owner->m_launcherManager->generateJvmArguments(
        QStringLiteral("0"), m_instanceId, assetIndexFor(m_versionJson), m_versionJson,
        m_javaSettings, true, javaMajorFor(m_versionJson), m_versionJvmArgs,
        QStringLiteral("WTFML-Test"), QString(), window, screen, quickPlay, &roots);
    if (m_args.isEmpty()) {
        finishAll(QStringLiteral("没有拼出启动参数。"));  // i18n-ignore
        return;
    }
    m_store.updateStatus(m_workspaceId, QStringLiteral("running"));
    m_launchId = newLaunchId();
    beginRun();
}

void AgentTestLaunchRunner::cancel() {
    if (!m_active) return;
    m_active = false;
    if (m_cancelFlag) m_cancelFlag->store(true);
    if (m_pollTimer) m_pollTimer->stop();
    if (m_timeoutTimer) m_timeoutTimer->stop();
    if (m_startTimer) m_startTimer->stop();
    stopProcess();
    if (!m_workspaceId.isEmpty()) {
        m_store.updateStatus(m_workspaceId, QStringLiteral("cancelled"));
    }
    m_done = {};
}
