/**
 * @file TaskDetailController.hpp
 * @brief 任务详情页那一族：开页、页上那六颗按钮各自去哪儿、页怎么重摆。
 *
 * 从 MainWindow 搬出来的（「接着拆，拆点小的功能」）。搬走的是**功能**：这一页的
 * 状态（页对象，**建一次留着**）和这一页上大部分决定都在这一层了 —— 包括"「打开日志」该开
 * 哪一份日志"（那场游戏的日志窗 / 启动器自己那份 / 那扇窗被关掉了要说一句）、"「打开目标
 * 位置」找不到实例目录时要不要去看它是不是一个服务端"、"复制出来的摘要长什么样"。
 *
 * 留在主窗口那一头的只有两条：**取消 / 重试**。它们跨着两个执行者（服务端安装自己跑
 * `QProcess`，其余走 `VersionManager` 那一批），判据本体是 `MainWindow::cancelWouldWork()` /
 * `retryWouldWork()` —— 那两处必须逐字对齐，所以它们留在同一个地方，这一层只按名字转手。
 *
 * NOTE: 页对象是**建一次留着**的（用户按过「清除已完成」之后，页上那份数据就作废了）——
 * 所以每一发动作之后都要按同一把钥匙重摆一遍（见 `action()` 尾巴那一段）。
 */
#pragma once

#include "TaskDetailPage.hpp"
#include "tasks/TaskManager.hpp"

#include <QObject>
#include <QString>

#include <functional>

class ActivityCenterController;
class CrashReportController;
class GameSessionController;
class OverlayHost;
class VersionManager;
class QWidget;

class TaskDetailController final : public QObject {
    Q_OBJECT

public:
    /// 这一层做不了、得回头问主窗口的四件事。
    struct Host {
        /// 往启动器日志里写一句（主窗口那句 `appendLog`）。
        std::function<void(const QString &)> log;
        /// 「这一项**此刻**按取消真的会做点什么吗」—— `MainWindow::cancelWouldWork()`。
        /// 页上那颗「取消」露不露、露出来点不点得动，都由它说了算。
        std::function<bool(const Tasks::TaskSnapshot &)> cancelWouldWork;
        /// 页上那颗「取消」：**跨两个执行者**，怎么分派由主窗口那一段说了算（见文件头）。
        std::function<void(const QString &)> cancelTask;
        /// 页上那颗「重试」：同上。
        std::function<void(const QString &)> retryTask;
        std::function<void(const QString &)> repairImport;
        /// 「继续未完成操作」：主窗口按报告钥匙把那趟文件操作接着做完。
        std::function<void(const QString &)> recoverOperation;
        std::function<void(const QString &)> pauseTask;
        /// 「询问智能体」：把这次崩溃的摘要与实例交给智能体页（主窗口那条
        /// `askAgentAboutCrash()`）。页那一层在调它之前会先把这一页收起来 —— 那一路要
        /// 在智能体页上弹 Auto-safe 问话，覆盖层得空出来。
        std::function<void(const QString &context, const QString &instanceId)> askAgent;
    };

    TaskDetailController(ActivityCenterController *activity, CrashReportController *crash,
                         VersionManager *versions, GameSessionController *sessions,
                         OverlayHost *overlay, QWidget *pageParent, Host host,
                         QObject *parent = nullptr);

    /// 开这一页（覆盖层整页）。**不排下一轮** —— 点行的那一下人是看着面板点的，
    /// 晚一轮再开等于"面板收起来了，页没开"。
    void open(const QString &key);
    /// 页上（或者任务中心那一行行尾）按了某一颗。
    void action(const QString &key, TaskDetailPage::Action action);
    /// 「打开日志」：这一项自己的那一场 / 启动器自己那份，见 .cpp 里那段。
    void openLog(const QString &key);
    /// 「打开目标位置」：实例目录，认不出来再看它是不是一个服务端。
    void openTarget(const QString &key);
    /// 「复制摘要」：把这一页上那几行字拼成纯文本进剪贴板。
    void copySummary(const QString &key);

private:
    /// 一把钥匙 ⇒ 这一页要画的那一份。**和任务中心面板共用的唯一翻译点**（那个是
    /// `ActivityCenterController::item()`，这一条只是把"能不能取消/重试"喂给它）。
    /// **没有副作用**（面板那个 `taskCenterItems()` 有"看一眼 = 看过了"那一笔）。
    bool fillItem(const QString &key, TaskCenter::Item *out) const;
    void log(const QString &message) const;
    /// 页开着的时候账本动了：按这一项的号重摆一遍。
    void refreshOpenPage();
    /// 上面那条的**合并版**：同一轮事件里账本动了几十下（下载进度是逐块报的）也只重摆一次。
    void scheduleRefreshOpenPage();

    ActivityCenterController *m_activity = nullptr;
    CrashReportController *m_crash = nullptr;
    VersionManager *m_versions = nullptr;
    GameSessionController *m_sessions = nullptr;
    OverlayHost *m_overlay = nullptr;
    QWidget *m_pageParent = nullptr;
    Host m_host;
    /// 这一页（建一次留着、之后复用）。
    TaskDetailPage *m_page = nullptr;
    /// `scheduleRefreshOpenPage()` 已经排了一发、还没跑。
    bool m_refreshPending = false;
};
