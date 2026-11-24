#include "ActivityCenterController.hpp"
#include <QTimer>
#include <algorithm>

#include "InstanceFilesPage.hpp"
#include "manage/InstanceFiles.hpp"
#include "util/AppPaths.hpp"
#include <QFileInfo>

namespace {

TaskCenter::StepState presentationStepState(Tasks::TaskStepState state) {
    switch (state) {
    case Tasks::TaskStepState::Pending:
        return TaskCenter::StepState::Pending;
    case Tasks::TaskStepState::Running:
        return TaskCenter::StepState::Running;
    case Tasks::TaskStepState::Done:
        return TaskCenter::StepState::Done;
    case Tasks::TaskStepState::Failed:
        return TaskCenter::StepState::Failed;
    }
    return TaskCenter::StepState::Pending;
}

}  // namespace

ActivityCenterController::ActivityCenterController(QObject *parent)
    : QObject(parent), m_tasks() {
    connect(&m_tasks, &Tasks::TaskManager::taskChanged, this,
            [this](Tasks::TaskId) { emit activityChanged(); });
    connect(&m_tasks, &Tasks::TaskManager::taskFinished, this,
            [this](Tasks::TaskId id, bool) {
                m_transferSpeeds.remove(id);
                m_queue.removeIf([id](const QueuedJob &job) { return job.id == id; });
                for (auto it = m_runningLanes.begin(); it != m_runningLanes.end();)
                    if (it.value() == id) it = m_runningLanes.erase(it); else ++it;
                scheduleDispatch(); emit activityChanged();
            });
    connect(&m_tasks, &Tasks::TaskManager::historyChanged, this, [this] {
        emit historyChanged();
        emit activityChanged();
    });
}

void ActivityCenterController::loadHistory() {
    m_transferSpeeds.clear();
    m_tasks.loadHistory();
    // 账本从盘上读回来了，"还没看过的失败"那一份却还停在空表上（它是内存
    // 里的，新进程一开始就是空的）。症状：上一次会话里失败、用户还没来得及点「知道了」
    // 的任务，重启之后**底栏那颗红点不见了** —— 而面板「需要处理」里照样列着它
    // （那一栏看的是落盘的 `attentionAcknowledged`）。两处说的不是一回事，用户就会漏掉
    // 那件真的失败过的事。
    //
    // 「需要处理」认详情确认，底栏红点还认列表里已经看过的失败。
    QSet<Tasks::TaskId> unread;
    for (const Tasks::TaskSnapshot &task : m_tasks.all()) {
        if (task.state == Tasks::TaskState::Failed && !task.failureSeen && !task.attentionAcknowledged) {
            unread.insert(task.id);
        }
    }
    if (unread != m_unreadFailures) {
        m_unreadFailures = unread;
        emit activityChanged();
    }
}

void ActivityCenterController::clearFinished() {
    m_tasks.clearFinished();
    // 「清除已完成」把行删了，可"还没看过的失败"那一份**也得跟着收窄**。
    // 那一份是按 id 记的（底栏的合并摘要 `TaskSnapshot.hpp::hasFailure` 直接看它），而
    // `markViewed()` 只在 `m_tasks.has(id)` 时才肯把 id 摘掉 —— 行一没，这个 id 就再没有
    // 任何出口（`markAllViewed()` 在生产代码里没有调用者）。症状：面板里一行都不剩、
    // 底栏写着「暂无后台任务」，那颗红色失败点却永远亮着，直到重启启动器（它不落盘）。
    //
    // 触发它**不需要**什么特殊操作：面板正停在「进行中」筛选、或者搜索词恰好匹配不到
    // 那一行失败项时，那一行根本没被建出来（面板的"逐行标记看过"只标建出来的行），
    // 于是它就是一条"没看过"的失败 —— 这时点「清除已完成」。
    bool pruned = false;
    for (auto it = m_unreadFailures.begin(); it != m_unreadFailures.end();) {
        if (m_tasks.has(*it)) {
            ++it;
        } else {
            it = m_unreadFailures.erase(it);
            pruned = true;
        }
    }
    // 上面这一笔必须**自己再发一发**。`clearFinished()` 里的
    // `historyChanged` 是在**删行那一刻**同步发出去的，主窗口接上就重画底栏 —— 那时候
    // 这份"没看过的失败"还带着刚被删掉的那些号，于是红点按旧账画了出来；等我们在这里
    // 收窄完，再也没有人重画它，红点就挂在底栏上直到下一次账本变动。
    if (pruned) {
        emit activityChanged();
    }
}

