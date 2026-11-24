/**
 * @file AuthManager.cpp
 * @brief 执行微软 OAuth 登录、换出 Minecraft 令牌并存储登录信息。
 * @note 处理基于浏览器的登录重定向、回调处理、Xbox/XSTS/Minecraft 令牌链、存取与刷新。
 *
 * 这个文件里的网络请求都是**同步**的（QEventLoop + 超时），因为调用方是界面上的一次
 * 点击或者一次启动：那两条路本来就等着一个结果，异步化只会让调用点多出一堆状态机。
 * 内嵌事件循环期间界面仍然会重画（Qt 在处理事件），所以"正在登录…"是看得见的。
 */

#include "AuthManager.hpp"

#include "AccountStore.hpp"    // 刷新成功后把新凭据写回列表
#include "OAuthEndpoints.hpp"  // 动态回调、PKCE 与 state
#include "TokenStore.hpp"      // 凭据落盘的字节格式（唯一出处）
#include "../net/HttpClient.hpp"  // 同步 HTTP（唯一出处）

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <limits>  // tokenSecondsLeft() 那个夹取（int 的上界）
#include "util/SecretRedactor.hpp"

namespace {
// 正版登录只使用构建时明确配置的项目应用 ID；未配置时在本地提示，禁止发起授权。
#ifdef WTFML_CLIENT_ID
static constexpr const char kClientId[] = WTFML_CLIENT_ID;
#else
static constexpr const char kClientId[] = "";
#endif

// NOTE: 两份凭据落在哪些键上、以及那串字节怎么保护，都**不在这里** ——
// 见 TokenStore.hpp（多账号列表里的副本必须和活动槽是同一套格式，所以它只有一个
// 出处）。这一层要知道的只有"键名从 TokenStore 取"。
static constexpr const char kOAuthAuthorizeUrl[] =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize";
static constexpr const char kOAuthTokenUrl[] =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
static constexpr const char kXboxLiveAuthUrl[] = "https://user.auth.xboxlive.com/user/authenticate";
static constexpr const char kXstsAuthUrl[] = "https://xsts.auth.xboxlive.com/xsts/authorize";
static constexpr const char kMinecraftLoginUrl[] =
    "https://api.minecraftservices.com/authentication/login_with_xbox";
static constexpr const char kMinecraftProfileUrl[] =
    "https://api.minecraftservices.com/minecraft/profile";
#ifdef WTFML_REDIRECT_URI
static constexpr const char kRedirectBase[] = WTFML_REDIRECT_URI;
#else
static constexpr const char kRedirectBase[] = "http://localhost/";
#endif
static constexpr const char kOAuthScope[] = "XboxLive.signin offline_access";
// 等浏览器回来的上限。见 AuthManager::loginInFlight() 的说明。
static constexpr int kLoginTimeoutMinutes = 5;
// NOTE: 值和 LauncherManager 的 kDefaultPlayerName 一致。那边用它兜底（调用方没给名字时），
// 这里是没有账户时的"名字"。两处都写死同一个字面量是因为它们各有各的来路，
// 谁都不该 include 对方 —— 改的时候一起改。
static constexpr const char kDefaultPlayerName[] = "Player";

AuthManager::EndpointOverrides g_endpointOverrides;

QString effectiveClientId() {
    return g_endpointOverrides.clientId.trimmed().isEmpty()
               ? QString::fromLatin1(kClientId)
               : g_endpointOverrides.clientId.trimmed();
}

QString effectiveOAuthTokenUrl() {
    return g_endpointOverrides.oauthTokenUrl.trimmed().isEmpty()
               ? QString::fromLatin1(kOAuthTokenUrl)
               : g_endpointOverrides.oauthTokenUrl.trimmed();
}

QString effectiveXboxLiveAuthUrl() {
    return g_endpointOverrides.xboxLiveAuthUrl.trimmed().isEmpty()
               ? QString::fromLatin1(kXboxLiveAuthUrl)
               : g_endpointOverrides.xboxLiveAuthUrl.trimmed();
}

QString effectiveXstsAuthUrl() {
    return g_endpointOverrides.xstsAuthUrl.trimmed().isEmpty()
               ? QString::fromLatin1(kXstsAuthUrl)
               : g_endpointOverrides.xstsAuthUrl.trimmed();
}

QString effectiveMinecraftLoginUrl() {
    return g_endpointOverrides.minecraftLoginUrl.trimmed().isEmpty()
               ? QString::fromLatin1(kMinecraftLoginUrl)
               : g_endpointOverrides.minecraftLoginUrl.trimmed();
}

QString effectiveMinecraftProfileUrl() {
    return g_endpointOverrides.minecraftProfileUrl.trimmed().isEmpty()
               ? QString::fromLatin1(kMinecraftProfileUrl)
               : g_endpointOverrides.minecraftProfileUrl.trimmed();
}

/// NOTE: 这里只剩个**名字**：真正的结构在 HttpClient.hpp（那条同步请求的规矩、含"超时了
/// 要 abort、abort 之后别读正文"那一串）只有一份，登录链和皮肤接口共用。留这行别名
/// 是为了让下面那几十处调用点一个都不用动 —— 搬家的改动越小越不容易搬出 bug。
using HttpResult = HttpClient::Reply;

/// 发一个 JSON 请求并同步等结果（登录链用的都是 GET/POST，所以这里就这一个开关）。
/// @param post true 发 POST（body 就是请求体），false 发 GET。
/// @param contentType 空则不加 Content-Type 头（GET 不需要）。
/// @param bearer 非空则加 Authorization: Bearer 头。
HttpResult sendJson(QNetworkAccessManager *manager, const QUrl &url, bool post,
                    const QByteArray &body, const QByteArray &contentType,
                    const QByteArray &bearer) {
    // 超时用 HttpClient 那一份（kDefaultTimeoutMs，也是 15 秒），**不在这里再写一个常量**：
    // 写一份自己的值和它一样、名字不一样的常量，"15 秒"在仓库里就有两个出处 ——
    // 哪天有人只改了一处，登录链和皮肤接口的超时就会悄悄不一样，而这是
    // 只有网络慢的时候才看得出来、看出来了也未必查得到的那种差别。
    SecretRedactor::remember(QString::fromUtf8(bearer));
    auto result = HttpClient::send(manager, url, post ? HttpClient::Verb::Post : HttpClient::Verb::Get,
                            body, contentType, bearer, HttpClient::kDefaultTimeoutMs);
    // Register newly issued Microsoft/Xbox/Minecraft tokens before any later error/log path.
    SecretRedactor::rememberCredentialFields(result.json);
    return result;
}

/// 失败时那句"…失败：<为什么>"。有网络层的错就说网络层的，否则说 HTTP 状态码。
QString httpFailureText(const HttpResult &result, const QString &what) {
    if (!result.networkError.isEmpty()) {
        return QObject::tr("%1：%2").arg(what, SecretRedactor::redact(result.networkError));
    }

    // 429：限流。**必须**在下面那条通用分支之前拦下 —— 通用分支认的正是正文里那个
    // `error` 字段（Minecraft 服务 429 时它就是 "TOO_MANY_REQUESTS"），端给用户等于
    // 说了句他照做不了的话。文案只有一个出处，皮肤披风那条路说的是同一句。
    if (result.status == HttpClient::kStatusTooManyRequests) {
        return HttpClient::throttledText(what);
    }

    // 有些端点把原因写在正文里（Minecraft 服务是 errorMessage，Xbox 是 Message）。
    // 只认这几个字段、不把正文原样丢出去：正文里还有 traceId / path / 令牌之类的东西。
    // NOTE: 这一句是"HTTP 403"和"HTTP 403：Invalid app registration"的区别 —— 后者才是
    // 用户能拿去查、能照着做的那句话（真机上折在 login_with_xbox 时就是它）。
    for (const char *key : {"errorMessage", "Message", "error"}) {
        const QString detail = result.json.value(QString::fromLatin1(key)).toString();
        if (!detail.isEmpty()) {
            return QObject::tr("%1（HTTP %2）：%3").arg(what).arg(result.status).arg(SecretRedactor::redact(detail));
        }
    }
    return QObject::tr("%1（HTTP %2）。").arg(what).arg(result.status);
}

/// OAuth 令牌端点的失败正文长这样：{"error": "...", "error_description": "..."}。
/// 优先报 description —— invalid_grant 这种错误码对用户没有意义，描述里那句
/// "the user or administrator has not consented" 才有。**不**把正文原样丢出去：
/// 里面还有 trace id 之类的东西。
/// 正文怎么翻在 AuthManager::oauthErrorText() 里（纯函数，验证工程钉得住）；这里只管
/// 从响应正文里把两个字段取出来，以及"这压根不是 OAuth 的失败正文"那条分支。
QString oauthFailureText(const HttpResult &result, const QString &what) {
    const QString code = result.json.value("error").toString();
    if (code.isEmpty()) {
        return httpFailureText(result, what);
    }
    return AuthManager::oauthErrorText(code,
                                      SecretRedactor::redact(result.json.value("error_description").toString()), what);
}

/// XBL/XSTS 的 DisplayClaims.xui[0].uhs —— 账户在 Xbox 那边的标识（user hash）。
/// XSTS 的响应里也会带一遍，所以 XSTS 那步拿它覆盖 XBL 那步的值。
QString firstUserHash(const QJsonObject &json) {
    const QJsonArray xui = json.value("DisplayClaims").toObject().value("xui").toArray();
    return xui.isEmpty() ? QString() : xui.first().toObject().value("uhs").toString();
}
}  // namespace

