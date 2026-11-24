/**
 * @file TaskManager.cpp
 * @brief 任务账本的实现（形状与取舍见头文件）。
 */

#include "TaskManager.hpp"

#include <algorithm>
#include <limits>

#include <QDebug>
#include <QSet>

#include "TaskHistory.hpp"

namespace {

/// 「已完成」那一栏的顺序：最近的在前。
///
/// 只比 `finishedAt` 是不够的 —— 批量下载/批量文件操作会在同一毫秒里收掉几十项
/// （`QDateTime` 只到毫秒），并列时 `std::sort` 给出的是**未定义**的相对顺序。
/// 面板每次重画都重排一遍，并列出会让这些行在两次重画之间换位；落盘截断那头
/// 更实际：`finishedHistoryLimit()` 卡在第 50 条边上时，丢掉的是"并列里的任意一条"，
/// 而不是最旧那一条。号是单调发的 ⇒ 同毫秒里号大的就是后发起的，拿它当第二把尺。
bool finishedNewerFirst(const Tasks::TaskSnapshot &a, const Tasks::TaskSnapshot &b) {
    if (a.finishedAt != b.finishedAt) {
        return a.finishedAt > b.finishedAt;
    }
    return a.id > b.id;
}

}  // namespace

namespace Tasks {

TaskManager::TaskManager(QObject *parent) : QObject(parent) {
    // NOTE: `m_historyPath` 留空 = 使用启动器级默认账本；测试可以显式指到临时文件。
}

TaskManager::~TaskManager() = default;

QString TaskManager::resolvedHistoryPath() const {
    return m_historyPath.isEmpty() ? defaultHistoryPath() : m_historyPath;
}

int TaskManager::loadHistory() {
    QList<TaskSnapshot> loaded = load(resolvedHistoryPath());
    // 读回来的是**上一份账本**（含当时还在跑的那几项 —— 它们这会儿已经是 `Interrupted`）。
    // 当前账本整个换掉：这个方法只在启动时调一次，那时候手上本来就没有东西。
    //
    // 盘上那份是**普通文本文件**（还可能被手工改过、被两个进程前后覆写
    // 过），所以"号唯一、且最大那个加一不溢出"不是可以当成的先验，得在这里立住：
    // · **重号**：`find()`/`snapshot()` 只认第一条，后面那几条从此再也收不了尾；
    // 而面板拿 `QString::number(id)` 当行钥匙，重号 = 两行共用一个钥匙，点其中
    // 一行的「重试 / 打开日志 / 打开目标」打到的是**另一条**身上。
    // · **`TaskId` 最大值**：`task.id + 1` 在原地回绕成 0 ⇒ `m_nextId` 退回 1 ⇒
    // 新任务和盘上某条历史撞号，而那一条是终态、`finish()` 会直接回 false ——
    // **真在跑的那件事再也收不了尾**（正是 TaskId.hpp 里那段注释要挡的东西，
    // 只是它假定盘上的号是"我们自己写下去的"）。
    // 两种都当"认不出是谁的那一行"跳过：和 `fromJson()` 里 `id == 0` 同一种处置。
    constexpr TaskId kMaxTaskId = std::numeric_limits<TaskId>::max();
    QSet<TaskId> seenIds;
    QList<TaskSnapshot> unique;
    unique.reserve(loaded.size());
    for (const TaskSnapshot &task : loaded) {
        if (task.id == invalidTaskId() || task.id == kMaxTaskId || seenIds.contains(task.id)) {
            continue;
        }
        seenIds.insert(task.id);
        unique.append(task);
    }
    m_tasks = std::move(unique);
    m_nextId = 1;
    for (const TaskSnapshot &task : m_tasks) {
        m_nextId = std::max(m_nextId, task.id + 1);
    }
    emit historyChanged();
    return m_tasks.size();
}

bool TaskManager::saveHistory() const {
    // 已完成的按上限截断：**留最近的**（列表已经按时间排过，这里自己数一遍，不依赖顺序）。
    // 子任务**不落盘**：盘上那一份是"用户意图"的账（父任务），子任务只是这一趟里的几步。
    QList<TaskSnapshot> keep;
    keep.reserve(m_tasks.size());
    QList<TaskSnapshot> finishedTasks;
    for (const TaskSnapshot &task : m_tasks) {
        if (task.parentId != invalidTaskId()) {
            continue;
        }
        if (isTerminalState(task.state)) {
            finishedTasks.append(task);
        } else {
            keep.append(task);
        }
    }
    std::sort(finishedTasks.begin(), finishedTasks.end(), finishedNewerFirst);
    const int limit = finishedHistoryLimit();
    for (int i = 0; i < finishedTasks.size() && i < limit; ++i) {
        keep.append(finishedTasks.at(i));
    }
    const bool saved = save(resolvedHistoryPath(), keep);
    // 落盘失败（盘满、目录变成只读、账本被别的进程删成目录……）不能一声不吭：
    // 这一次会话看着一切正常，关掉启动器以后历史就没了。日志里至少得留一行。
    if (!saved) {
        qWarning() << "Failed to save the task history to" << resolvedHistoryPath();
    }
    return saved;
}

TaskId TaskManager::begin(TaskKind kind, const QString &title, const QString &targetId,
                          const QList<TaskStep> &steps, const TaskCapabilities &capabilities, bool queued) {
    const TaskId id = allocateId();
    if (id == invalidTaskId()) {
        return id;
    }
    TaskSnapshot task;
    task.id = id;
    task.kind = kind;
    task.state = queued ? TaskState::Queued : TaskState::Running;
    task.title = title;
    task.targetId = targetId;
    task.steps = steps;
    task.capabilities = capabilities;
    if (!queued) task.startedAt = QDateTime::currentDateTime();
    m_tasks.append(task);
    touch(task.id, /*persist=*/true);
    return task.id;
}

TaskId TaskManager::allocateId() {
    constexpr TaskId kMaxTaskId = std::numeric_limits<TaskId>::max();
    TaskId id = m_nextId;
    if (id == invalidTaskId() || id == kMaxTaskId) {
        id = 1;
    }
    for (;;) {
        if (id == kMaxTaskId) {
            return invalidTaskId();
        }
        if (find(id) == nullptr) {
            break;
        }
        ++id;
    }
    m_nextId = id + 1;
    return id;
}

TaskId TaskManager::beginChild(TaskId parent, TaskKind kind, const QString &title,
                               const QString &targetId, const QList<TaskStep> &steps,
                               const TaskCapabilities &capabilities, int parentStep) {
    TaskSnapshot *owner = find(parent);
    // 父任务不在或已经收尾时不能挂子任务：那一行不会再有人更新，子任务会变成
    // "跑完了但界面上什么都没有"。调用方应退回 `begin()`。
    if (owner == nullptr || owner->parentId != invalidTaskId()
        || !isActiveState(owner->state)) {
        return invalidTaskId();
    }
    if (parentStep >= 0 && parentStep < owner->steps.size()) {
        owner->steps[parentStep].state = TaskStepState::Running;
        owner->steps[parentStep].note.clear();
    }
    owner->detail = title;
    const TaskId id = allocateId();
    if (id == invalidTaskId()) {
        return id;
    }
    TaskSnapshot task;
    task.id = id;
    task.parentId = parent;
    task.parentStep = parentStep >= 0 && parentStep < owner->steps.size() ? parentStep : -1;
    task.kind = kind;
    task.state = TaskState::Running;
    task.title = title;
    task.targetId = targetId;
    task.steps = steps;
    task.capabilities = capabilities;
    task.startedAt = QDateTime::currentDateTime();
    m_tasks.append(task);
    // 子任务不落盘；父任务那一步刚变了，但它也不落盘（步骤/进度不在摘要里）。
    touch(parent, /*persist=*/false);
    return id;
}

bool TaskManager::startQueued(TaskId id) {
    auto *task = find(id);
    if (task == nullptr || task->state != TaskState::Queued) return false;
    task->state = TaskState::Running;
    task->startedAt = QDateTime::currentDateTime();
    touch(id, true);
    return true;
}

bool TaskManager::pause(TaskId id) {
    auto *task = find(id);
    if (!task || task->state != TaskState::Running || !task->capabilities.canPause) return false;
    task->state = TaskState::Paused;
    task->capabilities.canPause = false;
    touch(id, true);
    return true;
}

bool TaskManager::queueRecovery(TaskId id) {
    auto *task = find(id);
    if (!task || !task->capabilities.canRecover ||
        (task->state != TaskState::Paused && !isTerminalState(task->state))) return false;
    task->state = TaskState::Queued;
    task->finishedAt = {};
    task->errorSummary.clear();
    task->capabilities.canRecover = false;
    task->capabilities.canCancel = true;
    touch(id, true);
    return true;
}

TaskSnapshot *TaskManager::find(TaskId id) {
    if (id == invalidTaskId()) {
        return nullptr;
    }
    for (TaskSnapshot &task : m_tasks) {
        if (task.id == id) {
            return &task;
        }
    }
    return nullptr;
}

TaskSnapshot TaskManager::snapshot(TaskId id) const {
    for (const TaskSnapshot &task : m_tasks) {
        if (task.id == id) {
            return task;
        }
    }
    return TaskSnapshot();
}

bool TaskManager::has(TaskId id) const {
    return id != invalidTaskId() && snapshot(id).id == id;
}

void TaskManager::touch(TaskId id, bool persist) {
    if (persist) {
        saveHistory();
    }
    emit taskChanged(id);
}

/// 三个收尾入口的公共部分。
///
/// 「终态只能落一次」在这里兜住：**已经收尾的、找不到的，一律回 false 并且一个信号都不发**。
/// 上游那几条路（下载批次收尾、进程结束、用户点取消）都可能在同一个状态上被触发两遍 ——
/// 真按第二遍再记一次，"同一件事出现两行历史""进度条上的数字比总数还大"就是从这里来的。
bool TaskManager::finish(TaskId id, TaskState state, const QString &errorSummary) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || isTerminalState(task->state)) {
        return false;
    }
    task->state = state;
    task->errorSummary = errorSummary;
    task->finishedAt = QDateTime::currentDateTime();
    if (state == TaskState::Failed) {
        for (auto &step : task->steps)
            if (step.state == TaskStepState::Running) step.state = TaskStepState::Failed;
    }
    // 收尾之后**只**收掉"干活时才有的"那两位，其余几位原样留着。
    //
    // 不能写成 `task->capabilities = TaskCapabilities()`：「重试」和 
    // 崩溃项都是**收尾之后才露**的按钮：
    // · 「重试」：`ActivityCenterController::finish()` 在 `fail()` **之前**把 `canRetry`
    // 写进快照（`setCapabilities()` 只收活动态），下一句 `fail()` 走进这里再整份抹掉的话
    // ⇒ `Tasks::retryOffered()` 恒 false，界面上永远没有那颗按钮；
    // · 崩溃项（`CrashReportController::report()`）建出来那一刻就带着
    // `canExportReport` / `canOpenCrashDir`，随即 `finish()` —— 一起抹掉的话详情页
    // 上「导出错误报告」「打开崩溃报告目录」「打开日志」一颗都不露。
    // 「取消 / 暂停」反过来只在活动态有意义（`ActivityCenterController::item()` 那边也
    // 再按状态挡一道），收尾时清掉它们是这里唯一该做的事。
    task->capabilities.canCancel = false;
    task->capabilities.canPause = false;
    if (state == TaskState::Succeeded) {
        task->doneUnits = task->totalUnits;
        task->detail.clear();
    }
    const TaskSnapshot finishedChild = *task;
    propagateChild(finishedChild);
    pruneFinished();
    // 子任务只在这一次会话里存在：它的收尾不需要重写盘上那份历史。
    if (finishedChild.parentId == invalidTaskId()) {
        saveHistory();
    }
    emit taskChanged(id);
    emit taskFinished(id, state == TaskState::Succeeded);
    emit historyChanged();
    return true;
}

