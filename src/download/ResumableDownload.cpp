#include "ResumableDownload.hpp"
#include <QCoreApplication>
#include "../net/RequestBudget.hpp"
#include "DownloadManager.hpp"
#include "DownloadRetryPolicy.hpp"
#include "SlowSourcePolicy.hpp"
#include "ZipMagic.hpp"
#include "../util/DownloadDiagnostics.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <algorithm>

namespace {
QString digest(const QString &path, const QSharedPointer<QAtomicInt> &abort) {
    if (QFileInfo(path).isSymLink()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (abort->loadRelaxed()) return {};
        const auto bytes = file.read(64 * 1024);
        if (bytes.isEmpty()) return {};
        hash.addData(bytes);
    }
    return QString::fromLatin1(hash.result().toHex());
}
QString hashText(const QByteArray &text) { return QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex()); }
void removeParts(const QString &directory) {
    // 只删除本协调器命名的文件，不递归删除目录，不跟随符号链接 / junction。
    // `QDir::entryList` 默认**不会**替你筛掉链接（`QDir::NoSymLinks` 没给），
    // 而 junction 在 `NoSymLinks` 下也照样列得出来（Qt 只认 symlink）—— 所以逐条自己判。
    // 目录本身是链接的那一档在 `start()` 里已经拒了，这里挡的是"链接混在缓存目录里"。
    static const QRegularExpression name(QStringLiteral("^part-[0-9]+\\.bin$"));
    for (const auto &file : QDir(directory).entryList({QStringLiteral("part-*.bin")}, QDir::Files | QDir::Hidden | QDir::System)) {
        if (!name.match(file).hasMatch()) continue;
        const QString path = QDir(directory).filePath(file);
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) continue;
        QFile::remove(path);
    }
    const QString manifest = QDir(directory).filePath(QStringLiteral("manifest.json"));
    const QFileInfo manifestInfo(manifest);
    if (!manifestInfo.isSymLink() && !manifestInfo.isJunction()) QFile::remove(manifest);
}
bool strongEtag(const QByteArray &value) {
    return value.size() >= 2 && value.size() <= 4096 && value.startsWith('"') && value.endsWith('"') && !value.contains('\r') && !value.contains('\n');
}
/// 「持续低速」的观察窗口（毫秒）。非 0 只可能来自验证工程
/// （`ResumableDownload::setLowSpeedWindowForTest`）—— 产品代码里没人写它。
qint64 &lowSpeedWindowOverride() {
    static qint64 ms = 0;
    return ms;
}
}

ResumableDownload::ResumableDownload(QObject *parent, NetworkManagerFactory networkFactory)
    : QObject(parent), m_networkFactory(std::move(networkFactory)) {
    // 每 30 秒问一次"还在动吗"；真正的期限是 kStuckGiveUpMs（5 分钟）。
    m_stuckTimer = new QTimer(this);
    m_stuckTimer->setInterval(30 * 1000);
    connect(m_stuckTimer, &QTimer::timeout, this, &ResumableDownload::checkStuck);
    // 持续低速比较的节拍：一个窗口至少看几下，别只在窗口边界上采一个点。
    m_rateTimer = new QTimer(this);
    connect(m_rateTimer, &QTimer::timeout, this, &ResumableDownload::maybeCompareSources);
}
ResumableDownload::~ResumableDownload() { cancel(); }

void ResumableDownload::setLowSpeedWindowForTest(qint64 ms) {
    lowSpeedWindowOverride() = qMax<qint64>(0, ms);
}

qint64 ResumableDownload::lowSpeedWindowMs() {
    const qint64 override = lowSpeedWindowOverride();
    return override > 0 ? override : SlowSourcePolicy::kSustainedWindowMs;
}

QString ResumableDownload::cacheDirectory(const QUrl &url, const QString &target, const QString &root) {
    const QString base = root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/range-downloads") : root;
    return QDir(base).filePath(hashText(url.toEncoded() + '\n' + QFileInfo(target).absoluteFilePath().toUtf8()));
}

void ResumableDownload::setSpeedLimitBytesPerSecond(qint64 bytesPerSecond) {
    m_speedLimitBytesPerSecond = bytesPerSecond;
    // 已经在跑的那一份也当场改（设置页改限速时，正在下的分段作业也要慢下来）。
    if (m_files != nullptr) {
        m_files->setSpeedLimitBytesPerSecond(bytesPerSecond);
    }
}

