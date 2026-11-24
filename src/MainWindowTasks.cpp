/**
 * @file MainWindowTasks.cpp
 * @brief 任务中心：账本在底栏和任务中心面板上的投影，以及"取消 / 重试 / 收尾"那几条动作。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片只认账本（`ActivityCenterController` 后面的 `Tasks::TaskManager`）和窗口上那两块
 * 投影（底栏中间那一格、任务中心面板），再加一条"这一批下载的账"（
 * `DownloadTaskLedgerController`）。它不碰启动、安装、实例树 —— 从 `MainWindow.cpp`
 * 分出来，"底栏那条摘要为什么是这一句"不必在整扇窗里翻。
 *
 * 【判据不在这一层】
 *
 * 收起态显示谁、优先级、刚完成那一项还露多久，在 `Tasks::collapsedSummary()`（那一层
 * 的唯一出处）；"按取消真的会做点什么吗"在 `cancelWouldWork()` 里，而它必须和
 * `onTaskDetailAction()` 里那条分派逐字对齐 —— 所以两条放在同一个文件里，改一条时另一条
 * 就在上面几行。
 *
 * 【一条规矩】
 *
 * 这里面一个汉字都不该进日志：进度不进日志（日志里那一份是给人事后排查的）。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\InstanceFilesPage.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/BackupController.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/TaskDetailController.hpp"
#include "controllers/ToolboxController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/LaunchController.hpp"
#include "download/DownloadManager.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionManager.hpp"

#include <QDateTime>
#include <QList>
#include <QProcess>

using namespace MainWindowViewData;

// ============================ 任务中心 ============================
//
// 这一小片是**产品侧通往账本的唯一一道门**：上面那些执行者（下载 / 版本 / 启动）的信号
// 在这里被翻成一次 `begin()` / `setProgress()` / `succeed()`，反过来底栏和任务中心面板
// 只读账本、不认识任何一个执行者。
//
// NOTE: 这几个函数里一个汉字都不该进日志 —— 规矩是「进度不进日志」（日志里那
// 一份是给人事后排查的，几千个资源对象各写一行会把它淹掉）。

void MainWindow::updateBottomBarTaskSummary() {
    const QDateTime now = QDateTime::currentDateTime();
    // 收起态显示谁 —— **唯一出处**在账本层那条纯函数里（那一串优先级 + "刚完成那一
    // 项还露着"的时间窗）。这里一个字都不加规则：加了就等于有两套优先级。
    const Tasks::TaskSummary picked = m_activityCenter->collapsedSummary(
        now, MainWindowUI::taskSummaryRecentWindowMs());

    QString title = picked.title;
    if (picked.state == Tasks::TaskState::Running && m_activityCenter->transferSpeed(picked.id) >= 0)
        title += QStringLiteral(" / %1/s").arg(
            InstanceFilesPage::formatSize(m_activityCenter->transferSpeed(picked.id)));

    // ---------- 同步等待与账本进度的合并 ----------
    // 同步等待期间 UI 处于等待网络或子进程的状态，因此底栏标题保留"正在准备..."之类的
    // 等待文案；但只要账本里当前有活动任务且能报出确定百分比（如准备 natives 的完成数、
    // 下载 Java 运行时的字节进度），进度环照样显示确定进度，不一律抹成 -1 转圈。
    // 若当前没有活动任务（或该任务本身不可测），Tasks::applySyncWait() 仍返回 percent = -1
    // 让进度环维持不确定态，也不会借用"刚完成任务"的 100%。
    const Tasks::TaskSummary resolved = Tasks::applySyncWait(picked, m_syncWaitStatus);

    MainWindowUI::BottomBarTaskSummary summary;
    summary.hasTask = resolved.id != Tasks::invalidTaskId() || !m_syncWaitStatus.isEmpty();
    summary.state = ActivityCenterController::presentationState(resolved.state);
    summary.percent = resolved.percent;
    summary.extraCount = resolved.extraCount;
    summary.title = m_syncWaitStatus.isEmpty() ? title : resolved.title;
    summary.taskTitle = resolved.id != Tasks::invalidTaskId() ? picked.title : QString();
    if (!resolved.currentStep.isEmpty() && resolved.currentStepIndex >= 0 && resolved.stepCount > 0) {
        summary.currentStep = tr("第 %1 / %2 步 · %3")
                                  .arg(resolved.currentStepIndex + 1)
                                  .arg(resolved.stepCount)
                                  .arg(resolved.currentStep);
    } else if (!resolved.detail.isEmpty()) {
        summary.currentStep = resolved.detail;
    } else if (!m_syncWaitStatus.isEmpty() && summary.taskTitle != m_syncWaitStatus) {
        summary.currentStep = m_syncWaitStatus;
    }
    // 红点看的是"**有没有没看过的失败**"，和正显示的是哪一项无关（正在下载而上一件
    // 事失败了，两个信号都该看得见）—— 这条也是账本层算的。
    summary.unreadFailure = resolved.hasFailure;

    m_ui->setBottomBarTaskSummary(summary);

    // ---------- 「刚完成那一项还露着」的倒计时 ----------
    // 这一档**没有任何信号**会来告诉界面"到期了"：不自己起一个单发定时器的话，那句话会
    // 一直挂在底栏上，直到下一次有东西变 —— 而那时候可能已经过了十分钟（一次下载下完、
    // 用户就去干别的了）。
    m_taskSummaryTimer->stop();
    if (summary.hasTask && Tasks::isTerminalState(picked.state)
        && picked.id != Tasks::invalidTaskId()) {
        const Tasks::TaskSnapshot task = m_activityCenter->tasks()->snapshot(picked.id);
        if (task.finishedAt.isValid()) {
            const qint64 left = MainWindowUI::taskSummaryRecentWindowMs()
                                - task.finishedAt.msecsTo(now);
            if (left > 0) {
                // +50 ms：保证这一发落在**过了期限之后**。掐着期限那一毫秒醒来会算出
                // 同一个 left（还是 > 0），于是立刻再起一发 —— 那就是个死循环。
                m_taskSummaryTimer->start(int(left) + 50);
            }
        }
    }
}

/// 「这一项**此刻**按『取消』真的会做点什么吗」。
///
/// 判据和 `cancelTaskFromDetail()` 的分派共用执行器状态。启动任务分两段：进程起来之后
/// 按绑定的会话号查找进程（同一实例开了多份时也只停对应会话），起来之前看启动流程还在
/// 不在半路（`LaunchController::isStarting()`）—— 那几段（换令牌 / natives / 下 Java）
/// 现在都能在下一个检查点停下。
///
/// 为什么非要现问一遍：账本里那位 `canCancel` 是 `begin()` 按**种类**记的（"这一类活
/// **允许**取消"），可这一类活里有几段**就是取消不掉**的 —— 加载器的安装器进程正在跑
/// 的时候（那是个 `QProcess`，不在下载批次里）。整合包后台解压和最终校验则已有合作式取消，
/// 由 `VersionManager::canCancelDownload()` 现问工人状态，不再仅依赖下载器是否繁忙。
bool MainWindow::cancelWouldWork(const Tasks::TaskSnapshot &task) const {
    if (task.kind == Tasks::TaskKind::Launch) {
        if (!Tasks::isActiveState(task.state)) {
            return false;
        }
        // 进程已经起来了：只认**这一项自己绑定的那一份会话**（同一实例开了多份时也只停对应会话）。
        if (task.sessionId > 0) {
            return m_launcherManager->session(task.sessionId).sessionId == task.sessionId;
        }
        // 还没起来：启动流程还在半路（换令牌 / 准备 natives / 下 Java 运行时那几段，
        // 最长几分钟）就能取消 —— 点了之后流程在下一个检查点停下，不拉起游戏。
        return m_launchController != nullptr && m_launchController->isStarting();
    }
    if (m_modTransferCancels.contains(task.id)) return true;
    // ⓪ 「实例更新」父任务：准备核心那一段可以取消（文件事务那一段由流程自己
    // 把能力位关掉，这里也问同一份入口）。
    if (m_updateFlowTask != Tasks::invalidTaskId() && task.id == m_updateFlowTask
        && m_updateFlowCancel && m_updateFlowCancelAvailable
        && m_updateFlowCancelAvailable()) {
        return true;
    }
    // ③ 后台导入世界 / 原地升级 / 回滚：工人和引擎都在安全点看这一位，取消得动。
    // 判据和 `cancelTaskFromDetail()` 里那一发逐字对齐。
    if (m_instanceTransferController != nullptr
        && m_instanceTransferController->canCancelTask(task.id)) {
        return true;
    }
    // ④ 备份 / 快照 / 还原：引擎在快照、解压、校验与提交前检查处看取消位
    // （仅最终改名那一下不可中断）。判据和 `cancelTaskFromDetail()` 同口径。
    if (m_backupController != nullptr && m_backupController->canCancelTask(task.id)) {
        return true;
    }
    if (m_presetController && m_presetController->ownsModTask(task.id)) return true;
    if (m_toolboxController && m_toolboxController->ownsDownloadTask(task.id))
        return m_toolboxController->canCancelDownloadTask(task.id);
    if (m_downloadBatches && m_downloadBatches->canCancel(task.id)) return true;
    // ① 服务端安装那一趟：自己跑 `QProcess` + 自己的元数据请求，不走下载批次。
    if (m_serverInstall != nullptr && m_serverInstall->isRunning()
        && m_serverInstallTaskId != Tasks::invalidTaskId() && task.id == m_serverInstallTaskId) {
        return true;
    }
    // ② 其余的（下载 / 安装 / 导入）底下都是 `VersionManager` 那一批：有批次在飞才真能掐。
    // 判据里**必须**带"这一项自己就是那一批"。只看"下载器忙不忙"的话，
    // 任何被记成 Install 的**别种**任务（典型：升级副本那种后台搬文件，见
    // InstanceDialogController 的 begin）都会长出一颗「取消」，按下去掐掉的却是当时
    // 正好在飞的**另一批**版本下载。`cancelTaskFromDetail()` 里那一句分派必须同口径。
    // 再补：`VersionManager` 上可以同时挂两批，账本里也就有两个号 ——
    // ① `m_downloadTaskLedger->task()`（`DownloadTaskLedgerController::begin()` 记的版本 /
    //    整合包 / 资源下载）；
    // ② `m_updateBatch.task`（「更新某个实例的 N 个资源」，见
    // `updateInstanceResources()`）。两批都是 `canCancelDownload()` 的真主人，少认一个
    // 就是"取消按钮整个消失"。别的号（升级副本、导入搬文件…）仍然一律不认。
    const bool ownsVersionBatch =
        task.id == m_downloadTaskLedger->task()
        || (m_updateBatch.task != Tasks::invalidTaskId() && task.id == m_updateBatch.task);
    return m_versionManager != nullptr && ownsVersionBatch
           && m_versionManager->canCancelDownload();
}

bool MainWindow::retryWouldWork(const Tasks::TaskSnapshot &task) const {
    // 判据本体在账本层的纯函数 `Tasks::retryOffered()`（verify 的 AL 节钉着它那张真值表）——
    // 这一层只负责把执行器的“现在还能不能重试”喂给任务中心；失败清单属于哪一项由
    // ActivityCenterController 自己持有。
    const bool held = m_versionManager != nullptr && m_versionManager->canRetryDownload();
    return m_activityCenter->retryWouldWork(task, held);
}

/// 账本里的一项 ⇒ 界面要的一份。**`taskCenterItems()`（面板）和任务详情页共用的唯一
/// 翻译点** —— 两边各写一遍的话，同一项在面板上和详情页上会说出不同的话。
TaskCenter::Item MainWindow::taskCenterItem(const Tasks::TaskSnapshot &task) const {
    const bool retryPayloadAvailable =
        m_versionManager != nullptr && m_versionManager->canRetryDownload();
    return m_activityCenter->item(task, cancelWouldWork(task), retryPayloadAvailable,
                                  m_crashReports->hasRecord(task.id));
}

QList<TaskCenter::Item> MainWindow::taskCenterItems() {
    const QSet<Tasks::TaskId> crashRecordIds = m_crashReports->recordIds();
    bool viewStateChanged = false;
    const bool retryPayloadAvailable =
        m_versionManager != nullptr && m_versionManager->canRetryDownload();
    QList<TaskCenter::Item> items = m_activityCenter->items(
        [this](const Tasks::TaskSnapshot &task) { return cancelWouldWork(task); },
        retryPayloadAvailable, crashRecordIds, &viewStateChanged);
    if (viewStateChanged) {
        updateBottomBarTaskSummary();
    }
    return items;
}

void MainWindow::openTaskDetailPage(const QString &key) {
    // 兼容入口：面板的 rowActivated 和验证工程按这个名字调用。
    m_taskDetail->open(key);
}

void MainWindow::onTaskDetailAction(const QString &key, TaskDetailPage::Action action) {
    m_taskDetail->action(key, action);
}

MainWindowUI::LaunchCard MainWindow::launchCardFor(const Tasks::TaskSnapshot &task, bool offline) {
    using Phase = MainWindowUI::LaunchCard::Phase;
    MainWindowUI::LaunchCard card;
    card.taskId = task.id;
    card.instanceKey = task.targetId;
    card.login = offline ? tr("离线登录") : tr("微软账号登录");
    // 步骤见 LaunchController：校验登录状态 / 检查启动文件 / 准备 Java 运行环境 / 等待游戏启动 / 完成。
    int step = 0;
    while (step < task.steps.size() && task.steps.at(step).state == Tasks::TaskStepState::Done) ++step;
    const bool counted = task.totalUnits > 0 && (step == 1 || step == 2);
    switch (step) {
    case 0: card.stage = tr("校验登录"); break;
    case 1:
        card.stage = !task.detail.isEmpty() ? task.detail
                    : (counted ? tr("补全文件与资源") : tr("检查文件"));
        break;
    case 2: card.stage = tr("准备 Java"); break;
    default: card.stage = tr("等待游戏窗口"); break;
    }
    switch (task.state) {
    case Tasks::TaskState::Succeeded:
        card.phase = Phase::Succeeded;
        break;
    case Tasks::TaskState::Failed:
        card.phase = task.errorSummary.isEmpty() ? Phase::Idle : Phase::Failed;
        card.reason = task.errorSummary;
        break;
    case Tasks::TaskState::Cancelled:
    case Tasks::TaskState::Interrupted:
        card.phase = Phase::Idle;
        break;
    default:
        card.phase = Phase::Running;
        if (counted) {
            card.done = task.doneUnits;
            card.total = task.totalUnits;
            card.bytes = step == 2;  // 下 Java 按字节报
        }
        break;
    }
    return card;
}

void MainWindow::syncLaunchCard(Tasks::TaskId id) {
    if (id < m_launchCardTask) return;  // 更早那一次启动：卡片只说最新的
    const Tasks::TaskSnapshot task = m_activityCenter->tasks()->snapshot(id);
    if (task.kind != Tasks::TaskKind::Launch) return;
    if (id != m_launchCardTask) {
        if (!Tasks::isActiveState(task.state)) return;
        m_launchCardTask = id;
    }
    m_ui->setLaunchCard(launchCardFor(task, offlineIdentity()));
}

bool MainWindow::cancelLaunch(const Tasks::TaskSnapshot &task) {
    if (!cancelWouldWork(task)) {
        return false;
    }
    if (task.sessionId > 0) {
        // 进程已经在跑：停那份会话（带那一问"游戏不会存档"的确认）。
        stopGameSession(task.sessionId);
        return true;
    }
    // 还没起来：让启动流程在下一个检查点停下（取消来源写「任务中心」，日志与终态按它分）。
    m_launchController->abortStart(LaunchController::StartCancelSource::TaskCenter);
    return true;
}

void MainWindow::cancelTaskFromDetail(const QString &key) {
    bool transferParsed = false;
    const auto transferId = key.toULongLong(&transferParsed);
    if (transferParsed) {
        const auto task = m_activityCenter->tasks()->snapshot(transferId);
        if (task.kind == Tasks::TaskKind::Launch) {
            // 进程起来前后走的是同一条入口（见 `cancelLaunch()`）—— 主页启动卡片
            // （#65）那颗「取消」也调它，两处不能各推一遍。
            cancelLaunch(task);
            return;
        }
    }
    // 「实例更新」父任务：准备阶段这一颗接到真的那一批下载上（文件事务阶段流程会把
    // 能力位关掉，所以这里不会把提交中的副本/快照打断）。
    if (transferParsed && m_updateFlowTask != Tasks::invalidTaskId()
        && transferId == m_updateFlowTask && m_updateFlowCancel && m_updateFlowCancel()) {
        return;
    }
    if (transferParsed && m_modTransferCancels.contains(transferId)) {
        m_modTransferCancels.value(transferId)();
        return;
    }
    // ③ 后台导入世界 / 原地升级 / 回滚：和 `cancelWouldWork()` 里那一问同口径 ——
    // 只有这一趟自己的取消位会被置上，不会顺手掐到别人的下载或别的实例的活。
    // 「取消之后会怎样」那句话由控制器按任务种类写（见 `InstanceTransferController::cancelTask()`）。
    if (transferParsed && m_instanceTransferController != nullptr
        && m_instanceTransferController->cancelTask(transferId)) {
        return;
    }
    // ④ 备份 / 快照 / 还原：和 `cancelWouldWork()` 里那一问同口径 —— 只掐这一趟。
    if (transferParsed && m_backupController != nullptr
        && m_backupController->cancelTask(transferId)) {
        appendLog(tr("已请求取消备份或还原；已取消的任务不会留下不完整的文件。"));
        return;
    }
    bool directKey = false;
    const auto directId = key.toULongLong(&directKey);
    if (directKey && m_toolboxController && m_toolboxController->cancelDownloadTask(directId)) return;
    if (directKey && m_downloadBatches && m_downloadBatches->cancel(directId)) return;
    if (directKey && m_presetController && m_presetController->ownsModTask(directId) && m_presetController->cancelModInstall()) return;
    // 取消要**按这一项是谁**分派：服务端安装那一趟和版本/资源下载那一批不是同一份
    // 状态（一个自己跑 QProcess + 自己的元数据请求，一个走 `VersionManager` 的批次）。
    // 两边都无脑叫一遍的话，取消一次服务端安装会顺手把别人正在跑的下载掐掉 ——
    // 而下载那一批可能是用户在下载页上排的、跟这一项毫无关系。
    //
    // NOTE: 这一条**留在 MainWindow**（`TaskDetailController` 只是按名字转手过来）：判据
    // 必须和 `cancelWouldWork()` 那条**逐字对齐** —— "那颗按钮露不露"和"按下去做的
    // 是哪一件事"是同一件事的两面，摆在一起才看得住。
    if (m_serverInstall != nullptr && m_serverInstall->isRunning()
        && m_serverInstallTaskId != Tasks::invalidTaskId()
        && key == QString::number(m_serverInstallTaskId)) {
        m_serverInstall->cancel();
        return;
    }
    // 和 `cancelWouldWork()` 的 ② 逐字同口径：版本下载 / 整合包导入那一批认
    // `m_downloadTaskLedger->task()`，「更新某个实例资源」那一批认 `m_updateBatch.task`。
    // 两者底下是同一个下载器，所以都是同一条 `cancelActiveDownload()`。
    if (m_versionManager != nullptr
        && (key == QString::number(m_downloadTaskLedger->task())
            || (m_updateBatch.task != Tasks::invalidTaskId()
                && key == QString::number(m_updateBatch.task)))) {
        // 取消走的是和"真失败"同一条终态信号（success=false），那几个槽
        // 分不出来 —— 于是用户刚点完取消就被弹一句「下载失败」、账本上记成 Failed、底栏
        // 还亮起一颗"没看过的失败"红点。这一位只在下面这一发里为真：终态信号是**同步**
        // 发完的（`cancelActiveDownload()` 返回之前），所以槽里读到的就是"这一次是取消"。
        m_downloadTaskLedger->setUserCancelInFlight(true);
        const bool cancelled = m_versionManager->cancelActiveDownload();
        m_downloadTaskLedger->setUserCancelInFlight(false);
        if (cancelled) {
            appendLog(tr("下载已取消，已下载的文件会保留。"));
        }
    }
}

void MainWindow::retryTaskFromDetail(const QString &key) {
    // 只认**这一项自己**那一批（`retryWouldWork()` 就是这条判据的唯一出处）：界面那边
    // 已经把不合条件的那颗按钮收起来了，这里是第二道闸 —— 页对象是**建一次留着**的，
    // 用户完全可能在页开着的时候又失败了一批，而按钮画的还是上一帧那份。
    bool parsed = false;
    // 行钥匙是 64 位的 `TaskId`（见 `ActivityCenterController::markViewed()` 那段的 NOTE）。
    const Tasks::TaskId retryId = key.toULongLong(&parsed);
    if (!parsed || m_versionManager == nullptr) {
        return;
    }
    const Tasks::TaskSnapshot retryTask = m_activityCenter->tasks()->snapshot(retryId);
    if (!retryWouldWork(retryTask)) {
        return;
    }
    // 忙的时候要**说一句**：`retryDownload()` 会因为
    // 「一次只有一批」返回 false，界面一声不吭就是"点了没反应"。
    //
    // 判据里补上 `m_downloadInFlight`。只问 `isDownloading()` 有一个
    // **真的窗口**：版本安装刚点下去、正在取 version.json 的那一段（元数据作业，
    // 不计进 `DownloadManager::isBusy()`），忙态已经是 true、账本里"安装 X"那一件
    // 也已经建好了，可这里会放行 —— 于是 `adopt(newTask)` 把安装那一件
    // 的号**顶掉**。之后安装那一路收尾（`onVersionDetailsFetched()` 失败支 /
    // `onAllDownloadsFinished()`）收的是**别人的号**，自己那件永远停在"进行中"。
    // 服务端安装那一段同理：它的下载批次是共用下载器上的，正在下的那几秒 `isDownloading()`
    // 已经为真；这里补的是"它正占着那一批"那一位（安装器跑着的那几分钟不算）。
    if (m_downloadInFlight || m_versionManager->isDownloading()
        || (m_serverInstall != nullptr && m_serverInstall->ownsDownloadBatch())) {
        InlineMessage::information(
            m_ui->overlay(), tr("暂时无法操作"),
            tr("已有下载或安装任务正在进行，请结束后再重试这几个文件。"));
        return;
    }
    // 重试也要在任务中心里看得见：账本里原来那一件已经是终态了（终态只落一次），
    // 不另起一行的话，这一次重试期间看不到进度、也取消不了。名字沿用原来那一件。
    //
    // 号和名字都要在 `retryDownload()` **之前**摆好：失败清单里那几个文件要是盘上
    // 其实都已经好好的（"重下 = 校验跳过"），这一批会**同步**收尾 —— 先摆好才认领得到
    // 那一条终态。名字从数据层取（`retryBatchLabel()`），因为 retryDownload() 一进去
    // 就把失败清单吃掉了；空名字 = 版本文件那一路，终态走 `versionDownloadFinished`。
    const QString retryLabel = m_versionManager->retryBatchLabel();
    const Tasks::TaskId newTask = beginTask(
        retryTask.kind, tr("重试：%1").arg(retryTask.title), retryTask.targetId);
    m_downloadTaskLedger->adopt(newTask);
    m_downloadTaskLedger->setResourceLabel(retryLabel);
    if (!m_versionManager->retryDownload()) {
        // 正常走不到（忙态上面刚问过）：别在账本里留一条永远"进行中"的行。
        m_activityCenter->tasks()->fail(newTask, tr("无法重试下载。"));
        m_downloadTaskLedger->clear();
        return;
    }
    // 那一格失败清单**刚被这一按吃掉了**（`retryDownload()` 自己 reset）⇒ 号也跟着
    // 松开，不然同一颗按钮留在页上还能再点一次（再点一次什么都不会发生）。
    m_activityCenter->markRetryConsumed();
    appendLog(tr("正在重试上次未下载的文件…"));
}

Tasks::TaskId MainWindow::beginTask(Tasks::TaskKind kind, const QString &title,
                                    const QString &targetId,
                                    const QList<Tasks::TaskStep> &steps) {
    return m_activityCenter->begin(kind, title, targetId, steps);
}

void MainWindow::finishTask(Tasks::TaskId id, bool success, const QString &errorSummary) {
    // 那格失败清单只认**这一批的主人**（见 `DownloadTaskLedgerController::owner()`）。
    // **不能**问 `canRetryDownload()`（全局）：那样任何一件任务在"清单还攥着"
    // 的时候收尾，账本都会把重试资格记到**它**头上：
    // · 真失败的那一件（比如补全文件）上的「重试」凭空消失；
    // · 服务端安装 / 导出 / 启动这一类**跟下载批次毫无关系**的任务反而长出「重试」，
    // 点下去重做的是别人的文件。
    const bool ownsDownloadBatch = m_downloadTaskLedger->owns(id);
    const bool retryPayloadAvailable = ownsDownloadBatch && m_versionManager != nullptr
                                       && m_versionManager->canRetryDownload();
    m_activityCenter->finish(id, success, errorSummary, retryPayloadAvailable);
    // 收过尾了，这一批不再"占着"重试资格 —— 清单本身（m_failedBatch）还在，
    // 露不露由账本里那一位 m_retryableTaskId 说了算。
    if (ownsDownloadBatch) {
        m_downloadTaskLedger->clearOwner();
    }
}

void MainWindow::setUpdateFlowCancel(Tasks::TaskId task, std::function<bool()> cancel,
                                     std::function<bool()> available) {
    m_updateFlowTask = task;
    m_updateFlowCancel = std::move(cancel);
    m_updateFlowCancelAvailable = std::move(available);
}

void MainWindow::clearUpdateFlowCancel(Tasks::TaskId task) {
    if (task == Tasks::invalidTaskId() || m_updateFlowTask == task) {
        m_updateFlowTask = Tasks::invalidTaskId();
        m_updateFlowCancel = nullptr;
        m_updateFlowCancelAvailable = nullptr;
    }
    m_downloadTaskLedger->dropParentIf(task);
}
