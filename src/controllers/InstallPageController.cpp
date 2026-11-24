/**
 * @file InstallPageController.cpp
 * @brief 安装页那棵树、名称框、版本列表怎么摆（见 .hpp 那段）。
 *
 * 这一层**不装东西、不写盘、不自己联网**：真正的安装请求由
 * `InstallPageController::installClicked()` 发（在本文件末尾），版本列表的问与答在 `VersionManager`，
 * 名字合不合法在 `InstanceFiles::validateDirName()`。
 */

#include "controllers/InstallPageController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "PresetInstallPicker.hpp"
#include "LoadingState.hpp"
#include "presets/PresetStore.hpp"
#include "VersionIcons.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/PresetController.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/VersionManager.hpp"
#include "presets/PresetTypes.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/AppPaths.hpp"

#include <QFile>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

InstallPageController::InstallPageController(MainWindow *owner)
    : QObject(owner), m_owner(*owner), m_selectedLoader(MainWindowUI::modLoaderNoneIndex()) {}

void InstallPageController::openPage(const QString &gameVersion) {
    if (gameVersion.isEmpty()) {
        return;
    }
    m_gameVersion = gameVersion;
    m_nameTouched = false;

    // 这一层只管界面：要装的是哪个版本、名称框里默认写什么、那五条加载器现在该是什么
    // 状态。真正的安装请求在 installClicked() 里发出去。
    m_owner.m_ui->getInstallTargetLabel()->setText(MainWindowUI::installTargetText(gameVersion));
    // 默认名跟着"选中的加载器"走（没选就是原版号本身）—— updateLoaderRows() 会顺手
    // 把它算出来，所以这里先摆状态、再让它收尾。
    // 注意每次进安装页都**回到「不装（纯原版）」**：上一个实例选的是哪条加载器跟这一个
    // 没关系，留着的话下一个版本会顶着一个"上次顺手选的 Forge"进来。
    m_selectedLoader = MainWindowUI::modLoaderNoneIndex();
    // 钉住的版本号同理：换了一个游戏版本，上一份能装的版本就完全不是这一份了
    // （Forge 每个游戏版本能装的版本完全不同）。
    m_chosenVersion.clear();
    // 收起来 + 清掉上次灌进去的子条目（**不能**只 clear 整棵树：那样顶层那五条也一起
    // 没了）。takeChildren() 交出来的那些条目已经不带爹了，得自己删 —— 不删就是每进一次
    // 安装页漏一批（几十上百个 QTreeWidgetItem）。requestAllLoaderVersions() 里对每个
    // 加载器再来一遍，顺手把"正在获取…"占位摆上。
    for (int i = 0; i < MainWindowUI::modLoaderOptionTexts().size(); ++i) {
        QTreeWidgetItem *item = loaderItem(i);
        if (item == nullptr) {
            continue;
        }
        item->setExpanded(false);
        qDeleteAll(item->takeChildren());
    }
    updateLoaderRows();
    // 四个加载器能装哪些版本：**进这一页就全问出去**，不能"点开了再获取"。点开哪条
    // 现拉哪条会留下"点不开了"的状态（见 fillLoaderVersionItems() 里那段 NOTE）。
    requestAllLoaderVersions();
    // 名称框里的默认值：勾的是「不装」时就是原版号本身（那正是"装原版"这条老路）。
    updateInstallName();

    // 实例预设那一行：摘要按 `m_presetChoices` 刷一遍。
    // 用户的选择**不跟着翻页重置**：换一个版本再看一眼，不该让他已经挑好的
    // 哪几类预设悄悄没了（取消弹窗也不写它 —— 见 openInstallPresetPicker()）。
    updatePresetSummary();

    // 树那一层留着不动（用户按返回时回的就是它，连展开状态都还在）。
    m_owner.m_ui->getGameStack()->setCurrentIndex(MainWindowUI::downloadGameInstallIndex());
    m_owner.m_ui->getInstallNameEdit()->setFocus();
    m_owner.appendLog(MainWindow::tr("准备安装，基于原版 %1。").arg(gameVersion));
}

void InstallPageController::backToVersionTree() {
    // 只翻页，什么都不清：树里那份展开状态、名称框里敲过的字都留着 —— 用户按返回
    // 是想"再看一眼别的版本"，不是想"放弃刚才那些输入"。真要放弃，重进一次就重算了。
    m_owner.m_ui->getGameStack()->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
}

QString InstallPageController::defaultInstanceName() const {
    // 目录名的形状是 `1.18.2-Forge_40.3.12`、`1.21.1-NeoForge_21.1.210`。
    // 没选加载器时就是原版号本身 —— 那一份**不写薄 json**（id == 父版本），装出来的就是
    // 原版那个实例。
    //
    // NOTE: 刻意**不**走 VersionDisplay::formatVersionName()：那是**显示名**的拼法
    // （按自定义名 / 加载器名拼，`realVersion` 缺了还会被目录名兜底），而目录名要的是
    // 确定、可预期、跟显示规则无关。
    if (m_gameVersion.isEmpty()) {
        return QString();
    }
    VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
    const int index = selectedLoader();
    if (!VersionManager::loaderKindFromIndex(index, &kind)) {
        return m_gameVersion;
    }
    const QString loaderName = VersionManager::loaderDisplayName(kind);
    const QString pinned = pinnedLoaderVersion(index);
    // 钉的是一个**具体版本号**时原样写进目录名（`1.18.2-Forge_40.3.12`）。
    // 钉的是**那两档**（`@latest` / `@stable`）时**不带后缀**（`1.21.4-Forge`），
    // 和「自动（推荐）」写出来的名字一致。两个理由：
    // · 那两档最终装到哪个版本号，要等安装那条路联网挑完才知道（见
    // VersionManager::installLoaderVersion），这一刻算不出来 —— 写 `_stable` 是拿
    // "哪一档"当版本号用，而真实版本号在装完那份 wtfml.meta 的 loaderVersion 里；
    // · 名字一样才走得上「已经有叫这个的实例了 → 再装一次是校验并补齐」那条路。带后缀
    // 的话，用户"同一个游戏同一个加载器"再装一次会多出一个并行实例，而他手上的老实例
    // 还叫 `1.21.4-Forge`。同一档换个版本装进同一个实例，本来就是这条路支持的用法。
    LoaderMeta::LoaderChannel channel;
    const bool byChannel = LoaderMeta::loaderChannelFromKey(pinned, &channel);
    const QString suffix = byChannel ? QString() : pinned;
    return suffix.isEmpty() ? QStringLiteral("%1-%2").arg(m_gameVersion, loaderName)
                            : QStringLiteral("%1-%2_%3").arg(m_gameVersion, loaderName,
                                                             suffix);
}

