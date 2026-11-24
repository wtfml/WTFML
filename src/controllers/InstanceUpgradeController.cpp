#include "InstanceTransferController.hpp"
#include <QCoreApplication>
#include "InstancePagesController.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "operations/InstanceUpgrade.hpp"
#include "util/AppPaths.hpp"
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>

void InstanceTransferController::upgradeInPlace(QString sourceId, QString targetId,
    std::function<void(bool, const QString &)> whenDone,
    const QStringList &reviewedKeptMods, bool reviewedModPlan,
    Tasks::TaskId parentTask, const QHash<int, int> &stageParentSteps,
    std::function<void(bool, bool)> whenSettled,
    const QList<InstanceUpgrade::ModReplacement> &replacements) {
    const auto fail = [&](const QString &problem) {
        InlineMessage::warning(m_overlay, tr("原地升级"), problem);
        if (whenSettled) whenSettled(false, false);
        if (whenDone) whenDone(false, problem);
    };
    if (m_copyBusy || sourceId == targetId
        || !InstanceFiles::validateDirName(sourceId).isEmpty()
        || !InstanceFiles::validateDirName(targetId).isEmpty()) {
        fail(tr("实例名无效、目标就是原实例，或已有文件任务正在运行。")); return;
    }
    const QString game = AppPaths::minecraftDir();
    if (sourceId.compare(m_versions->instanceGameVersion(sourceId), Qt::CaseInsensitive) == 0) {
        fail(tr("当前目录名是共享的原版版本号。为避免后续安装器把它当基础核心重新写入，请先复制为自定义名称的独立实例，再执行原地升级。"));
        return;
    }
    const QString dependent = InstanceUpgrade::dependentInstanceProblem(game, sourceId);
    if (!dependent.isEmpty()) { fail(dependent); return; }
    m_versions->clearVersionJsonCache();
    const QString inheritance = m_versions->inheritanceProblem(targetId);
    if (!inheritance.isEmpty()) { fail(inheritance); return; }
    InstanceUpgrade::Core core;
    core.mergedJson = m_versions->getVersionJson(targetId);
    core.clientJar = InstanceFiles::jarPath(targetId);
    core.gameVersion = m_versions->instanceGameVersion(targetId);
    const auto target = m_versions->displayParts(targetId);
    core.loaderName = InstancePagesController::loaderName(target, m_versions, &core.loaderVersion);
    QStringList locked{sourceId, targetId, InstanceFiles::jarOwnerId(targetId)};
    locked.removeAll(QString());
    locked.removeDuplicates();
    const auto unavailable = [&] {
        if (m_copyBusy) return true;
        for (const auto &id : locked)
            if ((m_launcher && m_launcher->isVersionRunning(id))
                || (m_host.isBeingModified && m_host.isBeingModified(id))) return true;
        return false;
    };
    if (unavailable()) { fail(tr("相关实例正在运行、安装或处理文件，完成后再试。")); return; }
    if (core.mergedJson.isEmpty() || core.gameVersion.isEmpty() || !QFileInfo(core.clientJar).isFile()) {
        fail(tr("目标核心不完整，请先安装或修复目标版本。")); return;
    }
    const auto source = m_versions->displayParts(sourceId);
    QString sourceLoaderVersion;
    const QString sourceLoader = InstancePagesController::loaderName(source, m_versions, &sourceLoaderVersion);
    const bool contextChanged = m_versions->instanceGameVersion(sourceId) != core.gameVersion
        || sourceLoader.compare(core.loaderName, Qt::CaseInsensitive) != 0
        || sourceLoaderVersion != core.loaderVersion;
    core.replacements = replacements;
    if (reviewedModPlan) {
        core.disableOldMods = true;
        core.keepOldMods = reviewedKeptMods;
    } else if (contextChanged && QFileInfo(AppPaths::versionDir(sourceId) + "/mods").isDir()) {
        const int choice = InlineMessage::ask(m_overlay, MessageKind::Question, tr("旧模组处理"),
            tr("游戏版本或加载器将变更，原有模组可能不兼容。\n\n建议禁用原有模组，文件将以 .disabled 后缀保留。"),
            {tr("禁用旧模组"), tr("保持原状"), tr("取消升级")}, 0, -1, false);
        if (choice != 0 && choice != 1) {
            if (whenSettled) whenSettled(false, true);
            if (whenDone) whenDone(false, {});
            return;
        }
        core.disableOldMods = choice == 0;
    } else {
        core.disableOldMods = false;
    }
    if (!InlineMessage::question(m_overlay, tr("原地升级"),
        tr("将「%1」的游戏核心替换为 %2 / %3。\n\n本次升级保留当前实例的存档、截图、配置和其他用户文件。替换前将创建并校验快照，原目录可通过「回滚原地升级」恢复。\n\n请保存设置后再升级。确认继续？")
            .arg(displayName(sourceId), core.gameVersion, core.loaderName.isEmpty() ? tr("原版") : core.loaderName),
        tr("备份并升级"), tr("取消"), false, true)) {
        if (whenSettled) whenSettled(false, true);
        if (whenDone) whenDone(false, {});
        return;
    }
    // Questions run nested event loops; recheck every lock immediately before starting.
    if (unavailable() || AppPaths::minecraftDir() != game) { fail(tr("实例或游戏目录状态已变化，没有开始升级。")); return; }
    const InstanceUpgrade::Request request{game, sourceId,
        QUuid::createUuid().toString(QUuid::WithoutBraces), displayName(sourceId), -1, {}, {}};
    // 引擎阶段（`Stage` 的枚举值）⇒ 子任务第几步。原地升级那七步与枚举前七个
    // 一一对应，所以不传也能用；父任务那一份由调用方给（它自己的步骤更粗）。
    runUpgradeJob(request, core, locked, false, std::move(whenDone), parentTask,
                  stageParentSteps, QString(), std::move(whenSettled));
}