AuthManager::AuthManager(QObject *parent)
    : QObject(parent),
      // NOTE: 默认玩家名**不是界面文案，别翻译** —— 它就是启动游戏时 --username 的
      // 值（离线与正版两条分支都取它），也是"还没登录"时的占位名字。
      // 登录成功之后会被档案里的真名替掉，见 applyMinecraftTokenData()。
      m_playerName(QString::fromLatin1(kDefaultPlayerName)),
      m_callbackServer(new QTcpServer(this)),
      m_networkManager(new QNetworkAccessManager(this)) {
    connect(m_callbackServer, &QTcpServer::newConnection, this, &AuthManager::handleNewConnection);
    // 开窗时把上次登录的账户读回来：底栏一上来就显示上次那个名字，而不是占位的 "Player"。
    // 这一句**不碰网络**（loadMinecraftTokenData() 只读一遍 QSettings、再解一层
    // DPAPI，见 TokenStore），
    // 所以放构造函数里是安全的 —— 要换令牌得走 loadSavedToken()，那会发请求。
    if (applyMinecraftTokenData(loadMinecraftTokenData())) {
        m_needsRelogin = AccountStore::needsRelogin(m_uuid);
    }
}

// 两句写给玩家：账号页「添加账号」菜单里「微软账号」置灰时悬停的就是它，刷新正版账号失败时也弹它。
QString AuthManager::loginConfigurationError() {
    if (QString::fromLatin1(kClientId).trimmed().isEmpty()) {
        return tr("此版本未配置微软登录。已保存的账号仍会保留，可使用离线账号进入游戏。");
    }
    if (OAuthEndpoints::loopbackRedirectBase(QString::fromLatin1(kRedirectBase)).isEmpty()) {
        return tr("此版本的微软登录配置有误。可使用离线账号进入游戏。");
    }
    return {};
}

bool AuthManager::loginMicrosoft() {
    if (m_loginInFlight) {
        // 已经在等了。这里**不能**重开：换掉 code verifier 会让已经打开的那个浏览器
        // 标签页的回调作废（微软那头回 invalid_grant），用户看着登录页莫名其妙失败。
        return false;
    }

    const QString configError = loginConfigurationError();
    if (!configError.isEmpty()) {
        emit loginFailed(configError);
        return false;
    }

    m_oauthRedirectUri = OAuthEndpoints::listenForCallback(
        *m_callbackServer, OAuthEndpoints::loopbackRedirectBase(QString::fromLatin1(kRedirectBase)));
    if (m_oauthRedirectUri.isEmpty()) {
        emit loginFailed(tr("无法启动本地登录回调：%1").arg(m_callbackServer->errorString()));
        return false;
    }

    m_codeVerifier = OAuthEndpoints::randomToken();
    SecretRedactor::remember(m_codeVerifier);
    m_oauthState = OAuthEndpoints::randomToken();
    SecretRedactor::remember(m_oauthState);
    const QUrl authUrl = buildAuthUrl();
    m_loginInFlight = true;
    // 新的一趟开始了，上一趟留下的"覆盖过微软槽"这一笔不许跟过来（见 handleNewConnection()）。
    m_microsoftTokenOverwritten = false;
    if (m_loginTimeout == nullptr) {
        m_loginTimeout = new QTimer(this);
        m_loginTimeout->setSingleShot(true);
        connect(m_loginTimeout, &QTimer::timeout, this, [this] {
            // 浏览器那头没回来才算这个超时。**已经回来、正在换令牌**的时候不算：
            // 回调处理函数这会儿跑在内嵌事件循环里（换令牌要发好几条请求，见
            // handleNewConnection），它收尾时自己会关服务器、复位 m_loginInFlight、
            // 停掉这个定时器。
            // NOTE: 少了这一句，正好卡在这段时间里到点的定时器会**抢先**发一个 loginFailed
            // —— 用户看见"登录没有完成（等了 5 分钟）"，几十毫秒之后回调那头发
            // loginSucceeded，一次登录发出两个相反的信号：界面先按失败把按钮放回可点，
            // 再按成功刷一遍底栏，而弹窗是模态的，用户点掉它之后才轮到成功那一步。
            // 回调那头本来就挡住了重入（handleNewConnection 开头的同一个判据），
            // 只有这里漏了。换令牌那条链每一条请求都有自己的 15 秒超时
            // （见 HttpClient::kDefaultTimeoutMs），所以这里不再另设窗口也不会一直挂着。
            if (m_handlingCallback) {
                return;
            }
            resetLoginAttempt();
            // 走 loginFailed 而不是自己憋着：界面就是靠它把几个按钮放回可点状态的。
            // NOTE: 这句**不许点名某个按钮**：用户手上看到的文案会变，
            // 这里只说下一步做什么。
            emit loginFailed(tr("等待浏览器返回授权结果超过 %1 分钟，登录已超时。\n请重新登录，或检查网络和代理设置。")
                                 .arg(kLoginTimeoutMinutes));
        });
    }
    m_loginTimeout->start(kLoginTimeoutMinutes * 60 * 1000);

    emit loginStarted();
    if (!QDesktopServices::openUrl(authUrl)) {
        resetLoginAttempt();
        emit loginFailed(tr("无法打开默认浏览器。"));
        return false;
    }
    return true;
}

