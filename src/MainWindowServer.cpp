/**
 * @file MainWindowServer.cpp
 * @brief 服务端那两页的接线：列表重扫、点一行之后发生什么、控制台怎么开、设置页那几组动作、修 / 删 / 起停，以及「新建服务端」那一页。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语只有一样：`ServerRegistry` 里那几台服务端，以及
 * `ServerWorkspaceController` / `ServerRunController` / `ServerConsoleController` /
 * `ServerSettingsPageController` 给的那几格事实。它不碰实例树、下载批次、账号 ——
 * 从 `MainWindow.cpp` 分出来，改"点一行服务端之后发生什么"时不必在整扇窗里翻。
 *
 * 「新建服务端」那一页（`openNewServerPage()` / 三档那一串 / `installServer()`）也在这里：
 * 它和列表共用同一份 `ServerRegistry` 与 `ServerInstallController`，"装好之后回到列表"
 * 那条收尾本来就在这片的 `finishUpdateBatch()` 旁边。
 *
 * 文件顶上那个匿名命名空间也跟着搬：`loaderMarkIndexForServerFlavor()` 只给这一片算
 * "服务端八档挂哪张图"，`kMaxLoggedServerLineChars` 只给这里截日志行 —— 留在原来的
 * 地方就是"一个只在别处用的常量"。内链接的东西不跨翻译单元，所以它必须整段跟着走。
 *
 * 【这一片里没有判据】
 *
 * 命令怎么拼、起来没有、宽限多久才允许强杀，在 `server/ServerRun.hpp`；
 * 装到哪一步、还差哪一份元数据，在 `download/ServerInstall.hpp` 与
 * `server/ServerInstallRun.hpp`；EULA 与那几份文件在 `server/ServerCheck.hpp`。
 * 这里只把"谁按了什么 → 调哪一个 / 界面上那颗按钮该写什么字"接起来。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\ListRowDelegate.hpp"
#include "..\ui\LoadingState.hpp"
#include "..\ui\VersionIcons.hpp"
#include "..\ui\OverlayHost.hpp"
#include "..\ui\Theme.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/ServerConsoleController.hpp"
#include "controllers/ServerManagementActions.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerSettingsPageController.hpp"
#include "controllers/ServerVersionPicker.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "download/DownloadManager.hpp"
#include "download/ServerInstall.hpp"
#include "net/ControlGet.hpp"
#include "net/HttpClient.hpp"
#include "server/ServerInstallRun.hpp"
#include "server/ServerModel.hpp"
#include "server/ServerProcess.hpp"
#include "server/ServerRegistry.hpp"
#include "manage/VersionManager.hpp"
#include "server/ServerRun.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/AppPaths.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>

using namespace MainWindowViewData;

namespace {

/// 服务端那一档在 `VersionIcons::loaderMark()` 那张表里的下标。
///
/// 那张表的顺序是 0 Fabric / 1 Forge / 2 NeoForge / 3 OptiFine / 4 Quilt，5（「不装（纯原版）」）
/// 和越界的下标给草方块。服务端八档里 Fabric / Forge / NeoForge / Quilt 有自己那张图，
/// 其余（原版 / Paper / Purpur / Spigot）一律拿草方块 —— 别去新画一张，也别借 OptiFine 的。
int loaderMarkIndexForServerFlavor(ServerInstall::Flavor flavor) {
    switch (flavor) {
    case ServerInstall::Flavor::Fabric:
        return 0;
    case ServerInstall::Flavor::Forge:
        return 1;
    case ServerInstall::Flavor::NeoForge:
        return 2;
    case ServerInstall::Flavor::Quilt:
        return 4;
    default:
        return 5;
    }
}

/// 启动器日志里一行最多留这么多个字符。服务端那些行可以很长（mod 列表、堆栈），
/// 原样抄进去的话底栏那个日志窗会被一条长句撑得没法看。**只截写进日志的这一份**，
/// 控制台那一页里照旧是原话。
constexpr int kMaxLoggedServerLineChars = 400;

}  // namespace

void MainWindow::refreshServerList() {
    if (m_ui == nullptr) {
        return;
    }
    QTreeWidget *tree = m_ui->getServerList();
    if (tree == nullptr) {
        return;
    }
    const QString keepId = [&tree] {
        const QTreeWidgetItem *current = tree->currentItem();
        return current == nullptr ? QString() : current->data(0, Qt::UserRole).toString();
    }();

    tree->clear();
    const QList<ServerRegistry::Entry> entries = ServerRegistry::scanAll();
    const QSize rowSize(0, ListRowDelegate::rowHeight());
    for (const ServerRegistry::Entry &entry : entries) {
        auto *item = new QTreeWidgetItem(tree);
        item->setData(0, Qt::UserRole, entry.serverId);
        // 行首那张图：档名对应的加载器标志（认不出来的档一律草方块，见那一处）。
        item->setData(0, ListRowDelegate::CoverRole,
                      QIcon(VersionIcons::loaderMark(loaderMarkIndexForServerFlavor(entry.flavor), 48)));
        item->setSizeHint(0, rowSize);
        // 名字 / 副行 / 第三格的字与种类 —— **全在 updateServerRow() 一处**
        // （刚建出来的那一行也要走一遍，别在这儿再抄一份摆法）。
        updateServerRow(entry.serverId);
    }
    if (QTreeWidgetItem *first = tree->topLevelItem(0)) {
        tree->setCurrentItem(first);
    }
    // 把选中项还回去（重扫前后停在同一行上）。用户上一次选的那一个还在就选它。
    if (!keepId.isEmpty()) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            if (item != nullptr && item->data(0, Qt::UserRole).toString() == keepId) {
                tree->setCurrentItem(item);
                break;
            }
        }
    }
    if (QLabel *empty = m_ui->getServerEmptyLabel()) {
        empty->setVisible(entries.isEmpty());
    }
    m_ui->setServerFolderText(tr("%1（%2 个服务端）")
                                  .arg(QDir::toNativeSeparators(ServerRegistry::rootDir()))
                                  .arg(entries.size()));
}

void MainWindow::openNewServerPage() {
    if (m_ui == nullptr) {
        return;
    }
    QWidget *formHost = m_ui->getServerFormHost();
    QStackedWidget *stack = m_ui->getServerStack();
    if (formHost == nullptr || stack == nullptr || formHost->layout() == nullptr) {
        return;
    }
    const bool fromDownload = sender() == m_ui->getDownloadNewServerButton();
    const QString selected = fromDownload
        ? m_ui->getDownloadNewServerButton()->property("selectedVersion").toString().trimmed()
        : QString();
    // 管理入口没有选过游戏版本，先回列表；不能借用主页实例的版本跳过这一步。
    if (selected.isEmpty()) {
        cancelPendingServerVersionProbe();
        m_ui->getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        {
            const QScopedValueRollback<bool> suppress(m_suppressGameInstallServerJump, true);
            m_ui->getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryServerIndex());
        }
        stack->setCurrentIndex(MainWindowUI::downloadServerListIndex());
        return;
    }
    showNewServerForm(selected, fromDownload);
}

void MainWindow::showNewServerForm(const QString &selected, bool fromDownload) {
    if (m_ui == nullptr) {
        return;
    }
    QWidget *formHost = m_ui->getServerFormHost();
    QStackedWidget *stack = m_ui->getServerStack();
    if (formHost == nullptr || stack == nullptr || formHost->layout() == nullptr) {
        return;
    }
    cancelPendingServerVersionProbe();
    if (m_newServerPage == nullptr) {
        // 它**不是**单独开的一页：和「下载 → 游戏」那张安装页一样，就住在服务端那一栏的
        // 第二层里（产品要求"服务端下载和游戏下载一个样，不要搞独立的新界面"）。
        // 降成普通控件嵌进去之后，它的 accepted / rejected 只当"这一层该干什么"的通知。
        m_newServerPage = new NewServerPage(formHost);
        new ServerVersionPicker(m_newServerPage, m_versionManager);
        m_newServerPage->setWindowFlags(Qt::Widget);
        m_newServerPage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        formHost->layout()->addWidget(m_newServerPage);
        m_newServerPage->show();
        connect(m_newServerPage, &NewServerPage::fieldsChanged, this,
                [this] { updateNewServerSummary(m_newServerPage); });
        connect(m_newServerPage, &QDialog::accepted, this,
                &MainWindow::createServerFromNewServerPage);
        connect(m_newServerPage, &QDialog::rejected, this, &MainWindow::leaveNewServerPage);
    }
    // 摆那八档（**每次开页都重喂**：页面是复用的）。哪几档要填哪几格由这一层翻好
    // （`ServerInstall` 那一层知道，页面不知道）。
    QList<NewServerPage::FlavorOption> options;
    for (const ServerInstall::Flavor flavor : ServerInstall::allFlavors()) {
        NewServerPage::FlavorOption option;
        option.token = ServerInstall::flavorToken(flavor);
        option.name = ServerInstall::flavorName(flavor);
        option.note = ServerInstall::flavorNote(flavor);
        // 哪几档要哪几格 —— 和 `ServerInstall::planFor()` 里"缺它会 ok=false"的那几条一字对应。
        switch (flavor) {
        case ServerInstall::Flavor::Forge:
        case ServerInstall::Flavor::NeoForge:
            option.needsLoader = true;
            break;
        case ServerInstall::Flavor::Fabric:
            option.needsLoader = true;
            option.needsInstaller = true;
            break;
        case ServerInstall::Flavor::Quilt:
            option.needsInstaller = true;
            break;
        case ServerInstall::Flavor::Paper:
        case ServerInstall::Flavor::Purpur:
            option.needsBuild = true;
            break;
        default:
            break;
        }
        option.needsLoaderGame = true;
        options.append(option);
    }
    m_newServerPage->setFlavors(options, ServerInstall::flavorToken(ServerInstall::Flavor::Vanilla));
    // 默认名与安装版本都来自刚选中的服务端版本。
    const QString currentVersion = selected;
    m_newServerPage->setDefaults(
        ServerRegistry::defaultServerName(ServerInstall::Flavor::Vanilla, currentVersion),
        currentVersion);
    // 返回行右边那句（和游戏安装页「将基于原版 26.3 安装」同一格）：版本已经在列表里选好了。
    m_ui->setServerFormIntro(currentVersion.isEmpty()
                                 ? tr("将安装一个新服务端")
                                 : tr("将安装 %1 服务端").arg(currentVersion));
    if (auto *version = m_newServerPage->findChild<QLineEdit *>(MainWindowUI::newServerGameVersionEditName()))
        version->setReadOnly(fromDownload && !selected.isEmpty());
    if (auto *install = m_newServerPage->findChild<QPushButton *>(MainWindowUI::newServerCreateButtonName()))
        install->setText(fromDownload ? tr("安装") : tr("创建"));
    updateNewServerSummary(m_newServerPage);
    // accept()/reject() 会把这一页藏起来（`QDialog` 的老规矩）—— 再开一次要能露出来。
    m_newServerPage->show();
    // 走「下载 → 服务端」同一条路（和「游戏」下载一致）：切到下载页、翻到「服务端」那一栏、
    // 停在第二层那张安装表单上。不再开覆盖层 —— 用户要的就是"别搞独立的新界面"。
    if (QTabWidget *tabs = m_ui->getTabWidget()) {
        const int tab = MainWindowUI::downloadTabIndex();
        if (tab >= 0 && tab < tabs->count()) {
            tabs->setCurrentIndex(tab);
        }
    }
    if (QListWidget *categories = m_ui->getDownloadCategoryList()) {
        if (categories->currentRow() != MainWindowUI::downloadCategoryServerIndex()) {
            const QScopedValueRollback<bool> suppress(m_suppressGameInstallServerJump, true);
            categories->setCurrentRow(MainWindowUI::downloadCategoryServerIndex());
        }
    }
    stack->setCurrentIndex(MainWindowUI::downloadServerFormIndex());
    if (auto *name = m_newServerPage->findChild<QLineEdit *>(MainWindowUI::newServerNameEditName())) {
        name->setFocus();
        name->selectAll();
    }
}

void MainWindow::cancelPendingServerVersionProbe() {
    ++m_serverProbeGeneration;
    const bool wasProbing = m_serverProbeRequest != nullptr;
    delete m_serverProbeRequest.data();
    m_serverProbeRequest = nullptr;
    if (m_ui != nullptr && m_ui->getServerFormHost() != nullptr) {
        LoadingState::finish(m_ui->getServerFormHost());
    }
    if (wasProbing && m_ui != nullptr && m_ui->getServerStack() != nullptr &&
        m_ui->getServerStack()->currentIndex() == MainWindowUI::downloadServerFormIndex()) {
        m_ui->getServerStack()->setCurrentIndex(MainWindowUI::downloadServerListIndex());
    }
}

void MainWindow::fallbackToServerVersionList(const QString &title, const QString &message,
                                             bool isWarning) {
    cancelPendingServerVersionProbe();
    if (m_ui == nullptr) {
        return;
    }
    if (QTabWidget *tabs = m_ui->getTabWidget()) {
        const int tab = MainWindowUI::downloadTabIndex();
        if (tab >= 0 && tab < tabs->count()) {
            tabs->setCurrentIndex(tab);
        }
    }
    if (QListWidget *categories = m_ui->getDownloadCategoryList()) {
        if (categories->currentRow() != MainWindowUI::downloadCategoryServerIndex()) {
            const QScopedValueRollback<bool> suppress(m_suppressGameInstallServerJump, true);
            categories->setCurrentRow(MainWindowUI::downloadCategoryServerIndex());
        }
    }
    if (QStackedWidget *stack = m_ui->getServerStack()) {
        stack->setCurrentIndex(MainWindowUI::downloadServerListIndex());
    }
    if (m_newServerPage != nullptr) {
        m_newServerPage->show();
    }
    appendLog(message);
    if (m_ui->overlay() != nullptr) {
        if (isWarning) {
            InlineMessage::warning(m_ui->overlay(), title, message);
        } else {
            InlineMessage::information(m_ui->overlay(), title, message);
        }
    }
}

void MainWindow::openServerPageForGameVersion(const QString &gameVersion) {
    if (m_ui == nullptr) {
        return;
    }
    const QString version = gameVersion.trimmed();
    QStackedWidget *stack = m_ui->getServerStack();
    QWidget *formHost = m_ui->getServerFormHost();
    if (version.isEmpty() || stack == nullptr || formHost == nullptr || formHost->layout() == nullptr) {
        return;
    }
    cancelPendingServerVersionProbe();

    // Already on the server form for this exact version: preserve user edits.
    if (m_newServerPage != nullptr &&
        stack->currentIndex() == MainWindowUI::downloadServerFormIndex() &&
        m_newServerPage->gameVersion() == version) {
        m_newServerPage->show();
        return;
    }

    const QJsonObject localJson = m_versionManager->getVersionJson(version);
    if (!localJson.isEmpty()) {
        if (ServerInstall::vanillaServerDownload(localJson).ok()) {
            m_serverAvailabilityCache.insert(version, true);
            showNewServerForm(version, true);
            return;
        }
        if (localJson.contains(QStringLiteral("downloads"))) {
            m_serverAvailabilityCache.insert(version, false);
            fallbackToServerVersionList(
                tr("暂无对应服务端"),
                tr("版本 %1 没有对应的服务端，已返回服务端版本列表。").arg(version));
            return;
        }
    }

    if (m_serverAvailabilityCache.contains(version)) {
        if (m_serverAvailabilityCache.value(version)) {
            showNewServerForm(version, true);
            return;
        }
        fallbackToServerVersionList(
            tr("暂无对应服务端"),
            tr("版本 %1 没有对应的服务端，已返回服务端版本列表。").arg(version));
        return;
    }

    QString officialUrl;
    if (DownloadManager *downloads = m_versionManager->downloads()) {
        for (const MinecraftVersion &entry : downloads->getAvailableVersions()) {
            if (entry.id == version) {
                officialUrl = entry.url;
                break;
            }
        }
    }
    if (officialUrl.isEmpty()) {
        fallbackToServerVersionList(
            tr("暂无对应服务端"),
            tr("版本 %1 没有对应的服务端，已返回服务端版本列表。").arg(version));
        return;
    }

    if (m_serverProbeNetwork == nullptr) {
        m_serverProbeNetwork = new QNetworkAccessManager(this);
    }
    const int generation = ++m_serverProbeGeneration;
    if (m_newServerPage != nullptr) {
        m_newServerPage->hide();
    }
    m_ui->setServerFormIntro(tr("将安装 %1 服务端").arg(version));
    stack->setCurrentIndex(MainWindowUI::downloadServerFormIndex());
    LoadingState::begin(formHost, tr("正在检查 %1 服务端…").arg(version));

    const QUrl parsedOfficial(officialUrl);
    const bool isLocalTest = parsedOfficial.host() == QLatin1String("127.0.0.1") ||
                             parsedOfficial.host() == QLatin1String("localhost");
    DownloadManager *downloads = m_versionManager->downloads();
    const bool useMirrorFirst =
        !isLocalTest && downloads != nullptr && downloads->mirrorPreferred();
    const QString firstUrl =
        useMirrorFirst ? DownloadManager::mirrorVersionJsonUrl(version) : officialUrl;
    const QString fallbackUrl = useMirrorFirst ? officialUrl : QString();
    startServerVersionProbe(version, firstUrl, fallbackUrl, generation);
}

void MainWindow::startServerVersionProbe(const QString &version, const QString &url,
                                         const QString &fallbackUrl, int generation) {
    if (m_serverProbeNetwork == nullptr || generation != m_serverProbeGeneration) {
        return;
    }
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_serverProbeRequest = RequestBudget::ControlGet::start(
        this, m_serverProbeNetwork, request, HttpClient::kDefaultTimeoutMs,
        [this, version, fallbackUrl, generation](QNetworkReply *reply, const QString &jobError) {
            if (generation != m_serverProbeGeneration) {
                if (reply != nullptr) {
                    reply->deleteLater();
                }
                return;
            }
            m_serverProbeRequest = nullptr;
            QByteArray payload;
            QString errorText = jobError;
            bool ok = false;
            if (reply != nullptr) {
                ok = reply->error() == QNetworkReply::NoError;
                if (ok) {
                    payload = reply->readAll();
                } else {
                    errorText = reply->errorString();
                }
                reply->deleteLater();
            }
            if (!ok) {
                if (!fallbackUrl.isEmpty()) {
                    startServerVersionProbe(version, fallbackUrl, QString(), generation);
                    return;
                }
                fallbackToServerVersionList(
                    tr("无法确认服务端"),
                    tr("未能获取 %1 的版本信息：%2").arg(version, errorText),
                    /*isWarning=*/true);
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                if (!fallbackUrl.isEmpty()) {
                    startServerVersionProbe(version, fallbackUrl, QString(), generation);
                    return;
                }
                fallbackToServerVersionList(
                    tr("无法确认服务端"),
                    tr("未能解析 %1 的版本信息。").arg(version),
                    /*isWarning=*/true);
                return;
            }
            const QJsonObject obj = doc.object();
            m_probedServerVersionJsons.insert(version, obj);
            const bool hasServer = ServerInstall::vanillaServerDownload(obj).ok();
            m_serverAvailabilityCache.insert(version, hasServer);
            if (!hasServer) {
                fallbackToServerVersionList(
                    tr("暂无对应服务端"),
                    tr("版本 %1 没有对应的服务端，已返回服务端版本列表。").arg(version));
                return;
            }
            LoadingState::finish(m_ui->getServerFormHost());
            showNewServerForm(version, true);
        });
}