void InstanceTransferController::upgradeToCopy(const QString &sourceId, const QString &newName,
    const QString &targetCoreId,
    std::function<void(bool, const QString &)> whenDone,
    const QStringList &reviewedKeptMods, bool reviewedModPlan,
    Tasks::TaskId parentTask, const QHash<int, int> &stageParentSteps,
    std::function<void(bool, bool)> whenSettled,
    const QList<InstanceUpgrade::ModReplacement> &replacements,
    const QStringList &copyRoots) {
    const auto fail = [&](const QString &problem) {
        InlineMessage::warning(m_overlay, tr("创建更新副本"), problem);
        if (whenSettled) whenSettled(false, false);
        if (whenDone) whenDone(false, problem);
    };
    const QString name = newName.trimmed();
    if (m_copyBusy || !InstanceFiles::validateDirName(sourceId).isEmpty()
        || !InstanceFiles::validateDirName(name).isEmpty()
        || sourceId.compare(name, Qt::CaseInsensitive) == 0) {
        fail(tr("实例名无效、副本名与原实例相同，或已有文件任务正在运行。"));
        return;
    }
    if (QFileInfo::exists(AppPaths::versionDir(name))) {
        fail(tr("实例名称已存在，未覆盖文件。请更换副本名称。"));
        return;
    }
    const QString game = AppPaths::minecraftDir();
    if (sourceId.compare(m_versions->instanceGameVersion(sourceId), Qt::CaseInsensitive) == 0) {
        // 共享原版核心不能原地替换；副本这条路本来安全（源不动），但为了让副本不继续
        // 继承"目录名就是原版版本号"的身份，这里仍要求先复制成自定义名称的独立实例。
        fail(tr("当前目录名是共享的原版版本号。请先复制为自定义名称的独立实例，再创建更新副本。"));
        return;
    }
    if ((m_launcher && m_launcher->isVersionRunning(sourceId))
        || (m_host.isBeingModified && m_host.isBeingModified(sourceId))) {
        fail(tr("源实例正在运行、安装或处理文件，完成后再试。"));
        return;
    }
    m_versions->clearVersionJsonCache();
    const QString inheritance = m_versions->inheritanceProblem(targetCoreId);
    if (!inheritance.isEmpty()) { fail(inheritance); return; }
    InstanceUpgrade::Core core;
    core.mergedJson = m_versions->getVersionJson(targetCoreId);
    core.clientJar = InstanceFiles::jarPath(targetCoreId);
    core.gameVersion = m_versions->instanceGameVersion(targetCoreId);
    const auto target = m_versions->displayParts(targetCoreId);
    core.loaderName = InstancePagesController::loaderName(target, m_versions, &core.loaderVersion);
    if (core.mergedJson.isEmpty() || core.gameVersion.isEmpty() || !QFileInfo(core.clientJar).isFile()) {
        fail(tr("目标核心不完整，请先安装或修复目标版本。"));
        return;
    }
    core.replacements = replacements;
    if (reviewedModPlan) {
        core.disableOldMods = true;
        core.keepOldMods = reviewedKeptMods;
    } else {
        // 未经过逐文件确认的调用（兼容入口）只能保守处理：旧模组一律禁用、保留文件。
        core.disableOldMods = true;
    }
    const auto unavailable = [&] {
        if (m_copyBusy) return true;
        for (const auto &id : QStringList{sourceId, targetCoreId, InstanceFiles::jarOwnerId(targetCoreId)})
            if (!id.isEmpty()
                && ((m_launcher && m_launcher->isVersionRunning(id))
                    || (m_host.isBeingModified && m_host.isBeingModified(id)))) return true;
        return false;
    };
    if (unavailable() || AppPaths::minecraftDir() != game) {
        fail(tr("实例或游戏目录状态已变化，没有开始创建副本。"));
        return;
    }
    InstanceUpgrade::Request request{game, name,
        QUuid::createUuid().toString(QUuid::WithoutBraces), displayName(sourceId), -1, {}, {}};
    request.copyRoots = copyRoots;
    request.sourceInstanceId = sourceId;
    const QString sourceDir = AppPaths::versionDir(sourceId);
    runUpgradeJob(request, core,
                  QStringList{sourceId, targetCoreId, InstanceFiles::jarOwnerId(targetCoreId)},
                  false, std::move(whenDone), parentTask, stageParentSteps, sourceDir,
                  std::move(whenSettled));
}

