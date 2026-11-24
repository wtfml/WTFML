/**
 * @file GroupDialog.cpp
 * @brief 实现实例分组设置对话框。
 * @note 组列表及行尾上下移三角的交互在本页；几何与绘制见 GroupListDelegate。
 */

#include "GroupDialog.hpp"

#include "Theme.hpp"   // 「删除」那颗按钮的身份（危险 = 红字红边）

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include "MainWindowPageLayout.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>
#include <QUuid>
#include <QVBoxLayout>

#include "GroupListDelegate.hpp"
// 内嵌提示那一套：这个对话框自己就是"一页"，所以要弹的那两句
// 得从自己往上找那一层覆盖层 —— 见 OverlayHost::forWidget。
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "MainWindowUI.hpp"

namespace {

/// 一个新的组 id。
///
/// NOTE: 和 InstanceGroupStore::newGroupId() 是同一件事，这里没有复用而是各写一份
/// —— ui/ 不依赖 src/（见 .hpp 文件头），为了一个 QUuid 的调用把业务层拖进来
/// 不划算。两份必须**同格式**（WithoutBraces 的 UUID），因为这里的 id 会被
/// 原样写进那个文件。
QString newGroupId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

}  // namespace

GroupDialog::GroupDialog(const QList<Group> &groups, const QHash<QString, int> &memberCounts,
                         const QList<Group> &defaultGroups,
                         const QHash<QString, int> &defaultCounts, QWidget *parent)
    : QDialog(parent),
      m_groups(groups),
      m_memberCounts(memberCounts),
      m_defaultGroups(defaultGroups),
      m_defaultCounts(defaultCounts) {
    setWindowTitle(windowTitle());
    resize(720, 480);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(16);

    // 步骤 137：左右两张小节卡片。左边是清单（标题行右头「新建分组」），右边是**选中那一组**
    // 的设置：标题就是组名，下面重命名 / 删除，再下面两条设置行（开关在行尾）。
    // 「默认展开 / 设为收藏组」改的是选中那一组 ⇒ 摆在写着那一组名字的卡片里，关系一眼看得出。
    auto *body = new QHBoxLayout;
    body->setSpacing(16);
    mainLayout->addLayout(body, 1);

    // 卡片标题行右头挂一颗按钮：把 makeSectionCard() 放进去的那条标题拿出来，和按钮并一排。
    const auto headingRow = [](QVBoxLayout *layout, QWidget *trailing) {
        QLayoutItem *item = layout->takeAt(0);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(item->widget(), 1);
        delete item;
        if (trailing != nullptr) row->addWidget(trailing);
        layout->insertLayout(0, row);
    };
    const auto caption = [](const QString &text, QWidget *parent) {
        auto *label = new QLabel(text, parent);
        label->setWordWrap(true);
        label->setFont(Theme::textFont(label->font(), Theme::TextRole::Caption));
        const auto tone = [label] {
            QPalette palette = label->palette();
            palette.setColor(QPalette::WindowText, Theme::tokens().secondary);
            label->setPalette(palette);
        };
        tone();
        Theme::follow(label, tone);
        return label;
    };

    {
        QVBoxLayout *layout = nullptr;
        QFrame *groupBox = MainWindowPageLayout::makeSectionCard(tr("分组"), this, &layout);
        m_addButton = new QPushButton(tr("新建分组…"), groupBox);
        m_addButton->setObjectName(addGroupButtonName());
        headingRow(layout, m_addButton);  // 「新建」= 创建 ⇒ 默认那一档（Accent）

        m_groupList = new QListWidget(groupBox);
        m_groupList->setObjectName(groupListName());
        // 行尾那两颗「上移 / 下移」三角（见 GroupListDelegate
        // 文件头）。代理只管画，认下那一下的是 GroupDialog::eventFilter() —— 挂的是**视口**，
        // 因为鼠标事件是按光标位置投给最上面那个孩子的（挂在表上收不到）。
        m_groupList->setItemDelegate(new GroupListDelegate(m_groupList));
        m_groupList->viewport()->installEventFilter(this);
        layout->addWidget(m_groupList, 1);
        layout->addWidget(caption(tr("行尾箭头调整顺序，按住 Shift 直接放到最上 / 最下。"), groupBox));
        body->addWidget(groupBox, 5);
    }

    {
        QVBoxLayout *layout = nullptr;
        QFrame *detail = MainWindowPageLayout::makeSectionCard(QString(), this, &layout);
        m_detailTitle = qobject_cast<QLabel *>(layout->itemAt(0)->widget());
        m_detailTitle->setObjectName(detailTitleName());
        // 组名最长 32 个字：标题不许把整张窗顶宽，放不下就裁掉，全名在悬停提示里。
        // shortcut: 裁的是硬边不是省略号，真有人起满 32 字再按宽度做 elide。
        m_detailTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

        // 对**选中那一组**做的事，挂在组名那一行右头（和左卡「新建分组…」同一个高度）。
        // 「重命名…」带省略号是 UI_COPY 4.1 的规矩（会弹一格输入）；「删除」只问一句确认，不带。
        auto *actions = new QWidget(detail);
        auto *actionsLayout = new QHBoxLayout(actions);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(8);
        m_renameButton = new QPushButton(tr("重命名…"), detail);
        m_renameButton->setObjectName(renameGroupButtonName());
        Theme::markButton(m_renameButton, Theme::ButtonKind::Neutral);
        m_removeButton = new QPushButton(tr("删除"), detail);
        m_removeButton->setObjectName(removeGroupButtonName());
        // 「删除」删的是**整个分组**（组里那些实例会掉回未分组）⇒ 危险那一档。
        Theme::markButton(m_removeButton, Theme::ButtonKind::Danger);
        actionsLayout->addWidget(m_renameButton);
        actionsLayout->addWidget(m_removeButton);
        headingRow(layout, actions);

        // 两条设置行：标题 + 一句说明，开关在行尾（实例设置概览同一个 makeSettingRow）。
        m_expandedCheck = new QCheckBox(detail);
        m_expandedCheck->setObjectName(expandedCheckName());
        m_expandedCheck->setAccessibleName(tr("默认展开"));
        m_expandedCheck->setToolTip(tr("这一组在实例列表里默认展开。点组头也能切换。"));
        Theme::markSwitch(m_expandedCheck);
        layout->addWidget(MainWindowPageLayout::makeSettingRow(
            detail, MainWindowPageLayout::makeSettingRowTitle(tr("默认展开"), detail),
            tr("在实例列表里默认展开这一组。"), m_expandedCheck));
        // 打开的那一组变成**收藏组**（全清单只能有一个，所以打开这一颗会把别组那一颗关掉，
        // 见 setGroupPinned()）。说明**必须短**（上限见 pinnedHintText()）。
        m_pinnedCheck = new QCheckBox(detail);
        m_pinnedCheck->setObjectName(pinnedCheckName());
        m_pinnedCheck->setAccessibleName(pinnedCheckText());
        m_pinnedCheck->setToolTip(
            tr("只能设置一个收藏组。启用此组会取消其他组的收藏状态。"));
        Theme::markSwitch(m_pinnedCheck);
        layout->addWidget(MainWindowPageLayout::makeSettingRow(
            detail, MainWindowPageLayout::makeSettingRowTitle(pinnedCheckText(), detail), pinnedHintText(),
            m_pinnedCheck));
        layout->addSpacing(4);
        layout->addWidget(caption(tr("删除分组不会删除实例，它们会回到未分组。"), detail));
        layout->addStretch(1);
        body->addWidget(detail, 4);
    }

    {
        // 「新建」永远可点（一个组都没有的时候它是唯一的出路，而"组一个都没有"是删得
        // 出来的状态）；别的要有选中的组才有意义，可用性由 refreshActionButtons() 统一管
        // （它连"已经在最上面了就别给上移"也一起管）。
        connect(m_addButton, &QPushButton::clicked, this, [this] {
            bool ok = false;
            const QString name = InlineMessage::getText(OverlayHost::forWidget(this),
                                                        tr("新建分组"), tr("组名："), QString(),
                                                        &ok);
            if (ok) {
                createGroup(name);
            }
        });
        connect(m_renameButton, &QPushButton::clicked, this, [this] {
            const QString groupId = selectedGroupId();
            if (groupId.isEmpty()) {
                return;
            }
            // 初始文本是现在的名字 —— 用户多半只是想改一两个字。
            bool ok = false;
            const QString name = InlineMessage::getText(
                OverlayHost::forWidget(this), tr("重命名分组"), tr("组名："), groupName(groupId),
                &ok);
            if (ok) {
                renameGroup(groupId, name);
            }
        });
        connect(m_removeButton, &QPushButton::clicked, this, [this] {
            const QString groupId = selectedGroupId();
            if (groupId.isEmpty()) {
                return;
            }
            // 先问再删，而且说清**实例不会被删**：用户看到"删除"两个字时的第一反应
            // 就是怕连里面的东西一起没了，而这里确实不会。
            const bool answer = InlineMessage::question(OverlayHost::forWidget(this), tr("删除分组"),
                tr("将删除分组「%1」，其中 %2 个实例将归入未分组。\n\n确定删除？")
                    .arg(groupName(groupId))
                    .arg(m_memberCounts.value(groupId)), tr("删除"), tr("取消"), false,
                    /*destructive=*/true);
            if (answer) {
                deleteGroup(groupId);
            }
        });
        // 上移/下移走组清单**行尾那两颗三角**
        // （一次一格；**按住 Shift** 点就是直接放到最顶上 / 最底下）。
        // 那一套手势整个在 eventFilter() 里。
        connect(m_expandedCheck, &QCheckBox::toggled, this, [this](bool checked) {
            // m_updatingExpanded：这是 refreshActionButtons() 在摆状态（跟着选中的组），
            // 不是用户点的 —— 不挡的话每选一行都会把那一行的状态"改"成它本来就有的值。
            if (m_updatingExpanded) {
                return;
            }
            setGroupExpanded(selectedGroupId(), checked);
        });
        connect(m_pinnedCheck, &QCheckBox::toggled, this, [this](bool checked) {
            // m_updatingPinned：同上 —— 这一发是 refreshActionButtons() 在摆状态。
            if (m_updatingPinned) {
                return;
            }
            setGroupPinned(selectedGroupId(), checked);
        });
        connect(m_groupList, &QListWidget::currentRowChanged, this,
                [this] { refreshActionButtons(); });
        // 双击一行 = 重命名它（列表上的常规手势，省一次"先选中再点按钮"）。
        //
        // NOTE: 行尾那一带（两颗箭头占的那两格）**到不了这里** —— eventFilter() 把那几发
        // 整个吃掉了。不这样的话，用户连点两下那颗三角想挪两格，第二发会弹出一个
        // 重命名框（挪一格 + 挡住手上的事）。
        connect(m_groupList, &QListWidget::itemDoubleClicked, this, [this] {
            if (m_renameButton->isEnabled()) {
                m_renameButton->click();
            }
        });
    }

    auto *buttons = new QHBoxLayout;
    // 「恢复默认分组」摆在最左边（离取消/确定远远的）：它是个"把整张表推倒重来"的
    // 动作，和那两颗"这一趟改完了没有"的按钮不是一类东西，挨着摆会点错。
    m_restoreButton = new QPushButton(tr("恢复默认分组"), this);
    m_restoreButton->setObjectName(restoreDefaultsButtonName());
    // 次要那一档（只有字）：这张窗改的只是工作副本，按「取消」整个作废；
    // 真要恢复时弹的那一问已经是危险样式。用不着在底栏再挂一颗红的。
    Theme::markButton(m_restoreButton, Theme::ButtonKind::Ghost);
    buttons->addWidget(m_restoreButton);
    buttons->addStretch(1);
    auto *cancelButton = new QPushButton(tr("取消"), this);
    cancelButton->setObjectName(cancelButtonName());
    Theme::markButton(cancelButton, Theme::ButtonKind::Neutral);
    auto *confirmButton = new QPushButton(tr("确定"), this);
    confirmButton->setObjectName(confirmButtonName());
    confirmButton->setDefault(true);
    buttons->addWidget(cancelButton);
    buttons->addWidget(confirmButton);
    mainLayout->addLayout(buttons);

    // 恢复默认是个**不可逆**的动作（自己建的组全没了），而且用户此刻多半以为自己只是
    // 在改一个名字 —— 所以先问，而且把两件事说清：会换成哪五个、实例一个都不删。
    connect(m_restoreButton, &QPushButton::clicked, this, [this] {
        QStringList names;
        names.reserve(m_defaultGroups.size());
        for (const Group &group : m_defaultGroups) {
            names.append(group.name);
        }
        const bool answer = InlineMessage::question(OverlayHost::forWidget(this), tr("恢复默认分组设置"),
            tr("现在的分组会被换成这 %1 个默认分组：%2。\n\n自己建的分组会消失，里面的实例不会被删，会按版本类型回到默认分组或未分组。\n\n确定要恢复吗？")
                .arg(names.size())
                .arg(names.join(tr("、"))), tr("是"), tr("否"), false,
                /*destructive=*/true);
        if (answer) {
            restoreDefaults();
        }
    });

    // 「确定」接 accept()、「取消」接 reject()。QDialog::exec() 的返回值区分得开，
    // 调用方只在 Accepted 时才去看 groups()。**注意**：这里所有槽改的都只是工作副本，
    // 「恢复默认」也一样 —— 半路按取消 = 这一趟什么都没发生。
    connect(confirmButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    rebuildGroupList();
}

bool GroupDialog::eventFilter(QObject *watched, QEvent *event) {
    if (m_groupList == nullptr || watched != m_groupList->viewport() || event == nullptr) {
        return QDialog::eventFilter(watched, event);
    }
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
        && type != QEvent::MouseButtonDblClick) {
        return QDialog::eventFilter(watched, event);
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton) {
        return QDialog::eventFilter(watched, event);
    }
    // 哪一行、点在行里哪儿 —— 用视图自己的 indexAt()（滚动位置、代理的布局都在它手里），
    // 不用 `m_groups.at(row)`：行和组是两个东西摆在一起而已，认行只认视图。
    const QModelIndex index = m_groupList->indexAt(mouse->position().toPoint());
    if (!index.isValid()) {
        return QDialog::eventFilter(watched, event);
    }
    // 这一行整块没有方框（两颗都挪不动：表里只剩一条分组）时就当普通一行 —— 不吃这一发、
    // 也不弹提示。判据和代理画不画那个框、弹不弹话读的是同一句
    // （GroupListDelegate::showsMoveBox()），不会出现「看着什么都没有、点下去却被吃掉」。
    if (!GroupListDelegate::showsMoveBox(index)) {
        return QDialog::eventFilter(watched, event);
    }
    const QRect row = m_groupList->visualRect(index);
    const QPoint pos = mouse->position().toPoint();
    const bool inUp = GroupListDelegate::moveUpRect(row).contains(pos);
    const bool inDown = !inUp && GroupListDelegate::moveDownRect(row).contains(pos);
    if (!inUp && !inDown) {
        return QDialog::eventFilter(watched, event);  // 点的不是那两格：这一行该怎样还怎样
    }
    // 落在那两格里以后**一律吃掉**，哪怕这一侧挪不动（第一行没有上移、最后一行没有
    // 下移）。不吃的话那一带会漏给"点这一行"：那一行会顺手被选中，连点的第二发还会弹出
    // 重命名框。
    // 漏出去的那几格**本来就没有画箭头**（见 GroupListDelegate::canMoveUp()），所以
    // "点下去什么都没发生"是这里唯一说得过去的解释。
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
        // 点得快的时候，第二发（以及后面每一发）到的是**双击**事件而不是按下事件（见
        // ui/RowActivation.hpp 那段）—— 这里要**每一发都算**：用户连点三下就是想挪三格。
        // 放下（Release）那一发只负责被吃掉，不然松手时视图还会补一发 clicked。
        const QListWidgetItem *item = m_groupList->item(index.row());
        const QString groupId = item != nullptr ? item->data(Qt::UserRole).toString() : QString();
        // Shift 就读**这一发事件上**的修饰键（不是 QApplication::keyboardModifiers()：
        // 后者读的是"此刻键盘的全局状态"，验证工程造这一发事件时对不上）。
        const bool toEdge = mouse->modifiers().testFlag(Qt::ShiftModifier);
        if (inUp && GroupListDelegate::canMoveUp(index)) {
            if (toEdge) {
                moveGroupToTop(groupId);
            } else {
                moveGroupUp(groupId);
            }
        } else if (inDown && GroupListDelegate::canMoveDown(index)) {
            if (toEdge) {
                moveGroupToBottom(groupId);
            } else {
                moveGroupDown(groupId);
            }
        }
    }
    return true;
}