QTreeWidgetItem *InstallPageController::loaderItem(int loaderIndex) const {
    // 负数是「预设」那一行的哨兵（`installPresetRowRole()`），不是加载器下标 ——
    // 放进去会正好把它找出来（那个 UserRole 就是 -1）。
    if (loaderIndex < 0) {
        return nullptr;
    }
    QTreeWidget *tree = m_owner.m_ui->getLoaderTree();
    if (tree == nullptr) {
        return nullptr;
    }
    // 顺序是界面自己按 modLoaderOptionNames() 摆的（第 i 条就是第 i 个加载器），
    // 但**身份**仍然从 userData 读 —— 摆的顺序哪天变了，这里不会跟着错。
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt() == loaderIndex) {
            return item;
        }
    }
    return nullptr;
}

void InstallPageController::updateLoaderRows() {
    // 那棵树的样子**全部**在这一处决定（别处一个 setText/setData 都不许写）：
    // · 选中的那一条：第二行小字写它选的那一格（版本号 / 「最新版」/「稳定版」）；
    // · 其余几条加载器：第二行小字写「与 X 不兼容」；
    // · 「不装（纯原版）」那条：没有第二行（它不是加载器，不跟谁冲突）；它仍然存在，
    // 但**不画在界面上**（只留内部状态，见 MainWindowUI 里那段 NOTE）。
    // NOTE: 选中某一条之后，**其余几条加载器变灰、也点不开**（选中之后其余几条不能
    // 点开）。改选别的加载器先点**当前这条行尾的 ×**
    // —— 那一下退回「不装（纯原版）」，其余几条重新亮起来（那条内部条目本身不画，
    // 界面上没有重复的「不装」选项可点）；换版本则点当前这条底下的版本号。
    // 本启动器一次只装一个加载器，所以"不兼容"说的是"两个不能同时装"，而灰掉
    // 说的是"这一下点了也不会变成装它"。
    const int selected = selectedLoader();
    VersionManager::LoaderKind selectedKind = VersionManager::LoaderKind::Fabric;
    const bool hasLoader = VersionManager::loaderKindFromIndex(selected, &selectedKind);
    const QString selectedName =
        hasLoader ? VersionManager::loaderDisplayName(selectedKind) : QString();

    const int count = MainWindowUI::modLoaderOptionTexts().size();
    for (int i = 0; i < count; ++i) {
        QTreeWidgetItem *item = loaderItem(i);
        if (item == nullptr) {
            continue;
        }
        VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
        const bool isLoader = VersionManager::loaderKindFromIndex(i, &kind);
        QString subText;
        if (isLoader && hasLoader) {
            if (i == selected) {
                const QString pinned = pinnedLoaderVersion(i);
                LoaderMeta::LoaderChannel channel;
                if (LoaderMeta::loaderChannelFromKey(pinned, &channel)) {
                    // 挑的是那一档：小字写档名（版本号要等装的时候才定下来）。
                    subText = MainWindowUI::loaderVersionChannelShortText(
                        channel == LoaderMeta::LoaderChannel::Newest);
                } else if (pinned.isEmpty()) {
                    // 没挑过（版本列表没取到）：按默认那一档说 —— 稳定档。
                    subText = MainWindowUI::loaderVersionChannelShortText(false);
                } else {
                    subText = pinned;
                }
            } else {
                subText = MainWindowUI::loaderConflictText(selectedName);
            }
        }
        // 角色是 rowSubTextRole()：空串 = 那一行不画第二行小字（代理那边统一处理）。
        item->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowSubTextRole(), subText);
        // 选中的那条加载器：行尾（箭头左边）画一个「取消」小叉 —— 点它 = 不装这个加载器了、
        // 退回「不装（纯原版）」（选中之后在展开箭头左侧摆一个小叉，点它就是取消安装
        // 这个加载器）。**只有这一条**有这个叉：其余几条（包括「不装」）没有"要取消的东西"。
        // 点它的那一下由 MainWindow::eventFilter 接（几何是 VersionTreeDelegate::clearRect()）。
        item->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowClearRole(),
                      isLoader && hasLoader && i == selected);
        // 灰那几条：选中了某个加载器之后**其余几条加载器**不可用（见上面那段 NOTE）。
        // 顺手把灰掉的那条**收起来** —— 收着的时候它底下的版本号看不见，而"展开着一份
        // 版本列表、整条却是灰的"看着像"点得动、只是不让点"（点一下才知道不是）。
        // 「不装（纯原版）」不参与：它不是加载器（isLoader 为假），而且是那条退路。
        if (isLoader) {
            const bool locked = hasLoader && i != selected;
            if (locked && item->isExpanded()) {
                item->setExpanded(false);
            }
            item->setDisabled(locked);
        }
    }
}

void InstallPageController::selectLoader(int loaderIndex) {
    // 「不装（纯原版）」和四个加载器都是合法值；表外的值（界面表和枚举哪天对不上）
    // 一律当作"没选加载器"—— 宁可装成原版，也不要拿着一个不认识的下标去猜加载器。
    VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
    m_selectedLoader = VersionManager::loaderKindFromIndex(loaderIndex, &kind)
                                ? loaderIndex
                                : MainWindowUI::modLoaderNoneIndex();
    updateLoaderRows();
    // 默认名里带着加载器名（和它钉的版本号），换一条就得重算一次。
    updateInstallName();
    // NOTE: **这里不发任何请求**。"有哪些版本"是进安装页时一次性全问好的
    // （见 requestAllLoaderVersions()：不能点开了再获取）。走这一句的只有两条路：点一条**版本号**（选中它爹那条加载器）、
    // 点「不装（纯原版）」那一条。点一条加载器**不再**选中它（那一下只展开/收起，
    // 见 onLoaderTreeClicked() 上面那段）—— 点一下就去碰那棵树会造成"点不开了"，别再走回去。
}

void InstallPageController::requestAllLoaderVersions() {
    // 进安装页时把**四个加载器**的版本列表一次性全问出去（不能点开了再获取，
    // 得全获取完）。四条各占一格请求（见 VersionManager 里 m_loaderVersionRequests
    // 那段），所以四条真能同时在飞。
    //
    // 代价是进一次安装页约 500 KB（Forge 那份 maven 清单自己就 211 KB）—— 换来的是
    // "点开哪一条都立刻有东西看"，而不是"点开 → 等一下 → 才出列表"那一下。
    if (m_gameVersion.isEmpty()) {
        return;  // 没定下来装哪个游戏版本，问不出"这个版本上有哪些加载器版本"
    }
    for (int i = 0; i < MainWindowUI::modLoaderOptionTexts().size(); ++i) {
        VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
        if (!VersionManager::loaderKindFromIndex(i, &kind)) {
            continue;  // 「不装（纯原版）」那一条：原版没有加载器版本可问
        }
        QTreeWidgetItem *item = loaderItem(i);
        if (item == nullptr) {
            continue;
        }
        // 先摆一条行内加载态占住那一条底下的位置：空着一条看着像"这个加载器一个
        // 版本都没有"。回复在 onLoaderVersionsFetched() 里收，按"加载器 + 游戏版本"认领。
        item->setExpanded(false);
        qDeleteAll(item->takeChildren());
        LoadingState::beginRow(item, MainWindowUI::loaderVersionFetchingText());
        m_owner.m_versionManager->requestLoaderVersions(m_gameVersion, kind);
    }
}