void MainWindow::createServerFromNewServerPage() {
    if (m_ui == nullptr || m_newServerPage == nullptr) {
        return;
    }
    // 「创建」真按下去了 —— 建这个服务端：名字洗成目录名、重名加后缀、写 `.wtfml-server`。
    const QString desired = ServerRegistry::sanitizeId(m_newServerPage->serverName());
    if (desired.isEmpty()) {
        // 话说完就停在表单上：刚填的东西不能因为一句提醒就没收。
        InlineMessage::warning(m_ui->overlay(), QStringLiteral("这个名字不能当目录名"),
                               QStringLiteral("换一个名字再试（名字里不能只有标点或空白）。"));
        return;
    }
    const QString serverId = ServerRegistry::uniqueServerId(desired);
    ServerModel::Info info;
    info.name = m_newServerPage->serverName();
    ServerInstall::Flavor flavor = ServerInstall::Flavor::Vanilla;
    if (ServerInstall::flavorFromToken(m_newServerPage->flavorToken(), &flavor)) {
        info.flavor = flavor;
        info.flavorKnown = true;
    }
    info.gameVersion = m_newServerPage->gameVersion();
    info.loaderVersion = m_newServerPage->loaderVersion();
    info.installerVersion = m_newServerPage->installerVersion();
    info.buildId = m_newServerPage->buildId();
    info.memoryMb = m_newServerPage->memoryMb();
    QString problem;
    if (!ServerRegistry::createServer(serverId, info, &problem)) {
        InlineMessage::critical(m_ui->overlay(), QStringLiteral("建不了这个服务端"), problem);
        return;
    }
    // 建好了：表单那一层退回去（下次进来从版本列表开始），并切到「管理 → 服务端」——
    // 用户刚建的那一行就在那儿。
    leaveNewServerPage();
    setInstanceMode(MainWindowUI::InstanceMode::Server);
    if (QTabWidget *tabs = m_ui->getTabWidget()) {
        const int tab = MainWindowUI::instancesTabIndex();
        if (tab >= 0 && tab < tabs->count()) {
            tabs->setCurrentIndex(tab);
        }
    }
    // 「创建」= 建目录 + 记一份账 + **当趟就把东西下下来 / 跑安装器**（不是"建完让
    // 用户自己再点一下安装"）。排在切页之后：那一行先以「未安装」露出来，
    // 紧接着任务中心就动起来 —— 用户看到的是"这一行正在变成能用的"。
    ServerInstall::Request request;
    request.flavor = info.flavor;
    request.gameVersion = info.gameVersion;
    request.loaderVersion = info.loaderVersion;
    request.installerVersion = info.installerVersion;
    request.buildId = info.buildId;
    installServer(serverId, request);
}

