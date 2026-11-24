/**
 * @file TaskManager.hpp
 * @brief 任务账本：**唯一**记录"现在有哪些事在跑、各自跑到哪儿了"的地方。
 *
 * 核心要求：「不把任务中心做成第二套业务逻辑 —— 任务中心只**聚合状态和
 * 操作**，不负责真正下载或启动」。所以这个类里没有一条网络、没有一个进程、不碰任何业务
 * 规则：`DownloadManager` / `VersionManager` / `LauncherManager` 照旧是执行者，`MainWindow`
 * 负责把它们的信号翻成这里的一次 `begin()` / `setProgress()` / `succeed()`。
 *
 * 【它管什么】
 *
 * * 发号（`begin()` 回一个 `TaskId`，之后所有更新都按这个号认人）；
 * * 按 id 改一项任务的某几个字段，并把"变了"告诉外面（`taskChanged`）；
 * * **终态只能落一次**（`succeed()` / `fail()` / `cancel()` 第二次调用什么都不做、
 * 一个信号都不发）—— 验收那条「相同信号重复抵达不会生成重复任务或
 * 重复完成」就是这一条；
 * * 落盘/读回任务历史（`TaskHistory`），读回来的那次把没跑完的标成"已中断"。
 *
 * 【为什么不认识界面】
 *
 * `ui/` **不 include `src/`**（见 `ui/MainWindowUI.hpp` 与 `src/MainWindow.cpp` 里那几段）。
 * 所以界面那边拿到的不是 `TaskSnapshot`，而是 `MainWindow` 翻好的 `TaskCenterRowData`
 * （见 `ui/TaskCenterPanel.hpp`）。这个类里也就一个汉字都没有 —— 状态该写成什么词是界面
 * 的事，不是账本的事。
 *
 * 【要不要构造它】
 *
 * 不用：它只在 `MainWindow` 里有一个（`m_taskManager`）。但**它一个 QWidget 都不碰、
 * 一个网络都不开**，所以验证工程可以直接 `new` 它、喂几条假任务进去，把上面那几条规则
 * 钉死（见 tests/main.cpp 的 AM 节）。
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "TaskId.hpp"
#include "TaskSnapshot.hpp"

namespace Tasks {

class TaskManager : public QObject {
    Q_OBJECT

public:
    explicit TaskManager(QObject *parent = nullptr);
    ~TaskManager() override;

    /// 历史文件落在哪。默认是 `defaultHistoryPath()`（启动器级固定路径）。
    ///
    /// NOTE: 验证工程会把它指到一个临时目录 —— 不然那几条断言会往真数据目录里写东西。
    /// `setHistoryPath(QString())` 是"回到默认那份"，不是"写到空路径去"。
    QString historyPath() const { return resolvedHistoryPath(); }
    void setHistoryPath(const QString &path) { m_historyPath = path; }

    /// 从盘上读一份历史，**启动时调一次**。会把当前账本整个换成盘上那一份
    /// （读回来的没跑完的都成了 `Interrupted`）。回读到了几条。
    int loadHistory();

    /// 把账本写回去（已完成的按 `finishedHistoryLimit()` 截断）。
    bool saveHistory() const;

    // ---------- 发起 / 推进 / 收尾 ----------

    /// 记一件新任务。回它的号（`invalidTaskId()` 那种情况不会出现：号是自增的）。
    ///
    /// @param steps 一开始那几步（可以先给空表，跑到哪一步再 `setStepState()`）。
    /// @param capabilities 它**允许**什么操作（见 `TaskCapabilities` 那段：按钮不靠种类猜）。
    TaskId begin(TaskKind kind, const QString &title, const QString &targetId = QString(),
                 const QList<TaskStep> &steps = QList<TaskStep>(),
                 const TaskCapabilities &capabilities = TaskCapabilities(), bool queued = false);
    /// 以 @p parent 的第 @p parentStep 步为归属记一件**子任务**。
    ///
    /// 子任务**不进** `active()` / `finished()` / `all()`：任务中心只应出现一个父任务
    /// （一次"升级实例"里准备核心、复制、快照都是它的步骤，不是另外三件事）。子任务的
    /// 细节和字节进度会镜像到父任务上，收尾时也只改父任务那一步的状态 —— 父任务自己
    /// 什么时候收尾由串起这些子任务的流程决定（子任务失败不等于父任务已经收完尾：那一边
    /// 可能还要回滚或保留恢复材料）。
    ///
    /// @param parent 必须是一个活着的父任务；认不出来时返回 `invalidTaskId()`，
    /// 调用方该退回 `begin()` 而不是把这次操作丢掉。
    TaskId beginChild(TaskId parent, TaskKind kind, const QString &title,
                      const QString &targetId = QString(),
                      const QList<TaskStep> &steps = QList<TaskStep>(),
                      const TaskCapabilities &capabilities = TaskCapabilities(),
                      int parentStep = -1);
    bool startQueued(TaskId id);
    bool pause(TaskId id);
    bool queueRecovery(TaskId id);

    /// 换一句"现在在干什么"（面板里标题下面那行小字）。
    bool setDetail(TaskId id, const QString &detail);

    /// 更正任务标题与目标标识（例如一键安装续装冲突改名后同步目标实例）。
    bool setTitleAndTarget(TaskId id, const QString &title, const QString &targetId);

    /// 启动那一件任务是**在进程起来之前**记的，那时会话号还不存在 ⇒ 只能事后补这一笔。
    /// `sessionId <= 0` 什么都不做。已经对上过的那一项**不再改口**（一项任务只属于一份
    /// 会话）。**不落盘**（见 `TaskSnapshot::sessionId`）。
    bool setSessionId(TaskId id, int sessionId);
    bool acknowledgeAttention(TaskId id);
    bool markFailureSeen(TaskId id);

    /// 整表换掉步骤。
    bool setSteps(TaskId id, const QList<TaskStep> &steps);

    /// 改第 @p index 步的状态（越界的下标什么都不做、回 false）。
    bool setStepState(TaskId id, int index, TaskStepState state,
                      const QString &note = QString());

    /// 记单位进度（文件数 / 条目数）。`totalUnits <= 0` = **不可测**，界面画不确定态。
    bool setProgress(TaskId id, qint64 doneUnits, qint64 totalUnits);

    /// 记字节进度。下载那一类两样都记：百分比按单位算，详情里那句"2.8 MB / 18 MB"按字节。
    bool setBytes(TaskId id, qint64 bytesDone, qint64 bytesTotal);

    /// 改某一项**允许**什么操作。装上加载器之后中途变成"不能取消了"就是这么来的）。
    bool setCapabilities(TaskId id, const TaskCapabilities &capabilities);

    /// 给一项已经收尾的任务挂上"继续未完成操作"的入口，以及那份报告的钥匙。
    /// 只对**已收尾**的任务有效：还在跑的那一项没有"继续"这回事。
    bool setRecovery(TaskId id, const QString &recoveryToken, bool canRecover,
                     const QString &blockedReason = QString());

    /// 收尾三兄弟。**终态只落一次**：第二次调用回 false 且一个信号都不发。
    bool succeed(TaskId id);
    bool fail(TaskId id, const QString &errorSummary);
    bool cancel(TaskId id);

    /// 用户点了「清除已完成」：把终态那些从账本里删掉（并落盘）。
    void clearFinished();

    // ---------- 读 ----------

    /// 还在跑的（按发起的先后）。面板上面那一栏就是它。
    QList<TaskSnapshot> active() const;
    /// 已经收尾的（**最近的在前**）。面板下面那一栏就是它。
    QList<TaskSnapshot> finished() const;
    /// 全部（活动在前、收尾的在后，各自内部有序）——给"两个都要"的读者。
    QList<TaskSnapshot> all() const;
    /// 挂在 @p parent 底下的子任务（按发起先后）。**不进任务中心**，只给流程和验证用。
    QList<TaskSnapshot> childrenOf(TaskId parent) const;

    TaskSnapshot snapshot(TaskId id) const;
    bool has(TaskId id) const;

signals:
    /// 某一项**变了**（包括刚建出来那一下）。带号，外面可以只重画那一行。
    void taskChanged(TaskId id);
    /// 某一项收了尾。@p success 是 `Succeeded` 那一档（取消/中断都不算成功）。
    void taskFinished(TaskId id, bool success);
    /// "已完成"那一栏的内容变了（收尾、清除、读盘）。
    void historyChanged();

private:
    /// 这一趟到底写哪个文件：`m_historyPath` 非空就用它，空就用启动器级的默认路径。
    /// 测试仍可通过 setHistoryPath() 把账本隔离到临时目录。
    QString resolvedHistoryPath() const;
    /// 按号找一项。找不到回 nullptr。
    TaskSnapshot *find(TaskId id);
    /// 发一个新号（推进 `m_nextId`，并避开盘上已经有的号）。号用尽时回 `invalidTaskId()`。
    TaskId allocateId();
    /// 收尾的公共部分（三个入口都走它）。
    bool finish(TaskId id, TaskState state, const QString &errorSummary);
    /// 子任务的状态变化往父任务那一格上镜（详情 / 字节 / 那一步的状态）。
    /// NOTE: **不改父任务的终态** —— 子任务失败不等于这一件意图已经收完尾。
    void propagateChild(const TaskSnapshot &child);
    /// 把内存里超量的终态项按**落盘那份口径**丢掉（留最近的 `finishedHistoryLimit()` 条）。
    ///
    /// NOTE: 不剪的话面板会一直长（跑一天的批量下载能攒几百条），而重启之后只剩 50 条 ——
    /// 用户看到的是"昨天那件失败的任务今天找不到了"，却没有任何一处说过会这样。
    /// 剪的时机是 `finish()`（刚多出一条终态时），活动中的任务一条都不动。
    void pruneFinished();
    /// 变了之后该干的事：落盘（需要的话）再发 `taskChanged`。
    void touch(TaskId id, bool persist);

    QList<TaskSnapshot> m_tasks;
    TaskId m_nextId = 1;
    QString m_historyPath;
};

}  // namespace Tasks
