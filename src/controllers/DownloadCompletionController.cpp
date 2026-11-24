/**
 * @file DownloadCompletionController.cpp
 * @brief 下载批次收尾（见 .hpp 那段）。这一片是**接线**：一条终态信号分成六种来路说话，
 *        中间还夹着"装完之后把预设落上去"那一步。
 *
 * 判据全在别处：该装哪几份、指纹对不对、应用之后说什么在 `PresetController`；任务账在
 * `ActivityCenterController`；"能启动的实例"那张表在 `installingVersionIds()`。
 */

#include "controllers/DownloadCompletionController.hpp"
#include "TextMarks.hpp"

#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "controllers/InstallOccupancy.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "manage/VersionManager.hpp"

#include <QPointer>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

DownloadCompletionController::DownloadCompletionController(MainWindow *owner)
    : QObject(owner), m_owner(*owner) {}

void DownloadCompletionController::begin(const QString &instanceId,
                                         const QList<QPair<QString, bool>> &presets,
                                         const QMap<QString, QString> &fingerprints) {
    m_pendingInstance = instanceId;
    m_pendingPresets = presets;
    m_fingerprints = fingerprints;
    m_modsReady = false;
    m_modsOk = true;
    m_modsReport.clear();
}

void DownloadCompletionController::clear() {
    m_pendingPresets.clear();
    m_pendingInstance.clear();
    m_fingerprints.clear();
    m_modsReady = false;
    m_modsOk = true;
    m_modsReport.clear();
}

