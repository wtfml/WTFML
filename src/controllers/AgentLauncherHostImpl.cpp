/**
 * @file AgentLauncherHostImpl.cpp
 * @brief 智能体启动器工具的产品实现（见 .hpp）。
 */

#include "AgentLauncherHostImpl.hpp"

#include "MainWindow.hpp"

#include "InstancePagesController.hpp"
#include "agent/AgentPaths.hpp"
#include "agent/AgentRedaction.hpp"
#include "manage/BackupFiles.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/LaunchHistory.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "operations/TransferPlanner.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"
#include "util/SystemMemory.hpp"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>

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
    : m_owner(owner), m_maintenance(AgentPaths::maintenanceBackupDir()) {}

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
        if (problem != nullptr) *problem = QStringLiteral("主窗口未接入。");
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
        if (problem != nullptr) *problem = QStringLiteral("实例不存在。");
        return false;
    }
    if (facts != nullptr) *facts = factsFor(instanceId);
    return true;
}

bool AgentLauncherHostImpl::startupDiagnosis(const QString &instanceId,
                                             StartupDiagnosis::Report *report, QString *problem) {
    if (instanceId.isEmpty() || m_owner == nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("实例不存在。");
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
        if (problem != nullptr) *problem = QStringLiteral("实例不存在。");
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
            *problem = QStringLiteral("没有这一份实例的启动记录（可能还没启动过）。");
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
    const QString logPath = AppPaths::minecraftDir() + QStringLiteral("/logs/latest.log");
    const QString logText = readTail(logPath, kLogTailBytes);
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
    Q_UNUSED(instanceId);
    const QString latestLog = AppPaths::minecraftDir() + QStringLiteral("/logs/latest.log");
    const QString launcherLog = AppPaths::minecraftDir() + QStringLiteral("/logs/wtfml.log");
    QString source = QStringLiteral("logs/latest.log");
    QString raw = readTail(latestLog, kLogTailBytes);
    if (raw.isEmpty()) {
        source = QStringLiteral("logs/wtfml.log");
        raw = readTail(launcherLog, kLogTailBytes);
    }
    if (raw.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("找不到日志文件（latest.log / wtfml.log）。");
        return false;
    }
    const QStringList tail = lastLines(raw, qBound(1, lines, 500));
    // 路径/账户字段不外发（§9.4.1）：日志里出现本机路径的地方一律替换成占位符。
    QString joined = tail.join(QLatin1Char('\n'));
    joined = AgentDiagnosticProjection::scrubPaths(joined);
    if (text != nullptr) *text = joined;
    if (relativeSource != nullptr) *relativeSource = source;
    return true;
}

bool AgentLauncherHostImpl::crashReports(const QString &instanceId,
                                         QList<AgentHostCrashReport> *items, QString *problem) {
    Q_UNUSED(instanceId);
    Q_UNUSED(problem);
    if (items == nullptr) return false;
    items->clear();
    QDir dir(AppPaths::crashReportsDir());
    if (!dir.exists()) return true;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files,
                                                  QDir::Time);
    for (const QFileInfo &file : files) {
        AgentHostCrashReport report;
        report.fileName = file.fileName();
        report.modifiedAt = file.lastModified();
        report.bytes = file.size();
        report.headText = AgentDiagnosticProjection::scrubPaths(
            readHead(file.absoluteFilePath(), kCrashHeadBytes));
        items->append(report);
        if (items->size() >= 20) break;
    }
    return true;
}

bool AgentLauncherHostImpl::integrity(const QString &instanceId, VersionIntegrity::Report *report,
                                      QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("主窗口未接入。");
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
        items->append(entry);
    }
    return true;
}

bool AgentLauncherHostImpl::setModEnabled(const QString &instanceId, const QString &fileName,
                                          bool enabled, QString *previous, QString *problem) {
    QDir dir(modsDir(instanceId));
    if (!dir.exists()) {
        if (problem != nullptr) *problem = QStringLiteral("这个实例没有 mods 目录。");
        return false;
    }
    QString source = fileName;
    QString target;
    if (enabled) {
        if (!source.endsWith(QStringLiteral(".jar.disabled"))) {
            if (problem != nullptr) *problem = QStringLiteral("这个文件不是停用状态。");
            return false;
        }
        target = source;
        target.chop(int(qstrlen(".disabled")));
    } else {
        if (!source.endsWith(QStringLiteral(".jar"))) {
            if (problem != nullptr) *problem = QStringLiteral("这个文件不是启用状态。");
            return false;
        }
        target = source + QStringLiteral(".disabled");
    }
    if (QFileInfo::exists(dir.filePath(target))) {
        if (problem != nullptr) *problem = QStringLiteral("目标文件名已经存在，未改动任何文件。");
        return false;
    }
    if (!dir.rename(source, target)) {
        if (problem != nullptr) *problem = QStringLiteral("重命名失败（可能被占用）。");
        return false;
    }
    if (previous != nullptr) *previous = source;
    return true;
}