void ResumableDownload::start(const QUrl &url, const QString &target, const QString &sha1, qint64 size, bool overwrite, const QString &cacheRoot) {
    if (!cancel()) { emit statusChanged(tr("正在提交完整文件，请等待保存结束。")); return; }
    delete m_files;
    m_files = new DownloadManager(this, m_networkFactory);
    // 分段正文的字节数同时记进"本窗口"那一格：持续低速比较看的是**合计**速率，
    // 单条链路的速度由内部下载器的看门狗各自管。
    connect(m_files, &DownloadManager::transferBytes, this, [this](qint64 bytes) {
        if (m_stage == Stage::Parts) m_rateBytes += bytes;
        emit transferBytes(bytes);
    });
    // 先共用一个令牌桶，再推限速（顺序不能反：setter 写的是桶里那一份）。
    m_files->shareSpeedLimitBucketWith(m_speedBucketOwner);
    m_files->setUseMirror(false);
    m_files->setMaxConcurrentDownloads(kPartConcurrency);
    // （限速）：分段作业绕在自己这个下载器上 —— 限值必须在这儿也推一份，
    // 否则"大于 4 MiB 的文件"整批不受限速管（设置里明明写着 1 MB/s，大文件照样满速）。
    m_files->setSpeedLimitBytesPerSecond(m_speedLimitBytesPerSecond);
    // 关键：协调器的单文件回退（startSingle）会带着 sha1 + size 走同一个 DownloadManager，
    // 不关掉自动分段就会"探测 → 回退单发 → 又被升级成分段"无限递归。
    m_files->setSegmentedDownloadEnabled(false);
    m_abort = QSharedPointer<QAtomicInt>::create(0);
    m_running = true; m_stage = Stage::Probe;
    m_url = url; m_target = target; m_sha1 = sha1; m_expectedSize = size; m_overwrite = overwrite;
    m_total = -1; m_count = 0; m_complete.clear(); m_live.clear(); m_etag.clear(); m_networkAttempted = false;
    m_partsRetried = false; m_partsRetryPending = false;
    m_rateTimer->stop(); m_rateBytes = 0; m_nextCompareMs = 0; m_compareClock.start();
    m_sample = nullptr;
    m_urlHash = hashText(url.toEncoded()); m_cache = cacheDirectory(url, target, cacheRoot);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) || !QDir::isAbsolutePath(target)) {
        finish(false, tr("下载地址或保存路径无效。")); return;
    }
    const QFileInfo cacheInfo(m_cache);
    if (cacheInfo.isSymLink() || cacheInfo.isJunction() || !QDir().mkpath(m_cache)) {
        finish(false, tr("续传缓存目录不可用。"));
        return;
    }
    m_lock = QSharedPointer<QLockFile>::create(QDir(m_cache).filePath(QStringLiteral("lock")));
    if (!m_lock->tryLock(0)) { finish(false, tr("同一下载的缓存正被其他任务使用，请稍后重试。")); return; }
    connect(m_files, &DownloadManager::downloadProgress, this, [this](const QString &file, qint64 done, qint64 total) {
        if (!m_running) return;
        if (m_stage == Stage::Single) { emit progress(done, total); return; }
        if (m_stage != Stage::Parts) return;
        bool valid = false;
        const int index = file.mid(5, file.size() - 9).toInt(&valid);
        if (valid && index >= 0 && index < m_count && !m_complete.contains(index)) {
            m_live[index] = std::clamp<qint64>(done, 0, partLength(index)); emitProgress();
        }
    });
    connect(m_files, &DownloadManager::downloadFinished, this, [this](const QString &file, bool success, const QString &error) {
        if (!m_running) return;
        if (m_stage == Stage::Single) {
            if (success) clearCacheFiles();
            finish(success, error); return;
        }
        if (m_stage != Stage::Parts) return;
        if (!success) {
            // 第一轮里某一条分段失败：**先只补缺的那几条**，不要立刻整文件重下。
            // 已经完整校验过的分段都在缓存里；重新探测/校验后跳过它们，
            // 所以这一轮最坏只是把失败的那一条再走一遍；而整文件重下在 41 MB 的
            // 客户端 jar 上就是白扔掉已经下好的几十 MB。
            // 分段请求本身已由内部 DownloadManager 做过有限重试，剩下的失败多半来自
            // "落点过期/单条链路死掉"，换一条落点重来一次通常就过去了。
            //
            // 必须**等这一批剩下的分段都收尾**再补：现在就重发，同一批里还有别的
            // 分段在飞，`downloadParts()` 会往同一个 part 文件再开一条写流。所以这里
            // 只挂个标记，真正重发在 allDownloadsFinished 里（见 start() 的连接）。
            if (!m_partsRetried) {
                if (!m_partsRetryPending) {
                    m_partsRetryPending = true;
                    if (DownloadDiag::enabled()) {
                        DownloadDiag::line(QStringLiteral("resumable part failed, will retry missing parts only after this batch settles: %1 (complete=%2/%3)")
                                               .arg(error)
                                               .arg(m_complete.size())
                                               .arg(m_count));
                    }
                }
                return;
            }
            // Keep verified parts rather than silently redownloading the entire payload.
            // The outer manager can still fall back to an explicitly supplied official URL.
            m_stage = Stage::Switching;
            const auto generation = m_generation;
            QTimer::singleShot(0, this, [this, generation, error] {
                if (m_running && generation == m_generation) finish(false, error);
            });
            return;
        }
        bool valid = false;
        const int index = file.mid(5, file.size() - 9).toInt(&valid);
        if (!valid || index < 0 || index >= m_count) { finish(false, tr("分段编号无效。")); return; }
        const auto hash = digest(partPath(index), m_abort); // 每段上限 4 MiB；重启全量校验在后台。
        if (hash.isEmpty()) { finish(false, tr("无法校验已下载的分段。")); return; }
        m_complete[index] = hash; m_live.remove(index);
        QString problem;
        if (!saveManifest(&problem)) { finish(false, problem); return; }
        emit checkpointSaved(completedBytes()); emitProgress();
        if (m_complete.size() == m_count) {
            m_stage = Stage::Switching;
            const auto generation = m_generation;
            QTimer::singleShot(0, this, [this, generation] { if (m_running && generation == m_generation) assemble(); });
        }
    });
    // 第一轮某条分段失败后，等**这一批**剩下的分段都收尾再补缺的那几条。
    // 立刻重发会和还在飞的分段抢同一个 part 文件，所以失败的处理只挂标记，
    // 真正重发落在这里。
    connect(m_files, &DownloadManager::allDownloadsFinished, this, [this](bool) {
        if (!m_running || !m_partsRetryPending) return;
        m_partsRetryPending = false;
        if (m_stage != Stage::Parts) return;
        m_partsRetried = true;
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(QStringLiteral("resumable retry missing parts: missing=%1/%2")
                                   .arg(m_count - m_complete.size())
                                   .arg(m_count));
        }
        // 从 DownloadManager 自己的"整批结束"信号里再起一批会把内部记账卷进去，
        // 排到下一轮事件循环再发。
        const auto generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (!m_running || generation != m_generation || m_stage != Stage::Parts) return;
            m_stage = Stage::Probe;
            probe(); // Refresh expired signed URLs, then revalidate the saved parts/ETag.
        });
    });
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable start url=%1 target=%2 sha1=%3 size=%4 overwrite=%5 cache=%6")
                               .arg(DownloadDiag::safeUrl(m_url),
                                    QDir::toNativeSeparators(m_target),
                                    m_sha1.isEmpty() ? QStringLiteral("-") : m_sha1)
                               .arg(m_expectedSize)
                               .arg(m_overwrite ? QStringLiteral("yes") : QStringLiteral("no"),
                                    QDir::toNativeSeparators(m_cache)));
    }
    emit statusChanged(tr("正在检查分段与续传支持…"));
    m_progressClock.start();
    m_stuckTimer->start();
    probe();
}

