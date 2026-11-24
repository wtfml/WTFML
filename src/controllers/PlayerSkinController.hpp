/**
 * @file PlayerSkinController.hpp
 * @brief 玩家外观那一整套（选哪一张脸、取回来、记在谁名下、降级到哪一张）从主窗口搬出来。
 *
 * 【为什么这一件事该有一个自己的 controller】
 *
 * 它是**账号域里的一个闭环**：输入是"现在是谁、现在选着哪个实例、现在是离线还是正版"，
 * 输出是"屏幕上该画哪一张皮肤 + 右栏那句话该说哪一句 + 这张脸记在谁名下"。这里面有真
 * 决策 ——
 *
 * · **哪一张**：离线账号先看**用户自己挑的那张**（`OfflineSkin`：「换一张…」），没有才按
 * 游戏自己的规矩算官方默认皮肤（`DefaultSkin`，图在客户端 JAR 里），再算不出来就显示内置
 * Steve（预览不联网、不随机）；正版账号先用盘上那份档案画出来，再后台刷一次。
 * · **认领**：贴图是按 URL 分开回来的（皮肤一张、披风一张），回来时得认得出"这一张是
 * 谁的、是皮肤还是披风"，中间用户可能已经切了账号 —— 过期的整份丢掉。
 * · **降级**：外观是装饰 —— 取不到就静默退回默认外观，只留一行日志（全仓唯一一处
 * 不弹窗的错误，理由见 `PlayerSkinStore.hpp` 文件头）。
 *
 * 主窗口在这一块只做三件事：收信号、把图交给控件、按 `hintFacts()` 选一句话。
 *
 * 【边界（谁在这一层里，谁不在）】
 *
 * · 在：挑图 / 取图 / 认领 / 记账 / 退回默认外观 / 「现在手上这张是不是真的」。
 * · 不在：**文案**。那句话该说哪一句由 `MainWindow::updateSkinHint()` 按 `hintFacts()`
 * 挑（文案的唯一出处是 `ui/MainWindowUI` 的那五个 `skinHint*Text`）；这一层只说事实。
 * · 不在：底栏那格头像图、那个窗里的 3D 预览 —— 它们和主页那个小人是**同一张图**的三个
 * 窗口，所以都由 `skinChanged()` 发给主窗口一处摆（贴在 `applyPlayerSkin` 那个信号的
 * 注释里：全仓唯一一处决定"现在显示的是哪一套外观"的仍然只有这一条）。
 * · 不在：换皮肤/换披风（要令牌，走 `SkinApi`）—— 那是那个窗上的动作，做完回来叫一次
 * `reload()` 即可。
 */

#pragma once

#include "manage/PlayerSkinStore.hpp"  // PlayerSkinProfile（成员里按值存一份，得看见定义）

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

class AuthManager;

class PlayerSkinController final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 「右栏那句话该说哪一句」要的那几格事实。
     *
     * 之所以给几格事实而不是直接给一句话：这一层不该认识中文文案，也不该认识
     * `MainWindowUI`（那是 `ui/` 的东西）。挑句子留在一处（`MainWindow::updateSkinHint()`）。
     */
    struct HintFacts {
        /// 当前账号**本身**是离线账号（判据见 `offlineIdentity()`）。
        bool offline = false;
        /// 有登录态（正版账号）。和上面那一格互斥，都假 = 未登录。
        bool loggedIn = false;
        /// 屏幕上现在真的画着一张皮肤（不是退回去的默认外观）。
        bool hasSkin = false;
        /// 那张皮肤还配着披风。
        bool hasCape = false;
        /// 有请求在路上（那句话是"正在获取"还是"取不到"就看它）。
        bool fetching = false;
        /// 手上这张是"照游戏规矩算出来的官方默认皮肤"时，是**按哪个实例**算的（版本 id）。
        /// 空 = 手上那张不是算出来的那一张（是手动挑的、或者正版档案里那张）。
        QString offlineDefaultSkinVersionId;
        /// 手上这张是**用户自己挑的**（「账号与外观」里那颗「换一张…」，见 `OfflineSkin`）。
        /// 和上面那格互斥：手动挑的那张不按实例算，换实例也不会变。
        bool offlineCustomSkin = false;
    };

    PlayerSkinController(AuthManager *auth, PlayerSkinStore *store, QObject *parent = nullptr);

    /// 这一趟走的是不是**离线身份**（当前账号本身是不是一个离线账号）。
    ///
    /// **唯一出处**：主窗口那一位现在委托到这里（`MainWindow::offlineIdentity()`）—— 底栏
    /// 那行身份、启动时要不要令牌、外观这条路联不联网，读的都是它。三处各判一次的话，就会
    /// 出现"底栏说离线、启动时却去要凭据"。
    static bool offlineIdentity();

    /// 现在的那几格事实（见 `HintFacts`）。
    HintFacts hintFacts() const;

    /// 「按哪个实例去猜那张官方默认皮肤的缓存」—— 按可能性从高到低（见 .cpp）。
    /// 给底栏账号面板那一格用（它只读盘上已经躺着的东西，一步都不联网）。
    QStringList offlineDefaultSkinCacheVersions(const QString &currentVersionId,
                                                const QString &lastLaunchedVersionId) const;

    /**
     * @brief 重新走一遍：开窗 / 登录成功 / 切账号 / 换版本 / 换完外观之后都走它。
     *
     * **先默认外观兜底**（这一步之后那张脸任何时候都不是空白的），再拿缓存档案里的贴图
     * （命中缓存是**同步**的，所以开窗那一瞬间就是上次的样子），最后后台刷一次档案。
     *
     * @param versionId 现在选着的实例（离线账号那张官方默认皮肤**跟着它走** —— 换版本这张脸
     * 真的会变，两套规矩算出来的不是同一张，见 `DefaultSkin.hpp` 文件头）。
     */
    void reload(const QString &versionId);

    // ---------- 下面几个是"外部把一条消息喂进来"的入口 ----------
    //
    // 它们平时由这个类**自己接的公用的信号**叫（PlayerSkinStore 那三个），
    // 之所以公开，是因为两条路都要用：
    // · 主窗口那几个同名的**薄壳槽**（`MainWindow::onSkinProfileReady` 等）转手到这里 ——
    // 验证工程就是按那几个名字打的（`QMetaObject::invokeMethod`），那是全程不联网演
    // "档案到了 / 贴图到了 / 挑好的那张回来了"的唯一办法；
    // · 将来别的域（比如那个窗上的"换完皮肤"）也可以直接把结果喂进来，不必知道内部状态。
    //
    // NOTE: `applyPlayerSkin()` 也在这四个里 —— 主窗口那个同名槽转手到它，而它是这一层
    // **唯一**一处改"现在画着哪两张图"的地方（载不进来就退回默认外观那一条也在里面）。
    void applyPlayerSkin(const QString &skinPath, const QString &capePath, bool slim);
    /// 换披风成功后先按**已知的新地址**更新本地档案并换图，不等下一次档案请求回来。
    void setCapeUrl(const QString &capeUrl);
    void onProfileReady(const QString &uuid, const PlayerSkinProfile &profile);
    void onTextureReady(const QString &url, const QString &path);

