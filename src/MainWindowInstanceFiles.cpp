/**
 * @file MainWindowInstanceFiles.cpp
 * @brief 实例文件与实例设置的读写接线：读一页、批量开关 / 删除、拷贝实例、导入存档、把设置落盘。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"某个实例目录里的东西"：`updateInstanceFiles()`（页键 → 哪一份页面数据）、
 * `toggleInstanceFiles()` / `deleteInstanceFiles()`、`copyInstanceTo()`（含后台拷贝）、
 * `importWorldInto()` / `renameInstanceWorld()`、`requestInstanceFileIcons()` /
 * `pushInstanceFileUpdates()`，以及实例设置落盘那一条 `applyInstanceSettings()`。
 * 从 `MainWindow.cpp` 分出来，"点了保存之后到底写了哪几个文件夹"只在一个文件里翻。
 *
 * 【判据不在这一层】
 *
 * 页键与页表的形状在 `InstanceSettingsDialog` / `InstanceFilesPage`；盘上的读写、事务登记
 * 与回收站在各自的 controller（`InstanceFileController` / `InstanceSettingsController` /
 * `InstanceTransferController`）；"哪些文件属于哪一页"的规则也在那边。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\InstanceFilesPage.hpp"
#include "..\ui\InstanceSettingsDialog.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/InstanceFileController.hpp"
#include "controllers/InstanceIconController.hpp"
#include "controllers/InstanceSettingsController.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "manage/UpdateRollback.hpp"
#include "manage/VersionManager.hpp"
#include "server/ServerInstallRun.hpp"
#include "tasks/TaskSnapshot.hpp"
#include "util/AppPaths.hpp"

#include <QDir>

using namespace MainWindowViewData;

bool MainWindow::deleteInstanceFile(const QString &path) {
    return m_instanceFileController->remove(path);
}

bool MainWindow::removeInstanceFileUnasked(const QString &path, QString *problem) {
    return m_instanceFileController->removeUnasked(path, problem);
}

// ---------- 文件管理那几页的批量按钮----------
//
// 入口全是那一页发出的**一批路径**（见 `InstanceFilesPage::selectionBar()`）。三颗按钮
// 各自一件：更新 / 禁用·启用一把 / 删除一把。**全选 / 反选 / 取消**三颗不在这儿 —— 它们
// 只动选中集，一个字节的盘都不碰，所以那边自己就办了。
//
// 【为什么「更新」是一批下，不是一条一条走 `installResourceVersion()`】
// 逐条走做不到：`downloadResourceFiles()` 头一道闸就是 `downloadManager->isBusy()`，
// 第二发会被直接拒掉 —— 那一圈里只有第一条真下得动，剩下 N-1 条全是"已有下载任务正在
// 进行"的失败框。理由写在头文件那段里。

void MainWindow::updateInstanceFiles(const QString &instanceId, const QString &pageKey,
                                     const QStringList &paths) {
    if (instanceId.isEmpty() || paths.isEmpty()) {
        return;
    }
    // 落点由**页键**算（`mods` / `datapacks` / …，唯一出处在 instanceFileDirs()）。
    const QString pageDir = instanceFileDirs(instanceId).value(pageKey);
    if (!pageKey.isEmpty() && pageDir.isEmpty()) {
        // 正常走不到：这套东西只在那四个"能下载的"页上开（见 InstanceFilesPage）——
        // 但"哪几页"这个判据在 ui/ 那一边，这边不能假设递进来的键一定认得。
        InlineMessage::warning(m_ui->overlay(), tr("更新失败"),
                               tr("不知道这一页的文件该下到哪儿去。"));
        return;
    }
    // 一趟检测的结果（实例 id → 路径 → 换成哪一版）。**没查过 = 空表** —— 那时候一条都
    // 拼不出文件清单，下面那句会拦住（见那一段）。
    const QHash<QString, ResourceCatalog::VersionEntry> targets =
        m_instanceUpdateTargets.value(instanceId);

    // 先看下载器空不空，**在摆回滚点之前**。下面那句
    // `downloadResourceFiles()` 本来就失败得快（另一个批次开着时它返回 false），可那时
    // 候回滚点已经摆好了 —— 这一批几百 MB 的文件会先被原样抄一份、再原样撤掉，用户
    // 看到的是"点了更新，卡一下，然后说没能开始"。判据和「补全文件」那道闸同一个，
    // 外加那三套安装状态机（`downloadResourceFiles()` 拒绝它们的理由见那边）和
    // "服务端正占着共用下载器"那一位（它开的是同一批，被我们 beginBatch() 顶掉之后
    // 它那一条任务永远等不到收尾）。
    if (m_downloadInFlight || m_versionManager->isDownloading()
        || m_versionManager->isLoaderInstalling() || m_versionManager->isModpackImporting()
        || m_versionManager->isNamedInstanceInstalling()
        || (m_serverInstall != nullptr && m_serverInstall->ownsDownloadBatch())) {
        InlineMessage::information(
            m_ui->overlay(), tr("暂时无法操作"),
            tr("已有下载或安装任务正在进行，请结束后再更新这几个文件。"));
        return;
    }

    QList<VersionManager::ResourceFile> files;
    QStringList skipped;
    /// 和 `files` **一一对应**的那两条：实例内相对路径（回滚点要它）和盘上那个绝对路径
    /// （收尾时按文件名找回来用）。**不是 `paths`** —— 那里面还夹着被跳过的那几条。
    QStringList relativePaths;
    QStringList updatedPaths;
    const QString instanceDir = AppPaths::versionDir(instanceId);
    for (const QString &path : paths) {
        const auto it = targets.constFind(path);
        const ResourceCatalog::FileEntry *file =
            it == targets.constEnd() ? nullptr : it->primaryFile();
        if (file == nullptr || file->url.isEmpty() || file->filename.isEmpty()) {
            // 两种来路合成一句：①这一条没在上一趟检测的结果里（用户是先勾上再点更新、
            // 而那份结果里没有它 —— 比如他勾的是一条"说不清"的）；②那一版接口没给地址
            // （CurseForge 的作者关掉了第三方分发）。
            skipped.append(QFileInfo(path).fileName());
            continue;
        }
        // 必须按**盘上这个文件自己的名字**就地换：按接口给的文件名下到"这一页那个
        // 目录"里的话，两家接口给的文件名常常和盘上那一个不一样（`sodium-fabric-0.6.13.jar`
        // vs `sodium-0.6.13.jar`）⇒ 换完之后盘上**多出一个 jar、旧的还在**，游戏同时加载
        // 两个同名模组那一类怪事就是从这儿来的，而且"按几次更新就多几份"。
        // NOTE: 这一条也让回滚点那本账成立 —— 它记的是**盘上这个文件**（相对路径），
        // 换个名字的话还原回去的就不是原来那一个了。
        const QFileInfo info(path);
        VersionManager::ResourceFile resourceFile;
        resourceFile.url = file->url;
        resourceFile.filename = info.fileName();
        resourceFile.dir = info.absolutePath();
        resourceFile.sha1 = file->sha1;
        resourceFile.size = file->size;
        files.append(resourceFile);
        // `relativePaths` 是**相对实例目录**那一条（`mods/foo.jar`，不是 `foo.jar`）——
        // 回滚点照着它去实例里找回原文件（见 UpdateRollback::create）。写成光文件名的
        // 话，回滚点会以为"原来没有这个文件"，撤销那一下做的就是**删掉它**。
        relativePaths.append(QDir(instanceDir).relativeFilePath(path));
        updatedPaths.append(path);
    }
    if (files.isEmpty()) {
        // 一条都拼不出来：多半是**还没查过**（那一页上「可更新」的计数是 0、按钮本该是灰的，
        // 所以正常也走不到这儿）。说清下一步怎么办，别只是"没反应"。
        InlineMessage::warning(m_ui->overlay(), tr("更新失败"),
                               tr("未找到这些文件的可用更新。请在「概览」中检查更新，再打开「更新中心」。"));
        return;
    }
    if (!skipped.isEmpty()) {
        // 只记账不弹框：这一批**已经开起来了**，再插一个框等于让用户白点一次。
        appendLog(tr("这几条查不到要换成哪一版，没有一起更新：%1").arg(skipped.join(QStringLiteral("、"))));
    }

    // ---------- 回滚点（「更新前创建轻量回滚点，失败时可恢复原文件」）----------
    //
    // **动手之前**摆好，摆不成就**整个停下来**（见 UpdateRollback::create 那段：用户是
    // 看着"能退回来"这句话才按下去的，抄不出来还硬更新比没有这个功能更糟）。
    //
    // NOTE: 相对路径就是 `files` 里那些**文件名**（四个"能下载的资源"目录都在实例目录的
    // 下一层，所以"这个文件在实例里的相对路径"就是它自己那个名字）—— 这一点由
    // `instanceFileDirs()` 保证（那些目录都是 `<实例目录>/<目录名>`）。
    QString rollbackProblem;
    const UpdateRollback::Snapshot snapshot =
        UpdateRollback::create(instanceDir, instanceId, relativePaths, &rollbackProblem,
                               /*keepPrevious=*/true);
    if (!snapshot.isValid()) {
        InlineMessage::warning(m_ui->overlay(), tr("没有更新"),
                               tr("无法为待更新文件创建回滚点，已取消更新。\n\n%1")
                                   .arg(rollbackProblem));
        appendLog(tr("「%1」未更新，创建回滚点失败（%2）。")
                      .arg(instanceDisplayName(instanceId), rollbackProblem));
        return;
    }
    appendLog(tr("更新前留了一份回滚点：%1 个文件（%2）。")
                  .arg(snapshot.fileCount())
                  .arg(InstanceFilesPage::formatSize(snapshot.totalBytes())));

    // ---------- 这一批的名字 + 任务账本 ----------
    //
    // 名字里带上数量 —— 收尾那句要能自己说清是哪一批。★ 这个名字**同时是**账本认这一批的
    // 钥匙（见 m_updateBatch 那段）：下载器回来的只有"文件名"，而"文件名 → 第几步"这层
    // 对应关系只有这一处知道。
    const QString label = tr("「%1」里的 %2 个更新")
                              .arg(instanceDisplayName(instanceId))
                              .arg(files.size());
    m_updateBatch = UpdateBatchRun{};
    m_updateBatch.label = label;
    m_updateBatch.instanceId = instanceId;
    // 一个资源一步（「更新任务显示每个资源的结果，不只显示总数」）。
    QList<Tasks::TaskStep> steps;
    for (int i = 0; i < files.size(); ++i) {
        const QString name = files.at(i).filename;
        steps.append({name, Tasks::TaskStepState::Pending, QString()});
        // 文件名 → 步骤下标。★ 同名多条时按**先后**取（`takeFirst()`），所以这里 append
        // 的顺序就是它们下完报回来的顺序。
        m_updateBatch.pendingSteps[name].append(i);
        // 文件名 → 盘上那条的路径（成功之后要从「可更新」名单里摘的**就是它**）。
        m_updateBatch.pathByFileName.insert(name, updatedPaths.value(i));
    }
    m_updateBatch.task = beginTask(
        Tasks::TaskKind::Download, tr("更新「%1」的 %2 个资源")
                                       .arg(instanceDisplayName(instanceId))
                                       .arg(files.size()),
        instanceId, steps);
    m_activityCenter->tasks()->setProgress(m_updateBatch.task, 0, static_cast<qint64>(files.size()));
    // 这一批的失败清单归它（`finishUpdateBatch()` 会先把 `m_updateBatch` 清空，所以
    // "重试"资格不能等到收尾时现读，见 finishTask()）。
    m_downloadTaskLedger->setOwner(m_updateBatch.task);

    appendLog(tr("正在更新 %1（%2）。")
                  .arg(label, QDir::toNativeSeparators(files.first().dir)));
    // `busyInstanceId` 给这个实例：正在往它的 `mods/` 里写文件（见 downloadResourceFiles
    // 里那段 —— 界面靠它把这一个实例从"能启动"里滤掉）。
    //
    // NOTE: 那个 `destinationDir` 形参现在只当**兜底**用（每条自己带 `dir`，见上面那段）；
    // 传第一条的目录 —— 它非空就行（那一处只判空）。
    if (!m_versionManager->downloadResourceFiles(files.first().dir, instanceId, label, files)) {
        // 没开起来（已经有别的批次在跑 / 那三套安装状态机占着）：账本里那一条**当场收尾**，
        // 别留一条永远"正在跑"的记录，回滚点也一起撤掉（一个字节都没动过，留着只会让
        // 更新中心上那颗「撤销上一次更新」指向一次没发生的更新）。
        m_activityCenter->tasks()->fail(m_updateBatch.task, tr("无法开始批量更新。"));
        m_downloadTaskLedger->clearOwnerIf(m_updateBatch.task);
        m_updateBatch = UpdateBatchRun{};
        UpdateRollback::discard(instanceId);
    } else {
        UpdateRollback::commit(instanceId);
    }
    // NOTE: 这一批下完之后**不刷新那一页**。理由：换的是同一个文件名，列表上那一行
    // 一个字都不会变；而"可更新"那几颗计数在收尾时按**真换成功的**那几条摘掉
    // （见 finishUpdateBatch）。
}

