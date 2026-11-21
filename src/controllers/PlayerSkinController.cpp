/**
 * @file PlayerSkinController.cpp
 * @brief 实现玩家外观的选取、取回、认领与降级。
 */
#include "controllers/PlayerSkinController.hpp"

#include <QFile>

// `SkinModel::defaultSteveSkin()` —— 载不进那张贴图时的**兜底外观**（内置的自绘默认皮肤，
// 见 assets/skins/README.md；不打包官方那张 Steve 贴图）。`ui/` 在这一层的上面两级（src/controllers/ →
// src/ → 仓库根），和 MainWindow.cpp 那边写 `..\ui\...` 是同一个意思。
#include "../../ui/SkinModel.hpp"
#include "auth/AccountStore.hpp"
#include "auth/AuthManager.hpp"
#include "manage/DefaultSkin.hpp"
#include "manage/OfflineSkin.hpp"

PlayerSkinController::PlayerSkinController(AuthManager *auth, PlayerSkinStore *store,
                                           QObject *parent)
    : QObject(parent), m_auth(auth), m_store(store) {
    // 数据源是主窗口那边的成员对象（它们的生命周期本来就比外观这一件事长得多），
    // 所以这里只接信号、不接管所有权 —— 认人靠的是它们自己。主窗口在构造里就把它们连上，
    // 见 MainWindow 的接线那一段。
    if (m_store != nullptr) {
        connect(m_store, &PlayerSkinStore::profileReady, this, &PlayerSkinController::onProfileReady);
        connect(m_store, &PlayerSkinStore::textureReady, this, &PlayerSkinController::onTextureReady);
        connect(m_store, &PlayerSkinStore::failed, this, &PlayerSkinController::onStoreFailed);
    }
}

bool PlayerSkinController::offlineIdentity() {
    // 判据只有一个：**当前账号本身**是不是一个离线账号（不是"主页上某个框勾没勾"，
    // 那个开关 已经去掉了，见 AccountStore）。离线账号的 uuid 是
    // md5("OfflinePlayer:" + 名字)，所以"同名 = 同一个身份"。
    //
    // 读的是**盘上的活动槽**（AccountStore 那几个函数都是静态的、直接读写盘），不是
    // AuthManager 里那份内存副本 —— 两者在"切账号"那一刻会短暂不一致（activate() 先写盘，
    // 紧接着才调 setOfflinePlayer() / reloadStoredAccount()），而盘上那份才是权威。
    // 没有当前账号（从没登录、或者刚把当前那个删掉）时 activeUuid() 是空串，isOffline()
    // 对空串回 false —— 那种状态是"未登录"，不是"离线"。
    //
    // NOTE: 这一位是**全仓的唯一出处**（`MainWindow::offlineIdentity()` 只是一句转手）：
    // 底栏那行身份、启动时要不要令牌、外观这条路联不联网，三处各判一次就会出现
    // "底栏说离线、启动时却去要凭据"。它是静态的，所以判它不需要一个 controller 实例。
    return AccountStore::isOffline(AccountStore::activeUuid());
}

PlayerSkinController::HintFacts PlayerSkinController::hintFacts() const {
    HintFacts facts;
    facts.offline = offlineIdentity();
    facts.loggedIn = m_auth != nullptr && m_auth->loggedIn();
    facts.hasSkin = !m_skinPath.isEmpty();
    facts.hasCape = !m_capePath.isEmpty();
    facts.fetching = m_skinFetchInFlight;
    facts.offlineDefaultSkinVersionId = m_offlineDefaultSkinVersion;
    facts.offlineCustomSkin = m_offlineCustomSkin;
    return facts;
}

QStringList PlayerSkinController::offlineDefaultSkinCacheVersions(
    const QString &currentVersionId, const QString &lastLaunchedVersionId) const {
    // 猜"当初按哪个实例算的那张官方默认皮肤"，**按可能性从高到低**：
    // · 上一轮真的算过的那一个（m_offlineDefaultSkinVersion）—— 最准的一个；
    // · 现在选中的那个实例 —— 用户切到谁身上就按谁算；
    // · 上次启动过的版本 —— 换过实例、或者那会儿没选中任何版本时的兜底。
    // 三档都空就是空表（那时候这条路直接落空，退回首字母方块）。
    // NOTE: 一个账号只要**当过**当前账号，它的那张图就躺在盘上，而这三种猜测里总有一个
    // 能命中 —— 这正是"面板里三个人的脸都能画出来"的来源。
    QStringList versions;
    const auto add = [&versions](const QString &versionId) {
        if (!versionId.isEmpty() && !versions.contains(versionId)) {
            versions.append(versionId);
        }
    };
    add(m_offlineDefaultSkinVersion);
    add(currentVersionId);
    add(lastLaunchedVersionId);
    return versions;
}