void TaskManager::propagateChild(const TaskSnapshot &child) {
    if (child.parentId == invalidTaskId()) {
        return;
    }
    TaskSnapshot *owner = find(child.parentId);
    if (owner == nullptr) {
        return;
    }
    // 详情跟着「现在在做什么」走；字节进度也一样（准备核心那一截子任务是下载）。
    if (isActiveState(child.state)) {
        owner->detail = child.detail;
        owner->bytesDone = child.bytesDone;
        owner->bytesTotal = child.bytesTotal;
    }
    if (child.parentStep >= 0 && child.parentStep < owner->steps.size()) {
        TaskStep &step = owner->steps[child.parentStep];
        switch (child.state) {
        case TaskState::Succeeded:
            step.state = TaskStepState::Done;
            if (!child.errorSummary.isEmpty()) step.note = child.errorSummary;
            break;
        case TaskState::Failed:
            step.state = TaskStepState::Failed;
            step.note = child.errorSummary;
            break;
        case TaskState::Interrupted:
            step.state = TaskStepState::Failed;
            break;
        case TaskState::Cancelled:
            // 取消不是那一步做错了：保持原状，由串起这些步骤的流程决定父任务怎么收尾。
            break;
        case TaskState::Queued:
        case TaskState::Running:
        case TaskState::WaitingUser:
        case TaskState::Paused:
            step.state = TaskStepState::Running;
            break;
        }
    }
    if (isActiveState(child.state)) {
        owner->detail = child.detail;
    }
    touch(owner->id, /*persist=*/false);
}