void MainWindow::leaveNewServerPage() {
    cancelPendingServerVersionProbe();
    if (m_ui == nullptr) {
        return;
    }
    if (QStackedWidget *stack = m_ui->getServerStack()) {
        stack->setCurrentIndex(MainWindowUI::downloadServerListIndex());
    }
    // `accept()` / `reject()` 会把这一页藏起来；它就住在那层里，下次进来要还看得见。
    if (m_newServerPage != nullptr) {
        m_newServerPage->show();
    }
}

void MainWindow::installServer(const QString &serverId, const ServerInstall::Request &request) {
    if (m_serverInstall == nullptr || serverId.isEmpty()) {
        return;
    }
    // 先单任务聚合。下载那一层只有一份批次状态，同时开两批会让两边的收尾互相算错
    // —— 所以忙的时候说清楚，而不是让它排着队（排队还没做）。
    if (m_serverInstall->isRunning()) {
        InlineMessage::warning(m_ui->overlay(), tr("先等这一个装完"),
                               tr("同一时间只能安装一个服务端：下载共用同一条队列，两批同时运行"
                                  "会打乱彼此的进度。"));
        return;
    }
    // 这一档这一轮接没接。**判据的唯一出处是 `ServerInstallRun::flavorSupported()`** ——
    // 页面上那颗「创建」的禁用读的也是它（Spigot：列出但明说暂不支持）。
    if (!ServerInstallRun::flavorSupported(request.flavor)) {
        InlineMessage::warning(m_ui->overlay(), tr("暂不支持此选项"),
                               ServerInstallRun::unsupportedReason(request.flavor));
        return;
    }
    const ServerInstall::Plan plan = ServerInstall::planFor(request);
    if (!plan.ok) {
        // `planFor()` 给的 `problem` 就是给用户看的那句话（那一层承诺过），别再另编一句。
        InlineMessage::critical(m_ui->overlay(), tr("无法安装这个服务端"), plan.problem);
        return;
    }
    // 跑安装器的那三档（Forge / NeoForge / Quilt）才要 java。**在记账之前**问：
    // 问不到就什么都没排（不往任务中心里塞一项注定会失败的活）。
    QString java;
    if (!pickServerInstallerJava(request, &java)) {
        return;  // 已经弹过窗说清楚为什么了
    }

    const QStringList names = ServerInstallRun::stepNames(request, plan);
    const Tasks::TaskId id = beginTask(Tasks::TaskKind::Install,
                                       tr("安装服务端 %1").arg(serverId), serverId,
                                       taskStepsFromNames(names, 0));
    m_serverInstallTaskId = id;
    m_serverInstallStepCount = names.size();
    // NOTE: 百分比按"走到第几格"算（`stepsChanged` / `stepChanged` 那两条连线里往前推）。
    // 服务端安装的总量（一共要下多少字节）要等元数据取回来才知道，在那之前报任何
    // 百分数都是编的 —— 同 `Tasks::percentOf()` 那条"测不出就不显示"的规矩。
    m_activityCenter->tasks()->setProgress(id, 0, qMax(1, names.size()));

    // 起手先切到服务端那一页：用户刚点的就是那儿，而"装到哪个实例里"这件事在这一页上
    // （任务中心那一条也带 targetId，点「打开目标位置」就是它的目录）。
    setInstanceMode(MainWindowUI::InstanceMode::Server);
    m_serverInstall->start(request, serverId, java);
}

