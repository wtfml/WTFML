/**
 * @file ResourceStore.cpp
 * @brief ResourceStore 的实现。发请求、认钥匙、把正文交给 ResourceCatalog 那一层去读。
 */
#include "ResourceStore.hpp"
#include <QCoreApplication>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QUrl>
#include <QRegularExpression>

#include <utility>

#include "manage/ModpackImport.hpp"  // curseForgeApiBase() —— 那条接口根地址的唯一出处
#include "net/ControlGet.hpp"
#include "net/HttpClient.hpp"
#include "util/AppPaths.hpp"
#include "util/ImageDecode.hpp"

namespace {

/// 「这一串文件各自该换成哪一版」那一批**一发最多带多少个 sha1**。
///
/// 100 这个数和 `InstanceIconLookup::kLookupChunk` / `VersionManager::kLookupChunk` 是
/// **同一个数**（那两处也是"一次带一串哈希/指纹"）—— 三处在做同一件事，没有理由各写一个。
/// 一个 295 个模组的整合包 = 3 发，而不是 295 发（295 发会直接撞上限流，
/// 见 .hpp 里 fetchLatestVersions 那段）。
constexpr int kLatestChunk = 100;

/// CurseForge 那条路要额外带的头：`x-api-key`（**只有用户自己填了 key 才有**）。
///
/// 默认那条路是社区镜像、不要 key（见 ModpackImport::curseForgeApiBase）：那时候一个头都
/// 不发。用户填了 key 就改走官方，官方**不带这个头一律 403**（实测 ）——
/// 头名和 VersionManager 那边一字不差（两处都是"问 CurseForge 要东西"，没有理由不一样）。
QList<QPair<QByteArray, QByteArray>> apiKeyHeaders(const QString &apiKey) {
    QList<QPair<QByteArray, QByteArray>> headers;
    const QString trimmed = apiKey.trimmed();
    if (!trimmed.isEmpty()) {
        headers.append({QByteArrayLiteral("x-api-key"), trimmed.toUtf8()});
    }
    return headers;
}

/// 图标那一发的超时：**比** kDefaultTimeoutMs 短得多，理由和 PlayerSkinStore 那边同一个
/// （见它 kTransferTimeoutMs 那段）：图标是**装饰**。
constexpr int kIconTimeoutMs = 8000;

/// 图标那一发用的请求（超时和上面那条不一样，另外还可能要带"变了没有"那两个头）。
/// NOTE: 它的**定义在下面**（这个匿名命名空间外面）—— 它是 ResourceStore 的静态成员，
/// 不是这里的一个自由函数。提到类里去是为了让 build/verify 能钉住那张真值表
/// （"哪条凭据发哪个头"），见 ResourceStore.hpp 里那段说明。

/// 这一发的回复里那两条凭据（下一回核对要用）。
ResourceStore::IconValidators validatorsOf(QNetworkReply *reply) {
    ResourceStore::IconValidators validators;
    validators.etag = QString::fromUtf8(reply->rawHeader("ETag"));
    validators.lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified"));
    return validators;
}

/// 一串图标字节 → 一张图。解不出来回一张 null 的 QPixmap。
///
/// 一律过 ImageDecode（**不是** `QPixmap::loadFromData`）：接口给的那条图是 webp，这份 Qt
/// 自己解不了，得让 ImageDecode 去问系统的 WIC。盘上那份缓存同理 —— 里面两种字节都会躺着。
QPixmap decodeIcon(const QByteArray &bytes) {
    return QPixmap::fromImage(ImageDecode::decode(bytes));
}

/// project_id 能不能直接当文件名用。
bool isSafeFileStem(const QString &id) {
    if (id.isEmpty() || id == QStringLiteral(".") || id == QStringLiteral("..")) {
        return false;
    }
    for (const QChar ch : id) {
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
                        (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
                        (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                        ch == QLatin1Char('.') || ch == QLatin1Char('_') || ch == QLatin1Char('-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

QNetworkRequest ResourceStore::resourceRequest(const QString &url,
                                               const QList<QPair<QByteArray, QByteArray>> &headers,
                                               const QByteArray &contentType) {
    QNetworkRequest request{QUrl(url)};
    // Modrinth 的接口规矩里写着要能认出是谁在问（它按这个封滥用）。别的接口那边都不发这个
    // 头 —— 但这里是**每一条**都发：那个头里没有任何用户信息，而"哪一发发了、哪一发没发"
    // 这种差别一旦存在就会有人照着"上一处"抄错（这个函数存在的理由就是它只有一份）。
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("WTFML"));
    // 空串 = 这一发没有正文，一个头都不加（见声明处那段：那个空头不是"没有类型"，
    // 它是一句不成形的话）。
    if (!contentType.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    for (const QPair<QByteArray, QByteArray> &header : headers) {
        request.setRawHeader(header.first, header.second);
    }
    // 这条 transferTimeout 现在**由作业再写一次**（`ControlGet` 拿"整项剩余时间"盖上去，
    // 见 ControlGet.hpp）：同一个数，写着只是让这一份请求单拎出来看也是自洽的。
    // 真正的截止在作业那一层 —— 它是排队 + 传输的合计。
    request.setTransferTimeout(HttpClient::kDefaultTimeoutMs);
    return request;
}

QNetworkRequest ResourceStore::iconRequest(const QString &url, const IconValidators &validators) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("WTFML"));
    request.setTransferTimeout(kIconTimeoutMs);
    // 同上：作业会按"整项剩余时间"再盖一次（排队 + 传输共用一个截止）。
    // 空的那条不发（见声明处那段：一个空的条件头不是"没有凭据"，是一句不成形的话）。
    if (!validators.etag.isEmpty()) {
        request.setRawHeader("If-None-Match", validators.etag.toUtf8());
    }
    if (!validators.lastModified.isEmpty()) {
        request.setRawHeader("If-Modified-Since", validators.lastModified.toUtf8());
    }
    return request;
}

/// 这个 reply 上的 HTTP 状态码（没连上时是 0）。
int statusOf(QNetworkReply *reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

/// 状态码在 2xx（真的拿到了东西，而不是"拿到了一个错误页"）。
bool okStatus(int status) { return status >= 200 && status < 300; }

/// 没拿到回复（排队到点 / 被掐 / 管理器先没）时那句原因。
///
/// 收场和"连都没连上"（状态码 0）一样，只是原因那句来自 `ControlGet`（它写的是
/// "等待网络请求超时。"这类）。CurseForge 那段"填一份 key"的提示不往这里加：
/// 这话说的是"这一发没轮到/被掐了"，不是"那个接口不给你"。
QString unansweredText(const QString &what, const QString &jobError) {
    return QCoreApplication::translate("ResourceStore", "%1失败：%2").arg(what, jobError);
}

ResourceStore::ResourceStore(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

ResourceStore::~ResourceStore() { abortAll(); }

void ResourceStore::setCurseForgeApiKey(const QString &apiKey) { m_curseForgeApiKey = apiKey.trimmed(); }

void ResourceStore::setCurseForgeApiBaseOverride(const QString &base) {
    m_curseForgeApiBaseOverride = base.trimmed();
}

void ResourceStore::setModrinthApiBaseOverride(const QString &base) {
    m_modrinthApiBaseOverride = base.trimmed();
}

QString ResourceStore::curseForgeApiBase() const {
    // **唯一出处**：key 空不空、走镜像还是官方，全在 ModpackImport 那一处判（和
    // VersionManager::resolveCurseForgeFiles 调的是同一个函数）—— 这里只是把手上那两条递进去。
    return ModpackImport::curseForgeApiBase(m_curseForgeApiKey, m_curseForgeApiBaseOverride);
}

void ResourceStore::fetchLatestVersions(const QString &requestKey, const QStringList &sha1s,
                                        const QString &gameVersion, const QString &loader) {
    // 同一把钥匙上又来一批（用户又点了一次「检查更新」）：旧那几发**先掐掉** ——
    // 和 `fetchVersions()` 开头那两句同一个理由（回来得晚的那一发不该盖掉新的）。
    // 掐 = 删作业：排队中的不会再发出去，已发出的 abort 且不回调。下面那个回调拿"代号"
    // 认人那一句仍然要留 —— 换一批之后这一格可能已经是新的了（见 LatestRun）。
    if (auto previous = m_latestRuns.find(requestKey); previous != m_latestRuns.end()) {
        const QList<QPointer<RequestBudget::ControlGet>> stale = previous->replies;
        m_latestRuns.erase(previous);
        for (const QPointer<RequestBudget::ControlGet> &job : stale) {
            delete job.data();  // 已经自己收场的那几格是 null，跳过（见 LatestRun::replies）
        }
    }

    const auto emptyAnswer = [this, &requestKey]() {
        emit latestVersionsFinished(requestKey,
                                    QHash<QString, ResourceCatalog::VersionEntry>(), QString());
    };

    // 先把要发几发数出来（空表 / 全是空白的 sha1 = **一发都不发**，照样回一发）。
    QList<QByteArray> bodies;
    for (int offset = 0; offset < sha1s.size(); offset += kLatestChunk) {
        const QByteArray body = ResourceCatalog::versionFilesUpdateRequestBody(
            sha1s.mid(offset, kLatestChunk), gameVersion, loader);
        if (!body.isEmpty()) {
            bodies.append(body);
        }
    }
    if (bodies.isEmpty()) {
        emptyAnswer();
        return;
    }

    LatestRun run;
    run.generation = ++m_latestGeneration;
    run.pending = bodies.size();
    const quint64 generation = run.generation;
    const QString url = ResourceCatalog::versionFilesUpdateUrl(m_modrinthApiBaseOverride);
    m_latestRuns.insert(requestKey, run);

    for (const QByteArray &body : bodies) {
        // `Content-Type` **必须给**（`resourceRequest` 那三个参数里最后一个）：
        // QNetworkAccessManager 不会替一个 POST 猜它，而缺了它的正文那边读不成 JSON。
        auto *job = RequestBudget::ControlGet::startPost(
            this, m_net, resourceRequest(url, {}, QByteArrayLiteral("application/json")), body,
            HttpClient::kDefaultTimeoutMs,
            [this, requestKey, generation](QNetworkReply *reply, const QString &jobError) {
                auto it = m_latestRuns.find(requestKey);
                // 这一把钥匙上还是不是我这一批（**代号**）。被换掉的那一批的作业是**删掉的**，
                // 根本不会回调；这一句防的是"同一把钥匙上又开了一批"那一格。
                if (it == m_latestRuns.end() || it->generation != generation) {
                    return;
                }
                const QString what = QCoreApplication::translate("ResourceStore", "读取版本列表");
                QHash<QString, ResourceCatalog::VersionEntry> part;
                bool ok = false;
                if (reply == nullptr) {
                    it->problem = unansweredText(what, jobError);
                } else {
                    const int status = statusOf(reply);
                    const QByteArray answer = reply->readAll();
                    if (status == HttpClient::kStatusTooManyRequests) {
                        it->problem = HttpClient::throttledText(what);
                    } else if (!okStatus(status)) {
                        it->problem = failureText(reply, answer, what);
                    } else {
                        // 200 也得**看一眼形状**：网关的拦截页也是 200。读不出来就是
                        // "这一批没问到" —— 一条都不许当成"已是最新"（见 .hpp 里那条 NOTE）。
                        const QJsonDocument document = QJsonDocument::fromJson(answer);
                        if (!document.isObject()) {
                            it->problem = QCoreApplication::translate("ResourceStore", "版本列表无法解析（接口返回的不是一份版本清单）");
                        } else {
                            part = ResourceCatalog::parseVersionFilesUpdate(answer);
                            ok = true;
                        }
                    }
                }
                if (ok) {
                    for (auto found = part.constBegin(); found != part.constEnd(); ++found) {
                        it->merged.insert(found.key(), found.value());
                    }
                }
                if (--it->pending > 0) {
                    return;  // 还有几发没回来，等它们
                }
                // 收尾：**把这一批整个拿出来**再发信号 —— 发出去之后接的人可能立刻又开一批，
                // 那时候这一格已经不是我的了。挪出去（不是拷一份）：这批里那几格哈希可能
                // 有上百条，发完就扔，没必要再复制一遍。
                LatestRun finished = std::move(*it);
                m_latestRuns.erase(it);
                emit latestVersionsFinished(requestKey, finished.merged, finished.problem);
            });
        m_latestRuns[requestKey].replies.append(job);
    }
}

void ResourceStore::abortAll() {
    // 取消 = 删作业：排队中的一个字节都不会发，发出去的那一发在作业析构里 abort 且不回调。
    // 所以这里既不用发 finished 让回调自己去认"这一发过期了"，也不会在窗口关掉之后
    // 还收到已经没人要的回调。
    const QList<RequestBudget::ControlGet *> replies = m_replies.values();
    m_replies.clear();
    // 批次那几发也要掐（它们是**独立**的一张表，见 m_latestRuns 那段）—— 少掐这一处的
    // 症状是"窗口关了之后那几个回调还会回来"，而那时候 `this` 已经被析构了。
    const QList<LatestRun> latestRuns = m_latestRuns.values();
    m_latestRuns.clear();
    for (const LatestRun &run : latestRuns) {
        // 一格是 null = 那一发已经自己收场了（`ControlGet::finish()` 里 `deleteLater()`），
        // 再删一次就是二次析构（见 LatestRun::replies 那段）。
        for (const QPointer<RequestBudget::ControlGet> &job : run.replies) {
            delete job.data();
        }
    }
    qDeleteAll(replies);
    m_iconPending.clear();
}

QString ResourceStore::failureText(QNetworkReply *reply, const QByteArray &body,
                                   const QString &what, ResourceCatalog::Source source,
                                   bool hasKey) {
    const int status = statusOf(reply);
    const QString description =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("description")).toString();

    /// CurseForge 那一趟要**多给一句**用户自己能走的路（见声明处那段）：默认走的是社区
    /// 镜像，它限流了、停了、或者官方那边改了规矩，用户在设置里填一份自己的 key 就能绕过去。
    /// 措辞和 VersionManager::resolveCurseForgeFiles 那条路对齐（同一个来源、同一个出口）。
    QString hint;
    if (source == ResourceCatalog::Source::CurseForge) {
        hint = !hasKey
                   ? QCoreApplication::translate("ResourceStore", "\n\n公共接口连接受限。可在设置页「下载」中配置个人 CurseForge API Key。")
                   : QCoreApplication::translate("ResourceStore", "\n\nCurseForge API Key 无效或已过期，请重新配置。");
    }

    if (status > 0) {
        if (!description.isEmpty()) {
            return QCoreApplication::translate("ResourceStore", "%1失败（HTTP %2）：%3%4")
                .arg(what)
                .arg(status)
                .arg(description, hint);
        }
        return QCoreApplication::translate("ResourceStore", "%1失败（HTTP %2）%3").arg(what).arg(status).arg(hint);
    }
    // 压根没连上（断网、DNS、超时）。errorString() 是 Qt 给的那句英文，带上比不带强。
    return QCoreApplication::translate("ResourceStore", "%1失败：%2%3").arg(what, reply->errorString(), hint);
}

void ResourceStore::search(const QString &requestKey, ResourceCatalog::Kind kind,
                           ResourceCatalog::Source source, const QString &query,
                           const QString &gameVersion, const QString &loader,
                           const QString &category) {
    // 换掉上一发 = 删作业：还排在 Control 队列里的一个字节都不会发，已经发出去的那一发作废、
    // **不回调**。所以下面那些回调里不再需要"这一发还是不是手上这个"的比较。
    delete m_replies.take(requestKey);

    // 用户又动了一下（换分类 / 又敲了一次回车）= 那一批图标重新开始算。这道闸只管
    // "这一屏别一张张干等"，不该管一整个会话：他刚才多半就是去把代理打开了
    // （见 m_iconGaveUp 那段）。
    m_iconHostFailures = 0;
    m_iconGaveUp = false;

    const bool isCurseForge = source == ResourceCatalog::Source::CurseForge;
    // 接口根地址**两条路都递**：CurseForge 那一侧 key 空不空、走镜像还是
    // 官方，都在 curseForgeApiBase() 里一处判；Modrinth 那一侧就是我们手上这一份 override
    // （空 = 官方那一份，见 setModrinthApiBaseOverride）。
    const QString url = ResourceCatalog::searchUrl(
        kind, query, 0, kSearchPageSize, gameVersion, loader, source, category,
        isCurseForge ? curseForgeApiBase() : m_modrinthApiBaseOverride);
    auto *job = RequestBudget::ControlGet::start(
        this, m_net,
        resourceRequest(url, isCurseForge ? apiKeyHeaders(m_curseForgeApiKey)
                                          : QList<QPair<QByteArray, QByteArray>>()),
        HttpClient::kDefaultTimeoutMs,
        [this, requestKey, source](QNetworkReply *reply, const QString &jobError) {
            // 先把钥匙摘掉再发信号：接的人可能立刻又搜一次，那时候这一格归他不归我。
            m_replies.remove(requestKey);

            ResourceCatalog::SearchResult result;
            if (reply == nullptr) {
                // 排队到点 / 被掐 / 管理器先没：这一发没成，得让界面收场（不能一直转圈）。
                result.problem = unansweredText(QCoreApplication::translate("ResourceStore", "搜索"), jobError);
                emit searchFinished(requestKey, result);
                return;
            }

            const int status = statusOf(reply);
            const QByteArray body = reply->readAll();

            if (status == HttpClient::kStatusTooManyRequests) {
                // 429 说的不是"请求哪儿写错了"，是"你问得太勤了"，得跟用户这么说（唯一出处）。
                result.problem = HttpClient::throttledText(QCoreApplication::translate("ResourceStore", "搜索"));
            } else if (okStatus(status)) {
                // 2xx 才交给解析器。它读不懂时会自己报「接口返回的数据读不懂」——
                // 那是对的：拿到 200 却读不出 hits，问题就在正文上。
                result = source == ResourceCatalog::Source::CurseForge
                             ? ResourceCatalog::parseCurseForgeSearch(body)
                             : ResourceCatalog::parseSearch(body);
            } else {
                result.problem = failureText(reply, body, QCoreApplication::translate("ResourceStore", "搜索"), source,
                                             !m_curseForgeApiKey.isEmpty());
            }
            emit searchFinished(requestKey, result);
        });
    m_replies.insert(requestKey, job);
}

void ResourceStore::fetchModrinthFiles(const QString &requestKey, const QStringList &sha1s) {
    delete m_replies.take(requestKey);
    QSet<QString> requested;
    const QRegularExpression hashPattern(QStringLiteral("^[0-9a-fA-F]{40}$"));
    for (const auto &sha : sha1s) {
        if (!hashPattern.match(sha).hasMatch()) {
            emit modrinthFilesFinished(requestKey, {}, {}, QCoreApplication::translate("ResourceStore", "文件身份查询包含无效 SHA-1。")); return;
        }
        requested.insert(sha.toLower());
    }
    if (requested.size() > 100) {
        emit modrinthFilesFinished(requestKey, {}, {}, QCoreApplication::translate("ResourceStore", "单批文件身份查询超过 100 项。")); return;
    }
    if (requested.isEmpty()) { emit modrinthFilesFinished(requestKey, {}, {}, {}); return; }
    QString base = m_modrinthApiBaseOverride.isEmpty()
        ? QStringLiteral("https://api.modrinth.com/v2") : m_modrinthApiBaseOverride;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const auto body = QJsonDocument(QJsonObject{{QStringLiteral("algorithm"), QStringLiteral("sha1")},
        {QStringLiteral("hashes"), QJsonArray::fromStringList(requested.values())}}).toJson(QJsonDocument::Compact);
    auto *job = RequestBudget::ControlGet::startPost(this, m_net,
        resourceRequest(base + QStringLiteral("/version_files"), {},
                        QByteArrayLiteral("application/json")),
        body, HttpClient::kDefaultTimeoutMs,
        [this, requestKey, requested](QNetworkReply *reply, const QString &jobError) {
        m_replies.remove(requestKey);
        if (reply == nullptr) {
            emit modrinthFilesFinished(requestKey, {}, {}, unansweredText(QCoreApplication::translate("ResourceStore", "识别已安装模组"), jobError));
            return;
        }
        const auto body = reply->readAll();
        if (!okStatus(statusOf(reply))) {
            emit modrinthFilesFinished(requestKey, {}, {}, failureText(reply, body, QCoreApplication::translate("ResourceStore", "识别已安装模组"))); return;
        }
        const auto document = QJsonDocument::fromJson(body);
        if (!document.isObject()) {
            emit modrinthFilesFinished(requestKey, {}, {}, QCoreApplication::translate("ResourceStore", "模组哈希查询响应不是有效对象。")); return;
        }
        QHash<QString, QString> projects;
        QHash<QString, ResourceCatalog::VersionEntry> versions;
        QSet<QString> ambiguous;
        QString problem;
        const auto object = document.object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString sha = it.key().toLower();
            if (!requested.contains(sha) || ambiguous.contains(sha)) continue;
            const auto entry = it.value().toObject();
            const QString project = entry.value(QStringLiteral("project_id")).toString().trimmed();
            const auto parsed = ResourceCatalog::parseVersions(QJsonDocument(QJsonArray{entry}).toJson());
            bool matched = false;
            if (parsed.size() == 1) for (const auto &file : parsed.first().files)
                if (file.sha1.compare(sha, Qt::CaseInsensitive) == 0) matched = true;
            if (project.isEmpty() || parsed.size() != 1 || parsed.first().id.isEmpty() || !matched) {
                problem = QCoreApplication::translate("ResourceStore", "部分模组身份响应缺少项目/发行版，或文件哈希不符；未采信这些条目。");
                continue;
            }
            if (projects.contains(sha) && (projects.value(sha) != project || versions.value(sha).id != parsed.first().id)) {
                projects.remove(sha);
                versions.remove(sha);
                ambiguous.insert(sha);
                problem = QCoreApplication::translate("ResourceStore", "同一文件哈希返回了相互冲突的项目或发行版，未采信。");
                continue;
            }
            projects.insert(sha, project);
            versions.insert(sha, parsed.first());
        }
        emit modrinthFilesFinished(requestKey, projects, versions, problem);
    });
    m_replies.insert(requestKey, job);
}

void ResourceStore::fetchModrinthVersion(const QString &requestKey, const QString &versionId) {
    delete m_replies.take(requestKey);
    QString base = m_modrinthApiBaseOverride.isEmpty()
        ? QStringLiteral("https://api.modrinth.com/v2") : m_modrinthApiBaseOverride;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    auto *job = RequestBudget::ControlGet::start(
        this, m_net,
        resourceRequest(base + QStringLiteral("/version/")
                        + QString::fromLatin1(QUrl::toPercentEncoding(versionId))),
        HttpClient::kDefaultTimeoutMs,
        [this, requestKey, versionId](QNetworkReply *reply, const QString &jobError) {
        m_replies.remove(requestKey);
        if (reply == nullptr) {
            emit modrinthVersionFinished(requestKey, {}, {},
                unansweredText(QCoreApplication::translate("ResourceStore", "读取精确前置版本"), jobError));
            return;
        }
        const QByteArray body = reply->readAll();
        if (!okStatus(statusOf(reply))) {
            emit modrinthVersionFinished(requestKey, {}, {},
                failureText(reply, body, QCoreApplication::translate("ResourceStore", "读取精确前置版本")));
            return;
        }
        const auto document = QJsonDocument::fromJson(body);
        const auto object = document.object();
        const QString project = object.value(QStringLiteral("project_id")).toString().trimmed();
        const auto parsed = ResourceCatalog::parseVersions(QJsonDocument(QJsonArray{object}).toJson());
        if (!document.isObject() || project.isEmpty() || parsed.size() != 1 || parsed.first().id != versionId) {
            emit modrinthVersionFinished(requestKey, {}, {},
                QCoreApplication::translate("ResourceStore", "精确前置版本响应缺少项目身份或发行版 ID 不匹配。"));
            return;
        }
        emit modrinthVersionFinished(requestKey, project, parsed.first(), {});
    });
    m_replies.insert(requestKey, job);
}

void ResourceStore::fetchCurseForgeVersion(const QString &requestKey, const QString &fileId) {
    bool valid = false;
    const qint64 id = fileId.toLongLong(&valid);
    if (!valid || id <= 0 || id > 9007199254740991LL || QString::number(id) != fileId) {
        emit curseForgeVersionFinished(requestKey, {}, {}, QCoreApplication::translate("ResourceStore", "CurseForge 文件 ID 无效。"));
        return;
    }
    delete m_replies.take(requestKey);
    auto request = resourceRequest(curseForgeApiBase() + QStringLiteral("/mods/files"), apiKeyHeaders(m_curseForgeApiKey));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto *job = RequestBudget::ControlGet::startPost(this, m_net, request,
        QJsonDocument(QJsonObject{{"fileIds", QJsonArray{QJsonValue(id)}}}).toJson(QJsonDocument::Compact),
        HttpClient::kDefaultTimeoutMs,
        [this, requestKey, fileId](QNetworkReply *reply, const QString &jobError) {
        m_replies.remove(requestKey);
        if (reply == nullptr) {
            emit curseForgeVersionFinished(requestKey, {}, {},
                unansweredText(QCoreApplication::translate("ResourceStore", "读取精确前置文件"), jobError));
            return;
        }
        const auto body = reply->readAll();
        if (!okStatus(statusOf(reply))) {
            emit curseForgeVersionFinished(requestKey, {}, {}, failureText(reply, body,
                QCoreApplication::translate("ResourceStore", "读取精确前置文件"), ResourceCatalog::Source::CurseForge, !m_curseForgeApiKey.isEmpty()));
            return;
        }
        const auto document = QJsonDocument::fromJson(body);
        const auto data = document.object().value(QStringLiteral("data")).toArray();
        const auto parsed = ResourceCatalog::parseCurseForgeVersions(body);
        const qint64 project = data.size() == 1 ? data.first().toObject().value(QStringLiteral("modId")).toInteger(-1) : -1;
        if (project <= 0 || project > 9007199254740991LL || parsed.size() != 1 || parsed.first().id != fileId) {
            emit curseForgeVersionFinished(requestKey, {}, {}, QCoreApplication::translate("ResourceStore", "精确前置文件响应缺少项目身份或文件 ID 不匹配。"));
            return;
        }
        emit curseForgeVersionFinished(requestKey, QString::number(project), parsed.first(), {});
    });
    m_replies.insert(requestKey, job);
}

void ResourceStore::fetchVersions(const QString &requestKey, const QString &projectId,
                                  ResourceCatalog::Source source, const QString &gameVersion,
                                  const QString &loader) {
    delete m_replies.take(requestKey);

    const bool isCurseForge = source == ResourceCatalog::Source::CurseForge;
    const QString url = ResourceCatalog::versionsUrl(
        projectId, gameVersion, loader, source,
        isCurseForge ? curseForgeApiBase() : m_modrinthApiBaseOverride);
    auto *job = RequestBudget::ControlGet::start(
        this, m_net,
        resourceRequest(url, isCurseForge ? apiKeyHeaders(m_curseForgeApiKey)
                                          : QList<QPair<QByteArray, QByteArray>>()),
        HttpClient::kDefaultTimeoutMs,
        [this, requestKey, source](QNetworkReply *reply, const QString &jobError) {
        m_replies.remove(requestKey);

        const QString what = QCoreApplication::translate("ResourceStore", "读取版本列表");
        if (reply == nullptr) {
            emit versionsFinished(requestKey, QList<ResourceCatalog::VersionEntry>(),
                                  unansweredText(what, jobError));
            return;
        }

        const int status = statusOf(reply);
        const QByteArray body = reply->readAll();

        if (status == HttpClient::kStatusTooManyRequests) {
            emit versionsFinished(requestKey, QList<ResourceCatalog::VersionEntry>(),
                                  HttpClient::throttledText(what));
            return;
        }
        if (!okStatus(status)) {
            emit versionsFinished(requestKey, QList<ResourceCatalog::VersionEntry>(),
                                  failureText(reply, body, what, source,
                                              !m_curseForgeApiKey.isEmpty()));
            return;
        }
        // 200 也分两种情况：真给了一份版本清单（哪怕是空的 = 这个版本下确实没有），
        // 还是给了个别的东西（网关的拦截页之类）。
        // **这两种在界面上必须是两句话**：前者是"这个资源在 1.21.1 上没有"，后者是
        // "没问到"。混成一句的话，用户会以为自己选错了版本，而问题在网络上
        // （同一个坑在加载器树那边踩过：见 MainWindow 里「无可用版本」那句 NOTE）。
        //
        // NOTE: "一份版本清单"长什么样**是按来源分的**（见声明处那段）：Modrinth 是一整个
        // 数组，CurseForge 是 `{"data":[…]}` 那个对象。只看 `isArray()` 的话，
        // CurseForge 那一页每次都会说"版本列表读不懂" —— 而它其实好得很。
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const bool looksLikeList =
            source == ResourceCatalog::Source::CurseForge
                ? (document.isObject() &&
                   document.object().value(QStringLiteral("data")).isArray())
                : document.isArray();
        if (!looksLikeList) {
            emit versionsFinished(requestKey, QList<ResourceCatalog::VersionEntry>(),
                                  QCoreApplication::translate("ResourceStore", "版本列表无法解析（接口返回的不是一份版本清单）"));
            return;
        }
        emit versionsFinished(requestKey, ResourceCatalog::parseVersions(body), QString());
    });
    m_replies.insert(requestKey, job);
}

void ResourceStore::fetchProjects(const QString &requestKey, const QStringList &projectIds,
                                  ResourceCatalog::Source source) {
    delete m_replies.take(requestKey);
    // 空表：没什么可问的，**照样回一发**（"一个都不用问"也是走完了 —— 接的人可能在等
    // 这一发收尾，不回的话他那一格永远停在"正在问…"）。
    if (projectIds.isEmpty()) {
        emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(), QString());
        return;
    }

    const bool isCurseForge = source == ResourceCatalog::Source::CurseForge;
    const QList<QPair<QByteArray, QByteArray>> headers =
        isCurseForge ? apiKeyHeaders(m_curseForgeApiKey)
                     : QList<QPair<QByteArray, QByteArray>>();

    // 两条路一个收尾：两家回来的东西形状不同，但"怎么失败、什么时候摘钥匙"是一样的。
    const RequestBudget::ControlGet::Completion completion =
        [this, requestKey, source](QNetworkReply *reply, const QString &jobError) {
        m_replies.remove(requestKey);
        const QString what = QCoreApplication::translate("ResourceStore", "读取资源信息");

        if (reply == nullptr) {
            emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(),
                                  unansweredText(what, jobError));
            return;
        }

        const int status = statusOf(reply);
        const QByteArray body = reply->readAll();

        if (status == HttpClient::kStatusTooManyRequests) {
            emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(),
                                  HttpClient::throttledText(what));
            return;
        }
        if (!okStatus(status)) {
            emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(),
                                  failureText(reply, body, what, source,
                                              !m_curseForgeApiKey.isEmpty()));
            return;
        }
        // 少几条 / 一条都没有**不算错**（作者删了、号本来就不对），所以这里不判"像不像
        // 一份项目清单"：真给了一份别的东西（网关拦截页）时解析器回空表，接的人退成
        // "用项目号显示" —— 那正是它能做的最好的事（见声明处那段）。
        emit projectsFinished(requestKey,
                              source == ResourceCatalog::Source::CurseForge
                                  ? ResourceCatalog::parseCurseForgeMods(body)
                                  : ResourceCatalog::parseProjects(body),
                              QString());
    };