void TaskManager::pruneFinished() {
    // 和 saveHistory() 用同一把尺子（`finishedNewerFirst`）：活着的一律留着，
    // 终态里超出上限的那些挑**最旧的**丢。不排序就按插入顺序丢的话，同一毫秒里
    // 收掉的一批（批量下载）丢掉的是"并列里的任意一条"，和落盘那份对不上。
    QList<TaskSnapshot> finishedTasks;
    finishedTasks.reserve(m_tasks.size());
    for (const TaskSnapshot &task : m_tasks) {
        // 只有父任务占「已完成」那一栏；子任务随父任务一起生灭。
        if (task.parentId == invalidTaskId() && isTerminalState(task.state)) {
            finishedTasks.append(task);
        }
    }
    const int limit = finishedHistoryLimit();
    if (finishedTasks.size() > limit) {
        std::sort(finishedTasks.begin(), finishedTasks.end(), finishedNewerFirst);
        QSet<TaskId> drop;
        for (int i = limit; i < finishedTasks.size(); ++i) {
            drop.insert(finishedTasks.at(i).id);
        }
        m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(),
                                     [&drop](const TaskSnapshot &task) {
                                         return drop.contains(task.id);
                                     }),
                      m_tasks.end());
    }
    // 父任务已经不在的子任务没有任何出口（它们从不单独列出）——一并清掉，
    // 不然账本会随「清除已完成 / 截断」一点点漏下看不见的行。
    QSet<TaskId> live;
    for (const TaskSnapshot &task : m_tasks) {
        if (task.parentId == invalidTaskId()) live.insert(task.id);
    }
    m_tasks.removeIf([&live](const TaskSnapshot &task) {
        return task.parentId != invalidTaskId() && !live.contains(task.parentId);
    });
}

