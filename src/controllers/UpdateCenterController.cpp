/**
 * @file UpdateCenterController.cpp
 * @brief 「更新中心」那一页的状态、翻译与四个动作（见 .hpp 里那三段说明）。
 *
 * 【这一段原来的形状，原样带过来】
 *
 * 判据在 `src/manage/UpdatePolicy`（"这一条现在要不要提示"）、回滚在
 * `src/manage/UpdateRollback`（"动手之前先留一份旧的"）、页面在 `ui/UpdateCenterPage`
 * （只画不判）。这一层夹在中间，干的只有三件事：
 * ① 把上面那一趟检测的原始结论翻成页面认的那一份（`reportFor()`）；
 * ② 用户按下去之后把它翻成一个动作（更新 / 记一笔忽略固定 / 还原）；
 * ③ 每一批更新起一个任务账本条目，并**逐个资源**回报结果。
 *
 * NOTE: ③ 那一半（`m_updateBatch`、`onUpdateBatchFileFinished()` / `finishUpdateBatch()`）
 * 仍留在 `MainWindow`：它挂在下载器回来的那条路上，和这一页开没开无关。
 *
 * 【为什么忽略 / 固定一动就要重算一遍名单】
 *
 * 「可更新」那一档在文件管理页上是个**筛选**（`m_instanceOutdatedPaths`）。不重算的话，
 * 用户刚忽略掉的那一条仍然算在"可更新 3 条"里 —— 而他明明看着那一行写着「已忽略这一版」。
 */

#include "controllers/UpdateCenterController.hpp"
#include <QCoreApplication>
#include "manage/InstanceFiles.hpp"

#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "InlineMessage.hpp"
#include "InstanceFilesPage.hpp"
#include "OverlayHost.hpp"
#include "download/ModDependencies.hpp"
#include "manage/UpdateRollback.hpp"
#include "util/AppPaths.hpp"

#include <QFileInfo>
#include <QHash>
#include <QTimer>

UpdateCenterController::UpdateCenterController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

void UpdateCenterController::open(const QString &instanceId) {
    OverlayHost *overlay = m_owner.m_ui->overlay();
    if (overlay == nullptr || instanceId.isEmpty()) {
        return;
    }
    if (m_page == nullptr) {
        // 父对象是主窗口、不是这一层：这一页在覆盖层里的生命周期和别的那几页一样，
        // 归主窗口管（这一层只是拿着一个不拥有的指针）。
        m_page = new UpdateCenterPage(&m_owner);
        connect(m_page, &UpdateCenterPage::updateRequested, this,
                &UpdateCenterController::onUpdateRequested);
        connect(m_page, &UpdateCenterPage::ignoreToggled, this,
                &UpdateCenterController::onIgnoreToggled);
        connect(m_page, &UpdateCenterPage::pinToggled, this,
                &UpdateCenterController::onPinToggled);
        connect(m_page, &UpdateCenterPage::undoRequested, this,
                &UpdateCenterController::onUndoRequested);
        // 收页钩子（见 `OverlayHost::pageClosed()`）：这一页收掉之后"说的是谁"不再作数。
        // 以前是靠 `execPage()` 返回之后那一句清的，现在开页不等它了，就在收页这一发上清。
        // 认页面对象：别的页开着的时候不会误伤。
        connect(overlay, &OverlayHost::pageClosed, this, [this](QWidget *page) {
            if (page == m_page) {
                m_instanceId.clear();
            }
        });
    }
    // 这一页说的是哪个实例：**动作处理函数全靠它**（页面是复用的，而点动作时它可能已经
    // 关了 —— 不许去问"现在选中的是哪个实例"，同 m_integrityReportVersionId 那条）。
    m_instanceId = instanceId;
    // 摆结论。现在开页是**非阻塞**的（`OverlayHost::showPage()`）：这一页自己不等任何
    // 结果，收页时上面那条钩子把 `m_instanceId` 清掉。
    m_page->setReport(reportFor(instanceId));
    if (!overlay->showPage(m_page, UpdateCenterPage::windowTitle())) {
        // 摆不上（这一层刚被别人占上）：这一页说的实例当场作废，留一句日志。
        m_instanceId.clear();
        m_owner.appendLog(MainWindow::tr("无法打开更新中心：界面已被另一页占用。"));
    }
}

