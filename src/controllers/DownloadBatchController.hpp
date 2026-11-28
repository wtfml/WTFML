#pragma once
#include "tasks/TaskId.hpp"
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <functional>
#include <atomic>
#include <memory>

class ActivityCenterController;
class DownloadManager;

// Connects file checkpoints to the activity ledger. It never reconstructs a Java process.
//
// NOTE: `Q_OBJECT` 是为了 `tr()`。没有它的话 `tr` 落到 `QObject::tr`，译文上下文是
// "QObject"，而 `assets/i18n/*.ts` 里这些条目在 "DownloadBatchController" 下 ——
// 英文界面里会一直显示源文。
class DownloadBatchController final : public QObject {
    Q_OBJECT
public:
    DownloadBatchController(ActivityCenterController *activity, DownloadManager *downloads,
        std::function<Tasks::TaskId()> activeTask, QObject *parent = nullptr);
    ~DownloadBatchController() override;
    void load();
    bool owns(Tasks::TaskId id) const;
    bool canCancel(Tasks::TaskId id) const;
    bool pause(Tasks::TaskId id);
    /// 继续这一项（暂停中的批次 / 失败的批次）。@param problemOut 拒绝时回填原因 ——
    /// 界面拿它说一句，否则用户看到的只是"按钮没反应"。
    bool resume(Tasks::TaskId id, QString *problemOut = nullptr);
    bool cancel(Tasks::TaskId id);
    void shutdown();
    bool isRestoring() const { return (m_restoring && m_current != Tasks::invalidTaskId()) || m_verifying != Tasks::invalidTaskId(); }
    void setRestoreAllowed(std::function<bool()> check) { m_restoreAllowed = std::move(check); }
    void setContinuation(std::function<QJsonObject()> snapshot, std::function<bool(Tasks::TaskId, const QJsonObject &)> resume,
        std::function<bool(const QJsonObject &, const std::atomic_bool *)> validate = {},
        std::function<QJsonObject(const QJsonObject &, const std::atomic_bool *)> targets = {}) {
        m_continuationSnapshot = std::move(snapshot); m_continue = std::move(resume);
        m_continuationValidation = std::move(validate);
        m_continuationTargets = std::move(targets);
    }
private:
    void sync();
    void complete(bool success);
    /// 把"接续意图里那份要读盘的快照"（`presetTargets`）挪到后台去算，算完补进恢复记录。
    ///
    /// 为什么挪：那一拍要把目标实例的配置目录整棵走一遍、每一份整文件算 SHA-256
    /// （resourcepacks/ 和 shaderpacks/ 里的文件是几十上百 MB 的），而它原来是在
    /// `batchStarted` 里**同步**拍的 —— 就是用户按下"安装"、第一份文件开始下的那一刻。
    /// 代价量在 `tests/core_modules/PresetSnapshotScaleTests.cpp`。
    void requestContinuationTargets(Tasks::TaskId id, const QString &path, const QJsonObject &intent);
    void stopContinuationTargets();
    ActivityCenterController *m_activity;
    DownloadManager *m_downloads;
    std::function<Tasks::TaskId()> m_activeTask;
    std::function<bool()> m_restoreAllowed;
    std::function<QJsonObject()> m_continuationSnapshot;
    std::function<bool(Tasks::TaskId, const QJsonObject &)> m_continue;
    std::function<bool(const QJsonObject &, const std::atomic_bool *)> m_continuationValidation;
    std::function<QJsonObject(const QJsonObject &, const std::atomic_bool *)> m_continuationTargets;
    std::shared_ptr<std::atomic_bool> m_targetsCancel;
    quint64 m_targetsGeneration = 0;
    QHash<Tasks::TaskId, QString> m_records;
    QString m_root;
    Tasks::TaskId m_current = Tasks::invalidTaskId();
    Tasks::TaskId m_verifying = Tasks::invalidTaskId();
    std::shared_ptr<std::atomic_bool> m_verifyCancel;
    quint64 m_verifyGeneration = 0;
    bool m_restoring = false, m_stopping = false;
};
