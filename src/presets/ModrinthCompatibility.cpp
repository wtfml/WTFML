/**
 * @file ModrinthCompatibility.cpp
 * @brief `ModrinthCompatibility.hpp` 的实现。
 */

#include "ModrinthCompatibility.hpp"
#include <QCoreApplication>
#include "download/ResourceCatalog.hpp"
#include "net/ControlGet.hpp"

#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

namespace Presets {
namespace {

/// 正文硬顶。平台版本表比一般元数据大：Modrinth 的 `/version` 每条都带着 changelog 全文，
/// 几百上千个版本的项轻松超过 `ControlGet` 的 2 MiB 出厂值。这里抬到 8 MiB —— 仍然是
/// 一个顶（异常答复不许无界缓冲），只是不会把一份正常答复截成"这一发失败"。
constexpr int kVersionBodyLimit = 8 * 1024 * 1024;

/// 429 之后别再去敲门的那段窗口（毫秒）。退避只对"这一发没成"有用；被限流时
/// 立刻再发只会把窗口拖得更长，所以整段时间内同一个平台的查询直接给「暂无法确认」。
constexpr qint64 kThrottleWindowMs = 30 * 1000;

/// 游戏版本那个 JSON 数组参数（Modrinth 的写法：`game_versions=["1.21.1"]`）。
QString versionFilter(const QString &gameVersion) {
    return QStringLiteral("[\"%1\"]").arg(gameVersion.trimmed());
}

/// 加载器 → CurseForge 的 `modLoaderType`（官方那套数：Forge 1 / Fabric 4 / Quilt 5 /
/// NeoForge 6）。认不出来的回 0 = 不加这一格（宁可放宽，也不猜一个错的类型）。
int curseForgeLoaderType(const QString &loader) {
    const QString value = normalizeCompatibilityLoader(loader);
    if (value == QLatin1String("forge")) {
        return 1;
    }
    if (value == QLatin1String("fabric")) {
        return 4;
    }
    if (value == QLatin1String("quilt")) {
        return 5;
    }
    if (value == QLatin1String("neoforge")) {
        return 6;
    }
    return 0;
}

QString projectPath(const QString &projectId) {
    return QString::fromLatin1(QUrl::toPercentEncoding(projectId));
}

QString throttleText(qint64 untilMs) {
    const qint64 leftMs = untilMs - QDateTime::currentMSecsSinceEpoch();
    const qint64 seconds = qMax<qint64>(1, (leftMs + 999) / 1000);
    return QCoreApplication::translate("ModrinthCompatibility", "平台请求受限（约需等待 %1 秒），暂时无法确认。").arg(seconds);
}

CompatibilityState classifyVersions(const QList<ResourceCatalog::VersionEntry> &versions,
    const CompatibilityContext &context, bool incomplete, QString *detail) {
    bool unknown = incomplete;
    for (const auto &version : versions) {
        if (context.releaseFilter != QStringLiteral("all") && context.releaseFilter != version.versionType) continue;
        if ((!context.gameVersion.isEmpty() && version.gameVersions.isEmpty()) ||
            (!normalizeCompatibilityLoader(context.loader).isEmpty() && version.loaders.isEmpty())) { unknown = true; continue; }
        if (!context.gameVersion.isEmpty() && !version.gameVersions.contains(context.gameVersion)) continue;
        QStringList loaders;
        for (const auto &loader : version.loaders) loaders.append(normalizeCompatibilityLoader(loader));
        if (!normalizeCompatibilityLoader(context.loader).isEmpty() &&
            !loaders.contains(normalizeCompatibilityLoader(context.loader))) continue;
        return CompatibilityState::Matched;
    }
    *detail = unknown ? QCoreApplication::translate("ModrinthCompatibility", "版本信息不完整，暂时无法确认；未判定为缺失。")
        : QCoreApplication::translate("ModrinthCompatibility", "平台无兼容当前游戏、加载器与发布筛选的发行版。");
    return unknown ? CompatibilityState::Unknown : CompatibilityState::Missing;
}

}  // namespace

PlatformCompatibilityProvider::PlatformCompatibilityProvider(const QUrl &modrinthRoot,
                                                             QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_modrinthRoot(modrinthRoot) {
    // 结尾斜杠去掉一次：下面拼地址时只认"根 + /xxx"这一种形状。
    QString root = m_modrinthRoot.toString();
    while (root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    m_modrinthRoot = QUrl(root);
}

PlatformCompatibilityProvider::~PlatformCompatibilityProvider() = default;

void PlatformCompatibilityProvider::setCurseForge(const QUrl &base, const QByteArray &apiKey) {
    QString root = base.toString();
    while (root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    m_curseForgeBase = QUrl(root);
    m_curseForgeKey = apiKey.trimmed();
    // Official API requires the user's key; the configured public mirror does not.
    m_curseForgeAvailable = !root.isEmpty() && (!m_curseForgeKey.isEmpty()
        || base.host().compare(QStringLiteral("api.curseforge.com"), Qt::CaseInsensitive) != 0);
}

int PlatformCompatibilityProvider::retryDelayMs(int attempt) {
    // 400 ms、800 ms…… 只给瞬态失败用；平台明确答复（200 / 404）从不走这里。
    return 400 * (attempt + 1);
}

QString PlatformCompatibilityProvider::cacheScope() const {
    return m_modrinthRoot.toString() + QLatin1Char('|') + m_curseForgeBase.toString()
        + QLatin1Char('|') + QString::fromLatin1(QCryptographicHash::hash(
            m_curseForgeKey, QCryptographicHash::Sha256).toHex());
}

void PlatformCompatibilityProvider::query(const ModReference &reference,
                                          const CompatibilityContext &context, Reply reply) {
    queryCancelable(reference, context, std::move(reply));
}

CompatibilityProvider::Cancel PlatformCompatibilityProvider::queryCancelable(
    const ModReference &reference, const CompatibilityContext &context, Reply reply) {
    if (!reply) {
        return {};
    }
    const auto state = std::make_shared<QueryState>();
    const QString platform = normalizeModPlatform(reference.platform);
    if (platform == QLatin1String("modrinth")) {
        queryModrinth(reference, context, reply, 0, state);
    } else if (platform == QLatin1String("curseforge")) {
        queryCurseForge(reference, context, reply, 0, state);
    } else {
        reply(CompatibilityState::Invalid, QCoreApplication::translate("ModrinthCompatibility", "无法识别资源平台。"));
    }
    return [state] {
        state->cancelled = true;
        // 排队中的作业删掉之后**一个字节都不会发**；已经发出去的那一发在析构里 abort。
        // 取消不回调 —— 结论由检查器按超时那一路收（见 `CompatibilityChecker::beginQuery`）。
        delete state->job.data();
    };
}

void PlatformCompatibilityProvider::queryModrinth(const ModReference &reference,
                                                  const CompatibilityContext &context,
                                                  const Reply &reply, int attempt,
                                                  const std::shared_ptr<QueryState> &state) {
    if (state->cancelled) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_modrinthThrottledUntilMs > now) {
        reply(CompatibilityState::Unknown, throttleText(m_modrinthThrottledUntilMs));
        return;
    }

    QUrl url(m_modrinthRoot.toString() + QStringLiteral("/project/") +
             projectPath(reference.projectId) + QStringLiteral("/version"));
    QUrlQuery query;
    if (!context.gameVersion.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("game_versions"), versionFilter(context.gameVersion));
    }
    // 只有这四个是 Modrinth 认的模组加载器。`optifine` / 别的名字**不加**这一格：
    // 加一个它不认的加载器名等于什么都查不到，那是假"缺失"。
    const QString loader = normalizeCompatibilityLoader(context.loader);
    if (loader == QLatin1String("fabric") || loader == QLatin1String("forge") ||
        loader == QLatin1String("neoforge") || loader == QLatin1String("quilt")) {
        query.addQueryItem(QStringLiteral("loaders"), versionFilter(loader));
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "WTFML (Minecraft launcher)");
    request.setRawHeader("Accept", "application/json");
    // 与加载器版本查询、模板/清单这些短请求共用 Control 名额；排队和传输合起来一个截止。
    // 作业挂在 `this`（提供方）名下：提供方没了，手里的作业跟着没，不会对着已析构的对象回调。
    state->job = RequestBudget::ControlGet::start(
        this, m_network, request, m_transferTimeoutMs,
        [this, reply, context, reference, attempt, state](QNetworkReply *networkReply,
                                                          const QString &jobError) {
            if (state->cancelled) return;
            // 没有回复可读（管理器先没 / 回复先没）：如实报「暂无法确认」。**不在这一层重发**
            // —— 重发要用的那个 QNAM 多半正在析构。
            if (networkReply == nullptr) {
                reply(CompatibilityState::Unknown,
                      jobError.isEmpty() ? QCoreApplication::translate("ModrinthCompatibility", "查询中断，暂时无法确认。") : jobError);
                return;
            }
            // （回复由 ControlGet 收尾统一 deleteLater，这里不碰它的生命周期。）
            // 瞬态失败：还有名额就退避一会儿再来一发；名额用完才报「暂无法确认」。
            const auto retryOrUnknown = [this, reply, context, reference, attempt, state](
                                            const QString &why) {
                if (attempt + 1 < m_maxAttempts) {
                    QTimer::singleShot(retryDelayMs(attempt), this,
                                       [this, reference, context, reply, attempt, state]() {
                                           queryModrinth(reference, context, reply, attempt + 1,
                                                         state);
                                       });
                    return;
                }
                reply(CompatibilityState::Unknown, why);
            };
            if (networkReply->property(RequestBudget::kDeadlineExpiredProperty).toBool()) {
                // 超时不在这一层重试：检查器那一层有总时限，再挂一轮只会拖长。
                reply(CompatibilityState::Unknown, QCoreApplication::translate("ModrinthCompatibility", "查询超时，暂时无法确认。"));
                return;
            }
            const int status =
                networkReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (networkReply->error() != QNetworkReply::NoError && status == 0) {
                // 连 HTTP 响应都没拿到：网络层的事，**不是**缺失。
                retryOrUnknown(QCoreApplication::translate("ModrinthCompatibility", "查询失败：%1").arg(networkReply->errorString()));
                return;
            }
            if (status == 404) {
                reply(CompatibilityState::Missing, QCoreApplication::translate("ModrinthCompatibility", "Modrinth 无此项目。"));
                return;
            }
            if (status == 429) {
                // 限流：记下一个窗口，窗口里的后续查询直接「暂无法确认」，
                // 不去把平台的桶敲得更空。429 本身从不算缺失。
                m_modrinthThrottledUntilMs =
                    QDateTime::currentMSecsSinceEpoch() + kThrottleWindowMs;
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "Modrinth 请求受限，暂时无法确认，请稍后重试。"));
                return;
            }
            if (status == 408 || status >= 500) {
                retryOrUnknown(QCoreApplication::translate("ModrinthCompatibility", "接口返回 HTTP %1，暂时无法确认。").arg(status));
                return;
            }
            if (status < 200 || status >= 300) {
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "接口返回 HTTP %1，暂时无法确认。").arg(status));
                return;
            }
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(networkReply->readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isArray()) {
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "平台返回格式异常，暂时无法确认兼容性。"));
                return;
            }
            const QJsonArray versions = document.array();
            if (versions.isEmpty()) {
                reply(CompatibilityState::Missing,
                      QCoreApplication::translate("ModrinthCompatibility", "Modrinth 无兼容 %1%2 的发行版。")
                          .arg(context.gameVersion.isEmpty() ? QCoreApplication::translate("ModrinthCompatibility", "当前版本")
                                                             : context.gameVersion,
                               context.loader.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" / ") + context.loader));
                return;
            }
            const auto parsed = ResourceCatalog::parseVersions(document.toJson());
            QString detail;
            const auto result = classifyVersions(parsed, context, parsed.size() != versions.size(),
                                                 &detail);
            reply(result, detail);
        },
        kVersionBodyLimit);
}