bool AuthManager::loginInFlight() const { return m_loginInFlight; }

void AuthManager::resetLoginAttempt() {
    m_callbackServer->close();
    m_oauthRedirectUri.clear();
    m_oauthState.clear();
    m_codeVerifier.clear();
    m_loginInFlight = false;
    if (m_loginTimeout != nullptr) {
        m_loginTimeout->stop();
    }
}

QString AuthManager::loadSavedToken(QString *errorMessage) {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return QString();
    };

    // 统一按 6 小时窗口：盘上令牌剩余有效期 >= 6 小时才直接用，不足 6 小时或已过期就刷新。
    // 先 reloadStoredAccount() 确保内存与盘上一致（即使后面刷新失败，本地档案与现有令牌仍保留）。
    reloadStoredAccount();
    if (m_tokenExpiry.isValid() && tokenSecondsLeft() >= tokenRefreshWindowSecs) {
        m_lastRefreshFailureKind = RefreshFailureKind::None;
        return m_accessToken;
    }

    QString failure;
    if (!refreshMinecraftToken(&failure)) {
        return fail(failure);
    }
    return m_accessToken;
}

AuthManager::RefreshFailureKind AuthManager::classifyRefreshFailure(
    int status, const QString &networkError, const QString &oauthError) {
    if (!networkError.isEmpty() || status <= 0) {
        return RefreshFailureKind::Network;
    }
    if (status == HttpClient::kStatusTooManyRequests || status >= 500) {
        return RefreshFailureKind::Network;
    }
    const QString code = oauthError.trimmed().toLower();
    if (code == QLatin1String("temporarily_unavailable") ||
        code == QLatin1String("server_error")) {
        return RefreshFailureKind::Network;
    }
    if (code == QLatin1String("invalid_grant") ||
        code == QLatin1String("interaction_required") ||
        code == QLatin1String("login_required") ||
        code == QLatin1String("consent_required") ||
        code == QLatin1String("unauthorized_client") ||
        code == QLatin1String("invalid_client")) {
        return RefreshFailureKind::ReloginRequired;
    }
    if (status == 401) {
        return RefreshFailureKind::ReloginRequired;
    }
    return RefreshFailureKind::Other;
}

void AuthManager::setEndpointOverridesForTest(const EndpointOverrides &overrides) {
    g_endpointOverrides = overrides;
}

int AuthManager::tokenSecondsLeft() const {
    // m_tokenExpiry 只在 applyMinecraftTokenData() 认下盘上那份凭据时才被写上（而且已经
    // 减掉那 30 秒余量）。认不下来 = 没有可用的到期时刻 = -1（"该换一张了"，见头里那段）。
    if (!m_tokenExpiry.isValid()) {
        return -1;
    }
    // **夹到 int 里再返回**：secsTo() 是 qint64，而盘上那个 expires_at 是别人给的字节 ——
    // 一个离谱的远期时刻（几百年的那种）会让这个转换溢出成负数，于是"剩得远远够用"
    // 会被读成"已经不剩了"，调用方就会去换一张根本不需要换的令牌（溢出之后取到的是
    // 负值）。负数那一头也夹到 0："已经过期"和"剩 0 秒"对调用方
    // 是同一件事，而 -1 这个哨兵只留给"根本没有可用时刻"那一档。
    const qint64 left = QDateTime::currentDateTimeUtc().secsTo(m_tokenExpiry);
    const qint64 cap = std::numeric_limits<int>::max();
    return static_cast<int>(qBound(qint64(0), left, cap));
}

bool AuthManager::refreshMinecraftToken(QString *errorMessage) {
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    };

    m_lastRefreshFailureKind = RefreshFailureKind::None;
    const QString refreshToken = loadMicrosoftTokenData().value("refresh_token").toString();
    SecretRedactor::remember(refreshToken);
    const QString refreshUuid = AccountStore::activeUuid();
    if (refreshToken.isEmpty()) {
        m_lastRefreshFailureKind = RefreshFailureKind::ReloginRequired;
        const QString targetUuid = !refreshUuid.isEmpty() ? refreshUuid : m_uuid;
        if (!targetUuid.isEmpty() && !AccountStore::isOffline(targetUuid)) {
            m_needsRelogin = true;
            persistNeedsRelogin(targetUuid, true);
        }
        return fail(tr("未找到完整的登录信息，请重新登录。"));
    }

    QString microsoftToken;
    QString failure;
    RefreshFailureKind failureKind = RefreshFailureKind::Other;
    if (!requestMicrosoftTokenRefresh(refreshToken, &microsoftToken, &failure, refreshUuid,
                                      &failureKind) ||
        !runMinecraftChain(microsoftToken, &failure, refreshUuid, &failureKind)) {
        m_lastRefreshFailureKind = failureKind;
        if (failureKind == RefreshFailureKind::ReloginRequired) {
            const QString targetUuid = !refreshUuid.isEmpty() ? refreshUuid : m_uuid;
            m_needsRelogin = true;
            if (!targetUuid.isEmpty() && !AccountStore::isOffline(targetUuid)) {
                persistNeedsRelogin(targetUuid, true);
            }
        }
        // 走到这儿的原因多数是网络或者账户本身的问题，写一行给命令行看
        // （用户那边由调用方弹窗，见 loadSavedToken() 的 errorMessage）。
        qWarning().noquote() << "刷新登录失败：" << failure;
        return fail(failure);
    }

    m_lastRefreshFailureKind = RefreshFailureKind::None;
    m_needsRelogin = false;

    // 刷新出来的新令牌**要写回列表里那份副本**。
    // 列表里那份是"切回这个账号"时的唯一来路（见 AccountStore 文件头那种两栏结构）；
    // 不写回的话，活动槽里的 refresh_token 每隔几天就刷新一轮，列表里那份却停在登录
    // 那一天。切走再切回来
    // 把那份旧的写回活动槽，轻的是一次白刷新（微软那边已经轮换作废了），重的就只能重开
    // 浏览器登录。上面两个 store*TokenData() 已经把新令牌放进活动槽了，这里搬的就是新的。
    //
    // NOTE: 收不进列表**不算这次刷新失败**：活动槽里那份是好的、游戏照常能启动，
    // 为一件"备份没更新"的事拦住启动不值得（原因写一行日志）。
    // 收之前再认一次人（见上面 refreshUuid 那段）。切换过的话活动槽已经不是
    // 这一趟刷的那个人了，这时 captureActive() 收进去的会是新旧混着的一对 —— 不收。
    if (refreshUuid.isEmpty() || AccountStore::activeUuid() == refreshUuid) {
        QString captureError;
        if (!AccountStore::captureActive(&captureError)) {
            qWarning().noquote() << "刷新成功，但没能把凭据写回账号列表：" << captureError;
        }
    } else {
        qWarning().noquote() << "刷新途中账号被切走了，这一趟不写回账号列表。";
    }
    return true;
}

