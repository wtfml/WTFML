/**
 * @file MainWindowBottomBar.cpp
 * @brief 底栏那一行的投影：同步等待、全局忙态、「正在跑」那三色方块、三根进度条。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"底栏这会儿该露什么"：`enterSyncWait()` / `leaveSyncWait()`（点启动那一趟
 * 里 UI 线程在等网络或等进程时，让窗口照常重绘、不当场挂成"(无响应)"）、
 * `setDownloadUiBusy()`（全局忙态，挡住会打架的操作）、`updateBottomBar()` /
 * `updateBottomBarRunState()`（那一行与左下角那颗方块）、`updateDownloadProgressUi()` /
 * `onVersionDownloadStats()`（三根进度条）。分出来之后，"底栏为什么是这种颜色 / 这句话"
 * 只在一个文件里翻。
 *
 * 【唯一出处】
 *
 * 左下角那颗方块的三态与"正在启动优先"的规矩在 `RunStateMark`；任务那一段的摘要只有
 * `Tasks::collapsedSummary()`；取色只用底栏的调色板。
 *
 * 【判据不在这一层】
 *
 * 会话与下载的事实来自 `LauncherManager` / `GameSessionController` /
 * `DownloadTaskLedgerController`；这一层只把它们摆成底栏的样子。
 */

#include "MainWindow.hpp"

#include "..\ui\Animations.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\SkinModel.hpp"
#include "auth/AuthManager.hpp"
#include "util/HomeGreeting.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/InstallPageController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "launcher/LauncherManager.hpp"
#include "server/ServerProcess.hpp"
#include "server/ServerRegistry.hpp"
#include "tasks/TaskSnapshot.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPixmap>
#include <QWidget>

using namespace MainWindowViewData;

