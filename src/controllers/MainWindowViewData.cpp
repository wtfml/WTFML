/**
 * @file MainWindowViewData.cpp
 * @brief `MainWindowViewData` 的实现（从 MainWindow.cpp 那个一万三千行的文件里搬出来）。
 *
 * 这里面每一段都带着原来那份注释 —— 它们解释的是"为什么这么翻"，和搬迁前一个字不差。
 * 只做了一件事：把**没人再用**的几段（世界设置那两趟翻译的旧副本、`syncComboToTree`、
 * 对话框那几份 `toDialog*` / `windowMode*`）删掉 —— 它们的正本已经在
 * `WorldSettingsPage` / `ConnectionsController` / `InstanceDialogController` 里。
 */

#include "controllers/MainWindowViewData.hpp"

#include <QFileInfo>
#include <QScrollBar>
#include <QTimer>

#include <QEvent>

namespace MainWindowViewData {

/**
 * @brief 读下拉框里选中的版本号（**不是**显示文本）。
 * @param combo 版本下拉框。
 * @return 选中的版本号；没选中任何项时返回空串。
 *
 * NOTE: 取的是 Qt::UserRole 里存的裸 id，不是 currentText()。这两个值**是分家的**：
 * 条目文本可能是 `我的整合包-1.20.1-Forge`，而真正进
 * `--version` 和文件路径的永远是 id。拿文本当版本号用会一路静默地拼出错误路径。
 *
 * currentIndex() 为 -1 表示"当前什么都没选"（下拉框空着，或者刚好被 clear() 过），
 * 这时 currentData() 也要当没选。显式写 currentIndex() 是为了说清为什么空串可以当
 * 判据：任何版本号都不会是空串。
 */
QString selectedVersionId(const QComboBox *combo) {
    return combo->currentIndex() < 0 ? QString() : combo->currentData().toString();
}

// 任务账本到界面 DTO 的枚举翻译由 ActivityCenterController 统一负责。

/**
 * @brief 一串步骤名 ⇒ 账本要的那张步骤表（`Tasks::TaskStep`），第 @p runningIndex 格是
 * 正在跑的那一格。
 *
 * 服务端安装那四段报步骤的方式是"一条直线、一格一格往前走"（`ServerInstallRun::stepNames()`
 * 给名字，`stepsChanged` / `stepChanged` 给走到哪儿了），而账本要的是**整张表**。这一处就是
 * 那个翻译：下标小于正在跑的都算跑完了、等于的算在跑、大于的还是待办。
 *
 * @param runningIndex 负数 = 还没开始（全都算待办）。
 */
QList<Tasks::TaskStep> taskStepsFromNames(const QStringList &names, int runningIndex) {
    QList<Tasks::TaskStep> steps;
    steps.reserve(names.size());
    for (int i = 0; i < names.size(); ++i) {
        Tasks::TaskStep step;
        step.name = names.at(i);
        if (i < runningIndex) {
            step.state = Tasks::TaskStepState::Done;
        } else if (i == runningIndex) {
            step.state = Tasks::TaskStepState::Running;
        } else {
            step.state = Tasks::TaskStepState::Pending;
        }
        steps.append(step);
    }
    return steps;
}

QList<Tasks::TaskStep> modpackOneClickSteps() {
    return taskStepsFromNames(
        {QObject::tr("下载整合包文件"), QObject::tr("解析清单与检查环境"),
         QObject::tr("安装原版与加载器"), QObject::tr("下载模组与清单文件"),
         QObject::tr("解压覆盖文件"), QObject::tr("校验文件并完成")},
        0);
}

/**
 * @brief 把一批版本灌进下拉框，并尽量保住用户原来选中的那一项。
 * @param combo 目标下拉框。
 * @param parts 每个版本的显示名组成部分。
 *
 * 条目文本是 formatVersionName() 拼出来的显示名，版本号存在 Qt::UserRole 里，
 * 两者由 selectedVersionId() / currentText() 分开取。同一个版本在两个下拉框里
 * 可能显示得不一样（比如一个 `我的整合包-1.20.1`、一个 `1.20.1`），这是两个数据源
 * 各自独立设计的必然结果：已安装那边能从磁盘读到自定义名，在线清单 800+ 条只填 id。
 * 别为了"看着一致"给在线清单加逐条文件 I/O。
 *
 * clear() 会把用户选中的版本一起丢掉，所以刷新必须自己把选中项捡回来。
 *
 * 这一层只管"保住"：原来选的还在清单里就选回去，不在了就停在 currentIndex() == -1。
 * 停在 -1 之后挑哪一个由调用方按 resolveCurrentInstance() 定（有实例必有当前实例，
 * 见 MainWindow::refreshInstalledVersions()）。
 */
void fillVersionCombo(QComboBox *combo, const QList<VersionDisplayParts> &parts) {
    const QString previous = selectedVersionId(combo);

    // 重灌过程会发 currentIndexChanged。现在接的这些槽都是幂等的，但先挡上，
    // 免得以后有人接槽时被中间态（先变成 -1 再选中）刷两遍。
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const VersionDisplayParts &part : parts) {
        // NOTE: 必须逐条 addItem(显示名, id)。addItems(QStringList) 是**不设**
        // Qt::UserRole 的，用它就等于所有条目的 currentData() 都是无效 QVariant，
        // selectedVersionId() 会一路返回空串 —— 启动按钮就再也启不动任何版本了。
        combo->addItem(formatVersionName(part), part.id);
    }

    // NOTE: 别指望 QComboBox 自己选中第 0 项。只要下拉框设了非空 placeholderText，
    // 往空模型里插第一批数据时它不会自动选中任何一项。选中项只能自己恢复。
    // 也必须按 id（findData）找回来，不能按显示文本（findText）——显示名会随
    // 自定义名/开关变，拿它当身份标识一定找错。
    if (previous.isEmpty()) {
        return;
    }
    if (const int restored = combo->findData(previous); restored >= 0) {
        combo->setCurrentIndex(restored);
    }
}

// ======================================================================
// 实例页那张树（分组）
// ======================================================================
//
// 这一组自由函数和上面 `fillVersionCombo` 那一类直筒子列表的写法**不能合并**：实例列表
// 要两层（组 + 实例）。硬合并的写法是"到处判 item 是组还是实例"，
// 那样每多一个调用点就多一次判错的机会。
//
// NOTE: 下载页那份"可安装版本"是版本树，不是下拉框 + 列表，所以这一组只服务实例页。

/**
 * @brief 一个树条目代表的实例 id；**组头返回空串**。
 * @param item 树条目；nullptr 也返回空串。
 *
 * 所有读这棵树的地方都必须过这一道，不许直接 `currentItem()->data(0, Qt::UserRole)`。
 * 组头天然能被选中（QTreeWidget::setCurrentItem() 不看 ItemIsSelectable），而组头上
 * 那个 role 是空的 —— 界面于是悄悄变成"没选中任何实例"，用户按启动得到"未选择版本"，
 * 而屏幕上明明有一行亮着。
 *
 * NOTE: 判据是**它有没有组 id 那个 role**，而不是"实例 id 是不是空的"。后者在
 * "某个实例的 id 恰好是空串"时会误判，而这个判据不该依赖那个前提。
 */
