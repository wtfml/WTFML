/**
 * @file MainWindowRepair.cpp
 * @brief 把实例里缺的东西补齐：修复文件（`repairInstanceFiles()`）与提前预取资源（`maybePrefetchAssetDirector()`）。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"还差什么、现在能不能补"：两件事都要先过那几道守卫（另有实例在下载 /
 * 正在启动 / 已经占着同一批），补完还要把忙态与任务收回来 —— 收尾的两套分支写反一处就是
 * "任务一直转圈"。放在一个文件里，守卫与收尾可以上下对着看。
 *
 * 【判据不在这一层】
 *
 * 占用状态只有 `InstallOccupancy` 一家；盘上怎么修在 `VersionManager`，资源预取在
 * `IntegrityController`；任务账本在 `DownloadTaskLedgerController`。
 */

#include "MainWindow.hpp"

#include "TextMarks.hpp"
#include "..\ui\InlineMessage.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/IntegrityController.hpp"
#include "manage/VersionManager.hpp"
#include "server/ServerInstallRun.hpp"
#include "util/AppPaths.hpp"

#include <QDir>

void MainWindow::repairInstanceFiles(const QString &versionId) {
    if (versionId.isEmpty()) {
        return;
    }

    // 三道守卫，各说各的：用户点的是"检查这个实例"，说清"现在为什么不行"比一句笼统的
    // "忙"有用。第一道看的是**这个实例自己**（不等于"有下载在跑"——别的实例在装不该拦它，
    // 但这里点的是补全，而补全要开一批下载，所以第二道照样得拦）。
    const bool thisInstanceBusy = m_installOccupancy.modifying(versionId);
    if (thisInstanceBusy) {
        InlineMessage::information(
            m_ui->overlay(), tr("暂时无法操作"),
            tr("「%1」正在安装、导入或检查，请完成后再试。")
                .arg(instanceDisplayName(versionId)));
        return;
    }
    if (m_downloadInFlight || m_versionManager->isDownloading()
        || (m_serverInstall != nullptr && m_serverInstall->ownsDownloadBatch())) {
        InlineMessage::information(
            m_ui->overlay(), tr("暂时无法操作"),
            tr("已有下载或安装任务正在进行，请结束后再检查这个实例。"));
        return;
    }

    // 读不出 json / 继承链不完整：**先说清楚**，别把忙态置上再让数据层回一句内部话。
    // 尤其继承链那一条 —— "缺文件"和"父版本整个没装"是两件事，后者补不出来。
    const QString inheritanceProblem = m_versionManager->inheritanceProblem(versionId);
    if (!inheritanceProblem.isEmpty()) {
        appendLog(tr("无法校验 %1 的文件：版本继承链不完整").arg(versionId));
        InlineMessage::warning(m_ui->overlay(), tr("版本不完整"), inheritanceProblem);
        return;
    }
    const QJsonObject versionJson = m_versionManager->getVersionJson(versionId);
    if (versionJson.isEmpty()) {
        appendLog(tr("无法校验 %1 的文件：找不到 version.json").arg(versionId));
        InlineMessage::warning(
            m_ui->overlay(), tr("版本信息缺失"),
            tr("未找到 version.json，无法解析文件依赖。").arg(versionId));
        return;
    }

    // 不弹一次"确定吗"：这颗按钮自己就是那一下确认（它写着"开始检查…"，下面那句说明也把
    // 要干的事说全了），再问一遍只是让用户多点一次。真的会花时间的地方在底栏那根进度条上。
    const QStringList flagged = m_integrityController->flaggedPaths(versionId);
    appendLog(tr("正在校验「%1」的文件，跳过已存在的文件…").arg(instanceDisplayName(versionId)));
    // 顺序：**先记名字再置忙**。setDownloadUiBusy() 里那句 refreshInstalledVersions() 读的
    // 就是这个判据（正在补全的实例不该出现在启动列表里），反过来的话它漏得出那一次刷新。
    m_installOccupancy.setBatch(InstallOccupancy::Batch::Repair, versionId);
    setDownloadUiBusy(true);
    m_versionManager->repairVersionFiles(versionId, flagged);
}

void MainWindow::maybePrefetchAssetDirector(const QString &versionId) {
    if (versionId.isEmpty() || m_versionManager == nullptr) {
        return;
    }
    // 先问一句"要不要做"：绝大多数实例认不出消费者模组，那一支**连任务行都不该有**。
    // 这一步只读盘（`mods/` 文件名 + 盘上已有的对象），不发信号、不写盘。
    const VersionManager::AssetDirectorPrefetchPlan plan =
        m_versionManager->assetDirectorPrefetchPlan(versionId);
    if (!plan.applicable) {
        return;
    }
    if (!plan.refusal.isEmpty()) {
        // 超上限：说清是哪套、多大，一个字节都不下。这条只在真的点了导入时出现。
        appendLog(TextMarks::failed(tr("未预取「%1」的首次启动资源：%2"))
                      .arg(instanceDisplayName(versionId), plan.refusal));
        InlineMessage::warning(m_ui->overlay(), tr("未预取首次启动资源"),
                               tr("未预取「%1」的首次启动资源。\n\n%2\n\n游戏可正常启动，并会在首次运行时下载缺失资源。")
                                   .arg(instanceDisplayName(versionId), plan.refusal));
        return;
    }
    if (plan.upToDate) {
        appendLog(tr("「%1」的首次启动资源已齐全（%2 项），本次无需下载。")
                      .arg(instanceDisplayName(versionId))
                      .arg(plan.objects));
        return;
    }
    // 三道守卫：这个实例（或别的批次）正忙就不抢批次状态。数据层那边还会再挡一次。
    if (m_downloadInFlight || m_versionManager->isDownloading()
        || m_installOccupancy.anyModifying()) {
        appendLog(tr("其他下载或安装正在运行，已跳过「%1」的首次启动资源预取。")
                      .arg(instanceDisplayName(versionId)));
        return;
    }
    // 顺序：**先记名字再置忙**（`DownloadTaskLedgerController::begin()` 的判据就是这几个 id）。
    m_installOccupancy.setBatch(InstallOccupancy::Batch::Prefetch, versionId);
    setDownloadUiBusy(true);
    appendLog(tr("正在预取「%1」的 GTNH 首次启动资源（%2 项，预计 %3 MiB，保存到 %4）…")
                  .arg(instanceDisplayName(versionId))
                  .arg(plan.objects)
                  .arg(plan.bytesToDownload / (1024.0 * 1024.0), 0, 'f', 1)
                  .arg(QDir::toNativeSeparators(AppPaths::assetsDir() + QStringLiteral("/asset_director"))));
    if (!m_versionManager->prefetchAssetDirectorResources(versionId)) {
        // 数据层一个字节都没动（不适用 / 被拒 / 已齐）：它可能已经同步发过终态（那时上面那个
        // 分支已经收完尾了），也可能什么都没发 —— 后者要在这里把忙态和任务收回来。
        if (m_installOccupancy.is(InstallOccupancy::Batch::Prefetch, versionId)) {
            m_installOccupancy.clearBatch(InstallOccupancy::Batch::Prefetch);
            setDownloadUiBusy(false);
        }
    }
}
