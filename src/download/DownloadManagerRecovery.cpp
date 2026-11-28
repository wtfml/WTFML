#include "DownloadManager.hpp"
#include <QCoreApplication>
#include "DownloadCheckpoint.hpp"
#include "ResumableDownload.hpp"
#include <QSignalBlocker>

bool DownloadManager::enableCheckpoint(const QString &path, const QJsonObject &context) {
    if (!m_batchOpen || m_checkpoint || !DownloadCheckpoint::safePath(path)) return false;
    auto receipt = QSharedPointer<DownloadCheckpoint>::create();
    receipt->path = path; receipt->context = context;
    if (QFileInfo::exists(path) || !receipt->save()) return false;
    m_checkpoint = receipt;
    return true;
}
QString DownloadManager::checkpointPath() const { return m_checkpoint ? m_checkpoint->path : QString(); }
QJsonObject DownloadManager::checkpointContext() const { return m_checkpoint ? m_checkpoint->context : QJsonObject(); }
bool DownloadManager::updateCheckpointContext(const QJsonObject &context) {
    if (!m_checkpoint) return false;
    m_checkpoint->context = context;
    const bool saved = m_checkpoint->save();
    if (saved) emit checkpointChanged();
    return saved;
}
bool DownloadManager::deferCheckpointContinuation(bool refreshSources) {
    if (!m_checkpoint) return false;
    m_checkpoint->context.insert("phase", "continue-install");
    m_checkpoint->context.insert("refreshSources", refreshSources);
    const bool saved = m_checkpoint->save();
    m_checkpoint.reset(); // Retain the receipt; only an explicit second action may run the installer.
    return saved;
}
bool DownloadManager::releaseCheckpoint() {
    if (!m_checkpoint) return false;
    const bool saved = m_checkpoint->save();
    m_checkpoint.reset(); // 只放锁：文件留给「继续未完成操作」，不 emit（理由见 hpp）
    return saved;
}
void DownloadManager::forgetCheckpoint() {
    if (m_checkpoint) m_checkpoint->discard();
    m_checkpoint.reset();
    emit checkpointChanged();
}
bool DownloadManager::canPauseBatch() const {
    // m_precheckActive / m_precheckQueue：还有几份"盘上那份到底对不对"没问完（见
    // `PrecheckJob`）。**这一段不许暂停** —— 暂停会按恢复记录重放，而没问完的那几份
    // 还没登记进去（原件指纹都还没算出来），重放时它们就丢了。等它们问完再让暂停出现，
    // 和 m_reuseActive 那条同一个道理。
    if (!m_checkpoint || m_checkpointFailed || m_checkpoint->files.isEmpty() || m_batchPaused ||
        m_batchOpen || m_batchEmitted || m_reuseActive > 0 || m_precheckActive > 0
        || !m_precheckQueue.isEmpty()) return false;
    // The home/download-page manifest request neither registers file jobs nor
    // belongs to this batch. cancelBatch() leaves it alive; waiting for it would
    // hide pause for an unrelated resource update on a slow network.
    for (const auto &job : m_metadataReplies)
        if (job.label != QStringLiteral("version_manifest.json")) return false;
    for (auto *reply : activeDownloads.keys()) if (reply->property("wtfmlAssetIndex").toBool()) return false;
    for (auto *job : m_segmentJobs.keys()) if (!job->canCancel()) return false;
    return isBusy();
}
bool DownloadManager::pauseBatch() {
    if (!canPauseBatch() || !m_checkpoint->save()) return false;
    m_suspendingBatch = true; // User cancellation discards receipts; a pause retains this one.
    cancelBatch();
    m_suspendingBatch = false; m_batchPaused = true;
    emit checkpointChanged();
    return true;
}
bool DownloadManager::resumeBatch(QString *problem) {
    const auto reject = [problem](const QString &reason) { if (problem) *problem = reason; return false; };
    if (!m_batchPaused || !m_checkpoint || pendingDownloads > 0 || !m_segmentJobs.isEmpty() || m_reuseActive > 0
        || m_precheckActive > 0 || !m_precheckQueue.isEmpty())
        return reject(tr("下载尚未结束，请稍后继续。"));
    const auto receipt = m_checkpoint;
    for (const auto &file : receipt->files) if (!DownloadCheckpoint::unchanged(file))
        return reject(tr("目标文件已被外部修改，未覆盖：%1。请重新预览。").arg(file.path));
    if (problem) problem->clear();
    return resumeValidatedBatch(receipt, {});
}
bool DownloadManager::resumeValidatedBatch(const QSharedPointer<DownloadCheckpoint> &receipt,
    const QHash<QString, QJsonObject> &targets) {
    // Suppress the owning UI's new-batch registration when replaying this exact batch.
    QSignalBlocker blocker(this);
    beginBatch();
    m_checkpoint = receipt;
    m_resumeTargets = targets;
    // Restore outward signals before any synchronous skipped/completed file callbacks.
    blocker.unblock();
    for (const auto &file : receipt->files)
        downloadFile(file.url, file.path, file.sha1, file.size, VerifyMode::SizeAndSha1, file.official);
    endBatch();
    emit checkpointChanged();
    return true;
}
bool DownloadManager::restoreBatch(const QString &path, QString *problem) {
    if (isBusy() || m_checkpoint) { if (problem) *problem = tr("已有下载正在进行，请稍后继续。"); return false; }
    auto receipt = QSharedPointer<DownloadCheckpoint>::create();
    if (!receipt->load(path)) { if (problem) *problem = tr("恢复记录不可读或被占用，原件已保留。"); return false; }
    m_checkpoint = receipt; m_batchPaused = true;
    if (!resumeBatch(problem)) { m_checkpoint.reset(); m_batchPaused = false; return false; }
    return true;
}
std::function<QString()> DownloadManager::checkpointGuard(const QString &path) const {
    const auto *found = m_checkpoint ? m_checkpoint->find(path) : nullptr;
    if (found) {
        const auto verified = m_resumeTargets.constFind(QDir::cleanPath(path).toCaseFolded());
        if (verified != m_resumeTargets.cend()) {
            const auto original = verified.value();
            return [path, original] {
                return DownloadCheckpoint::fingerprint(path) == original ? QString()
                    : QCoreApplication::translate("DownloadManagerRecovery", "下载期间目标已变化，未覆盖，请重新预览：%1").arg(path);
            };
        }
        const auto &entry = *found;
        if (!DownloadCheckpoint::unchanged(entry)) return [path] { return QCoreApplication::translate("DownloadManagerRecovery", "目标已被外部修改，未覆盖：%1").arg(path); };
        const auto original = DownloadCheckpoint::fingerprint(path);
        return [path, original] {
            return !original.isEmpty() && DownloadCheckpoint::fingerprint(path) == original ? QString()
                : QCoreApplication::translate("DownloadManagerRecovery", "下载期间目标已变化，未覆盖，请重新预览：%1").arg(path);
        };
    }
    return m_commitGuard;
}