Tasks::TaskId ActivityCenterController::enqueue(Tasks::TaskKind kind, const QString &title, const QString &target,
    const QList<Tasks::TaskStep> &steps, const QString &lane, QObject *owner, std::function<void(Tasks::TaskId)> start) {
    if (lane.isEmpty() || owner == nullptr || !start || m_queue.size() >= 64) return Tasks::invalidTaskId();
    const auto id = m_tasks.begin(kind, title, target, steps, defaultCapabilities(kind), true);
    // 号发完了（`TaskManager::begin()` 那段：0 和最大值都不用，全占满时老实回
    // `invalidTaskId()`）。**绝不能把 0 排进队列**：`dispatch()` 拿 `snapshot(0)` 找不到，
    // `cancel()` / `startQueued()` 全是 no-op，于是这一件作业**无声无息地消失**，而调用方
    // 还以为它排上了（按钮按下去了、底栏什么也没有）。
    if (id == Tasks::invalidTaskId()) {
        return id;
    }
    m_queue.append({id, lane, owner, std::move(start)});
    m_tasks.setDetail(id, tr("等待执行；可拖动手柄调整同队列内的顺序。"));
    connect(owner, &QObject::destroyed, this, [this, id] {
        // 这里只能保证“尚未调用执行器”的任务被安全取消；运行任务由执行器自身收尾。
        // 不能把已进入原子提交的操作在 owner 析构时伪报为取消。
        if (m_tasks.has(id) && m_tasks.snapshot(id).state == Tasks::TaskState::Queued) m_tasks.cancel(id);
    });
    scheduleDispatch(); return id;
}
bool ActivityCenterController::isQueued(Tasks::TaskId id) const {
    return m_tasks.snapshot(id).state == Tasks::TaskState::Queued &&
        std::any_of(m_queue.cbegin(), m_queue.cend(), [id](const QueuedJob &job) { return job.id == id && job.owner; });
}
bool ActivityCenterController::moveQueued(Tasks::TaskId id, int delta) {
    if ((delta != -1 && delta != 1) || !isQueued(id)) return false;
    for (int i = 0; i < m_queue.size(); ++i) if (m_queue[i].id == id) {
        for (int j = i + delta; j >= 0 && j < m_queue.size(); j += delta) {
            if (m_queue[j].lane != m_queue[i].lane || !isQueued(m_queue[j].id)) continue;
            m_queue.swapItemsAt(i, j); emit activityChanged(); return true;
        }
        return false;
    }
    return false;
}
void ActivityCenterController::scheduleDispatch() {
    if (m_dispatchPending) return;
    m_dispatchPending = true;
    QTimer::singleShot(0, this, [this] { m_dispatchPending = false; dispatch(); });
}

void ActivityCenterController::releaseLane(Tasks::TaskId id) {
    for (auto it = m_runningLanes.begin(); it != m_runningLanes.end();)
        if (it.value() == id) it = m_runningLanes.erase(it); else ++it;
    scheduleDispatch();
}

bool ActivityCenterController::queueRecovery(Tasks::TaskId id, QObject *owner, std::function<void(Tasks::TaskId)> start) {
    if (!owner || !start || m_queue.size() >= 64 || isQueued(id) || !m_tasks.queueRecovery(id)) return false;
    m_queue.append({id, QStringLiteral("direct-download"), owner, std::move(start)});
    scheduleDispatch();
    return true;
}
void ActivityCenterController::dispatch() {
    // 启动前先摘出回调，执行器可以同步完成/取消甚至排入下一项，不能保留容器迭代器。
    for (;;) {
        int ready = -1;
        for (int i = 0; i < m_queue.size(); ++i) if (!m_runningLanes.contains(m_queue[i].lane)) { ready = i; break; }
        if (ready < 0) return;
        const auto job = m_queue.takeAt(ready);
        if (!job.owner || m_tasks.snapshot(job.id).state != Tasks::TaskState::Queued) { m_tasks.cancel(job.id); continue; }
        m_runningLanes.insert(job.lane, job.id);
        if (!m_tasks.startQueued(job.id)) { m_runningLanes.remove(job.lane); continue; }
        // startQueued 发信号后所有者可能被同步关闭；未调用执行器前仍可安全取消。
        if (!job.owner) { m_tasks.cancel(job.id); continue; }
        job.start(job.id);
    }
}

Tasks::TaskId ActivityCenterController::begin(Tasks::TaskKind kind, const QString &title,
                                               const QString &targetId,
                                               const QList<Tasks::TaskStep> &steps) {
    return m_tasks.begin(kind, title, targetId, steps, defaultCapabilities(kind));
}

