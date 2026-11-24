#include "InlineMessage.hpp"
#include <QCoreApplication>
#include "OverlayHost.hpp"
#include "DirectDownloadController.hpp"
#include "ActivityCenterController.hpp"
#include "download/ResumableDownload.hpp"
#include "DownloadToolPanel.hpp"
#include "download/DownloadCache.hpp"
#include "download/DownloadCheckpoint.hpp"
#include "util/AppPaths.hpp"
#include <QUuid>
#include <QLockFile>
#include <QSettings>
#include <QThread>
#include <QFileInfo>
#include <QLocale>
#include <QTimer>
#include <algorithm>

DirectDownloadController::DirectDownloadController(ActivityCenterController *activity, OverlayHost *overlay, QObject *parent)
    : QObject(parent), m_activity(activity), m_overlay(overlay) {
    // Follow the launcher's stable task ledger, not the currently selected game folder.
    m_recoveryPath = (m_activity ? QFileInfo(m_activity->tasks()->historyPath()).absolutePath()
                                : AppPaths::defaultMinecraftDir()) + QStringLiteral("/wtfml.download-recovery");
    QTimer::singleShot(0, this, [this] { loadRecovery(); });
    auto *rateTimer = new QTimer(this);
    rateTimer->setInterval(1000);
    connect(rateTimer, &QTimer::timeout, this, [this] {
        if (m_running && m_showTransferTelemetry) updateProgress(m_lastDone, m_lastTotal);
    });
    rateTimer->start();
    if (m_activity != nullptr) connect(m_activity->tasks(), &Tasks::TaskManager::taskFinished, this,
        [this](Tasks::TaskId id, bool) { m_pendingPaths.remove(id); });
    if (m_activity != nullptr) connect(m_activity, &QObject::destroyed, this, [this] {
        // MainWindow 的 QObject 子对象销毁顺序不能成为任务账本的隐式寿命保证。
        m_activity = nullptr; m_stopping = true; m_running = false; m_pendingPaths.clear();
        if (m_download != nullptr) m_download->cancel();
    });
}

DirectDownloadController::~DirectDownloadController() { shutdown(); }

void DirectDownloadController::attach(DownloadToolPanel *panel) {
    if (panel == nullptr || m_panel == panel) return;
    m_panel = panel;
    QSettings settings;
    panel->cacheLimit->setValue(settings.value(QStringLiteral("Toolbox/CacheLimitMiB"), 2048).toInt());
    panel->directory->setText(settings.value(QStringLiteral("Toolbox/DownloadDirectory"), panel->directory->text()).toString());
    connect(panel->cacheLimit, &QSpinBox::valueChanged, this, [](int value) {
        QSettings().setValue(QStringLiteral("Toolbox/CacheLimitMiB"), value);
    });
    connect(panel->directory, &QLineEdit::editingFinished, this, [this] {
        if (m_panel && QDir(m_panel->directory->text()).exists())
            QSettings().setValue(QStringLiteral("Toolbox/DownloadDirectory"), m_panel->directory->text());
    });
    panel->onCache = [this](int action) {
        if (action == 3 && !InlineMessage::question(m_overlay, tr("清空闲置续传缓存"),
            tr("只删未被任务使用的分段和续传记录，已下载的文件不受影响。"),
            tr("清空"), tr("取消"), false, true)) return;
        manageCache(action);
    };
    panel->onStart = [this] {
        DirectDownloadRequest request;
        QString error;
        if (!DirectDownloadRequest::parse(m_panel->url->text(), m_panel->directory->text(), m_panel->name->text(), &request, &error)) {
            m_panel->showStatus(error); return;
        }
        const bool exists = QFileInfo::exists(request.path);
        if (exists && !InlineMessage::question(m_overlay, tr("确认替换文件"),
                tr("%1 已存在。下载成功后替换它；取消或失败会保留原文件。").arg(QFileInfo(request.path).fileName()),
                tr("替换"), tr("取消"), false)) return;
        if (!start(request, exists, &error)) m_panel->showStatus(error);
    };
    panel->onCancel = [this] { cancel(m_task); };
}

void DirectDownloadController::panelShown() {
    if (!m_panel) return;
    m_panel->setBusy(m_running);
    if (!m_status.isEmpty()) m_panel->showStatus(m_status);
    manageCache(0);  // 进页面自动统计闲置缓存
}

