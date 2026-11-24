#include "controllers/IntegrityController.hpp"

#include "controllers/ActivityCenterController.hpp"
#include "IntegrityReportPage.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "manage/VersionManager.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <utility>

IntegrityController::IntegrityController(VersionManager *versions,
                                         ActivityCenterController *activity,
                                         OverlayHost *overlay, QWidget *pageParent,
                                         Host host, QObject *parent)
    : QObject(parent), m_versions(versions), m_activity(activity), m_overlay(overlay),
      m_pageParent(pageParent), m_host(std::move(host)) {}

IntegrityController::~IntegrityController() { shutdown(); }

QString IntegrityController::displayName(const QString &id) const {
    return m_host.displayName ? m_host.displayName(id) : id;
}

void IntegrityController::log(const QString &message) const {
    if (m_host.log) m_host.log(message);
}

bool IntegrityController::scanning(const QString &versionId) const {
    return !versionId.isEmpty() && m_scanVersionId == versionId;
}

QStringList IntegrityController::flaggedPaths(const QString &versionId) const {
    // 报告只有**一份**（最近那一趟）。问的是别的实例时一个字都不回：把 A 的清单交给 B 的
    // 补全，会让 B 去重下几个跟它没关系的文件（而且那些路径在 B 那边根本对不上号）。
    if (versionId.isEmpty() || m_reportVersionId != versionId) {
        return QStringList();
    }
    return m_report.problemPaths();
}

void IntegrityController::shutdown() {
    m_abort.storeRelaxed(1);
    if (m_thread != nullptr) m_thread->wait();
    for (QThread *thread : findChildren<QThread *>()) {
        if (thread != m_thread) thread->wait();
    }
}

