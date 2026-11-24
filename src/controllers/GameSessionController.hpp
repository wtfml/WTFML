#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

class QTimer;

#include "tasks/TaskManager.hpp"

class GameLogWindow;
class LauncherManager;
class LauncherSettings;
class LaunchHistoryController;
class MainWindowUI;
class VersionManager;

/// **一次游戏会话从"进程起来"到"结束"**那一段的全部（ 收进来）。
///
/// 这一层管的是四件事，它们从头到尾说的都是同一件事（"这一份会话现在怎么样"）：
/// ① **账本**：哪几份会话的启动进度还在走、各自对上哪件任务、这一份当初是谁（实例名 /
/// 身份 / Java / 内存 / 加载器），以及用户主动停过哪几份；
/// ② **日志窗**：哪几份开着窗、哪几份用户手动关过（关过的不再自动弹）、以及"导出"要用的
/// 那一份事实快照；
/// ③ **进程信号进来之后干什么**：出话 → 启动那件任务收尾、底栏让位；退出 → 记「上一次
/// 启动」的结局、异常退出补一项、收掉启动进度、把窗口叫回来；
/// ④ **停止**：问一句（强杀不存档）、记"用户停的"、kill、收尾。
///
/// 这些状态**全在这一层**（不再散在 `MainWindow` 的九个私有成员 + 一句 `friend class`
/// 里）：主窗口只递几样"只有它说得出来"的东西（见 Host），**判断和记账全在这一层**。
///
/// 唯一一个仍然写这一份账本的外人是 `LaunchController` —— 它在"进程真的起来了"那一刻
/// 调 `beginSession()`（那是这一层唯一说得出口的开端）。除此之外谁也不许碰这九个成员。
class GameSessionController final : public QObject {
    Q_OBJECT
public:
    /// 一趟启动**在进程起来之前**就已经定下来、而退出时再也拿不到的那几个值。
    ///
    /// NOTE: Java 可执行文件那条路径是挑完 Java 之后才知道的（`ensureJavaForVersion` 会把它
    /// 写进 `javaSettings.javaPath`），退出时 `javaSettings` 早就不是那一趟的了 ——
    /// 所以必须在启动那一刻抄下来。主版本**不在这里**：这一趟实际用的那个主版本是
    /// 异常退出那一刻现读 `<javaPath>/release` 得到的（见 buildCrashRecord 那段），
    /// 平时一次盘都不读。
    struct GameSessionFacts {
        QString instanceId;
        QString instanceName;
        QString accountName;
        qint64 processId = 0;
        qint64 startedAtMs = 0;
        int requiredJavaMajor = 0;
        QString javaPath;
        int xmxMb = 0;
        int xmsMb = 0;
        QString loaderName;
        QString loaderVersion;
    };

    /// NOTE: 这不是"把会话表抄一份"——抄的是**导出那一份文件要用的那几个值**，而且只在
    /// "这一份开着日志窗"时才有（窗关了、会话结束了，这一笔立刻删掉，见
    /// `ensureGameLogWindow()` 里那个 finished 接法）。不删的话它会随开过的实例数一直长。
    struct GameLogFacts {
        QString instanceId;   ///< `versions/` 下面那个目录名
        QString accountName;  ///< 这一趟用的身份
        qint64 processId = 0;
        qint64 startedAtMs = 0;
        bool finished = false;
        bool crashed = false;
        int exitCode = 0;
    };