void ResumableDownload::probe(int attempt) {
    if (!m_running) return;
    const auto cooldown = DownloadRetryPolicy::coolingDownMs(m_url);
    if (cooldown > 0) {
        const auto generation = m_generation;
        QTimer::singleShot(int(cooldown) + 1, this, [this, generation, attempt] {
            if (m_running && generation == m_generation) probe(attempt);
        });
        return;
    }
    auto lease = RequestBudget::tryAcquire(RequestBudget::Lane::Bulk);
    if (!lease) {
        const auto generation = m_generation;
        QTimer::singleShot(25, this, [this, generation, attempt] {
            if (m_running && generation == m_generation) probe(attempt);
        });
        return;
    }
    m_networkAttempted = true;
    QNetworkRequest request(m_url);
    request.setRawHeader("Range", "bytes=0-0"); request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(15000);
    auto *reply = m_files->networkAccess()->get(request);
    RequestBudget::attach(reply, lease);
    m_probe = reply;
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("probe send attempt=%1 url=%2 range=bytes=0-0 timeout=15000")
                               .arg(attempt)
                               .arg(DownloadDiag::safeUrl(m_url)));
    }
    reply->setReadBufferSize(4096);
    const auto generation = m_generation;
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply] {
        // 不支持 Range 的服务器可能开始返回几 GB 正文；立即停止探测，不把它读进内存。
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        static const QRegularExpression range(QStringLiteral("^bytes 0-0/[0-9]+$"));
        if (status == 200 || (status == 206 && !range.match(QString::fromLatin1(reply->rawHeader("Content-Range"))).hasMatch())) {
            reply->setProperty("rangeUnsupported", true); reply->abort();
        }
    });
    connect(reply, &QIODevice::readyRead, this, [reply] {
        auto data = reply->property("probeBytes").toByteArray();
        data += reply->read(qMax<qint64>(0, 2 - data.size()));
        reply->setProperty("probeBytes", data);
        if (data.size() > 1) { reply->setProperty("rangeUnsupported", true); reply->abort(); }
    });
    QTimer::singleShot(20000, reply, [reply] {
        if (reply->isFinished()) return;
        // 这是探测器自己的兜底（15 秒传输超时之外再兜一层）。它不是用户取消 ——
        // 不标记的话下面那条 `OperationCanceledError` 不是瞬态错误，会把整条分段作业
        // 直接判死（用户看到的是"操作已取消"，实际上谁也没取消）。
        reply->setProperty("wtfmlProbeGuardAbort", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, attempt] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto code = reply->error();
        const auto header = reply->rawHeader("Content-Range");
        const auto etag = reply->rawHeader("ETag");
        // 部分 CDN 对 Range 探测回 404，完整 GET 却能跳转到真实文件。
        // 只回退一次普通下载，仍校验状态、大小和哈希，不会把真正的 404 当成功。
        const bool unsupported = reply->property("rangeUnsupported").toBool() || status == 200 || status == 400 || status == 404 || status == 405 || status == 416 || status == 501;
        auto data = reply->property("probeBytes").toByteArray();
        if (data.isEmpty()) data = reply->read(2);
        const bool guardAbort = reply->property("wtfmlProbeGuardAbort").toBool();
        const auto reason = guardAbort ? tr("探测源站超时。") : DownloadRetryPolicy::displayError(reply->error(), reply->errorString());
        const auto retryAfter = reply->rawHeader("Retry-After");
        const auto effectiveUrl = reply->url();
        reply->deleteLater();
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(QStringLiteral("probe reply attempt=%1 status=%2 error=%3(%4) contentRange=%5 etag=%6 probeBytes=%7 effectiveUrl=%8 unsupportedFlag=%9")
                                   .arg(attempt)
                                   .arg(status > 0 ? QString::number(status) : QStringLiteral("-"))
                                   .arg(QString::number(int(code)), reason)
                                   .arg(QString::fromLatin1(header).trimmed().isEmpty()
                                            ? QStringLiteral("-")
                                            : QString::fromLatin1(header).trimmed(),
                                        etag.isEmpty() ? QStringLiteral("-")
                                                       : QString::fromLatin1(etag))
                                   .arg(data.size())
                                   .arg(DownloadDiag::safeUrl(effectiveUrl))
                                   .arg(reply->property("rangeUnsupported").toBool()
                                            ? QStringLiteral("yes")
                                            : QStringLiteral("no")));
        }
        if (generation != m_generation || !m_running) return;
        m_probe = nullptr;
        if (unsupported) {
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe -> single: range unsupported status=%1")
                                       .arg(status > 0 ? QString::number(status)
                                                       : QStringLiteral("-")));
            }
            startSingle();
            return;
        }
        if (code != QNetworkReply::NoError) {
            // 429 用专用退避（通用那条 0.5/1/2 秒比限流窗口还快）；探测兜底掐出来的
            // 超时也算瞬态，至少隔 2 秒再探，别把"源站很慢"当成"源站没有"。
            int delay = -1;
            if (status == 429) {
                delay = DownloadRetryPolicy::throttleDelayMs(attempt, retryAfter);
                DownloadRetryPolicy::coolDown(m_url, qMax(30000, delay));
                if (delay >= 0) delay = qMax(30000, delay);
            } else if (DownloadRetryPolicy::transientReply(reply) || guardAbort) {
                delay = DownloadRetryPolicy::delayMs(attempt, retryAfter);
                if (delay >= 0 && guardAbort) delay = qMax(delay, 2000);
            }
            if (delay >= 0) {
                if (DownloadDiag::enabled()) {
                    DownloadDiag::line(QStringLiteral("probe transient retry attempt=%1 delay=%2ms retryAfter=%3")
                                           .arg(attempt + 1)
                                           .arg(delay)
                                           .arg(retryAfter.isEmpty()
                                                    ? QStringLiteral("-")
                                                    : QString::fromLatin1(retryAfter)));
                }
                QTimer::singleShot(delay, this, [this, generation, attempt] { if (m_running && generation == m_generation) probe(attempt + 1); });
                return;
            }
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe failed: %1").arg(reason));
            }
            finish(false, reason);
            return;
        }
        static const QRegularExpression pattern(QStringLiteral("^bytes 0-0/([0-9]+)$"));
        const auto match = pattern.match(QString::fromLatin1(header));
        bool valid = false;
        const qint64 total = match.captured(1).toLongLong(&valid);
        if (status != 206 || !valid || data.size() != 1 || total <= 0) {
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe -> single: invalid probe response status=%1 total=%2 probeBytes=%3")
                                       .arg(status)
                                       .arg(valid ? QString::number(total) : QStringLiteral("-"))
                                       .arg(data.size()));
            }
            startSingle();
            return;
        }
        if (m_expectedSize > 0 && total != m_expectedSize) {
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe rejected: source size %1 != expected %2")
                                       .arg(total)
                                       .arg(m_expectedSize));
            }
            finish(false, tr("源站文件大小与预期不一致。"));
            return;
        }
        m_total = total;
        m_source = effectiveUrl;
        // 续传身份只认**入口地址**：BMCLAPI 每次都 302 到 60 秒过期的预签名落点，把落点
        // 算进身份会让每次重启的 urlHash 都变，跨重启续传永远失效。内容是否变化由
        // manifest 里的 ETag / 总长 / 分段 SHA-256 和整文件 SHA-1 判断。
        if (strongEtag(etag)) m_etag = etag;
        // 没有强校验依据时不跨请求拼接，以免源文件变化而大小未变。
        // 先限制大小再做向上取整，避免恶意 Content-Range 的整数溢出。
        if (total < kMinimumBytes || total > kMaximumBytes ||
            (m_etag.isEmpty() && m_sha1.isEmpty())) {
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe -> single: unsafe for ranges total=%1 etag=%2 sha1=%3")
                                       .arg(total)
                                       .arg(m_etag.isEmpty() ? QStringLiteral("-")
                                                             : QString::fromLatin1(m_etag),
                                            m_sha1.isEmpty() ? QStringLiteral("-") : m_sha1));
            }
            startSingle();
            return;
        }
        m_partSize = std::max<qint64>(1024 * 1024,
                                      std::min<qint64>(4 * 1024 * 1024,
                                                       (total + kPartConcurrency - 1) / kPartConcurrency));
        if ((total + m_partSize - 1) / m_partSize > 8192) {
            if (DownloadDiag::enabled()) {
                DownloadDiag::line(QStringLiteral("probe -> single: would need %1 parts (limit 8192)")
                                       .arg((total + m_partSize - 1) / m_partSize));
            }
            startSingle();
            return;
        }
        m_count = int((total + m_partSize - 1) / m_partSize);
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(QStringLiteral("probe -> parts total=%1 partSize=%2 parts=%3 etag=%4 effectiveUrl=%5")
                                   .arg(total)
                                   .arg(m_partSize)
                                   .arg(m_count)
                                   .arg(m_etag.isEmpty() ? QStringLiteral("-")
                                                         : QString::fromLatin1(m_etag),
                                        DownloadDiag::safeUrl(effectiveUrl)));
        }
        prepareCache();
    });
}