    RequestBudget::ControlGet *job = nullptr;
    if (isCurseForge) {
        // 那一家是 `POST /mods` + 一个**正文明文**的号表（`{"modIds":[…],"filterPcOnly":true}`，
        // 构造那一份的唯一出处是 ModpackImport，别在这儿再拼一遍）。号是**数字**：递一串
        // base62 上去发出去的是一个语法就不对的请求，而回给用户的是"没取到名字"。
        QList<int> numericIds;
        numericIds.reserve(projectIds.size());
        for (const QString &id : projectIds) {
            bool ok = false;
            const int value = id.toInt(&ok);
            if (ok) {
                numericIds.append(value);
            }
        }
        if (numericIds.isEmpty()) {
            emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(), QString());
            return;
        }
        const QByteArray body = ModpackImport::curseForgeModsRequestBody(numericIds);
        if (body.isEmpty()) {
            emit projectsFinished(requestKey, QList<ResourceCatalog::Hit>(), QString());
            return;
        }
        // 第三个参数（`Content-Type`）**不是可选的**：漏了它这条接口回 400（见
        // resourceRequest 那段），而 400 在界面上长得和"这个项目没了"一样。
        job = RequestBudget::ControlGet::startPost(
            this, m_net,
            resourceRequest(curseForgeApiBase() + QStringLiteral("/mods"), headers,
                            QByteArrayLiteral("application/json")),
            body, HttpClient::kDefaultTimeoutMs, completion);
    } else {
        // 根地址走**同一个 override**（见 search() 那段）：不加这一格的话，详情页那一行
        // 前置的名字在验证工程里会打到真接口上 —— 而它回来得慢、回什么都看不出来。
        job = RequestBudget::ControlGet::start(
            this, m_net,
            resourceRequest(ResourceCatalog::projectsRequestUrl(projectIds, m_modrinthApiBaseOverride),
                            headers),
            HttpClient::kDefaultTimeoutMs, completion);
    }
    m_replies.insert(requestKey, job);
}

