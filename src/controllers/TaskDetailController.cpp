#include "controllers/TaskDetailController.hpp"

#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "TaskCenterPanel.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/GameSessionController.hpp"
#include "GameLogWindow.hpp"
#include "diag/GameExitDiagnosis.hpp"
#include "manage/VersionManager.hpp"
#include "server/ServerRegistry.hpp"
#include "tasks/TaskManager.hpp"
#include "util/AppPaths.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTimer>
#include <QUrl>

#include <utility>

TaskDetailController::TaskDetailController(ActivityCenterController *activity,
                                           CrashReportController *crash, VersionManager *versions,
                                           GameSessionController *sessions, OverlayHost *overlay,
                                           QWidget *pageParent, Host host, QObject *parent)
    : QObject(parent), m_activity(activity), m_crash(crash), m_versions(versions),
      m_sessions(sessions), m_overlay(overlay), m_pageParent(pageParent), m_host(std::move(host)) {
    // 这一页开着的时候账本**照样在动**（下载会跑完 / 失败、启动那几步会往前走、
    // 用户在别处把这一批取消了），而页上那一份 `TaskCenter::Item` 是点开那一刻的快照。
    // 只在"在这一页上按了某颗按钮"时才重摆的话，一个正在下载的任务在页上会永远停在
    // 40%，底下的调用早就收尾了；「取消」那颗按钮也还亮着（点了才发现已经没什么可取消）。
    // 这些信号照常收得到（开页不再是嵌套事件循环，主事件循环把它们照常转过来），
    // 重摆就会当场出现在屏幕上。
    //
    // NOTE: 只接账本这两条（`taskChanged` 每一发都来，所以走合并版）。页没开着时什么都不
    // 做（见 refreshOpenPage 的第一行），不会为了没人看的页一遍遍造控件。
    connect(activity->tasks(), &Tasks::TaskManager::taskChanged, this,
            [this](Tasks::TaskId) { scheduleRefreshOpenPage(); });
    connect(activity->tasks(), &Tasks::TaskManager::historyChanged, this,
            [this] { scheduleRefreshOpenPage(); });
}

void TaskDetailController::log(const QString &message) const {
    if (m_host.log) m_host.log(message);
}

bool TaskDetailController::fillItem(const QString &key, TaskCenter::Item *out) const {
    const QSet<Tasks::TaskId> crashRecordIds = m_crash->recordIds();
    const bool retryPayloadAvailable =
        m_versions != nullptr && m_versions->canRetryDownload();
    return m_activity->itemForKey(
        key, out,
        [this](const Tasks::TaskSnapshot &task) {
            return m_host.cancelWouldWork ? m_host.cancelWouldWork(task) : false;
        },
        retryPayloadAvailable, crashRecordIds);
}

void TaskDetailController::open(const QString &key) {
    if (m_overlay == nullptr) {
        return;
    }
    if (m_overlay->isOpen()) {
        // 已经有覆盖层开着（另一页、或者一条提示）。这一层不做页面栈 —— 和 `openPage()`
        // 返回 nullptr 是同一件事（见 OverlayHost.hpp 那段）。**这里不排下一轮**：点行的
        // 那一下人是看着面板点的，晚一轮再开等于"面板收起来了，页没开"。
        return;
    }
    if (m_page == nullptr) {
        m_page = new TaskDetailPage(m_pageParent);
        connect(m_page, &TaskDetailPage::actionRequested, this,
                [this](const QString &key, TaskDetailPage::Action action) { this->action(key, action); });
    }
    // 摆这一项要画的东西（先摆内容、再摆页：页一露出来就该是这一项的样子）。
    TaskCenter::Item item;
    if (!fillItem(key, &item)) {
        // 号早就没了（用户按过「清除已完成」，或者这一项是上一轮读盘读回来的历史）——
        // 页照开，摆一份空的就是了（页上会写"（没有名字的任务）"）。**不弹窗**：用户没做错
        // 什么，而且这一页本来就有个"我在看某一项"的位置。
        item.key = key;
    }
    m_page->setTask(item);
    // 归哪一颗选项卡：面板是从底栏中间那格弹出来的，而那一格**不属于任何一颗选项卡**
    // （底栏是 tabWidget 的兄弟）—— 所以 `ownerTab` 给 -1 = 开页那一刻的那一颗（见
    // OverlayHost 里 ownerTab 那一段；用户切走时这一页照样"收着"）。
    //
    // 这里用**非阻塞**的 `showPage()`：这一页自己不等任何结果，收页（返回箭头 /
    // 页上按 Esc / 别处要开新的一页）全由覆盖层自己收干净。以前走 `execPage()`，点一下
    // 任务那一行就卡在嵌套事件循环里，页开着的时候"下面"的每一句都要按那个循环分清楚。
    if (!m_overlay->showPage(m_page, TaskDetailPage::windowTitle())) {
        // 摆在门口那一下被抢了（另一页刚占上这一层）：什么都没发生，留一句日志
        // —— 用户点的这一下不该由他再点第二遍来找原因。
        log(tr("无法打开任务详情页：界面已被另一页占用。"));
    }
}