void InstallPageController::fillLoaderVersionItems(int loaderIndex,
                                        const LoaderMeta::LoaderVersionList &versions,
                                        bool includeChannels) {
    QTreeWidgetItem *item = loaderItem(loaderIndex);
    if (item == nullptr) {
        return;
    }
    // 别动用户已经选中的那一条：列表可能刚好是"同一条"（缓存命中的那次会走到这里），
    // 重灌一次把它顶回第 0 条就是"我选的 47.4.10 自己变回最新版了"。选中的版本号在
    // m_chosenVersion 里，所以重灌只是把条目重建一遍、再把光标放回去。
    const QString chosen = pinnedLoaderVersion(loaderIndex);
    const bool wasExpanded = item->isExpanded();
    // 连"正在获取…"那条一起清掉：它就是等这一份列表用的。
    qDeleteAll(item->takeChildren());
    // 这一版上有没有正式版 —— 稳定那一行灰不灰就看它。**列表为空时一律当作"不知道"**：
    // 那时候这份列表根本没取到（下面 includeChannels 为假的那一支是"问到了、就是没有"，
    // 连这两行都不摆），而"让安装器自己挑"那条路不依赖这份列表，所以不该把用户堵死。
    bool hasStable = false;
    for (const LoaderMeta::LoaderVersionEntry &entry : versions) {
        hasStable = hasStable || entry.stable;
    }
    // 头两条 = 那两档（见 MainWindowUI::loaderVersionLatestText 上面那段）。它们的 userData
    // 是 LoaderMeta::loaderChannelKey() 那两个键，再往下才是具体版本号。
    //
    // includeChannels 为假 = 这一版**一个能装的都没有**（versions 也是空的）：连这两档
    // 都不留 —— 理由写在 onLoaderVersionsFetched() 那段（留着等于让用户挑一条必然装不
    // 出来的路）。调用方保证"includeChannels 假"只在这种情况出现。
    QTreeWidgetItem *latestItem = nullptr;
    QTreeWidgetItem *stableItem = nullptr;
    if (includeChannels) {
        const auto mark = [loaderIndex](QTreeWidgetItem *row) {
            row->setIcon(MainWindowUI::loaderTreeKeyColumn(),
                         QIcon(VersionIcons::loaderMark(
                             loaderIndex, MainWindowUI::versionTreeIconSize())));
        };
        latestItem = new QTreeWidgetItem(item);
        latestItem->setText(MainWindowUI::loaderTreeKeyColumn(),
                            MainWindowUI::loaderVersionLatestText());
        latestItem->setData(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole,
                            LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Newest));
        mark(latestItem);

        stableItem = new QTreeWidgetItem(item);
        stableItem->setText(MainWindowUI::loaderTreeKeyColumn(),
                            MainWindowUI::loaderVersionStableText());
        stableItem->setData(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole,
                            LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Stable));
        mark(stableItem);
        // **问到了、可一条正式版都没有**（NeoForge + 26.3 就是：11 个构建全是 `-beta`）：
        // 这一行灰掉、第二行小字把原因写出来。留着能点的它等于让用户挑一条必然装不出来的
        // 路 —— 装的时候会以"还没有支持 X 的版本"收场。
        if (!versions.isEmpty() && !hasStable) {
            stableItem->setDisabled(true);
            stableItem->setData(MainWindowUI::loaderTreeKeyColumn(),
                                MainWindowUI::rowSubTextRole(),
                                MainWindowUI::loaderVersionStableMissingText());
        }
    }
    QTreeWidgetItem *chosenItem = nullptr;
    for (const LoaderMeta::LoaderVersionEntry &entry : versions) {
        const QString &version = entry.version;
        auto *child = new QTreeWidgetItem(item);
        child->setText(MainWindowUI::loaderTreeKeyColumn(), version);
        child->setData(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole, version);
        // 子条目前面挂**它自己那个加载器**的标志（那一列列出来的是"Fabric 能装哪些版本"，
        // 前面顶个草方块会让人以为是原版号）。
        child->setIcon(MainWindowUI::loaderTreeKeyColumn(),
                       QIcon(VersionIcons::loaderMark(loaderIndex,
                                                      MainWindowUI::versionTreeIconSize())));
        // 不是正式版的那几条：第二行小字标出「测试版」。**照列、也能装**（beta 版虽然
        // 不稳定，但是也能使用）—— 这一行小字是"它是什么"，不是"它不行"。
        if (!entry.stable) {
            child->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                           MainWindowUI::loaderVersionBetaSubText());
        }
        if (version == chosen) {
            chosenItem = child;
        }
    }
    // 光标放回"现在钉着的那一条"—— 展开之后一眼看到的就是它。没挑过（空串）时摆在
    // **默认会走的那一档**上：稳定档（它灰了就是最新档），和安装那条路的默认值一致。
    // **这不改选中状态**：选中是 m_selectedLoader 的事，这里只是画光标。
    if (chosenItem == nullptr) {
        LoaderMeta::LoaderChannel channel;
        if (LoaderMeta::loaderChannelFromKey(chosen, &channel)) {
            chosenItem = channel == LoaderMeta::LoaderChannel::Newest ? latestItem : stableItem;
        } else {
            // 没挑过、或者钉的那条这次没列出来（换了游戏版本那种）：退回默认那一档。
            chosenItem = (stableItem != nullptr && !stableItem->isDisabled()) ? stableItem
                                                                             : latestItem;
        }
    }
    QTreeWidget *tree = m_owner.m_ui->getLoaderTree();
    // chosenItem 可能是空指针（"这一版一个能装的都没有"那次：连那两档都没有，底下只剩一条
    // 灰的）。那种情况下这一列本来就没有"现在钉着的那一条"可指 —— 什么都不做。
    if (tree != nullptr && wasExpanded && chosenItem != nullptr) {
        tree->setCurrentItem(chosenItem);
    }
    // 列表是灌在**已经展开**的那一条底下的：条目换了一批，那棵树的高度得跟着重算
    // （MainWindowUI 那边挂在 expanded/collapsed 上，可这一批是原地换的、信号不会响）。
    // 收起来的那条不用管：它在展开的那一刻会自己算一次。
    if (wasExpanded) {
        item->setExpanded(false);
        item->setExpanded(true);
    }
}

