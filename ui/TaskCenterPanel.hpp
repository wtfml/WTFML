/**
 * @file TaskCenterPanel.hpp
 * @brief 任务中心：底栏中间那格向上弹出来的那块面板。
 *
 * 【为什么另起一个类，不接着用 `BottomBarPanel`】
 *
 * `BottomBarPanel` 那一行的形状是"图标 + 两行字 + 行尾几颗按钮"，是给
 * "一行一个实例/账号"用的，不适合任务行。任务那一行要装的东西不一样：标题 + 一行小字 + 一根进度条 +
 * 一串步骤圆点，还要分成「当前工作 / 已完成」两栏。硬塞进短行那个壳里，行里那些
 * `bottomBarPanelRow*` 的名字也会跟着变成谎话（验证工程按名字找控件）。
 *
 * 复用的是**弹出来的那套机制**：`Qt::Popup`（点外面自动收、Esc 关、不抢主窗口焦点）、
 * 贴着某颗按钮、`Anim::slideUp()` 从下面滑上来、以及"点另一颗按钮要能收旧开新"那两问
 * （`OutsidePressHandler`，连同 `WA_NoMouseReplay` 那道闸 —— 那几条 NOTE 在
 * `MainWindowUI.cpp` 的 `BottomBarPanel` 里，这里是对着那份抄的）。
 *
 * 【界面这一头不认识任务账本】
 *
 * `ui/` **不 include `src/`**。所以这个文件里放的是**给界面看的**一行长什么样
 * （`TaskCenter::Item`），`MainWindow` 那边把 `Tasks::TaskSnapshot` 翻成它。这一层里
 * 出现的每个字都是界面自己的事（"正在下载"这种词不该写在账本里）。
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

class QScrollArea;
class QPushButton;
class QLabel;
class QComboBox;
class QLineEdit;
class QResizeEvent;

namespace TaskVisuals { class ActionOverflow; }

namespace TaskCenter {

/// 一步走到哪儿了。**和 `src/tasks/TaskSnapshot.hpp` 的 `TaskStepState` 一一对应**，
/// 但两边各有一份（`ui/` 不许 include `src/`）—— 翻这一下的地方是
/// `MainWindow::taskCenterItems()`，那里同时是"这两个枚举必须同构"的唯一见证。
enum class StepState { Pending, Running, Done, Failed };

/// 一项任务当前所处的状态。和 `Tasks::TaskState` 一一对应；UI 保留完整状态，避免把
/// Cancelled / Interrupted 这种终态压成“不是失败，所以成功”。
enum class State { Queued, Running, WaitingUser, Paused, Succeeded, Failed, Cancelled, Interrupted };

/// 一步：名字 + 到哪儿了。`note` 是那一步的补充说明（见 `stepForTask()`）。
struct Step {
    QString name;
    StepState state = StepState::Pending;
    QString note;
};

/// 这一项**当下允许哪些操作**。和 `Tasks::TaskCapabilities` 一一对应（唯一翻译点 =
/// `MainWindow::taskCenterItems()`）。
///
/// 它**不是**"这个状态本来该能干什么"，而是**账本那一刻记下来的那一位** —— 面板和详情页
/// 只照着它摆按钮，不再自己按状态推一遍。两处各推一遍的结局是"界面上有一颗点了没反应的
/// 按钮"（推的那一边不知道下载批次已经不在了）。
struct Capabilities {
    bool cancel = false;
    bool pause = false;
    bool retry = false;
    bool openTarget = false;
    bool openLog = false;
    /// 「导出错误报告」/「打开崩溃报告目录」：只有游戏异常退出那一项为真。
    bool exportReport = false;
    bool openCrashDir = false;
    bool repairImport = false;
    /// 「继续未完成操作」：这一项留下了一份可续的文件操作报告（见 `OperationReport`）。
    bool recover = false;
};

/// 面板里的一行（活动的那几行 + 已完成的那几行，形状一样、画法不同）。
struct Item {
    QString queueGroup; // 非空才有真实执行队列；运行/历史行不显示拖动手柄。
    int queueOrder = 2147483647;
    /// 认人用。`MainWindow` 那边是 `TaskId` 的十进制字符串（面板拿它当**钥匙**，
    /// 不当行号 —— 行号会随着"清掉一条已完成"整体挪位）。
    QString key;
    QString title;
    /// 标题下面那行小字（"已下载 12 / 40  文件名"）。空 = 不画那一行。
    QString detail;
    /// -1 = **不可测**（没有 Content-Length 那一类）：画不确定态，绝不编一个百分比出来
    /// （「无 Content-Length 的 Fabric 下载显示'已下载多少'，
    /// 不显示假百分比」——"已下载多少"那句走 `detail`）。
    int percent = -1;
    /// 字节级进度那一句（"已下载 12.3 MB / 45.6 MB"）。空 = 这一项不按字节记账。
    /// **只有详情页用它**：面板上那一行装不下（那一行小字是 `detail`）。
    QString bytesText;
    State state = State::Queued;
    /// 用户**还没看过**的一次失败：行尾挂一个红点（看见之后由 MainWindow 撤掉）。
    bool unreadFailure = false;
    /// 失败/中断已经在详情中查看；不等同于已完成等待用户选择。
    bool attentionAcknowledged = false;
    /// 起于什么时候。**只有详情页用**（面板上那几行只画收尾时间，见 `finishedAt`）。
    QDateTime startedAt;
    /// 什么时候收的尾。已完成那一行右边那个"19:42"由**界面自己**从它算出来
    /// （见 `TaskCenter::timeText()`）—— 时间怎么写成字是界面的事，不该由账本那边拼好了送进来。
    QDateTime finishedAt;
    /// 失败/中断的简短原因（账本里的 `errorSummary`）。空 = 没出过错。
    /// 「任务失败能看到失败阶段、简短原因和'查看日志/重试'」——这是"简短原因"那一半。
    QString errorSummary;
    /// 「这一项为什么不能取消」。空 = 不解释；非空且还在跑 = 详情页摆一行小字。
    /// 有这一格的用处：没有它，用户只能看见"取消按钮不见了"，猜不出是"做完了"还是
    /// "这一段停不得"。
    QString cancelBlockedReason;
    QList<Step> steps;
    Capabilities capabilities;
};

/// 面板打开/刷新时现问一遍要画什么。**一次调用 = 一次快照**（跟 `PanelRowsProvider`
/// 一个路数：不缓存，"上一次那几行"留着迟早会在界面上说假话）。
using ItemProvider = std::function<QList<Item>()>;

/// 行尾那颗小按钮上的动作（「行内快捷动作：取消 / 查看详情」那一半）。
///
/// 「查看详情」不在这一枚举里 —— 它是**点那一行本身**（整行的行为，不是行尾一颗按钮）。
/// 两样分开是因为它们的位置不一样：详情页是"我想看看这是怎么回事"，取消是"别下了"，
/// 后者要是也藏在详情页里，用户得点两下才够得着。
enum class RowAction { Cancel, Retry, OpenTarget, OpenLog, ExportReport, OpenCrashDir, Seen, Viewed, MoveUp, MoveDown, RepairImport, Recover, Pause };

/// 行尾那颗小按钮被点了。参数是那一行的 `key` 和点的是哪一颗。
using RowActionHandler = std::function<void(const QString &key, RowAction action)>;

/// 「清除已完成」被点了。**清完由调用方让面板刷新一次**（refresh()），面板不知道账本在哪。
using ClearFinishedHandler = std::function<void()>;

/// 一步那一个小记号。**唯一出处**：面板里画它、验证工程比对它，都读这一个函数。
QString stepMark(StepState state);

/// 一步在**某一项任务**里该画成什么（名字、记号、备注都可能是改写过的）。
///
/// NOTE: 任务收尾时，正在跑的那一步会原样留在 `Running`（`TaskManager::finish()` 只把
/// 失败那一档的 Running 改成 Failed —— 取消/中断是"人叫停的"，改写成一堆 ✗ 是
/// 在冤枉那些步骤）。于是**画**的那一侧要补这一眼：任务已经有终态记号了，步骤再
/// 画一个 ● 就是"这一项已经结束、里面还有东西在跑"的自相矛盾。
/// 面板行、详情页、复制成文本三处**都走这一个函数**。
Step stepForTask(State taskState, const Step &step);

/// 任务还在走（排队 / 运行 / 等用户 / 暂停）。收尾过的一律 false。
bool stateIsActive(State state);

/// 已结束任务左边的状态记号。活动状态返回空串。
QString terminalMark(State state);

/// @p when 该写成"几点几分"（已完成那一行右边那个）。**时间格式的唯一出处**放在这里，
/// 而不是让 `MainWindow` 拼好一个字符串送进来：那一层里有权的是"这件事什么时候完的"，
/// 不是"这件事该写成年月日还是时分"。
QString timeText(const QDateTime &when);

}  // namespace TaskCenter

/**
 * @brief 一块贴在某颗按钮正上方、可以随时重建内容的 `Qt::Popup` 面板。
 *
 * NOTE: 和 `BottomBarPanel` 一样，它是个**窗口**（`Qt::Popup` 一定是顶层窗口），但 parent
 * 照旧挂着 —— 这样 `findChild` 找得到它（验证工程靠这个量几何），生命周期也不用
 * 单独管。
 */
