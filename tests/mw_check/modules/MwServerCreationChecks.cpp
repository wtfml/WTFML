#include "Support.hpp"
#include "ListRowDelegate.hpp"
#include "VersionIcons.hpp"
#include "ServerSettingsPage.hpp"
int MwServerCreationChecks(const QDir &root) {
    // 判据层（洗名字 / 认档 / 读写 meta）在 verify 的 ZW / ZX 段；这一节只验"这一按接出去
    // 是什么效果"。服务端根目录按用户的答复落在 `<数据目录>/Servers`（一个自己的文件夹，
    // 不碰 `versions/`、也不和客户端的 `servers.dat` 混）。
    {
        const QString originalDir = AppPaths::minecraftDir();
        QTemporaryDir root;
        check(root.isValid(), "T39 前提：拿得到一个临时的数据根目录");
        // 数据根目录**两头都要改**（理由见 T19/T36）：MainWindow 构造里会拿盘上那个「当前
        // 游戏文件夹」（QSettings `Launcher/CurrentFolder`）再把 AppPaths 改一次 —— 只改
        // 这一处的话，构造函数一跑数据目录就飘回 exe 工作目录那份 `.minecraft` 去了，
        // 这一节往盘上摆的服务端全落在那儿。收尾要**逐字还原**那两个键。
        QSettings sandbox;
        const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
        const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
        sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
        sandbox.sync();
        AppPaths::setMinecraftDir(root.path());
        check(!QDir(AppPaths::serversDir()).exists(),
              "T39 前提：起手一个 servers/ 都没有（服务端根目录按需创建，不预建）");

        MainWindow w;
        w.show();
        QApplication::processEvents();
        {
            auto *download = byObjectName<QPushButton>(&w, MainWindowUI::downloadNewServerButtonName());
            auto *tabs = byObjectName<QTabWidget>(&w, MainWindowUI::tabWidgetName());
            auto *serverStack =
                w.findChild<QStackedWidget *>(MainWindowUI::downloadServerStackName());
            const int oldTab = tabs ? tabs->currentIndex() : 0;
            if (download) { download->setProperty("selectedVersion", QStringLiteral("1.21.1")); download->click(); }
            QApplication::processEvents();
            // 服务端下载和游戏下载**一个形状**：点一个版本 = 进「服务端」那一栏的第二层
            // （不是另开一页、不是覆盖层）。所以这一按当场就到位，不用再等嵌套循环。
            auto *page = byObjectName<NewServerPage>(&w, MainWindowUI::newServerPageName());
            auto *version = page ? page->findChild<QLineEdit *>(MainWindowUI::newServerGameVersionEditName()) : nullptr;
            check(download && page && !page->isWindow() && version &&
                      version->text() == QStringLiteral("1.21.1") && version->isReadOnly() &&
                      tabs && tabs->currentIndex() == MainWindowUI::downloadTabIndex() &&
                      serverStack &&
                      serverStack->currentIndex() == MainWindowUI::downloadServerFormIndex(),
                  "server download config uses the selected version and lives in the download form layer");
            if (page) page->reject();
            QApplication::processEvents();
            check(serverStack &&
                      serverStack->currentIndex() == MainWindowUI::downloadServerListIndex(),
                  "leaving the server form returns to the version list layer");
            if (download) download->setProperty("selectedVersion", QVariant());
            if (tabs && oldTab >= 0 && oldTab < tabs->count()) tabs->setCurrentIndex(oldTab);
        }

        auto *clientChip = byObjectName<QPushButton>(
            &w, MainWindowUI::instanceModeButtonName(MainWindowUI::InstanceMode::Client));
        auto *serverChip = byObjectName<QPushButton>(
            &w, MainWindowUI::instanceModeButtonName(MainWindowUI::InstanceMode::Server));
        auto *newServerBtn = byObjectName<QPushButton>(&w, MainWindowUI::newServerButtonName());
        auto *stack = byObjectName<QStackedWidget>(&w, MainWindowUI::instanceModeStackName());
        auto *serverTree = byObjectName<QTreeWidget>(&w, MainWindowUI::serverListName());
        auto *emptyLabel = byObjectName<QLabel>(&w, MainWindowUI::serverEmptyLabelName());
        const bool wired = clientChip != nullptr && serverChip != nullptr &&
                           newServerBtn != nullptr && stack != nullptr &&
                           serverTree != nullptr && emptyLabel != nullptr;
        check(wired,
              "T39.0 前提：模式那两颗 chip / 「新建服务端…」/ 换页容器 / 服务端那棵树 / "
              "空状态那句都找得到");
        if (wired) {
            // ---------- ① 起手：一个服务端都没有 ----------
            // 这里读的是 `isHidden()` 而**不是** `isVisible()`：起手界面停在客户端那一页，
            // 服务端那一页整个是藏着的，于是它下面任何一个控件的 `isVisible()` 都是 false
            // —— 拿它判"空状态那句摆着没有"会假红。`isHidden()` 只问"这一个控件自己被
            // `setVisible(false)` 过没有"（`refreshServerList()` 用 `setVisible()` 摆它），
            // 正是这里要问的那件事。
            check(!emptyLabel->isHidden() && serverTree->topLevelItemCount() == 0,
                  "★ T39.1 一个服务端都没有：空状态那句摆着、树里一行都没有",
                  QStringLiteral("空状态被藏=%1 行数=%2")
                      .arg(emptyLabel->isHidden() ? 1 : 0)
                      .arg(serverTree->topLevelItemCount()));

            // ---------- ② 点「服务端」那颗 ⇒ 切页 **而且重扫盘** ----------
            // 先**绕开界面**直接用判据层往盘上摆一个，再点 chip：要是这一按没有重扫，列表
            // 就还是空的 —— 这一条量的正是"切过去时真的重扫了一遍"（不是只切了页）。
            ServerModel::Info info;
            info.name = QStringLiteral("甲服务端");
            info.flavor = ServerInstall::Flavor::Vanilla;
            info.flavorKnown = true;
            info.gameVersion = QStringLiteral("1.20.1");
            QString problem;
            check(ServerRegistry::createServer(QStringLiteral("甲服务端"), info, &problem),
                  "T39.2 前提：先用判据层在盘上摆一个服务端", problem);
            serverChip->click();
            QApplication::processEvents();
            check(stack->currentIndex() == 1, "★ T39.2 点「服务端」⇒ 界面切到服务端那一页");
            check(serverTree->topLevelItemCount() == 1 && !emptyLabel->isVisible(),
                  "★★ T39.2 而且**当场重扫了盘**：刚摆上去那一个出现了、空状态那句收起来"
                  "（不重扫的话列表会一直停在「什么都没有」）",
                  QStringLiteral("行数=%1").arg(serverTree->topLevelItemCount()));

            // ---------- ③ 那一行画的是什么 ----------
            {
                QTreeWidgetItem *row = serverTree->topLevelItem(0);
                check(row != nullptr &&
                          row->data(0, Qt::UserRole).toString() == QStringLiteral("甲服务端"),
                      "★ T39.3 那一行的身份（UserRole）是 serverId（= 目录名），不是显示名");
                const QString sub =
                    row != nullptr ? row->data(0, ListRowDelegate::SubtitleRole).toString() : QString();
                check(row != nullptr && row->text(0).contains(QStringLiteral("甲服务端")) &&
                          sub.contains(ServerModel::statusName(ServerModel::Status::NotInstalled)),
                      "★★ T39.3 第一行是显示名、第二行有状态；这一档**还没装 server.jar** ⇒ 是「未安装」",
                      row != nullptr ? row->text(0) + QStringLiteral(" / ") + sub : QStringLiteral("<没有行>"));
                check(row != nullptr && !row->isDisabled() &&
                          row->data(0, ListRowDelegate::LaunchKindRole).toInt() ==
                              static_cast<int>(ListRowDelegate::LaunchKind::Repair) &&
                          row->data(0, ListRowDelegate::LaunchTextRole).toString() ==
                              MainWindowUI::serverRepairText(),
                      "★ T39.3 没装好的那一行**点得动**（不置灰：单击进设置、第三格是「修复」）");
                check(row != nullptr &&
                          row->toolTip(0).contains(QDir::toNativeSeparators(
                              AppPaths::serverDir(QStringLiteral("甲服务端")))),
                      "★ T39.3 tooltip 里有那个目录的绝对路径（悬停提示里给目录）");
            }

            // ---------- ④ 丢一个 server.jar 进去 ⇒ 再扫就是「可启动」 ----------
            {
                QFile jar(AppPaths::serverDir(QStringLiteral("甲服务端")) +
                          QStringLiteral("/server.jar"));
                bool made = false;
                if (jar.open(QIODevice::WriteOnly)) {
                    jar.close();
                    made = true;
                }
                check(made, "T39.4 前提：往那个目录里丢了一个 server.jar");
                clientChip->click();
                serverChip->click();  // 再切一次 = 再重扫一遍
                QApplication::processEvents();
                QTreeWidgetItem *row = serverTree->topLevelItem(0);
                const QString sub =
                    row != nullptr ? row->data(0, ListRowDelegate::SubtitleRole).toString() : QString();
                check(row != nullptr &&
                          sub.contains(ServerModel::statusName(ServerModel::Status::Ready)) &&
                          row->data(0, ListRowDelegate::LaunchTextRole).toString() ==
                              MainWindowUI::serverStartText(),
                      "★★ T39.4 丢进 server.jar 再扫 ⇒ 这一档变成「可启动」、第三格是「启动」",
                      row != nullptr ? sub : QStringLiteral("<没有行>"));

                // ---- 单击一行 = 进这个服务端的设置（和客户端一致）；点在行尾「更多」那格上不算 ----
                // 切到实例页并放宽窗口：行要有真实宽度，行身那个点才不会落到行尾三格上
                if (auto *tabs = w.findChild<QTabWidget *>()) {
                    tabs->setCurrentIndex(MainWindowUI::instancesTabIndex());
                }
                w.resize(1000, 700);
                QApplication::processEvents();
                const QRect rowRect = serverTree->visualItemRect(row);
                const auto clickAt = [&](const QPoint &at) {
                    QMouseEvent press(QEvent::MouseButtonPress, at, serverTree->viewport()->mapToGlobal(at),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, at, serverTree->viewport()->mapToGlobal(at),
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(serverTree->viewport(), &press);
                    QApplication::sendEvent(serverTree->viewport(), &release);
                    QApplication::processEvents();
                };
                check(w.findChild<ServerSettingsPage *>() == nullptr ||
                          !w.findChild<ServerSettingsPage *>()->isVisible(),
                      "T39.4b 前提：服务端设置页还没开");
                const QPoint bodyPoint(rowRect.left() + 20, rowRect.center().y());
                check(bodyPoint.x() < ListRowDelegate::actionRect(rowRect, ListRowDelegate::Action::Folder).left(),
                      "T39.4b 前提：行身那个点落在行尾三格的左边", QString::number(rowRect.width()));
                clickAt(bodyPoint);
                auto *settingsPage = w.findChild<ServerSettingsPage *>();
                check(settingsPage != nullptr && settingsPage->isVisible(),
                      "★★ T39.4b 单击服务端行（行身）⇒ 打开这个服务端的设置页");
                if (settingsPage != nullptr) {
                    settingsPage->reject();
                    QApplication::processEvents();
                }
            }

            // ---------- ④b 每一档对应哪张图 + 页头那句计数 == 列表行数 ----------
            {
                struct FlavorIcon { ServerInstall::Flavor flavor; int mark; };
                // loaderMark 表：0 Fabric / 1 Forge / 2 NeoForge / 3 OptiFine / 4 Quilt / 5 草方块。
                // 服务端只有这四档有自己的图；Paper / Purpur / Spigot（和原版）是草方块，不借 OptiFine / Quilt 的。
                const FlavorIcon expected[] = {
                    {ServerInstall::Flavor::Paper, 5},    {ServerInstall::Flavor::Purpur, 5},
                    {ServerInstall::Flavor::Fabric, 0},   {ServerInstall::Flavor::Forge, 1},
                    {ServerInstall::Flavor::NeoForge, 2}, {ServerInstall::Flavor::Quilt, 4},
                    {ServerInstall::Flavor::Spigot, 5},
                };
                QStringList ids;
                for (const FlavorIcon &f : expected) {
                    ServerModel::Info info;
                    info.name = QStringLiteral("图标-") + ServerInstall::flavorToken(f.flavor);
                    info.flavor = f.flavor;
                    info.flavorKnown = true;
                    info.gameVersion = QStringLiteral("1.20.1");
                    QString why;
                    check(ServerRegistry::createServer(info.name, info, &why),
                          "T39.4c 前提：摆下这一档的服务端", why);
                    ids << info.name;
                }
                clientChip->click();
                serverChip->click();
                QApplication::processEvents();
                const auto imageOf = [](const QVariant &icon) {
                    return icon.value<QIcon>().pixmap(96, 96).toImage();
                };
                for (const FlavorIcon &f : expected) {
                    const QString id = QStringLiteral("图标-") + ServerInstall::flavorToken(f.flavor);
                    QTreeWidgetItem *hit = nullptr;
                    for (int i = 0; i < serverTree->topLevelItemCount(); ++i) {
                        if (serverTree->topLevelItem(i)->data(0, Qt::UserRole).toString() == id) {
                            hit = serverTree->topLevelItem(i);
                        }
                    }
                    check(hit != nullptr &&
                              imageOf(hit->data(0, ListRowDelegate::CoverRole)) ==
                                  QIcon(VersionIcons::loaderMark(f.mark, 48)).pixmap(96, 96).toImage(),
                          "★★ T39.4c 每一档端类型挂对应的图（Fabric/Forge/NeoForge/Quilt 各一张，Paper/Purpur/Spigot 草方块）",
                          ServerInstall::flavorToken(f.flavor));
                }
                // 前提：5 张图两两不同（不然上面的逐档比较分不出借错图）
                bool allDistinct = true;
                for (const int i : {0, 1, 2, 4, 5}) {
                    for (const int j : {0, 1, 2, 4, 5}) {
                        allDistinct = allDistinct && (i == j || VersionIcons::loaderMark(i, 48).toImage() !=
                                                                   VersionIcons::loaderMark(j, 48).toImage());
                    }
                }
                check(allDistinct, "T39.4c 前提：Fabric / Forge / NeoForge / Quilt / 草方块五张图两两不同");
                // 页头计数 = 列表行数
                auto *folderLabel = byObjectName<QLabel>(&w, MainWindowUI::instanceFolderLabelName());
                check(folderLabel != nullptr &&
                          folderLabel->text().contains(
                              QStringLiteral("（%1 个服务端）").arg(serverTree->topLevelItemCount())),
                      "★★ T39.4d 页头那句「（N 个服务端）」的 N 等于列表行数",
                      folderLabel != nullptr ? folderLabel->text() + QStringLiteral(" / 行数 ") +
                                                   QString::number(serverTree->topLevelItemCount())
                                             : QStringLiteral("<没有标签>"));
                for (const QString &id : ids) {
                    QDir(AppPaths::serverDir(id)).removeRecursively();
                }
                clientChip->click();
                serverChip->click();  // 清掉临时的几个，后面几节按原来的行数数
                QApplication::processEvents();
            }

            // ---------- ⑤ 点「新建服务端…」：收页之后真的建了目录 ----------
            {
                const QString newId = QStringLiteral("新服务端");
                check(!QDir(AppPaths::serverDir(newId)).exists(),
                      "T39.5 前提：这个名字的目录起手不存在");
                // 表单**不是**单独开的一页：它住在「下载 → 服务端」那一栏的第二层里，
                // 点开当场就到位（没有嵌套循环），填完再按「创建」。
                newServerBtn->click();
                QApplication::processEvents();
                auto *serverStack = w.findChild<QStackedWidget *>(MainWindowUI::downloadServerStackName());
                check(serverStack && serverStack->currentIndex() == MainWindowUI::downloadServerListIndex(),
                      "management new server starts at version selection even after a previous form");
                auto *download = byObjectName<QPushButton>(&w, MainWindowUI::downloadNewServerButtonName());
                if (download) {
                    download->setProperty("selectedVersion", QStringLiteral("1.20.1"));
                    download->click();
                }
                QApplication::processEvents();
                auto *page = byObjectName<NewServerPage>(&w, MainWindowUI::newServerPageName());
                check(page != nullptr && !page->isWindow(),
                      "★ T39.5 点「新建服务端…」开的就是那张表单（按 objectName 找得到，"
                      "嵌在内容区那一层里而不是另开一个窗）");
                if (page != nullptr) {
                    // 用**真的控件**填（不是 `setDefaults()`）：那两个入口套了 QSignalBlocker，
                    // 只有走 `setText()` 才会发 `fieldsChanged()`、让翻译点重算计划 —— 「创建」
                    // 也才会从"点不动"变成能点（名字/版本都填了，计划才成立）。
                    if (auto *nameEdit =
                            page->findChild<QLineEdit *>(MainWindowUI::newServerNameEditName())) {
                        nameEdit->setText(QStringLiteral("新服务端"));
                    }
                    if (auto *verEdit = page->findChild<QLineEdit *>(
                            MainWindowUI::newServerGameVersionEditName())) {
                        verEdit->setText(QStringLiteral("1.20.1"));
                    }
                    if (auto *btn = page->findChild<QPushButton *>(
                            MainWindowUI::newServerCreateButtonName())) {
                        check(btn->isEnabled(),
                              "★ T39.5 填了名字 + 游戏版本之后「创建」能点了（翻译点把那句问题"
                              "撤了）");
                        btn->click();  // = accept()，MainWindow 接住之后去建目录、写 meta
                    }
                }
                QApplication::processEvents();
                check(QDir(AppPaths::serverDir(newId)).exists() &&
                          QFile::exists(AppPaths::serverMetaPath(newId)),
                      "★★ T39.5 收下那一页之后：`servers/新服务端/` 真的建出来了、里面有一份 "
                      "`.wtfml-server`（建这个服务端不是只改内存）");
                check(stack->currentIndex() == 1,
                      "★ T39.5 建完自动切到「服务端」那一页（用户刚建的那一个就在那儿）");
                check(serverTree->topLevelItemCount() == 2,
                      "★ T39.5 新那一个也出现在列表里了（切页顺手重扫过）",
                      QString::number(serverTree->topLevelItemCount()));
            }

            // ---------- ⑥ 下载页「服务端」那一栏那颗「新建服务端…」走同一条路 ----------
            //
            // 下载页左栏补了「服务端」一栏，那一页只有
            // 一句说明 + 一颗「新建服务端…」。它和实例页那颗**必须**是同一个动作 ——
            // 另写一条实现的话，两边迟早会在"建什么、建到哪儿"上分家。
            {
                auto *categories = byObjectName<QListWidget>(
                    &w, MainWindowUI::downloadCategoryListName());
                auto *downloadNewServer = byObjectName<QPushButton>(
                    &w, MainWindowUI::downloadNewServerButtonName());
                check(categories != nullptr && downloadNewServer != nullptr,
                      "★ T39.5b 前提：下载页分类列表和「服务端」那一栏那颗按钮都找得到");
                if (categories != nullptr && downloadNewServer != nullptr) {
                    auto *serverStack =
                        w.findChild<QStackedWidget *>(MainWindowUI::downloadServerStackName());
                    categories->setCurrentRow(MainWindowUI::downloadCategoryServerIndex());
                    QApplication::processEvents();
                    downloadNewServer->setProperty("selectedVersion", QStringLiteral("1.20.1"));
                    downloadNewServer->click();
                    QApplication::processEvents();
                    auto *page = byObjectName<NewServerPage>(&w, MainWindowUI::newServerPageName());
                    check(page != nullptr &&
                              serverStack &&
                              serverStack->currentIndex() ==
                                  MainWindowUI::downloadServerFormIndex(),
                          "★★ T39.5b 下载页「服务端」那一栏那颗按钮开的就是「新建服务端」"
                          "那张表单（和实例页那颗同一个动作）");
                    if (page != nullptr) {
                        page->reject();  // 退回版本列表那一层（这一条只管"开的是哪一张表单"，不建东西）
                        QApplication::processEvents();
                    }
                    check(serverTree->topLevelItemCount() == 2,
                          "T39.5b 收页之后服务端列表一条都没多（reject 那条路不建东西）",
                          QString::number(serverTree->topLevelItemCount()));
                }
            }
        }

        w.close();
        QApplication::processEvents();
        sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
        sandbox.sync();
        AppPaths::setMinecraftDir(originalDir);
        check(AppPaths::minecraftDir() == originalDir, "T39.9 数据根目录换回来了");
    }
    return 0;
}



