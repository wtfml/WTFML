/**
 * @file AgentLauncherHostImpl.cpp
 * @brief 智能体启动器工具的产品实现（见 .hpp）。
 */

#include "AgentLauncherHostImpl.hpp"

#include "AgentModPlatform.hpp"
#include "AgentRepairRunner.hpp"
#include "AgentTestLaunchRunner.hpp"

#include "MainWindow.hpp"

#include "BackupController.hpp"

#include "InstancePagesController.hpp"
#include "agent/AgentInstancePaths.hpp"
#include "agent/AgentPaths.hpp"
#include "agent/AgentRedaction.hpp"
#include "manage/BackupFiles.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/LaunchHistory.hpp"
#include "download/DownloadManager.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "operations/TransferPlanner.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"
#include "util/SystemMemory.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace {

constexpr qint64 kLogTailBytes = 256 * 1024;
constexpr qint64 kCrashHeadBytes = 8 * 1024;
constexpr int kIntegrityBudgetMs = 8000;
constexpr qint64 kBackupFileLimit = 8 * 1024 * 1024;
constexpr qint64 kBackupTotalLimit = 32 * 1024 * 1024;
constexpr int kBackupFileCountLimit = 400;

QString readHead(const QString &path, qint64 bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(file.read(bytes));
}

QString readTail(const QString &path, qint64 bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    const qint64 size = file.size();
    if (size > bytes) file.seek(size - bytes);
    return QString::fromUtf8(file.readAll());
}

QStringList lastLines(const QString &text, int lines) {
    QStringList all = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const int from = qMax(0, all.size() - lines);
    return all.mid(from);
}

/// 这一趟实际用的 Java 主版本：只读 `<javaHome>/release`，绝不起进程（D9 才允许起命令）。
int javaMajorForPath(const QString &javaPath) {
    if (javaPath.isEmpty()) return -1;
    QFileInfo info(javaPath);
    QString home = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (QFileInfo(home).fileName().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        home = QFileInfo(home).absolutePath();
    }
    const QString release = home + QStringLiteral("/release");
    const QString text = readHead(release, 64 * 1024);
    if (text.isEmpty()) return -1;
    return GlobalJavaSettings::majorVersionFromReleaseFile(text);
}

}  // namespace

AgentLauncherHostImpl::AgentLauncherHostImpl(MainWindow *owner)
    : m_owner(owner),
      m_maintenance(AgentPaths::maintenanceBackupDir()),
      m_testLaunchRunner(owner != nullptr ? std::make_unique<AgentTestLaunchRunner>(owner)
                                          : nullptr),
      m_repairRunner(owner != nullptr ? std::make_unique<AgentRepairRunner>(
                                           owner,
                                           [this](const QString &key, const QJsonValue &value,
                                                  QString *problem) {
                                               QJsonObject applied;
                                               return applySetting(key, value, &applied, problem);
                                           },
                                           [this](const QString &key) {
                                               AgentHostSettings settings;
                                               QString problem;
                                               if (!this->settings(&settings, &problem)) return QJsonValue();
                                               return settings.values.value(key);
                                           })
                                      : nullptr) {}

AgentLauncherHostImpl::~AgentLauncherHostImpl() = default;

// ---------------------------------------------------------------- 模组（S5.4）

