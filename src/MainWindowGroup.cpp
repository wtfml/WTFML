/**
 * @file MainWindowGroup.cpp
 * @brief 实例分组：组行上的动作、「分组设置」那一扇窗、组成员与改名 / 删除 / 挪序。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语只有一个：`InstanceGroupStore` 里那几张表（组、成员、展开状态）。
 * 从 `onGroupInstanceClicked()`（开「分组设置」）到 `openGroupMenuFor()`（组行行尾那颗
 * 齿轮的菜单），中间每一件事的落点都是那个 store：开窗时的快照、确定时整份重写、
 * 「清空成员」、把选中的实例挪进 / 挪出组、改名、删除、记住展开。从实例树那一摊里分出来，
 * "这个组里到底有谁"只在一个文件里翻。
 *
 * 【判据不在这一层】
 *
 * 组与成员的规则在 `manage/InstanceGroupStore`（含五个默认组的种子）；
 * 组行的几何（齿轮占哪一格、三角画在哪）在 `ui/InstanceGroupDelegate`；
 * 快照与"哪一格灰着"在 `controllers/MainWindowViewData` 的 `groupRowFor` 那一家。
 */

#include "MainWindow.hpp"

#include "..\ui\GroupDialog.hpp"
#include "..\ui\InlineMessage.hpp"
#include "..\ui\InstanceGroupDelegate.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\OverlayHost.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/VersionManager.hpp"

#include <QCursor>
#include <QDialog>
#include <QMenu>
#include <QTreeWidgetItem>

using namespace MainWindowViewData;

void MainWindow::onGroupInstanceClicked() {
    // 被占着就先让位（静默收掉那一层 + 排到下一轮），见 runWhenOverlayFree 那段。
    runWhenOverlayFree([this] { openGroupDialogNow(); });
}

void MainWindow::openGroupDialogNow() {
    // 每个组里有多少个实例：从**当前那张树**上数，不重新扫盘。它就是用户此刻看到的
    // 东西，而对话框拿它显示「组名 (N)」和"删这个组会有多少实例变成未分组"——
    // 两处都该和屏幕上的数字一致。
    //
    // NOTE: 一个实例都没有的组**不在树上**（"空白的分组隐藏"）⇒ 这里数出来是 0，
    // 那也正是实话；它在对话框里照样列着、照样能改名排序，只是括号里写着 0。
    QHash<QString, int> counts;
    const QTreeWidget *tree = m_ui->getInstanceList();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = tree->topLevelItem(i);
        const QVariant groupId = item->data(0, MainWindowUI::groupIdRole());
        if (groupId.isValid()) {
            counts.insert(groupId.toString(), item->childCount());
        }
    }

    // 「恢复默认分组设置」之后每个组里会有几个实例：恢复 = 用户自己建的组全没了 ⇒ 那些
    // 实例回到"没表过态"，于是按版本类型自动归类（defaultGroupIdFor()）—— 落点只有这里
    // 算得出来，而对话框要在那一句话里说清"是不是真的一个实例都不删"。
    //
    // 取的实例集合和上面那张树是同一份（在装的那几个不算）：两处数的要是同一批东西，
    // 不然"恢复之后原版有 3 个"和屏幕上看到的数字对不上。
    const QList<InstanceGroupStore::Group> defaults = InstanceGroupStore::defaultGroups();
    QSet<QString> defaultIds;
    for (const InstanceGroupStore::Group &group : defaults) {
        defaultIds.insert(group.id);
    }
    const QSet<QString> installing = installingVersionIds();
    QHash<QString, int> defaultCounts;
    for (const QString &id : m_versionManager->installedVersions()) {
        if (installing.contains(id)) {
            continue;
        }
        const QString groupId =
            defaultGroupIdFor(m_versionManager->displayParts(id), m_versionManager, defaultIds);
        defaultCounts[groupId] = defaultCounts.value(groupId) + 1;
    }

    // NOTE: 这张窗**不读选中的实例** ——
    // 一个实例都没选也开得出来，那是"先建几个组、再往里放东西"的正常顺序。
    // 把实例放进某个组是组行行尾那颗齿轮的事（见 onGroupActionRequested()）。
    // NOTE: 这张页**每次开都新建一份**（组清单 / 人数是开页那一刻的快照，留着重开会
    // 拿旧数字）。它不归 `m_groupStore` 也不归覆盖层：收页之后由下面那条钩子
    // `deleteLater()` 收掉。
    auto *dialog = new GroupDialog(toDialogGroups(m_groupStore->groups()), counts,
                                   toDialogGroups(defaults), defaultCounts, this);
    m_groupDialog = dialog;
    // 整页盖在内容区上（见 OverlayHost::showPage：摆上去就返回，不卡调用方）。
    // 这一页自己没有标题行，页眉那句就是它唯一一处标题（和它自己的窗口标题同一个出处）。
    //
    // 收页 = 用户把这一页收掉（确定 / 取消 / 返回箭头 / Esc / 被别的页顶掉 —— 全汇到
    // `pageClosed()`）。只有「确定」才落盘；其余几档磁盘一个字节都没动（对话框只改自己
    // 手里那份工作副本）。`result()` 和 `execPage()` 的返回值是同一套：确定 = Accepted。
    //
    // NOTE: 接在 `dialog` 上（不是 `this` 上）当上下文：这一页收掉之后对话框就
    // `deleteLater()` 了，连接跟着一起没；不接在 this 上的话，每开一次就多留一条永远
    // 不会匹配上的连接。
    connect(
        m_ui->overlay(), &OverlayHost::pageClosed, dialog,
        [this, dialog](QWidget *page) {
            if (page != dialog) {
                return;
            }
            m_groupDialog = nullptr;
            if (dialog->result() == QDialog::Accepted) {
                applyGroupList(dialog);
            }
            dialog->deleteLater();
        },
        Qt::SingleShotConnection);
    if (!m_ui->overlay()->showPage(dialog, GroupDialog::windowTitle())) {
        // 覆盖层刚被另一页占上了（正常走不到：入口已经先 `runWhenOverlayFree()` 让过位）。
        m_groupDialog = nullptr;
        dialog->deleteLater();
        appendLog(tr("无法打开分组设置：界面已被另一页占用。"));
    }
}