void PlayerSkinController::reload(const QString &versionId) {
    // 先摆默认外观。**这一步之后，右栏那个小人任何时候都不是空白的** —— 未登录、
    // 还没取到、取不到，看到的都是它。默认外观是内置的自绘默认皮肤（见
    // SkinModel::defaultSteveSkin()），不是把官方那张 Steve 贴图打进仓库。
    applyPlayerSkin(QString(), QString(), false);
    // 上一次那个账号的档案要清掉：换账号登录时它已经没用了，留着会让"贴图回来时
    // 认领"那一步把上一套外观当成这一套。
    m_skinProfile = PlayerSkinProfile();
    // 同理：下面重新算出来之前，"这张脸是按哪个版本算的"这件事不能留着上一轮的值 ——
    // 留着会让右栏那句话拿旧版本的显示名去说新的一张图。
    m_offlineDefaultSkinVersion.clear();
    m_offlineCustomSkin = false;

    // ---------- 离线账号那一档：手动挑的 → 算出来的 → 内置 Steve ----------
    //
    // 这一档必须在下面 !loggedIn() **之前**：离线账号必然同时是"没有登录态"
    // （AuthManager::setOfflinePlayer() 把 uuid 和令牌一起清了，见 AuthManager.cpp），
    // 摆在那句后面就等于永远走不到 —— 离线账号的预览永远是那张写死的
    // 内置 Steve。
    //
    // 判据是 offlineIdentity()（当前账号本身是不是离线账号），不是"有没有勾某个框"。
    // **未登录（压根没有当前账号）不走这一档**：没有身份就没有可记的地方（那张图要按
    // 账号记在盘上），也没人可挑。
    if (offlineIdentity()) {
        // ⓪ **用户自己挑的那张最优先**（「账号与外观」里那颗「换一张…」，见 OfflineSkin）。
        // 这是一句明确的产品次序：手动挑过 = 用户说了算，游戏规矩算出来的那张只是"没有
        // 手动挑过时"的默认值。它不像官方默认皮肤那样跟着实例走 —— 换版本不该把用户
        // 亲自挑的那张换掉。
        const QString accountUuid = AccountStore::activeUuid();
        const QString manualPath = OfflineSkin::skinPathFor(accountUuid);
        if (!manualPath.isEmpty() && QFile::exists(manualPath)) {
            m_offlineCustomSkin = true;
            // 图是从盘上直接读的，不是"在路上"。
            m_skinFetchInFlight = false;
            applyPlayerSkin(manualPath, QString(),
                            OfflineSkin::loadModel(accountUuid) == QStringLiteral("slim"));
            emit hintChanged();
            return;
        }

        // ① 先算"游戏会给这个名字分配哪一张官方默认皮肤"（见 DefaultSkin）：那个"随机"其实
        // 是确定的，图就在**本地那个版本的客户端 JAR** 里，一个字节都不用下。同一个
        // (账号, 版本) 算过一次之后连 JAR 都不碰（缓存命中 = 两次 QFile::exists）。
        //
        // 名字取的是**进 --username 的那一个**（m_auth->playerName()）而不是
        // AccountStore 里那份存档：游戏是拿前者算 uuid 的，差一个字符就是另一张脸。
        // 版本取的是当前选中的那个实例 —— 换版本这张脸真的会变（两套规矩算出来的不是
        // 同一张，见 DefaultSkin.hpp 文件头），所以换版本时外面会重叫一次 reload()。
        QString defaultPath;
        bool defaultSlim = false;
        QString defaultReason;
        const DefaultSkin::Result resolveResult = DefaultSkin::resolve(
            m_auth->playerName(), versionId, &defaultPath, &defaultSlim, &defaultReason);
        if (resolveResult == DefaultSkin::Result::Resolved) {
            m_offlineDefaultSkinVersion = versionId;
            // 这张图**不是一份档案**（它没有地址、离线账号也没有披风），所以 m_skinProfile
            // 保持空着：上一轮若有别人的贴图回来，地址对不上就丢掉了（见 onTextureReady）。
            // NOTE: 这条路上 m_skinFetchInFlight 一直是 false —— 图是从盘上（或 JAR 里）
            // 直接拿的，不是"在路上"，那句话不该说"正在获取"。
            applyPlayerSkin(defaultPath, QString(), defaultSlim);
            return;
        }
        // Unavailable 是**常态**：没装任何版本、或者那个目录里没有客户端 JAR（不是原版
        // 客户端，第三方安装器做的薄壳 JAR 就长那样）—— 都不值得报。Failed 才是真出错
        // （读 JAR 失败、写不出去），那种留一行日志，不然"怎么老是随机那张"无从查起。
        if (resolveResult == DefaultSkin::Result::Failed) {
            emit logMessage(tr("算离线默认皮肤失败：%1").arg(defaultReason));
        }

        // ② 算不出来（没选实例，或加载器实例的 JAR 里没有原版贴图）就留着开头摆好的内置 Steve：
        // 游戏里是按名字分配默认皮肤的，预览不再随机抽一张，免得和游戏里不一样。
        m_skinFetchInFlight = false;
        emit hintChanged();
        return;
    }

    if (!m_auth->loggedIn()) {
        // 没登录就没有 uuid，连问都不用问（问也问不出东西来）。那句话现在是
        // "未登录，显示的是默认外观"，由**开头那次 applyPlayerSkin()** 顺带写好了
        // —— 那一步发回来的 skinChanged 会让主窗口重挑一次。这里不再发。
        return;
    }
    const QString uuid = m_auth->playerUuid();
    // 当前账号是离线账号的意思是"别联网"（启动时走的就是离线登录），这时候还发一个外观
    // 请求是自相矛盾的 —— 所以整条联网路径都跳过，只认已经躺在缓存里的那一份。
    //
    // NOTE: 判据是 offlineIdentity()，不是"勾选框勾没勾"。这一档**必然**同时是
    // "没有登录态"（setOfflinePlayer() 把 uuid 和令牌一起清了），上面那句 !loggedIn()
    // 其实已经先返回了 —— 留着它是为了这条路自己读起来是完整的，不靠上面的早退。
    const bool offline = offlineIdentity();

    // 1) 盘上那份档案：开窗那一刻零网络把**上次**的样子画出来。贴图是内容寻址的，
    // 命中缓存时 fetchTexture() 是**同步**发信号回来的，所以这一句之后就画好了。
    PlayerSkinProfile cached;
    if (PlayerSkinStore::loadCachedProfile(uuid, &cached)) {
        m_skinProfile = cached;
        requestTexture(cached.skinUrl, /*allowDownload=*/!offline);
        requestTexture(cached.capeUrl, /*allowDownload=*/!offline);
    }

    // 2) 后台刷一次档案：用户在游戏里换了皮肤，重开启动器就能看到。回来那份和手上
    // 这份一样时 onProfileReady() 什么都不做。
    if (!offline) {
        m_store->refreshForUuid(uuid);
    }
    emit hintChanged();
}

