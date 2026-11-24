/**
 * @file DownloadManagerSegmented.cpp
 * @brief 分段下载这一套状态：判定能不能分段、开一条分段作业、它结束时结账。
 *
 * 分段作业跑在 `ResumableDownload` 自己那份下载器上，和外层"一条回复怎么流动"是两套
 * 接线。搬出 `DownloadManager.cpp` 之后，分段作业的四条函数与 `m_segmentJobs` 在一处，
 * 外层只负责在排队那一步决定 `job.segmented`。
 */

#include "DownloadManager.hpp"
#include <QCoreApplication>

#include "DownloadCache.hpp"
#include "ResumableDownload.hpp"

#include "../util/DownloadDiagnostics.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

bool DownloadManager::segmentedEligible(const QString &url, const QString &expectedSha1,
                                        qint64 expectedSize, const DownloadRange &range) {
    // 分段请求本身（range.active()）绝不能再被升级一次。
    if (range.active()) return false;
    if (expectedSize < ResumableDownload::kMinimumBytes ||
        expectedSize > ResumableDownload::kMaximumBytes) {
        return false;
    }
    if (expectedSha1.isEmpty()) return false;
    const QUrl target(url);
    return target.scheme() == QStringLiteral("http") || target.scheme() == QStringLiteral("https");
}

int DownloadManager::segmentConnections() const {
    return m_segmentJobs.size() * ResumableDownload::kPartConcurrency;
}

void DownloadManager::startSegmentedDownload(const PendingJob &job) {
    const QString targetUrl =
        effectiveDownloadUrl(job.url, job.officialUrl, mirrorDemotedFor(job.host));
    const bool rerouted = (targetUrl != job.url);

    auto *task = new ResumableDownload(this, m_networkFactory);
    task->setCommitGuard(checkpointGuard(job.destinationPath));
    // 限速要跟着这一条走：它在 start() 里会建自己那份下载器（见那个 setter 的说明）。
    // 并且**和外层共用一个令牌桶** —— 每个分段作业各拿一份上限的话，
    // 设 1 MB/s 时三个作业加上外层能跑出四倍。
    task->setSpeedLimitBucketOwner(this);
    task->setSpeedLimitBytesPerSecond(m_speedBucket->limit);
    SegmentJob record;
    record.url = targetUrl;
    record.destinationPath = job.destinationPath;
    record.expectedSha1 = job.expectedSha1;
    record.expectedSize = job.expectedSize;
    // 已经因为镜像降级改道到官方了，"退回官方"这条退路就用掉了（和 startDownload 同源）。
    record.officialUrl = rerouted ? QString() : job.officialUrl;
    // 分段途中合计速率持续太慢时，协调器会拿这条退路取一段样本比一比（快得多就整条
    // 换过去）。口径与 record.officialUrl 一致：没有退路的作业不设，比较不会发生。
    task->setFallbackUrl(QUrl(record.officialUrl));
    record.host = QUrl(targetUrl).host();
    record.officialRetried = job.officialRetried;
    record.plainRetried = job.plainRetried;
    record.generation = m_batchGeneration;
    // 先登记、先连信号，再 start()：start() 在本地准备失败时会**同步**发 finished，
    // 晚一步就会漏掉那一次失败（那条作业会永远占着名额）。
    m_segmentJobs.insert(task, record);

    connect(task, &ResumableDownload::transferBytes, this, [this, task](qint64 bytes) {
        const auto it = m_segmentJobs.constFind(task);
        if (it != m_segmentJobs.constEnd() && it->generation == m_batchGeneration)
            emit transferBytes(bytes);
    });

    connect(task, &ResumableDownload::progress, this, [this, task](qint64 done, qint64 total) {
        const auto it = m_segmentJobs.constFind(task);
        if (it == m_segmentJobs.constEnd() || it->generation != m_batchGeneration) return;
        emit downloadProgress(QFileInfo(it->destinationPath).fileName(), done, total);
        emitBatchProgress(it->destinationPath);
    });
    connect(task, &ResumableDownload::finished, this,
            [this, task](bool success, const QString &error) {
                finishSegmentedJob(task, success, error);
            });

    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("segment start gen=%1 url=%2 dest=%3 sha1=%4 size=%5 parts=%6 official=%7")
                               .arg(m_batchGeneration)
                               .arg(DownloadDiag::safeUrl(targetUrl),
                                    QDir::toNativeSeparators(job.destinationPath),
                                    job.expectedSha1.isEmpty() ? QStringLiteral("-")
                                                               : job.expectedSha1)
                               .arg(job.expectedSize)
                               .arg(ResumableDownload::kPartConcurrency)
                               .arg(job.officialUrl.isEmpty()
                                        ? QStringLiteral("-")
                                        : DownloadDiag::safeUrl(job.officialUrl)));
    }

    // ResumableDownload 要求绝对路径；记录里仍保留调用方原来那条（信号/失败清单用它）。
    m_startingSegment = true;
    task->start(QUrl(targetUrl), QFileInfo(job.destinationPath).absoluteFilePath(),
                job.expectedSha1, job.expectedSize, m_allowOverwrite,
                m_rangeCacheRoot.isEmpty() ? DownloadCache::root() : m_rangeCacheRoot);
    m_startingSegment = false;
}