bool MainWindow::applyGroupList(const GroupDialog *dialog) {
    if (dialog == nullptr) {
        return false;
    }
    const QList<InstanceGroupStore::Group> wanted = toStoreGroups(dialog->groups());

    // 变了才写：每点一次「确定」都重写一遍文件（连带刷新 mtime）而其实什么都没改，是
    // 最常见的一次操作。比较是**逐字段**的（连"展开还是收着"那一格一起比）—— 用户完全
    // 可能只动了那一格，而它一样要留住。
    if (wanted == m_groupStore->groups()) {
        return true;
    }
    QString error;
    if (!m_groupStore->replaceGroups(wanted, &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("分组保存失败"), error);
        return false;
    }
    // NOTE: 这里**不**手动重灌那张树。这条写路径成功之后数据层会发 groupsChanged，
    // 界面接的是同一个刷新函数（见 setupConnections）—— 在这里再来一次就是
    // 同一件事做两遍，而且"哪些路径会刷新"会变成两处各自维护的约定。
    return true;
}

void MainWindow::onGroupActionRequested(const QString &groupId, int action) {
    // 这一组此刻的样子（名字、实例数、展开状态）。**动作菜单里每一格都用的同一份**：
    // 菜单是照快照摆的，而这里要的是"现在"—— 菜单开着的时候别处可能已经把它删了，
    // 那几种情况一律当"什么都没发生"（点了个过期的菜单项不该弹一个用户看不懂的框）。
    MainWindowUI::GroupRow row;
    if (!groupRowFor(m_ui, m_groupStore, groupId, &row)) {
        return;
    }

    switch (action) {
    case MainWindowUI::GroupActionMoveInto: {
        // "把选中的实例移进哪一组"：列的是**全部**分组（含树上一个实例都没有、藏起来的
        // 那些）—— 那是把实例放进一个空组的唯一入口（见 MainWindowUI::openGroupPicker()）。
        const QString selectedId = selectedVersionId(m_ui->getInstalledVersionCombo());
        if (selectedId.isEmpty()) {
            InlineMessage::information(m_ui->overlay(), tr("移入分组"), tr("先选中一个实例。"));
            return;
        }
        // 弹在**原来那颗齿轮**下面（用户看到的是"刚才那块框换了个内容"）。那一行理论上
        // 一定还在（菜单就是从它那儿弹出来的）；万一不在了就退回光标那里，框照样出得来，
        // 不会神秘消失。
        QRect anchor = gearAnchorFor(m_ui, groupId);
        if (anchor.isEmpty()) {
            anchor = QRect(QCursor::pos(), QSize(1, 1));
        }
        // 灰哪一格按那个实例的**老家**判（不是"它那一条画在哪个组下面"—— 置顶着的有两行，
        // 点哪一行会给出不同答案，理由见 groupRowsForMenu() 那条 NOTE）。
        const QString selectedKey = instanceKey(selectedId);
        m_ui->openGroupPicker(
            groupRowsForMenu(m_ui, m_groupStore, selectedKey,
                             homeGroupIdOf(m_groupStore, selectedKey,
                                           m_versionManager->displayParts(selectedId),
                                           m_versionManager, knownGroupIdsOf(m_groupStore))),
            anchor);
        return;
    }
    case MainWindowUI::GroupActionClearMembers:
        clearGroupMembers(groupId, row.name);
        return;
    case MainWindowUI::GroupActionRename: {
        // 「问」和「做」分开（和实例页那颗「重命名…」同一个拆法）：做的那一半
        // renameGroupTo() 能在验证工程里直接驱动。
        //
        // NOTE: 拆成两步的道理是"做"那一半（renameGroupTo()）不用开界面就能验。
        // 问名字那一格是 InlineMessage::getText()，验证工程能按 objectName 找到
        // QLineEdit、真的把字打进去再点确定（见 InlineMessageBox::setLineEdit）。
        bool ok = false;
        const QString name =
            InlineMessage::getText(m_ui->overlay(), tr("重命名分组"), tr("组名："), row.name, &ok);
        if (ok) {
            renameGroupTo(groupId, name);
        }
        return;
    }
    case MainWindowUI::GroupActionToggleExpand: {
        // 先摆树里那一行，再记 —— rememberGroupExpanded() 只管写盘（见它的 NOTE），
        // 所以"这一下要让用户看见什么"是调用方的事。两处管的是同一格，摆完就是它。
        //
        // NOTE: 状态从**那一行自己**读（`!item->isExpanded()`）而不是拿菜单那份快照里的
        // `row.expanded`：菜单是弹出来的那一刻拍的，中间用户完全可能又点了一下组头
        // （那一下也会落盘）。和版本树/加载器树那几处"先算再做"同一个教训。
        QTreeWidget *tree = m_ui->getInstanceList();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            if (item->data(0, MainWindowUI::groupIdRole()).toString() != groupId) {
                continue;
            }
            const bool expand = !item->isExpanded();
            item->setExpanded(expand);
            rememberGroupExpanded(groupId, expand);
            return;
        }
        // 那一行不在树上（这一组空了 —— 空组是藏着的 —— 或者刚被别处删了）：没有"看得见的
        // 那一行"要摆，把状态记下来就好。
        rememberGroupExpanded(groupId, !row.expanded);
        return;
    }
    case MainWindowUI::GroupActionDelete: {
        // 先问再删，而且说清**实例不会被删**：用户看到"删除"两个字时的第一反应就是怕
        // 连里面的东西一起没了，而这里确实不会（和对话框里那句一模一样）。
        const bool answer = InlineMessage::question(m_ui->overlay(), tr("删除分组"),
            tr("「%1」会被删除，组里的 %2 个实例不会被删掉，它们只是变成未分组。\n\n"
               "确定要删除吗？")
                .arg(row.name)
                .arg(row.memberCount), tr("是"), tr("否"), false, /*destructive=*/true);
        if (answer) {
            deleteGroupById(groupId);
        }
        return;
    }
    }
    // 越界（理论上到不了：菜单是照 MainWindowUI::groupActionCount() 循环摆的）。
}

