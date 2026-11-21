/**
 * @file MainWindowAccount.cpp
 * @brief 「账号与外观」那一扇窗的接线：登录 / 切号 / 登出 / 上传皮肤 / 挑披风，以及底栏两块面板里账号那一行。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"现在是谁、他长什么样"：`offlineIdentity()`（离线身份的唯一出处）、
 * `refreshAccountSkin()`（把账号那一扇窗的预览与提示铺一遍）、`onAccountSkinButtonClicked()`
 * 那一扇的开关，以及登录成功 / 失败 / 切换 / 移除之后各自要刷新什么。皮肤贴图本身落到窗口
 * 上那几处在 `MainWindowSkin.cpp`；这里只管"谁"和"动作之后刷新什么"。
 *
 * 【底栏面板那一支为什么也在这里】
 *
 * `onBottomBarPanelAction()` 是底栏两块上拉面板**唯一**的入口，一个函数同时分派账号那一行与
 * 「正在跑的游戏」那一行 —— 拆不开。它夹在账号这一串动作中间，且账号那一支（登出 / 选择）
 * 正是这里的主角；跑着的游戏那一支只是把编号转给 `stopGameSession()` /
 * `openInstanceSettings()` / `GameSessionController`。`openGameLogForSession()` 是日志窗那个
 * 兼容入口（底栏与验证工程按名字调用），跟着入口一起走。
 *
 * 【盘上的账不在这一层】
 *
 * 多账号列表在 `auth/AccountStore`，凭据字节格式在 `auth/TokenStore`，
 * 界面的样子在 `ui/AccountSkinDialog`。这一层只接线，不读注册表、不排版。
 */

#include "MainWindow.hpp"

#include "..\ui\AccountSkinDialog.hpp"
#include "..\ui\InlineMessage.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\OverlayHost.hpp"
#include "auth/AccountStore.hpp"
#include "auth/AuthManager.hpp"
#include "auth/SkinApi.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PlayerSkinController.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "launcher/LauncherManager.hpp"
#include "launcher/RunningProcesses.hpp"
#include "manage/OfflineSkin.hpp"
#include "server/ServerProcess.hpp"
#include "server/ServerRegistry.hpp"
#include "util/ImageDecode.hpp"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopedValueRollback>

using namespace MainWindowViewData;

// ============================ 「账号与外观」那个模块 ============================
//
// 三件事在这个模块里的分工（写清楚免得下次改错地方）：
// - **界面的样子**全在 ui/AccountSkinDialog.{hpp,cpp}（它不 include 任何 src/ 下的头）；
// - **盘上的账**在 src/auth/AccountStore（多账号列表）+ TokenStore（凭据字节格式）；
// - **下面这一片**是接线：谁来读谁、失败怎么报、哪个动作之后要刷新什么。
// 所以这里没有任何"界面布局"的代码，那边也没有任何"读注册表"的代码。

void MainWindow::onAccountSkinButtonClicked() {
    // 覆盖层已经开着时**不能直接 return** —— 屏幕上什么都不发生：用户在别的选项卡上逛，
    // 实例设置那一页还收着占住那一层；这时按「账号与外观」看着就是"按钮点坏了"。
    // 所以并进同一套规矩：**静默收掉占着的那一层，再开**（让位那一段的唯一出处是
    // `runWhenOverlayFree()`）。
    runWhenOverlayFree([this] {
        setupAccountSkinDialog();
        OverlayHost *overlay = m_ui->overlay();
        if (overlay == nullptr) {
            return;  // 没有覆盖层就开不了（主界面建的时候就有，正常到不了这一支）
        }
        // 首帧先摆好皮肤操作，列表等页面出现后再后台查询。
        m_accountSkinDialog->setOfflineSkinState(offlineIdentity(),
                                                 OfflineSkin::hasSkin(AccountStore::activeUuid()));
        // 这一版微软登录用不了（没配应用 ID）时，「添加账号」菜单里「微软账号」置灰并带上原因。
        m_accountSkinDialog->setOnlineLoginUnavailable(AuthManager::loginConfigurationError());
        if (!overlay->showPage(m_accountSkinDialog, AccountSkinDialog::windowTitle())) {
            appendLog(tr("无法打开账号与外观页：界面已被另一页占用。"));
            return;
        }
        QMetaObject::invokeMethod(this, [this] { refreshAccountSkin(); }, Qt::QueuedConnection);
    });
}