bool ActivityCenterController::bindSession(Tasks::TaskId id, int sessionId) {
    // NOTE: 不再在这里给 Launch 补 `canCancel`：那一类从 `defaultCapabilities()` 起就
    // 按"能取消"记（进程起来前由启动流程自己停下，起来后停会话），见那边。
    return m_tasks.setSessionId(id, sessionId);
}

void ActivityCenterController::finish(Tasks::TaskId id, bool success,
                                      const QString &errorSummary,
                                      bool retryPayloadAvailable) {
    if (id == Tasks::invalidTaskId() || !m_tasks.has(id)) {
        return;
    }
    if (success) {
        if (m_retryableTaskId == id) {
            m_retryableTaskId = Tasks::invalidTaskId();
        }
        m_tasks.succeed(id);
        return;
    }

    if (retryPayloadAvailable) {
        Tasks::TaskCapabilities caps = m_tasks.snapshot(id).capabilities;
        caps.canRetry = true;
        m_tasks.setCapabilities(id, caps);
        m_retryableTaskId = id;
    }
    if (m_tasks.fail(id, errorSummary)) {
        m_unreadFailures.insert(id);
        emit activityChanged();
    }
}

Tasks::TaskSummary ActivityCenterController::collapsedSummary(const QDateTime &now,
                                                               qint64 recentWindowMs) const {
    return Tasks::collapsedSummary(m_tasks.all(), m_unreadFailures, now, recentWindowMs);
}

bool ActivityCenterController::isUnreadFailure(Tasks::TaskId id) const {
    return m_unreadFailures.contains(id);
}

bool ActivityCenterController::markAllViewed() {
    if (m_unreadFailures.isEmpty()) {
        return false;
    }
    m_unreadFailures.clear();
    return true;
}

bool ActivityCenterController::markSeen(const QString &key) {
    bool ok = false;
    const auto id = key.toULongLong(&ok);
    if (!ok || !m_unreadFailures.remove(id)) return false;
    if (!m_tasks.markFailureSeen(id)) emit activityChanged();
    return true;
}

void ActivityCenterController::setTransferSpeed(Tasks::TaskId id, qint64 bytesPerSecond) {
    if (!m_tasks.has(id) || m_tasks.snapshot(id).state != Tasks::TaskState::Running) return;
    if (transferSpeed(id) == bytesPerSecond) return;
    if (bytesPerSecond < 0) m_transferSpeeds.remove(id);
    else m_transferSpeeds.insert(id, bytesPerSecond);
    emit activityChanged();
}

bool ActivityCenterController::markViewed(const QString &key) {
    bool ok = false;
    // 行钥匙是 `QString::number(TaskId)`，而 `TaskId` 是 64 位 —— 用 `toInt()`
    // 解析会在号越过 INT_MAX 时失败（手工改过的历史文件就能造出来，见
    // `TaskManager::loadHistory()`）⇒ 这一行的「已看过」永远落不下去，
    // 底栏那颗红点到重启都在。同一个钥匙在 `itemForKey()` / 详情页两个动作 /
    // `retryTaskFromDetail()` 里也有，全部按 64 位读。
    const auto id = key.toULongLong(&ok);
    if (!ok || !m_tasks.has(id)) return false;
    const auto state = m_tasks.snapshot(id).state;
    // 等待选择不能因为“看过”而消失，只随业务完成选择后离开 WaitingUser。
    if (state != Tasks::TaskState::Failed && state != Tasks::TaskState::Interrupted) return false;
    if (!m_tasks.acknowledgeAttention(id)) return false;
    m_unreadFailures.remove(id);
    emit activityChanged();
    return true;
}

bool ActivityCenterController::retryWouldWork(const Tasks::TaskSnapshot &task,
                                               bool retryPayloadAvailable) const {
    return Tasks::retryOffered(task, m_retryableTaskId, retryPayloadAvailable);
}

void ActivityCenterController::markRetryConsumed() {
    m_retryableTaskId = Tasks::invalidTaskId();
}

TaskCenter::State ActivityCenterController::presentationState(Tasks::TaskState state) {
    switch (state) {
    case Tasks::TaskState::Queued:
        return TaskCenter::State::Queued;
    case Tasks::TaskState::Running:
        return TaskCenter::State::Running;
    case Tasks::TaskState::WaitingUser:
        return TaskCenter::State::WaitingUser;
    case Tasks::TaskState::Paused:
        return TaskCenter::State::Paused;
    case Tasks::TaskState::Succeeded:
        return TaskCenter::State::Succeeded;
    case Tasks::TaskState::Failed:
        return TaskCenter::State::Failed;
    case Tasks::TaskState::Cancelled:
        return TaskCenter::State::Cancelled;
    case Tasks::TaskState::Interrupted:
        return TaskCenter::State::Interrupted;
    }
    return TaskCenter::State::Interrupted;
}