QString GroupDialog::selectedGroupId() const {
    if (m_groupList == nullptr) {
        return QString();
    }
    const QListWidgetItem *item = m_groupList->currentItem();
    if (item == nullptr) {
        return QString();
    }
    // 组 id 存在 UserRole 里（和实例列表同一个约定：显示文本会变，不是身份标识）。
    return item->data(Qt::UserRole).toString();
}

QString GroupDialog::groupName(const QString &groupId) const {
    for (const Group &group : m_groups) {
        if (group.id == groupId) {
            return group.name;
        }
    }
    return QString();
}

int GroupDialog::rowOfGroup(const QString &groupId) const {
    if (groupId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_groups.size(); ++i) {
        if (m_groups.at(i).id == groupId) {
            return i;
        }
    }
    return -1;
}

QString GroupDialog::validateGroupName(const QString &rawName, const QString &exceptGroupId) const {
    const QString name = rawName.trimmed();
    if (name.isEmpty()) {
        return tr("组名不能是空的。");
    }
    if (name.size() > maxGroupNameLength()) {
        return tr("组名最多 %1 个字，现在有 %2 个。").arg(maxGroupNameLength()).arg(name.size());
    }
    for (const Group &group : m_groups) {
        if (group.id == exceptGroupId) {
            continue;
        }
        // 不区分大小写：界面上「Forge 整合包」和「forge 整合包」看着就是同一个，
        // 放过去的结果是用户在同名两组之间搬来搬去，永远搞不清自己在哪个组里。
        if (group.name.compare(name, Qt::CaseInsensitive) == 0) {
            return tr("已经有一个叫「%1」的组了。").arg(group.name);
        }
    }
    return QString();
}