void MainWindow::setupAccountSkinDialog() {
    if (m_accountSkinDialog != nullptr) {
        return;
    }
    m_accountSkinDialog = new AccountSkinDialog(this);
    connect(m_accountSkinDialog, &AccountSkinDialog::loginRequested, this,
            &MainWindow::onAccountLoginRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::offlineAccountRequested, this,
            &MainWindow::onOfflineAccountRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::switchRequested, this,
            &MainWindow::onAccountSwitchRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::removeRequested, this,
            &MainWindow::onAccountRemoveRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::refreshRequested, this,
            [this] { refreshAccountSkin(/*forceCapeRefresh=*/true); });
    connect(m_accountSkinDialog, &AccountSkinDialog::skinUploadRequested, this,
            &MainWindow::onSkinUploadRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::offlineSkinPickRequested, this,
            &MainWindow::onOfflineSkinPickRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::offlineSkinResetRequested, this,
            &MainWindow::onOfflineSkinResetRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::capeSelectRequested, this,
            &MainWindow::onCapeSelectRequested);
    connect(m_accountSkinDialog, &AccountSkinDialog::hideCapeRequested, this,
            &MainWindow::onCapeHideRequested);
    connect(m_skinApi, &SkinApi::ownedCapesFinished, this,
            [this](const QString &uuid, const QString &error) {
                if (uuid != AccountStore::activeUuid() ||
                    !m_authManager->loggedIn() || m_authManager->loginInFlight()) {
                    return;
                }
                m_accountSkinDialog->setCapeLoading({});
                m_accountSkinDialog->setStatusError(error);
                if (error.isEmpty()) {
                    applyCachedCapes(uuid);
                } else {
                    appendLog(tr("拉取披风列表失败：%1").arg(error));
                }
            });
    // 披风格画真贴图：走皮肤那套贴图缓存（按地址认领，别的贴图回来对不上地址就不理）。
    connect(m_skinStore, &PlayerSkinStore::textureReady, m_accountSkinDialog,
            [this](const QString &url, const QString &path) {
                m_accountSkinDialog->setCapeTexture(url, QImage(path));
            });
}

bool MainWindow::offlineIdentity() const {
    // 判据（连那段"为什么读盘不读内存副本"的道理）在
    // `PlayerSkinController::offlineIdentity()` 里 —— 外观那一套要按它决定"这一趟能不能
    // 联网"，而这一层还有十几处读它，所以留一句转手。**唯一出处仍然是那一个静态函数**：
    // 谁要判"现在是不是离线身份"都该走它，不要在这儿另写一份。
    return PlayerSkinController::offlineIdentity();
}

QString MainWindow::accountSkinStatusText() const {
    // 当前账号是离线账号时先说这一句：名字就在上面那一列里躺着，比任何泛泛的说法都具体。
    //
    // NOTE: **离线 = 当前账号本身是离线账号**，没有第二种"离线"。
    const QString active = AccountStore::activeUuid();
    if (AccountStore::isOffline(active)) {
        return AccountSkinDialog::offlineIdentityText(AccountStore::nameOf(active));
    }
    if (!m_authManager->loggedIn()) {
        return AccountSkinDialog::notLoggedInNote();
    }
    return AccountSkinDialog::identityText(m_authManager->playerName());
}

void MainWindow::refreshAccountRows() {
    if (m_accountSkinDialog == nullptr) {
        return;
    }
    QList<AccountSkinDialog::AccountRow> rows;
    const QList<SavedAccount> accounts = AccountStore::accounts();
    rows.reserve(accounts.size());
    for (const SavedAccount &account : accounts) {
        AccountSkinDialog::AccountRow row;
        row.uuid = account.uuid;
        row.name = account.name;
        row.savedAt = account.savedAt;
        row.current = account.isCurrent;
        row.offline = account.offline;
        row.needsRelogin = account.needsRelogin;
        // 每个账号自己的头像：只读本地缓存（和主页右上账号面板同一个函数），没有才是首字母。
        row.face = accountFacePixmap(account, 64).toImage();
        rows.append(row);
    }
    m_accountSkinDialog->setAccounts(rows);
}

