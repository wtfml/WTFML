#include "FileOperationRecoveryController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "manage/FolderManager.hpp"
#include "manage/WorldFiles.hpp"
#include "operations/TransferApply.hpp"
#include "operations/TransferPlanner.hpp"
#include "util/AppPaths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <utility>

namespace {

const char *kContextWorldDir = "worldDir";
const char *kContextSavesDir = "savesDir";
const char *kContextName = "name";
const char *kContextDataRoot = "dataRoot";
const char *kContextSourceDir = "sourceDir";
const char *kContextTargetDir = "targetDir";
const char *kContextKeys = "keys";
const char *kContextItems = "items";
const char *kContextSourcePath = "sourcePath";
const char *kContextTargetPath = "targetPath";

QString contextString(const QJsonObject &context, const char *key) {
    return context.value(QLatin1String(key)).toString();
}

Tasks::TaskStep step(const QString &name, Tasks::TaskStepState state) {
    return Tasks::TaskStep{name, state, {}};
}

OperationReport::Entry entry(const QString &path, OperationReport::EntryState state,
                             const QString &reason = QString(), qint64 bytes = 0) {
    OperationReport::Entry out;
    out.path = path;
    out.state = state;
    out.reason = reason;
    out.bytes = bytes;
    return out;
}

/// 单文件原子复制（续做用）。失败时目标保持原样或不存在。
bool copyFileAtomically(const QString &source, const QString &target, QString *problem) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法读取 %1（%2）").arg(source, in.errorString());
        return false;
    }
    const QFileInfo info(target);
    if (!QDir().mkpath(info.absolutePath())) {
        if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法创建目录 %1").arg(info.absolutePath());
        return false;
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法写入 %1（%2）").arg(target, out.errorString());
        return false;
    }
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty()) {
            if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法读取 %1（%2）").arg(source, in.errorString());
            return false;
        }
        if (out.write(chunk) != chunk.size()) {
            if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法写入 %1（%2）").arg(target, out.errorString());
            return false;
        }
    }
    if (!out.commit()) {
        if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "无法发布 %1（%2）").arg(target, out.errorString());
        return false;
    }
    return true;
}

}  // namespace

FileOperationRecoveryController::FileOperationRecoveryController(OverlayHost *overlay, Host host,
                                                                 QObject *parent)
    : QObject(parent), m_overlay(overlay), m_host(std::move(host)) {}

void FileOperationRecoveryController::log(const QString &text) const {
    if (m_host.log) m_host.log(text);
}

bool FileOperationRecoveryController::registerReport(const OperationReport::Report &report,
                                                     QString *problem) {
    if (report.id.isEmpty()
        || (report.entries.isEmpty() && report.totalFiles <= 0)) {
        if (problem) *problem = QCoreApplication::translate("FileOperationRecoveryController", "报告缺少编号或无任何条目，未登记。");
        return false;
    }
    const QString dataRoot = contextString(report.context, kContextDataRoot).isEmpty()
        ? AppPaths::minecraftDir()
        : contextString(report.context, kContextDataRoot);
    OperationReport::Report saved = report;
    saved.updatedAt = QDateTime::currentDateTime();
    if (saved.startedAt.isNull()) {
        saved.startedAt = saved.updatedAt;
    }
    const QString path = OperationReport::reportPath(dataRoot, saved.id);
    if (!OperationReport::saveReport(path, saved, problem)) {
        return false;
    }
    // 上一次会话留下的、已经没有报告在用的暂存，这里顺手收掉（只碰本模块自己的目录名）。
    cleanupStaleStaging(dataRoot);
    m_jobs.insert(saved.id, Job{path, dataRoot, saved});
    log(QCoreApplication::translate("FileOperationRecoveryController", "已登记可续的文件操作：%1（%2）").arg(saved.title, path));
    return true;
}

bool FileOperationRecoveryController::has(const QString &token) const {
    return m_jobs.contains(token);
}

QString FileOperationRecoveryController::reportPathFor(const QString &token) const {
    return m_jobs.value(token).reportPath;
}