QString ResourceStore::iconCachePath(const QString &cacheId) {
    if (!isSafeFileStem(cacheId)) {
        return QString(); // 不是文件名安全的那种 id：宁可不缓存（见声明处）
    }
    // 后缀 `.icon` 不是 `.png` —— 里面可能是 webp（见声明处那条 NOTE）。
    return AppPaths::iconCacheDir() + QLatin1Char('/') + cacheId + QStringLiteral(".icon");
}

bool ResourceStore::loadIconFromCache(const QString &cacheId, QByteArray *bytesOut) {
    const QString path = iconCachePath(cacheId);
    if (path.isEmpty()) {
        return false;
    }
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close(); // 见 build/verify 里 writeFileAt 那段 NOTE：句柄不放手，后面删不掉
    if (bytesOut != nullptr) {
        *bytesOut = bytes;
    }
    return true;
}

QString ResourceStore::iconMetaPath(const QString &cacheId) {
    const QString icon = iconCachePath(cacheId);
    if (icon.isEmpty()) {
        return QString(); // 不是文件名安全的那种 id（和 iconCachePath 同一个理由）
    }
    return icon + QStringLiteral(".meta");
}

ResourceStore::IconValidators ResourceStore::loadIconValidators(const QString &cacheId,
                                                               const QString &url) {
    IconValidators validators;
    const QString path = iconMetaPath(cacheId);
    if (path.isEmpty()) {
        return validators;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return validators; // 没有这个文件 = 没有凭据（正常情况，不是错误）
    }
    // 四行纯文本（ETag / Last-Modified / 这份凭据是哪条地址给的 / 上一次核对是什么时候），
    // 顺序写死。行数不够就只认有的那几行：老版本写的那些（只有前两行、三行）读出来后面
    // 那几条是空的，按"没记地址""不知道核过没有"处理。
    const QList<QByteArray> lines = file.readAll().split('\n');
    file.close();
    if (lines.size() > 0) {
        validators.etag = QString::fromUtf8(lines.at(0)).trimmed();
    }
    if (lines.size() > 1) {
        validators.lastModified = QString::fromUtf8(lines.at(1)).trimmed();
    }
    // 第四行：上一次**核对**是什么时候（epoch 毫秒）。读不出来 / 老版本
    // 没有这一行 ⇒ 留 0 ⇒ "不知道" ⇒ 照旧核一遍（见 IconValidators::checkedAtMs 那段）。
    if (lines.size() > 3) {
        validators.checkedAtMs = QString::fromUtf8(lines.at(3)).trimmed().toLongLong();
    }
    // 凭据得和"问哪条地址"配成一对。同一个 project 有**两条**图标地址
    // （接口那条 96px 的 webp / 自己拼的 icon.png，见 requestIcon），而缓存和凭据都只按
    // project_id 存 —— 于是"上回问的是哪条"在这份文件里根本没记，这一发就会拿着 **A 的
    // ETag 去问 B**：主机不认时答 200（白带一个没用的条件头，还得再下一整张，白等），
    // 万一两条地址的 ETag 撞上了就是 304 —— 于是这一格一直画着"另一条地址的那张图"，
    // 而且**看起来一个错都没有**（和我们刻意要防的那类事是同一类）。
    // 所以：记着地址、且对不上，就当**没有**凭据（下一发整张重来，代价只是一次干净的重下）。
    const QString storedUrl = lines.size() > 2 ? QString::fromUtf8(lines.at(2)).trimmed() : QString();
    if (!storedUrl.isEmpty() && !url.isEmpty() && storedUrl != url) {
        return IconValidators();
    }
    return validators;
}