void DownloadManager::finishSegmentedJob(ResumableDownload *task, bool success,
                                         const QString &error) {
    if (!task) return;
    if (m_shuttingDown) {
        task->deleteLater();
        return;
    }
    // start() 同步发出来的 finished：等这一层调用栈退出去再结账，避免在 pump 的循环里
    // 再次进 pump、改动 m_pendingJobs 的当前位置。
    if (m_startingSegment) {
        QTimer::singleShot(0, this, [this, task, success, error] {
            finishSegmentedJob(task, success, error);
        });
        return;
    }
    const auto it = m_segmentJobs.find(task);
    if (it == m_segmentJobs.end()) {
        // 已经取消（cancel() 不会发 finished）或重复回调。
        task->deleteLater();
        return;
    }
    const SegmentJob record = *it;
    m_segmentJobs.erase(it);

    const QString fileName = QFileInfo(record.destinationPath).fileName();
    const bool stale = record.generation != m_batchGeneration;
    bool requeued = false;

    if (stale) {
        // 上一批的残留：文件本身照旧落盘，但计数 / 进度 / 完成信号一个字都不碰。
    } else if (success) {
        emit downloadFinished(fileName, true, QString());
    } else if (!task->networkAttempted() && !record.plainRetried) {
        // 本地准备失败（路径无效、缓存目录不可写、同一下载的缓存被占用）：这不是源站
        // 失败，改走普通下载，别把本来能下成的文件判死。
        PendingJob retry;
        retry.url = record.url;
        retry.destinationPath = record.destinationPath;
        retry.expectedSha1 = record.expectedSha1;
        retry.expectedSize = record.expectedSize;
        retry.officialUrl = record.officialUrl;
        retry.host = record.host;
        retry.segmented = false;
        retry.officialRetried = record.officialRetried;
        retry.plainRetried = true;
        requeueDownloadJob(retry);
        requeued = true;
    } else if (!record.officialUrl.isEmpty() && !record.officialRetried) {
        PendingJob retry;
        retry.url = record.officialUrl;
        retry.destinationPath = record.destinationPath;
        retry.expectedSha1 = record.expectedSha1;
        retry.expectedSize = record.expectedSize;
        retry.officialUrl = QString();  // 只退一次
        retry.host = QUrl(record.officialUrl).host();
        retry.segmented =
            m_segmentEnabled &&
            segmentedEligible(retry.url, retry.expectedSha1, retry.expectedSize, DownloadRange());
        retry.officialRetried = true;
        retry.plainRetried = record.plainRetried;
        requeueDownloadJob(retry);
        requeued = true;
    } else {
        m_failedJobs++;
        FailedJob failed;
        // 记**入口地址**，不是 302 落点：failedJobs() 的重试会拿它再发一次，
        // 预签名落点几秒后就过期了。
        failed.url = record.url;
        failed.destinationPath = record.destinationPath;
        failed.expectedSha1 = record.expectedSha1;
        failed.expectedSize = record.expectedSize;
        failed.officialUrl = record.officialUrl;
        m_failedJobInfos.append(failed);
        if (!record.destinationPath.isEmpty() && !m_failedPaths.contains(record.destinationPath)) {
            m_failedPaths << record.destinationPath;
        }
        emit downloadFinished(fileName, false,
                              error.isEmpty() ? QCoreApplication::translate("DownloadManagerSegmented", "分段下载失败") : error);
    }

    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("segment finish gen=%1 dest=%2 result=%3 error=%4 stale=%5 retried=%6")
                               .arg(record.generation)
                               .arg(QDir::toNativeSeparators(record.destinationPath),
                                    success ? QStringLiteral("ok") : QStringLiteral("fail"),
                                    error.isEmpty() ? QStringLiteral("-") : error)
                               .arg(stale ? QStringLiteral("yes") : QStringLiteral("no"),
                                    requeued ? QStringLiteral("yes") : QStringLiteral("no")));
    }

    task->deleteLater();
    if (!stale && !requeued) {
        // 和 onDownloadReplyFinished 的收尾一致：done 是"已结账"，失败也含在内。
        m_doneJobs++;
        emitBatchProgress(record.destinationPath);
    }
    // 先腾出预算再判整批是否结束（顺序和 onDownloadReplyFinished 一样）。
    pumpDownloadQueue();
    checkAllDownloadsFinished();
}