void DirectDownloadController::manageCache(int action) {
    if (m_stopping) return;
    if (m_cacheBusy) { if (action == 1) m_cacheTrimPending = true; return; }
    m_cacheBusy = true;
    const auto kind = action == 1 ? DownloadCache::Action::Trim : action == 2 ? DownloadCache::Action::ClearExpired
        : action == 3 ? DownloadCache::Action::ClearIdle : DownloadCache::Action::Inspect;
    QSettings settings;
    const qint64 limit = qint64(qBound(128, settings.value(QStringLiteral("Toolbox/CacheLimitMiB"), 2048).toInt(), 65536)) * 1024 * 1024;
    const auto result = QSharedPointer<DownloadCache::Result>::create();
    auto *worker = QThread::create([result, kind, limit] { *result = DownloadCache::run(kind, limit); });
    if (m_panel) m_panel->cacheStatus->setText(tr("正在整理…"));
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, result] {
        m_cacheBusy = false;
        if (m_panel) {
            QString text = tr("闲置 %1").arg(QLocale().formattedDataSize(qMax<qint64>(0, result->bytes)));
            if (result->removed > 0) text += tr(" · 本次释放 %1").arg(QLocale().formattedDataSize(result->removed));
            if (result->busy > 0) text += tr(" · 跳过占用 %1").arg(result->busy);
            if (result->failed > 0) text += tr(" · 失败 %1").arg(result->failed);
            if (result->incomplete) text += tr(" · 统计不完整");
            m_panel->cacheStatus->setText(text);
        }
        if (m_cacheTrimPending) { m_cacheTrimPending = false; manageCache(1); }
    });
    worker->start();
}

bool DirectDownloadController::start(const DirectDownloadRequest &request, bool allowOverwrite, QString *error) {
    const auto reject = [error](const QString &text) { if (error) *error = text; return false; };
    if (m_stopping) return reject(tr("启动器正在关闭，不能新增下载。"));
    if (!m_activity) return reject(tr("任务中心尚未就绪。"));
    loadRecovery();
    if (m_recoveries.size() >= 64) return reject(tr("待恢复下载已达64项，请先继续或取消已有任务。"));
    if (!QDir::isAbsolutePath(request.path)) return reject(tr("目标文件必须是绝对路径。"));
    DirectDownloadRequest validated;
    if (!DirectDownloadRequest::parse(request.url.toString(), QFileInfo(request.path).absolutePath(), QFileInfo(request.path).fileName(), &validated, error)) return false;
    if (!allowOverwrite && QFileInfo::exists(validated.path)) return reject(tr("目标文件已存在，请确认覆盖或更换文件名。"));
    const auto samePath = [&](const QString &path) { return QDir::cleanPath(path).compare(QDir::cleanPath(validated.path), Qt::CaseInsensitive) == 0; };
    for (auto it = m_recoveries.begin(); it != m_recoveries.end(); ++it) {
        const auto state = m_activity->tasks()->snapshot(it.key()).state;
        if (samePath(it->request.path) && it->request.url != validated.url &&
            (state == Tasks::TaskState::Paused || Tasks::isTerminalState(state))) {
            const auto previous = it->request.url; it->request.url = validated.url;
            if (!saveRecovery()) { it->request.url = previous; return reject(tr("无法保存新的下载地址，未继续。")); }
            const auto id = it.key();
            if (!resume(id)) return reject(tr("目标已变化或任务无法继续，请检查任务中心提示。"));
            m_lastSubmitted = id;
            m_activity->startReady();
            if (error) error->clear();
            return true;
        }
    }
    const bool reserved = std::any_of(m_recoveries.cbegin(), m_recoveries.cend(),
        [&](const Recovery &r) { return samePath(r.request.path); });
    if (reserved || (m_running && samePath(m_path)) || std::any_of(m_pendingPaths.cbegin(), m_pendingPaths.cend(), samePath))
        return reject(tr("该保存路径已有进行中或排队的下载，请更换文件名。"));
    const auto id = m_activity->enqueue(Tasks::TaskKind::Download,
        tr("直链下载 / %1").arg(QFileInfo(validated.path).fileName()), QFileInfo(validated.path).absolutePath(),
        {{tr("下载文件"), Tasks::TaskStepState::Pending, {}}, {tr("校验并保存"), Tasks::TaskStepState::Pending, {}}},
        QStringLiteral("direct-download"), this, [this, validated, allowOverwrite](Tasks::TaskId job) { startJob(job, validated, allowOverwrite); });
    if (id == Tasks::invalidTaskId()) return reject(tr("等待队列已满，请稍后再添加。"));
    m_lastSubmitted = id; m_ownedTasks.insert(id); m_pendingPaths.insert(id, validated.path);
    // Preserve the user's consent against the exact original, not merely the same pathname.
    if (m_recoveryReadable) {
        const auto original = DownloadCheckpoint::fingerprint(validated.path);
        if (original.isEmpty()) { m_activity->tasks()->cancel(id); return reject(tr("目标不可读或包含链接，未开始下载。")); }
        m_recoveries.insert(id, {validated, QUuid::createUuid().toString(QUuid::WithoutBraces), original});
        if (!saveRecovery()) {
            m_recoveries.remove(id); m_activity->tasks()->cancel(id);
            return reject(tr("无法保存下载恢复记录，未开始下载。"));
        }
    }
    auto caps = m_activity->tasks()->snapshot(id).capabilities; caps.canOpenLog = false;
    m_activity->tasks()->setCapabilities(id, caps);
    if (!m_running) m_activity->startReady();
    else if (m_panel) m_panel->showStatus(tr("已加入等待队列，可在任务中心拖动调整顺序。"));
    if (error) error->clear();
    return true;
}