void TaskDetailController::action(const QString &key, TaskDetailPage::Action action) {
    switch (action) {
    case TaskDetailPage::Action::Pause: {
        TaskCenter::Item fresh;
        if (fillItem(key, &fresh) && fresh.capabilities.pause && m_host.pauseTask) m_host.pauseTask(key);
        break;
    }
    case TaskDetailPage::Action::RepairImport: {
        TaskCenter::Item fresh;
        if (fillItem(key, &fresh) && fresh.capabilities.repairImport && m_host.repairImport) {
            const auto task = m_activity->tasks()->snapshot(key.toULongLong());
            m_host.repairImport(task.targetId);
        }
        break;
    }
    case TaskDetailPage::Action::Recover: {
        TaskCenter::Item fresh;
        if (fillItem(key, &fresh) && fresh.capabilities.recover && m_host.recoverOperation) {
            m_host.recoverOperation(key);
        }
        break;
    }
    case TaskDetailPage::Action::Cancel:
        // 取消要**按这一项是谁**分派：服务端安装那一趟和版本/资源下载那一批不是同一份
        // 状态（一个自己跑 QProcess + 自己的元数据请求，一个走 `VersionManager` 的批次）。
        // 两边都无脑叫一遍的话，取消一次服务端安装会顺手把别人正在跑的下载掐掉 ——
        // 而下载那一批可能是用户在下载页上排的、跟这一项毫无关系。
        // **那一条分派留在主窗口**（它和 `cancelWouldWork()` 必须逐字对齐，见文件头）。
        if (m_host.cancelTask) m_host.cancelTask(key);
        break;
    case TaskDetailPage::Action::Retry:
        // 只认**这一项自己**那一批（`retryWouldWork()` 就是这条判据的唯一出处）：界面那边
        // 已经把不合条件的那颗按钮收起来了，这里是第二道闸 —— 页对象是**建一次留着**的，
        // 用户完全可能在页开着的时候又失败了一批，而按钮画的还是上一帧那份。
        // 同「取消」：那一条判据留在主窗口。
        if (m_host.retryTask) m_host.retryTask(key);
        break;
    case TaskDetailPage::Action::OpenLog:
        openLog(key);
        break;
    case TaskDetailPage::Action::OpenTarget:
        openTarget(key);
        break;
    case TaskDetailPage::Action::CopySummary:
        copySummary(key);
        break;
    case TaskDetailPage::Action::ExportReport:
        m_crash->exportReport(key);
        break;
    case TaskDetailPage::Action::OpenCrashDir:
        m_crash->openReportsDirectory();
        break;
    }
    // 取消 / 重试都会让账本立刻动起来（终态信号是同步发的），这一页上那几行跟着重摆一遍
    // —— 不重摆的话用户看到的还是"正在进行"，而底下那一批已经收尾了。
    if (m_page != nullptr) {
        TaskCenter::Item fresh;
        if (fillItem(key, &fresh)) {
            m_page->setTask(fresh);
        }
    }
}