class TaskCenterPanel : public QWidget {
public:
    /// 「点在面板外面」那一下要问外面两件事（两个都按**全局坐标**说话）。两问的顺序不能换：
    /// · `claimed` —— 收回去**之前**问：这一下落在底栏那三颗按钮上吗？（是 ⇒ 钉
    /// `WA_NoMouseReplay`，把 Qt 那记重放压掉）
    /// · `dismissed` —— 收回去**之后**叫：去开该开的那一块（点的要是同一颗就什么都不开）。
    /// 理由全文见 `MainWindowUI.cpp` 里 `BottomBarPanel::handleOutsidePress()` 那两段 NOTE。
    struct OutsidePressHandler {
        std::function<bool(const QPoint &globalPos)> claimed;
        std::function<void(const QPoint &globalPos)> dismissed;
    };

    /// @param anchor 贴着哪颗按钮弹（也决定"至少和按钮一样宽"）。
    /// @param provider 现问一遍要画什么（可以给空的 = 面板里只有一句兜底的话）。
    /// @param onClear 「清除已完成」被点了该告诉谁（可以给空的 = 那就没有那颗按钮）。
    TaskCenterPanel(QWidget *anchor, const TaskCenter::ItemProvider &provider,
                    const TaskCenter::ClearFinishedHandler &onClear,
                    const OutsidePressHandler &onOutsidePress, QWidget *parent,
                    bool embedded = false);
    ~TaskCenterPanel() override;