    /// 这一层伸手去够"只有主窗口说得出来"的那几样。一个一个都是**问**，不是拿：日志窗要摆到
    /// 启动器旁边（那份几何只有主窗口有）、异常退出那一项要记进主窗口的账本、导出那两份文件
    /// 的保存框要挂在主窗口上 —— 这些不在这一层的份内，但它们**什么时候**发生是这一层说了算。
    struct Host {
        /// 记一句日志。
        std::function<void(const QString &text)> log;
        /// 账本上那件「启动 X」的第 1 步（就是"起来了"）完成了。
        std::function<void(Tasks::TaskId id)> launchTaskStepDone;
        std::function<void(Tasks::TaskId id, int sessionId)> bindTaskSession;
        /// 把那件任务收尾（终态只落一次，见 `TaskManager::finish()`）。
        std::function<void(Tasks::TaskId id, bool success, const QString &errorSummary)> finishLaunchTask;
        /// 把那件任务按**用户取消**收尾（`TaskState::Cancelled`，不记失败、不点红点）。
        /// 用户自己按「停止游戏」停在一行日志都没出的时候，那一次启动**不是
        /// 失败** —— 走 `finishLaunchTask(..., false, ...)` 的话，任务中心会多出一条
        /// 红色的「启动 X 失败」外加底栏那颗红点，全是用户自己点出来的。
        std::function<void(Tasks::TaskId id)> cancelLaunchTask;
        /// 底栏按当前状态重画一遍（左下角那颗三态方块 + 中间那格）。
        std::function<void()> updateBottomBar;
        /// 实例树上那些"（运行中）"重画一遍。
        std::function<void()> updateInstanceTreeRunState;
        /// 实例的显示名（"N 个实例"那两句话、以及启动那一趟的快照要用）。
        std::function<QString(const QString &versionId)> instanceDisplayName;
        std::function<void(int sessionId, int exitCode, bool crashed)> reportGameCrash;
        /// 日志窗底下那颗「导出日志…」。
        std::function<void(int sessionId)> exportGameLog;
        std::function<void(int sessionId)> exportGameErrorReport;
        /// 启动器现在占着的那块几何（日志窗要摆到它**旁边**，别一上来就压住它）。
        std::function<QRect()> launcherFrame;
        /// 「收起启动器，游戏退出后自动回来」那一档：把窗口叫回来了。
        std::function<void()> restoreWindowAfterGame;
    };

    GameSessionController(LauncherManager *launcherManager, LauncherSettings *settings,
                          VersionManager *versionManager, LaunchHistoryController *launchHistory,
                          MainWindowUI *ui, Host host, QObject *parent);

    // ---------- 账本（写侧只有两处：LaunchController 的 beginSession，和"用户停的"那一笔）----------

    /// **这一层唯一的开端**：进程真的起来了（`startGame()` 返回了非零会话号）。
    ///
    /// 从这一刻起：启动进度算在走（到这一份自己吐出第一行输出为止）、这一份对上那件任务、
    /// 名字和启动事实各留一份（退出那一刻它们都查不到了）。
    void beginSession(int sessionId, const QString &displayName, Tasks::TaskId launchTask,
                      const GameSessionFacts &facts);

    /// 现在还有没有"启动进度在走"的会话（底栏左下角那颗方块的三态判据之一）。
    bool hasLaunchingSessions() const { return !m_launchingSessions.isEmpty(); }

    /// 这一份会话当初的启动事实（退出那一刻用它，之后就没有了）。没有这一号 = 空结构。
    GameSessionFacts sessionFacts(int sessionId) const { return m_sessionLaunchFacts.value(sessionId); }

    /// 这一份会话的日志窗（没开着 = nullptr）。**别存这个指针**：窗带 WA_DeleteOnClose，
    /// 关掉就销毁了（见 `ensureGameLogWindow()` 那段）。
    GameLogWindow *gameLogWindow(int sessionId) const {
        return m_gameLogWindows.value(sessionId, nullptr);
    }

    GameLogFacts logFacts(int sessionId) const { return m_gameLogFacts.value(sessionId); }

    /// 这一份会话**最近一段原始输出**（不管日志窗开没开、有没有被用户关掉）。
    ///
    /// 存的是最近 `kSessionLogTailLines` 行原文（级别在查询时现认，规则仍是
    /// `RuntimeOutputModel::classifyLine` 那一个出处）。日志窗还开着时报告优先读窗
    /// （那边留 20000 行），**只有窗不在**才用这一份。
    QString sessionLogTail(int sessionId) const;
    QString sessionLogTailErrors(int sessionId) const;

    /// 关启动器那一刻把还开着的日志窗收掉。
    ///
    /// 那些窗**没有父窗口**（在 Windows 上"有主的窗"永远压在启动器上面，见
    /// `ensureGameLogWindow()` 里那段），所以 Qt 不会顺手把它们带走 —— 不自己收的话，
    /// 关掉启动器之后屏幕上会留着几扇无主的日志窗。
    void closeAllGameLogWindows();