TaskCenter::Item ActivityCenterController::item(const Tasks::TaskSnapshot &task,
                                                 bool cancelAvailable,
                                                 bool retryPayloadAvailable,
                                                 bool crashRecordAvailable) const {
    TaskCenter::Item result;
    result.key = QString::number(task.id);
    result.title = task.title;
    const auto displayText = [](QString text) {
        // Historical Qt errors remain intact on disk; localise their known UI spelling.
        const QString canceled = tr("网络请求已中止");
        text.replace(QStringLiteral("Operation canceled"), canceled, Qt::CaseInsensitive);
        text.replace(QStringLiteral("Operation cancelled"), canceled, Qt::CaseInsensitive);
        text.replace(QStringLiteral("这一版不在列表里（可能不是给这个实例的，或者作者已经删了它）。"),
                     tr("当前文件版本不在兼容版本列表中。"));
        return text;
    };
    result.detail = displayText(task.detail);
    result.percent = Tasks::percentOf(task);
    result.state = presentationState(task.state);
    for (int i = 0; i < m_queue.size(); ++i) if (m_queue[i].id == task.id && isQueued(task.id)) {
        result.queueGroup = m_queue[i].lane; result.queueOrder = i; break;
    }
    result.unreadFailure = task.state == Tasks::TaskState::Failed
                           && isUnreadFailure(task.id);
    result.attentionAcknowledged = task.attentionAcknowledged;
    result.finishedAt = task.finishedAt;
    result.startedAt = task.startedAt;
    result.errorSummary = displayText(task.errorSummary);
    if (task.title == tr("资源更新检查") && task.state == Tasks::TaskState::Failed) {
        const QString summary = tr("更新检查结果可能不完整");
        if (result.detail.isEmpty()) {
            result.detail = result.errorSummary.startsWith(summary + QLatin1Char('\n'))
                ? result.errorSummary.section(QLatin1Char('\n'), 1) : result.errorSummary;
        }
        result.errorSummary = summary;
    }
    if (task.bytesTotal > 0) {
        result.bytesText = tr("已下载 %1 / %2")
                               .arg(InstanceFilesPage::formatSize(task.bytesDone),
                                    InstanceFilesPage::formatSize(task.bytesTotal));
    }
    if (task.state == Tasks::TaskState::Running && transferSpeed(task.id) >= 0) {
        if (!result.bytesText.isEmpty()) result.bytesText += QStringLiteral(" / ");
        result.bytesText += InstanceFilesPage::formatSize(transferSpeed(task.id)) + QStringLiteral("/s");
    }

    result.capabilities.cancel = task.capabilities.canCancel
                                 && Tasks::isActiveState(task.state) && cancelAvailable;
    result.capabilities.pause = task.capabilities.canPause
                                && Tasks::isActiveState(task.state);
    result.capabilities.retry = retryWouldWork(task, retryPayloadAvailable);
    result.capabilities.openLog = task.capabilities.canOpenLog;
    result.capabilities.openTarget = task.capabilities.canOpenTarget;
    result.capabilities.recover = task.capabilities.canRecover
        && (Tasks::isTerminalState(task.state) || task.state == Tasks::TaskState::Paused) && !task.recoveryToken.isEmpty();
    result.capabilities.repairImport = task.kind == Tasks::TaskKind::Import
        && (task.state == Tasks::TaskState::Failed || task.state == Tasks::TaskState::Interrupted)
        && !task.targetId.isEmpty()
        && InstanceFiles::validatePathSegment(task.targetId).isEmpty()
        && QFileInfo::exists(InstanceFiles::importMarkerPath(task.targetId))
        && QFileInfo::exists(AppPaths::versionDir(task.targetId) + QStringLiteral("/wtfml.import-report.json"));
    result.capabilities.exportReport = task.capabilities.canExportReport
                                       && crashRecordAvailable;
    result.capabilities.openCrashDir = task.capabilities.canOpenCrashDir;
    // 「为什么没有取消」：账本那边明写的优先；「这一类本来允许取消、只是这一段停不得」
    // （加载器安装器在跑、整合包解压…）由这里补一句通用说法 —— 那是执行器的临时状态，
    // 没必要让每个执行器都自己拼一遍。
    result.cancelBlockedReason = task.capabilities.cancelBlockedReason;
    if (Tasks::isActiveState(task.state) && !result.capabilities.cancel
        && result.cancelBlockedReason.isEmpty() && task.capabilities.canCancel) {
        result.cancelBlockedReason = tr("当前步骤无法安全中止。请在此步骤完成后取消。");
    }

    result.steps.reserve(task.steps.size());
    for (const Tasks::TaskStep &step : task.steps) {
        result.steps.append({step.name, presentationStepState(step.state), displayText(step.note)});
    }
    return result;
}