void MainWindow::refreshAccountSkin(bool forceCapeRefresh) {
    const QString uuid = AccountStore::activeUuid();
    const bool online = !uuid.isEmpty() && !offlineIdentity() && m_authManager->loggedIn();
    if (m_accountSkinDialog == nullptr) {
        startPlayerSkinLoad();
        return;
    }
    if (m_authManager->loginInFlight()) {
        return;
    }

    refreshAccountRows();
    const bool offline = offlineIdentity();
    m_accountSkinDialog->setBusy(false);
    m_accountSkinDialog->setStatusText(accountSkinStatusText());
    m_accountSkinDialog->setStatusError({});
    m_accountSkinDialog->setCapeLoading({});
    m_accountSkinDialog->setOfflineSkinState(offline, OfflineSkin::hasSkin(uuid));
    if (!online || m_accountSkinDialog->capesOwner() != uuid) {
        m_accountSkinDialog->clearCapes();
    }
    startPlayerSkinLoad();
    if (!online) {
        return;
    }
    if (m_accountSkinDialog->capesOwner() != uuid) {
        applyCachedCapes(uuid);
    }
    if (!m_accountSkinDialog->isVisible()) {
        return;
    }
    QString token = m_authManager->savedAccessToken();
    if (token.isEmpty() || m_authManager->tokenSecondsLeft() <= 0) {
        if (!forceCapeRefresh || (!m_capeTokenRefreshUuid.isEmpty() && m_capeTokenRefreshUuid != uuid)) {
            m_accountSkinDialog->setStatusError(tr("登录凭据已过期，披风列表未更新。"));
            return;
        }
        m_accountSkinDialog->setCapeLoading(tr("正在获取披风列表…"));
        if (m_capeTokenRefreshUuid == uuid) {
            return;
        }
        QScopedValueRollback refreshGuard(m_capeTokenRefreshUuid, uuid);
        QString error;
        token = m_authManager->loadSavedToken(&error);
        if (AccountStore::activeUuid() != uuid || m_authManager->loginInFlight()) {
            return;
        }
        if (token.isEmpty()) {
            m_accountSkinDialog->setCapeLoading({});
            m_accountSkinDialog->setStatusError(error);
            return;
        }
    }
    if (!forceCapeRefresh && m_skinApi->cachedCapes(uuid, /*freshOnly=*/true)) {
        return;
    }
    m_accountSkinDialog->setCapeLoading(tr("正在获取披风列表…"));
    m_skinApi->requestOwnedCapes(uuid, token);
}

void MainWindow::applyCachedCapes(const QString &uuid) {
    const auto *capes = m_skinApi->cachedCapes(uuid);
    if (capes == nullptr) {
        return;
    }
    QList<AccountSkinDialog::CapeRow> rows;
    rows.reserve(capes->size());
    for (const OwnedCape &cape : *capes) {
        rows.append({cape.id, cape.alias, cape.active, cape.url});
    }
    m_accountSkinDialog->setCapes(rows, m_accountSkinDialog->capesOwner() == uuid);
    m_accountSkinDialog->setCapesOwner(uuid);
    for (const auto &row : rows) {
        m_skinStore->fetchTexture(row.url);
    }
}

void MainWindow::onAccountLoginRequested() {
    // 和设置页那个登录按钮**同一条路**：AuthManager 那三个信号是共用的，所以登录成功
    // 之后 onLoginSucceeded() 会把凭据收进列表、把这个窗刷新一遍，一个字都不用在这里补。
    // 返回值 true = 流程开了头；两条 false 的路径 AuthManager 会先发一次 loginFailed()，
    // 弹窗由 onLoginFailed() 统一负责。
    // 返回值刻意不管：失败那两条路径 AuthManager 会先发一次 loginFailed()，弹窗由
    // onLoginFailed() 统一负责（直接连接，所以这里返回之前那个窗已经弹出来了）。
    // 忙态也不在这里摆 —— loginMicrosoft() 一起头就会发 loginStarted()，由
    // onLoginStarted() 统一处理（见那段说明：一个来源，不给界面留第二份状态）。
    m_authManager->loginMicrosoft();
}

void MainWindow::onOfflineAccountRequested(const QString &name) {
    QString error;
    // 名字合不合法在这里判（其实是 AccountStore 判，唯一出处）：空名字、名字里有空格、
    // 超过 16 个字符。失败按产品约定弹窗 —— 这是用户主动发起、而且被挡住了的操作。
    if (!AccountStore::addOffline(name, &error)) {
        InlineMessage::warning(m_ui->overlay(), tr("添加离线账号失败"), error);
        return;
    }
    const QString trimmed = name.trimmed();
    // 离线账号 = "用这个名字离线进游戏"。**"这一趟走离线"这件事不需要另外打开** ——
    // addOffline() 已经把它设成当前账号了，而启动那条路读的正是"当前账号是不是离线账号"
    // （offlineIdentity()）。
    //
    // 名字进 AuthManager：底栏左下那行和启动参数里的 --username 取的都是它。
    m_authManager->setOfflinePlayer(trimmed);
    if (m_accountSkinDialog != nullptr) {
        // 添加成功之后才清输入框：失败时留着让用户改一下再点（重打一遍是白费）。
        m_accountSkinDialog->clearOfflineNameDraft();
    }
    updateBottomBar();
    appendLog(tr("已添加离线账号「%1」并切换到它。").arg(trimmed));
    // 列表、状态行、预览、披风这一整套刷新 —— 和切换账号之后做的事情一样。
    //
    refreshAccountSkin();
}