bool TaskManager::succeed(TaskId id) {
    return finish(id, TaskState::Succeeded, QString());
}

bool TaskManager::fail(TaskId id, const QString &errorSummary) {
    return finish(id, TaskState::Failed, errorSummary);
}

bool TaskManager::cancel(TaskId id) {
    return finish(id, TaskState::Cancelled, QString());
}

bool TaskManager::acknowledgeAttention(TaskId id) {
    auto *task = find(id);
    if (task == nullptr || task->attentionAcknowledged ||
        (task->state != TaskState::Failed && task->state != TaskState::Interrupted)) return false;
    task->attentionAcknowledged = true;
    touch(id, true);
    return true;
}

bool TaskManager::markFailureSeen(TaskId id) {
    auto *task = find(id);
    if (task == nullptr || task->state != TaskState::Failed || task->failureSeen) return false;
    task->failureSeen = true;
    touch(id, true);
    return true;
}

bool TaskManager::setDetail(TaskId id, const QString &detail) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state) || task->detail == detail) {
        return false;
    }
    task->detail = detail;
    // **不落盘**：详情是"此刻在干什么"，盘上那一份是摘要（见 TaskHistory.hpp）。
    const TaskSnapshot mirrored = *task;
    propagateChild(mirrored);
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setTitleAndTarget(TaskId id, const QString &title, const QString &targetId) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state)) {
        return false;
    }
    if (task->title == title && task->targetId == targetId) {
        return false;
    }
    task->title = title;
    task->targetId = targetId;
    touch(id, task->parentId == invalidTaskId());
    return true;
}

