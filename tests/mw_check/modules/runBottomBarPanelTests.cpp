#include "Support.hpp"
#include "auth/SkinApi.hpp"
#include "ActivityPopover.hpp"
#include "ServerConsolePage.hpp"
#include "ServerSettingsPage.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "manage/InstanceMetaFile.hpp"
#include "server/ServerProcess.hpp"
#include "TitleBar.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/TaskDetailController.hpp"

#include <QAbstractButton>
#include <QKeyEvent>
#include <QLineEdit>
#include <QShortcut>
#include <QTabWidget>

void runBottomBarPanelTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "AH0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    // 数据根目录**两头都要改**（理由见 ZG0 那段：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次）。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));

    // 两个实例：一个正式版、一个远古版。**分类从 meta 的 type 读**（产品那条路也是，
    // 见 MainWindow.cpp 里 versionChannelIndexOf()），所以这里按它写。
    const QString releaseId = QStringLiteral("1.20.1");
    const QString oldId = QStringLiteral("a1.2.6");
    makeInstalledVersion(versionsDir, releaseId);
    makeInstalledVersion(versionsDir, oldId);
    makeVersionMeta(versionsDir, releaseId, QByteArrayLiteral("{\"type\":\"release\"}"));
    makeVersionMeta(versionsDir, oldId, QByteArrayLiteral("{\"type\":\"old_alpha\"}"));

    // ---- AH0 五个账号：只有最后三个该出现在右面板里 ----
    // 列表整份由这一节自己写（**只有 saved_at 是手挑的**：产品的写方用的是秒级的
    // QDateTime::currentDateTimeUtc()，同一个 run 里连着写几个会撞在同一秒上，那样
    // "最近三个"这件事就没法验了）。Bob 那一项走产品的写方（putCredentials +
    // captureActive），拿的是**真的两份凭据副本** —— 后面点他那一行的「登录」要真的
    // 切过去，副本是假的就切不动。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());

    const QString aliceUuid = QStringLiteral("11111111111111111111111111111111");
    const QString carolUuid = QStringLiteral("22222222222222222222222222222222");
    const QString bobUuid = QStringLiteral("33333333333333333333333333333333");
    // Dave：列表里那种**不完整**的项（只有名字，没有两份凭据副本）—— AH11 拿它试"切不过去"。
    const QString daveUuid = QStringLiteral("44444444444444444444444444444444");
    const QString daveName = QStringLiteral("Dave");
    const QString alexName = QStringLiteral("Alex");
    const QString steveName = QStringLiteral("Steve");
    const QString alexUuid = AccountStore::offlineUuidFor(alexName);
    const QString steveUuid = AccountStore::offlineUuidFor(steveName);

    // 两个到期时刻都**按"现在"算**（不写死日期）：写死的话，"剩得多"和"剩不到 6 小时"
    // 这两档就跟着测试机器上的时钟走 —— 哪天（比如跑在 2027 年）它们会自己换个位置，
    // 于是"该刷新的没刷新""不该刷新的却发了个包"这种假红/假绿就冒出来了。
    const QString longExpiry = QDateTime::currentDateTimeUtc().addDays(30).toString(Qt::ISODate);
    const QString shortExpiry = QDateTime::currentDateTimeUtc().addSecs(60 * 60).toString(Qt::ISODate);

    {
        QJsonObject microsoft;
        microsoft.insert(QStringLiteral("access_token"), QStringLiteral("ms-at-Bob"));
        microsoft.insert(QStringLiteral("refresh_token"), QStringLiteral("rt-bob"));
        QJsonObject minecraft;
        minecraft.insert(QStringLiteral("access_token"), QStringLiteral("at-Bob"));
        // 还有 30 天：令牌**够用**（剩 ≥6 小时那一档，点「登录」不该去换新的）。
        minecraft.insert(QStringLiteral("expires_at"), longExpiry);
        minecraft.insert(QStringLiteral("name"), QStringLiteral("Bob"));
        minecraft.insert(QStringLiteral("uuid"), bobUuid);
        check(TokenStore::writeJson(TokenStore::microsoftKey(), microsoft) &&
                  TokenStore::writeJson(TokenStore::minecraftKey(), minecraft),
              "AH0 Bob 的两份凭据按产品那套格式写进活动槽了");
        check(AccountStore::captureActive(),
              "AH0 收进账号列表（列表里那一项带着真的可用副本）");
    }

    // 一份"离线账号"的列表项（没有凭据，只有一个名字和那个推出来的 uuid）。
    const auto offlineEntry = [](const QString &uuid, const QString &name,
                                 const QString &savedAt) {
        QJsonObject item;
        item.insert(QStringLiteral("uuid"), uuid);
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("saved_at"), savedAt);
        item.insert(QStringLiteral("offline"), true);
        return item;
    };
    // 一份"正版账号"的列表项。@param refreshToken 空 = 微软那份里**没有** refresh_token
    // （= 换不出新令牌那一档，见 AuthManager::refreshMinecraftToken() 第一道判据）。
    const auto onlineEntry = [](const QString &uuid, const QString &name, const QString &savedAt,
                                const QString &refreshToken, const QString &minecraftExpiry) {
        QJsonObject microsoft;
        microsoft.insert(QStringLiteral("access_token"), QStringLiteral("ms-at-") + name);
        if (!refreshToken.isEmpty()) {
            microsoft.insert(QStringLiteral("refresh_token"), refreshToken);
        }
        QJsonObject minecraft;
        minecraft.insert(QStringLiteral("access_token"), QStringLiteral("at-") + name);
        minecraft.insert(QStringLiteral("expires_at"), minecraftExpiry);
        minecraft.insert(QStringLiteral("name"), name);
        minecraft.insert(QStringLiteral("uuid"), uuid);
        QJsonObject item;
        item.insert(QStringLiteral("uuid"), uuid);
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("saved_at"), savedAt);
        item.insert(QStringLiteral("microsoft"), microsoft);
        item.insert(QStringLiteral("minecraft"), minecraft);
        return item;
    };

    // 先让 Alex 成为**当前账号**（产品那条路：addOffline 加完顺手切过去）。此刻列表里
    // 只有 Bob 和 Alex，而 Alex 没有凭据 —— 底栏第二行该写「离线登录」。
    QString offlineError;
    check(AccountStore::addOffline(alexName, &offlineError) &&
              AccountStore::activeUuid() == alexUuid,
          "AH0 加一个离线账号 Alex 并切过去（当前身份 = 离线账号）", offlineError);

    // 再按上面那套时间把整份列表重写一遍。**顺序是刻意的**：saved_at 最新的是 Bob
    // （手写的 ，故意不用"现在"——这样"最近三个"这条判据跟测试机器上的时钟
    // 一点关系都没有），然后是 Carol / Alex / Steve / Alice。于是右面板该出的是
    // Bob、Carol、Alex 这三行，Alice 和 Steve 不出现。
    {
        QJsonArray array;
        array.append(offlineEntry(alexUuid, alexName, QStringLiteral("2026-09-20T00:00:00Z")));
        array.append(offlineEntry(steveUuid, steveName, QStringLiteral("2026-09-10T00:00:00Z")));
        array.append(onlineEntry(aliceUuid, QStringLiteral("Alice"),
                                 QStringLiteral("2026-09-01T00:00:00Z"),
                                 QStringLiteral("rt-alice"),
                                 QStringLiteral("2999-01-01T00:00:00Z")));
        // Carol：微软那份只有 access_token（换不出新令牌），而 Minecraft 那张只剩一小时
        // —— 正好的"剩不到 6 小时"那一档，而刷新会在第一道判据上停下（不发请求）。
        array.append(onlineEntry(carolUuid, QStringLiteral("Carol"),
                                 QStringLiteral("2026-09-23T00:00:00Z"), QString(),
                                 shortExpiry));
        QJsonArray all = TokenStore::readJson(TokenStore::accountsKey())
                             .value(QStringLiteral("accounts"))
                             .toArray();
        QJsonObject bobEntry;
        for (const QJsonValue &value : all) {
            if (value.toObject().value(QStringLiteral("uuid")).toString() == bobUuid) {
                bobEntry = value.toObject();
            }
        }
        check(!bobEntry.isEmpty(),
              "AH0 Bob 那一项找得回来（下面这几个账号要跟它摆在同一个列表里）");
        bobEntry.insert(QStringLiteral("saved_at"), QStringLiteral("2026-12-31T00:00:00Z"));
        array.append(bobEntry);

        QJsonObject rootObject;
        rootObject.insert(QStringLiteral("accounts"), array);
        check(TokenStore::writeJson(TokenStore::accountsKey(), rootObject),
              "AH0 五个账号按挑好的时间写进列表");
        check(AccountStore::accounts().size() == 5 &&
                  AccountStore::activeUuid() == alexUuid,
              "AH0 列表里五个账号，当前还是 Alex（重写列表没把身份搅掉）",
              QStringLiteral("%1 个 / 当前 %2")
                  .arg(AccountStore::accounts().size())
                  .arg(AccountStore::activeUuid()));
    }

    MainWindow w;
    w.show();
    QApplication::processEvents();
    QPushButton *instancesButton =
        byObjectName<QPushButton>(&w, MainWindowUI::runStateButtonName());
    QPushButton *accountsButton =
        byObjectName<QPushButton>(&w, MainWindowUI::accountSkinButtonName());
    LauncherManager *manager = w.findChild<LauncherManager *>();
    AuthManager *auth = w.findChild<AuthManager *>();
    check(instancesButton != nullptr && accountsButton != nullptr && manager != nullptr &&
              auth != nullptr,
          "AH0 底栏那颗运行状态按钮、主页右上那颗账号胶囊、LauncherManager、AuthManager 都找得到");

    // 面板每次点开都是**新的一个**（关掉就 deleteLater），所以每开一次都重新捞一遍。
    //
    // NOTE: 开之前先把已经开着的那块收掉。那两颗按钮是**开关**（点同一颗 = 收回，见
    // MainWindowUI 里 togglePanel 那段），开着的时候再点一下是"只收不开" ——
    // 不收掉就点，捞回来的是那块正在被删的旧面板（findChild 在本轮里还找得到它，
    // 下一轮就是空的），断言会红在一个和产品无关的地方。
    // 账号那一边是**账号弹层**（主页右上那颗账号胶囊弹出来的 AccountPopover），左边那一块
    // 还是底栏面板；下面几个 helper 两种都认（按 objectName 分）。
    const auto isAccountPopover = [](QWidget *panel) {
        return panel != nullptr && panel->objectName() == MainWindowUI::accountPopoverName();
    };
    const auto openPanel = [&](QPushButton *button) -> QWidget * {
        for (const QString &name : {MainWindowUI::bottomBarPanelName(), MainWindowUI::accountPopoverName()}) {
            if (QWidget *open = w.findChild<QWidget *>(name); open != nullptr && open->isVisible()) {
                open->close();
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
        w.raise();
        w.activateWindow();
        QApplication::processEvents();
        button->click();
        QApplication::processEvents();
        return w.findChild<QWidget *>(button == accountsButton ? MainWindowUI::accountPopoverName()
                                                               : MainWindowUI::bottomBarPanelName());
    };
    const auto rowTitle = [&isAccountPopover](QWidget *panel, int index) {
        QLabel *label = panel == nullptr
                            ? nullptr
                            : panel->findChild<QLabel *>(
                                  isAccountPopover(panel)
                                      ? QStringLiteral("accountPopoverRowTitle%1").arg(index)
                                      : MainWindowUI::bottomBarPanelRowTitleName(index));
        return label == nullptr ? QString() : label->text();
    };
    const auto rowSubtitle = [&isAccountPopover](QWidget *panel, int index) {
        QLabel *label = panel == nullptr
                            ? nullptr
                            : panel->findChild<QLabel *>(
                                  isAccountPopover(panel)
                                      ? QStringLiteral("accountPopoverRowSub%1").arg(index)
                                      : MainWindowUI::bottomBarPanelRowSubtitleName(index));
        return label == nullptr ? QString() : label->text();
    };
    const auto rowIcon = [&isAccountPopover](QWidget *panel, int index) -> QPixmap {
        if (isAccountPopover(panel)) {
            // 弹层里画的是圆角过的那张；比"是不是同一张脸"看圆角前那张（wtfmlFaceSource）。
            QLabel *face = panel->findChild<QLabel *>(QStringLiteral("accountPopoverRowFace%1").arg(index));
            return face == nullptr ? QPixmap() : face->property("wtfmlFaceSource").value<QPixmap>();
        }
        QLabel *label = panel == nullptr
                            ? nullptr
                            : panel->findChild<QLabel *>(
                                  MainWindowUI::bottomBarPanelRowIconName(index));
        return label == nullptr ? QPixmap() : label->pixmap();
    };
    // 账号弹层里第几行那颗（整行就是一颗按钮）。
    const auto accountRow = [](QWidget *panel, int index) -> QAbstractButton * {
        return panel == nullptr ? nullptr
                                : panel->findChild<QAbstractButton *>(
                                      QStringLiteral("accountPopoverRow%1").arg(index));
    };
    // 当前账号那一行打勾（accountPopoverCheck 挂在那一行里）。
    const auto rowChecked = [&accountRow](QWidget *panel, int index) {
        QAbstractButton *row = accountRow(panel, index);
        return row != nullptr && row->findChild<QWidget *>(QStringLiteral("accountPopoverCheck")) != nullptr;
    };
    // 点账号弹层的一行：弹层先收，动作排在下一轮（见 AccountPopover::connectAction）。
    const auto clickRow = [](QAbstractButton *row) {
        if (row == nullptr) return;
        row->click();
        QApplication::processEvents();
        QApplication::processEvents();
    };
    const auto rowAction = [](QWidget *panel, int index, int action) -> QPushButton * {
        return panel == nullptr
                   ? nullptr
                   : panel->findChild<QPushButton *>(
                         MainWindowUI::bottomBarPanelRowActionName(index, action));
    };
    // 按行标题找那一行（找不到 -1）。
    //
    // NOTE: 从 AH11 起，"第 0 行是谁"就不再由 AH7 那套 fixture 决定了 —— AH11 刻意把账号
    // 列表**整个换掉**（它要造一个"副本不完整"的项，那一项还得排在前面）。写死下标的
    // 那条断言就是这么红的：点的是"第 0 行"，落到了 Dave 身上，报出来却像"登出没生效"。
    // 凡是要点某一个具体账号的行，一律先按名字找行号。
    const auto rowIndexOf = [&rowTitle](QWidget *panel, const QString &title) {
        for (int row = 0; row < 16; ++row) {
            if (rowTitle(panel, row) == title) {
                return row;
            }
        }
        return -1;
    };
    const auto stopAllSessions = [&manager] {
        for (const LauncherManager::GameSession &game : manager->runningGames()) {
            manager->stopGame(game.sessionId);
        }
    };

    // 轨底运行状态入口（issue #85）：count 个进程在跑时，图标轨和展开轨各量一遍。
    // 量三件事：入口可见且计数对；点开的面板贴着入口、行数 = 计数；两种轨之间入口不跳位。
    // 窗口先拉到 1280（窄于 1024 时轨被强制收起，展不开），量完还原。
    int runEntryTop[2] = {0, 0};
    int runEntryHeight[2] = {0, 0};
    const Qt::FocusPolicy runEntryFocusPolicy =
        instancesButton != nullptr ? instancesButton->focusPolicy() : Qt::NoFocus;
    const auto checkRunEntry = [&](const QString &tag, int count) {
        auto *rail = w.findChild<TitleBar *>();
        QWidget *footer = w.ui() != nullptr ? w.ui()->getBottomBar() : nullptr;
        check(rail != nullptr && footer != nullptr && instancesButton != nullptr,
              tag + QStringLiteral(" 找得到导航轨、轨底和运行状态入口"));
        if (rail == nullptr || footer == nullptr || instancesButton == nullptr) {
            return;
        }
        const QSize oldSize = w.size();
        w.resize(1280, qMax(w.height(), 800));
        QApplication::processEvents();
        for (const bool expanded : {false, true}) {
            rail->setCollapsed(!expanded);
            QApplication::processEvents();
            const QString stage = tag + (expanded ? QStringLiteral(" 展开轨") : QStringLiteral(" 图标轨"));
            QLabel *label = byObjectName<QLabel>(&w, MainWindowUI::runStateLabelName());
            QWidget *textBox = instancesButton->findChild<QWidget *>(QStringLiteral("runStateText"));
            check(instancesButton->isVisible() &&
                      instancesButton->accessibleName() == MainWindowUI::runStateRunningCountText(count),
                  stage + QStringLiteral("：入口可见，进程数 = ") + QString::number(count),
                  QStringLiteral("可见 %1 / 读屏 %2")
                      .arg(instancesButton->isVisible())
                      .arg(instancesButton->accessibleName()));
            const bool shapeOk = expanded
                ? instancesButton->width() == TitleBar::navItemWidth() && textBox != nullptr &&
                      textBox->isVisible() && label != nullptr &&
                      label->text() == MainWindowUI::runStateRunningCountText(count)
                : instancesButton->width() < 80 && textBox != nullptr && !textBox->isVisible();
            check(shapeOk, stage + QStringLiteral("：样子对（展开 = 整格带字，图标轨 = 只剩图标）"),
                  QStringLiteral("宽 %1").arg(instancesButton->width()));
            check(instancesButton->toolTip() == MainWindowUI::bottomBarRunPanelTip(),
                  stage + QStringLiteral("：悬停提示沿用 bottomBarRunPanelTip()"), instancesButton->toolTip());
            // 入口和工具箱不重叠，同在轨里。
            QWidget *tools = w.ui()->getToolboxButton();
            const int top = instancesButton->mapTo(footer, QPoint(0, 0)).y();
            runEntryTop[expanded ? 1 : 0] = top;
            runEntryHeight[expanded ? 1 : 0] = instancesButton->height();
            check(tools != nullptr && top + instancesButton->height() <= tools->mapTo(footer, QPoint(0, 0)).y() &&
                      instancesButton->mapTo(footer, QPoint(0, 0)).x() + instancesButton->width() <= footer->width(),
                  stage + QStringLiteral("：入口在工具箱上面、没挤压它、没伸出轨外"));
            // 面板贴着这颗入口弹出：左边缘对齐、落在入口上方；行数 = 进程数。
            QWidget *panel = openPanel(instancesButton);
            const QPoint buttonTopLeft = instancesButton->mapToGlobal(QPoint(0, 0));
            check(panel != nullptr && panel->isVisible() && panel->x() == buttonTopLeft.x() &&
                      panel->geometry().bottom() < buttonTopLeft.y() &&
                      !rowTitle(panel, count - 1).isEmpty() && rowTitle(panel, count).isEmpty(),
                  stage + QStringLiteral("：面板贴着入口弹出，行数 = 进程数"),
                  panel == nullptr ? QStringLiteral("没弹出")
                                   : QStringLiteral("面板 x=%1 底=%2 / 入口 x=%3 顶=%4")
                                         .arg(panel->x()).arg(panel->geometry().bottom())
                                         .arg(buttonTopLeft.x()).arg(buttonTopLeft.y()));
            if (panel != nullptr) {
                panel->close();
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // 键盘：焦点环只在键盘焦点属性为真时画；Tab 到入口后回车、空格各开一次面板。
            instancesButton->setFocus(Qt::TabFocusReason);
            instancesButton->setProperty("wtfmlKeyboardFocus", false);
            instancesButton->update();
            const QImage plain = instancesButton->grab().toImage();
            instancesButton->setProperty("wtfmlKeyboardFocus", true);
            instancesButton->update();
            const QImage ringed = instancesButton->grab().toImage();
            check(instancesButton->hasFocus() && plain != ringed,
                  stage + QStringLiteral("：有键盘焦点时画焦点环，没有时不画"));
            instancesButton->setProperty("wtfmlKeyboardFocus", false);
            for (const int key : {int(Qt::Key_Return), int(Qt::Key_Space)}) {
                instancesButton->setFocus(Qt::TabFocusReason);
                QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
                QApplication::sendEvent(instancesButton, &down);
                QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
                QApplication::sendEvent(instancesButton, &up);
                QApplication::processEvents();
                QWidget *byKey = w.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
                check(byKey != nullptr && byKey->isVisible(),
                      stage + (key == Qt::Key_Return ? QStringLiteral("：回车打开面板") : QStringLiteral("：空格打开面板")));
                if (byKey != nullptr) {
                    byKey->close();
                    QApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            }
        }
        check(runEntryTop[0] == runEntryTop[1],
              tag + QStringLiteral("：展开轨与图标轨之间入口不跳位"),
              QStringLiteral("图标轨顶 %1 / 展开轨顶 %2").arg(runEntryTop[0]).arg(runEntryTop[1]));
        rail->setCollapsed(true);
        w.resize(oldSize);
        QApplication::processEvents();
    };
    // 没有进程：入口照旧在（常驻，几何和有进程时一样），写「未运行」，点不开，焦点策略不变。
    const auto checkRunEntryIdle = [&](const QString &tag) {
        auto *rail = w.findChild<TitleBar *>();
        QWidget *footer = w.ui() != nullptr ? w.ui()->getBottomBar() : nullptr;
        QWidget *tools = w.ui() != nullptr ? w.ui()->getToolboxButton() : nullptr;
        if (rail == nullptr || footer == nullptr || tools == nullptr || instancesButton == nullptr) {
            check(false, tag + QStringLiteral(" 找得到轨底的几颗入口"));
            return;
        }
        const QSize oldSize = w.size();
        w.resize(1280, qMax(w.height(), 800));
        QApplication::processEvents();
        for (const bool expanded : {false, true}) {
            rail->setCollapsed(!expanded);
            QApplication::processEvents();
            const QString stage = tag + (expanded ? QStringLiteral(" 展开轨") : QStringLiteral(" 图标轨"));
            QLabel *label = byObjectName<QLabel>(&w, MainWindowUI::runStateLabelName());
            QWidget *textBox = instancesButton->findChild<QWidget *>(QStringLiteral("runStateText"));
            check(instancesButton->isVisible() &&
                      instancesButton->accessibleName() == MainWindowUI::runStateNotRunningText(),
                  stage + QStringLiteral("：入口常驻，读屏写「未运行」"), instancesButton->accessibleName());
            check(instancesButton->toolTip() == MainWindowUI::runStateNotRunningText(),
                  stage + QStringLiteral("：悬停提示「未运行」"), instancesButton->toolTip());
            check(expanded ? (textBox != nullptr && textBox->isVisible() && label != nullptr &&
                              label->isVisible() && label->text() == MainWindowUI::runStateNotRunningText())
                           : (instancesButton->width() < 80 && textBox != nullptr && !textBox->isVisible()),
                  stage + QStringLiteral("：展开轨写「未运行」，图标轨只剩弱化图标"));
            const int top = instancesButton->mapTo(footer, QPoint(0, 0)).y();
            check(top == runEntryTop[expanded ? 1 : 0] &&
                      instancesButton->height() == runEntryHeight[expanded ? 1 : 0] &&
                      top + instancesButton->height() <= tools->mapTo(footer, QPoint(0, 0)).y(),
                  stage + QStringLiteral("：空闲与运行中几何一样，进程起停时轨上不跳位"),
                  QStringLiteral("顶 %1 / 运行时 %2，高 %3 / %4")
                      .arg(top).arg(runEntryTop[expanded ? 1 : 0])
                      .arg(instancesButton->height()).arg(runEntryHeight[expanded ? 1 : 0]));
            check(!w.ui()->runPanelAvailable() &&
                      instancesButton->focusPolicy() == runEntryFocusPolicy,
                  stage + QStringLiteral("：空闲时点不开，焦点策略不随状态变"));
            instancesButton->setFocus(Qt::TabFocusReason);
            for (const int key : {int(Qt::Key_Return), int(Qt::Key_Space)}) {
                QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
                QApplication::sendEvent(instancesButton, &down);
                QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
                QApplication::sendEvent(instancesButton, &up);
                QApplication::processEvents();
            }
            QWidget *opened = w.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            check(opened == nullptr || !opened->isVisible(),
                  stage + QStringLiteral("：空闲时回车、空格什么也不弹"));
        }
        rail->setCollapsed(true);
        w.resize(oldSize);
        QApplication::processEvents();
    };

    // ---- AH1 三份会话（同一个实例开两份 + 另一个实例一份）----
    EffectiveJavaSettings holderSettings;
    // javaPath = **这个 exe 自己**，argv 里那一个词让它睡够四分钟（见 main() 开头）。
    holderSettings.javaPath = QCoreApplication::applicationFilePath();
    const QStringList holderArgs{QStringLiteral("session-holder")};
    const int s1 = manager->startGame(holderArgs, holderSettings, releaseId,
                                      QStringLiteral("1.20.1"));
    const int s2 = manager->startGame(holderArgs, holderSettings, releaseId,
                                      QStringLiteral("1.20.1"));
    const int s3 = manager->startGame(holderArgs, holderSettings, oldId,
                                      QStringLiteral("a1.2.6"));
    check(s1 > 0 && s2 > 0 && s3 > 0 && manager->runningGames().size() == 3,
          "AH1 三份会话真的起来了（1.20.1 开两份 + a1.2.6 一份）",
          QStringLiteral("会话 %1/%2/%3 · 在跑 %4 份")
              .arg(s1)
              .arg(s2)
              .arg(s3)
              .arg(manager->runningGames().size()));
    if (s1 <= 0 || s2 <= 0 || s3 <= 0) {
        stopAllSessions();
        AppPaths::setMinecraftDir(originalDir);
        return;
    }
    const auto stableIdFor = [](const QString &versionId) {
        return InstanceMetaFile::read(AppPaths::versionMetaPath(versionId))
            .value(QStringLiteral("instanceId")).toString();
    };
    const QString releaseStableId = stableIdFor(releaseId);
    const QString oldStableId = stableIdFor(oldId);
    const QString playTimePath = LaunchHistory::historyPath();
    check(!releaseStableId.isEmpty() && !oldStableId.isEmpty() && releaseStableId != oldStableId,
          "AH1 客户端进程启动后为两份实例各自保留稳定 ID");

    // ---- AH2 左面板：一行一个会话 ----
    {
        // 先让底栏**重画一次**再断言"点得开"。
        //
        // NOTE: 这一步是必须的：AH1 那三份会话是**直接调 manager->startGame() 起的**，
        // 绕过了 MainWindow 这条启动路；会话开始信号记游玩时长，不刷新底栏。
        // 所以"有会话在跑"这件事本身不会让底栏重画，
        // 左下那一格的点开状态还停在"上一回算出来的那一档"。AH6 那条要量的正是
        // "产品算出来了没有"，所以这里得先走一遍真路（扳机见 repaintBottomBar()，
        // 它一行状态都不改）。
        repaintBottomBar(&w);

        // 再钉一句"有会话在跑时那一格是点得开的"（ 加的那条判据的另一半）：
        // 会话是**直接调 manager->startGame() 起的**，所以这句同时是"产品真的在那条路上
        // 重画过底栏"的证人 —— 少了它，下面 AH6 那条"停光了点不开"改成"永远点不开"
        // 也能绿。
        check(instancesButton != nullptr &&
                  w.ui()->runPanelAvailable(),
              "AH2 有会话在跑时，左下那一格点得开（鼠标收得到）",
              instancesButton == nullptr
                  ? QStringLiteral("<没找到那颗按钮>")
                  : QStringLiteral("收鼠标 %1")
                        .arg(!w.ui()->runPanelAvailable()
                                 ? QStringLiteral("是")
                                 : QStringLiteral("不是")));
        QWidget *panel = openPanel(instancesButton);
        check(panel != nullptr && panel->isVisible(), "AH2 点底栏左边那颗按钮弹出了左面板");
        if (panel != nullptr) {
            check(rowTitle(panel, 0) == QStringLiteral("a1.2.6") &&
                      rowTitle(panel, 1) == QStringLiteral("1.20.1") &&
                      rowTitle(panel, 2) == QStringLiteral("1.20.1"),
                  "AH2 三份会话按最新启动排最上（a1.2.6 在第 0 行，两个 1.20.1 在后）",
                  QStringLiteral("[%1] [%2] [%3]")
                      .arg(rowTitle(panel, 0), rowTitle(panel, 1), rowTitle(panel, 2)));
            check(panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowTitleName(3)) ==
                      nullptr,
                  "AH2 不会有第四行（行数按 runningGames() 来，不是写死的）");
            const auto showsPlayTime = [&rowSubtitle, panel](int row) {
                const QString text = rowSubtitle(panel, row);
                return text.startsWith(QStringLiteral("本次 ")) && text.contains(QStringLiteral(" · 累计 "));
            };
            check(showsPlayTime(0) && showsPlayTime(1) && showsPlayTime(2),
                  "AH2 每个会话都显示对应实例的本次和累计游玩时长");
            const QString initialPlayTime = rowSubtitle(panel, 0);
            const QPointer<QWidget> watchedPanel(panel);
            check(pumpUntil([&] {
                      const QString text = rowSubtitle(watchedPanel.data(), 0);
                      return !text.isEmpty() && text != initialPlayTime;
                  }, 2500),
                  "AH2 面板不用重新打开便会刷新游玩时长");
            // 行首那个方块按**分类**给：正式版（草方块）和远古版（土块）不是同一张图，
            // 而同一类的两份是同一张。分类那一套判据和产品是同一个出处（VersionCategory）。
            check(!rowIcon(panel, 0).isNull() && !rowIcon(panel, 1).isNull() &&
                      rowIcon(panel, 1).toImage() == rowIcon(panel, 2).toImage() &&
                      rowIcon(panel, 0).toImage() != rowIcon(panel, 1).toImage(),
                  "AH2 行首那块的图按实例分类给（两个正式版一样、远古版不一样）");
            // 行尾三颗按钮（结束/设置/日志控制台图标）：均有图形与 tooltip。
            int missing = 0;
            for (int row = 0; row < 3; ++row) {
                for (int action = 0; action < 3; ++action) {
                    QPushButton *button = rowAction(panel, row, action);
                    if (button == nullptr || button->icon().isNull() ||
                        button->toolTip().isEmpty() || !button->text().isEmpty()) {
                        ++missing;
                    }
                }
            }
            check(missing == 0,
                  "AH2 每一行行尾都有三颗图标按钮（结束/设置/日志，各带一句 tooltip）",
                  QStringLiteral("不合格 %1 颗").arg(missing));
            check(rowAction(panel, 0, 3) == nullptr,
                  "AH2 行尾就三颗（多一颗说明编号和产品对不上了）");
        }
    }

    // ---- AH2b 三份会话（同实例多开 + 另一个实例）在两种轨下的运行状态入口 ----
    checkRunEntry(QStringLiteral("AH2b 三份会话"), 3);

    // ---- AH3 齿轮：开的是**这一行那个实例**的设置 ----
    // 点的是第 0 行（a1.2.6），而"当前选中的实例"是另一个 —— 标题里写的是哪一个，
    // 就说明它认的是行里带的版本号，不是下拉框里那个。
    {
        QWidget *panel = openPanel(instancesButton);
        // 行尾那颗齿轮是**第 1 颗**（第 0 颗是电源）—— 这两个号数是产品那边的约定
        // （MainWindow.cpp 里 kRunningPanelStopAction / kRunningPanelSettingsAction），
        // 这里刻意写死：真把两颗换了位置，这一节会红（点成电源 → 弹的是「停止游戏」）。
        QPushButton *gear = rowAction(panel, 0, 1);
        QString dialogTitle;
        check(gear != nullptr, "AH3 第 0 行那颗齿轮找得到");
        if (gear != nullptr) {
            gear->click();
            // 面板先收、动作排到下一轮（见 MainWindowUI 里 dispatch 那段），所以要转事件循环
            // —— 助手自己也泵，直到那一页真的开出来为止。
            closeInstanceSettingsPage(&dialogTitle);
            check(dialogTitle.contains(oldId),
                  "AH3 开的是那一行那个实例的设置（不是下拉框里选中的那个）", dialogTitle);
            check(g_modalTitle.isEmpty(),
                  "AH3 也没有别的框冒出来（齿轮只开设置窗）", g_modalTitle);
        }
    }

    // ---- AH3b 第三颗「日志」：点一下就把**这一行那份会话**的日志窗开出来 ----
    // 对应「运行中实例弹层优先展示实例名、运行时长和
    // 『打开日志/停止』等可用动作」）。这一颗走的是**另一条路**：`openGameLogForSession()`
    // —— "用户点名要看"，它必须能跨过两道闸：
    // · 「显示游戏日志」那个开关（默认是**关**的：它拦的是"自动冒出来的窗"）；
    // · `m_gameLogDismissedSessions`（用户关过这一扇就不再自动开）。
    // 所以这一节四段：① 点一下开出一扇（而且那时候盘上这个开关确实是关的）；
    // ② 同一个会话再点一下还是那一扇（抬到前面，不重开）；③ 用户关掉它、再点 → **又开
    // 出来**（"关过就不再开"那条闸对点名要看的那一发不成立）；④ 收尾关掉。
    {
        const auto logWindowsNow = []() {
            QList<GameLogWindow *> found;
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (auto *log = qobject_cast<GameLogWindow *>(top)) {
                    found.append(log);
                }
            }
            return found;
        };
        const auto dropLogWindows = [&logWindowsNow]() {
            for (GameLogWindow *left : logWindowsNow()) {
                left->close();
            }
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QApplication::processEvents();
        };
        dropLogWindows();
        check(logWindowsNow().isEmpty(), "AH3b 前提：动手之前一扇日志窗都没有");

        // 前提②：盘上那个开关现在是**关**的 —— 少了这一条，下面那几条量的就成了"开着的时候
        // 建不建窗"，而那件事 AE 节已经量过了。
        LauncherSettings ah3bSettings;
        check(!ah3bSettings.showGameLog(),
              "AH3b 前提：盘上「显示游戏日志」是关着的（这一颗点出来才算跨过了那道闸）");

        QWidget *panel = openPanel(instancesButton);
        // 第 2 颗 = 日志图标；a1.2.6 现在排在第 0 行（最新启动排最上）。
        QPushButton *logButton = rowAction(panel, 0, 2);
        check(logButton != nullptr, "AH3b 第 0 行（a1.2.6）那颗「日志」找得到");
        if (logButton != nullptr) {
            logButton->click();
            // 面板先收、动作排到下一轮（见 MainWindowUI 里 dispatch 那段）。
            QApplication::processEvents();
            const QList<GameLogWindow *> opened = logWindowsNow();
            check(opened.size() == 1,
                  "AH3b 点「日志」开出一扇窗（开关关着也开 —— 这是用户点名要的那一发）",
                  QStringLiteral("开出来 %1 扇").arg(opened.size()));
            check(opened.value(0) != nullptr &&
                      opened.value(0)->windowTitle().contains(oldId),
                  "AH3b 那一扇的标题是**这一行**那个实例（不是当前选中的那个）",
                  opened.value(0) != nullptr ? opened.value(0)->windowTitle() : QString());
            const QString manualLine = QStringLiteral("[AH3b] 手动打开后的输出");
            const bool delivered = QMetaObject::invokeMethod(
                &w, "onProcessOutput", Qt::DirectConnection, Q_ARG(int, s3),
                Q_ARG(QString, manualLine));
            QApplication::processEvents();
            check(delivered && opened.value(0) != nullptr &&
                      opened.value(0)->allLogText().contains(manualLine),
                  "★★ AH3b 自动显示关着时，手动打开的日志窗照样继续收到后续输出",
                  opened.value(0) != nullptr ? opened.value(0)->allLogText()
                                             : QStringLiteral("<没有窗>"));

            // ② 同一个会话再点一次：还是那一扇（已经开着就抬到前面，一个字都不重开）。
            QWidget *panelAgain = openPanel(instancesButton);
            QPushButton *logAgain = rowAction(panelAgain, 0, 2);
            check(logAgain != nullptr, "AH3b 再点开面板，那颗「日志」还在原位");
            if (logAgain != nullptr) {
                logAgain->click();
                QApplication::processEvents();
                check(logWindowsNow().size() == 1,
                      "AH3b 同一个会话再点一次：还是那一扇（抬到前面，不重开一扇）",
                      QString::number(logWindowsNow().size()));
            }

            // ③ 用户把它关了（= 走 finished，记进"关过就不再自动开"），再点「日志」——
            // 必须**又开出来**。这一条是"点名要看"和"自动冒出来"两条路的**分界**：
            // 少了它，把 openGameLogForSession 写成"照 showGameLogLine 那套闸走"也绿。
            dropLogWindows();
            check(logWindowsNow().isEmpty(), "AH3b 用户把那一扇关掉了（关掉就销毁）");
            QWidget *panelThird = openPanel(instancesButton);
            QPushButton *logThird = rowAction(panelThird, 0, 2);
            if (logThird != nullptr) {
                logThird->click();
                QApplication::processEvents();
                check(logWindowsNow().size() == 1,
                      "AH3b 关掉之后再点一次：**又开出来**（「关过就不再自动开」拦的是自动那一发）",
                      QStringLiteral("开出来 %1 扇").arg(logWindowsNow().size()));
            } else {
                check(false, "AH3b 第三次点开面板，那颗「日志」找不到了");
            }
            dropLogWindows();
        }
    }

    // ---- AH4 面板是**快照**：开着的这几秒里列表变了，回来时按 key 认人 ----
    // 这一条是这个设计（行里带 key 而不是行号）唯一的照妖镜：把面板开着，从**外面**
    // 停掉第一份会话，然后再点原来第 1 行那颗电源。按行号认人的实现会去停"现在排第 1 的
    // 那一份"（= a1.2.6 那一份），于是红的会是"停错了人"；按 key 认的停的是当初那行
    // 对应的那一份（= 第二份 1.20.1）。
    {
        QWidget *panel = openPanel(instancesButton);
        check(manager->runningGames().size() == 3, "AH4 前提：面板开着的时候还是三份会话",
              QString::number(manager->runningGames().size()));
        check(manager->stopGame(s1),
              "AH4 从外面把第一份会话停掉（模拟「它自己退出了」）");
        const bool gone = pumpUntil([&manager, s1] { return manager->session(s1).sessionId == 0; },
                                    5000);
        check(gone, "AH4 那一份真的从会话表里收掉了（不是只 kill 了进程）");
        check(LaunchHistory::loadPlayTime(playTimePath, releaseStableId).lastMs == -1,
              "AH4 同实例还有一个进程在跑时不结算");

        QPushButton *power = rowAction(panel, 1, 0);
        check(power != nullptr, "AH4 面板上那一行的电源还在（面板不会自己重画，这是快照）");
        if (power != nullptr) {
            answerStopGamePrompt();
            power->click();
            QApplication::processEvents();
            check(g_modalTitle == QStringLiteral("停止游戏") && g_modalWasWarning,
                  "AH4 点电源会先问一句（那是强杀，游戏不会存档）",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(g_modalText.contains(QStringLiteral("1.20.1")) &&
                      g_modalText.contains(QStringLiteral("丢失未保存的进度")),
                  "AH4 那一问里点了实例名、也说清了会丢进度", g_modalText);
            const bool s2Gone = pumpUntil([&manager, s2] { return manager->session(s2).sessionId == 0; },
                                          5000);
            check(s2Gone && manager->session(s3).sessionId == s3,
                  "AH4 停的是**那一行**那份会话（第二份 1.20.1），另一份 a1.2.6 一个字节没动",
                  QStringLiteral("第二份 %1 / a1.2.6 %2")
                      .arg(manager->session(s2).sessionId)
                      .arg(manager->session(s3).sessionId));
            check(manager->runningGames().size() == 1,
                  "AH4 现在只剩一份在跑", QString::number(manager->runningGames().size()));
            const auto playTime = LaunchHistory::loadPlayTime(playTimePath, releaseStableId);
            check(playTime.lastMs > 0 && playTime.totalMs == playTime.lastMs &&
                      LaunchHistory::loadPlayTime(playTimePath, oldStableId).lastMs == -1,
                  "AH4 最后一份同实例进程退出才落盘，另一个实例独立计时");
        }
    }

    // ---- AH5 重新点开：现算一遍（剩下的那一份）----
    {
        QWidget *panel = openPanel(instancesButton);
        check(rowTitle(panel, 0) == QStringLiteral("a1.2.6") &&
                  panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowTitleName(1)) ==
                      nullptr,
              "AH5 面板不缓存上一次那几行：再点开是现算的一份",
              QStringLiteral("[%1]").arg(rowTitle(panel, 0)));
        // 顺手用面板上那颗电源把最后一份也停掉 —— 那一行的 key 就是它。
        QPushButton *power = rowAction(panel, 0, 0);
        if (power != nullptr) {
            answerStopGamePrompt();
            power->click();
            QApplication::processEvents();
        }
        const bool allGone =
            pumpUntil([&manager] { return manager->runningGames().isEmpty(); }, 5000);
        check(allGone, "AH5 面板上那颗电源把最后一份也停了（会话全清，不会留着锁住这个 exe）",
              QString::number(manager->runningGames().size()));
    }

    // ---- AH6 一份都没在跑：左下那一格**点不开**（ 要求：「没有运行中的
    // 实例时让那个实例栏点不开，箭头隐藏」）----
    //
    // 上面 AH2 量的是另一半（有会话时点得开），两条合起来才是"它会**变**"的证人。
    // 这一步的触发走的是**真产品路径**：AH5 用面板上那颗电源停了最后一份 → LauncherManager
    // 发 gameFinished → MainWindow 重画底栏 → setRunPanelAvailable(false)。
    {
        check(instancesButton != nullptr &&
                  !w.ui()->runPanelAvailable(),
              "AH6 一份会话都没在跑时，左下那一格点不开了（鼠标收不到）",
              instancesButton == nullptr
                  ? QStringLiteral("<没找到那颗按钮>")
                  : QStringLiteral("收鼠标 %1")
                        .arg(!w.ui()->runPanelAvailable()
                                 ? QStringLiteral("是")
                                 : QStringLiteral("不是")));
        if (instancesButton != nullptr) {
            instancesButton->click();
            QApplication::processEvents();
            check(w.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) == nullptr,
                  "AH6 点它**什么都不弹**（不是弹一块只有那句空话的面板）");
        }
        checkRunEntryIdle(QStringLiteral("AH6d 进程全部结束后"));
    }

    // ---- AH6b 服务端 + 客户端混排进运行进程面板与任务中心「运行中的进程」----
    {
        const QString serverId = QStringLiteral("42");
        const QString serverName = QStringLiteral("测试服务端42");
        ServerModel::Info info;
        info.name = serverName;
        info.flavor = ServerInstall::Flavor::Vanilla;
        info.flavorKnown = true;
        info.gameVersion = QStringLiteral("1.20.1");
        QString problem;
        check(ServerRegistry::createServer(serverId, info, &problem),
              "AH6b 前提：在盘上创建数字 id 的服务端 42", problem);
        const QString serverDir = AppPaths::serverDir(serverId);
        {
            QFile jar(serverDir + QStringLiteral("/server.jar"));
            if (jar.open(QIODevice::WriteOnly)) {
                jar.close();
            }
        }
        auto *workspace = w.findChild<ServerWorkspaceController *>();
        auto *runStateLbl = w.findChild<QLabel *>(MainWindowUI::runStateLabelName());
        check(workspace != nullptr && runStateLbl != nullptr,
              "AH6b 拿得到 ServerWorkspaceController 和 runStateLabel");
        if (workspace != nullptr && runStateLbl != nullptr) {
            ServerRun::ServerProcess *srv = workspace->ensureProcess(serverId);
            srv->start(serverId, serverDir, QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--fake-server"), QStringLiteral("normal")}, 4000);
            check(pumpUntil([srv] { return srv->status() == ServerModel::Status::Running; }, 20000),
                  "AH6b 假服务端起来了");
            check(runStateLbl->text() == MainWindowUI::runStateRunningCountText(1)
                      && instancesButton != nullptr
                      && w.ui()->runPanelAvailable(),
                  "★★ AH6b 只有服务端在跑时左下角显示「1 个进程运行中」且面板可展开",
                  runStateLbl->text());
            checkRunEntry(QStringLiteral("AH6b 只有服务端"), 1);

            // 隔至少 25ms 再起一个客户端会话，确保客户端启动时间更晚、混排排第 0 行
            QThread::msleep(30);
            const int sMixed =
                manager->startGame(holderArgs, holderSettings, releaseId, QStringLiteral("1.20.1"));
            check(sMixed > 0, "AH6b 再起一个客户端形成客户端+服务端混排");
            repaintBottomBar(&w);
            check(runStateLbl->text() == MainWindowUI::runStateRunningCountText(2),
                  "★★ AH6b 1 客户端 + 1 服务端合计显示「2 个进程运行中」", runStateLbl->text());
            checkRunEntry(QStringLiteral("AH6b 客户端 + 服务端"), 2);

            QWidget *panel = openPanel(instancesButton);
            check(rowTitle(panel, 0) == QStringLiteral("1.20.1")
                      && rowTitle(panel, 1) == serverName,
                  "★★ AH6b 面板按启动时间混排（后起的客户端 1.20.1 在第 0 行，服务端在第 1 行）",
                  QStringLiteral("[%1] [%2]").arg(rowTitle(panel, 0), rowTitle(panel, 1)));
            // 行尾图标一致校验：客户端第 2 颗日志图标与服务端第 2 颗控制台图标同一图标像素
            QPushButton *cliLog = rowAction(panel, 0, 2);
            QPushButton *srvCon = rowAction(panel, 1, 2);
            check(cliLog != nullptr && srvCon != nullptr && !cliLog->icon().isNull()
                      && !srvCon->icon().isNull()
                      && cliLog->icon().pixmap(14, 14).toImage()
                             == srvCon->icon().pixmap(14, 14).toImage(),
                  "★★ AH6b 客户端「日志」与服务端「控制台」使用同一个图标");

            // 点服务端第 2 颗：开控制台
            if (srvCon != nullptr) {
                srvCon->click();
                QApplication::processEvents();
            }
            auto *consolePage = w.findChild<ServerConsolePage *>();
            check(consolePage != nullptr && consolePage->isVisible(),
                  "★★ AH6b 点服务端行的控制台图标打开了 ServerConsolePage");
            if (w.ui() != nullptr && w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()) {
                w.ui()->overlay()->evict();
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }

            // 任务中心里不再有「运行中的进程」栏（统一从轨底入口查看）
            if (auto *summaryBtn = w.findChild<QPushButton *>(MainWindowUI::taskSummaryButtonName())) {
                summaryBtn->click();
                QApplication::processEvents();
                ActivityPopover::openAllIn(&w);
                QApplication::processEvents();
            }
            if (QWidget *tcPanel = w.findChild<QWidget *>(MainWindowUI::taskCenterPanelName())) {
                check(tcPanel->findChild<QWidget *>(QStringLiteral("taskCenterRunningPane")) == nullptr &&
                          w.findChild<QPushButton *>(QStringLiteral("taskCenterRunningCollapse")) == nullptr,
                      "★★ AH6b 任务中心没有「运行中的进程」栏和它的开关（有进程在跑也一样）");
                if (w.ui() != nullptr && w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()) {
                    w.ui()->overlay()->evict();
                    QApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            }

            // 点服务端第 1 颗（设置）：进服务端设置页
            QWidget *panelForSettings = openPanel(instancesButton);
            if (QPushButton *srvGear = rowAction(panelForSettings, 1, 1)) {
                srvGear->click();
                QApplication::processEvents();
            }
            auto *srvSettingsPage = w.findChild<ServerSettingsPage *>();
            check(srvSettingsPage != nullptr && srvSettingsPage->isVisible(),
                  "★★ AH6b 点服务端行的设置图标进入服务端管理页");
            if (w.ui() != nullptr && w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()) {
                w.ui()->overlay()->evict();
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }

            // 点服务端第 0 颗（停止）：走正常停服存盘流程，不直接强杀
            QWidget *panelForStop = openPanel(instancesButton);
            bool sawStopping = false;
            if (QPushButton *srvStop = rowAction(panelForStop, 1, 0)) {
                srvStop->click();
                QApplication::processEvents();
                sawStopping = (srv->status() == ServerModel::Status::Stopping)
                              || (srv->status() == ServerModel::Status::Stopped);
            }
            const bool srvStoppedCleanly =
                pumpUntil([srv] { return !srv->isRunning(); }, 10000);
            check(sawStopping && srvStoppedCleanly
                      && srv->status() == ServerModel::Status::Stopped && !srv->wasForceKilled(),
                  "★★ AH6b 点服务端行「结束」走正常停服存盘流程，未直接强杀",
                  ServerModel::statusName(srv->status()));

            // 收尾停掉客户端会话
            manager->stopGame(sMixed);
            pumpUntil([manager] { return manager->runningGames().isEmpty(); }, 5000);
        }
        QDir(AppPaths::serversDir()).removeRecursively();
    }

    // ---- AH6e 只开一个客户端、同一个实例多开：入口计数跟着变 ----
    {
        const int single = manager->startGame(holderArgs, holderSettings, releaseId, QStringLiteral("1.20.1"));
        repaintBottomBar(&w);
        checkRunEntry(QStringLiteral("AH6e 一个客户端"), 1);
        const int again = manager->startGame(holderArgs, holderSettings, releaseId, QStringLiteral("1.20.1"));
        repaintBottomBar(&w);
        checkRunEntry(QStringLiteral("AH6e 同一实例多开"), 2);
        manager->stopGame(single);
        manager->stopGame(again);
        pumpUntil([manager] { return manager->runningGames().isEmpty(); }, 5000);
        repaintBottomBar(&w);
        checkRunEntryIdle(QStringLiteral("AH6e 全部结束后"));
    }

    {
        auto *activity = w.findChild<ActivityCenterController *>();
        auto *sessions = w.findChild<GameSessionController *>();
        auto *detail = w.findChild<TaskDetailController *>();
        auto *launch = w.findChild<LaunchController *>();
        check(activity && sessions && detail && launch, "Launch task controllers are available");
        if (activity && sessions && detail && launch) {
            // ---- AH6c 进程起来之前也能取消（#61）----
            //
            // 启动流程那几段（换令牌 / 准备 natives / 下 Java 运行时）最长几分钟，期间
            // 任务中心那颗「取消」现在就得能用：按下去之后流程在下一个检查点停下，
            // 不拉起游戏，任务记成「已取消」而不是失败。
            const auto preTask = activity->begin(Tasks::TaskKind::Launch,
                                                 QStringLiteral("Launch test"),
                                                 releaseId);
            const QString preKey = QString::number(preTask);
            check(!w.taskCancelWouldWorkForTest(activity->tasks()->snapshot(preTask)),
                  "AH6c 没有启动流程在走时，新建的启动任务没有可取消的东西");
            // 摆出"启动流程正走在半路"（真实入口只有 onStartButtonClicked()，这里用
            // 测试钩子直接把那一面旗立起来）。
            launch->setStartingForTest(true);
            check(w.taskCancelWouldWorkForTest(activity->tasks()->snapshot(preTask)),
                  "AH6c 进程还没起来、启动流程在半路时，cancelWouldWork 为真");
            detail->open(preKey);
            auto *preCancel = w.findChild<QPushButton *>(MainWindowUI::taskDetailCancelName());
            check(preCancel && !preCancel->isHidden(),
                  "AH6c 进程起来前详情页就露「取消」");
            if (preCancel != nullptr && !preCancel->isHidden()) {
                preCancel->click();
                check(launch->abortRequestedForTest()
                          && launch->cancelSourceForTest()
                                 == LaunchController::StartCancelSource::TaskCenter,
                      "AH6c 按「取消」把取消请求交给启动流程（来源 = 任务中心）");
                check(activity->tasks()->snapshot(preTask).state == Tasks::TaskState::Running,
                      "AH6c 流程走到下一个检查点之前，任务还在「进行中」");
                // 流程在检查点上早退时收尾的就是这一句（见 TaskAbortGuard）。
                launch->finishAbortedStart(preTask);
                check(activity->tasks()->snapshot(preTask).state == Tasks::TaskState::Cancelled,
                      "AH6c 用户取消的启动任务记「已取消」，不记失败");
            }
            launch->setStartingForTest(false);
            w.ui()->overlay()->dismiss();
            if (Tasks::isActiveState(activity->tasks()->snapshot(preTask).state)) {
                activity->tasks()->cancel(preTask);
            }

            // ---- 进程起来之后：取消只停这一份会话（d5e70087 那条）----
            const auto task = activity->begin(Tasks::TaskKind::Launch, QStringLiteral("Launch test"), releaseId);
            const QString key = QString::number(task);
            detail->open(key);
            auto *cancel = w.findChild<QPushButton *>(MainWindowUI::taskDetailCancelName());
            check(cancel && cancel->isHidden(),
                  "Launch cannot be cancelled before a process is bound and while no launch runs");

            const int target = manager->startGame(holderArgs, holderSettings, releaseId, releaseId);
            const int other = manager->startGame(holderArgs, holderSettings, releaseId, releaseId);
            check(target > 0 && other > 0, "Two test processes for the same instance are running");
            if (target > 0 && other > 0) {
                GameSessionController::GameSessionFacts facts;
                facts.instanceId = releaseId;
                facts.instanceName = releaseId;
                sessions->beginSession(target, releaseId, task, facts);
                const bool offered = pumpUntil([cancel] { return cancel && !cancel->isHidden(); }, 1000);
                check(offered, "Launch task displays Cancel when its process starts");
                if (offered) {
                    answerNextPrompt(1);
                    cancel->click();
                    check(g_modalTitle == QStringLiteral("停止游戏") && manager->session(target).sessionId == target,
                          "Declining the stop confirmation leaves the game running");

                    answerStopGamePrompt();
                    cancel->click();
                    const bool stopped = pumpUntil([&] {
                        return manager->session(target).sessionId == 0
                               && activity->tasks()->snapshot(task).state == Tasks::TaskState::Cancelled;
                    }, 5000);
                    check(stopped && manager->session(other).sessionId == other,
                          "Cancelling a launch stops only its process and records the task as cancelled");
                }
            }
            w.ui()->overlay()->dismiss();
            for (int session : {target, other}) {
                if (manager->session(session).sessionId == 0) continue;
                w.markGameStopRequestedForTest(session);
                manager->stopGame(session);
            }
            pumpUntil([&] { return manager->runningGames().isEmpty(); }, 5000);
            if (Tasks::isActiveState(activity->tasks()->snapshot(task).state)) activity->tasks()->cancel(task);
        }
    }

    // ---- AH7 账号弹层：最近登录的三个账户（缓存里拿）+ 菜单区 ----
    const int refreshFailedBefore = countOccurrences(messagesSnapshot(), QStringLiteral("无法刷新「"));
    const int refreshedOnceBefore =
        countOccurrences(messagesSnapshot(), QStringLiteral("已自动更新"));
    {
        // 胶囊本身：当前是离线账号 Alex ⇒ 名字 Alex、小状态「离线」、头像有脸。
        {
            QLabel *pillName = w.findChild<QLabel *>(QStringLiteral("homeAccountLabel"));
            QLabel *pillType = w.findChild<QLabel *>(QStringLiteral("homeAccountTypeLabel"));
            QLabel *pillFace = w.findChild<QLabel *>(QStringLiteral("homeAvatarLabel"));
            check(pillName != nullptr && pillType != nullptr && pillFace != nullptr &&
                      pillName->text() == QStringLiteral("Alex") &&
                      pillType->text() == MainWindowUI::accountTypeOfflineText() && !pillType->isHidden() &&
                      !pillFace->pixmap().isNull(),
                  "AH7 主页胶囊：名字 Alex + 小状态「离线」+ 一张脸",
                  pillName != nullptr && pillType != nullptr
                      ? pillName->text() + QStringLiteral(" / ") + pillType->text()
                      : QString());
            // 有实例、今天打开过 ⇒ 按时段问候，名字和胶囊同一个。
            QLabel *title = w.findChild<QLabel *>(MainWindowUI::homeHeroTitleName());
            check(title != nullptr && title->text().endsWith(QStringLiteral(" Alex")),
                  "AH7 主页大标题问候带上当前账号名 Alex", title != nullptr ? title->text() : QString());
        }
        QWidget *panel = openPanel(accountsButton);
        check(panel != nullptr && panel->isVisible(), "AH7 点主页右上那颗账号胶囊弹出了账号弹层");
        if (panel != nullptr) {
            check(rowTitle(panel, 0) == QStringLiteral("Bob") &&
                      rowTitle(panel, 1) == QStringLiteral("Carol") &&
                      rowTitle(panel, 2) == QStringLiteral("Alex"),
                  "AH7 三行 = 按 saved_at 倒序的**最后三个**（Bob 最新，然后是 Carol、Alex）",
                  QStringLiteral("[%1] [%2] [%3]")
                      .arg(rowTitle(panel, 0), rowTitle(panel, 1), rowTitle(panel, 2)));
            check(rowTitle(panel, 3) == MainWindowUI::accountAddText() &&
                      rowTitle(panel, 4) == MainWindowUI::accountSkinButtonText() &&
                      rowTitle(panel, 5) == MainWindowUI::accountLogoutText() &&
                      accountRow(panel, 6) == nullptr,
                  "AH7 三个最近账号之后是菜单区「添加账号」「管理账号与皮肤」「退出登录」"
                  "（Alice 和 Steve 不在最近账号列表里）",
                  QStringLiteral("[%1] [%2] [%3]")
                      .arg(rowTitle(panel, 3), rowTitle(panel, 4), rowTitle(panel, 5)));
            check(rowSubtitle(panel, 0) == MainWindowUI::bottomBarPanelOnlineAccountText() &&
                      rowSubtitle(panel, 1) == MainWindowUI::bottomBarPanelOnlineAccountText() &&
                      rowSubtitle(panel, 2) == MainWindowUI::bottomBarPanelOfflineAccountText(),
                  "AH7 每一行第二句说的是「这是个什么账号」（离线那行写「离线」）",
                  QStringLiteral("[%1] [%2] [%3]")
                      .arg(rowSubtitle(panel, 0), rowSubtitle(panel, 1), rowSubtitle(panel, 2)));
            // ---- AH7b 行首那张脸（ 要求：「在右下角那个上拉框里名字左边
            // 加一个头像（因为选择/登录过，所以肯定会有缓存，应该不会遇到丢失的情况）」）----
            //
            // 这一条**只认缓存**：点开面板不许联网、不许换令牌、也不许读客户端 JAR
            // （读 JAR 要起一次 PowerShell，几百毫秒挂在"点开"这一下上是不行的，见
            // accountFacePixmap() 那段）。所以三行都得当场有图，而且三张图分别是谁，
            // 下面这几条各钉一处：
            // · 当前账号（Alex）那一行必须**就是底栏那颗头像**缩下来的 —— 同一个窗口里
            // 同一个人不可能有两张脸（这条是那个保证本身）；
            // · 另外两个正版账号这一节的 fixture **一个字节的档案缓存都没写过**，
            // 所以走的是"自己画的首字母方块"那条兜底：逐像素等于 letterAvatar(名字)。
            const int iconSize = MainWindowUI::bottomBarPanelIconSize();
            check(!rowIcon(panel, 0).isNull() && !rowIcon(panel, 1).isNull() &&
                      !rowIcon(panel, 2).isNull() &&
                      rowIcon(panel, 0).size() == QSize(iconSize, iconSize),
                  "AH7b 三行行首各有一张脸（尺寸 = bottomBarPanelIconSize()）",
                  QStringLiteral("%1x%2 / %3x%4 / %5x%6")
                      .arg(rowIcon(panel, 0).width())
                      .arg(rowIcon(panel, 0).height())
                      .arg(rowIcon(panel, 1).width())
                      .arg(rowIcon(panel, 1).height())
                      .arg(rowIcon(panel, 2).width())
                      .arg(rowIcon(panel, 2).height()));
            {
                QLabel *barAvatar = byObjectName<QLabel>(&w, MainWindowUI::avatarLabelName());
                check(barAvatar != nullptr && !barAvatar->pixmap().isNull() &&
                          rowIcon(panel, 2).toImage() ==
                              barAvatar->pixmap()
                                  .scaled(iconSize, iconSize, Qt::IgnoreAspectRatio,
                                          Qt::FastTransformation)
                                  .toImage(),
                      "AH7b 当前账号（Alex）那一行 = 底栏那颗头像缩到 24（同一个人只有一张脸）",
                      barAvatar != nullptr
                          ? QStringLiteral("底栏 %1x%2")
                                .arg(barAvatar->pixmap().width())
                                .arg(barAvatar->pixmap().height())
                          : QStringLiteral("<底栏那颗头像没找到>"));
            }
            check(rowIcon(panel, 0).toImage() ==
                          QPixmap::fromImage(
                              SkinModel::letterAvatar(QStringLiteral("Bob"), iconSize))
                              .toImage() &&
                      rowIcon(panel, 1).toImage() ==
                          QPixmap::fromImage(
                              SkinModel::letterAvatar(QStringLiteral("Carol"), iconSize))
                              .toImage(),
                  "AH7b 盘上没有档案缓存的两个正版账号退回自己画的首字母方块（按名字画的）");
            // 兜底那张的底色是**算出来的**（qHash(名字) 取色相）。这条断言非有不可：
            // 不量它的话，"每个人都是同一个灰方块"也能让上面那条绿。
            // 底色是**算出来的**（qHash(名字) 取色相），所以这两条缺一不可：只量"两个人
            // 不一样"的话，"每个人都是同一个灰方块"也能绿；只量"等于自己那个色"的话，
            // 写死一个常量也能绿。
            //
            // NOTE: 这里敢拿 `QColor::operator==` 直接比，是因为 avatarFallbackColor()
            // 交出来的是**按 8 位量化过的**色（见那边那段：Qt 6 的色值是 16 位的，
            // fromHsv() 直接比会假不相等 —— 名字/rgba/redF 全一样，只有 == 说不一样）。
            check(SkinModel::avatarFallbackColor(QStringLiteral("Bob")) !=
                      SkinModel::avatarFallbackColor(QStringLiteral("Carol")),
                  "AH7b 兜底那块的底色按名字算（Bob 和 Carol 不是同一个色）",
                  QStringLiteral("Bob %1 / Carol %2")
                      .arg(SkinModel::avatarFallbackColor(QStringLiteral("Bob")).name())
                      .arg(SkinModel::avatarFallbackColor(QStringLiteral("Carol")).name()));
            check(rowIcon(panel, 0).toImage().pixelColor(0, 0) ==
                          SkinModel::avatarFallbackColor(QStringLiteral("Bob")) &&
                      rowIcon(panel, 1).toImage().pixelColor(0, 0) ==
                          SkinModel::avatarFallbackColor(QStringLiteral("Carol")),
                  "AH7b 而每一块的底色就是他自己名字算出来的那个色",
                  QStringLiteral("Bob 期望 %1 量到 %2 / Carol 期望 %3 量到 %4")
                      .arg(SkinModel::avatarFallbackColor(QStringLiteral("Bob")).name())
                      .arg(rowIcon(panel, 0).toImage().pixelColor(0, 0).name())
                      .arg(SkinModel::avatarFallbackColor(QStringLiteral("Carol")).name())
                      .arg(rowIcon(panel, 1).toImage().pixelColor(0, 0).name()));
            // 当前账号（Alex）那一行打勾、只有那一行；别人那两行整行可点（点了 = 切过去）。
            check(rowChecked(panel, 2) && !rowChecked(panel, 0) && !rowChecked(panel, 1),
                  "AH7 当前账号（Alex）那一行打勾，只有那一行");
            check(accountRow(panel, 0) != nullptr && accountRow(panel, 0)->isEnabled() &&
                      accountRow(panel, 1) != nullptr && accountRow(panel, 1)->isEnabled(),
                  "AH7 别人那两行整行可点（不再是行尾一颗「登录」/「选择」）");

            // ---- AH8 点一个令牌不足 6 小时的正版账号：只切号、不触发刷新、不弹窗 ----
            QAbstractButton *carolButton = accountRow(panel, 1);
            dismissNextPrompt();
            clickRow(carolButton);
            check(AccountStore::activeUuid() == carolUuid,
                  "AH8 切到那个账号了",
                  AccountStore::activeUuid());
            {
                QLabel *pillName = w.findChild<QLabel *>(QStringLiteral("homeAccountLabel"));
                QLabel *pillType = w.findChild<QLabel *>(QStringLiteral("homeAccountTypeLabel"));
                check(pillName != nullptr && pillType != nullptr && pillName->text() == QStringLiteral("Carol") &&
                          pillType->text() == MainWindowUI::homeAccountMicrosoftText() && !pillType->isHidden(),
                      "AH8 主页胶囊跟着换成 Carol + 「微软账号」",
                      pillName != nullptr && pillType != nullptr
                          ? pillName->text() + QStringLiteral(" / ") + pillType->text()
                          : QString());
                QLabel *title = w.findChild<QLabel *>(MainWindowUI::homeHeroTitleName());
                check(title != nullptr && title->text().endsWith(QStringLiteral(" Carol")),
                      "AH8 主页大标题问候跟着换成 Carol", title != nullptr ? title->text() : QString());
            }
            check(g_modalTitle.isEmpty(),
                  "AH8 切号不再刷新令牌，也不弹任何错误框",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(countOccurrences(messagesSnapshot(), QStringLiteral("无法刷新「")) ==
                      refreshFailedBefore,
                  "AH8 日志里没有「无法刷新」");
            check(countOccurrences(messagesSnapshot(), QStringLiteral("已自动更新")) ==
                      refreshedOnceBefore,
                  "AH8 日志里也没有「已自动更新」（切号不刷新）");
        }
    }

    // ---- AH9 再点开：Carol 成了「当前」（打勾），点离线那一行切过去 ----
    {
        QWidget *panel = openPanel(accountsButton);
        check(rowTitle(panel, 0) == QStringLiteral("Bob") &&
                  rowTitle(panel, 2) == QStringLiteral("Alex"),
              "AH9 三行还是那三个账号（切账号不改 saved_at，顺序不变）",
              QStringLiteral("[%1] [%2] [%3]")
                  .arg(rowTitle(panel, 0), rowTitle(panel, 1), rowTitle(panel, 2)));
        check(rowChecked(panel, 1) && !rowChecked(panel, 0) && !rowChecked(panel, 2),
              "AH9 刚切过去的 Carol 那一行打勾（判据是活动槽，不是列表里记的那个键）");
        QAbstractButton *alexButton = accountRow(panel, 2);
        check(alexButton != nullptr && alexButton->isEnabled(), "AH9 离线账号 Alex 那一行可点");

        // 点那一行：离线账号没有令牌这回事，切过去就完事 —— 不弹窗、不发请求。
        if (alexButton != nullptr) {
            dismissNextPrompt();
            clickRow(alexButton);
            check(AccountStore::activeUuid() == alexUuid, "AH9 切到那个离线账号了",
                  AccountStore::activeUuid());
            check(g_modalTitle.isEmpty(),
                  "AH9 切离线账号**什么都不弹**（它没有令牌可换）", g_modalTitle);
            QLabel *accountType =
                byObjectName<QLabel>(&w, MainWindowUI::accountTypeLabelName());
            check(accountType != nullptr &&
                      accountType->text() == MainWindowUI::accountTypeOfflineText() &&
                      auth->playerName() == alexName,
                  "AH9 底栏也跟着换了（第二行「离线」、名字是这个人）",
                  accountType != nullptr
                      ? accountType->text() + QStringLiteral(" / ") + auth->playerName()
                      : QStringLiteral("<没找到>"));
        }
    }

    // ---- AH10 令牌**够用**那一档：一个包都不发 ----
    // Bob 那张到 2999 年（剩得远远不止 6 小时），所以点他那一行不该去换新的。
    // 判据有三条：不弹窗、"已自动更新"那条日志没有增加、而且此刻剩的时间确实 ≥6 小时
    // （把"为什么可以不换"这条依据也钉住 —— 少了它，这条断言在"两边都不换"的实现下也绿）。
    {
        QWidget *panel = openPanel(accountsButton);
        QAbstractButton *bobButton = accountRow(panel, 0);
        check(bobButton != nullptr && rowTitle(panel, 0) == QStringLiteral("Bob"),
              "AH10 Bob 那一行找得到", rowTitle(panel, 0));
        if (bobButton != nullptr) {
            dismissNextPrompt();
            clickRow(bobButton);
            check(AccountStore::activeUuid() == bobUuid, "AH10 切到 Bob 了",
                  AccountStore::activeUuid());
            check(g_modalTitle.isEmpty(), "AH10 没有弹任何框（够用就不换，一个包都不发）",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(countOccurrences(messagesSnapshot(), QStringLiteral("已自动更新")) ==
                      refreshedOnceBefore,
                  "AH10 也没有「已自动更新」那一行日志");
            check(auth->tokenSecondsLeft() >= AuthManager::tokenRefreshWindowSecs,
                  "AH10 依据：此刻这张令牌剩得 ≥6 小时（所以上面那条不是漏做了）",
                  QStringLiteral("剩 %1 秒 / 门槛 %2 秒")
                      .arg(auth->tokenSecondsLeft())
                      .arg(AuthManager::tokenRefreshWindowSecs));
            QLabel *accountType =
                byObjectName<QLabel>(&w, MainWindowUI::accountTypeLabelName());
            check(accountType != nullptr &&
                      accountType->text() == MainWindowUI::accountTypeOnlineText(),
                  "AH10 底栏第二行换成「正版」", accountType != nullptr ? accountType->text()
                                                                        : QStringLiteral("<没找到>"));
        }
    }

    // ---- AH11 切不过去那一档：**不能拿上一个人的令牌接着往下走** ----
    // 列表里那一项可能**不完整**（用户换过 Windows 账户、或者某一版写坏过，会留下这种
    // "只有名字没有副本"的项）。activate() 会拒绝它、框也弹了 —— 这一节钉住的是拒绝之后
    // **不再往下走**：活动槽里还是 Bob，而 Bob 这张令牌**偏偏只剩不到 6 小时**。缺了那道守卫
    // 的实现会接着拿 Bob 的令牌去换一张新的，还会把弹窗里的名字写成点的那一行（Dave）。
    //
    // NOTE: 这里**同时**把活动槽里微软那份的 refresh_token 拿掉，是刻意的：缺守卫的那一版
    // 真会走到 refreshMinecraftToken()，而带着一个假的 refresh_token 去请求就是
    // **真联网**（这一套断言必须离线跑，见文件头）。没有它就停在第一道判据上，所以
    // 那条路即使被走通了也一个包都不发。
    {
        QJsonObject shortMinecraft;
        shortMinecraft.insert(QStringLiteral("access_token"), QStringLiteral("at-Bob"));
        shortMinecraft.insert(QStringLiteral("expires_at"), shortExpiry);
        shortMinecraft.insert(QStringLiteral("name"), QStringLiteral("Bob"));
        shortMinecraft.insert(QStringLiteral("uuid"), bobUuid);
        QJsonObject noRefreshMicrosoft;
        noRefreshMicrosoft.insert(QStringLiteral("access_token"), QStringLiteral("ms-at-Bob"));
        check(TokenStore::writeJson(TokenStore::minecraftKey(), shortMinecraft) &&
                  TokenStore::writeJson(TokenStore::microsoftKey(), noRefreshMicrosoft),
              "AH11 活动槽换成「Bob 的令牌只剩不到 6 小时」（微软那份没有 refresh_token）");
        // 内存里那一份也得跟着换 —— tokenSecondsLeft() 读的是它，不是盘上。
        // 用 reloadStoredAccount() 纯本地读盘，不触发网络刷新。
        auth->reloadStoredAccount();
        check(!auth->savedAccessToken().isEmpty(),
              "AH11 这一份按产品那条路读回内存");
        // NOTE: 认人用 playerName() 而**不是** playerUuid()：后者是带连字符的那个形状
        // （dashedUuid()），和列表里那个 32 位十六进制不是一串字。
        check(auth->playerName() == QStringLiteral("Bob") &&
                  auth->tokenSecondsLeft() < AuthManager::tokenRefreshWindowSecs &&
                  auth->tokenSecondsLeft() > 0,
              "AH11 前提：内存里现在是 Bob、令牌剩得到但不到 6 小时（少了这一条，下面那几条在"
              "「两边都不刷」的实现下也会绿）",
              QStringLiteral("%1 / 剩 %2 秒 / 门槛 %3 秒")
                  .arg(auth->playerName())
                  .arg(auth->tokenSecondsLeft())
                  .arg(AuthManager::tokenRefreshWindowSecs));

        {
            QJsonArray array;
            QJsonObject dave;
            dave.insert(QStringLiteral("uuid"), daveUuid);
            dave.insert(QStringLiteral("name"), daveName);
            dave.insert(QStringLiteral("saved_at"), QStringLiteral("2026-12-31T00:00:00Z"));
            // **刻意不给** microsoft / minecraft 两份副本 —— 就是上面说的"不完整"那一项。
            array.append(dave);
            QJsonObject bobEntry;
            for (const QJsonValue &value : TokenStore::readJson(TokenStore::accountsKey())
                                               .value(QStringLiteral("accounts"))
                                               .toArray()) {
                if (value.toObject().value(QStringLiteral("uuid")).toString() == bobUuid) {
                    bobEntry = value.toObject();
                }
            }
            bobEntry.insert(QStringLiteral("saved_at"), QStringLiteral("2026-09-30T00:00:00Z"));
            array.append(bobEntry);
            QJsonObject rootObject;
            rootObject.insert(QStringLiteral("accounts"), array);
            check(!bobEntry.isEmpty() && TokenStore::writeJson(TokenStore::accountsKey(), rootObject) &&
                      AccountStore::accounts().size() == 2 &&
                      AccountStore::activeUuid() == bobUuid,
                  "AH11 列表换成两项：Dave（副本不完整）在前 + Bob（当前）在后",
                  QStringLiteral("%1 个 / 当前 %2")
                      .arg(AccountStore::accounts().size())
                      .arg(AccountStore::activeUuid()));
        }

        const int noRefreshBefore = countOccurrences(messagesSnapshot(), QStringLiteral("无法刷新「"));
        const int noExtraTokenBefore =
            countOccurrences(messagesSnapshot(), QStringLiteral("已自动更新"));
        QWidget *panel = openPanel(accountsButton);
        check(panel != nullptr, "AH11 账号弹层弹出来了");
        check(rowTitle(panel, 0) == daveName && rowTitle(panel, 1) == QStringLiteral("Bob"),
              "AH11 两行就是这两个账号（Dave 的 saved_at 更新）",
              QStringLiteral("[%1] [%2]").arg(rowTitle(panel, 0), rowTitle(panel, 1)));
        QAbstractButton *daveButton = accountRow(panel, 0);
        check(daveButton != nullptr && daveButton->isEnabled() && !rowChecked(panel, 0),
              "AH11 Dave 那一行点得动（列表里看着就是个正版账号）");
        if (daveButton != nullptr) {
            dismissNextPrompt();
            // **兜住第二个框**：缺了那道守卫的那一版会接着去换 Bob 的令牌，换不出来于是**又弹
            // 一个**「登录令牌没能刷新」。只认一个框的助手（dismissNextModal）在第二个框上
            // 会一直等它 —— 那是**测试卡死**，不是红。再排一发（它自己也会重排 0/300 两发），
            // 顺序上第二个框的标题会覆盖第一个，所以下面那几条断言的判据就是"最后一个框"。
            QTimer::singleShot(250, [] { dismissNextPrompt(); });
            clickRow(daveButton);
            check(g_modalTitle == QStringLiteral("切换账号失败") && g_modalWasWarning,
                  "AH11 点它弹的是「切换账号失败」（那一项没有副本，切不过去）", g_modalTitle);
            check(g_modalText.contains(QStringLiteral("凭据不完整")),
                  "AH11 框里说清是那份凭据不完整", g_modalText);
            check(!g_modalText.contains(QStringLiteral("已切换至")),
                  "AH11 框里**没有**「已切换至…」那半句（那是另一个框的话，写在这儿就是假的）",
                  g_modalText);
            check(AccountStore::activeUuid() == bobUuid &&
                      auth->playerName() == QStringLiteral("Bob"),
                  "AH11 没换人：活动槽和内存里都还是 Bob",
                  QStringLiteral("%1 / %2").arg(AccountStore::activeUuid(), auth->playerName()));
            check(countOccurrences(messagesSnapshot(), QStringLiteral("无法刷新「")) == noRefreshBefore,
                  "AH11 也没有拿 Bob 的令牌去换（点的是别人的那一行，这条缺了守卫就会红）");
            check(countOccurrences(messagesSnapshot(), QStringLiteral("已自动更新")) ==
                      noExtraTokenBefore,
                  "AH11 更没有「顺手换了一张新的」那一行（Bob 的令牌只剩不到 6 小时，"
                  "所以「没发生」不是侥幸）");
        }
    }

    // ---- AH11b 点账号弹层菜单区那一行「退出登录」：清掉登录状态，账号留下来 ----
    //
    // 登出只有这一个入口（「管理账号与皮肤」里只有删除），所以它得一直在菜单区里。
    //
    // 这一节要钉的是三个**分开**的事实，少一条这个功能就不成立：
    // ① 盘上的登录状态真没了（活动槽那两份凭据被抹掉，不是只把界面换个字）；
    // ② **账号还留在列表里**（登出 ≠ 删除）—— 所以点那一行的「登录」还能回来；
    // ③ 内存里那一份也清了（不然底栏和启动参数拿到的还是上一个人的名字，
    // 等于"看着登出了、手里还攥着他的状态"）。
    {
        // 先把状态摆回正常的"Bob 是当前、活动槽里是他那份好凭据"：AH11 是**故意**把活动槽
        // 写坏的（它要验"切不过去之后不许拿上一个人的令牌接着走"），别让这一节从那种状态
        // 开始 —— 那样量的就不是"登出"了。activate() 会把 Bob 列表里那份副本（rt-bob、
        // 还有 30 天）写回活动槽。
        check(AccountStore::activate(bobUuid),
              "AH11b 前提：切回 Bob，活动槽里放他列表里那份好凭据");
        check(AccountStore::activeUuid() == bobUuid,
              "AH11b 前提：此刻的当前账号是 Bob（少这一条，下面那句「登出之后没当前账号了」"
              "在压根没登录的实现下也是绿的）",
              AccountStore::activeUuid());

        QWidget *panel = openPanel(accountsButton);
        // AH11 把列表换成了 Dave + Bob（Dave 的 saved_at 更新，排在前面）—— 所以"第 0 行"
        // 现在是 Dave。按名字找 Bob 那一行。
        const int bobRow = rowIndexOf(panel, QStringLiteral("Bob"));
        check(bobRow >= 0 && rowChecked(panel, bobRow),
              "AH11b 账号弹层上有 Bob 那一行、打着勾（他就是当前账号；AH11 换过列表，行号不许再写死）",
              QString::number(bobRow));
        const int logoutRow = rowIndexOf(panel, MainWindowUI::accountLogoutText());
        QAbstractButton *bobLogout = logoutRow >= 0 ? accountRow(panel, logoutRow) : nullptr;
        check(bobLogout != nullptr && bobLogout->isEnabled(),
              "AH11b 菜单区那一行「退出登录」找得到、能点", QString::number(logoutRow));

        const int logoutLogBefore = countOccurrences(messagesSnapshot(), QStringLiteral("已退出账号"));
        if (bobLogout != nullptr) {
            dismissNextPrompt();
            clickRow(bobLogout);
            // 登出是**可逆**的一步（账号还在列表里，点一下就回来），所以不拦一下 ——
            // 和「删除所选」正相反，那一条是删盘的、要确认。
            check(g_modalTitle.isEmpty(), "AH11b 登出**不弹窗**",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(AccountStore::activeUuid().isEmpty(),
                  "AH11b 盘上没有当前账号了（是「没登录」，不是「换成了另一个人」）",
                  AccountStore::activeUuid());
            check(AccountStore::contains(bobUuid) &&
                      AccountStore::nameOf(bobUuid) == QStringLiteral("Bob"),
                  "AH11b **账号还在列表里** —— 这一条就是「登出」和「删除」的分界");
            check(TokenStore::readJson(TokenStore::minecraftKey()).isEmpty() &&
                      TokenStore::readJson(TokenStore::microsoftKey()).isEmpty(),
                  "AH11b 活动槽那两份凭据真从盘上抹掉了（只把界面换个字不算登出）");
            check(!auth->loggedIn() && auth->playerName() != QStringLiteral("Bob"),
                  "AH11b 内存里那一份也清了（不然底栏还挂着上一个人的名字）",
                  auth->playerName());
            QLabel *accountType =
                byObjectName<QLabel>(&w, MainWindowUI::accountTypeLabelName());
            check(accountType != nullptr &&
                      accountType->text() == MainWindowUI::accountTypeNotLoggedInText(),
                  "AH11b 底栏第二行退回「未登录」那一档",
                  accountType != nullptr ? accountType->text()
                                         : QStringLiteral("<没找到>"));
            check(countOccurrences(messagesSnapshot(), QStringLiteral("已退出账号")) ==
                      logoutLogBefore + 1,
                  "AH11b 日志里留了一行（命令行那一路看得见）");
        }

        // 再点开：没有任何一行打勾（没有当前账号了），菜单区也不再摆「退出登录」；Bob 那行还在。
        panel = openPanel(accountsButton);
        const int bobRowAgain = rowIndexOf(panel, QStringLiteral("Bob"));
        QAbstractButton *bobLogin = bobRowAgain >= 0 ? accountRow(panel, bobRowAgain) : nullptr;
        check(bobRowAgain >= 0 && bobLogin != nullptr,
              "AH11b 再点开：Bob 那一行还在（登出没把他从列表里删掉）",
              QString::number(bobRowAgain));
        bool anyChecked = false;
        for (int row = 0; row < 8; ++row) anyChecked = anyChecked || rowChecked(panel, row);
        check(!anyChecked && rowIndexOf(panel, MainWindowUI::accountLogoutText()) < 0,
              "AH11b 再点开：没有一行打勾、也没有「退出登录」（登出之后没有当前账号）");

        // 点那一行就回来了 —— 这一条才是"登出没删账号"在**界面**上的样子
        // （前面那条 contains() 说的是盘上，这条说的是用户看得到的东西）。
        if (bobLogin != nullptr) {
            dismissNextPrompt();
            clickRow(bobLogin);
            check(AccountStore::activeUuid() == bobUuid,
                  "AH11b 点那一行就回来了（账号没丢）",
                  AccountStore::activeUuid());
            // Bob 那份副本里是 30 天后到期 → 够用，所以这一下不该联网、也不该弹窗。
            check(g_modalTitle.isEmpty(), "AH11b 回来这一下不弹窗（副本里的令牌够用）",
                  g_modalTitle);
        }
    }

    // ---- AH12b 从「未登录」点那一行切到离线账号：**主页那张脸得跟着换** ----
    //
    // 用户 报的 bug，原话：「现在从未登录切换到选择离线账号不会随机给皮肤了」。
    //
    // 根因在 `refreshAccountSkin()` 最前面那句「窗还没建就直接 return」：主页那个小人、
    // 底栏头像、那个窗里的 3D 预览**都只在 `startPlayerSkinLoad()` → `applyPlayerSkin()`
    // 里更新**，所以没点开过「账号与外观…」的会话里，切账号这条路上的换脸整个被那句吞了
    // —— 从"未登录"切过去时，屏幕上一直停着未登录那张兜底占位图。
    //
    // 切账号现在能从右面板直接点（不必先开「账号与外观…」，那条路上那句 return 进不去）。
    //
    // 这一节的形状就是用户那两下，一步都不自己造：**建一个离线账号 → 用菜单区「退出登录」
    // 退回未登录 → 再点她那一行**。判据落在"屏幕上那张图变没变"上，不是"某个
    // 函数被调过"—— 用户看的也是前者。
    {
        const QString lenaName = QStringLiteral("Lena");
        const QString lenaUuid = AccountStore::offlineUuidFor(lenaName);
        const QColor lenaTint(0x40, 0xA0, 0x60);
        // 给 Lena 存一张手选的本地皮肤（纯色）：离线账号没有别的皮肤来源时画的是内置 Steve，
        // 和下面「未登录」的占位图一样，看不出"脸换了"。
        {
            QImage tinted(64, 64, QImage::Format_ARGB32_Premultiplied);
            tinted.fill(lenaTint);
            QString pickError;
            check(OfflineSkin::save(lenaUuid, tinted, QStringLiteral("classic"), &pickError),
                  "AH12b 前提：给 Lena 存一张手选的本地皮肤", pickError);
        }
        // 名字进 AuthManager 由产品那条路自己做（onOfflineAccountRequested 里那句
        // setOfflinePlayer），所以这儿只按产品那条路把它加出来。
        QMetaObject::invokeMethod(&w, "onOfflineAccountRequested", Qt::DirectConnection,
                                  Q_ARG(QString, lenaName));
        QApplication::processEvents();
        SkinViewer *viewer = byObjectName<SkinViewer>(&w, MainWindowUI::skinViewerName());
        QLabel *hint = byObjectName<QLabel>(&w, MainWindowUI::skinHintLabelName());
        check(viewer != nullptr && hint != nullptr,
              "AH12b 前提：主页那个预览和那句话说外观的话都找得到");
        check(AccountStore::activeUuid() == lenaUuid,
              "AH12b 前提：加出来的离线账号成了当前账号", AccountStore::activeUuid());
        // 这一条现在就该绿 —— 它同时也是"加账号那一路的换脸"（和下面切账号那一路是同一
        // 句 return 挡的）。**兜底那张是内置默认皮肤**，和 Lena 那张纯色差得远。
        check(viewer != nullptr && viewer->skin().pixelColor(0, 0) == lenaTint,
              "AH12b 加完它就画上了她手选的本地皮肤（不是兜底占位图）",
              viewer != nullptr ? viewer->skin().pixelColor(0, 0).name()
                                : QStringLiteral("<没找到>"));

        // ---- 第一下：点菜单区「退出登录」，退回未登录 ----
        QWidget *panel = openPanel(accountsButton);
        const int row = rowIndexOf(panel, lenaName);
        check(row >= 0 && rowChecked(panel, row), "AH12b 账号弹层上有 Lena 那一行、打着勾",
              QString::number(row));
        const int logoutRow = rowIndexOf(panel, MainWindowUI::accountLogoutText());
        QAbstractButton *logout = logoutRow >= 0 ? accountRow(panel, logoutRow) : nullptr;
        check(logout != nullptr, "AH12b 她是当前账号 → 菜单区有「退出登录」");
        if (logout != nullptr) {
            dismissNextPrompt();
            clickRow(logout);
            check(AccountStore::activeUuid().isEmpty(),
                  "AH12b 登出之后没有当前账号了（未登录）", AccountStore::activeUuid());
            const QImage placeholder = SkinModel::defaultSteveSkin();
            check(viewer->skin() == placeholder,
                  "AH12b 登出之后主页退回那张兜底占位图（下面那条才有得比）");
            // 这一条是**判据的前提**，不是装饰：下面"切过去之后脸换了"如果成立、而这一条
            // 不成立（登出之后没退回占位图），那说明"变了"是碰巧的、和这次切换没关系。
            check(hint->text() == MainWindowUI::skinHintNotLoggedInText(),
                  "AH12b 那句话也说未登录", hint->text());

            // ---- 第二下：点她那一行切过去 ----
            panel = openPanel(accountsButton);
            const int row2 = rowIndexOf(panel, lenaName);
            QAbstractButton *select = row2 >= 0 ? accountRow(panel, row2) : nullptr;
            check(select != nullptr && !rowChecked(panel, row2),
                  "AH12b 不再是当前账号 → 她那一行不打勾、可点");
            if (select != nullptr) {
                dismissNextPrompt();
                clickRow(select);
                check(AccountStore::activeUuid() == lenaUuid,
                      "AH12b 切到 Lena 了", AccountStore::activeUuid());
                // **这一条就是用户报的那件事**：屏幕上那张脸必须真的换掉。
                check(viewer->skin().pixelColor(0, 0) == lenaTint && viewer->skin() != placeholder,
                      "AH12b **主页那张脸跟着换了**（从兜底占位图换成她手选的本地皮肤）",
                      QStringLiteral("%1 → %2")
                          .arg(placeholder.pixelColor(0, 0).name())
                          .arg(viewer->skin().pixelColor(0, 0).name()));
                check(hint->text() == MainWindowUI::skinHintOfflineCustomSkinText(lenaName),
                      "AH12b 那句话也跟着换成离线自定义皮肤那一档", hint->text());
            }
        }
    }

    // ---- AH13 切号/开页不刷新 + 嵌套循环界面响应实测 + 启动网络失败三选（#67） ----
    {
        auto *activity = w.findChild<ActivityCenterController *>();
        check(activity != nullptr, "AH13 前提：ActivityCenterController 找得到");

        // 本地模拟服务端：可统计认证请求与披风档案请求，也可制造慢响应 / 断网
        QTcpServer mockAuth;
        check(mockAuth.listen(QHostAddress::LocalHost, 0), "AH13 模拟认证服务器监听成功");
        int tokenHits = 0;
        int skinProfileHits = 0;
        int delayMs = 0;
        bool holdCapeProfile = false;
        std::function<void()> releaseCapeProfile;
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(mockAuth.serverPort());
        QObject::connect(&mockAuth, &QTcpServer::newConnection, &mockAuth, [&] {
            while (auto *sock = mockAuth.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock] {
                    QByteArray buf = sock->property("buf").toByteArray() + sock->readAll();
                    sock->setProperty("buf", buf);
                    const qsizetype hdrEnd = buf.indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    int contentLen = 0;
                    for (const QByteArray &line : buf.left(hdrEnd).split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:")) {
                            contentLen = line.trimmed().mid(15).trimmed().toInt();
                        }
                    }
                    if (buf.size() < hdrEnd + 4 + contentLen) return;
                    const QByteArray path = buf.left(buf.indexOf("\r\n")).split(' ').value(1);
                    if (path == "/token") {
                        ++tokenHits;
                    } else if (path == "/minecraft/profile") {
                        ++skinProfileHits;
                    }
                    const auto respond = [sock = QPointer<QTcpSocket>(sock), path, bobUuid, base] {
                        if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;
                        QByteArray body = QByteArrayLiteral("{}");
                        if (path == "/token") {
                            body = QByteArrayLiteral("{\"access_token\":\"ms-2\",\"refresh_token\":\"rt-2\"}");
                        } else if (path == "/xbl" || path == "/xsts") {
                            body = QByteArrayLiteral("{\"Token\":\"t\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"u\"}]}}");
                        } else if (path == "/mc") {
                            body = QByteArrayLiteral("{\"access_token\":\"mc-ok-67\",\"expires_in\":86400}");
                        } else if (path == "/profile") {
                            body = QStringLiteral("{\"id\":\"%1\",\"name\":\"Bob\"}").arg(bobUuid).toUtf8();
                        } else if (path == "/minecraft/profile") {
                            body = QStringLiteral(
                                       "{\"id\":\"%1\",\"name\":\"Bob\",\"skins\":[],"
                                       "\"capes\":[{\"id\":\"cape-1\",\"alias\":\"Migrator\","
                                       "\"state\":\"ACTIVE\",\"url\":\"%2/cape.png\"}]}")
                                       .arg(bobUuid, base)
                                       .toUtf8();
                        }
                        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
                        sock->write(body);
                        sock->disconnectFromHost();
                    };
                    if (path == "/minecraft/profile" && holdCapeProfile) {
                        releaseCapeProfile = respond;
                    } else if (delayMs > 0) {
                        QTimer::singleShot(delayMs, sock, respond);
                    } else {
                        respond();
                    }
                });
            }
        });

        AuthManager::EndpointOverrides overrides;
        overrides.oauthTokenUrl = base + QStringLiteral("/token");
        overrides.xboxLiveAuthUrl = base + QStringLiteral("/xbl");
        overrides.xstsAuthUrl = base + QStringLiteral("/xsts");
        overrides.minecraftLoginUrl = base + QStringLiteral("/mc");
        overrides.minecraftProfileUrl = base + QStringLiteral("/profile");
        overrides.clientId = QStringLiteral("test-client-id");
        AuthManager::setEndpointOverridesForTest(overrides);
        SkinApi::setBaseUrlOverride(base);

        // 写入一个剩余 1 小时（< 6h，未过期）且有 refresh_token 的账号 Bob，再切去离线账号 Alex
        {
            QJsonObject ms{{QStringLiteral("access_token"), QStringLiteral("ms-bob")},
                           {QStringLiteral("refresh_token"), QStringLiteral("rt-bob")}};
            QJsonObject mc{{QStringLiteral("access_token"), QStringLiteral("mc-bob-1h")},
                           {QStringLiteral("expires_at"),
                            QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)},
                           {QStringLiteral("name"), QStringLiteral("Bob")},
                           {QStringLiteral("uuid"), bobUuid}};
            check(TokenStore::writeJson(TokenStore::microsoftKey(), ms) &&
                      TokenStore::writeJson(TokenStore::minecraftKey(), mc) &&
                      AccountStore::captureActive(),
                  "AH13 准备不足 6h（未过期）的账号 Bob");
            check(AccountStore::addOffline(alexName), "AH13 先切回 Alex");
            auth->setOfflinePlayer(alexName);
        }

        // 1a) 切号不刷新；未过期账号打开账号页会用现有令牌拉披风、但不刷新令牌
        tokenHits = 0;
        skinProfileHits = 0;
        {
            QWidget *panel = openPanel(accountsButton);
            const int bobRow = rowIndexOf(panel, QStringLiteral("Bob"));
            QAbstractButton *bobBtn = bobRow >= 0 ? accountRow(panel, bobRow) : nullptr;
            check(bobBtn != nullptr, "AH13 账号弹层找得到 Bob");
            if (bobBtn != nullptr) {
                dismissNextPrompt();
                clickRow(bobBtn);
            }
        }
        check(AccountStore::activeUuid() == bobUuid && tokenHits == 0,
              "AH13 面板切到剩余 < 6h 的 Bob 不发刷新请求",
              QStringLiteral("active=%1 tokenHits=%2").arg(AccountStore::activeUuid()).arg(tokenHits));

        holdCapeProfile = true;
        QMetaObject::invokeMethod(&w, "onAccountSkinButtonClicked", Qt::DirectConnection);
        QApplication::processEvents();
        auto *capeDialog = w.findChild<AccountSkinDialog *>();
        auto *skinApi = w.findChild<SkinApi *>();
        auto *capeLoading = capeDialog ? capeDialog->findChild<QLabel *>(QStringLiteral("accountCapeLoading")) : nullptr;
        auto *capeList = capeDialog ? capeDialog->findChild<QListWidget *>(AccountSkinDialog::capeListName()) : nullptr;
        auto *accountList = capeDialog ? capeDialog->findChild<QListWidget *>(AccountSkinDialog::accountListName()) : nullptr;
        auto *switchButton = capeDialog ? capeDialog->findChild<QPushButton *>(AccountSkinDialog::switchButtonName()) : nullptr;
        auto *applyCape = capeDialog ? capeDialog->findChild<QPushButton *>(AccountSkinDialog::applyCapeButtonName()) : nullptr;
        const bool awaitingCapeResponse = pumpUntil(
            [&] { return bool(releaseCapeProfile); }, HttpClient::kDefaultTimeoutMs);
        check(awaitingCapeResponse && skinProfileHits == 1 &&
                  tokenHits == 0 && capeLoading && !capeLoading->isHidden() &&
                  accountList && accountList->isEnabled(),
              "AH13 披风请求仍在等待时，账号列表可操作且未刷新令牌",
              QStringLiteral("pending=%1 profiles=%2 tokens=%3 loading=%4 accountsEnabled=%5")
                  .arg(awaitingCapeResponse).arg(skinProfileHits).arg(tokenHits)
                  .arg(capeLoading && !capeLoading->isHidden()).arg(accountList && accountList->isEnabled()));
        {
            if (accountList) {
                for (int i = 0; i < accountList->count(); ++i) {
                    if (accountList->item(i)->data(Qt::UserRole).toString() == alexUuid) {
                        accountList->setCurrentRow(i);
                        break;
                    }
                }
            }
            check(switchButton && switchButton->isEnabled(),
                  "AH13 披风尚未返回时可点击切换账号");
            if (switchButton) switchButton->click();
            check(AccountStore::activeUuid() == alexUuid && capeLoading && capeLoading->isHidden(),
                  "AH13 未等披风响应就切换到了离线账号");
        }
        holdCapeProfile = false;
        if (releaseCapeProfile) std::exchange(releaseCapeProfile, {})();
        check(skinApi && pumpUntil([&] { return skinApi->cachedCapes(bobUuid) != nullptr; }, 5000) &&
                  capeDialog && capeDialog->capesOwner().isEmpty() && capeList && capeList->isHidden(),
              "AH13 迟到的披风响应进入原账号缓存，离线账号界面保持当前身份");

        QMetaObject::invokeMethod(&w, "onAccountSwitchRequested", Qt::DirectConnection, Q_ARG(QString, bobUuid));
        check(AccountStore::activeUuid() == bobUuid && tokenHits == 0 && skinProfileHits == 1 &&
                  capeDialog && capeDialog->capesOwner() == bobUuid && capeList && capeList->count() == 2,
              "AH13 切回 Bob 立即展示缓存，未重复请求");

        if (capeDialog && capeList && capeLoading && applyCape) {
            capeList->setCurrentRow(1);
            holdCapeProfile = true;
            emit capeDialog->refreshRequested();
            check(pumpUntil([&] { return skinProfileHits == 2 && bool(releaseCapeProfile); }, 5000) &&
                      tokenHits == 0 && capeLoading->isVisible() && applyCape->isEnabled() &&
                      capeDialog->selectedCapeId() == QStringLiteral("cape-1"),
                  "AH13 手动刷新期间，已有披风仍可选择和应用");
            holdCapeProfile = false;
            if (releaseCapeProfile) std::exchange(releaseCapeProfile, {})();
            check(pumpUntil([&] { return capeLoading->isHidden(); }, 5000) &&
                      capeDialog->selectedCapeId() == QStringLiteral("cape-1"),
                  "AH13 后台刷新保留所选披风");
        }
        if (w.ui() != nullptr && w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()) {
            w.ui()->overlay()->closeOverlay();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        // 1b) 令牌已过期时打开账号页：不刷新令牌、不拉披风，失败行显示「登录凭据已过期，披风列表未更新。」
        {
            QJsonObject ms{{QStringLiteral("access_token"), QStringLiteral("ms-bob")},
                           {QStringLiteral("refresh_token"), QStringLiteral("rt-bob")}};
            QJsonObject mc{{QStringLiteral("access_token"), QStringLiteral("mc-bob-expired")},
                           {QStringLiteral("expires_at"),
                            QDateTime::currentDateTimeUtc().addSecs(-3600).toString(Qt::ISODate)},
                           {QStringLiteral("name"), QStringLiteral("Bob")},
                           {QStringLiteral("uuid"), bobUuid}};
            check(TokenStore::writeJson(TokenStore::microsoftKey(), ms) &&
                      TokenStore::writeJson(TokenStore::minecraftKey(), mc) &&
                      AccountStore::captureActive(),
                  "AH13 准备已过期账号 Bob（验证开页提示）");
            auth->reloadStoredAccount();
        }
        tokenHits = 0;
        skinProfileHits = 0;
        QMetaObject::invokeMethod(&w, "onAccountSkinButtonClicked", Qt::DirectConnection);
        QApplication::processEvents();
        {
            auto *dialog = w.findChild<AccountSkinDialog *>();
            auto *errLabel = dialog ? dialog->findChild<QLabel *>(AccountSkinDialog::statusErrorLabelName()) : nullptr;
            auto *retryBtn = dialog ? dialog->findChild<QPushButton *>(AccountSkinDialog::retryButtonName()) : nullptr;
            check(tokenHits == 0 && skinProfileHits == 0 && errLabel != nullptr && errLabel->isVisibleTo(dialog) &&
                      errLabel->text() == QStringLiteral("登录凭据已过期，披风列表未更新。") &&
                      retryBtn != nullptr && retryBtn->isVisibleTo(dialog),
                  "AH13 令牌已过期时开页不刷新不拉披风，失败行显示规定提示与重试按钮",
                  QStringLiteral("tokenHits=%1 skinHits=%2 err=%3")
                      .arg(tokenHits)
                      .arg(skinProfileHits)
                      .arg(errLabel ? errLabel->text() : QString()));
            if (retryBtn != nullptr) {
                retryBtn->click();
                pumpUntil([&] { return skinProfileHits == 1 && capeLoading && capeLoading->isHidden(); }, 5000);
            }
            auto *capeList = dialog ? dialog->findChild<QListWidget *>(AccountSkinDialog::capeListName()) : nullptr;
            check(tokenHits == 1 && skinProfileHits == 1 && errLabel != nullptr && !errLabel->isVisibleTo(dialog) &&
                      errLabel->text().isEmpty() && capeList != nullptr && capeList->count() == 2,
                  "AH13 点失败行「重试」走正常刷新路径并拉出披风列表",
                  QStringLiteral("tokenHits=%1 skinHits=%2 capes=%3")
                      .arg(tokenHits)
                      .arg(skinProfileHits)
                      .arg(capeList ? capeList->count() : -1));
        }
        if (w.ui() != nullptr && w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()) {
            w.ui()->overlay()->closeOverlay();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        // 2) 实测嵌套事件循环等待网络响应期间窗口事件/重绘/定时器照常响应
        {
            QJsonObject mc{{QStringLiteral("access_token"), QStringLiteral("mc-bob-1h")},
                           {QStringLiteral("expires_at"),
                            QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)},
                           {QStringLiteral("name"), QStringLiteral("Bob")},
                           {QStringLiteral("uuid"), bobUuid}};
            check(TokenStore::writeJson(TokenStore::minecraftKey(), mc), "AH13 重置为 < 6h 令牌以触发刷新");
        }
        delayMs = 250;
        tokenHits = 0;
        int timerTicks = 0;
        int paintEvents = 0;
        struct PaintCounter : QObject {
            int *count = nullptr;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint && count != nullptr) ++(*count);
                return false;
            }
        } paintCounter;
        paintCounter.count = &paintEvents;
        w.installEventFilter(&paintCounter);
        QTimer uiPulse;
        uiPulse.setInterval(30);
        QObject::connect(&uiPulse, &QTimer::timeout, &w, [&] {
            ++timerTicks;
            w.update();
        });
        uiPulse.start();
        QString slowErr;
        const QString slowTok = auth->loadSavedToken(&slowErr);
        uiPulse.stop();
        w.removeEventFilter(&paintCounter);
        delayMs = 0;
        check(slowTok == QStringLiteral("mc-ok-67") && timerTicks >= 3 && paintEvents >= 1,
              "AH13 实测嵌套事件循环等待网络期间 UI 定时器与窗口重绘照常响应",
              QStringLiteral("ticks=%1 paints=%2 err=%3").arg(timerTicks).arg(paintEvents).arg(slowErr));

        // 3) 断网（关闭端口）启动：弹出三选（「重试」「离线游玩」「取消」）
        // 先把 Bob 的令牌设为已过期，但保留有效本地档案和 access_token
        {
            QJsonObject ms{{QStringLiteral("access_token"), QStringLiteral("ms-bob")},
                           {QStringLiteral("refresh_token"), QStringLiteral("rt-bob")}};
            QJsonObject mc{{QStringLiteral("access_token"), QStringLiteral("mc-offline-kept")},
                           {QStringLiteral("expires_at"),
                            QDateTime::currentDateTimeUtc().addSecs(-3600).toString(Qt::ISODate)},
                           {QStringLiteral("name"), QStringLiteral("Bob")},
                           {QStringLiteral("uuid"), bobUuid}};
            check(TokenStore::writeJson(TokenStore::microsoftKey(), ms) &&
                      TokenStore::writeJson(TokenStore::minecraftKey(), mc) &&
                      AccountStore::captureActive(),
                  "AH13 准备已过期正版账号 Bob");
            auth->reloadStoredAccount();
        }
        mockAuth.close();
        QTcpServer closedProbe;
        check(closedProbe.listen(QHostAddress::LocalHost, 0), "AH13 分配关闭端口模拟断网");
        const quint16 deadPort = closedProbe.serverPort();
        closedProbe.close();
        overrides.oauthTokenUrl = QStringLiteral("http://127.0.0.1:%1/token").arg(deadPort);
        AuthManager::setEndpointOverridesForTest(overrides);

        // 确保版本下拉框选中 releaseId
        if (QComboBox *combo = w.ui()->getInstalledVersionCombo()) {
            const int idx = combo->findData(releaseId);
            if (idx >= 0) combo->setCurrentIndex(idx);
        }

        // 3a) 点「取消」（下标 2）：任务记「已取消」、不清理正版身份
        {
            const int tasksBefore = activity ? activity->tasks()->all().size() : 0;
            PromptAnswerer answerer({2});
            answerer.start();
            QMetaObject::invokeMethod(&w, "onStartButtonClicked", Qt::DirectConnection);
            pumpUntil([&] { return !answerer.answered.isEmpty(); }, 5000);
            answerer.stop();
            check(!answerer.answered.isEmpty() &&
                      answerer.answered.at(0).title == QStringLiteral("无法刷新登录凭据") &&
                      answerer.answered.at(0).text.contains(QStringLiteral("无法刷新登录凭据：")) &&
                      answerer.answered.at(0).text.contains(QStringLiteral("以离线方式进入游戏（无法加入正版服务器）")) &&
                      answerer.answered.at(0).buttons == QStringList({QStringLiteral("重试"), QStringLiteral("离线游玩"), QStringLiteral("取消")}),
                  "AH13 断网启动弹出「重试」「离线游玩」「取消」三选及规定文案",
                  answerer.answered.isEmpty() ? QStringLiteral("未弹框")
                                              : answerer.answered.at(0).buttons.join(QStringLiteral(",")));
            const auto allSnaps = activity ? activity->tasks()->all() : QList<Tasks::TaskSnapshot>{};
            const auto finishedSnaps = activity ? activity->tasks()->finished() : QList<Tasks::TaskSnapshot>{};
            check(allSnaps.size() == tasksBefore + 1 && !finishedSnaps.isEmpty() &&
                      finishedSnaps.first().state == Tasks::TaskState::Cancelled,
                  "AH13 点「取消」后任务中心将启动任务记为「已取消」",
                  finishedSnaps.isEmpty() ? QString() : QString::number(int(finishedSnaps.first().state)));
            check(AccountStore::activeUuid() == bobUuid && !AccountStore::isOffline(bobUuid) &&
                      auth->loggedIn(),
                  "AH13 取消后保留正版账号身份");
        }

        // 3b) 先点「重试」（下标 0），再点「离线游玩」（下标 1）：步骤 0 完成（离线游玩），继续用本地正版档案推进启动
        {
            const int tasksBefore = activity ? activity->tasks()->all().size() : 0;
            // 第1次弹三选点「重试」(0)；第2次弹三选点「离线游玩」(1)；后续 pre-launch「无法启动」点确定(0)
            PromptAnswerer answerer({0, 1, 0});
            answerer.start();
            QMetaObject::invokeMethod(&w, "onStartButtonClicked", Qt::DirectConnection);
            pumpUntil([&] { return answerer.answered.size() >= 3; }, 5000);
            answerer.stop();
            check(answerer.answered.size() >= 3 &&
                      answerer.answered.at(0).title == QStringLiteral("无法刷新登录凭据") &&
                      answerer.answered.at(1).title == QStringLiteral("无法刷新登录凭据") &&
                      answerer.answered.at(2).title == QStringLiteral("版本信息缺失"),
                  "AH13 先「重试」再「离线游玩」后进入后续启动检查（不再卡在认证）",
                  answerer.answered.size() >= 3 ? answerer.answered.at(2).title : QStringLiteral("数量不足"));
            const auto allSnaps = activity ? activity->tasks()->all() : QList<Tasks::TaskSnapshot>{};
            const auto finishedSnaps = activity ? activity->tasks()->finished() : QList<Tasks::TaskSnapshot>{};
            check(allSnaps.size() == tasksBefore + 1 && !finishedSnaps.isEmpty() &&
                      !finishedSnaps.first().steps.isEmpty() &&
                      finishedSnaps.first().steps.at(0).state == Tasks::TaskStepState::Done &&
                      finishedSnaps.first().steps.at(0).note.contains(QStringLiteral("离线游玩")),
                  "AH13 离线游玩通过校验登录步骤并记录本地档案方式启动");
            check(AccountStore::activeUuid() == bobUuid && !AccountStore::isOffline(bobUuid) &&
                      auth->loggedIn() && auth->playerName() == QStringLiteral("Bob") &&
                      auth->playerUuid() == AuthManager::dashedUuid(bobUuid) &&
                      auth->savedAccessToken() == QStringLiteral("mc-offline-kept"),
                  "AH13 离线游玩不清掉正版身份，保留本地名字、UUID 和现有令牌");
        }

        SkinApi::setBaseUrlOverride(QString());
        AuthManager::setEndpointOverridesForTest({});
    }

    // ---- AH14 键盘入口：主页回车 = 点「启动游戏」 ----
    //
    // 判据是"这一发有没有变成一次点击"，不是"游戏真的起来了"：产品那条接线在这一节里
    // 摘掉、换成计数器 —— 否则这一下会真去起一个进程。
    {
        if (w.ui()->getTabWidget() != nullptr) {
            w.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
            QApplication::processEvents();
        }
        QPushButton *start = w.ui()->getStartButton();
        check(start != nullptr && start->isEnabled(),
              "AH14 主页上那颗「启动游戏」点得动（它是给既有接线用的兼容控件，平时收着）");
        if (start != nullptr) {
            QObject::disconnect(start, &QPushButton::clicked, &w, nullptr);
            int launches = 0;
            QObject::connect(start, &QPushButton::clicked, &w, [&launches] { ++launches; });

            const auto pressEnter = [&w](bool autoRepeat = false) {
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), autoRepeat);
                QApplication::sendEvent(&w, &press);
                QApplication::processEvents();
            };

            // 焦点在窗口本身（= 没有交互焦点）：回车等于一次点击。
            w.setFocus();
            QApplication::processEvents();
            pressEnter();
            check(launches == 1, "AH14 主页没有交互焦点时，回车点一次「启动游戏」",
                  QStringLiteral("%1 次").arg(launches));

            // 按住不放的那几发不算。
            pressEnter(true);
            check(launches == 1, "AH14 回车的 auto-repeat 不再多点一次",
                  QStringLiteral("%1 次").arg(launches));

            // 不在主页（切到下载页）：这一发不启动。
            if (QTabWidget *tabs = w.ui()->getTabWidget()) {
                tabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
                QApplication::processEvents();
            }
            pressEnter();
            check(launches == 1, "AH14 不在主页时回车不启动",
                  QStringLiteral("%1 次").arg(launches));

            // 焦点落在可交互控件上：这一发归它自己，窗口不抢。
            if (QTabWidget *tabs = w.ui()->getTabWidget()) {
                tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
                QApplication::processEvents();
            }
            QWidget *interactive = nullptr;
            for (QWidget *widget : w.findChildren<QWidget *>()) {
                if (!widget->isVisible()) continue;
                if (qobject_cast<QAbstractButton *>(widget) != nullptr ||
                    qobject_cast<QLineEdit *>(widget) != nullptr) {
                    interactive = widget;
                    break;
                }
            }
            check(interactive != nullptr, "AH14 主页上找得到一颗可交互控件（下面拿它验焦点那一档）");
            if (interactive != nullptr) {
                interactive->setFocus();
                QApplication::processEvents();
                pressEnter();
                check(launches == 1, "AH14 焦点在可交互控件上时窗口不抢回车",
                      QStringLiteral("%1 次").arg(launches));
            }

            // NOTE: 产品那条接线这一节里不再接回去（本函数到这儿只剩收尾，没人再点它）。
        }
    }

    // ---- AH15 键盘入口：启动中 Esc = 取消启动 ----
    //
    // "流程走在半路、游戏进程还没起来"这一态用产品自己的测试钩子搭出来（和 AH6c 同一招），
    // 然后按真键：判据是取消请求有没有交到启动流程、来源是不是"任务中心"。
    {
        auto *activity = w.findChild<ActivityCenterController *>();
        auto *launch = w.findChild<LaunchController *>();
        check(activity != nullptr && launch != nullptr, "AH15 拿到任务账本和启动流程");
        if (activity != nullptr && launch != nullptr) {
            // 先看浮层挡着那一档：Esc 归浮层，不该把取消发下去。
            w.ui()->overlay()->openPrompt(false);
            QApplication::processEvents();
            QKeyEvent escToOverlay(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&w, &escToOverlay);
            check(!launch->abortRequestedForTest(), "AH15 浮层开着时 Esc 不取消启动（先归浮层）");
            w.ui()->overlay()->dismiss();
            if (w.ui()->overlay()->isOpen()) {
                w.ui()->overlay()->closeOverlay();
            }
            QApplication::processEvents();
            check(w.ui()->overlay() == nullptr || !w.ui()->overlay()->isOpen(),
                  "AH15 浮层收干净了（下面那一发才是真按 Esc）");
            check(QApplication::activePopupWidget() == nullptr, "AH15 没有弹出层占着 Esc");

            const auto task = activity->begin(Tasks::TaskKind::Launch, QStringLiteral("Esc cancel"), releaseId);
            QApplication::processEvents();
            launch->setStartingForTest(true);
            check(w.taskCancelWouldWorkForTest(activity->tasks()->snapshot(task)),
                  "AH15 这一档「取消得动」（流程在半路、进程还没起来）");
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&w, &esc);
            QApplication::processEvents();
            check(launch->abortRequestedForTest(), "AH15 启动中按 Esc 把取消请求交给启动流程",
                  QStringLiteral("starting=%1 popup=%2 overlayOpen=%3")
                      .arg(launch->isStarting())
                      .arg(QApplication::activePopupWidget() != nullptr)
                      .arg(w.ui()->overlay() != nullptr && w.ui()->overlay()->isOpen()));
            check(launch->cancelSourceForTest() == LaunchController::StartCancelSource::TaskCenter,
                  "AH15 Esc 和启动卡片上那颗「取消」同一条路（来源 = 任务中心）");
            launch->setStartingForTest(false);
            if (Tasks::isActiveState(activity->tasks()->snapshot(task).state)) {
                activity->tasks()->cancel(task);
            }
        }
    }

    // ---- AH16 键盘入口：F5 点当前页那颗刷新 ----
    //
    // F5 走的是 QShortcut（`QKeySequence::Refresh`）：这里按它自己的信号驱动一次，
    // 判据是"当前页那颗刷新被点了、换到没有刷新的页面就不再点"。产品那条接线先摘掉，
    // 免得这一下真去发请求（这一跑末尾会核对假外网有没有收到没路由的地址）。
    {
        QShortcut *f5 = nullptr;
        for (QShortcut *shortcut : w.findChildren<QShortcut *>()) {
            if (shortcut->key() == QKeySequence(QKeySequence::Refresh)) {
                f5 = shortcut;
                break;
            }
        }
        check(f5 != nullptr, "AH16 主窗口上挂着 F5 那一条快捷键");

        if (QTabWidget *tabs = w.ui()->getTabWidget()) {
            tabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
            QApplication::processEvents();
        }
        QPushButton *refresh = w.ui()->getDownloadHeaderRefreshButton();
        check(refresh != nullptr && refresh->isVisible() && refresh->isEnabled(),
              "AH16 下载页页头那颗刷新露着、点得动");
        if (f5 != nullptr && refresh != nullptr) {
            QObject::disconnect(refresh, nullptr, nullptr, nullptr);
            int clicks = 0;
            QObject::connect(refresh, &QPushButton::clicked, &w, [&clicks] { ++clicks; });
            QMetaObject::invokeMethod(f5, "activated");
            QApplication::processEvents();
            check(clicks == 1, "AH16 下载页上按 F5 点一次页头那颗刷新",
                  QStringLiteral("%1 次").arg(clicks));

            // 换到没有刷新按钮的那一页：F5 什么都不做（而不是回头点上一页那颗）。
            if (QTabWidget *tabs = w.ui()->getTabWidget()) {
                tabs->setCurrentIndex(MainWindowUI::toolboxTabIndex());
                QApplication::processEvents();
            }
            QMetaObject::invokeMethod(f5, "activated");
            QApplication::processEvents();
            check(clicks == 1, "AH16 没有刷新按钮的页面上 F5 什么都不做",
                  QStringLiteral("%1 次").arg(clicks));
        }
    }

    // ---- AH12 收尾：会话一份不剩、账号清掉、目录换回来 ----
    stopAllSessions();
    const bool empty = pumpUntil([&manager] { return manager->runningGames().isEmpty(); }, 5000);
    check(empty, "AH12 会话全停（留一个 holder 会锁住这个 exe，下次构建写不进去）",
          QString::number(manager->runningGames().size()));

    // NOTE: "会话全停之后那一格点不开"这条钉在 AH6（最后一份会话停下来的那一刻就量），
    // 这里不再抄一遍 —— 同一个判据写两处，改一处忘一处就只剩一条是绿的。

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "AH12 这一节写的那些账号清干净了（后面不该知道它们存在过）");

    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "AH12 数据根目录换回来了");
}