    /// 在内置查看器里打开一份日志文件（`.log` / `.txt` / `.log.gz`，认 gzip 头）。
    /// 读盘与解压在工作线程；同一路径已经开着就抬到前面。和游戏会话无关，
    /// 窗同样没有父窗口，`closeAllGameLogWindows()` 一并收掉。
    void openLogFile(const QString &path);

    /// 「收起启动器，游戏退出后回来」那一档：窗口是为**启动游戏**才藏起来的。
    /// 只有它被记下来，游戏退出时才会把窗口叫回来 —— 用户自己最小化、自己收起来的不算。
    void noteHiddenForGame() { m_hiddenForGame = true; }

    // ---------- 进程信号进来的三个槽（主窗口按名字转过来，见 mw_check 那些钉子）----------

    void onProcessOutput(int sessionId, const QString &text);
    void onProcessError(int sessionId, const QString &text);
    void onGameFinished(int sessionId, int exitCode, QProcess::ExitStatus exitStatus);
    /// 这一号会话在**真的起来之前**就被收回（`LauncherManager::sessionDiscarded`）——
    /// 把刚才那条报错顺手替它建出来的东西收干净（见实现里那段）。
    void onSessionDiscarded(int sessionId);
    /// 游戏第一次出窗口之前最多等多久（毫秒）；过了就按首条输出那一刻算成功。
    static int gameWindowWaitMs() { return 120000; }

    // ---------- 日志窗 ----------

    /// 这一份会话的下一条输出（「显示游戏日志」开着、或者那一扇已经开着才会写进去）。
    void showGameLogLine(int sessionId, const QString &text);

    /// 要一扇日志窗（没有就建）。@p userAsked = 用户自己点名要的（底栏那一行那颗「日志」），
    /// 它能把"用户关过这一份"那一笔记号撤掉。
    GameLogWindow *ensureGameLogWindow(int sessionId, bool userAsked);

    /// 用户点名看某一号会话的日志（底栏那一行那颗「日志」）。已经开着就抬到前面来；
    /// 没开着、而会话还在跑，就给他建一扇；会话已经结束了就照实说（日志只活在窗里）。
    void openGameLogForSession(int sessionId);

    // ---------- 启动中那两句、以及停止 ----------

    QString launchingInstanceName() const;
    QString launchingStatusName() const;
    bool confirmStopGame(const QString &displayName, int sessions);
    void stopGameSession(int sessionId);

    // ---------- 给验证工程摆现场（产品代码里**不要**调）----------

    /// 把某一号会话的"启动事实"直接摆出来（真起一个游戏进程是验证工程做不到的事）。
    void seedSessionFacts(int sessionId, const QString &versionId, const QString &displayName,
                          int requiredJavaMajor, const QString &javaPath, int xmxMb, int xmsMb,
                          const QString &loaderName, const QString &loaderVersion);
    /// 这一号会话是用户从启动器里主动停止的。
    void markUserStoppedForTest(int sessionId) { m_userStoppedSessions.insert(sessionId); }

private:
    LauncherManager *m_launcherManager = nullptr;
    LauncherSettings *m_settings = nullptr;
    VersionManager *m_versionManager = nullptr;
    LaunchHistoryController *m_launchHistory = nullptr;
    /// 内嵌提示要挂在这一层的遮罩上（`confirmStopGame()` 那一问）。
    MainWindowUI *m_ui = nullptr;
    Host m_host;

    // ---------- ① 账本 ----------