QString instanceIdOf(const QTreeWidgetItem *item) {
    if (item == nullptr) {
        return QString();
    }
    if (item->data(0, MainWindowUI::groupIdRole()).isValid()) {
        return QString();
    }
    return item->data(0, MainWindowUI::instanceIdRole()).toString();
}

/**
 * @brief 按分组把实例灌进实例页那张树。
 * @param tree 目标树。
 * @param buckets 组（含虚拟的「未分组」，见 InstanceGroupBucket）。
 * @param runningIds 此刻在跑的那些实例 id（显示名后面那句「（运行中）」）。
 * @param marks 每个实例行前面那个小图标（`id -> QIcon`，由 instanceRowMark() 算好）。
 * 缺一个不画图标，不影响别的（验证工程直接摆一张只有几个键的表也能灌）。
 * @param pinnedGroupId **置顶组**是哪一个（InstanceGroupStore::pinnedGroupId()）。空串 =
 * 这份 .minecraft 里没有置顶组 ⇒ 哪一行都不摆那格箭头（点下去没有去处）。
 * 非空时按 @p pinnedInstanceIds 给两个互斥的角色：不在里面 ⇒ `rowPinRole()`
 * （悬停画 ↑ 置顶）、在里面 ⇒ `rowUnpinRole()`（悬停画 ↓ 取消置顶）。两个角色都由
 * fillInstanceTree 写，代理/eventFilter 两边读。
 * @param pinnedInstanceIds 此刻**正在置顶**的那些实例 id（`InstanceGroupStore::isPinned()`）。
 * 它和 @p pinnedGroupId 是**两件事**：前者是"哪些实例置着顶"、后者只是"这一份
 * .minecraft 里有没有置顶组"。有置顶组但一个实例都没置顶时前者是空的、后者非空。
 *
 * NOTE: 【那两个角色为什么按**实例**给，而不是按"这一行画在哪一组"给】
 * "这一行的组 id 就是置顶组"和"这个实例置着顶"**不等值**：置着顶的那个实例在**两个
 * 组里各有一行**，两行说的是同一个实例的同一个状态。按"画在哪一组"给的话老家那一行
 * 会画成 ↑（= 置顶），而它其实已经置着顶了 —— 点下去是"再置一次"（空操作），在用户
 * 眼里就是"这颗按了没用"。所以判据是**实例自己**置没置顶，两行拿到同一个答案（都画 ↓）。
 *
 * 两种形态，由调用方给进来的 buckets 决定（它才知道有没有真实分组）：
 * - **只有一格、groupId 为空** → 平铺，不插组头。这是"还没分过组"的样子。
 * - 否则每格画一个组头，实例挂在它下面。**一个实例都没有的组不画**（空白的分组隐藏）—— 藏起来的只是那行标题，组本身还在 wtfml.folders 里、
 * 还在「分组设置…」里，也照样能被接收实例（那颗齿轮的第一项列的是**全部**组，
 * 含这些藏着的，见 MainWindowUI::openGroupPicker()）。
 *
 * NOTE: 灌完**不动选中项**（和 fillVersionList 一致），收尾由调用方 syncTreeToCombo()。
 * NOTE: 每个实例条目的 tooltip 是**目录的完整路径**。显示名和目录名可能不一样
 * （自定义名、拼进去的加载器名、或者用户在资源管理器里改过目录名），而
 * "这个实例到底在哪个目录"是排查问题时唯一要问的东西。
 */
/// 一个实例在树里那一行该写什么：显示名（后面缀上"（运行中）"当它真的在跑时）。
///
/// **唯一一处**拼这一行 —— 灌树（fillInstanceTree）和游戏起落时那次局部刷新
/// （MainWindow::updateInstanceTreeRunState）都走它。分开写的话，游戏起来那一刻
/// 后缀会跟灌树时那版慢慢漂开。
QString instanceTreeItemText(const VersionDisplayParts &part, bool running) {
    const QString base = formatVersionName(part);
    return running ? base + MainWindowUI::instanceRunningSuffixText() : base;
}

void fillInstanceTree(QTreeWidget *tree, const QList<InstanceGroupBucket> &buckets,
                      const QSet<QString> &runningIds,
                      const QHash<QString, QIcon> &marks, const QString &pinnedGroupId,
                      const QSet<QString> &pinnedInstanceIds) {
    const QSignalBlocker blocker(tree);
    tree->clear();

    // 平铺还是插组头，按**调用方给的** buckets 判（它才知道那一份 .minecraft 里有没有
    // 真实分组），不看下面滤掉空组之后剩下什么：分组全空的那一档确实一行都不画，
    // 但那仍然是"分过组的"，形态不该因此翻过来。
    const bool flat = buckets.size() == 1 && buckets.first().groupId.isEmpty();
    for (const InstanceGroupBucket &bucket : buckets) {
        // 一个实例都没有的组**不画**（见函数头）。平铺那一格不参与：那不是一个组。
        if (!flat && bucket.parts.isEmpty()) {
            continue;
        }
        QTreeWidgetItem *parent = nullptr;
        if (!flat) {
            parent = new QTreeWidgetItem(tree);
            parent->setText(0, MainWindowUI::groupItemText(bucket.name, bucket.parts.size()));
            parent->setData(0, MainWindowUI::groupIdRole(), bucket.groupId);
            // 行尾那颗**齿轮**：几何的唯一出处是 InstanceGroupDelegate::gearRect()，
            // 认下那一下的是 MainWindow::eventFilter()。虚拟的「未分组」那一格也挂
            // （它是"把实例放进某个组"的最后一条路，见 MainWindowUI::rowGroupGearRole()）。
            parent->setData(0, MainWindowUI::rowGroupGearRole(), true);
            // 组头**不给选中**：它是"一堆东西的标题"，不是"一个能启动的实例"。
            // 清掉 ItemIsSelectable 之后，选择模型会拒绝把它放进"被选中"的集合，
            // 所以点它**不会亮**（ui_check 的 M 段真的发了一次鼠标事件钉住这条）。
            //
            // NOTE: 但**它仍然会变成 currentItem()** —— QAbstractItemView 在
            // mousePressEvent 里只查 ItemIsEnabled 就把当前项挪过去了，选择命令
            // 是后一步的事。所以这一行是"别让它高亮"，**不是**"别让它被选中"；
            // 真正兜住"当前选中的是哪个实例"的是 instanceIdOf() 那道守卫，
            // 每一个读 currentItem() 的地方都绕不过去。
            parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
            // 展开还是收着**由那一组自己记着**（bucket.expanded ← wtfml.folders 里那一格）：
            // 用户点一下组行就翻过来并落盘，于是"下次打开还照这样画"。默认那五个组都是
            // 展开的（新组的默认值也一样）—— 折起来的组里有什么，用户得点一下才知道。
            parent->setExpanded(bucket.expanded);
        }
        // 平铺时挂在**不可见的根**下面，那和"顶层条目"是一回事，而且省掉了
        // "parent 是空的就换一个构造函数"那条分叉（两个构造函数收的类型不一样）。
        QTreeWidgetItem *holder = parent != nullptr ? parent : tree->invisibleRootItem();
        for (const VersionDisplayParts &part : bucket.parts) {
            auto *item = new QTreeWidgetItem(holder);
            item->setText(0, instanceTreeItemText(part, runningIds.contains(part.id)));
            item->setData(0, MainWindowUI::instanceIdRole(), part.id);
            item->setToolTip(0, QDir::toNativeSeparators(AppPaths::versionDir(part.id)));
            // 行前面那个小图标（原版草方块 / Forge 铁砧…）。谁画的、怎么认出来的
            // 见 instanceRowMark() 那段 —— 这一层只负责把它挂上去。
            const auto mark = marks.constFind(part.id);
            if (mark != marks.constEnd()) {
                item->setIcon(0, mark.value());
            }
            // 中间那一格（**一对反义词共用的一格**，见 MainWindowUI::rowUnpinRole() 那段）
            // 的判据：画它的是 InstanceGroupDelegate::paint()、认下那一下的是
            // MainWindow::eventFilter()，三处读的都是**这两个角色**。
            // - `rowPinRole()` = 还没置顶 ⇒ 悬停画 ↑「置顶」；
            // - `rowUnpinRole()` = 已经置顶了 ⇒ 悬停画 ↓「取消置顶」（置顶组里的实例悬停时
            // 右侧显示下箭头，即取消置顶）；
            // - 两个都假 = **这一份 .minecraft 里没有置顶组**（pinnedGroupId 是空串：
            // 那一格点下去没有去处，见 InstanceGroupStore::pinnedGroupId() 那三条出口）
            // ⇒ 这一格空着、也不弹话。
            // 两个判据互斥（同一个 pinned，一正一反），所以这一格永远只有一个方向。
            //
            // NOTE: 判据是**这个实例**置没置顶（见函数头那条 NOTE）—— 所以置着顶的那个实例
            // 在老家和置顶组里的**那两行画的是同一颗 ↓**，不是一个 ↑ 一个 ↓。
            // NOTE: "已经在置顶组里 ⇒ 不画上箭头"：它仍然是"不画 ↑"，画的是 ↓
            // （"挪不动的不画"这条先例在 ui/GroupListDelegate 里）。
            const bool pinned = pinnedInstanceIds.contains(part.id);
            item->setData(0, MainWindowUI::rowPinRole(),
                          !pinnedGroupId.isEmpty() && !pinned);
            item->setData(0, MainWindowUI::rowUnpinRole(),
                          !pinnedGroupId.isEmpty() && pinned);
        }
    }

    // NOTE: 组行**只有两个动作**：点这一行=展开/收起、点齿轮=那一组的更多操作。
    // 组序的上下移动不在这里 —— 它在「分组设置…」那张表里，且"能不能挪"是算出来的
    // （行序就是组序），不需要角色位，见 ui/GroupListDelegate。
}