bool MainWindow::pickServerInstallerJava(const ServerInstall::Request &request, QString *out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    // 这一档跑不跑安装器 —— 判据是计划里有没有给工具参数（`installerJarName()` 的唯一出处
    // 就在那一层，这里不按"是哪一档"另判一遍）。
    const ServerInstall::Plan plan = ServerInstall::planFor(request);
    if (!plan.ok || ServerInstallRun::installerJarName(plan).isEmpty()) {
        return true;  // 不跑安装器 ⇒ 用不上 java（下下来就是服务端）
    }
    // 判据和装加载器那条路（`pickInstallerJava()`）**同一套**：版本 json 里的 `javaVersion`
    // 说了算；盘上没有这个版本的 json（客户端还没装）就不知道要求，此时原样用配的那一份
    // —— 不在这一步拦人（猜一个主版本比不猜更危险，同那边的理由）。
    const QString configuredJava = m_javaSettings->javaPath();
    QString java = configuredJava;
    int requiredJavaMajor = 0;
    const QJsonObject gameJson = m_versionManager->getVersionJson(request.gameVersion);
    if (!gameJson.isEmpty()) {
        requiredJavaMajor = gameJson[QStringLiteral("javaVersion")]
                                .toObject()[QStringLiteral("majorVersion")]
                                .toInt();
    }
    QString reason;
    if (!m_javaSettings->installerJavaFor(requiredJavaMajor, configuredJava, &java, &reason)) {
        appendLog(tr("装这个服务端要的 Java 拿不到：%1").arg(reason));
        InlineMessage::warning(m_ui->overlay(), tr("Java 版本不够"), reason);
        return false;
    }
    if (java != configuredJava) {
        appendLog(tr("这个版本要 Java %1，装的时候改用 %2").arg(requiredJavaMajor).arg(java));
    }
    *out = java;
    return true;
}