void MainWindow::onBottomBarPanelAction(bool account, const QString &key, int action) {
    if (account) {
        if (action != kAccountPanelUseAction) {
            return;  // 右面板那一行只有一颗按钮；别的编号是接线错了，什么都不做
        }
        // 「添加账号」「登录」「管理账号与皮肤」都进账号与外观那一页（登录 / 添加离线账号都在那儿）。
        if (key == kAccountPanelManageKey || key == kAccountPanelAddKey || key == kAccountPanelLoginKey) {
            onAccountSkinButtonClicked();
            return;
        }
        // 其余是账号行（key = uuid）：点当前那一行什么都不做（它已经打着勾），点别人 = 切过去。
        // 登出是菜单区那一行「退出登录」（上面）。
        if (key == kAccountPanelLogoutKey) {
            logoutCurrentAccount();
            return;
        }
        if (key == AccountStore::activeUuid()) {
            return;
        }
        useAccountFromPanel(key);
        return;
    }

    // 服务端进程那一档（key = "server:" + serverId），和客户端会话号彻底分开。
    QString serverId;
    if (RunningProcesses::parseServerKey(key, &serverId)) {
        switch (action) {
            case kRunningPanelStopAction: {
                ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
                if (process == nullptr || !process->isRunning()) {
                    appendLog(tr("这个服务端刚刚已经停止了。"));
                    return;
                }
                if (process->status() != ServerModel::Status::Stopping) {
                    m_serverRun->stop(serverId);
                } else if (process->forceKillAllowed()) {
                    m_serverRun->forceKill(serverId);
                } else {
                    appendLog(tr("%1：正在停服并等待存盘，宽限期内暂不强制结束。").arg(serverId));
                }
                return;
            }
            case kRunningPanelSettingsAction:
                if (!ServerRegistry::serverExists(serverId)) {
                    appendLog(tr("找不到这个服务端的目录。"));
                    return;
                }
                openServerSettingsPage(serverId);
                return;
            case kRunningPanelLogAction:
                openServerConsole(serverId);
                return;
            default:
                return;
        }
    }

    // 客户端会话（key = 会话号）。两个编号各是一颗按钮，
    // "关"和"改设置"是两件完全不同的事 —— 认编号，不认按钮上画着什么。
    switch (action) {
        case kRunningPanelStopAction:
            stopGameSession(key.toInt());
            return;
        case kRunningPanelSettingsAction: {
            // 进的是**这一行那个实例**的设置 —— 面板那一行自己带着版本号，和"当前选中的
            // 那个实例"没关系（用户点的就是这一行）。
            const LauncherManager::GameSession game = m_launcherManager->session(key.toInt());
            if (game.sessionId == 0) {
                appendLog(tr("这个实例刚刚已经结束了。"));
                return;
            }
            openInstanceSettings(game.versionId);
            return;
        }
        case kRunningPanelLogAction:
            openGameLogForSession(key.toInt());
            return;
        default:
            return;
    }
}

void MainWindow::openGameLogForSession(int sessionId) {
    // 保留这个兼容入口：底栏和验证工程按这个名字调用。
    m_gameSessionController->openGameLogForSession(sessionId);
}

void MainWindow::logoutCurrentAccount() {
    // 认人用 activeUuid()，不用面板那一行的 key —— 面板弹出来到用户点下这一颗之间隔着
    // 他看那两眼的时间，这中间账号是可能被换掉的（recentAccountPanelRows() 那段注释里
    // 说的是同一件事）。眼下这两个值必然相等（onBottomBarPanelAction() 就是拿它分流的），
    // 但"登出谁"这件事只该有一个出处。
    const QString uuid = AccountStore::activeUuid();
    if (uuid.isEmpty()) {
        return;  // 本来就没登录（那一行不该长成「登出」）
    }
    const QString name = AccountStore::nameOf(uuid);
    if (!AccountStore::logout()) {
        // 别替失败编一句"现在还是登录着"：可能一个键都没清掉，也可能清掉了一半（见
        // AccountStore::logout() 的 R13）。说"没能清干净"在任何一种情况下都是事实。
        InlineMessage::warning(m_ui->overlay(), tr("退出登录失败"),
                             tr("无法清除本地登录状态。请重试；若持续失败请检查启动器目录写权限。"));
        return;
    }
    // 内存里那一份也得跟着清 —— AuthManager 只在构造和换账号那两处读过盘，不重读的话
    // 底栏、启动参数、外观那几条路拿到的还是**上一个人的名字**（等于"看着登出了，
    // 手里还攥着他的状态"）。清干净之后 loggedIn() 是 false，界面自己退回未登录那一档。
    m_authManager->reloadStoredAccount();
    updateBottomBar();
    appendLog(tr("已退出账号「%1」（账号仍保留在列表中，点该行的「选择」或「登录」即可重新登录）。")
                  .arg(name));
    // 外观也得跟着回默认：预览、账号列表里那个「当前」标记、披风列表都要重画一遍。
    // 没登录这一档走上半截就返回了（loadSavedToken() 取不到令牌），不会去联网。
    refreshAccountSkin();
}