bool GroupDialog::createGroup(const QString &name) {
    const QString problem = validateGroupName(name, QString());
    if (!problem.isEmpty()) {
        InlineMessage::warning(OverlayHost::forWidget(this), tr("新建分组失败"), problem);
        return false;
    }

    Group group;
    group.id = newGroupId();
    group.name = name.trimmed();
    m_groups.append(group);

    rebuildGroupList();
    // 新建完就选中它：用户刚建完一个组，下一步多半是给它换个名字或者挪个位置。
    m_groupList->setCurrentRow(m_groups.size() - 1);
    return true;
}

bool GroupDialog::renameGroup(const QString &groupId, const QString &name) {
    const int row = rowOfGroup(groupId);
    if (row < 0) {
        // 找不到这个组（比如它刚被删掉了）。不弹窗：这不是用户此刻做的操作，
        // 弹一个他看不懂的框只会让人以为坏了。
        return false;
    }
    const QString problem = validateGroupName(name, groupId);
    if (!problem.isEmpty()) {
        InlineMessage::warning(OverlayHost::forWidget(this), tr("重命名分组失败"), problem);
        return false;
    }

    m_groups[row].name = name.trimmed();
    // 名字和**展开状态**都不在列表文字里（文字是「组名 (N)」），所以只把那一行的字换了 ——
    // 重建整张列表会把选中项和滚动位置都抖一下，而这里什么都没多。
    if (QListWidgetItem *item = m_groupList->item(row)) {
        const QSignalBlocker blocker(m_groupList);
        item->setText(groupItemText(m_groups.at(row).name, m_memberCounts.value(groupId)));
    }
    refreshActionButtons();
    return true;
}

