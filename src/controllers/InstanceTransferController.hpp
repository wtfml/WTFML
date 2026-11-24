#pragma once

#include "operations/OperationReport.hpp"
#include "operations/InstanceUpgrade.hpp"
#include "tasks/TaskId.hpp"
#include "tasks/TaskSnapshot.hpp"

#include <QHash>
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>

class LauncherManager;
class OverlayHost;
class VersionManager;

class InstanceTransferController final : public QObject {
    Q_OBJECT
public:
    struct Host {
        std::function<bool(const QString &)> isBeingModified;
        std::function<QString(const QString &)> displayName;
        std::function<QString(const QString &, const QString &)> instanceDirForKey;
        std::function<void()> refreshInstances;
        std::function<void(const QString &)> log;
        /// 后台拷树期间把源实例记进/摘出「正在搬文件」名单（产品那边就是
        /// `FileOperationOccupancy`（主窗口那份状态））。不记的话，复制跑到一半用户
        /// 照样能启动这一份 —— 游戏一边写存档、拷树一边读，副本里就是半截世界。
        std::function<void(const QString &, bool)> setFileOperationBusy;
        // 任务中心那一格（「不在 UI 线程直接复制整个目录」）。全可空。
        std::function<Tasks::TaskId(Tasks::TaskKind, const QString &, const QString &,
                                    const QList<Tasks::TaskStep> &)>
            beginTask;
        /// 在父任务的第 N 步底下记一件**子任务**（任务中心只出现父任务）。
        /// 父任务认不出来时回 `invalidTaskId()`，控制器会退回 `beginTask`。
        std::function<Tasks::TaskId(Tasks::TaskId, Tasks::TaskKind, const QString &,
                                    const QString &, const QList<Tasks::TaskStep> &)>
            beginChildTask;
        std::function<void(Tasks::TaskId, const QString &)> setTaskDetail;
        std::function<void(Tasks::TaskId, qint64, qint64)> setTaskProgress;
        std::function<void(Tasks::TaskId, qint64, qint64)> setTaskBytes;
        std::function<void(Tasks::TaskId, int, Tasks::TaskStepState)> setTaskStepState;
        /// 改某一项**允许**什么操作。目前只用来开「取消」和写「为什么不能取消」——
        /// `beginTask` 没有能力位参数，而账本的默认位是**按种类**给的。
        std::function<void(Tasks::TaskId, const Tasks::TaskCapabilities &)> setTaskCapabilities;
        std::function<void(Tasks::TaskId, bool, const QString &)> finishTask;
        /// 收尾后挂上「继续未完成操作」（报告钥匙）。没有未完成项时传空串摘掉入口。
        std::function<void(Tasks::TaskId, const QString &)> setTaskRecovery;
        /// 登记一份 `OperationReport`（写盘 + 进恢复表）。回 false = 没登记上（只影响恢复入口，
        /// 不影响这一次操作本身的结果）。
        std::function<bool(const OperationReport::Report &)> registerRecovery;
        /// 用户按了「取消」、工人随即停下之后，把账本里那一项收成 Cancelled
        /// （**不是** Failed：取消不算失败，不该亮红点、也不该弹失败提示）。
        std::function<void(Tasks::TaskId)> markTaskCancelled;
    };
    InstanceTransferController(VersionManager *versions, LauncherManager *launcher,
                               OverlayHost *overlay, Host host, QObject *parent = nullptr);

    /// 「复制实例」**同步**那一版：四道闸 + 拷树都在调用线程里做完，回"成没成"。
    ///
    /// NOTE: 验证工程（`mw_check` / 升级副本）按同步约定钉着它；界面上的那一颗「复制实例」
    /// 走下面那个后台版 —— 拷一整个实例动辄几十秒，不能把 UI 线程按住。
    bool copyInstance(const QString &sourceId, const QString &destGameFolder,
                      const QString &newName);

    /// 同一套闸，但**拷树在后台线程**（见 .cpp 里那段 NOTE）。完成之后回调回到主线程；
    /// 任务中心那一项会把「规划 → 执行 → 验证」三步走完。
    void copyInstanceInBackground(const QString &sourceId, const QString &destGameFolder,
                                  const QString &newName,
                                  std::function<void(bool, const QString &)> whenDone = {},
                                  bool announce = true);

