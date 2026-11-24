#include "GameSessionController.hpp"
#include <QCoreApplication>
#include "util/SecretRedactor.hpp"

#include "GameLogWindow.hpp"
#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "RuntimeOutputModel.hpp"
#include "controllers/LaunchHistoryController.hpp"
#include "diag/GameExitDiagnosis.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"
#include "tasks/TaskManager.hpp"

#include "util/LogFileReader.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
struct WindowProbe {
    DWORD pid = 0;
    bool found = false;
};

BOOL CALLBACK probeGameWindow(HWND hwnd, LPARAM param) {
    auto *probe = reinterpret_cast<WindowProbe *>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != probe->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
    probe->found = true;
    return FALSE;
}
#endif

/// 进程 @p pid 有没有一扇看得见的顶层窗口（= 游戏窗口出来了）。
/// shortcut: 只认进程自己的窗口（游戏 JVM 是启动器直接拉起的），不追子进程；套壳启动的整合包出现了再补。
/// 非 Windows 不探，直接当已出现（首条输出即算成功，同改之前）。
bool gameWindowShown(qint64 pid) {
#ifdef Q_OS_WIN
    WindowProbe probe;
    probe.pid = static_cast<DWORD>(pid);
    EnumWindows(probeGameWindow, reinterpret_cast<LPARAM>(&probe));
    return probe.found;
#else
    Q_UNUSED(pid);
    return true;
#endif
}

QString sessionLogTag(const LauncherManager::GameSession &game) {
    if (game.sessionId == 0 || game.displayName.isEmpty()) return {};
    return QStringLiteral("[%1] ").arg(game.displayName);
}
}  // namespace

GameSessionController::GameSessionController(LauncherManager *launcherManager, LauncherSettings *settings,
                                             VersionManager *versionManager,
                                             LaunchHistoryController *launchHistory, MainWindowUI *ui,
                                             Host host, QObject *parent)
    : QObject(parent), m_launcherManager(launcherManager), m_settings(settings),
      m_versionManager(versionManager), m_launchHistory(launchHistory), m_ui(ui),
      m_host(std::move(host)) {}

void GameSessionController::beginSession(int sessionId, const QString &displayName,
                                         Tasks::TaskId launchTask, const GameSessionFacts &facts) {
    // 四笔一起写：它们**同寿**（都在 onGameFinished 末尾用掉并删掉）。拆开写的话，
    // "进程起来了但还没出话"这一段里读名字的那几处会读到半截。
    //
    // 顺序上安全：startGame() 内部那条"游戏已启动"的 processOutput 是在它 return 之前
    // 同步发出的，那时这一份还不在 m_launchingSessions 里，所以不会被当成"游戏已经出话了"
    // （见下面 onProcessOutput() 那段）。
    m_launchingSessions.insert(sessionId);
    m_launchTaskIds.insert(sessionId, launchTask);
    m_sessionDisplayNames.insert(sessionId, displayName);
    m_sessionLaunchFacts.insert(sessionId, facts);
    if (m_host.bindTaskSession && launchTask != Tasks::invalidTaskId()) {
        m_host.bindTaskSession(launchTask, sessionId);
    }
}

void GameSessionController::closeAllGameLogWindows() {
    // NOTE: 先把表清空再逐个 delete。窗的 finished 处理函数会来动这张表
    // （m_gameLogWindows.remove），一边删一边被删的容器是未定义行为。
    // values() 拿的是一份拷贝，清表在这一刻，所以那几下 remove 落空而不出错。
    const QList<GameLogWindow *> logWindows = m_gameLogWindows.values();
    m_gameLogWindows.clear();
    for (GameLogWindow *window : logWindows) {
        delete window;
    }
    const auto fileWindows = m_logFileWindows.values();
    m_logFileWindows.clear();
    for (const QPointer<GameLogWindow> &window : fileWindows) {
        delete window.data();
    }
}