// ======================================================================
// 服务端的「跑起来 / 停下来 / 控制台」（操作、停止时序）
// ======================================================================
//
// 这一片里**没有一条判据**：那条命令怎么拼、起来没有、退了算哪一档状态、宽限多久才允许
// 强杀，全在 `ServerRun.hpp`（纯头，verify 的 ZZ 一节钉死）；进程本身在 `ServerProcess`
// （mw_check 的 T41 拿本工程自己的 exe 当"服务端"跑整趟）。这一片是**接线** ——
// "谁在什么时候按了什么 → 调哪一个"，以及"界面上那颗按钮现在该写什么字"。

void MainWindow::activateServerRow(const QString &serverId) {
    if (serverId.isEmpty()) {
        return;
    }
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    if (!entry.installed) {
        repairServer(serverId);  // 未装好 ⇒ 第三格就是「修复」
        return;
    }
    const ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
    if (process != nullptr && process->status() == ServerModel::Status::Stopping) {
        return;  // 停止中：第三格是灰的，等它自己退干净
    }
    if (process != nullptr && process->isRunning()) {
        m_serverRun->stop(serverId);
        return;
    }
    m_serverRun->start(serverId);
}

void MainWindow::openServerConsole(const QString &serverId) {
    // 兼容入口：服务端菜单和 ConnectionsController 按这个名字调用。
    m_serverConsole->open(serverId);
}

void MainWindow::updateServerConsole() {
    // 状态那几行的唯一出处现在在控制器里（见 ServerConsoleController::refresh()）。
    m_serverConsole->refresh();
}

void MainWindow::openServerFolder(const QString &serverId) {
    if (serverId.isEmpty()) {
        return;
    }
    const QString dir = ServerRegistry::describe(serverId).dirPath;
    appendLog(tr("正在打开 %1。").arg(QDir::toNativeSeparators(dir)));
    // 走 QDesktopServices 而不是自己 QProcess 起 explorer.exe（和"打开实例文件夹"同一条路）。
    // 开不了要说一句：静静地什么都不发生，用户只会一遍遍点它。
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        InlineMessage::warning(m_ui->overlay(), tr("无法打开目录"),
                               tr("无法打开目录：\n%1").arg(QDir::toNativeSeparators(dir)));
    }
}