void TaskDetailController::openLog(const QString &key) {
    // 这里打开的是**启动器自己那次启动的日志**（`<数据目录>/logs/wtfml.log`），不是某一场
    // 游戏的日志窗。
    //
    // 理由：任务中心记的是**启动器干的活**（下载 / 安装 / 导入 / 启动这一段调度），那些话
    // 全在这份日志里；而 `GameLogWindow` 装的是**游戏进程的输出**，它是按会话号开的、
    // 会话结束就没了（见 GameSessionController 里那一份表）。拿一次"下载失败"去开游戏日志，
    // 开出来的多半是空的、或者上一次玩的那一场 —— 那比不开更让人糊涂。
    //
    // 「游戏异常退出」那一项另外给「打开日志」一个**明确的**去处：它就是下面这一段。
    bool parsed = false;
    // 行钥匙是 64 位的 `TaskId`（见 `ActivityCenterController::markViewed()` 那段的 NOTE）。
    const Tasks::TaskId id = key.toULongLong(&parsed);
    const int crashSessionId = parsed ? m_crash->sessionIdForTask(id) : 0;
    if (crashSessionId > 0) {
        // 这一项知道自己是**哪一场**。异常退出时日志窗不会自动关（见
        // `GameLogWindow::autoCloseOnExit()`），所以它多半还在 —— 抬到前面来，而不是去开
        // 启动器自己那份日志（那里面只有调度那几句，看不到游戏说了什么）。
        if (GameLogWindow *window = m_sessions->gameLogWindow(crashSessionId)) {
            window->show();
            window->raise();
            window->activateWindow();
            return;
        }
        InlineMessage::warning(
            m_overlay, tr("日志窗口已关闭"),
            tr("游戏日志窗口已关闭。可点击「导出错误报告」保存末尾错误日志（错误最多 %1 行，完整日志最多 %2 行）。")
                .arg(GameExitDiagnosis::kExcerptLineLimit)
                .arg(GameExitDiagnosis::kLogTailLineLimit));
        return;
    }
    const QString path = AppPaths::minecraftDir() + QStringLiteral("/logs/wtfml.log");
    if (!QFile::exists(path)) {
        InlineMessage::warning(m_overlay, tr("暂无任务日志"), tr("这一次启动到现在没有写过日志。"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void TaskDetailController::openTarget(const QString &key) {
    bool ok = false;
    const Tasks::TaskId id = key.toULongLong(&ok);
    if (!ok || !m_activity->tasks()->has(id)) {
        return;
    }
    const Tasks::TaskSnapshot task = m_activity->tasks()->snapshot(id);
    // 目标就是那一项自己记的那个 id，空的话落到数据目录 —— 一样"点得开"，不给一颗点了
    // 没反应的按钮。
    //
    // 服务端启动明确记的是服务端 id，两类目标可能同名。安装与备份仍兼容旧记录的查找方式。
    QString dir;
    if (task.kind == Tasks::TaskKind::ServerLaunch) {
        dir = AppPaths::serverDir(task.targetId);
    } else if (task.kind == Tasks::TaskKind::Download && QDir::isAbsolutePath(task.targetId)) {
        dir = task.targetId;
    } else if (task.targetId.isEmpty()) {
        dir = AppPaths::minecraftDir();
    } else {
        dir = AppPaths::versionDir(task.targetId);
        if (!QDir(dir).exists() && ServerRegistry::serverExists(task.targetId)) {
            dir = ServerRegistry::describe(task.targetId).dirPath;
        }
    }
    if (!QDir().exists(dir)) {
        InlineMessage::warning(m_overlay, tr("无法打开目标位置"),
                               tr("这一项说的是 %1，而那个目录现在不在。")
                                   .arg(QDir::toNativeSeparators(dir)));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void TaskDetailController::copySummary(const QString &key) {
    TaskCenter::Item item;
    if (!fillItem(key, &item)) {
        return;
    }    // 摘要 = 这一页上那几行字，纯文本（贴到哪儿都读得下去）。**不带上整份日志**：
    QStringList lines;
    lines << QStringLiteral("%1: %2").arg(MainWindowUI::taskDetailStateText(item.state),
                                          item.title);
    if (item.startedAt.isValid() || item.finishedAt.isValid()) {
        lines << MainWindowUI::taskDetailTimeLineText(item.startedAt, item.finishedAt);
    }
    if (!item.detail.isEmpty()) {
        lines << item.detail;
    }
    if (!item.bytesText.isEmpty()) {
        lines << item.bytesText;
    }
    for (const TaskCenter::Step &raw : item.steps) {
        // 和面板/详情页同一套收尾归一（终态任务里不出现"仍在跑"的步骤）。
        const TaskCenter::Step step = TaskCenter::stepForTask(item.state, raw);
        lines << TaskCenter::stepMark(step.state) + QLatin1Char(' ') + step.name
                     + (step.note.isEmpty() ? QString() : QStringLiteral(" / ") + step.note);
    }
    if (!item.errorSummary.isEmpty()) {
        lines << QString() << item.errorSummary;
    }
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    log(tr("这一项的摘要已经复制到剪贴板。"));
}

void TaskDetailController::refreshOpenPage() {
    if (m_page == nullptr || !m_page->isVisible()) {
        return;
    }
    const QString key = m_page->taskKey();
    if (key.isEmpty()) {
        return;
    }
    TaskCenter::Item fresh;
    if (fillItem(key, &fresh)) {
        m_page->setTask(fresh);
        return;
    }
    // 号已经不在账本上了（「清除已完成」把行删掉了，或者这一页摆的是上一轮读盘的历史）。
    // **不弹窗、不自动关页**：页上那份快照仍然是"它当时是什么"，用户没做错什么 ——
    // 和 `open()` 里同一条理由。
}

void TaskDetailController::scheduleRefreshOpenPage() {
    if (m_page == nullptr || !m_page->isVisible() || m_refreshPending) {
        return;
    }
    m_refreshPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshPending = false;
        refreshOpenPage();
    });
}