void GameSessionController::openLogFile(const QString &path) {
    if (GameLogWindow *open = m_logFileWindows.value(path).data()) {
        open->showNormal();
        open->raise();
        open->activateWindow();
        return;
    }
    // 父窗口传 nullptr：理由同 ensureGameLogWindow()。
    auto *window = new GameLogWindow(QString(), nullptr);
    window->beginFile(path);
    m_logFileWindows.insert(path, window);
    const int cascade = m_logFileWindows.size() - 1;
    // 读盘 + gzip 解压（起 PowerShell）都在工作线程；窗在读的时候被关掉就丢弃结果。
    auto *watcher = new QFutureWatcher<LogFileReader::Loaded>(window);
    connect(watcher, &QFutureWatcher<LogFileReader::Loaded>::finished, window, [window, watcher] {
        const LogFileReader::Loaded loaded = watcher->result();
        if (loaded.ok) {
            window->setFileLines(loaded.lines, loaded.cutHead);
        } else {
            window->setFileProblem(loaded.problem);
        }
    });
    watcher->setFuture(QtConcurrent::run([path] { return LogFileReader::load(path); }));
    connect(window, &GameLogWindow::openExternalRequested, this, [this](const QString &file) {
        if (!QFileInfo(file).isFile() || !QDesktopServices::openUrl(QUrl::fromLocalFile(file))) {
            InlineMessage::warning(m_ui->overlay(), tr("打开文件"),
                                   tr("无法打开文件：%1").arg(QDir::toNativeSeparators(file)));
        }
    });
    connect(window, &GameLogWindow::exportLogRequested, window, [this, window, path] {
        // 先取文本再弹框：保存框是嵌套事件循环，期间窗可能被关掉。
        const QString text = window->allLogText();
        QString name = QFileInfo(path).fileName();
        if (name.endsWith(QStringLiteral(".gz"), Qt::CaseInsensitive)) name.chop(3);
        if (!name.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)
            && !name.endsWith(QStringLiteral(".log"), Qt::CaseInsensitive))
            name += QStringLiteral(".txt");
        const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                      .filePath(name);
        const QString target = QFileDialog::getSaveFileName(
            window, tr("导出日志"), suggested, tr("文本文件 (*.log *.txt);;所有文件 (*)"));
        if (target.isEmpty()) return;
        const QByteArray bytes = text.toUtf8();
        QSaveFile file(target);
        const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                        && file.write(bytes) == bytes.size() && file.commit();
        if (!ok) {
            InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                                   tr("这个文件没能写出来（%1）：\n%2")
                                       .arg(QDir::toNativeSeparators(target), file.errorString()));
        }
    });
    const QRect frame = m_host.launcherFrame();
    const QScreen *screen = QGuiApplication::screenAt(frame.center());
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen != nullptr) {
        window->move(GameLogWindow::placementFor(frame, window->size(), screen->availableGeometry(), cascade));
    }
    window->show();
}

void GameSessionController::onProcessOutput(int sessionId, const QString &rawText) {
    const QString text = SecretRedactor::redact(rawText);
    m_host.log(tr("[游戏输出] %1%2").arg(sessionLogTag(m_launcherManager->session(sessionId)), text));
    rememberSessionLogText(sessionId, text);
    // 「显示游戏日志」开着时，同一段字也进那一份的游戏日志窗。**放在 appendLog 之后**是
    // 刻意的：日志文件那一份是永远要写的（用户的排查底账），弹窗是附加的。
    showGameLogLine(sessionId, text);
    // 游戏自己出话了 = JVM 和游戏都初始化到能打日志了，**那一份**的"正在启动"到此为止。
    // 底栏中间那格让给下载进度（如果有），左下角换成"实例运行中"。
    //
    // NOTE: 按会话号摘，不按"有没有在启动" —— 同时启动两三个实例时，用布尔量的话第一个
    // 出话就会把后面那几个的启动进度一起收掉。
    launchOutputSeen(sessionId);
}

void GameSessionController::onProcessError(int sessionId, const QString &rawText) {
    const QString text = SecretRedactor::redact(rawText);
    m_host.log(tr("[游戏错误] %1%2").arg(sessionLogTag(m_launcherManager->session(sessionId)), text));
    // stderr 也进底账（和 stdout 同一个去处）—— 很多版本把日志整个写在 stderr 上。
    rememberSessionLogText(sessionId, text);
    // stderr 也进日志窗（下面那一行），理由和上面那条一样。**不**给它加"错误"前缀：
    // 游戏往 stderr 写的不全是错误（很多模组把 INFO 也写 stderr），在窗里加前缀反而误导。
    showGameLogLine(sessionId, text);
    // stderr 出东西也算"起来了"：有些版本把日志全写 stderr，等 stdout 会一直等下去。
    launchOutputSeen(sessionId);
}