UpdateCenterPage::Report UpdateCenterController::reportFor(const QString &instanceId) const {
    // **唯一翻译点**（和 integrityReportForPage() / taskCenterItem() 同一个路数）。
    UpdateCenterPage::Report out;
    out.instanceName = m_owner.instanceDisplayName(instanceId);

    const auto result = m_owner.m_instanceUpdateResults.constFind(instanceId);
    if (result == m_owner.m_instanceUpdateResults.constEnd()) {
        // 没查过：这一页**没有结论**可摆（列表整个空着），用户要看到的是这句话 + 下一步。
        out.error = MainWindow::tr("此实例尚未检查更新。");
        return out;
    }
    const QString instanceDir = AppPaths::versionDir(instanceId);
    int outdatedCount = 0;
    for (const ModDependencies::UpdateVerdict &verdict : result.value().verdicts) {
        UpdateCenterPage::Row row;
        row.path = verdict.item.path;
        row.fileName = QFileInfo(verdict.item.path).fileName();
        // 名字取不到时就空着 —— 页面会退回文件名（`updateCenterRowLabelText()`，
        // 和下面日志里那个 displayName 逐字同一个规矩）。
        row.name = verdict.item.title;
        row.currentSha1 = verdict.item.sha1;
        switch (verdict.state) {
        case ModDependencies::UpdateState::Outdated:
            row.state = UpdateCenterPage::State::Outdated;
            row.targetVersionId = verdict.latestVersionId;
            row.targetText = verdict.latestVersionNumber;
            ++outdatedCount;
            break;
        case ModDependencies::UpdateState::UpToDate:
            row.state = UpdateCenterPage::State::UpToDate;
            // 「已是最新（1.2.3）」—— 版本号就是判"最新"时那一条（`judgeUpdate()` 看的是
            // 列表第一条，`latestVersionNumber` 记的正是它）。
            row.currentText = verdict.latestVersionNumber;
            break;
        case ModDependencies::UpdateState::Unknown:
            row.state = UpdateCenterPage::State::Unknown;
            row.note = verdict.reason;
            break;
        }
        // 「忽略 / 固定」那一档：**这一处算，页面不再判一遍**（判两遍迟早对不上：用户会
        // 看到那一行写着"已忽略"、而它还挂在"可更新"那一组的计数里）。
        const UpdatePolicy::Entry entry = UpdatePolicy::entryOf(
            m_policy, instanceId,
            UpdatePolicy::storeKey(instanceDir, InstanceFilesPage::identityPath(row.path)));
        switch (UpdatePolicy::suppressed(entry, row.currentSha1, row.targetVersionId)) {
        case UpdatePolicy::Suppress::Ignored:
            row.suppress = UpdateCenterPage::Suppress::Ignored;
            break;
        case UpdatePolicy::Suppress::Pinned:
            row.suppress = UpdateCenterPage::Suppress::Pinned;
            break;
        case UpdatePolicy::Suppress::None:
            break;
        }
        out.rows.append(row);
    }
    out.summary = ModDependencies::updateSummaryText(result.value().verdicts.size(), outdatedCount);
    out.note = result.value().lookupNote;

    // 回滚点：**每次开页都重新读一遍盘**（不缓存 —— 撤销那一下就是把它用掉，缓存一份就
    // 会出现"按了撤销、那一颗还在"）。
    bool hasSnapshot = false;
    const UpdateRollback::Snapshot snapshot = UpdateRollback::read(instanceId, &hasSnapshot);
    if (hasSnapshot) {
        out.undoFileCount = snapshot.fileCount();
        out.undoText = MainWindow::tr("撤销上一次更新（%1 个文件）").arg(snapshot.fileCount());
    }
    return out;
}