void InstallPageController::onLoaderVersionsFetched(const QString &gameVersion, int loaderIndex, bool success,
                                         const LoaderMeta::LoaderVersionList &versions,
                                         const QString &errorMessage) {
    // 这一发回复可能已经过时了（用户换了游戏版本）。按"这条回复讲的是不是现在这一页"来判，
    // 过时的直接丢 —— 填错地方的下场是"点了 Fabric 却看到 Forge 的版本号"，那不报错、
    // 只会装错东西。
    if (gameVersion != m_gameVersion) {
        return;
    }
    // 那棵树是按**下标**摆的，日志里那句"是哪个加载器"要的是名字，所以这里翻译回枚举；
    // 反函数在这边失手（表没改全）就当没收到 —— 宁可那一条底下空着，也不要填错。
    VersionManager::LoaderKind kind;
    if (!VersionManager::loaderKindFromIndex(loaderIndex, &kind)) {
        return;
    }
    QTreeWidgetItem *item = loaderItem(loaderIndex);
    if (item == nullptr) {
        return;
    }
    if (!success) {
        // 拉不到：那一条底下**只留那两档**，外加一条灰的「列表没取到」把话说到 ——
        // 不钉版本这条路是不需要这份列表的（安装器自己会挑，问的是另一份东西：
        // promotions / maven 清单，见 installLoaderVersion 那条路），所以"列不出来"不该把
        // 安装整件事堵死，但也不能一声不吭。
        // 两档都**不灰**：这一刻我们根本不知道有没有正式版（那份列表没到手），而"让安装器
        // 自己挑"这条路由数据层去挑、挑不出来会在安装那条路上说清楚。
        // 具体原因（超时？404？）写进日志 —— 那是给排查用的，不是给用户读的。
        fillLoaderVersionItems(loaderIndex, LoaderMeta::LoaderVersionList(),
                               /*includeChannels=*/true);
        // 行尾「重试」：只撤这一行（那两档留着），原样再问一次。
        LoadingState::failRow(
            LoadingState::beginRow(item), MainWindowUI::loaderVersionFailedText(),
            [this, loaderIndex, kind, gameVersion] {
                QTreeWidgetItem *loader = loaderItem(loaderIndex);
                if (loader == nullptr || gameVersion != m_gameVersion) {
                    return;
                }
                for (int i = loader->childCount() - 1; i >= 0; --i) {
                    if (loader->child(i)->data(0, LoadingState::rowLoadingRole()).isValid()) {
                        delete loader->takeChild(i);
                    }
                }
                LoadingState::beginRow(loader, MainWindowUI::loaderVersionFetchingText());
                m_owner.m_versionManager->requestLoaderVersions(m_gameVersion, kind);
            });
        m_owner.appendLog(MainWindow::tr("%1 在 %2 上有哪些版本没取到：%3")
                      .arg(VersionManager::loaderDisplayName(kind), gameVersion, errorMessage));
        return;
    }
    if (versions.isEmpty()) {
        // **问到了、就是没有**（四种加载器上都可能：Forge 还没支持某个刚发的快照）。
        // 这时候底下**不留那两档**（列表里没有能装的版本，就不保留「自动」那两档）：
        // 留着它等于让用户挑一条必然装不出来的路 —— 安装那条路会以"X 还没有支持 Y 的版本"
        // 收场，那一下本来在这一眼就该说完（这种情况摆「无可用版本」而不是「列表没取到」）。
        //
        // NOTE: 起"**有预览版、没有正式版**"的那种版本**不算**这一支 ——
        // 「beta 版虽然不稳定，但是也能使用」，所以那一份列表非空、照列，
        // 只是稳定那一行是灰的。
        // 于是这条加载器**一个能点的子条目都没有**，也就选不中它 —— 这正是"这一条我装不了"
        // 该有的样子；底下那条灰的「无可用版本」只是把原因写出来。
        fillLoaderVersionItems(loaderIndex, LoaderMeta::LoaderVersionList(),
                               /*includeChannels=*/false);
        auto *none = new QTreeWidgetItem(item);
        none->setText(MainWindowUI::loaderTreeKeyColumn(),
                      MainWindowUI::loaderVersionUnavailableText());
        none->setDisabled(true);
        m_owner.appendLog(MainWindow::tr("%1 暂无支持 %2 的版本。")
                      .arg(VersionManager::loaderDisplayName(kind), gameVersion));
        return;
    }
    // 这一版上一条正式版都没有（真实例子：NeoForge + 26.3，11 个构建全是 `-beta`）：
    // 用户还什么都没挑的时候，把默认那一档**挪到「最新版」**上 —— 不然什么都没点、点安装，
    // 走的是稳定档那条必然装不出来的路（那一行同时也是灰的，见 fillLoaderVersionItems）。
    //
    // NOTE: 只在他**没挑过**的时候改（空串）。他已经挑过的那一格不许动 —— 那是他自己的
    // 选择，哪怕挑的正是一条现在没列出来的版本号。
    if (pinnedLoaderVersion(loaderIndex).isEmpty()) {
        bool hasStable = false;
        for (const LoaderMeta::LoaderVersionEntry &entry : versions) {
            hasStable = hasStable || entry.stable;
        }
        if (!hasStable) {
            m_chosenVersion.insert(
                loaderIndex, LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Newest));
        }
    }
    // 灌进去就完了 —— 不存缓存：这一批是进安装页时一次性问全的（requestAllLoaderVersions），
    // 每一条在这次安装页里只会被灌这一次。
    fillLoaderVersionItems(loaderIndex, versions, /*includeChannels=*/true);
}

void InstallPageController::updateInstallName() {
    QLineEdit *edit = m_owner.m_ui->getInstallNameEdit();
    if (!m_nameTouched) {
        // 用户还没自己改过名字：默认名跟着"勾中的加载器 + 它选的那个版本"走。
        // 挡信号是必须的 —— 不挡的话这一句会触发 textChanged，于是"程序填的名字"
        // 被当成"用户改过"，从此默认名再也不跟着加载器变了。
        const QSignalBlocker blocker(edit);
        edit->setText(defaultInstanceName());
    }
    updateInstallNameState();
}

void InstallPageController::updateInstallNameState() {
    QLineEdit *edit = m_owner.m_ui->getInstallNameEdit();
    const QString name = edit->text().trimmed();

    // 名字合不合法：判据在数据层（InstanceFiles::validateDirName()，和"重命名实例"、
    // "整合包导入"用的是同一套规矩）。这里只用它的结论，不抄一份规则过来。
    const QString invalid = InstanceFiles::validateDirName(name);
    const bool canInstall = invalid.isEmpty();

    // 名字框底下只有那行**红字**：名字不能用的原因（就是 validateDirName() 那句话的
    // 字面意思）。能用的时候空着 + 藏起来 —— 藏起来（而不是留一个空行）是为了让
    // 加载器树和安装按钮的位置不被这一行顶动。那句"装下去会怎样"的灰色说明**没了**
    // （用户点名是废话，见 MainWindowDownloadPage 里那一段）。
    QLabel *error = m_owner.m_ui->getInstallNameError();
    error->setText(invalid);
    error->setVisible(!canInstall);
    // 「安装」按钮：名字不合法就点不动（省掉一次"点了才弹一句名字不对"的往返）。
    // 忙的时候也不给点（那是 setDownloadUiBusy 的活，这里只是别把它顶回来）。
    m_owner.m_ui->getInstallButton()->setEnabled(canInstall && !m_owner.m_downloadInFlight);
}

void InstallPageController::noteNameEdited() {
    // 用户动过这个名字了：此后默认名不再跟着加载器变（不然他敲到一半的名字会被
    // 一次"勾了 Forge"顶掉）。
    m_nameTouched = true;
    updateInstallNameState();
}