bool AgentLauncherHostImpl::lookupModPlatform(const QString &instanceId,
                                              const QString &gameVersion,
                                              const QString &loaderVersion,
                                              const QString &loader, const QStringList &sha1s,
                                              const PlatformCompletion &done, QString *problem) {
    Q_UNUSED(instanceId);
    Q_UNUSED(loaderVersion);
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");  // i18n-ignore
        return false;
    }
    if (m_modPlatformBusy) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "上一次平台查询尚未结束。");
        return false;
    }
    m_modPlatformBusy = true;
    AgentModPlatform::LookupQuery query;
    query.sha1s = sha1s;
    query.gameVersion = gameVersion;
    query.loader = loader;
    query.apiBase = m_owner->m_settings->modrinthApiBaseOverride();
    QPointer<MainWindow> owner(m_owner);
    (void)QtConcurrent::run([this, owner, query, done]() {
        const AgentModPlatform::LookupResult result = AgentModPlatform::lookupModrinth(query);
        MainWindow *window = owner.data();
        if (window == nullptr) return;
        QMetaObject::invokeMethod(
            window,
            [this, result, done]() {
                m_modPlatformBusy = false;
                if (done) done(result.entries, result.problem);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool AgentLauncherHostImpl::resolveModTarget(const QString &instanceId, const QString &projectId,
                                             const QString &versionId,
                                             const QString &expectedSha1,
                                             const ResolveCompletion &done, QString *problem) {
    Q_UNUSED(instanceId);
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");  // i18n-ignore
        return false;
    }
    if (m_modResolveBusy) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "上一次地址解析尚未结束。");
        return false;
    }
    m_modResolveBusy = true;
    AgentModPlatform::TargetQuery query;
    query.projectId = projectId;
    query.versionId = versionId;
    query.expectedSha1 = expectedSha1;
    query.gameVersion = factsFor(instanceId).gameVersion;
    query.loader = factsFor(instanceId).loaderName.toLower();
    query.apiBase = m_owner->m_settings->modrinthApiBaseOverride();
    QPointer<MainWindow> owner(m_owner);
    (void)QtConcurrent::run([this, owner, query, done]() {
        const AgentModPlatform::TargetResult result = AgentModPlatform::resolveModrinth(query);
        MainWindow *window = owner.data();
        if (window == nullptr) return;
        QMetaObject::invokeMethod(
            window,
            [this, result, done]() {
                m_modResolveBusy = false;
                if (done) done(result.target, result.problem);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool AgentLauncherHostImpl::downloadModFile(const AgentHostModTarget &target,
                                            const QString &stagingDir,
                                            const DownloadCompletion &done, QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");  // i18n-ignore
        return false;
    }
    if (m_modDownloadJob) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "上一次下载尚未结束。");
        return false;
    }
    if (target.url.isEmpty() || target.fileName.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "下载目标不完整。");
        return false;
    }
    if (!QDir().mkpath(stagingDir)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "无法创建暂存目录。");
        return false;
    }
    if (m_modDownloader == nullptr) {
        m_modDownloader = new DownloadManager(nullptr);
        m_modDownloader->setAllowOverwrite(false);
        QObject::connect(
            m_modDownloader, &DownloadManager::downloadFinished, m_owner,
            [this](const QString &fileName, bool success, const QString &errorMessage) {
                if (!m_modDownloadJob || m_modDownloadJob->fileName != fileName) return;
                auto job = std::move(m_modDownloadJob);
                AgentModHost::DownloadOutcome outcome;
                outcome.ok = success;
                outcome.problem = errorMessage;
                if (success) {
                    const QString path = m_modDownloader->property("wtfml.agent.stagedPath").toString();
                    outcome.stagedPath = path;
                    QFile file(path);
                    outcome.bytes = file.exists() ? file.size() : -1;
                    if (file.open(QIODevice::ReadOnly)) {
                        QCryptographicHash hash(QCryptographicHash::Sha1);
                        while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
                        outcome.sha1 = QString::fromLatin1(hash.result().toHex());
                    }
                }
                m_modDownloader->setProperty("wtfml.agent.stagedPath", QVariant());
                if (job && job->done) job->done(outcome);
            });
    }
    const QString stagedPath =
        stagingDir + QLatin1Char('/') + QLatin1String("mod-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".jar");
    m_modDownloader->setProperty("wtfml.agent.stagedPath", stagedPath);
    m_modDownloadJob = std::make_unique<ModDownloadJob>();
    m_modDownloadJob->fileName = QFileInfo(stagedPath).fileName();
    m_modDownloadJob->done = done;
    m_modDownloader->downloadFile(target.url, stagedPath, target.sha1, target.size);
    return true;
}

bool AgentLauncherHostImpl::repairCoreFiles(const QString &instanceId,
                                            const QStringList &flaggedPaths,
                                            const RepairCompletion &done, QString *problem) {
    if (m_owner == nullptr || m_owner->m_versionManager == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");  // i18n-ignore
        return false;
    }
    if (m_owner->m_versionManager->downloads()->isBusy()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "已有下载任务正在进行，请等待其结束后再修复。");
        return false;
    }
    struct State {
        bool finished = false;
        RepairCompletion completion;
    };
    auto state = std::make_shared<State>();
    state->completion = done;
    auto *guard = new QObject(m_owner);
    QObject::connect(
        m_owner->m_versionManager, &VersionManager::versionDownloadFinished, guard,
        [state, guard, instanceId](const QString &id, bool ok, const QString &message) {
            if (state->finished || id != instanceId) return;
            state->finished = true;
            guard->deleteLater();
            if (state->completion) {
                AgentModHost::RepairOutcome outcome;
                outcome.ok = ok;
                outcome.problem = message;
                state->completion(outcome);
            }
        });
    m_owner->m_versionManager->repairVersionFiles(instanceId, flaggedPaths);
    return true;
}

QString AgentLauncherHostImpl::instanceDir(const QString &instanceId) const {
    return AppPaths::versionDir(instanceId);
}

QString AgentLauncherHostImpl::modsDir(const QString &instanceId) const {
    return InstancePagesController::fileDirs(instanceId).value(QStringLiteral("mod"));
}

AgentDiagnosticProjection::InstanceFacts AgentLauncherHostImpl::factsFor(
    const QString &instanceId) const {
    AgentDiagnosticProjection::InstanceFacts facts;
    facts.instanceId = instanceId;
    if (instanceId.isEmpty() || m_owner == nullptr) return facts;
    facts.instanceName = m_owner->instanceDisplayName(instanceId);
    const VersionDisplayParts parts = m_owner->m_versionManager->displayParts(instanceId);
    facts.gameVersion = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
    QString loaderVersion;
    facts.loaderName = InstancePagesController::loaderName(parts, m_owner->m_versionManager,
                                                          &loaderVersion);
    facts.loaderVersion = loaderVersion;
    facts.requiredJavaMajor = m_owner->m_versionManager->getVersionJson(instanceId)
                                  .value(QStringLiteral("javaVersion"))
                                  .toObject()
                                  .value(QStringLiteral("majorVersion"))
                                  .toInt(-1);
    const JavaSettingsOverride override =
        javaOverrideFromJson(m_owner->m_versionManager->versionMeta(instanceId));
    const EffectiveJavaSettings java =
        resolveJavaSettings(m_owner->m_javaSettings->effective(), override);
    facts.usedJavaMajor = javaMajorForPath(java.javaPath);
    const SystemMemory::Usage usage = SystemMemory::read();
    if (java.autoMemory) {
        const int modCount = m_owner->instanceModCount(instanceId);
        facts.xmxMb = GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb, modCount);
        facts.xmsMb = GlobalJavaSettings::suggestXmsMb(facts.xmxMb);
    } else {
        facts.xmxMb = java.xmx;
        facts.xmsMb = java.xms;
    }
    facts.autoMemory = java.autoMemory;
    const QDir mods(modsDir(instanceId));
    if (mods.exists()) {
        const QStringList names =
            mods.entryList({QStringLiteral("*.jar"), QStringLiteral("*.jar.disabled")},
                           QDir::Files, QDir::Name);
        for (const QString &name : names) {
            if (name.endsWith(QStringLiteral(".jar.disabled"))) {
                ++facts.disabledModCount;
            } else {
                ++facts.modCount;
            }
        }
    }
    facts.running = m_owner->installingVersionIds().contains(instanceId);
    return facts;
}