void InstanceTransferController::rollbackUpgrade(QString sourceId,
    std::function<void(bool, const QString &)> whenDone) {
    const auto abort = [&](const QString &problem) {
        InlineMessage::warning(m_overlay, tr("回滚原地升级"), problem);
        if (whenDone) whenDone(false, problem);
    };
    if (!InstanceFiles::validateDirName(sourceId).isEmpty() || m_copyBusy
        || (m_launcher && m_launcher->isVersionRunning(sourceId))
        || (m_host.isBeingModified && m_host.isBeingModified(sourceId))) {
        abort(tr("实例正在运行或处理文件，未开始回滚。")); return;
    }
    const QString game = AppPaths::minecraftDir();
    const auto ids = InstanceUpgrade::recoverableTransactions(game, sourceId);
    if (ids.isEmpty()) { abort(tr("没有可回滚的原地升级记录。普通快照请使用快照恢复。")); return; }
    // Most recent retained transaction; after rolling it back the preceding one remains available.
    const InstanceUpgrade::Request request{game, sourceId, ids.first(), displayName(sourceId), -1, {}, {}};
    if (!InlineMessage::question(m_overlay, tr("回滚原地升级"),
        tr("恢复「%1」最近一次原地升级前的完整目录。\n\n当前版本也会另行保留，不删除文件。升级之后的新存档进度不会合并到旧目录；如需保留，可从事务目录中的 replaced 取回。")
            .arg(displayName(sourceId)), tr("恢复原实例"), tr("取消"), false, true)) {
        if (whenDone) whenDone(false, {});
        return;
    }
    if (m_copyBusy || (m_launcher && m_launcher->isVersionRunning(sourceId))
        || (m_host.isBeingModified && m_host.isBeingModified(sourceId))
        || AppPaths::minecraftDir() != game) { abort(tr("实例或游戏目录状态已变化，未开始回滚。")); return; }
    runUpgradeJob(request, {}, {sourceId}, true, std::move(whenDone));
}