void MainWindow::clearGroupMembers(const QString &groupId, const QString &groupName) {
    QTreeWidget *tree = m_ui->getInstanceList();
    QTreeWidgetItem *groupItem = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(0, MainWindowUI::groupIdRole()).toString() == groupId) {
            groupItem = item;
            break;
        }
    }
    if (groupItem == nullptr) {
        return;  // 藏起来的空组（或者刚被删了）：没有可移的东西
    }
    QStringList keys;
    for (int i = 0; i < groupItem->childCount(); ++i) {
        const QString versionId = instanceIdOf(groupItem->child(i));
        if (!versionId.isEmpty()) {
            keys.append(instanceKey(versionId));
        }
    }
    if (keys.isEmpty()) {
        return;
    }

    // 「移出分组」对**没表过态**的那些实例也得写一条：不写的话它们下一次刷新又被自动归类
    // 拉回这一组来（写下去的是"明确选了未分组"那个保留值，见 InstanceGroupStore::
    // ungroupedMarker()）—— 表现就是"点了移出，可它们还在里面"。
    QString error;
    int moved = 0;
    {
        // 挡信号：下面每写一条都会发一次 groupsChanged，而界面接的那条路是**重灌整棵树**
        // （连带重扫一遍盘）。不挡的话 N 个实例就是 N 次全盘重扫，而实例可以有几十个。
        // 写完自己刷一次就够 —— 和 seedDefaultGroups() 那一段同一个路数。
        const QSignalBlocker blocker(m_groupStore);
        for (const QString &key : keys) {
            if (!m_groupStore->assign(key, QString(), &error)) {
                break;
            }
            ++moved;
        }
    }
    refreshInstalledVersions();
    if (moved < keys.size()) {
        // 半路写失败了。**已经写下去的那些是落盘了的**（说不说它都变了），所以只说清
        // 哪几个没成，别让用户以为整件事都没发生。
        InlineMessage::critical(m_ui->overlay(), tr("移出分组失败"),
            tr("「%1」中的 %2 个实例未能移出：\n%3").arg(groupName).arg(keys.size() - moved).arg(error));
        return;
    }
    appendLog(tr("已把「%1」里的 %2 个实例移出分组").arg(groupName).arg(moved));
}