    /// 哪几份会话的启动进度还在走：从"进程真的起来了"（startGame 返回非零）到那一份自己
    /// 吐出第一行输出为止 —— 中间那段是 JVM 和游戏自己在初始化，界面上没别的反馈。
    /// 命名刻意不叫 m_launching：它表示的是"这段进度要显示"而不是"正在启动"。
    ///
    /// 存的是**会话号**不是版本号：同一个实例可以被多开，按版本号记的话第二个起来就把
    /// 第一个那笔盖掉了（然后第一份出话时"启动中"照样挂着，直到它退出才收）。
    QSet<int> m_launchingSessions;
    /// 会话号 → 那一次启动的任务号（和 m_launchingSessions / m_sessionDisplayNames 同寿）。
    QHash<int, Tasks::TaskId> m_launchTaskIds;
    /// 已经出话、还在等游戏窗口的那几份：会话号 → 开始等的时刻（毫秒）。启动任务在
    /// 窗口出来那一刻才算成（主页启动卡片的「等待游戏窗口」）；见 launchOutputSeen()。
    QHash<int, qint64> m_awaitingWindow;
    QTimer *m_windowPoll = nullptr;
    void launchOutputSeen(int sessionId);
    void pollGameWindows();
    void finishLaunchSucceeded(int sessionId);
    /// 会话号 → 那一份的实例显示名。**启动那一刻记一次**，退出时用掉、删掉。
    ///
    /// 唯一用途是 onGameFinished() 里那行日志的 `[实例名] ` 前缀 —— 那时会话已经从
    /// LauncherManager 的表里摘掉了（discard 在 emit 之前，是有意的），`session(id)` 返回的
    /// 是空快照，所以名字只能自己留一份。多开时没有它，"游戏进程已结束，退出码：0" 这一行
    /// 分不清是谁退了（它上面那些 [游戏输出] 行都有名字，对比之下更刺眼）。
    QHash<int, QString> m_sessionDisplayNames;
    /// 会话号 → 那一趟的启动事实。**启动成功那一刻记、退出那一刻用掉并删掉**（和
    /// `m_sessionDisplayNames` 同寿）—— 不清的话它会随着开过的实例数一直长。
    QHash<int, GameSessionFacts> m_sessionLaunchFacts;
    /// 用户已经确认"停止游戏"、正在等 QProcess::finished 回来的会话。
    /// 强杀通常会给出 CrashExit / 非零退出码，但这是用户主动操作，不能据此生成异常退出任务。
    QSet<int> m_userStoppedSessions;
    /// 窗口是不是为了「收起启动器，游戏退出后回来」这一档才藏起来的。
    /// 只有它为真时 onGameFinished() 才会把窗口叫回来 —— 用户自己最小化的窗口
    /// 不该被游戏的退出给弹出来。
    bool m_hiddenForGame = false;

    // ---------- ② 日志窗 ----------

    /// 哪几份会话现在开着日志窗（会话号 → 那个窗）。**一份会话一个窗**：多开时两份输出
    /// 混在一个窗里等于没法看，见 ui/GameLogWindow.hpp 的说明。
    ///
    /// NOTE: 那个窗建出来就带 WA_DeleteOnClose（关掉即销毁），所以这里的指针**会**失效 ——
    /// 窗自己在 finished() 里把这一笔摘掉（见 showGameLogLine 里的接法）。不要在这里
    /// 存裸指针却在别处直接用：除了 showGameLogLine() 和 onGameFinished()，
    /// 没有第三处读它。
    QHash<int, GameLogWindow *> m_gameLogWindows;
    /// 文件查看器（路径 → 窗）。窗关掉即销毁，QPointer 自己变空。
    QHash<QString, QPointer<GameLogWindow>> m_logFileWindows;
    /// 用户**手动关掉过**日志窗的那几份会话。关掉之后这一份不再自动开新窗 ——
    /// "我刚把它关了、下一行输出又给我弹回来"是这类窗最招人烦的失败模式。
    /// 会话结束（onGameFinished）时清掉：下一份游戏是另一回事。
    QSet<int> m_gameLogDismissedSessions;
    QHash<int, GameLogFacts> m_gameLogFacts;

    // ---------- ③ 进程输出的底账（和日志窗无关）----------

    QHash<int, QStringList> m_sessionLogLines;
    /// 会话号 → 还没等到换行的那半行。QProcess 吐出来的是一段一段的字节，一行可能分两发
    /// 到（和 `GameLogWindow` 里 m_pending 同一个道理）。
    QHash<int, QString> m_sessionLogPending;
    /// 一份会话留多少行。500 行的 logTail 上限（GameExitDiagnosis::kLogTailLineLimit）
    /// 之外还要留得下更有用的上下文，又不至于把几个实例的日志堆到几十 MB。
    static constexpr int kSessionLogTailLines = 4000;

    void rememberSessionLogText(int sessionId, const QString &text);
};
