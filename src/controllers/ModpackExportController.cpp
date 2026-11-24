#include "ModpackExportController.hpp"
#include <QCoreApplication>
#include "TextMarks.hpp"

#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <utility>

ModpackExportController::ModpackExportController(VersionManager *versions,
                                                 LauncherSettings *settings,
                                                 OverlayHost *overlay, Host host,
                                                 QObject *parent)
    : QObject(parent), m_versions(versions), m_settings(settings), m_overlay(overlay),
      m_host(std::move(host)) {}

QString ModpackExportController::displayName(const QString &id) const {
    return m_host.displayName ? m_host.displayName(id) : id;
}
void ModpackExportController::log(const QString &text) const {
    if (m_host.log) m_host.log(text);
}
void ModpackExportController::setStatus(const QString &text) const {
    if (m_host.setStatus) m_host.setStatus(text);
}

QList<Tasks::TaskStep> ModpackExportController::initialSteps() const {
    // 四步就对着这条流程（规划 → 兼容性确认 → 执行 → 验证）。第一步起手就是
    // Running —— 任务记出来的那一刻分析已经在路上了。
    const QStringList names{
        tr("规划：扫描实例文件并反查资源出处"),
        tr("兼容性确认：清单不完整时问一句"),
        tr("执行：写入整合包"),
        tr("验证：核对写出来的文件"),
    };
    QList<Tasks::TaskStep> steps;
    steps.reserve(names.size());
    for (int i = 0; i < names.size(); ++i) {
        Tasks::TaskStep step;
        step.name = names.at(i);
        step.state = i == 0 ? Tasks::TaskStepState::Running : Tasks::TaskStepState::Pending;
        steps.append(step);
    }
    return steps;
}

void ModpackExportController::setTaskDetail(const QString &text) const {
    if (m_host.setTaskDetail && m_taskId != Tasks::invalidTaskId()) {
        m_host.setTaskDetail(m_taskId, text);
    }
}
void ModpackExportController::setTaskProgress(qint64 doneUnits, qint64 totalUnits) const {
    if (m_host.setTaskProgress && m_taskId != Tasks::invalidTaskId()) {
        m_host.setTaskProgress(m_taskId, doneUnits, totalUnits);
    }
}
void ModpackExportController::setTaskStepState(int index, Tasks::TaskStepState state) const {
    if (m_host.setTaskStepState && m_taskId != Tasks::invalidTaskId()) {
        m_host.setTaskStepState(m_taskId, index, state);
    }
}
void ModpackExportController::finishTask(bool success, const QString &errorSummary) {
    if (m_taskId != Tasks::invalidTaskId()) {
        const Tasks::TaskId id = m_taskId;
        if (success) {
            setTaskProgress(4, 4);  // 最后一步的进度在收尾前补齐（表来自 initialSteps()）
        }
        m_taskId = Tasks::invalidTaskId();
        if (m_host.finishTask) m_host.finishTask(id, success, errorSummary);
    }
}
void ModpackExportController::cancelTask() {
    if (m_taskId != Tasks::invalidTaskId()) {
        const Tasks::TaskId id = m_taskId;
        m_taskId = Tasks::invalidTaskId();
        if (m_host.cancelTask) m_host.cancelTask(id);
    }
}