    /// 贴的是哪颗按钮（"再点一次收回"要按它认人：点的是同一颗才算收回）。
    QWidget *anchor() const { return m_anchor; }

    /// 第一次打开：摆到 anchor 正上方、**带滑入动效**、显示出来。
    void popUp();

    /// 作为覆盖层里的完整工作区使用。此时不做屏幕定位，也没有 Popup 的点击外部关闭语义。
    void showEmbedded();
    QWidget *headerActions() const { return m_toolbar; }

    /// 内容变了重画一遍（面板开着的时候 MainWindow 会喊）。**不重播滑入动效** ——
    /// 它这会儿已经在屏幕上，再滑一次看着就是"整块面板跳了一下"。
    void refresh();
    /// 合并当前事件轮的状态通知，避免在行内动作回调中删除发送者。
    void requestRefresh();

    /// 面板里的一行被点了要告诉谁（参数是那一行的 `key`）。
    ///
    /// **可以给空的** = 面板里那几行点了没反应（验证工程搭裸面板时就是这样）。
    /// 挂上了才会给那几行手型光标 —— "看着能点、点了没反应"比不给更糟。
    void setRowActivatedHandler(const std::function<void(const QString &key)> &handler) {
        m_onRowActivated = handler;
        ++m_rowHandlerRevision;
    }