// ---------------------------------------------------------------------------
// 「实例预设」那一行（点它开弹窗、选完写回这里、摘要刷在那一行的第二行小字上）
// ---------------------------------------------------------------------------

QTreeWidgetItem *InstallPageController::presetRowItem() const {
    QTreeWidget *tree = m_owner.m_ui->getLoaderTree();
    if (tree == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt() ==
            MainWindowUI::installPresetRowRole()) {
            return item;
        }
    }
    return nullptr;
}

void InstallPageController::updatePresetSummary() {
    QTreeWidgetItem *item = presetRowItem();
    if (item == nullptr) {
        return;
    }
    // ★ 数的是**真正会应用的那几类**，不是 `m_presetChoices` 里有几条：用户没表过态的
    // 那一类跟着默认项走，摘要也得把它算上（不然弹窗里明明选着、行上却写"未选择"）。
    const int selected = currentPresetSelections().size();
    item->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                  MainWindowUI::installPresetSummaryText(selected));
}

QList<QPair<QString, QString>> InstallPageController::currentPresetSelections() const {
    QList<QPair<QString, QString>> out;
    // 用户没表过态的那几类才问默认项 —— 表过态的（哪怕选的是"不使用预设"）一律听用户的。
    // 只在真需要时才去读盘：四类都有显式答案时这一次调用一个文件都不碰。
    QMap<QString, QString> defaults;
    bool defaultsLoaded = false;
    // 按四类的次序（不是 QMap 的字典序）：显示 / 应用 / 日志里那几句话的次序要一样。
    for (const PresetController::InstallCategory &category : PresetController::installCategories()) {
        QString id;
        if (m_presetChoices.contains(category.key)) {
            id = m_presetChoices.value(category.key);
        } else if (m_owner.m_presetController != nullptr) {
            VersionManager::LoaderKind defaultLoader;
            if (category.key == QStringLiteral("mods") &&
                (!VersionManager::loaderKindFromIndex(selectedLoader(), &defaultLoader) ||
                 defaultLoader == VersionManager::LoaderKind::OptiFine)) continue;
            if (!defaultsLoaded) {
                defaults = m_owner.m_presetController->categoryDefaultsInEffect();
                defaultsLoaded = true;
            }
            id = defaults.value(category.key);
        }
        if (!id.isEmpty()) {
            out.append({category.key, id});
        }
    }
    return out;
}

