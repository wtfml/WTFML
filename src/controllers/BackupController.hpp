#pragma once

#include "tasks/TaskId.hpp"

#include <QAtomicInt>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <functional>

class ActivityCenterController;
class InstanceSettingsDialog;
class OverlayHost;
class QThread;
class VersionManager;
namespace BackupFiles {
enum class Kind;
struct Manifest;
struct WriteResult;
struct RestoreResult;
}

class BackupController final : public QObject {
    Q_OBJECT

public:
    struct Host {
        std::function<QString(const QString &)> displayName;
        std::function<QString(const QString &)> instanceIdOfPath;
        std::function<bool(const QString &)> isRunning;
        std::function<bool(const QString &)> isBeingModified;
        std::function<void(const QString &)> log;
        std::function<void(InstanceSettingsDialog *, const QString &)> refreshWorld;
        std::function<void(const QString &)> refreshServer;
        /// 记 / 摘"这一份实例正在搬文件"（唯一出处是 `MainWindow::installingVersionIds()`）。
        /// 还原一份实例会把整个目录**换掉**、快照 / 世界备份会把整个目录**读一遍**，
        /// 这两类期间它都不能被启动、改名或删掉。
        std::function<void(const QString &, bool)> setFileOperationBusy;
        /// 盘上的实例清单变了（还原之后 meta / 图标 / json 全都是快照里的那一份）。
        std::function<void()> refreshInstances;
        /// 记 / 摘"这个**服务端**的世界正在备份或还原"。世界还原要把世界目录整个换掉、
        /// 世界备份要把它整个读一遍，这一趟期间它不能启动、不能删。
        /// 和上面那一格分开：上面那一位落进的是 `installingVersionIds()`（实例的世界），
        /// 服务端不是实例 —— 拿实例 id 去记的话，那一份服务端根本没人认。
        std::function<void(const QString &, bool)> setServerFileOperationBusy;
    };

    BackupController(VersionManager *versions, ActivityCenterController *activity,
                     OverlayHost *overlay, Host host, QObject *parent = nullptr);
    ~BackupController() override;

    bool busy() const;
    void shutdown();
    void openFolder(BackupFiles::Kind kind);
    void inspectRecovery();
    void backupWorld(const QString &worldDir);
    void restoreWorld(const QString &worldDir, InstanceSettingsDialog *dialog = nullptr);
    void snapshotInstance(const QString &versionId);
    /// 从快照还原一整个实例。
    ///
    /// @param dialog 从实例设置页点进来时那一页（可为空）。还原会把目录里的文件整个
    /// 换掉，而那一页的控件里还是**还原之前**的值 —— 留着它，用户随便改一格就会
    /// 把旧值写回去。所以还原期间它先被禁用，动过盘之后直接收掉。
    void restoreInstance(const QString &versionId, InstanceSettingsDialog *dialog = nullptr);

    Tasks::TaskId startWrite(const BackupFiles::Manifest &manifest, const QString &sourceDir,
                             const QString &targetZip, const QString &title,
                             const QString &targetId, const QString &successTitle,
                             const QString &refreshServerId = QString(),
                             const QString &failureTitle = QString());
    Tasks::TaskId startRestore(const QString &zipPath, const QString &targetParentDir,
                               const QString &title, const QString &targetId,
                               const QString &successTitle,
                               const QString &refreshServerId = QString(),
                               const QString &failureTitle = QString());
    /// 任务中心那一项此刻按「取消」停不停得下来（那一颗按钮的唯一判据）。
    /// 只认**正在跑的这一趟**：`m_taskId` 之外的一律 false。
    bool canCancelTask(Tasks::TaskId id) const;
    /// 用户按的取消。引擎在快照 / 解压 / 校验与提交前检查处停下；仅最终改名那一段
    /// 不可中断。停下之后暂存由引擎自己清，盘上不会留下半成品。
    bool cancelTask(Tasks::TaskId id);

private:
    static QString sizeText(qint64 bytes);
    static QString pickLine(const QString &zipPath, const QString &from = QString());
    void pickZip(BackupFiles::Kind kind, const QString &label, const QString &what,
                 std::function<void(QString)> selected);
    bool m_scanning = false;
    QString displayName(const QString &id) const;
    void log(const QString &message) const;
    void writeFinished(const BackupFiles::WriteResult &result, const QString &successTitle,
                       const QString &failureTitle);
    void restoreFinished(const BackupFiles::RestoreResult &result, const QString &successTitle,
                         const QString &failureTitle);

    VersionManager *m_versions = nullptr;
    ActivityCenterController *m_activity = nullptr;
    OverlayHost *m_overlay = nullptr;
    Host m_host;
    Tasks::TaskId m_taskId = Tasks::invalidTaskId();
    QThread *m_thread = nullptr;
    QAtomicInt m_abort{0};
    /// 「用户按的取消」和「启动器要关了（`m_abort` 只由 `shutdown()` 置）」是两回事：
    /// 前者要照常收尾（摘忙标记、账本记 Cancelled、不弹失败框），后者一个字都不许碰界面。
    std::atomic_bool m_userCancelled{false};
    QString m_refreshWorldDir;
    QString m_refreshServerId;
    /// 这一趟记下的"哪个服务端的世界正在被读写"（服务端备份 / 世界还原）。
    /// 和 `m_refreshServerId` 分开：那一格只是"收尾时要不要重摆那一页"，这一格决定
    /// 收尾时要不要把「动不得」的标记摘掉 —— 还原在动盘之前就失败的话，那一页不一定
    /// 会被刷新，但标记一定得摘。
    QString m_busyServerId;
    QPointer<InstanceSettingsDialog> m_refreshDialog;
    /// 还原那一趟记的"哪一份实例正在搬文件"（收尾时按它摘标记）。**实例还原与世界
    /// 还原共用这一格**：两者都要把 `versions/<id>/` 底下的东西整个换掉，都记同一个
    /// "忙"；`busy()` 保证它们不会同时跑。
    QString m_restoreInstanceId;
    /// 快照 / 世界备份那一趟正在**读**哪一份实例（收尾时按它摘标记）。
    /// 读的是整个目录树 —— 跑着的这十几秒里这一份实例如果被删 / 改名 / 启动，包里会
    /// 静默少掉那些文件，而收尾照样报"打好了一份"。所以它和复制 / 还原一样记进
    /// `MainWindow::installingVersionIds()`（那边是"谁动不得"的唯一出处）。
    QString m_writeInstanceId;
    /// 只有**实例**还原会设：那一页在还原之后全是旧值，收尾时直接收掉。世界还原不设
    /// （页面留着，只重列存档行）。
    QPointer<InstanceSettingsDialog> m_restoreDialog;
};