    /// 后台那一份还在拷吗（再按一次「复制实例」会被挡下来）。
    bool copying() const { return m_copyBusy; }
    /// 原地升级：完整 ZIP 快照 → 校验 → 保留原目录 → 发布 → 校验。
    /// @param parentTask 非 0 时把执行的子任务挂到那一项任务底下（任务中心只露父任务）。
    /// @param stageParentSteps 引擎阶段 ⇒ 父任务第几步（可空）。
    void upgradeInPlace(QString sourceId, QString targetId,
                        std::function<void(bool, const QString &)> whenDone = {},
                        const QStringList &reviewedKeptMods = {}, bool reviewedModPlan = false,
                        Tasks::TaskId parentTask = Tasks::invalidTaskId(),
                        const QHash<int, int> &stageParentSteps = {},
                        std::function<void(bool ok, bool cancelled)> whenSettled = {},
                        const QList<InstanceUpgrade::ModReplacement> &replacements = {});
    /// 「创建更新副本」：**源实例保持不动**，只复制一次候选目录、完整校验后换核心并发布。
    ///
    /// 这是 2.1.1 的空间优化：不再为副本另做整份 ZIP 快照（源实例本身可回滚），
    /// 但复制后与发布后的逐文件校验都保留。失败/取消时暂存会被收掉，源实例不受影响。
    void upgradeToCopy(const QString &sourceId, const QString &newName,
                       const QString &targetCoreId,
                       std::function<void(bool, const QString &)> whenDone = {},
                       const QStringList &reviewedKeptMods = {}, bool reviewedModPlan = false,
                       Tasks::TaskId parentTask = Tasks::invalidTaskId(),
                       const QHash<int, int> &stageParentSteps = {},
                       std::function<void(bool ok, bool cancelled)> whenSettled = {},
                       const QList<InstanceUpgrade::ModReplacement> &replacements = {},
                       const QStringList &copyRoots = {});
    void rollbackUpgrade(QString sourceId,
                         std::function<void(bool, const QString &)> whenDone = {});

    bool importWorld(const QString &instanceId, const QString &worldDir, const QString &name);
    /// 界面入口：复制在工作线程执行，完成后才刷新；同步入口保留给兼容调用。
    void importWorldInBackground(const QString &instanceId, const QString &worldDir,
                                 const QString &name,
                                 std::function<void(bool, const QString &)> whenDone = {});
    /// 「删除世界」那一趟：确认过了才进来，整棵目录树在工作线程里**移进回收站**。
    ///
    /// NOTE: 删除**不提供取消**。删到一半停下留下的是"半截世界"，比删完更难收拾；
    /// 账本按 `TaskKind::Other` 记账，并且明写「不能取消」的原因（见 `beginTask` 之后那一发）。
    /// 移不进去时照实报失败，并把原因（`moveToRecycleBin()` 给的那一句）一起带出去。
    void deleteWorldInBackground(const QString &instanceId, const QString &worldDir,
                                 std::function<void(bool, const QString &)> whenDone = {});
    /// 任务中心那一项此刻按「取消」真的停得下来吗（那一颗按钮的唯一判据）。
    bool canCancelTask(Tasks::TaskId id) const;
    /// 掐掉一次后台导入 / 原地升级 / 回滚。三处都是**协作式**取消：
    /// · 导入：工人每 1 MiB 看一次取消位；已经发布到 saves 的正式世界不会被删。
    /// · 原地升级：引擎在每个安全点看取消位，停下时原实例未改变（或已自动恢复）；
    ///   仅最终改名那一下不可中断，按下之后最多晚到这个阶段末尾。
    bool cancelTask(Tasks::TaskId id);
    bool renameWorld(const QString &instanceId, const QString &worldDir, const QString &newName);

private:
    void runUpgradeJob(const InstanceUpgrade::Request &request, const InstanceUpgrade::Core &core,
                       const QStringList &lockedIds, bool restore,
                       std::function<void(bool, const QString &)> whenDone,
                       Tasks::TaskId parentTask = Tasks::invalidTaskId(),
                       const QHash<int, int> &stageParentSteps = {},
                       const QString &copySourceDir = QString(),
                       std::function<void(bool ok, bool cancelled)> whenSettled = {});
    QString displayName(const QString &id) const;
    void log(const QString &text) const;
    void announceCopySuccess(const QString &sourceId, const QString &destGameFolder,
                             const QString &name);

    /// 一次后台长活（导入世界 / 原地升级 / 回滚）的取消状态。
    /// 工人只碰这两个原子位，主线程管这张表。
    struct CancelState {
        std::atomic_bool abort{false};
        /// 「是用户按的取消」和「启动器要关了 / 超时」是两回事：前者收成 Cancelled、
        /// 不弹失败提示；后者照旧按失败说清楚。
        std::atomic_bool cancelledByUser{false};
    };

    VersionManager *m_versions = nullptr;
    LauncherManager *m_launcher = nullptr;
    OverlayHost *m_overlay = nullptr;
    Host m_host;
    bool m_copyBusy = false;
    Tasks::TaskId m_copyTaskId = Tasks::invalidTaskId();
    QHash<Tasks::TaskId, std::shared_ptr<CancelState>> m_cancelStates;
};

