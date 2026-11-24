/**
 * @file MainWindowInstanceActions.cpp
 * @brief 实例行上那几件事的接线：改名、固定 / 取消固定、删除，以及删除与服务器列表两条路的「说人话」投影。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是**一个实例本身**（不是它的文件、也不是它的分组）：`applyRename()` 那一层
 * 薄壳（验证工程按名字驱动它，见 mw_check 的 S 段）、`pinInstance()` / `unpinInstance()`、
 * `confirmDeleteInstance()` / `deleteInstance()`，以及两条把 controller 的事实说成人话的
 * 投影 —— `showInstanceSettingsNotice()` 与 `showServerListNotice()`。
 *
 * 【"说人话"那两张表的纪律】
 *
 * 标题和正文在这一层挑，controller 那边只有"哪一档、是谁、数据层说的原因"。每一句话都被
 * 验证工程按原文对数，改一个字就是回归；分出来的好处是这类改动只在一个文件里审。
 *
 * 【判据不在这一层】
 *
 * 删除的可见判据（别的账号还在跑的实例、正在下载 / 安装的实例）来自
 * `installingVersionIds()` 与 `VersionManager`；盘上的动作在 `InstanceSettingsController` /
 * `ServerListController`。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "controllers/InstanceSettingsController.hpp"
#include "controllers/ServerListController.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/VersionManager.hpp"
#include "util/AppPaths.hpp"

#include <QDir>

bool MainWindow::applyRename(const QString &oldId, const QString &newId) {
    // 为什么留着这一层而不是把调用点全改成问 controller：验证工程按**名字**驱动这个槽
    // （`QMetaObject::invokeMethod(this, "applyRename", ...)`，见 mw_check 的 S 段）——
    // 那是"这个槽驱动得起来"这条判据本身，不是实现细节。同 applyGroupList / applyInstanceSettings。
    return m_instanceSettings->rename(oldId, newId);
}

void MainWindow::showInstanceSettingsNotice(const InstanceSettingsController::Notice &notice) {
    // 这一层的活是**把事实说成人话**：controller 那边只有"哪一档、是谁、数据层说的原因"，
    // 标题和正文在这里挑。下面每一句话都被验证工程按行数/原文对数，改一个字就是回归。
    switch (notice.kind) {
    case InstanceSettingsController::Notice::Kind::RenameBlockedRunning:
        // 「先关掉游戏再改名」这句不能省：用户很可能以为"改个名字而已"，看到"拒绝访问"
        // 会去查杀毒软件。
        InlineMessage::warning(m_ui->overlay(), tr("重命名实例"),
                               tr("「%1」正在运行，请退出游戏后再重命名。")
                                   .arg(instanceDisplayName(notice.instanceId)));
        break;
    case InstanceSettingsController::Notice::Kind::RenameBlockedScanning:
        // 说清楚"等它结束"（不是"重开启动器"）：那一趟还在跑，等它自己结束就行。
        InlineMessage::warning(m_ui->overlay(), tr("重命名实例"),
                               tr("「%1」正在做完整性检查，请结束后再改名。")
                                   .arg(instanceDisplayName(notice.instanceId)));
        break;
    case InstanceSettingsController::Notice::Kind::RenameBlockedBusy:
        // 装 / 导入 / 复制 / 快照 / 还原这几类作业都攥着改名前的绝对路径。把话说全，
        // 用户才知道"等一会儿再改"是等什么。
        InlineMessage::warning(m_ui->overlay(), tr("重命名实例"),
                               tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），"
                                  "等它结束后再改名。")
                                   .arg(instanceDisplayName(notice.instanceId)));
        break;
    case InstanceSettingsController::Notice::Kind::RenameRejected:
        // `detail` 就是 `InstanceFiles::validateRenameTarget()` 给的那一整句，原样说。
        InlineMessage::warning(m_ui->overlay(), tr("重命名实例"), notice.detail);
        break;
    case InstanceSettingsController::Notice::Kind::RenameFailed:
        // 这句原因可能带"回滚到什么程度"，原样说 —— 加工它只会丢掉那个信息。
        InlineMessage::critical(m_ui->overlay(), tr("重命名失败"), notice.detail);
        break;
    case InstanceSettingsController::Notice::Kind::RenameGroupNotMoved:
        // 说清楚"名字改成了、归属没搬"：不能只弹"改名失败"，那会让人再点一次。
        InlineMessage::warning(m_ui->overlay(), tr("分组归属没跟着搬"),
                               tr("实例已重命名为「%1」，但分组未能迁移：\n%2\n\n可在「分组」中重新设置。")
                                   .arg(notice.instanceId, notice.detail));
        break;
    case InstanceSettingsController::Notice::Kind::SettingsIdFailed:
        // 连身份都写不下去 ⇒ **整次没保存**（说清这一点的同时把"为什么"也给出来：
        // 文件被占着 / 目录不可写，这两条用户自己能查）。
        InlineMessage::critical(m_ui->overlay(), tr("实例设置保存失败"),
                                tr("无法写入「%1」的 wtfml.meta，设置未保存。\n\n请检查文件是否被占用，或所在目录是否可写。")
                                    .arg(instanceDisplayName(notice.instanceId)));
        break;
    case InstanceSettingsController::Notice::Kind::SettingsGroupNotMoved:
        // 和改名那一条同款，只是没有新名字：设置**存上了**，只是归属没搬 ⇒ 不能说成
        // "没能保存"（那会让用户重来一次）。
        InlineMessage::warning(m_ui->overlay(), tr("分组归属没跟着搬"),
                               tr("「%1」的分组迁移失败：\n%2\n\n可在「分组」中重新设置。")
                                   .arg(instanceDisplayName(notice.instanceId), notice.detail));
        break;
    case InstanceSettingsController::Notice::Kind::IconNotSaved:
        // 只有图标这一项没成 —— 说清"别的东西照常保存了"（否则用户会以为整次白改）。
        InlineMessage::warning(m_ui->overlay(), tr("图标保存失败"), notice.detail);
        break;
    case InstanceSettingsController::Notice::Kind::SettingsSaveFailed:
        // 数据层不弹窗是约定，所以弹在这一层。detail 里已经写清了"是哪个文件、为什么"。
        InlineMessage::critical(m_ui->overlay(), tr("实例设置保存失败"), notice.detail);
        break;
    }
}

void MainWindow::showServerListNotice(const ServerListController::Notice &notice) {
    // 和上面那一条同款：controller 那边只有"哪一档、数据层说的原因"，标题和正文在这儿挑。
    // 每一句话都被验证工程按原文对数，改一个字都会红。
    switch (notice.kind) {
    case ServerListController::Notice::Kind::UpdateFailed:
        InlineMessage::warning(m_ui->overlay(), tr("编辑服务器失败"), notice.detail);
        break;
    case ServerListController::Notice::Kind::AddRejected:
        // 地址为空这一档**没有** detail（`detail` 是数据层给的原因，而这一档根本没走到
        // 数据层）—— 所以这句话写在弹窗这一边，和 `updateEntry` 那边是同一句。
        InlineMessage::warning(m_ui->overlay(), tr("添加服务器失败"),
                               tr("服务器地址不能为空。"));
        break;
    case ServerListController::Notice::Kind::AddFailed:
        InlineMessage::warning(m_ui->overlay(), tr("添加服务器失败"), notice.detail);
        break;
    case ServerListController::Notice::Kind::RemoveFailed:
        InlineMessage::warning(m_ui->overlay(), tr("移除服务器失败"), notice.detail);
        break;
    }
}

void MainWindow::pinInstance(const QString &versionId) {
    // 「置顶」= 把这个实例挪进**置顶组**，同时记下"它置顶之前待在哪一组"。
    // 两件事一起做是 store 那一层的实现（InstanceGroupStore::pinInstance()）——
    // 分两次写的话，中间那一份文件自相矛盾，取消置顶就会给出一个不一样的答案。
    // 哪一组是置顶组由 store 回答（清单里勾了「设为置顶组」的那一组；一个都没勾就退回名字
    // 叫「置顶」的那个默认组，那是没有勾选时的兜底；都没有则是空串）。
    //
    // 没有置顶组 ⇒ 那一格本来就**没画**（fillInstanceTree 里那一格判的就是它）。走到这儿
    // 只可能是"树画出来之后、点下去之前"用户把那一组删了。什么都不做、也不弹窗：这一下
    // 本来就没有去处，而屏幕上那颗箭头此刻也已经不在了，屏幕底下那条"已置顶"的日志更不该
    // 写出来（store 那一层同样不写盘，见 InstanceGroupStore::pinInstance()）。
    if (m_groupStore->pinnedGroupId().isEmpty()) {
        return;
    }

    // 归属键走 instanceKey()（有稳定 id 用 id、没有用目录名）—— 读和写必须同一个键，
    // 否则存下去的那一刻就对不上了（见 InstanceGroupStore 文件头）。
    QString error;
    if (!m_groupStore->pinInstance(instanceKey(versionId), &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("收藏失败"), error);
        return;
    }
    // 写成功会发 groupsChanged ⇒ 整棵树重灌（那个实例挪进置顶组、它那一行那颗上箭头消失，
    // 因为已经在里面了）。**不再手动刷一遍**：手动刷只会和信号那条路撞在一起。
    appendLog(tr("已收藏：%1").arg(instanceDisplayName(versionId)));
}

void MainWindow::unpinInstance(const QString &versionId) {
    // 「取消置顶」= 把这个实例**送回它置顶之前待的那一组**。三种记录三种落点都写在
    // InstanceGroupStore::unpinInstance() / 那个文件头那一段里：
    // 记着组 id ⇒ 回那个组；记着"明确选了未分组"⇒ 回未分组；记着 null（置顶前压根没表过
    // 态）⇒ 把那一条归属**删掉**、让显示层按版本类型重新归类。
    //
    // NOTE: **没有记录**（用户自己用「移入分组」把它塞进置顶组的，或者置顶组里一直就有的
    // 旧数据）⇒ 落未分组，和「移出分组」逐字同一个落点。这一档**不猜**它本来该归哪
    // 一组：那条信息真的不存在（从来没被记下来过），猜错的表现是"取消置顶之后它跳去
    // 了一个我没选过的组"，比"落在未分组里、用户自己再挪一次"糟得多 —— 用户直接塞的
    // 就是归未分类那一档。
    //
    // NOTE: 不判"它到底在不在置顶组里" —— 那一格只在 rowUnpinRole() 为真的行上画得出来，
    // 走到这儿就说明在。真要在外面，这一下也只是写一条"未分组"，无害。
    QString error;
    if (!m_groupStore->unpinInstance(instanceKey(versionId), &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("取消收藏失败"), error);
        return;
    }
    // 写成功会发 groupsChanged ⇒ 整棵树重灌（那个实例回到记录里那一组、它那一行那颗箭头
    // 换回朝上的）。**不再手动刷一遍**，理由同 pinInstance()。
    appendLog(tr("已取消收藏：%1").arg(instanceDisplayName(versionId)));
}

bool MainWindow::confirmDeleteInstance(const QString &versionId) {
    if (versionId.isEmpty()) {
        return false;
    }
    const QString name = instanceDisplayName(versionId);

    // 闸口一：**正在跑的不能删**。放在最前面、并且必须排在真正动手之前 —— 运行中的实例在
    // Windows 上 jar 被 JVM 锁着，删到一半才失败的话用户看到的是一句"拒绝访问"，而真正
    // 该说的是这一句（和「重命名」那一条逐字同款）。
    if (m_launcherManager->isVersionRunning(versionId)) {
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在运行，请退出游戏后再删除。").arg(name));
        return false;
    }
    // 闸口二：**正在装 / 正在导入的也不能删**。判据用的是 `installingVersionIds()` ——
    // 「现在谁动不得」的唯一出处（底栏那颗启动按钮、下拉框都是问它）。在这里删掉的后果
    // 很具体：那一路还在往这个目录里写，写到收尾那一步会**把目录重新建出来**（json /
    // meta 那两笔），用户看到的是"删掉了、过一会儿它自己又回来了，而且是半截的"。
    if (installingVersionIds().contains(versionId)) {
        // 这一份集合里不只有"正在安装"：复制 / 快照 / 还原也在这棵目录树上读写，删除
        // 会让它们写到一半的目标消失（然后被 `mkpath()` 重新建出一个半截的）。话说全。
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），"
                                 "等它结束后再删。")
                                  .arg(name));
        return false;
    }
    // 闸口三：**正在体检的也不能删**（「同一实例不允许同时扫描和删除/
    // 改名」）。后台那一趟还在拿这个实例的绝对路径 stat 文件，删到一半它就在扫一堆不存在的
    // 路径，报告里全是假的"缺文件"。
    if (integrityScanningVersion(versionId)) {
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在做完整性检查，请结束后再删。").arg(name));
        return false;
    }

    // 这一趟会动用户的东西（虽然进回收站可还原），所以先问一句。正文里把目录的
    // 完整路径写出来：用户要能看清删的是哪一个目录、它长在哪，而不是只知道"某个实例"。
    const auto answer = InlineMessage::question(
        m_ui->overlay(), tr("删除实例"),
        tr("将实例「%1」及其目录 %2 中的所有文件移入回收站。此游戏目录及其他实例会保留。\n\n确认移入回收站？")
            .arg(name, QDir::toNativeSeparators(AppPaths::versionDir(versionId))),
        tr("删除"), tr("取消"), false, /*destructive=*/true);
    if (!answer) {
        return false;  // 取消不是错误，不写日志
    }
    return deleteInstance(versionId);
}

