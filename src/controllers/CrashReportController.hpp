#pragma once

#include "diag/GameExitDiagnosis.hpp"
#include "tasks/TaskId.hpp"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>

#include <functional>

class ActivityCenterController;
class OverlayHost;
class QWidget;

class CrashReportController final : public QObject {
    Q_OBJECT

public:
    struct LaunchFacts {
        QString instanceId;
        QString instanceName;
        QString accountName;
        qint64 processId = 0;
        qint64 startedAtMs = 0;
        int requiredJavaMajor = 0;
        QString javaPath;
        int xmxMb = 0;
        int xmsMb = 0;
        QString loaderName;
        QString loaderVersion;
    };

    struct Host {
        std::function<void(const QString &)> log;
    };

    CrashReportController(ActivityCenterController *activity, OverlayHost *overlay,
                          QWidget *dialogParent, Host host, QObject *parent = nullptr);

    Tasks::TaskId report(int sessionId, int exitCode, bool crashed,
                         const LaunchFacts &launch, const QString &errorLog,
                         const QString &fullLog);
    QSet<Tasks::TaskId> recordIds() const;
    bool hasRecord(Tasks::TaskId id) const;

    /// 账上**还没处理掉**的那几笔异常退出 —— 底栏那颗红方块的判据就是"这个空不空"。
    ///
    /// 【为什么不是 recordIds()】
    ///
    /// `m_records` 只在 `report()` 里顺手清一遍（见那个函数开头那两行），所以它**留着已经
    /// 作废的号**是常态；而"用户处理掉没有"这件事只有账本说得清（`Tasks::TaskManager::has()`：
    /// 清掉那一项 —— 任务中心里那颗「清理」走的就是 `clearFinished()` —— 号就从账本里没了）。
    /// 于是这里的每一项都要过一遍账本，回出去的是**两处都还在**的那些。
    QSet<Tasks::TaskId> pendingRecordIds() const;

    /// 上面那几笔里**最靠后**那一笔的实例显示名（底栏第二行写它）。没有 pending 的回空串。
    ///
    /// 用户 定的是「显示最后启动的那个实例的状态」：几件事同时摆着的时候，
    /// 说话的是**最近那一个**。号是单调递增的（见 `Tasks::TaskId` 的文件头），所以"最靠后"
    /// 就是号最大的那一个，不用另存时间戳。
    QString latestPendingInstanceName() const;

    int sessionIdForTask(Tasks::TaskId id) const;
    void exportReport(const QString &key);
    void openReportsDirectory();

private:
    struct Record {
        GameExitDiagnosis::Facts facts;
        int sessionId = 0;
        QString accountName;
        qint64 processId = 0;
        QDateTime startedAt;
        QString errorExcerpt;
        QString logTail;
        int errorLines = 0;
        int totalLines = 0;
    };

    Record buildRecord(int sessionId, int exitCode, bool crashed,
                       const LaunchFacts &launch, const QString &errorLog,
                       const QString &fullLog) const;
    static int lineCount(const QString &text);
    static QString lastLines(const QString &text, int maxLines);
    static QString exportDirectory();
    static QString writeExportFile(const QString &path, const QString &text);
    void log(const QString &message) const;

    ActivityCenterController *m_activity = nullptr;
    OverlayHost *m_overlay = nullptr;
    QWidget *m_dialogParent = nullptr;
    Host m_host;
    QHash<Tasks::TaskId, Record> m_records;
};