bool MainWindow::setSelectedInstanceHomeGroup(const QString &groupId) {
    // 取值口径和另外两颗实例级按钮一样：**下拉框**说了算（它就是"当前选中的那个实例"），
    // 不是树的 currentItem()（点一下组头它也会变）。
    return setInstanceHomeGroup(selectedVersionId(m_ui->getInstalledVersionCombo()), groupId);
}

bool MainWindow::setInstanceHomeGroup(const QString &versionId, const QString &groupId) {
    if (versionId.isEmpty()) {
        // 挑目标那个框只在"有选中的实例"时才点得动（groupActionEnabled 那一条），
        // 而菜单开着的时候那个实例可能被删掉了 —— 真走到这儿就说一句，别静默。
        InlineMessage::information(m_ui->overlay(), tr("移入分组"), tr("先选中一个实例。"));
        return false;
    }
    const QString key = instanceKey(versionId);
    // 已经在这一组里了（挑目标那个框里那一格本来就灰着，这儿是给别的入口的保险）。
    //
    // 置顶着的那一档**不在这里返回**：它的老家和 members 指着的是两个组，而"盘上已经是
    // 什么样"这件事得由 store 那一层按老家判（下面那句 setInstanceHomeGroup() 自己做）。
    // 在这里拿 groupIdOf() 当代替的话，"把它移进它老家那一组"会被判成"已经做好了"而
    // 直接返回 —— 看着像没事，其实是要写的那一格没写。
    //
    // NOTE: 目标非空时"没表过态"和"明确选了它"要分开看 —— 光比"存的是不是它"会把
    // "明确选了它"当成已经做好了；目标为空（未分组）时**一律写**：那条保留值
    // 才是"别再自动归类"的意思，而一个从来没表过态的实例正需要它（不写的话它
    // 下一次刷新又被拉回默认组去，表现就是"选了未分组但没生效"）。
    if (!groupId.isEmpty() && !m_groupStore->isPinned(key) &&
        m_groupStore->groupIdOf(key) == groupId) {
        return true;
    }

    QString error;
    // 走 setInstanceHomeGroup() 而不是 assign()：置顶着的实例这一档**只改老家那一格**，
    // members 继续指着置顶组（那是在不在置顶组里的判据）。理由（以及"走 assign 会把老家
    // 一起丢掉"那两条）写在 InstanceGroupStore::setInstanceHomeGroup() 上面。没置顶的
    // 实例它内部就是原样委托给 assign()。
    if (!m_groupStore->setInstanceHomeGroup(key, groupId, &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("移入分组失败"), error);
        return false;
    }
    QString destination = MainWindowUI::ungroupedGroupText();
    for (const InstanceGroupStore::Group &group : m_groupStore->groups()) {
        if (group.id == groupId) {
            destination = group.name;
            break;
        }
    }
    appendLog(tr("实例已分组：%1 → %2").arg(key, destination));
    // NOTE: 这里**不**手动重灌那张树 —— 写成功之后数据层会发 groupsChanged（见上面那段）。
    return true;
}