void MainWindow::repairServer(const QString &serverId) {
    if (serverId.isEmpty()) {
        return;
    }
    const ServerModel::Info info = ServerRegistry::readMeta(serverId);
    if (info.isEmpty()) {
        // 从外面拖进来的服务端就是这样：盘上有，但那一份"我们记的"不在 ⇒ 不知道它该按
        // 哪一档重装。这句话要给出下一步，不能只说"修不了"。
        InlineMessage::warning(
            m_ui->overlay(), tr("无法重新安装此实例"),
            tr("实例缺少安装记录，无法确定需补充的启动文件。请使用原启动器补齐文件，或重新安装实例。"));
        return;
    }
    ServerInstall::Request request;
    request.flavor = info.flavor;
    request.gameVersion = info.gameVersion;
    request.loaderVersion = info.loaderVersion;
    request.installerVersion = info.installerVersion;
    request.buildId = info.buildId;
    installServer(serverId, request);
}

void MainWindow::removeServer(const QString &serverId) {
    if (serverId.isEmpty()) {
        return;
    }
    const ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
    if (process != nullptr && process->isRunning()) {
        // 跑着的进程还开着这个目录里的文件（世界那几份是它自己在写），先让它停下。
        InlineMessage::warning(m_ui->overlay(), tr("服务端正在运行"),
                               tr("服务端正在运行，请停止运行后再删除。"));
        return;
    }
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    const QString shown = entry.name.isEmpty() ? serverId : entry.name;
    // 世界正在备份 / 还原：那一趟还在往这个目录里读 / 写，删掉它等于把正在解压的落点
    // 从底下抽走（备份那一趟则会把"删到一半的目录"打进包里）。
    if (m_serverRun->fileBusy(serverId)) {
        InlineMessage::warning(m_ui->overlay(), tr("暂时不能删除"),
                               tr("「%1」的世界正在备份或还原，请完成后再删除。").arg(shown));
        return;
    }
    // 这是**唯一一处会删用户东西的地方**，而且删的是别人几个月的存档 —— 所以：默认那
    // 一边是「取消」（`defaultYes = false`，回车不会把它删掉）、那颗按钮按危险档染红。
    const bool go = InlineMessage::question(
        m_ui->overlay(), tr("删除服务端"),
        tr("将删除服务端「%1」（含存档、模组与配置）。文件将移至回收站。\n\n确定删除？")
            .arg(shown),
        tr("删除"), tr("取消"), false, true);
    if (!go) {
        return;
    }
    QString problem;
    if (!ServerRegistry::removeServer(serverId, &problem)) {
        InlineMessage::critical(m_ui->overlay(), tr("删除失败"), problem);
        return;
    }
    // 进程对象一起收掉：它现在没在跑，但表里留着的话，下次 `describe()` 同名的那个会
    // 拿到上一辈子的状态（而且行都没了，它再发信号就是对着空气说）。
    m_serverWorkspace->removeProcess(serverId);
    m_serverConsole->forget(serverId);
    appendLog(tr("已删除服务端 %1。").arg(serverId));
    refreshServerList();
}

// ===========================================================================
// 「服务端设置」那一页 —— 这里只留两条薄壳
// ===========================================================================
//
// 这一页的**全部状态**（页对象 / 现在说的是哪个服务端 / 能不能改 / 端口那一次的结论）
// 和决定它的那几条（开页、重摆整页、保存、放弃改动、切页合编辑、体检）都搬进了
// `src/controllers/ServerSettingsPageController` —— 为什么搬、搬走了什么，见那个 .hpp。
//
// 主窗口这边只留两个名字，因为还有三处按名字叫它们：
//
// ① `openServerSettingsPage()`：服务端列表的右键菜单（`ServerMenuController::run()`），
//    以及 mw_check 的 T42（**经元对象系统**驱动 —— 所以它必须继续是 `private slots:`）；
// ② `refreshServerSettingsPage()`：备份收尾那一发（`BackupController` 的宿主回调），
//    以及 `ServerManagementActions` 里那五处"改完盘上那份、把页面重摆一遍"。
//
// NOTE: 其余的（保存 / 放弃 / 合编辑 / 保存带 / 体检）原来也只有这一页自己的信号在叫，
// 搬走之后一个外人都没有，所以**没有留壳** —— 留一个没人叫的空壳只会让人以为"主窗口还管
// 这一页"。

void MainWindow::openServerSettingsPage(const QString &serverId) {
    m_serverSettings->open(serverId);
}

void MainWindow::refreshServerSettingsPage() {
    m_serverSettings->refresh();
}


// ---------------------------------------------------------------------------
// 玩家名单（「玩家管理」）
// ---------------------------------------------------------------------------

void MainWindow::refreshServerPlayers() {
    ServerManagementActions::refreshServerPlayers(this);
}

void MainWindow::addServerPlayer() {
    ServerManagementActions::addServerPlayer(this);
}

void MainWindow::removeServerPlayer(const QString &identity) {
    ServerManagementActions::removeServerPlayer(this, identity);
}

void MainWindow::pickServerResourcePack() {
    ServerManagementActions::pickServerResourcePack(this);
}

QString MainWindow::serverContentFolder() const {
    return ServerManagementActions::serverContentFolder(this);
}

void MainWindow::refreshServerContent() {
    ServerManagementActions::refreshServerContent(this);
}

void MainWindow::addServerContent() {
    ServerManagementActions::addServerContent(this);
}

void MainWindow::toggleServerContent(const QString &name, bool enable) {
    ServerManagementActions::toggleServerContent(this, name, enable);
}

void MainWindow::openServerContentFolder() {
    ServerManagementActions::openServerContentFolder(this);
}

void MainWindow::kickServerPlayer(const QString &name) {
    ServerManagementActions::kickServerPlayer(this, name);
}

void MainWindow::runServerCheckAction(const QString &token) {
    ServerManagementActions::runServerCheckAction(this, token);
}