bool PlayerSkinController::requestTexture(const QString &url, bool allowDownload) {
    if (url.isEmpty()) {
        // 这个人没有披风（多数账号都没有）。**常态**，不是错误，也不是失败。
        return false;
    }
    if (QFile::exists(PlayerSkinStore::cachePathForUrl(url))) {
        // 命中缓存：fetchTexture() 会同步发 textureReady 回来，所以这一句之后那张
        // 贴图已经在屏幕上了。
        m_store->fetchTexture(url);
        return true;
    }
    if (allowDownload) {
        // 这一张要真的去下载了 —— 那句话得说"正在获取"，而不是"取不到"。
        m_skinFetchInFlight = true;
        m_store->fetchTexture(url);
    }
    return false;
}

void PlayerSkinController::onProfileReady(const QString &uuid, const PlayerSkinProfile &profile) {
    // 回来的时候身份可能已经变了：用户在请求飞着的时候换了账号、删掉了那个账号，或者
    // 登出了。**不是同一个身份就整份丢掉** —— 这份档案是发请求时那个人的，照着它落盘
    // 会把 A 的脸写进 profile-<B>.json（那份文件跨重启生效：下次开窗先按它画出 A 的
    // 外观，直到下一次刷新成功才纠正），照着它画就是"切了账号，脸还是上一个人的"。
    //
    // 判据取 uuidKey()：它和 refreshForUuid() 里用的是同一个规整函数，所以"带不带横线"
    // 不会让这一比较失败。
    if (uuid.isEmpty() || uuid != PlayerSkinStore::uuidKey(m_auth->playerUuid())) {
        return;
    }
    const bool sameAsBefore = profile.sameAs(m_skinProfile);
    const QString previousCapeUrl = m_skinProfile.capeUrl;
    m_skinProfile = profile;
    if (sameAsBefore && !m_skinPath.isEmpty()) {
        // 手上的就是这一份，而且已经在屏幕上 → 这次刷新**完全看不见**：不重下贴图、
        // 不重画、那句话也不动。用户在游戏里没换皮肤时，绝大多数刷新都走这一条。
        m_skinFetchInFlight = false;
        emit hintChanged();
        return;
    }

    QString error;
    // 落盘用**信号带回来的那个身份**，不用 m_auth 里当下那个（上面那道闸之后
    // 它俩本来就相等了 —— 写 uuid 是让"这份是谁的"在读者眼里只有一处出处）。
    if (!PlayerSkinStore::saveProfile(uuid, profile, &error)) {
        // 存不下来不影响这一次显示（贴图照样下、照样画），只是下次开窗要先重取一遍。
        emit logMessage(tr("存玩家外观档案失败：%1").arg(error));
    }
    // 披风那一格只在**地址真的换了**的时候才作废。
    //
    // 作废是必须的：新档案里没有披风时（用户在游戏里把披风摘了），留着的那条旧路径会让
    // 下一趟 applyPlayerSkin(skin, m_capePath) 把上一套的披风又画回去，而且再没有哪一趟
    // 会来纠正它 —— 新的披风那一张根本不会到达。
    //
    // 但**不能一律清掉**：披风先到、皮肤还在路上的时候（见 onTextureReady）那一格是
    // "记忆"，不是"现在画着的东西"，清了就等于把刚记住的那张丢了 —— 于是同一个 bug 换个
    // 位置又出现一次。所以判据是"地址换没换"，而不是"档案换没换"。
    //
    // 皮肤那一格不用管：它下一趟必被新贴图覆盖，留着还能让"新披风先到"那一趟有皮肤可配。
    if (profile.capeUrl != previousCapeUrl) {
        m_capePath.clear();
    }
    requestTexture(profile.skinUrl, /*allowDownload=*/true);
    requestTexture(profile.capeUrl, /*allowDownload=*/true);
    emit hintChanged();
}