bool GroupDialog::deleteGroup(const QString &groupId) {
    const int row = rowOfGroup(groupId);
    if (row < 0) {
        return false;
    }
    m_groups.removeAt(row);
    m_memberCounts.remove(groupId);

    rebuildGroupList();
    return true;
}

bool GroupDialog::moveGroupUp(const QString &groupId) {
    const int row = rowOfGroup(groupId);
    if (row <= 0) {
        // 已经在最上面（或者压根没这个组）：什么都不做。**不弹窗也不返回错误文字** ——
        // 界面上到顶的那一侧根本不画箭头，这条路只有验证工程会走。
        return false;
    }
    m_groups.swapItemsAt(row, row - 1);
    rebuildGroupList();
    return true;
}

bool GroupDialog::moveGroupDown(const QString &groupId) {
    const int row = rowOfGroup(groupId);
    if (row < 0 || row >= m_groups.size() - 1) {
        return false;
    }
    m_groups.swapItemsAt(row, row + 1);
    rebuildGroupList();
    return true;
}

bool GroupDialog::moveGroupToTop(const QString &groupId) {
    const int row = rowOfGroup(groupId);
    if (row <= 0) {
        return false;  // 已经在最上面（或者压根没这个组）：和 moveGroupUp() 同一档
    }
    // 一步到位：摘出来插到最前面（不是"swap 很多次"—— 那样中间那几格会被换乱）。
    m_groups.move(row, 0);
    rebuildGroupList();
    return true;
}