OperationReport::Report FileOperationRecoveryController::reportFor(const QString &token) const {
    const auto it = m_jobs.constFind(token);
    if (it == m_jobs.cend()) {
        return OperationReport::Report();
    }
    bool ok = false;
    const OperationReport::Report fromDisk = OperationReport::loadReport(it->reportPath, &ok);
    return ok ? fromDisk : it->report;
}

QString FileOperationRecoveryController::writeFailureReport(const QString &token,
                                                            const QString &reason) {
    const auto it = m_jobs.constFind(token);
    if (it == m_jobs.cend()) {
        return QString();
    }
    OperationReport::Report report = reportFor(token);
    if (report.id.isEmpty()) {
        report = it->report;
    }
    if (!reason.isEmpty()) {
        report.failureReason = reason;
    }
    const QString path = OperationReport::writeFailureText(it->reportPath, report);
    if (path.isEmpty()) {
        return QString();
    }
    log(QCoreApplication::translate("FileOperationRecoveryController", "未能完成全部文件操作，原因报告：%1").arg(path));
    return path;
}

QStringList FileOperationRecoveryController::cleanupStaleStaging(const QString &dataRoot) {
    return OperationReport::cleanupOwnedStaging(dataRoot);
}

bool FileOperationRecoveryController::resume(const QString &token) {
    const auto it = m_jobs.constFind(token);
    if (it == m_jobs.cend()) {
        InlineMessage::warning(m_overlay, tr("继续未完成操作"),
                               tr("原操作的恢复记录已清除。请重新发起操作。"));
        return false;
    }
    // 已经在续做的那一份（连点两下那颗按钮）：**不许**再开一条。两条续做会共用同一个
    // stagingDir 和目标文件，世界导入 / 批量复制都会被写坏（见 `m_resuming`）。
    if (m_resuming.contains(token)) {
        InlineMessage::information(m_overlay, tr("继续未完成操作"),
                                   tr("此项正在继续，请等待完成后查看结果。"));
        return false;
    }
    OperationReport::Report report = reportFor(token);
    if (!report.hasUnfinished()) {
        InlineMessage::information(m_overlay, tr("继续未完成操作"),
                                   tr("「%1」已无未完成项。").arg(report.title));
        return false;
    }
    // 实例忙（游戏在跑 / 另一项文件操作占着）时先不动盘：写一份原因报告比"试一下又失败"更有用。
    if (!report.instanceId.isEmpty() && m_host.isInstanceBusy && m_host.isInstanceBusy(report.instanceId)) {
        const QString reason = tr("「%1」正在运行或正被另一项文件操作占用，请等待其结束后再选择「继续未完成操作」。")
                                   .arg(m_host.displayName ? m_host.displayName(report.instanceId)
                                                           : report.instanceId);
        const QString failure = writeFailureReport(token, reason);
        InlineMessage::warning(m_overlay, tr("继续未完成操作"),
                               reason + (failure.isEmpty()
                                             ? QString()
                                             : tr("\n\n原因报告：\n%1").arg(QDir::toNativeSeparators(failure))));
        return false;
    }
    const QString title = tr("继续：%1").arg(report.title);
    const Tasks::TaskId task = m_host.beginTask
        ? m_host.beginTask(Tasks::TaskKind::Other, title, report.instanceId,
                           {step(tr("继续未完成的文件操作"), Tasks::TaskStepState::Running)})
        : Tasks::invalidTaskId();
    if (task == Tasks::invalidTaskId()) {
        return false;
    }
    Job job = *it;
    job.report = report;
    m_resuming.insert(token);
    runResume(job, task);
    return true;
}