void ModpackExportController::exportPack(const QString &versionId,
                                         const QString &targetPath) {
    if (versionId.isEmpty() || targetPath.isEmpty()) return;
    if (m_busy) {
        InlineMessage::information(m_overlay, tr("正在导出"),
                                   tr("已经有一份整合包正在打包，等它完成后再开始下一份。"));
        return;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(versionId)) {
        InlineMessage::information(
            m_overlay, tr("暂时无法操作"),
            tr("「%1」的文件正在变动。等待安装、导入或检查完成后再导出。")
                .arg(displayName(versionId)));
        return;
    }
    const auto format = targetPath.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)
                            ? ModpackExport::Format::CurseForge
                            : ModpackExport::Format::Modrinth;
    const QString instanceDir = AppPaths::versionDir(versionId);
    if (!QFileInfo(instanceDir).isDir()) {
        log(tr("导出失败：找不到实例目录 %1").arg(instanceDir));
        InlineMessage::warning(m_overlay, tr("导出失败"),
                               tr("找不到这个实例的目录：\n%1")
                                   .arg(QDir::toNativeSeparators(instanceDir)));
        return;
    }
    const QString gameVersion = m_versions->instanceGameVersion(versionId);
    if (gameVersion.isEmpty()) {
        log(tr("无法导出 %1，因为未识别其原版游戏版本").arg(versionId));
        InlineMessage::warning(
            m_overlay, tr("无法识别原版游戏版本"),
            tr("整合包清单需要有效的原版游戏版本。此实例的版本记录缺失或无效。\n\n可重新安装所需版本后导出。"));
        return;
    }
    QString loaderKey;
    QString loaderVersion;
    const QString loaderProblem =
        m_versions->instanceLoaderInfo(versionId, &loaderKey, &loaderVersion);
    if (!loaderProblem.isEmpty()) {
        log(tr("无法导出 %1，因为未识别其加载器").arg(versionId));
        InlineMessage::warning(m_overlay, tr("此实例不符合整合包导出要求"), loaderProblem);
        return;
    }
    ModpackExport::ExportRequest request;
    request.format = format;
    request.instanceDir = instanceDir;
    request.name = displayName(versionId);
    request.version = QStringLiteral("1.0.0");
    request.gameVersion = gameVersion;
    request.loaderKey = loaderKey;
    request.loaderVersion = loaderVersion;
    log(tr("正在导出「%1」（%2）…")
            .arg(displayName(versionId),
                 format == ModpackExport::Format::CurseForge ? tr("CurseForge 规范")
                                                             : tr("Modrinth 规范")));
    m_busy = true;
    // 任务中心：先记一项，再由它推着这四步走。验证工程不接这几个
    // 回调也照样能导出 —— 任务中心只是同一段流程的镜子，不是新的执行路径。
    if (m_host.beginTask) {
        m_taskId = m_host.beginTask(Tasks::TaskKind::Export,
                                    tr("导出整合包 %1").arg(displayName(versionId)),
                                    versionId, initialSteps());
        setTaskProgress(0, initialSteps().size());
        setTaskDetail(tr("正在扫描实例文件…"));
    }
    setStatus(tr("正在后台分析整合包…"));
    const QString modrinthBase = m_settings->modrinthApiBaseOverride();
    const QString curseForgeKey = m_settings->curseForgeApiKey();
    const QString curseForgeBase = m_settings->curseForgeApiBaseOverride();
    const QPointer<ModpackExportController> self(this);
    auto throttle = std::make_shared<QElapsedTimer>();
    const ModpackExport::ExportProgress progress =
        [self, throttle](int done, int total, const QString &current) {
            if (!self || (throttle->isValid() && throttle->elapsed() < 100 && done < total)) return;
            throttle->start();
            QMetaObject::invokeMethod(self.data(), [self, done, total, current] {
                if (!self) return;
                const QString text = ModpackExportController::tr("正在后台分析整合包…（%1/%2）%3")
                                         .arg(done).arg(total)
                                         .arg(current.isEmpty()
                                                  ? QString()
                                                  : QFileInfo(current).fileName());
                self->setStatus(text);
                self->setTaskDetail(text);
            }, Qt::QueuedConnection);
        };
    auto *watcher = new QFutureWatcher<ModpackExport::ExportPlan>(this);
    connect(watcher, &QFutureWatcher<ModpackExport::ExportPlan>::finished, this,
            [this, watcher, targetPath, format] {
                const auto plan = watcher->result();
                watcher->deleteLater();
                setStatus({});
                continueExport(plan, targetPath, format);
            });
    watcher->setFuture(QtConcurrent::run(
        [request, progress, self, modrinthBase, curseForgeKey, curseForgeBase] {
            VersionManager worker;
            worker.setModrinthApiBaseOverride(modrinthBase);
            worker.setCurseForgeApiKey(curseForgeKey);
            worker.setCurseForgeApiBaseOverride(curseForgeBase);
            return worker.buildExportPlan(request, progress, [self] { return self.isNull(); });
        }));
}