bool GroupDialog::moveGroupToBottom(const QString &groupId) {
    const int row = rowOfGroup(groupId);
    if (row < 0 || row >= m_groups.size() - 1) {
        return false;
    }
    m_groups.move(row, m_groups.size() - 1);
    rebuildGroupList();
    return true;
}

void GroupDialog::setGroupExpanded(const QString &groupId, bool expanded) {
    const int row = rowOfGroup(groupId);
    if (row < 0) {
        return;
    }
    if (m_groups.at(row).expanded == expanded) {
        return;
    }
    m_groups[row].expanded = expanded;
    // 别的不动：列表文字里没有"展开还是收着"（那一格是树上画出来的），勾选框就是刚被点的
    // 那个东西。真要重画的是**勾选框另一头**（比如验证工程直接调这个槽）—— 那种情况下
    // refreshActionButtons() 会把勾选框摆正，顺手也把"上移/下移"的灰不灰重算一遍。
    refreshActionButtons();
}

void GroupDialog::setGroupPinned(const QString &groupId, bool pinned) {
    const int row = rowOfGroup(groupId);
    if (row < 0) {
        // 找不到这个组（比如它刚被删掉了）：什么都不做。这张窗里选中的那一行一定认得出来，
        // 所以这条只有验证工程拿一个过期的 id 来调才会走到。
        return;
    }
    if (m_groups.at(row).pinned == pinned) {
        return;
    }
    // 置顶组只能有一个 —— 一句把目标那一组和别的所有组一起摆好：pinned 为 true 时只有
    // 目标留下 true，为 false 时全清（含目标自己）。和 InstanceGroupStore::setGroupPinned()
    // 里那句**逐字对应**：两边对"谁当家"的看法必须一样，否则对话框里看到的那一份
    // （按「确定」会落盘的正是它）和存储层自己算出来的那一份会对不上。
    for (Group &group : m_groups) {
        group.pinned = pinned && group.id == groupId;
    }
    // 整张表重画（和 setGroupExpanded() 只刷按钮不同）：这一下动的不止选中的那一组 ——
    // 别的组这一位也被清了，而勾选框是跟着选中的那一行摆的，重画一遍顺带让
    // refreshActionButtons() 再走一次，"现在谁是置顶组"立刻是对的。
    rebuildGroupList();
}