signals:
    /// 现在该画这一套外观了。**全仓唯一一处**决定"哪一套"的地方就是它。
    /// @param skin 一定是**画得出来的**图（载不进来时这一层已经退回默认外观了）。
    /// @param cape 空图 = 这一套没有披风（常态）。
    /// @param slim 模型是不是"纤细"那一档。
    void skinChanged(const QImage &skin, const QImage &cape, bool slim);

    /// 事实变了，那句话该重挑一遍（`MainWindow::updateSkinHint()`）。
    /// NOTE: **画了新图**那一趟走的是 `skinChanged()` —— 主窗口在那里顺带把话也挑了，
    /// 所以这里不再多发一次（两发都发只是白跑一遍，但会让"谁在什么时候刷那句话"
    /// 多一个来路，排查时看不出是哪个）。
    void hintChanged();

    /// 给人事后看的一行日志（主窗口的 `appendLog()`）。这一层自己不打日志 ——
    /// 那几行字的 prefix 是"取玩家皮肤失败"这一档，得和主窗口别的日志走在同一条路上
    /// （验证工程就是数那条路上的行数）。
    void logMessage(const QString &text);

private:
    /// 要一张贴图。@param allowDownload false（当前是离线账号时）**只认已经在缓存里的那一份**
    /// —— fetchTexture() 在缓存不命中时会真的去下一个，那正是"离线"要求别做的事。
    /// @return 这一张是不是**已经在手上了**（缓存命中）。不是"发出去请求了"。
    bool requestTexture(const QString &url, bool allowDownload);

    /// 档案/贴图那条路上的失败（`PlayerSkinStore::failed`）。
    void onStoreFailed(const QString &message);
    AuthManager *m_auth;
    /// 玩家外观的数据层（免鉴权问官方 + 内容寻址的本地缓存）。
    PlayerSkinStore *m_store;
    // 现在屏幕上用的是哪两张贴图（本地路径）。**只记真的载入成功了的** —— 所以空串
    // 就等于"现在显示的是默认外观"，右栏那句话的判据之一就是它。
    QString m_skinPath;
    QString m_capePath;
    // 最近一次**要过**的那份档案。唯一的用途：贴图是按 URL 分开回来的（皮肤一个、
    // 披风一个），回来时得知道"这一张是皮肤还是披风"，也有个东西能判"和上次一样吗"。
    PlayerSkinProfile m_skinProfile;
    // 手上这张是"照游戏规矩算出来的官方默认皮肤"时，是按哪个实例算的（见 `HintFacts`）。
    // NOTE: 它有两个读者 —— 那句话要按它说出版本名，底栏面板要按它去猜缓存路径；两处都
    // 是"用户换版本时要按新版本**重算**"这件事的产物。
    QString m_offlineDefaultSkinVersion;
    // 手上这张是用户在「账号与外观」里自己挑的那张（见 `OfflineSkin`）。换账号 / 换版本
    // 都重算，所以它只在**这一轮**算出来之后才为真（reload() 开头会清掉）。
    bool m_offlineCustomSkin = false;
    // 去官方问的这一次还在路上（决定那句话是"正在获取"还是"取不到"）。
    // NOTE: **只表示"真的发出去了一个请求"**，缓存命中不算 —— 命中是同步回来的，
    // 标成"在路上"会让那句话卡在"正在获取"上下不来。
    bool m_skinFetchInFlight = false;
};