void MainWindow::enterSyncWait(const QString &statusText) {
    // 这一段里 UI 线程在等网络或等进程（见 .hpp 里那三种情况），Qt 不重绘、不响应输入，
    // Windows 过几秒还会给标题挂上"(无响应)"——用户看到的就是"点了启动就卡死"。
    // 这里能做的只有"进去之前先把话说出来"：底栏那格写上在等什么，再套一个忙指针。
    m_syncWaitStack.append(m_syncWaitStatus);
    m_syncWaitStatus = statusText;
    updateBottomBar();
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    // 不刷这一次，上面那些改动要等窗口重新绘制时才看得见 —— 而那要等到这段路走完。
    // 排掉用户输入是**必须的**：不排的话这次 processEvents() 会把用户在这期间的点击
    // （比如"再点一次启动"）送回来，于是同一段路走两遍，比卡着更糟。
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::leaveSyncWait() {
    // 多出来的 leave 直接忽略：那样说明调用方出了错，但这里能做的只有别把忙指针再还一次
    // （restoreOverrideCursor() 多还一次会把**别人的**忙指针弹掉）。
    if (m_syncWaitStack.isEmpty()) {
        return;
    }
    m_syncWaitStatus = m_syncWaitStack.takeLast();
    QGuiApplication::restoreOverrideCursor();
    updateBottomBar();
}

void MainWindow::setDownloadUiBusy(bool busy) {
    m_downloadInFlight = busy;
    // ---------- 这几项**故意不灰** ----------
    // 启动按钮、「已安装版本」下拉框、实例页那张树不能跟着 busy 一起灰：理由是
    // **"在下载"和"能启动什么"是两件事**，而且现在按版本分开了 ——
    // 正在装的那个从 launchable 里滤掉（见 installingVersionIds()），列表里剩下的
    // 都是文件齐的。所以"界面显示选的 A、实际在下的却是 B"这种误判在做完过滤之后
    // 不可能发生，而"装 1.20.1 的时候开一个早就装好的 1.18.2"是用户明确要的。
    //
    // 真正危险的那一种由 onStartButtonClicked() 里那道**按版本**的闸口兜住。
    //
    // 安装页那一整层（名称框、四个加载器、安装按钮、返回）**照样灰**：在装什么是点击
    // 那一刻定下的，这时候改名字/换加载器并不会把手上这次安装换掉（真正干活的
    // VersionManager 已经拿着那个枚举在跑了），留着能改只会让人以为改生效了。
    m_ui->getInstallButton()->setEnabled(!busy);
    m_ui->getInstallNameEdit()->setEnabled(!busy);
    m_ui->getVersionTree()->setEnabled(!busy);
    // 从安装页上退回去也不给（退回去那棵树也是灰的）：那会让人以为"这次安装取消了"。
    m_ui->getInstallBackButton()->setEnabled(!busy);
    // 「导入整合包…」也一起灰：导入期间再点一次，选文件和问名字那两个框走完，
    // 数据层才会用一句"已有任务正在进行"把它顶回来 —— 白走一趟对话框。
    m_ui->getImportModpackButton()->setEnabled(!busy);
    // 下载页「整合包」那一页上那颗是**同一件事的另一个入口**（同一个槽，见
    // setupConnections 里那条），所以它得跟着一样灰 —— 一个灰着、另一个亮着，用户点的
    // 正好是亮着那颗，走的是同一条被顶回来的路。
    if (QPushButton *downloadImport = m_ui->getDownloadFilterImportModpackButton()) {
        downloadImport->setEnabled(!busy);
    }
    // 实例页左边那列也一起灰：换游戏文件夹 = 换一整套 .minecraft，下载中途换过去
    // 会让这一批文件一半写进 A、一半写进 B。列表本体与「添加」的可用性一律由
    // updateFolderButtons() 一处说了算（它同时看这一位和长流程举的牌子），这里不再
    // 单独 setEnabled —— 两处各判一次，漏掉的那一处就是上面那种"半批在里面半批在外面"。
    // 「移除」的灰/亮仍由齿轮菜单自己按行算（见 FolderMenu 的 protectedFolder）。
    //
    // 「模组加载器」那棵树一起灰，理由同上。忙完解禁走 m_installPage->updateLoaderRows()
    // 一处重算（那棵树的样子全由它一处决定）；「安装」那个按钮还要看名称合法不合法，
    // 走 m_installPage->updateInstallNameState() 一处。
    m_ui->getLoaderTree()->setEnabled(!busy);
    m_installPage->updateLoaderRows();
    m_installPage->updateInstallNameState();
    // 重灌一遍"能启动的实例"：上面那条过滤（在装的那个先不出现）要立刻生效 ——
    // 置忙那一刻它该从列表里消失，置闲那一刻它该回来（成功的话还在
    // `DownloadCompletionController::onFinished` 里被选中，那句也在这个函数之后跑）。
    refreshInstalledVersions();
    updateFolderButtons();

    // ---------- 底栏那一格那个数字：从这里起/到这里止只由账本说了算 ----------
    // 进度条和"已下载 d / t"那几样都在**任务自己身上**（`Tasks::TaskSnapshot`），
    // 这一格只是账本的一块投影 —— 要清的不是三个成员，而是"把这一批的那件任务收掉"。
    if (busy) {
        m_downloadTaskLedger->begin();
    } else {
        // 这一批到此为止（"为什么这是兜底、为什么不会把成功改写成失败"逐字在
        // `DownloadTaskLedgerController::finish()` 里）。
        m_downloadTaskLedger->finish(tr("这一批下载没有正常结束。"));
    }
    updateBottomBar();
}

void MainWindow::updateHomeGreeting() {
    // 名字和上面账号胶囊同一个判据：登录了（或是离线账号）才带名字。
    const bool signedIn = offlineIdentity() || m_authManager->loggedIn();
    const QString name = signedIn ? m_authManager->playerName().trimmed() : QString();
    m_ui->setHomeHeroTitle(HomeGreeting::text(QDateTime::currentDateTime(), m_greetingLastOpened,
                                              m_greetingHadInstances, name),
                           name);
}

void MainWindow::updateBottomBar() {
    // ---------- 左：头像 + 用户名 ----------
    // 登录过就是档案里的真名（登录那一刻拿到的，或者开窗时从盘上读回来的那份）；
    // 没登录是占位的 "Player" —— 这里不自己造假名字，也不去猜。
    const QString name = m_authManager->playerName();
    m_ui->getAccountLabel()->setText(name);
    // 名字下面那行：这次启动会用哪种身份。**判据只看一件事**（当前账号是不是离线账号），
    // 不自己推"应该用哪个" —— 真正的判据在 onStartButtonClicked() 里，两处一旦各写一套
    // 就会出现"底栏说正版、启动时却说请先登录"。现在两处读的是**同一个函数**
    // （offlineIdentity()），连"各写一套"的可能都没有了。
    // NOTE: loggedIn() 是"盘上有账户"的意思（AuthManager::loggedIn() = uuid 非空），
    // 令牌过期了它照样是 true —— 那没关系：这一行说的是**身份**，令牌过没过期是
    // 启动那一刻刷新才知道的事（那时失败会有弹窗说清原因）。
    const QString accountType =
        offlineIdentity()
            ? MainWindowUI::accountTypeOfflineText()
            : (m_authManager->loggedIn() ? MainWindowUI::accountTypeOnlineText()
                                         : MainWindowUI::accountTypeNotLoggedInText());
    m_ui->getAccountTypeLabel()->setText(accountType);
    // 主页右上那颗账号胶囊是账号的唯一可见入口；底栏那份标签只为兼容保留数据。
    // 没登录（也不是离线账号）：名字那里写「登录」、小状态收起、头像画占位（pixmap 留空）。
    const bool signedIn = offlineIdentity() || m_authManager->loggedIn();
    if (QLabel *homeName = m_ui->getHomeAccountLabel()) {
        homeName->setText(signedIn ? name : MainWindowUI::homeAccountSignInText());
    }
    if (QLabel *homeType = m_ui->getHomeAccountTypeLabel()) {
        homeType->setText(offlineIdentity() ? MainWindowUI::accountTypeOfflineText()
                                            : MainWindowUI::homeAccountMicrosoftText());
        homeType->setVisible(signedIn);
    }
    updateHomeGreeting();
    // 头像优先用**真皮肤**抠出来的那一块（m_avatarPixmap，在 applyPlayerSkin() 里算好
    // 存着 —— 这里每次进度刷新都会被调到，不能现算）。没有它的时候（未登录、还没取到、
    // 取不到、贴图载不进来）退回首字母方块：退路必须留着，一块空白的头像比一个首字母
    // 更像"界面坏了"。
    // NOTE: setPixmap()/setText() 都会把另一份内容清掉（QLabel 只显示最后设的那一个），
    // 所以这里不用自己清。
    if (m_avatarPixmap.isNull()) {
        m_ui->getAvatarLabel()->setText(name.isEmpty() ? QStringLiteral("?")
                                                       : name.left(1).toUpper());
    } else {
        m_ui->getAvatarLabel()->setPixmap(m_avatarPixmap);
    }
    if (QLabel *avatar = m_ui->getHomeAvatarLabel()) {
        if (!signedIn) {
            avatar->setPixmap(QPixmap());
        } else if (m_avatarPixmap.isNull()) {
            avatar->setPixmap(QPixmap::fromImage(
                SkinModel::headAvatar(SkinModel::defaultSteveSkin(), avatar->width())));
        } else {
            avatar->setPixmap(m_avatarPixmap.scaled(avatar->size(), Qt::KeepAspectRatio,
                                                    Qt::FastTransformation));
        }
    }

    // ---------- 中：任务摘要 ----------
    // 这一格的内容**不在这里算**：显示谁是 `Tasks::collapsedSummary()` 那条纯规则的事，
    // 画成什么样是 `MainWindowUI::setBottomBarTaskSummary()` 的事。这里只负责"状态变了，
    // 重画一遍"。
    //
    // NOTE: 这一格**不能**写成三支抢优先级（启动 / 下载 / 同步等待）。要的是
    // "启动不再抢走并覆盖下载"：那两件事现在是账本里并列的两行，收起态挑一行展示而已，
    // 没有"借走"这回事，也就没有"借完要照原样还回去"。同步等待是唯一还压在上面的一层，
    // 理由写在 updateBottomBarTaskSummary() 里。
    updateBottomBarTaskSummary();

    // ---------- 右：当前实例运行状态 ----------
    // 运行状态单独一个函数（和中间那格 `updateBottomBarTaskSummary()` 对称）。
    // 理由不是"函数太长"：红那一档现在看的是**账本**（账上还有没有没处理的异常退出），
    // 而账本每变一次都会来一发信号 —— 一发信号就重画整条底栏（连头像、账号名一起）是浪费，
    // 也把"这事儿归谁刷新"弄糊了。拆开之后：账本变 ⇒ 只叫这一格和中间那一格。
    updateBottomBarRunState();
}

QList<RunningProcesses::Entry> MainWindow::runningProcessEntries() const {
    const QList<LauncherManager::GameSession> games = m_launcherManager->runningGames();
    const QStringList runningServers =
        m_serverWorkspace != nullptr ? m_serverWorkspace->runningServerIds() : QStringList();
    QList<RunningProcesses::Entry> entries;
    entries.reserve(games.size() + runningServers.size());
    for (const LauncherManager::GameSession &game : games) {
        RunningProcesses::Entry entry;
        entry.kind = RunningProcesses::Entry::Kind::Client;
        entry.key = QString::number(game.sessionId);
        entry.startedAtMs = game.startedAtMs;
        entry.sessionId = game.sessionId;
        entry.versionId = game.versionId;
        entry.displayName = game.displayName.isEmpty() ? instanceDisplayName(game.versionId) : game.displayName;
        entry.accountName = game.accountName;
        entries.append(entry);
    }
    for (const QString &serverId : runningServers) {
        const ServerRegistry::Entry desc = ServerRegistry::describe(serverId);
        const ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
        RunningProcesses::Entry entry;
        entry.kind = RunningProcesses::Entry::Kind::Server;
        entry.key = RunningProcesses::makeServerKey(serverId);
        qint64 startedAtMs = m_serverRun != nullptr ? m_serverRun->startedAtMs(serverId) : 0;
        if (startedAtMs <= 0 && process != nullptr) {
            startedAtMs = process->startedAtMs();
        }
        entry.startedAtMs = startedAtMs;
        entry.serverId = serverId;
        entry.displayName = desc.name.isEmpty() ? serverId : desc.name;
        entry.serverFlavor = desc.flavor;
        entry.serverStopping =
            process != nullptr && process->status() == ServerModel::Status::Stopping;
        entry.serverForceKillAllowed = process != nullptr && process->forceKillAllowed();
        entries.append(entry);
    }
    RunningProcesses::sortNewestFirst(entries);
    return entries;
}

/// 底栏左下那一格（运行状态）整个重画一遍 —— 方块的颜色、第一行、第二行。
/// 只显示两种状态：「未运行」（灰）与「%1 个进程运行中」（绿，客户端与服务端合计）；
/// 启动与崩溃提示交由任务中心处理。
void MainWindow::updateBottomBarRunState() {
    // 与底栏使用同一份进程事实；队列刷新避免删除正在发 clicked 的行内按钮。
    m_ui->refreshTaskCenterPanel();
    QLabel *runStateLabel = m_ui->getRunStateLabel();
    QLabel *runStateDetailLabel = m_ui->getRunStateDetailLabel();
    QLabel *runStateMarkLabel = m_ui->getRunStateMarkLabel();
    const QList<RunningProcesses::Entry> entries = runningProcessEntries();
    // 主页启动区主按钮：当前实例在跑就写「停止游戏」（和这一格看的是同一份会话事实）。
    const QString currentId = selectedVersionId(m_ui->getInstalledVersionCombo());
    m_ui->setHomeLaunchRunning(!currentId.isEmpty() && m_launcherManager->isVersionRunning(currentId));

    const int totalCount = RunningProcesses::totalCount(entries);
    const bool anyRunning = totalCount > 0;
    const MainWindowUI::RunStateMark mark =
        anyRunning ? MainWindowUI::RunStateMark::Running : MainWindowUI::RunStateMark::Idle;
    runStateMarkLabel->setPixmap(
        MainWindowUI::runStateMarkPixmap(mark, m_ui->getBottomBar()->palette()));
    runStateMarkLabel->setVisible(true);

    // 左下那一格现在点不点得开 = 运行进程面板里有没有进程（客户端或服务端任一在跑即可展开）。
    m_ui->setRunPanelAvailable(anyRunning);
    // 图标轨里这一格只画图标和进程数；数和下面那句「%1 个进程运行中」是同一份。
    m_ui->setRunStateCount(totalCount);

    if (!anyRunning) {
        runStateLabel->setText(MainWindowUI::runStateNotRunningText());
        runStateLabel->setVisible(true);
        runStateDetailLabel->setText(QString());
        runStateDetailLabel->setVisible(false);
        return;
    }

    runStateLabel->setText(MainWindowUI::runStateRunningCountText(totalCount));
    runStateLabel->setVisible(true);

    if (totalCount == 1 && entries.first().kind == RunningProcesses::Entry::Kind::Client) {
        const RunningProcesses::Entry &only = entries.first();
        const bool showDetail = MainWindowUI::runStateShowsDetail(
            m_ui->isHomeTabCurrent(), currentId, only.versionId, false);
        runStateDetailLabel->setVisible(showDetail);
        runStateDetailLabel->setText(showDetail ? instanceDisplayName(only.versionId) : QString());
        return;
    }

    int clientCount = 0;
    for (const RunningProcesses::Entry &entry : entries) {
        if (entry.kind == RunningProcesses::Entry::Kind::Client) {
            ++clientCount;
        }
    }
    QStringList lines;
    lines.reserve(entries.size());
    for (const RunningProcesses::Entry &entry : entries) {
        if (entry.kind == RunningProcesses::Entry::Kind::Client) {
            const QString name = instanceDisplayName(entry.versionId);
            lines.append((clientCount > 1 && !entry.accountName.isEmpty())
                             ? MainWindowUI::runStateDetailWithAccount(name, entry.accountName)
                             : name);
        } else {
            lines.append(entry.displayName);
        }
    }
    runStateDetailLabel->setVisible(!lines.isEmpty());
    runStateDetailLabel->setText(lines.join(QStringLiteral("、")));
}

// ---------- 任务中心（底栏那格的投影 + 任务动作） ----------
// 在 `src/MainWindowTasks.cpp`（同一个类，另一个翻译单元）。


// ---------- 脸上那张图（外观拉取 / 落盘 / 铺到窗口上） ----------
// 在 `src/MainWindowSkin.cpp`（同一个类，另一个翻译单元）。


QString MainWindow::instanceDisplayName(const QString &versionId) const {
    // 用下拉框里那条文本，也就是用户看惯了的显示名（可能是"我的整合包-1.20.1-Forge"），
    // 而不是裸目录名。找不到（版本刚被删、或者压根不在本地）时退回版本号本身 ——
    // 底栏宁可显示一个用户没见过的目录名，也不能什么都不显示。
    const QComboBox *combo = m_ui->getInstalledVersionCombo();
    const int index = combo->findData(versionId);
    return index >= 0 ? combo->itemText(index) : versionId;
}

void MainWindow::updateDownloadProgressUi(int done, int total, int failed,
                                          const QString &currentFile) {
    // 这一批对上的那件任务（没有这一批 = 无效号）。**进度只往它身上写** —— 底栏那一格和
    // 任务中心面板都是从账本投影出来的，没有第二处要同步（账本是唯一真相）。
    const Tasks::TaskId task = m_downloadTaskLedger->task();

    // 标题下面那行小字。**没报过总数时只说"已下载多少"，不许编一个百分比出来**。
    // `total <= 0` 在这里是"这一批还没报过总数"（版本 json 还在路上、
    // 或者加载器安装器那类还算不出要下几个文件）——不是"这一批下了 0 个"。
    QString detail;
    if (total <= 0) {
        detail = done > 0 ? tr("已下载 %1 个文件").arg(done) : tr("正在准备下载…");
    } else {
        detail = tr("已下载 %1 / %2").arg(done).arg(total);
        if (failed > 0) {
            detail += tr("，失败 %1").arg(failed);
        }
        if (!currentFile.isEmpty()) {
            // 路径可能很长（asset 目录层级深），只显示文件名
            detail += QStringLiteral("  ") + QFileInfo(currentFile).fileName();
        }
    }

    if (task != Tasks::invalidTaskId() && m_activityCenter->tasks()->has(task)) {
        // 两样都记：数量那半边决定百分比（条子画多长），文字那半边是"现在在干什么"。
        // NOTE: `total <= 0` 原样传给账本（它是"不可测"的判据，见 Tasks::percentOf()）——
        // 上面那句文案里的 done/total 是**说给人听**的，不要把 0 也传进去。
        m_activityCenter->tasks()->setProgress(task, done > 0 ? done : 0, total > 0 ? total : 0);
        m_activityCenter->tasks()->setDetail(task, detail);
        // ⚠ 这一段里**不要**再自己写进度条和状态栏。它们由 `taskChanged` → 底栏重画那一发
        // 统一摆（`Anim::smoothProgress()` 也在那条路上）——两处各写一次，用户看到的是
        // 同一格在 240 ms 的追值动画里被第二只手按住，实际表现是"条子偶尔一顿"。
    }

    // 这里**不需要**"这一格现在不归下载就一个像素都不许动"那种闸：这一句只改
    // **任务自己的** detail，改不到底栏那一格的标题（那一格的标题由
    // `Tasks::collapsedSummary()` 现算，而"在同步等待"那一层压在它上面，见
    // updateBottomBarTaskSummary()）。"启动那几句话被 200 ms 一帧
    // 的下载顶掉"在结构上不可能发生。
}

void MainWindow::onVersionDownloadStats(int done, int total, int failed,
                                        const QString &currentFile) {
    // 前缘节流：200 ms 内的后续更新直接丢掉。不这么做的话，5000 多个 asset 会疯狂重排
    // 状态栏，界面反而卡住。
    // NOTE: 丢掉的那些**没有**"结束时补刷"这回事 —— 底栏那一格停在上一次越过阈值的
    // 那一帧，最多比真实进度旧 200 ms。整批结束由 setDownloadUiBusy(false) 把那一件
    // 任务收掉（收尾之后那一格换成"（已完成）"，停在哪一帧都看不出来了）。
    if (!m_downloadTaskLedger->takeProgressTick()) {
        return;
    }
    updateDownloadProgressUi(done, total, failed, currentFile);
}