bool AgentLauncherHostImpl::settings(AgentHostSettings *settings, QString *problem) {
    if (m_owner == nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("主窗口未接入。");
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
        if (problem != nullptr) *problem = QStringLiteral("主窗口未接入。");
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
        if (problem != nullptr) *problem = QStringLiteral("这个设置键不在白名单里。");
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
    QList<AgentBackupEntry> entries;
    if (!m_maintenance.list(&entries, problem)) return false;
    for (const AgentBackupEntry &entry : entries) {
        AgentHostBackupRef ref;
        ref.backupId = entry.backupId;
        ref.owner = entry.owner;
        ref.source = entry.source;
        ref.label = entry.reason.isEmpty() ? QStringLiteral("维护备份") : entry.reason;
        ref.sourceSessionId = entry.sourceSessionId;
        ref.createdAt = entry.createdAt;
        ref.bytes = entry.size;
        ref.restorable = entry.owner == QStringLiteral("agent");
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
        if (problem != nullptr) *problem = QStringLiteral("实例目录不存在。");
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
    plan->saveLocation = AgentPaths::maintenanceBackupDir();
    return true;
}

bool AgentLauncherHostImpl::createBackup(const QString &sessionId, const QString &instanceId,
                                         const AgentHostBackupPlan &plan, QString *backupId,
                                         QString *problem) {
    const QString root = instanceDir(instanceId);
    QList<QPair<QString, QByteArray>> files;
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
        if (problem != nullptr) *problem = QStringLiteral("没有读到可备份的文件。");
        return false;
    }
    return m_maintenance.createFromFiles(sessionId, instanceId, files,
                                         QStringLiteral("backup.create：%1").arg(plan.scope),
                                         backupId, problem);
}

bool AgentLauncherHostImpl::restoreBackup(const QString &sessionId, const QString &backupId,
                                          QString *preRestoreBackupId, QString *problem) {
    QList<QPair<QString, QByteArray>> files;
    if (!m_maintenance.readFiles(backupId, &files, problem)) return false;
    QList<AgentBackupEntry> entries;
    m_maintenance.list(&entries);
    QString instanceId;
    for (const AgentBackupEntry &entry : entries) {
        if (entry.backupId == backupId) instanceId = entry.instanceId;
    }
    if (instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("备份元数据缺少实例 id，拒绝恢复。");
        return false;
    }
    const QString root = instanceDir(instanceId);
    if (!QFileInfo(root).isDir()) {
        if (problem != nullptr) *problem = QStringLiteral("实例目录已不存在。");
        return false;
    }
    // 恢复前快照：把**现在**这几个文件存一份，恢复出错还能原路回来。
    QList<QPair<QString, QByteArray>> current;
    for (const auto &file : files) {
        QFile existing(root + QLatin1Char('/') + file.first);
        if (!existing.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = existing.read(kBackupFileLimit + 1);
        if (bytes.size() > kBackupFileLimit) continue;
        current.append({file.first, bytes});
    }
    QString snapshotId;
    if (!current.isEmpty() &&
        !m_maintenance.createFromFiles(sessionId, instanceId, current,
                                       QStringLiteral("backup.restore 恢复前快照"), &snapshotId,
                                       nullptr)) {
        if (problem != nullptr) *problem = QStringLiteral("恢复前快照失败，已中止恢复。");
        return false;
    }
    for (const auto &file : files) {
        const QString relative = QString::fromUtf8(file.first.toUtf8());
        const QString normalized = QDir::cleanPath(relative);
        if (normalized.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(normalized) ||
            InstanceFiles::validatePathSegment(normalized.split(QLatin1Char('/')).constFirst())
                .isEmpty()) {
            if (problem != nullptr) *problem = QStringLiteral("备份里的路径不合法：%1").arg(relative);
            return false;
        }
        const QString target = root + QLatin1Char('/') + normalized;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile out(target);
        if (!out.open(QIODevice::WriteOnly) || out.write(file.second) != file.second.size() ||
            !out.commit()) {
            if (problem != nullptr) *problem = QStringLiteral("写回失败：%1").arg(normalized);
            return false;
        }
    }
    if (preRestoreBackupId != nullptr) *preRestoreBackupId = snapshotId;
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
                         QStringLiteral("只读检查；执行请走升级向导（upgrade.openWizard），"
                                        "由现有事务与回滚负责。")}};
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
        if (problem != nullptr) *problem = QStringLiteral("不认识的页面。");
        return false;
    }
    if (instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("会话还没有锁定实例。");
        return false;
    }
    m_owner->openInstanceSettings(instanceId, pageKey);
    return true;
}