void DirectDownloadController::startJob(Tasks::TaskId id, const DirectDownloadRequest &validated, bool allowOverwrite) {
    m_pendingPaths.remove(id);
    m_task = id;
    delete m_download;
    m_download = new ResumableDownload(this);
    m_path = validated.path;
    m_running = true;
    m_activity->tasks()->setStepState(id, 0, Tasks::TaskStepState::Running);
    m_activity->tasks()->setStepState(id, 1, Tasks::TaskStepState::Pending);
    m_status = tr("正在连接；进度可在任务中心查看。");
    if (!m_recoveryReadable) m_status += tr("恢复记录不可用或被占用，原件已保留；本次下载不提供暂停和跨重启恢复。");
    m_activity->tasks()->setDetail(m_task, m_status);
    m_rateClock.start(); m_rate.reset(); m_transferred = 0; m_lastRateUi = -1000;
    m_lastDone = 0; m_lastTotal = -1; m_showTransferTelemetry = false;
    if (m_panel) { m_panel->setBusy(true); m_panel->showProgress(0, 0, m_status); }
    connect(m_download, &ResumableDownload::progress, this, [this](qint64 done, qint64 total) {
        if (!m_running) return;
        m_showTransferTelemetry = true;
        updateProgress(done, total);
    });
    connect(m_download, &ResumableDownload::transferBytes, this, [this](qint64 bytes) {
        if (!m_running || bytes <= 0) return;
        if (m_transferred == 0) m_rate.reset(m_rateClock.elapsed());
        m_transferred += bytes;
    });
    connect(m_download, &ResumableDownload::statusChanged, this, [this](const QString &message) {
        if (!m_running) return;
        m_showTransferTelemetry = false; // Preserve connection/checksum/commit phase messages.
        m_status = message;
        syncPauseCapability();
        m_activity->tasks()->setDetail(m_task, message);
        if (m_panel) m_panel->showStatus(message);
    });
    connect(m_download, &ResumableDownload::finished, this, [this](bool success, const QString &message) { finish(success, message); });
    if (m_recoveries.contains(id)) {
        const auto original = m_recoveries.value(id).original;
        const auto target = validated.path;
        const auto guard = [target, original] {
            return DownloadCheckpoint::fingerprint(target) == original ? QString()
                : QCoreApplication::translate("DirectDownloadController", "目标已被外部修改，请重新预览；未覆盖。");
        };
        if (!guard().isEmpty()) { finish(false, guard()); return; }
        m_download->setCommitGuard(guard);
    }
    m_download->start(validated.url, validated.path, {}, -1, allowOverwrite);
    syncPauseCapability();
    manageCache(1);
}