bool AgentLauncherHostImpl::instanceList(
    QList<AgentDiagnosticProjection::InstanceFacts> *items, QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");
        return false;
    }
    if (items == nullptr) return false;
    items->clear();
    const QStringList ids = m_owner->m_versionManager->installedVersions();
    for (const QString &id : ids) items->append(factsFor(id));
    return true;
}

bool AgentLauncherHostImpl::instanceFacts(const QString &instanceId,
                                          AgentDiagnosticProjection::InstanceFacts *facts,
                                          QString *problem) {
    if (instanceId.isEmpty() || !QFileInfo(instanceDir(instanceId)).isDir()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "实例不存在。");
        return false;
    }
    if (facts != nullptr) *facts = factsFor(instanceId);
    return true;
}

bool AgentLauncherHostImpl::startupDiagnosis(const QString &instanceId,
                                             StartupDiagnosis::Report *report, QString *problem) {
    if (instanceId.isEmpty() || m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "实例不存在。");
        return false;
    }
    const StartupDiagnosis::Inputs inputs = m_owner->startupDiagnosisInputs(instanceId);
    if (report != nullptr) *report = StartupDiagnosis::analyze(inputs);
    return true;
}

bool AgentLauncherHostImpl::crashFacts(const QString &instanceId,
                                       GameExitDiagnosis::Facts *facts, QStringList *hints,
                                       QString *problem) {
    if (instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "实例不存在。");
        return false;
    }
    const QList<LaunchHistory::Record> records = LaunchHistory::load(LaunchHistory::historyPath());
    LaunchHistory::Record latest;
    for (const LaunchHistory::Record &record : records) {
        if (record.instanceId != instanceId) continue;
        if (!latest.isValid() || record.startedAt > latest.startedAt) latest = record;
    }
    if (!latest.isValid()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentLauncherHostImpl", "该实例没有启动记录，可能尚未启动过。");
        }
        return false;
    }
    GameExitDiagnosis::Facts built;
    built.instanceId = instanceId;
    built.instanceName = latest.instanceName;
    built.crashed = latest.result == LaunchHistory::Result::Crash;
    // 账本里没有退出码这一格：0 = 不知道，别编一个具体的 NTSTATUS 出来。
    built.exitCode = 0;
    const AgentDiagnosticProjection::InstanceFacts info = factsFor(instanceId);
    built.loaderName = info.loaderName;
    built.loaderVersion = info.loaderVersion;
    built.requiredJavaMajor = info.requiredJavaMajor > 0 ? info.requiredJavaMajor : 0;
    built.usedJavaMajor = info.usedJavaMajor > 0 ? info.usedJavaMajor : 0;
    built.xmxMb = info.xmxMb;
    built.xmsMb = info.xmsMb;
    const SystemMemory::Usage usage = SystemMemory::read();
    built.totalMemoryMb = usage.totalMb;
    // 游戏日志在实例目录里（`--gameDir versions/<id>`）；数据根那一份只作为兜底。
    const QList<AgentInstancePaths::Source> logs = AgentInstancePaths::logSources(instanceId);
    const QString logText = logs.isEmpty() ? QString() : readTail(logs.constFirst().path, kLogTailBytes);
    built.logCaptured = !logText.isEmpty();
    built.sawOutOfMemory = GameExitDiagnosis::logShowsOutOfMemory(logText);
    built.sawMissingDependency = GameExitDiagnosis::logShowsMissingDependency(logText);
    int errors = 0;
    for (const QString &line : logText.split(QLatin1Char('\n'))) {
        if (line.contains(QStringLiteral("ERROR")) || line.contains(QStringLiteral("FATAL"))) {
            ++errors;
        }
    }
    built.errorLineCount = qMin(errors, GameExitDiagnosis::kExcerptLineLimit);
    if (facts != nullptr) *facts = built;
    if (hints != nullptr) *hints = GameExitDiagnosis::hints(built);
    return true;
}