bool ResourceStore::saveIconValidators(const QString &cacheId,
                                       const IconValidators &validators, const QString &url) {
    const QString path = iconMetaPath(cacheId);
    if (path.isEmpty()) {
        return false;
    }
    // ⚠ 不按"两条都没有就不留一份空文件"处理：这一行的时间戳本身就不是空的。
    const qint64 checkedAtMs = QDateTime::currentMSecsSinceEpoch();
    QDir().mkpath(AppPaths::iconCacheDir());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false; // best-effort：写不进去只是下次多要一次整张，不是错误
    }
    // 第三行是**这条凭据对应哪条地址**：读的那一侧靠它判断"这份凭据
    // 还是不是问这条地址用的"，见 loadIconValidators 里那条 NOTE。
    // 第四行是**上一次核对的时间**：见 IconValidators::checkedAtMs。
    const QByteArray text = validators.etag.toUtf8() + '\n' + validators.lastModified.toUtf8() +
                            '\n' + url.toUtf8() + '\n' +
                            QByteArray::number(checkedAtMs) + '\n';
    const bool ok = file.write(text) == text.size();
    file.close();
    return ok;
}

bool ResourceStore::saveIconToCache(const QString &cacheId, const QByteArray &bytes) {
    const QString path = iconCachePath(cacheId);
    if (path.isEmpty()) {
        return false;
    }
    QDir().mkpath(AppPaths::iconCacheDir());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false; // best-effort：写不进去不是错误（这一次的图照样发出去）
    }
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