void MainWindow::toggleInstanceFiles(const QStringList &paths, bool disable) {
    m_instanceFileController->toggleMany(paths, disable);
}

void MainWindow::deleteInstanceFiles(const QStringList &paths) {
    m_instanceFileController->removeMany(paths);
}

bool MainWindow::copyInstanceTo(const QString &sourceId, const QString &destGameFolder,
                                const QString &newName) {
    return m_instanceTransferController->copyInstance(sourceId, destGameFolder, newName);
}

void MainWindow::copyInstanceToInBackground(const QString &sourceId,
                                            const QString &destGameFolder,
                                            const QString &newName) {
    m_instanceTransferController->copyInstanceInBackground(sourceId, destGameFolder, newName);
}

bool MainWindow::importWorldInto(const QString &instanceId, const QString &worldDir,
                                 const QString &name) {
    return m_instanceTransferController->importWorld(instanceId, worldDir, name);
}

bool MainWindow::renameInstanceWorld(const QString &instanceId, const QString &worldDir,
                                     const QString &newName) {
    return m_instanceTransferController->renameWorld(instanceId, worldDir, newName);
}

// ---------- 「文件管理」那几页的图标----------
//
// 入口由 InstanceDialogController 调用；扫描状态、缓存和资源映射归 InstanceIconController。

