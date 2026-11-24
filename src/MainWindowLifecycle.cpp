/**
 * @file MainWindowLifecycle.cpp
 * @brief 窗口的生命周期：析构里那几趟必须等完的后台活、外框的原生消息、关窗那一刻。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这三件事回答的是同一个问题："这个窗口要没了（或者正要没）的时候，还有什么不能半路
 * 丢下"。`~MainWindow()` 里那四趟线程（体检 / 备份 / 图标反查 / 检查更新）一条都不能
 * 少等 —— 少等就是 Qt 的 `QThread: Destroyed while thread is still running` 直接 abort；
 * `closeEvent()` 里是"还有东西在跑，真的要走吗"；`nativeEvent()` 是外框那两条消息。
 * 放在一个文件里，加一趟线程时那张单子（"这一层起的，析构里必须一个一个等过来"）就在
 * 眼前。
 *
 * 【判据不在这一层】
 *
 * 怎么等、等多久收在各控制器自己的 `shutdown()` / `waitForLookup()` 里；外框那两条
 * 消息怎么回在 `ui/WindowChrome`；"要不要问一句"的问句走 `InlineMessage::ask()`。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "server/ServerProcess.hpp"
#include "settings/LauncherSettings.hpp"
#include "server/ServerRegistry.hpp"
#include "..\ui\WindowChrome.hpp"
#include "controllers/AgentController.hpp"
#include "controllers/AgentLauncherHostImpl.hpp"
#include "controllers/BackupController.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/InstanceIconController.hpp"
#include "controllers/InstanceUpdateCheckController.hpp"
#include "controllers/IntegrityController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "controllers/ToolboxController.hpp"
#include "launcher/LauncherManager.hpp"

#include <QCloseEvent>

MainWindow::~MainWindow() {
    m_launchHistory->finishAllPlayTimes();
    if (m_agentController) m_agentController->shutdown();
    if (m_downloadBatches) m_downloadBatches->shutdown();
    // 日志窗**没有父窗口**（理由见 GameSessionController::ensureGameLogWindow() 那段），
    // 所以 Qt 不会顺手把它们带走 —— 这里自己收，否则关掉启动器之后屏幕上会留着几扇无主的
    // 日志窗。那一份表连同收法都在那一位控制器里（它建的那些窗，它自己收）。
    m_gameSessionController->closeAllGameLogWindows();
    // 体检那一趟可能还在跑（几千次 stat，慢盘上要好几秒）。**先把收手旗子举起来再等它**
    // —— worker 每比几条看一眼那面旗，看到就退（见 submitIntegrityCheck 里那段）。
    //
    // NOTE: 不等的话是**崩**，不是"悄悄漏一个线程"：m_integrityThread 的父对象是 this（下面
    // 这个对象要散了），而 Qt 对"QThread 还在跑、父对象却析构了"是直接 abort
    // （`QThread: Destroyed while thread is still running`）。
    m_integrityController->shutdown();
    // 工具箱里"算哈希"那一条也是 `QThread::create()` 出来的（父对象是 controller 自己，
    // 同一个坑，同一个收法）。
    m_toolboxController->shutdown();
    // 备份 / 还原那一趟同理。**这一趟没法中途收手**（见 .hpp 里 m_backupAbort 那段
    // —— 停下来只会留一个半截的包或者删了一半的目录），所以这里只能**等它跑完**：
    // 一个几百 MB 的实例快照要几十秒，关窗会晚这么多才真的关上。
    //
    // NOTE: 这一位真正的用处是"跑完之后别把结果排回一个正在拆的窗口" —— 引擎不读它。
    // NOTE: 不等的话同上：`m_backupThread` 的父对象是 this，Qt 直接 abort。
    m_backupController->shutdown();
    // 剩下两趟后台活。它们和上面那两趟是**同一个坑**：
    // `QThread::create()` 出来的这几个对象，父对象都是 this（下面这个对象要散了），而 Qt
    // 对"线程还在跑、父对象却析构了"是直接 abort。这两趟又都是**网络活** ——
    // · 图标反查：打开实例设置（或文件管理那几页）就起，几个 sha1 + 一发反查；
    // · 检查更新：点「检查更新」就起。
    // 关窗那一刻正卡在等响应上是最平常的一件事（`HttpClient` 那一条要等 15 秒）——
    // 不在这里等，那十几秒里关启动器就是崩一下。
    //
    // 这一段的规矩**照着这张单子来**：**这一层（或者这一层的子控制器）**起了的线程，
    // 析构里必须**一个一个等过来**。眼下是四条（体检 / 备份 / 图标反查 / 检查更新）——
    // 再加一趟就得多一条，加的时候连同这句一起改。四条都把"等"收进了各自的
    // 控制器（`shutdown()` / `waitForLookup()`），这里在 UI 析构前统一调用。
    // （`ServerPing::Pinger` 那八条不在这张单子上：它们的父对象是 Pinger，
    // 而 `~Pinger()` 自己就 abort+wait，见那边。）
    //
    // NOTE: 这两趟**没有收手旗子**（`InstanceIconLookup::lookup()` 和检查更新那一趟里头没有
    // "看着旗子退出来"的口子），所以这里只能**等它跑完** —— 卡在超时上的话，关窗就会
    // 晚那十几秒才真的关上。那比崩一下强（同一个取舍，见上面备份那一段）。
    // NOTE: 图标那一趟起它的线程父对象是**那个控制器**（不是这一层）——
    // 和"父对象是 this"是同一个坑（控制器也是 this 的子对象，一样要散），所以这一
    // 条照样得留：等的活儿在控制器里，等这一下得在这一层发。
    m_instanceIconController->waitForLookup();
    m_instanceUpdateCheck->waitForLookup();
    delete m_ui;
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    // 外框那两条消息（WM_NCCALCSIZE / WM_NCHITTEST）转给 ui/WindowChrome —— 那一份的
    // 文件头把"为什么走这条路、为什么不走 Qt 那条 ExpandedClientAreaHint"写全了。
    //
    // 时机上多说一句：Qt 是在**处理这两条消息之前**把消息交到这里来的
    // （qwindowscontext.cpp 那句 `filterNativeEvent(platformWindow->window(), &msg, result)`
    // 排在它自己那个大 switch 前面），所以这里回的"这一条到此为止"是算数的 —— Qt 那套
    // 非客户区处理和我们自己的外框不会打架。
    WindowChrome *chrome = m_ui != nullptr ? m_ui->chrome() : nullptr;
    if (chrome != nullptr && chrome->handleNativeEvent(message, result)) {
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // 窗口大小先落盘（节流那一发可能还没到点）；关窗被取消也无妨，值本来就该是这个。
    if (m_windowSizeSaveTimer != nullptr) m_windowSizeSaveTimer->stop();
    m_settings->setMainWindowSize(MainWindowUI::rememberedWindowSize(this));
    // 合雾播完回来的这一趟：前面的确认、交出进程都已经做过，直接关。
    if (m_closeFog == WindowMotion::CloseFog::Done) { QMainWindow::closeEvent(event); return; }
    if (m_presetController && !m_presetController->leaveEditor()) { event->ignore(); return; }
    const QList<LauncherManager::GameSession> games = m_launcherManager->runningGames();
    // 还在跑的那些服务端（键 = serverId，**按名字排序** —— 提示里报出来的名字每次都得是
    // 同一个顺序，不然同一件事两句话看着像两件事）。
    const QStringList serverIds = m_serverWorkspace->runningServerIds();

    if (m_launchController->isStarting()) {
        const int choice = InlineMessage::ask(
            m_ui->overlay(), MessageKind::Question, tr("正在启动"),
            tr("正在校验登录并准备 Java。退出启动器将取消本次启动。\n\n确认退出？"),
            QStringList{tr("取消启动并关闭"), tr("继续启动")}, 1, 0);
        if (choice < 0) {
            // `ask()` 回 -1 不是"用户选了继续启动"，而是**这一问根本没被摆出来**
            // （已经有一条提示开着；提示不叠提示，见 InlineMessage::ask）。那时候底下什么
            // 反馈都没有 —— 用户点关闭、窗口不动、也没有一句解释，只能再点一次。照旧不关
            // （启动还在半路），但把原因说出来；那条提示答完以后再点关闭会照常问。
            appendLog(tr("关闭被搁置：还有一条提示没答完，答完再关一次。"));
            event->ignore();
            return;
        }
        if (choice != 0) {
            event->ignore();
            return;
        }
        m_launchController->abortStart(LaunchController::StartCancelSource::LauncherClosing);
    }

    if (games.isEmpty() && serverIds.isEmpty()) {
        if (WindowMotion::interceptClose(this, event, m_closeFog)) return;
        QMainWindow::closeEvent(event);
        return;
    }

    if (!games.isEmpty()) {
        // 游戏还在跑。**什么都不做**的实际后果是**把游戏强杀**：
        // MainWindow 是 main() 里的栈对象，窗口关掉 → exec() 返回 → MainWindow 析构 →
        // LauncherManager（它的子对象）析构 → m_gameProcess（它的子对象）析构，而
        // QProcess 的析构是这么写的（Qt 6.10，qprocess.cpp）：
        //
        // if (d->processState != NotRunning) { qWarning() << ...; kill(); waitForFinished(); }
        //
        // 也就是说，用户只是想关掉启动器，游戏却收到一个 TerminateProcess（Windows 上
        // kill() 就是它）—— 游戏来不及存档、没有"保存并退出"。PCL 那种启动器关掉之后游戏
        // 照跑，java.exe 本来也活得过启动器，这一步只是不让上面那条析构链碰到它。
        //
        // 所以这里问一句，而且**默认那一边是"别动游戏"**（回车 = 取消，不关窗口）：用户要
        // 是真想停游戏，在游戏里按"退出游戏"那条路才是干净的。
        // 同时开着好几份时，真名报出来（"还有 3 个游戏在跑：A、B、C"）比"游戏还在运行"
        // 有用得多 —— 用户要判断的正是"我还开着哪些"。
        QStringList names;
        names.reserve(games.size());
        for (const LauncherManager::GameSession &game : games) {
            names.append(instanceDisplayName(game.versionId));
        }
        const QString text =
            games.size() == 1
                ? tr("「%1」仍在运行。关闭启动器后，游戏、存档和联机会继续运行。\n\n确认关闭启动器？")
                      .arg(names.first())
                : tr("仍有 %1 个游戏在运行：%2。\n\n关闭启动器后，这些游戏会继续运行。确认关闭启动器？")
                      .arg(games.size())
                      .arg(names.join(QStringLiteral("、")));
        // 按钮文字自己写：启动器没装 Qt 的中文翻译，标准按钮在界面上是英文的 Yes/No。
        // NOTE: 关掉窗口这一发**本身**在跑 closeEvent，不能开独立窗（界面上只有内容区
        // 那一块覆盖层）—— 内嵌提示在 closeEvent 里同样作数：窗口这时候
        // 还没关掉，`execPrompt` 那个嵌套事件循环照转，"事件循环里再开事件循环"
        // 这件事和阻塞弹窗一样。
        // 默认那一边照旧是**取消**（下标 1），回车 = 别动游戏。
        // 第三格是 destructiveIndex —— 两个都不是危险动作（关的是启动器，游戏照跑），
        // 所以给 -1。
        const int choice = InlineMessage::ask(
            m_ui->overlay(), MessageKind::Question, tr("游戏正在运行"), text,
            QStringList{tr("关掉启动器"), tr("取消")}, 1, -1);
        if (choice != 0) {
            event->ignore();
            return;
        }
    }

    if (!serverIds.isEmpty()) {
        // 服务端和游戏**不是一回事**，所以单独问一次（问一句再决定
        // ——「停止服务端」/「保持后台运行」）。
        //
        // 为什么值得单独问：服务端不是"关掉启动器它也一样活着"的那种进程 —— 它是我们的子
        // 进程，启动器一退，那条析构链就会把它 TerminateProcess 掉，**正在写的世界文件
        // 就烂在那儿**。而"停干净"和"留着它继续跑"都是合理的诉求：前者是"我这台机器上
        // 不想再开服了"，后者是"我还想让它继续开着，只是把启动器关了"。
        QStringList names;
        names.reserve(serverIds.size());
        for (const QString &id : serverIds) {
            const ServerRegistry::Entry entry = ServerRegistry::describe(id);
            names.append(entry.name.isEmpty() ? id : entry.name);
        }
        const QString text =
            serverIds.size() == 1
                ? tr("服务端「%1」仍在运行。\n\n可选择保存并停止服务端，或关闭启动器后让它在后台继续运行。")
                      .arg(names.first())
                : tr("仍有 %1 个服务端在运行：%2。\n\n可选择保存并停止服务端，或让它们在后台继续运行。")
                      .arg(serverIds.size())
                      .arg(names.join(QStringLiteral("、")));
        // 默认那一边是**保持后台运行**（下标 1）：和游戏那一条同理 —— 不主动动别人的
        // 进程更安全（突然停下来那一发万一赶在写存档的中间，就是丢存档的姿势）。
        const int choice = InlineMessage::ask(
            m_ui->overlay(), MessageKind::Question, tr("服务端正在运行"), text,
            QStringList{tr("停止服务端"), tr("保持后台运行")}, 1, -1);
        if (choice == 0) {
            // 停要**等它走完**，而且等的是宽限那两个数算出来的时间（`ServerRun::
            // gracefulStopMs()` —— 模组越多、存档越大，给的越宽）。这一句是阻塞的：
            // 界面在这几秒里不动。选择让它静态地等，是因为另一种做法（发完 stop 就让
            // 析构去杀）等于是**把这一问白问了** —— 那正是丢存档的那条路。
            // R14：等完要看结果。`stopAndWait()` 回 false 的意思是**没能按正常那条路停下来**
            // （stop 那一句写不进去，或者宽限期过了进程还在 —— 两种都走了强杀，强杀也不一定
            // 成功）。无条件说"已经停下来"等于替用户保管了一句不成立的承诺，而那句话背后
            // 是"存档写完了没有"。
            QStringList notStopped;
            for (int index = 0; index < serverIds.size(); ++index) {
                if (!m_serverRun->stopAndWait(serverIds.at(index))) {
                    notStopped.append(names.at(index));
                }
            }
            if (notStopped.isEmpty()) {
                appendLog(tr("已关掉启动器，服务端已经停下来。"));
            } else {
                appendLog(tr("启动器已关闭，但服务端 %1 未正常停止。最近的存档修改可能未保存。")
                              .arg(notStopped.join(QStringLiteral("、"))));
            }
        } else {
            // 交出去：脱离所有权（`ServerProcess::detach()` 里那段说明），启动器退了它们
            // 照样在系统里跑。
            for (const QString &id : serverIds) {
                m_serverWorkspace->detach(id);
            }
            appendLog(tr("已关掉启动器，服务端继续在跑。"));
        }
    }

    if (!games.isEmpty()) {
        // 顺序要紧：**先**把进程交出去（脱离所有权 + 断信号），再让窗口关掉 —— 关窗口之后
        // 紧接着就是析构链，交出去那一步必须在那之前做完（见 LauncherManager::detachRunningGame）。
        m_launchHistory->finishAllPlayTimes();
        m_launcherManager->detachRunningGame();
        appendLog(tr("已关掉启动器，游戏继续在跑。"));
    }
    if (WindowMotion::interceptClose(this, event, m_closeFog)) return;
    QMainWindow::closeEvent(event);
}