void ResourceStore::dropIconFromCache(const QString &cacheId) {
    const QString path = iconCachePath(cacheId);
    if (!path.isEmpty()) {
        QFile::remove(path);
    }
    // 凭据跟着一起删（见声明处那条 NOTE）：留一条"对应着已经不在的那份字节"的 ETag，
    // 下一发就会问出 304 来 —— 于是这一格永远画不出东西，而且**看起来一个错都没有**。
    const QString meta = iconMetaPath(cacheId);
    if (!meta.isEmpty()) {
        QFile::remove(meta);
    }
}

bool ResourceStore::isHostLevelIconFailure(int httpStatus) {
    // 0 = 压根没连上（断网、DNS、超时）。404/429 不算：那两条说明主机是活的。
    if (httpStatus == 0) {
        return true;
    }
    if (httpStatus == 404 || httpStatus == HttpClient::kStatusTooManyRequests) {
        return false;
    }
    return httpStatus >= 500;
}

void ResourceStore::noteIconFailure(int httpStatus) {
    if (!isHostLevelIconFailure(httpStatus)) {
        return;
    }
    ++m_iconHostFailures;
    if (m_iconHostFailures >= kIconGiveUpAfter) {
        m_iconGaveUp = true;
    }
}

void ResourceStore::answerIcon(const QString &iconId) {
    // 见声明处那条 NOTE：手上那张（旧的但没坏）重发一遍，别把已经画上去的那一格擦掉。
    if (const auto it = m_icons.constFind(iconId); it != m_icons.constEnd()) {
        emit iconReady(iconId, it.value());
        return;
    }
    emit iconReady(iconId, QPixmap());
}