bool AgentLauncherHostImpl::logTail(const QString &instanceId, int lines, QString *text,
                                    QString *relativeSource, QString *problem) {
    // 顺序：实例目录的 latest.log → 数据根的 latest.log → 数据根的 wtfml.log。
    QString source;
    QString raw;
    for (const AgentInstancePaths::Source &candidate :
         AgentInstancePaths::logSources(instanceId)) {
        raw = readTail(candidate.path, kLogTailBytes);
        if (raw.isEmpty()) continue;
        source = candidate.label;
        break;
    }
    if (raw.isEmpty()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate(
                "AgentLauncherHostImpl",
                "未找到日志文件（实例目录的 logs/latest.log 或数据根的 logs/latest.log、logs/wtfml.log）。");
        }
        return false;
    }
    const QStringList tail = lastLines(raw, qBound(1, lines, 500));
    // 路径/账户字段不外发（§9.4.1）：本机路径、启动参数、MAC/SID、用户名的位置一律替换。
    QString joined = tail.join(QLatin1Char('\n'));
    joined = AgentDiagnosticProjection::scrubLogText(joined);
    if (text != nullptr) *text = joined;
    if (relativeSource != nullptr) *relativeSource = source;
    return true;
}

bool AgentLauncherHostImpl::logSources(const QString &instanceId,
                                       QList<AgentHostLogSource> *items, QString *problem) {
    Q_UNUSED(problem);
    if (items == nullptr) return false;
    items->clear();
    for (const AgentInstancePaths::Source &candidate :
         AgentInstancePaths::logSources(instanceId)) {
        AgentHostLogSource item;
        item.key = candidate.key;
        item.label = candidate.label;
        item.path = candidate.path;
        const QFileInfo info(candidate.path);
        item.bytes = info.size();
        item.modifiedAt = info.lastModified();
        items->append(item);
    }
    return true;
}

bool AgentLauncherHostImpl::crashReports(const QString &instanceId,
                                         QList<AgentHostCrashReport> *items, QString *problem) {
    Q_UNUSED(problem);
    if (items == nullptr) return false;
    items->clear();
    // 崩溃报告在实例目录的 `crash-reports/` 里（游戏以 `--gameDir versions/<id>` 启动）；
    // 数据根那一份是别的启动器/旧启动留下的，两处合并、同名以实例目录为准。
    for (const AgentInstancePaths::CrashReportFile &file :
         AgentInstancePaths::collectCrashReports(instanceId, 20)) {
        AgentHostCrashReport report;
        report.fileName = file.fileName;
        report.path = file.path;
        report.label = file.label;
        report.modifiedAt = file.modifiedAt;
        report.bytes = file.bytes;
        report.headText =
            AgentDiagnosticProjection::scrubLogText(readHead(file.path, kCrashHeadBytes));
        items->append(report);
    }
    return true;
}