void AuthManager::reloadStoredAccount() {
    m_lastRefreshFailureKind = RefreshFailureKind::None;
    // applyMinecraftTokenData() 认不下来（凭据缺字段）时**什么都不改** —— 那是"没读到"
    // 而不是"读到一份空的"，所以上一个人的名字得自己清掉，否则底栏会继续显示一个
    // 已经没有凭据的账号。
    if (!applyMinecraftTokenData(loadMinecraftTokenData())) {
        m_accessToken.clear();
        m_tokenExpiry = QDateTime();
        m_uuid.clear();
        m_playerName = QString::fromLatin1(kDefaultPlayerName);
        m_needsRelogin = false;
    } else {
        m_needsRelogin = AccountStore::needsRelogin(m_uuid);
    }
}

QString AuthManager::playerName() const { return m_playerName; }

void AuthManager::setOfflinePlayer(const QString &name) {
    m_accessToken.clear();
    m_tokenExpiry = QDateTime();
    m_uuid.clear();
    m_playerName = name.isEmpty() ? QString::fromLatin1(kDefaultPlayerName) : name;
    m_lastRefreshFailureKind = RefreshFailureKind::None;
    m_needsRelogin = false;
}

bool AuthManager::loggedIn() const { return !m_uuid.isEmpty(); }

QString AuthManager::playerUuid() const { return dashedUuid(m_uuid); }

QString AuthManager::dashedUuid(const QString &raw) {
    QString hex = raw;
    hex.remove(QLatin1Char('-'));
    if (hex.size() != 32) {
        return QString();
    }

    QString out;
    out.reserve(36);
    for (int i = 0; i < hex.size(); ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            out.append(QLatin1Char('-'));
        }
        out.append(hex.at(i));
    }
    return out;
}

QString AuthManager::oauthErrorText(const QString &code, const QString &description,
                                    const QString &what) {
    // 微软的描述尾巴是 "Trace ID: … Correlation ID: … Timestamp: …"：对用户没有意义，
    // 也和这个文件里别处的规矩一致（httpFailureText 只认几个字段、不把正文原样丢出去）。
    QString detail = description;
    const int traceAt = detail.indexOf(QStringLiteral("Trace ID:"));
    if (traceAt > 0) {
        detail = detail.left(traceAt).trimmed();
    }

    // 见头文件那条 NOTE：这张 refresh_token 属于**另一个应用**，换不出来是必然的，
    // 重试没有意义。说清"只能重新登录"，别让用户去查网络或者账号。
    if (description.contains(QStringLiteral("AADSTS70000"), Qt::CaseInsensitive)) {
        return tr("%1：登录凭据由另一个应用签发（AADSTS70000）。\n请重新登录以获取当前应用的凭据。")
            .arg(what);
    }

    if (!code.isEmpty()) {
        return tr("%1：%2").arg(what, detail.isEmpty() ? code : detail);
    }
    return QString();
}

QString AuthManager::xstsErrorMessage(qint64 xerr) {
    // 这些码来自 XSTS 的 401 响应正文（{"Identity":"0","XErr":2148916233,...}）。
    // 2148916233 是最常见的那个，而且它跟"买没买 Minecraft"完全无关 —— 很多人会以为
    // 自己游戏白买了，所以那句话要说清楚"去 xbox.com 登一次"这条路。
    // NOTE: 参数是 qint64：这些码超过 int 的范围（2148916233 > 2^31-1）。
    switch (xerr) {
        case 2148916227LL:
            return tr("此微软账号已被 Xbox Live 封禁，请更换账号。");
        case 2148916233LL:
            return tr("此微软账号尚未创建 Xbox 档案。\n请使用同一账号在 xbox.com 创建档案，再重新登录。");
        case 2148916235LL:
            return tr("Xbox Live 在此微软账号所属的国家或地区不可用。\n请更换账号，或检查微软账号的地区设置。");
        case 2148916236LL:
        case 2148916237LL:
            return tr("此账号需要验证年龄。请在 Xbox 官网完成成人验证后重新登录。");
        case 2148916238LL:
            return tr("此账号需要监护人授权。请由监护人将其加入 Microsoft 家庭组后重新登录。");
        default:
            return tr("XSTS 授权失败（错误码 %1）。").arg(xerr);
    }
}

QString AuthManager::appRegistrationHint(const QString &bodyMessage) {
    // login_with_xbox 的这条 403 很容易被当成"令牌不对、再登一次"，而它其实是**应用本身**
    // 的问题：同一个账户、同一张 XSTS 令牌，换一个被批准的应用 ID 立刻就能过。所以这句
    // 要写清三件事：账号没问题、卡在哪、下一步去哪做什么。
    if (!bodyMessage.contains(QStringLiteral("Invalid app registration"), Qt::CaseInsensitive)) {
        return QString();
    }

    return tr("该 Azure 应用尚未获准访问 Minecraft 登录服务。\n请由应用管理员在 https://aka.ms/mce-reviewappid 提交应用程序（客户端）ID。获批后可使用同一账号重新登录。");
}

void AuthManager::handleNewConnection() {
    QTcpSocket *socket = m_callbackServer->nextPendingConnection();
    if (!socket) {
        return;
    }

    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);

    // 处理真回调那条路上有网络请求（内嵌事件循环），这段时间里浏览器顺手来要的
    // /favicon.ico 会**重入**到这里。它没有授权码 —— 放它进来只会变成一次假的
    // "登录失败"（用户正等着登录，结果先挨一个失败弹窗）。
    if (!m_loginInFlight || m_handlingCallback) {
        socket->disconnectFromHost();
        return;
    }

    auto *deadline = new QTimer(socket);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, socket, [socket] { socket->disconnectFromHost(); });
    deadline->start(10000);
    socket->setProperty("wtfmlCallbackBuffer", QByteArray());
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, deadline] {
        QByteArray data = socket->property("wtfmlCallbackBuffer").toByteArray();
        data += socket->readAll();
        // 一个 GET 回调请求头只有几百字节；收过这么多还没有结束标记的，不是我们等的
        // 那份请求（也不给本机别的程序拿它当上传口）。
        if (data.size() > 64 * 1024) {
            socket->disconnectFromHost();
            return;
        }
        socket->setProperty("wtfmlCallbackBuffer", data);
        if (!data.contains("\r\n\r\n")) {
            return;
        }
        deadline->stop();
        // 只交出一次：后续再来的字节不属于这一份请求（而且处理期间正卡在内嵌循环里）。
        socket->disconnect(this);
        processCallbackRequest(socket, data);
    });
}

