/**
 * @file TaskSnapshot.hpp
 * @brief 一项任务的**完整样子**，以及"底栏收起态该显示谁"那几条纯规则。
 *
 * 「主任务与步骤分开」。一项任务 = 一个用户意图（"安装 1.21.4 Fabric"、
 * "启动 生存整合包"），它内部**可以有几千个下载作业** —— 那些是步骤，不是任务。
 *
 * 【这一层是什么、不是什么】
 *
 * * 是：任务的**形状**（种类 / 状态 / 进度 / 步骤 / 能力）和几条纯函数（谁优先、百分比
 * 怎么算、收起态显示谁）。
 * * 不是：任务的账本。谁在跑、跑到哪儿、什么时候结束，全在 `TaskManager` 里。
 *
 * 这个头文件**只有值**：一个 QObject 都没有、一个信号都没有，也不碰盘。所以 `verify`
 * 能直接喂一组快照进去钉那几条规则（见 `tests/main.cpp` 的 AL 节）。
 *
 * 【为什么状态是一个枚举，不是几个 bool】
 *
 * 要求的：「不要再用几个 bool 拼状态」。`(inFlight, ok, cancelled)` 那种
 * 组合能拼出 8 种，其中一半是没有意义的（"在跑但成功"、"取消且失败"），而每加一个场景就
 * 要多立一条"这两个同时为真时按谁"的暗规则 —— 那些规则没人找得到，只能靠读代码。
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>

#include "TaskId.hpp"

namespace Tasks {

/// 这件事是干什么的。**只用来分类和排优先级**，界面上的按钮不靠它猜（见
/// `TaskCapabilities` 那段）。
enum class TaskKind {
    Install,    // 装一个版本 / 给实例装加载器 / 补全一个实例（repair）
    Download,   // 下资源（模组 / 资源包 / 光影 / 数据包）
    Launch,     // 启动一个实例
    Import,     // 导入整合包
    Export,     // 导出整合包
    Java,       // Java 的扫描 / 下载
    Integrity,  // 完整性扫描
    Crash,      // 游戏异常退出之后补的那一项。**一建出来就是终态**，从不"在跑"
    Backup,     // 打一份存档 / 实例快照，或者从一份备份还原回去
    Other,
    ServerLaunch, // 启动服务端，直到服务端报告就绪
};

/// 任务的状态。八个，一个不多一个不少 —— 和状态枚举逐字对齐。
enum class TaskState {
    Queued,       // 排着队，还没轮到（当前只有单批次，这个状态基本只在盘上那半出现）
    Running,      // 正在跑
    WaitingUser,  // **卡在等用户**（弹了个提示、要用户选一下）。收起态里它排第一
    Paused,       // 执行器已释放网络活动，明确等待用户继续。
    Succeeded,    // 成功
    Failed,       // 失败（带 `TaskSnapshot::errorSummary` 那句原因）
    Cancelled,    // 用户取消的
    Interrupted,  // 启动器上次异常退出时它还没跑完（**读盘时**才出现，见 TaskHistory）
};

/// 一步的状态。
enum class TaskStepState {
    Pending,
    Running,
    Done,
    Failed,
};

/// 任务里的一步（「元数据、下载、校验、安装器、收尾」）。
struct TaskStep {
    QString name;                      // 「校验文件」「准备 Java」
    TaskStepState state = TaskStepState::Pending;
    QString note;                      // 失败原因 / 这一步的补充说明，可空
};

/// 这项任务**允许**什么操作。按钮不靠任务种类猜。
///
/// 为什么要有这一层而不是"看 kind 出按钮"：同是 `Install`，Forge 安装器跑到"不可安全
/// 中断的那一段"时**不能取消**，而普通下载任何时候都能。拿 kind 猜的话，界面
/// 就得把"什么时候不能取消"再推一遍，而那个知识只在下命令的那一层手上有。
struct TaskCapabilities {
    bool canCancel = false;
    /// 「为什么不能取消」。只有 `canCancel == false` 时才有意义，详情页原样摆给用户。
    /// 空地留空 = 这一项不需要解释（比如已经收尾）。**不落盘**：它不是摘要的一部分。
    QString cancelBlockedReason;
    bool canPause = false;   // 由支持安全暂停的执行器动态提供。
    bool canRetry = false;   // 恒 false（取消/重试还没做）
    /// 「继续未完成操作」：这一项留下了一份可以接着做的文件操作报告（见
    /// `OperationReport`）。只在这一次会话里成立 —— 报告本身在盘上，但账本读回来时
    /// 不带能力位（见 `TaskHistory::toJson()`），所以重启后不露出这颗按钮。
    bool canRecover = false;
    /// 「为什么不能继续」。只有 `canRecover == false` 时才有意义的解释（可空）。
    QString recoverBlockedReason;
    bool canOpenTarget = false;
    bool canOpenLog = false;
    /// 「导出错误报告」。**只有游戏异常退出那一项**为真 —— 报告要的是一份会话事实
    /// （Java 版本 / 内存参数 / 退出码），别的那几类任务没有这些东西。
    bool canExportReport = false;
    /// 「打开崩溃报告目录」。同样只有游戏异常退出那一项为真。
    bool canOpenCrashDir = false;
};

/// 一项任务在**某一刻**的样子。
struct TaskSnapshot {
    TaskId id = invalidTaskId();
    TaskKind kind = TaskKind::Other;
    TaskState state = TaskState::Queued;
    QString title;       // 「安装 1.21.4 Fabric」
    QString detail;      // 「正在下载资源对象…」
    QString targetId;    // 实例 id / 服务端 id，可空
    /// 父任务（0 = 自己是父任务或独立任务）。**子任务不进任务中心**：它们是同一个用户
    /// 意图里的几步（准备核心 / 复制 / 快照），面板和总进度只认父任务（见
    /// `TaskManager::beginChild()`）。**不落盘**：盘上只留父任务那一行。
    TaskId parentId = invalidTaskId();
    /// 这个子任务对应父任务第几步（`-1` = 不映射）。子任务收尾时只改那一步的状态。
    int parentStep = -1;
    /// 「继续未完成操作」认的那份报告（`OperationReport::Report::id`）。
    /// **不落盘**：它指向的是一份这次操作留下的现场，重启后由报告自己说话。
    QString recoveryToken;
    /// **只活在内存里，不落盘**：会话号是每次启动器运行从 1 开始的，重启之后同一个号
    /// 指的是另一趟。读回来的历史一律是 0（`TaskHistory` 也不写它）。
    int sessionId = 0;
    qint64 doneUnits = 0;
    qint64 totalUnits = 0;  // <= 0 表示不可测（**不许编造百分比**）
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;  // <= 0 表示未知
    QDateTime startedAt;
    QDateTime finishedAt;
    QList<TaskStep> steps;
    TaskCapabilities capabilities;
    QString errorSummary;   // 失败那一句话（只有 Failed 有）
    bool attentionAcknowledged = false; // 失败/中断详情已查看，与业务终态分开保存。
    bool failureSeen = false;             // 失败已在列表里出现，用于跨会话隐藏红点。
};

// —— 下面几条都是**纯函数**（不看时钟、不看盘、不看 QObject）——

/// 还在进行中（会占着"当前工作"那一栏）。
inline bool isActiveState(TaskState state) {
    return state == TaskState::Queued || state == TaskState::Running
           || state == TaskState::WaitingUser || state == TaskState::Paused;
}

/// 已经收尾了（会进"已完成"那一栏 / 任务历史）。
inline bool isTerminalState(TaskState state) {
    return !isActiveState(state);
}

/// 这一项此刻该不该露那颗「重试」。
///
/// 「逐任务能力表」的重试那一半（取消那一半是
/// `MainWindow::cancelWouldWork()`，两者是孪生兄弟）。
///
/// 三样**缺一不可**，每一道挡住的都是真出现过的那一种毛病：
///
/// | 那一道 | 挡住什么 |
/// |---|---|
/// | `capabilities.canRetry` | 这一类活**允许**重试吗（账本起的表说了算，界面不按种类猜） |
/// | `isTerminalState(state)` | 还在跑的那一项不该露它（正在下的时候"重试"没有含义） |
/// | `task.id == failedBatchOwner && failedBatchHeld` | 点了**真会重做它自己那几个文件**吗 |
///
/// 第三道为什么必须带上号：那份"失败清单"（`VersionManager::m_failedBatch`）**只有一格**、
/// 只活在内存里 —— 新一批失败会把它盖掉、启动器重启会把它整个丢掉。账本里那位 `canRetry`
/// 是**失败那一刻钉下的历史事实**（★ 同样只活在内存里：`TaskHistory::toJson()` 不写
/// capabilities，重启读回来的历史每位都是默认 false —— 所以它连"跨重启"都撑不到），光看它的话，
/// 用户点开上一件失败任务，
/// 上面那颗「重试」点下去重做的是**后一批**的文件（或者重启后什么都不会发生）—— 一颗按了
/// 会做错事的按钮，比一颗没有的按钮糟得多。
///
/// @param failedBatchOwner 那一格失败清单现在**是谁的**（没有 = `Tasks::invalidTaskId()`）。
/// @param failedBatchHeld 那一格现在**还在不在**（内存里那份快照还在 = 重试点得动）。
inline bool retryOffered(const TaskSnapshot &task, TaskId failedBatchOwner, bool failedBatchHeld) {
    return task.capabilities.canRetry && isTerminalState(task.state)
           && failedBatchHeld && task.id == failedBatchOwner;
}

/// 这一项报得出百分比吗。`totalUnits <= 0` 就是**不可测** —— 返回 -1，调用方画不确定态。
///
/// 这条是验收里那句「无 Content-Length 的 Fabric 下载显示'已下载多少'，不显示假
/// 百分比」的唯一出处。四分五裂地各写一遍"总数小于等于 0 就别画百分号"迟早会漏一处。
inline int percentOf(const TaskSnapshot &task) {
    if (task.kind == TaskKind::Launch && !task.steps.isEmpty()) {
        if (task.state == TaskState::Succeeded) return 100;
        qsizetype completed = 0;
        while (completed < task.steps.size() &&
               task.steps.at(completed).state == TaskStepState::Done) {
            ++completed;
        }
        const double partial = completed < task.steps.size() && task.totalUnits > 0
            ? double(qBound<qint64>(0, task.doneUnits, task.totalUnits)) / double(task.totalUnits)
            : 0.0;
        return int(100.0 * (double(completed) + partial) / double(task.steps.size()));
    }
    if (task.totalUnits <= 0) {
        return -1;
    }
    const qint64 done = task.doneUnits < 0 ? 0 : task.doneUnits;
    const int percent = int(done * 100 / task.totalUnits);
    return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

/// 收起态那格"显示谁"的优先级（**数值小的先显示**）；返回 -1 = 这一项不参与收起态。
///
/// 按下面这一串从高到低排：`WaitingUser > Failed(未读) > Launch > Install/Repair >
/// Download > Export`。**这只是"收起时展示谁"，不会隐藏列表里的其他任务**（要求）。
///
/// @param unreadFailure 这一项是"用户还没看过的失败"。未读的失败比"正在启动"还靠前 ——
/// 用户没看见的那件事最该先被他看见；看过了就退回它那一类自己的位置。
/// 成功/取消/中断那三档**只在 `recentWindowMs` 之内**才露面（「完成任务短暂
/// 显示后收进历史」），由 `collapsedSummary()` 那边判，这里不管时间。
inline int summaryPriority(const TaskSnapshot &task, bool unreadFailure) {
    if (isActiveState(task.state)) {
        if (task.state == TaskState::WaitingUser) {
            return 0;
        }
        switch (task.kind) {
        case TaskKind::Launch:
        case TaskKind::ServerLaunch:
            return 2;
        case TaskKind::Install:
            return 3;
        case TaskKind::Download:
            return 4;
        case TaskKind::Import:
            return 3;  // 导入整合包 = "装一整套东西"，和安装同一档
        case TaskKind::Export:
        // `Backup` 和导出同一档：都是"本地打包一个目录"，只读盘、没有网络、
        // 也就没有"等着它"的那种紧迫感 —— 让位给上面那几档。
        case TaskKind::Backup:
            return 5;
        case TaskKind::Java:
        case TaskKind::Integrity:
        case TaskKind::Other:
        // `Crash` 是"一建出来就已经结束"的一项（见枚举上那句），走到这儿只可能是盘上
        // 读回来的一份坏数据；按最低那一档摆着，不抢任何一格。
        case TaskKind::Crash:
            return 6;
        }
        return 6;
    }
    if (task.state == TaskState::Failed) {
        // 未读的失败排第二（在 WaitingUser 之后、Launch 之前），看过的失败退到最后一档
        // —— 它还在（最近那个时间窗里），只是不再抢那一格。
        return unreadFailure ? 1 : 8;
    }
    return 8;  // Succeeded / Cancelled / Interrupted：只在"刚完成"那个窗口里露一下
}

/// 任务当前步骤的摘要。
struct TaskStepSummary {
    int index = -1;  // 0-based，无步骤时为 -1
    int total = 0;
    QString name;
};

/// 找出任务当前最值得展示的那一步。
inline TaskStepSummary currentStepOf(const TaskSnapshot &task) {
    TaskStepSummary result;
    const int total = task.steps.size();
    if (total <= 0) {
        return result;
    }
    result.total = total;
    for (int i = 0; i < total; ++i) {
        if (task.steps.at(i).state == TaskStepState::Running) {
            result.index = i;
            result.name = task.steps.at(i).name;
            return result;
        }
    }
    if (task.state == TaskState::Failed) {
        for (int i = 0; i < total; ++i) {
            if (task.steps.at(i).state == TaskStepState::Failed) {
                result.index = i;
                result.name = task.steps.at(i).name;
                return result;
            }
        }
    }
    if (isActiveState(task.state)) {
        for (int i = 0; i < total; ++i) {
            if (task.steps.at(i).state == TaskStepState::Pending) {
                result.index = i;
                result.name = task.steps.at(i).name;
                return result;
            }
        }
        result.index = total - 1;
        result.name = task.steps.at(total - 1).name;
        return result;
    }
    for (int i = total - 1; i >= 0; --i) {
        if (task.steps.at(i).state == TaskStepState::Done) {
            result.index = i;
            result.name = task.steps.at(i).name;
            return result;
        }
    }
    result.index = 0;
    result.name = task.steps.at(0).name;
    return result;
}

/// 收起态那一格要画的东西。
struct TaskSummary {
    TaskId id = invalidTaskId();  // 0 = 一个任务都没有（画「暂无后台任务」）
    TaskState state = TaskState::Queued;
    TaskKind kind = TaskKind::Other;
    QString title;
    QString detail;
    QString currentStep;
    int currentStepIndex = -1;
    int stepCount = 0;
    int percent = -1;      // < 0 = 不可测
    int extraCount = 0;    // 「另有 N 项」（除了显示的这一项以外，还有几项在进行中）
    bool hasFailure = false;  // 红点：用户还没看过的失败任务存在
};

/**
 * @brief 收起态显示谁 —— **唯一出处**。
 *
 * @param tasks 账本里所有任务的快照（顺序不限）。
 * @param unreadFailures 用户**还没看过**的那几件失败任务的号。
 * @param now 现在。**传进来是为了可测** —— 这一层不看时钟。
 * @param recentWindowMs 「刚完成」还能占着那一格多久（「完成任务短暂显示后收进历史」）。
 * 传 0 = 完全不显示已完成的任务。
 *
 * 规则（那三条，逐条对上）：
 * · 一项都没有 ⇒ `id == 0`（调用方画低强调度的「暂无后台任务」，**不画空进度条**）；
 * · 有活动的 ⇒ 取优先级最高的那一项；同一档取**后发起的**（同一档里后点的那件事才是
 * 用户刚做的）；剩下的活动任务数写进 `extraCount`；
 * · 红点看的是**有没有未读的失败**，跟显示的是哪一项无关 —— 正在下载而上一件事失败了，
 * 两个信号都该看得见。
 */