void InstanceTransferController::runUpgradeJob(const InstanceUpgrade::Request &request,
    const InstanceUpgrade::Core &core, const QStringList &lockedIds, bool restore,
    std::function<void(bool, const QString &)> whenDone, Tasks::TaskId parentTask,
    const QHash<int, int> &stageParentSteps, const QString &copySourceDir,
    std::function<void(bool, bool)> whenSettled) {
    m_copyBusy = true;
    for (const auto &id : lockedIds) if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(id, true);
    const bool copyMode = !copySourceDir.isEmpty();
    const QString title = restore ? tr("回滚原地升级 %1").arg(displayName(request.instanceId))
        : copyMode ? tr("创建更新副本 %1").arg(request.instanceId)
                   : tr("原地升级 %1").arg(displayName(request.instanceId));
    QList<Tasks::TaskStep> steps;
    const QStringList names = restore
        ? QStringList{tr("验证保留的原目录"), tr("保留当前版本并恢复原实例"), tr("完成")}
        : copyMode
        ? QStringList{tr("复制源实例到暂存副本"), tr("逐文件校验暂存副本"),
                      tr("应用新核心并发布"), tr("发布后逐文件校验"), tr("完成")}
        : QStringList{tr("准备并校验用户文件副本"), tr("创建完整快照"), tr("验证完整快照"),
                      tr("保留原目录"), tr("替换核心"), tr("逐文件校验"), tr("完成")};
    for (const QString &name : names)
        steps.append({name, Tasks::TaskStepState::Pending, {}});
    const int stepCount = steps.size();
    // 引擎阶段（`Stage` 的枚举值）⇒ 子任务第几步。默认那份是按原地升级那七步写的；
    // 副本模式只有五步，顺序也不同（见上）。
    QHash<int, int> stageSteps = restore
        ? QHash<int, int>{{int(InstanceUpgrade::Stage::Checking), 0}}
        : copyMode
        ? QHash<int, int>{{int(InstanceUpgrade::Stage::Checking), 0},
                          {int(InstanceUpgrade::Stage::Copy), 0},
                          {int(InstanceUpgrade::Stage::VerifyCopy), 1},
                          {int(InstanceUpgrade::Stage::Publish), 2},
                          {int(InstanceUpgrade::Stage::Verify), 3},
                          {int(InstanceUpgrade::Stage::Done), 3}}
        : QHash<int, int>{{int(InstanceUpgrade::Stage::Checking), 0},
                          {int(InstanceUpgrade::Stage::Snapshot), 1},
                          {int(InstanceUpgrade::Stage::VerifySnapshot), 2},
                          {int(InstanceUpgrade::Stage::RetainOriginal), 3},
                          {int(InstanceUpgrade::Stage::Publish), 4},
                          {int(InstanceUpgrade::Stage::Verify), 5},
                          {int(InstanceUpgrade::Stage::Done), 6}};
    const auto task = m_host.beginChildTask && parentTask != Tasks::invalidTaskId()
        ? m_host.beginChildTask(parentTask, Tasks::TaskKind::Other, title, request.instanceId, steps)
        : m_host.beginTask ? m_host.beginTask(Tasks::TaskKind::Other, title, request.instanceId, steps)
                           : Tasks::invalidTaskId();
    // 升级 / 回滚在引擎里每一步之间都看取消位（准备、快照、校验、发布前后），所以
    // 这里把「取消」开出来。`Other` 的默认能力位是关的 —— 光靠种类判断，这一项
    // 永远长不出那颗按钮。
    const auto state = std::make_shared<CancelState>();
    if (task != Tasks::invalidTaskId()) m_cancelStates.insert(task, state);
    if (m_host.setTaskCapabilities) {
        Tasks::TaskCapabilities caps;
        caps.canCancel = true;
        m_host.setTaskCapabilities(task, caps);
    }
    const auto stage = std::make_shared<std::atomic<int>>(0);
    auto *watcher = new QFutureWatcher<InstanceUpgrade::Result>(this);
    auto *timer = new QTimer(watcher);
    connect(timer, &QTimer::timeout, watcher, [this, stage, task, restore, stepCount, stageSteps,
                                               parentTask, stageParentSteps, copyMode] {
        const int current = stage->load();
        const int step = stageSteps.value(current, -1);
        if (m_host.setTaskDetail) m_host.setTaskDetail(task,
            restore ? tr("正在恢复保留的原目录…")
                    : copyMode ? tr("创建更新副本：阶段 %1 / %2").arg(qMax(0, step) + 1).arg(stepCount)
                               : tr("原地升级：阶段 %1 / %2").arg(qMax(0, step) + 1).arg(stepCount));
        if (m_host.setTaskProgress) m_host.setTaskProgress(task, qMax(0, step), stepCount);
        if (m_host.setTaskStepState) {
            for (int i = 0; i < qMax(0, step); ++i) m_host.setTaskStepState(task, i, Tasks::TaskStepState::Done);
            if (step >= 0) m_host.setTaskStepState(task, step, Tasks::TaskStepState::Running);
        }
        // 父任务那一条线：只改它自己那几步的记号（任务的终态由串起这些步骤的流程决定）。
        const int parentStep = stageParentSteps.value(current, -1);
        if (parentTask != Tasks::invalidTaskId() && parentStep >= 0 && m_host.setTaskStepState) {
            m_host.setTaskStepState(parentTask, parentStep, Tasks::TaskStepState::Running);
        }
    });
    connect(watcher, &QFutureWatcher<InstanceUpgrade::Result>::finished, this,
        [this, watcher, timer, lockedIds, task, stage, title, request, whenDone, stepCount,
         stageSteps, parentTask, stageParentSteps, copyMode,
         whenSettled = std::move(whenSettled)] {
            timer->stop();
            const auto result = watcher->result();
            watcher->deleteLater();
            if (task != Tasks::invalidTaskId()) m_cancelStates.remove(task);
            for (const auto &id : lockedIds) if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(id, false);
            m_copyBusy = false;
            m_versions->clearVersionJsonCache();
            if (m_host.refreshInstances) m_host.refreshInstances();
            // 「用户按了取消」只在**真的没做成**时才算数：按下的那一瞬间引擎可能正好
            // 走完了最后一个安全点、升级已经成功 —— 该报成功（盘上确实换好了）。
            // 按过取消不代表引擎安全取消；并发 I/O 失败或回滚失败不能被它掩盖。
            const bool cancelled = result.cancelled && !result.ok;
            QString detail = result.problem;
            if (result.ok) {
                detail = tr("已完成。禁用旧模组 %1 个。恢复材料：%2")
                             .arg(result.disabledMods).arg(InstanceUpgrade::transactionDir(request));
                if (!result.changes.isEmpty()) {
                    detail += QCoreApplication::translate("InstanceUpgradeController", "\n文件变更：\n") + result.changes.join("\n");
                }
            }
            if (cancelled && detail.isEmpty()) {
                detail = tr("已取消：原实例没有发布新核心（已经动过的那部分会自动恢复）。");
            }
            if (!result.snapshotZip.isEmpty()) detail += tr("\n完整快照：%1").arg(result.snapshotZip);
            if (m_host.setTaskDetail) m_host.setTaskDetail(task, detail);
            if (!cancelled && m_host.setTaskStepState) {
                if (result.ok) for (int i = 0; i < stepCount; ++i) m_host.setTaskStepState(task, i, Tasks::TaskStepState::Done);
                else {
                    const int failed = stageSteps.value(stage->load(), -1);
                    if (failed >= 0) {
                        m_host.setTaskStepState(task, failed, Tasks::TaskStepState::Failed);
                    }
                }
            }
            if (m_host.setTaskProgress && result.ok) m_host.setTaskProgress(task, stepCount, stepCount);
            // 父任务：这一步的记号 + 那句进度说明。**不在这儿收尾** —— 父任务还可能有
            // 下一步（副本模式准备完核心还要继续），由调用方按整趟结果决定成败。
            if (parentTask != Tasks::invalidTaskId() && m_host.setTaskStepState) {
                const int parentStep = stageParentSteps.value(stage->load(), -1);
                if (result.ok) {
                    QSet<int> done;
                    for (auto it = stageParentSteps.cbegin(); it != stageParentSteps.cend(); ++it) {
                        if (it.value() >= 0) done.insert(it.value());
                    }
                    for (const int parentDoneStep : done) {
                        m_host.setTaskStepState(parentTask, parentDoneStep,
                                                Tasks::TaskStepState::Done);
                    }
                } else if (!cancelled && parentStep >= 0) {
                    m_host.setTaskStepState(parentTask, parentStep, Tasks::TaskStepState::Failed);
                }
                if (m_host.setTaskDetail) m_host.setTaskDetail(parentTask, detail);
            }
            if (cancelled) {
                // 取消不是失败：账本收成 Cancelled（不亮红点），也不弹失败提示。
                if (m_host.markTaskCancelled) m_host.markTaskCancelled(task);
                else if (m_host.finishTask) m_host.finishTask(task, false, detail);
                log(title + '\n' + detail);
            } else {
                if (m_host.finishTask) m_host.finishTask(task, result.ok, detail);
                log(title + '\n' + detail);
            }
            // Close stale settings before showing results, so it cannot write old fields back.
            if (whenSettled) whenSettled(result.ok, cancelled);
            const QPointer<InstanceTransferController> guard(this);
            if (whenDone) {
                whenDone(result.requiresReload || result.rolledBack || result.ok
                             || (!cancelled && result.rollbackAvailable),
                         detail);
            }
            if (!guard) return;
            if (m_overlay && !m_overlay->promptOpen()) {
                if (result.ok) InlineMessage::information(m_overlay, title, detail);
                else if (cancelled) InlineMessage::information(m_overlay, title, detail);
                else InlineMessage::critical(m_overlay, title, detail);
            }
        });
    timer->start(100);
    watcher->setFuture(QtConcurrent::run([request, core, restore, stage, state, copySourceDir] {
        const auto cancelled = [state] {
            return state->abort.load(std::memory_order_relaxed);
        };
        try {
            if (restore) return InstanceUpgrade::rollback(request, cancelled);
            if (!copySourceDir.isEmpty()) {
                return InstanceUpgrade::prepareCopyAndCommit(
                    request, core, copySourceDir,
                    [stage](InstanceUpgrade::Stage value) {
                        stage->store(static_cast<int>(value));
                    }, cancelled);
            }
            return InstanceUpgrade::prepareAndCommit(request, core, [stage](InstanceUpgrade::Stage value) {
                stage->store(static_cast<int>(value));
            }, cancelled);
        } catch (...) {
            InstanceUpgrade::Result result;
            result.requiresReload = true;
            result.problem = QCoreApplication::translate("InstanceUpgradeController", "升级工作发生异常；恢复检查：")
                + InstanceUpgrade::recoverInterrupted(request.gameDir).join('\n');
            return result;
        }
    }));
}

