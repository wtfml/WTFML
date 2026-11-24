/**
 * @file MainWindowSkin.cpp
 * @brief 「脸上那张图」：外观从哪儿来、换完怎么铺到窗口上、底栏那句提示挑哪一档。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语只有一张图：`PlayerSkinController` 手上那套外观（贴图 / 披风 / 是不是细
 * 手臂），和它落到窗口上的那几处（底栏头像、主页那个小人、缩略图、账号与外观那个窗）。
 * 它不碰启动、实例树、下载 —— 从 `MainWindow.cpp` 分出来，"外观为什么没刷新"时不必在
 * 整扇窗里翻。
 *
 * 【三条薄壳必须留在类上】
 *
 * `onSkinProfileReady()` / `onSkinTextureReady()` /
 * `applyPlayerSkin()` 是**验证工程按名字 `QMetaObject::invokeMethod` 打的**（mw_check 的
 * X 节）：那是全程不联网验完整条外观路径的唯一办法。所以名字必须留在
 * `MainWindow` 上，逻辑一行都不能留在这里（全在 `PlayerSkinController`）。换翻译单元不
 * 影响按名字调用；改这三条的名字会。
 *
 * 【两个唯一出处】
 *
 * `startPlayerSkinLoad()` 是全仓"把外观拉一遍"的唯一出口（开窗末尾、登录成功、切账号、
 * 换版本、换完外观、退出登录六处都走它）；`onPlayerSkinChanged()` 是"现在显示的是哪一套"
 * 的唯一出口。两处都不该在别处再写一份。
 */

#include "MainWindow.hpp"

#include "..\ui\AccountSkinDialog.hpp"
#include "..\ui\SkinModel.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PlayerSkinController.hpp"
#include "controllers/ToolboxController.hpp"
#include "auth/AuthManager.hpp"
#include "manage/DefaultSkin.hpp"
#include "manage/OfflineSkin.hpp"
#include "manage/PlayerSkinStore.hpp"
#include "settings/LauncherSettings.hpp"

#include <QFile>
#include <QPixmap>

using namespace MainWindowViewData;

void MainWindow::startPlayerSkinLoad() {
    // NOTE: 六处调用点（开窗末尾、登录成功、切账号、换版本、换完外观、退出登录）都走
    // 这条"把外观拉一遍"的路。
    m_playerSkin->reload(selectedVersionId(m_ui->getInstalledVersionCombo()));
}

// ---------- 下面四个是**薄壳**：验证工程按名字打量它们 ----------
//
// mw_check 的 X 节（喂一份外观 / 演"档案到了""贴图到了"）和 AF5/AF6/AF7（演"挑好的那张回来
// 了"）都是 `QMetaObject::invokeMethod` 直接打这几个名字 —— 那是**全程不联网**验完整条外观
// 路径的唯一办法，所以名字必须留在这里、逻辑一行都不能留在这里。

void MainWindow::onSkinProfileReady(const QString &uuid, const PlayerSkinProfile &profile) {
    // 整份判断在 `PlayerSkinController::onProfileReady()`：认人（回来的是不是发请求时那位）、
    // 落盘、和手上那份一样就什么都不做，以及"披风地址换了才作废"那一条。
    m_playerSkin->onProfileReady(uuid, profile);
}

void MainWindow::onSkinTextureReady(const QString &url, const QString &path) {
    // 整份判断在 `PlayerSkinController::onTextureReady()`：两张贴图**分开**回来，按 URL 认领
    // 到该去的那一格，再用手上这两格重画一遍（谁先到都能画）。
    m_playerSkin->onTextureReady(url, path);
}

void MainWindow::applyPlayerSkin(const QString &skinPath, const QString &capePath, bool slim) {
    // 整份判断在 `PlayerSkinController::applyPlayerSkin()`：那里管"载不进来就退回默认外观"
    // （那张图没有地址、离线账号也没有披风），载成功才把那两格路径记下来。
    // 这一层收到的是那边发回来的 `skinChanged`（见 onPlayerSkinChanged）。
    m_playerSkin->applyPlayerSkin(skinPath, capePath, slim);
}