void InstallPageController::openPresetPicker() {
    if (m_owner.m_presetController == nullptr) {
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("预设不可用"),
                               MainWindow::tr("预设功能尚未初始化，无法选择预设。"));
        return;
    }
    // 弹框一点就出来（这一张开着时整页盖着，点不到第二下）。真有一张提示开着就不做事：
    // 再发一次请求会把开着那张的数据作废（只认最新一次），它就一直停在加载态。
    if (m_owner.m_ui->overlay()->promptOpen()) {
        return;
    }
    // 这一趟弹窗里改的东西**只落在这张本地草稿上**：取消 / 点「前往预设管理」都
    // 不写 `m_presetChoices`（取消不丢安装配置）。
    QMap<QString, QString> draft = m_presetChoices;

    const QString instanceName = m_owner.m_ui->getInstallNameEdit()->text().trimmed();
    struct StatusText {
        QString status;
        QStringList reasons;
        bool problem = false;
    };
    // 一份预设"对着这个还没装出来的实例"会做什么 —— **只读**。状态行只说结论，
    // 原因（缺失 / 未知 / 跳过 / 覆盖）收在下面那块可展开的地方。
    const auto describe = [this, &instanceName](const QString &presetId) {
        StatusText out;
        if (presetId.isEmpty()) {
            return out;
        }
        if (instanceName.isEmpty()) {
            out.status = MainWindow::tr("先在上面填好版本名称。");
            out.problem = true;
            return out;
        }
        const PresetController::InstallPreview preview =
            m_owner.m_presetController->previewForInstall(presetId, instanceName);
        if (!preview.problem.isEmpty()) {
            out.status = preview.problem;
            out.problem = true;
            return out;
        }
        if (!preview.blockedReason.isEmpty()) {
            out.status = preview.blockedReason;
            out.problem = true;
            return out;
        }
        if (!preview.ok) {
            out.status = MainWindow::tr("这一份算不出计划。");
            out.problem = true;
            return out;
        }
        QString text = MainWindow::tr("应用 %1 项").arg(preview.itemCount);
        if (preview.skippedCount > 0) {
            text += MainWindow::tr("，跳过 %1 项").arg(preview.skippedCount);
        }
        if (preview.blockedCount > 0) {
            text += MainWindow::tr("，%1 部分不可用").arg(preview.blockedCount);
        }
        if (preview.needsConfirm) {
            text += MainWindow::tr("，需确认覆盖");
        }
        out.status = text;
        out.reasons = preview.reasons;
        // 「要做不了 / 要确认」才染红；"跳过几项"是正常计划的一部分。
        out.problem = preview.blockedCount > 0 || preview.needsConfirm;
        return out;
    };


    // 「模组」那一类：状态行让给兼容检查，计划那几行收进原因里。
    const QString modsCategoryKey = Presets::presetCategoryKey(Presets::PresetCategory::Mods);

    // 这一块**必须在堆上**：`InlineMessageBox::setContent()` 会 `setParent(box)`，
    // 弹窗析构时按 QObject 的规矩把子控件一起 delete。栈对象被 delete 是未定义行为。
    auto *picker = new PresetInstallPicker();
    const QString pickerLoaderName =
        VersionManager::loaderKindFromIndex(selectedLoader(), nullptr)
            ? MainWindowUI::modLoaderOptionTexts().value(selectedLoader())
            : QString();
    picker->setContext(gameVersion(), pickerLoaderName);
    // 数据回来那一半：四类、复合预设、模组那一类的首轮兼容检查。
    const auto fill = [&, picker, modsCategoryKey, pickerLoaderName](
                          const PresetController::InstallPickerData &pickerData) {
        QList<PresetInstallPicker::Category> categories;
        // 四类的可选项和复合预设那一张表**来自同一次扫盘**（`requestInstallPickerData()` 那段
        // 写着为什么不能再按类各扫一趟）：这一整段只认那一份（后台读回来的）快照。
        // 每类的默认项：「用户没在弹窗里表过态」的那几类一进来就按它摆好 —— 这就是"默认"在
        // 安装页的意思（用户在弹窗里选过，哪怕是选"不使用预设"，也一律听用户的）。
        const QMap<QString, QString> defaults =
            m_owner.m_presetController != nullptr ? m_owner.m_presetController->categoryDefaultsInEffect()
                                                  : QMap<QString, QString>();
        for (const PresetController::InstallCategory &one : PresetController::installCategories()) {
            PresetInstallPicker::Category category;
            category.key = one.key;
            category.title = one.title;
            for (const Presets::PresetSummary &preset : pickerData.optionsFor(one.key)) {
                category.options.append({preset.id, preset.name, true, !preset.componentCategories.isEmpty(),
                                         preset.itemCount, QIcon(preset.iconPath)});
            }
            const bool userChose = draft.contains(one.key);
            category.selectedId = userChose ? draft.value(one.key) : defaults.value(one.key);
            if (!userChose && !category.selectedId.isEmpty()) {
                bool offered = false;
                for (const PresetInstallPicker::Option &option : category.options) {
                    if (option.id == category.selectedId) {
                        offered = true;
                        break;
                    }
                }
                // 默认项不在这一类的可选项里（默认指着的是一份别的类的 / 一份没露面的子项）⇒
                // 当这一类没有默认。**不**学下面那条"用户选过、盘上没了"的样子去摆一行
                // "已删除"：用户压根没选过它。
                if (!offered) {
                    category.selectedId.clear();
                }
            }
            // 选过、但盘上已经不在的那一份：**留在下拉框里**并标出来（「已删除/损坏的
            // 所选项明确提示，不静默换为另一项」）。名字也读不出来了，只能拿 id 认。
            if (!category.selectedId.isEmpty()) {
                bool found = false;
                for (const PresetInstallPicker::Option &option : category.options) {
                    if (option.id == category.selectedId) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    category.options.append({category.selectedId, category.selectedId, false});
                }
                if (one.key == modsCategoryKey) {
                    // 模组引用要联网查（本地/缓存那部分立刻就出来），先摆一句"正在检查"。
                    category.status = MainWindow::tr("正在检查模组兼容性…");
                    category.problem = false;
                } else {
                    const StatusText status = describe(category.selectedId);
                    category.status = status.status;
                    category.reasons = status.reasons;
                    category.problem = status.problem;
                }
            }
            categories.append(category);
        }
        picker->setCategories(categories);
        QList<PresetInstallPicker::Composite> composites;
        for (const PresetController::InstallPickerPreset &choice : pickerData.presets) {
            if (choice.summary.componentCategories.isEmpty() || !choice.componentsLoaded) continue;
            PresetInstallPicker::Composite option; option.id=choice.summary.id; option.name=choice.summary.name;
            option.parts = choice.components;
            option.icon = QIcon(choice.summary.iconPath);
            option.itemCount = choice.summary.itemCount;
            option.counts = choice.summary.componentItemCounts;
            composites.append(option);
        }
        picker->setComposites(composites);

        // 打开时已经选着的那一份（用户选的或这一类的默认）：立刻开查
        // （本地能判的那几条同步就出来，其余排队）。
        const QString initialModsPreset = picker->selectedId(modsCategoryKey);
        if (!initialModsPreset.isEmpty()) {
            // 先挂再开查：本地能判完的那一轮会同步发 finished，把它撤掉。
            picker->setChecking(modsCategoryKey, true);
            m_owner.m_presetController->startModCheck(initialModsPreset, gameVersion(),
                                              pickerLoaderName, true);
        }
    };
    // 预设数据在**后台读**（列表走 `Presets::CatalogLoader` 的异步快照，复合预设的 manifest
    // 也在工作线程里读）：点击这一帧不扫盘，右栏先挂加载态。两层 QPointer：控制器先没了 /
    // 弹框已经关了（picker 跟 box 一起析构，`fill` 引用的这几个局部也就过期了）⇒ 什么都不做。
    QPointer<InstallPageController> self(this);
    QPointer<PresetInstallPicker> alive(picker);
    std::function<void()> request;
    request = [this, self, alive, &fill, &request] {
        alive->beginLoading();
        m_owner.m_presetController->requestInstallPickerData(
            [self, alive, &fill, &request](const PresetController::InstallPickerData &data) {
                if (self.isNull() || alive.isNull()) {
                    return;
                }
                if (!data.problem.isEmpty()) {
                    alive->failLoading(data.problem, [&request] { request(); });
                    return;
                }
                fill(data);
            });
    };

    InlineMessageBox box(MessageKind::Question, MainWindowUI::installPresetPickerTitle(),
                         QString(),
                         {MainWindowUI::installPresetConfirmText(),
                          MainWindowUI::installPresetCancelText()},
                         0, -1, &m_owner);
    box.setContent(picker, 800);  // box 接管它（弹窗析构时一起 delete）
    // 设计稿 presetpick-r120：提问底色 + 拼图图标（ⓘ 在本仓库专指「详情 / 说明」）。
    box.setHeaderIcon(QStringLiteral("puzzle"));

    QObject::connect(picker, &PresetInstallPicker::pendingChanged, &box, [&box](bool pending) {
        if (auto *confirm = box.findChild<QPushButton *>(InlineMessageBox::buttonName(0)))
            confirm->setEnabled(!pending);
    });
    // 候选没读回来时「确定选择」点不得：拿空表确定会把已经挑好的几类整个清掉。
    // setCategories() 发 pendingChanged(false) 时由上面那条放开。
    if (auto *confirm = box.findChild<QPushButton *>(InlineMessageBox::buttonName(0)))
        confirm->setEnabled(false);

    // 兼容检查那几行：一条引用一行（匹配的不列，只说有问题的 + 暂无法确认的）。
    const auto modCompatReasons = [](const QList<Presets::CompatibilityResult> &results) {
        QStringList out;
        for (const Presets::CompatibilityResult &result : results) {
            if (result.state == Presets::CompatibilityState::Matched) {
                continue;
            }
            QString stateText;
            switch (result.state) {
            case Presets::CompatibilityState::Missing:
                stateText = MainWindow::tr("缺失");
                break;
            case Presets::CompatibilityState::Unknown:
                stateText = MainWindow::tr("暂无法确认");
                break;
            case Presets::CompatibilityState::Invalid:
                stateText = MainWindow::tr("引用无法识别");
                break;
            case Presets::CompatibilityState::Pending:
            case Presets::CompatibilityState::Checking:
                stateText = MainWindow::tr("检查中");
                break;
            case Presets::CompatibilityState::Matched:
                continue;
            }
            const QString name = result.name.isEmpty() ? result.projectId : result.name;
            QString line = QCoreApplication::translate("InstallPageController", "%1（%2）").arg(name, stateText);
            if (!result.detail.isEmpty()) {
                line += QCoreApplication::translate("InstallPageController", "：%1").arg(result.detail);
            }
            line += result.required ? MainWindow::tr("【必要项】") : MainWindow::tr("【可选项】");
            out.append(line);
        }
        return out;
    };

    // 兼容检查的每一发都只写到「模组」那一页 —— 且只有现在选的还是同一份时才写
    // （换了一份 / 收掉弹窗前那一刻，旧 generation 的快照不许盖掉新的）。
    QObject::connect(m_owner.m_presetController, &PresetController::modCheckUpdated, &box,
                     [picker, modsCategoryKey, modCompatReasons](
                         const PresetController::ModCheckUpdate &update) {
                         if (update.presetId.isEmpty() ||
                             picker->selectedId(modsCategoryKey) != update.presetId) {
                             return;
                         }
                         const bool problem = update.summary.requiredMissing > 0 ||
                                              update.summary.invalid > 0;
                         picker->setStatus(modsCategoryKey, update.summary.text(),
                                           modCompatReasons(update.results), problem, update.summary.total,
                                           update.summary.missing, update.summary.unknown + update.summary.invalid);
                         picker->setChecking(modsCategoryKey, !update.finished);
                     });
    // 弹窗一关就作废这一代（在途请求的回调按 generation 丢掉，不再擦已经关掉的那几行）。
    QObject::connect(&box, &QObject::destroyed, m_owner.m_presetController,
                     [this]() { m_owner.m_presetController->cancelModCheck(); });

    // 换一份就现算一次（还是只读）。算不出来也照实写在那一行上 —— 别把上一份的数留着。
    // 模组那一类的状态行归兼容检查，计划那几句收进「查看计划与原因」里。
    QObject::connect(
        picker, &PresetInstallPicker::selectionChanged, &box,
        [this, picker, &describe, modsCategoryKey, pickerLoaderName](const QString &key,
                                                                    const QString &presetId) {
            const StatusText status = describe(presetId);
            if (key != modsCategoryKey) {
                picker->setStatus(key, status.status, status.reasons, status.problem);
                return;
            }
            if (presetId.isEmpty()) {
                m_owner.m_presetController->cancelModCheck();
                picker->setStatus(key, QString(), {}, false);
                return;
            }
            QStringList reasons = status.reasons;
            if (!status.status.isEmpty()) {
                reasons.prepend(status.status);
            }
            picker->setStatus(key, MainWindow::tr("正在检查模组兼容性…"), reasons, status.problem);
            picker->setChecking(key, true);
            m_owner.m_presetController->startModCheck(presetId, gameVersion(), pickerLoaderName,
                                              true);
        });


    // 「前往预设管理」：收掉这张弹窗，**把用户带过去**，安装上下文一个字都不动。
    bool goManage = false;
    QObject::connect(picker, &PresetInstallPicker::manageRequested, &box, [this, &goManage]() {
        goManage = true;
        // 走 dismiss()：提示那一档发 promptDismissed ⇒ execPrompt 的嵌套循环醒过来。
        // 直接 closeOverlay() 的话调用方要一直等下去（见 OverlayHost 那段）。
        m_owner.m_ui->overlay()->dismiss();
    });

    request();
    m_owner.m_ui->overlay()->execPrompt(&box);
    if (goManage) {
        // 安装页那一趟的状态全在内存里（版本 / 名字 / 已选的几类）—— 切过去再切回来
        // 还是原来那一份（保留安装上下文并返回）。
        m_owner.m_ui->getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        m_owner.setInstanceMode(MainWindowUI::InstanceMode::Preset);
        return;
    }
    if (box.clickedIndex() != 0) {
        // 取消 / Esc / 点外面：草稿整个丢掉，界面上的摘要一个字都不变。
        return;
    }

    // 点「确定选择」：**整张表按弹窗里现在那几项重建**（不能只往里 insert —— 用户把
    // 某一类改回「不使用」时，旧的那一条必须消失）。
    m_presetChoices.clear();
    for (const QPair<QString, QString> &one : picker->selections()) {
        m_presetChoices.insert(one.first, one.second);
    }
    updatePresetSummary();
}