void AuthManager::processCallbackRequest(QTcpSocket *socket, const QByteArray &requestData) {
    // 另一个回调已经在走令牌链了（内嵌事件循环期间别的连接照样收得到）—— 同
    // `handleNewConnection()` 开头那道闸，这里再判一次：异步收之后，"另一个连接"
    // 完全可能在第一条已经开跑之后才把请求头收全。
    if (!m_loginInFlight || m_handlingCallback) {
        socket->disconnectFromHost();
        return;
    }

    const auto parameters = OAuthEndpoints::callbackParameters(
        requestData.left(requestData.indexOf("\r\n")), m_oauthRedirectUri, m_oauthState);
    if (!parameters) {
        socket->disconnectFromHost();
        return;
    }
    const QString code = parameters->queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
    SecretRedactor::remember(code);
    const QString error = SecretRedactor::redact(parameters->queryItemValue(QStringLiteral("error")));

    // NOTE: 这里**不要**把 code / error 的内容写进日志。授权码是密钥材料（PKCE 挡的是
    // "被别人截走后换成令牌"，不是"被别人抄走"），而 Qt 的 qDebug 默认打到 stderr，
    // 谁把启动器的输出重定向了谁就拿到一份。要确认回调有没有成功，看下面的分支和
    // loginSucceeded/loginFailed 信号就够了。

    m_handlingCallback = true;
    // 已接收有效回调，关掉监听。当前连接继续用于返回登录结果。
    m_callbackServer->close();

    const QPointer<QTcpSocket> socketGuard(socket);

    QString responseHtml;
    bool success = false;
    QString failureMessage;

    if (!code.isEmpty()) {
        QString microsoftToken;
        // 两段合起来才算登录成功：先拿授权码换微软令牌，再用它换 Minecraft 令牌和档案。
        // 前半段成功、后半段失败时盘上会留着微软那份令牌（换到它就落盘了，见
        // exchangeAuthorizationCode()），**但 Minecraft 令牌和玩家名一个都不写** ——
        // 那份微软令牌单独用不了，它是"下次续期用的凭据"，不是"已登录"。
        // NOTE: 这里的原注释写的是"一份令牌都不落盘"，与代码不符：微软令牌在
        // exchangeAuthorizationCode() 里就存了。 真机上正是靠盘上这份
        // 微软令牌把失败的链重放了一遍，才查出 login_with_xbox 的 403。
        if (exchangeAuthorizationCode(code, &microsoftToken, &failureMessage) &&
            runMinecraftChain(microsoftToken, &failureMessage)) {
            success = true;
            responseHtml =
                tr("<html><body><h1>登录成功</h1><p>可关闭此窗口。</p></body></html>");
        } else {
            responseHtml = tr("<html><body><h1>登录失败</h1><p>%1</p></body></html>")
                               .arg(failureMessage.toHtmlEscaped());
        }
    } else if (!error.isEmpty()) {
        failureMessage = error;
        responseHtml = tr("<html><body><h1>登录失败</h1><p>%1</p></body></html>")
                           .arg(error.toHtmlEscaped());
    } else {
        failureMessage = tr("未收到授权码。");
        responseHtml = tr("<html><body><h1>登录回调无效</h1><p>%1</p></body></html>")
                           .arg(failureMessage.toHtmlEscaped());
    }

    const QByteArray responseBody = responseHtml.toUtf8();
    QByteArray response;
    response.reserve(responseBody.size() + 256);
    response.append("HTTP/1.1 200 OK\r\n");
    response.append("Content-Type: text/html; charset=UTF-8\r\n");
    response.append("Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n");
    response.append("Content-Length: ");
    response.append(QByteArray::number(responseBody.size()));
    response.append("\r\nConnection: close\r\n\r\n");
    response.append(responseBody);

    if (!socketGuard.isNull()) {
        socket->write(response);
        socket->flush();
        socket->waitForBytesWritten(3000);

        // 当套接字仍然连接时断开连接
        if (socket->state() == QAbstractSocket::ConnectedState) {
            socket->disconnectFromHost();
            if (socket->state() != QAbstractSocket::UnconnectedState) {
                socket->waitForDisconnected(3000);
            }
        }
    }

    m_handlingCallback = false;
    resetLoginAttempt();

    // 信号**放在最后**发：接收方要弹模态窗，而模态窗会卡住它后面那句 —— 先发信号的话，
    // 浏览器那个标签页得等用户把启动器的弹窗点掉才能显示结果。
    if (success) {
        qInfo().noquote() << "登录成功：" << m_playerName;
        emit loginSucceeded(m_playerName);
    } else {
        // 这一趟开头的 exchangeAuthorizationCode() 已经把盘上那份 Minecraft 凭据清掉了
        // （那是 有意的一步，为的是不让"旧账号的 uuid 配新账号的刷新令牌"
        // 这种错配留在盘上）。可 **m_uuid / m_playerName / m_accessToken 是另一份**，
        // 谁都没去动它 —— 于是盘上写着"没登录"、内存里还登着上一个账号：
        // · 底栏左下照旧显示上一个账号，而点启动时那句判据（offlineIdentity()，
        // 读的是盘）走的是"在线"那一支；
        // · 接着 loadSavedToken() 会拿 microsoft 槽里那份**新账号**的刷新令牌换出
        // 新账号来 —— 一次"失败"的登录，点一下启动就悄悄成了另一个人的登录。
        // reloadStoredAccount() 正是"内存跟着盘走"的唯一出处（切账号那条路用的是它），
        // 盘上那份刚被清空 ⇒ 它会退回未登录。
        //
        // NOTE: 当前账号是**离线账号**时不能走那一句：盘上那份凭据本来就没被动过
        // （离线账号没有凭据），而 reloadStoredAccount() 会把 m_playerName 抹成
        // 占位值 "Player"，底栏那句"离线登录"照旧说着 —— 名字和身份当场对不上，
        // 启动参数里 --username 也会变成 Player。那种情况什么都不用做。
        //
        // 另一处同样要堵：**盘上那份微软令牌
        // 也得清掉**，否则整件事根本没防住。
        //
        // 上面那两个"点一下启动就悄悄成了另一个人的登录"里，作祟的不是内存那份身份，
        // 而是**盘上的微软槽** —— 而 loadSavedToken() 读的正是盘：
        // · 这一趟 exchangeAuthorizationCode() 已经把微软槽覆盖成**新账号 B** 的
        // 刷新令牌了（:685 清掉 minecraft 槽，紧接着写微软槽）；
        // · runMinecraftChain() 失败 ⇒ minecraft 槽是空的，而微软槽里躺着 B 的凭据
        // （上面那句"盘上留着微软那份、好把链重放一遍"说的就是它）；
        // · 于是 reloadStoredAccount() 把**内存**退回"未登录"之后，用户点一下启动：
        // loadSavedToken() → minecraft 槽是空的 ⇒ 走 refreshMinecraftToken() ⇒
        // **从微软槽读出 B 的刷新令牌**，换出 B 的 Minecraft 令牌起游戏，收尾那句
        // captureActive() 还会把 B 补进账号列表 —— 一次报过"登录失败"的登录，
        // 一步就悄悄成了另一个人的登录，屏幕上全程没有任何提示。
        // ⇒ 既然这次失败**换过人**（m_microsoftTokenOverwritten），微软槽就得跟 minecraft
        // 槽一样清干净：盘上只剩"没登录"这一种状态，和内存、和界面上那句"登录失败"
        // 三边对齐。不清的话，清掉 minecraft 槽这道闸只拦住了"两个槽错配"，
        // "指针说是 A、槽里是 B"这个错配只是往上挪了一层。
        //
        // NOTE: **只在换过人时清**（就是上面那一位存在的理由）。refreshMinecraftToken()
        // 那条路（续期，不是登录）也会覆盖微软槽，但它覆盖的是**同一个账号**的
        // 令牌，清掉等于把一份好好的凭据扔了 —— 那条路也不走这里（它不走 handleNewConnection，
        // 失败只是把 errorMessage 交回调用方）。
        // NOTE: 代价是**"把链重放一遍"这个方便没了**（B 那份刷新令牌被清掉，用户得回到
        // 浏览器再登一次）。这是有意的取舍：那个"方便"的实质就是**本次启动时按一份
        // 谁也认不出来的凭据换身份**，而"不静默改用户的东西"是这个启动器的硬规矩
        // （不静默换 Java、不静默改内存都是同一条）。真要重放，日志里那次失败的
        // 原因照旧写着。
        // NOTE: 上一个账号（A）的凭据**不会因此丢** —— 这一趟开头的 captureActive()
        // 已经先把它收进账号列表了（见上面那道闸），切回来一键的事。
        // 这一句**不看在用的是哪种账号**（离线也一样要清）：离线时候选不上启动那条路
        // 只是因为 offlineIdentity() 拦着不许读凭据，可盘上那份凭据是**真的**躺着的 ——
        // 哪天用户加了个在线账号再用一次「刷新外观」之类读盘的地方，它照样会被翻出来。
        // 换了人又没换到底的凭据，一个字节都不该留。
        if (m_microsoftTokenOverwritten) {
            TokenStore::clear(TokenStore::microsoftKey());
            m_microsoftTokenOverwritten = false;
            qWarning().noquote()
                << QCoreApplication::translate("AuthManager", "账号切换未完成。已清除本次登录获取的微软令牌，请重新登录。");
        }
        const QString activeAfterFailure = AccountStore::activeUuid();
        if (!AccountStore::isOffline(activeAfterFailure)) {
            reloadStoredAccount();
        }
        qWarning().noquote() << "登录失败：" << failureMessage;
        emit loginFailed(failureMessage);
    }
}