void MainWindow::useAccountFromPanel(const QString &uuid) {
    if (uuid.isEmpty() || uuid == AccountStore::activeUuid()) {
        // 已经是它了。那一行的按钮本来就是灰的（见 recentAccountPanelRows()），走到这儿
        // 只是防一手。
        return;
    }
    // 切换账号只读盘并更新界面，不触发网络刷新（用到令牌时再按 6 小时窗口按需刷新）。
    onAccountSwitchRequested(uuid);
}

void MainWindow::onAccountSwitchRequested(const QString &uuid) {
    if (uuid.isEmpty() || uuid == AccountStore::activeUuid()) {
        return;  // 已经是它了：写盘一遍再读回来，白折腾
    }
    // 先问清是哪一种：activate() 之后盘上的状态已经变了，那时候再问容易问错。
    const bool offline = AccountStore::isOffline(uuid);
    QString error;
    if (!AccountStore::activate(uuid, &error)) {
        InlineMessage::warning(m_ui->overlay(), tr("切换账号失败"), error);
        return;
    }
    // 盘上换人了，内存里那一份也得跟着换 —— 不换的话底栏和启动参数里还是上一个人的名字
    // （AuthManager 只在构造时读过盘，见 reloadStoredAccount()）。
    if (offline) {
        // 离线账号没有凭据可读（活动槽刚被它清空），名字从账号列表里来。
        // "这一趟走离线"不用另外记一笔：activate() 已经把当前账号换成它了，
        // 启动那条路读的就是这个（offlineIdentity()）。
        m_authManager->setOfflinePlayer(AccountStore::nameOf(uuid));
    } else {
        m_authManager->reloadStoredAccount();
    }
    updateBottomBar();
    appendLog(tr("已切换到账号「%1」。").arg(AccountStore::nameOf(uuid)));
    // 切号不刷新令牌；若页开着且当前令牌未过期，用本地令牌拉新账号的披风，已过期则在失败行提示。
    refreshAccountSkin();
}

void MainWindow::onAccountRemoveRequested(const QString &uuid) {
    if (uuid.isEmpty()) {
        return;
    }
    const QString name = AccountStore::nameOf(uuid);
    // 删掉的正好是**当前**账号时，活动槽也一起清空（等于退出登录）—— 这件事必须在确认框
    // 里说清楚，用户点下去才知道自己会退出登录（见 AccountStore.hpp 文件头）。
    const bool wasCurrent = AccountStore::activeUuid() == uuid;
    const QString question =
        wasCurrent ? tr("确定删除当前账号「%1」？本地登录凭证将一并清除。")
                         .arg(name)
                   : tr("确定删除账号「%1」？本地登录凭证将一并清除。").arg(name);
    if (!InlineMessage::question(m_ui->overlay(), tr("删除账号"), question, tr("删除"), tr("取消"), true,
                                            /*destructive=*/true)) {
        return;
    }
    QString removeError;
    if (!AccountStore::remove(uuid, &removeError)) {
        // 两种失败：列表里本来就没有它；或者列表项删了、但活动槽那几个键没清干净
        // （R13，removeError 里写着原因）。
        InlineMessage::warning(m_ui->overlay(), tr("删除账号失败"),
                             removeError.isEmpty() ? tr("这个账号不在已保存的列表里。")
                                                   : removeError);
        return;
    }
    if (wasCurrent) {
        m_authManager->reloadStoredAccount();
        updateBottomBar();
    }
    m_skinApi->discardCapes(uuid);
    appendLog(tr("已删除账号「%1」。").arg(name));
    refreshAccountSkin();
}