// ======================== 点「安装」========================
//
// 这一段原来是 `MainWindow::onInstallButtonClicked()`：它读的全是**这一页的选择**
// （版本号 / 加载器 / 钉的版本 / 已挑的预设），所以连「这一次装什么」的判断一起搬过来了。
// 交出去的东西只有两样：`m_owner.m_downloadCompletion->begin(...)`（这一趟排上了哪几份
// 预设，跟着下载走）和 `VersionManager::installNamed*()`。

void InstallPageController::installClicked() {
    if (m_owner.m_downloadInFlight) {
        m_owner.appendLog(MainWindow::tr("已有下载任务正在进行，请等它结束。"));
        return;
    }
    // NOTE: 必须写 this-> —— 局部变量在它自己的初始化式里就已经可见，裸 gameVersion() 会解析成那个变量。
    const QString gameVersion = this->gameVersion();
    if (gameVersion.isEmpty()) {
        // 正常走不到（安装页只能从树里一个具体版本进来）。真走到了就是界面状态错了，
        // 宁可不做 —— 猜一个版本装下去会装到用户没点过的东西上。
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("没有选中版本"),
                             MainWindow::tr("未指定原版依赖，请返回版本列表重新选择。"));
        return;
    }

    const QString name = m_owner.m_ui->getInstallNameEdit()->text().trimmed();
    const QString invalid = InstanceFiles::validateDirName(name);
    if (!invalid.isEmpty()) {
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("名字不能用"), invalid);
        return;
    }

    const int loaderIndex = selectedLoader();
    VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
    const bool withLoader = VersionManager::loaderKindFromIndex(loaderIndex, &kind);

    // 已经下过就不给"直接重下"（几 GB 白跑一趟），而是校验并补齐缺失/损坏的文件 ——
    // 这是本启动器一贯的规矩。名字和原版号一样时问的是"这个版本"，不一样时问的是
    // "这个实例"，两句话都得像人说的。
    if (QFile::exists(AppPaths::versionJsonPath(name))) {
        const bool sameAsVanilla = name == gameVersion;
        const auto answer = InlineMessage::question(m_owner.m_ui->overlay(), sameAsVanilla ? MainWindow::tr("版本已存在") : MainWindow::tr("实例已存在"),
            sameAsVanilla
                ? MainWindow::tr("版本 %1 已经下载过了。\n是否校验并修复缺失或损坏的文件？").arg(name)
                : MainWindow::tr("实例「%1」已经装过了。\n是否校验并修复缺失或损坏的文件？").arg(name), MainWindow::tr("是"), MainWindow::tr("否"), true);
        if (!answer) {
            m_owner.appendLog(MainWindow::tr("已取消对 %1 的校验。").arg(name));
            return;
        }
        m_owner.appendLog(MainWindow::tr("正在校验 %1 的文件，跳过已存在的文件…").arg(name));
    }

    const bool supportsMods = withLoader && (kind == VersionManager::LoaderKind::Fabric ||
        kind == VersionManager::LoaderKind::Forge || kind == VersionManager::LoaderKind::NeoForge ||
        kind == VersionManager::LoaderKind::Quilt);
    if (!supportsMods) {
        for (const auto &choice : currentPresetSelections()) {
            if (choice.first == QStringLiteral("mods")) {
                InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("需要模组加载器"),
                    MainWindow::tr("请选择模组加载器，或取消预设。"));
                return;
            }
        }
    }

    // ---- 实例预设：带着走的那几份先把计划摊开 ----
    //
    // 这一步在**发请求之前**：用户看到的是"装好之后会改哪些文件、有没有覆盖"的精确计划，
    // 而不是装完了才发现某个段是「覆盖」。点了取消 ⇒ 整个安装也不开始（他取消的是这一次
    // "带预设的安装"，不是"不要装了"再点一次——但那两种在屏幕上分不开，宁可不做）。
    //
    // pending 在这一刻定死：从点下安装到 versionDownloadFinished 之间用户还可能去改
    // 弹窗里的选择，而那一动不该改变这一趟已经发出去的安装（见 `DownloadCompletionController` 那段）。
    m_owner.m_downloadCompletion->clear();
    const QList<QPair<QString, QString>> presetSelections = currentPresetSelections();
    if (!presetSelections.isEmpty()) {
        if (m_owner.m_presetController == nullptr) {
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("预设不可用"),
                                   MainWindow::tr("预设功能尚未初始化，无法应用预设。"));
            return;
        }
        // 四类里那几句标题（"游戏设置 / 模组 …"）只从 controller 拿，界面这边不抄一份。
        const QList<PresetController::InstallCategory> categories =
            PresetController::installCategories();

        QStringList summaryLines;
        QList<QPair<QString, bool>> pending;
        QMap<QString, QString> fingerprints;
        for (const QPair<QString, QString> &one : presetSelections) {
            const PresetController::InstallPreview preview =
                m_owner.m_presetController->previewForInstall(one.second, name);
            if (!preview.ok || !preview.blockedReason.isEmpty() || !preview.problem.isEmpty()) {
                const QString why =
                    !preview.problem.isEmpty() ? preview.problem : preview.blockedReason;
                QString title;
                for (const PresetController::InstallCategory &category : categories) {
                    if (category.key == one.first) {
                        title = category.title;
                        break;
                    }
                }
                InlineMessage::warning(
                    m_owner.m_ui->overlay(), MainWindow::tr("无法应用此预设"),
                    MainWindow::tr("%1「%2」：%3")
                        .arg(title, preview.presetName,
                             why.isEmpty() ? MainWindow::tr("这一份算不出计划。") : why));
                return;
            }
            // 每一份自己那一道「覆盖」闸：装好之后按这一格决定要不要再问一次。
            pending.append({one.second, preview.needsConfirm});
            fingerprints.insert(one.second, preview.fingerprint);
            summaryLines.append(
                MainWindow::tr("• %1：%2").arg(preview.presetName, preview.summary));
        }
        InlineMessageBox box(
            MessageKind::Question, MainWindow::tr("安装完成后应用预设"),
            MainWindow::tr("安装「%1」后将应用以下预设：\n\n%2")
                .arg(name, summaryLines.join(QLatin1Char('\n'))),
            {MainWindow::tr("安装并应用"), MainWindow::tr("取消")}, 0, -1, &m_owner);
        box.setNote(MainWindow::tr("应用前会将受影响的文件备份到预设目录的 backups/ 子目录。"));
        m_owner.m_ui->overlay()->execPrompt(&box);
        if (box.clickedIndex() != 0) {
            m_owner.appendLog(MainWindow::tr("已取消带预设的安装。"));
            return;
        }
        m_owner.m_downloadCompletion->begin(name, pending, fingerprints);
    }

    QString installerJava = m_owner.m_javaSettings->javaPath();
    if (withLoader && !pickInstallerJava(gameVersion, loaderIndex, &installerJava)) {
        return;  // 里面已经弹过窗说清原因了，界面还没进忙态，直接退出就是干净的状态
    }

    // 忙要在点击这一刻就置上：版本 json / 加载器列表都是异步拉回来的，中间这段时间
    // 按钮还能点的话，用户会重复触发（和别处同一个理由）。
    //
    // NOTE: **顺序不能反**（「安装」那一档占用必须在 `setDownloadUiBusy(true)` 之前）。
    // `setDownloadUiBusy()` 内部会调 refreshInstalledVersions()，而那份"能启动的实例"
    // 列表是按这一档过滤出来的（installingVersionIds()）。反过来的
    // 话，置忙那一刻读到的还是**上一次**的值：对一个已经装好的实例点「安装」（校验并
    // 修复）时，它整段安装期间都留在可启动列表里 —— 用户点得动它，点下去却被
    // onStartButtonClicked() 那道按版本的闸口弹一句"文件还没有下载完成"。
    // 那正是 MainWindow.hpp 写明不许出现的"列表里能选、点下去被拦"。
    // （applyModpackImport() 那边本来就是"先记占用再置忙"，这里对齐它。）
    m_owner.m_installOccupancy.setBatch(InstallOccupancy::Batch::Install, name);
    m_owner.setDownloadUiBusy(true);
    m_owner.m_downloadTaskLedger->resetProgressThrottle();
    if (withLoader) {
        m_owner.m_versionManager->installNamedLoader(name, gameVersion, kind, pinnedLoaderVersion(loaderIndex),
                                            installerJava);
    } else {
        m_owner.m_versionManager->installNamedVanilla(name, gameVersion);
    }
}