bool MainWindow::deleteInstance(const QString &versionId) {
    // 真正动手的那一半（问的那一半在 confirmDeleteInstance() 里）。和「重命名」是同一个
    // 切法：验证工程直接驱动这一半，弹窗那一半它驱动不了。切在这里的另一个好处是
    // **守卫和动作在同一个函数里** —— 任何调用点（含以后新加的）都绕不过去。
    if (versionId.isEmpty()) {
        return false;
    }
    const QString name = instanceDisplayName(versionId);
    if (m_launcherManager->isVersionRunning(versionId)) {
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在运行，请退出游戏后再删除。").arg(name));
        return false;
    }
    // 闸口二（和上面那一半逐字同款，理由也逐字相同）：守卫和动作留在同一个函数里，
    // 任何调用点都绕不过去 —— 包括以后直接从别处调 deleteInstance() 的那些。
    if (installingVersionIds().contains(versionId)) {
        // 这一份集合里不只有"正在安装"：复制 / 快照 / 还原也在这棵目录树上读写，删除
        // 会让它们写到一半的目标消失（然后被 `mkpath()` 重新建出一个半截的）。话说全。
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），"
                                 "等它结束后再删。")
                                  .arg(name));
        return false;
    }
    // 闸口三（同上，理由见 confirmDeleteInstance 那一半）：正在体检的不能删。
    if (integrityScanningVersion(versionId)) {
        InlineMessage::warning(m_ui->overlay(), tr("删除实例"),
                              tr("「%1」正在做完整性检查，请结束后再删。").arg(name));
        return false;
    }

    QString error;
    if (!m_versionManager->deleteInstance(versionId, &error)) {
        // 失败原因是数据层给的**一整句**（可能带"删到一半"那种状态说明），原样弹出来
        // —— 和「删除游戏文件夹」那条路一个规矩。
        InlineMessage::critical(m_ui->overlay(), tr("删除实例失败"), error);
        return false;
    }

    // 分组归属**故意不动**：wtfml.folders 里可能还留着这个实例的一条归属（它指向的组还在，
    // 所以不会被 store 的清成员规则扫掉）。留着是有意的，理由是**它的代价是零**：
    // - 它对界面没有任何影响：归属只有在"这个实例还在"的时候才会被问到，而那张树是
    // 按**盘上的实例**建的（遍历 installedVersions()，每条再拿 instanceKey() 去问归属）
    // ⇒ 指向已删实例的那些条目从来不会被遍历到。「组名 (N)」里的 N 也是从树上数的，
    // 不是从归属表里数的。换句话说：既没有幽灵行，也没有虚高的计数。
    // - 为它写一条"删实例时顺手清归属"要新加一个 store 的 API（现在只有整份
    // replaceGroups()），换来的只是少几个字节的死数据。
    //
    // 注意：**别拿"重装能回原组"当理由**。归属的键是 instanceKey() = `wtfml.meta` 里的
    // 稳定 id（没有才退回目录名），而 deleteInstance() 把整个目录连 `wtfml.meta` 一起删了
    // ⇒ 重装出来的是一个**没有 id 的新目录**，键退回目录名，而那条残留的键是旧 UUID，
    // 永远不可能再对上。那个 id 是**惰性生成**的：用户只要开过一次实例设置按过确定
    // （ensureInstanceId()）或者改过名就会落一个 —— 所以"重装回原组"只对**从来没拿过 id
    // 的老实例**（键就是目录名）成立，其余的残留就是永远的死数据。
    // ⇒ 上面那两条"代价为零"的理由照样成立。
    //
    // 真嫌它脏的话，下一次在「分组设置」里按确定会整份重写这个文件（那时成员表按现有的
    // 组重建，指向已删实例的那些条目照旧保留）。
    refreshInstalledVersions();
    appendLog(tr("已删除实例：%1").arg(versionId));
    return true;
}
