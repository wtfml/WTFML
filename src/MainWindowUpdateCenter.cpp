/**
 * @file MainWindowUpdateCenter.cpp
 * @brief 实例更新检测与更新中心：检测那一趟、可更新名单、更新中心那一页、换文件与收尾。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"这个实例有哪些资源有新版本、要不要换"。它是一条**顺序不能换**的链
 * （分节横幅里写着）：检测（`checkInstanceUpdates()` + 那几发回调）→ 可更新名单
 * （`rebuildInstanceOutdatedPaths()` / `saveUpdatePolicy()`）→ 更新中心那一页
 * （`openUpdateCenter()` / `updateCenterReportForPage()`）→ 换文件
 * （`onUpdateBatchFileFinished()` / `finishUpdateBatch()`）。分出来之后，一条链的全部
 * 环节在一个文件里按顺序摆着。
 *
 * 【两条说死的规矩】
 *
 * - 检测这一趟**不自动下载、不自动改盘、不问"要不要更新"** —— 报告里只说事实。
 * - 收尾之后**再开一次更新中心**走 `openPageWhenIdle()`：后台收的尾可能跑了好几分钟，
 *   用户早翻走了，不驱逐他正看着的那一页。
 *
 * 【判据不在这一层】
 *
 * 检测与阈值在 `InstanceUpdateCheckController`，名单与策略在 `UpdateCenterController`，
 * 换文件的账在 `DownloadTaskLedgerController`；"可更新"的键来自 `installedModKeys()`。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\UpdateCenterPage.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/InstanceUpdateCheckController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/UpdateCenterController.hpp"

#include <QSet>

using namespace MainWindowViewData;

// ============================ 实例更新检测 ============================
//
// 【这一趟在做什么（顺序不能换）】
//
// 【为什么别的事都不做】
//
// 不自动下载、不自动改盘、不问"要不要更新" —— 要的是"检测"。报告里只说事实
// （哪几个有新版本、最新的那一版叫什么），后面那一步是他自己的事。

void MainWindow::checkInstanceUpdates(const QString &instanceId, InstanceSettingsDialog *dialog,
                                     const QStringList &selectedPaths) {
    m_instanceUpdateCheck->checkInstanceUpdates(instanceId, dialog, selectedPaths);
}

void MainWindow::onUpdateCheckLookupFinished(const InstanceIconLookup::Result &result,
                                            const QString &instanceId) {
    m_instanceUpdateCheck->onUpdateCheckLookupFinished(result, instanceId);
}

// 更新检测：**按钥匙发那一发"问这个项目的版本列表"**（一把钥匙一发）。
//
// 单独一个函数的缘由是它有两个调用点 —— 反查那一趟回来时对 `itemsByKey` 里每把钥匙发一遍，
// 以及批量那一发回来时给"退回问 CurseForge"的那些补发（见 `onUpdateCheckLatestFinished()`）。
// 两处要发的东西是**逐字同一句**，抄第二份的话迟早有一边忘了带上来源。
void MainWindow::requestUpdateCheckVersions(const QString &requestKey) {
    m_instanceUpdateCheck->requestUpdateCheckVersions(requestKey);
}

void MainWindow::onUpdateCheckLatestFinished(
    const QString &requestKey, const QHash<QString, ResourceCatalog::VersionEntry> &latest,
    const QString &problem) {
    m_instanceUpdateCheck->onUpdateCheckLatestFinished(requestKey, latest, problem);
}

void MainWindow::onUpdateCheckVersionsFinished(const QString &requestKey,
                                              const QList<ResourceCatalog::VersionEntry> &versions,
                                              const QString &problem) {
    m_instanceUpdateCheck->onUpdateCheckVersionsFinished(requestKey, versions, problem);
}

// 参数**按值收**（别改回 `const QString &`）：下面那句 `m_updateCheck =
// UpdateCheckRun{}` 会把这个成员清空，而 `onUpdateCheckVersionsFinished` 那边递进来的
// 正是 `m_updateCheck.instanceId` **本身** —— 按引用收的话，函数体第一句就把自己这个参数
// 指的那块 QString 抹成空的，于是整份名单记在**空键**底下（`m_instanceOutdatedPaths`）。
// 症状非常有欺骗性：**当着一页的面筛得出来**（那一趟是拿 `run.instanceId` 直接递过去的，
// 而 `run` 是赋值前拷出来的），关掉这一页再进来就永远筛不出东西、还重新挂着「还没有查过
// 更新」—— 而那一趟明明刚跑完。屏幕上看不出任何异常，日志也是干净的。
// QString 是隐式共享的，按值收不多拷一个字节的正文。
void MainWindow::finishUpdateCheck(QString instanceId) {
    m_instanceUpdateCheck->finishUpdateCheck(instanceId);
}

// ===========================================================================
// 更新中心
// ===========================================================================
//
// 这一段现在只剩两条薄壳：那一页的**全部状态**（"忽略 / 固定"那一本账、页面对象、开页时
// 记下的实例 id）以及把它翻成动作的那几条，都在 `controllers/UpdateCenterController`
// （检测那一趟自己另有一层，见 `controllers/InstanceUpdateCheckController`）。
//
// 【为什么还留着这两条壳】
//
// 按名字叫它们的**不止这里**：检测收尾那条路要清一遍过期的固定、再把「可更新」名单重算
// 一次（`InstanceUpdateCheckController::finishUpdateCheck()`）。壳的意思只是"主窗口仍然认
// 这个名字"，实现随状态一起搬走了 —— 状态和动它的那几条在同一个 .cpp 里。

void MainWindow::rebuildInstanceOutdatedPaths(const QString &instanceId) {
    m_updateCenter->rebuildOutdatedPaths(instanceId);
}

bool MainWindow::saveUpdatePolicy() {
    return m_updateCenter->savePolicy();
}

void MainWindow::setInstanceMode(MainWindowUI::InstanceMode mode) {
    if (m_ui != nullptr) {
        m_ui->setInstanceMode(mode);  // 停在哪一页 + 谁勾着（唯一出处）
        // 预设编辑器里取消离开时界面留在原页：这边也不换档，免得记下的模式和看到的那页对不上。
        if (QStackedWidget *stack = m_ui->getInstanceModeStack();
            stack != nullptr && stack->currentIndex() != static_cast<int>(mode))
            return;
    }
    m_instanceMode = mode;
    if (mode == MainWindowUI::InstanceMode::Server) {
        // 切到服务端那一页时顺手重扫一遍：盘上那个 servers/ 可能刚被别的东西动过
        // （用户在资源管理器里删了一个、或者拖进来一个装好的）。
        refreshServerList();
    } else if (mode == MainWindowUI::InstanceMode::Preset && m_presetController != nullptr) {
        // 预设那一页是常驻的，切过来时刷新一遍（列表 / 实例下拉框都可能刚变过）。
        m_presetController->open();
    }
}

// ---------- 服务端那一页的接线 ----------
// 列表怎么重扫、行怎么点、控制台怎么开、设置页那几组动作、修 / 删 / 起停，以及「新建服务端」
// 那一页，都在 `src/MainWindowServer.cpp`（同一个类，另一个翻译单元）。


// 这两条是「更新中心」那一页在**主窗口这一侧**的入口：状态和动作都在
// `controllers/UpdateCenterController`（见那一段的说明）。
void MainWindow::openUpdateCenter(const QString &instanceId) {
    m_updateCenter->open(instanceId);
}

UpdateCenterPage::Report MainWindow::updateCenterReportForPage(const QString &instanceId) const {
    return m_updateCenter->reportFor(instanceId);
}

void MainWindow::onUpdateBatchFileFinished(const QString &label, const QString &fileName,
                                           bool success, const QString &errorMessage) {
    if (m_updateBatch.label.isEmpty() || m_updateBatch.label != label) {
        return;  // 不是这一批（别的一批资源下载也在发同一个信号）
    }
    QList<int> &pending = m_updateBatch.pendingSteps[fileName];
    if (pending.isEmpty()) {
        return;  // 这一条不在我们登记过的名单里（正常走不到）
    }
    const int index = pending.takeFirst();
    if (m_activityCenter->tasks()->has(m_updateBatch.task)) {
        m_activityCenter->tasks()->setStepState(m_updateBatch.task, index,
                                    success ? Tasks::TaskStepState::Done
                                            : Tasks::TaskStepState::Failed,
                                    success ? QString() : errorMessage);
    }
    if (success) {
        const QString path = m_updateBatch.pathByFileName.value(fileName);
        if (!path.isEmpty()) {
            m_updateBatch.succeeded.append(path);
        }
        m_updateBatch.lines.append(tr("• %1：已替换为新版本").arg(fileName));
    } else {
        ++m_updateBatch.failedSteps;
        m_updateBatch.lines.append(
            tr("• %1：替换失败：%2").arg(fileName, errorMessage));
    }
}

void MainWindow::finishUpdateBatch(bool success, const QString &errorMessage) {
    const UpdateBatchRun run = m_updateBatch;
    m_updateBatch = UpdateBatchRun{};
    if (run.label.isEmpty()) {
        return;
    }
    // 取消和真失败走同一条终态信号（见 `DownloadTaskLedgerController::userCancelInFlight()`）——
    // 用户按了取消就记 Cancelled，不记 Failed、不点亮底栏那颗"没看过的失败"红点。
    const bool userCancelled = !success && m_downloadTaskLedger->userCancelInFlight();
    if (userCancelled && m_activityCenter->tasks()->has(run.task)) {
        m_activityCenter->tasks()->cancel(run.task);
    } else if (m_activityCenter->tasks()->has(run.task)) {
        m_activityCenter->tasks()->setDetail(run.task, tr("已替换 %1 个，失败 %2 个。")
                                               .arg(run.succeeded.size())
                                               .arg(run.failedSteps));
        finishTask(run.task, success && run.failedSteps == 0,
                   run.failedSteps > 0 ? tr("有 %1 个文件未能替换。").arg(run.failedSteps)
                                       : errorMessage);
    }
    // finishTask() 认领过就会自己清掉；这一段是"那一件已经不在账本里"的兜底，
    // 别让重试资格留在一次已经收尾的更新上（见 `clearOwnerIf()`）。
    m_downloadTaskLedger->clearOwnerIf(run.task);
    appendLog(success ? tr("更新「%1」结束：换好 %2 个，失败 %3 个。")
                            .arg(instanceDisplayName(run.instanceId))
                            .arg(run.succeeded.size())
                            .arg(run.failedSteps)
                      : tr("「%1」的更新未完成：%2").arg(instanceDisplayName(run.instanceId), errorMessage));
    // 「不只显示总数」：**逐个资源**写进去（日志里那一行一行就是每个资源的结果）。
    for (const QString &line : run.lines) {
        appendLog(QStringLiteral("  ") + line);
    }

    if (!run.succeeded.isEmpty()) {
        // 换掉的这几条从「可更新」名单里摘掉（不摘的话文件管理页上那几行还标着"可更新"，
        // 而它们刚被换成了最新那一版 —— 再点一次「更新」会白下一遍）。
        // NOTE: 只摘**这一批真换成功的**那几条；失败的留在名单里（那一行仍然是对的）。
        const QStringList outdated = m_instanceOutdatedPaths.value(run.instanceId);
        if (!outdated.isEmpty()) {
            QStringList keep;
            for (const QString &path : outdated) {
                if (!run.succeeded.contains(path)) {
                    keep.append(path);
                }
            }
            m_instanceOutdatedPaths.insert(run.instanceId, keep);
        }
        // 那一份"换成哪一版"的账也一起摘（同一次动，见 m_instanceUpdateTargets 那段）。
        QHash<QString, ResourceCatalog::VersionEntry> targets =
            m_instanceUpdateTargets.value(run.instanceId);
        for (const QString &path : run.succeeded) {
            targets.remove(path);
        }
        m_instanceUpdateTargets.insert(run.instanceId, targets);
    }

    const QString text = run.lines.isEmpty() ? errorMessage : run.lines.join(QLatin1Char('\n'));
    if (!success || run.failedSteps > 0) {
        InlineMessage::warning(m_ui->overlay(), tr("部分更新文件未能替换"),
                               tr("有 %1 个文件未能替换。\n\n%2\n\n"
                                  "可以在「更新中心」里撤销上一次更新。")
                                   .arg(run.failedSteps)
                                   .arg(text));
    }
    // 收尾之后**再开一次更新中心**：用户刚按的就是它上面那一颗，他下一步要么看看剩下的
    // 那几条、要么撤销（失败了的话）。这是后台收的尾（下载可能跑了好几分钟，用户早翻到
    // 别的页去了）⇒ 走 `openPageWhenIdle()`：**不驱逐**他正看着的那一页，等它收干净再开。
    const QString instanceId = run.instanceId;
    openPageWhenIdle([this, instanceId] { openUpdateCenter(instanceId); });
}