bool MainWindow::renameGroupTo(const QString &groupId, const QString &newName) {
    QList<InstanceGroupStore::Group> groups = m_groupStore->groups();
    int row = -1;
    for (int i = 0; i < groups.size(); ++i) {
        if (groups.at(i).id == groupId) {
            row = i;
            break;
        }
    }
    if (row < 0) {
        return false;  // 这一组已经被别处删掉了：什么都不做，也不弹窗（不是用户此刻做的事）
    }

    const QString name = newName.trimmed();
    // 两条规矩和 GroupDialog 里那一份**逐字一样**（名字是用户认这个组的东西，两个入口
    // 得用同一把尺子）：空名字和重名都不收。那份校验在 ui/ 里，而 ui/ 不 include src/
    // 是反过来也成立的（那边给的是 main 这一侧拿不到的类型），所以这两句是各写一份的
    // —— 改了这边记得连那边一起改。
    if (name.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), tr("重命名分组失败"), tr("组名不能是空的。"));
        return false;
    }
    if (name.size() > GroupDialog::maxGroupNameLength()) {
        InlineMessage::warning(m_ui->overlay(), tr("重命名分组失败"),
                             tr("组名最多 %1 个字，现在有 %2 个。")
                                 .arg(GroupDialog::maxGroupNameLength())
                                 .arg(name.size()));
        return false;
    }
    for (const InstanceGroupStore::Group &group : groups) {
        if (group.id != groupId && group.name.compare(name, Qt::CaseInsensitive) == 0) {
            InlineMessage::warning(m_ui->overlay(), tr("重命名分组失败"),
                                 tr("已经有一个叫「%1」的组了。").arg(group.name));
            return false;
        }
    }
    if (groups.at(row).name == name) {
        return true;  // 改成一模一样的名字：不写盘
    }

    // 组改名**不动归属** —— 归属挂的是组的 id，不是名字（见 InstanceGroupStore::Group）。
    groups[row].name = name;
    QString error;
    if (!m_groupStore->replaceGroups(groups, &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("重命名分组失败"), error);
        return false;
    }
    return true;
}

bool MainWindow::deleteGroupById(const QString &groupId) {
    QList<InstanceGroupStore::Group> groups = m_groupStore->groups();
    QString name;
    for (int i = 0; i < groups.size(); ++i) {
        if (groups.at(i).id == groupId) {
            name = groups.at(i).name;
            groups.removeAt(i);
            break;
        }
    }
    if (name.isEmpty()) {
        return false;  // 找不到（已经被别处删了）
    }

    QString error;
    if (!m_groupStore->replaceGroups(groups, &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("删除分组失败"), error);
        return false;
    }
    // 组里的实例**一个都不删**：它们指向的组没了，那些归属按 replaceGroups() 的规矩一起
    // 被清掉 —— 于是回到"没表过态"，下一次刷新按版本类型自动归类。和对话框里删一个组
    // 走的是同一条路、同一个说法。
    appendLog(tr("分组已删除：%1").arg(name));
    return true;
}

void MainWindow::rememberGroupExpanded(const QString &groupId, bool expanded) {
    QString error;
    // NOTE: 写盘但**不发 groupsChanged**（理由见 InstanceGroupStore::setGroupExpanded()）——
    // 所以那一行的展开状态由调用方自己摆好，这里只管记。
    if (!m_groupStore->setGroupExpanded(groupId, expanded, &error)) {
        // 只记一句日志、不弹窗：这一下**已经生效了**（树上那一格就是刚摆的），没留住的
        // 只是"下次打开还照这样"——它挡不住用户做任何事，而每点一下弹一个框反倒是把用户
        // 从手上的事里打断（产品约束管的是"会挡住操作"的那一类错误）。
        appendLog(error);
    }
}

void MainWindow::openGroupMenuFor(QTreeWidgetItem *item) {
    if (item == nullptr || !item->data(0, MainWindowUI::rowGroupGearRole()).toBool()) {
        return;
    }
    const QString groupId = item->data(0, MainWindowUI::groupIdRole()).toString();
    // 快照里带上"此刻有没有选中的实例、它在不在这组里"：菜单第一项能不能点、挑目标那个
    // 框里哪一格灰着，都由它决定。
    MainWindowUI::GroupRow row;
    const QString selectedId = selectedVersionId(m_ui->getInstalledVersionCombo());
    const QString selectedKey = instanceKey(selectedId);
    if (!groupRowFor(m_ui, m_groupStore, groupId, &row, selectedKey,
                     homeGroupIdOf(m_groupStore, selectedKey,
                                   m_versionManager->displayParts(selectedId),
                                   m_versionManager, knownGroupIdsOf(m_groupStore)))) {
        return;  // 那一组已经不在了（菜单还没来得及弹它就被删了）
    }
    // 锚点 = 那一行行尾那颗齿轮占的格子（几何的唯一出处见 InstanceGroupDelegate::gearRect()）。
    m_ui->openGroupMenu(row, gearAnchorFor(m_ui, groupId));
}