QString ResumableDownload::partPath(int index) const { return QDir(m_cache).filePath(QStringLiteral("part-%1.bin").arg(index)); }
qint64 ResumableDownload::partLength(int index) const { return std::min(m_partSize, m_total - index * m_partSize); }
qint64 ResumableDownload::completedBytes() const { qint64 result = 0; for (auto it = m_complete.cbegin(); it != m_complete.cend(); ++it) result += partLength(it.key()); return result; }
void ResumableDownload::emitProgress() {
    // 有任何一段在动就算"有进展"：卡死兜底（checkStuck）看的是这个表。
    m_progressClock.restart();
    qint64 result = completedBytes();
    for (auto bytes : m_live) result += bytes;
    emit progress(result, m_total);
}

void ResumableDownload::prepareCache() {
    m_stage = Stage::Prepare;
    m_live.clear(); // Discard partial progress from the previous settled range batch.
    emit statusChanged(tr("正在后台校验续传缓存…"));
    struct Result { QMap<int, QString> parts; };
    auto result = QSharedPointer<Result>::create();
    const auto cache = m_cache, identity = m_urlHash, sha1 = m_sha1;
    const auto etag = m_etag;
    const auto total = m_total, partSize = m_partSize;
    const int count = m_count;
    const auto token = m_abort; const auto lock = m_lock; const auto generation = m_generation;
    auto *worker = QThread::create([=] {
        QFile state(QDir(cache).filePath(QStringLiteral("manifest.json")));
        QJsonObject json;
        if (state.open(QIODevice::ReadOnly) && state.size() < 4 * 1024 * 1024) json = QJsonDocument::fromJson(state.readAll()).object();
        state.close();
        if (json.value("schema").toInt() != 1 || json.value("urlHash").toString() != identity ||
            json.value("etag").toString().toLatin1() != etag || json.value("sha1").toString() != sha1 ||
            qint64(json.value("total").toDouble()) != total || qint64(json.value("partSize").toDouble()) != partSize) {
            removeParts(cache); return;
        }
        for (const auto &value : json.value("parts").toArray()) {
            if (token->loadRelaxed()) return;
            const auto item = value.toObject(); const int index = item.value("index").toInt(-1);
            if (index < 0 || index >= count) continue;
            const auto file = QDir(cache).filePath(QStringLiteral("part-%1.bin").arg(index));
            const auto hash = item.value("sha256").toString();
            if (hash.size() == 64 && QFileInfo(file).size() == std::min(partSize, total - index * partSize) && digest(file, token) == hash)
                result->parts[index] = hash;
        }
        Q_UNUSED(lock);
    });
    connect(worker, &QThread::finished, this, [this, result, generation] {
        if (!m_running || generation != m_generation) return;
        m_complete = result->parts;
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(QStringLiteral("resumable cache: restored=%1/%2 cache=%3")
                                   .arg(QString::number(m_complete.size()), QString::number(m_count),
                                    QDir::toNativeSeparators(m_cache)));
        }
        emit statusChanged(m_complete.isEmpty() ? tr("正在并行下载分段…") : tr("已恢复 %1 个完整分段，继续下载剩余部分…").arg(m_complete.size()));
        downloadParts();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

bool ResumableDownload::saveManifest(QString *error) {
    QJsonArray parts;
    for (auto it = m_complete.cbegin(); it != m_complete.cend(); ++it) parts.append(QJsonObject{{"index", it.key()}, {"sha256", it.value()}});
    const QJsonObject state{{"schema", 1}, {"urlHash", m_urlHash}, {"etag", QString::fromLatin1(m_etag)}, {"sha1", m_sha1},
                            {"total", double(m_total)}, {"partSize", double(m_partSize)}, {"parts", parts}};
    QSaveFile file(QDir(m_cache).filePath(QStringLiteral("manifest.json")));
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = tr("无法保存续传记录：%1").arg(file.errorString());
        return false;
    }
    return true;
}

void ResumableDownload::downloadParts() {
    if (m_complete.size() == m_count) { assemble(); return; }
    m_stage = Stage::Parts;
    // 持续低速比较的窗口从这里重新起算（换源、补段之后都算新的一轮）。
    m_rateBytes = 0;
    m_rateClock.start();
    if (!m_fallbackUrl.isEmpty()) {
        m_rateTimer->setInterval(int(qBound<qint64>(500, lowSpeedWindowMs() / 5, qint64(5000))));
        m_rateTimer->start();
    }
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable parts: missing=%1/%2 partSize=%3 total=%4 url=%5")
                               .arg(m_count - m_complete.size())
                               .arg(m_count)
                               .arg(m_partSize)
                               .arg(QString::number(m_total), DownloadDiag::safeUrl(m_source)));
    }
    QString problem;
    if (!saveManifest(&problem)) { finish(false, problem); return; }
    emitProgress();
    m_files->setAllowOverwrite(true);
    m_files->beginBatch();
    // Reuse the probed node for this batch instead of repeatedly hitting the dispatcher.
    // A settled failed batch reprobes once to refresh expired signatures safely.
    for (int index = 0; index < m_count; ++index) {
        if (m_complete.contains(index)) continue;
        DownloadRange range{index * m_partSize, index * m_partSize + partLength(index) - 1, m_total, m_etag};
        m_files->downloadFile(m_source.toString(), partPath(index), {}, partLength(index), VerifyMode::AlwaysDownload, {}, range);
    }
    m_files->endBatch();
}