namespace {

/// 用户挑的皮肤图**文件**的字节上限（一张合法的皮肤不过几 KB；这里挡的是"把一整部
/// 电影当皮肤选进来"那种 —— 读进来的每个字节都要占内存）。
constexpr qint64 kMaxPickedSkinBytes = 16LL * 1024 * 1024;

/// 皮肤图的解码上限。皮肤只可能是 64×64 / 64×32（真正的判据是
/// `SkinApi::isAcceptableSkinSize()`），256 只是给将来放宽留余量；这一道挡的是
/// "照一个荒唐的声明去分配"（见 `ImageDecode::Limits`）。
constexpr ImageDecode::Limits kPickedSkinLimits{256, 256 * 256};

} // namespace

void MainWindow::onSkinUploadRequested(const QString &path, const QString &variant) {
    // 先把文件读出来。**尺寸那条规矩只在 SkinApi::isAcceptableSkinSize() 里**（唯一出处），
    // 这里不自己再判一遍。但"读不出来"得在这里说 —— 它连尺寸都没有。
    QFile file(path);
    const QString fileName = QFileInfo(path).fileName();
    // 文件本身也封一个顶：一张合法皮肤不过几 KB，读进来的每一字节都要占内存。
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
        || file.size() > kMaxPickedSkinBytes) {
        InlineMessage::warning(m_ui->overlay(), tr("上传皮肤失败"),
                             tr("无法读取文件「%1」，或图片尺寸超出限制。")
                                 .arg(fileName));
        return;
    }
    const QByteArray png = file.readAll();
    file.close();
    // 宽高从**同一份字节**里拿（不再按路径读第二遍）。皮肤图是用户挑的本机文件，
    // 也可能来自群里那个"皮肤包" —— 先问尺寸、超了不解（见 `ImageDecode::Limits`）。
    const QImage image = ImageDecode::decode(png, kPickedSkinLimits);
    if (image.isNull()) {
        InlineMessage::warning(m_ui->overlay(), tr("上传皮肤失败"),
                             tr("「%1」不是一张能读出来的图片。").arg(fileName));
        return;
    }
    if (!SkinApi::isAcceptableSkinSize(image.width(), image.height())) {
        InlineMessage::warning(m_ui->overlay(), tr("上传皮肤失败"),
                             tr("皮肤图必须是 64×64（新格式）或者 64×32（老格式），这张是 "
                                "%1×%2。")
                                 .arg(image.width())
                                 .arg(image.height()));
        return;
    }

    QString error;
    const QString token = m_authManager->loadSavedToken(&error);
    if (token.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), tr("上传皮肤失败"), error);
        return;
    }
    if (!m_skinApi->uploadSkin(token, png, variant, &error)) {
        InlineMessage::warning(m_ui->overlay(), tr("上传皮肤失败"), error);
        return;
    }
    appendLog(tr("皮肤已上传（模型 %1）。").arg(variant));
    // 换完立刻重拉一遍：预览要显示**刚传上去的**那一张，而不是等下次开窗。
    refreshAccountSkin();
}

void MainWindow::onOfflineSkinPickRequested(const QString &path, const QString &variant) {
    // 离线账号那一颗。**尺寸那条规矩仍然只在 SkinApi::isAcceptableSkinSize() 里**
    // （和正版上传那条路同一个出处，见 onSkinUploadRequested）。
    const QString uuid = AccountStore::activeUuid();
    if (!AccountStore::isOffline(uuid)) {
        // 界面上这一颗只在离线档露着；真走到这里说明状态已经变了（比如另一处刚切了账号）。
        InlineMessage::warning(m_ui->overlay(), tr("换皮肤失败"),
                               tr("当前不是离线账号，这一张没有换。"));
        return;
    }
    const QString fileName = QFileInfo(path).fileName();
    // 和正版上传那条路同一个上限、同一个理由（那是一张用户挑的本机文件，而
    // "皮肤包"里的图不一定是好心的）。
    QImage image;
    QFile skinFile(path);
    if (skinFile.open(QIODevice::ReadOnly) && skinFile.size() > 0
        && skinFile.size() <= kMaxPickedSkinBytes) {
        image = ImageDecode::decode(skinFile.readAll(), kPickedSkinLimits);
        skinFile.close();
    }
    if (image.isNull()) {
        InlineMessage::warning(m_ui->overlay(), tr("换皮肤失败"),
                               tr("读不到「%1」这张图（或者它不是一张能读出来的图片）。")
                                   .arg(fileName));
        return;
    }
    if (!SkinApi::isAcceptableSkinSize(image.width(), image.height())) {
        InlineMessage::warning(m_ui->overlay(), tr("换皮肤失败"),
                               tr("皮肤图必须是 64×64（新格式）或者 64×32（老格式），这张是 "
                                  "%1×%2。")
                                   .arg(image.width())
                                   .arg(image.height()));
        return;
    }
    QString error;
    if (!OfflineSkin::save(uuid, image, variant, &error)) {
        InlineMessage::warning(m_ui->overlay(), tr("换皮肤失败"), error);
        return;
    }
    appendLog(tr("已换成自己挑的皮肤（模型 %1）。").arg(OfflineSkin::loadModel(uuid)));
    // 重走一遍：主页的小人、底栏的头像、这一页的 3D 预览都是同一份图（见
    // PlayerSkinController::reload() 里手动那张排在最前面）。
    refreshAccountSkin();
}