void PlayerSkinController::onTextureReady(const QString &url, const QString &path) {
    // 两张贴图是**分开**回来的（皮肤一张、披风一张），所以这里做的是"把这一张记到它
    // 该去的那一格，再用手上这两格重画一遍" —— 谁先到都能画。
    const bool slim = m_skinProfile.variant == QStringLiteral("slim");
    if (!url.isEmpty() && url == m_skinProfile.skinUrl) {
        // 皮肤到手了：不再是"正在获取"。就算这张图后面载不进来，也该由那句
        // "取不到…"来说，而不是一直停在"正在获取"。
        m_skinFetchInFlight = false;
        applyPlayerSkin(path, m_capePath, slim);
        return;
    }
    if (!url.isEmpty() && url == m_skinProfile.capeUrl) {
        if (m_skinPath.isEmpty()) {
            // **披风先到了，皮肤还在路上**（披风那张小得多，经常先下完）。
            //
            // 这时候千万不能顺手 applyPlayerSkin(m_skinPath, path, slim) —— 皮肤那个参数
            // 是空的，而 applyPlayerSkin() 里"皮肤载不进来就退回默认外观"那条路会**把
            // 披风一起清掉**（默认外观本来就没有披风）。等皮肤那一趟回来时 m_capePath
            // 已经是空的，披风再也补不回来了：表现是**皮肤有了、披风永远没有**，而且
            // 每一趟都停在默认外观那句话上。 真机上就是这个现象。
            //
            // 所以这里只**记住**，不画：皮肤那一趟（上面那个分支）会带上 m_capePath
            // 一起画出来。这一小段时间里屏幕上是上一套外观（或者默认外观），不闪。
            m_capePath = path;
            // 皮肤那一趟**根本不会来**的时候（这个人的档案里没有皮肤那一格 ——
            // Mojang 的档案里"只有披风、没有 SKIN"是可能的），这一位必须在这儿落回 false：
            // 它的意思是"还有图在下"，而下完的就是手上这一张、皮肤那一张从没被请求过。
            // 不落的话那句「正在获取…」会一直挂着 —— 没有任何一条路会再来纠正它（皮肤
            // 那个分支永远不会执行、也不会失败，因为没有请求发出去）。
            // NOTE: 皮肤在路上的时候**不能**落（那才是正常的一小段），所以判据是
            // "档案里根本没有皮肤地址"，而不是"m_skinPath 还空着"。
            // NOTE: 这里**故意不发 `hintChanged()`**：那句话一直是靠"画一次"顺带刷的，
            // 而这一趟不画 —— 也就是说"档案里只有披风"那种人的「正在获取…」会一直挂着。
            if (m_skinProfile.skinUrl.isEmpty()) {
                m_skinFetchInFlight = false;
            }
            return;
        }
        applyPlayerSkin(m_skinPath, path, slim);
        return;
    }
    // 两个都对不上：这是一次**过期**的回调（档案已经换成别人了）。丢掉 —— 照着它画
    // 等于把上一个账号的皮肤画回来。
}

