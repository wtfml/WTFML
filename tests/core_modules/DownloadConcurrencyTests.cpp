#include "Support.hpp"
#include "download/ResourceStore.hpp"

#include <QEventLoop>
#include <QTimer>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <cstring>

namespace {
class ManifestRaceReply final : public QNetworkReply {
public:
    ManifestRaceReply(const QNetworkRequest &request, QByteArray response, int delay, QObject *parent,
                      std::function<void(QNetworkReply *, bool)> stalled = {})
        : QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        QTimer::singleShot(delay, this, [this, response = std::move(response), stalled] {
            if (isFinished()) return;
            const bool slow = response == QByteArray("FORGE_REDIRECT_SLOW") || response == QByteArray("SLOW");
            const bool redirects = response.startsWith("FORGE_REDIRECT_");
            if (redirects || response == QByteArray("NO_DATA") || slow) {
                if (redirects) {
                    QUrl target = this->request().url();
                    target.setHost(QStringLiteral("mediafilez.forgecdn.net"));
                    emit redirected(target);
                    setUrl(target);
                    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, slow ? 200 : 302);
                }
                if (slow) {
                    m_body = QByteArray("o");
                    emit readyRead();
                }
                if (stalled) stalled(this, slow);
                return;
            }
            if (response == QByteArray("NETWORK_FAILURE")) {
                setError(QNetworkReply::UnknownNetworkError, QStringLiteral("controlled failure"));
                setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 503);
            } else if (response == QByteArray("NOT_FOUND")) {
                setError(QNetworkReply::ContentNotFoundError, QStringLiteral("controlled 404"));
                setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 404);
            } else if (response == QByteArray("REDIRECT_DNS_FAILURE")) {
                const QUrl target(QStringLiteral("https://mirror-node.invalid/file"));
                emit redirected(target);
                setUrl(target);
                setError(QNetworkReply::HostNotFoundError, QStringLiteral("controlled DNS failure"));
            } else {
                m_body = response;
            }
            setFinished(true); emit readyRead(); emit finished();
        });
    }
    void abort() override {
        if (isFinished()) return;
        setError(OperationCanceledError, QStringLiteral("cancelled"));
        setFinished(true); emit finished();
    }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override {
        const qint64 count = qMin<qint64>(maximum, m_body.size() - m_offset);
        if (count <= 0) return -1;
        std::memcpy(data, m_body.constData() + m_offset, size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
class ManifestRaceNetwork final : public QNetworkAccessManager {
public:
    using Response = std::function<QPair<QByteArray,int>(const QUrl &, int)>;
    ManifestRaceNetwork(QObject *owner, Response response) : QNetworkAccessManager(owner), response(std::move(response)) {}
    int requests = 0;
    QList<QUrl> urls;
    std::function<void(QNetworkReply *, bool)> stalled;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        urls.append(request.url());
        const auto reply = response(request.url(), ++requests);
        return new ManifestRaceReply(request, reply.first, reply.second, this, stalled);
    }
private:
    Response response;
};

void watchStalledReplies(ManifestRaceNetwork *network, DownloadManager &manager) {
    network->stalled = [&manager](QNetworkReply *reply, bool slow) {
        const auto tick = [&manager] { QMetaObject::invokeMethod(&manager, "onStallWatchdogTick"); };
        if (slow) {
            // The first sample sees the one delivered byte. A second sample reaches the strike limit.
            tick();
            tick();
        } else {
            QTimer::singleShot(80, reply, tick);
        }
    };
}
QByteArray manifestBody(const QByteArray &id) {
    return "{\"versions\":[{\"id\":\"" + id + "\",\"type\":\"release\",\"url\":\"https://example.test/version.json\"}]}";
}
void runManifestRaceScenario(int scenario) {
    ManifestRaceNetwork *network = nullptr;
    DownloadManager manager(nullptr, [&](QObject *owner) {
        network = new ManifestRaceNetwork(owner, [scenario](const QUrl &url, int number) {
            const bool official = url.host() == QStringLiteral("piston-meta.mojang.com") || url.host() == QStringLiteral("launchermeta.mojang.com");
            if (scenario == 0) return qMakePair(manifestBody(official ? "official" : "domestic"), official ? 40 : 5);
            if (scenario == 1) return qMakePair(official ? manifestBody("official") : QByteArray("{\"versions\":[]}"), official ? 25 : 5);
            if (scenario == 2) return qMakePair(QByteArray("not json"), official ? 25 : 5);
            return qMakePair(manifestBody(number <= 2 ? "stale" : "current"), number <= 2 ? 50 : 10);
        });
        return network;
    });
    int emissions = 0;
    QObject::connect(&manager, &DownloadManager::versionManifestFetched, &manager, [&](const auto &) { ++emissions; });
    manager.fetchVersionManifest();
    check(network->requests == 2, "manifest starts both domestic and official requests concurrently");
    if (scenario == 3) manager.fetchVersionManifest();
    QEventLoop loop; QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
    const auto versions = manager.getAvailableVersions();
    const QString expected = scenario == 0 ? QStringLiteral("domestic") : scenario == 3 ? QStringLiteral("current") : QStringLiteral("official");
    check(emissions == 1 && (scenario == 2 ? versions.isEmpty() : versions.size() == 1 && versions.first().id == expected),
          "manifest race emits once, validates payloads, cancels losers and rejects stale refreshes",
          QStringLiteral("scenario %1 / signals %2 / entries %3").arg(scenario).arg(emissions).arg(versions.size()));
}
}