bool GroupDialog::restoreDefaults() {
    m_groups = m_defaultGroups;
    // 实例数跟着换成"恢复之后每个默认组里会有几个"（进参里那段说明：这一格只有调用方
    // 算得出来）。不换的话，列表上会显示恢复**之前**那些组里的数字，而那几个组已经没了。
    m_memberCounts = m_defaultCounts;
    rebuildGroupList();
    return true;
}

void GroupDialog::rebuildGroupList() {
    // 重建之后把选中项停在原来那个组上（按 id 找回）：改个名、挪一下、恢复默认之后
    // 选中项跳到别处，用户就得重新找一遍。
    const QString previous = selectedGroupId();

    {
        const QSignalBlocker blocker(m_groupList);
        m_groupList->clear();
        // 行高和版本下载那棵树一样。这儿的行只有一行字，不给的话就是字那么高 ——
        // 同一份组名在实例页那棵树上和在这张表里高矮不一样，看着像两个东西。
        //
        // **条目还要再加一条缝**（GroupListDelegate::rowGap()）：46 高一行紧挨着一行码
        // 起来，两条组名之间只剩行内那点余量，扫一眼分不出这是两条。画出来的那一块仍然是 46 高
        // —— 代理按 contentRect() 把底下那一条收掉（字、背景、行尾那个方框都按它摆）。
        const int rowHeight =
            MainWindowUI::versionTreeRowHeight() + GroupListDelegate::rowGap();
        for (const Group &group : m_groups) {
            auto *item = new QListWidgetItem(
                groupItemText(group.name, m_memberCounts.value(group.id)), m_groupList);
            item->setData(Qt::UserRole, group.id);
            item->setSizeHint(QSize(0, rowHeight));
        }
        // 原来没选中（刚打开）就停在第一组：右边那张卡片写的是"选中那一组"，空着没意义。
        const int row = rowOfGroup(previous);
        if (row >= 0) {
            m_groupList->setCurrentRow(row);
        } else if (m_groupList->count() > 0) {
            m_groupList->setCurrentRow(0);
        }
    }
    // 选中项是在挡着信号的时候摆的，那条 currentRowChanged 收不到，
    // 那几颗按钮的可用性得在这里收尾。
    refreshActionButtons();
}

