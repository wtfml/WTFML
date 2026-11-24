#include "Support.hpp"

#include <QEventLoop>
#include <QTimer>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <cstring>

namespace {
class ManifestRaceReply final : public QNetworkReply {
public:
    ManifestRaceReply(const QNetworkRequest &request, QByteArray body, int delay, QObject *parent)
        : QNetworkReply(parent), m_body(std::move(body)) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        QTimer::singleShot(delay, this, [this] {
            if (isFinished()) return;
            if (m_body == QByteArray("NETWORK_FAILURE"))
                setError(QNetworkReply::UnknownNetworkError, QStringLiteral("controlled failure"));
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
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        urls.append(request.url());
        const auto reply = response(request.url(), ++requests);
        return new ManifestRaceReply(request, reply.first, reply.second, this);
    }
private:
    Response response;
};
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
    }


}