QUrl AuthManager::buildAuthUrl() const {
    QUrl url(kOAuthAuthorizeUrl);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), kClientId);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("response_mode"), QStringLiteral("query"));
    OAuthEndpoints::addRedirectUri(query, m_oauthRedirectUri);
    query.addQueryItem(QStringLiteral("scope"), kOAuthScope);
    query.addQueryItem(QStringLiteral("code_challenge"), OAuthEndpoints::codeChallenge(m_codeVerifier));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("state"), m_oauthState);
    url.setQuery(query);
    return url;
}

bool AuthManager::exchangeAuthorizationCode(const QString &code, QString *accessToken,
                                            QString *errorMessage) {
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), kClientId);
    body.addQueryItem(QStringLiteral("code"), QString::fromLatin1(QUrl::toPercentEncoding(code)));
    OAuthEndpoints::addRedirectUri(body, m_oauthRedirectUri);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    body.addQueryItem(QStringLiteral("code_verifier"), m_codeVerifier);

    const HttpResult result =
        sendJson(m_networkManager, QUrl(kOAuthTokenUrl), true,
                 body.query(QUrl::FullyEncoded).toUtf8(), "application/x-www-form-urlencoded",
                 QByteArray());

    if (!result.ok()) {
        *errorMessage = oauthFailureText(result, tr("获取微软登录令牌失败"));
        return false;
    }

    // ── 覆盖微软槽之前的两道闸 ──────────────────────────────
    // 下面这一句写的是"活动槽里那份微软令牌"。而一个账号条目是**两个槽拼出来的**
    // （AccountStore::captureActive()：uuid 取自 minecraft 槽、微软令牌取自 microsoft
    // 槽），新登录要等 runMinecraftChain() 一路走到底、:772 才写 minecraft 那一半。
    // 于是从这一句到那一句之间，盘上那个"账号"是 **A 的 uuid 配 B 的微软令牌**：
    // ① 这中间只要有人调 captureActive()（用户在等登录的时候点一下「添加离线账号」，
    // makeOfflineCurrent() 里正有一句），写出来的条目就是上面那个错配的东西。
    // ② 更要紧的是**失败**那条路：runMinecraftChain() 挂掉（真机上最常见的就是
    // login_with_xbox 那条 403）之后没人会再调 captureActive()，盘上就**永久**停在
    // 这个错配状态 —— activeUuid() 说是 A，而 refreshMinecraftToken() 从 microsoft
    // 槽读出来的是 B 的 refresh_token，拿它去续 A 的登录必然失败；同时 A 原来那份
    // refresh_token 已经被这一句覆盖掉了（凭据**只存在活动槽里**，
    // 盘上再没有第二份，只能重开浏览器登一遍）。
    // 所以：先把现在这位**收进列表**（凭据留一份），再把 minecraft 槽**清掉**（两个槽不
    // 再可能错配）。失败时的结果是"没登录"，但上一个账号在列表里、一键就切得回来 ——
    // 比"错配着继续用 + 凭据丢了"好。
    //
    // NOTE: 清掉 minecraft 槽**不影响**这里刻意保留的那半个凭据：文件头那句"失败时盘上
    // 留着微软那份，好把链重放一遍"说的是 microsoft 槽，它照旧写着新令牌。
    // 这道闸的判据不能只有"活动槽里有人"（`activeUuid()` 非空）：
    // **离线账号也满足它**（它靠 `CurrentAccountUuid` 那一格算"当前"，见
    // AccountStore::activeUuid() 第二档）。于是"用着离线账号去登微软"这条路上，
    // captureActive() 必然失败（离线账号本来就没有凭据可存，makeOfflineCurrent() 把两个
    // 槽都清空了），每次都在命令行甩一句"盘上没有可保存的登录凭据。"。
    // 那句话既不是错误也不新 —— 真正读日志的人会被它骗着去找一个不存在的故障。
    // 判据换成"当前这位是个**在线**账号"：只有它才有凭据要保。
    const QString currentUuid = AccountStore::activeUuid();
    if (!currentUuid.isEmpty() && !AccountStore::isOffline(currentUuid)) {
        QString captureError;
        if (!AccountStore::captureActive(&captureError)) {
            // 收不进去**不拦这次登录**：登录是另一件事，而且"列表读不出来"这种情况
            // captureActive() 自己会拒绝覆盖（见它的 NOTE），盘上不会更坏。
            qWarning().noquote() << "登录之前，没能把当前账号收进列表：" << captureError;
        }
    }
    TokenStore::clear(TokenStore::minecraftKey());

    if (!storeMicrosoftTokenData(result.json)) {
        *errorMessage = tr("无法写入启动器设置，登录信息未保存。");
        return false;
    }
    // 从这一句起，盘上的微软槽已经是**新账号**的了。这一趟后面还有
    // runMinecraftChain() 那半截（XSTS + 换 Minecraft 令牌 + 拉档案），它会失败
    // （超时 / 429 / 那道 403 都算）。失败收尾时必须记得"这里换过人" —— 见 handleNewConnection()
    // 里失败那一支的 NOTE。
    m_microsoftTokenOverwritten = true;

    *accessToken = result.json.value("access_token").toString();
    if (accessToken->isEmpty()) {
        *errorMessage = tr("微软登录服务未返回令牌。");
        return false;
    }
    return true;
}

