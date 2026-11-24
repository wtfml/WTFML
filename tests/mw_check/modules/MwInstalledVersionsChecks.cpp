#include "Support.hpp"
#include <QScopeGuard>
int MwInstalledVersionsChecks(const QDir &versionsDir, MainWindow &window, QStackedWidget * &gameStack, QLineEdit * &installNameEdit, QLabel * &installNameError, QPushButton * &installBtn, QPushButton * &installBackBtn, GlobalJavaSettings * &windowJava, const std::function<void(const QString &)> &pinGlobalJava, const std::function<void()> &unpinGlobalJava) {
    {
        // 用别名而不是 `using VersionManager::LoaderKind`：后者在块作用域里
        // GCC 直接报「using-declaration for member at non-class scope」。
        using LoaderKind = VersionManager::LoaderKind;
        // 加载器安装开始时会打「开始安装 <加载器> <版本>」。下面 Y3/Y4 都要断言"这一段里
        // 一次都没开始过" —— 用**增量**而不是绝对条数：前面那些段有没有打过同一句是它们
        // 的事（某次真走了一趟下载就会打），不该混进这一条里。
        const int installsBeforeY = countOccurrences(messagesSnapshot(), QStringLiteral("开始安装"));

        // Y1. 界面上那张表和 VersionManager 认的加载器**同序同名**。这两张表一个摆控件、
        // 一个把下标翻成枚举，对不上的症状是「勾了 Fabric 装出 Forge」：不报错、
        // 不崩，只是装错东西 —— 所以哪怕这条看着像在验一张常量表，也得验。
        const QStringList names = MainWindowUI::modLoaderOptionNames();
        const QStringList texts = MainWindowUI::modLoaderOptionTexts();
        const int kindCount = VersionManager::loaderKindCount();
        const int noneIndex = MainWindowUI::modLoaderNoneIndex();
        // Y2 和 Y3 都要点 Forge。写成常量是因为这两处关心的是"同一个钮"，而它排第几是
        // 一张会变的表（哪天真加了个加载器，`buttons.at(1)` 就悄悄指向别人了）。
        const int forgeIndex = 1;
        // Y2c 要拿一条**别的**加载器来试"灰掉了点不动"。第 0 条是 Fabric（顺序由上面那条
        // "第 1 条是 Forge" 的检查一起钉着：两张表同序）。
        const int fabricIndex = 0;
        // 上面那条按"第 1 条是 Forge"钉死了；这条是它的**反面**：改点 OptiFine 那一节
        // 按文案找下标，这样两张表真被重排时红的是这里、而不是"点了别的东西还全绿"。
        const int optifineIndex = texts.indexOf(QStringLiteral("OptiFine"));
        // Y2d 要拿 NeoForge 验"上游只发过 beta"那一版（26.3 就是，见 verify 的 Y7）。
        const int neoforgeIndex = texts.indexOf(QStringLiteral("NeoForge"));
        check(neoforgeIndex >= 0 && neoforgeIndex != forgeIndex && neoforgeIndex != noneIndex &&
                  neoforgeIndex != optifineIndex,
              "NeoForge 也在表里，而且是**另一条**（Y2d 点的是它）",
              QString::number(neoforgeIndex));
        check(names.size() > forgeIndex && texts.at(forgeIndex) == QStringLiteral("Forge"),
              "第 1 条是 Forge（Y2/Y3 点的是它）", texts.value(forgeIndex));
        check(optifineIndex >= 0 && optifineIndex != forgeIndex && optifineIndex != noneIndex,
              "OptiFine 也在表里，而且是另一条（改点它那条要有个落点）",
              QString::number(optifineIndex));
        const int quiltIndex = texts.indexOf(QStringLiteral("Quilt"));
        check(quiltIndex == 4 &&
                  texts.mid(0, 4) == QStringList({QStringLiteral("Fabric"), QStringLiteral("Forge"),
                                                  QStringLiteral("NeoForge"), QStringLiteral("OptiFine")}),
              "Y1 现有四种加载器顺序不变（0..3），Quilt 排在第 4 位",
              texts.join(QLatin1Char(',')));
        check(names.size() == texts.size() && names.size() == kindCount + 1,
              "那组钮 = 五个加载器 + 一个「不装（纯原版）」",
              QString("names=%1 texts=%2 loaders=%3")
                  .arg(names.size())
                  .arg(texts.size())
                  .arg(kindCount));
        check(noneIndex == kindCount, "「不装」正好排在五个加载器后面（下标 == 加载器个数）",
              QString("%1 vs %2").arg(noneIndex).arg(kindCount));
        int matched = 0;
        for (int i = 0; i < kindCount; ++i) {
            LoaderKind kind = LoaderKind::Fabric;
            if (VersionManager::loaderKindFromIndex(i, &kind) &&
                VersionManager::loaderDisplayName(kind) == texts.at(i)) {
                ++matched;
            }
        }
        check(matched == kindCount, "第 i 个钮上写的就是 VersionManager 认的那个加载器（同序同名）",
              QString("%1/%2 对上").arg(matched).arg(kindCount));
        // 「不装」**不能**翻成任何一个加载器。翻得成的话，"勾着「不装」点下载"就成了一次
        // 没人点过头的加载器安装 —— 这条是那第 6 个选项存在的全部意义。
        LoaderKind noneKind = LoaderKind::Fabric;
        check(!VersionManager::loaderKindFromIndex(noneIndex, &noneKind),
              "「不装」翻不成加载器", VersionManager::loaderDisplayName(noneKind));
        check(!VersionManager::loaderKindFromIndex(-1, &noneKind) &&
                  !VersionManager::loaderKindFromIndex(kindCount + 1, &noneKind),
              "越界的下标也翻不成加载器（界面那张表将来变长变短时的兜底）");
        check(VersionManager::loaderInstallSupported(LoaderKind::Fabric) &&
                  VersionManager::loaderInstallSupported(LoaderKind::Forge) &&
                  VersionManager::loaderInstallSupported(LoaderKind::NeoForge) &&
                  VersionManager::loaderInstallSupported(LoaderKind::OptiFine) &&
                  VersionManager::loaderInstallSupported(LoaderKind::Quilt),
              "五个加载器都接通了（含 Quilt）");

        // Y2. 真窗口上那棵加载器树：五条顶层条目、每条的身份就是下标、**点一下真的会
        // 改掉"要装哪一个"**（要求："做成和上一页的版本选择相同的模式，点击以后
        // 展开版本，选择版本以后收缩，显示版本"）。
        //
        // NOTE: 之前这里戳的是五个 QRadioButton + 一个 QButtonGroup。
        // "选中哪一条"现在住在 InstallPageController::m_selectedLoader 里（界面上**没有**
        // 任何"勾"的状态），所以可观察的东西只剩两样：
        // · 名称框里的默认名（勾了 Forge 就是 `1.21.4-Forge`）；
        // · 那棵树每一行第二行的小字（选中的那条写自己钉的版本号，其余几条写
        // 「与 X 不兼容」）。
        // 这两样正是用户看得见的东西，比"组里的 checkedId"更接近用户实际拿到的那份。
        QTreeWidget *loaderTree = byObjectName<QTreeWidget>(&window, MainWindowUI::loaderTreeName());
        QVector<QTreeWidgetItem *> rows;
        for (int i = 0; loaderTree != nullptr && i < loaderTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = loaderTree->topLevelItem(i);
            if (item != nullptr &&
                item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt() >= 0) {
                rows.append(item);
            }
        }
        const auto rowSubText = [loaderTree](QTreeWidgetItem *item) {
            return item != nullptr
                       ? item->data(MainWindowUI::loaderTreeKeyColumn(),
                                    MainWindowUI::rowSubTextRole())
                             .toString()
                       : QString();
        };
        const auto clickRow = [&window](QTreeWidget *tree, QTreeWidgetItem *item) {
            // 走的是真窗口上那条信号路（QTreeWidget::itemClicked → MainWindow 里那个
            // 处理点击的 lambda）。伪造鼠标事件要先把控件滚到可见、算好坐标，而落到
            // 产品里的还是这一条路。
            QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 0));
            QApplication::processEvents();
        };
        // 上面那个走的是"信号"那条近路，**绕开了鼠标**。行里那个「取消」小叉（选中那条
        // 加载器右边那个 ×）必须走**真的鼠标事件**：产品把它接在视口的事件过滤器上
        // （QEvent::MouseButtonPress/Release），伪造 itemClicked 正好绕过要验的那一段。
        // 视口坐标 = 条目矩形的坐标系（visualItemRect() 也是视口的坐标）。
        const auto clickAt = [](QTreeWidget *tree, const QPoint &pos) {
            QWidget *viewport = tree->viewport();
            const QPointF local(pos);
            const QPointF global(viewport->mapToGlobal(pos));
            QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &release);
            QApplication::processEvents();
        };
        check(loaderTree != nullptr && rows.size() == names.size() &&
                  loaderTree->topLevelItemCount() == names.size() + 1,
              "加载器那棵树按 objectName 找得回来：四个加载器 + 「不装」+ 一条预设行",
              QString("树 %1，条目 %2/%3")
                  .arg(loaderTree != nullptr ? QStringLiteral("有") : QStringLiteral("没有"))
                  .arg(rows.size())
                  .arg(names.size()));

        // NOTE: 下面这块整段是"按控件找得到才验"的 —— 找不到就只留下一条失败（上面那条），
        // 不再往下戳 nullptr。找不到的典型原因是有人改了 objectName 或把控件挪了家，
        // 那时候后面那十几条一起红只会淹没真正的原因。
        if (loaderTree != nullptr && rows.size() == names.size()) {
            int sameId = 0;
            int sameText = 0;
            for (int i = 0; i < rows.size(); ++i) {
                const int id = rows.at(i)
                                   ->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole)
                                   .toInt();
                sameId += id == i ? 1 : 0;
                sameText += rows.at(i)->text(MainWindowUI::loaderTreeKeyColumn()) == texts.at(i) ? 1 : 0;
            }
            check(sameId == names.size(), "每条的身份（userData）就是它在两张表里的下标",
                  QString("%1/%2").arg(sameId).arg(names.size()));
            check(sameText == names.size(), "每条上写的字就是表里那条文案",
                  QString("%1/%2").arg(sameText).arg(names.size()));
            // 开局：五条都没有第二行小字（还没选任何加载器，"和谁不兼容"这句话没有主语）。
            // 「不装」那条**永远**不写第二行 —— 它不是加载器，不跟谁冲突。
            QStringList subs;
            for (QTreeWidgetItem *row : rows) {
                subs << rowSubText(row);
            }
            // NOTE: 判据不能写成 `subs.join(...).isEmpty()` —— 五条空串 join 出来是
            // "||||"（非空），那样这条会永远红。得逐条判。
            check(std::all_of(subs.cbegin(), subs.cend(),
                              [](const QString &s) { return s.isEmpty(); }),
                  "开局五条的第二行小字都是空的（还没选加载器，没什么可说的）",
                  subs.join(QLatin1Char('/')));
            // NOTE: "点一下展开/再点一下关闭""选中之后其余几条变灰"这些都要有真条目才验得
            // 出来，所以它们在 Y2c 里（那儿有产品自己填进去的那几条）。
            clickRow(loaderTree, rows.at(forgeIndex));
            QStringList subsAfterClick;
            for (QTreeWidgetItem *row : rows) {
                subsAfterClick << rowSubText(row);
            }
            check(std::all_of(subsAfterClick.cbegin(), subsAfterClick.cend(),
                              [](const QString &s) { return s.isEmpty(); }),
                  "点一下加载器那一条**不会**选中它（那一下只管展开/收起，不选任何东西）",
                  subsAfterClick.join(QLatin1Char('/')));
            clickRow(loaderTree, rows.at(optifineIndex));
            check(rowSubText(rows.at(optifineIndex)).isEmpty(),
                  "换一条点也一样（点到 OptiFine 上也没选中它）",
                  rowSubText(rows.at(optifineIndex)));
            check(rowSubText(rows.at(noneIndex)).isEmpty(),
                  "「不装（纯原版）」那条不写第二行小字（它不是加载器，不跟谁冲突）",
                  rowSubText(rows.at(noneIndex)));
        }

        // Y3. **删掉了**（原来是"勾一个没接通的加载器 -> 弹「还没做」"）。
        //
        // NOTE: startLoaderInstall() 里那段守卫**故意留着**（见那边的 NOTE）：将来往枚举
        // 里加第五个加载器时正是它接住"数据层还没接上"那种情况。OptiFine 真装一次的
        // 端到端证明搬到了 `verify.exe loader-install 1.18.2 optifine`（联网那条）。

        // Y3/Y4 两段都在**安装页**上戳：进安装页（点树里一个版本号那条路）→ 名称框里
        // 是默认名 → 点一条加载器 → 按「安装」。控件**全都得找回来**（找不到的典型原因
        // 是有人改了 objectName 或把控件挪了家，那时候后面十几条一起红只会淹没真正的原因）。
        const bool loaderTreeReady = loaderTree != nullptr && rows.size() == names.size();
        check(installNameEdit != nullptr &&
                  installNameError != nullptr && installBtn != nullptr &&
                  installBackBtn != nullptr && gameStack != nullptr && loaderTreeReady,
              "安装页那一套控件都找得到（名称框 / 那行红字 / 安装 / 返回 / 两层堆叠 / "
              "加载器树）");

        if (installNameEdit != nullptr &&
            installNameError != nullptr && installBtn != nullptr && installBackBtn != nullptr &&
            gameStack != nullptr && loaderTreeReady) {
            // Y3 要用一个"下过一半"的版本目录：产品在"这个版本下过没有"那一问上判的就是
            // <版本>.json 在不在，所以先摆好它。
            const QString kPickedId = QStringLiteral("1.21.4");
            const QString pickedJson = versionsDir.filePath(kPickedId + QLatin1Char('/') +
                                                            kPickedId + QStringLiteral(".json"));
            writeFileAt(pickedJson, "{}");
            check(QFile::exists(pickedJson), "沙盒里摆好了一个「下过一半」的版本目录", pickedJson);

            // 「← 返回版本列表」：先按一下确认它真回得去（这一条是纯接线 —— 少一根
            // connect 的症状是"点了没反应"，而那种毛病用户不会报，只会觉得启动器坏了）。
            installBackBtn->click();
            QApplication::processEvents();
            check(gameStack->currentIndex() == MainWindowUI::downloadGameTreeIndex(),
                  "按「← 返回版本列表」回到树那一层",
                  QString::number(gameStack->currentIndex()));

            // 进安装页。真窗口上走的是"点树里一条版本号"（QTreeWidget::itemClicked →
            // openInstallPage()）；这里直接调那个槽，理由和别处一样：伪造鼠标点树条目
            // 要先把树灌上数据（那要联网拉在线清单），而这一段要验的是**进了安装页之后**
            // 的事 —— 跟那个版本号是清单里来的还是手写进来的无关。
            //
            // NOTE: 这一页要**真的翻到前面来**（下面 Y2b/Y2c 验的是"页面上看得见的行为"）。
            // 藏在别的页后面的控件不排布局，而"灌列表顺手把那一行撑开"这件事只有在
            // 控件可见时才会发生 —— 那条撑开的路是
            // QAbstractItemView::currentChanged → `if (d->autoScroll) scrollTo(...)`，
            // 而它在 isVisible() 为假时只记一句"等显示出来再说"，什么都不做。
            // （验过：把 bug 放回去，翻页前这一节全绿、翻页后当场红。）
            QTabWidget *downloadTabs = window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
            const int tabBeforeInstall = downloadTabs != nullptr ? downloadTabs->currentIndex() : -1;
            if (downloadTabs != nullptr) {
                downloadTabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
            }
            QApplication::processEvents();
            QMetaObject::invokeMethod(&window, "openInstallPage", Qt::DirectConnection,
                                      Q_ARG(QString, kPickedId));
            QApplication::processEvents();
            check(gameStack->currentIndex() == MainWindowUI::downloadGameInstallIndex(),
                  "点一个版本号之后翻到安装页那一层",
                  QString::number(gameStack->currentIndex()));
            check(installNameEdit->text() == kPickedId,
                  "名称框里的默认名就是那个原版号（没勾加载器时就是它本身）",
                  installNameEdit->text());
            // NOTE: "重名 = 同一个实例"那句灰色说明已经删了（用户点名是废话），
            // 名字框底下只剩红字；这里只量名字合法时「安装」是亮的。
            check(installBtn->isEnabled(), "默认名合法，「安装」是亮的");

            // 实例预设那一行（「OptiFine 下预设行、
            // 紧凑弹窗、四类切换及本次安装选择」）。它现在**是加载器树上的第六行**，
            // 不再是卡片里那三件控件（老的下拉框 / 查看 / 勾选框已经整块退场）。
            // 这一行的身份是负数（`installPresetRowRole()`），所以加载器那张下标表
            // 一个字节都没动；位置**固定**在「不装（纯原版）」前面一行。
            {
                QTreeWidgetItem *presetRow = nullptr;
                if (loaderTree != nullptr) {
                    for (int i = 0; i < loaderTree->topLevelItemCount(); ++i) {
                        QTreeWidgetItem *item = loaderTree->topLevelItem(i);
                        if (item != nullptr &&
                            item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole)
                                    .toInt() == MainWindowUI::installPresetRowRole()) {
                            presetRow = item;
                            break;
                        }
                    }
                }
                check(presetRow != nullptr && loaderTree != nullptr &&
                          loaderTree->indexOfTopLevelItem(presetRow) ==
                              MainWindowUI::modLoaderNoneIndex(),
                      "★ 加载器树上多了一条「预设」行，就排在「不装（纯原版）」前面"
                      "（P1：OptiFine 下预设行）",
                      QString::number(presetRow != nullptr && loaderTree != nullptr
                                          ? loaderTree->indexOfTopLevelItem(presetRow)
                                          : -1));
                const QString presetSubText =
                    presetRow != nullptr
                        ? presetRow->data(MainWindowUI::loaderTreeKeyColumn(),
                                          MainWindowUI::rowSubTextRole())
                              .toString()
                        : QString();
                check(presetRow != nullptr &&
                          presetRow->text(MainWindowUI::loaderTreeKeyColumn()) ==
                              MainWindowUI::installPresetRowText() &&
                          presetSubText == MainWindowUI::installPresetSummaryText(0),
                      "★ 那一行写「预设」，小字一开始是「未选择」（这一趟还没挑过）",
                      presetRow != nullptr
                          ? presetRow->text(MainWindowUI::loaderTreeKeyColumn()) +
                                QStringLiteral(" / ") + presetSubText
                          : QStringLiteral("<没有这一行>"));

                // 点它：开的是**内容区中间那一块紧凑弹窗**（`execPrompt`），不是另开一页、
                // 也不是独立窗口。弹窗里四类各一页，默认停在第 0 页（游戏设置）。
                // 检查员排在这一发前面 —— 产品那条路会阻塞在嵌套事件循环里，
                // 定时器正是在那个循环里醒过来的（见本文件那些提示助手）。
                const int pageBefore = gameStack->currentIndex();
                bool pickerSeen = false;
                bool contextOk = false;
                bool tabsOk = false;
                bool firstPageOk = false;
                bool headerIconOk = false;
                bool switchOk = false;
                int tabsSeen = -1;
                QString pickerNote;
                const auto inspectPicker = [&] {
                    InlineMessageBox *box = currentPrompt();
                    if (box == nullptr) {
                        pickerNote = QStringLiteral("没弹东西");
                        return;
                    }
                    auto *picker = box->findChild<PresetInstallPicker *>();
                    if (picker == nullptr) {
                        pickerNote = QStringLiteral("弹的不是预设弹窗");
                        box->close();
                        return;
                    }
                    pickerSeen = true;
                    contextOk = picker->accessibleDescription().contains(kPickedId);
                    auto *tabs = picker->findChild<QListWidget *>(PresetInstallPicker::tabBarName());
                    const auto categories = PresetController::installCategories();
                    tabsSeen = tabs ? tabs->count() : -1;
                    tabsOk = tabs && tabsSeen == categories.size() + 1 && categories.size() == 5;
                    if (tabsOk) for (int i = 0; i < categories.size(); ++i)
                        tabsOk &= tabs->item(i)->text().startsWith(categories[i].title);
                    auto *stack = picker->findChild<QStackedWidget *>();
                    firstPageOk = stack != nullptr && stack->currentIndex() == 0;
                    // 标题图标块不是提问弹框那枚 ⓘ（本仓库 ⓘ 专指「详情 / 说明」）。
                    InlineMessageBox plain(MessageKind::Question, QString(), QString(), {}, 0, -1);
                    const auto chipOf = [](QWidget *owner) {
                        auto *chip = owner->findChild<QLabel *>(QStringLiteral("inlineMessageIcon"),
                                                                Qt::FindDirectChildrenOnly);
                        return chip ? chip->pixmap().toImage() : QImage();
                    };
                    headerIconOk = !chipOf(box).isNull() && chipOf(box) != chipOf(&plain);
                    // 「四类切换」：点第 2 页签，正文那一层得跟着翻。
                    if (tabs != nullptr && stack != nullptr && tabs->count() > 2) {
                        tabs->setCurrentRow(2);
                        QApplication::processEvents();
                        switchOk = stack->currentIndex() == 2;
                    }
                    // 取消（第二颗）：草稿整个丢掉 —— 摘要一个字都不许变。
                    if (auto *cancel =
                            box->findChild<QPushButton *>(InlineMessageBox::buttonName(1))) {
                        cancel->click();
                    } else {
                        box->close();
                    }
                };
                // 预设数据在**后台读**（`PresetController::requestInstallPickerData()`）：
                // 步骤 136 起点完那一下弹框立刻出来、右栏挂加载态，数据回来才填。等它填好再验。
                //
                // NOTE: 检查不能只在点击之后才排 —— `clickRow()` 自己会转一圈事件循环，
                // 数据快的时候弹窗就在那一圈里摆上并阻塞在 `execPrompt()` 的嵌套循环里。
                // 所以先挂一个 10ms 的轮询定时器：它在哪个循环里醒过来都行，一旦看见
                // 预设弹窗就验一遍并点取消（点完 execPrompt 才返回）。
                //
                // 顺手验两件 136 的承诺，都在弹框**摆出来那一刻**（Show）验：那时 execPrompt 的
                // 嵌套循环还没转，后台那一趟不可能已经填进来 —— 轮询等不到加载态（沙盒里预设
                // 目录是空的，第一拍之前就读完了），只有这一刻是确定的。
                // ① 读回来之前「确定选择」是灰的（拿空表确定会把挑好的几类清掉）；
                // ② 这时再点一下「预设」：不另开、也不重发请求（重发会把这一张的数据作废，
                //    它就一直停在加载态，下面那个"等它填好"就等不到）。
                // 等不到时把看到的样子记下来（没弹 / 弹的是别的提示 / 停在加载态），红了好查。
                bool inspected = false;
                bool shownLoading = false;
                bool loadingConfirmEnabled = false;
                bool secondClicked = false;
                bool secondClickKeptPrompt = false;
                bool filledConfirmEnabled = false;
                QString lastSeen = QStringLiteral("没弹东西");
                QElapsedTimer pickerClock;
                struct ShowSpy : QObject {
                    std::function<void(QObject *)> onShow;
                    bool eventFilter(QObject *watched, QEvent *event) override {
                        if (event->type() == QEvent::Show && onShow) onShow(watched);
                        return false;
                    }
                } showSpy;
                showSpy.onShow = [&](QObject *watched) {
                    auto *box = qobject_cast<InlineMessageBox *>(watched);
                    auto *picker = box != nullptr ? box->findChild<PresetInstallPicker *>() : nullptr;
                    if (picker == nullptr || secondClicked) return;
                    auto *pickerStack = picker->findChild<QStackedWidget *>();
                    auto *confirm = box->findChild<QPushButton *>(InlineMessageBox::buttonName(0));
                    shownLoading = pickerStack != nullptr && pickerStack->count() == 0;
                    loadingConfirmEnabled = confirm == nullptr || confirm->isEnabled();
                    secondClicked = true;
                    // 不转事件循环（这是在 Show 里）：只走一遍产品那条点击路，它应当当场返回。
                    QMetaObject::invokeMethod(loaderTree, "itemClicked", Qt::DirectConnection,
                                              Q_ARG(QTreeWidgetItem *, presetRow), Q_ARG(int, 0));
                    secondClickKeptPrompt = currentPrompt() == box;
                };
                qApp->installEventFilter(&showSpy);
                const auto removeSpy = qScopeGuard([&] { qApp->removeEventFilter(&showSpy); });
                QTimer inspectPoll;
                inspectPoll.setInterval(10);
                QObject::connect(&inspectPoll, &QTimer::timeout, [&] {
                    if (inspected) return;
                    InlineMessageBox *box = currentPrompt();
                    auto *picker = box != nullptr ? box->findChild<PresetInstallPicker *>() : nullptr;
                    if (picker == nullptr) {
                        if (box != nullptr) {
                            const auto *title = box->findChild<QLabel *>(InlineMessageBox::titleLabelName());
                            lastSeen = QStringLiteral("开着的是别的提示「%1」").arg(title ? title->text() : QString());
                        }
                        return;
                    }
                    auto *pickerStack = picker->findChild<QStackedWidget *>();
                    if (pickerStack == nullptr || pickerStack->count() == 0) {
                        lastSeen = QStringLiteral("预设弹窗停在加载态（%1 ms）").arg(pickerClock.elapsed());
                        // 兜底得在这里：测试那头正卡在 clickRow → execPrompt 里，外面那个
                        // pumpUntil 要等这张框关了才轮得到。
                        if (pickerClock.elapsed() > 5000) {
                            inspectPoll.stop();
                            box->close();
                        }
                        return;
                    }
                    auto *confirm = box->findChild<QPushButton *>(InlineMessageBox::buttonName(0));
                    filledConfirmEnabled = confirm != nullptr && confirm->isEnabled();
                    inspected = true;
                    inspectPoll.stop();
                    inspectPicker();  // 点取消：execPrompt 这才返回
                });
                // 前面几节排下的提示探针（`dismissNextPrompt()` 那一族）会盯 20 秒，抢先把这张框
                // 点掉（实测 11 ms 就被点了取消）。理由同 PromptAnswerer::start：从这一刻起作废它们。
                ++g_modalHandlerGeneration;
                inspectPoll.start();
                pickerClock.start();
                auto *overlayHost = window.findChild<OverlayHost *>(OverlayHost::overlayHostName());
                const auto clickState = [&] {
                    return QStringLiteral("行灰 %1 / promptOpen %2 / 有提示 %3")
                        .arg(presetRow != nullptr && presetRow->isDisabled())
                        .arg(overlayHost != nullptr && overlayHost->promptOpen())
                        .arg(currentPrompt() != nullptr);
                };
                const QString beforeClick = clickState();
                if (presetRow != nullptr && loaderTree != nullptr) {
                    clickRow(loaderTree, presetRow);
                }
                const QString afterClick = clickState();
                // 弹框开出来过的话这一句早已成立（框在 clickRow 里开、验、关完）；这里只兜"压根没弹"。
                pumpUntil([&] { return inspected; }, 5000);
                inspectPoll.stop();
                if (!inspected) {
                    pickerNote = QStringLiteral("5 秒内没等到预设弹窗填好：%1（点前 %2；点后 %3）").arg(lastSeen, beforeClick, afterClick);
                    if (InlineMessageBox *stuck = currentPrompt()) stuck->close();  // 别把后面几段一起挡住
                }
                pumpUntil([] { return currentPrompt() == nullptr; }, 5000);
                check(pickerSeen,
                      "★★ 点「预设」那一行开的是中间那块紧凑弹窗（不是另开一页）",
                      pickerNote);
                check(contextOk,
                      "★ 预设弹窗保留安装检查上下文");
                check(tabsOk && tabsSeen == 6,
                      "★★ 预设弹窗包含五类内容和综合预设导航",
                      QStringLiteral("页签 %1").arg(tabsSeen));
                check(firstPageOk, "★ 弹窗默认停在第一类（游戏设置）");
                check(headerIconOk, "预设弹窗标题图标块换成拼图，不是提问弹框的 ⓘ");
                check(switchOk, "★ 点第二个页签，正文跟着翻到第二类");
                check(shownLoading && !loadingConfirmEnabled,
                      "★ 预设候选读回来之前「确定选择」是灰的（拿空表确定会把挑好的几类清掉）",
                      QStringLiteral("摆出来时在加载态 %1 / 「确定」亮着 %2")
                          .arg(shownLoading).arg(loadingConfirmEnabled));
                check(inspected && filledConfirmEnabled, "★ 读回来之后「确定选择」亮起来");
                check(secondClicked && secondClickKeptPrompt && inspected,
                      "★ 弹窗开着时再点「预设」：不另开一张、也不把这一张的数据作废（照样等得到填好）",
                      QStringLiteral("再点过 %1 / 还是同一张 %2").arg(secondClicked).arg(secondClickKeptPrompt));
                const QString presetSubTextAfter =
                    presetRow != nullptr
                        ? presetRow->data(MainWindowUI::loaderTreeKeyColumn(),
                                          MainWindowUI::rowSubTextRole())
                              .toString()
                        : QString();
                check(presetSubTextAfter == MainWindowUI::installPresetSummaryText(0),
                      "★★ 取消那一发什么都不写：那一行小字还是「未选择」"
                      "（P1 验收：取消不丢安装配置）",
                      presetSubTextAfter);
                check(gameStack->currentIndex() == pageBefore,
                      "★ 弹窗开着、收掉之后，下面那一页都没被换走（即开不跳页）",
                      QString("%1 → %2").arg(pageBefore).arg(gameStack->currentIndex()));
            }

            // Y2b. **列表是进这一页就全问出去的**，不是点开哪条才问哪条（要求：
            // "获取列表不能点开了再获取，得全获取完"）。所以此刻四条加载器底下都已经
            // 摆好了一格东西（占位「正在获取…」，离线时很快会被换成
            // 「自动（推荐）」+「列表没取到」）。两样都算数，判据是"底下不是空的"。
            //
            // NOTE: 这一条顺手守着另一个 bug（ 同一个改动里修的）：灌列表那一下
            // 如果顺手动了光标，`QTreeWidget::setCurrentItem()` 会把**收着的**父条目
            // 撑开（QTreeView::scrollTo 头一句就是"展开所有收着的父条目"）—— 表现是
            // "刚进安装页，四条加载器自己弹开了"。所以下面还要逐条断言它们**都收着**。
            {
                int loaderRows = 0;
                int collapsedRows = 0;
                int prefetched = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    VersionManager::LoaderKind rowKind = VersionManager::LoaderKind::Fabric;
                    if (!VersionManager::loaderKindFromIndex(i, &rowKind)) {
                        continue;  // 「不装（纯原版）」那条：原版没有加载器版本可问
                    }
                    ++loaderRows;
                    collapsedRows += rows.at(i)->isExpanded() ? 0 : 1;
                    prefetched += rows.at(i)->childCount() > 0 ? 1 : 0;
                }
                check(loaderRows == kindCount && prefetched == kindCount,
                      "进安装页时四条加载器底下都已经在动了（列表是一次性全问出去的，不是点开才问）",
                      QString("%1/%2").arg(prefetched).arg(kindCount));
                check(collapsedRows == kindCount,
                      "刚进安装页时四条加载器都还是**收着**的（没有哪一条自己弹开）",
                      QString("收着 %1/%2").arg(collapsedRows).arg(kindCount));
            }

            // Y2c. 点开一条加载器 → 选一个版本 → 再点开： 用户报的那一串
            // （"fabric选了自动以后列表就点不开了"）。根因在**灌列表那一下动了光标**
            // （见 Y2b 那段 NOTE），所以这一节**故意走产品自己那条填列表的路**
            // （onLoaderVersionsFetched 那个槽，真窗口上由
            // VersionManager::loaderVersionsFetched 打过来），而不是手工往树上塞条目
            // —— 用户报的那个 bug 就藏在"灌列表那一下"里，手工塞条目正好绕开它、
            // 那一节就成了一条永远绿的假断言。
            // 版本号是假的（离线拿不到真清单），形状和产品填的那份一致。
            if (loaderTree != nullptr && rows.size() == names.size()) {
                // 先等预取那四发各自落地（离线 = 四条失败、联网 = 真清单）。理由：
                // 每次进安装页只问一轮，等它们落地之后这一页就不会再有人来填了 ——
                // 下面填的那份才是**确定**的（不然一条迟到的失败回复会把假版本号冲掉，
                // 这一节就成了看运气）。上限 5 秒；等不到也照旧往下走。
                const auto firstChildText = [](QTreeWidgetItem *item) {
                    return item != nullptr && item->childCount() > 0
                               ? item->child(0)->text(MainWindowUI::loaderTreeKeyColumn())
                               : QString();
                };
                for (int i = 0; i < 100; ++i) {
                    if (firstChildText(rows.at(forgeIndex)) !=
                        MainWindowUI::loaderVersionFetchingText()) {
                        break;
                    }
                    QEventLoop turn;
                    QTimer::singleShot(50, &turn, &QEventLoop::quit);
                    turn.exec();
                }

                // 三个都是正式版 —— "只有 beta"那一路在 Y2d 里单独验。
                const LoaderMeta::LoaderVersionList fakes{
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("47.4.10"), true},
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("47.3.0"), true},
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("47.2.20"), true}};
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, forgeIndex),
                                          Q_ARG(bool, true),
                                          Q_ARG(LoaderMeta::LoaderVersionList, fakes),
                                          Q_ARG(QString, QString()));
                QApplication::processEvents();
                QTreeWidgetItem *row = rows.at(forgeIndex);
                check(!row->isExpanded() && row->childCount() == fakes.size() + 2,
                      "Y2c 前提：列表填进来了（两档 + 三个版本号 = 5 条），那一条**还收着**",
                      QString("收着=%1 子条目=%2")
                          .arg(row->isExpanded() ? 1 : 0)
                          .arg(row->childCount()));

                clickRow(loaderTree, row);
                check(row->isExpanded(),
                      "点一下加载器那一条，它展开了（底下就是能挑的那些版本号）");
                check(rowSubText(row).isEmpty(),
                      "★ 展开**不等于**选中它（第二行小字还是空的 —— 旧代码在这里就写成「自动（推荐）」了）",
                      rowSubText(row));
                clickRow(loaderTree, row);
                check(!row->isExpanded(),
                      "★ 再点一下那一条，它**收起来**（用户原话：「点一下，展开，再点一下 关闭」）",
                      QString("展开=%1").arg(row->isExpanded() ? 1 : 0));
                // 选中 = 点底下那两档里的一档。这里点「下载稳定版」——
                // 它就是「自动（推荐）」那一档挑的东西（正式版），userData 是 `@stable`
                // （见 LoaderMeta::loaderChannelKey）。
                clickRow(loaderTree, row);
                clickRow(loaderTree, row->child(1));
                check(!row->isExpanded(),
                      "点「下载稳定版」那一条，那一列**收起来**");
                check(rowSubText(row) == MainWindowUI::loaderVersionChannelShortText(false),
                      "收起来之后挑的那一档写在那一行的第二行小字上（用户原话：选择版本以后收缩，显示版本）",
                      rowSubText(row));
                // 其余几条加载器：要求"剩余的加载器（如果不兼容的话）显示为与【已选中的
                // 加载器名称】不兼容"。这句话是**一处**算出来的（MainWindowUI::loaderConflictText），
                // 这里逐条比对它，而不是自己拼一句 —— 自己拼的话，产品换一种说法这条就红了。
                const QString wantConflict = MainWindowUI::loaderConflictText(
                    VersionManager::loaderDisplayName(LoaderKind::Forge));
                int conflicted = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    if (i == forgeIndex || i == noneIndex) {
                        continue;
                    }
                    conflicted += rowSubText(rows.at(i)) == wantConflict ? 1 : 0;
                }
                check(conflicted == kindCount - 1,
                      "选了 Forge 之后，其余三条加载器写着「与 Forge 不兼容」（一个都不能漏）",
                      QString("%1/%2，样子：%3")
                          .arg(conflicted)
                          .arg(kindCount - 1)
                          .arg(rowSubText(rows.value(0))));
                check(rowSubText(rows.at(noneIndex)).isEmpty(),
                      "「不装（纯原版）」那条**没有**不兼容那句话（它不是加载器）",
                      rowSubText(rows.at(noneIndex)));
                // 要求："选择以后让其他选项变成不可用（不能点开）的模式"。
                // 三条：其余几条变灰（不可用）、灰的那几条**点不开**、选中的那条和
                // 「不装（纯原版）」仍然可用（前者要能回去改版本，后者是唯一的退路 ——
                // 其余几条都灰着的时候总得有一条点得动，不然就选不回纯原版了）。
                int disabledOthers = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    if (!VersionManager::loaderKindFromIndex(i, nullptr)) {
                        continue;  // 「不装（纯原版）」那条不参与
                    }
                    if (i == forgeIndex) {
                        continue;
                    }
                    disabledOthers += rows.at(i)->isDisabled() ? 1 : 0;
                }
                check(disabledOthers == kindCount - 1,
                      "★ 选中的那条之外，其余三条加载器都变灰了（不可用）",
                      QString("%1/%2").arg(disabledOthers).arg(kindCount - 1));
                check(!row->isDisabled() && !rows.at(noneIndex)->isDisabled(),
                      "★ 选中的那条和「不装（纯原版）」仍然可用（前者改版本、后者是退路）");
                // 要求："选中以后在那个 > 左侧加一个 x 来取消安装对应版本"。
                // 那个标记挂在 rowClearRole() 上（代理按它画、MainWindow 按它判断点中了
                // 没有），五条里**只有选中的那条**该带着它。
                const auto rowClearFlag = [loaderTree](QTreeWidgetItem *item) {
                    return item != nullptr &&
                           item->data(MainWindowUI::loaderTreeKeyColumn(),
                                      MainWindowUI::rowClearRole())
                               .toBool();
                };
                int clearFlagged = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    clearFlagged += rowClearFlag(rows.at(i)) ? 1 : 0;
                }
                check(clearFlagged == 1 && rowClearFlag(row),
                      "★ 五条里只有选中的那条带着「取消」小叉（别人没有可取消的东西）",
                      QString("%1/%2").arg(clearFlagged).arg(rows.size()));
                // 那一格的几何由代理给（VersionTreeDelegate::clearRect()）—— 这里就用
                // 产品那一份算，别自己再推一遍（自己推的会和画出来的错开，而那正是"看着
                // 点中了叉、其实点的是那一行"）。
                const QRect crossBox =
                    VersionTreeDelegate::clearRect(loaderTree->visualItemRect(row));
                check(!crossBox.isNull() &&
                          crossBox.width() > 0 &&
                          crossBox.right() < loaderTree->viewport()->width() &&
                          crossBox.left() > loaderTree->visualItemRect(row).center().x(),
                      "那个叉算得出来、落在行里靠右那一格（在右边那个 > 的左边）",
                      QString("%1x%2 @x=%3，行 %4 宽")
                          .arg(crossBox.width())
                          .arg(crossBox.height())
                          .arg(crossBox.x())
                          .arg(loaderTree->visualItemRect(row).width()));
                // 点叉之前那一行**是收着的**（选中它那一下会把它收起来）。点完还收着 =
                // 那一下**没有**被当成"点了这一行"（那种点法是展开/收起）—— 用户按住取消、
                // 看到的却是它自己展开一下，那就是这一条要防的。
                check(!row->isExpanded(), "点叉之前那一行是收着的（下面才好判那一下有没有走偏）");
                clickAt(loaderTree, crossBox.center());
                check(rowSubText(row).isEmpty() && !rowClearFlag(row) &&
                          rowSubText(rows.at(noneIndex)).isEmpty(),
                      "★ 点那个叉 = 取消装这个加载器（退回「不装（纯原版）」，小叉跟着消失）",
                      QString("Forge 小字=「%1」 叉=%2")
                          .arg(rowSubText(row))
                          .arg(rowClearFlag(row) ? 1 : 0));
                check(!row->isExpanded(),
                      "★ 点叉那一下**没有**顺手把那一行展开/收起（点的是行里的按钮，不是那一行）",
                      QString("展开=%1").arg(row->isExpanded() ? 1 : 0));
                check(installNameEdit->text() == kPickedId,
                      "★ 取消之后默认名跟着退回纯原版（里面不再带 -Forge）",
                      installNameEdit->text());
                int freedAgain = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    freedAgain += rows.at(i)->isDisabled() ? 0 : 1;
                }
                check(freedAgain == rows.size(),
                      "★ 取消之后四条加载器一起解禁（又能点开了）",
                      QString("%1/%2").arg(freedAgain).arg(rows.size()));
                // 反证：点在**叉左边一点点**（还是这一行、但不是那个叉）—— 那一下仍然
                // 是"点这一行"，该展开就展开。没有这一条的话，"把整个行尾都吞掉"那种改坏
                // 了过滤范围的写法也能全绿。
                clickAt(loaderTree, QPoint(crossBox.left() - 4, crossBox.center().y()));
                check(row->isExpanded(),
                      "★ 叉左边一点点那一下仍然是「点这一行」（展开）—— 过滤器只管那个叉",
                      QString("展开=%1").arg(row->isExpanded() ? 1 : 0));
                clickAt(loaderTree, QPoint(crossBox.left() - 4, crossBox.center().y()));
                check(!row->isExpanded(), "再点一下那一点，收起来（那一行还是老规矩）");
                // 回到"选中 Forge"这个状态，好接着往下验"选中的那条之外都灰了"。
                clickRow(loaderTree, row);
                clickRow(loaderTree, row->child(1));
                check(rowSubText(row) == MainWindowUI::loaderVersionChannelShortText(false),
                      "重新选中 Forge（下面几条接着按「选中的是 Forge」来验）",
                      rowSubText(row));
                QTreeWidgetItem *other = rows.at(fabricIndex);
                clickRow(loaderTree, other);
                check(!other->isExpanded() && rowSubText(other) == wantConflict,
                      "★ 点一条灰掉的加载器：没反应（没展开、也没变成选它）",
                      QString("展开=%1 小字=%2")
                          .arg(other->isExpanded() ? 1 : 0)
                          .arg(rowSubText(other)));
                // 这一条就是用户报的那个"点不开了"（选中之后仍然来回都能开）。
                clickRow(loaderTree, row);
                check(row->isExpanded(),
                      "★ 选中的那一条再点还是能展开（「点不开了」正是这一条）",
                      QString("展开=%1 子条目=%2")
                          .arg(row->isExpanded() ? 1 : 0)
                          .arg(row->childCount()));
                // 具体版本号从第 3 条起（0/1 是那两档）。故意点**第 4 条**：它挑的是
                // fakes 里第 2 个，钉住"点哪一条就钉哪一条"，不是"永远挑第一条版本号"。
                clickRow(loaderTree, row->child(3));
                check(!row->isExpanded() && rowSubText(row) == fakes.at(1).version,
                      "点一个具体版本号：那一列收起来，第二行小字换成那个版本号",
                      rowSubText(row));
                clickRow(loaderTree, row);
                check(row->isExpanded(),
                      "再点开一次还是能开（这一串动作来回走都不该失灵）",
                      QString("展开=%1").arg(row->isExpanded() ? 1 : 0));
                // 「不装（纯原版）」那条：它没有子条目可展开，点它就是**选中它**（回纯原版）。
                clickRow(loaderTree, rows.at(noneIndex));
                check(rowSubText(row).isEmpty(),
                      "★ 点「不装（纯原版）」= 选中它，那几句「不兼容」一起消失",
                      rowSubText(row));
                int enabledAgain = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    enabledAgain += rows.at(i)->isDisabled() ? 0 : 1;
                }
                check(enabledAgain == rows.size(),
                      "★ 回到「不装」之后，四条加载器一起解禁（都能点开了）",
                      QString("%1/%2").arg(enabledAgain).arg(rows.size()));
                // 收尾：把这一条还原成"没挑过"的样子。**不删** row 底下的子条目 ——
                // Y3 那一段要按"点一条版本号"那条真路选中 Forge，而那条路要求有版本可点
                // （列表是进安装页时才问的，删了就得自己造假条目）。只把钉的版本退回
                // 「稳定档」那一档，再选回「不装」。
                clickRow(loaderTree, row->child(1));
                clickRow(loaderTree, rows.at(noneIndex));
                check(rowSubText(row).isEmpty() && !row->isExpanded(),
                      "Y2c 收尾：选回「不装」，那一条的第二行小字没了、也收着",
                      rowSubText(row));

                // 要求："如果对应版本不支持，就不要保留自动了，这个情况下的
                // 列表没取到 换成无可用版本"。这是**另一件事**：问到了、清单里就是没有
                // （不是没问到）。数据层那边（VersionManager::parseLoaderVersions）把
                // 这两种答案分开了 —— 空列表照 success 发出来，error 只留给"没问到"；
                // 这里验的是界面按这个分别做对了什么。
                // 拿 OptiFine 那一条来做：Forge 底下还有条目，Y3 要用。
                QTreeWidgetItem *emptyRow = rows.at(optifineIndex);
                // 先等四条预取都落地（谁落得晚，谁就会把下面填的这份冲掉 —— 那样这一节
                // 就成了看运气）。上限 5 秒，等不到也照旧往下走。
                for (int i = 0; i < 100; ++i) {
                    bool fetching = false;
                    for (int r = 0; r < rows.size(); ++r) {
                        fetching = fetching || firstChildText(rows.at(r)) ==
                                                   MainWindowUI::loaderVersionFetchingText();
                    }
                    if (!fetching) {
                        break;
                    }
                    QEventLoop turn;
                    QTimer::singleShot(50, &turn, &QEventLoop::quit);
                    turn.exec();
                }
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, optifineIndex),
                                          Q_ARG(bool, true),
                                          Q_ARG(LoaderMeta::LoaderVersionList,
                                                LoaderMeta::LoaderVersionList()),
                                          Q_ARG(QString, QString()));
                QApplication::processEvents();
                const auto childText = [](QTreeWidgetItem *item, int i) {
                    return item != nullptr && item->childCount() > i
                               ? item->child(i)->text(MainWindowUI::loaderTreeKeyColumn())
                               : QString();
                };
                check(emptyRow->childCount() == 1,
                      "★ 这一版一个能装的都没有时，底下**只剩一条** —— 那两档都不留了"
                      "（用户原话：「如果对应版本不支持，就不要保留自动了」）",
                      QString("子条目=%1：%2")
                          .arg(emptyRow->childCount())
                          .arg(childText(emptyRow, 0)));
                check(childText(emptyRow, 0) == MainWindowUI::loaderVersionUnavailableText(),
                      "★ 那一条写的是「无可用版本」（用户原话：「这个情况下的 列表没取到 换成"
                      "无可用版本」）",
                      childText(emptyRow, 0));
                check(childText(emptyRow, 1) != MainWindowUI::loaderVersionFailedText() &&
                          emptyRow->child(0)->text(MainWindowUI::loaderTreeKeyColumn()) !=
                              MainWindowUI::loaderVersionFailedText(),
                      "★ 它**不是**「列表没取到」—— 那句话留给「没问到」，两件事不能混"
                      "（混了的话，断网和「这版就是没有」在屏幕上一模一样）",
                      MainWindowUI::loaderVersionUnavailableText() + QStringLiteral(" / ") +
                          MainWindowUI::loaderVersionFailedText());
                check(emptyRow->child(0)->isDisabled(),
                      "那条「无可用版本」是灰的（它只是把原因写出来，不是给人点的）");
                // 点它不该选中这一条加载器 —— 一个能装的版本都没有的时候，选中它就是让
                // 用户走上一条必然装不出来的路（安装那一下会以"X 还没有支持 Y 的版本"收场）。
                clickRow(loaderTree, emptyRow);
                clickRow(loaderTree, emptyRow->child(0));
                check(rowSubText(emptyRow).isEmpty() &&
                          !installNameEdit->text().contains(QStringLiteral("OptiFine")),
                      "★ 点它没反应：这一条**选不中**（底下没有能装的版本）",
                      QString("小字=「%1」 名字=%2").arg(rowSubText(emptyRow)).arg(installNameEdit->text()));
                // 反证（同一件事的另一半）：**没问到**（success=false）仍然是老样子 ——
                // 底下留着「自动（推荐）」、外加一条灰的「列表没取到」。没有这一条的话，
                // "把两种情况都当成没问到"那种改法也能全绿，而那正是这次要分开的东西。
                // 拿 Fabric 那一条做（Forge 底下那份 Y3 还要用）。
                QTreeWidgetItem *failedRow = rows.at(fabricIndex);
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, fabricIndex),
                                          Q_ARG(bool, false),
                                          Q_ARG(LoaderMeta::LoaderVersionList,
                                                LoaderMeta::LoaderVersionList()),
                                          Q_ARG(QString, QStringLiteral("网络不通")));
                QApplication::processEvents();
                check(failedRow->childCount() == 3 &&
                          childText(failedRow, 0) == MainWindowUI::loaderVersionLatestText() &&
                          childText(failedRow, 1) == MainWindowUI::loaderVersionStableText() &&
                          childText(failedRow, 2) == MainWindowUI::loaderVersionFailedText(),
                      "★ 反证：**没问到**的时候那两档照留、底下写着「列表没取到」"
                      "（不钉版本那条路不靠这份列表，所以列不出来也不该把安装堵死）",
                      QString("%1 条：%2 / %3 / %4")
                          .arg(failedRow->childCount())
                          .arg(childText(failedRow, 0))
                          .arg(childText(failedRow, 1))
                          .arg(childText(failedRow, 2)));
                check(!failedRow->child(0)->isDisabled() && !failedRow->child(1)->isDisabled() &&
                          failedRow->child(2)->isDisabled(),
                      "★ 那两档仍然点得动（这一刻并不知道有没有正式版），灰的只是那句「列表没取到」");
                clickRow(loaderTree, failedRow);
                clickRow(loaderTree, failedRow->child(1));
                check(rowSubText(failedRow) == MainWindowUI::loaderVersionChannelShortText(false),
                      "★ 反证：没问到的时候「稳定版」那一档还是能选中的（选完小字写着它）",
                      rowSubText(failedRow));
                clickRow(loaderTree, rows.at(noneIndex));
                check(rowSubText(failedRow).isEmpty(),
                      "收尾：选回「不装」，Fabric 那条的小字也跟着清干净",
                      rowSubText(failedRow));

                // Y2d. ★ 用户报的那一版（NeoForge 26.3）：上游**只发过 `-beta`**，
                // 一条正式版都没有。三件事一起验：
                // · 「下载稳定版」那一行**灰掉**、第二行小字写「没有正式版」
                // —— 留着一条能点的它等于让用户挑一条必然装不出来的路；
                // · beta 那几条**照列、还能点**（要求："beta 版虽然不稳定，但是也能
                // 使用"），只是每条第二行小字写着「测试版」——那是"它是什么"，不是
                // "它不行"；
                // · 用户什么都还没挑的时候，默认那一档得**挪到「最新版」**上，不然点安装
                // 走的还是稳定档那条必然装不出来的路（光标该落在最新档那一条上）。
                QTreeWidgetItem *betaRow = rows.at(neoforgeIndex);
                // 先等预取那一发落地（不然一条迟到的真回复会把下面这份冲掉，这一节就成了
                // 看运气）。上限 5 秒，等不到也照旧往下走。
                for (int i = 0; i < 100; ++i) {
                    if (firstChildText(betaRow) != MainWindowUI::loaderVersionFetchingText()) {
                        break;
                    }
                    QEventLoop turn;
                    QTimer::singleShot(50, &turn, &QEventLoop::quit);
                    turn.exec();
                }
                const LoaderMeta::LoaderVersionList betas{
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("26.3.0.8-beta"), false},
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("26.3.0.7-beta"), false},
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("26.3.0.6-beta"), false}};
                // 先展开：光标只在"那一行开着"的时候才会被放（见 fillLoaderVersionItems 里
                // 那段 NOTE），而下面正是要靠光标判"默认那一档挪到哪儿了"。
                clickRow(loaderTree, betaRow);
                check(betaRow->isExpanded() && rowSubText(betaRow).isEmpty(),
                      "Y2d 前提：NeoForge 那一条展开了、而且还没被选中过（第二行小字空着）",
                      QStringLiteral("展开=%1 小字=「%2」")
                          .arg(betaRow->isExpanded() ? 1 : 0)
                          .arg(rowSubText(betaRow)));
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, neoforgeIndex),
                                          Q_ARG(bool, true),
                                          Q_ARG(LoaderMeta::LoaderVersionList, betas),
                                          Q_ARG(QString, QString()));
                QApplication::processEvents();
                check(betaRow->childCount() == betas.size() + 2,
                      "Y2d 两档 + 三条 beta = 5 条（beta 一条不落）",
                      QString::number(betaRow->childCount()));
                check(childText(betaRow, 0) == MainWindowUI::loaderVersionLatestText() &&
                          childText(betaRow, 1) == MainWindowUI::loaderVersionStableText(),
                      "Y2d 头两条就是那两档（最新版在前、稳定版在后）",
                      QStringLiteral("%1 / %2")
                          .arg(childText(betaRow, 0), childText(betaRow, 1)));
                check(childText(betaRow, 2) == QStringLiteral("26.3.0.8-beta") &&
                          childText(betaRow, 4) == QStringLiteral("26.3.0.6-beta"),
                      "Y2d 两档之后才是版本号，一条不少",
                      QStringLiteral("%1 / %2").arg(childText(betaRow, 2), childText(betaRow, 4)));
                int betaMarked = 0;
                int betaClickable = 0;
                for (int i = 2; i < betaRow->childCount(); ++i) {
                    betaMarked += rowSubText(betaRow->child(i)) ==
                                          MainWindowUI::loaderVersionBetaSubText()
                                      ? 1
                                      : 0;
                    betaClickable += betaRow->child(i)->isDisabled() ? 0 : 1;
                }
                check(betaMarked == betas.size() && betaClickable == betas.size(),
                      "★ Y2d 三条 beta 都写着「测试版」**而且都点得动**（beta 能用，不是不给装）",
                      QStringLiteral("标了 %1 条 / 能点 %2 条").arg(betaMarked).arg(betaClickable));
                check(betaRow->child(1)->isDisabled() &&
                          rowSubText(betaRow->child(1)) ==
                              MainWindowUI::loaderVersionStableMissingText(),
                      "★ Y2d 一条正式版都没有 -> 「下载稳定版」那一行**灰掉**、小字写「没有正式版」",
                      QStringLiteral("灰=%1 小字=「%2」")
                          .arg(betaRow->child(1)->isDisabled() ? 1 : 0)
                          .arg(rowSubText(betaRow->child(1))));
                check(!betaRow->child(0)->isDisabled(),
                      "★ Y2d 「下载最新版」那一行照样点得动（它挑得出东西）");
                // 默认那一档挪没挪走：用户没挑过（小字还是空的），光标却已经落在「最新版」
                // 那一条上了 —— 这就是"产品自己把默认从稳定档挪开"的证据（点安装走的是它）。
                QTreeWidgetItem *betaCurrent = loaderTree->currentItem();
                check(betaCurrent == betaRow->child(0),
                      "★ Y2d 没人挑过、又一条正式版都没有 -> 光标落在「下载最新版」那条上"
                      "（默认那一档自己挪走了）",
                      betaCurrent != nullptr
                          ? betaCurrent->text(MainWindowUI::loaderTreeKeyColumn())
                          : QStringLiteral("（光标不在任何一条上）"));
                // 点那条灰的稳定档：没反应（用户挑不中一条必然装不出来的路）
                clickRow(loaderTree, betaRow->child(1));
                check(rowSubText(betaRow).isEmpty(),
                      "★ Y2d 点那条灰的「下载稳定版」没反应（选不中它）",
                      rowSubText(betaRow));
                // 点「下载最新版」= 选中这一条加载器
                clickRow(loaderTree, betaRow->child(0));
                check(rowSubText(betaRow) == MainWindowUI::loaderVersionChannelShortText(true),
                      "★ Y2d 点「下载最新版」= 选中它，小字写「最新版」",
                      rowSubText(betaRow));
                check(installNameEdit->text() ==
                          kPickedId + QStringLiteral("-") + QStringLiteral("NeoForge"),
                      "Y2d 选中之后默认名跟着走（原版号-加载器名）",
                      installNameEdit->text());
                // 点一条 beta 版本号：钉死那一条（它就是"能用的那个测试版"）
                clickRow(loaderTree, betaRow);
                clickRow(loaderTree, betaRow->child(3));
                check(rowSubText(betaRow) == QStringLiteral("26.3.0.7-beta"),
                      "★ Y2d 点一个 beta 版本号也照样钉得死（第 4 条点的是第 2 个 beta）",
                      rowSubText(betaRow));
                // 收尾：选回「不装」，别把这个状态留给后面几节
                clickRow(loaderTree, rows.at(noneIndex));
                check(rowSubText(betaRow).isEmpty() && !installNameEdit->text().contains(
                                                          QStringLiteral("NeoForge")),
                      "Y2d 收尾：选回「不装」，小字清干净、默认名也退回纯原版",
                      QStringLiteral("小字=「%1」 名字=%2")
                          .arg(rowSubText(betaRow))
                          .arg(installNameEdit->text()));

                // Y2e. Quilt 接入安装页：支持版本可选、其余四条置灰互斥、不支持版本显示「无可用版本」。
                QTreeWidgetItem *quiltRow = rows.at(quiltIndex);
                const LoaderMeta::LoaderVersionList quiltVersions{
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("0.27.0-beta.1"), false},
                    LoaderMeta::LoaderVersionEntry{QStringLiteral("0.26.4"), true}};
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, quiltIndex),
                                          Q_ARG(bool, true),
                                          Q_ARG(LoaderMeta::LoaderVersionList, quiltVersions),
                                          Q_ARG(QString, QString()));
                QApplication::processEvents();
                check(quiltRow->childCount() == 4 &&
                          childText(quiltRow, 2) == QStringLiteral("0.27.0-beta.1") &&
                          childText(quiltRow, 3) == QStringLiteral("0.26.4"),
                      "Y2e Quilt 兼容版本列表灌入（两档 + 2 个版本）",
                      QString::number(quiltRow->childCount()));
                clickRow(loaderTree, quiltRow);
                clickRow(loaderTree, quiltRow->child(3));
                check(!quiltRow->isExpanded() && rowSubText(quiltRow) == QStringLiteral("0.26.4") &&
                          installNameEdit->text() ==
                              kPickedId + QStringLiteral("-Quilt_0.26.4"),
                      "Y2e 选中 Quilt 0.26.4：行收起、小字写版本号、默认名拼成 <mc>-Quilt_0.26.4",
                      installNameEdit->text());
                const QString wantQuiltConflict = MainWindowUI::loaderConflictText(
                    VersionManager::loaderDisplayName(LoaderKind::Quilt));
                int quiltDisabledOthers = 0;
                for (int i = 0; i < rows.size(); ++i) {
                    if (!VersionManager::loaderKindFromIndex(i, nullptr) || i == quiltIndex) continue;
                    if (rows.at(i)->isDisabled() && rowSubText(rows.at(i)) == wantQuiltConflict) {
                        ++quiltDisabledOthers;
                    }
                }
                check(quiltDisabledOthers == kindCount - 1,
                      "Y2e 选中 Quilt 后，其余四条加载器写「与 Quilt 不兼容」并禁用",
                      QString("%1/%2").arg(quiltDisabledOthers).arg(kindCount - 1));
                clickRow(loaderTree, rows.at(noneIndex));
                QMetaObject::invokeMethod(&window, "onLoaderVersionsFetched", Qt::DirectConnection,
                                          Q_ARG(QString, kPickedId), Q_ARG(int, quiltIndex),
                                          Q_ARG(bool, true),
                                          Q_ARG(LoaderMeta::LoaderVersionList, LoaderMeta::LoaderVersionList()),
                                          Q_ARG(QString, QString()));
                QApplication::processEvents();
                check(quiltRow->childCount() == 1 &&
                          childText(quiltRow, 0) == MainWindowUI::loaderVersionUnavailableText() &&
                          quiltRow->child(0)->isDisabled(),
                      "Y2e Quilt 不兼容游戏版本：仅显示灰色的「无可用版本」",
                      childText(quiltRow, 0));
                clickRow(loaderTree, quiltRow);
                clickRow(loaderTree, quiltRow->child(0));
                check(rowSubText(quiltRow).isEmpty() && !installNameEdit->text().contains(QStringLiteral("Quilt")),
                      "Y2e 点击「无可用版本」不会选中 Quilt");
                if (quiltRow->isExpanded()) clickRow(loaderTree, quiltRow);
            }
            // 翻回原来那一页（后面几节拿窗口当"开局那个样子"用）。
            if (downloadTabs != nullptr) {
                downloadTabs->setCurrentIndex(tabBeforeInstall);
                QApplication::processEvents();
            }

            // 选中 Forge：**点它底下「下载稳定版」那一条**（ 起点顶层
            // 只管展开/收起，选中是点它底下那一条那一下的事）。默认名跟着变成 `1.21.4-Forge`
            // （真机上那些实例就是这个形状）。
            clickRow(loaderTree, rows.at(forgeIndex));  // 先展开（底下才有条目可点）
            clickRow(loaderTree, rows.at(forgeIndex)->child(1));
            check(installNameEdit->text() ==
                      kPickedId + QStringLiteral("-") + QStringLiteral("Forge"),
                  "选了 Forge 的「下载稳定版」，默认名跟着加载器走（原版号-加载器名）",
                  installNameEdit->text());
            check(rowSubText(rows.at(forgeIndex)) == MainWindowUI::loaderVersionChannelShortText(false),
                  "那一条的第二行小字写着它现在钉的是哪一档（还没挑具体版本，所以是「稳定版」）",
                  rowSubText(rows.at(forgeIndex)));
            // Y3. 勾一个**接通了、但这一次装不了**的加载器：版本 json 要的 Java 本机给不出来，
            // 就得在"还没开始装"那一刻停下 —— 弹「Java 版本不够」、界面不进忙态、一个
            // 字节都不下，也不许拿着一份认不出来的 Java 硬上（那样用户等半天等来一句
            // "下载失败"，永远猜不到该去装个新 Java）。
            //
            // 这一条验的是 onInstallButtonClicked() → pickInstallerJava() 那段接线：
            // 版本 json 的 javaVersion.majorVersion → GlobalJavaSettings::installerJavaFor()。
            // installerJavaFor() 自己的判据（"配的那份太老才换"、"够新的里面挑最小的
            // 那个"）由 verify 的 M 段逐条钉住；这里**唯一**能错的就是没接上 ——
            // 忘了读版本 json、读错了键、或者这段整个不在。
            //
            // 为什么写 majorVersion = 99：这条得在**任何**机器上离线跑绿，不能依赖
            // "本机最高是 Java 多少"。99 是个没有机器能满足的要求，于是
            // installerJavaFor() 必然走到"挑不出够新的"那一支。
            // 为什么先把配置指到一个不存在的路径上：空路径 = 走 PATH 上那份，那条路
            // （"压根没配过"就是这个状态）会原样往下装。这里走的是设置页那个组合框 ——
            // 用户真会走的那条线，顺带验了它落盘。
            writeFileAt(pickedJson,
                        QByteArrayLiteral("{\"id\":\"1.21.4\",\"javaVersion\":{\"majorVersion\":99}}"));
            const QString kBogusJava = QStringLiteral("C:/没有这个目录/java.exe");
            check(windowJava != nullptr, "拿得到窗口里那份 Java 设置（下面要把它钉死）");
            if (windowJava != nullptr) {
                pinGlobalJava(kBogusJava);
                QApplication::processEvents();
                GlobalJavaSettings probe;
                check(probe.javaPath() == kBogusJava && !probe.autoSelectJava(),
                      "设置页里选中那份不存在的 Java 之后，盘上也认它（手动档）",
                      probe.javaPath() + QStringLiteral(" auto=") +
                          (probe.autoSelectJava() ? QStringLiteral("1") : QStringLiteral("0")));

                const int installsBeforeJava =
                    countOccurrences(messagesSnapshot(), QStringLiteral("开始安装"));
                dismissNextPrompt();
                installBtn->click();
                QApplication::processEvents();
                check(g_modalTitle == QStringLiteral("Java 版本不够"),
                      "点「安装」弹的是「Java 版本不够」",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
                check(g_modalWasWarning, "它是警告框（这是一条错误，不是「还没做」那种提示）");
                check(g_modalText.contains(QStringLiteral("Java 99")) &&
                          g_modalText.contains(QStringLiteral("设置")),
                      "弹窗说清了要哪个 Java、以及该去哪儿指路径", g_modalText);
                check(installBtn->isEnabled() && installNameEdit->isEnabled(),
                      "界面没进忙态（「安装」按钮和名称框还是亮的）");
                check(loaderTree->isEnabled() && !rows.at(forgeIndex)->isDisabled(),
                      "加载器那棵树也没灰掉（整棵树只在忙态才灰；Forge 是选中的那条，"
                      "也不该是灰的 —— 灰的那几条是「没选中」的那个意思）");
                check(installNameEdit->text() ==
                          kPickedId + QStringLiteral("-") + QStringLiteral("Forge"),
                      "选的 Forge 还在（拒绝一次不该把选择改掉）", installNameEdit->text());
                check(countOccurrences(messagesSnapshot(), QStringLiteral("开始安装")) ==
                          installsBeforeJava,
                      "一次安装都没开始过（没拿着那份认不出来的 Java 硬上）",
                      QString::number(installsBeforeJava) + QStringLiteral(" → ") +
                          QString::number(countOccurrences(messagesSnapshot(),
                                                           QStringLiteral("开始安装"))));
                check(countOccurrences(messagesSnapshot(),
                                       QStringLiteral("这个版本要的 Java 拿不到")) >= 1,
                      "日志里也留了一句");

                // 收拾：翻回「自动」那一档。**路径本身清不掉**（产品有意不让"自动"这一档
                // 顺手抹掉上次挑中的路径 —— 见 onJavaUseAuto() 里那句注释），所以盘上还
                // 留着那条不存在的路径；后面只剩 Y4 的原版那条路和 O 节的清理，都不读 Java。
                unpinGlobalJava();
                QApplication::processEvents();
                check(windowJava->autoSelectJava() &&
                          windowJava->javaPath() == kBogusJava,
                      "翻回「自动」了，而上次挑中的那条路径照旧留在盘上（那是缓存，不清）",
                      QStringLiteral("auto=%1 path=%2")
                          .arg(windowJava->autoSelectJava() ? 1 : 0)
                          .arg(windowJava->javaPath()));
            }

            // Y4. 点回「不装（纯原版）」再点「安装」：走回原来那条路（先问一句"要不要
            // 校验并修复"），**不装任何加载器**。产品在"这个版本下过没有"那一问上判的
            // 就是 <版本>.json 在不在，所以答「否」之后它会就地停下，不碰网络。
            clickRow(loaderTree, rows.at(noneIndex));
            check(installNameEdit->text() == kPickedId,
                  "点回「不装」，默认名跟着回到原版号本身（用户没自己改过名字）",
                  installNameEdit->text());
            check(rowSubText(rows.at(forgeIndex)).isEmpty(),
                  "没选加载器了，那句「不兼容」也一起消失（那句话的主语没了）",
                  rowSubText(rows.at(forgeIndex)));
            answerNextPrompt(1);
            installBtn->click();
            QApplication::processEvents();
            check(g_modalTitle == QStringLiteral("版本已存在"),
                  "走的是原版那条路（问的是「版本已存在，要不要校验」）",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(g_modalText.contains(QStringLiteral("校验")), "问的是校验并补齐那件事", g_modalText);
            // NOTE: 这句话里**没有**「版本」两个字（"已取消对 1.21.4 的校验"）：安装页那
            // 一问对"装原版"和"装一个带名字的实例"是同一句话，名字可能是
            // `1.21.4-Forge_47.4.10` 这种 —— 加个「版本」反而说不通。
            check(countOccurrences(messagesSnapshot(),
                                   QStringLiteral("已取消对 1.21.4 的校验")) == 1,
                  "答「否」之后就地停下（没有开始任何下载）");
            check(installBtn->isEnabled() && installNameEdit->isEnabled(),
                  "界面同样没进忙态");
            const int installsAfterY = countOccurrences(messagesSnapshot(), QStringLiteral("开始安装"));
            check(installsAfterY == installsBeforeY, "这一段里一次加载器安装都没开始过",
                  QString("%1 → %2").arg(installsBeforeY).arg(installsAfterY));

            // Y5. 名字不合法（要求："如果版本名字存在非法字符，不要替换掉
            // 那个「装好后在实例...」，而是在下面添加一行红色字"）：原因写在名称框底下
            // **另起的那行红字**里，上面那句"装好之后在「实例」页里能看到它"**照旧
            // 写着**；同时「安装」点不动。
            // 走的是**用户真会走的那条线**（名称框里敲字 → textChanged），不是直接
            // 调刷新函数 —— 少一根 connect 的症状正是"敲了没反应"，而那种毛病
            // 从"函数本身对不对"上看不出来。
            // NOTE: 判"这行红字显没显形"用的是 **isHidden()**，不是 isVisible()。理由：
            // 这一节开头那一下把下载页翻到前面来过，但 Y2d 收尾时又把选项卡翻回去了
            // （见上面那段"翻回原来那一页"），所以这一刻**那一页上每个控件的
            // isVisible() 都是 false** —— 拿它判"产品让不让这行字显形"会永远判红，
            // 而且红得跟被测的改动毫无关系。isHidden() 说的是"产品自己按下去的那个
            // 显/隐开关"（QWidget::setVisible），跟祖先链在不在前面无关，正是这里
            // 要量的东西。
            const QString invalidName = QStringLiteral("a/b");
            installNameEdit->setText(invalidName);
            QApplication::processEvents();
            // 那一行红字写的就是数据层那句原因（界面不抄一份规则，所以这里比的也是它）。
            check(!installNameError->isHidden() &&
                      installNameError->text() == InstanceFiles::validateDirName(invalidName),
                  "★ 名字里有非法字符时，底下多出一行红字，写的是数据层那句原因",
                  QStringLiteral("显出来=%1 字=「%2」")
                      .arg(installNameError->isHidden() ? 0 : 1)
                      .arg(installNameError->text()));
            check(!installBtn->isEnabled(), "名字不合法时「安装」是灰的（省掉一次往返）");
            installNameEdit->setText(QString());
            QApplication::processEvents();
            check(!installNameError->isHidden() && !installNameError->text().isEmpty(),
                  "名字清空时那行红字说的是「不能是空的」",
                  installNameError->text());
            check(!installBtn->isEnabled(), "空名字时「安装」也是灰的");
            // 名字改回能用的：红字**收起来**（不是留一行空的）。
            installNameEdit->setText(QStringLiteral("1.21.4-Forge"));
            QApplication::processEvents();
            check(installNameError->isHidden() && installNameError->text().isEmpty(),
                  "★ 名字能用了 -> 那行红字连人带字一起收起来（不是留一行空的在那儿）",
                  QStringLiteral("显出来=%1 字=「%2」")
                      .arg(installNameError->isHidden() ? 0 : 1)
                      .arg(installNameError->text()));
            check(installBtn->isEnabled(), "名字能用了，「安装」又亮了");

            // 收拾：这个假版本目录不能留给 O 节（O 要数沙盒里有几个版本）。删之前
            // 先确认它本来不在沙盒里 —— 免得哪天真有人拿 1.21.4 当固定装置。
            QDir(versionsDir.filePath(kPickedId)).removeRecursively();
            check(!QDir(versionsDir.filePath(kPickedId)).exists(), "Y3/Y4 那个假版本目录删干净了");

            // 把选择恢复成产品的默认值。后面 O 节还会刷一次下拉框，让它看到的是
            // "开局那个样子"，别把 Y 段的试验品留在窗口上。
            // NOTE: 这里**不比名称框** —— Y5 刚把名字清空过（"空名字"那条），而用户动过
            // 名称框之后默认名就不再跟着加载器走了（onInstallNameEdited 那条规矩），
            // 所以这会儿框里本来就是空的。要比的是树上那几句小字。
            clickRow(loaderTree, rows.at(noneIndex));
            check(rowSubText(rows.at(forgeIndex)).isEmpty(),
                  "Y 段收尾：点回「不装」，那几句「不兼容」也一起没了",
                  rowSubText(rows.at(forgeIndex)));
        }
    }
    return 0;
}