void ResourceStore::requestIcon(const ResourceCatalog::Hit &hit) {
    if (hit.projectId.isEmpty()) {
        return; // 没有 id 就认不了这一格（缓存路径也拼不出来）
    }
    // 认这一格用的是**缓存键**（带来源前缀），不是裸 project_id —— 两个来源的 id 空间会撞，
    // 撞上的症状是"这一格永远画着另一个资源的图标"（见声明处那段）。
    const QString iconId = hit.iconCacheId();
    if (const auto it = m_icons.constFind(iconId); it != m_icons.constEnd()) {
        emit iconReady(iconId, it.value());
        return;
    }
    if (m_iconMissing.contains(iconId)) {
        emit iconReady(iconId, QPixmap());
        return;
    }
    if (m_iconPending.contains(iconId)) {
        return; // 已经在问了，等那一发
    }

    // 盘上那份：先把它顶上（这一发回来之前列表上就有图）。**不问它存了多久** ——
    // 用户定的是"缓存默认不过期"，换新版靠下面那一次核对，不是靠把旧图作废。
    // NOTE: 这一段**排在"放弃了"那道闸前面** —— 盘上那些一个请求都不用发，没理由跟着
    // 一起放弃（网络不通的时候它们正是唯一还能画出来的图）。
    QByteArray cached;
    bool onDisk = false;
    if (loadIconFromCache(iconId, &cached)) {
        const QPixmap icon = decodeIcon(cached);
        if (icon.isNull()) {
            // 盘上那份读不出图（写到一半、或者被别的东西动过）。删掉当没有 —— 留着的话
            // 每进一次这一页都要白读一遍，而且永远画不出图来。
            dropIconFromCache(iconId);
        } else {
            onDisk = true;
            m_icons.insert(iconId, icon);
            emit iconReady(iconId, icon);
            // 上面那张已经发出去了，下面这几种情况到此为止：
            // * 这一趟运行已经核过它了 —— 用户要的是"每次打开"核一遍，不是每翻一页核一遍；
            // * 这一批已经不问了（网络不通那阵子）。**核不了就不核**，别把这一发记进
            // m_iconRevalidated：用户把代理打开、又点一下刷新重搜，那时候该给它一次机会
            // （重搜会清掉 m_iconGaveUp，见成员那段）。
            if (m_iconRevalidated.contains(iconId) || m_iconGaveUp) {
                return;
            }
            // 记账**在发之前**：这一趟运行对它的那一次核对就是下面这一发（成了不成、要不要
            // 退回去重试，都不再问第二遍）。**只有"盘上有那份"的才记** —— 盘上没有的话
            // 下面那一发是"第一次拿这张图"，它失败了得留下重试的机会（见上面那条）。
            m_iconRevalidated.insert(iconId);
        }
    }

    if (m_iconGaveUp) {
        // 见成员那段：这一批已经不问了。盘上那张（上面那一段）该发的已经发过。
        if (!m_icons.contains(iconId)) {
            emit iconReady(iconId, QPixmap());
        }
        return;
    }

    // 挑这一发要哪条地址。**两条地址的差别是"主次"**，而"这一台用不用得了那条主的"是
    // 一次探测就定下来的事实：
    // * 接口给的那条（`hit.iconUrl`）**这一台用得了** → 先要它。Modrinth 给的大多是
    // 96px 的 webp（样张实测 4800 B，而同一张的 icon.png 是 35218 B），它是 webp，
    // 得这台机器解得了才有意义；CurseForge 给的是 256px 的 png，直接就能要。
    // * 用不了（压根没给地址，或者是一条本机没有解码器的 webp）→ **直接**要兜底那条
    // （`hit.iconPngUrl()`：Modrinth 自己按 id 拼、CurseForge 是接口给的 logo.url）。
    // 不是"试了失败再退"—— 那会白等一个超时。
    // 挑错了的代价只有"少省那 1.1 秒"，挑不动才是问题。
    //
    // NOTE: 小多少是**按张**变的（ icon-live 真跑 20 张：18 份 webp 一共
    // 189 KB，平均十来 KB；同一批走 PNG 要大好几倍）。别把"7 倍"当成常数写进
    // 别的地方 —— 那是 AANobbMI 那一张的数。
    const QString givenUrl = hit.iconUrl;
    const bool givenUsable = !givenUrl.isEmpty() &&
                             (!ResourceCatalog::isWebpUrl(givenUrl) ||
                              ImageDecode::webpAvailable());
    const QString fallbackUrl = hit.iconPngUrl();
    const QString url = givenUsable ? givenUrl : fallbackUrl;
    if (url.isEmpty()) {
        emit iconReady(iconId, QPixmap());
        return;
    }

    // 这一趟运行对它的那一次核对**已经在上面记过账了**（只有"盘上有那份"的才记）。
    // 这一发就是本趟唯一的一发：成不成、要不要退回去重试，都不再问第二遍。
    // 兜底只在**两条地址不是同一个东西**时留着（CurseForge 只给了一条、或者接口给的那条
    // 本来就是 icon.png 时，退回去还是同一发请求）—— 详见声明处 startIconRequest 那段。
    // 盘上那份字节和它的凭据一起带上：带凭据才问得出 304，带字节才比得出"一模一样"。
    const IconValidators validators = onDisk ? loadIconValidators(iconId, url) : IconValidators();

    // **盘上那份最近核过就整趟不问它**（要求：「模组图标做本地缓存
    // 功能，不要每次都拉取」；那笔账见声明处【攒够 kIconRevalidateTtlMs 才核对一次】）。
    // 判据只有这一处：`.icon.meta` 第四行那个时间戳（`IconValidators::checkedAtMs`），
    // 0 = 不知道（老版本写的 meta / 主机没给过凭据的文件）⇒ **照旧核一遍**。
    // ⚠ 这一句必须排在"挑地址"之后：凭据是**跟着一条地址**的，`loadIconValidators()`
    // 地址对不上时回的就是空凭据（连同这个时间戳一起作废）—— 判断用的正是那一条路。
    if (onDisk && validators.checkedAtMs > 0 &&
        QDateTime::currentMSecsSinceEpoch() - validators.checkedAtMs < kIconRevalidateTtlMs) {
        return;
    }

    startIconRequest(iconId, url, url == fallbackUrl ? QString() : fallbackUrl,
                     onDisk ? cached : QByteArray(), validators);
}

