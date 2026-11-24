/**
 * @file PlayerSkinStore.hpp
 * @brief 免鉴权地向官方要玩家的皮肤/披风，并把贴图缓存下来。
 * @note 只用 QtCore + QtNetwork，**一个 QtGui 类都不碰**（它发出去的是"文件路径"，
 * 不是 QImage）—— 主 verify 工程只链 Core/Network，这些解析和缓存判断
 * （哪一段该取什么、缓存命中要不要发请求）**没有一行能靠肉眼看截图验准**，
 * 拆成纯函数就是为了能在那里逐条钉住。
 *
 * 【为什么走 sessionserver 而不是登录链】
 *
 * `https://sessionserver.mojang.com/session/minecraft/profile/<uuid>` 是**免鉴权**的：
 * 知道 uuid 就能问。走它就不必碰 AuthManager 那条同步登录链 —— 那条链在 token 过期时会
 * **同步**重走一遍刷新（见 loadSavedToken()），在启动路径上调它等于卡界面。而 uuid 本身
 * 是从 QSettings 恢复的（AuthManager::playerUuid()），登录成功过的用户重启后不用
 * 重新登录就取得到皮肤。
 *
 * 【为什么"取不到"只是记日志，不弹窗】
 *
 * 这是全仓唯一的例外，理由要写清楚：产品约束是"错误要弹窗"，但那些错误（启动失败、
 * 下载失败、版本对不上）都会让用户**做不成他想做的事**，所以必须拦下来说一句。外观
 * 不一样 —— 它是**装饰**，取不到时界面自动退回默认外观（用户还是能启动游戏），
 * 而"每次开窗都弹一个『皮肤没拿到』"对一个可选功能来说纯属打扰。所以这里是
 * 静默降级 + 一行日志；调用方连对话框都不用接。
 *
 * 【缓存分两种，理由不同】
 *
 * - **贴图**：内容寻址 `<skinsDir>/<url 的 sha1>.png`。贴图 URL 里那一段本来就是内容
 * 哈希（同一个 URL 永远同一张图），所以这个缓存**永不失效**，命中就直接用、
 * 一个请求都不发。
 * - **档案**：`<skinsDir>/profile-<uuid>.json`。它**会**变（用户在游戏里换了皮肤），
 * 缓存它的唯一目的是"开窗那一刻零网络先画出上次那套外观"，随后后台再刷一遍。
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

/// 一份解出来的档案。空字符串 = 这个人没有那一样。
struct PlayerSkinProfile {
    /// 皮肤 PNG 的地址。**空 = 这份档案没用**（没皮肤就没有外观可言，披风单独存在没意义）。
    QString skinUrl;
    /// 披风 PNG 的地址。多数人没有。
    QString capeUrl;
    /// "slim"（纤细，Alex）或 "classic"（经典，Steve）。
    QString variant;

    bool isEmpty() const { return skinUrl.isEmpty(); }

    /// 逐字段比较。用途只有一个：刷新回来的档案和手上这份**一样**时就不必重下贴图、
    /// 也不必重画（皮肤没换的时候，后台那次刷新应该是完全看不见的）。
    bool sameAs(const PlayerSkinProfile &other) const {
        return skinUrl == other.skinUrl && capeUrl == other.capeUrl && variant == other.variant;
    }
};

/// 这个类型会**跟着信号走出去**（profileReady），所以声明成元类型，让它能跨线程/排队
/// 连接传（Qt 的规矩：信号里出现的自定义类型都该有这一行）。顺带也让验证工程能拿它
/// 当 Q_ARG 用 —— 于是"档案到了"那条路能在**不联网**的情况下被驱动。
Q_DECLARE_METATYPE(PlayerSkinProfile)

class PlayerSkinStore : public QObject {
    Q_OBJECT

public:
    /// 一次档案请求该怎么看。**做成返回值而不是就地 if**：204 是这套协议里唯一一处
    /// 反直觉的地方（它不是错误、也不是"这个人没有皮肤"，而是"这个人查不到"），
    /// 而"HTTP 状态码怎么读"这件事没有一行能靠肉眼验准 —— 纯函数才钉得住。
    enum class ProfileFetchOutcome {
        Ok,           ///< 200，body 里有一份解得出皮肤的档案。
        NoSuchPlayer, ///< 204：官方那边查不到这个人（也用来表达限流）。**不是错误**。
        /// 429：官方在限流。**也不是错误**（外观那一栏照样沿用上次那份），但它和 204
        /// 有一处**不一样**：204 说不清是"没这个人"还是"别问了"，而 429 明说了是后者 ——
        /// 所以接下来要**退避**，不能像别的失败那样"下次刷新再问一遍"（见
        /// kThrottleBackoffMs： 真机上那个"又429"就是在这儿来的）。
        Throttled,
        Failed,       ///< 其余（网络失败、5xx、200 但 body 解不出来）。
    };

    explicit PlayerSkinStore(QObject *parent = nullptr);

    // ---------- 纯函数：解析与路径 ----------

    /**
     * @brief 解一份 sessionserver 的响应。
     *
     * 信封长这样：`{"id":…,"name":…,"properties":[{"name":"textures","value":"<base64>"}]}`，
     * 那个 base64 解出来才是真正的档案，里面有 `textures.SKIN.url`、
     * `textures.CAPE.url`、`SKIN.metadata.model`（"slim" 才有这个字段）。
     *
     * @param errorOut 解不出来时那句给日志看的原因。**不弹窗**（见文件头）。
     * @return 解出来的档案；skinUrl 为空 = 没解出来（此时 *errorOut 非空）。
     */
    static PlayerSkinProfile parseProfileJson(const QByteArray &body, QString *errorOut = nullptr);

    /// 一次响应属于哪种结果（判据见 ProfileFetchOutcome）。
    static ProfileFetchOutcome classifyProfileResponse(int httpStatus, const QByteArray &body);

    /// 被限流（429）之后，这段时间里**一个请求都不再发**（毫秒）。
    ///
    /// NOTE: 不按这个退避，被挡那一下之后**每次刷新照样再去问一遍**，于是每问一次被挡
    /// 一次、日志里再添一行 —— 看着像启动器坏了，其实是自己在往限流窗口里撞。
    /// 退避一分钟：外观那一栏沿用上次那份（和 204 同一套降级），一分钟后再问。
    static constexpr int kThrottleBackoffMs = 60000;

    /// 现在该不该因为限流而跳过这次请求？
    /// @param throttledUntil 上次被限流算出来的解禁时刻；**无效 = 从没被限流过**。
    static bool withinThrottleBackoff(const QDateTime &throttledUntil, const QDateTime &now);

    /// 某个贴图 URL 在本地缓存里的路径：`<skinsDir>/<sha1(url)>.png`（见文件头）。
    /// NOTE: 静态而不是成员：它只依赖 AppPaths（当前游戏目录），不依赖这个对象的任何状态。
    static QString cachePathForUrl(const QString &url);

    /// 把档案里那个贴图地址变成**真去取**时用的 QUrl。无效 QUrl = 这个地址不能取。
    ///
    /// 这里干两件事（见 .cpp 里那段 NOTE）：
    ///
    /// 1. **http → https**。官方档案里的地址就是 `http://textures.minecraft.net/...`，
    /// 所以"不是 https 就拒"那条会把**每一份真档案**都拒掉 —— 表现是右栏永远画着
    /// 默认外观、那句话永远说"取不到"。同一个内容寻址的地址 https 拿到的字节一模一样。
    /// 2. **只认官方那个 host**。地址是从外面解出来的，不能让它把请求带到任意 host 上去
    /// （协议只认 http/https 还是不够的：`https://<别人家的域名>/x.png` 一样会被放行）。
    ///
    /// NOTE: 缓存路径和 textureReady() 发出去的 url 用的都是**档案里原样那个**地址，
    /// **不经过这里**。升级只发生在真正发请求那一刻 —— 一旦让两侧不一致，
    /// 调用方按 URL 认领贴图（MainWindow::onSkinTextureReady）就会认不出来。
    static QUrl textureRequestUrl(const QString &url);

    /// 把官方贴图的**内容哈希**拼成贴图地址：`https://textures.minecraft.net/texture/<hash>`。
    ///
    /// NOTE: 这个函数存在的唯一理由是**那个 host 只能有一处知道**（见 textureRequestUrl 里
    /// "只认官方那个 host"那一条）。离线账号的随机皮肤是从第三方库里挑出来的，但那张图
    /// 本身还是躺在官方 CDN 上 —— 挑它的是别人、**装它的是这里**，于是"这个地址算不算
    /// 数"仍然只有这一个判据。
    ///
    /// @param hash 64 位十六进制（官方那个 hash 的形态）。不是这个形态就返回空串 ——
    /// 它来自外面，不能拿它拼出一个野地址。
    static QString textureUrlForHash(const QString &hash);

    /// 把免鉴权档案端点和贴图根地址指到本机假服务器（**只有验证工程会调**）。
    /// 空串 = 官方地址。必须在发出任何请求之前设置；之后只读。
    static void setEndpointOverrides(const QString &profileBase, const QString &textureBase);

    /// 档案缓存的文件名用的键：去掉横线、转小写（32 位十六进制）。
    ///
    /// 两个地方必须用同一个形式，否则会各存一份：**URL** 里官方只认这个没有横线的形式，
    /// 而 AuthManager::playerUuid() 给出来的是**带横线**的那种（它要显示给用户看）。
    /// 顺手转小写是因为官方一律小写，大写进去查不到人。认不出来（不是 32 位十六进制）
    /// 返回空串，调用方据此当"没有 uuid"处理。
    static QString uuidKey(const QString &uuid);

    /// 某个 uuid 的档案缓存路径：`<skinsDir>/profile-<uuidKey>.json`。
    /// uuid 认不出来时返回空串（**不是**拼出一个野路径）。
    static QString cachedProfilePath(const QString &uuid);

    /// 读上次存的那份档案。文件不存在 / 读不懂 / uuid 认不出来都返回 false（不抛）。
    /// @param out 成功时写入。失败时不动它 —— 调用方手上那份别被一个空值覆盖掉。
    static bool loadCachedProfile(const QString &uuid, PlayerSkinProfile *out);

    /// 把一份档案存下来（下次开窗靠它零网络先画出来）。
    /// @param errorOut 可选，写失败的原因（给日志看）。
    static bool saveProfile(const QString &uuid, const PlayerSkinProfile &profile,
                            QString *errorOut = nullptr);

    /// 贴图字节是不是一张能用的皮肤 PNG。
    ///
    /// 只认 64 宽的图（高 64 = 新格式，32 = 老格式，见 SkinModel::completeLegacySkin）。
    ///
    /// NOTE: 这里**只看 PNG 文件头和 IHDR 里的宽高**，不真的解码 —— 那要 QtGui，而这个
    /// 类必须留在 Core 里（见文件头）。够用：要挡的是"下回来的是一张网页/一张错误图"
    /// 这类东西，它们连 PNG 头都没有；至于"字节完好但内容诡异"的图，解码那一步在
    /// 界面侧，QImage 读不出来时照样退回默认外观，兜底是双份的。
    static bool looksLikeSkinPng(const QByteArray &bytes);

    // ---------- 联网 ----------

    // 两发请求（档案、贴图）都走 RequestBudget::ControlGet：和别的短请求共用四个
    // Control 名额，排队 + 传输一起算进 kTransferTimeoutMs。没有名额时只排队、不阻塞
    // 界面；删掉作业（或对象析构）就是取消 —— 排队的那一发一个字节都不会发、也不回调。
    // 拿不到回复（排队到点 / 被掐 / 管理器先没）仍然发 failed()：调用方要靠它把
    // "正在获取"那一格落回去（和以前"超时 = 网络错误"的收场一致）。

    /**
     * @brief 异步要一次档案，回来发 profileReady()。
     *
     * 查不到（204）和被限流（429）都**不发信号**，只记一行日志：前者多半是限流或者一个
     * 已经注销的账号，后者明确就是限流 —— 两种情况下手上那份缓存（或者默认外观）都比
     * "清空"更接近用户想看到的。429 还会顺带**退避**（见 kThrottleBackoffMs），
     * 退避窗口里连着调用这个方法也不会再发请求。其余失败发 failed()。
     *
     * @param uuid 带不带横线都行（见 uuidKey()）。认不出来直接不发请求。
     */
    void refreshForUuid(const QString &uuid);

    /**
     * @brief 异步要一张贴图，回来发 textureReady()。
     *
     * **命中缓存就同步发信号**（"同步"这件事本身是可断言的：一个网络请求不可能同步回来）。
     * 下回来的字节过不了 looksLikeSkinPng() 就丢掉、不落盘、发 failed()。
     *
     * @param url 贴图地址。空串直接返回（"这个人没有披风"是常态，不是错误）。
     */
    void fetchTexture(const QString &url);