bool AgentLauncherHostImpl::modsDirectory(const QString &instanceId, QString *path,
                                          QString *problem) {
    const QString dir = modsDir(instanceId);
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "mods 目录不存在。");
        return false;
    }
    if (path != nullptr) *path = dir;
    return true;
}

bool AgentLauncherHostImpl::integrity(const QString &instanceId, VersionIntegrity::Report *report,
                                      QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");
        return false;
    }
    QString planError;
    const QList<VersionIntegrity::ExpectedFile> files =
        m_owner->m_versionManager->integrityPlan(instanceId, &planError);
    VersionIntegrity::Report result;
    if (!planError.isEmpty()) {
        result.error = planError;
        if (report != nullptr) *report = result;
        return true;
    }
    result.checked = int(files.size());
    // 时间预算：这一层跑在 UI 线程上，几千个文件逐个比不能无界（超了就给部分结果）。
    QElapsedTimer timer;
    timer.start();
    for (const VersionIntegrity::ExpectedFile &file : files) {
        if (timer.elapsed() > kIntegrityBudgetMs) break;
        const VersionIntegrity::Verdict verdict = VersionIntegrity::check(file);
        if (!verdict.ok()) result.problems.append(verdict);
    }
    if (report != nullptr) *report = result;
    return true;
}

bool AgentLauncherHostImpl::mods(const QString &instanceId, QList<AgentHostModEntry> *items,
                                 QString *problem) {
    Q_UNUSED(problem);
    if (items == nullptr) return false;
    items->clear();
    QDir dir(modsDir(instanceId));
    if (!dir.exists()) return true;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.jar"), QStringLiteral("*.jar.disabled")},
                          QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        AgentHostModEntry entry;
        entry.fileName = file.fileName();
        entry.enabled = file.fileName().endsWith(QStringLiteral(".jar"));
        entry.bytes = file.size();
        entry.path = file.absoluteFilePath();
        items->append(entry);
    }
    return true;
}

bool AgentLauncherHostImpl::setModEnabled(const QString &instanceId, const QString &fileName,
                                          bool enabled, QString *previous, QString *problem) {
    QDir dir(modsDir(instanceId));
    if (!dir.exists()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "该实例没有 mods 目录。");
        return false;
    }
    QString source = fileName;
    QString target;
    if (enabled) {
        if (!source.endsWith(QStringLiteral(".jar.disabled"))) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "该文件未被禁用。");
            return false;
        }
        target = source;
        target.chop(int(qstrlen(".disabled")));
    } else {
        if (!source.endsWith(QStringLiteral(".jar"))) {
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "该文件未启用。");
            return false;
        }
        target = source + QStringLiteral(".disabled");
    }
    if (QFileInfo::exists(dir.filePath(target))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "目标文件名已存在，未修改任何文件。");
        return false;
    }
    if (!dir.rename(source, target)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "重命名失败，文件可能被占用。");
        return false;
    }
    if (previous != nullptr) *previous = source;
    return true;
}

bool AgentLauncherHostImpl::settings(AgentHostSettings *settings, QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");
        return false;
    }
    if (settings == nullptr) return false;
    QJsonObject values;
    values.insert(QStringLiteral("memory.auto"), m_owner->m_javaSettings->autoMemory());
    values.insert(QStringLiteral("memory.xmx"), m_owner->m_javaSettings->xmx());
    values.insert(QStringLiteral("memory.xms"), m_owner->m_javaSettings->xms());
    values.insert(QStringLiteral("java.autoSelect"), m_owner->m_javaSettings->autoSelectJava());
    values.insert(QStringLiteral("args.extraJvm"), m_owner->m_javaSettings->extraJvmArgs());
    values.insert(QStringLiteral("launch.afterLaunch"),
                  LauncherSettings::behaviourKey(m_owner->m_settings->launcherBehaviour()));
    values.insert(QStringLiteral("launch.showGameLog"), m_owner->m_settings->showGameLog());
    values.insert(QStringLiteral("launch.processPriority"),
                  LauncherSettings::processPriorityKey(m_owner->m_settings->processPriority()));
    settings->values = values;
    // `java.path` 只写不读（路径不外发）。
    settings->writableKeys = {QStringLiteral("memory.auto"),
                              QStringLiteral("memory.xmx"),
                              QStringLiteral("memory.xms"),
                              QStringLiteral("java.autoSelect"),
                              QStringLiteral("java.path"),
                              QStringLiteral("args.extraJvm"),
                              QStringLiteral("launch.afterLaunch"),
                              QStringLiteral("launch.showGameLog"),
                              QStringLiteral("launch.processPriority")};
    return true;
}

