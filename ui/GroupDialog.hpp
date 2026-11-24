/**
 * @file GroupDialog.hpp
 * @brief 「分组设置…」对话框：管**分组本身** —— 有哪些组、叫什么、什么顺序、默认展开还是
 * 收着、哪一个是置顶组。
 * @note 【它不管实例】
 *
 * 【进参出参都是值，本文件不 include 任何 src/ 下的头】
 *
 * 【约定：对话框只改自己手里那份清单，不碰磁盘】
 *
 * 用户点「确定」之前，磁盘上的 wtfml.folders 一个字节都不能动 —— 这是所有对话框的共同
 * 约定（"只搭控件、值由调用方灌"）。所以这里存的是 m_groups 这份**工作副本**，确定之后由
 * MainWindow 把它和原来那份比一比，不一样才去写盘。
 *
 * 【约定：按钮那半和干活那半分开】
 *
 * 「新建…」要问一句名字、「删除」要问一句确认、「恢复默认分组设置…」也要确认。这三下
 * 都是**内嵌**在内容区里的（`InlineMessage::getText()` / `question()`）——
 * 但它们**卡住调用方**（内部一个嵌套事件循环），所以真正的逻辑分开放在
 * createGroup() / renameGroup() / deleteGroup() / moveGroupUp() / moveGroupDown() /
 * setGroupExpanded() / restoreDefaults() 这几个 public 槽里，按钮那条路只是"问一句 → 调它们"。
 * 干活那一半不该依赖界面在不在，验证工程驱动的也是那几个槽。
 *
 * 【上移/下移走组清单行尾那两颗三角】
 *
 * 上移/下移点组清单**行尾那两颗三角**（画在 ui/GroupListDelegate 里，
 * 认下那一下的是下面那个 eventFilter()）。上面那几个槽和
 * MainWindow::onRenameInstanceClicked() / applyRename() 的拆法
 * 完全一样，理由也一样：这一半里藏着真会错的东西（重名怎么办、删组要不要连实例一起删、
 * 顺序换到边界上怎么办），它必须验得了。
 */
#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class QCheckBox;
class QEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QWidget;

class GroupDialog : public QDialog {
    Q_OBJECT

public:
    /// 一个分组。和 InstanceGroupStore::Group 逐字段相同（见文件头）。
    struct Group {
        QString id;
        QString name;
        /// 这一组在实例树里**画成展开的还是收着的**（要求里的「默认展开/关闭」）。
        ///
        /// NOTE: 它既是"默认"也是"当前" —— **只有这一份状态**（全文在
        /// InstanceGroupStore::Group 那段）。这张窗里的勾选框改的就是它，
        /// 而且改完要等用户按「确定」才落盘（对话框不碰磁盘那条约定）。
        bool expanded = true;

        /// 这一组是**置顶组**：实例行行尾那颗上箭头点了就把实例移进这一组。
        ///
        /// NOTE: 全清单里**最多一个** —— 这一格改的不是"这一组怎么样"，而是"别的组都不是
        /// 置顶组了"，所以 setGroupPinned() 把别人一起清掉（和
        /// InstanceGroupStore::setGroupPinned() 那句逐字对应）。这份工作副本和
        /// store 那份逐字段相同（见文件头），包括这一格。
        bool pinned = false;

        friend bool operator==(const Group &, const Group &) = default;
    };

    /**
     * @brief 构造对话框。
     * @param groups 现有的组清单（顺序就是界面上的顺序，也是树上的顺序）。
     * @param memberCounts 每个组里有多少个实例（按组 id）。缺的键当 0。
     * 只用来显示「组名 (N)」和"删这个组会有多少实例回到未分组"。
     * @param defaultGroups 五个默认分组（"恢复默认"要换成的就是它们）。
     * 顺序就是界面上从上到下的顺序，名字用来拼确认框里那句话。
     * @param defaultCounts 恢复默认**之后**每个默认组里会有多少个实例（按组 id）。
     * 这一格是调用方算出来的：恢复默认会把用户自己建的组全拆掉，
     * 那些实例回到"没表过态"，于是按版本类型自动归类 —— 落到哪一组
     * 只有 MainWindow 那边算得出来（defaultGroupIdFor()）。
     * 缺的键当 0。
     * @param parent 父窗口。
     */
    GroupDialog(const QList<Group> &groups, const QHash<QString, int> &memberCounts,
                const QList<Group> &defaultGroups, const QHash<QString, int> &defaultCounts,
                QWidget *parent = nullptr);

    /// 用户摆好的最终组清单。没改过的话就是进参那一份。
    QList<Group> groups() const { return m_groups; }

public slots:
    /// 新建一个组。名字为空、或者和已有的组重名时**不建**，弹一个警告框说明原因。
    /// @return 真的建了返回 true。
    bool createGroup(const QString &name);