void ResumableDownload::maybeCompareSources() {
    if (!m_running || m_stage != Stage::Parts) return;
    // 没有退路、已经换过一次（换完就把备用地址清空了）、样本还在飞、或者设了限速：
    // 这几种情况下"慢"都不是源站的证据（限速是本地令牌桶，样本会绕过它，比出来的是假的）。
    if (m_fallbackUrl.isEmpty() || m_sample || m_speedLimitBytesPerSecond > 0) return;
    if (m_compareClock.elapsed() < m_nextCompareMs) return;
    const qint64 window = m_rateClock.elapsed();
    if (window < lowSpeedWindowMs()) return;
    const qint64 rate = m_rateBytes * 1000 / qMax<qint64>(1, window);
    const qint64 remaining = m_total - completedBytes();
    // 换源会把入口换成备用地址，而备用地址的 ETag 很可能和现在的不同 ——
    // `prepareCache()` 认不出就整批作废旧分段。只有"前路还长"才值得冒这个险：
    // 备用地址快三倍时，最坏情况（从零重下）也才和"照现在的速度下完剩下的"打平。
    const bool worthSwitching = remaining > 0 && remaining * 3 > m_total;
    const bool slow = SlowSourcePolicy::shouldCompare(window, m_rateBytes, remaining,
                                                      m_speedLimitBytesPerSecond > 0,
                                                      lowSpeedWindowMs());
    if (!slow || !worthSwitching) {
        // 这一窗不算数：下一个窗口重新攒（速率是**窗口内**的合计，不能越拖越平均）。
        m_rateBytes = 0;
        m_rateClock.start();
        return;
    }
    auto lease = RequestBudget::tryAcquire(RequestBudget::Lane::Bulk);
    if (!lease) {
        // 名额紧：不要拿着一个越来越长的窗口去比 —— 下一拍再来。
        m_rateBytes = 0;
        m_rateClock.start();
        return;
    }
    QNetworkRequest request(m_fallbackUrl);
    request.setRawHeader("Range", "bytes=0-65535");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setTransferTimeout(6000);
    auto *reply = m_files->networkAccess()->get(request);
    RequestBudget::attach(reply, lease);
    m_sample = reply;
    reply->setReadBufferSize(64 * 1024);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto bytes = std::make_shared<qint64>(0);
    connect(reply, &QIODevice::readyRead, reply, [reply, bytes] {
        *bytes += reply->read(qMax<qint64>(0, 65536 - *bytes)).size();
        if (*bytes >= 65536) {
            reply->setProperty("wtfmlSampleComplete", true);
            reply->abort();  // 不认 Range 的服务器会把整个文件倒过来，64 KiB 就够了
        }
    });
    QTimer::singleShot(6000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable source comparison start currentBps=%1 windowMs=%2 complete=%3/%4 fallback=%5")
                               .arg(rate)
                               .arg(window)
                               .arg(m_complete.size())
                               .arg(m_count)
                               .arg(DownloadDiag::safeUrl(m_fallbackUrl)));
    }
    const auto generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, rate, clock, bytes] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool sampled = (status == 200 || status == 206) &&
            (reply->error() == QNetworkReply::NoError ||
             reply->property("wtfmlSampleComplete").toBool());
        const bool switchSource =
            sampled && SlowSourcePolicy::better(rate, *bytes, qMax<qint64>(1, clock->elapsed()));
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(QStringLiteral("resumable source comparison: status=%1 bytes=%2 elapsedMs=%3 currentBps=%4 switch=%5")
                                   .arg(status > 0 ? QString::number(status) : QStringLiteral("-"))
                                   .arg(*bytes)
                                   .arg(clock->elapsed())
                                   .arg(rate)
                                   .arg(switchSource ? QStringLiteral("yes") : QStringLiteral("no")));
        }
        reply->deleteLater();
        if (m_sample == reply) m_sample = nullptr;
        if (!m_running || generation != m_generation || m_stage != Stage::Parts) return;
        // 换与不换都从这里开一个新窗口；没换成的退避一分钟再比，别拿 64 KiB 反复探。
        m_rateBytes = 0;
        m_rateClock.start();
        m_nextCompareMs = m_compareClock.elapsed() + 60000;
        if (!switchSource) return;
        switchToFallback();
    });
}