signals:
    /// 拿到一份档案。@param uuid 这份是**谁的**（和 refreshForUuid() 收到的是同一个，
    /// 已经过 uuidKey() 规整）。
    ///
    /// NOTE: 这里**不做去重**，每次成功都发。理由是"要不要重下贴图"得和手上那份比，
    /// 而手上那份在调用方手里 —— 去重逻辑放在那边一处即可（PlayerSkinProfile::
    /// sameAs() 就是给它用的），两边各留一份比较反而会不一致。
    ///
    /// NOTE: 信号必须**带上身份**。只带一份 profile 的话，调用方只能拿"当下
    /// 那个账号"去落盘 —— 于是"刷新还在飞的时候切了账号"这条路会把 A 的档案
    /// 写进 `profile-<B>.json`（那份文件是**跨重启**的：下次开窗先按它画出 A 的
    /// 脸，直到下一次刷新成功才纠正）。PlayerSkinController::onProfileReady() 开头
    /// 那道身份判断防的就是这件事。
    void profileReady(const QString &uuid, const PlayerSkinProfile &profile);
    /// 一张贴图在本地就绪了。@param path 本地文件路径 —— 界面自己去读成 QImage。
    void textureReady(const QString &url, const QString &path);
    /// 出错了。**调用方只记日志**（见文件头）。
    void failed(const QString &message);

private:
    /// 自己的管理器，不和 DownloadManager 共用：那个的超时是 30 s（下载大文件用的），
    /// 而这一个请求是可选的外观请求，不该拖着用户的东西。见 kTransferTimeoutMs。
    /// NOTE: 管理器 ≠ 名额。两发请求都走 `RequestBudget::ControlGet`，排队/截止在那边管。
    QNetworkAccessManager *m_network;

    /// 被限流之后"什么时候可以再问"。从没被限流过就是无效值（见 withinThrottleBackoff：
    /// 判据是"无效 = 没被限流过"，所以不需要再加一个布尔量）。
    QDateTime m_throttledUntil;
};