bool InstallPageController::pickInstallerJava(const QString &gameVersion, int loaderIndex, QString *out) {
    // 安装器自己就是个 Java 程序（Forge/NeoForge 是 `java -jar <安装器>.jar --installClient
    // <目录>`，OptiFine 那个是无界面入口 + 换 APPDATA，见 VersionManager::runInstaller），
    // 所以要的运行环境和"这个版本要的 Java"是同一件事：版本 json 里的 javaVersion。
    // Fabric 那条路只拉 json + 下 maven 库，一个进程都不起，所以这一段只管要跑安装器的那些。
    //
    // 拿不到版本 json（原版还没装）就不知道要求，此时原样用配的那份 —— 不在这一步拦人：装
    // 加载器本来就会把原版补齐，而"要哪个 Java"从那份 json 里读最准，猜一个主版本比不猜更
    // 危险（见启动那条路上"拿不到 version.json 就不能猜"那段注释）。
    // 参数收下标、在这里翻一次枚举（和界面别处同一处翻译，见 VersionManager 里那段注释）。
    // 翻不出来 = 没勾加载器 / 勾的是「不装」：一个外部安装器都不跑，直接放行。
    VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
    if (!VersionManager::loaderKindFromIndex(loaderIndex, &kind)) {
        return true;
    }
    if (kind == VersionManager::LoaderKind::Fabric
        || kind == VersionManager::LoaderKind::Quilt) {
        // 这两条都只拉 profile json + maven 库，一个外部安装器进程都不起。
        return true;
    }
    const QString configuredJava = m_owner.m_javaSettings->javaPath();
    QString installerJava = configuredJava;
    int requiredJavaMajor = 0;
    const QJsonObject gameJson = m_owner.m_versionManager->getVersionJson(gameVersion);
    if (!gameJson.isEmpty()) {
        requiredJavaMajor = gameJson["javaVersion"].toObject()["majorVersion"].toInt();
    }
    QString javaReason;
    if (!m_owner.m_javaSettings->installerJavaFor(requiredJavaMajor, configuredJava, &installerJava,
                                          &javaReason)) {
        m_owner.appendLog(MainWindow::tr("这个版本要的 Java 拿不到：%1").arg(javaReason));
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("Java 版本不够"), javaReason);
        return false;
    }
    if (installerJava != configuredJava) {
        m_owner.appendLog(MainWindow::tr("这个版本要 Java %1，装的时候改用 %2").arg(requiredJavaMajor).arg(installerJava));
    }
    *out = installerJava;
    return true;
}
