/**
 * @file TaskDetailPage.hpp
 * @brief 任务详情页：点任务中心里的一行，在内容区整块开出来的那一页。
 *
 * 【它是"页"，不是窗口】
 *
 * 开出来的方式：`OverlayHost::showPage()` 把这一页整块盖在内容区上（**顶上四颗导航和底栏
 * 不盖**），顶上那条 `← 返回` 就是这一页的出路。它是**非阻塞**的 —— 这一页不收任何结果，
 * 摆上去调用方就回来了（`execPage()` 那种"卡住调用方等用户点返回"在这里没有用武之地）。
 * `QDialog` 只是个壳 —— 它是全仓所有"内嵌页"的共同基类形状（`OverlayHost` 认 `QDialog`），
 * 不是"要另开一个窗口"。
 *
 * 【它一个字节的业务都不碰】
 *
 * `ui/` 不 include `src/`。所以这一页拿到的是 `TaskCenter::Item`（面板那一行的同一份形状），
 * 用户点的那几颗按钮只发一个 `actionRequested(key, action)` 出去 —— 取消谁、重试什么、日志在哪、
 * 摘要是哪几句，全由 `MainWindow` 说了算（它手里才有任务账本、下载器、日志窗）。
 *
 * 【按钮为什么按"能力位"摆，而不按状态推】
 *
 * 上面那一排的露与不露**只读 `Item::capabilities`**（那是账本在收尾那一刻记下来的"当时允许
 * 什么"）。这一页要是自己按状态再推一遍，推出来的和账本记的迟早会漂开，而漂开的表现是
 * "界面上有一颗点了没反应的按钮" —— 正是这一页要避免的那种。
 *
 * 【为什么"失败"要专门留一块地方写原因】
 *
 * 「任务失败能看到失败阶段、简短原因和'查看日志/重试'」。失败阶段 = 步骤那一栏里那个
 * `✗`；简短原因 = `Item::errorSummary` 那一块。两样都要**在页上一眼看得见**，不能只留在日志里
 * —— 日志是给"还想往下查"的人看的。
 */

#pragma once

#include <QDialog>
#include <QString>

#include <memory>

#include "TaskCenterPanel.hpp"

class QLabel;
class QPushButton;
class QVBoxLayout;
/// 自绘的进度条（`ui/TaskProgressBar`）。这一页那一根和底栏、面板是同一个。
class TaskProgressBar;
namespace TaskVisuals { class ActionOverflow; }

class TaskDetailPage : public QDialog {
    Q_OBJECT

public:
    /// 用户在这一页上点了什么。**动作的语义全在 MainWindow 那边**，这里只报"点了哪一个"。
    enum class Action {
        Cancel,       ///< 「取消」：掐掉当前这一批下载。
        Retry,        ///< 「重试」：只重做上一次失败的那几个文件。
        OpenLog,      ///< 「打开日志」：开日志窗（那一个是全仓唯一允许另开窗口的东西）。
        OpenTarget,   ///< 「打开目标位置」：在文件管理器里打开这一项的目标（实例目录 / 落点目录）。
        CopySummary,  ///< 「复制摘要」：把这一项的摘要（含错因）放进剪贴板。
        ExportReport, ///< 「导出错误报告」：把这一趟的事实 + 日志写成一份能发出去的文本。
        OpenCrashDir, ///< 「打开崩溃报告目录」：在文件管理器里打开 `crash-reports/`。
        RepairImport, ///< 打开未完成整合包的本地补齐报告。
        Recover,      ///< 「继续未完成操作」：接着做完那份报告里还没做完的文件操作。
        Pause,
        OpenAgentSession, ///< 「查看会话」：切到智能体页，打开发起这一项的会话。
    };

    explicit TaskDetailPage(QWidget *parent = nullptr);
    ~TaskDetailPage() override;

    /// 这一项眼下提供不提供 @p button 那个操作（露在那一排，或收在「更多…」菜单里）。
    bool offers(const QPushButton *button) const;

    /// 这一页的标题（`OverlayHost::showPage` 顶上那一条写的字）。
    static QString windowTitle() { return tr("任务详情"); }

    /// 摆一项任务。**每次开页都重新喂一遍**（同一页对象会被复用，见 MainWindow 那边）。
    ///
    /// @param item 面板那一行的同一份形状（`MainWindow::taskCenterItems()` 是唯一翻译点）。
    void setTask(const TaskCenter::Item &item);

    /// 现在摆的是哪一项（上面那几个槽回话时要带上它 —— 页开着的时候账本可能已经又动了，
    /// 这一页摆的是**点开那一刻**的那一份）。
    QString taskKey() const { return m_key; }

    // ---------- 供验证工程与 MainWindow 调的那几个槽 ----------
    // NOTE: 按钮那条路是"点一下 → 发一个信号"，真正的干活在 MainWindow。这几个槽是**另一条
    // 路**（MainWindow 自己也可能从别处触发同一个动作），所以按钮的信号和这些槽
    // 故意不是同一个东西。

    /// 用户点了 @p action（按钮的信号槽直接转发到它）。`MainWindow` 接 `actionRequested`。
    void requestAction(Action action) { emit actionRequested(m_key, action); }

signals:
    /// 用户点了某颗按钮（`key` 是 `setTask()` 那一刻那一项的钥匙）。
    void actionRequested(const QString &key, TaskDetailPage::Action action);

private:
    /// 按 `m_item` 把整页重画一遍。
    void rebuild();
    /// 造一颗动作按钮（挂到自己身上，点了就 `requestAction()`）。
    QPushButton *makeActionButton(const QString &text, Action action,
                                  const QString &objectName);

    TaskCenter::Item m_item;
    QString m_key;
    QVBoxLayout *m_box = nullptr;
    /// 每次 `setTask()` 都会整块重建这一块内容（标题下面那几行）。
    QWidget *m_body = nullptr;

    QLabel *m_stateLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_detailLabel = nullptr;
    QLabel *m_bytesLabel = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_errorTitle = nullptr;  ///< 「出了什么问题」小标题（和下面那块一起露）
    TaskProgressBar *m_bar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_logButton = nullptr;
    QPushButton *m_targetButton = nullptr;
    QPushButton *m_reportButton = nullptr;
    QPushButton *m_crashDirButton = nullptr;
    QPushButton *m_sessionButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_repairButton = nullptr;
    QPushButton *m_recoverButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    std::unique_ptr<TaskVisuals::ActionOverflow> m_overflow;
};