void ResumableDownload::switchToFallback() {
    if (!m_running || m_stage != Stage::Parts || m_fallbackUrl.isEmpty()) return;
    const QUrl fallback = m_fallbackUrl;
    // 只换一次：换过去之后不再把原入口当"退路"来回弹（两个都不快时只会来回跳）。
    m_fallbackUrl = QUrl();
    m_rateTimer->stop();
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable source switch from=%1 to=%2 complete=%3/%4")
                               .arg(DownloadDiag::safeUrl(m_url),
                                    DownloadDiag::safeUrl(fallback))
                               .arg(m_complete.size())
                               .arg(m_count));
    }
    // 换的是**入口地址**：备用地址自己也可能 302 到一条很快过期的落点。重新走一遍
    // `probe()` 拿落点和 ETag —— `m_source` 是那一趟的产物，拿入口地址直接去发 Range
    // 会绕过它，签名 URL 那类落点 60 秒就作废了（和 `checkStuck()` 重新探测同一个理由）。
    // 分段正文一条都不跨来源拼：ETag 变了 `prepareCache()` 会把旧分段整批作废。
    // 这一轮补段的次数也给新来源重新记一份（换源只有一次，不会因此无限重试）。
    m_url = fallback;
    m_partsRetried = false;
    m_partsRetryPending = false;
    m_stage = Stage::Probe;  // 先改状态：`cancelBatch()` 的收尾信号别再走进分段分支
    m_files->cancelBatch();
    m_live.clear();
    m_progressClock.restart();
    m_rateBytes = 0;
    m_rateClock.start();
    m_nextCompareMs = 0;
    probe();
}