void GroupDialog::refreshActionButtons() {
    const QString groupId = selectedGroupId();
    const int row = rowOfGroup(groupId);
    const bool hasSelection = row >= 0;
    m_renameButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
    m_detailTitle->setText(hasSelection ? m_groups.at(row).name : tr("未选择分组"));
    m_detailTitle->setToolTip(hasSelection ? m_groups.at(row).name : QString());
    // NOTE: 上移/下移**不在这儿启用/禁用** —— 它们画在每一行自己的行尾，挪不动的那一侧
    // 根本不画（判据是 ui/GroupListDelegate::canMoveUp()/canMoveDown()，和
    // eventFilter() 认下那一下读的是同一对函数）。这一点和"选中的那一组"没关系，
    // 所以刷按钮可用性这条路上没有它们的事。

    // 勾选框跟着选中的那一组走。摆状态**必须挡一下**：setChecked() 会发 toggled，
    // 而那个槽会把这次"摆"当成用户点的一下，绕回去改一遍状态（然后又是下一次刷新……）。
    m_updatingExpanded = true;
    m_expandedCheck->setEnabled(hasSelection);
    m_expandedCheck->setChecked(hasSelection && m_groups.at(row).expanded);
    m_updatingExpanded = false;

    // 「设为置顶组」和上面那颗同一个写法、同一个理由（既挡"用代码摆状态"那一下，也管
    // "没选中任何组"时那颗灰着、摆成 false）。两颗**各自一个** m_updating*：摆第一颗时
    // 的复位不能把第二颗那一发也放过去。
    m_updatingPinned = true;
    m_pinnedCheck->setEnabled(hasSelection);
    m_pinnedCheck->setChecked(hasSelection && m_groups.at(row).pinned);
    m_updatingPinned = false;
}