bool AuthManager::requestMicrosoftTokenRefresh(const QString &refreshToken, QString *accessToken,
                                               QString *errorMessage,
                                               const QString &expectedActiveUuid,
                                               RefreshFailureKind *failureKind) {
    const QString clientId = effectiveClientId();
    if (clientId.isEmpty()) {
        if (errorMessage) *errorMessage = loginConfigurationError();
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }
    SecretRedactor::remember(refreshToken);
    if (refreshToken.isEmpty()) {
        *errorMessage = tr("登录凭据无法续期，请重新登录。");
        if (failureKind) *failureKind = RefreshFailureKind::ReloginRequired;
        return false;
    }

    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), clientId);
    body.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    body.addQueryItem(QStringLiteral("scope"), kOAuthScope);

    const HttpResult result =
        sendJson(m_networkManager, QUrl(effectiveOAuthTokenUrl()), true,
                 body.query(QUrl::FullyEncoded).toUtf8(), "application/x-www-form-urlencoded",
                 QByteArray());
    if (!result.ok()) {
        *errorMessage = oauthFailureText(result, tr("刷新微软令牌失败"));
        if (failureKind) {
            *failureKind = classifyRefreshFailure(
                result.status, result.networkError,
                result.json.value(QStringLiteral("error")).toString());
        }
        return false;
    }

    QJsonObject tokenData = result.json;
    // 刷新时微软**通常会**轮换 refresh_token，但没带新的那种情况也得撑住：不带就把原来
    // 那张留着。丢了它，下次过期就只能让用户重新登一遍（还得再开一次浏览器）。
    if (tokenData.value("refresh_token").toString().isEmpty()) {
        tokenData.insert(QStringLiteral("refresh_token"), refreshToken);
    }
    if (!expectedActiveUuid.isEmpty() && AccountStore::activeUuid() != expectedActiveUuid) {
        *errorMessage = tr("刷新期间切换了账号，已取消刷新，原有登录信息已保留。");
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }
    if (!storeMicrosoftTokenData(tokenData)) {
        *errorMessage = tr("无法写入启动器设置，登录信息未保存。");
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }

    *accessToken = tokenData.value("access_token").toString();
    if (accessToken->isEmpty()) {
        *errorMessage = tr("微软登录服务未返回令牌。");
        if (failureKind) *failureKind = RefreshFailureKind::Network;
        return false;
    }
    return true;
}

bool AuthManager::runMinecraftChain(const QString &microsoftAccessToken, QString *errorMessage,
                                    const QString &expectedActiveUuid,
                                    RefreshFailureKind *failureKind) {
    QString xblToken;
    QString userHash;
    if (!requestXboxLiveToken(microsoftAccessToken, &xblToken, &userHash, errorMessage,
                              failureKind)) {
        return false;
    }

    QString xstsToken;
    // XSTS 的响应里也会带一遍 uhs，以它为准（XBL 那步的 uhs 是同一个账户，但 XSTS 是
    // 真正授权给 Minecraft 的那一步，用它给的更稳妥）
    if (!requestXstsToken(xblToken, &xstsToken, &userHash, errorMessage, failureKind)) {
        return false;
    }

    QString mcToken;
    int expiresIn = 0;
    if (!requestMinecraftToken(userHash, xstsToken, &mcToken, &expiresIn, errorMessage,
                               failureKind)) {
        return false;
    }

    QString name;
    QString uuid;
    if (!fetchPlayerProfile(mcToken, &name, &uuid, errorMessage, failureKind)) {
        return false;
    }

    if (!expectedActiveUuid.isEmpty() && AccountStore::activeUuid() != expectedActiveUuid) {
        *errorMessage = tr("刷新期间切换了账号，已取消刷新，原有登录信息已保留。");
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }
    if (!storeMinecraftTokenData(mcToken, expiresIn, name, uuid)) {
        *errorMessage = tr("无法写入启动器设置，登录信息未保存。");
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }
    return true;
}

bool AuthManager::requestXboxLiveToken(const QString &microsoftAccessToken, QString *xblToken,
                                       QString *userHash, QString *errorMessage,
                                       RefreshFailureKind *failureKind) {
    QJsonObject properties;
    properties.insert(QStringLiteral("AuthMethod"), QStringLiteral("RPS"));
    properties.insert(QStringLiteral("SiteName"), QStringLiteral("user.auth.xboxlive.com"));
    // "d=" 前缀是 XBL 的写法：RpsTicket 可以是微软令牌，也可以是别处拿来的一张票据，
    // d= 指明这是前者（微软令牌）。
    properties.insert(QStringLiteral("RpsTicket"),
                      QStringLiteral("d=") + microsoftAccessToken);

    QJsonObject body;
    body.insert(QStringLiteral("Properties"), properties);
    body.insert(QStringLiteral("RelyingParty"), QStringLiteral("http://auth.xboxlive.com"));
    body.insert(QStringLiteral("TokenType"), QStringLiteral("JWT"));

    const HttpResult result =
        sendJson(m_networkManager, QUrl(effectiveXboxLiveAuthUrl()), true,
                 QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json",
                 QByteArray());

    if (!result.ok()) {
        *errorMessage = httpFailureText(result, tr("Xbox Live 认证失败"));
        if (failureKind) {
            *failureKind = classifyRefreshFailure(result.status, result.networkError);
        }
        return false;
    }

    *xblToken = result.json.value(QStringLiteral("Token")).toString();
    *userHash = firstUserHash(result.json);
    if (xblToken->isEmpty() || userHash->isEmpty()) {
        *errorMessage = tr("Xbox Live 未返回认证令牌。");
        if (failureKind) *failureKind = RefreshFailureKind::Network;
        return false;
    }
    return true;
}

bool AuthManager::requestXstsToken(const QString &xblToken, QString *xstsToken, QString *userHash,
                                   QString *errorMessage, RefreshFailureKind *failureKind) {
    QJsonObject properties;
    properties.insert(QStringLiteral("SandboxId"), QStringLiteral("RETAIL"));
    properties.insert(QStringLiteral("UserTokens"), QJsonArray{xblToken});

    QJsonObject body;
    body.insert(QStringLiteral("Properties"), properties);
    body.insert(QStringLiteral("RelyingParty"), QStringLiteral("rp://api.minecraftservices.com/"));
    body.insert(QStringLiteral("TokenType"), QStringLiteral("JWT"));

    const HttpResult result =
        sendJson(m_networkManager, QUrl(effectiveXstsAuthUrl()), true,
                 QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json",
                 QByteArray());

    if (!result.ok()) {
        // 这一步的失败**几乎全是账户本身的问题**（没注册过 Xbox 账户、地区不支持、
        // 未成年……），所以正文里那个 XErr 才是要给用户看的东西
        const qint64 xerr =
            result.json.value(QStringLiteral("XErr")).toVariant().toLongLong();
        *errorMessage = xerr != 0 ? xstsErrorMessage(xerr)
                                  : httpFailureText(result, tr("XSTS 授权失败"));
        if (failureKind) {
            *failureKind = xerr != 0 ? RefreshFailureKind::Other
                                     : classifyRefreshFailure(result.status, result.networkError);
        }
        return false;
    }

    *xstsToken = result.json.value(QStringLiteral("Token")).toString();
    const QString hash = firstUserHash(result.json);
    if (!hash.isEmpty()) {
        *userHash = hash;
    }
    if (xstsToken->isEmpty() || userHash->isEmpty()) {
        *errorMessage = tr("Xbox 未返回 XSTS 授权令牌。");
        if (failureKind) *failureKind = RefreshFailureKind::Network;
        return false;
    }
    return true;
}