void ModpackExportController::continueExport(const ModpackExport::ExportPlan &plan,
                                             const QString &targetPath,
                                             ModpackExport::Format format) {
    if (plan.cancelled) {
        m_busy = false;
        log(tr("导出已取消。"));
        cancelTask();
        return;
    }
    if (!plan.problem.isEmpty()) {
        m_busy = false;
        log(tr("导出失败：%1").arg(plan.problem));
        setTaskStepState(0, Tasks::TaskStepState::Failed);
        finishTask(false, plan.problem);
        InlineMessage::warning(m_overlay, tr("导出失败"), plan.problem);
        return;
    }
    // 规划走完了：进兼容性确认那一步。
    setTaskStepState(0, Tasks::TaskStepState::Done);
    setTaskStepState(1, Tasks::TaskStepState::Running);
    setTaskProgress(1, 4);
    setTaskDetail(tr("资源识别完成：已查询 %1 个文件，识别出 %2 个。")
                      .arg(plan.queried).arg(plan.matched));
    QStringList reasons;
    if (plan.lookupFailed) reasons.append(plan.lookupProblem);
    if (!ModpackExport::reimportable(plan.spec)) {
        reasons.append(tr("未识别可由平台下载的资源。生成的清单无法用于整合包导入。"));
    }
    if (!reasons.isEmpty()) {
        setTaskDetail(tr("部分文件的来源未知"));
        const int choice = InlineMessage::ask(
            m_overlay, MessageKind::Question, tr("继续导出吗"),
            tr("%1\n\n继续导出会保留来源未知的文件。导入后，启动器无法单独更新这些文件或去除重复项。")
                .arg(reasons.join(QStringLiteral("\n\n"))),
            {tr("继续导出"), tr("取消")}, 1, -1);
        if (choice != 0) {
            m_busy = false;
            log(tr("已取消导出。"));
            cancelTask();
            return;
        }
    }
    setTaskStepState(1, Tasks::TaskStepState::Done);
    setTaskStepState(2, Tasks::TaskStepState::Running);
    setTaskProgress(2, 4);
    setTaskDetail(tr("正在写入整合包…"));
    setStatus(tr("正在后台打包整合包…"));
    const QPointer<ModpackExportController> self(this);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, plan, targetPath, format] {
                const QString problem = watcher->result();
                watcher->deleteLater();
                m_busy = false;
                setStatus({});
                if (!problem.isEmpty()) {
                    log(tr("导出失败：%1").arg(problem));
                    setTaskStepState(2, Tasks::TaskStepState::Failed);
                    finishTask(false, problem);
                    InlineMessage::warning(m_overlay, tr("导出失败"), problem);
                    return;
                }
                // 验证那一步：打完不算完 —— 盘上那份得真的在、且像个 zip（开头是 `PK`）。
                setTaskStepState(2, Tasks::TaskStepState::Done);
                setTaskStepState(3, Tasks::TaskStepState::Running);
                setTaskProgress(3, 4);
                setTaskDetail(tr("正在核对写出来的文件…"));
                const QString verifyProblem = [] (const QString &path) {
                    const QFileInfo info(path);
                    if (!info.isFile()) return tr("打包已结束，但未找到输出文件：%1")
                                                   .arg(QDir::toNativeSeparators(path));
                    if (info.size() <= 0) return tr("写出来的整合包是空的：%1")
                                                   .arg(QDir::toNativeSeparators(path));
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly)) {
                        return tr("无法打开导出的文件：%1\n%2")
                            .arg(QDir::toNativeSeparators(path), file.errorString());
                    }
                    const QByteArray magic = file.read(2);
                    if (magic != QByteArrayLiteral("PK")) {
                        return tr("写出来的文件不是 zip（开头不是 PK）：%1")
                            .arg(QDir::toNativeSeparators(path));
                    }
                    return QString();
                }(targetPath);
                if (!verifyProblem.isEmpty()) {
                    log(tr("导出失败：%1").arg(verifyProblem));
                    setTaskStepState(3, Tasks::TaskStepState::Failed);
                    finishTask(false, verifyProblem);
                    InlineMessage::warning(m_overlay, tr("导出失败"), verifyProblem);
                    return;
                }
                setTaskStepState(3, Tasks::TaskStepState::Done);
                setTaskDetail(tr("已写入 %1").arg(QDir::toNativeSeparators(targetPath)));
                finishTask(true);
                const QString shown = QDir::toNativeSeparators(targetPath);
                log(TextMarks::done(tr("整合包已导出：%1").arg(shown)));
                QStringList lines{tr("已经写到：\n%1").arg(shown)};
                lines.append(tr("已识别 %1 个待检查文件中的 %2 个，并写入下载清单。其余 %3 个文件已包含在整合包中。")
                                 .arg(plan.queried).arg(plan.matched).arg(plan.overrides.size()));
                if (plan.notDistributable > 0)
                    lines.append(tr("有 %1 个文件禁止第三方从平台下载，已包含在整合包内。")
                                     .arg(plan.notDistributable));
                if (plan.keptLocalState > 0)
                    lines.append(tr("有 %1 个文件已重命名或禁用。导出时会保留这些文件的名称和启用状态。")
                                     .arg(plan.keptLocalState));
                if (plan.notDistributableUnknown > 0)
                    lines.append(tr("无法从 CurseForge 确认另外 %1 个文件的分发权限。文件已随包导出，导入后无法自动更新或去除重复项。")
                                     .arg(plan.notDistributableUnknown));
                if (!plan.hashFailures.isEmpty())
                    lines.append(tr("有 %1 个文件读取失败，无法计算摘要，已随整合包导出。")
                                     .arg(plan.hashFailures.size()));
                if (!plan.collect.skippedDirs.isEmpty())
                    lines.append(tr("未导出 %1（共 %2 个文件）。保留接收方原有存档和截图。")
                                     .arg(plan.collect.skippedDirs.join(tr("、")),
                                          QString::number(plan.collect.skippedFileCount)));
                lines.append(tr("这是 %1 的包：别人在启动器里用「导入整合包…」即可导入安装。")
                                 .arg(format == ModpackExport::Format::CurseForge
                                          ? tr("CurseForge 规范") : tr("Modrinth 规范")));
                InlineMessage::information(m_overlay, tr("导出完成"),
                                           lines.join(QStringLiteral("\n\n")));
            });
    watcher->setFuture(QtConcurrent::run([plan, targetPath, self] {
        VersionManager worker;
        return worker.writePackArchive(plan, targetPath, [self] { return self.isNull(); });
    }));
}