void PlayerSkinController::onStoreFailed(const QString &message) {
    // 外观是**装饰**：取不到就静默退回默认外观。这是全仓唯一一处不弹窗的错误，
    // 理由（别的错误都让用户做不成他想做的事，而这个只是不好看）写在
    // PlayerSkinStore.hpp 文件头。但日志得留一行，否则真出问题无从查起。
    emit logMessage(tr("取玩家皮肤失败：%1").arg(message));
    m_skinFetchInFlight = false;
    if (m_skinPath.isEmpty()) {
        // 已经有真皮肤在屏幕上时**不改那句话**：多数失败是**披风**那一张（皮肤早就画
        // 好了），这时候说"取不到皮肤"是错的。
        emit hintChanged();
    }
}

void PlayerSkinController::applyPlayerSkin(const QString &skinPath, const QString &capePath,
                                           bool slim) {
    QImage skin;
    if (!skinPath.isEmpty()) {
        skin = QImage(skinPath);
    }
    QImage cape;
    if (!capePath.isEmpty()) {
        cape = QImage(capePath);
    }
    if (skin.isNull()) {
        // 载不进来（文件被删了、下了一半、或者路径本来就是空的）→ 退回默认外观。
        // 这是**正常的一条路**，不是异常：这条路上 UI 什么都不用说，右栏那句话会由
        // 主窗口按"手上有没有真皮肤"重新挑一档（skinChanged 那一趟它会顺带刷）。
        skin = SkinModel::defaultSteveSkin();
        cape = QImage();
        m_skinPath.clear();
        m_capePath.clear();
    } else {
        // 只记**真的载入成功了的**路径：所以 m_skinPath 为空就等于"现在显示的是默认外观"。
        m_skinPath = skinPath;
        m_capePath = cape.isNull() ? QString() : capePath;
    }

    // 剩下的都是"把这套外观摆到那几个窗口上""图省着算一次""那句话重挑一遍"—— 三件事都
    // 只有主窗口做得到（那些控件是它的），所以这里发一张**已经画得出来的图**出去。
    // NOTE: 那张图必须是**已经载进来的**（上面退回默认外观那一步保证了这一点）—— 主窗口
    // 那边是直接把这张图交给控件的，它没有"载不进来怎么办"这一说。
    emit skinChanged(skin, cape, slim);
}

void PlayerSkinController::setCapeUrl(const QString &capeUrl) {
    const QString uuid = PlayerSkinStore::uuidKey(m_auth->playerUuid());
    if (uuid.isEmpty()) {
        return;
    }
    if (m_skinProfile.capeUrl == capeUrl) {
        if (!m_skinPath.isEmpty()) {
            const QString cached = PlayerSkinStore::cachePathForUrl(capeUrl);
            if (!capeUrl.isEmpty() && QFile::exists(cached)) {
                applyPlayerSkin(m_skinPath, cached, m_skinProfile.variant == QStringLiteral("slim"));
            }
        }
        return;
    }
    const bool hasProfile = !m_skinProfile.skinUrl.isEmpty();
    m_skinProfile.capeUrl = capeUrl;
    if (hasProfile) {
        QString error;
        if (!PlayerSkinStore::saveProfile(uuid, m_skinProfile, &error)) {
            emit logMessage(tr("存玩家外观档案失败：%1").arg(error));
        }
    }
    if (m_skinPath.isEmpty()) {
        return;
    }
    if (capeUrl.isEmpty()) {
        m_capePath.clear();
        applyPlayerSkin(m_skinPath, {}, m_skinProfile.variant == QStringLiteral("slim"));
        return;
    }
    requestTexture(capeUrl, /*allowDownload=*/true);
}