bool TaskManager::setSessionId(TaskId id, int sessionId) {
    // 一项任务只对**一份**会话。已经对上了还要换个号 = 调用方那边把两件事搞混了，
    // 这里不替它改口（改了口之后"这一项说的是哪一次运行"就再也说不清）。
    TaskSnapshot *task = find(id);
    if (task == nullptr || sessionId <= 0 || task->sessionId != 0) {
        return false;
    }
    task->sessionId = sessionId;
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setSteps(TaskId id, const QList<TaskStep> &steps) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state)) {
        return false;
    }
    task->steps = steps;
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setStepState(TaskId id, int index, TaskStepState state, const QString &note) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state) || index < 0
        || index >= task->steps.size()) {
        return false;
    }
    if (task->kind == TaskKind::Launch && state == TaskStepState::Done &&
        task->steps.at(index).state == TaskStepState::Running) {
        // 步骤结束与计数清零一起发布，避免进度先退回本步起点。
        task->doneUnits = 0;
        task->totalUnits = 0;
    }
    task->steps[index].state = state;
    task->steps[index].note = note;
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setProgress(TaskId id, qint64 doneUnits, qint64 totalUnits) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state)) {
        return false;
    }
    if (task->doneUnits == doneUnits && task->totalUnits == totalUnits) {
        return false;  // 一模一样的一帧不用再喊一遍（下载进度是每 200 ms 来一次的）
    }
    task->doneUnits = doneUnits;
    task->totalUnits = totalUnits;
    const TaskSnapshot mirrored = *task;
    propagateChild(mirrored);
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setBytes(TaskId id, qint64 bytesDone, qint64 bytesTotal) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state)) {
        return false;
    }
    if (task->bytesDone == bytesDone && task->bytesTotal == bytesTotal) {
        return false;
    }
    task->bytesDone = bytesDone;
    task->bytesTotal = bytesTotal;
    const TaskSnapshot mirrored = *task;
    propagateChild(mirrored);
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setCapabilities(TaskId id, const TaskCapabilities &capabilities) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || !isActiveState(task->state)) {
        return false;
    }
    task->capabilities = capabilities;
    touch(id, /*persist=*/false);
    return true;
}

bool TaskManager::setRecovery(TaskId id, const QString &recoveryToken, bool canRecover,
                              const QString &blockedReason) {
    TaskSnapshot *task = find(id);
    if (task == nullptr || (!isTerminalState(task->state) && task->state != TaskState::Paused)) {
        return false;
    }
    task->recoveryToken = recoveryToken;
    task->capabilities.canRecover = canRecover;
    task->capabilities.recoverBlockedReason = canRecover ? QString() : blockedReason;
    touch(id, /*persist=*/false);
    return true;
}

void TaskManager::clearFinished() {
    const int before = m_tasks.size();
    QSet<TaskId> removedParents;
    for (const TaskSnapshot &task : m_tasks) {
        if (task.parentId == invalidTaskId() && isTerminalState(task.state)) {
            removedParents.insert(task.id);
        }
    }
    m_tasks.removeIf([&removedParents](const TaskSnapshot &task) {
        return isTerminalState(task.state) || removedParents.contains(task.parentId);
    });
    if (m_tasks.size() == before) {
        return;  // 本来就没有已完成的东西：不落盘也不发信号
    }
    saveHistory();
    emit historyChanged();
}

QList<TaskSnapshot> TaskManager::active() const {
    QList<TaskSnapshot> result;
    for (const TaskSnapshot &task : m_tasks) {
        // 子任务不进任务中心：一次用户意图只露一个父任务。
        if (task.parentId == invalidTaskId() && isActiveState(task.state)) {
            result.append(task);
        }
    }
    // 台账里的顺序就是发起的顺序 —— 只有一处往里面加东西（`begin()`），所以它天然有序。
    return result;
}

QList<TaskSnapshot> TaskManager::finished() const {
    QList<TaskSnapshot> result;
    for (const TaskSnapshot &task : m_tasks) {
        if (task.parentId == invalidTaskId() && isTerminalState(task.state)) {
            result.append(task);
        }
    }
    std::sort(result.begin(), result.end(), finishedNewerFirst);
    return result;
}

QList<TaskSnapshot> TaskManager::all() const {
    QList<TaskSnapshot> result = active();
    result.append(finished());
    return result;
}

QList<TaskSnapshot> TaskManager::childrenOf(TaskId parent) const {
    QList<TaskSnapshot> result;
    for (const TaskSnapshot &task : m_tasks) {
        if (task.parentId == parent) {
            result.append(task);
        }
    }
    return result;
}

}  // namespace Tasks
