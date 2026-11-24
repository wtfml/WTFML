#include "DownloadBatchController.hpp"
#include "ActivityCenterController.hpp"
#include "download/DownloadManager.hpp"
#include "download/DownloadCheckpoint.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QUuid>

DownloadBatchController::DownloadBatchController(ActivityCenterController *activity, DownloadManager *downloads,
    std::function<Tasks::TaskId()> activeTask, QObject *parent)
    : QObject(parent), m_activity(activity), m_downloads(downloads), m_activeTask(std::move(activeTask)) {
    m_root = QFileInfo(activity->tasks()->historyPath()).absolutePath() + "/download-batches";
    connect(downloads, &DownloadManager::batchStarted, this, [this] {
        if (m_stopping || m_restoring) return;
        const auto id = m_activeTask();
        if (id == Tasks::invalidTaskId() || !m_activity->tasks()->has(id)) return;
        const auto task = m_activity->tasks()->snapshot(id);
        if (QDir(m_root).entryList({"*.receipt"}, QDir::Files).size() >= 64) return;
        const auto path = m_root + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".receipt";
        const auto intent = m_continuationSnapshot ? m_continuationSnapshot() : QJsonObject();
        if (m_downloads->enableCheckpoint(path, {{"title", task.title}, {"task", QString::number(id)},
            {"target", task.targetId}, {"filesOnly", true}, {"continuation", intent}})) {
            m_current = id; m_records.insert(id, path);
            requestContinuationTargets(id, path, intent);
        }
    });
    connect(downloads, &DownloadManager::checkpointChanged, this, &DownloadBatchController::sync);
    connect(downloads, &DownloadManager::batchProgress, this, [this] { sync(); });
    connect(downloads, &DownloadManager::allDownloadsFinished, this, &DownloadBatchController::complete);
    connect(activity->tasks(), &Tasks::TaskManager::taskFinished, this, [this](Tasks::TaskId id, bool) {
        if (m_activity->tasks()->snapshot(id).state != Tasks::TaskState::Cancelled || !owns(id)) return;
        if (id == m_current) { m_downloads->forgetCheckpoint(); m_current = Tasks::invalidTaskId(); }
        else { DownloadCheckpoint receipt; if (receipt.load(m_records.value(id))) receipt.discard(); }
        m_records.remove(id);
    });
    connect(activity, &QObject::destroyed, this, [this] { m_activity = nullptr; m_stopping = true; });
    connect(downloads, &QObject::destroyed, this, [this] { m_downloads = nullptr; m_stopping = true; });
}
DownloadBatchController::~DownloadBatchController() {
    if (m_verifyCancel) m_verifyCancel->store(true, std::memory_order_relaxed);
    stopContinuationTargets();
}
void DownloadBatchController::load() {
    if (m_stopping || !DownloadCheckpoint::safePath(m_root)) return;
    const auto names = QDir(m_root).entryList({"*.receipt"}, QDir::Files, QDir::Name);
    if (names.size() > 64) return; // Keep unexpected/unbounded evidence intact.
    for (const auto &name : names) {
        const auto path = m_root + '/' + name;
        if (m_records.values().contains(path)) continue;
        DownloadCheckpoint receipt;
        if (!receipt.load(path) || receipt.files.isEmpty()) continue;
        bool ok = false; const auto oldId = receipt.context.value("task").toString().toULongLong(&ok);
        Tasks::TaskId id = Tasks::invalidTaskId();
        if (ok && m_activity->tasks()->has(oldId)) {
            const auto old = m_activity->tasks()->snapshot(oldId);
            const bool sameIntent = old.title == receipt.context.value("title").toString()
                && old.targetId == receipt.context.value("target").toString();
            if (sameIntent && ((old.state == Tasks::TaskState::Succeeded && receipt.context.value("phase") != "continue-install") || old.state == Tasks::TaskState::Cancelled)) {
                receipt.discard(); continue;
            }
            if (sameIntent && (Tasks::isTerminalState(old.state) || old.state == Tasks::TaskState::Paused)) id = oldId;
        }
        const bool hasContinuation = !receipt.context.value("continuation").toObject().isEmpty();
        const bool reused = id != Tasks::invalidTaskId();
        if (!reused) id = m_activity->begin(hasContinuation ? Tasks::TaskKind::Install : Tasks::TaskKind::Download,
            receipt.context.value("title").toString(), receipt.context.value("target").toString());
        if (receipt.context.value("phase") == "continue-install") {
            if (reused) {
                m_activity->tasks()->setRecovery(id, name, true);
                if (!m_activity->tasks()->queueRecovery(id) || !m_activity->tasks()->startQueued(id)) continue;
            }
            auto caps = m_activity->tasks()->snapshot(id).capabilities; caps.canPause = true;
            m_activity->tasks()->setCapabilities(id, caps); m_activity->tasks()->pause(id);
            m_activity->tasks()->setDetail(id, receipt.context.value("refreshSources").toBool()
                ? tr("部分暂存文件未补齐，等待明确继续以重新查询平台地址和兼容性；未自动部署。")
                : tr("文件已补齐，等待明确点击继续安装；执行前仍会检查目标和预设变化。"));
        } else if (!reused) m_activity->finish(id, false, tr("上次文件下载中断。先补齐文件，不会自动执行安装器。"), false);
        else m_activity->tasks()->setDetail(id, tr("上次文件下载中断。先补齐文件，不会自动执行安装器。"));
        receipt.context.insert("task", QString::number(id));
        if (!receipt.save()) continue;
        m_records.insert(id, path);
        m_activity->tasks()->setRecovery(id, name, true);
    }
}
void DownloadBatchController::stopContinuationTargets() {
    if (m_targetsCancel) m_targetsCancel->store(true, std::memory_order_relaxed);
    // 代号一加，算完的那一份也不许再补进记录（记录可能已经换成别的批次了）。
    ++m_targetsGeneration;
    m_targetsCancel.reset();
}
void DownloadBatchController::requestContinuationTargets(Tasks::TaskId id, const QString &path,
    const QJsonObject &intent) {
    // 只有"要写预设"的那几趟才需要这一拍：没预设就不读那棵树，一拍都不发。
    // 服务端安装那份意图**没有 `instance`**（配置树不按实例分），它的接续不靠这一拍。
    if (!m_continuationTargets || m_stopping || intent.isEmpty() ||
        intent.contains("presetTargets") || intent.value("presets").toArray().isEmpty() ||
        intent.value("instance").toString().isEmpty()) return;
    const auto generation = ++m_targetsGeneration;
    const auto cancel = m_targetsCancel = std::make_shared<std::atomic_bool>(false);
    const auto targets = m_continuationTargets;
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, cancel, generation, id, path] {
        const auto computed = watcher->result();
        watcher->deleteLater();
        if (generation != m_targetsGeneration) return;
        m_targetsCancel.reset();
        if (m_stopping || !m_downloads || m_current != id || m_downloads->checkpointPath() != path) return;
        if (cancel->load(std::memory_order_relaxed)) return;
        auto context = m_downloads->checkpointContext();
        auto continuation = context.value("continuation").toObject();
        // 拍不出来（路径上有关口、文件读不动）就**整条接续意图都不要** —— 半截意图
        // （有预设、没有 `presetTargets`）到了恢复那一步照样被 `unchangedInitializationContinuation`
        // 挡下来，留着只会让"接着装"出现一个按不动的按钮。这样这一批就按普通下载收尾。
        if (computed.isEmpty()) continuation = QJsonObject();
        else continuation.insert("presetTargets", computed);
        context.insert("continuation", continuation);
        m_downloads->updateCheckpointContext(context);
    });
    watcher->setFuture(QtConcurrent::run([targets, cancel, intent] {
        return cancel->load(std::memory_order_relaxed) ? QJsonObject() : targets(intent, cancel.get());
    }));
}
bool DownloadBatchController::owns(Tasks::TaskId id) const { return m_records.contains(id); }
bool DownloadBatchController::canCancel(Tasks::TaskId id) const {
    return owns(id) && (id != m_current || m_restoring || (m_downloads && m_downloads->batchPaused()));
}
void DownloadBatchController::sync() {
    if (!m_activity || !m_downloads || !owns(m_current)) return;
    if (m_downloads->checkpointPath().isEmpty()) {
        m_activity->tasks()->setRecovery(m_current, {}, false);
        m_records.remove(m_current); m_current = Tasks::invalidTaskId(); return;
    }
    auto caps = m_activity->tasks()->snapshot(m_current).capabilities;
    caps.canPause = m_downloads->canPauseBatch();
    m_activity->tasks()->setCapabilities(m_current, caps);
    if (m_restoring) {
        const auto stats = m_downloads->stats();
        m_activity->tasks()->setProgress(m_current, stats.done, stats.total);
        m_activity->tasks()->setDetail(m_current, tr("补齐文件 %1 / %2；尚未执行安装器。").arg(stats.done).arg(stats.total));
    }
}
bool DownloadBatchController::pause(Tasks::TaskId id) {
    if (m_stopping || !owns(id) || id != m_current || !m_downloads->canPauseBatch()) return false;
    // Set the ledger before cancellation: aborted old replies must not end the task.
    auto caps = m_activity->tasks()->snapshot(id).capabilities; caps.canPause = true;
    m_activity->tasks()->setCapabilities(id, caps);
    if (!m_downloads->pauseBatch()) return false;
    caps = m_activity->tasks()->snapshot(id).capabilities; caps.canPause = true;
    m_activity->tasks()->setCapabilities(id, caps);
    if (!m_activity->tasks()->pause(id)) return false;
    m_activity->tasks()->setRecovery(id, QFileInfo(m_records.value(id)).fileName(), true);
    m_activity->tasks()->setDetail(id, tr("已暂停文件下载。已保存完整分段；继续前会检查目标变化，安装器和提交阶段不可暂停。"));
    return true;
}
bool DownloadBatchController::resume(Tasks::TaskId id) {
    if (m_stopping || !owns(id) || m_verifying != Tasks::invalidTaskId()) return false;
    const bool live = id == m_current && m_downloads->batchPaused();
    if (!live && m_downloads->isBusy()) return false;
    if (!live && m_restoreAllowed && !m_restoreAllowed()) {
        m_activity->tasks()->setDetail(id, tr("有游戏、启动或文件操作正在进行，请结束后再继续文件恢复。")); return false;
    }
    QString problem;
    DownloadCheckpoint receipt;
    if (!live && !receipt.load(m_records.value(id))) return false;
    const auto entries = live ? m_downloads->m_checkpoint->files : receipt.files;
    QSet<QString> roots;
    for (const auto &entry : entries) roots.insert(WriteDirectoryGuard::rootFor(QFileInfo(entry.path).absolutePath()));
    for (const auto &root : roots) if (!WriteDirectoryGuard::acquire(root, &problem)) {
        m_activity->tasks()->setRecovery(id, QFileInfo(m_records.value(id)).fileName(), false, problem); return false;
    }
    if (!live && receipt.context.value("phase") == "continue-install") {
        if (!m_continue) return false;
        if (!m_activity->tasks()->queueRecovery(id) || !m_activity->tasks()->startQueued(id)) return false;
        m_activity->tasks()->setDetail(id, tr("正在校验恢复文件；完成后才继续安装。"));
        auto capabilities = m_activity->tasks()->snapshot(id).capabilities;
        capabilities.canPause = false;
        capabilities.canCancel = true;
        capabilities.cancelBlockedReason.clear();
        m_activity->tasks()->setCapabilities(id, capabilities);
        m_verifying = id;
        const auto generation = ++m_verifyGeneration;
        const auto cancel = m_verifyCancel = std::make_shared<std::atomic_bool>(false);
        const auto held = QSharedPointer<DownloadCheckpoint>::create(std::move(receipt));
        const auto entries = held->files; // Worker owns values only; never reads controller/UI state.
        auto *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, held, cancel, generation, id] {
            const bool valid = watcher->result();
            watcher->deleteLater();
            if (generation != m_verifyGeneration) return;
            m_verifying = Tasks::invalidTaskId();
            m_verifyCancel.reset();
            if (m_stopping || !m_activity || !m_downloads) return;
            if (cancel->load(std::memory_order_relaxed) || !owns(id) ||
                m_activity->tasks()->snapshot(id).state == Tasks::TaskState::Cancelled) {
                held->discard();
                return;
            }
            if (!valid || m_downloads->isBusy() || (m_restoreAllowed && !m_restoreAllowed())) {
                m_activity->finish(id, false, tr("文件已变化或恢复条件已改变，请重新预览安装；未执行安装器。"), false);
                m_activity->tasks()->setRecovery(id, QFileInfo(held->path).fileName(), false, tr("请重新预览安装。"));
                return;
            }
            const auto path = m_records.take(id);
            if (m_continue && m_continue(id, held->context.value("continuation").toObject())) {
                held->discard();
                return;
            }
            m_records.insert(id, path);
            m_activity->finish(id, false, tr("目标、安装环境或原配置已变化，请重新预览安装。"), false);
            m_activity->tasks()->setRecovery(id, QFileInfo(path).fileName(), false, tr("请重新预览安装。"));
        });
        const auto validate = m_continuationValidation;
        const auto intent = held->context.value("continuation").toObject();
        watcher->setFuture(QtConcurrent::run([entries, cancel, validate, intent] {
            for (const auto &entry : entries) {
                if (cancel->load(std::memory_order_relaxed) || !DownloadCheckpoint::unchanged(entry, cancel.get())) return false;
            }
            return !cancel->load(std::memory_order_relaxed) && (!validate || validate(intent, cancel.get()));
        }));
        return true;

    }
    if (!m_activity->tasks()->queueRecovery(id) || !m_activity->tasks()->startQueued(id)) return false;
    m_activity->tasks()->setDetail(id, tr("正在后台校验下载目标；完成后才恢复文件下载。"));
    auto caps = m_activity->tasks()->snapshot(id).capabilities;
    caps.canPause = false; caps.canCancel = true; caps.cancelBlockedReason.clear();
    m_activity->tasks()->setCapabilities(id, caps);
    m_verifying = id;
    const auto generation = ++m_verifyGeneration;
    const auto cancel = m_verifyCancel = std::make_shared<std::atomic_bool>(false);
    const auto held = live ? m_downloads->m_checkpoint : QSharedPointer<DownloadCheckpoint>::create(std::move(receipt));
    using Targets = QHash<QString, QJsonObject>;
    auto *watcher = new QFutureWatcher<Targets>(this);
    connect(watcher, &QFutureWatcher<Targets>::finished, this, [this, watcher, held, cancel, generation, id, live] {
        const auto targets = watcher->result();
        watcher->deleteLater();
        if (generation != m_verifyGeneration) return;
        m_verifying = Tasks::invalidTaskId(); m_verifyCancel.reset();
        if (m_stopping || !m_activity || !m_downloads) return;
        if (cancel->load(std::memory_order_relaxed) || !owns(id) ||
            m_activity->tasks()->snapshot(id).state == Tasks::TaskState::Cancelled) { held->discard(); return; }
        const bool sameBatch = live ? m_downloads->m_checkpoint == held && m_downloads->batchPaused()
                                    : !m_downloads->isBusy() && !m_downloads->m_checkpoint;
        if (targets.size() != held->files.size() || !sameBatch || (!live && m_restoreAllowed && !m_restoreAllowed())) {
            const auto reason = tr("文件已变化或恢复条件已改变，请重新预览；未恢复下载。");
            m_activity->finish(id, false, reason, false);
            m_activity->tasks()->setRecovery(id, QFileInfo(held->path).fileName(), false, reason);
            return;
        }
        m_current = id;
        if (!live) m_restoring = true; // A live executor still owns installation/terminal signals.
        m_downloads->resumeValidatedBatch(held, targets);
        sync();
    });
    watcher->setFuture(QtConcurrent::run([entries, cancel] {
        Targets targets;
        for (const auto &entry : entries) {
            QJsonObject current;
            if (cancel->load(std::memory_order_relaxed) || !DownloadCheckpoint::unchanged(entry, cancel.get(), &current)) return Targets();
            targets.insert(QDir::cleanPath(entry.path).toCaseFolded(), current);
        }
        return cancel->load(std::memory_order_relaxed) ? Targets() : targets;
    }));
    return true;
}
void DownloadBatchController::complete(bool success) {
    if (!m_activity || !owns(m_current)) return;
    const auto id = m_current;
    const auto intent = m_downloads->checkpointContext().value("continuation").toObject();
    // Platform URLs may expire. Only the preset planner can resolve a fresh URL;
    // it must recheck compatibility and review substitutions on an explicit action.
    const bool refreshPreset = !success && intent.value("kind") == "preset-mods";
    const bool continuation = (success || refreshPreset) && m_restoring && m_continue && !intent.isEmpty();
    if (continuation) {
        if (!m_downloads->deferCheckpointContinuation(refreshPreset)) {
            m_activity->finish(id, false, tr("文件已保留，但安装接续记录未能保存；未执行安装器，请再次继续或重新预览。"), false);
            m_activity->tasks()->setRecovery(id, QFileInfo(m_records.value(id)).fileName(), true);
            m_current = Tasks::invalidTaskId(); m_restoring = false; return;
        }
        m_activity->tasks()->setDetail(id, refreshPreset
            ? tr("部分暂存文件未补齐。再次点击继续将重新查询平台地址和兼容性，必要文件校验通过后才应用；未自动部署。")
            : tr("文件已补齐。再次点击继续，才会按原选择继续安装并应用预设；安装器未自动执行。"));
        auto caps = m_activity->tasks()->snapshot(id).capabilities; caps.canPause = true;
        m_activity->tasks()->setCapabilities(id, caps); m_activity->tasks()->pause(id);
        m_activity->tasks()->setRecovery(id, QFileInfo(m_records.value(id)).fileName(), true);
        m_current = Tasks::invalidTaskId(); m_restoring = false;
        return;
    }
    if (m_restoring) {
        m_activity->tasks()->setDetail(id, success ? tr("已补齐并校验记录中的文件。请回原安装流程继续；安装器未自动执行。")
            : tr("仍有文件未完成，已保留记录，可再次继续。"));
        m_activity->finish(id, success, success ? QString() : tr("文件下载未完成。"), false);
    }
    if (success) {
        m_downloads->forgetCheckpoint(); m_records.remove(id);
        m_activity->tasks()->setRecovery(id, {}, false);
    } else m_activity->tasks()->setRecovery(id, QFileInfo(m_records.value(id)).fileName(), true);
    m_current = Tasks::invalidTaskId(); m_restoring = false;
}
bool DownloadBatchController::cancel(Tasks::TaskId id) {
    if (!owns(id)) return false;
    if (id == m_current) stopContinuationTargets(); // 记录马上就要被丢掉，别让它再补进去
    if (id == m_verifying && m_verifyCancel) {
        m_verifyCancel->store(true, std::memory_order_relaxed);
        const bool live = id == m_current;
        m_activity->tasks()->cancel(id);
        if (live && m_downloads) m_downloads->cancelBatch();
        m_activity->tasks()->setRecovery(id, {}, false);
        return true;
    }
    if (id == m_current && !m_restoring) return false; // VM owns its terminal signal, including a paused batch.
    if (id == m_current) {
        m_downloads->cancelBatch(); m_current = Tasks::invalidTaskId(); m_restoring = false;
    } else { DownloadCheckpoint receipt; if (!receipt.load(m_records.value(id))) return false; receipt.discard(); }
    m_records.remove(id); m_activity->tasks()->cancel(id); m_activity->tasks()->setRecovery(id, {}, false);
    return true;
}
void DownloadBatchController::shutdown() { stopContinuationTargets(); if (m_verifyCancel) m_verifyCancel->store(true, std::memory_order_relaxed); if (!m_stopping) { if (m_downloads && m_downloads->canPauseBatch()) pause(m_current); m_stopping = true; } }