inline TaskSummary collapsedSummary(const QList<TaskSnapshot> &tasks,
                                    const QSet<TaskId> &unreadFailures,
                                    const QDateTime &now,
                                    qint64 recentWindowMs) {
    TaskSummary summary;

    int activeCount = 0;
    for (const TaskSnapshot &task : tasks) {
        if (isActiveState(task.state)) {
            ++activeCount;
        }
        // 只认账本里**还在**的那几件失败：截断（pruneFinished）丢掉的老失败号可能还留在未读集合里，
        // 它在面板里没有行、永远标不成「看过」—— 算它的话红点会挂到重启，而底栏那格也无事可显示。
        if (task.state == TaskState::Failed && unreadFailures.contains(task.id)) {
            summary.hasFailure = true;
        }
    }

    const TaskSnapshot *best = nullptr;
    int bestPriority = 0;
    QDateTime bestStarted;
    for (const TaskSnapshot &task : tasks) {
        int priority = -1;
        const bool unreadFailure =
            (task.state == TaskState::Failed) && unreadFailures.contains(task.id);
        if (isActiveState(task.state) || unreadFailure) {
            priority = summaryPriority(task, unreadFailure);
        } else if (task.state == TaskState::Succeeded || task.state == TaskState::Failed
                   || task.state == TaskState::Cancelled
                   || task.state == TaskState::Interrupted) {
            // 「刚完成」那一档：只在时间窗里露面。没时间戳（理论上不会）就当它早过期。
            const bool recent =
                recentWindowMs > 0 && task.finishedAt.isValid()
                && task.finishedAt.msecsTo(now) >= 0 && task.finishedAt.msecsTo(now) <= recentWindowMs;
            if (recent) {
                priority = summaryPriority(task, unreadFailure);
            }
        }
        if (priority < 0) {
            continue;
        }
        if (best == nullptr || priority < bestPriority
            || (priority == bestPriority && task.startedAt > bestStarted)) {
            best = &task;
            bestPriority = priority;
            bestStarted = task.startedAt;
        }
    }

    if (best == nullptr) {
        return summary;
    }
    summary.id = best->id;
    summary.state = best->state;
    summary.kind = best->kind;
    summary.title = best->title;
    summary.detail = best->detail;
    const TaskStepSummary step = currentStepOf(*best);
    summary.currentStep = step.name;
    summary.currentStepIndex = step.index;
    summary.stepCount = step.total;
    summary.percent = percentOf(*best);
    // 「另有 N 项」数的是**还在进行中**的那几项（显示的这一项是活动的话要减掉它自己）。
    summary.extraCount = activeCount - (isActiveState(best->state) ? 1 : 0);
    if (summary.extraCount < 0) {
        summary.extraCount = 0;
    }
    return summary;
}

/// 同步等待期间与当前账本摘要的合并口径（纯函数）。
/// 界面此时确实阻塞，因此底栏展示 `syncWaitStatus` 文案；只要账本里存在活动任务且
/// 报得出百分比（`percent >= 0`），进度环保留确定百分比，不用 -1 覆盖。刚完成的最近任务
/// 不参与同步等待的进度借用（保持 -1）。
inline TaskSummary applySyncWait(const TaskSummary &base, const QString &syncWaitStatus) {
    if (syncWaitStatus.isEmpty()) {
        return base;
    }
    const bool activeTask = (base.id != invalidTaskId() && isActiveState(base.state));
    TaskSummary merged;
    merged.hasFailure = base.hasFailure;
    merged.extraCount = activeTask ? base.extraCount : 0;
    merged.state = TaskState::Running;
    merged.title = syncWaitStatus;
    if (activeTask) {
        merged.id = base.id;
        merged.kind = base.kind;
        merged.detail = base.detail;
        merged.currentStep = base.currentStep;
        merged.currentStepIndex = base.currentStepIndex;
        merged.stepCount = base.stepCount;
        merged.percent = base.percent >= 0 ? base.percent : -1;
    }
    return merged;
}

}  // namespace Tasks