bool AuthManager::requestMinecraftToken(const QString &userHash, const QString &xstsToken,
                                        QString *mcToken, int *expiresIn, QString *errorMessage,
                                        RefreshFailureKind *failureKind) {
    QJsonObject body;
    // 固定写法：XBL3.0 x=<user hash>;<XSTS 令牌>
    body.insert(QStringLiteral("identityToken"),
                QStringLiteral("XBL3.0 x=%1;%2").arg(userHash, xstsToken));

    const HttpResult result =
        sendJson(m_networkManager, QUrl(effectiveMinecraftLoginUrl()), true,
                 QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json",
                 QByteArray());

    if (!result.ok()) {
        // 这一步的 403 有一种很具体的原因（应用没被 Mojang 批准），单独认出来说明白，
        // 别只回一句"HTTP 403"。
        const QString hint =
            appRegistrationHint(result.json.value(QStringLiteral("errorMessage")).toString());
        *errorMessage =
            hint.isEmpty() ? httpFailureText(result, tr("登录 Minecraft 服务失败")) : hint;
        if (failureKind) {
            *failureKind = !hint.isEmpty()
                               ? RefreshFailureKind::Other
                               : classifyRefreshFailure(result.status, result.networkError);
        }
        return false;
    }

    *mcToken = result.json.value(QStringLiteral("access_token")).toString();
    *expiresIn = result.json.value(QStringLiteral("expires_in")).toInt(0);
    if (mcToken->isEmpty()) {
        *errorMessage = tr("Minecraft 登录服务未返回令牌。");
        if (failureKind) *failureKind = RefreshFailureKind::Network;
        return false;
    }
    return true;
}

bool AuthManager::fetchPlayerProfile(const QString &mcToken, QString *name, QString *uuid,
                                     QString *errorMessage, RefreshFailureKind *failureKind) {
    const HttpResult result = sendJson(m_networkManager, QUrl(effectiveMinecraftProfileUrl()), false,
                                       QByteArray(), QByteArray(), mcToken.toUtf8());

    // 404 在这一步的意思很具体：令牌是好的，但这个账户没买过 Minecraft（或者买游戏的是
    // 另一个账户）。这条要单独说 —— 否则用户看到的是"登录失败"，会以为是密码或者网络问题。
    if (result.status == 404) {
        *errorMessage = tr("此微软账号没有 Minecraft 游戏档案。\n请使用已购买游戏且已创建档案的账号登录。");
        if (failureKind) *failureKind = RefreshFailureKind::Other;
        return false;
    }
    if (!result.ok()) {
        *errorMessage = httpFailureText(result, tr("读取游戏档案失败"));
        if (failureKind) {
            *failureKind = classifyRefreshFailure(result.status, result.networkError);
        }
        return false;
    }

    *name = result.json.value(QStringLiteral("name")).toString();
    *uuid = result.json.value(QStringLiteral("id")).toString();
    if (name->isEmpty() || uuid->isEmpty()) {
        *errorMessage = tr("Minecraft 游戏档案缺少玩家名称。");
        if (failureKind) *failureKind = RefreshFailureKind::Network;
        return false;
    }
    return true;
}

bool AuthManager::storeMicrosoftTokenData(const QJsonObject &tokenData) {
    // 微软那份要留的是 refresh_token（换新令牌全靠它）和 access_token（换 XBL 令牌用）。
    // 少任何一个，这份数据下次都用不了 —— 不如不写，免得盘上躺着一份坏凭据。
    if (tokenData.value(QStringLiteral("access_token")).toString().isEmpty() ||
        tokenData.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
        qWarning() << "微软令牌响应里缺少 access_token 或 refresh_token，不落盘";
        return false;
    }

    QString failure;
    if (!TokenStore::writeJson(TokenStore::microsoftKey(), tokenData, &failure)) {
        qWarning().noquote() << "微软令牌写盘失败：" << failure;
        return false;
    }
    return true;
}

bool AuthManager::storeMinecraftTokenData(const QString &mcToken, int expiresIn,
                                          const QString &name, const QString &uuid) {
    QJsonObject tokenData;
    tokenData.insert(QStringLiteral("access_token"), mcToken);
    // 盘上存**到期时刻**而不是 expires_in：下次开窗要判"还能不能用"，存秒数还得自己
    // 记住是什么时候存的。
    //
    // NOTE: 选 ISO 字符串的真正理由只有"和别处存时间的方式一致、人用调试工具看到的是个日期"
    // （验证工程里读那份 json 也是按 ISODate 解）—— 盘上那一格是 TokenStore 加密过的密文
    // （DPAPI + base64，整个 JSON 一起加），QSettings 走的是 NativeFormat，落在注册表
    // HKCU\\Software\\WTFML\\WTFML 下，压根不是 ini 文件，所以"在 ini 里能看懂"站不住。
    tokenData.insert(QStringLiteral("expires_at"),
                     QDateTime::currentDateTimeUtc().addSecs(expiresIn).toString(Qt::ISODate));
    tokenData.insert(QStringLiteral("name"), name);
    tokenData.insert(QStringLiteral("uuid"), uuid);

    if (!applyMinecraftTokenData(tokenData)) {
        return false;
    }

    QString failure;
    if (!TokenStore::writeJson(TokenStore::minecraftKey(), tokenData, &failure)) {
        qWarning().noquote() << "Minecraft 令牌写盘失败：" << failure;
        return false;
    }
    m_needsRelogin = false;
    m_lastRefreshFailureKind = RefreshFailureKind::None;
    persistNeedsRelogin(uuid, false);
    return true;
}

void AuthManager::persistNeedsRelogin(const QString &uuid, bool required) {
    const bool before = AccountStore::needsRelogin(uuid);
    AccountStore::setNeedsRelogin(uuid, required);
    if (AccountStore::needsRelogin(uuid) != before) {
        emit needsReloginChanged();
    }
}

QJsonObject AuthManager::loadMicrosoftTokenData() const {
    return TokenStore::readJson(TokenStore::microsoftKey());
}

QJsonObject AuthManager::loadMinecraftTokenData() const {
    return TokenStore::readJson(TokenStore::minecraftKey());
}

bool AuthManager::applyMinecraftTokenData(const QJsonObject &tokenData) {
    const QString accessToken = tokenData.value(QStringLiteral("access_token")).toString();
    const QString name = tokenData.value(QStringLiteral("name")).toString();
    const QString uuid = tokenData.value(QStringLiteral("uuid")).toString();
    if (accessToken.isEmpty() || name.isEmpty() || uuid.isEmpty()) {
        return false;
    }

    const QDateTime expiry =
        QDateTime::fromString(tokenData.value(QStringLiteral("expires_at")).toString(),
                              Qt::ISODate);
    // 提前 30 秒算过期：正好在过期那一刻发出去启动，游戏那边会拿到一张已经失效的令牌
    m_tokenExpiry = expiry.isValid() ? expiry.addSecs(-30) : QDateTime();
    SecretRedactor::remember(accessToken);
    m_accessToken = accessToken;
    m_playerName = name;
    m_uuid = uuid;
    return true;
}