void IntegrityController::submit(const QString &versionId, const BusyFacts &busy) {
    if (versionId.isEmpty() || scanning(versionId)) return;
    if (!m_scanVersionId.isEmpty()) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
            tr("「%1」正在做完整性检查，请完成后再检查这个实例。")
                .arg(displayName(m_scanVersionId)));
        return;
    }
    if (busy.targetBeingModified) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
            tr("「%1」正在安装、导入或检查，请完成后再试。")
                .arg(displayName(versionId)));
        return;
    }
    if (busy.anotherDownloadRunning) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("已有下载或安装任务正在进行，请结束后再检查这个实例。"));
        return;
    }
    const QString inheritanceProblem = m_versions->inheritanceProblem(versionId);
    if (!inheritanceProblem.isEmpty()) {
        log(tr("无法检查 %1 的文件：版本继承链不完整").arg(versionId));
        InlineMessage::warning(m_overlay, tr("版本不完整"), inheritanceProblem);
        return;
    }
    QString planError;
    const QList<VersionIntegrity::ExpectedFile> files =
        m_versions->integrityPlan(versionId, &planError);
    if (!planError.isEmpty()) {
        log(tr("无法检查 %1 的文件：%2").arg(versionId, planError));
        InlineMessage::warning(m_overlay, tr("版本信息缺失"), planError);
        return;
    }
    QList<Tasks::TaskStep> steps;
    for (VersionIntegrity::Group group : VersionIntegrity::allGroups())
        steps.append({VersionIntegrity::groupName(group), Tasks::TaskStepState::Pending, {}});
    m_taskId = m_activity->begin(Tasks::TaskKind::Integrity,
                                 tr("检查「%1」的文件").arg(displayName(versionId)),
                                 versionId, steps);
    m_activity->tasks()->setProgress(m_taskId, 0, qint64(files.size()));
    log(tr("正在检查「%1」的文件（%2 个）…").arg(displayName(versionId)).arg(files.size()));
    m_scanVersionId = versionId;
    m_abort.storeRelaxed(0);
    if (m_thread != nullptr) {
        // **不能**在这里对旧的 `m_thread` 调 deleteLater()。它能被走到这儿，
        // 是因为上一趟的 `finished()` 已经把 `m_scanVersionId` 清掉了 —— 那条线程正是
        // 刚把 `finished()` 排到主线程、自己还差最后几条指令才退出。对一条仍在跑的
        // QThread 调 deleteLater() 会让 Qt 处理 DeferredDelete 时
        // `qFatal("QThread: Destroyed while thread is still running")` —— 整个启动器
        // 当场消失（ServerPing 那边 踩过同一个坑）。线程对象自己接好了
        // finished → deleteLater（见下面创建处），这里只要放掉指针。
        m_thread = nullptr;
    }
    m_thread = QThread::create([this, files] {
        constexpr int stride = 64;
        VersionIntegrity::Report report;
        report.checked = int(files.size());
        int groupIndex = 0;
        int lastReported = 0;
        for (int i = 0; i < files.size(); ++i) {
            if (m_abort.loadRelaxed() != 0) return;
            const VersionIntegrity::ExpectedFile &file = files.at(i);
            if (VersionIntegrity::Verdict verdict = VersionIntegrity::check(file); !verdict.ok())
                report.problems.append(verdict);
            const int currentGroup = VersionIntegrity::groupIndex(file.group);
            const bool boundary = currentGroup != groupIndex;
            groupIndex = currentGroup;
            if (boundary || i - lastReported >= stride || i + 1 == files.size()) {
                lastReported = i;
                QMetaObject::invokeMethod(this,
                    [this, checked = i + 1, total = int(files.size()), currentGroup] {
                        progress(checked, total, currentGroup);
                    }, Qt::QueuedConnection);
            }
        }
        QMetaObject::invokeMethod(this, [this, report] { finished(report); }, Qt::QueuedConnection);
    });
    m_thread->setParent(this);
    // 自己收自己（见 submit() 里那段：重开一趟时**不许**对还在跑的线程 deleteLater）。
    // 先放指针、再排删除：`shutdown()` 里那句 `m_thread->wait()` 是在析构时跑的，
    // 而 deleteLater 会把这个对象真的删掉 —— 指针不清掉的话，那一句就是对一块已经
    // 释放的内存调 wait()。两件事都接到 `finished` 上，按连接顺序投递，清指针一定排在
    // 删除之前（两个接收者都住在主线程，投递顺序就是连接顺序）。
    connect(m_thread, &QThread::finished, this, [this, thread = m_thread] {
        if (m_thread == thread) m_thread = nullptr;
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void IntegrityController::progress(int checked, int total, int groupIndex) {
    if (m_taskId == Tasks::invalidTaskId() || !m_activity->tasks()->has(m_taskId)) return;
    m_activity->tasks()->setProgress(m_taskId, checked, total);
    const int count = int(VersionIntegrity::allGroups().size());
    for (int i = 0; i < count; ++i)
        m_activity->tasks()->setStepState(
            m_taskId, i, i < groupIndex ? Tasks::TaskStepState::Done
                        : i == groupIndex ? Tasks::TaskStepState::Running
                                          : Tasks::TaskStepState::Pending);
}

void IntegrityController::finished(const VersionIntegrity::Report &report) {
    const QString versionId = m_scanVersionId;
    m_scanVersionId.clear();
    if (m_taskId != Tasks::invalidTaskId() && m_activity->tasks()->has(m_taskId)) {
        for (int i = 0; i < int(VersionIntegrity::allGroups().size()); ++i)
            m_activity->tasks()->setStepState(m_taskId, i, Tasks::TaskStepState::Done);
        m_activity->tasks()->setDetail(m_taskId, report.summary());
        m_activity->finish(m_taskId, report.error.isEmpty(), report.error, false);
    }
    m_taskId = Tasks::invalidTaskId();
    if (versionId.isEmpty()) return;
    log(report.error.isEmpty()
            ? tr("「%1」完整性检查完成：%2").arg(displayName(versionId), report.summary())
            : tr("「%1」完整性检查未能完成：%2").arg(displayName(versionId), report.error));
    m_report = report;
    m_reportVersionId = versionId;
    if (m_host.openWhenOverlayFree)
        m_host.openWhenOverlayFree([this, versionId] { openReport(versionId); });
}

void IntegrityController::openReport(const QString &versionId) {
    if (m_overlay == nullptr || m_reportVersionId != versionId) return;
    if (m_page == nullptr) {
        m_page = new IntegrityReportPage(m_pageParent);
        connect(m_page, &IntegrityReportPage::actionRequested, this,
                [this](IntegrityReportPage::Action action) { reportAction(int(action)); });
    }
    IntegrityReportPage::Report view;
    view.instanceName = displayName(versionId);
    view.summary = m_report.summary();
    view.error = m_report.error;
    for (VersionIntegrity::Group group : VersionIntegrity::allGroups()) {
        IntegrityReportPage::Group item;
        item.name = VersionIntegrity::groupName(group);
        item.missing = m_report.missingIn(group);
        item.corrupt = m_report.corruptIn(group);
        for (const VersionIntegrity::Verdict &v : m_report.problemsInGroup(group))
            item.rows.append({v.label, v.detail(), v.path});
        view.groups.append(item);
    }
    m_page->setReport(view);
    // 非阻塞地摆上去（见 `OverlayHost::showPage()`）：这一页后面没有一行代码要等它收掉，
    // 以前走 `execPage()` 只是让"体检完自动开报告"这一发把调用栈卡在嵌套循环里。
    if (!m_overlay->showPage(m_page, IntegrityReportPage::windowTitle())) {
        // 摆不上（这一层刚被别人占上）：留一句日志，别让用户以为体检白跑了。
        log(tr("无法打开完整性检查报告：界面已被另一页占用。"));
    }
}

void IntegrityController::reportAction(int action) {
    if (action == int(IntegrityReportPage::Action::CopyReport)) {
        copyReport();
        return;
    }
    if (action != int(IntegrityReportPage::Action::Repair) || m_reportVersionId.isEmpty()) return;
    const QString id = m_reportVersionId;
    if (m_page != nullptr) m_page->reject();
    QTimer::singleShot(0, this, [this, id] { if (m_host.repair) m_host.repair(id); });
}

void IntegrityController::copyReport() {
    if (m_reportVersionId.isEmpty()) return;
    QGuiApplication::clipboard()->setText(
        VersionIntegrity::plainReport(m_report, displayName(m_reportVersionId)));
    log(tr("完整性检查报告已经复制到剪贴板。"));
}
