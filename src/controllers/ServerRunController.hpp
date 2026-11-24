/**
 * @file ServerRunController.hpp
 * @brief 服务端**跑起来 / 停下来 / 退出之后怎么办**那一套状态与接线（原来摊在
 *        `MainWindow` 里）。
 *
 * 【为什么单独一层】
 *
 * 这里管理谁的世界正在备份 / 还原（`m_fileBusy`）、这一轮连着崩了
 * 几次（`m_restartAttempts`）、这一次退出是不是我们让它停的（`m_stopRequested`）、
 * 以及它是几点起来的（`m_startedAt`）。这些状态决定两件看得见的事 —— 那一行现在写着什么
 * （跑着 / 崩了 / 停了），以及"崩了之后要不要自己再拉起来"。它们和 `MainWindow` 上其余
 * 几百个成员摆在一起时，漏一格的症状很难认：最典型的是**自动重启次数不清零**，浏览器
 * 里那台服务器崩第三次之后就再也不自己起来了，而日志只说"次数用完了"。
 *
 * 【这一层一条判据都不自己写】
 *
 * * 那条命令怎么拼、宽限多久才允许强杀：`ServerRun.hpp`（纯头，verify 的 ZZ 一节钉死）；
 * * 崩了之后该不该重启、歇几秒：`ServerRestart::decide()`（verify 的 ZAF 一节）；
 * * 进程本身在 `ServerWorkspaceController` 的 `ServerProcess` 里（mw_check 的 T41 拿
 *   本工程自己的 exe 当"服务端"跑整趟）。
 *
 * 这一层只做三件事：把按钮那一下翻成一个动作、把退出回调翻成"要不要重启"、以及在那
 * 两种时候把界面那几行刷一遍。
 *
 * NOTE: 它**不拥有**进程：进程的账在 `ServerWorkspaceController`（按服务端 id 一格），
 * 这一层只按 id 去问它。所以"谁在跑"这件事仍然只有一处。
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "tasks/TaskId.hpp"

class MainWindow;

class ServerRunController final : public QObject {
public:
    explicit ServerRunController(MainWindow *owner);

    /// 起一个服务端。**四道闸**（装好了没有 / 已经在跑 / EULA / 命令拼不拼得出来）和
    /// Java、内存、宽限那两个入参全在这里收口（它们原来在 `MainWindow::startServer()`）。
    /// @param automatic 自动重启那一路：不弹窗（用户多半不在跟前），也不清崩溃计数。
    void start(const QString &serverId, bool automatic = false);
    /// 让它停下来（写 `stop`）：先请求优雅退出，写不进去就只剩强杀（见 .cpp 里那段）。
    void stop(const QString &serverId);
    /// 关窗选择停服时，记录主动停止并等进程退出。
    bool stopAndWait(const QString &serverId);
    /// 强制结束。**也是"这是我们让它死的"** —— 退出码再难看也不按崩溃算。
    void forceKill(const QString &serverId);

    /// 收到该服务端的就绪信号后完成启动任务。
    void onReady(const QString &serverId);

    /// 进程退出回调（接 `ServerWorkspaceController::ServerProcess::exited`）：
    /// 判 `ServerRestart::Why`、清/加崩溃计数、必要时排那一发自动重启。
    void onExited(const QString &serverId, int exitCode);

    /// 这一份的世界正在备份 / 还原（起服和"删服务端"两道闸读的就是它）。
    void setFileBusy(const QString &serverId, bool busy);
    bool fileBusy(const QString &serverId) const { return m_fileBusy.contains(serverId); }
    bool isStarting(const QString &serverId) const { return m_startupTasks.contains(serverId); }

    /// 用户把「异常退出后自动重启」关掉了：这一轮的计数清零（再打开时从第 1 次算起）。
    void forgetRestartAttempts(const QString &serverId) { m_restartAttempts.remove(serverId); }

    /// 进程本次启动的时间戳（毫秒；若未登记则返回 0）。
    qint64 startedAtMs(const QString &serverId) const {
        const QDateTime dt = m_startedAt.value(serverId);
        return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
    }

private:
    MainWindow &m_owner;
    /// 这一轮连着崩了几次（下标 = 服务端 id）。跑够久再崩算新的一轮，见 `onExited()`。
    QHash<QString, int> m_restartAttempts;
    /// 每台是几点起来的（判"跑够久了没有"用）。退出时取走。
    QHash<QString, QDateTime> m_startedAt;
    /// 服务端报告就绪或提前退出时，结束对应的启动任务。
    QHash<QString, Tasks::TaskId> m_startupTasks;
    /// 这一次退出是**我们**让它停的（用户按了停止 / 强杀 / 关自动重启）。
    QSet<QString> m_stopRequested;
    /// 世界正在备份 / 还原的那几台。
    QSet<QString> m_fileBusy;
};