void GameSessionController::launchOutputSeen(int sessionId) {
    if (!m_launchingSessions.remove(sessionId)) return;
    // 这一份出话了：底栏那格「正在启动」到此为止。账本上那件「启动 X」要等**游戏窗口**出来才算成
    // （主页启动卡片停在「等待游戏窗口」）；探不到窗口的平台、或者窗口已经在了，就在这一刻收。
    // 收在启动这一刻、而不是"游戏退出"那一刻：之后几个小时是"游戏在运行"，不是"启动中"。
    m_host.updateBottomBar();
    const qint64 pid = m_launcherManager->session(sessionId).processId;
    if (pid <= 0 || gameWindowShown(pid)) {
        finishLaunchSucceeded(sessionId);
        return;
    }
    m_awaitingWindow.insert(sessionId, QDateTime::currentMSecsSinceEpoch());
    if (m_windowPoll == nullptr) {
        m_windowPoll = new QTimer(this);
        m_windowPoll->setInterval(500);
        connect(m_windowPoll, &QTimer::timeout, this, &GameSessionController::pollGameWindows);
    }
    m_windowPoll->start();
}

void GameSessionController::pollGameWindows() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int> done;
    for (auto it = m_awaitingWindow.cbegin(); it != m_awaitingWindow.cend(); ++it) {
        const qint64 pid = m_launcherManager->session(it.key()).processId;
        // 等满 gameWindowWaitMs() 还没窗口（无窗口的服务端式启动、窗口探不到）：按已出话算成，别让卡片一直挂着。
        if (pid <= 0 || gameWindowShown(pid) || now - it.value() >= gameWindowWaitMs()) done.append(it.key());
    }
    for (const int id : done) {
        m_awaitingWindow.remove(id);
        finishLaunchSucceeded(id);
    }
    if (m_awaitingWindow.isEmpty() && m_windowPoll != nullptr) m_windowPoll->stop();
}

void GameSessionController::finishLaunchSucceeded(int sessionId) {
    const Tasks::TaskId launchTask = m_launchTaskIds.take(sessionId);
    m_host.launchTaskStepDone(launchTask);
    m_host.finishLaunchTask(launchTask, true, QString());
}

void GameSessionController::onSessionDiscarded(int sessionId) {
    // `startGame()` 的三条早退路径（会话数到顶 / 找不到 Java / 进程起不来）
    // 会先带号发一句 `processError`，再把这一号会话收回。那一句本身是对的（用户要看到
    // 原因，`LaunchController` 那边负责弹出来），但它走的是**和游戏 stderr 同一条槽** ——
    // `showGameLogLine()` 收到"这一份的第一行"就给这一号建了一扇游戏日志窗，底账也按号
    // 记了一笔。会话一收，从此再没有任何信号提到这个号：那扇窗不会自己关（自动关窗只
    // 发生在"正常以 0 退出"那条路上，见 onGameFinished()），底账也不会被删 —— 每启动
    // 失败一次就多留一扇只有一行字的窗和一笔内存，直到用户手动去关。
    //
    // 这里把刚才顺手建出来的那几笔一起收掉。窗走 `close()`（不是 `delete`）：和用户自己
    // 关窗、以及 closeAllGameLogWindows() 之外的收尾走同一条路，finished 里那一笔
    // "用户关过"随之落下 —— 下面紧跟着就把它清掉（这一份已经结束了，那个集合不该为它
    // 留名，和 onGameFinished() 末尾同一个理由）。
    if (GameLogWindow *window = m_gameLogWindows.take(sessionId)) {
        window->close();
    }
    m_gameLogDismissedSessions.remove(sessionId);
    m_gameLogFacts.remove(sessionId);
    m_sessionLogLines.remove(sessionId);
    m_sessionLogPending.remove(sessionId);
    m_sessionDisplayNames.remove(sessionId);
}

void GameSessionController::showGameLogLine(int sessionId, const QString &text) {
    // sessionId <= 0 是"不挂在任何实例上的话"（比如"同时运行的游戏已经有 8 个了"那一句，
    // 它是 startGame 在登记会话之前发的，见那边的说明）。那种话没有日志窗可言。
    if (sessionId <= 0) {
        return;
    }
    // 开关只管“要不要自动冒出一扇新窗”。已经存在的窗可能是用户从底栏明确点开的；
    // 即使自动显示关着，那扇窗也必须继续收到输出，否则用户得到的只是一扇永远为空的窗。
    if (!m_settings->showGameLog() && !m_gameLogWindows.contains(sessionId)) {
        return;
    }
    GameLogWindow *window = ensureGameLogWindow(sessionId, /*userAsked=*/false);
    if (window == nullptr) {
        return;
    }
    window->appendText(text);
}