bool AgentLauncherHostImpl::applySetting(const QString &key, const QJsonValue &value,
                                         QJsonObject *applied, QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "主窗口未接入。");
        return false;
    }
    if (key == QStringLiteral("memory.auto")) {
        m_owner->m_javaSettings->setAutoMemory(value.toBool());
    } else if (key == QStringLiteral("memory.xmx")) {
        m_owner->m_javaSettings->setXmx(value.toInt());
    } else if (key == QStringLiteral("memory.xms")) {
        m_owner->m_javaSettings->setXms(value.toInt());
    } else if (key == QStringLiteral("java.autoSelect")) {
        m_owner->m_javaSettings->setAutoSelectJava(value.toBool());
        m_owner->m_javaSettings->save();
    } else if (key == QStringLiteral("java.path")) {
        m_owner->m_javaSettings->setJavaPath(value.toString());
        m_owner->m_javaSettings->save();
    } else if (key == QStringLiteral("args.extraJvm")) {
        m_owner->m_javaSettings->setExtraJvmArgs(value.toString());
    } else if (key == QStringLiteral("launch.afterLaunch")) {
        m_owner->m_settings->setLauncherBehaviour(
            LauncherSettings::behaviourFromKey(value.toString()));
    } else if (key == QStringLiteral("launch.showGameLog")) {
        m_owner->m_settings->setShowGameLog(value.toBool());
    } else if (key == QStringLiteral("launch.processPriority")) {
        m_owner->m_settings->setProcessPriority(
            LauncherSettings::processPriorityFromKey(value.toString()));
    } else {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "该设置键不在白名单中。");
        return false;
    }
    if (applied != nullptr) *applied = QJsonObject{{QStringLiteral("key"), key}};
    return true;
}

bool AgentLauncherHostImpl::javaVersions(QList<int> *majors, QString *problem) {
    if (majors == nullptr) return false;
    majors->clear();
    const QMap<int, QString> scanned = GlobalJavaSettings::scanLocalJavaVersions();
    for (auto it = scanned.begin(); it != scanned.end(); ++it) majors->append(it.key());
    Q_UNUSED(problem);
    return true;
}

bool AgentLauncherHostImpl::backups(QList<AgentHostBackupRef> *items, QString *problem) {
    if (items == nullptr) return false;
    items->clear();
    Q_UNUSED(problem);
    // 新格式（启动器备份目录）与冻结的旧库一起列；旧库只读，恢复前快照一律进新格式。
    for (const AgentBackups::Item &item : AgentBackups::list(QString(), &m_maintenance)) {
        AgentHostBackupRef ref;
        ref.backupId = item.backupId;
        ref.owner = QStringLiteral("agent");
        ref.source = item.legacy() ? QStringLiteral("agent-session") : QStringLiteral("launcher-files");
        ref.label = AgentBackups::label(item);
        ref.sourceSessionId = item.sessionId;
        ref.instanceId = item.instanceId;
        ref.createdAt = item.createdAt;
        ref.bytes = item.bytes;
        ref.restorable = true;
        items->append(ref);
    }
    // 启动器自己的实例/世界备份只列文件（读清单要起一次外部进程，太贵）：只引用不改写。
    const QStringList launcherDirs{BackupFiles::backupDir(BackupFiles::Kind::Instance),
                                   BackupFiles::backupDir(BackupFiles::Kind::World)};
    for (const QString &dirPath : launcherDirs) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;
        const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.zip")}, QDir::Files,
                                                      QDir::Time);
        for (const QFileInfo &file : files) {
            AgentHostBackupRef ref;
            ref.backupId = file.completeBaseName();
            ref.owner = QStringLiteral("launcher");
            ref.source = QStringLiteral("launcher-backup");
            ref.label = file.fileName();
            ref.createdAt = file.lastModified();
            ref.bytes = file.size();
            ref.restorable = false;
            items->append(ref);
            if (items->size() >= 200) break;
        }
    }
    return true;
}