void runDownloadConcurrencyTests() {
    section("ZO. 「同时最多下几个」那几档");

    // ---------- ZO0. 四档 + 出厂值 ----------
    {
        const QList<int> presets = DownloadConcurrency::presets();
        check(presets == QList<int>({6, 12, 24, 48}),
              "ZO0 四档就是那四条线（6/12/24/48），次序 = 界面上的次序",
              QStringList([&presets] {
                  QStringList out;
                  for (int v : presets) { out << QString::number(v); }
                  return out;
              }()).join(QStringLiteral(",")));
        check(presets.contains(DownloadConcurrency::kDefault),
              "★ ZO0 出厂那一档**必须**是四档之一（否则下拉框一上来就指不到盘上那个值）",
              QString::number(DownloadConcurrency::kDefault));
        check(DownloadConcurrency::kMin >= 1,
              "★ ZO0 下限不小于 1（0 会让队列一泵都不泵，表现是「下载停在 0%」）",
              QString::number(DownloadConcurrency::kMin));
        check(DownloadConcurrency::kMax >= presets.last(),
              "ZO0 上限不低于最激进那一档（不然那一档自己就落在界外了）",
              QString::number(DownloadConcurrency::kMax));
        check(DownloadManager::kMaxConcurrentDownloads == DownloadConcurrency::kDefault,
              "★ ZO0 DownloadManager 那个常量 == 判据层的出厂值（同一个数，不是各写一遍）",
              QString::number(DownloadManager::kMaxConcurrentDownloads));
    }

    // ---------- ZO1. 归一化：越界回默认、表外的合法值原样留 ----------
    {
        check(DownloadConcurrency::normalize(0) == DownloadConcurrency::kDefault,
              "★★ ZO1 0 ⇒ 默认（不夹到下限 1：那等于「用户自己选了一个 1 路」）",
              QString::number(DownloadConcurrency::normalize(0)));
        check(DownloadConcurrency::normalize(-5) == DownloadConcurrency::kDefault,
              "ZO1 负数 ⇒ 默认",
              QString::number(DownloadConcurrency::normalize(-5)));
        check(DownloadConcurrency::normalize(999) == DownloadConcurrency::kDefault,
              "★★ ZO1 越过大上限 ⇒ **默认**（不夹到 64：夹到边上会让被限流的形状看起来像用户选的）",
              QString::number(DownloadConcurrency::normalize(999)));
        check(DownloadConcurrency::normalize(DownloadConcurrency::kMin) == DownloadConcurrency::kMin &&
                  DownloadConcurrency::normalize(DownloadConcurrency::kMax) == DownloadConcurrency::kMax,
              "ZO1 两个边界值本身都留得住");
        check(DownloadConcurrency::normalize(32) == 32,
              "★★ ZO1 表外的合法值（手改过配置的 32）**原样留** —— 回默认等于把用户手写的数抹掉",
              QString::number(DownloadConcurrency::normalize(32)));
        bool presetsIntact = true;
        for (int v : DownloadConcurrency::presets()) {
            if (DownloadConcurrency::normalize(v) != v || !DownloadConcurrency::isPreset(v)) {
                presetsIntact = false;
            }
        }
        check(presetsIntact, "ZO1 四档自己过一遍归一化都原样回来，而且都被认成「是那一档」");
        check(!DownloadConcurrency::isPreset(32) && !DownloadConcurrency::isPreset(0),
              "ZO1 32 和 0 都不是那四档之一（下拉框要给它补一格「自定义」）");
    }

    // ---------- ZO2. DownloadManager 上真在用哪一档 ----------
    {
        DownloadManager mgr;
        check(mgr.maxConcurrentDownloads() == DownloadManager::kMaxConcurrentDownloads,
              "ZO2 刚建的下载器用的是**出厂那一档**（谁都不设的时候行为一个字不变）",
              QString::number(mgr.maxConcurrentDownloads()));
        mgr.setMaxConcurrentDownloads(6);
        check(mgr.maxConcurrentDownloads() == 6, "★ ZO2 设成 6 ⇒ 就是 6",
              QString::number(mgr.maxConcurrentDownloads()));
        mgr.setMaxConcurrentDownloads(999);
        check(mgr.maxConcurrentDownloads() == DownloadConcurrency::kDefault,
              "★ ZO2 设一个越界的值 ⇒ 回到默认（归一化也在 setter 里过一遍）",
              QString::number(mgr.maxConcurrentDownloads()));
        mgr.setMaxConcurrentDownloads(12);
        check(mgr.maxConcurrentDownloads() == 12, "ZO2 再设成 12 ⇒ 12",
              QString::number(mgr.maxConcurrentDownloads()));
    }

    // ---------- ZO3. 「还能不能再发一条」那一问按**给的那个上限**算 ----------
    {
        // 上限 6 那一档：到 6 条就一条都不发（不管后面还排着多少）。
        check(DownloadManager::mayIssueAnotherDownload(5, 1, 6),
              "ZO3 上限 6、飞着 5 条、还排着 1 条 ⇒ 发");
        check(!DownloadManager::mayIssueAnotherDownload(6, 1, 6),
              "★★ ZO3 上限 6、飞着 6 条 ⇒ **不发**（哪怕后面还排着一百条）");
        check(!DownloadManager::mayIssueAnotherDownload(6, 0, 6),
              "ZO3 上限 6、飞着 6 条、队列空 ⇒ 也不发（队列空那一半照旧）");
        check(DownloadManager::mayIssueAnotherDownload(0, 1, 6),
              "ZO3 一条都没飞、有排队的 ⇒ 发");
        check(!DownloadManager::mayIssueAnotherDownload(0, 0, 6),
              "ZO3 队列空 ⇒ 不发（没东西可发）");
        // 两参数那个版本 = 拿出厂那一档问一遍（验证工程的老断言按它钉边界）。
        check(DownloadManager::mayIssueAnotherDownload(
                  DownloadManager::kMaxConcurrentDownloads - 1, 1) &&
                  !DownloadManager::mayIssueAnotherDownload(
                      DownloadManager::kMaxConcurrentDownloads, 1),
              "★ ZO3 两参数那个版本就是拿出厂那一档问的（老断言不跟着改动漂）");
    }    for (int scenario = 0; scenario < 4; ++scenario) runManifestRaceScenario(scenario);
    {
        QTemporaryDir directory;
        ManifestRaceNetwork *network = nullptr;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [](const QUrl &, int number) {
                return qMakePair(number == 1 ? QByteArray("NETWORK_FAILURE") : QByteArray("verified-client"), 5);
            });
            return network;
        });
        manager.setUseMirror(false);
        int completions = 0; bool succeeded = false;
        QEventLoop loop;
        QObject::connect(&manager, &DownloadManager::downloadFinished, &loop,
            [&](const QString &, bool ok, const QString &) { ++completions; succeeded = ok; loop.quit(); });
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("verified-client", QCryptographicHash::Sha1).toHex());
        const QString path = directory.filePath(QStringLiteral("client.jar"));
        manager.downloadClientJar(QStringLiteral("1.21.1"), QStringLiteral("https://piston-data.mojang.com/client.jar"),
                                  path, hash, 15);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit); loop.exec();
        QFile file(path); const bool opened = file.open(QIODevice::ReadOnly);
        check(completions == 1 && succeeded && network->requests == 2 &&
              network->urls.value(1) == QUrl(DownloadManager::mirrorClientJarUrl(QStringLiteral("1.21.1"))) && opened && file.readAll() == QByteArray("verified-client"),
              "official-priority client download falls back to domestic source and still verifies its hash");
        const auto [received, total] = manager.batchByteProgress();
        check(received == 15 && total == 15,
              "a failed attempt does not add its bytes to the completed file's batch progress");
    }
    {
        QTemporaryDir directory;
        ManifestRaceNetwork *network = nullptr;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [](const QUrl &, int number) {
                return qMakePair(number < 3 ? QByteArray("REDIRECT_DNS_FAILURE") : QByteArray("ok"), 5);
            });
            return network;
        });
        const QString official = QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/neoforge/21.1.234/file");
        const QString mirror = DownloadManager::mirrorUrl(official);
        bool succeeded = false;
        QEventLoop loop;
        QObject::connect(&manager, &DownloadManager::downloadFinished, &loop,
            [&](const QString &, bool ok, const QString &) { succeeded = ok; loop.quit(); });
        manager.downloadFile(mirror, directory.filePath(QStringLiteral("download")), {}, -1,
                             VerifyMode::ExistsOnly, official);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        check(succeeded && network->urls == QList<QUrl>{QUrl(mirror), QUrl(mirror), QUrl(official)},
              "镜像落点 DNS 失败后从入口重试一次，再换到同一文件的官方地址");
    }
    {
        QTemporaryDir directory;
        ManifestRaceNetwork *network = nullptr;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [](const QUrl &, int number) {
                return qMakePair(number == 1 ? QByteArray("FORGE_REDIRECT_STALL") : QByteArray("ok"), 5);
            });
            return network;
        });
        manager.setNoProgressGiveUpMs(40);
        watchStalledReplies(network, manager);
        const QUrl entry(QStringLiteral("https://edge.forgecdn.net/files/test"));
        bool succeeded = false;
        QEventLoop loop;
        QObject::connect(&manager, &DownloadManager::downloadFinished, &loop,
            [&](const QString &, bool ok, const QString &) { succeeded = ok; loop.quit(); });
        manager.downloadFile(entry.toString(), directory.filePath(QStringLiteral("download")), {}, 2,
                             VerifyMode::ExistsOnly);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        check(succeeded && network->urls == QList<QUrl>{entry, entry},
              "CurseForge 跳转后未收到正文时从原入口重试");
    }
    for (int scenario : {0, 1, 2, 3}) {
        const bool startWithProgress = scenario == 2;
        const bool mirrorOk = scenario == 0 || scenario == 2;
        const bool succeededExpected = scenario != 3;
        QTemporaryDir directory;
        const QUrl entry(QStringLiteral("https://edge.forgecdn.net/files/123/456/mod%2Bforge%20file.jar"));
        const QUrl mirror(QStringLiteral("https://mod.mcimirror.top/files/123/456/mod+forge%20file.jar"));
        const QUrl direct(QStringLiteral("https://mediafilez.forgecdn.net/files/123/456/mod%2Bforge%20file.jar"));
        ManifestRaceNetwork *network = nullptr;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [&](const QUrl &url, int) {
                if (url == mirror) return qMakePair(mirrorOk ? QByteArray("ok") :
                    (scenario == 1 ? QByteArray("NOT_FOUND") : QByteArray("NO_DATA")), 5);
                if (url == direct) return qMakePair(succeededExpected ? QByteArray("ok") : QByteArray("no"), 5);
                return qMakePair(QByteArray(startWithProgress ? "FORGE_REDIRECT_SLOW" : "FORGE_REDIRECT_STALL"), 5);
            });
            return network;
        });
        manager.setNoProgressGiveUpMs(startWithProgress ? 2000 : 40);
        watchStalledReplies(network, manager);
        const QString path = directory.filePath(QStringLiteral("mod.jar"));
        const QString sha1 = QString::fromLatin1(QCryptographicHash::hash("ok", QCryptographicHash::Sha1).toHex());
        QEventLoop loop;
        int completions = 0;
        bool succeeded = false;
        QObject::connect(&manager, &DownloadManager::allDownloadsFinished, &loop, [&](bool ok) {
            ++completions;
            succeeded = ok;
            loop.quit();
        });
        manager.downloadFile(entry.toString(), path, sha1, 2, VerifyMode::SizeAndSha1);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        QList<QUrl> expectedUrls{entry, entry, mirror};
        if (!mirrorOk) expectedUrls.append(direct);
        check(completions == 1 && network->urls == expectedUrls &&
                  succeeded == succeededExpected && QFileInfo::exists(path) == succeededExpected,
              startWithProgress ? "CurseForge 文件开始传输后失速，可切换 MCIM 并完成作业"
                                : "CurseForge 超时重试一次入口，随后尝试 MCIM 和备用直连并校验 SHA-1");
        if (!succeededExpected) {
            const auto jobs = manager.failedJobs();
            check(jobs.size() == 1 && QUrl(jobs.first().url) == entry &&
                  jobs.first().reason.contains(QStringLiteral("edge.forgecdn.net → mediafilez.forgecdn.net → "
                                                             "edge.forgecdn.net → mediafilez.forgecdn.net → "
                                                             "mod.mcimirror.top → mediafilez.forgecdn.net")),
                  "资源下载失败保留原始地址及包括重定向的尝试顺序");
        }
    }

    for (bool originRecovers : {true, false}) {
        QTemporaryDir directory;
        const QUrl origin(QStringLiteral("https://cdn.modrinth.com/data/test/versions/v/mod.jar"));
        const QUrl sibling = origin.resolved(QUrl(QStringLiteral("other.jar")));
        const QUrl mirror(QStringLiteral("https://mod.mcimirror.top/data/test/versions/v/mod.jar"));
        ManifestRaceNetwork *network = nullptr;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [&](const QUrl &url, int attempt) {
                if (url == sibling || url == mirror || (originRecovers && attempt == 2)) {
                    return qMakePair(QByteArray("ok"), 5);
                }
                return qMakePair(QByteArray(originRecovers ? "NETWORK_FAILURE" : "SLOW"), 5);
            });
            return network;
        });
        watchStalledReplies(network, manager);
        const QString sha1 = QString::fromLatin1(QCryptographicHash::hash("ok", QCryptographicHash::Sha1).toHex());
        QEventLoop loop;
        bool success = false;
        QObject::connect(&manager, &DownloadManager::allDownloadsFinished, &loop, [&](bool ok) {
            success = ok;
            loop.quit();
        });
        manager.downloadFile(origin.toString(), directory.filePath(QStringLiteral("mod.jar")), sha1, 2,
                             VerifyMode::SizeAndSha1);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        const QList<QUrl> expectedUrls = originRecovers ? QList<QUrl>{origin, origin}
                                                        : QList<QUrl>{origin, origin, mirror};
        check(success && network->urls == expectedUrls,
              "Modrinth 瞬时错误先重试原站，持续低速时才切换该文件的下载源");
        if (!originRecovers) {
            check(!manager.mirrorDemotedFor(origin.host()), "MCIM 回退不触发官方 CDN 主机降级");
            success = false;
            manager.downloadFile(sibling.toString(), directory.filePath(QStringLiteral("other.jar")), sha1, 2,
                                 VerifyMode::SizeAndSha1);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            loop.exec();
            check(success && network->urls.last() == sibling, "同一 CDN 的后续文件仍从原站下载");
        }
    }

    {
        QTemporaryDir directory;
        const QUrl origin(QStringLiteral("https://edge.forgecdn.net/files/123/456/large.jar"));
        const QUrl mirror(QStringLiteral("https://mod.mcimirror.top/files/123/456/large.jar"));
        const QUrl direct(QStringLiteral("https://mediafilez.forgecdn.net/files/123/456/large.jar"));
        const QByteArray payload(4 * 1024 * 1024, 'm');
        const QString sha1 = QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
        QList<QUrl> requests;
        DownloadManager manager(nullptr, [&](QObject *owner) {
            return new ManifestRaceNetwork(owner, [&](const QUrl &url, int) {
                requests.append(url);
                if (requests.size() == 1) return qMakePair(QByteArray("ok"), 5); // Range unsupported.
                if (url == origin) return qMakePair(QByteArray("NETWORK_FAILURE"), 5);
                if (url == mirror) return qMakePair(QByteArray("NOT_FOUND"), 5);
                return qMakePair(payload, 5);
            });
        });
        manager.setRangeCacheRoot(directory.filePath(QStringLiteral("ranges")));
        QEventLoop loop;
        bool success = false;
        QObject::connect(&manager, &DownloadManager::allDownloadsFinished, &loop, [&](bool ok) {
            success = ok;
            loop.quit();
        });
        const QString path = directory.filePath(QStringLiteral("large.jar"));
        manager.downloadFile(origin.toString(), path, sha1, payload.size(), VerifyMode::SizeAndSha1);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        check(success && requests == QList<QUrl>{origin, origin, origin, mirror, direct} &&
              QFileInfo(path).size() == payload.size(),
              "分段探测不支持 Range 时沿用文件级换源顺序，整个文件只提交一次");
    }

    for (int status : {200, 404, 503}) {
        ManifestRaceNetwork *network = nullptr;
        ResourceStore store(nullptr, [&](QObject *owner) {
            network = new ManifestRaceNetwork(owner, [status](const QUrl &, int number) {
                if (number > 1 && status == 200) return qMakePair(QByteArray("[]"), 5);
                return qMakePair(status == 404 ? QByteArray("NOT_FOUND") : QByteArray("NETWORK_FAILURE"), 5);
            });
            return network;
        });
        QEventLoop loop;
        int completions = 0;
        QString reason;
        QObject::connect(&store, &ResourceStore::versionsFinished, &loop,
                         [&](const QString &, const auto &, const QString &problem) {
            ++completions;
            reason = problem;
            loop.quit();
        });
        store.fetchVersions(QStringLiteral("modrinth-test"), QStringLiteral("sodium"),
                            ResourceCatalog::Source::Modrinth, QStringLiteral("1.21.1"));
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        const QUrl official = network->urls.value(0);
        const QUrl mirror = network->urls.value(1);
        const bool urlsOk = official.host() == QLatin1String("api.modrinth.com") &&
            (status == 404 ? network->requests == 1 :
                network->requests == 2 && mirror.host() == QLatin1String("mod.mcimirror.top") &&
                mirror.path() == QStringLiteral("/modrinth") + official.path() &&
                mirror.query(QUrl::FullyEncoded) == official.query(QUrl::FullyEncoded));
        check(completions == 1 && urlsOk &&
                  (status == 200 ? reason.isEmpty() : reason.contains(QString::number(status))),
              "Modrinth 版本请求在暂时性故障时切换一次，保留筛选和最终错误", reason);
    }

}