void UpdateCenterController::rebuildOutdatedPaths(const QString &instanceId) {
    if (instanceId.isEmpty()) {
        return;
    }
    // 没查过 = 什么都不动。★ **不能**在那儿写一个空名单："键在不在 = 查过没有"（见
    // m_instanceOutdatedPaths 那段），塞一个空表进去等于替用户说了一句"查过了，都是最新"。
    const auto result = m_owner.m_instanceUpdateResults.constFind(instanceId);
    if (result == m_owner.m_instanceUpdateResults.constEnd()) {
        return;
    }
    const QString instanceDir = AppPaths::versionDir(instanceId);

    QStringList outdatedPaths;
    for (const ModDependencies::UpdateVerdict &verdict : result.value().verdicts) {
        if (verdict.state != ModDependencies::UpdateState::Outdated) {
            continue;
        }
        // 判据**只此一处**（见 UpdatePolicy 文件头：忽略记的是"那一版"、固定记的是"那一份"）。
        // 钥匙先摘掉 `.disabled` —— 和文件管理页那把钥匙逐字同一个（`identityPath()`）。
        const UpdatePolicy::Entry entry = UpdatePolicy::entryOf(
            m_policy, instanceId,
            UpdatePolicy::storeKey(instanceDir, InstanceFilesPage::identityPath(verdict.item.path)));
        if (UpdatePolicy::suppressed(entry, verdict.item.sha1, verdict.latestVersionId)
            != UpdatePolicy::Suppress::None) {
            continue;
        }
        outdatedPaths.append(verdict.item.path);
    }
    m_owner.m_instanceOutdatedPaths.insert(instanceId, outdatedPaths);

    // 这一页要是正开着就**当场重摆**（用户刚按下的就是它上面那一行）。
    refreshPageIfOpen(instanceId);
}

bool UpdateCenterController::savePolicy() {
    QString problem;
    if (UpdatePolicy::save(m_policy, UpdatePolicy::storePath(), &problem)) {
        return true;
    }
    // 写不成**只写日志**：用户按的是一颗"以后别提示我"，弹一个"存不进去"的框除了让他分心
    // 以外没有下一步可做 —— 而那一份记录在这一次会话里仍然是生效的（内存里那份还在）。
    m_owner.appendLog(MainWindow::tr("无法保存更新中心的记录：%1").arg(problem));
    return false;
}

void UpdateCenterController::refreshPageIfOpen(const QString &instanceId) {
    if (m_page != nullptr && m_instanceId == instanceId) {
        m_page->setReport(reportFor(instanceId));
    }
}

void UpdateCenterController::onUpdateRequested(const QStringList &paths) {
    const QString instanceId = m_instanceId;
    if (instanceId.isEmpty() || paths.isEmpty()) {
        return;
        // 改过主类/JVM 参数的整合包（lwjgl3ify 一类）：换模组可能让启动配置对不上，先确认。
        {
            bool customLaunch = false;
            QFile versionJson(InstanceFiles::jsonPath(instanceId));
            if (versionJson.open(QIODevice::ReadOnly)) {
                const QString mainClass = QJsonDocument::fromJson(versionJson.readAll())
                    .object().value(QStringLiteral("mainClass")).toString();
                static const QSet<QString> standard{
                    QStringLiteral("net.minecraft.client.main.Main"),
                    QStringLiteral("net.minecraft.launchwrapper.Launch"),
                    QStringLiteral("cpw.mods.bootstraplauncher.BootstrapLauncher"),
                    QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient")};
                customLaunch = !mainClass.isEmpty() && !standard.contains(mainClass);
            }
            if (customLaunch
                && !InlineMessage::question(
                    m_owner.m_ui->overlay(), tr("这个整合包带自定义启动配置"),
                    tr("更新所选模组可能影响主类、JVM 参数或专用库，导致实例无法启动。\n\n建议先备份实例。确认继续？"),
                    tr("仍要继续"), tr("取消"), false)) {
                return;
            }
        }
    }
    // 先把这一页收掉（收页本身是同步的，但这一发正站在页面自己的槽里），**排到下一轮**
    // 再动手 —— 更新那一批失败了会往同一条覆盖层上弹提示，当场弹会压在还没退栈的页面上。
    // （和完整性报告页那颗「补齐…」是同一个路数。）
    if (m_page != nullptr) {
        m_page->reject();
    }
    const QStringList selected = paths;
    QTimer::singleShot(0, this, [this, instanceId, selected] {
        // 页键给空串：这一批是**跨类**勾出来的，落点全按每条自己那个目录算（见头文件那段）。
        m_owner.updateInstanceFiles(instanceId, QString(), selected);
    });
}

void UpdateCenterController::onIgnoreToggled(const QString &path, bool ignored) {
    const QString instanceId = m_instanceId;
    if (instanceId.isEmpty() || path.isEmpty()) {
        return;
    }
    const QString key = UpdatePolicy::storeKey(
        AppPaths::versionDir(instanceId), InstanceFilesPage::identityPath(path));
    if (key.isEmpty()) {
        return;  // 不在实例目录底下（正常走不到）：宁可不记，也不拿一个空键去混记录
    }
    // 要忽略的是**要换上的那一版**（不是"这个项目"）—— 那一版的 id 从上一趟检测的原始
    // 结论里取（`m_instanceUpdateTargets` 那一份是**全部**可更新的，忽略过的那条也在里面）。
    QString targetVersionId;
    if (ignored) {
        const QHash<QString, ResourceCatalog::VersionEntry> targets =
            m_owner.m_instanceUpdateTargets.value(instanceId);
        const auto it = targets.constFind(path);
        if (it != targets.constEnd()) {
            targetVersionId = it.value().id;
        }
        if (targetVersionId.isEmpty()) {
            // 拼不出"要换成哪一版"就**不许记这一条**：记下去的是"忽略某一版"，而那一版是谁
            // 都不知道 —— 那样记出来的记录永远对不上任何一次检测（等于一颗点了没反应的按钮）。
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("无法忽略这一条"),
                                   MainWindow::tr("未找到可固定的版本。请重新检查更新。"));
            return;
        }
    }
    UpdatePolicy::setIgnored(m_policy, instanceId, key, ignored ? targetVersionId : QString());
    savePolicy();
    m_owner.appendLog(
        ignored ? MainWindow::tr("「更新中心」：%1 的这一版以后不再提示更新。")
                      .arg(QFileInfo(path).fileName())
                : MainWindow::tr("「更新中心」：%1 又参与更新提示了。").arg(QFileInfo(path).fileName()));
    rebuildOutdatedPaths(instanceId);
    // 按完**当场重喂这一页**。不喂的话那一行照旧写着「可更新到 X」、三颗
    // chip 也照旧——用户按的是"以后别再提示我"，屏幕上却一点变化都没有，他只能靠"再点
    // 一次看看会不会还写那句话"来判断自己按中了没有。重喂之后那一行写「已忽略」、那一颗
    // 变成「不再忽略」、底下那颗「更新选中的 N 项」的计数也跟着减一。
    // （`setReport()` 的重建走 `deleteLater()`，所以从 chip 自己的 clicked 里重喂是安全的；
    // 撤销那条路同样这么做。）
    // NOTE: 上面 `rebuildOutdatedPaths()` 已经做过这一件（`refreshPageIfOpen()`），这里不再重复。
}