void MainWindow::onPlayerSkinChanged(const QImage &skin, const QImage &cape, bool slim) {
    // 收到的两张图一定是**画得出来的**（载不进来时那一边已经退回默认外观了），所以这里只管
    // 摆，不再有一句"这张图打不开怎么办"。
    m_ui->getSkinViewer()->setSkin(skin, cape, slim);
    if (m_toolboxController) m_toolboxController->setPlayerSkin(skin, cape, slim);
    // 头像在这里**算一次存起来**：updateBottomBar() 每次进度刷新（几百毫秒一次）都会被调，
    // 在它里面现算等于一直在重渲染同一张 8×8 的皮肤。裁切和放大都在
    // SkinModel::headAvatar() 里（最近邻放大，保持像素画的那个方块感）。
    m_avatarPixmap = QPixmap::fromImage(SkinModel::headAvatar(skin, MainWindowUI::avatarSize()));
    // 那个窗里的 3D 预览跟着**同一份贴图**走：它和这里是同一个东西的两个窗口，各下各的、
    // 各画各的就会出现"主页换了、那个窗还是旧外观"。挂在这一路上是因为全仓**唯一**一处决定
    // "现在显示的是哪一套外观"的就是 `PlayerSkinController::skinChanged` 这一条（贴图从哪来、
    // 档案怎么换都不影响这条）。
    if (m_accountSkinDialog != nullptr) {
        m_accountSkinDialog->setLooks(skin, cape, slim);
    }
    // 画了新东西 → 那句话按"手上有没有真皮肤"重挑一档（那一边把事实都准备好了）。
    updateSkinHint();
    updateBottomBar();
}