void FileOperationRecoveryController::runResume(const Job &job, Tasks::TaskId task) {
    if (m_host.setTaskDetail) m_host.setTaskDetail(task, tr("正在继续未完成的文件操作…"));
    QString problem;
    bool started = false;
    switch (job.report.kind) {
    case OperationReport::Kind::WorldImport:
        started = resumeWorldImport(job, task, &problem);
        break;
    case OperationReport::Kind::InstanceTransfer:
        started = resumeTransfer(job, task, &problem);
        break;
    case OperationReport::Kind::FileCopy:
    case OperationReport::Kind::Batch:
        started = resumeFileCopy(job, task, &problem);
        break;
    case OperationReport::Kind::ModReplacement:
        // 适配替换的续做入口是重新开始那次实例更新（下载结果与预览版本必须一致）；
        // 这里只负责把原因说清楚，不假装能单独补下。
        problem = tr("模组适配替换不能单独续做；请重新打开实例更新向导，重新检查后执行。");
        break;
    }
    if (!started) {
        // 连"开始补做"都做不到：照实收失败，并把原因写进报告文件。
        m_resuming.remove(job.report.id);
        OperationReport::Report report = job.report;
        report.failureReason = problem;
        report.updatedAt = QDateTime::currentDateTime();
        QString saveProblem;
        OperationReport::saveReport(job.reportPath, report, &saveProblem);
        m_jobs[job.report.id] = Job{job.reportPath, job.dataRoot, report};
        if (m_host.finishTask) m_host.finishTask(task, false, problem);
        if (m_host.setTaskRecovery) m_host.setTaskRecovery(task, report.id);
        const QString failure = writeFailureReport(report.id, problem);
        InlineMessage::warning(m_overlay, tr("继续未完成操作"),
                               problem + (failure.isEmpty()
                                              ? QString()
                                              : tr("\n\n原因报告：\n%1").arg(QDir::toNativeSeparators(failure))));
        if (m_host.log) m_host.log(problem);
    }
}

bool FileOperationRecoveryController::resumeWorldImport(const Job &job, Tasks::TaskId task,
                                                        QString *problem) {
    const QJsonObject context = job.report.context;
    const QString worldDir = contextString(context, kContextWorldDir);
    const QString savesDir = contextString(context, kContextSavesDir);
    const QString name = contextString(context, kContextName);
    if (worldDir.isEmpty() || savesDir.isEmpty() || name.isEmpty()) {
        if (problem) *problem = tr("报告中缺少重新导入此世界所需信息（源、目标或名称缺失）。");
        return false;
    }
    auto report = std::make_shared<OperationReport::Report>(job.report);
    const QString reportPath = job.reportPath;
    const QString token = job.report.id;
    WorldFiles::ImportOptions options;
    options.conflictPolicy = report->conflictPolicy;
    options.stagingDir = report->stagingDir;
    if (options.stagingDir.isEmpty()) {
        // 世界导入的暂存放在 saves 里（同卷改名发布）；报告里没记时才用这条兜底。
        options.stagingDir = savesDir + QStringLiteral("/.wtfml-world-staging/") + token;
    }
    auto *watcher = new QFutureWatcher<WorldFiles::ImportResult>(this);
    connect(watcher, &QFutureWatcher<WorldFiles::ImportResult>::finished, this,
            [this, watcher, report, reportPath, token, task, name] {
                watcher->deleteLater();
                const auto result = watcher->result();
                report->stagingDir = result.stagingDir;
                report->totalFiles = qMax(report->totalFiles, result.totalFiles);
                report->totalBytes = qMax(report->totalBytes, result.totalBytes);
                report->doneFiles = result.copiedFiles + result.skippedFiles;
                report->skippedFiles = result.skippedFiles;
                report->doneBytes = result.copiedBytes;
                report->failedFiles = result.failedFiles;
                report->cancelled = result.cancelled;
                report->complete = result.ok;
                report->failureReason = result.problem;
                report->entries.clear();
                report->entries.append(entry(name,
                    result.ok ? (result.skipped ? OperationReport::EntryState::Skipped
                                                : OperationReport::EntryState::Done)
                              : OperationReport::EntryState::Failed,
                    result.problem));
                for (const QString &failure : result.failures) {
                    const int split = failure.indexOf(QCoreApplication::translate("FileOperationRecoveryController", "："));
                    report->entries.append(entry(
                        split > 0 ? failure.left(split) : failure,
                        OperationReport::EntryState::Failed,
                        split > 0 ? failure.mid(split + 1) : QString()));
                }
                settle(task, *report, reportPath, result.ok, result.cancelled, result.problem);
                if (m_host.refreshInstances) m_host.refreshInstances();
            });
    watcher->setFuture(QtConcurrent::run([worldDir, savesDir, name, options] {
        return WorldFiles::importWorld(worldDir, savesDir, name, options);
    }));
    return true;
}

