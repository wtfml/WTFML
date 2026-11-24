/**
 * @file ServerRestart.hpp
 * @brief 「服务端异常退出之后要不要自动拉起」那一层判据（「重启策略 + 上限」）。
 *
 * 【为什么要单独一层】
 *
 * 「自动重启」听上去只有一句 `if (崩了) 再起一次`，但真正会出事的是**边界**：
 *
 * · **用户自己停的**不许再拉起来 —— 点了「停止」之后它过五秒又回来了，那是最吓人的一种；
 * · **正常退出**（退出码 0，控制台里敲了 `stop`）也不许拉；
 * · **崩了**才拉，而且**连着崩几次就收手**：一个 1.20 服务端半秒崩一次，没有上限的话
 * 一晚上就是几万条日志、几万次磁盘写，用户第二天看到的是一台卡死的机器；
 * · 两次之间要**退避**：崩得越快、等得越久，给用户留出看控制台的时间；
 * · 它跑够久（超过 `resetAfterSeconds()`）之后再崩，算**新的一轮**，计数从头来 ——
 * 不然一台"每周崩一次"的服务端会在第三次之后永远不再自动起来。
 *
 * 这一层全是纯函数：喂「为什么退的」「已经连着重启过几次」「策略」进来，回一个决定。
 * 不读盘、不看时钟、不起进程 —— 所以 `verify` 能直接钉（ZAF 一节），真接线在 `MainWindow`
 * （`ServerRunController::onExited()` 那一段），策略存在 `ServerModel::Info`（`.wtfml-server` 里那两格）。
 */

#pragma once

#include <QString>

namespace ServerRestart {

/// 这一次是**怎么退的**。三档就是界面上/日志里要分开说的三种。
enum class Why {
    UserStop,   ///< 用户自己点的「停止」/「强制结束」（或者我们替用户停的）
    CleanExit,  ///< 它自己退的，退出码 0（控制台里敲了 `stop`、或者它认为该结束了）
    Crash,      ///< 非 0 退出（eula 没同意 / 端口被占 / 模组崩了 ……）
};

/**
 * @brief 按「进程报的」那两样事实判这一次是哪一档。
 * @param userStopped 这一次退出之前我们发过停止 / 强杀（`MainWindow` 记着）
 * @param forceKilled `ServerProcess::wasForceKilled()`
 * @param exitCode 退出码（`FailedToStart` 那种是 −1）
 *
 * 用户停的优先：哪怕它这一趟退成了非 0（被强杀就是这样），也**不是**崩溃。
 */
Why whyFor(bool userStopped, bool forceKilled, int exitCode);

/// 这一档会不会触发自动重启的考虑（只有 `Crash` 会）。
bool isAbnormal(Why why);

/// 界面上 / 日志里那一档的说法（「你自己停的」「正常退出」「异常退出」）。
QString whyName(Why why);

// ---------------------------------------------------------------------------
// 策略
// ---------------------------------------------------------------------------

/// 出厂那一档最多连着重启几次。
inline constexpr int kDefaultMaxAttempts = 3;
/// 能配的上限 / 下限（用户手改 ini / meta 的时候兜住）。
inline constexpr int kMinMaxAttempts = 1;
inline constexpr int kMaxMaxAttempts = 10;
/// 第一次重启之前等几秒；之后每多一次翻一倍，封顶 `kMaxDelaySeconds()`。
inline constexpr int kBaseDelaySeconds = 5;
inline constexpr int kMaxDelaySeconds = 60;
/// 它跑够这么久再崩，算新的一轮（计数从头来）。五分钟。
inline constexpr int kResetAfterSeconds = 300;

/// 一项策略。`maxAttempts` 是「**连着**自动重启几次之后收手」。
struct Policy {
    bool enabled = false;
    int maxAttempts = kDefaultMaxAttempts;
};

/// 越界的次数回**出厂那一档**（不夹到边上：夹到边上会让"被限流"看起来像用户自己选的，
/// 同 `DownloadConcurrency::normalize()` 那条规矩）。
int normalizeMaxAttempts(int value);
Policy normalize(const Policy &policy);

/// 第 @p attempt 次重启（1 起）之前等几秒：5、10、20、40、60、60…
int delaySeconds(int attempt);

// ---------------------------------------------------------------------------
// 决定
// ---------------------------------------------------------------------------

/// `decide()` 的结论。
struct Decision {
    /// 这一趟要不要自动重启。
    bool restart = false;
    /// 重启之前等几秒（`restart = false` 时没有意义，回 0）。
    int delaySeconds = 0;
    /// 要写进日志 / 说出来的一句话。`restart = false` 时：为什么不再拉（或者空）。
    QString note;
};

/**
 * @brief 这一次退出之后怎么办。
 * @param policy 用户的策略。
 * @param why 这一次是怎么退的。
 * @param attemptsUsed 这一轮里**已经**自动重启过几次（0 = 还没重启过）。
 *
 * 规矩：
 * · 不是 `Crash` ⇒ 不重启（`note` 空 —— 正常退出不用多说什么）；
 * · 策略没开 ⇒ 不重启（`note` 空）；
 * · 已经重启过 `maxAttempts` 次 ⇒ 不重启，`note` 是「连着 N 次都失败了，不再自动重启」；
 * · 否则重启，等 `delaySeconds(attemptsUsed + 1)` 秒，`note` 说清楚第几次 / 共几次。
 */
Decision decide(const Policy &policy, Why why, int attemptsUsed);

}  // namespace ServerRestart