void MainWindow::onOfflineSkinResetRequested() {
    const QString uuid = AccountStore::activeUuid();
    if (!AccountStore::isOffline(uuid)) {
        return;  // 这一颗只在离线档露着，同上
    }
    if (!OfflineSkin::clear(uuid)) {
        InlineMessage::warning(m_ui->overlay(), tr("恢复默认外观失败"),
                               tr("删除皮肤缓存失败，请检查数据目录写权限。"));
        return;
    }
    appendLog(tr("已恢复默认外观。"));
    refreshAccountSkin();
}

void MainWindow::onCapeSelectRequested(const QString &capeId) {
    if (!capeId.isEmpty()) {
        changeCape(capeId);
    }
}

void MainWindow::onCapeHideRequested() {
    changeCape({});
}

void MainWindow::changeCape(const QString &capeId) {
    const QString uuid = AccountStore::activeUuid();
    if (uuid.isEmpty() || m_accountSkinDialog == nullptr || m_accountSkinDialog->capesOwner() != uuid ||
        !m_authManager->loggedIn() || m_authManager->loginInFlight()) {
        return;
    }
    if (m_capeWriteInFlight || !m_capeTokenRefreshUuid.isEmpty() ||
        (m_lastCapeAction.isValid() && m_lastCapeAction.elapsed() < 1500)) {
        appendLog(tr("换披风太频繁，请稍后再试。"));
        return;
    }
    QScopedValueRollback writeGuard(m_capeWriteInFlight, true);
    m_lastCapeAction.start();
    const bool hide = capeId.isEmpty();
    const QString title = hide ? tr("取消披风失败") : tr("更换披风失败");
    QString error;
    QString token = m_authManager->savedAccessToken();
    if (token.isEmpty() || m_authManager->tokenSecondsLeft() <= 0) {
        token = m_authManager->loadSavedToken(&error);
    }
    if (AccountStore::activeUuid() != uuid || m_authManager->loginInFlight()) {
        return;
    }
    if (token.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), title, error);
        return;
    }
    const bool ok = hide ? m_skinApi->hideCape(token, &error)
                         : m_skinApi->setActiveCape(token, capeId, &error);
    if (ok) {
        m_skinApi->updateCachedCape(uuid, capeId);
        // 接口写成功不等于档案请求马上能拿到新地址（Mojang 那边有传播延迟）。先把
        // 列表里这一件的地址按当前档案落下来，主页和预览立即换图，不等下一次刷新。
        QString capeUrl = hide ? QString() : m_accountSkinDialog->capeUrl(capeId);
        bool capeFound = hide || !capeUrl.isEmpty();
        if (!hide && !capeFound) {
            // 列表还没拿到地址时退回缓存列表，两条来源看的是同一批披风。
            if (const auto *capes = m_skinApi->cachedCapes(uuid)) {
                for (const OwnedCape &cape : *capes) {
                    if (cape.id == capeId) {
                        capeUrl = cape.url;
                        capeFound = true;
                        break;
                    }
                }
            }
        }
        if (capeFound) m_playerSkin->setCapeUrl(capeUrl);
        appendLog(hide ? tr("已取消披风（不显示）。") : tr("已更换披风。"));
    }
    if (AccountStore::activeUuid() != uuid || m_authManager->loginInFlight()) {
        return;
    }
    if (!ok) {
        InlineMessage::warning(m_ui->overlay(), title, error);
        return;
    }
    m_accountSkinDialog->setCapeLoading({});
    m_accountSkinDialog->setStatusError({});
    applyCachedCapes(uuid);
    // 这里**不再** `startPlayerSkinLoad()`：换披风已经按 selected cape 的地址直接换了图，
    // 再 reload 一次会先摆回默认外观、再去读档案，旧披风有可能在中间被重新画出来。
}

