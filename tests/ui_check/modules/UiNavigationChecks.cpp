#include "Support.hpp"
int UiNavigationChecks(QMainWindow &window, MainWindowUI &ui, QComboBox * &installed, QPushButton * &startBtn, QStackedWidget * &gameStack, QTreeWidget * &versionTree, QLineEdit * &installNameEdit, QPushButton * &installBtn, QPushButton * &installBackBtn, QTreeWidget * &loaderTree, QListWidget * &folderList, QTreeWidget * &instanceList, QLabel * &instanceFolderLabel, QTabWidget * &tabs) {

    if (tabs != nullptr) {
        check(tabs->objectName() == MainWindowUI::tabWidgetName(), "选项卡的 objectName 是约定的那个",
              tabs->objectName());
        check(tabs->count() == 6, "六个选项卡（五个上导航 + 导航轨底部的工具箱）", QString::number(tabs->count()));
        const QStringList titles = {tabs->tabText(0), tabs->tabText(1), tabs->tabText(2),
                                    tabs->tabText(3), tabs->tabText(4)};
        check(titles == QStringList({QStringLiteral("主页"), QStringLiteral("管理"),
                                     QStringLiteral("下载"), QStringLiteral("设置"),
                                     QStringLiteral("智能体")}) &&
                  tabs->tabText(MainWindowUI::toolboxTabIndex()) == QStringLiteral("工具箱"),
              "选项卡依次是 主页/管理/下载/设置/智能体，第六个是工具箱", titles.join(QLatin1Char('/')));
        check(tabs->currentIndex() == MainWindowUI::homeTabIndex(),
              "打开就停在主页（即『启动界面』）", QString::number(tabs->currentIndex()));
        check(ui.isHomeTabCurrent(), "isHomeTabCurrent() 与停在主页一致");

        // 那四个下标常量**逐个对着标签文字核一遍**（包括 instances 那一个）：
        // 它们是"界面第几页"这件事在代码里的唯一说法（`openInstanceSettings` 就靠
        // `instancesTabIndex()` 钉住"实例设置这一页归谁"，钉错了的症状是"点来点去点多了
        // 进不去实例设置、还会被拽到下载页" —— 用户 报的那个）。
        // 写死成 0/1/2/3 是不行的：那样改一次标签顺序两边一起错，等于没钉。
        const auto tabIndexNamed = [tabs](const QString &text) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == text) {
                    return i;
                }
            }
            return -1;
        };
        check(MainWindowUI::homeTabIndex() == tabIndexNamed(QStringLiteral("主页")) &&
                  MainWindowUI::instancesTabIndex() == tabIndexNamed(QStringLiteral("管理")) &&
                  MainWindowUI::downloadTabIndex() == tabIndexNamed(QStringLiteral("下载")) &&
                  MainWindowUI::settingsTabIndex() == tabIndexNamed(QStringLiteral("设置")) &&
                  MainWindowUI::agentTabIndex() == tabIndexNamed(QStringLiteral("智能体")),
              "五个「第几页」的常量都对着标签文字核过了（首页/管理/下载/设置/智能体）",
              QStringLiteral("常量 %1/%2/%3/%4/%5 ｜ 实际 %6/%7/%8/%9/%10")
                  .arg(MainWindowUI::homeTabIndex())
                  .arg(MainWindowUI::instancesTabIndex())
                  .arg(MainWindowUI::downloadTabIndex())
                  .arg(MainWindowUI::settingsTabIndex())
                  .arg(MainWindowUI::agentTabIndex())
                  .arg(tabIndexNamed(QStringLiteral("主页")))
                  .arg(tabIndexNamed(QStringLiteral("管理")))
                  .arg(tabIndexNamed(QStringLiteral("下载")))
                  .arg(tabIndexNamed(QStringLiteral("设置")))
                  .arg(tabIndexNamed(QStringLiteral("智能体"))));

        // 每一页都按 objectName 找得到：底栏那条"当前在不在启动界面"的规则、
        // 以及 mw_check 定位控件，都只认名字不认下标以外的显示内容。
        const auto pageByName = [tabs](const QString &name) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->widget(i)->objectName() == name) {
                    return tabs->widget(i);
                }
            }
            return static_cast<QWidget *>(nullptr);
        };
        QWidget *homePage = pageByName(MainWindowUI::homeTabName());
        QWidget *instancePage = pageByName(MainWindowUI::instanceTabName());
        QWidget *downloadPage = pageByName(MainWindowUI::downloadTabName());
        QWidget *settingsPage = pageByName(MainWindowUI::settingsTabName());
        QWidget *agentPage = pageByName(MainWindowUI::agentTabName());
        check(homePage != nullptr && instancePage != nullptr && downloadPage != nullptr &&
                  settingsPage != nullptr && agentPage != nullptr,
              "五页都能按 objectName 找回来");

        // 两个下拉框分家：一个在主页（启动用），一个在下载页（下载用）。
        // 它们本来就是两个互不相干的数据源，只是都叫"版本"，不能混成一个。
        const auto isAncestor = [](QWidget *ancestor, QWidget *child) {
            return isDescendantOf(child, ancestor);
        };
        check(homePage != nullptr && isAncestor(homePage, installed),
              "「已安装版本」下拉框在主页");
        check(downloadPage != nullptr && isAncestor(downloadPage, gameStack) &&
                  isAncestor(downloadPage, versionTree) && isAncestor(downloadPage, installNameEdit) &&
                  isAncestor(downloadPage, installBtn),
              "版本树和安装页那一套控件都在下载页");
        // 下载页左栏那棵「模组加载器」树**是树**：五个顶层条目 +
        // 点开某一条才出现的版本号，用户要的"和上一页的版本选择相同的模式"）。
        {
            const QStringList texts = MainWindowUI::modLoaderOptionTexts();
            // 这张表是"给六条起名"的前提：多一条少一条，树上就会有一条既没名字也对不上字
            // （而那种错不会报错，只会装错加载器）。
            check(texts.size() == 6,
                  "那张表是 6 条（五个加载器 + 一个「不装（纯原版）」）",
                  QString("texts=%1").arg(texts.size()));
            check(texts.mid(0, 4) == QStringList({QStringLiteral("Fabric"), QStringLiteral("Forge"),
                                                  QStringLiteral("NeoForge"), QStringLiteral("OptiFine")}) &&
                      texts.at(4) == QStringLiteral("Quilt"),
                  "现有四种加载器顺序不变，Quilt 追加在第 5 格");
            check(QSet<QString>(texts.begin(), texts.end()).size() == texts.size(),
                  "六段文案互不相同");
            check(loaderTree != nullptr &&
                      loaderTree->objectName() == MainWindowUI::loaderTreeName(),
                  "加载器树找得回来（按 objectName）",
                  loaderTree != nullptr ? loaderTree->objectName() : QStringLiteral("<null>"));
            check(loaderTree != nullptr && downloadPage != nullptr &&
                      isAncestor(downloadPage, loaderTree),
                  "这棵树在下载页");
            // 五条顶层条目：每条写的字、带着的下标（UserRole）都对得上那张表。
            // 下标就是 VersionManager::LoaderKind 的顺序 + 末尾那条「不装」——
            // 下标和枚举之间只有一处翻译（loaderKindFromIndex），这里把它排整齐就少一个
            // "点了 A 装成 B"的机会。
            // （P1）：树顶多了第六行「预设」—— 它不是加载器，身份是负数的
            // `installPresetRowRole()`，**插在「不装」前面**（于是「不装」的树上是 5、
            // 表里的下标仍旧是 4）。下面按"表里的下标 → 树上第几行"翻：
            // none 之前一对一，none 及其后整体后移一位。
            const auto treeRowForLoader = [](int loaderIndex) {
                return loaderIndex < MainWindowUI::modLoaderNoneIndex() ? loaderIndex
                                                                        : loaderIndex + 1;
            };
            check(loaderTree != nullptr && loaderTree->topLevelItemCount() == texts.size() + 1,
                  "树上 = 五条加载器 + 一条「预设」行",
                  loaderTree != nullptr ? QString::number(loaderTree->topLevelItemCount())
                                        : QStringLiteral("<null>"));
            for (int i = 0; i < texts.size() && loaderTree != nullptr &&
                            i < loaderTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *item = loaderTree->topLevelItem(treeRowForLoader(i));
                if (item == nullptr) {
                    check(false, QString("第 %1 条找得到").arg(i));
                    continue;
                }
                check(item->text(MainWindowUI::loaderTreeKeyColumn()) == texts.at(i),
                      QString("第 %1 条上写的是那张表里的字").arg(i),
                      item->text(MainWindowUI::loaderTreeKeyColumn()));
                check(item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt() == i,
                      QString("第 %1 条的 UserRole 就是它自己的下标").arg(i),
                      item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toString());
                // 每条前面挂的是**自己那个标志**（草方块不算 —— 那是"没给图标"时代理的
                // 兜底，给错了会让人以为这条是原版）。
                check(!item->icon(MainWindowUI::loaderTreeKeyColumn()).isNull(),
                      QString("第 %1 条前面挂着标志（%2）").arg(i).arg(texts.value(i)));
            }
            // 第 5 条是「不装」：五个加载器占 0..4 是有讲究的（见上）。树上它是第 6 行
            // （「预设」那一行插在它前面）。
            check(loaderTree != nullptr && loaderTree->topLevelItemCount() == 7 &&
                      loaderTree->topLevelItem(6)->text(MainWindowUI::loaderTreeKeyColumn()) ==
                          QStringLiteral("不装（纯原版）") &&
                      MainWindowUI::modLoaderNoneIndex() == 5,
                  "第 5 条是「不装（纯原版）」（0..4 留给五个加载器；树下标的映射没动，"
                  "只是树上多了一行预设）",
                  loaderTree != nullptr && loaderTree->topLevelItemCount() == 7
                      ? loaderTree->topLevelItem(6)->text(MainWindowUI::loaderTreeKeyColumn())
                      : QStringLiteral("<缺>"));
            // 那条「预设」行：字 / 身份 / 小字（「未选择」）三样。
            {
                QTreeWidgetItem *presetRow = loaderTree != nullptr
                                                 ? loaderTree->topLevelItem(
                                                       MainWindowUI::modLoaderNoneIndex())
                                                 : nullptr;
                check(presetRow != nullptr &&
                          presetRow->text(MainWindowUI::loaderTreeKeyColumn()) ==
                              MainWindowUI::installPresetRowText() &&
                          presetRow->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole)
                                  .toInt() == MainWindowUI::installPresetRowRole() &&
                          presetRow->data(MainWindowUI::loaderTreeKeyColumn(),
                                          MainWindowUI::rowSubTextRole())
                                  .toString() == MainWindowUI::installPresetSummaryText(0) &&
                          !presetRow->icon(MainWindowUI::loaderTreeKeyColumn()).isNull(),
                      "★ 「预设」那一行在「不装」前面：写「预设」、身份是负数、小字「未选择」、"
                      "前面挂着拼图（P1）",
                      presetRow != nullptr
                          ? presetRow->text(MainWindowUI::loaderTreeKeyColumn()) +
                                QStringLiteral(" / ") +
                                presetRow->data(MainWindowUI::loaderTreeKeyColumn(),
                                                MainWindowUI::rowSubTextRole())
                                    .toString()
                          : QStringLiteral("<缺>"));
            }
            // 搭出来的这一刻五条全收着：底下那条版本号是点开才出现的（要求
            // "点击以后展开版本"），一上来就摊开会把安装按钮挤下去。
            bool anyExpanded = false;
            for (int i = 0; loaderTree != nullptr && i < loaderTree->topLevelItemCount(); ++i) {
                anyExpanded = anyExpanded || loaderTree->topLevelItem(i)->isExpanded();
            }
            check(!anyExpanded, "刚搭好时五条都是收着的");
            // 竖的那几条：和版本树**一模一样**——表头藏着、行高常数、左边那个小三角不画、
            // 同一个代理、同一个字号。用户要的就是"和上一页相同的模式"，两边长得不一样的话
            // 那句话就只兑现了一半。
            check(loaderTree != nullptr && loaderTree->isHeaderHidden() &&
                      loaderTree->uniformRowHeights() && !loaderTree->rootIsDecorated() &&
                      !loaderTree->expandsOnDoubleClick() &&
                      loaderTree->columnCount() == 1,
                  "这棵树和版本树同一套竖的：藏表头/行高常数/不画左边小三角/双击不展开/一列");
            check(loaderTree != nullptr && loaderTree->itemDelegate() != nullptr,
                  "这棵树上挂着代理（图标 + 第二行小字 + 右边那个箭头都靠它画）");
            // 两棵树共用同一个代理，只有**一处**不一样：叶子那一层右边画不画那个 `>`。
            // 版本树上那个 `>` 的意思是"点进去"（点一个版本号进安装页）；这一棵树的
            // 叶子（两档 + 具体版本号）点一下是**选中它**、底下没有下一页，所以不画
            // （要求："模组加载器的具体版本右侧就不用 > 了（毕竟这个层次
            // 没有下拉框了）"）。**这一位是构造参数，产品那边传错了不报错、也不变灰、
            // 点了也照常工作** —— 只有在屏幕上看出来，"传没传对"就得在这儿钉住。
            // 光验"传 false 就不画"是不够的（探针自己也能过），所以分成两条：这条钉
            // 产品那根线，下面 C 段那条钉"不画"这件事本身。
            {
                const auto *loaderDelegate =
                    loaderTree != nullptr
                        ? qobject_cast<const VersionTreeDelegate *>(loaderTree->itemDelegate())
                        : nullptr;
                const auto *versionDelegate =
                    versionTree != nullptr
                        ? qobject_cast<const VersionTreeDelegate *>(versionTree->itemDelegate())
                        : nullptr;
                check(loaderDelegate != nullptr && !loaderDelegate->leafArrow(),
                      "★ 加载器那棵树传的是「这一层叶子不画行尾箭头」（那一层没有下拉框）",
                      QStringLiteral("代理=%1 leafArrow=%2")
                          .arg(loaderDelegate != nullptr ? QStringLiteral("有")
                                                         : QStringLiteral("没有"))
                          .arg(loaderDelegate != nullptr && loaderDelegate->leafArrow() ? 1 : 0));
                check(versionDelegate != nullptr && versionDelegate->leafArrow(),
                      "★ 版本树那一棵照旧画（版本号右边那个 > 是「点进去」）",
                      QStringLiteral("代理=%1 leafArrow=%2")
                          .arg(versionDelegate != nullptr ? QStringLiteral("有")
                                                          : QStringLiteral("没有"))
                          .arg(versionDelegate != nullptr && versionDelegate->leafArrow() ? 1 : 0));
            }
            check(loaderTree != nullptr && versionTree != nullptr &&
                      loaderTree->font().pointSize() == versionTree->font().pointSize(),
                  "字号和版本树一样（同一档 versionTreeFontStep）",
                  loaderTree != nullptr && versionTree != nullptr
                      ? QString("%1 vs %2")
                            .arg(loaderTree->font().pointSize())
                            .arg(versionTree->font().pointSize())
                      : QStringLiteral("<null>"));
            check(loaderTree != nullptr && versionTree != nullptr &&
                      loaderTree->iconSize() == versionTree->iconSize(),
                  "标志尺寸和版本树一样（versionTreeIconSize）",
                  loaderTree != nullptr && versionTree != nullptr
                      ? QString("%1x%2 vs %3x%4")
                            .arg(loaderTree->iconSize().width())
                            .arg(loaderTree->iconSize().height())
                            .arg(versionTree->iconSize().width())
                            .arg(versionTree->iconSize().height())
                      : QStringLiteral("<null>"));
        }
        check(homePage != nullptr && !isAncestor(downloadPage, installed) &&
                  isAncestor(homePage, startBtn),
              "「启动游戏」和已安装版本下拉框同处主页");
        check(downloadPage != nullptr && isAncestor(downloadPage, installBackBtn),
              "「← 返回版本列表」跟着安装页在下载页");
        check(downloadPage != nullptr && !isAncestor(homePage, installBtn),
              "「安装」不在主页（它只在下载页的安装页上）");
        check(instancePage != nullptr && isAncestor(instancePage, folderList) &&
                  isAncestor(instancePage, instanceList) &&
                  instancePage->findChildren<QPushButton *>().size() == 13,
              // UI 7b 从 11 变 14：页头那颗文件夹胶囊（instanceFolderPill，点开是文件夹浮层）
              // + 空状态那两颗（「去下载页装新版本」「导入整合包…」）。
              // 14 又变 13：空状态里的「导入整合包…」去掉（页头右上已有同一颗），只剩「去下载页装新版本」。
              // 十一个 = 下面那三颗 + 搜索行那四颗筛选 chip + 模式行那三颗
              // （客户端实例 / 服务端 / 预设）+ 右下那颗「新建服务端…」（M4）。
              // 模式行 从两颗变三颗：预设管理从「设置」里搬了过来，这一颗是三颗里的第三颗。
              // 三个 = 左栏「游戏文件夹」那一组的两个（「添加文件夹…」+「导入整合包…」）
              // + 「分组设置…」那一颗（它在「实例列表」那个框外面的右上角）。
              // 这个数字是**故意**钉死的：往这一页加控件时，要么它是这里数得着的一个，
              // 要么就该想清楚为什么加（多一个少一个都要在这里说一声）。
              // 从 6 变 5：「停止游戏…」删了（要求「实例里停止游戏按钮删了」）。
              // 同一天傍晚 5 变 3：右栏那排「重命名… / 实例设置…」两颗跟着整排删了，各自
              // 搬进了「实例设置」页和"点这一条实例行"（要求「删掉实例页面右侧三个
              // 按钮（因为都已经分别有入口了），顺便把下面的文本干掉」）—— 「分组设置…」
              // 留下来挪到框外右上角（追加答复「分组设置先放右上角框外不删了」）。
              // 从 3 变 7：正文面上方那一行**搜索与筛选** ——
              // 一个搜索框（QLineEdit，不是按钮）+ 四颗筛选 chip（是按钮）。四颗为什么是
              // 按钮：它们要"亮着/不亮"这个状态（和资源详情页那排游戏版本、文件页那四颗
              // 筛子同一档控件），拿 QCheckBox 来摆是另一套观感。四颗的名字 / 文字 / 互斥
              // 在下面 U32 那一段，这一条数的就是"这一页上到底有几颗 QPushButton"。
              // 从 7 又到 10：实例页顶部多了**模式切换**（客户端实例 | 服务端）
              // 与服务端那半页（M4）。三颗新的：模式行那两颗 chip
              // （要"亮着/不亮"这个状态，理由同上面那四颗筛选 chip）+「新建服务端…」
              // （用户答"服务端根目录 = 一个自己的 Servers/ 文件夹"之后，服务端那半页的
              // 建入口）。
              // 实例行行尾那三颗（齿轮/上箭头/垃圾桶）**不是按钮**：它们是代理画出来的格子，
              // 数不进来（它们的命中在 mw_check 里量）。
              "实例页是「左文件夹 + 右实例」两张列表 + 模式行三颗 + 三颗页面按钮 + 四颗筛选 chip",
              QString::number(instancePage != nullptr
                                  ? instancePage->findChildren<QPushButton *>().size()
                                  : -1));
        // 「停止游戏…」删干净了没有 —— 按 objectName **反着找**（照上面「移除文件夹…」
        // 那两条的写法）。产品那头 stopGameButtonName() 已经删掉了，所以判据里只能写
        // 字面量，而这条要钉的正是"它找不回来了"。
        //
        // NOTE: 停游戏这件事**没有**跟着没掉，只是入口只剩底栏左面板里那一行的电源
        // （那一行的行为在 mw_check 里量）。这条只钉界面：这一页上不再有那一颗。
        check(instancePage != nullptr &&
                  instancePage->findChild<QPushButton *>(
                      QStringLiteral("stopGameButton")) == nullptr,
              "「停止游戏…」那个按钮真的不在了（按 objectName 找不到）");
        // 右栏那一排按钮只剩**一颗**（「分组设置…」），另外两颗各自的新家见下面两条
        // "反着找"。
        //
        // NOTE: 这一颗**不读**选中的实例，所以没有"没选中"这回事，也就不用变灰；
        // 它照旧一律可点。
        {
            const auto *groupBtn = instancePage->findChild<QPushButton *>(
                MainWindowUI::groupInstanceButtonName());
            check(groupBtn != nullptr, "实例页能按 objectName 找回「分组设置…」");
            check(groupBtn != nullptr && groupBtn->isEnabled() &&
                      groupBtn->toolTip().isEmpty(),
                  "「分组设置…」是能点的，而且不挂着「还没做」那种提示",
                  groupBtn != nullptr ? groupBtn->toolTip() : QStringLiteral("<null>"));
            check(groupBtn != nullptr &&
                      groupBtn->text() == MainWindowUI::groupInstanceText(),
                  "按钮上写的是约定的那句话",
                  groupBtn != nullptr ? groupBtn->text() : QString());
            // 它在「实例列表」那个**框外面**（要求「分组设置先放右上角框外不删了」）。
            // 这里只钉"它的父控件不是那个框"这一半（框外）；"在右上角、在框上面"那一半
            // 是几何，在 Z 节里量。
            check(groupBtn != nullptr && groupBtn->parentWidget() != nullptr &&
                      qobject_cast<QGroupBox *>(groupBtn->parentWidget()) == nullptr,
                  "「分组设置…」不在任何一个 group box 里面（框外右上角那一颗）",
                  groupBtn != nullptr && groupBtn->parentWidget() != nullptr
                      ? groupBtn->parentWidget()->objectName()
                      : QStringLiteral("<null>"));
            // 两颗**删干净了没有** —— 按 objectName **反着找**（照「停止游戏…」那条的
            // 写法）。产品那头这两个名字常量已经删掉了，所以判据里只能写字面量，而这条
            // 要钉的正是"它们找不回来了"：这两个功能**没有**跟着没掉，各自搬去了别处
            // （「实例设置」= 点这一条实例行 / 行尾那颗齿轮；「重命名…」= 实例设置页里）。
            check(instancePage->findChild<QPushButton *>(
                      QStringLiteral("renameInstanceButton")) == nullptr,
                  "「重命名…」那颗按钮真的不在了（按 objectName 找不到）");
            check(instancePage->findChild<QPushButton *>(
                      QStringLiteral("instanceSettingsButton")) == nullptr,
                  "「实例设置…」那颗按钮真的不在了（按 objectName 找不到）");

            // 「导入整合包…」 从右栏挪到左栏「游戏文件夹」那一组、紧挨在
            // 「添加文件夹…」**下面**（要求：「导入整合包放在左边添加文件夹下面」）。
            // 几何（"在它下面、同一栏、一边宽"）在 Z 节量，这里只量身份 / 文案 / 可点 /
            // 宽度策略。
            //
            // NOTE: 它和右栏那三个不是一回事：那三个是"对选中的实例做什么"（没选中会弹一句
            // "先选中一个实例"），这个是**造**一个新实例 —— 所以它归左栏那一组，
            // 也不跟着右栏那一排的忙态规则走。
            QPushButton *importBtn =
                instancePage->findChild<QPushButton *>(MainWindowUI::importModpackButtonName());
            check(importBtn != nullptr, "实例页能按 objectName 找回「导入整合包…」");
            check(importBtn != nullptr && importBtn->text() == MainWindowUI::importModpackText(),
                  "按钮上写的是约定的那句话",
                  importBtn != nullptr ? importBtn->text() : QStringLiteral("<null>"));
            check(importBtn != nullptr && importBtn->isEnabled() && !importBtn->toolTip().isEmpty(),
                  "导入按钮可用，格式说明按需显示",
                  importBtn != nullptr ? importBtn->toolTip() : QStringLiteral("<null>"));
            // 宽度策略**不再是 Fixed**（在右栏时怕把列表挤窄）—— 挪到左栏之后它和
            // 「添加文件夹…」一样顶满这一栏，那两颗才看着是一组。这条钉的就是"别再有人
            // 把它改回 Fixed"（改回去在界面上是一颗短按钮，不报错，只有这条能看出来）。
            check(importBtn != nullptr &&
                      importBtn->sizePolicy().horizontalPolicy() != QSizePolicy::Fixed,
                  "宽度策略不是 Fixed —— 它顶满左栏（和「添加文件夹…」同宽）",
                  importBtn != nullptr
                      ? QString::number(int(importBtn->sizePolicy().horizontalPolicy()))
                      : QStringLiteral("<null>"));
            check(importBtn != nullptr &&
                      isAncestor(instancePage->findChild<QWidget *>(
                                     MainWindowUI::rightColumnName(QStringLiteral("instance"))),
                                 importBtn),
                  "在右栏页头（UI 7b 设计稿：和「分组设置…」同一排右上角）");
        }

        check(folderList != nullptr &&
                  folderList->objectName() == MainWindowUI::folderListName() &&
                  instanceList != nullptr &&
                  instanceList->objectName() == MainWindowUI::instanceListName() &&
                  instanceFolderLabel != nullptr &&
                  instanceFolderLabel->objectName() == MainWindowUI::instanceFolderLabelName(),
              "文件夹列表 / 实例列表 / 当前文件夹那句话的 objectName 都是约定的那个");
        // 左边装文件夹、右边装实例：两张列表不能同栏（同栏就退化成一次只看得见一张）。
        check(instancePage != nullptr && folderList != nullptr && instanceList != nullptr &&
                  isAncestor(instancePage->findChild<QWidget *>(MainWindowUI::instanceFolderPopupName()),
                             folderList) &&
                  isAncestor(instancePage->findChild<QWidget *>(
                                 MainWindowUI::rightColumnName(QStringLiteral("instance"))),
                             instanceList),
              "文件夹列表在页头胶囊点开的浮层里、实例列表在右栏（UI 7b）");
        // 账号那一整块（正版登录 / 加离线账号 / 切换 / 删除 / 皮肤披风）只在
        // 「账号与外观」那个窗里做，设置页这格是它的入口 —— 和主页右栏那个同一个窗、
        // 同一句文案（accountSkinButtonText()）。
        // 设置页不再有「账户」一节：这颗按钮只是隐藏的兼容入口，画不出来，也没有「账户」小节标题。
        {
            bool accountGroup = false;
            if (settingsPage != nullptr)
                for (QWidget *w : settingsPage->findChildren<QWidget *>())
                    if (w->property("settingsSectionTitle").toString() == QStringLiteral("账户")) accountGroup = true;
            check(ui.getAccountSkinSettingsButton() != nullptr &&
                      !ui.getAccountSkinSettingsButton()->isVisibleTo(settingsPage) && !accountGroup,
                  "设置页里没有「账户」一节，账号入口只在主页 / 导航轨浮层（设置里只剩隐藏的兼容入口）");
        }
        // 反面：那格**不再**摆「微软登录」按钮（登录过叫「重新登录」那种）—— 它只会登录
        // 一件事，加离线账号/切换/删除一件都做不到，这些并进那个窗里了。
        check(window.findChild<QWidget *>(QStringLiteral("loginButton")) == nullptr,
              "设置页那格原来的「微软登录」按钮已经拿掉了（它做不了账号管理那几件事）");

        // 那对「可安装版本 下拉框 + 版本下载 列表」**不许**回来（要求
        // "点击具体版本后进入一个新页面"）。下载页里**只剩**那四个加载器
        // 版本组合框（它们排在安装页每条加载器右边），一个多余的都别再摆回来。
        // 控件只能按名字找 —— 名字就是契约。
        check(window.findChild<QComboBox *>(QStringLiteral("availableVersionCombo")) == nullptr &&
                  window.findChild<QListWidget *>(QStringLiteral("availableVersionList")) == nullptr &&
                  window.findChild<QPushButton *>(QStringLiteral("downloadButton")) == nullptr,
              "旧的「可安装版本 下拉框 / 版本下载 列表 / 下载版本 按钮」都没了");
    }
 return 0;
}