bool AgentLauncherHostImpl::planBackup(const QString &instanceId, const QStringList &scopes,
                                       AgentHostBackupPlan *plan, QString *problem) {
    if (plan == nullptr) return false;
    const QString root = instanceDir(instanceId);
    if (!QFileInfo(root).isDir()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "实例目录不存在。");
        return false;
    }
    QStringList relativePaths;
    qint64 bytes = 0;
    const auto addFile = [&](const QString &relative) {
        const QFileInfo info(root + QLatin1Char('/') + relative);
        if (!info.isFile()) return;
        if (relativePaths.size() >= kBackupFileCountLimit) return;
        if (bytes + info.size() > kBackupTotalLimit) return;
        relativePaths.append(relative);
        bytes += info.size();
    };
    for (const QString &scope : scopes) {
        if (scope == QStringLiteral("config")) {
            QDirIterator it(root + QStringLiteral("/config"), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                addFile(QDir(root).relativeFilePath(it.filePath()));
            }
        } else if (scope == QStringLiteral("mods")) {
            QDirIterator it(modsDir(instanceId), QDir::Files);
            while (it.hasNext()) {
                it.next();
                addFile(QDir(root).relativeFilePath(it.filePath()));
            }
        } else if (scope == QStringLiteral("options")) {
            addFile(QStringLiteral("options.txt"));
        } else if (scope == QStringLiteral("servers")) {
            addFile(QStringLiteral("servers.dat"));
        }
    }
    plan->scope = scopes.join(QStringLiteral("+"));
    plan->relativePaths = relativePaths;
    plan->bytes = bytes;
    plan->saveLocation = BackupFiles::backupDir(BackupFiles::Kind::Files);
    return true;
}

bool AgentLauncherHostImpl::createBackup(const QString &sessionId, const QString &instanceId,
                                         const AgentHostBackupPlan &plan, QString *backupId,
                                         QString *problem) {
    const QString root = instanceDir(instanceId);
    QList<BackupFiles::FilePayload> files;
    qint64 total = 0;
    for (const QString &relative : plan.relativePaths) {
        QFile file(root + QLatin1Char('/') + relative);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.read(kBackupFileLimit + 1);
        if (bytes.size() > kBackupFileLimit) continue;
        total += bytes.size();
        if (total > kBackupTotalLimit) break;
        files.append({relative, bytes});
    }
    if (files.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "未读取到可备份的文件。");
        return false;
    }
    return AgentBackups::write(QString(AgentBackups::kTriggerCreate), instanceId, sessionId,
                               QString(), plan.scope, files, backupId, nullptr,
                               problem);
}

bool AgentLauncherHostImpl::restoreBackup(const QString &sessionId, const QString &backupId,
                                          QString *preRestoreBackupId, QString *problem) {
    const auto failWith = [problem](const QString &text) {
        if (problem != nullptr) *problem = text;
        return false;
    };
    AgentBackups::Item item;
    for (const AgentBackups::Item &candidate : AgentBackups::list(QString(), &m_maintenance)) {
        if (candidate.backupId == backupId) item = candidate;
    }
    if (item.backupId.isEmpty()) {
        return failWith(QCoreApplication::translate("AgentLauncherHostImpl", "找不到该备份。"));
    }
    const QString instanceId = item.instanceId;
    if (instanceId.isEmpty()) {
        return failWith(QCoreApplication::translate("AgentLauncherHostImpl", "备份元数据缺少实例 ID，已拒绝恢复。"));
    }
    const QString root = instanceDir(instanceId);
    if (!QFileInfo(root).isDir()) {
        return failWith(QCoreApplication::translate("AgentLauncherHostImpl", "实例目录已不存在。"));
    }
    // 和备份页「从快照恢复…」同三道闸：运行中、正被复制/安装/还原、备份任务进行中。
    if (m_owner != nullptr &&
        ((m_owner->m_launcherManager != nullptr && m_owner->m_launcherManager->isVersionRunning(instanceId)) ||
         m_owner->installingVersionIds().contains(instanceId) ||
         (m_owner->m_backupController != nullptr && m_owner->m_backupController->busy()))) {
        return failWith(QCoreApplication::translate("AgentLauncherHostImpl", "实例正在运行，或正被复制、备份、还原或安装占用，已拒绝恢复。请等待其结束后再试。"));
    }
    QList<BackupFiles::FilePayload> files;
    if (!AgentBackups::readFiles(item, &m_maintenance, &files, problem)) return false;
    BackupFiles::Manifest safety;
    safety.root = QStringLiteral("files");
    safety.label = instanceId;
    safety.sourceInstanceId = instanceId;
    safety.sessionId = sessionId;
    if (m_owner != nullptr) m_owner->markFileOperationBusy(instanceId);
    const BackupFiles::RestoreResult restored = BackupFiles::restoreFiles(root, files, safety);
    if (m_owner != nullptr) m_owner->releaseFileOperationBusy(instanceId);
    BackupFiles::pruneFilesBackups(instanceId, sessionId);
    if (!restored.ok) return failWith(restored.problem);
    if (preRestoreBackupId != nullptr) {
        *preRestoreBackupId =
            restored.safetyZip.isEmpty() ? QString() : QFileInfo(restored.safetyZip).fileName();
    }
    return true;
}