void MainWindow::requestInstanceFileIcons(InstanceSettingsDialog *dialog, const QString &versionId) {
    m_instanceIconController->requestInstanceFileIcons(dialog, versionId);
}

void MainWindow::pushInstanceFileUpdates(InstanceSettingsDialog *dialog,
                                         const QString &instanceId) {
    if (dialog == nullptr) {
        // 这一页已经收掉了（或者压根没开着）——那就不递，盘上那份账照样记着
        // （下一次开这一页时由开页那条路补上，见调用点）。
        return;
    }
    // 键在不在 = **查过没有**，值是查出来可更新的那些路径。
    //
    // 这一份账（`m_instanceOutdatedPaths`）是**检查更新**那一趟的，不是图标那一套 ——
    // 所以留在这一层，不归 InstanceIconController。
    const auto it = m_instanceOutdatedPaths.constFind(instanceId);
    const bool checked = it != m_instanceOutdatedPaths.constEnd();
    dialog->setFileUpdates(checked ? it.value() : QStringList(), checked);
}

bool MainWindow::applyInstanceSettings(const QString &versionId,
                                      InstanceSettingsDialog &dialog) {
    // 那一整套（补稳定 id → 搬归属键 → 拼字段 → 图标那几个字节 → 写 meta →
    // 判要不要重扫树 → 分组/置顶那三条规则）归 `InstanceSettingsController::apply()`。
    // 这一层剩下的是**只有它能做的那一半**：把对话框现读成一份 `Edit`（那些值），以及
    // 回答"它老家本来在哪一组"（`homeGroupIdOf()`，显示层那一套）。
    //
    // NOTE: 这个适配器只能留在这一层 —— `InstanceSettingsDialog` 是 `ui/` 的类型，而
    // controller 那一层不许 include `ui/`。所以"能验得了"这件事靠的是把 `Edit`
    // 切出来：`applyInstanceSettings()` 收对话框引用、验证工程驱动不了它，而
    // `apply()` 收的是纯值，验得了。
    const InstanceSettingsDialog::Override override = dialog.override();

    InstanceSettingsController::Edit edit;
    edit.java = toSettingsOverride(override);
    edit.extraJvmArgs = dialog.appendJvmArgs();
    edit.window = toWindowOverride(override);
    edit.warnJavaMismatch = dialog.warnJavaMismatch();
    edit.iconChoice = dialog.iconChoice();
    edit.pickedIconPath = dialog.pickedIconPath();
    // 这个常量住在 ui/ 那一侧（它是对话框上那一格显示成「本地那张」的判据）⇒ 从这里填进去，
    // controller 那边**不抄一份**。
    edit.localIconValue = InstanceSettingsDialog::localIconValue();
    edit.pageChoices = dialog.pageChoices();
    edit.pinned = dialog.pinned();
    edit.chosenGroupId = dialog.chosenGroupId();
    // "老家"要按版本分类现算 —— 那一位住在这一层（`homeGroupIdOf()`，实例树上那一行画在
    // 哪个组里用的也是它）。
    edit.homeGroupAtOpen =
        homeGroupIdOf(m_groupStore, instanceKey(versionId),
                      m_versionManager->displayParts(versionId), m_versionManager,
                      knownGroupIdsOf(m_groupStore));

    bool pickedIconStored = false;
    const bool ok = m_instanceSettings->apply(versionId, edit, &pickedIconStored);
    // 那张图**真落进实例目录了** ⇒ 让对话框忘掉源路径。留着的话，往后每一
    // 次"改了就写"（改个内存、勾个开关）都会**再从那个源文件抄一遍** —— 而那张图早就不
    // 需要了；用户把源文件挪走/删掉之后，下一次保存还会平白弹一句「图标没保存」（盘上
    // 明明已经是这张图）。meta 没写成（`!ok`）时**不清**：这一次等于没成，还得靠它重试。
    if (ok && pickedIconStored) {
        dialog.markPickedIconSaved();
    }
    return ok;
}