/**
 * @brief 让实例树里的选中项跟上下拉框（两边都按 id 找，不按文本）。
 * @param combo 版本下拉框。
 * @param tree 实例页那张树。
 *
 * 找不到那个 id（清单刚刷新、实例被删了）时**清空选中**，和列表版一样。
 */
void syncTreeToCombo(const QComboBox *combo, QTreeWidget *tree) {
    const QString id = selectedVersionId(combo);
    // 挡信号：这里是"跟着别人变"，不该反过来再把下拉框动一遍。真正的用户操作才发信号。
    const QSignalBlocker blocker(tree);
    if (!id.isEmpty()) {
        QTreeWidgetItemIterator it(tree);
        while (*it != nullptr) {
            if (instanceIdOf(*it) == id) {
                tree->setCurrentItem(*it);
                return;
            }
            ++it;
        }
    }
    // 没找到（或者下拉框本来就没选）：留空，别硬选一个别的。
    tree->setCurrentItem(nullptr);
}

/**
 * @brief 把实例树的滚动位置放回重建前那儿。
 * @param tree 实例页那张树（@c nullptr 当没事发生）。
 * @param value 重建前 @c verticalScrollBar()->value()。
 *
 * 为什么不是"选的时候别滚"：`setCurrentItem()` 落在**藏着的**树上是滚不动的（这会儿
 * range 还是 0），Qt 把这一滚攒到树**第一次显示、排完版**那一下才补 —— 拦在
 * `syncTreeToCombo()` 周围的 @c scrollTo() 挡不住它（量过：挡了照样 0 → 底）。
 * 所以只能等 range 真算出来、再等一轮事件循环（那一滚是 show 里同步做的）之后放回去。
 *
 * 树已经露着的那一趟（装完一个实例、改完分组之后重灌）排版是现成的，当场放回。
 */
namespace {
constexpr auto kScrollRestoreName = "wtfmlInstanceTreeScrollRestorer";

// One pending restore per tree. Show also matters when the range is already
// valid: hiding/reopening a populated tree need not emit rangeChanged again.
class TreeScrollRestorer final : public QObject {
public:
    explicit TreeScrollRestorer(QTreeWidget &tree) : QObject(&tree), m_tree(tree), m_timer(this) {
        setObjectName(QString::fromLatin1(kScrollRestoreName));
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (!m_pending || !m_tree.isVisible()) return;
            m_tree.verticalScrollBar()->setValue(m_value);
            m_pending = false;
        });
        connect(tree.verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { schedule(); });
        tree.installEventFilter(this);
        tree.viewport()->installEventFilter(this);
    }
    void restore(int value) {
        m_timer.stop();
        m_value = value;
        m_pending = true;
        if (m_tree.isVisible()) {
            m_tree.verticalScrollBar()->setValue(value);
            schedule(); // Rebuilding a visible tree can still defer its layout.
        }
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == &m_tree && event->type() == QEvent::Hide) {
            if (!m_pending) m_value = m_tree.verticalScrollBar()->value();
            m_pending = true;
            m_timer.stop();
        }
        if (m_pending && watched == &m_tree && event->type() == QEvent::Show) {
            m_tree.doItemsLayout();
            m_tree.verticalScrollBar()->setValue(m_value);
            schedule();
        }
        if (m_pending && watched == m_tree.viewport() && event->type() == QEvent::Paint) {
            // Animation snapshots render before the queued restore can run.
            m_tree.verticalScrollBar()->setValue(m_value);
            m_pending = false;
            m_timer.stop();
        }
        return false;
    }
private:
    void schedule() {
        if (m_pending && m_tree.isVisible()) m_timer.start(0);
    }
    QTreeWidget &m_tree;
    QTimer m_timer;
    int m_value = 0;
    bool m_pending = false;
};
} // namespace

void restoreTreeScroll(QTreeWidget *tree, int value) {
    if (!tree) return;
    auto *owner = tree->findChild<QObject *>(QString::fromLatin1(kScrollRestoreName), Qt::FindDirectChildrenOnly);
    auto *restorer = owner ? static_cast<TreeScrollRestorer *>(owner) : new TreeScrollRestorer(*tree);
    restorer->restore(value);
}

QList<InstanceGroupStore::Group> toStoreGroups(const QList<GroupDialog::Group> &from) {
    QList<InstanceGroupStore::Group> to;
    to.reserve(from.size());
    for (const GroupDialog::Group &group : from) {
        InstanceGroupStore::Group item;
        item.id = group.id;
        item.name = group.name;
        // 「默认展开」也搬过来：它在两边是同一个字段（不是"界面上的显示状态"），
        // 丢了的话在对话框里摆好的展开状态按一次确定就被重置回默认。
        item.expanded = group.expanded;
        // 「设为置顶组」同样要搬。这一格漏掉的表现很具体：用户在对话框里
        // 勾了置顶组、按确定，实例行行尾那颗上箭头**还是往老的那一组送** —— 勾选框像是
        // 没生效，而两份清单比出来"没变"的话连盘都不写（见 Group::operator==）。
        item.pinned = group.pinned;
        to.append(item);
    }
    return to;
}