void MainWindow::onLoginStarted() {
    // 从这一刻到浏览器回调回来可能有几十秒（用户要输账号密码、可能还要过二次验证）。
    // 界面上要说明自己在等什么的地方是**那个窗**：按钮一起变灰、状态行写清在等什么。
    //
    // NOTE: 写在这里（信号）而不是点下去那一下（槽）：「在登录中」就只有一个来源
    // （AuthManager 自己），以后再多一个入口起这个流程，界面照样跟得上。
    // 超时兜底也在那边 —— 浏览器再也不回来时它会发一次 loginFailed()，
    // 忙态由 onLoginFailed() 松开。
    if (m_accountSkinDialog != nullptr) {
        m_accountSkinDialog->setCapeLoading({});
        m_accountSkinDialog->setBusy(true, tr("正在等浏览器完成登录…"));
    }
}

void MainWindow::onLoginSucceeded(const QString &playerName) {
    // 底栏左下的用户名和头像都换成真名 —— 这是"启动器有反应"里最该看见的那一下
    updateBottomBar();
    // 外观也要跟着换成**这个账号**的：底下那行名字已经变了，右栏还画着上一个人的
    // 皮肤，是这一屏里最容易看出来"没跟上"的一处。这里走的是和开窗同一条路
    // （先默认外观兜底，再拿缓存，最后后台刷），所以离线勾着时也一样不联网。
    startPlayerSkinLoad();
    // 这次登录的凭据要**收进多账号列表**（列表里就多一条，或者更新已有的那一条）：
    // 不收的话，用户切到别的账号之后就再也回不到这一个了 —— 那是这一整块功能最要紧的
    // 一步，而不是可选的记账。
    QString accountError;
    if (!AccountStore::captureActive(&accountError)) {
        // 收不进列表**不影响这一次登录**（凭据已经在活动槽里，启动游戏照旧），所以不弹窗；
        // 但要说一声，否则用户下次开那个窗会发现"我刚登录的账号怎么不在列表里"。
        appendLog(tr("保存账号到列表失败：%1").arg(accountError));
    }
    if (m_accountSkinDialog != nullptr) {
        m_skinApi->discardCapes(AccountStore::activeUuid());
        m_accountSkinDialog->clearCapes();
        refreshAccountSkin();
    }
    // 用户刚在浏览器里看到过「登录成功」，这里再说一声是为了把**登录到的是哪个账户**
    // 摆出来：拿错微软账户登录（家里共用的机器上很常见）只有这句话能看出来。
    InlineMessage::information(m_ui->overlay(), tr("登录成功"), tr("已登录为「%1」。").arg(playerName));
}

void MainWindow::onLoginFailed(const QString &errorMessage) {
    if (m_accountSkinDialog != nullptr) {
        // 那个窗上也可能有一个人在等（它是从「添加账号」→「微软账号」起的流程）。忙态必须在这里
        // 松开，否则弹窗关掉之后它那几个按钮一直灰着，看着像卡死了。
        m_accountSkinDialog->setBusy(false);
        m_accountSkinDialog->setStatusText(accountSkinStatusText());
    }
    // 登录失败还可能是"**上一个在线账号已经不在盘上了**"。
    // 登录那一趟开头会把活动槽里的 Minecraft 凭据清掉（防"旧 uuid 配新刷新令牌"的错配，
    // 见 AuthManager::exchangeAuthorizationCode），而 AuthManager 在失败那条路上已经把
    // 内存里那份身份跟着盘清了（见 handleNewConnection 里那段）。**界面得跟着画一遍**：
    // 底栏那个名字是上一次 updateBottomBar() 摆上去的，不重画就会一直停在上一个人的名字
    // 上，而底栏第二行"未登录"的判断（offlineIdentity()，读盘）与启动那句判据都说"没人"。
    //
    // 条件写全是为了**别动离线账号那一档**：那时候盘上什么都没变（离线账号没有凭据），
    // 底栏该继续写离线那个名字。而"从来没登录过 + 登录失败"也会走进来一次，那一趟是
    // 幂等的：画出来还是原来那张默认脸、占位名字。
    if (!offlineIdentity() && !m_authManager->loggedIn()) {
        updateBottomBar();
        refreshAccountSkin();
    }
    // 登录失败是**挡住启动**的错误（没有令牌就点不动「启动游戏」），按产品约定必须弹窗。
    // 正文是 AuthManager 那边给的、能直接给用户看的话（"这个微软账户还没注册过 Xbox 账户，
    // 到 xbox.com 登录一次"这种），不是一句"登录失败"。
    InlineMessage::warning(m_ui->overlay(), tr("登录失败"), errorMessage);
}