bool FileOperationRecoveryController::resumeTransfer(const Job &job, Tasks::TaskId task,
                                                     QString *problem) {
    const QJsonObject context = job.report.context;
    const QString sourceDir = contextString(context, kContextSourceDir);
    const QString targetDir = contextString(context, kContextTargetDir);
    QStringList keys;
    for (const QJsonValue &value : context.value(QLatin1String(kContextKeys)).toArray()) {
        keys.append(value.toString());
    }
    QList<TransferPlanner::Item> items;
    for (const QJsonValue &value : context.value(QLatin1String(kContextItems)).toArray()) {
        const QJsonObject object = value.toObject();
        TransferPlanner::Item item;
        item.key = object.value(QStringLiteral("key")).toString();
        item.relativePath = object.value(QStringLiteral("relativePath")).toString();
        item.isDirectory = object.value(QStringLiteral("isDirectory")).toBool(true);
        item.exists = true;
        item.label = item.key;
        items.append(item);
    }
    if (sourceDir.isEmpty() || targetDir.isEmpty() || items.isEmpty() || keys.isEmpty()) {
        if (problem) *problem = tr("报告中缺少继续迁移所需信息（源、目标或勾选项缺失）。");
        return false;
    }
    auto report = std::make_shared<OperationReport::Report>(job.report);
    const QString reportPath = job.reportPath;
    auto *watcher = new QFutureWatcher<TransferApply::Result>(this);
    connect(watcher, &QFutureWatcher<TransferApply::Result>::finished, this,
            [this, watcher, report, reportPath, task, items, keys] {
                watcher->deleteLater();
                const auto result = watcher->result();
                bool verified = false;
                QString verifyProblem;
                if (result.ok) {
                    const auto checks = TransferApply::verify(
                        report->sourceRoot, report->targetRoot, items, keys);
                    verified = checks.ok && checks.missingFiles == 0;
                    verifyProblem = checks.text();
                }
                report->doneFiles = result.copiedFiles;
                report->skippedFiles = result.skippedFiles;
                report->failedFiles = result.failedFiles;
                report->failureReason = result.failedFiles > 0 ? result.problem : verifyProblem;
                report->complete = result.ok && verified && result.failedFiles == 0;
                report->entries.clear();
                auto recordedKeys = QSet<QString>();
                for (const auto &line : result.failures) {
                    const int split = line.indexOf(QCoreApplication::translate("FileOperationRecoveryController", "："));
                    const QString key = split > 0 ? line.left(split) : line;
                    if (recordedKeys.contains(key)) continue;
                    recordedKeys.insert(key);
                    report->entries.append(entry(key, OperationReport::EntryState::Failed,
                                                 split > 0 ? line.mid(split + 1) : QString()));
                }
                for (const QString &missing : keys) {
                    if (!recordedKeys.contains(missing)) {
                        report->entries.append(entry(
                            missing, report->complete ? OperationReport::EntryState::Done
                                                      : OperationReport::EntryState::Pending));
                    }
                }
                settle(task, *report, reportPath, report->complete, false,
                       report->failureReason);
                if (m_host.refreshInstances) m_host.refreshInstances();
            });
    watcher->setFuture(QtConcurrent::run([sourceDir, targetDir, items, keys] {
        return TransferApply::apply(sourceDir, targetDir, items, keys);
    }));
    return true;
}