void ResourceStore::startIconRequest(const QString &iconId, const QString &url,
                                     const QString &fallbackUrl, const QByteArray &onDiskBytes,
                                     const IconValidators &validators) {
    if (url.isEmpty()) {
        emit iconReady(iconId, QPixmap());
        return;
    }

    // 图标这一发也进"在飞"那张表，见 abortAll() 那条 NOTE：它原来只掐
    // 接口那几发，图标是**表外**的 —— 于是"把在飞的都掐掉"这句话对图标不成立（今天唯一的
    // 调用者是析构，所以没现出症状，但契约这么说就得这么做）。顺带白拿一件东西：同一个
    // project 已经在飞的那一发会被掐掉，和接口那两条请求同款。
    // 键加个前缀：两边的键都是自己拼的字符串，加前缀是让"这一格是谁"看得出来，也不会撞。
    const QString requestKey = QStringLiteral("icon:") + iconId;
    delete m_replies.take(requestKey);
    m_iconPending.insert(iconId);
    auto *job = RequestBudget::ControlGet::start(
        this, m_net, iconRequest(url, validators), kIconTimeoutMs,
        [this, iconId, url, fallbackUrl, onDiskBytes, requestKey,
         validators](QNetworkReply *reply, const QString &jobError) {
        // 先把钥匙摘掉再判：下面"换一条地址再要一次"会往同一个 key 上开新的一发。
        m_replies.remove(requestKey);
        m_iconPending.remove(iconId);

        if (reply == nullptr) {
            // 排队到点 / 被掐 / 管理器先没：这一发不算"主机答话了"，和超时同一档 ——
            // 记一次失败、把占位发回去（这个信号的契约是"每一发都会回一次"）。
            // 这里不读 jobError：图标是次要的东西，界面上没有一行字容得下它。
            Q_UNUSED(jobError);
            noteIconFailure(0);
            answerIcon(iconId);
            return;
        }

        const int status = statusOf(reply);
        const QByteArray body = reply->readAll();

        if (status == HttpClient::kStatusNotModified) {
            // **没变**。盘上那份就是最新的：图一个字节都不写、不发信号、界面上一格都不动
            // （它上面画的本来就是这一张）。
            // NOTE: 这个码只可能在"带着凭据去问"时出现。凭据和盘上那份对不上（比如那份
            // 被别的东西换过）时主机给的是 200，走下面那条路 —— 所以这条路不担心
            // "凭据说没变、其实变了"。
            // NOTE: 也**不算**主机级失败、更不算成功：它就是"问过了，答案是不用动"。
            // 那道"连着三次不通就放弃"的闸跟它无关（主机明明答话了）。
            saveIconValidators(iconId, validators, url);
            return;
        }

        if (status == HttpClient::kStatusTooManyRequests) {
            // 图标是**次要**的东西：为它弹一个"你问得太勤了"的窗、把用户正在看的列表打断，
            // 代价比"这个方块空着"大得多。所以这里静静退回去画占位，不问用户。
            // （限流那句话的唯一出处仍然是 HttpClient::throttledText，只是这里不显示它。）
            //
            // NOTE: 退回**也要发**一个图标，不能直接 return。这个信号现在的契约是
            // "每一发请求都会回一次"（界面按它把那一格摆正，见 onResourceIconReady）——
            // 唯独这一条不发的话，被限流的那批行会永远比别的行少一格。
            // 429 **不算**主机级失败（主机明明答话了），也不换地址重试（换一条也是
            // 同一个主机，这一分钟它照样不给你）。
            answerIcon(iconId);
            return;
        }

        QPixmap icon;
        if (okStatus(status)) {
            icon = decodeIcon(body);
        }
        if (icon.isNull()) {
            // "没解出一张图"。**先看要不要换一条地址** —— 见声明处那段：主机答话了、
            // 而且手上还有另一条地址，就再要一次（这一次不算失败）。pending 位在重发那一
            // 发里重新立起来，所以这中间"同一格别问第二遍"的规矩不破。
            if (!fallbackUrl.isEmpty() && !isHostLevelIconFailure(status)) {
                startIconRequest(iconId, fallbackUrl, QString(), QByteArray(), IconValidators());
                return;
            }
            if (status == 404) {
                // 作者没传过图标（或者删了）。这是"没有"，别再问第二遍 —— 顺手把盘上那张
                // 旧的也丢掉：真删了的话，留着它只会每次进这一页重演一遍"画出来又被撤掉"。
                m_iconMissing.insert(iconId);
                m_icons.remove(iconId);
                dropIconFromCache(iconId);
                emit iconReady(iconId, QPixmap());
                return;
            }
            // 别的失败（断网、超时、5xx）**不进** m_iconMissing：那是一次没成，下次还该再试。
            noteIconFailure(status);
            answerIcon(iconId);
            return;
        }
        m_iconHostFailures = 0; // 这一路是通的（成功一次就把那道闸放回去）
        // 凭据**先更新**（这一发可能给了新的 ETag：内容没变、标识变了也是常事），
        // 下面那条"没变就直接走人"的路也得留着它，不然下次这一发就白问了。
        saveIconValidators(iconId, validatorsOf(reply), url);
        // 主机**不认**条件头时会把整张再发一遍 —— 这时候比一下字节，一模一样就等于
        // "没变"：不写盘、不发信号（重发一遍会让列表上那一格闪一下，而这趟运行里
        // 它本来就已经是这张图了）。
        if (!onDiskBytes.isEmpty() && body == onDiskBytes) {
            return;
        }
        m_icons.insert(iconId, icon);
        saveIconToCache(iconId, body);
        emit iconReady(iconId, icon);
    });
    m_replies.insert(requestKey, job);
}