QString MainWindow::serverWorldName(const QString &serverId) const {
    return ServerManagementActions::serverWorldName(this, serverId);
}

QString MainWindow::serverWorldDir(const QString &serverId) const {
    return ServerManagementActions::serverWorldDir(this, serverId);
}

int MainWindow::serverWorldBackupCount(const QString &serverId) const {
    return ServerManagementActions::serverWorldBackupCount(this, serverId);
}

void MainWindow::backupServerWorld(const QString &serverId) {
    ServerManagementActions::backupServerWorld(this, serverId);
}

void MainWindow::restoreServerWorldBackup(const QString &serverId, const QString &zipPath) {
    ServerManagementActions::restoreServerWorldBackup(this, serverId, zipPath);
}

void MainWindow::deleteServerBackup(const QString &zipPath) {
    ServerManagementActions::deleteServerBackup(this, zipPath);
}

bool MainWindow::ensureServerEula(const QString &serverId) {
    // 已经写着接受 = 直接放行（`ServerRegistry::readEula()` 是判据的唯一出处，它自己也
    // 就是 `ServerModel::eulaAccepted()` 的落点）。
    if (ServerRegistry::readEula(serverId)) {
        return true;
    }
    OverlayHost *overlay = m_ui == nullptr ? nullptr : m_ui->overlay();
    if (overlay == nullptr) {
        return false;
    }
    // 默认那一颗给 **-1**：这一格上一颗都不许被回车打中 —— "把同意两个字打在屏幕上"
    // 这件事不能被一次误按回车办了。
    InlineMessageBox box(MessageKind::Question, ServerRun::eulaPromptTitle(),
                         ServerRun::eulaPromptText(),
                         QStringList{tr("同意并启动"), tr("取消")}, -1, -1);
    // 勾选框和「同意并启动」绑在一起：没勾之前那一颗是灰的（见 setCheckBox() 那段）。
    box.setCheckBox(ServerRun::eulaCheckBoxText(), 0);
    box.setLink(ServerRun::eulaLinkText(), ServerRun::eulaUrl());
    // 那一行链接被点了 ⇒ **由这一层开**（和"打开实例文件夹"同一条路：要写日志、开不了
    // 要说一句）。那一页自己不开浏览器（见 setLink() 那段）。
    connect(&box, &InlineMessageBox::linkActivated, this, [this](const QString &url) {
        if (url.isEmpty()) {
            return;
        }
        appendLog(tr("正在打开 %1。").arg(url));
        if (!QDesktopServices::openUrl(QUrl(url))) {
            InlineMessage::warning(m_ui->overlay(), tr("无法打开默认浏览器"),
                                   tr("无法打开此网址：\n%1").arg(url));
        }
    });
    overlay->execPrompt(&box, false);
    // 两道都要过：点了「同意并启动」**并且**那个框是勾上的。只按前者的话，那个勾就成了
    // 摆设（而那正是"只有你自己摁出来"这句话唯一的技术落点）。
    if (box.clickedIndex() != 0 || !box.checkBoxChecked()) {
        appendLog(tr("未接受 EULA，%1 未启动。").arg(serverId));
        return false;
    }
    QString problem;
    if (!ServerRegistry::acceptEula(serverId, &problem)) {
        InlineMessage::critical(m_ui->overlay(), tr("写不了 eula.txt"), problem);
        return false;
    }
    appendLog(tr("%1：已接受 EULA，已将 eula=true 写入 eula.txt。").arg(serverId));
    return true;
}

ServerInstall::Plan MainWindow::serverLaunchPlan(const QString &serverId, QString *problem) const {
    return ServerWorkspaceController::launchPlan(serverId, problem);
}

void MainWindow::updateServerRow(const QString &serverId) {
    QTreeWidget *tree = m_ui == nullptr ? nullptr : m_ui->getServerList();
    if (tree == nullptr || serverId.isEmpty()) {
        return;
    }
    QTreeWidgetItem *item = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *candidate = tree->topLevelItem(i);
        if (candidate != nullptr
            && candidate->data(0, Qt::UserRole).toString() == serverId) {
            item = candidate;
            break;
        }
    }
    if (item == nullptr) {
        return;  // 这一行已经不在列表里了（删掉了 / 还没重扫出来）
    }
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    const ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
    const ServerModel::Status status =
        process != nullptr
            ? process->status()
            : (entry.installed ? ServerModel::Status::Ready : ServerModel::Status::NotInstalled);

    // 和客户端同一种行（ListRowDelegate）：第一行自定义名，第二行「Minecraft 版本 · 端类型 · 状态」。
    QStringList facts;
    if (!entry.gameVersion.isEmpty()) {
        facts << tr("Minecraft %1").arg(entry.gameVersion);
    }
    facts << ServerInstall::flavorName(entry.flavor) << ServerModel::statusName(status);
    item->setText(0, entry.name);
    item->setData(0, ListRowDelegate::SubtitleRole, facts.join(QStringLiteral(" · ")));
    QString tip = entry.note;
    if (!tip.isEmpty()) {
        tip += QLatin1Char('\n');
    }
    tip += QDir::toNativeSeparators(entry.dirPath);
    item->setToolTip(0, tip);

    // 第三格（悬停才露）一行只有一个，按状态换身份：修复 / 停止 / 启动；其余项在「更多」与右键里。
    ListRowDelegate::LaunchKind kind = ListRowDelegate::LaunchKind::Start;
    QString text = MainWindowUI::serverStartText();
    QString launchTip = tr("启动这个服务端");
    if (!entry.installed) {
        kind = ListRowDelegate::LaunchKind::Repair;
        text = MainWindowUI::serverRepairText();
        launchTip = tr("实例尚未安装完成。请选择补装以下载缺失文件。");
    } else if (process != nullptr && process->isRunning()) {
        const bool stopping = status == ServerModel::Status::Stopping;
        kind = stopping ? ListRowDelegate::LaunchKind::Busy : ListRowDelegate::LaunchKind::Stop;
        text = stopping ? MainWindowUI::serverStoppingText() : MainWindowUI::serverStopText();
        launchTip = stopping ? tr("已发送停止请求，正在等待保存并退出")
                             : tr("保存后自行退出（推荐）");
    }
    item->setData(0, ListRowDelegate::LaunchKindRole, static_cast<int>(kind));
    item->setData(0, ListRowDelegate::LaunchTextRole, text);
    item->setData(0, ListRowDelegate::ActionTipRole,
                  QStringList{tr("打开服务端目录"), tr("更多操作"), launchTip});
}