void PlatformCompatibilityProvider::queryCurseForge(const ModReference &reference,
                                                    const CompatibilityContext &context,
                                                    const Reply &reply, int attempt,
                                                    const std::shared_ptr<QueryState> &state) {
    if (state->cancelled) return;
    if (!m_curseForgeAvailable) {
        reply(CompatibilityState::Unknown,
              QCoreApplication::translate("ModrinthCompatibility", "未配置 CurseForge 接口，暂时无法确认。"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_curseForgeThrottledUntilMs > now) {
        reply(CompatibilityState::Unknown, throttleText(m_curseForgeThrottledUntilMs));
        return;
    }

    QUrl url(m_curseForgeBase.toString() + QStringLiteral("/mods/") +
             projectPath(reference.projectId) + QStringLiteral("/files"));
    QUrlQuery query;
    if (!context.gameVersion.trimmed().isEmpty()) {
        query.addQueryItem(QStringLiteral("gameVersion"), context.gameVersion.trimmed());
    }
    const int loaderType = curseForgeLoaderType(context.loader);
    if (loaderType > 0) {
        query.addQueryItem(QStringLiteral("modLoaderType"), QString::number(loaderType));
    }
    query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("50"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "WTFML (Minecraft launcher)");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("x-api-key", m_curseForgeKey);
    // 和 Modrinth 那条同口径：共用 Control 名额，排队 + 传输一个截止。
    state->job = RequestBudget::ControlGet::start(
        this, m_network, request, m_transferTimeoutMs,
        [this, reply, context, reference, attempt, state](QNetworkReply *networkReply,
                                                          const QString &jobError) {
            if (state->cancelled) return;
            if (networkReply == nullptr) {
                reply(CompatibilityState::Unknown,
                      jobError.isEmpty() ? QCoreApplication::translate("ModrinthCompatibility", "查询中断，暂时无法确认。") : jobError);
                return;
            }
            const auto retryOrUnknown = [this, reply, context, reference, attempt, state](
                                            const QString &why) {
                if (attempt + 1 < m_maxAttempts) {
                    QTimer::singleShot(retryDelayMs(attempt), this,
                                       [this, reference, context, reply, attempt, state]() {
                                           queryCurseForge(reference, context, reply, attempt + 1,
                                                           state);
                                       });
                    return;
                }
                reply(CompatibilityState::Unknown, why);
            };
            if (networkReply->property(RequestBudget::kDeadlineExpiredProperty).toBool()) {
                reply(CompatibilityState::Unknown, QCoreApplication::translate("ModrinthCompatibility", "查询超时，暂时无法确认。"));
                return;
            }
            const int status =
                networkReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (networkReply->error() != QNetworkReply::NoError && status == 0) {
                retryOrUnknown(QCoreApplication::translate("ModrinthCompatibility", "查询失败：%1").arg(networkReply->errorString()));
                return;
            }
            if (status == 404) {
                reply(CompatibilityState::Missing, QCoreApplication::translate("ModrinthCompatibility", "CurseForge 无此项目。"));
                return;
            }
            if (status == 429) {
                m_curseForgeThrottledUntilMs =
                    QDateTime::currentMSecsSinceEpoch() + kThrottleWindowMs;
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "CurseForge 请求受限，暂时无法确认，请稍后重试。"));
                return;
            }
            if (status == 408 || status >= 500) {
                retryOrUnknown(QCoreApplication::translate("ModrinthCompatibility", "接口返回 HTTP %1，暂时无法确认。").arg(status));
                return;
            }
            if (status < 200 || status >= 300) {
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "接口返回 HTTP %1，暂时无法确认。").arg(status));
                return;
            }
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(networkReply->readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "平台返回格式异常，暂时无法确认兼容性。"));
                return;
            }
            if (!document.object().value(QStringLiteral("data")).isArray()) {
                reply(CompatibilityState::Unknown,
                      QCoreApplication::translate("ModrinthCompatibility", "接口没有返回文件清单，暂时无法确认。"));
                return;
            }
            const QJsonArray files = document.object().value(QStringLiteral("data")).toArray();
            if (files.isEmpty()) {
                reply(CompatibilityState::Missing,
                      QCoreApplication::translate("ModrinthCompatibility", "CurseForge 无兼容 %1%2 的发行版。")
                          .arg(context.gameVersion.isEmpty() ? QCoreApplication::translate("ModrinthCompatibility", "当前版本")
                                                             : context.gameVersion,
                               context.loader.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" / ") + context.loader));
                return;
            }
            const auto parsed = ResourceCatalog::parseCurseForgeVersions(document.toJson());
            const auto pagination =
                document.object().value(QStringLiteral("pagination")).toObject();
            const bool incomplete = parsed.size() != files.size() ||
                                    pagination.value(QStringLiteral("totalCount")).toInteger() >
                                        files.size();
            QString detail;
            const auto result = classifyVersions(parsed, context, incomplete, &detail);
            reply(result, detail);
        },
        kVersionBodyLimit);
}

}  // namespace Presets