void GameSessionController::rememberSessionLogText(int sessionId, const QString &text) {
    if (sessionId <= 0 || text.isEmpty()) {
        return;
    }
    QString &pending = m_sessionLogPending[sessionId];
    pending += text;
    QStringList lines = pending.split(QLatin1Char('\n'));
    pending = lines.takeLast();
    // 一行长到 1 MB 还没等到换行（有的模组会把整份配置文件打出来）：别让这半行无限长。
    if (pending.size() > 1024 * 1024) {
        lines.append(pending);
        pending.clear();
    }
    if (lines.isEmpty()) {
        return;
    }
    QStringList &tail = m_sessionLogLines[sessionId];
    tail += lines;
    if (tail.size() > kSessionLogTailLines) {
        tail = tail.mid(tail.size() - kSessionLogTailLines);
    }
}

QString GameSessionController::sessionLogTail(int sessionId) const {
    // 和 `GameLogWindow::allLogText()` 同一个拼法：整行各带换行，末尾那半行原样接上。
    const QStringList lines = m_sessionLogLines.value(sessionId);
    const QString pending = m_sessionLogPending.value(sessionId);
    if (lines.isEmpty()) {
        return pending;
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n') + pending;
}

QString GameSessionController::sessionLogTailErrors(int sessionId) const {
    // 认级别的规则只有一个出处（`RuntimeOutputModel::classifyLine`）—— 和日志窗、
    // 错误报告是同一套，不在这里按字符串再猜一遍。
    const QStringList lines = m_sessionLogLines.value(sessionId);
    if (lines.isEmpty()) {
        return {};
    }
    QStringList errors;
    RuntimeOutputModel::Level last = RuntimeOutputModel::Level::Info;
    for (const QString &line : lines) {
        last = RuntimeOutputModel::classifyLine(line, last);
        if (RuntimeOutputModel::levelIndex(last)
            >= RuntimeOutputModel::levelIndex(RuntimeOutputModel::Level::Error)) {
            errors.append(line);
        }
    }
    return errors.isEmpty() ? QString() : errors.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

GameLogWindow *GameSessionController::ensureGameLogWindow(int sessionId, bool userAsked) {
    if (sessionId <= 0) {
        return nullptr;
    }
    GameLogWindow *window = m_gameLogWindows.value(sessionId, nullptr);
    if (window != nullptr) {
        return window;
    }
    // 用户关掉过这一份的窗就不再**自动**开（见 m_gameLogDismissedSessions 的说明）。
    // 用户自己点名要的那一发例外，而且顺手把那一笔记号撤了。
    if (m_gameLogDismissedSessions.contains(sessionId)) {
        if (!userAsked) {
            return nullptr;
        }
        m_gameLogDismissedSessions.remove(sessionId);
    }
    // 第一次收到**这一份**的输出才建窗（不去"启动成功"那一刻建）：这样三条早退路径
    // 上一个空窗都不会冒出来，而"进程真的起来了"那一句（startGame 内部同步发的第一条
    // processOutput）正好在建窗这一刻被写上 —— 用户看到的第一行就是"游戏已启动"。
    const QString name = m_launcherManager->session(sessionId).displayName;
    // **父窗口传 nullptr**：在 Windows 上"有主的窗"永远压在自己的主窗上面，
    // 用户点启动器也翻不上来 —— 日志窗会一直遮住启动器。这是平台规则，不是能调的
    // 风格，所以只能不给它主。代价是"跟着启动器一起关掉"要自己接
    // （见 closeAllGameLogWindows()）。
    window = new GameLogWindow(name, nullptr);
    // 错开的位置数**在插进去之前**取：开第二扇时那是 1（往下错一点，不盖住第一扇）。
    const int cascade = m_gameLogWindows.size();
    m_gameLogWindows.insert(sessionId, window);
    // 这一份会话的事实留一个快照（「导出错误报告」要用，见 GameLogFacts 的说明）：
    // **建窗这一刻**取，因为再往后这个会话只会越来越空（结束时记录会被摘掉）。
    // 那一刻会话一定还在（窗是收到它的第一行输出才建的）。
    {
        const LauncherManager::GameSession live = m_launcherManager->session(sessionId);
        GameLogFacts snapshot;
        snapshot.instanceId = live.versionId;
        snapshot.accountName = live.accountName;
        snapshot.processId = live.processId;
        snapshot.startedAtMs = live.startedAtMs;
        m_gameLogFacts.insert(sessionId, snapshot);
    }
    // 底下那两颗导出键。它们只发信号，弹框、写盘全在主窗口那边 —— `ui/`
    // 那一层不认识数据目录，也不该认识（见 GameLogWindow 的文件头）。
    connect(window, &GameLogWindow::exportLogRequested, this,
            [this, sessionId] { m_host.exportGameLog(sessionId); });
    connect(window, &GameLogWindow::exportReportRequested, this,
            [this, sessionId] { m_host.exportGameErrorReport(sessionId); });
    // 窗自己带 WA_DeleteOnClose，所以**关掉就会销毁**。销毁之前先把这一笔摘掉，
    // 顺便记下"用户关过这一份" —— 两个动作都必须在 finished 里做，不能在 destroyed 里：
    // finished 是"用户关窗"这一个动作的信号，destroyed 在父窗口析构时也会来一次，
    // 而那一次不该被当成"用户不想看日志"。
    connect(window, &QDialog::finished, this, [this, sessionId](int) {
        m_gameLogWindows.remove(sessionId);
        m_gameLogDismissedSessions.insert(sessionId);
        // 窗没了，那份快照也就没用了（它唯一的用途是"给那一扇窗导一份报告"）——
        // 不清的话它会随着开过的实例数一直长。
        m_gameLogFacts.remove(sessionId);
    });
    // 摆到启动器**旁边**（右 → 左 → 居中），别一上来就压住它 —— 用户抱怨的另一半
    // 正是"挡着"。启动器跨屏时按它在那一块屏的可用区域算（扣掉任务栏）。
    const QRect frame = m_host.launcherFrame();
    const QScreen *screen = QGuiApplication::screenAt(frame.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        window->move(
            GameLogWindow::placementFor(frame, window->size(), screen->availableGeometry(), cascade));
    }
    // 非模态（没用 exec()）：游戏跑着的时候用户还要在启动器上干别的。做成模态的话
    // 那扇窗会把整个启动器挡住 —— 一个"看输出"的窗没有拦人的道理。
    window->show();
    return window;
}

void GameSessionController::openGameLogForSession(int sessionId) {
    if (sessionId <= 0) {
        return;
    }
    // 那一扇已经开着（"随时能看输出"的常驻窗）⇒ 抬到前面来，一个字都不重开。
    if (GameLogWindow *window = m_gameLogWindows.value(sessionId, nullptr)) {
        window->show();
        window->raise();
        window->activateWindow();
        return;
    }
    // 窗还没建出来。两种情形在**产品行为**上差得很远，所以这儿分开办：
    //
    // · 会话还在跑（窗还没建 = 那一刻还没收到过它的输出，见 ensureGameLogWindow 里
    // "第一条输出才建窗"那一段，或者是**用户自己关过**这一扇，见 m_gameLogDismissed-
    // Sessions）⇒ 用户点名要看，就给他建一扇出来。这一刻可能是空的（一句输出都还没有），
    // 那一窗自己会摆一句"还没有输出"—— 从这个窗往后每一行都能看见。
    // · 会话已经结束了 ⇒ 那一份日志**已经没了**（它只活在窗里，见 m_gameLogWindows 那
    // 一段）。这里**不留一句"点不动"**：说清"它结束了、日志跟着没了"。
    // （异常退出那一路另有去处：任务中心那一项记着当时那两段日志，见 openTaskLog()。）
    const LauncherManager::GameSession game = m_launcherManager->session(sessionId);
    if (game.sessionId == 0) {
        m_host.log(tr("游戏已退出，日志窗口已关闭。"));
        return;
    }
    GameLogWindow *window = ensureGameLogWindow(sessionId, /*userAsked=*/true);
    if (window == nullptr) {
        return;
    }
    window->show();
    window->raise();
    window->activateWindow();
}

void GameSessionController::onGameFinished(int sessionId, int exitCode, QProcess::ExitStatus exitStatus) {
    m_launchHistory->finishPlayTime(sessionId);
    // 必须在任何“是否异常”的判断之前取走：QProcess::kill() 在 Windows 上通常表现为
    // CrashExit / 非零退出码，但从任务中心主动点“停止游戏”不是一次意外崩溃。
    const bool userStopped = m_userStoppedSessions.remove(sessionId);
    LauncherManager::GameSession game;
    game.sessionId = sessionId;
    game.displayName = m_sessionDisplayNames.value(sessionId);
    const QString tag = sessionLogTag(game);
    if (userStopped) {
        m_host.log(tr("%1游戏已由用户停止。").arg(tag));
    } else if (exitStatus == QProcess::CrashExit) {
        m_host.log(tr("%1游戏崩溃，退出码：%2").arg(tag).arg(exitCode));
    } else {
        m_host.log(tr("%1游戏进程已结束，退出码：%2").arg(tag).arg(exitCode));
    }
    // 日志窗那边也收个尾，两种收法：
    //
    // - **正常以 0 退出就自己关掉**（ 用户说的「如果游戏正常以0退出就自动关闭日志
    // 界面」）。那一趟没有任何可看的东西（真出了事不会给 0），留一扇只写着一行"已退出"的
    // 窗等于替用户留了个垃圾。判据是 `GameLogWindow::autoCloseOnExit()`（**唯一出处**，
    // 验证工程拿真值表钉它）—— 注意崩溃一律不关，哪怕码正好是 0。
    // - 其余情况**留着**（用户正是要看崩溃前那几行），只补一行"到此为止"，不关窗也不销毁
    // —— 关不关是用户的事（他关了就走 finished 那条路）。末尾这行必须是**末行**才读得懂：
    // "进程已结束，退出码 -1" 之后还要有东西的话，用户会以为窗口没跟上。
    //
    // NOTE: 关窗走的是 close() → WA_DeleteOnClose → finished 那条路（它会顺手把这一笔
    // 记进"用户关过的不再自动开"）。下面那句 remove 因此**必须在关窗之后**：关窗
    // 那一刻刚记上的那一笔，紧接着就要清掉（这一份已经结束了，那个集合不该为它留名）。
    // 退出这件事记进那一份快照（「导出错误报告」的头部要写退出码，而**这个值是这一刻才知道
    // 的**、错过就再也拿不到了）。必须排在下面那次 close() **之前**：正常退出会关窗，而关窗
    // 那个 finished 里会把整份快照删掉。
    if (m_gameLogFacts.contains(sessionId)) {
        GameLogFacts &snapshot = m_gameLogFacts[sessionId];
        snapshot.finished = true;
        snapshot.crashed = exitStatus == QProcess::CrashExit;
        snapshot.exitCode = exitCode;
    }
    if (GameLogWindow *logWindow = m_gameLogWindows.value(sessionId, nullptr)) {
        if (userStopped ||
            GameLogWindow::autoCloseOnExit(exitCode, exitStatus == QProcess::CrashExit)) {
            logWindow->close();
        } else {
            logWindow->setExited(exitCode, exitStatus == QProcess::CrashExit);
            logWindow->appendText(exitStatus == QProcess::CrashExit
                                      ? tr("\n[游戏已崩溃退出，退出码：%1]\n").arg(exitCode)
                                      : tr("\n[游戏已退出，退出码：%1]\n").arg(exitCode));
        }
    }
    // 这一份结束了，"用户关过它的日志窗"这件事不再有意义 —— 下一个实例是另一回事，
    // 不清的话这个集合会随着开过的实例数一直长。
    m_gameLogDismissedSessions.remove(sessionId);

    // ---------- 这一趟的结局写回「上一次启动」那本账 ----------
    //
    // 位置有三处讲究，全都要在这一段之前：
    // · `m_launchingSessions` 那一格判的是"游戏**连第一行日志都没打出来**就结束了"
    // （游戏吐出第一行输出时它才被摘掉，见 onProcessOutput）—— 那不算"玩过"；
    // · `m_sessionLaunchFacts` 里放着**启动时刻**，它是账上那一条唯一的钥匙
    // （会话早从 LauncherManager 的表里摘掉了，见上面那段 NOTE）；
    // · 那个判据本身只有一个出处（`GameExitDiagnosis::isAbnormal()`），下面
    // 「异常退出补一项」那一支读的也是**同一个值**，不重算第二遍（两处各算一次的话，
    // 以后改判据就会有一处漏改）。
    const bool abnormalExit =
        !userStopped && GameExitDiagnosis::isAbnormal(exitCode, exitStatus == QProcess::CrashExit);
    m_launchHistory->recordFinish(
        m_sessionLaunchFacts.value(sessionId).startedAtMs,
        LaunchHistoryController::resultFor(abnormalExit, m_launchingSessions.contains(sessionId)),
        QDateTime::currentMSecsSinceEpoch());

    // 进程没了，它那一份的"启动中"要收掉；左下角那条"实例运行中"由 runningGames() 现算，
    // 这里只管重画（那一份在 LauncherManager 里已经从表里摘掉了）。
    m_launchingSessions.remove(sessionId);
    // 账本上那件「启动 X」要是**还活着**，说明这一次它一行输出都没出就结束了（正常那一
    // 路在上面 onProcessOutput() / onProcessError() 里已经收过尾了，那个表里就没它了）。
    // 进程起来了但游戏自己没撑到能打日志 —— 按退出码如实说，不一律记成失败：以 0 退出
    // 的（比如用户刚进去就自己关了）确实是一次成功的启动。
    //
    // NOTE: 对已经成功收过尾的那一件，这里什么都不做（终态只落一次，见 TaskManager::finish），
    // 所以"游戏跑了三小时后正常退出"不会把那次成功改写成失败。
    const auto finishingLaunchTask = m_launchTaskIds.take(sessionId);
    const bool outputSeen = m_awaitingWindow.remove(sessionId) > 0;  // 出过话、窗口还没出来就退了
    const bool launchSucceeded = exitCode == 0 && exitStatus != QProcess::CrashExit;
    // 用户自己按的「停止游戏」不是失败。`kill()` 在 Windows 上报的是
    // CrashExit / 非零退出码，不看 `userStopped` 的话，一次**主动取消**会以
    // 「游戏进程没等到能打日志就结束了，退出码 1」收尾 —— 任务中心多一行红色失败、
    // 底栏亮起红点，说的全是用户自己刚做的事。按 `Cancelled` 收才对得上他的操作
    // （和任务中心里手动取消一件任务同一个终态、同样不点红点）。
    if (userStopped) {
        m_host.cancelLaunchTask(finishingLaunchTask);
    } else {
        if (launchSucceeded) m_host.launchTaskStepDone(finishingLaunchTask);
        m_host.finishLaunchTask(finishingLaunchTask, launchSucceeded,
                                outputSeen ? tr("游戏窗口出现前进程就结束了，退出码 %1。").arg(exitCode)
                                           : tr("游戏进程没等到能打日志就结束了，退出码 %1。").arg(exitCode));
    }

    // ---------- 异常退出 → 账本上补一项「游戏异常退出」 ----------
    //
    // 位置有两处讲究：
    // · 排在**日志窗那一趟之后** —— 下面要读的就是那一窗里现在这份内容（末尾那行
    // "已崩溃退出"也在里面，正合适）；
    // · 排在 `m_sessionLaunchFacts.remove()` **之前** —— 那一份启动事实（实例名、身份、
    // Java 路径、内存、加载器）是这一项唯一的来源。
    // 判据是 `GameExitDiagnosis::isAbnormal()`（崩溃，或者非 0 退出码；崩了但码正好是 0 的
    // 也算 —— 那种"0"什么都不能证明），和那一层说的必须是同一条。
    // NOTE: `abnormalExit` 在上面（记账那一段）就算好了，这一支读的就是同一个值。
    if (abnormalExit) {
        m_host.reportGameCrash(sessionId, exitCode, exitStatus == QProcess::CrashExit);
    }
    // 名字也到此为止（它唯一的用途就是上面那一行前缀）。不清的话这个表会随着开过的
    // 实例数一直长 —— 和 m_gameLogDismissedSessions 同一个理由。
    m_sessionDisplayNames.remove(sessionId);
    // 进程输出的底账同理：**必须在 reportGameCrash 那一发之后**（它就是在那儿被读走的）。
    // 会话号不会回头复用，留着只会让"开过的实例数 × 4000 行"一直堆在内存里。
    m_sessionLogLines.remove(sessionId);
    m_sessionLogPending.remove(sessionId);
    // 启动事实同理（它唯一的用途是上面那一项）：**必须在 reportGameCrash 那一发之后**。
    m_sessionLaunchFacts.remove(sessionId);
    m_host.updateInstanceTreeRunState();
    m_host.updateBottomBar();

    // 「收起启动器，游戏退出后自动回来」那一档：见 applyLauncherBehaviourAfterLaunch()。
    // 只有**为了启动游戏才藏起来**的窗口才会被叫回来 —— 用户自己最小化、自己收起来的
    // 窗口不该因为游戏退出就跳出来。
    //
    // NOTE: 还要**一份都不剩**才叫回来。同时开着三个时，第一个退出就把窗口弹到用户脸上
    // 会打断他正在看的别的东西；那时他在启动器上想知道的是"还剩谁在跑"。
    if (m_hiddenForGame && m_launcherManager->runningCount() == 0) {
        m_hiddenForGame = false;
        m_host.restoreWindowAfterGame();
    }
}

QString GameSessionController::launchingInstanceName() const {
    // 启动那一下是**同步**的（startGame() 在 UI 线程上等进程起来，见 enterSyncWait），
    // 所以真正卡在同步等待里时最多只有一份。能同时有两份以上的只有"进程起来了、游戏还
    // 没出话"那段异步进度 —— 那时报其中一个名字是错的，报个数才诚实。
    if (m_launchingSessions.size() == 1) {
        const LauncherManager::GameSession game =
            m_launcherManager->session(*m_launchingSessions.constBegin());
        if (game.sessionId != 0) {
            return m_host.instanceDisplayName(game.versionId);
        }
    }
    return tr("%1 个实例").arg(m_launchingSessions.size());
}

QString GameSessionController::launchingStatusName() const {
    // 和上面 launchingInstanceName() 同一套判据（"只有一份时才报名字"，理由在那边），
    // 只有名字的**写法**不同：走 VersionDisplay::launchStatusName()，也就是
    // "有自定义名就只用它，没有就是 版本号-加载器名"，与显示名那一套无关。
    if (m_launchingSessions.size() == 1) {
        const LauncherManager::GameSession game =
            m_launcherManager->session(*m_launchingSessions.constBegin());
        if (game.sessionId != 0) {
            return launchStatusName(m_versionManager->displayParts(game.versionId));
        }
    }
    return tr("%1 个实例").arg(m_launchingSessions.size());
}

bool GameSessionController::confirmStopGame(const QString &displayName, int sessions) {
    // 「停」在 Windows 上就是 TerminateProcess（见 LauncherManager::stopGame()），游戏
    // 来不及存档。**这件事必须在这里说清楚**，默认那一边是"别停"（回车 = 取消）。
    //
    // NOTE: 停游戏只有底栏左面板那行电源走这儿（`stopGameSession()` 恒传 1），所以
    // `sessions > 1` 那半句是**死支**；留着它是因为它是这个函数的一部分
    // （"一次停几份"这件事问话里要说清），不是因为还有人在用。
    const QString text =
        sessions > 1
            ? tr("「%1」正在运行，将强制结束全部 %2 个游戏进程。\n\n未保存的进度会丢失。确认停止？")
                  .arg(displayName)
                  .arg(sessions)
            : tr("「%1」正在运行。强制结束游戏会丢失未保存的进度。\n\n确认停止？")
                  .arg(displayName);
    // 内嵌提示（不再另开一个窗）。默认那一边照旧是**取消**（下标 1），
    // 「停止游戏」是那一档里的危险动作（下标 0）。
    const int choice = InlineMessage::ask(
        m_ui->overlay(), MessageKind::Warning, tr("停止游戏"), text,
        QStringList{tr("停止游戏"), tr("取消")}, 1, 0);
    return choice == 0;
}

void GameSessionController::stopGameSession(int sessionId) {
    // 面板是**弹出来那一刻**的一张快照（它不会跟着刷新），所以"这一行还在不在"得现问一遍。
    const LauncherManager::GameSession game = m_launcherManager->session(sessionId);
    if (game.sessionId == 0) {
        // 用户看那两眼的时候它自己退出了。**不弹窗**：他什么都没做错，弹一个框只会让他
        // 以为出事了；面板上那一行也确实已经不在了。
        m_host.log(tr("这个实例刚刚已经结束了。"));
        return;
    }
    if (!confirmStopGame(game.displayName, 1)) {
        return;
    }
    // 先记再 kill，避免极快进程在 stopGame() 返回之前就把 finished 投递回来。
    m_userStoppedSessions.insert(sessionId);
    if (!m_launcherManager->stopGame(sessionId)) {
        m_userStoppedSessions.remove(sessionId);
        // 上面那一问期间它退出了（游戏自己崩了/被别处停了）。
        m_host.log(tr("「%1」刚刚已经结束了。").arg(game.displayName));
        return;
    }
    m_host.log(tr("已停止「%1」。").arg(game.displayName));
    // 进程真的没了要等 Qt 把 finished 送回来（onGameFinished 那边会重画），但"正在启动"
    // 这一格该现在收：用户刚看见的那句话已经不对了。
    m_launchingSessions.remove(sessionId);
    m_host.updateBottomBar();
}

void GameSessionController::seedSessionFacts(int sessionId, const QString &versionId,
                                             const QString &displayName, int requiredJavaMajor,
                                             const QString &javaPath, int xmxMb, int xmsMb,
                                             const QString &loaderName,
                                             const QString &loaderVersion) {
    // 见 MainWindow 里那个同名入口的说明：产品这一侧的唯一写点在 LaunchController 里，
    // 而它要真起一个游戏进程 —— 验证工程摆不出真进程，所以这一格直接摆。
    GameSessionFacts facts;
    facts.instanceId = versionId;
    facts.instanceName = displayName;
    facts.requiredJavaMajor = requiredJavaMajor;
    facts.javaPath = javaPath;
    facts.xmxMb = xmxMb;
    facts.xmsMb = xmsMb;
    facts.loaderName = loaderName;
    facts.loaderVersion = loaderVersion;
    m_sessionLaunchFacts.insert(sessionId, facts);
}