void UpdateCenterController::onPinToggled(const QString &path, bool pinned) {
    const QString instanceId = m_instanceId;
    if (instanceId.isEmpty() || path.isEmpty()) {
        return;
    }
    const QString key = UpdatePolicy::storeKey(
        AppPaths::versionDir(instanceId), InstanceFilesPage::identityPath(path));
    if (key.isEmpty()) {
        return;
    }
    // 固定记的是**盘上那一份现在的内容 sha1**（见 UpdatePolicy 文件头：判据手上只有 sha1）。
    // 算不出来（文件读不动）就**不许记** —— 记一条空 sha1 等于记了一条永远判不出来的记录。
    QString sha1;
    if (pinned) {
        for (const ModDependencies::UpdateVerdict &verdict :
             m_owner.m_instanceUpdateResults.value(instanceId).verdicts) {
            if (verdict.item.path == path) {
                sha1 = verdict.item.sha1;
                break;
            }
        }
        if (sha1.isEmpty()) {
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("固定不了这一条"),
                                   MainWindow::tr("无法读取文件或计算其 SHA-1，因此无法固定此版本。"));
            return;
        }
    }
    UpdatePolicy::setPinned(m_policy, instanceId, key, pinned ? sha1 : QString());
    savePolicy();
    m_owner.appendLog(
        pinned ? MainWindow::tr("「更新中心」：%1 固定在这一版上，以后不再提示更新。")
                     .arg(QFileInfo(path).fileName())
               : MainWindow::tr("「更新中心」：%1 不再固定了。").arg(QFileInfo(path).fileName()));
    // 同上（忽略那一档）：按完当场重喂，让那一行写成「已固定」、chip 翻成「不再固定」——
    // `rebuildOutdatedPaths()` 里那一下就是它。
    rebuildOutdatedPaths(instanceId);
}