void MainWindow::onServerStatusChanged(const QString &serverId, ServerModel::Status status) {
    if (status == ServerModel::Status::Running) {
        m_serverRun->onReady(serverId);
    }
    updateServerRow(serverId);
    updateServerConsole();
    // 设置页正开着这一个服务端：顶上那两句状态和整页只读闸要同一次跟着翻。**只翻这一套**
    // —— 整页重摆会把用户还没保存的改动冲掉（见 `refreshRunStateIfShowing()`）。
    m_serverSettings->refreshRunStateIfShowing(serverId);
    // 底栏左下角那一格是**全局**的：服务端起/停也要让那里从「暂无实例或服务端」翻成
    // 「服务端运行中」（见 `updateBottomBarRunState()` 里那段）。这一发很轻（只看会话表 +
    // 服务端那几张表），按状态变一次来一发没关系。
    updateBottomBarRunState();
    appendLog(tr("%1：%2").arg(serverId, ServerModel::statusName(status)));
}

void MainWindow::onServerLineAppended(const QString &serverId, const QString &line) {
    // 启动器日志里留一份（排查"它怎么起不来"时我们要的就是它自己的原话）。
    // 只有长到离谱的那些才截断，而且**只截写进日志的这一份** —— 控制台那一页照旧是原话。
    QString logged = line;
    if (logged.size() > kMaxLoggedServerLineChars) {
        logged = logged.left(kMaxLoggedServerLineChars) + QStringLiteral("…（这一行太长，截断了）");
    }
    appendLog(QStringLiteral("[%1] %2").arg(serverId, logged));
    // 控制台那一页正开着它的话，这一行也进那一页（认人认"这一页现在说的是谁"，见控制器）。
    m_serverConsole->appendLine(serverId, line);
}

void MainWindow::onServerForceKillAvailable(const QString &serverId) {
    const ServerRun::ServerProcess *process = m_serverWorkspace->process(serverId);
    const int seconds = (process != nullptr ? process->gracefulMs() : 0) / 1000;
    appendLog(tr("%1：%2").arg(serverId, ServerRun::forceKillNote(seconds)));
    // 正开着它那一页的话，往控制台里也插一行 + 把状态重摆一遍（那颗按钮为什么突然亮了
    // 得有个说法）—— 页不在就不做，判据在控制器里。
    m_serverConsole->announceForceKill(serverId, seconds);
    updateServerRow(serverId);
    updateBottomBarRunState();
}

int MainWindow::countServerMods(const QString &dirPath) {
    return ServerWorkspaceController::countEnabledMods(dirPath);
}

qint64 MainWindow::serverWorldBytes(const QString &dirPath) {
    return ServerWorkspaceController::defaultWorldBytes(dirPath);
}

void MainWindow::updateNewServerSummary(NewServerPage *page) {
    if (page == nullptr) {
        return;
    }
    // 名字先过一遍（洗空了就没得建）。
    const QString desiredId = ServerRegistry::sanitizeId(page->serverName());
    if (desiredId.isEmpty()) {
        page->setTargetDir(QString());
        page->setSummary(QString(), QStringLiteral("先给它起个名字（会当成目录名）。"));
        return;
    }
    // 「目录」那一行：服务端根目录只有一份（`<数据目录>/servers`，就是左栏那一列里的根），
    // 这一格是**算出来的** —— 走的和按下「创建」之后真建的那一个**同一条路**（sanitize →
    // unique），所以这一行写的地址和它最后落在盘上的那一个是同一个。
    page->setTargetDir(QDir::toNativeSeparators(
        AppPaths::serverDir(ServerRegistry::uniqueServerId(desiredId))));

    // 再让 `ServerInstall` 判这一份填法成不成立（缺哪一格它会说人话）。
    ServerInstall::Request request;
    ServerInstall::Flavor flavor = ServerInstall::Flavor::Vanilla;
    if (ServerInstall::flavorFromToken(page->flavorToken(), &flavor)) {
        request.flavor = flavor;
    }
    // Spigot：这一轮没接（官方不发成品，要用它的 BuildTools 现编，那一步要本机有 git）。
    // **那一档故意留在下拉框里**（用户得知道有这一档），选中它时明说原因，并把
    // 「创建」收起来 —— 不留一颗按下去什么都不发生的按钮。
    // 判据读的是 `ServerInstallRun::flavorSupported()` 这一个出处。
    if (!ServerInstallRun::flavorSupported(request.flavor)) {
        page->setSummary(QString(), ServerInstallRun::unsupportedReason(request.flavor));
        return;
    }
    request.gameVersion = page->gameVersion();
    request.loaderVersion = page->loaderVersion();
    request.installerVersion = page->installerVersion();
    request.buildId = page->buildId();
    const ServerInstall::Plan plan = ServerInstall::planFor(request);
    if (!plan.ok) {
        // `planFor` 给的 `problem` 就是给用户看的那句话（那一层承诺过）。
        page->setSummary(QString(), plan.problem);
        return;
    }
    // 成立了：**不再摆"这一档要下什么、按下去会发生什么"那一段说明** —— 用户点名
    // 那是废话（下载地址联不联网、jar 放哪、进度在任务中心，界面上不必先讲一遍）。
    // 这里只把上一档留下的问题句清掉；真正拦人的那几句（填法不成立 / 这一档不支持）
    // 由上面那几处 `setSummary(…, problem)` 露出来。
    page->setSummary(QString(), QString());
}