void DownloadCompletionController::onFinished(const QString &versionId, bool success,
                                               const QString &errorMessage) {
    if (m_owner.m_launchController->ownsStartupRepair(versionId)) return;
    // Keep the original installation task and busy guard alive until the asynchronous
    // preset download step completes. Re-enter once to use the existing finalization.
    if (success && m_owner.m_presetController && !m_modsReady &&
        m_owner.m_installOccupancy.is(InstallOccupancy::Batch::Install, versionId) &&
        versionId == m_pendingInstance && !m_pendingPresets.isEmpty()) {
        m_modsReady = true;
        auto steps = m_owner.m_activityCenter->tasks()->snapshot(m_owner.m_downloadTaskLedger->task()).steps;
        steps.append({MainWindow::tr("应用实例预设"), Tasks::TaskStepState::Running, MainWindow::tr("检查模组、前置与配置")});
        m_owner.m_activityCenter->tasks()->setSteps(m_owner.m_downloadTaskLedger->task(), steps);
        m_owner.m_activityCenter->tasks()->setProgress(m_owner.m_downloadTaskLedger->task(), 0, 0);
        m_owner.m_activityCenter->tasks()->setDetail(m_owner.m_downloadTaskLedger->task(), MainWindow::tr("正在检查并应用实例预设…"));
        QStringList ids;
        for (const auto &choice : m_pendingPresets) ids.append(choice.first);
        for (const auto &id : ids) {
            if (m_fingerprints.value(id).isEmpty() ||
                m_owner.m_presetController->presetFingerprint(id) != m_fingerprints.value(id)) {
                m_modsOk = false;
                m_modsReport = MainWindow::tr("预设在确认后发生变化或已损坏，未继续下载模组；请重新预览并确认。");
                onFinished(versionId, true, errorMessage);
                return;
            }
        }
        m_owner.appendLog(MainWindow::tr("正在应用实例预设：检查模组及必要前置…"));
        m_owner.m_presetController->installModsAfterInstall(ids, versionId,
            [this, versionId, errorMessage](bool ok, const QString &report) {
                m_modsOk = ok;
                m_modsReport = report;
                onFinished(versionId, true, errorMessage);
            });
        return;
    }
    // 这几笔要在 setDownloadUiBusy(false) **之前**清：那个函数会重灌"能启动的实例"，
    // 而判据里就有它们（见 installingVersionIds()）—— 顺序反了的话，刚装完的实例
    // 第一次刷新时还被滤着（要等下面那句再刷一次才出现），中间那一帧用户看到的是
    // "装完了可它不在列表里"。先清、后恢复界面，那一帧就不存在。
    //
    // `clearFor()` 把**和这个 id 对得上**的档全清掉，并告诉这一发该按哪一档说话
    // （优先级：导入 > 安装 > 补全 > 预取，见 `InstallOccupancy::active()`）。
    const InstallOccupancy::Batch batch = m_owner.m_installOccupancy.clearFor(versionId);
    const bool wasImporting = batch == InstallOccupancy::Batch::Import;
    const bool wasInstallingInstance = batch == InstallOccupancy::Batch::Install;
    // 「补全文件」那颗按钮和首启资源预取（见 maybePrefetchAssetDirector）走的是同一条
    // 公共终态信号，清场时机和上面两条逐字一样。
    const bool wasRepairing = batch == InstallOccupancy::Batch::Repair;
    const bool wasPrefetching = batch == InstallOccupancy::Batch::Prefetch;
    // 这一发是不是**用户自己按的取消**（判据见 `DownloadTaskLedgerController::userCancelInFlight()`）。
    // `cancelActiveDownload()` 走的是和"真失败"同一条终态信号，下面每一个分支都必须
    // 分开说：账本记 Cancelled、底栏不点红点、也不弹「失败」那个框。
    const bool userCancelled = !success && m_owner.m_downloadTaskLedger->userCancelInFlight();

    // 安装任务的最后一步：把这一趟带着的那份预设落上去（那条
    // 「生成并显示 ApplyPlan → 备份 → 应用预设 → 写入实例元数据和任务结果」）。
    //
    // 必须**在任务收尾之前**：预设应用本来就是安装任务的一步，它成没成要写进任务结果
    // 那句话里。这一趟写的是刚装出来的实例目录（几十 KB 的配置文件），而它此刻还在
    // `installingVersionIds()` 里 —— 不会有人在这期间把它启动起来。
    //
    // 应用失败**不把安装任务改判成失败**：游戏和加载器都装好了，能玩；预设没落上是
    // 另一句话。所以 `success` 原样传下去，预设那一句并进结果文本里（见下面那一段）。
    QString resultMessage = errorMessage;
    QString presetApplyProblem;
    if (!m_modsOk) presetApplyProblem = m_modsReport;
    if (wasInstallingInstance && success && !m_pendingPresets.isEmpty() &&
        m_pendingInstance == versionId) {
        if (m_owner.m_presetController != nullptr) {
            QStringList applied;
            QStringList failed;
            if (!m_modsReport.isEmpty()) {
                if (m_modsOk) applied.append(m_modsReport);
                else failed.append(m_modsReport);
            }
            for (const QPair<QString, bool> &one : m_pendingPresets) {
                if (m_fingerprints.value(one.first).isEmpty() ||
                    m_owner.m_presetController->presetFingerprint(one.first) != m_fingerprints.value(one.first)) {
                    presetApplyProblem = MainWindow::tr("预设在确认后发生变化或已损坏，未使用旧确认覆盖新内容，请重新预览。");
                    failed.append(presetApplyProblem);
                    continue;
                }
                // 四类各一份 —— 每一份都走同一条路；一份失败不把前面已经落上去的
                // 那几份从结果里抹掉（它们确实写进去了）。
                const PresetController::InstallApply outcome =
                    m_owner.m_presetController->applyAfterInstall(one.first, versionId, one.second);
                if (outcome.ok) {
                    applied.append(outcome.message);
                } else {
                    presetApplyProblem = outcome.message;
                    failed.append(outcome.message);
                }
            }
            if (failed.isEmpty()) {
                resultMessage = errorMessage + QStringLiteral("\n\n") +
                                applied.join(QLatin1Char('\n'));
            } else {
                resultMessage = QCoreApplication::translate("DownloadCompletionController", "%1\n\n实例可用，预设部分应用：\n%2")
                                    .arg(errorMessage, (applied + failed).join(QLatin1Char('\n')));
            }
        }
    }
    // 不管这一趟成没成、带没带预设，pending 都在这一刻清干净：下一次安装不该继承这一次的。
    if (m_modsReady) {
        const auto steps = m_owner.m_activityCenter->tasks()->snapshot(m_owner.m_downloadTaskLedger->task()).steps;
        if (!steps.isEmpty() && steps.last().name == MainWindow::tr("应用实例预设"))
            m_owner.m_activityCenter->tasks()->setStepState(m_owner.m_downloadTaskLedger->task(), steps.size() - 1,
                presetApplyProblem.isEmpty() ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Failed,
                presetApplyProblem);
    }
    if (wasImporting && m_owner.m_downloadTaskLedger->task() != Tasks::invalidTaskId()) {
        const Tasks::TaskId importTaskId = m_owner.m_downloadTaskLedger->task();
        Tasks::TaskManager *tasks = m_owner.m_activityCenter->tasks();
        const QList<Tasks::TaskStep> importSteps = tasks->snapshot(importTaskId).steps;
        if (success) {
            for (int i = 0; i < importSteps.size(); ++i) {
                tasks->setStepState(importTaskId, i, Tasks::TaskStepState::Done);
            }
            tasks->setDetail(importTaskId, MainWindow::tr("已装好"));
        } else if (!userCancelled) {
            for (int i = 0; i < importSteps.size(); ++i) {
                if (importSteps.at(i).state == Tasks::TaskStepState::Running) {
                    tasks->setStepState(importTaskId, i, Tasks::TaskStepState::Failed, resultMessage);
                }
            }
        }
    }
    m_pendingPresets.clear();
    m_pendingInstance.clear();
    m_modsReady = false;
    m_modsOk = true;
    m_modsReport.clear();
    m_fingerprints.clear();

    // 这一批在账本上收尾。**必须在 setDownloadUiBusy(false) 之前**：那个函数是"这一批
    // 结束了"的公共通道，它自己会在末尾兜一次底（见那边）——先在这儿按成败如实收，
    // 兜底那一句就什么都不做；反过来顺序的话，兜底会先把任务按失败收掉，这一句再说什么
    // 都晚了（终态只落一次）。
    if (userCancelled && m_owner.m_downloadTaskLedger->task() != Tasks::invalidTaskId()) {
        // 用户主动取消是**正常收场**，账本里那一档就叫 Cancelled
        // （服务端安装那条路也是这么收的，见 ConnectionsController 里那段 NOTE）。
        // 借 `finishTask(..., false, …)` 的话：状态是 Failed、底栏亮一颗"没看过的失败"
        // 红点、「需要处理」筛选里多一条假失败，而用户只是按了一下取消。
        m_owner.m_activityCenter->tasks()->cancel(m_owner.m_downloadTaskLedger->task());
        // 重试资格跟着松开（和 finishTask() 里那一句同一个理由：这一批已经收尾了）。
        m_owner.m_downloadTaskLedger->clearOwnerIf(m_owner.m_downloadTaskLedger->task());
    } else {
        m_owner.finishTask(m_owner.m_downloadTaskLedger->task(), success, resultMessage);
    }
    m_owner.m_downloadTaskLedger->clearTask();

    // 整批结束，无论成败都要把界面恢复回来
    m_owner.setDownloadUiBusy(false);
    m_owner.m_downloadTaskLedger->resetProgressThrottle();

    // Instance update chains core preparation into its next step; do not open an
    // unrelated installation-complete dialog or apply default presets here.
    if (m_owner.m_versionManager->property("instanceUpdatePreparedId").toString() == versionId) {
        m_owner.refreshInstalledVersions();
        return;
    }

    // 这一条讲的是整合包导入（数据层那边"一次点击 = 一个终态"，导入也走这同一个信号）。
    // 收尾那句话是**说给用户看的**（"整合包「X」导入好了…"），所以这里不再往上叠
    // 「版本 X 的文件已全部就绪」那一句 —— 对导入来说那句是错的：下的是这个整合包的
    // 模组，不是"某个版本的文件"。成功的样子是**实例列表里多一个**（顺便选中它）。
    if (wasImporting) {
        if (success) {
            m_owner.appendLog(TextMarks::done(tr("%1").arg(errorMessage)));
            // 刷新会把下拉框和实例页那张树一起重灌（同一个函数里做的），新实例就在里面。
            m_owner.refreshInstalledVersions();
            QComboBox *combo = m_owner.m_ui->getInstalledVersionCombo();
            const auto selectImportedVersion = [&]() {
                if (const int index = combo->findData(versionId); index >= 0) {
                    combo->setCurrentIndex(index);
                }
                m_owner.updateStartButtonText();
                if (QTreeWidget *tree = m_owner.m_ui->getInstanceList()) {
                    MainWindowViewData::syncTreeToCombo(combo, tree);
                    if (QTreeWidgetItem *current = tree->currentItem()) {
                        tree->scrollToItem(current);
                    }
                }
            };
            // 刷新只"保住原来选中的"，保不住"原来压根没选"：刚导入出来的这个顶上，
            // 省得用户还得自己去列表里找一遍（和下载完成那条路一个口径）。
            // NOTE: 那条口径是**两件事**：不但要"顶上"，还得"原来已经有选中的就别动"
            // —— 用户自己挑的版本比"刚做完的那个"更重要，所以下面那道
            // currentIndex() < 0 的守卫不能省。
            const bool oneClickModpack = m_owner.takeOneClickModpack(versionId);
            if (oneClickModpack || combo->currentIndex() < 0) {
                selectImportedVersion();
            }
            m_owner.updateStartButtonText();
            // 正在看任务中心浮层（`taskCenterPromptWorkspace` 占着提示位）时把收尾弹窗排进队列，
            // 等用户关掉任务中心立刻弹出，避免被「已有提示开着」静默吞掉。
            const auto showCompletePrompt = [weakOwner = QPointer<MainWindow>(&m_owner), versionId,
                                             errorMessage]() {
                if (weakOwner.isNull()) {
                    return;
                }
                QComboBox *targetCombo = weakOwner->m_ui->getInstalledVersionCombo();
                const auto applySelection = [weakOwner, targetCombo, versionId]() {
                    if (const int index = targetCombo->findData(versionId); index >= 0) {
                        targetCombo->setCurrentIndex(index);
                    }
                    weakOwner->updateStartButtonText();
                    if (QTreeWidget *tree = weakOwner->m_ui->getInstanceList()) {
                        MainWindowViewData::syncTreeToCombo(targetCombo, tree);
                        if (QTreeWidgetItem *current = tree->currentItem()) {
                            tree->scrollToItem(current);
                        }
                    }
                };
                const int action = InlineMessage::ask(
                    weakOwner->m_ui->overlay(), MessageKind::Information,
                    MainWindow::tr("整合包已装好"), errorMessage,
                    {MainWindow::tr("查看实例"), MainWindow::tr("立即启动"), MainWindow::tr("知道了")},
                    0, -1);
                if (action == 0) {
                    applySelection();
                    if (QTabWidget *tabs = weakOwner->m_ui->getTabWidget()) {
                        tabs->setCurrentIndex(MainWindowUI::instancesTabIndex());
                    }
                } else if (action == 1) {
                    applySelection();
                    if (QTabWidget *tabs = weakOwner->m_ui->getTabWidget()) {
                        tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
                    }
                }
                // 这次导入的东西要是 GTNH 那种会在首启里跑 MCLib AssetDirector 的包，
                // 趁热把那几十 MB 资源预取掉（体积、写入位置见 docs/features/MODPACK_IMPORT.md）。
                // 放在提示之后：那几个提示是"导入成了"的收尾，先让用户看见它。
                weakOwner->maybePrefetchAssetDirector(versionId);
                if (action == 1) {
                    QTimer::singleShot(0, weakOwner.data(), &MainWindow::onStartButtonClicked);
                }
            };
            if (OverlayHost *overlay = m_owner.m_ui->overlay();
                overlay == nullptr || !overlay->queueNotification(showCompletePrompt)) {
                showCompletePrompt();
            }
        } else if (userCancelled) {
            m_owner.takeOneClickModpack(versionId);
            // 用户自己按的取消：日志照写，但**不弹「导入失败」**（那不是失败）。
            m_owner.appendLog(TextMarks::failed(tr("整合包导入已由用户取消。")));
        } else {
            m_owner.takeOneClickModpack(versionId);
            m_owner.appendLog(TextMarks::failed(tr("整合包没有导入：%1").arg(errorMessage)));
            const auto showFailPrompt = [weakOwner = QPointer<MainWindow>(&m_owner), errorMessage]() {
                if (weakOwner.isNull()) {
                    return;
                }
                InlineMessage::warning(weakOwner->m_ui->overlay(), MainWindow::tr("导入失败"), errorMessage);
            };
            if (OverlayHost *overlay = m_owner.m_ui->overlay();
                overlay == nullptr || !overlay->queueNotification(showFailPrompt)) {
                showFailPrompt();
            }
        }
        return;
    }

    // 这一条讲的是"装一个带名字的实例"（安装页上点了「安装」）。它走的也是这同一个
    // 信号（数据层那边"一次点击 = 一个终态"），收尾那句话说的是**用户起的那个名字**。
    // 和整合包那段同一个道理：不再往上叠「版本 X 的文件已全部就绪」那一句。
    if (wasInstallingInstance) {
        if (success) {
            m_owner.appendLog(TextMarks::done(tr("%1").arg(resultMessage)));
            // 装出来的实例要能被启动：刷"已安装版本"那一摊，并把它选上（刷新只保得住
            // "原来选中的"，保不住"原来压根没选"）。
            m_owner.refreshInstalledVersions();
            QComboBox *installedCombo = m_owner.m_ui->getInstalledVersionCombo();
            if (installedCombo->currentIndex() < 0) {
                if (const int index = installedCombo->findData(versionId); index >= 0) {
                    installedCombo->setCurrentIndex(index);
                }
            }
            m_owner.updateStartButtonText();
            // 预设没落上：安装本身成了，所以**不改标题**（「安装失败」是假话），
            // 但那一句必须是 warning 的颜色 —— 用户这会儿最该看见的正是它。
            if (!presetApplyProblem.isEmpty()) {
                InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("安装完成，但预设没有应用上"),
                                       resultMessage);
            } else {
                InlineMessage::information(m_owner.m_ui->overlay(), MainWindow::tr("安装完成"), resultMessage);
            }
        } else if (userCancelled) {
            m_owner.appendLog(TextMarks::failed(tr("安装「%1」已由用户取消。").arg(m_owner.instanceDisplayName(versionId))));
        } else {
            m_owner.appendLog(TextMarks::failed(tr("%1").arg(errorMessage)));
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("安装失败"), errorMessage);
        }
        return;
    }

    // 这一条讲的是「补全文件」（实例设置页「启动」栏那颗按钮，）。走的也是公共的
    // versionDownloadFinished(id, ok, msg)：数据层那边"一次点击 = 一个终态"这条约定不破。
    // 这里要说的是**用户认得的那个名字**（实例名，不是目录名），而且不能说"下载完成" ——
    // 用户点的是"检查"，那就报"检查完"。
    if (wasRepairing) {
        const QString displayName = m_owner.instanceDisplayName(versionId);
        if (success) {
            m_owner.appendLog(TextMarks::done(tr("实例「%1」的文件校验完成：%2").arg(displayName, errorMessage)));
            // 补全期间它被滤出了"能启动的实例"（installingVersionIds() 里有它），回来之后
            // 把它选中 —— 和上面那条同一种情况：刷新只保得住"原来选中的"，保不住"原来被
            // 滤掉、选中的那一格顺位落到了别人身上"。
            QComboBox *installedCombo = m_owner.m_ui->getInstalledVersionCombo();
            if (installedCombo->currentIndex() < 0) {
                if (const int index = installedCombo->findData(versionId); index >= 0) {
                    installedCombo->setCurrentIndex(index);
                }
            }
            m_owner.updateStartButtonText();
            InlineMessage::information(m_owner.m_ui->overlay(), MainWindow::tr("检查完成"),
                                       MainWindow::tr("实例「%1」的文件都齐了。\n\n%2")
                                           .arg(displayName, errorMessage));
        } else if (userCancelled) {
            m_owner.appendLog(TextMarks::failed(tr("实例「%1」的文件校验已由用户取消。").arg(displayName)));
        } else {
            m_owner.appendLog(TextMarks::failed(tr("实例「%1」的文件没补齐：%2").arg(displayName, errorMessage)));
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("文件没补齐"),
                                   MainWindow::tr("未能取得实例「%1」的部分文件。\n\n%2")
                                       .arg(displayName, errorMessage));
        }
        return;
    }

    // GTNH 首启资源预取（2.1.6b）。走的是公共终态信号，但话术是另一件事：
    // 既不是"版本文件就绪"，也不是"校验完成" —— 它下的是共享的 assets/asset_director。
    // success + 空 message = 这一趟其实没什么要下的（数据层那边有意不打扰用户）。
    if (wasPrefetching) {
        const QString displayName = m_owner.instanceDisplayName(versionId);
        if (success) {
            if (errorMessage.isEmpty()) {
                m_owner.appendLog(MainWindow::tr("「%1」的首次启动资源已齐全，本次无需下载。").arg(displayName));
            } else {
                m_owner.appendLog(TextMarks::done(tr("「%1」的首次启动资源已预取：%2").arg(displayName, errorMessage)));
                InlineMessage::information(
                    m_owner.m_ui->overlay(), MainWindow::tr("首次启动资源已预取"),
                    MainWindow::tr("「%1」的首次启动资源已就绪。\n\n%2")
                        .arg(displayName, errorMessage));
            }
        } else if (userCancelled) {
            m_owner.appendLog(TextMarks::failed(tr("已取消「%1」的首次启动资源预取。已下载的文件会保留。"))
                          .arg(displayName));
        } else {
            m_owner.appendLog(TextMarks::failed(tr("「%1」的首次启动资源下载不完整：%2").arg(displayName, errorMessage)));
            // 重试 = 再发起一次预取（数据层那边会跳过校验通过的部分，不会从头再来）。
            // 这条不走账本里那颗"重试"：那一颗是给"失败清单"用的，而预取失败之后
            // 最该做的事是"再核一遍盘上那些、把缺的补上"，不是"把这份清单重下一遍"。
            const bool retry = InlineMessage::question(
                m_owner.m_ui->overlay(), MainWindow::tr("首次启动资源下载不完整"),
                MainWindow::tr("「%1」的 GTNH 首次启动资源下载不完整。\n\n%2\n\n游戏仍可启动，并会在首次运行时下载缺失资源。重试下载？")
                    .arg(displayName, errorMessage),
                MainWindow::tr("重试"), MainWindow::tr("完成"), true);
            if (retry) {
                m_owner.maybePrefetchAssetDirector(versionId);
            }
        }
        return;
    }

    if (success) {
        m_owner.appendLog(TextMarks::done(tr("版本 %1 下载完成：%2").arg(versionId, errorMessage)));

        // 刷新"已安装版本"下拉框：client jar 已经落到
        // .minecraft/versions/<id>/<id>.jar，installedVersions() 下次扫盘就能看到它。
        // NOTE: 这里可以放心刷新：fillVersionCombo() 自己负责把原来选中的版本捡回来。
        m_owner.refreshInstalledVersions();

        // 刷新只能"保住"原来的选中项，保不住"原来压根没选"：那种情况下把刚下完的
        // 版本顶上，省得用户下完还要自己去下拉框里再找一遍。原来已经有选中项时
        // 不覆盖 —— 用户自己挑的版本比"最新下完的那个"更重要。
        QComboBox *installedCombo = m_owner.m_ui->getInstalledVersionCombo();
        if (installedCombo->currentIndex() < 0) {
            // 按 id 找（findData），不是按显示文本（findText）——刚下完的版本可能
            // 已经有自定义名，显示文本不一定是版本号。
            if (const int index = installedCombo->findData(versionId); index >= 0) {
                installedCombo->setCurrentIndex(index);
            }
        }

        InlineMessage::information(m_owner.m_ui->overlay(), MainWindow::tr("下载完成"),
                                 MainWindow::tr("版本 %1 的文件已全部就绪。\n\n%2").arg(versionId, errorMessage));
    } else if (userCancelled) {
        // 用户自己按的取消：一句日志就够（`cancelTaskFromDetail()` 那边已经写过「已取消」）。
        // 不弹「下载失败」—— 那不是失败。
        m_owner.appendLog(TextMarks::failed(tr("版本 %1 的下载已由用户取消。").arg(versionId)));
    } else {
        // 失败就不刷新了：没成功就不该有新条目冒出来（jar 大概率没落地）
        m_owner.appendLog(TextMarks::failed(tr("版本 %1 下载失败：%2").arg(versionId, errorMessage)));
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("下载失败"),
                             MainWindow::tr("版本 %1 有文件未能下载完成。\n\n%2").arg(versionId, errorMessage));
    }
}