void ResumableDownload::startSingle() {
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable -> single (reason in previous probe line) url=%1 target=%2")
                               .arg(DownloadDiag::safeUrl(m_url),
                                    QDir::toNativeSeparators(m_target)));
    }
    m_stage = Stage::Switching;
    m_files->cancelBatch();
    m_stage = Stage::Single;
    emit statusChanged(tr("源站未满足安全分段条件，使用普通下载与有限重试。"));
    m_files->setAllowOverwrite(m_overwrite);
    m_files->setCommitGuard(m_commitGuard);
    m_files->beginBatch();
    m_files->downloadFile(m_url.toString(), m_target, m_sha1, m_expectedSize, VerifyMode::AlwaysDownload);
    m_files->endBatch();
}

void ResumableDownload::assemble() {
    m_stage = Stage::Assemble;
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable assemble start parts=%1 total=%2 target=%3")
                               .arg(m_count)
                               .arg(m_total)
                               .arg(QDir::toNativeSeparators(m_target)));
    }
    emit statusChanged(tr("所有分段已就绪，正在后台合并、校验并保存…"));
    auto result = QSharedPointer<QString>::create();
    const auto cache = m_cache, target = m_target, expectedHash = m_sha1;
    const auto complete = m_complete;
    const auto token = m_abort;
    const auto lock = m_lock;
    const auto generation = m_generation;
    const auto total = m_total; const bool overwrite = m_overwrite;
    const auto commitGuard = m_commitGuard;
    auto *worker = QThread::create([=] {
        Q_UNUSED(lock);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile output(target); output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) { *result = output.errorString(); return; }
        QCryptographicHash whole(QCryptographicHash::Sha1);
        qint64 written = 0; QByteArray prefix;
        for (auto it = complete.cbegin(); it != complete.cend(); ++it) {
            const QString path = QDir(cache).filePath(QStringLiteral("part-%1.bin").arg(it.key()));
            QFile part(path);
            if (QFileInfo(path).isSymLink() || !part.open(QIODevice::ReadOnly)) { *result = QCoreApplication::translate("ResumableDownload", "续传分段不可读。"); return; }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while (!part.atEnd()) {
                if (token->loadRelaxed()) return;
                const auto data = part.read(64 * 1024);
                if (data.isEmpty() || output.write(data) != data.size()) { *result = QCoreApplication::translate("ResumableDownload", "无法合并分段。"); return; }
                if (prefix.size() < 4) prefix += data.left(4 - prefix.size());
                hash.addData(data); whole.addData(data); written += data.size();
            }
            if (QString::fromLatin1(hash.result().toHex()) != it.value()) { *result = QCoreApplication::translate("ResumableDownload", "续传缓存已经改变，请重试以重新下载损坏分段。"); return; }
        }
        if (written != total || (!expectedHash.isEmpty() && QString::fromLatin1(whole.result().toHex()).compare(expectedHash, Qt::CaseInsensitive))) {
            *result = QCoreApplication::translate("ResumableDownload", "合并后的大小或 SHA-1 校验失败。"); return;
        }
        const auto ext = QFileInfo(target).suffix().toLower();
        if ((ext == QStringLiteral("jar") || ext == QStringLiteral("zip")) && expectedHash.isEmpty() &&
            !ZipMagic::looksLikeZipHead(prefix)) {
            *result = QCoreApplication::translate("ResumableDownload", "下载内容不是有效的 ZIP/JAR 文件。"); return;
        }
        if (token->loadRelaxed()) return;
        if (!overwrite && (QFileInfo::exists(target) || QFileInfo(target).isSymLink())) { *result = QCoreApplication::translate("ResumableDownload", "目标文件已出现，未覆盖。"); return; }
        if (commitGuard) { *result = commitGuard(); if (!result->isEmpty()) return; }
        // 取消和原子提交只有一个能获准：接受取消后绝不再替换旧目标。
        if (!token->testAndSetOrdered(0, 2)) return;
        if (!output.commit()) *result = output.errorString();
        else token->storeRelease(3);
    });
    connect(worker, &QThread::finished, this, [this, result, generation] {
        if (!m_running || generation != m_generation) return;
        if (DownloadDiag::enabled()) {
            DownloadDiag::line(result->isEmpty()
                                   ? QStringLiteral("resumable assemble ok target=%1 bytes=%2")
                                         .arg(QDir::toNativeSeparators(m_target))
                                         .arg(m_total)
                                   : QStringLiteral("resumable assemble failed: %1").arg(*result));
        }
        if (result->isEmpty()) { clearCacheFiles(); emit progress(m_total, m_total); }
        finish(result->isEmpty(), *result);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void ResumableDownload::checkStuck() {
    if (!m_running) return;
    // 只盯**网络阶段**：Prepare（本地校验缓存）和 Assemble（本地合并）可能合法地跑很久，
    // 磁盘慢不该被当成"下载卡住"。
    if (m_stage != Stage::Probe && m_stage != Stage::Parts) return;
    if (m_progressClock.elapsed() < kStuckGiveUpMs) return;
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable stuck stage=%1 elapsed=%2ms complete=%3/%4")
                               .arg(m_stage == Stage::Parts ? QStringLiteral("parts")
                                                            : QStringLiteral("probe"))
                               .arg(m_progressClock.elapsed())
                               .arg(m_complete.size())
                               .arg(m_count));
    }
    if (m_stage == Stage::Parts && !m_partsRetried) {
        // 进度账已经不可信：把这一批全掐掉、只补缺的段再来一次。这是**恢复**、不是失败，
        // 和"某一段失败"那条路共用同一个次数上限（m_partsRetried），所以最多多花一轮。
        //
        // 重发之前**必须重新 probe()**，不能直接 `downloadParts()`。
        // `m_source` 是探测那一趟拿到的 302 落点（BMCLAPI 那种预签名地址 60 秒就过期，
        // 见上面 `m_source` 那一行的注释），而能走到这里说明**至少 5 分钟**没有进展了 ——
        // 拿那个落点发 Range 等于打在一条早就作废的地址上：403 → 这一轮再失败一次 →
        // `m_partsRetried` 已经是 true ⇒ 落进"整条判死"，用户看到"下载失败"，而正确的
        // 恢复代码（`probe()` → `prepareCache()` 复核 ETag / 分段 SHA）就在旁边。
        // 源站内容换了的时候也一样：重新探测的 ETag 对不上 ⇒ 缓存整批作废、老实重下，
        // 而不是带着旧的 If-Range 去撞一个 416。
        // `probe()` 自己会走 `prepareCache()`，那儿会把上一批残留的 `m_live` 清掉
        // （不然进度会先虚高再回落 —— 进度账看着"跳过一段"就是这么来的）。
        m_partsRetried = true;
        m_partsRetryPending = false;
        m_stage = Stage::Probe;  // 先改状态：`cancelBatch()` 的收尾信号别再走进分段分支
        m_files->cancelBatch();
        m_live.clear();
        m_progressClock.restart();
        probe();
        return;
    }
    finish(false, tr("下载长时间没有任何进展，已停止。请检查网络后重试。"));
}