QList<TaskCenter::Item> ActivityCenterController::items(
    const std::function<bool(const Tasks::TaskSnapshot &)> &cancelAvailable,
    bool retryPayloadAvailable, const QSet<Tasks::TaskId> &crashRecordIds,
    bool *viewStateChanged) {
    const QList<Tasks::TaskSnapshot> snapshots = m_tasks.all();
    QList<TaskCenter::Item> result;
    result.reserve(snapshots.size());
    for (const Tasks::TaskSnapshot &task : snapshots) {
        result.append(item(task, cancelAvailable && cancelAvailable(task),
                           retryPayloadAvailable, crashRecordIds.contains(task.id)));
    }
    std::stable_sort(result.begin(), result.end(), [](const TaskCenter::Item &a, const TaskCenter::Item &b) {
        const auto rank = [](const TaskCenter::Item &item) {
            return item.state == TaskCenter::State::Queued ? 1 :
                (item.state == TaskCenter::State::Running || item.state == TaskCenter::State::WaitingUser || item.state == TaskCenter::State::Paused ? 0 : 2);
        };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.state == TaskCenter::State::Queued && b.state == TaskCenter::State::Queued && a.queueOrder < b.queueOrder;
    });
    // 这里**不**顺手把"没看过的失败"整批标成看过：`markAllViewed()` 这一发发生在
    // **面板筛子和搜索之前**，用户把面板停在「进行中」（或者输了搜索词）时，失败那几行
    // 根本没露脸，红点却已经灭了 —— 一次失败就这样被"悄悄看过"了。这一类标记由面板在
    // **真建出那一行**之后按行发 `RowAction::Viewed`（见 TaskCenterPanel::rebuild）。
    if (viewStateChanged != nullptr) {
        *viewStateChanged = false;
    }
    return result;
}

bool ActivityCenterController::itemForKey(
    const QString &key, TaskCenter::Item *out,
    const std::function<bool(const Tasks::TaskSnapshot &)> &cancelAvailable,
    bool retryPayloadAvailable, const QSet<Tasks::TaskId> &crashRecordIds) const {
    bool ok = false;
    const Tasks::TaskId id = key.toULongLong(&ok);
    if (!ok || out == nullptr || !m_tasks.has(id)) {
        return false;
    }
    const Tasks::TaskSnapshot task = m_tasks.snapshot(id);
    *out = item(task, cancelAvailable && cancelAvailable(task), retryPayloadAvailable,
                crashRecordIds.contains(id));
    return true;
}

Tasks::TaskCapabilities ActivityCenterController::defaultCapabilities(Tasks::TaskKind kind) {
    Tasks::TaskCapabilities caps;
    switch (kind) {
    case Tasks::TaskKind::Download:
    case Tasks::TaskKind::Install:
    case Tasks::TaskKind::Import:
    case Tasks::TaskKind::Launch:
        // Launch 从**进程起来之前**就允许取消：启动流程在下一个检查点停下
        // （见 LaunchController::abortStart()）。进程起来之后那一颗改为停会话，
        // 分派在 MainWindow::cancelLaunch() 里。
        caps.canCancel = true;
        break;
    case Tasks::TaskKind::Export:
        caps.cancelBlockedReason =
            tr("导出正在生成压缩包。为避免留下不完整的文件，压缩阶段无法取消。");
        break;
    case Tasks::TaskKind::Backup:
        caps.cancelBlockedReason = tr("这一项正在发布结果，发布过程中不能取消。");
        break;
    case Tasks::TaskKind::Java:
        caps.cancelBlockedReason = tr("正在准备 Java 运行环境，完成后会自动继续；这一项不支持取消。");
        break;
    case Tasks::TaskKind::Integrity:
        caps.cancelBlockedReason = tr("完整性检查在后台只读扫描，不会改动任何文件；它停不了，跑完会统一报结果。");
        break;
    case Tasks::TaskKind::Crash:
    case Tasks::TaskKind::ServerLaunch:
    case Tasks::TaskKind::Other:
        break;
    }
    if (kind == Tasks::TaskKind::Crash) {
        caps.canExportReport = true;
        caps.canOpenCrashDir = true;
    }
    caps.canOpenLog = true;
    caps.canOpenTarget = true;
    return caps;
}