bool FileOperationRecoveryController::resumeFileCopy(const Job &job, Tasks::TaskId task,
                                                     QString *problem) {
    const QJsonObject context = job.report.context;
    const QString sourcePath = contextString(context, kContextSourcePath);
    const QString targetPath = contextString(context, kContextTargetPath);
    if (sourcePath.isEmpty() || targetPath.isEmpty()) {
        if (problem) *problem = tr("报告中缺少重新复制此文件所需信息（源或目标缺失）。");
        return false;
    }
    auto report = std::make_shared<OperationReport::Report>(job.report);
    const QString reportPath = job.reportPath;
    const QString relative = report->entries.isEmpty() ? QFileInfo(targetPath).fileName()
                                                       : report->entries.constFirst().path;
    const auto policy = report->conflictPolicy;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, report, reportPath, task, relative] {
                watcher->deleteLater();
                const QString errorText = watcher->result();
                const bool ok = errorText.isEmpty();
                report->failedFiles = ok ? 0 : 1;
                report->doneFiles = ok ? 1 : 0;
                report->complete = ok;
                report->failureReason = errorText;
                report->entries.clear();
                report->entries.append(entry(relative,
                    ok ? OperationReport::EntryState::Done : OperationReport::EntryState::Failed,
                    errorText));
                settle(task, *report, reportPath, ok, false, errorText);
            });
    watcher->setFuture(QtConcurrent::run([sourcePath, targetPath, policy]() -> QString {
        const QFileInfo target(targetPath);
        if (target.exists()) {
            if (policy == OperationReport::ConflictPolicy::SkipExisting) {
                return QString();
            }
            if (policy == OperationReport::ConflictPolicy::Cancel) {
                return QCoreApplication::translate("FileOperationRecoveryController", "目标已存在；按当时选择的「取消」，未覆盖。");
            }
        }
        QString problem;
        if (!copyFileAtomically(sourcePath, targetPath, &problem)) {
            return problem;
        }
        return QString();
    }));
    return true;
}

void FileOperationRecoveryController::settle(Tasks::TaskId task, OperationReport::Report &report,
                                             const QString &reportPath, bool ok, bool cancelled,
                                             const QString &problem) {
    // 这一份不在"正在续做"里了 —— 成功 / 失败 / 取消三种收场都走这儿（R5）。
    m_resuming.remove(report.id);
    report.updatedAt = QDateTime::currentDateTime();
    report.cancelled = cancelled;
    report.complete = ok;
    if (!ok && !problem.isEmpty()) {
        report.failureReason = problem;
    }
    QString saveProblem;
    if (!OperationReport::saveReport(reportPath, report, &saveProblem) && m_host.log) {
        m_host.log(tr("无法将继续执行的结果写入报告：%1").arg(saveProblem));
    }
    const QString token = report.id;
    m_jobs[token] = Job{reportPath, m_jobs.value(token).dataRoot, report};
    if (ok) {
        if (m_host.setTaskDetail) m_host.setTaskDetail(task, report.summaryText());
        if (m_host.finishTask) m_host.finishTask(task, true, report.summaryText());
        OperationReport::removeReport(reportPath);
        m_jobs.remove(token);
        // 原来那条失败任务上的「继续未完成操作」**要摘掉**：报告和 job 都没了，那颗按钮
        // 再点只会得到"现场已经不在"，而屏幕上看起来像是这一件没做完。
        if (m_host.clearTaskRecovery) m_host.clearTaskRecovery(token);
        if (m_host.log) m_host.log(tr("已完成剩余文件操作：%1").arg(report.title));
        return;
    }
    if (cancelled) {
        if (m_host.setTaskDetail) m_host.setTaskDetail(task, tr("已取消，未发布的暂存保留着。"));
        if (m_host.finishTask) m_host.finishTask(task, false, tr("已取消，未完成项将保留。"));
        // 恢复入口只能挂在终态任务上。
        if (m_host.setTaskRecovery) m_host.setTaskRecovery(task, token);
        return;
    }
    if (m_host.setTaskDetail) m_host.setTaskDetail(task, report.summaryText());
    if (m_host.finishTask) m_host.finishTask(task, false, problem);
    if (m_host.setTaskRecovery) m_host.setTaskRecovery(task, token);
    const QString failure = writeFailureReport(token, problem);
    if (m_host.log) {
        m_host.log(tr("仍未完成全部文件操作：%1%2")
                       .arg(problem, failure.isEmpty()
                                        ? QString()
                                        : QStringLiteral("\n%1").arg(failure)));
    }
}