void UpdateCenterController::onUndoRequested() {
    const QString instanceId = m_instanceId;
    if (instanceId.isEmpty()) {
        return;
    }
    bool hasSnapshot = false;
    QString problem;
    const UpdateRollback::Snapshot snapshot =
        UpdateRollback::read(instanceId, &hasSnapshot, &problem);
    if (!hasSnapshot) {
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("没有可撤销的更新"),
                               problem.isEmpty() ? MainWindow::tr("这个实例没有回滚点。") : problem);
        return;
    }
    // 问一次再动手：这一下**会覆盖盘上现在的文件**（用户可能已经在游戏里玩了半天、
    // 自己换过东西）。撤销不是一个"点错了也没事"的动作。
    const QString text =
        MainWindow::tr("将撤销「%1」的上次更新。\n\n• 恢复 %2 个文件\n• 移除更新新增的 %3 个文件\n\n更新后对这些文件所做的修改将被覆盖。")
            .arg(m_owner.instanceDisplayName(instanceId))
            .arg(snapshot.fileCount())
            .arg(snapshot.addedCount());
    // 就一颗「继续」：这一问的目的是把"会发生什么"说在前面（不是二选一），所以
    // `destructiveIndex` 留空 —— 撤销本身是要用户想清楚才按的，但把它标成一档"危险动作"
    // 的红按钮，和这一段想说的"可以退回去"反而矛盾。
    if (InlineMessage::ask(m_owner.m_ui->overlay(), MessageKind::Warning,
                           MainWindow::tr("撤销上一次更新"), text, {MainWindow::tr("撤销")}, 0, -1)
        != 0) {
        return;
    }

    const UpdateRollback::RestoreResult result =
        UpdateRollback::restore(snapshot, AppPaths::versionDir(instanceId));
    m_owner.appendLog(MainWindow::tr("撤销「%1」上一次更新：换回去 %2 个、删掉 %3 个、失败 %4 个。")
                          .arg(m_owner.instanceDisplayName(instanceId))
                          .arg(result.restored)
                          .arg(result.removed)
                          .arg(result.failed));
    if (!result.ok()) {
        // **一条失败不影响其余**（见 UpdateRollback::restore 那段）：照实说清现在是半新半旧，
        // 并且**留着回滚点**（下一次再按还能把没退回去的那几条退回来）。
        InlineMessage::warning(
            m_owner.m_ui->overlay(), MainWindow::tr("部分文件回滚失败"),
            MainWindow::tr("已恢复 %1 个文件，移除 %2 个文件，%3 个文件操作失败。\n\n%4\n\n回滚点已保留，可重试。")
                .arg(result.restored)
                .arg(result.removed)
                .arg(result.failed)
                .arg(result.problems.join(QStringLiteral("\n"))));
    } else {
        // 全退回去了：这一份回滚点**用掉了**（删掉 —— 留着的话下一次开页还会露一颗
        // 「撤销上一次更新」，而它指向的那次更新已经不存在了）。
        UpdateRollback::discard(instanceId);
        m_owner.appendLog(
            MainWindow::tr("「%1」已经退回更新前的样子。").arg(m_owner.instanceDisplayName(instanceId)));
    }
    // 这一批文件的内容换过了 ⇒ 「固定这一版」那几条的判据（sha1）多半已经对不上，
    // 顺手清一遍过期的（不清的话那几条会一直挂着"已固定"，而它其实早就失效了）。
    QHash<QString, QString> sha1ByKey;
    for (const ModDependencies::UpdateVerdict &verdict :
         m_owner.m_instanceUpdateResults.value(instanceId).verdicts) {
        const QString entryKey = UpdatePolicy::storeKey(
            AppPaths::versionDir(instanceId), InstanceFilesPage::identityPath(verdict.item.path));
        if (!entryKey.isEmpty()) {
            sha1ByKey.insert(entryKey, verdict.item.sha1);
        }
    }
    if (UpdatePolicy::dropStalePins(m_policy, instanceId, sha1ByKey) > 0) {
        savePolicy();
    }
    // 重摆这一页 + 让文件管理那几页的「可更新」跟着更新（盘上那几个文件已经不是原来那份了）。
    rebuildOutdatedPaths(instanceId);
    // 文件管理页那一份"可更新"名单也得重摆（它记的是路径，路径没变，但内容变了）。
    // NOTE: 这一页现在没开着（撤销是从更新中心按的，而更新中心占着覆盖层），所以这里
    // 只需要把**会话缓存**写对 —— 下一次开那一页时由开页那条路补上。
}