QList<GroupDialog::Group> toDialogGroups(const QList<InstanceGroupStore::Group> &from) {
    QList<GroupDialog::Group> to;
    to.reserve(from.size());
    for (const InstanceGroupStore::Group &group : from) {
        GroupDialog::Group item;
        item.id = group.id;
        item.name = group.name;
        item.expanded = group.expanded;  // 见 toStoreGroups() 那段
        item.pinned = group.pinned;      // 见 toStoreGroups() 那段
        to.append(item);
    }
    return to;
}

// 「一个实例的启动五项覆盖」的两份表示之间的转换。理由和上面那两段一样：ui/ 不 include
// src/，所以 InstanceSettingsDialog 有自己的 Override、settings/ 有自己的
// JavaSettingsOverride，逐字段相同、由这两段循环在两边搬。

/**
 * @brief 组齿轮菜单（以及它里面"移入分组…"弹出的挑目标框）那一刻的那份**快照**。
 * @param ui 主界面（组行都长在它的实例树里）。
 * @param store 分组存储（组的清单、顺序、展开状态从这儿来）。
 * @param selectedKey 此刻选中的那个实例的归属键（instanceKey()）；空串 = 没选中。
 * @param selectedHomeGroupId 那个实例的**老家**在哪一组（`homeGroupIdOf()`）；空串 = 未分组
 * （也含"没选中"那一档，见下面的 NOTE）。挑目标那个框里被灰掉、写着「已经在这一组
 * 里了」的就是它。
 *
 * NOTE: 实例数是在**当前那张树**上数的（它就是用户此刻看到的东西）：藏起来的组不在树上
 * ⇒ 它是 0 —— 那正好也是"这一组里有几个实例"在那一刻的实话。置顶着的那一条在两个
 * 组里各画一行，所以那两个数里各算它一次 —— 这正是"那一组里有几行"的实话。
 *
 * NOTE: 【为什么老家由调用方给，不在这张树上现看】 "选中的那一条画在哪个组下面"
 * 不能当归属：同一个实例有**两行**（老家一行 + 置顶组里一行），"画在哪一组"取决于用户
 * 点的是哪一行 —— 同一个实例会给出两个不同的答案：点老家那行时「置顶」那格亮着，点置顶
 * 组那行时又变成老家那格亮着。而这一格灰掉的意思是"已经在这一组里了、点它没用"，
 * 跟"你刚好点的是哪一行"没关系。所以判据是老家 —— 它是实例自己的属性，两行拿到
 * 同一个答案。老家那个值 `homeGroupIdOf()` 只有 MainWindow 算得出来（要 VersionManager
 * 和版本类型），这个自由函数手上只有 ui/ 和 store，所以由调用方递进来。
 *
 * NOTE: 它是个自由函数（不是 MainWindow 的成员），因为 MainWindowUI::GroupRow 是 ui/ 里的
 * 嵌套类型，而 MainWindow.hpp **不 include ui/ 的头**（见那个文件头那几条前置声明）。
 * 摆在这儿的那几个私有成员函数只收窄（组 id / 一整行）就不必把它写进头文件。
 */
QList<MainWindowUI::GroupRow> groupRowsForMenu(const MainWindowUI *ui,
                                               const InstanceGroupStore *store,
                                               const QString &selectedKey,
                                               const QString &selectedHomeGroupId) {
    QHash<QString, int> counts;
    const QTreeWidget *tree = ui->getInstanceList();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *top = tree->topLevelItem(i);
        const QVariant groupId = top->data(0, MainWindowUI::groupIdRole());
        if (!groupId.isValid()) {
            // 平铺那一档（一个真实分组都没有时）：顶层条目**就是**实例本身，没有组头
            // 可数 —— 那一档连「未分组」那格都不摆（见下面 `!groups.isEmpty()` 那道闸），
            // 所以这儿一个数都不记，谁也不会来问。
            continue;
        }
        counts.insert(groupId.toString(), top->childCount());
    }

    const bool hasSelection = !selectedKey.isEmpty();
    QList<MainWindowUI::GroupRow> rows;
    const QList<InstanceGroupStore::Group> groups = store->groups();
    rows.reserve(groups.size() + 1);
    for (const InstanceGroupStore::Group &group : groups) {
        MainWindowUI::GroupRow row;
        row.groupId = group.id;
        row.name = group.name;
        row.memberCount = counts.value(group.id, 0);
        row.expanded = group.expanded;
        row.hasSelection = hasSelection;
        row.containsSelected = hasSelection && selectedHomeGroupId == group.id;
        rows.append(row);
    }
    // 虚拟的「未分组」排在最后（和树上同一个位置）。**只有真的分过组才摆它** ——
    // 一个真实分组都没有时列表是平铺的，那颗齿轮压根没地方挂，这张表也就没人看得见。
    if (!groups.isEmpty()) {
        MainWindowUI::GroupRow rest;
        rest.name = MainWindowUI::ungroupedGroupText();
        rest.memberCount = counts.value(QString(), 0);
        // 它没有记录（那一格不落盘），树上每次都是展开的 —— 快照照实说。
        rest.expanded = true;
        rest.hasSelection = hasSelection;
        rest.containsSelected = hasSelection && selectedHomeGroupId.isEmpty();
        rest.virtualGroup = true;
        rows.append(rest);
    }
    return rows;
}

/**
 * @brief 某一组此刻的那份快照（groupRowsForMenu() 里对应的那一条）。
 * @param row 拿到的那一行。@param selectedKey / @param selectedHomeGroupId 见
 * groupRowsForMenu()（两个都缺省 = 没选中实例）。
 * @return 这一组不在（菜单开着的时候被别处删了）时返回 false，@p row 原样不动。
 *
 * NOTE: 虚拟的「未分组」的组 id 就是空串，所以传空串进来拿到的是**它**，不是"没找到"。
 * 两个调用点要的都是"这一刻的样子"，所以它每次都重新翻一遍快照，不复用菜单里
 * 那一份 —— 菜单摆出来之后用户可能看两眼才点，这中间别处一行代码就能把它删了。
 */