    /// 面板里某一行**行尾那颗小按钮**被点了要告诉谁（见 `RowActionHandler`）。
    ///
    /// **可以给空的** = 行尾什么都不长（那几行就只有"点开详情页"这一条路；详情页里照样有
    /// 那一排按钮，所以取消这件事不会变得够不着）。
    void setRowActionHandler(const TaskCenter::RowActionHandler &handler) {
        m_onRowAction = handler;
        ++m_rowHandlerRevision;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    quint64 m_rowHandlerRevision = 0;
    bool m_dragRefreshPending = false;
    /// 把内容整块重建（两栏 + 那个「清除已完成」）。调用方负责随后摆位置。
    void rebuild();
    void rebuildDetail(const QList<TaskCenter::Item> &items);
    void selectTask(const QString &key);
    void syncSelection();
    /// 把**真在视口里露了脸**的失败行标成"看过了"（见 `rebuild()` 里那段 ★ ）。
    void acknowledgeVisibleFailures();
    /// 鼠标键还按着的时候不重建，排到松手之后（见 `refresh()` 里那段 ★ ）。
    void deferRefreshUntilRelease();
    /// 宽屏三列 / 窄屏“列表或详情占满整页”的唯一布局切换点。
    void applyWorkspaceMode();
    /// 右栏的宽度跟着工作区一起伸缩（唯一出处；`applyWorkspaceMode` / `place` / `resizeEvent` 都问它）。
    void updateDetailPaneWidth();
    /// 给某一行造一个"点了怎么办"（没挂处理器时回一个空的 —— 那一行就不可点）。
    std::function<void()> rowActivate(const QString &key);
    /// 摆到 anchor 正上方。@p animate true = 从下面滑上来（第一次），false = 直接挪过去。
    void place(bool animate);
    /// 点在面板外面那一下。返回 true = 这一下已经办完了。
    bool handleOutsidePress(QMouseEvent *event);

    QWidget *m_anchor = nullptr;
    TaskCenter::ItemProvider m_provider;
    TaskCenter::ClearFinishedHandler m_onClear;
    /// 面板里某一行被点了（参数是那一行的 `key`）。空 = 那几行不可点。
    std::function<void(const QString &key)> m_onRowActivated;
    /// 行尾那颗小按钮被点了（见 `RowActionHandler`）。空 = 行尾什么都不长。
    TaskCenter::RowActionHandler m_onRowAction;
    OutsidePressHandler m_onOutsidePress;
    /// 内容都装在这里面（面板本身只有一个滚动区 + 它的边距）。
    QWidget *m_content = nullptr;
    QWidget *m_toolbar = nullptr;
    QComboBox *m_filter = nullptr;
    QLineEdit *m_search = nullptr;
    QPushButton *m_clearFinished = nullptr;
    QWidget *m_detailContent = nullptr;
    /// 详情底下那排操作钮：钉在滚动区**外面**，窗口矮时也一直看得见。
    QWidget *m_detailFooter = nullptr;
    /// 详情底部那排「主钮 + 两颗扁平钮 + 更多…」的记账（每次重建详情换新）。
    std::unique_ptr<TaskVisuals::ActionOverflow> m_detailActions;
    QWidget *m_detailPane = nullptr;
    QScrollArea *m_scroll = nullptr;
    QScrollArea *m_detailScroll = nullptr;
    QString m_selectedKey;
    bool m_compact = false;
    bool m_compactDetailOpen = false;
    bool m_opened = false;
    bool m_refreshPending = false;
    /// 本次重建里**还没看过**的失败行钥匙（`rebuild()` 收集，`acknowledgeVisibleFailures()` 消费）。
    QSet<QString> m_unreadFailureKeys;
    /// 重建已经排到"鼠标松开之后"了（见 `refresh()` 里那段 ★ ）。
    bool m_refreshDeferred = false;
    /// 为了"松手之后再重建"已经等了几个 40 ms 拍（到 50 拍就放弃等待、照常重建）。
    int m_deferTicks = 0;
    /// 正在 `acknowledgeVisibleFailures()` 里（那一趟会 `onRowAction` → 重建，别被滚动条回调插进来）。
    bool m_ackInProgress = false;
    bool m_embedded = false;
};