    /// 把某个组改名。@return 改成功了返回 true。
    ///
    /// NOTE: 组改名**不动归属** —— 归属挂的是组的 id，不是名字。id 不变，所以
    /// "给组改个名"不会把组里的实例抖出去。这正是 id 和名字分开的理由。
    bool renameGroup(const QString &groupId, const QString &name);

    /// 删掉一个组。组里的实例**一个都不会被删**，它们只是变成未分组。
    /// @return 删掉了返回 true（找不到这个组时返回 false）。
    bool deleteGroup(const QString &groupId);

    /// 把某个组在清单里**往上挪一格**。
    /// 已经在最上面、或者找不到这个组时**什么都不做**，返回 false。
    bool moveGroupUp(const QString &groupId);

    /// 同上，往下挪一格。
    bool moveGroupDown(const QString &groupId);

    /// 把某个组**直接挪到最上面**（界面按住 Shift 点那颗箭头就是这一步）。
    /// 已经在最上面、或者找不到这个组时什么都不做，返回 false。
    ///
    /// NOTE: 和 moveGroupUp() 拆成两个槽（不是给那个加个 bool 参数）：**置顶不是"挪很多
    /// 格"**，它一步到位、和现在排第几无关。
    /// NOTE: 界面那条路（按住 Shift 点那颗箭头）读的是**那一发鼠标事件上的**修饰键
    /// （`QMouseEvent::modifiers()`），不是 QApplication::keyboardModifiers() ——
    /// 后者读的是"此刻键盘的全局状态"，验证工程造那一发事件时对不上。
    bool moveGroupToTop(const QString &groupId);

    /// 同上，**直接挪到最下面**。
    bool moveGroupToBottom(const QString &groupId);

    /// 改某一组的「默认展开」。
    ///
    /// NOTE: 摆勾选框的**那一下**会发一次 toggled，那一次不该被当成用户点的（那会绕回去
    /// 再改一遍状态）；挡它的是 m_updatingExpanded，和下面 m_updatingPinned 同一个路数。
    void setGroupExpanded(const QString &groupId, bool expanded);

    /// 把某一组设成**置顶组**（或者取消它）。设上去时**别的组这一位一律清掉** ——
    /// 置顶组只能有一个，而这一格的位置在文件里、在界面上都是"谁当家"这一件事。
    ///
    /// NOTE: 和 setGroupExpanded() **完全对称**：改的是 m_groups 这份**工作副本**，
    /// **一个字节都不碰磁盘**（等用户按「确定」，见文件头那条约定），勾选框那一发
    /// toggled 由 m_updatingPinned 挡。唯一的差别是它改完要 rebuildGroupList()：
    /// 这一下动的**不止选中的那一组**（别组这一位全被清掉了），重画一遍顺带把
    /// refreshActionButtons() 再走一次，于是"现在谁是置顶组"立刻是对的 —— 而勾选框
    /// 本来就是跟着选中的那一行摆的，用户切到哪个组看到的就是那一组的实话。
    void setGroupPinned(const QString &groupId, bool pinned);

    /// 把整份清单换成五个默认分组。
    ///
    /// NOTE: 这是**工作副本**上的动作，和别的槽一样等「确定」才落盘 —— 半路按「取消」
    /// 就等于什么都没发生。实例一个都不会被删：自己建的那些组分没了之后，里面的
    /// 实例回到"没表过态"，下一次刷新按版本类型自动归类（见 InstanceGroupStore::
    /// restoreDefaultGroups() 那段，两边的说法必须一致）。
    /// @return 换了返回 true（清单本来就等于默认那一份时也返回 true，只是没变）。
    bool restoreDefaults();

    // ---------- 控件的 objectName ----------
    // 验证工程靠这些名字认控件，别改回按条目文本去认（组名是用户起的，随时会变）。
    // / 这一页叫什么。**唯一出处** —— 覆盖层的页眉和这个窗自己的标题栏写的是同一句
    // / （页面型那一档里页眉就是标题栏，两边各写各的迟早会对不上）。
    static QString windowTitle() { return tr("分组设置"); }