QPixmap MainWindow::accountFacePixmap(const SavedAccount &account, int pixelSize) const {
    // 这条路**只读本地已经躺着的东西**：不联网、不换令牌、不读客户端 JAR。
    // 面板是"点一下就弹出来"的，要求的前提也是「（因为选择/登录过，所以肯定会有缓存，
    // 应该不会遇到丢失的情况）」—— 所以这里按"缓存里有就用、没有就自己画一个"来收尾，
    // 而不是去把缺的那张拉下来（拉皮肤要网络、要令牌，还要为每一行各等一次）。
    const int size = pixelSize > 0 ? pixelSize : MainWindowUI::bottomBarPanelIconSize();
    // 缓存图 → 头像。图上取不到（文件不在 / 不是一张皮肤）就是一张空图，让调用方接着退。
    const auto faceFromTexture = [size](const QString &path) -> QPixmap {
        if (path.isEmpty() || !QFile::exists(path)) {
            return {};
        }
        const QImage texture(path);
        if (texture.isNull()) {
            return {};
        }
        // headAvatar() 自己认 64×32 的老格式（那一档只取头和帽子，是同一块区域）。
        const QImage head = SkinModel::headAvatar(texture, size);
        return head.isNull() ? QPixmap() : QPixmap::fromImage(head);
    };

    // ① 当前账号：底栏那颗头像是**现成的**（m_avatarPixmap，applyPlayerSkin() 里算好的），
    // 缩一下就是这一行的图。
    //
    // 这一条必须排在最前面。它是"面板上这一行"和"底栏那颗"是同一张脸的**唯一保证** ——
    // 走下面那几条缓存路的话，当前账号的皮肤还没画上那一档时，面板会给出一张
    // 缓存里的旧图（或者干脆一个首字母方块），同一个窗口里同一个人两种脸。
    // NOTE: 缩放的插值方式跟着 headAvatar() 那套走（最近邻）—— 那张图本来就是 8×8
    // 放大出来的像素画，平滑插值只会把脸糊掉。
    if (account.isCurrent && !m_avatarPixmap.isNull()) {
        return m_avatarPixmap.scaled(size, size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }

    if (!account.offline) {
        // ② 正版账号：缓存里那份档案写着他的皮肤地址，图按地址的 sha1 躺在同一个缓存目录里
        // （PlayerSkinStore 的两级缓存，见那边的文件头）。地址是内容哈希，所以"本地有这张
        // 图"就等于"这是他当前的皮肤"。
        PlayerSkinProfile profile;
        if (PlayerSkinStore::loadCachedProfile(account.uuid, &profile) &&
            !profile.skinUrl.isEmpty()) {
            const QPixmap face = faceFromTexture(PlayerSkinStore::cachePathForUrl(profile.skinUrl));
            if (!face.isNull()) {
                return face;
            }
        }
    } else {
        // ③ 离线账号优先用**他自己挑的那张**（「账号与外观」→「换一张…」，见 OfflineSkin）。
        // 它排在这里是为了和 PlayerSkinController::reload() 的次序一致：那个函数里手动
        // 挑的也排在随机 / 算出来的前面。次序不一致的症状就是"主页是这张、面板是那张"。
        const QPixmap manualFace = faceFromTexture(OfflineSkin::skinPathFor(account.uuid));
        if (!manualFace.isNull()) {
            return manualFace;
        }
        // ④ 其次是**算出来的**那张官方默认皮肤的缓存图（见 DefaultSkin）。
        //
        // 它的缓存名带三段（账号 / 版本 / 条目），所以这里得先猜"当初是按哪个实例算的"。
        // 猜错只是 QFile::exists 落空，**绝不会触发读 JAR** —— 那一条路要起一次
        // PowerShell（几百毫秒），挂在"点开面板"这一下上是不行的。所以这里**直接拼
        // cachePathFor()**，一步都不走 resolve()。
        // 猜"当初按哪个实例算的"那三档在 PlayerSkinController 里（它才知道上一轮真的按谁
        // 算过），这里把另两格喂给它：当下选中的实例、上次启动的版本。
        const QStringList versions = m_playerSkin->offlineDefaultSkinCacheVersions(
            selectedVersionId(m_ui->getInstalledVersionCombo()),
            m_settings->lastLaunchedVersion());
        for (const QString &versionId : versions) {
            for (const DefaultSkin::Rule rule :
                 {DefaultSkin::Rule::Eighteen, DefaultSkin::Rule::Two}) {
                const DefaultSkin::Prediction prediction = DefaultSkin::predict(account.uuid, rule);
                if (!prediction.valid) {
                    continue;
                }
                const QPixmap face = faceFromTexture(
                    DefaultSkin::cachePathFor(account.uuid, versionId, prediction.entryPath));
                if (!face.isNull()) {
                    return face;
                }
            }
        }
    }

    // ⑤ 一张图都没有（离线账号取不到默认皮肤也是这样，预览那边显示内置 Steve，这里画首字母）—— 自己画一个首字母方块。**这是常态，不是出错**：从没登录过的正版
    // 账号（列表里那条只是个凭据副本）、刚加进来还没挑过皮肤的离线账号，本来就没有图。
    // 面板不能因为某一项没图就空着那一格（三行的行首一高一低比一个方块更难认）。
    return QPixmap::fromImage(SkinModel::letterAvatar(account.name, size));
}

void MainWindow::updateSkinHint() {
    // 外观那些成员在 PlayerSkinController 里，这里不去读它们本身，
    // 而是问它要**几格事实**再挑一句。分工是：那一边说"现在是什么状况"，这里说"这种状况
    // 该讲哪一句中文" —— 文案仍然只在这一层出现（`MainWindowUI` 那五句），那一层一个中文字
    // 都不认识（也不许认识 `ui/`）。
    const QString name = m_authManager->playerName();
    QLabel *hint = m_ui->getSkinHintLabel();
    const PlayerSkinController::HintFacts facts = m_playerSkin->hintFacts();
    // 离线账号有一套自己的话（"正版外观"那句对它是错的，"未登录"那句也不对 ——
    // 它是**故意**不登录的，读起来像出错了）。判据只有一个：当前账号本身是不是离线账号。
    if (facts.hasSkin) {
        if (!facts.offline) {
            hint->setText(MainWindowUI::skinHintLoadedText(name, facts.hasCape));
        } else if (facts.offlineCustomSkin) {
            // 用户自己挑的那张（「账号与外观」→「换一张…」）：这是明确的选择，既不是
            // "某个实例算出来的"，所以有它自己那一句。
            hint->setText(MainWindowUI::skinHintOfflineCustomSkinText(name));
        } else if (!facts.offlineDefaultSkinVersionId.isEmpty()) {
            // 算出来的那张官方默认皮肤（见 DefaultSkin）：说得比"本地预览"准一点 —— 这是
            // 那个版本的客户端本来就会给他分配的那一张。版本号**不是废话**：换版本这张脸
            // 真的会变（两套规矩算出来的不是同一张），那句话要能解释这件事。
            // NOTE: 这一格存的是目录名，显示名是这一层翻的（`instanceDisplayName()`）——
            // 那一层拿不到下拉框，也不该拿到。
            hint->setText(MainWindowUI::skinHintOfflineDefaultSkinText(
                name, instanceDisplayName(facts.offlineDefaultSkinVersionId)));
        } else {
            hint->setText(MainWindowUI::skinHintOfflineLoadedText(name));
        }
    } else if (facts.offline) {
        // 取不到默认皮肤，显示的是内置 Steve（没有请求在路上）。
        hint->setText(MainWindowUI::skinHintOfflineLoadedText(name));
    } else if (!facts.loggedIn) {
        hint->setText(MainWindowUI::skinHintNotLoggedInText());
    } else if (facts.fetching) {
        hint->setText(MainWindowUI::skinHintLoadingText(name));
    } else {
        hint->setText(MainWindowUI::skinHintFailedText(name));
    }
    // 账号页预览下面的说明：离线账号的预览和游戏里不一致时才有字（本地皮肤 / 内置 Steve）。
    if (m_accountSkinDialog != nullptr) {
        QString note;
        if (facts.offline && facts.offlineCustomSkin) {
            note = AccountSkinDialog::previewNoteLocalSkin();
        } else if (facts.offline && facts.offlineDefaultSkinVersionId.isEmpty()) {
            note = AccountSkinDialog::previewNoteSteve();
        }
        m_accountSkinDialog->setPreviewNote(note);
    }
}