bool groupRowFor(const MainWindowUI *ui, const InstanceGroupStore *store, const QString &groupId,
                 MainWindowUI::GroupRow *row, const QString &selectedKey,
                 const QString &selectedHomeGroupId) {
    const QList<MainWindowUI::GroupRow> rows =
        groupRowsForMenu(ui, store, selectedKey, selectedHomeGroupId);
    for (const MainWindowUI::GroupRow &item : rows) {
        if (item.groupId == groupId) {
            if (row != nullptr) {
                *row = item;
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief 某一组那一行行尾**齿轮**占的那一格，全局坐标（弹挑目标框时的锚点）。
 * @return 那一行不在树上（一个实例都没有的组是藏着的）时返回空矩形。
 *
 * 几何的唯一出处是 InstanceGroupDelegate::gearRect() —— 和代理画它、eventFilter 认下
 * 那一下用的是同一个式子（写歪了的症状就是"看着点中了齿轮、其实点的是那一行"）。
 */
QRect gearAnchorFor(const MainWindowUI *ui, const QString &groupId) {
    QTreeWidget *tree = ui->getInstanceList();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(0, MainWindowUI::groupIdRole()).toString() != groupId
            || !item->data(0, MainWindowUI::rowGroupGearRole()).toBool()) {
            continue;
        }
        const QRect gear = InstanceGroupDelegate::gearRect(tree->visualItemRect(item));
        return QRect(tree->viewport()->mapToGlobal(gear.topLeft()), gear.size());
    }
    return QRect();
}

JavaSettingsOverride toSettingsOverride(const InstanceSettingsDialog::Override &from) {
    JavaSettingsOverride to;
    to.javaPath = from.javaPath;
    to.xmx = from.xmx;
    to.xms = from.xms;
    to.autoMemory = from.autoMemory;
    to.extraJvmArgs = from.extraJvmArgs;
    return to;
}

/// 上面那个的反方向：对话框摆好的覆盖 → 要写进 meta 的那一组。
///
/// NOTE: 代号认不出来时**当没覆盖**（= 删键）而不是"退回小窗那一档" —— 认不出来只可能
/// 来自手改过的 meta 或旧程序写下的别的值，意思都是"不认识"，把它写成一个具体的
/// 档位等于替用户做了个他从没做过的选择。读侧（gameWindowOverrideFromJson）同一个态度。
GameWindowOverride toWindowOverride(const InstanceSettingsDialog::Override &from) {
    GameWindowOverride to;
    if (from.windowMode.has_value()) {
        GameWindowMode mode = GameWindowMode::Windowed;
        if (gameWindowModeFromToken(*from.windowMode, &mode)) {
            to.mode = mode;
        }
    }
    // 宽和高一起（对话框那边要么两个都有值、要么两个都没有，见 .hpp 里 override() 那段）。
    to.width = from.windowWidth;
    to.height = from.windowHeight;
    return to;
}


// ---------------------------------------------------------------------------
// 底栏那两块上拉面板（左：正在跑的实例 · 右：最近用过的账号）
// ---------------------------------------------------------------------------

/// 左面板 / 任务中心运行中进程行行尾的三颗按钮（客户端和服务端位置、图标一致）：
/// 电源（0）= 结束/停止，齿轮（1）= 实例或服务端设置，日志图标（2）= 打开日志窗/控制台。
/// @param ink 图标那一档颜色（和底栏那几行字同一个墨色 —— 画的图标不带自己的颜色）。
QList<MainWindowUI::PanelAction> runningPanelActions(const QColor &ink) {
    const int iconSize = MainWindowUI::bottomBarPanelActionIconSize();
    MainWindowUI::PanelAction stop;
    stop.icon = VersionIcons::power(iconSize, ink);
    // 图标按钮**必须**有 tooltip：一个电源符号说明不了"点了会把游戏直接杀掉、不会存档"。
    stop.tooltip = QObject::tr("关闭这个实例（直接结束游戏进程，游戏不会存档）");
    MainWindowUI::PanelAction settings;
    settings.icon = VersionIcons::gear(iconSize, Theme::tokens().primary);
    settings.tooltip = QObject::tr("打开这个实例的设置");
    MainWindowUI::PanelAction log;
    log.icon = VersionIcons::line(QStringLiteral("log"), iconSize, ink);
    log.tooltip = QObject::tr("打开这一场游戏的日志窗");
    return {stop, settings, log};
}

QList<MainWindowUI::PanelAction> runningServerPanelActions(const QColor &ink, bool stopping,
                                                           bool forceKillAllowed) {
    const int iconSize = MainWindowUI::bottomBarPanelActionIconSize();
    MainWindowUI::PanelAction stop;
    stop.icon = VersionIcons::power(iconSize, ink);
    if (!stopping) {
        stop.enabled = true;
        stop.tooltip = QObject::tr("停止这个服务端（先存盘并正常退出）");
    } else if (forceKillAllowed) {
        stop.enabled = true;
        stop.tooltip = QObject::tr("存盘等待已超时，点击强制结束这个服务端");
    } else {
        stop.enabled = false;
        stop.tooltip = QObject::tr("正在发送停服指令并等待存盘退出…");
    }
    MainWindowUI::PanelAction settings;
    settings.icon = VersionIcons::gear(iconSize, Theme::tokens().primary);
    settings.tooltip = QObject::tr("打开这个服务端的设置");
    MainWindowUI::PanelAction console;
    console.icon = VersionIcons::line(QStringLiteral("log"), iconSize, ink);
    console.tooltip = QObject::tr("打开服务端控制台");
    return {stop, settings, console};
}

QPixmap serverRowMark(ServerInstall::Flavor flavor, const QString &serverId, int size) {
    switch (flavor) {
    case ServerInstall::Flavor::Fabric:
        return VersionIcons::loaderMark(0, size);
    case ServerInstall::Flavor::Forge:
        return VersionIcons::loaderMark(1, size);
    case ServerInstall::Flavor::NeoForge:
        return VersionIcons::loaderMark(2, size);
    case ServerInstall::Flavor::Vanilla:
        return VersionIcons::grassBlock(size);
    default:
        return VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(serverId), size);
    }
}

/// 这个实例属于哪个分类（正式版 / 快照版 / 远古版 / 愚人节版）。
///
/// 单一出处是 VersionCategory::channelFor()（和下载页那棵树**同一个出处**，别在这儿另写
/// 一个 switch：写歪了的症状是"快照那一栏前面白草、远古那一栏还是绿草"，不报错、只是难看）。
/// 那条要一个 `type` 字段，两个地方能给：
/// - wtfml.meta 里手写的 `type`（**优先**：用户明确写下的东西盖过一切，他也可能是把
/// 一个实例"装作"另一个分类）；
/// - 没有就回实例自己的 `versions/<id>/<id>.json` 里读 —— 那才是这个字段的出处。
/// 两个都读不到（meta 丢了、json 也被删了）就当**正式版**：大多数实例是正式版，而
/// "一个方块都不画"在界面上是个说不清的空白。
///
/// 【`type` 必须能回落到那份 json 去读】
/// 只从 meta 读不够：全项目没有任何一处往 meta 里写过它（写侧那半从来没做出来）。
/// 于是这个函数对每一个已装实例都退化成"正式版"，看得见的后果有三处：快照版实例
/// 自动归类跑进「原版」组、默认组「快照版 / 远古版」永远是空的、实例页那一行的图标
/// （见 instanceRowMark()）全是草方块。"给每个版本一个对的图标"的底下就是这一条。
///
/// 所以选的是**回 json 读**，不是"装的时候往 meta 里抄一份 type"：
/// - json 本来就是这个字段的出处。抄一份出去就有两个可能不一致的地方（用户在资源管理器
/// 里换掉一份 json、或者把同一个版本号重装一遍，meta 里那份就过期了，而且看不出过期）；
/// - `getVersionJson()` 给的是**合并过**的那一份（顺 inheritsFrom 链），所以整合包实例
/// 的 type 是从它继承的原版那一层来的 —— 那正是它该显示的分类。而装的时候手里只有
/// `pack.gameVersion` 这个版本号，想写 type 反而要再回清单里查一次。
/// 代价是每个"meta 里没写 type 的"实例第一次问它时要读+解析一次那份 json；结果在
/// VersionManager 里带着（跟着合并缓存那一格走，见 `InstanceTraits`），所以一次会话里
/// 每个实例只读一次。
///
/// NOTE: 两条判据（meta 里的 `type`、json 里的 `type`）现在都由
/// `VersionManager::instanceTraits()` 一次答完 —— 它和"这个实例是哪个加载器"共用一次
/// meta 读。分开问的话，实例树每刷新一次就要对每个实例多问两遍 stat（行首图标一遍、
/// 自动归类一遍），200 个实例上那是几十毫秒。
VersionCategory::Channel versionChannelOf(const VersionManager *versions, const QString &versionId) {
    QString type = versions == nullptr ? QString() : versions->instanceTraits(versionId).type;
    // "文件不在就不问 json"那道闸不在这里了：它在 instanceTraits() 里面（同一条理由 ——
    // getVersionJson() 找不到文件会 qWarning，而这个函数每次刷新都对每个实例跑一遍）。
    if (type.isEmpty()) {
        type = QStringLiteral("release");
    }
    return VersionCategory::channelFor(versionId, type);
}

/// 这个实例该画哪个分类的方块（草方块 / 覆雪草块 / 土块 / 幸运方块）。
int versionChannelIndexOf(const VersionManager *versions, const QString &versionId) {
    return VersionCategory::channelIndex(versionChannelOf(versions, versionId));
}

/// 这个实例**实际**基于哪个加载器 —— 给界面看的名字（空串 = 没认出来）。
/// @param versionOut 认出来时顺手带上加载器版本（只有"加载器对不上"那道闸要用它）。
///
/// 判断与那两条规矩（"先看已经读过的那一格、为空再回扣一次版本 json"、"为什么必须回扣"）
/// 在 `InstancePagesController::loaderName()` 里。这个自由函数留着是因为 mw_check 的
/// 判据按名字驱动它，而且它在文件里还有十来处调用点。
QString instanceLoaderName(const VersionDisplayParts &parts, const VersionManager *versions,
                           QString *versionOut) {
    return InstancePagesController::loaderName(parts, versions, versionOut);
}

/// 加载器名字 → **分组用的那个小写键**（`VersionGroup::loader` 的写法）。
///
/// 两侧的词形**不是一套**，这是本仓库里最容易静默失灵的一处：`instanceLoaderName()`
/// 回的是显示名（「Fabric」/「Forge」/「NeoForge」/「Quilt」/「OptiFine」），而
/// `ResourceCatalog::groupVersions()` 的 `preferredLoader` 与 `VersionGroup::loader`
/// 收的都是小写那一套（「fabric」/「neoforge」…）。直接把显示名递进去 ⇒ 一个字都比不上
/// ⇒「把对应加载器排在前面」这件事**永久不生效，而屏幕上一点异常都没有**。
///
/// 认不出来（空串 / 别的加载器）回空串 = "没人在乎"，`groupVersions()` 就照旧排。
QString loaderGroupKey(const QString &loaderName) {
    return loaderName.trimmed().toLower();
}

/// 这个实例**没挑过图标**时该画哪一个方块（草方块 / 覆雪草块 / 土块 / 幸运方块，或者
/// 某个加载器的标志）—— 也叫它的「默认图标」。
///
/// 两条路，用的都是**另外两棵树上挂的同一份图标**（别在这儿另画一套）：
/// - **装了加载器的**（见 instanceLoaderName()：meta 里记着的，或者版本 json 自己说的）：
/// 给那个加载器的标志（Fabric / Forge / NeoForge / OptiFine），走
/// `VersionIcons::loaderMark()` —— 安装页那棵加载器树上挂的就是它；
/// - **没装加载器的**（原版 / 快照版 / 远古版 / 愚人节版）：按版本分类给，走
/// `VersionIcons::versionChannelMark()` —— 下载页那棵版本树上挂的就是它。
///
/// 于是"下载时就带"这件事**不需要任何落盘**：图标是从实例自己那份 json 的 `type` 现算的
/// （见 versionChannelOf()），而那份 json 是下载的时候写下去的 —— 装完第一次刷新就有图标，
/// 一个 type 都没写过的实例也一样算得出来。
///
/// NOTE: 加载器名 → 下标那一步用 `loaderKindFromIndex()` 反查，**不自己写一张"Forge 就是 1"
/// 的表**：那张表的唯一出处是 MainWindowUI::modLoaderOptionNames()（图标那边的下标
/// 就是照它排的），抄一遍就会在某次加加载器时慢慢漂开。
/// NOTE: 认不出来的加载器名（用户手写的别的名字、或者以后新加的加载器）也是草方块 ——
/// 和 loaderMark() 越界那条一个规矩：认不出来就画原版。
QPixmap defaultInstanceMark(const VersionDisplayParts &part, const VersionManager *versions,
                            int size) {
    // 加载器名和版本分类**一次问齐**（`instanceTraits()` 共用一次 meta 读）。分开问两遍
    // 的话，实例树每重灌一次、每个实例就多一遍 stat —— 200 个实例上这是几十毫秒的差价。
    const VersionManager::InstanceTraits traits =
        versions == nullptr ? VersionManager::InstanceTraits{} : versions->instanceTraits(part.id);
    // 安装器自己写下的那格压过 json 里认出来的（见 instanceLoaderName()）。
    const QString loader = part.loaderName.isEmpty() ? traits.loaderName : part.loaderName;
    if (!loader.isEmpty()) {
        for (int index = 0; index < MainWindowUI::modLoaderNoneIndex(); ++index) {
            VersionManager::LoaderKind kind;
            if (VersionManager::loaderKindFromIndex(index, &kind)
                && VersionManager::loaderDisplayName(kind) == loader) {
                return VersionIcons::loaderMark(index, size);
            }
        }
        return VersionIcons::grassBlock(size);
    }
    const QString type = traits.type.isEmpty() ? QStringLiteral("release") : traits.type;
    return VersionIcons::versionChannelMark(
        VersionCategory::channelIndex(VersionCategory::channelFor(part.id, type)), size);
}

/// 实例页某一行前面那个小图标（默认原版草块、forge 铁砧等；用户可以自己换一张，
/// 那一条在「实例设置 → 概览 → 图标」里）。
///
/// **挑过的压过没挑过的**：wtfml.meta 里有 icon 就按它走，没有才落到上面那个默认图标。
/// 三种形态的判据与写法见 VersionManager::iconField()（那是唯一出处，这里不再抄一遍）。
///
/// NOTE: 收 @p size 而不是自己去问两个 size 常量里的哪一个 —— 同一个实例的图标在实例树上和
/// 底栏「正在运行」那一行里**是同一张图**，只是边长不同（那两处各有各的常量）。
/// 让调用方递边长，这里就只需要一处判断。
/// NOTE: `builtin:` 那个前缀认不出来（用户手写了个 `builtin:草方块`）时**落到默认**而不是
/// 画一张空图 —— 和那几处"认不出来就当原版"同一个态度：屏幕上宁可显示一个不对的
/// 方块，也不要一个空洞。
/// NOTE: 本地那张走 VersionIcons::fileMark()（按路径 + 修改时间缓存）。**不在这儿直接
/// QPixmap(path)**：这个函数实例树每一行都要问一次，而每刷新一次整棵树重建一遍，
/// 用户挑的那张 1024×1024 的图会被解上成百遍。
QPixmap instanceRowMark(const VersionDisplayParts &part, const VersionManager *versions, int size) {
    const QString iconKey = VersionIcons::iconKeyFromValue(part.icon);
    if (!iconKey.isEmpty()) {
        const QPixmap mark = VersionIcons::iconMarkForKey(iconKey, size);
        if (!mark.isNull()) {
            return mark;
        }
    } else if (part.icon == InstanceSettingsDialog::localIconValue()) {
        // 「本地那张」= 实例目录下那份 icon.png（名字的唯一出处是 AppPaths::versionIconPath）。
        const QPixmap mark = VersionIcons::fileMark(AppPaths::versionIconPath(part.id), size);
        if (!mark.isNull()) {
            return mark;
        }
    }
    return defaultInstanceMark(part, versions, size);
}

/// 这一份 .minecraft 里**现在真的还在**的那些组 id。默认组被用户删掉之后它就不在里面了，
/// 于是一整组实例自动落到「未分组」—— 这正是「组被删掉后版本放到未分组里」那条规矩。
QSet<QString> knownGroupIdsOf(const InstanceGroupStore *groups) {
    QSet<QString> known;
    for (const InstanceGroupStore::Group &group : groups->groups()) {
        known.insert(group.id);
    }
    return known;
}

/// 一个实例**默认**该归到哪个默认组 —— 只给"用户还从来没给它表过态"的实例用（见调用点）。
///
/// 判据就是那五个组名：
/// - 装了模组加载器 → 可安装mod（判据见 instanceLoaderName()：**不只** wtfml.meta 那一格，
/// 版本 json 自己说的也算 —— PCL 那类合并实例 meta 里是空的，只按 meta 判会把装了加载器的
/// 实例全扔进「原版」，而「可安装mod」永远是空的）；
/// - 否则按版本分类：正式版 → 原版、快照版 → 快照版、远古版 → 远古版；
/// - **愚人节版没有对应的默认组**，返回空串 = 未分组。那五个里没有它，而把
/// 它塞进快照版是替用户做主（愚人节版的 type 也全是 snapshot，正是这个"顺理成章"
/// 的错法，见 VersionCategory.hpp 文件头）。
///
/// @param known 这一份 .minecraft 里**现在真的还在**的那些组 id。默认组被用户删掉之后
/// 它就不在 known 里了，于是一整组实例自动落到「未分组」—— 这正是「组被删掉后版本
/// 放到未分组里」那条规矩，为它写任何清理代码都是多余的。
QString defaultGroupIdFor(const VersionDisplayParts &part, const VersionManager *versions,
                          const QSet<QString> &known) {
    const auto existing = [&known](InstanceGroupStore::DefaultKind kind) {
        const QString id = InstanceGroupStore::defaultGroupId(kind);
        return known.contains(id) ? id : QString();
    };
    // 加载器那一条先判：装了加载器的快照版/远古版也算"可安装mod"（那才是它现在的样子）。
    //
    // 两条判据都由 `instanceTraits()` 一次答完（共用一次 meta 读）。这里**不能**改回
    // "先问 instanceLoaderName()、再问 versionChannelOf()"：那是每个实例两遍 stat，
    // 而自动归类在实例树每重灌一次时都要对每个实例跑一遍。
    const VersionManager::InstanceTraits traits =
        versions == nullptr ? VersionManager::InstanceTraits{} : versions->instanceTraits(part.id);
    const QString loader = part.loaderName.isEmpty() ? traits.loaderName : part.loaderName;
    if (!loader.isEmpty()) {
        return existing(InstanceGroupStore::DefaultKind::Moddable);
    }
    const QString type = traits.type.isEmpty() ? QStringLiteral("release") : traits.type;
    switch (VersionCategory::channelFor(part.id, type)) {
    case VersionCategory::Channel::Release:
        return existing(InstanceGroupStore::DefaultKind::Vanilla);
    case VersionCategory::Channel::Snapshot:
        return existing(InstanceGroupStore::DefaultKind::Snapshot);
    case VersionCategory::Channel::Old:
        return existing(InstanceGroupStore::DefaultKind::Old);
    case VersionCategory::Channel::AprilFools:
        break;
    }
    return QString();
}

/// 一个实例**显示在**哪一组里 —— 也叫它的「归属」，树上摆在哪一组就是它。
///
/// 和 `InstanceGroupStore::groupIdOf()`（**存的是**哪一组）不是一回事：从来没表过态的实例
/// 由显示层按版本分类自动归类（defaultGroupIdFor）。凡是"要说清这个实例现在归在哪"的地方
/// 都得走这一个函数 —— 设置对话框那一格和实例树各说各的话，用户就会看到"对话框里写着未
/// 分组、而树上明明把它摆在「原版」里"。
///
/// NOTE: 判据是"文件里有没有这一条"（isAssigned）、**不是"groupId 是不是空"**：用户在对话框
/// 里明确选了「未分组」的实例也算表过态（存的是那个保留值，读出来是空串），不该被自动
/// 归类又拉回默认组去 —— 否则就是"点了确定没生效"。
QString displayGroupIdOf(const InstanceGroupStore *groups, const QString &key,
                         const VersionDisplayParts &part, const VersionManager *versions,
                         const QSet<QString> &knownGroupIds) {
    QString groupId = groups->groupIdOf(key);
    if (groupId.isEmpty() && !groups->isAssigned(key)) {
        groupId = defaultGroupIdFor(part, versions, knownGroupIds);
    }
    return groupId;
}

/// 一个实例的「老家」—— 它**本来**属于哪一组（置顶组里那一条同名的只是多出来的）。
///
/// 置顶是**两边都显示**，所以"归属"不能只有一格（`groupIdOf()`）：置顶着的那一条在盘上是
/// "members 指着置顶组 + pinOrigin 记着老家"，于是"它本来在哪一组"成了**显示层要问的问题**
/// （树上那一条留在老家、实例设置里「分组…」那一格显示的也是它）。
///
/// 判据就是 `pinOriginOf()` 那三种形态，**逐条**对应 `unpinInstance()` 的落点 —— 两处必须
/// 看到同一个答案，否则"取消置顶之后它跳去了一个和对话框里写着的不一样的组"。所以：
/// - 没在置顶 ⇒ 就是 displayGroupIdOf()；
/// - 在置顶 + 记着一个组 ⇒ 那个组（**组已经没了就按"没有记录"处理**，见下）；
/// - 在置顶 + 记的是 null ⇒ 它置顶前没表过态 ⇒ 按版本类型现算一个（defaultGroupIdFor）；
/// - 在置顶 + 压根没有这一条 ⇒ 未分组（"用户自己塞进置顶组的"那一档）。
///
/// NOTE: 那个组被删掉之后**不写盘**（删组只清 members、pinOrigin 那一格要等下一次写盘的
/// 自清理）⇒ 这里必须自己判一次"那一组还在不在"，不然会返回一个**树上没有的组 id**，
/// 于是那一条同名的哪儿都不出现、看着像凭空少了一行。`unpinInstance()` 之所以不用
/// 判，是因为它紧接着就要写盘、那一次写盘本身就会把悬空的记录清掉。
QString homeGroupIdOf(const InstanceGroupStore *groups, const QString &key,
                      const VersionDisplayParts &part, const VersionManager *versions,
                      const QSet<QString> &knownGroupIds) {
    if (!groups->isPinned(key)) {
        return displayGroupIdOf(groups, key, part, versions, knownGroupIds);
    }
    QString origin;
    switch (groups->pinOriginOf(key, &origin)) {
    case InstanceGroupStore::PinOrigin::None:
        return QString();
    case InstanceGroupStore::PinOrigin::Auto:
        return defaultGroupIdFor(part, versions, knownGroupIds);
    case InstanceGroupStore::PinOrigin::Group:
        break;
    }
    if (origin == InstanceGroupStore::ungroupedMarker()) {
        return QString();
    }
    return knownGroupIds.contains(origin) ? origin : QString();
}

/// **一行 = 一个运行中进程**（客户端会话或服务端进程），按启动时间倒序排（最新启动的在最上面）。
QList<MainWindowUI::PanelRow> runningGamePanelRows(QList<RunningProcesses::Entry> processes,
                                                   const VersionManager *versions, const QColor &ink,
                                                   qint64 nowMs,
                                                   const std::function<QString(int)> &playTimeText) {
    QList<MainWindowUI::PanelRow> rows;
    rows.reserve(processes.size());
    // 一份都没在跑：摆一句说明（**不是**空白面板 —— 用户点开是想知道"有没有在跑"，
    // 空白答不了这个问题）。
    if (processes.isEmpty()) {
        MainWindowUI::PanelRow empty;
        empty.title = MainWindowUI::bottomBarPanelNoRunningTitle();
        empty.placeholder = true;
        rows.append(empty);
        return rows;
    }
    RunningProcesses::sortNewestFirst(processes);
    const int iconSize = MainWindowUI::bottomBarPanelIconSize();
    for (const RunningProcesses::Entry &proc : processes) {
        MainWindowUI::PanelRow row;
        if (proc.kind == RunningProcesses::Entry::Kind::Server) {
            row.icon = serverRowMark(proc.serverFlavor, proc.serverId, iconSize);
            row.title = proc.displayName;
            const auto serverSubtitle = [startedAtMs = proc.startedAtMs,
                                         stopping = proc.serverStopping]() -> QString {
                if (startedAtMs <= 0) {
                    return stopping ? QObject::tr("正在停止（存盘中）") : QObject::tr("服务端");
                }
                const QString dur = MainWindowUI::runningDurationText(
                    startedAtMs, QDateTime::currentMSecsSinceEpoch());
                return stopping ? QObject::tr("正在停止 · %1").arg(dur)
                                : QObject::tr("服务端 · %1").arg(dur);
            };
            if (proc.startedAtMs <= 0) {
                row.subtitle = serverSubtitle();
            } else {
                const QString dur = MainWindowUI::runningDurationText(proc.startedAtMs, nowMs);
                row.subtitle = proc.serverStopping
                                   ? QObject::tr("正在停止 · %1").arg(dur)
                                   : QObject::tr("服务端 · %1").arg(dur);
                row.liveSubtitle = serverSubtitle;
            }
            row.actions = runningServerPanelActions(ink, proc.serverStopping,
                                                    proc.serverForceKillAllowed);
            row.key = proc.key.isEmpty() ? RunningProcesses::makeServerKey(proc.serverId) : proc.key;
            rows.append(row);
            continue;
        }
        // 图标走**和实例树同一个函数**（instanceRowMark）：用户挑过就画他挑的那张，没挑过
        // 才按版本分类 / 加载器给。
        row.icon = instanceRowMark(versions->displayParts(proc.versionId), versions, iconSize);
        row.title = proc.displayName;
        row.subtitle = playTimeText(proc.sessionId);
        if (row.subtitle.isEmpty()) {
            row.subtitle = MainWindowUI::runningDurationText(proc.startedAtMs, nowMs);
        } else {
            row.liveSubtitle = [playTimeText, sessionId = proc.sessionId] {
                return playTimeText(sessionId);
            };
        }
        row.actions = runningPanelActions(ink);
        // 这一行是谁 = 那一份会话的编号（回来时按它认人，**不按"第几行"**）。
        row.key = proc.key.isEmpty() ? QString::number(proc.sessionId) : proc.key;
        rows.append(row);
    }
    return rows;
}

/// 一份**会话**的身份键：这个人是谁。
///
/// 和 `MainWindow::launchIdentityKey()` 是同一个规矩，**两处必须一致**（那边有整段说明）：
/// 离线会话的 `accountUuid` 是空的，拿 uuid 当键会把所有离线账号撞成同一个。
[[maybe_unused]] QString sessionIdentityKey(const LauncherManager::GameSession &session) {
    return session.accountUuid.isEmpty() ? session.accountName : session.accountUuid;
}

/// 「最近登录的几个账户」翻成账号弹层里的几行（几个由调用方给，见 kAccountPanelLimit）。
///
/// 口径：「最后登录的三个账户就拿缓存放（包含离线和正版，总共
/// 三个）」—— 读的就是**账号列表**那一条缓存（AccountStore::accounts()），不另外记账、
/// 不联网、不碰令牌。
///
/// 顺序按 `saved_at` **倒序**：那是这个列表里唯一一个"最近"（它写的是"这份登录凭据最后
/// 一次取到的时刻"，见 AccountStore::captureActive()）。NOTE: 切账号**不会**刷新它，所以
/// 准确的说法是"最近登录/添加过的三个"，而不是"最近用过的三个"—— 不新加一个字段是有意的
/// 取舍（新加键就得连带迁移已有数据、还有"登录取自"那句 tooltip 的语义）。
/// NOTE: `saved_at` 全是 `QDateTime::currentDateTimeUtc().toString(Qt::ISODate)` 写出来的
/// （唯一写方），形状一致，所以按文本比就是按时间比。
QList<MainWindowUI::PanelRow> recentAccountPanelRows(const QList<SavedAccount> &accounts, int limit,
                                                     const QHash<QString, QPixmap> &faces) {
    QList<MainWindowUI::PanelRow> rows;
    if (accounts.isEmpty()) {
        MainWindowUI::PanelRow empty;
        empty.title = MainWindowUI::bottomBarPanelNoAccountTitle();
        empty.placeholder = true;
        rows.append(empty);
        return rows;
    }

    QList<SavedAccount> sorted = accounts;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const SavedAccount &a, const SavedAccount &b) {
                         return a.savedAt > b.savedAt;
                     });
    if (limit > 0 && sorted.size() > limit) {
        sorted = sorted.mid(0, limit);
    }

    rows.reserve(sorted.size());
    for (const SavedAccount &account : sorted) {
        MainWindowUI::PanelRow row;
        // 行首那个头像**由调用方给**（`faces`，按 uuid 认人）：这里是纯函数，不能自己去
        // 读缓存、更不该去联网（见 MainWindow::accountFacePixmap() 那段 —— 它只碰本地
        // 已经躺着的图，一张都不下）。认人用 uuid 而不是名字：名字是可以重复的。
        // 取不到就是一张空图，那一行的行首**什么都不画**；这一档不该出现（最差也有一个
        // 首字母方块），只是不留"这里必须有一张图"的隐含前提。
        row.icon = faces.value(account.uuid);
        row.title = account.name;
        row.subtitle = account.offline ? MainWindowUI::bottomBarPanelOfflineAccountText()
                                       : MainWindowUI::bottomBarPanelOnlineAccountText();
        row.key = account.uuid;
        // 整行就是那颗按钮（账号弹层）：当前账号打勾，点别人 = 切过去（动作 0，见 onBottomBarPanelAction()）。
        row.checked = account.isCurrent;
        rows.append(row);
    }
    return rows;
}

}  // namespace MainWindowViewData
