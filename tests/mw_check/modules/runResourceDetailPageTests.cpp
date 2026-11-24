#include "Support.hpp"
void runResourceDetailPageTests() {
    FakeResourceApi api;
    check(api.start(), "ZJ0 本地假接口起来了");
    // 两张资源：名字和游戏版本**都不一样** —— 第 ② 条要的就是"点第二行不会开成第一行"。
    api.searchBody = QByteArrayLiteral(
        "{\"data\":[{\"id\":1546263,\"name\":\"Arizona\",\"slug\":\"arizona\","
        "\"summary\":\"一句简介\",\"downloadCount\":642,"
        "\"authors\":[{\"name\":\"birdmuncher\"}],\"categories\":[{\"name\":\"Technology\"}],"
        "\"latestFilesIndexes\":[{\"gameVersion\":\"1.21.1\",\"modLoader\":4},"
        "{\"gameVersion\":\"1.20.1\",\"modLoader\":4}]},"
        "{\"id\":2981874,\"name\":\"第二个模组\",\"slug\":\"second\","
        "\"summary\":\"另一句简介\",\"downloadCount\":12,"
        "\"authors\":[{\"name\":\"someone\"}],\"categories\":[{\"name\":\"Storage\"}],"
        // 第二行**故意多声明一条快照**（`1.21.2-pre1`）：ZJ9 那一节要在这条线上量
        // 「显示快照版」那颗勾选框（不勾 ⇒ 折成一颗 1.21.2、勾上 ⇒ 它自己一颗）——
        // 第一行那条线上没有快照，量不了这件事。两份接口清单都从这一份假接口来，
        // 所以下面那三条版本（filesBody）两行看到的是同一份。
        "\"latestFilesIndexes\":[{\"gameVersion\":\"1.21.1\",\"modLoader\":1},"
        "{\"gameVersion\":\"1.21.2-pre1\",\"modLoader\":1}]}],"
        "\"pagination\":{\"index\":0,\"pageSize\":20,\"resultCount\":2,\"totalCount\":2}}");
    // 三条版本。**id 刻意和行号不一样**（5100007 这种）：钥匙要是拿行号当 id，点第三行
    // 就会认成第一版，而那个错一个字都不报（见第 ⑤ 条）。
    // 一条声明 1.21.1、一条声明 1.21.1 但是测试版、一条只声明 1.20.1 —— 这样"筛"和"不筛"
    // 的组数/条数都不一样，chip 那几条才有得量。
    api.filesBody = QByteArrayLiteral(
        "{\"data\":["
        "{\"id\":5100003,\"displayName\":\"1.0.0 for Forge 1.21.1\","
        "\"fileName\":\"arizona-1.0.0.jar\",\"releaseType\":1,"
        "\"fileDate\":\"2026-09-01T10:00:00Z\",\"downloadCount\":120,\"fileLength\":1024,"
        "\"gameVersions\":[\"1.21.1\",\"Forge\",\"Client\"],"
        "\"downloadUrl\":\"http://127.0.0.1:1/arizona-1.0.0.jar\","
        "\"hashes\":[{\"value\":\"aa\",\"algo\":1}]},"
        "{\"id\":5100007,\"displayName\":\"1.1.0-beta for Forge 1.21.1\","
        "\"fileName\":\"arizona-1.1.0-beta.jar\",\"releaseType\":2,"
        "\"fileDate\":\"2026-09-10T10:00:00Z\",\"downloadCount\":5,\"fileLength\":1024,"
        "\"gameVersions\":[\"1.21.1\",\"Forge\"],"
        "\"downloadUrl\":\"http://127.0.0.1:1/arizona-1.1.0-beta.jar\"},"
        "{\"id\":5100011,\"displayName\":\"0.9.0 for Forge 1.20.1\","
        "\"fileName\":\"arizona-0.9.0.jar\",\"releaseType\":1,"
        "\"fileDate\":\"2026-06-01T10:00:00Z\",\"downloadCount\":80,\"fileLength\":1024,"
        "\"gameVersions\":[\"1.20.1\",\"Forge\"],"
        // 这一版声明了一条**必装前置**（relationType=3）：点「1.20.1」那一颗 chip 之后它
        // 就成了最上面那一版，中间那一行要去问「那个项目叫什么」（R3 量的是这一条路）。
        "\"dependencies\":[{\"modId\":999001,\"relationType\":3}],"
        "\"downloadUrl\":\"http://127.0.0.1:1/arizona-0.9.0.jar\"}]}");

    // 接口根地址是 MainWindow **构造那一刻**读进去的，所以先写 ini 再造窗口（和 ZI 同一个做法）。
    const QString kBaseKey = QStringLiteral("Launcher/CurseForgeApiBase");
    const auto restoreBase = [kBaseKey] {
        QSettings sandbox;
        sandbox.remove(kBaseKey);
        sandbox.sync();
    };
    {
        QSettings sandbox;
        sandbox.setValue(kBaseKey, api.baseUrl());
        sandbox.sync();
    }
    // 身份：一个**新加的离线账号**（外观走本地，不发外网请求）。
    const QString accountName = QStringLiteral("Detail");
    {
        QString offlineError;
        check(AccountStore::addOffline(accountName, &offlineError),
              "ZJ0 当前身份换成一个离线账号（这一节一个外网请求都不发）", offlineError);
    }

    // 伪造鼠标事件（按下 + 松开，**不**顺手把事件泵干净）：行矩形是视口坐标系的。
    // NOTE: 这里刻意不 processEvents —— 点行体那一下产品要"排到下一轮事件循环"才开页，
    // 而下面的检查员必须**排在它后面**。中间泵一次，顺序就反了（先跑检查员、
    // 那时候页还没开）。泵的那一下交给调用方。
    const auto clickRow = [](QTreeWidget *tree, const QPoint &pos) {
        QWidget *viewport = tree->viewport();
        const QPointF local(pos);
        const QPointF global(viewport->mapToGlobal(pos));
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &release);
    };

    {
        MainWindow w;
        w.show();
        // 取证用：设了 WTFML_DETAIL_SHOT_DIR 就把窗口缩到最小尺寸 900×590，并在下面 ZJ5 的几个
        // 节点各存一张整窗截图（真 MainWindow、离屏）。不设就什么都不变。
        const QString detailShotDir = qEnvironmentVariable("WTFML_DETAIL_SHOT_DIR");
        if (!detailShotDir.isEmpty()) {
            QDir().mkpath(detailShotDir);
            w.resize(900, 590);
        }
        const auto saveDetailShot = [&w, detailShotDir](const QString &name) {
            if (!detailShotDir.isEmpty()) {
                w.grab().save(QDir(detailShotDir).filePath(name + QStringLiteral(".png")));
            }
        };
        QApplication::processEvents();

        MainWindowUI *ui = w.ui();
        const QString key = ResourceCatalog::kindKey(ResourceCatalog::Kind::Mod);
        QComboBox *sourceCombo = ui == nullptr ? nullptr : ui->getDownloadFilterSourceCombo(key);
        QPushButton *searchButton = ui == nullptr ? nullptr : ui->getDownloadFilterSearchButton(key);
        QTreeWidget *tree = ui == nullptr ? nullptr : ui->getDownloadResourceTree(key);
        QTabWidget *tabs = ui == nullptr ? nullptr : ui->getTabWidget();
        OverlayHost *overlay = ui == nullptr ? nullptr : ui->overlay();
        QComboBox *installed = ui == nullptr ? nullptr : ui->getInstalledVersionCombo();
        const bool haveAll = sourceCombo != nullptr && searchButton != nullptr && tree != nullptr
                             && tabs != nullptr && overlay != nullptr && installed != nullptr;
        check(haveAll, "ZJ1 模组那一页的控件都在（来源框 / 搜索键 / 结果树 / 选项卡 / 覆盖层 /"
                       "「已安装版本」下拉框）");
        if (!haveAll) {
            restoreBase();
            return;
        }

        // 结果树在**哪一页**：下面要按坐标伪造鼠标事件，而藏在非当前页里的控件没有真实几何
        // （行矩形会是空的）。不写死页号 —— 从控件往上找它在选项卡里的那一页。
        // NOTE: 不能"找到 parentWidget() == tabs 的那一层再 indexOf"：选项卡自己那一叠页面
        // 外面还套着一个 QStackedWidget（addTab 装进去的），于是"直属于 tabWidget 的
        // 那个控件"是那个 stack，它 IndexOf 出来是 -1。逐层问 indexOf 才对。
        int tabIndex = -1;
        for (QWidget *walk = tree; walk != nullptr && tabIndex < 0; walk = walk->parentWidget()) {
            tabIndex = tabs->indexOf(walk);
        }
        check(tabIndex >= 0, "ZJ1 资源树所在那一页在选项卡里找得到（下面要把它翻成当前页）",
              QString::number(tabIndex));
        if (tabIndex < 0) {
            restoreBase();
            return;
        }
        tabs->setCurrentIndex(tabIndex);
        QApplication::processEvents();

        // 来源**先**切成 CurseForge：下面翻左栏那一列分类时，万一那一页顺手发一发
        // 「最热门的」，那一发也得落在假接口上（Modrinth 那条根地址是写死的，喂不过去，
        // 会真打到 api.modrinth.com）。
        const int cfIndex =
            sourceCombo->findData(ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge));
        check(cfIndex >= 0, "ZJ1 来源框里找得到 CurseForge");
        sourceCombo->setCurrentIndex(cfIndex);
        QApplication::processEvents();

        QListWidget *categories = ui->getDownloadCategoryList();
        QStackedWidget *stack = ui->getDownloadStack();
        int kindRow = -1;
        for (int i = 0; categories != nullptr && i < categories->count(); ++i) {
            if (categories->item(i)->data(Qt::UserRole).toString() == key) {
                kindRow = i;
                break;
            }
        }
        check(kindRow >= 0 && stack != nullptr, "ZJ1 左栏那一列里找得到「模组」那一行",
              QString::number(kindRow));
        if (kindRow < 0) {
            restoreBase();
            return;
        }
        categories->setCurrentRow(kindRow);
        QApplication::processEvents();
        check(tree->isVisible() && tree->viewport()->width() > 100 &&
                  tree->viewport()->height() >= MainWindowUI::versionTreeRowHeight(),
              "ZJ1 翻到「模组」那一页之后这张表真的在屏幕上（视口有真实几何 —— 不然后面按坐标"
              "伪造的鼠标事件一发都落不到行上，而那时候红的是一堆看起来毫不相干的断言）",
              QStringLiteral("视口 %1x%2 可见=%3")
                  .arg(tree->viewport()->width())
                  .arg(tree->viewport()->height())
                  .arg(tree->isVisible() ? 1 : 0));

        // 上一句可能顺手带出一发（那一页自己「获取最热门的」），所以先清账再点这一发。
        api.requests.clear();
        searchButton->click();
        const bool filled = pumpUntil([tree] { return tree->topLevelItemCount() == 2; }, 10000);
        check(filled, "ZJ2 两条结果都摆上了（这一条不通，下面每一条都无从谈起）",
              QString::number(tree->topLevelItemCount()));
        if (!filled) {
            restoreBase();
            return;
        }

        QTreeWidgetItem *first = tree->topLevelItem(0);
        QTreeWidgetItem *second = tree->topLevelItem(1);
        tree->scrollToItem(first);
        const QRect firstRect =
            first == nullptr ? QRect() : tree->visualItemRect(first);
        const QRect arrowZone = VersionTreeDelegate::arrowRect(firstRect);
        check(first != nullptr && first->text(0) == QStringLiteral("Arizona") &&
                  first->childCount() == 2 && !first->isExpanded(),
              "ZJ3 第一行是那个模组、底下挂着两个游戏版本、而且是收着的",
              QStringLiteral("子项 %1").arg(first == nullptr ? -1 : first->childCount()));
        check(!firstRect.isEmpty() &&
                  firstRect.height() == VersionTreeDelegate::resourceRowHeight() &&
                  firstRect.contains(arrowZone.center()) &&
                  arrowZone.left() > firstRect.center().x(),
              "ZJ3 行矩形和箭头那一格都是真的（那一页是当前页、这一行也滚到了可见）"
              "—— 不然后面几条量的都是空气",
              QStringLiteral("行 %1x%2 箭头格 %3x%4")
                  .arg(firstRect.width())
                  .arg(firstRect.height())
                  .arg(arrowZone.width())
                  .arg(arrowZone.height()));

        // ---- ZJ3. 箭头那一格：点它是展开，**不是**进这一页 ----

        clickRow(tree, arrowZone.center());
        QApplication::processEvents();
        check(!overlay->isOpen(),
              "ZJ3 点箭头那一格**不开详情页**（那一格是展开/收起 —— 判据和代理画箭头是同一个"
              "arrowRect，两处各算一份就会错开，而错开的形状是「看着点的是箭头，其实进了一页」）");
        check(first->isExpanded(), "ZJ3 而它真的展开了（说明这一下确实落到了那一行上）");

        // ---- ZJ4. 点行体：开的是这一行那一页 ----
        const int requestsBeforeOpen = api.requests.size();
        // 看门狗：万一这一条路没把页面收掉，这一发在 12 秒后把它收掉。挂住比红更糟 ——
        // 挂住的话这一整跑一条结果都拿不到。
        // NOTE: 收的必须是**返回箭头**（= reject），不能是 `closeOverlay()`：后者不经过
        // 页面自己的 reject，收页钩子和页面收尾那一套都不跑。
        // NOTE: 用**停得掉**的定时器，不是 `QTimer::singleShot`：这一节正常走完只要几十
        // 毫秒，留一发 12 秒的定时器在队列里，它就会在**后面某一节**响 —— 那时候
        // 开着的可能是别人的一层页或者一条提示，它照着"返回箭头"按下去，红的却是
        // 那一节（测试自己两只手打架，见 `g_modalHandlerGeneration` 那段同一个道理）。
        QTimer watchdog;
        watchdog.setSingleShot(true);
        watchdog.setInterval(12000);
        QObject::connect(&watchdog, &QTimer::timeout, &w, [overlay] {
            if (!overlay->isOpen()) {
                return;
            }
            if (auto *box = overlay->findChild<InlineMessageBox *>()) {
                box->close();  // 提示那一层（它也一样只认 finished）
            }
            if (auto *back =
                    overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            }
        });
        watchdog.start();
        // 检查员：**点完再排**，而且没开起来就**再排自己一轮**。
        // NOTE: 为什么不能排在点之前：产品那句
        // `QTimer::singleShot(0, this, …)` 和这一发是同一个东西 —— 带 context 的
        // 零间隔在 Qt 里走的是 `QMetaObject::invokeMethod(Qt::QueuedConnection)`，
        // 一条投递事件，**不建定时器**（见 qtimer.cpp 的 singleShotImpl：
        // `msec == 0 && receiver` 那一支直接投递）。同一个队列里先排的先跑，于是
        // 点之前排的那一发必然跑在开页那一发**前面**，看到的是"还没开页"。
        // 更坏的是那一页是**建一次留着**的（`m_resourceDetailPage`，见
        // setupResourceDetailPage），`findChild` 会把**上一次**的内容原样交出来 ——
        // 于是"点第二行看到的是第一行的名字"这种真 bug 恰好被这一条盖住。
        int openTries = 0;
        bool inspected = false;
        std::function<void()> inspect;
        inspect = [&]() {
            if (!overlay->pageOpen() && ++openTries <= 50) {
                // 还没开起来：再排自己一轮。两种推迟机制（投递事件 / 真定时器）下都对 ——
                // 真正跑这一发的是**开起来之后**那一趟事件循环。
                QTimer::singleShot(0, &w, inspect);
                return;
            }
            inspected = true;
            ResourceDetailPage *page = w.findChild<ResourceDetailPage *>();
            check(page != nullptr && overlay->pageOpen(),
                  "ZJ4 详情页那一份真的建出来了，而且就开在**覆盖层那一层**上"
                  "（不是独立窗口，见用户那条总目标）",
                  page == nullptr ? QStringLiteral("没有这一份")
                                  : QStringLiteral("有这一份，但没开着"));
            if (page == nullptr) {
                if (auto *back =
                        overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                }
                return;  // 后面每一条都要读这一页，读不了就别往下走
            }
            const QLabel *title =
                overlay->findChild<QLabel *>(OverlayHost::overlayTitleLabelName());
            check(title != nullptr && title->text() == QStringLiteral("Arizona"),
                  "ZJ4 页眉上写的是**这一行**那个资源名（用户要的「上方写名字」）",
                  title == nullptr ? QString() : title->text());
            check(page->titleText() == QStringLiteral("Arizona") &&
                      page->openButtonText() == QStringLiteral("转到 CurseForge"),
                  "ZJ4 头卡那颗按钮跟着这一行的来源走（这一行是 CurseForge 搜出来的）",
                  page->openButtonText());

            // 中间那一排 chip：「全部」+ 这一行声明过的两个游戏版本。
            const QList<QPushButton *> chips = page->chips();
            QStringList chipKeys;
            for (int i = 0; i < chips.size(); ++i) {
                chipKeys.append(page->chipKeyAt(i));
            }
            const auto chipIndexOf = [&page](const QString &wanted) {
                for (int i = 0; i < page->chips().size(); ++i) {
                    if (page->chipKeyAt(i) == wanted) {
                        return i;
                    }
                }
                return -1;
            };
            check(chips.size() == 3 && page->chipKeyAt(0).isEmpty() &&
                      page->chipTextAt(0) == QStringLiteral("全部") &&
                      chipKeys == QStringList({QString(), QStringLiteral("1.21"),
                                               QStringLiteral("1.20")}) &&
                      page->currentChip().isEmpty(),
                  "★ ZJ4 顶排是「全部」+ 这一行声明过的两个**大版本**（1.21、1.20，不是 1.21.1 "
                  "这种细版本），而且停在「全部」上（这两档是**搜索响应里就带着的**，为它不用多发一条请求）",
                  chipKeys.join(QStringLiteral("|")));

            // 版本清单：开页那一发回来之后才有东西。
            const bool loaded = pumpUntil([page] { return page->versionRowCount() == 3; }, 10000);
            check(loaded, "ZJ4 版本清单回来了并摆上了（三条）",
                  QString::number(page->versionRowCount()));
            saveDetailShot(QStringLiteral("detail-all"));
            check(page->placeholderText().isEmpty(),
                  "ZJ4 回来之后那句「正在读版本列表…」撤掉了（留着的话用户会以为一直在读）",
                  page->placeholderText());
            check(page->groupCount() == 2 &&
                      page->groupTitleAt(0) == QStringLiteral("Forge 1.21.1") &&
                      page->groupTitleAt(1) == QStringLiteral("Forge 1.20.1") &&
                      !page->groupExpandedAt(0) && !page->groupExpandedAt(1),
                  "★★ ZJ4 按「加载器 + 游戏版本」分成两组、新的在前，**默认两组都收着**（点标题行才展开）",
                  page->groupTitleAt(0) + QStringLiteral(" / ") + page->groupTitleAt(1));
            check(api.requests.size() == requestsBeforeOpen + 1 &&
                      api.lastRequest().contains(QStringLiteral("/v1/mods/1546263/files")) &&
                      !api.lastRequest().contains(QStringLiteral("gameVersion=")),
                  "ZJ4 开页只发了一发，而且是「整份清单」那一发（地址里**不带** gameVersion ="
                  "不筛）—— 带上它的话，用户看到的是「这一档下的版本」，而不是这个资源的全部",
                  api.lastRequest());

            QTreeWidget *list = page->versionList();
            QTreeWidgetItem *g0 = list == nullptr ? nullptr : list->topLevelItem(0);
            QTreeWidgetItem *r0 = g0 == nullptr ? nullptr : g0->child(0);
            QTreeWidgetItem *r1 = g0 == nullptr ? nullptr : g0->child(1);
            const QString r0Key =
                r0 == nullptr ? QString() : r0->data(0, ResourceDetailPage::rowKeyRole()).toString();
            check(r0 != nullptr && r0Key == QStringLiteral("5100003") &&
                      r0->text(0) == QStringLiteral("1.0.0 for Forge 1.21.1") &&
                      !r0->data(0, MainWindowUI::rowSubTextRole()).toString().isEmpty(),
                  "ZJ4 行上那把钥匙是**版本 id**（不是行号），正文写的是版本号，小字那一截也在",
                  r0Key);
            const QVariant mark0 =
                r0 == nullptr ? QVariant() : r0->data(0, MainWindowUI::rowIconRole());
            const QVariant mark1 =
                r1 == nullptr ? QVariant() : r1->data(0, MainWindowUI::rowIconRole());
            const QPixmap pix0 = mark0.value<QPixmap>();
            const QPixmap pix1 = mark1.value<QPixmap>();
            check(!pix0.isNull() &&
                      pix0.size() == QSize(ResourceDetailMarks::badgeSize(),
                                           ResourceDetailMarks::badgeSize()) &&
                      !pix1.isNull() && pix0.toImage() != pix1.toImage(),
                  "ZJ4 两行的渠道方块不一样（这一版是正式版、那一版是测试版）—— 渠道从接口"
                  "那一路真的传到了这一格",
                  QStringLiteral("%1x%2").arg(pix0.width()).arg(pix0.height()));

            // ---- ZJ5. 点一颗 chip：本地筛，一个请求都不发 ----
            const int requestsBeforeChip = api.requests.size();
            const int chip121 = chipIndexOf(QStringLiteral("1.21"));
            check(chip121 > 0, "ZJ5 找得到「1.21」那一颗（0 号恒是「全部」）");
            if (chip121 > 0) {
                chips.at(chip121)->click();
            }
            QApplication::processEvents();
            check(api.requests.size() == requestsBeforeChip,
                  "ZJ5 点 chip **一个请求都不发**（在这一排上筛，就是拿已经在手里的清单筛一遍，"
                  "不是按这一档再问一次接口）",
                  api.lastRequest());
            check(page->currentChip() == QStringLiteral("1.21") && page->chips().at(chip121)->isChecked() &&
                      page->groupCount() == 1 && page->versionRowCount() == 2 &&
                      page->groupTitleAt(0) == QStringLiteral("Forge 1.21.1"),
                  "★★ ZJ5 点 1.21：按这个大版本筛，下面只剩属于 1.21 的那一个文件分组（两条）",
                  QStringLiteral("%1 组 %2 条").arg(page->groupCount()).arg(page->versionRowCount()));
            check(!page->groupExpandedAt(0),
                  "★ ZJ5 筛出来的文件分组仍是收着的（不是从实例进来的，没有预选版本）");
            saveDetailShot(QStringLiteral("detail-series121"));
            // 展开、收起那个文件分组：顶排选中的大版本不变、也不重新筛。
            if (list != nullptr && list->topLevelItem(0) != nullptr) {
                list->topLevelItem(0)->setExpanded(true);
                QApplication::processEvents();
                check(page->groupExpandedAt(0) && page->currentChip() == QStringLiteral("1.21") &&
                          page->chips().at(chip121)->isChecked() && page->groupCount() == 1,
                      "★★ ZJ5 展开文件分组：选中的 1.21 不变");
                saveDetailShot(QStringLiteral("detail-series121-expanded"));
                list->topLevelItem(0)->setExpanded(false);
                QApplication::processEvents();
                check(!page->groupExpandedAt(0) && page->currentChip() == QStringLiteral("1.21") &&
                          page->groupCount() == 1,
                      "★★ ZJ5 再收起：筛选条件仍不变");
            }
            const int chip120 = chipIndexOf(QStringLiteral("1.20"));
            if (chip120 > 0) {
                chips.at(chip120)->click();
            }
            QApplication::processEvents();
            check(page->groupCount() == 1 && page->versionRowCount() == 1 &&
                      page->groupTitleAt(0) == QStringLiteral("Forge 1.20.1"),
                  "ZJ5 换一档换一份（1.20.1 那一条只落在 1.20 这一档下）",
                  QStringLiteral("%1 组 %2 条").arg(page->groupCount()).arg(page->versionRowCount()));

            // ---- ZJ5b. 前置名字**问不着**的时候，不许原地重发（R3）----
            //
            // 最上面那一版声明了一条必装前置（999001），而假接口对 `POST /v1/mods` 答的是
            // 「200 + 空数组」= 查询成功、一条都没有（项目被删 / 限流 / 网络没通都是这一档）。
            // 旧代码回来一次就往缓存里找不到名字、`unknownIds` 还是那几个，于是**立刻再发
            // 一发**同一把钥匙的请求 —— 详情页开着的时候就一直在打接口。
            {
                const bool asked = pumpUntil([&api] { return api.projectBatchHits >= 1; }, 10000);
                check(asked, "ZJ5b 前置的名字真的去问了（这一发是 `POST /v1/mods`）",
                      QStringLiteral("%1 发").arg(api.projectBatchHits));
                const int settled = api.projectBatchHits;
                // 再转 600 毫秒事件循环（判据永远是假 ⇒ 等满就回）：
                // 旧代码的"回去一次就再发一发"是在这一个窗口里发生的。
                pumpUntil([] { return false; }, 600);
                check(api.projectBatchHits == settled && settled == 1,
                      "★ ZJ5b 回来的是空表（问不着）——**不许**再发第二发（问一次算数）",
                      QStringLiteral("%1 发").arg(api.projectBatchHits));
            }
            chips.at(0)->click();
            QApplication::processEvents();
            check(page->currentChip().isEmpty() && page->groupCount() == 2 &&
                      page->versionRowCount() == 3,
                  "ZJ5 「全部」点回去又是整份（筛的是「摆出来的那一份」，不是把清单删了）",
                  QStringLiteral("%1 组 %2 条").arg(page->groupCount()).arg(page->versionRowCount()));

            // ---- ZJ6. 点某一版：没有主页默认值也能打开目标选择器 ----
            installed->setCurrentIndex(-1);
            QApplication::processEvents();
            check(installed->currentIndex() < 0,
                  "ZJ6 前提：主页没有选中的实例",
                  QString::number(installed->currentIndex()));
            QTreeWidgetItem *g1 = list == nullptr ? nullptr : list->topLevelItem(1);
            QTreeWidgetItem *target = g1 == nullptr ? nullptr : g1->child(0);
            if (g1 != nullptr) {
                g1->setExpanded(true);  // 文件分组默认收着，点行之前先摊开
            }
            if (list != nullptr && target != nullptr) {
                list->scrollToItem(target);
            }
            const QRect targetRect = target == nullptr ? QRect() : list->visualItemRect(target);
            const QString targetKey = target == nullptr
                                          ? QString()
                                          : target->data(0, ResourceDetailPage::rowKeyRole())
                                                .toString();
            check(target != nullptr && !targetRect.isEmpty() &&
                      targetKey == QStringLiteral("5100011"),
                  "ZJ6 要点的这一条在**第二组**里（它的 id 和行号、组号全都不一样）", targetKey);
            if (target != nullptr && !targetRect.isEmpty()) {
                dismissNextPrompt();
                clickRow(list, targetRect.center());
                QApplication::processEvents();
                check(g_modalTitle == QStringLiteral("下载到哪个实例"),
                      "ZJ6 点版本行打开内置目标选择器，取消不启动安装",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
                check(overlay->pageOpen(),
                      "ZJ6 选择器是叠在详情页上的，取消后详情页还在"
                      "（不是把用户踢回列表）");
            }

            // 收工：点返回箭头（见 OverlayHost::showPage，箭头 = reject）。
            if (auto *back =
                    overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            } else {
                overlay->closeOverlay();
            }
        };
        clickRow(tree, firstRect.center());  // 产品那一发在这一句里排上队
        QTimer::singleShot(0, &w, inspect);  // 所以这一发排在它后面
        QApplication::processEvents();
        watchdog.stop();
        check(inspected,
              "ZJ4 那一页真的开起来了（下面那些读数全建立在这一点上：没开起来的话它们量的是"
              "空气，而红的会是别处 —— 比如「上一次那一页」留下的名字）");
        check(first->isExpanded(),
              "ZJ4 点行体进页面那一下**不会顺手把这一行收起来**（按下/松开/双击三发都被"
              "MainWindow 吃掉了）");

        // ---- ZJ7. 再点第二行：开的是**它自己**那一页 ----
        // 钉的是"按 iconKey 认行"。认错了的症状是"点第二个资源看到的还是第一个的名字/chip"，
        // 而且一切都正常地跑着 —— 界面上没有人会报警。
        QString secondTitle;
        int secondChips = -1;
        QTimer watchdog7;  // 同上：停得掉的那种
        watchdog7.setSingleShot(true);
        watchdog7.setInterval(12000);
        QObject::connect(&watchdog7, &QTimer::timeout, &w, [overlay] {
            if (!overlay->isOpen()) {
                return;
            }
            if (auto *box = overlay->findChild<InlineMessageBox *>()) {
                box->close();
            }
            if (auto *back =
                    overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            }
        });
        watchdog7.start();
        int secondTries = 0;
        // ZJ10（导航那三下）的两格读数**必须放在外面**：第三下之后整层的收尾
        // （`closeOverlay()`）排在那一层的嵌套事件循环**返回之后**，而那一节自己是卡在
        // 那个循环的回调里跑的 —— 在它里面读，读到的是"还没收"。
        int navDismissCount = 0;
        bool navDismissTried = false;
        std::function<void()> inspectSecond;
        inspectSecond = [&]() {
            if (!overlay->pageOpen() && ++secondTries <= 50) {
                QTimer::singleShot(0, &w, inspectSecond);  // 同上：点完再排 + 没开起来再等一轮
                return;
            }
            if (!overlay->pageOpen()) {
                // 没开起来就把读数写成一句人话：`findChild` 交出来的那一份是**建一次留着**
                // 的，它肚里装的还是上一次那一页 —— 直接读它正好会把这个检查要抓的形状
                // （点第二行看到第一行）当成"通过"。
                secondTitle = QStringLiteral("（详情页没开起来）");
                return;
            }
            ResourceDetailPage *page = w.findChild<ResourceDetailPage *>();
            secondTitle = page == nullptr ? QString() : page->titleText();
            secondChips = page == nullptr ? -1 : page->chips().size();

            // ---- ZJ9. 这一页上方那颗「显示快照版」勾选框（端到端） ----
            //
            // 要求：「上方栏加一个显示快照版勾选框，更改以后自动刷新版本列表」。这一节量的
            // 就是后面那半句"自动刷新"：拨一下之后 chip 那一排**整个换一份**（折 ↔ 不折是两份
            // 不一样的表），而用户正按着的那一颗要**换算到新表上**（还在就留着、只在那一档里
            // 就换成同一个大版本的说法）—— 换算错了的症状是"整排 chip 都不亮、列表却少了一半"。
            //
            // 第二行这条线（1.21.1 + 1.21.2-pre1）是**唯一**一条带快照的：顶排的 key 是大版本，
            // 快照和正式版同属 1.21，所以拨这颗勾顶排不变、按着的 1.21 不动。
            // NOTE: 每一次拨动之后 chip 那些按钮**是新的**（setChips 把它们整个重建），所以
            // 下面一律按 key 现找现点，不留下任何一颗旧的按钮指针。
            if (page != nullptr && overlay->pageOpen()) {
                const auto chipKeys = [page] {
                    QStringList keys;
                    for (int i = 0; i < page->chips().size(); ++i) {
                        keys.append(page->chipKeyAt(i));
                    }
                    return keys;
                };
                const auto clickChip = [page](const QString &key) {
                    const QList<QPushButton *> now = page->chips();
                    for (int i = 0; i < now.size(); ++i) {
                        if (page->chipKeyAt(i) == key) {
                            now.at(i)->click();
                            return true;
                        }
                    }
                    return false;
                };
                auto *box =
                    page->findChild<QCheckBox *>(ResourceDetailPage::showSnapshotsCheckBoxName());
                check(box != nullptr && !box->isChecked(),
                      "ZJ9 开页那一下那颗勾是**不勾**的（用户原话「默认不勾选」）");
                // 这一页是**从下载页**点一行开出来的（哪怕点的是一个模组）
                // ⇒ 右上角那一格照旧是「显示快照版」。要求「而且和下载页有点区别，
                // **不要不小心把下载页改了**」—— 这一条就是那句话的判据：同一个页面对象，
                // 两条来路摆的东西不一样（见 MainWindow::DetailSource）。
                check(!page->modSettingsSlotShown() && box->isVisible() &&
                          page->modSettingsButton() != nullptr &&
                          !page->modSettingsButton()->isVisible(),
                      "★★ ZJ9 下载页点一行开出来的（这一行就是个模组）右上角照旧是"
                      "「显示快照版」—— 那颗「模组设置」只有从**实例的文件管理页**点一个模组"
                      "才摆，下载页一个字都不许改",
                      QStringLiteral("槽位=%1 勾选框露着=%2 模组设置露着=%3")
                          .arg(page->modSettingsSlotShown() ? QStringLiteral("模组设置")
                                                            : QStringLiteral("快照版"),
                               box->isVisible() ? QStringLiteral("是") : QStringLiteral("不是"),
                               page->modSettingsButton() != nullptr &&
                                       page->modSettingsButton()->isVisible()
                                   ? QStringLiteral("是")
                                   : QStringLiteral("不是")));
                check(chipKeys() == QStringList({QString(), QStringLiteral("1.21")}),
                      "★ ZJ9 不勾时顶排是「全部」+ 1.21（`1.21.1` 和 `1.21.2-pre1` 同属 1.21 这一组，"
                      "快照不单独占一颗）",
                      chipKeys().join(QStringLiteral("|")));
                // 版本清单：这一页开的那一发回来之后才有东西。
                const bool loaded =
                    pumpUntil([page] { return page->versionRowCount() == 3; }, 10000);
                check(loaded, "ZJ9 第二行那一页的版本清单也回来了（这一页开的那一发）",
                      QString::number(page->versionRowCount()));
                const int requestsBefore = api.requests.size();

                // ① 先落在 1.21 这一组上（这一档有两条）。
                check(clickChip(QStringLiteral("1.21")), "ZJ9 找得到「1.21」那一颗");
                QApplication::processEvents();
                check(page->currentChip() == QStringLiteral("1.21") &&
                          page->versionRowCount() == 2,
                      "ZJ9 前提：停在 1.21 上（这一档有两条）",
                      QStringLiteral("%1 条").arg(page->versionRowCount()));

                // ② 拨上：顶排的 key 是大版本，快照与正式版同属一组 ⇒ 那一排不变、按着的那一颗不动，
                // 列表重算后还是那两条。
                if (box != nullptr) {
                    box->click();
                }
                QApplication::processEvents();
                check(box != nullptr && box->isChecked() &&
                          chipKeys() == QStringList({QString(), QStringLiteral("1.21")}) &&
                          page->currentChip() == QStringLiteral("1.21") &&
                          page->versionRowCount() == 2,
                      "★★ ZJ9 勾上之后顶排还是 1.21（快照归入所属大版本），按着的还是 1.21、"
                      "列表还是那两条（他留在他正看着的那条线上，列表也**重算**过了）",
                      QStringLiteral("chip 「%1」/ %2 条")
                          .arg(page->currentChip())
                          .arg(page->versionRowCount()));
                check(api.requests.size() == requestsBefore,
                      "★ ZJ9 拨这颗勾**一个请求都不发**（折与不折是拿已经在手里的那份清单算的）",
                      api.lastRequest());

                // ③ 拨回去：同样不变（来回两下都对）。
                if (box != nullptr) {
                    box->click();
                }
                QApplication::processEvents();
                check(box != nullptr && !box->isChecked() &&
                          chipKeys() == QStringList({QString(), QStringLiteral("1.21")}) &&
                          page->currentChip() == QStringLiteral("1.21") &&
                          page->versionRowCount() == 2,
                      "ZJ9 再拨回去，按着的还是 1.21",
                      chipKeys().join(QStringLiteral("|")));

                // 收尾摆回不勾那一档（这一页是**建一次留着**的，下一次读它的人看到的该是
                // 默认那一档）。
                if (box != nullptr) {
                    box->click();
                }
                QApplication::processEvents();
            }

            // ---- ZJ10. 顶上那四颗导航按钮：切走 = 这一页收着，切回来 = 原样摆出来 ----
            //
            // 要求：切到别的选项卡时这一页自动收起；切回来原样摆出来 —— 连按着哪颗 chip、
            // 列表滚到哪儿都一起保留（四个选项卡都适用）。
            //
            // 这一节走**真按钮**（条上那四颗，`TitleBar::navButton()`），量的是那三下：
            // ① 切到别的选项卡 ⇒ 这一页**收着**（不是关掉：关掉的话"切回来还是原来那一页"
            // 就没了，用户再点回来只能重新开一遍）；
            // ② 再点回「下载」⇒ 原来那一页连"按着哪颗 chip、列表几条"一起回来；
            // ③ 站在「下载」上再点一下「下载」⇒ 回到这个功能的首页（那一层收掉）。
            if (page != nullptr && overlay->pageOpen()) {
                // 摆一个**说得出口的前提**：站到「1.21」那一颗上（这一档有两条）。上面
                // ZJ9 收尾那一下把它留在了折出来的 1.21.2 上（那一档下一条都没有）—— 照那个
                // 状态量"回来时一样不少"，两边都是"空"，量不出区别来。
                for (int i = 0; i < page->chips().size(); ++i) {
                    if (page->chipKeyAt(i) == QStringLiteral("1.21")) {
                        page->chips().at(i)->click();
                    }
                }
                QApplication::processEvents();
                check(page->currentChip() == QStringLiteral("1.21") &&
                          page->versionRowCount() == 2,
                      "ZJ10 前提：站到「1.21」那一颗上（这一档有两条）",
                      QStringLiteral("%1 条").arg(page->versionRowCount()));
                QTabWidget *navTabs = w.ui() == nullptr ? nullptr : w.ui()->getTabWidget();
                TitleBar *strip = w.ui() == nullptr ? nullptr : w.ui()->titleBar();
                auto *homeNav = strip == nullptr ? nullptr
                                                 : strip->navButton(MainWindowUI::homeTabIndex());
                auto *downloadNav =
                    strip == nullptr ? nullptr : strip->navButton(MainWindowUI::downloadTabIndex());
                check(navTabs != nullptr && homeNav != nullptr && downloadNav != nullptr &&
                          navTabs->currentIndex() == MainWindowUI::downloadTabIndex(),
                      "ZJ10 前提：条上那四颗导航按钮在、现在正站在「下载」上",
                      QStringLiteral("tab %1 / 条上 %2 颗")
                          .arg(navTabs != nullptr ? navTabs->currentIndex() : -1)
                          .arg(strip != nullptr ? strip->navCount() : -1));
                if (navTabs != nullptr && homeNav != nullptr && downloadNav != nullptr) {
                    homeNav->click();
                    QApplication::processEvents();
                    check(navTabs->currentIndex() == MainWindowUI::homeTabIndex() &&
                              overlay->pageOpen() && overlay->parked() && !overlay->isVisible(),
                          "★★ ZJ10 切到别的选项卡 = 这一页**收着**（开着、但不露面 —— "
                          "不是关掉，那一页还卡在它自己的事件循环里等着人回去）",
                          QStringLiteral("tab %1 / parked %2 / 可见 %3")
                              .arg(navTabs->currentIndex())
                              .arg(overlay->parked())
                              .arg(overlay->isVisible()));
                    // 收着的时候**不许按住**下面那一页：用户正要用它。
                    check(navTabs->currentWidget() != nullptr && navTabs->currentWidget()->isEnabled(),
                          "★ ZJ10 收着的时候底下那一页是活的（收起来的层没盖住任何东西）");
                    check(page->titleText() == secondTitle &&
                              page->currentChip() == QStringLiteral("1.21") &&
                              page->versionRowCount() == 2,
                          "★ ZJ10 收着的是**原来那一页**（连按着哪颗 chip、列表几条都在）",
                          QStringLiteral("「%1」/ chip 「%2」/ %3 条")
                              .arg(page->titleText())
                              .arg(page->currentChip())
                              .arg(page->versionRowCount()));

                    downloadNav->click();
                    QApplication::processEvents();
                    check(navTabs->currentIndex() == MainWindowUI::downloadTabIndex() &&
                              overlay->pageOpen() && !overlay->parked() && overlay->isVisible(),
                          "★★ ZJ10 再点回「下载」= 原来那个界面原样摆出来"
                          "（用户原话「那就回到下载 sodium 界面」）",
                          QStringLiteral("tab %1 / parked %2")
                              .arg(navTabs->currentIndex())
                              .arg(overlay->parked()));
                    check(page->currentChip() == QStringLiteral("1.21") &&
                              page->versionRowCount() == 2 && page->chips().size() == 2,
                          "★ ZJ10 回来时那一页的状态一样不少（chip 还是按着的那一颗、列表还是"
                          "那两条、那一排还是折过的那一份 —— 一个请求都不发）",
                          QStringLiteral("chip 「%1」/ %2 条")
                              .arg(page->currentChip())
                              .arg(page->versionRowCount()));

                    // ③ 站在「下载」上再点一下「下载」= 回到这个功能的首页。★ 收的必须是
                    // **返回箭头那一发**（`dismiss()` 发的就是它）：`closeOverlay()` 不经过
                    // 页面自己的 reject，收页钩子和页面收尾都不跑。（`backPressed` 只有返回
                    // 箭头和这一发会发，而这一节里返回箭头一次都没点过 ⇒ 数到的就是这一发。）
                    // NOTE: 这里只量"那一发发出来了没有"。整层收干净是这一发里的事
                    // （`reject ⇒ finished ⇒ closeOverlay`），但那一条判据放在后面读，
                    // 免得和这一发混在一起。
                    const QMetaObject::Connection navSpy = QObject::connect(
                        overlay, &OverlayHost::backPressed, overlay,
                        [&navDismissCount] { ++navDismissCount; });
                    const int requestsBeforeDismiss = api.requests.size();
                    downloadNav->click();
                    QApplication::processEvents();
                    check(navDismissCount == 1,
                          "★★ ZJ10 站在「下载」上再点一下「下载」= 发的是**返回箭头那一发**"
                          "（用户原话「在下载页点击 下载（顶上那个，不是下载确认键），那就回到"
                          "下载页的首页」）",
                          QStringLiteral("发了 %1 次").arg(navDismissCount));
                    check(navTabs->currentIndex() == MainWindowUI::downloadTabIndex() &&
                              api.requests.size() == requestsBeforeDismiss,
                          "ZJ10 那一下只收那一层：选项卡没动、也没有顺手再问一次接口");
                    QObject::disconnect(navSpy);
                    navDismissTried = true;
                }
            }

            // NOTE: ZJ10 走到第三下（导航收页）时这一发就免了 —— 那一页已经在收尾的路上，
            // 再照它按一下返回箭头就是同一件事做第二遍。
            if (!navDismissTried) {
                if (auto *back =
                        overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else {
                    overlay->closeOverlay();
                }
            }
        };
        tree->scrollToItem(second);
        // 先转一趟：`scrollToItem` 之后行矩形要等布局重算才是新的，不然后面按坐标伪造的
        // 那一发点的是**老位置**（点到的可能是别的行，甚至是第一行）。
        QApplication::processEvents();
        const QRect secondRect = second == nullptr ? QRect() : tree->visualItemRect(second);
        check(second != nullptr && !secondRect.isEmpty() &&
                  tree->viewport()->rect().contains(secondRect.center()),
              "ZJ7 第二行也在屏幕上（矩形是真的，而且这一格落在视口里 —— 不然后面那一发伪造的"
              "鼠标事件落不到它身上，红的却会是「名字对不上」）",
              QStringLiteral("行 %1x%2 视口 %3x%4")
                  .arg(secondRect.width())
                  .arg(secondRect.height())
                  .arg(tree->viewport()->width())
                  .arg(tree->viewport()->height()));
        if (second != nullptr && !secondRect.isEmpty()) {
            clickRow(tree, secondRect.center());
            QTimer::singleShot(0, &w, inspectSecond);
            QApplication::processEvents();
        }
        watchdog7.stop();
        check(secondTitle == QStringLiteral("第二个模组") && secondChips == 2,
              "ZJ7 点第二行开的是第二行那一页（名字对得上，顶排是「全部」+ 它声明的 1.21 一个大版本"
              "—— 1.21.1 和 1.21.2-pre1 同属 1.21；第一行那页是 3 颗，认错行就会是 3）",
              secondTitle + QStringLiteral(" / chip ")
                  + QString::number(secondChips));
        check(!overlay->isOpen(), "ZJ7 收干净了");
        // ZJ10 的收尾：第三下导航发的那一发**真的把整层收干净了**。要在这里读 —— ZJ10
        // 自己是在定时器回调里跑的，那一发之后的收尾不在它那段代码里；换到这一节读，
        // 两件事分得开。这一条要是坏了，症状是"点一下导航，那一页还占着内容区"。
        check(!navDismissTried || (navDismissCount == 1 && !overlay->isOpen()),
              "★★ ZJ10 那一下之后这一层真的收干净了（回到这个功能的首页）",
              QStringLiteral("发了 %1 次 / isOpen %2")
                  .arg(navDismissCount)
                  .arg(overlay->isOpen()));
    }

    restoreBase();
    check(!QSettings().contains(kBaseKey), "ZJ8 那个口子键还回去了（没留在沙盒里）");
}