void DirectDownloadController::updateProgress(qint64 done, qint64 total) {
    if (!m_running || m_activity == nullptr) return;
    m_lastDone = done; m_lastTotal = total;
    syncPauseCapability();
    m_activity->tasks()->setBytes(m_task, done, total);
    m_activity->tasks()->setProgress(m_task, done, total);
    const qint64 now = m_rateClock.elapsed();
    m_rate.sample(now, m_transferred);
    if (now - m_lastRateUi < 1000 && (total <= 0 || done < total)) return;
    m_lastRateUi = now;
    const auto size = [](qint64 bytes) { return QLocale().formattedDataSize(qMax<qint64>(0, bytes)); };
    const qint64 speed = m_rate.bytesPerSecond(now);
    m_status = tr("%1 / %2 / %3").arg(size(done), total > 0 ? size(total) : tr("大小未知"),
        speed < 0 ? tr("测量速度中…") : size(speed) + QStringLiteral("/s"));
    if (total > done) {
        const qint64 eta = m_rate.remainingSeconds(now, total - done);
        if (eta > 0) {
            const qint64 seconds = qMax<qint64>(5, ((eta + 2) / 5) * 5);
            const QString duration = seconds >= 3600
                ? tr("%1 小时 %2 分").arg(seconds / 3600).arg((seconds % 3600) / 60)
                : seconds >= 60 ? tr("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60)
                                : tr("%1 秒").arg(seconds);
            m_status += tr(" / 预计剩余约 %1").arg(duration);
        } else m_status += speed == 0 ? tr(" / 等待数据…") : tr(" / 正在估算剩余时间…");
    }
    m_activity->tasks()->setDetail(m_task, m_status);
    if (m_panel) m_panel->showProgress(done, total, m_status);
}

void DirectDownloadController::finish(bool success, const QString &error) {
    if (!m_running) return;
    const auto finishedTask = m_task;
    const auto finishedPath = m_path;
    m_running = false;
    const auto status = success ? tr("下载完成：%1").arg(QDir::toNativeSeparators(finishedPath)) : tr("下载失败：%1").arg(error);
    m_status = status;
    m_activity->tasks()->setStepState(finishedTask, 0, success ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Failed, error);
    m_activity->tasks()->setStepState(finishedTask, 1, success ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Failed, error);
    if (success) {
        const auto bytes = QFileInfo(finishedPath).size();
        m_activity->tasks()->setBytes(finishedTask, bytes, bytes);
        m_activity->tasks()->setProgress(finishedTask, bytes, bytes);
    }
    m_activity->tasks()->setDetail(finishedTask, status);
    m_activity->finish(finishedTask, success, error, false);
    if (success) { m_recoveries.remove(finishedTask); saveRecovery(); }
    else if (m_recoveries.contains(finishedTask))
        m_activity->tasks()->setRecovery(finishedTask, m_recoveries.value(finishedTask).token, true);
    if (m_panel && m_task == finishedTask && !m_running) { m_panel->setBusy(false); m_panel->showProgress(success ? 1 : 0, 1, status); }
}

bool DirectDownloadController::owns(Tasks::TaskId id) const { return m_ownedTasks.contains(id); }
bool DirectDownloadController::canCancel(Tasks::TaskId id) const {
    return m_activity != nullptr && owns(id) && (m_activity->isQueued(id) ||
        m_activity->tasks()->snapshot(id).state == Tasks::TaskState::Paused ||
        (m_recoveries.contains(id) && Tasks::isTerminalState(m_activity->tasks()->snapshot(id).state)) ||
        (id == m_task && m_running && m_download && m_download->canCancel()));
}
bool DirectDownloadController::cancel(Tasks::TaskId id) {
    if (!owns(id)) return false;
    if (m_activity != nullptr && (m_activity->isQueued(id) || m_activity->tasks()->snapshot(id).state == Tasks::TaskState::Paused ||
        (m_recoveries.contains(id) && Tasks::isTerminalState(m_activity->tasks()->snapshot(id).state)))) {
        m_pendingPaths.remove(id); m_recoveries.remove(id); saveRecovery(); m_activity->tasks()->cancel(id);
        m_activity->tasks()->setRecovery(id, {}, false);
        return true;
    }
    if (id != m_task || !m_running || m_download == nullptr) return true;
    const auto reportUnstoppable = [this]() {
        m_status = tr("正在校验并写入文件，此步骤无法取消。");
        if (m_panel) m_panel->showStatus(m_status);
        if (m_activity != nullptr) m_activity->tasks()->setDetail(m_task, m_status);
    };
    if (!canCancel(id)) {
        reportUnstoppable();
        return true;
    }
    if (!m_download->cancel()) {
        reportUnstoppable();
        return true;
    }
    m_running = false;
    m_recoveries.remove(id); saveRecovery();
    m_activity->tasks()->cancel(m_task);
    m_status = tr("已取消，原文件未替换；完整分段已保留，重开程序后用同一链接和目标可继续。");
    if (m_panel) { m_panel->setBusy(false); m_panel->showProgress(0, 1, m_status); }
    return true;
}
void DirectDownloadController::shutdown() {
    if (m_stopping) return;
    m_stopping = true;
    const auto waiting = m_pendingPaths.keys();
    for (auto id : waiting) if (!m_recoveries.contains(id)) cancel(id);
    if (m_running && !pause(m_task)) cancel(m_task);
    if (!m_running) m_recoveryLock.reset();
}