    static QString groupListName() { return QStringLiteral("groupList"); }
    static QString addGroupButtonName() { return QStringLiteral("addGroupButton"); }
    static QString renameGroupButtonName() { return QStringLiteral("renameGroupButton"); }
    static QString removeGroupButtonName() { return QStringLiteral("removeGroupButton"); }
    /// 右边那张卡片的标题：选中那一组的名字（没选中写「未选择分组」）。
    static QString detailTitleName() { return QStringLiteral("groupDetailTitle"); }
    /// 「默认展开」那颗开关（QCheckBox + Theme::markSwitch，按选中那一组的状态亮着）。
    static QString expandedCheckName() { return QStringLiteral("groupExpandedCheck"); }
    /// 「设为收藏组」那颗开关（右边卡片里「默认展开」下面那一行）。
    static QString pinnedCheckName() { return QStringLiteral("groupPinnedCheck"); }
    /// 那一行的标题。**唯一出处** —— 勾选框、验证工程认的那句话都念它。
    static QString pinnedCheckText() { return tr("设为收藏组"); }
    /// 那一行标题下的说明。
    ///
    /// NOTE: 「收藏组」光看那五个字不知道打开会怎样 —— 所以紧挨着它写一句。
    /// **要短**（40 个汉字以内）：右边那张卡片只占半张窗，说明一长就折成好几行。
    static QString pinnedHintText() {
        return tr("收藏的实例会同时显示在这个组和主页快速启动区。");
    }
    static QString restoreDefaultsButtonName() {
        return QStringLiteral("restoreDefaultsButton");
    }
    static QString confirmButtonName() { return QStringLiteral("groupDialogConfirmButton"); }
    static QString cancelButtonName() { return QStringLiteral("groupDialogCancelButton"); }

    /// 组名的长度上限。比实例目录名的 64 短，因为组名只出现在界面上（不进路径）、
    /// 而且它和实例数一起挤在同一行里。超过这个长度的名字只会被省略号截掉，
    /// 挡下来是为了让用户当场知道"名字太长了"，而不是过一会儿才发现看不全。
    ///
    /// NOTE: 组行行尾那颗齿轮里的「重命名这一组…」也归它管（那是另一条改名的路，
    /// MainWindow::renameGroupTo()）—— 两个入口必须是同一把尺子，不然"从这个口
    /// 进去能起 32 个字的名字、从那个口进不去"。
    static int maxGroupNameLength() { return 32; }

    /// 组在列表里的那行文字。
    /// @param name 组名。@param count 组里有多少个实例。
    ///
    /// NOTE: 个数放在名字里（而不是单开一列）是因为这个对话框很窄，多一列会把组名
    /// 挤成省略号 —— 而组名才是用户认这个组的东西。
    /// NOTE: 树上那个组头写的也是这一句（MainWindowUI::groupItemText() 转发的就是它）。
    static QString groupItemText(const QString &name, int count) {
        return QStringLiteral("%1 (%2)").arg(name).arg(count);
    }

protected:
    /// 组清单那张表**行尾那两颗箭头**那一下的接线口（挂在 `m_groupList->viewport()` 上）。
    ///
    /// 为什么是事件过滤器而不是委托的 editorEvent()：点箭头不能让那一行**顺手被选中**，
    /// 更不能让点得快的那第二发落到"双击 = 重命名"那条路上（那会弹一个模态框）——
    /// 而 itemClicked / itemDoubleClicked 是视图自己发的，代理那边压不住
    /// （见 ui/RowActivation.hpp 那段）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 重画左边那张组列表（组清单变了之后调用）。选中项按 id 保住。
    void rebuildGroupList();
    /// 「重命名」「删除」能不能点，以及那两个勾选框（「默认展开」「设为置顶组」）怎么摆。
    /// 列表的选中项一变就调（勾选框那一半顺手在这儿对齐）。
    void refreshActionButtons();
    /// 当前选中的是哪个组（没选中返回空串）。列表上选中的那一行对应的组 id。
    QString selectedGroupId() const;
    /// 组 id 对应的组名；没有这个组时返回空串。
    QString groupName(const QString &groupId) const;
    /// 组 id 在 m_groups 里的下标；没有返回 -1。
    int rowOfGroup(const QString &groupId) const;
    /// 组名合不合法。@param exceptGroupId 重名比较时跳过的那一个（改自己的名字时
    /// 不能和自己撞）。合法返回空串，否则返回给用户看的一句原因。
    QString validateGroupName(const QString &rawName, const QString &exceptGroupId) const;

    QList<Group> m_groups;
    /// 工作副本的实例数。只用来看「组名 (N)」和"删这个组有多少实例回到未分组"。
    QHash<QString, int> m_memberCounts;
    /// 五个默认分组（"恢复默认"要换成的那一份）。
    QList<Group> m_defaultGroups;
    /// 恢复默认之后每个默认组里会有多少实例（见进参那段）。
    QHash<QString, int> m_defaultCounts;
    /// true = 此刻正在用代码摆勾选框的状态，那一次 toggled 不算用户操作。
    bool m_updatingExpanded = false;
    /// 同上，管「设为置顶组」那颗（两颗各一个：一次刷新的两个勾选框摆的是**不同**的两格，
    /// 共用一个的话摆完第一颗的复位会把第二颗那一发也放过去）。
    bool m_updatingPinned = false;

    QListWidget *m_groupList = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QCheckBox *m_expandedCheck = nullptr;
    QCheckBox *m_pinnedCheck = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QLabel *m_detailTitle = nullptr;
};