void ResumableDownload::clearCacheFiles() const { removeParts(m_cache); }
void ResumableDownload::finish(bool success, const QString &error) {
    if (!m_running) return;
    m_running = false; m_stage = Stage::Done;
    m_stuckTimer->stop();
    // 比较样本也是本对象发出去的一条请求：收尾时一起掐掉（它的回调按代次认，不会误收）。
    m_rateTimer->stop();
    if (m_sample) { m_sample->abort(); m_sample = nullptr; }
    if (DownloadDiag::enabled()) {
        DownloadDiag::line(QStringLiteral("resumable finished success=%1 error=%2 target=%3")
                               .arg(success ? QStringLiteral("yes") : QStringLiteral("no"),
                                    error.isEmpty() ? QStringLiteral("-") : error,
                                    QDir::toNativeSeparators(m_target)));
    }
    if (!success && m_files) m_files->cancelBatch();
    m_lock.reset();
    emit finished(success, error);
}
bool ResumableDownload::canCancel() const {
    return m_running && (m_stage != Stage::Assemble || !m_abort || m_abort->loadAcquire() < 2);
}
bool ResumableDownload::cancel() {
    if (m_running && m_stage == Stage::Assemble && m_abort &&
        !m_abort->testAndSetOrdered(0, 1) && m_abort->loadAcquire() >= 2) return false;
    ++m_generation;
    m_running = false; m_stage = Stage::Done;
    if (m_stuckTimer) m_stuckTimer->stop();
    if (m_rateTimer) m_rateTimer->stop();
    if (m_sample) { m_sample->abort(); m_sample = nullptr; }
    if (m_abort) m_abort->storeRelaxed(1);
    if (m_probe) { m_probe->abort(); m_probe = nullptr; }
    if (m_files) m_files->cancelBatch();
    m_lock.reset();
    return true;
}