bool AgentLauncherHostImpl::upgradeInspect(const QString &instanceId, QJsonObject *data,
                                           QString *problem) {
    Q_UNUSED(problem);
    if (m_owner == nullptr || data == nullptr) return false;
    const VersionDisplayParts source = m_owner->m_versionManager->displayParts(instanceId);
    const QString sourceGame = source.realVersion.isEmpty() ? source.id : source.realVersion;
    QString sourceLoaderVersion;
    const QString sourceLoader = InstancePagesController::loaderName(
        source, m_owner->m_versionManager, &sourceLoaderVersion);
    QJsonArray targets;
    const QStringList ids = m_owner->m_versionManager->installedVersions();
    for (const QString &id : ids) {
        if (id == instanceId) continue;
        const VersionDisplayParts target = m_owner->m_versionManager->displayParts(id);
        const QString targetGame = target.realVersion.isEmpty() ? target.id : target.realVersion;
        QString targetLoaderVersion;
        const QString targetLoader = InstancePagesController::loaderName(
            target, m_owner->m_versionManager, &targetLoaderVersion);
        const TransferPlanner::Compatibility compatibility = TransferPlanner::compatibility(
            sourceGame, sourceLoader, targetGame, targetLoader);
        targets.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), m_owner->instanceDisplayName(id)},
            {QStringLiteral("gameVersion"), targetGame},
            {QStringLiteral("loaderName"), targetLoader},
            {QStringLiteral("loaderVersion"), targetLoaderVersion},
            {QStringLiteral("compatibility"), TransferPlanner::compatibilityName(compatibility)},
            {QStringLiteral("compatibilityText"),
             TransferPlanner::compatibilityText(compatibility, sourceGame, sourceLoader, targetGame,
                                                targetLoader)}});
        if (targets.size() >= 40) break;
    }
    *data = QJsonObject{{QStringLiteral("instance"), factsFor(instanceId).toJson()},
                        {QStringLiteral("targets"), targets},
                        {QStringLiteral("executionPath"),
                         QStringLiteral("Read-only check. To apply the upgrade, open the upgrade wizard "  // i18n-ignore
                                        "(upgrade.openWizard), which handles transactions and rollback.")}};  // i18n-ignore
    return true;
}

bool AgentLauncherHostImpl::openPage(const QString &page, const QString &instanceId,
                                     QString *problem) {
    if (m_owner == nullptr) return false;
    QString pageKey;
    if (page == QStringLiteral("instance-settings")) {
        pageKey = QStringLiteral("overview");
    } else if (page == QStringLiteral("logs")) {
        pageKey = QStringLiteral("logs");
    } else if (page == QStringLiteral("backups")) {
        pageKey = QStringLiteral("worldbackup");
    } else if (page == QStringLiteral("integrity")) {
        pageKey = QStringLiteral("integrity");
    } else if (page == QStringLiteral("upgrade")) {
        pageKey = QStringLiteral("upgrade");
    } else {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "未知页面。");
        return false;
    }
    if (instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "会话尚未锁定实例。");
        return false;
    }
    m_owner->openInstanceSettings(instanceId, pageKey);
    return true;
}

bool AgentLauncherHostImpl::startTestLaunch(const QString &instanceId, const QString &workspaceId,
                                            const AgentTestLaunchOptions &options,
                                            const Completion &done, QString *problem) {
    if (m_testLaunchRunner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "测试启动未接入。");
        return false;
    }
    return m_testLaunchRunner->start(instanceId, workspaceId, options, done, problem);
}

void AgentLauncherHostImpl::cancelTestLaunch() {
    if (m_testLaunchRunner != nullptr) m_testLaunchRunner->cancel();
}

bool AgentLauncherHostImpl::beginTransaction(const QString &instanceId,
                                              const QStringList &scopes,
                                              const BeginCompletion &done, QString *problem) {
    if (m_repairRunner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "修复事务未接入。");
        return false;
    }
    return m_repairRunner->begin(instanceId, scopes, done, problem);
}

bool AgentLauncherHostImpl::finishTransaction(const QString &transactionId, bool commit,
                                              const FinishCompletion &done, QString *problem) {
    if (m_repairRunner == nullptr) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentLauncherHostImpl", "修复事务未接入。");
        return false;
    }
    return m_repairRunner->finish(transactionId, commit, done, problem);
}
