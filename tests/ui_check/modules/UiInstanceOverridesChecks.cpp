#include "Support.hpp"
#include "InstanceSettingsSignature.hpp"
#include "InstanceSettingsPageChoices.hpp"
int UiInstanceOverridesChecks() {

    // ------------------------------------------------------------------
    // 这个对话框自己那一半：把控件读成一份 Override / QStringList。产品的另一半
    // （写进 wtfml.meta、失败弹窗）在 MainWindow 里，mw_check 的 U 段验它不渗进全局，
    // 而"这份值合起来会变成什么"由 verify 的 R 段钉住（resolveJavaSettings）。
    //
    // 驱动方式就是它和其它对话框的那条约定：**控件的值就是结果**，没有内部影子状态，
    // 所以这里摆完控件直接读 override()，不用走 exec()（模态窗在 offscreen 里驱不动）。
    {
        using Dlg = InstanceSettingsDialog;
        // 「实例名」那一格的初值取的是**目录名**（构造的第二个实参），不是标题上那个显示名。
        // 两个字符串在这里就分开摆着（刻意**不一样**）—— U1c 那条断言量的正是"取的是哪一个"：
        // 拿显示名当初值的话，用户进这一页什么都没改、按一下确定，实例就被改成了一个奇怪的
        // 名字（见 InstanceSettingsDialog.hpp 里 instanceId 那段 NOTE）。
        const QString instanceDisplayName = QStringLiteral("我的整合包");
        const QString instanceDirName = QStringLiteral("我的整合包-1.20.1-Forge");
        const Dlg::GlobalValues global = [] {
            Dlg::GlobalValues g;
            g.javaPath = QStringLiteral("C:/全局/java.exe");
            g.xmx = 4096;
            g.xms = 1024;
            g.autoMemory = false;
            g.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
            return g;
        }();
        // 全局是**手动档**（autoMemory=false）：下面"自动档下两行禁用"那几条要自己
        // 把那一行切成自定义才看得到，否则验的是"跟随全局跟着全局禁用"，两件事混一起。
        // 「跟随全局 / 自定义」那一格是**一个** QComboBox，下标 0 / 1。
        //
        // 下面这三个辅助就是那一格的读 / 写两半 —— 档位只有一个出处
        // （`choiceFollowIndex()` / `choiceCustomIndex()`），这里一个 `0` / `1` 都不写。
        const auto choiceCombo = [](const Dlg &d, const QString &field) {
            return d.findChild<QComboBox *>(Dlg::choiceComboName(field));
        };
        // 把这一项的档位切到自定义 / 跟随全局：下拉框的 `currentIndexChanged` **是接在写盘那条线上的**（见 .hpp 里那段），所以这一下
        // 等价于"用户真的改了一次档位"—— 那正是这些调用点想要的意思（它们紧接着就读
        // `override()`），别用 QSignalBlocker 盖掉。
        const auto setChoice = [&choiceCombo](const Dlg &d, const QString &field, bool custom) {
            if (auto *combo = choiceCombo(d, field)) {
                combo->setCurrentIndex(custom ? Dlg::choiceCustomIndex()
                                              : Dlg::choiceFollowIndex());
            }
        };
        const auto isCustom = [&choiceCombo](const Dlg &d, const QString &field) {
            const auto *combo = choiceCombo(d, field);
            return combo != nullptr && combo->currentIndex() == Dlg::choiceCustomIndex();
        };
        const auto isFollow = [&choiceCombo](const Dlg &d, const QString &field) {
            const auto *combo = choiceCombo(d, field);
            return combo != nullptr && combo->currentIndex() == Dlg::choiceFollowIndex();
        };
        // 那一格下拉框里第 @p index 项的文字。
        const auto choiceText = [&choiceCombo](const Dlg &d, const QString &field, int index) {
            const auto *combo = choiceCombo(d, field);
            return combo != nullptr ? combo->itemText(index) : QString();
        };
        const auto hintOf = [](const Dlg &d, const QString &field) {
            const auto *label = d.findChild<QLabel *>(Dlg::followHintName(field));
            return label != nullptr ? label->text() : QString();
        };
        const auto lineEditOf = [](const Dlg &d, const QString &name) {
            return d.findChild<QLineEdit *>(name);
        };
        // ---- U0 左栏（只有一列：按实例现算出来的页表）----
        //
        // 要求：「针对每个实例，在实例设置里个性化自定义，概览里面调整是否显示，把整个
        // 文件管理换了」。这一段钉的是**骨架**：一列页表 + 右边一层堆叠页、两张键表按下标
        // 严丝合缝、点第 N 条换第 N 页、没有入口的那一页（配置文件）照样摆得出来而左栏不亮、
        // 行号越界不清空右栏、以及"哪些控件落在哪一页里"。
        //
        // 那版的**三栏**（大类 | 中栏小类 | 内容）已经拆掉了：中栏那个控件在
        // 这一整页上一个都找不到（见下面那句硬编码的旧 objectName）。左栏从此是唯一的
        // 一列，而它的内容是按实例**算**出来的 —— 所以这里的键表也不再是写死的那三档。
        {
            const auto pageListOf = [](const Dlg &d) {
                return d.findChild<QListWidget *>(Dlg::pageListName());
            };
            const auto stackOf = [](const Dlg &d) {
                return d.findChild<QStackedWidget *>(Dlg::stackName());
            };
            // 页是**第一次切过去才建**的（见 InstanceSettingsDialog::ensurePage()）：表单页外层
            // 套 `scrollName()`，列表/树/编辑器/日志页直接铺在 slot 里避免三层嵌套。认页一律按
            // `panelName()` 找。
            const auto pageOf = [](const Dlg &d, const QString &key) -> QWidget * {
                return d.findChild<QWidget *>(Dlg::panelName(key));
            };
            // 右栏现在露着的那一页的 objectName（键 → 页，`currentPageKey()` 是唯一出处）。
            const auto currentPanel = [&pageOf](const Dlg &d) {
                QWidget *page = pageOf(d, d.currentPageKey());
                return page != nullptr ? page->objectName() : QString();
            };
            // 左栏那条导航是**树**：认人靠 `UserRole` 上那个键（`navItemOf()`），没有行号这一层 ——
            // 也就没有"差一格点错页"这种错法了。

            // 左栏那条导航是**一棵树**（`navigationTreeName()` —— 界面
            // 导航的唯一出处）。`pageListOf` 只指得到一列**旧的、`hide()` 着的**
            // `QListWidget`：它的内容还是四组十三行、`shownEntryKeys()` 改了它
            // 也不动。所以凡是"量左栏"的判据一律认下面这几个。
            //
            // 树的形状（`rebuildNavigationTree()`）：顶上两条光叶子（概览 / 启动），底下
            // 四组（内容管理 / 世界与联机 / 创作工具 / 维护），组里的叶子才是页。组的
            // `UserRole` 是**空的**、`UserRole + 1` 才是组号 —— 判据一并分开认这两格，
            // "组也被当成一页"这种错法（点一下组名右栏空掉）就当场红。
            const auto navTreeOf = [](const Dlg &d) {
                return d.findChild<QTreeWidget *>(Dlg::navigationTreeName());
            };
            // 左栏此刻**亮着**的那一条的键（亮的是组 / 什么都没亮 → 空）。
            const auto navKeyOf = [&navTreeOf](const Dlg &d) {
                QTreeWidget *tree = navTreeOf(d);
                QTreeWidgetItem *item = tree == nullptr ? nullptr : tree->currentItem();
                return item == nullptr ? QString() : item->data(0, Qt::UserRole).toString();
            };
            const auto navItemOf = [](QTreeWidget *tree, const QString &key) -> QTreeWidgetItem * {
                if (tree == nullptr || key.isEmpty()) {
                    return nullptr;
                }
                for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                    if ((*it)->data(0, Qt::UserRole).toString() == key) {
                        return *it;
                    }
                }
                return nullptr;
            };
            // 树里**能点的那些键**（叶子），按深度优先的次序 —— 组不算（它的键是空的）。
            const auto navLeafKeys = [](QTreeWidget *tree) {
                QStringList keys;
                if (tree == nullptr) {
                    return keys;
                }
                for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                    const QString key = (*it)->data(0, Qt::UserRole).toString();
                    if (!key.isEmpty()) {
                        keys.append(key);
                    }
                }
                return keys;
            };

            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);

            check(navTreeOf(dialog) != nullptr && stackOf(dialog) != nullptr &&
                      pageListOf(dialog) != nullptr,
                  "U0a 左栏那棵树、右栏那层堆叠页都按 objectName 找得回来",
                  QStringLiteral("树 %1 / 堆叠页 %2 / 旧那一列 %3")
                      .arg(navTreeOf(dialog) != nullptr ? QStringLiteral("在")
                                                        : QStringLiteral("<没有>"),
                           stackOf(dialog) != nullptr ? QStringLiteral("在")
                                                      : QStringLiteral("<没有>"),
                           pageListOf(dialog) != nullptr ? QStringLiteral("在")
                                                         : QStringLiteral("<没有>")));
            QPushButton *fixedLaunch =
                dialog.findChild<QPushButton *>(Dlg::launchInstanceButtonName());
            int launchRequests = 0;
            QObject::connect(&dialog, &Dlg::launchInstanceRequested,
                             [&launchRequests]() { ++launchRequests; });
            if (fixedLaunch != nullptr) {
                fixedLaunch->click();
            }
            check(fixedLaunch != nullptr &&
                      fixedLaunch->text() == Dlg::launchInstanceText() &&
                      fixedLaunch->property("wtfmlButtonKind").toString() ==
                          QStringLiteral("primary") &&
                      launchRequests == 1,
                  "★★ U0a 左栏底部有固定的主动作“启动实例”，任意页都走同一条启动请求",
                  fixedLaunch != nullptr ? fixedLaunch->text() : QStringLiteral("<没有按钮>"));

            // 「中栏」那个控件**没了**。名字是**硬编码**的：`subCategoryListName()` /
            // `categoryListName()` 那两个常量已经连声明一起删了 —— 而这一条要的正是
            // "拿旧名字也找不回来"（留着常量就写得出这一句，但那样测的是常量还在不在，
            // 不是控件还在不在）。
            check(dialog.findChild<QListWidget *>(QStringLiteral("instanceSettingsSubCategoryList")) ==
                          nullptr &&
                      dialog.findChild<QListWidget *>(QStringLiteral("instanceSettingsCategoryList")) ==
                          nullptr,
                  "★ U0a 两条导航列（大类 / 小类）在这一页上**一个都找不到"
                  "了**（左栏只剩一列）");

            // 左栏此刻有哪几条 = pinnedPageKeys() + optionalPageKeys()：这是"七条全可用、
            // 一条都没被勾掉"那个默认样子（这一份 Dlg 没被 MainWindow 收窄过）。
            // 列出来的那几条必须和 shownEntryKeys() **一模一样** —— 那是左栏的唯一出处，
            // 差一条就是"右栏能换到一页、左栏里却没这一条"。
            QStringList defaultKeys = Dlg::pinnedPageKeys();
            defaultKeys.append(Dlg::optionalPageKeys());
            check(Dlg::pinnedPageKeys().size() == Dlg::pinnedPageTexts().size() &&
                      Dlg::optionalPageKeys().size() == Dlg::optionalPageTexts().size() &&
                      !defaultKeys.isEmpty(),
                  "U0a 前提：两张键表各自与各自的文字表一样长");
            check(dialog.shownEntryKeys() == defaultKeys,
                  "★ U0a 左栏默认那一列 = 概览 / 启动 + 七条可选页（顺序就是 optionalPageKeys()）",
                  dialog.shownEntryKeys().join(QLatin1Char('|')));

            // U0a 左栏那棵树：**顶上两条光叶子 + 四组**。组的 `UserRole` 必须**空着**
            // （它自己不是一页），`UserRole + 1` 才是组号 —— 这两格混了的话"点一下组名
            // 右栏就空掉"这种错法量不出来。
            QTreeWidget *nav = navTreeOf(dialog);
            const QStringList navKeys = navLeafKeys(nav);
            const QStringList groupIds = {QStringLiteral("content"), QStringLiteral("worlds"),
                                          QStringLiteral("creative"),
                                          QStringLiteral("maintenance")};
            int groupHit = 0;
            QStringList groupSeen;
            for (int i = 0; nav != nullptr && i < nav->topLevelItemCount(); ++i) {
                QTreeWidgetItem *top = nav->topLevelItem(i);
                if (top == nullptr || top->data(0, Qt::UserRole + 1).toString().isEmpty()) {
                    continue;
                }
                groupSeen.append(top->data(0, Qt::UserRole + 1).toString());
            }
            for (const QString &id : groupIds) {
                groupHit += groupSeen.contains(id) ? 1 : 0;
            }
            check(nav != nullptr && nav->topLevelItemCount() == 6 &&
                      groupSeen.size() == groupIds.size() && groupHit == groupIds.size() &&
                      nav->topLevelItem(0)->data(0, Qt::UserRole).toString() ==
                          Dlg::overviewCategoryKey() &&
                      nav->topLevelItem(1)->data(0, Qt::UserRole).toString() ==
                          Dlg::launchCategoryKey(),
                  "★★ U0a 左栏那棵树 = 顶上两条光叶子（概览 / 启动）+ 四组（内容管理 / 世界与"
                  "联机 / 创作工具 / 维护）—— 组自己不是一页（它的 UserRole 是空的）",
                  QStringLiteral("顶层 %1 项、组 %2 个")
                      .arg(nav != nullptr ? nav->topLevelItemCount() : -1)
                      .arg(groupSeen.size()));

            // 每一条叶子说的都是 `pageText(key)` 那句话（那一句是左栏条目、勾选框标签、
            // tooltip 的唯一出处 —— 换了名字这里跟着红）。
            QStringList navTextMiss;
            for (const QString &key : navKeys) {
                QTreeWidgetItem *item = navItemOf(nav, key);
                if (item == nullptr || item->text(0) != Dlg::pageText(key)) {
                    navTextMiss.append(key);
                }
            }
            check(!navKeys.isEmpty() && navTextMiss.isEmpty(),
                  "U0a 树上每一条叶子的字都是 pageText() 给的（键与文字对不上就在这里红）",
                  navTextMiss.isEmpty() ? QStringLiteral("（%1 条全对上）").arg(navKeys.size())
                                        : navTextMiss.join(QLatin1Char('|')));

            // Default content pages plus direct creation/maintenance entries.
            // World backup stays under saves; intermediate maintenance menus are not navigation entries.
            const QStringList ownRowKeys = {
                QStringLiteral("screenshots"), QStringLiteral("replay"), QStringLiteral("structure"),
                QStringLiteral("diagnosis"), QStringLiteral("logs"), QStringLiteral("operationsmenu"),
                QStringLiteral("migration"), QStringLiteral("upgrade")};
            QStringList strayKeys;
            for (const QString &key : navKeys) {
                if (!defaultKeys.contains(key) && !Dlg::lifecyclePageKeys().contains(key)) {
                    strayKeys.append(key);
                }
            }
            int defaultRowHit = 0;
            int ownRowHit = 0;
            for (const QString &key : defaultKeys) {
                defaultRowHit += navKeys.contains(key) ? 1 : 0;
            }
            for (const QString &key : ownRowKeys) {
                ownRowHit += navKeys.contains(key) ? 1 : 0;
            }
            QStringList navDedup = navKeys;
            navDedup.removeDuplicates();
            check(strayKeys.isEmpty() && defaultRowHit == defaultKeys.size() &&
                      ownRowHit == ownRowKeys.size() &&
                      navKeys.size() == defaultKeys.size() + ownRowKeys.size() &&
                      navDedup.size() == navKeys.size(),
                  "★ U0a 树里的键不多不少：默认那九条一条不落、直接进入功能的生命周期页"
                  "都在、没有多余的野键（键两两不同 —— 它们要进 objectName）",
                  QStringLiteral("默认 %1/%2、自有行 %3/%4、共 %5 条（去重后 %6）")
                      .arg(defaultRowHit)
                      .arg(defaultKeys.size())
                      .arg(ownRowHit)
                      .arg(ownRowKeys.size())
                      .arg(navKeys.size())
                      .arg(navDedup.size()));
            // 每一条都说得出一句人话（pageText() 是左栏条目、勾选框标签、tooltip 的唯一出处）。
            int textHit = 0;
            for (const QString &key : defaultKeys) {
                textHit += Dlg::pageText(key).isEmpty() ? 0 : 1;
            }
            check(textHit == defaultKeys.size(),
                  "U0a 每一条（含那七页）都有一个非空的名字 —— pageText() 是唯一出处");

            // 两张键表的**关系**：七条可选页 = 文件那八页 - 配置文件。
            //
            // 这一条是这次改动的"形状"本身：「配置文件」是一个**没有入口**的页（它不在
            // 勾选表里、不在左栏里，只有 showPage("config") 够得着，见下面 U0d）。所以
            // "整张文件页表"比"可选页表"多出来的那一条，只能是它。
            QStringList filesMinusConfig = Dlg::filePageKeys();
            filesMinusConfig.removeAll(Dlg::configPageKey());
            QStringList optionalSorted = Dlg::optionalPageKeys();
            optionalSorted.sort();
            filesMinusConfig.sort();
            check(!optionalSorted.isEmpty() && optionalSorted == filesMinusConfig,
                  "★ U0a 七条可选页 = 文件那八页减去「配置文件」（它没有任何入口）",
                  QStringLiteral("可选 %1 条 / 文件页 %2 条")
                      .arg(Dlg::optionalPageKeys().size())
                      .arg(Dlg::filePageKeys().size()));
            // 「快捷方式」不再是一页（那九颗按钮在概览里）。键表少一条 ⇒
            // 少一页； 起又补上了那一整批**生命周期页**（worldbackup … operationsmenu
            // 那 16 条），所以 pageCount() 现在是 26 = 钉死 2 + 文件 8 + 生命周期 16。
            // 下面这一条把三张表的**关系**钉死，"加一页忘了动表"当场红。
            check(Dlg::pageCount() == 26 && Dlg::filePageKeys().size() == 8 &&
                      Dlg::lifecyclePageKeys().size() == 16 &&
                      Dlg::pageCount() == Dlg::pinnedPageKeys().size() +
                                             Dlg::filePageKeys().size() +
                                             Dlg::lifecyclePageKeys().size() &&
                      !Dlg::filePageKeys().contains(QStringLiteral("shortcut")) &&
                      Dlg::filePageFirstIndex() == 2,
                  "★ U0a 「快捷方式」不再是一页、那一批生命周期页各占一页 ⇒ 文件页 8 条、"
                  "生命周期页 16 条、pageCount() == 26",
                  QStringLiteral("pageCount %1 / 文件页 %2 / 生命周期页 %3")
                      .arg(Dlg::pageCount())
                      .arg(Dlg::filePageKeys().size())
                      .arg(Dlg::lifecyclePageKeys().size()));

            // 堆叠页：页数 = pageCount()，而且**顺序就是键表的顺序**（概览、启动，然后那
            // 八页文件页，最后是那十六条生命周期页）。加一页忘了动这张表的话，下面这几条
            // 当场红。
            QStackedWidget *stack = stackOf(dialog);
            QStringList expectedPanels = Dlg::pinnedPageKeys();
            expectedPanels.append(Dlg::filePageKeys());
            expectedPanels.append(Dlg::lifecyclePageKeys());
            check(expectedPanels.size() == Dlg::pageCount(),
                  "U0a 前提：pageCount() = 钉死那两条 + 文件那八页 + 生命周期那十六条");
            check(stack->count() == Dlg::pageCount(),
                  "U0a 堆叠页的页数 == pageCount()",
                  QStringLiteral("页上共 %1 页 / pageCount %2")
                      .arg(stack->count())
                      .arg(Dlg::pageCount()));
            // 页是第一次切过去才建的 ⇒ 先逐页切一遍，再按名字把面板认回来；
            // 顺带把**空位**那一层也按同一张键表对一遍（懒建之前先占位的就是它）。
            // 每一格（懒建之前先占位的空位）都按 pageCount() 那张键表排。
            QStringList slotOrder;
            for (int i = 0; i < expectedPanels.size(); ++i) {
                const QString &key = expectedPanels.at(i);
                QWidget *slot = i < stack->count() ? stack->widget(i) : nullptr;
                slotOrder.append(slot != nullptr && slot->objectName() ==
                                                       QStringLiteral("instanceSettingsSlot-") + key
                                     ? key
                                     : QString());
            }
            check(slotOrder == expectedPanels,
                  "★ U0a 堆叠页每一格（懒建之前占位的那一层）也和键表按下标对得上",
                  slotOrder.join(QLatin1Char('|')));
            // 每一页**第一次切过去就建出来**，objectName 与 panelName() 对得上。
            // NOTE: 有几个老页键（`integrity` / `repair` / …）已经并进别的页了：摆过去之后
            // 右栏停的是**并进去的那一页**，它们自己没有面板 —— 判据按产品自己的答案
            // （`currentPageKey()`）走，别在测试里抄 `maintenancePageKey()` 那张表。
            QStringList reachableKeys;
            for (const QString &key : expectedPanels) {
                dialog.showPage(key);
                const QString shown = dialog.currentPageKey();
                if (!shown.isEmpty() && !reachableKeys.contains(shown)) reachableKeys.append(shown);
            }
            const QSet<QString> formScrollKeys = {
                Dlg::overviewCategoryKey(), Dlg::launchCategoryKey(),
                QStringLiteral("operationsmenu"), QStringLiteral("migration"),
                QStringLiteral("upgrade")};
            QStringList builtPanels;
            int formScrollHit = 0;
            int formScrollExpected = 0;
            int viewportNoOuterScrollHit = 0;
            int viewportExpected = 0;
            for (const QString &key : reachableKeys) {
                QWidget *page = pageOf(dialog, key);
                if (page == nullptr || page->objectName() != Dlg::panelName(key)) continue;
                builtPanels.append(page->objectName());
                auto *scroll = dialog.findChild<QScrollArea *>(Dlg::scrollName(key));
                if (formScrollKeys.contains(key)) {
                    ++formScrollExpected;
                    formScrollHit += (scroll != nullptr && scroll->widget() == page && scroll->widgetResizable())
                                         ? 1
                                         : 0;
                } else {
                    ++viewportExpected;
                    viewportNoOuterScrollHit += (scroll == nullptr) ? 1 : 0;
                }
            }
            check(builtPanels.size() == reachableKeys.size() && !reachableKeys.isEmpty(),
                  "★ U0a 每一页（第一次切过去才建）都建得出来、objectName 与 panelName() 对得上",
                  QStringLiteral("%1/%2 页").arg(builtPanels.size()).arg(reachableKeys.size()));
            // 后面几条判的是"定位 / 选中"，先切回概览（这一节开始时停的就是它）。
            dialog.showPage(Dlg::overviewCategoryKey());
            // 键 → 下标那张表的**唯一出处**就是 stackPageIndexOf()：逐条对上，
            // 以及"钉死那两条在头"。
            int indexHit = 0;
            for (int i = 0; i < expectedPanels.size(); ++i) {
                indexHit += Dlg::stackPageIndexOf(expectedPanels.at(i)) == i ? 1 : 0;
            }
            check(indexHit == expectedPanels.size() &&
                      Dlg::stackPageIndexOf(Dlg::overviewCategoryKey()) == 0 &&
                      Dlg::stackPageIndexOf(Dlg::launchCategoryKey()) == 1,
                  "★ U0a stackPageIndexOf() 与堆叠页的实际下标逐条一致",
                  QStringLiteral("对上 %1/%2").arg(indexHit).arg(expectedPanels.size()));

            // 表单页外面套着它自己那一层滚动区，列表/树/编辑器/日志页不套外层滚动区避免三层嵌套。
            check(formScrollExpected > 0 && formScrollHit == formScrollExpected &&
                      viewportExpected > 0 && viewportNoOuterScrollHit == viewportExpected,
                  "★ U0a 表单页套独立滚动区（scrollName 与 panelName 一一对应），"
                  "列表/树/编辑器/日志页走视口容器不套外层滚动区",
                  QStringLiteral("表单 %1/%2、视口页无外层滚动 %3/%4")
                      .arg(formScrollHit).arg(formScrollExpected)
                      .arg(viewportNoOuterScrollHit).arg(viewportExpected));

            // 反过来：左栏**不在任何滚动区里**（装进去的话就又变回那一版了）。
            const auto insideScrollArea = [](const QWidget *widget) {
                for (const QWidget *w = widget; w != nullptr; w = w->parentWidget()) {
                    if (qobject_cast<const QScrollArea *>(w) != nullptr) {
                        return true;
                    }
                }
                return false;
            };
            check(!insideScrollArea(navTreeOf(dialog)),
                  "★ U0a 左栏那棵树**不在任何滚动区里**（它要钉在原地 —— 装进去就是"
                  "「滚内容连导航一起滚」那一版）");

            // 要求「实例设置里面的那个取消和确定删了」：整页上一颗都不该有。
            // 扫的是**文案**不是 objectName —— 那两个名字常量连声明一起删了，而这句话
            // 说的是"用户看得见的那两颗没了"。
            QStringList strayButtons;
            for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
                bool inSelectionBar = false;
                for (const QWidget *w = button; w != nullptr; w = w->parentWidget()) {
                    if (w->objectName() == InstanceFilesPage::selectionBarName()) {
                        inSelectionBar = true;
                        break;
                    }
                }
                if (inSelectionBar) {
                    continue;
                }
                const QString text = button->text().remove(QLatin1Char('&'));
                if (text == QStringLiteral("确定") || text == QStringLiteral("取消")) {
                    strayButtons.append(text);
                }
            }
            check(strayButtons.isEmpty(),
                  "★ U0a 这一页上**没有「确定」也没有「取消」**（改了就写，出口只有覆盖层"
                  "页眉上那颗返回箭头）",
                  strayButtons.isEmpty() ? QStringLiteral("（一颗都没有）")
                                         : strayButtons.join(QLatin1Char('|')));

            // 打开就是第一条：左栏亮「概览」、右栏是概览那一页。
            check(navKeyOf(dialog) == Dlg::overviewCategoryKey() &&
                      currentPanel(dialog) == Dlg::panelName(Dlg::overviewCategoryKey()),
                  "U0b 打开就停在「概览」（左栏亮哪一条、右栏就是哪一页）",
                  QStringLiteral("亮着 %1 / 右栏 %2")
                      .arg(navKeyOf(dialog), currentPanel(dialog)));

            // 左栏点哪一条 = 右栏换哪一页，**树上每一条叶子逐条走一遍**。树里那 17 条覆盖
            // 了三种挂法（顶层两条 / 组里那十五条 / 组本身不算），差一格就是"点「存档」开
            // 出来的是「模组」"，界面上不报任何东西。
            int entryPageHit = 0;
            QStringList entryPageMiss;
            const QStringList walkKeys = navLeafKeys(navTreeOf(dialog));
            for (const QString &key : walkKeys) {
                navTreeOf(dialog)->setCurrentItem(navItemOf(navTreeOf(dialog), key));
                if (currentPanel(dialog) != Dlg::panelName(key)) {
                    entryPageMiss.append(key);
                } else {
                    ++entryPageHit;
                }
            }
            check(!walkKeys.isEmpty() && entryPageHit == walkKeys.size(),
                  "★ U0c 左栏点哪一条 = 右栏换哪一页（树上那十七条叶子逐条走过）",
                  entryPageMiss.isEmpty()
                      ? QStringLiteral("对上 %1/%1 页").arg(entryPageHit)
                      : QStringLiteral("对上 %1/%2，没对上：%3")
                            .arg(entryPageHit)
                            .arg(walkKeys.size())
                            .arg(entryPageMiss.join(QLatin1Char('|'))));

            // 组**自己**点不动（它不是一页）：把组选中，右栏不许被清空 —— 这一条钉的就是
            // 上面那个"组的 UserRole 是空的"那份设计。
            {
                const QString beforeGroup = currentPanel(dialog);
                int groupItemsTried = 0;
                for (int i = 0; nav != nullptr && i < nav->topLevelItemCount(); ++i) {
                    QTreeWidgetItem *top = nav->topLevelItem(i);
                    if (top == nullptr || top->childCount() == 0) {
                        continue;
                    }
                    ++groupItemsTried;
                    nav->setCurrentItem(top);
                    QApplication::processEvents();
                }
                check(groupItemsTried > 0 && currentPanel(dialog) == beforeGroup,
                      "★ U0c 按组名（「内容管理」那一类）右栏**原地不动**（组不是一页）",
                      QStringLiteral("试了 %1 个组 / 右栏 %2")
                          .arg(groupItemsTried)
                          .arg(currentPanel(dialog)));
            }

            // 没有入口的那一页（「配置文件」）**照样摆得出来**：showPage() 认得它，
            // 而左栏里**没有它** ⇒ 选中被清空。这一条同时钉住两件事：
            // 页面本身还在（不然 showPage 直接 return、右栏原地不动），以及
            // "左栏不许亮着一个用户没点过的东西"。
            //
            // NOTE: 判据是"左栏什么都没亮"（`currentItem() == nullptr`）**不是**"亮着别的
            // 某一条" —— 后者会让用户以为"配置文件就是那一条"。
            dialog.showPage(Dlg::configPageKey());
            check(currentPanel(dialog) == Dlg::panelName(Dlg::configPageKey()) &&
                      navTreeOf(dialog)->currentItem() == nullptr,
                  "★ U0d showPage(\"config\") 摆得出那一页，而左栏里没有它（选中清空）",
                  QStringLiteral("右栏 %1 / 亮着 %2")
                      .arg(currentPanel(dialog),
                           navTreeOf(dialog)->currentItem() == nullptr
                               ? QStringLiteral("<空>")
                               : navKeyOf(dialog)));

            // 再摆回一条**有条目的**页：左栏那一行跟着亮起来（和用户自己点是同一条路）。
            dialog.showPage(Dlg::launchCategoryKey());
            check(currentPanel(dialog) == Dlg::panelName(Dlg::launchCategoryKey()) &&
                      navKeyOf(dialog) == Dlg::launchCategoryKey(),
                  "U0e showPage(\"launch\") 之后左栏亮的就是那一条、右栏就是启动那一页",
                  QStringLiteral("亮着 %1 / 右栏 %2")
                      .arg(navKeyOf(dialog), currentPanel(dialog)));

            // 越界：把当前项清成 nullptr 那一发会进到 currentItemChanged 里。右栏**不能**
            // 被清空（那种"点一下界面就空了"，用户只会当成崩了）。
            const QString beforeOutOfRange = currentPanel(dialog);
            navTreeOf(dialog)->setCurrentItem(nullptr);
            check(currentPanel(dialog) == beforeOutOfRange,
                  "★ U0f 左栏选中被清空之后右栏**原地不动**，没有被清空",
                  currentPanel(dialog));

            // 控件**真的落在该落的那一栏里** —— 这是每次重排都最容易错的地方（控件全部
            // 换过一次父容器，而"读值"那一侧一个字没改，落错了不会报错，只会让用户
            // 在「概览」里找不到「实例名」、或者找不到那几颗快捷按钮）。
            const auto belongsTo = [&pageOf, &expectedPanels, &dialog](const QWidget *widget) -> QString {
                for (const QWidget *w = widget; w != nullptr; w = w->parentWidget()) {
                    for (const QString &key : expectedPanels) {
                        QWidget *page = pageOf(dialog, key);
                        if (page != nullptr && w == page) {
                            return page->objectName();
                        }
                    }
                }
                return QString();
            };
            check(belongsTo(lineEditOf(dialog, Dlg::instanceNameEditName())) ==
                      Dlg::panelName(Dlg::overviewCategoryKey()),
                  "★ U0h 「实例名」那一格落在**概览**那一页里（分栏前它在整页最上面）",
                  belongsTo(lineEditOf(dialog, Dlg::instanceNameEditName())));
            check(belongsTo(lineEditOf(dialog, Dlg::javaPathEditName())) ==
                      Dlg::panelName(Dlg::launchCategoryKey()) &&
                      belongsTo(dialog.findChild<QWidget *>(Dlg::advancedBoxName())) ==
                          Dlg::panelName(Dlg::launchCategoryKey()),
                  "★ U0h Java 路径、内存那几行、高级选项那一块落在**启动**那一页里");
            check(belongsTo(dialog.findChild<QWidget *>(Dlg::formName(Dlg::overviewCategoryKey()))) ==
                          Dlg::panelName(Dlg::overviewCategoryKey()) &&
                      belongsTo(dialog.findChild<QWidget *>(Dlg::formName(Dlg::launchCategoryKey()))) ==
                          Dlg::panelName(Dlg::launchCategoryKey()),
                  "U0h 两页各自的表单也在自己那一页里（formName 与 panelName 一一对上）");

            // 那一排常用目录按钮和那七格勾选框都在**概览**里。
            // 前者是从「快捷方式」那一整页搬来的、勾选表是新写的，两样都换过父容器 ——
            // 而"换父容器"这件事一旦漏了一个，症状是"那一颗按钮在哪儿都找不着"
            // （它还在，只是挂在一页根本没露出来的控件底下）。
            //
            // NOTE: 那一排一度是十三条、而且可能收着几颗；
            // 去重后是**五条** —— 这一条数的仍然是"每一颗都在概览页里"（`belongsTo`
            // 看的是父链，和露不露无关），条数跟着捷径表走。
            int shortcutInOverview = 0;
            int checkboxesInOverview = 0;
            for (int i = 0; i < Dlg::shortcutTexts().size(); ++i) {
                shortcutInOverview +=
                    belongsTo(dialog.findChild<QPushButton *>(Dlg::shortcutButtonName(i))) ==
                            Dlg::panelName(Dlg::overviewCategoryKey())
                        ? 1
                        : 0;
            }
            for (const QString &key : Dlg::optionalPageKeys()) {
                checkboxesInOverview +=
                    belongsTo(dialog.findChild<QCheckBox *>(Dlg::pageCheckName(key))) ==
                            Dlg::panelName(Dlg::overviewCategoryKey())
                        ? 1
                        : 0;
            }
            check(shortcutInOverview == Dlg::shortcutTexts().size() &&
                      checkboxesInOverview == Dlg::optionalPageKeys().size(),
                  "★ U0h 五颗常用目录按钮与七格勾选框都落在**概览**那一页里",
                  QStringLiteral("按钮 %1/%2、勾选框 %3/%4")
                      .arg(shortcutInOverview)
                      .arg(Dlg::shortcutTexts().size())
                      .arg(checkboxesInOverview)
                      .arg(Dlg::optionalPageKeys().size()));
            // 「快捷方式」那一页**没了** —— 那一排按钮在概览上，而那一页的 formName 在这一页
            // 上找不回来（键表里已经没这一条）。
            check(dialog.findChild<QWidget *>(Dlg::formName(QStringLiteral("shortcut"))) == nullptr,
                  "★ U0h 「快捷方式」不再是单独一页（那一排按钮在概览页里）");

            // 几何：**只有一列**，左栏在内容区左边，宽度窄于内容区。
            dialog.show();
            QApplication::processEvents();
            QTreeWidget *navShown = navTreeOf(dialog);
            // NOTE: 左栏外面套了一层竖排容器（贴顶那个写法），所以它的 x() 是**相对那一层**
            // 的（永远是 0）。要比左右就得换算到整窗上，见 mapTo。
            const int listX = navShown->mapTo(&dialog, QPoint(0, 0)).x();
            const int stackX = stack->mapTo(&dialog, QPoint(0, 0)).x();
            check(listX < stackX && navShown->width() == Dlg::navigationTreeWidth(),
                  "U0g 左栏在内容区左边，宽度就是 navigationTreeWidth()",
                  QStringLiteral("x %1/%2，宽 %3（期望 %4）")
                      .arg(listX)
                      .arg(stackX)
                      .arg(navShown->width())
                      .arg(Dlg::navigationTreeWidth()));
            check(navShown->width() < stack->width(),
                  "U0g 左栏比内容区窄（导航列不该跟内容抢地方）",
                  QStringLiteral("%1 vs %2").arg(navShown->width()).arg(stack->width()));

            // U0g-2 左栏的宽度**不随"现在显示着哪几页"变**（树是定宽的，这条更硬）。
            //
            // 宽度只看表里**这一刻**有的那几条的话，用户在「高级选项」里勾掉「结构/原理图」
            // （左栏里最宽的那一条）之后，这一列会窄掉十来像素、**右栏整幅跟着横着跳一下**；
            // 同一个对话框开在两个实例上也可以不一样宽（"用不上的那一页"根本不在表里）。
            // 宽度是这一页的**骨架**，骨架不该跟着内容晃 —— 和「少一颗的那一行不摊开」是同一条
            // 性质的规矩。
            //
            // NOTE: 拿"把七条可选页**全勾掉**"来量是最狠也最省事的一发：树上那七条叶子当场
            // 消失（"内容管理"那一组会剩个空组），要是有哪一条比钉死那几条宽，宽度
            // 当场就会窄下来。
            {
                const int countBefore = navLeafKeys(navShown).size();
                const int widthBefore = navShown->width();
                QJsonObject hideAll;
                for (const QString &key : Dlg::optionalPageKeys()) {
                    hideAll.insert(key, false);
                }
                dialog.setPageChoices(hideAll);
                QApplication::processEvents();
                check(navLeafKeys(navShown).size() < countBefore,
                      "U0g-2 前提：七条可选页真的都从树上收起来了（不然这一条量的是空气）",
                      QStringLiteral("条目 %1 → %2")
                          .arg(countBefore)
                          .arg(navLeafKeys(navShown).size()));
                check(navShown->width() == widthBefore,
                      "★★ U0g-2 左栏宽度不跟着「显示哪些页」变（勾掉最宽的那一条也不缩）",
                      QStringLiteral("%1 → %2").arg(widthBefore).arg(navShown->width()));
            }

            // ---- U0j ★★ 页面按钮那一套的规格----
            //
            // 要求：「实例设置里面的所有按钮做成恒定宽度和高度的，每一行三个，间距适中
            // 且左右能充分利用」。这一段拿**概览页上那一排常用目录按钮**把这句话量出来：
            // ① 每一颗高度**一律** actionButtonHeight()；
            // ② 同一行那三颗**一样宽**（"恒定宽度"）；
            // ③ 同一行相邻两颗之间的空档正好是 actionButtonSpacing()（"间距适中"）；
            // ④ 第一行那一排的跨度**明显宽于**三颗最窄加起来（"左右能充分利用" —— 它们
            // 真的被摊满整幅，而不是挤成一小团靠左）。
            //
            // NOTE: 量之前必须两件事一起做 —— 先 `showPage()` 切到概览（`QStackedLayout`
            // 的 `minimumSize()` 与非当前页无关，非当前页**根本不参与布局**，量到的
            // 会是一堆 0），再把窗放大（窗一窄，第 ④ 条就退化成"挤满 = 各自顶着最窄
            // 那个数"，两边看不出差别）。
            //
            // NOTE: 这里量的 x()/width() 都是**相对那个装按钮的格子**的 —— 它们同父，
            // 比宽度和间距不需要换算坐标。
            // NOTE: 那一排一度是十三条；去重后回到**五条**
            // （默认"全都在"⇒ 五颗全在外面、也没有「更多目录…」）。下面这些是按
            // `shortcutTexts().size()` 现算的，一条都不写死条数。
            {
                dialog.resize(1000, 720);
                dialog.showPage(Dlg::overviewCategoryKey());
                QApplication::processEvents();
                QApplication::processEvents();

                QList<QPushButton *> shortcuts;
                int found = 0;
                for (int i = 0; i < Dlg::shortcutTexts().size(); ++i) {
                    QPushButton *button =
                        dialog.findChild<QPushButton *>(Dlg::shortcutButtonName(i));
                    shortcuts.append(button);
                    found += button != nullptr ? 1 : 0;
                }
                check(found == shortcuts.size() && !shortcuts.isEmpty(),
                      "U0j 前提：概览上那五颗常用目录按钮都按 objectName 找得回来",
                      QStringLiteral("找到 %1/%2").arg(found).arg(shortcuts.size()));

                QString heightBad;
                for (int i = 0; i < shortcuts.size(); ++i) {
                    const QPushButton *button = shortcuts.at(i);
                    if (button == nullptr || button->height() != Dlg::actionButtonHeight()) {
                        heightBad += QStringLiteral("%1(%2) ")
                                         .arg(i)
                                         .arg(button == nullptr ? -1 : button->height());
                    }
                }
                check(heightBad.isEmpty(),
                      "★★ U0j 概览上那一排常用目录按钮**高度一律** actionButtonHeight()",
                      heightBad.isEmpty()
                          ? QStringLiteral("每一颗都是 %1").arg(Dlg::actionButtonHeight())
                          : QStringLiteral("不是那个数的：") + heightBad);

                const int perRow = Dlg::actionButtonsPerRow();
                const int rows = (shortcuts.size() + perRow - 1) / perRow;
                QString rowBad;
                QStringList rowDetail;
                int firstRowSpan = -1;
                for (int r = 0; r < rows && (r + 1) * perRow <= shortcuts.size(); ++r) {
                    const QPushButton *a = shortcuts.at(r * perRow + 0);
                    const QPushButton *b = shortcuts.at(r * perRow + 1);
                    const QPushButton *c = shortcuts.at(r * perRow + 2);
                    if (a == nullptr || b == nullptr || c == nullptr) {
                        rowBad += QStringLiteral("第%1排有按钮没找到 ").arg(r);
                        continue;
                    }
                    const int gap1 = b->x() - (a->x() + a->width());
                    const int gap2 = c->x() - (b->x() + b->width());
                    rowDetail << QStringLiteral("第%1排 %2/%3/%4，间距 %5/%6")
                                     .arg(r)
                                     .arg(a->width())
                                     .arg(b->width())
                                     .arg(c->width())
                                     .arg(gap1)
                                     .arg(gap2);
                    const int wMin = qMin(a->width(), qMin(b->width(), c->width()));
                    const int wMax = qMax(a->width(), qMax(b->width(), c->width()));
                    if (wMax - wMin > 1) {
                        rowBad += QStringLiteral("第%1排三颗不一样宽 ").arg(r);
                    }
                    if (!(a->x() < b->x() && b->x() < c->x())) {
                        rowBad += QStringLiteral("第%1排的 x 不是递增 ").arg(r);
                    }
                    if (gap1 != Dlg::actionButtonSpacing() ||
                        gap2 != Dlg::actionButtonSpacing()) {
                        rowBad += QStringLiteral("第%1排的间距不是 actionButtonSpacing() ").arg(r);
                    }
                    if (r == 0) {
                        firstRowSpan = c->x() + c->width() - a->x();
                    }
                }
                check(rowBad.isEmpty(),
                      "★★ U0j 同一行那三颗**一样宽**（差最多 1 像素 = 除不尽的那一位）、"
                      "x 递增、相邻两颗的空档正好是 actionButtonSpacing()",
                      rowBad + rowDetail.join(QStringLiteral("；")));
                // 「充分利用」的判据：那一排比"三颗各自顶着最窄那个数"还**宽出整整一颗**
                // —— 宽出这么多只有一种来源：它们真的在摊满整幅。
                const int minSum = Dlg::actionButtonMinWidth() * perRow;
                check(firstRowSpan >= minSum + 2 * Dlg::actionButtonSpacing() +
                                          Dlg::actionButtonMinWidth(),
                      "★★ U0j 第一行那一排的跨度**明显宽于**三颗最窄加起来（它们真的摊满整幅，"
                      "而不是挤成一小团靠左）",
                      QStringLiteral("跨度 %1、三颗最窄加起来 %2（每颗至少 %3）")
                          .arg(firstRowSpan)
                          .arg(minSum)
                          .arg(Dlg::actionButtonMinWidth()));
            }
            dialog.hide();
        }

        // ---- U0h 概览栏那四行（图标+名称 / 两颗按钮 / 置顶 / 导出占位，）----
        //
        // 要求：「第一行 图标 名称 / 第二行 更改图标 分组… / 第三行 显示在置顶组中 /
        // 第四行 导出」。那几行都**不改启动参数**，改的是"这个实例是谁"：长什么样、摆在哪儿。
        //
        // 出参那条约定的**唯一例外**就在这一行里（图标那一行有十档、其中"本地图片"没有任何
        // 控件能表示它，所以它的当前值在成员里、按钮只是它的画，见 .hpp 里那段）。所以这里
        // 除了"读得回来"，还要逐档钉住"读出来的是什么、画出来的是不是同一个"。
        {
            const auto previewOf = [](const Dlg &d) {
                return d.findChild<QLabel *>(Dlg::iconPreviewName());
            };
            const auto candidateOf = [](const Dlg &d, int index) {
                return d.findChild<QToolButton *>(Dlg::iconCandidateButtonName(index));
            };
            // 「分组…」那颗按钮 + 它底下那个**常驻**浮层里的第 index 行。
            const auto groupButtonOf = [](const Dlg &d) {
                return d.findChild<QPushButton *>(Dlg::groupButtonName());
            };
            const auto groupMenuOf = [](const Dlg &d) {
                return d.findChild<QWidget *>(Dlg::groupMenuName());
            };
            const auto groupRowOf = [](const Dlg &d, int index) {
                return d.findChild<QPushButton *>(Dlg::groupMenuRowName(index));
            };
            const auto changeIconOf = [](const Dlg &d) {
                return d.findChild<QPushButton *>(Dlg::changeIconButtonName());
            };
            const auto iconMenuOf = [](const Dlg &d) {
                return d.findChild<QWidget *>(Dlg::iconMenuName());
            };
            const auto pinOf = [](const Dlg &d) {
                return d.findChild<QCheckBox *>(Dlg::pinCheckName());
            };

            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(),
                       true);
            const QStringList iconKeys = VersionIcons::iconCandidateKeys();
            const QStringList iconTexts = Dlg::iconCandidateTexts();

            bool allFound = previewOf(dialog) != nullptr && groupButtonOf(dialog) != nullptr &&
                            groupMenuOf(dialog) != nullptr && changeIconOf(dialog) != nullptr &&
                            iconMenuOf(dialog) != nullptr && pinOf(dialog) != nullptr &&
                            dialog.findChild<QPushButton *>(Dlg::iconPickButtonName()) != nullptr &&
                            dialog.findChild<QPushButton *>(Dlg::iconResetButtonName()) != nullptr;
            for (int i = 0; i < iconKeys.size(); ++i) {
                allFound = allFound && candidateOf(dialog, i) != nullptr;
            }
            check(allFound, "★ U0h 图标那一行（预览 + 八颗候选 + 选图/恢复默认）、两颗按钮"
                            "（更改图标 / 分组…）和它们那两个浮层、置顶勾选都按 objectName "
                            "找得回来");
            // 两个浮层**常驻**（不设 WA_DeleteOnClose、一次建好以后复用）⇒ 没弹出来的时候
            // 也找得到、里面的状态也是对的。判"此刻开着没开着"的判据是 isVisible()。
            check(!iconMenuOf(dialog)->isVisible() && !groupMenuOf(dialog)->isVisible(),
                  "★ U0h 两个浮层没弹出来的时候也在（常驻），而且这时候 isVisible() 是假"
                  "（否则验证工程没法自带一层「到底开没开」的判据）");
            check(iconKeys.size() == 8 && iconTexts.size() == iconKeys.size(),
                  "U0h 八个内置候选：键和界面上的名字一一对应");
            bool keysDistinct = true;
            for (int i = 0; i < iconKeys.size(); ++i) {
                for (int j = i + 1; j < iconKeys.size(); ++j) {
                    keysDistinct = keysDistinct && iconKeys.at(i) != iconKeys.at(j);
                }
            }
            // 键要写进 wtfml.meta，撞了就是"挑了这一张、读回来是那一张"，而且不报错。
            check(keysDistinct, "★ U0h 八个候选键两两不同（它们是写进 wtfml.meta 的身份）");

            // 初值：没被 setIcon() 叫过 = 「默认」那一档。
            check(dialog.iconChoice().isEmpty() && dialog.pickedIconPath().isEmpty(),
                  "U0h 没灌过初值时图标那一行停在「默认」（空串 = 这个键不写进 meta）",
                  dialog.iconChoice());
            bool noneChecked = true;
            for (int i = 0; i < iconKeys.size(); ++i) {
                noneChecked = noneChecked && !candidateOf(dialog, i)->isChecked();
            }
            check(noneChecked, "U0h 那一档下八颗按钮**一颗都没勾**（默认不是某一个候选）");

            // 逐档灌进去：读出来的是那个键，画出来的也只有那一颗勾着。
            const auto checkBuiltin = [&](int index) {
                dialog.setIcon(VersionIcons::iconValueForKey(iconKeys.at(index)), QPixmap(),
                               QPixmap());
                bool onlyThis = true;
                for (int i = 0; i < iconKeys.size(); ++i) {
                    onlyThis = onlyThis && (candidateOf(dialog, i)->isChecked() == (i == index));
                }
                return dialog.iconChoice() == VersionIcons::iconValueForKey(iconKeys.at(index)) &&
                       onlyThis;
            };
            bool allBuiltins = true;
            for (int i = 0; i < iconKeys.size(); ++i) {
                allBuiltins = allBuiltins && checkBuiltin(i);
            }
            check(allBuiltins, "★ U0h 八个内置候选逐档灌进去：读出来是那个键、八颗里只有那一颗勾着",
                  dialog.iconChoice());

            // 「本地那张」这一档：**没有一颗候选勾着**（它不是候选里的一员）。它在 meta 里的
            // 写法和候选不一样（`local`），而这两档混起来的表现是"挑了自己的图、下次打开
            // 变成某个内置候选"。
            const QPixmap localMark = [] {
                QPixmap mark(32, 32);
                mark.fill(Qt::red);
                return mark;
            }();
            dialog.setIcon(Dlg::localIconValue(), QPixmap(), localMark);
            bool anyChecked = false;
            for (int i = 0; i < iconKeys.size(); ++i) {
                anyChecked = anyChecked || candidateOf(dialog, i)->isChecked();
            }
            check(dialog.iconChoice() == Dlg::localIconValue() && !anyChecked,
                  "★ U0h 「本地那张」自成一档：读出来是 local、八颗候选一颗都不勾",
                  dialog.iconChoice());
            check(!previewOf(dialog)->pixmap().isNull(),
                  "U0h 「本地那张」那一档画的是调用方递进来的那张（不是空白）");

            // 点候选按钮 = 换到那一档（按钮是**能改值的**，不只是显示）。
            candidateOf(dialog, 3)->click();
            const bool clicked = dialog.iconChoice() == VersionIcons::iconValueForKey(iconKeys.at(3));
            check(clicked, "★ U0h 点第 N 颗候选 = 那一档（按钮和它显示的那张图是同一件事）",
                  dialog.iconChoice());
            // 弹出来时八格等宽（四列对齐），且挂 neutral 那一档（淡底 / 悬停加深 / 勾上强调色淡底），
            // 不是原生白底（深色主题下浅字落在白底上看不清）。三主题样张：ui_check_review_shots --popups。
            {
                dialog.show();
                changeIconOf(dialog)->click();
                QApplication::processEvents();
                bool sameWidth = iconMenuOf(dialog)->isVisible();
                bool neutral = true;
                for (int i = 0; i < iconKeys.size(); ++i) {
                    sameWidth = sameWidth && candidateOf(dialog, i)->width() == candidateOf(dialog, 0)->width();
                    neutral = neutral && candidateOf(dialog, i)->property("wtfmlButtonKind").toString()
                                             == Theme::buttonKindKey(Theme::ButtonKind::Neutral);
                }
                check(sameWidth && neutral, "★ U0h 图标浮层八格等宽、挂 neutral 选择格那一档");
                iconMenuOf(dialog)->close();
                dialog.hide();
            }
            // 「恢复默认」= 回到**空串**那一档，不是"回到第一个候选"。
            dialog.findChild<QPushButton *>(Dlg::iconResetButtonName())->click();
            bool clearedAll = dialog.iconChoice().isEmpty() && dialog.pickedIconPath().isEmpty();
            for (int i = 0; i < iconKeys.size(); ++i) {
                clearedAll = clearedAll && !candidateOf(dialog, i)->isChecked();
            }
            check(clearedAll, "★ U0h 「恢复默认」回到空串那一档（不是回到第一个候选）、也没勾着任何一颗");

            // 那一对拼/拆函数互逆 —— 它是写进 meta 的串和候选键之间唯一的桥。
            bool roundTrip = true;
            for (const QString &key : iconKeys) {
                roundTrip = roundTrip && VersionIcons::iconKeyFromValue(
                                            VersionIcons::iconValueForKey(key)) == key;
            }
            check(roundTrip, "U0h iconValueForKey / iconKeyFromValue 互逆（八个键逐个走过）");
            check(VersionIcons::iconKeyFromValue(Dlg::localIconValue()).isEmpty() &&
                      VersionIcons::iconKeyFromValue(QString()).isEmpty() &&
                      VersionIcons::iconKeyFromValue(QStringLiteral("dirt")).isEmpty(),
                  "★ U0h 「本地那张」/ 空串 / 光秃秃一个键名都不是内置那一档（认不出来给空串）");

            // ---- 分组那一行（「分组…」按钮 + 它底下那个浮层）----
            const QList<QPair<QString, QString>> groups = {
                {QStringLiteral("原版"), QStringLiteral("default-vanilla")},
                {QStringLiteral("可安装mod"), QStringLiteral("default-moddable")},
            };
            dialog.setGroupChoices(groups, QStringLiteral("default-moddable"));
            check(groupRowOf(dialog, 0) != nullptr && groupRowOf(dialog, 1) != nullptr &&
                      groupRowOf(dialog, 2) != nullptr && groupRowOf(dialog, 3) == nullptr,
                  "★ U0h 浮层里恰好摆着「未分组」+ 递进来的那两组（行数 = 1 + 组数，"
                  "objectName 从第 0 项起）");
            check(groupRowOf(dialog, 0)->text() == Dlg::ungroupedGroupText(),
                  "★ U0h 浮层第一项是「未分组」（它不是某一组，是这一列的「没有归属」）",
                  groupRowOf(dialog, 0)->text());
            check(groupRowOf(dialog, 1)->text() == QStringLiteral("原版") &&
                      groupRowOf(dialog, 2)->text() == QStringLiteral("可安装mod"),
                  "U0h 后面几行按调用方递进来的顺序（名字就是组名，id 才是身份）");
            check(dialog.chosenGroupId() == QStringLiteral("default-moddable"),
                  "U0h chosenGroupId() 读的是**当前那一行的组 id**（不是行上的字）",
                  dialog.chosenGroupId());
            check(!groupRowOf(dialog, 2)->isEnabled() && groupRowOf(dialog, 1)->isEnabled() &&
                      groupRowOf(dialog, 0)->isEnabled(),
                  "★ U0h 当前那一行（第 2 行 = 可安装mod）灰着、其余两行亮着 —— "
                  "「已经在这一组里了」是灰着的那一行说的",
                  QStringLiteral("未分组 %1 / 原版 %2 / 可安装mod %3")
                      .arg(groupRowOf(dialog, 0)->isEnabled())
                      .arg(groupRowOf(dialog, 1)->isEnabled())
                      .arg(groupRowOf(dialog, 2)->isEnabled()));
            check(!groupRowOf(dialog, 2)->toolTip().isEmpty(),
                  "★ U0h 灰着的那一行配了一句为什么（灰着不说话 = 用户以为界面坏了）",
                  groupRowOf(dialog, 2)->toolTip());
            check(groupButtonOf(dialog)->toolTip().contains(QStringLiteral("可安装mod")),
                  "★ U0h 按钮上的字是**固定的**（用户给的就是「分组…」），当前值走 tooltip",
                  groupButtonOf(dialog)->toolTip());
            // 点另一行 = 换到那一组，浮层跟着收起来。
            groupRowOf(dialog, 1)->click();
            check(dialog.chosenGroupId() == QStringLiteral("default-vanilla") &&
                      groupRowOf(dialog, 1)->isEnabled() == false &&
                      groupRowOf(dialog, 2)->isEnabled(),
                  "★ U0h 点一行就换到那一组（灰的那一格跟着挪过去）",
                  dialog.chosenGroupId());
            check(!groupMenuOf(dialog)->isVisible(),
                  "U0h 挑完就收（这颗按钮是「选一个」，不是一直摊着）");
            // 认不出来的 id（那一组刚被删掉）落在「未分组」上 —— 和实例树那边同一条规矩。
            dialog.setGroupChoices(groups, QStringLiteral("default-已经没了"));
            check(dialog.chosenGroupId().isEmpty() && !groupRowOf(dialog, 0)->isEnabled(),
                  "★ U0h 组 id 认不出来时停在「未分组」（不是停在随便某一组上）",
                  dialog.chosenGroupId());
            // 一个组都没有时也不该空着：至少还有「未分组」（那时候后面几行一个都不该剩 ——
            // 重建是**整个**重建，留着上一批的行数会让用户点到已经没了的组）。
            dialog.setGroupChoices({}, QString());
            check(groupRowOf(dialog, 0) != nullptr && groupRowOf(dialog, 1) == nullptr &&
                      dialog.chosenGroupId().isEmpty(),
                  "★ U0h 一个组都没有时浮层里只留「未分组」（上一批那几行真的没了）");

            // ---- 置顶那一行 ----
            //
            // 置顶是**两边都显示**（被设为置顶组的那个组里再放一个同名的）：
            // 这一格和「分组…」那一格**不互相影响** —— 一个管"要不要在
            // 置顶组里多摆一条"，一个管"老家在哪"。这里钉的是两者互不影响。
            dialog.setGroupChoices(groups, QStringLiteral("default-vanilla"));
            dialog.setPinned(true, QStringLiteral("default-moddable"));
            check(pinOf(dialog)->isChecked() && dialog.pinned(),
                  "★ U0h 置顶那一行的初值灌得进去（勾选框和 pinned() 说的是同一件事）");
            check(dialog.chosenGroupId() == QStringLiteral("default-vanilla") &&
                      groupRowOf(dialog, 1)->isEnabled() == false &&
                      groupButtonOf(dialog)->isEnabled(),
                  "★ U0h 勾上置顶**不动**「分组…」那一格（老家归老家、置顶归置顶）",
                  dialog.chosenGroupId());
            // 步骤 133 起提示挂在开关自己身上（makeSettingRow 各行同此惯例），行本身不再挂一份。
            check(pinOf(dialog)->toolTip() == Dlg::pinHint() &&
                      pinOf(dialog)->parentWidget()->toolTip().isEmpty(),
                  "收藏规则通过按需帮助提供，不重复占用页面（只挂在开关上一处）",
                  pinOf(dialog)->parentWidget()->toolTip());
            pinOf(dialog)->setChecked(false);
            check(!dialog.pinned() && dialog.chosenGroupId() == QStringLiteral("default-vanilla"),
                  "U0h 取消置顶：分组那一格还是原来那一组（两件事各管各的）");
            // 没有置顶组（用户把它删了）：勾选框**灰掉**并配一句为什么 —— 勾上这一下真的
            // 没有去处（产品的 pinInstance() 直接返回）。灰着不说话是这一层明令禁止的。
            dialog.setPinned(false, QString());
            check(!pinOf(dialog)->isEnabled() && !pinOf(dialog)->toolTip().isEmpty() &&
                      !dialog.pinned(),
                  "★ U0h 没有置顶组时勾选框灰掉、并说清为什么（不是「勾得上但什么都不发生」）",
                  pinOf(dialog)->toolTip());
            // 有置顶组时它是亮的、那句「没有置顶组」换回收藏规则（别让上一档那句话留着）。
            dialog.setPinned(false, QStringLiteral("default-moddable"));
            check(pinOf(dialog)->isEnabled() && pinOf(dialog)->toolTip() == Dlg::pinHint(),
                  "U0h 有置顶组时勾选框可用、那句「没有置顶组」的解释换回收藏规则",
                  pinOf(dialog)->toolTip());

            // ---- 那四行上的字----
            //
            // 概览这一栏**没有"标题在左、内容在右"的行名了**：那几句话变成了控件自己的文字
            // （要求「第一行 图标 名称 / 第二行 更改图标 分组… / 第三行 显示在置顶组中 /
            // 第四行 导出」）。于是"这一行是什么"和"这行上的字对不对"是同一件事 —— 字错了
            // 就等于这一行没名字，而下面 U1c 那条"实例名那行标签一处都没有"钉的是这一改动
            // 的另一半。**读的是产品的那个访问器**，不是这儿另抄一遍字（抄一遍的话改名字
            // 时它会跟着一起错，那种断言等于没有）。
            auto *nameEdit = dialog.findChild<QLineEdit *>(Dlg::instanceNameEditName());
            check(changeIconOf(dialog)->text() == Dlg::changeIconText() &&
                      groupButtonOf(dialog)->text() == Dlg::groupButtonText() &&
                      pinOf(dialog)->text() == Dlg::pinCheckText() && nameEdit != nullptr &&
                      Dlg::instanceNameText() == nameEdit->placeholderText(),
                  "★ U0h 那四行上的字就是用户给的那几句（实例名 / 更改图标 / 分组… / "
                  "显示在置顶组中）—— 这一栏没有行名，字就是名字",
                  QStringLiteral("「%1」「%2」「%3」")
                      .arg(changeIconOf(dialog)->text())
                      .arg(groupButtonOf(dialog)->text())
                      .arg(pinOf(dialog)->text()));
            // 第四行那颗「导出…」**是真的**：点得动、字和
            // tooltip 都换成产品那一份，而且**和「导出启动脚本」不是同一个 objectName**
            // —— 那两颗认错控件过一次（一个是整合包、一个是 .bat）。
            auto *exportPack = dialog.findChild<QPushButton *>(Dlg::exportButtonName());
            check(exportPack != nullptr && exportPack->isEnabled() &&
                      exportPack->text() == Dlg::exportText(),
                  "★ U0h 第四行那颗「导出…」是能点的了，"
                  "字是产品那个访问器给的",
                  exportPack != nullptr ? exportPack->text() : QString());
            check(exportPack != nullptr && exportPack->toolTip() == Dlg::exportButtonTooltip(),
                  "★ U0h 那颗的 tooltip 说全了两件事（.mrpack / .zip 各走哪套规范、认不出来"
                  "的原样进包），而下面看得见那行字短一截（页面上那条 E5 钉着它）",
                  exportPack != nullptr ? exportPack->toolTip() : QString());
            check(exportPack != nullptr &&
                      exportPack->objectName() != Dlg::exportScriptButtonName(),
                  "★ U0h 「导出整合包」和「导出启动脚本」是两个不同的控件"
                  "（名字长得像，认错的那一次是这儿）");
        }

        // ---- U0h2 「启动」栏那两颗动作按钮----
        //
        // 要求：「按钮文本直接做成设置名称」，举的两个例子就是「导出启动脚本」「补全版本
        // 文件」。于是这两项**没有行名**：按钮上那几个字就是它们的名字（两条 tooltip 和
        // 下面那句合起来的说明才是解释）。所以这里钉的是"字 == 产品的那个访问器"，而不是
        // 在断言里另抄一遍 —— 抄一遍的话改名字时两边一起错、断言等于没有。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(),
                       true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            auto *exportScript = dialog.findChild<QPushButton *>(Dlg::exportScriptButtonName());
            auto *repairFiles = dialog.findChild<QPushButton *>(Dlg::repairFilesButtonName());
            check(exportScript != nullptr && repairFiles != nullptr,
                  "★ U0h2 「启动」栏那两颗动作按钮按 objectName 找得回来（没有行名的那两项）");
            check(exportScript != nullptr && repairFiles != nullptr &&
                      exportScript->text() == Dlg::exportScriptText() &&
                      repairFiles->text() == Dlg::repairFilesText(),
                  "★ U0h2 两颗按钮上的字 == 这两项的名字（导出启动脚本 / 补全版本文件）",
                  QStringLiteral("「%1」「%2」")
                      .arg(exportScript != nullptr ? exportScript->text() : QString())
                      .arg(repairFiles != nullptr ? repairFiles->text() : QString()));
            check(exportScript != nullptr && repairFiles != nullptr &&
                      exportScript->toolTip() == Dlg::exportScriptHint() &&
                      repairFiles->toolTip() == Dlg::repairFilesHint(),
                  "U0h2 两颗按钮各自的 tooltip 就是那两句解释（字面量只有产品那一份）");
            check(exportScript->toolTip().contains(QStringLiteral("不保存令牌")),
                  "启动脚本说明与不持久化登录令牌的实现一致");
        }

        // ---- U0h3 概览页那颗「检查更新」----
        //
        // 要求：「实例设置那里做一个更新检测」。这一颗和上面那颗「导出…」是同一个形状
        // （一颗按钮 + 底下那句解释，点了之后**会出现一个结果**，而不是改一格设置就写盘），
        // 而且**只发信号**：反查盘上那几十个 jar 是谁、每个再去问一趟版本列表，全在
        // MainWindow 那边（`checkInstanceUpdates()`）—— 这一层连一个请求都不发，所以这里
        // 能单独构造它、单独点它。
        //
        // 钉三样，缺一不可：
        // · 那颗按钮按 objectName 找得回来、点得动、字是产品那个访问器给的（字错了等于
        // 这一项没名字）；
        // · 点一下**正好发一条** checkUpdatesRequested（点不动、发两条都不是这一档）；
        // · tooltip 那句里**必须有「说不清」这三个字** —— 判据是"盘上那个文件的 sha1 落
        // 在版本列表的第几条上"（`ModDependencies::judgeUpdate`），认不出来的那几条在
        // 界面上写的就是「说不清」。谁把它改成"已经全部检查过了"这一条必须红：用户会
        // 以为认不出来的那几条也是"没问题"。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(),
                       true);
            check(!dialog.findChild<QPushButton *>(Dlg::checkUpdatesButtonName()),
                  "manual update check is absent");
            int asked = 0;
            QObject::connect(&dialog, &Dlg::checkUpdatesRequested, &dialog, [&asked] { ++asked; });
            dialog.showPage(QStringLiteral("mod"));
            QApplication::processEvents();
            check(asked == 1, "entering mods requests one background check");
            dialog.showPage(QStringLiteral("mod"));
            QApplication::processEvents();
            check(asked == 1, "redisplaying current mod page does not duplicate checks");
            // 界面上的那三个字和判据那一层的 `updateUnknownText()` 必须是**同一个字**：
            // tooltip 里写的是给用户看的承诺（"认不出来的写「说不清」，那不是没问题"），
            // 而屏幕上真正印出来的那一格读的是 ModDependencies 那份（见
            // MainWindow::finishUpdateCheck）。两边各写一遍的话，谁改了一边都不会报错，
            // 用户看到的就是"tooltip 说会写「说不清」，报告里一个字都没有"。
            check(Dlg::checkUpdatesButtonTooltip().contains(ModDependencies::updateUnknownText()) &&
                      ModDependencies::updateUnknownText() != ModDependencies::updateUpToDateText() &&
                      ModDependencies::updateUnknownText() != ModDependencies::updateOutdatedText(),
                  "★ U0h3 tooltip 里那句和判据那一层的「说不清」是**同一个字**，而且三档"
                  "两两不同（「说不清」和「已是最新」混成一个词时这一条得红）",
                  ModDependencies::updateUnknownText());
            check(Dlg::checkUpdatesButtonName() != Dlg::exportButtonName() &&
                      Dlg::checkUpdatesButtonName() != Dlg::exportScriptButtonName(),
                  "U0h3 「检查更新」和「导出…」「导出启动脚本」是三个不同的 objectName"
                  "（同一个页面上三颗动作按钮，认错控件点下去就是没反应）");
        }

        // ---- U0i 实例图标那几张图本身（VersionIcons 那一侧）----
        //
        // 要求：「使用实际的那几个 mc 草块 / fabric 布料等的图片文件，…同时允许用户
        // 自主上传」。「用户自己上传」那一半落在 VersionIcons::fileMark()（读盘上任意一张图、
        // 缩成正方）。它是**唯一一处读用户文件的图标代码**，所以"读不动"和"读得动"两条
        // 路都要钉住 —— 前者决定用户挑了个 .txt 时屏幕上是什么，后者决定扁的图会不会被拉变形。
        {
            check(VersionIcons::iconMarkForKey(QStringLiteral("dirt"), 32).isNull() == false &&
                      VersionIcons::iconMarkForKey(QString(), 32).isNull() &&
                      VersionIcons::iconMarkForKey(QStringLiteral("草方块"), 32).isNull(),
                  "★ U0i 内置那八个键取得到图，认不出来的键给**空图**（调用方据此退回默认那张）");
            // 越界 = 草方块（和 loaderMark() 那个规矩一样：认不出来就当原版）。
            const QPixmap outOfRange = VersionIcons::iconCandidateMark(99, 32);
            check(!outOfRange.isNull() &&
                      outOfRange.toImage() == VersionIcons::grassBlock(32).toImage(),
                  "★ U0i 候选下标越界给草方块（不是一张空图）");

            QTemporaryDir scratch;
            check(scratch.isValid(), "U0i 前提：临时目录建得出来");
            const QString pngPath = scratch.filePath(QStringLiteral("pick.png"));
            // 一张**扁的**图（4×2）：正好能验"保持长宽比 + 补成正方"那一半 ——
            // 直接 scaled() 到 32×32 会把它拉成方的，而人像 / logo 被拉变形是最显眼的坏。
            QImage flat(4, 2, QImage::Format_ARGB32);
            flat.fill(QColor(255, 0, 0));
            check(flat.save(pngPath, "PNG"), "U0i 前提：那张扁图写得到盘上");

            const QPixmap mark = VersionIcons::fileMark(pngPath, 32);
            const QImage markImage = mark.toImage();
            // 扁图按长宽比缩到 32×32 那一格 ⇒ 32 宽 16 高，上下各留 8 像素透明。
            const bool centered = !markImage.isNull() && markImage.width() == 32 &&
                                  markImage.height() == 32 &&
                                  qAlpha(markImage.pixel(16, 16)) > 200 &&
                                  qAlpha(markImage.pixel(16, 1)) == 0 &&
                                  qAlpha(markImage.pixel(16, 30)) == 0;
            check(centered,
                  "★ U0i fileMark() 把扁图**保持长宽比**缩进那一格、四周补透明（不拉变形）",
                  QStringLiteral("%1x%2").arg(markImage.width()).arg(markImage.height()));

            // 读不动（用户在文件框里挑了个 .txt、或者那张图已经被删了）：空图，不是黑图。
            // 产品的表现因此是"退回默认那张"（用户在树上看见的还是一个方块），而不是
            // "我的图变成一块黑的了"。
            const QString notAnImage = scratch.filePath(QStringLiteral("note.txt"));
            QFile note(notAnImage);
            if (note.open(QIODevice::WriteOnly)) {
                note.write("这不是图");
                note.close();
            }
            check(VersionIcons::fileMark(notAnImage, 32).isNull() &&
                      VersionIcons::fileMark(scratch.filePath(QStringLiteral("没这个文件.png")), 32)
                          .isNull() &&
                      VersionIcons::fileMark(QString(), 32).isNull(),
                  "★ U0i 读不动 / 文件不在 / 空路径一律给**空图**（产品的退路是默认那张）");
        }

        // ---- U1 六项各一个档位下拉框 + 各一个控件，都按 objectName 找得回来 ----
        //
        // NOTE: 这里喂了窗口模式表。不喂的话「窗口模式」那一行**根本不建**（见构造函数
        // windowModes 那段），六项就只剩五项能够着 —— 那正是这一条要覆盖的第六项。
        {
            const QList<Dlg::WindowModeChoice> modes = {
                {QStringLiteral("小窗"), QStringLiteral("windowed")},
                {QStringLiteral("最大化"), QStringLiteral("maximized")},
                {QStringLiteral("全屏"), QStringLiteral("fullscreen")}};
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            const QStringList fields = {Dlg::javaPathField(),      Dlg::xmxField(),
                                        Dlg::xmsField(),           Dlg::extraJvmArgsField(),
                                        Dlg::windowModeField(),    Dlg::windowWidthField()};
            bool allChoices = true;
            bool allRows = true;
            QStringList wrong;
            for (const QString &field : fields) {
                const auto *combo = choiceCombo(dialog, field);
                const bool ok =
                    combo != nullptr && combo->count() == 2 &&
                    choiceText(dialog, field, Dlg::choiceFollowIndex()) == Dlg::followText() &&
                    choiceText(dialog, field, Dlg::choiceCustomIndex()) == Dlg::customText();
                allChoices = allChoices && ok;
                allRows = allRows &&
                          dialog.findChild<QWidget *>(Dlg::choiceRowName(field)) != nullptr;
                if (!ok) {
                    wrong << field;
                }
            }
            check(allChoices,
                  "★★ U1 六项「跟随全局 / 自定义」各是一个**两项的下拉框**（下标 0「跟随全局」、"
                  "下标 1「自定义」，文字就是那两句产品文案）",
                  wrong.isEmpty() ? QStringLiteral("六项都是")
                                  : wrong.join(QStringLiteral(" / ")));
            const QList<QRadioButton *> leftoverRadios = dialog.findChildren<QRadioButton *>();
            check(leftoverRadios.isEmpty(),
                  "★★ U1 这一整页里**一个单选框都不剩**（用户原话「所有单选框换成下拉的」）",
                  QStringLiteral("还剩 %1 颗").arg(leftoverRadios.size()));
            check(allRows,
                  "★★ U1 那六项「档位那一格」的 objectName 没变（choiceRowName() 照样找得回来）");
            check(choiceText(dialog, Dlg::javaPathField(), Dlg::choiceCustomIndex()) ==
                      Dlg::customText(),
                  "U1 下拉框里那两句就是约定的产品文案（「跟随全局」那句在别的行要写清当前值）");
            check(lineEditOf(dialog, Dlg::javaPathEditName()) != nullptr &&
                      lineEditOf(dialog, Dlg::extraJvmArgsEditName()) != nullptr &&
                      dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName()) != nullptr &&
                      dialog.findChild<QSpinBox *>(Dlg::memoryMinSpinName()) != nullptr &&
                      dialog.findChild<QPlainTextEdit *>(Dlg::appendJvmArgsEditName()) != nullptr,
                  "U1 五个控件 + 追加参数那个多行输入都找得回来");
            // 这一个窗里那两个内存数字框也装了滚轮防护（E2b 验的是设置页那一对）。
            // 后果和设置页那两个一样是"显示不对"，但这里的值会写进实例配置。
            {
                const QString maxProblem =
                    wheelGuardProblem(dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName()));
                const QString minProblem =
                    wheelGuardProblem(dialog.findChild<QSpinBox *>(Dlg::memoryMinSpinName()));
                check(maxProblem.isEmpty(),
                      "U1b 这个窗的「最大内存」数字框也装了滚轮防护", maxProblem);
                check(minProblem.isEmpty(),
                      "U1b 这个窗的「最小内存」数字框也装了滚轮防护", minProblem);
                QComboBox plainCombo;
                check(!wheelGuardProblem(&plainCombo).isEmpty(),
                      "U1b 反面：没装的控件照样会被判出来（判据不是恒真）");
                const QString sliderProblem =
                    wheelGuardProblem(dialog.findChild<QSlider *>(Dlg::memoryMaxSliderName()));
                check(sliderProblem.isEmpty(), "U1b 这个窗的「最大内存」那根条也装了滚轮防护",
                      sliderProblem);
            }
            // 「最小内存」那一整行**不显示了**（要求「不要 xms 了（或者
            // 隐藏起来），按照默认值」）。判据是"那一行自己被藏起来了"，不是"控件不在"
            // —— 对象还在、还跟着盘上的值走、override() 还照原样透传（见 .cpp 里那段）。
            // 反面对照取的是「最大内存」那一行（同样拿父控件问，它必须是露着的）。
            {
                auto *minSpin = dialog.findChild<QSpinBox *>(Dlg::memoryMinSpinName());
                auto *maxSpin = dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName());
                check(minSpin != nullptr && rowCellHidden(&dialog, Dlg::xmsField()),
                      "U1d 「最小内存」那一整行是藏起来的（撤掉的是给人改这一件事）",
                      minSpin != nullptr ? QString("那一格 hidden=%1")
                                               .arg(rowCellHidden(&dialog, Dlg::xmsField()) ? 1 : 0)
                                         : QStringLiteral("<控件没找到>"));
                check(maxSpin != nullptr && !rowCellHidden(&dialog, Dlg::xmxField()),
                      "U1d 反面：「最大内存」那一行照样露着（判据不是「这个窗什么都藏着」）");
            }

            // ---- U1c 「实例名」那一格（实例页右栏那个「重命名…」） ----
            // 它改的是这个实例的**身份**（磁盘上那个目录名），不是一项启动参数，所以单独一行、
            // 摆在这张表的最上面（先看清改的是哪一个，再谈别的）。这一格存在的理由就是下面
            // 那第二条断言：**初值取的是目录名、不是显示名** —— 拿显示名当初值的话，用户进
            // 这一页什么都没改、按一下确定，实例就被改成了一个奇怪的名字。
            {
                auto *nameEdit = dialog.findChild<QLineEdit *>(Dlg::instanceNameEditName());
                check(nameEdit != nullptr, "U1c 「实例名」那一格按 objectName 找得回来");
                check(nameEdit != nullptr && nameEdit->text() == instanceDirName,
                      "★ U1c 它的初值是**目录名**（instanceId），不是标题上那个显示名",
                      nameEdit != nullptr ? nameEdit->text() : QString());
                check(instanceDirName != instanceDisplayName,
                      "U1c 前提：这一节喂进去的两个名字本来就不一样（上面那条才量得出差别）",
                      QStringLiteral("%1 vs %2").arg(instanceDisplayName).arg(instanceDirName));
                // 那一句解释在界面上真的出现了 —— 从 Dlg:: 那几个 static 拿，断言里
                // 一个「实例名」的字面量都不抄（文案改了这里跟着改）。
                //
                // 「实例名」那**行名**没了（用户把概览排成
                // 「第一行 图标 名称（可编辑文本）」—— 那一行没有一个独占一行的标题，
                // 名字就在输入框的占位文字里）。所以这里钉的是**反过来**那一条：不能再有
                // 一个写着 instanceNameText() 的标签（留着的话界面上就是"标题 + 占位文字"
                // 把同一句话说两遍），而那句话确实由占位文字带着。
                int labelHits = 0;
                int hintHits = 0;
                const QList<QLabel *> labels = dialog.findChildren<QLabel *>();
                for (const QLabel *label : labels) {
                    labelHits += label->text() == Dlg::instanceNameText() ? 1 : 0;
                    hintHits += label->text() == Dlg::instanceNameHint() ? 1 : 0;
                }
                // UI 7b：概览改成「定宽标题列 | 控件」一行一项（设计稿 instance-overview），
                // 「实例名」回到行标题上、正好一份；占位文字留着（清空时才看得见，不算重复）。
                check(labelHits == 1 &&
                          nameEdit->placeholderText() == Dlg::instanceNameText(),
                      "★ U1c 「实例名」是那一行的标题、正好一份（占位文字也是那句）",
                      QStringLiteral("标签 %1 处 / 占位「%2」")
                          .arg(labelHits)
                          .arg(nameEdit->placeholderText()));
                check(hintHits == 0 && nameEdit->toolTip() == Dlg::instanceNameHint(),
                      "U1c 实例重命名说明在输入框提示中，不占用额外行",
                      QStringLiteral("找到 %1 处").arg(hintHits));
                // 读的是那一格里的字，而且**不 trim**：前后空格是用户自己写的，要原样返回。
                // （"改没改名"那条判据在 MainWindow 那边比 instanceId，不靠这里 trim。）
                nameEdit->setText(QStringLiteral("  我的整合包 x  "));
                check(dialog.instanceName() == QStringLiteral("  我的整合包 x  "),
                      "U1c instanceName() 读的就是那一格里的字、而且**不 trim**（前后空格都在）",
                      QStringLiteral("[%1]").arg(dialog.instanceName()));
            }

            // ---- U2 一项都不覆盖时：没有值，而且每行都写着"当前全局是哪个" ----
            // 五项逐个数，**故意不用** src 那边的 JavaSettingsOverride::isEmpty()：这一层
            // 的结构体是它自己的（ui/ 不 include src/），而"五项一个都没值"正是这一段的
            // 开场断言，写全了才不会被以后新加一项悄悄蒙过去。
            {
                const Dlg::Override fresh = dialog.override();
                check(!fresh.javaPath.has_value() && !fresh.xmx.has_value() &&
                          !fresh.xms.has_value() && !fresh.autoMemory.has_value() &&
                          !fresh.extraJvmArgs.has_value(),
                      "U2 全新对话框五项一项覆盖都没有（默认全跟随）");
            }
            check(hintOf(dialog, Dlg::javaPathField()) ==
                      Dlg::followHint(QStringLiteral("C:/全局/java.exe")),
                  "U2 Java 路径那行写着「跟随全局（当前 …）」",
                  hintOf(dialog, Dlg::javaPathField()));
            check(hintOf(dialog, Dlg::xmxField()) == Dlg::followHint(QStringLiteral("4096 MB")),
                  "U2 最大内存那行写的是全局那个数（不是空白）",
                  hintOf(dialog, Dlg::xmxField()));
            check(hintOf(dialog, Dlg::extraJvmArgsField()) ==
                      Dlg::followHint(QStringLiteral("-XX:+UseG1GC")),
                  "U2 额外参数那行也是",
                  hintOf(dialog, Dlg::extraJvmArgsField()));
        }

        // ---- U2b 防丢数据：启动页还没建出来时，那三问必须原样回去 ----
        //
        // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()），而写盘那一侧
        // 读的是 override() / appendJvmArgs() / warnJavaMismatch()。这几问要是"页没建就
        // 给空"，用户打开设置、只改一个图标，盘上那几项覆盖（Java 路径 / 内存 / 额外参数 /
        // 窗口）和版本提醒开关就被抹掉了 —— 界面上什么都看不出来，下一次启动才发现。
        {
            Dlg::Override current;
            current.javaPath = QStringLiteral("D:/jdk21/bin/java.exe");
            current.xmx = 6144;
            current.xms = 2048;
            current.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
            current.windowMode = QStringLiteral("fullscreen");
            current.windowWidth = 1600;
            current.windowHeight = 900;
            const QStringList append = {QStringLiteral("-Dfoo=bar baz"), QStringLiteral("-Xss2M")};
            const QList<Dlg::WindowModeChoice> modes = {
                {QStringLiteral("小窗"), QStringLiteral("windowed")},
                {QStringLiteral("全屏"), QStringLiteral("fullscreen")}};
            Dlg dialog(instanceDisplayName, instanceDirName, current, global, append, false, modes);
            const auto sameAsDisk = [&dialog, &current, &append]() {
                const Dlg::Override ov = dialog.override();
                return ov.javaPath == current.javaPath && ov.xmx == current.xmx &&
                       ov.xms == current.xms && ov.extraJvmArgs == current.extraJvmArgs &&
                       ov.windowMode == current.windowMode && ov.windowWidth == current.windowWidth &&
                       ov.windowHeight == current.windowHeight && !ov.autoMemory.has_value() &&
                       dialog.appendJvmArgs() == append && !dialog.warnJavaMismatch();
            };
            // ① 一页都没切：启动页还没建出来。
            check(sameAsDisk(),
                  "★ U2b 启动页还没建出来时 override() / appendJvmArgs() / warnJavaMismatch() "
                  "原样返回盘上那一份（不然改一下图标就把这几项覆盖抹了）",
                  dialog.appendJvmArgs().join(QLatin1Char('|')));
            // ② 切过去把页建出来：还是同一份。
            dialog.showPage(Dlg::launchCategoryKey());
            check(sameAsDisk(),
                  "★ U2b 切到启动页（页建出来了）之后那三问还是同一份",
                  dialog.appendJvmArgs().join(QLatin1Char('|')));
        }

        // ---- U3 逐项切「自定义」：只有那一项进 Override ----
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            setChoice(dialog, Dlg::javaPathField(), true);
            setChoice(dialog, Dlg::xmxField(), true);
            setChoice(dialog, Dlg::extraJvmArgsField(), true);
            lineEditOf(dialog, Dlg::javaPathEditName())->setText(QStringLiteral("D:/jdk8/java.exe"));
            dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->setValue(8192);
            lineEditOf(dialog, Dlg::extraJvmArgsEditName())->setText(QStringLiteral("-Xss2M"));

            const Dlg::Override ov = dialog.override();
            check(ov.javaPath.has_value() && *ov.javaPath == QStringLiteral("D:/jdk8/java.exe") &&
                      ov.xmx.has_value() && *ov.xmx == 8192 && ov.extraJvmArgs.has_value() &&
                      *ov.extraJvmArgs == QStringLiteral("-Xss2M"),
                  "U3 三项自定义的值都读出来了");
            check(!ov.xms.has_value() && !ov.autoMemory.has_value(),
                  "U3 没切「自定义」的那两项一个值都不给（跟随全局 = 键不存在）");
            check(hintOf(dialog, Dlg::xmxField()) !=
                      Dlg::followHint(QStringLiteral("4096 MB")),
                  "U3 切了自定义之后那行不再写「跟随全局（当前 …）」",
                  hintOf(dialog, Dlg::xmxField()));
        }

        // ---- U4 空的「自定义」Java 路径 = 跟随全局 ----
        // 不是"一个空路径覆盖"：实例覆盖了 javaPath 就等于手动档，而手动档 + 空路径是
        // **必然启动失败**的（"还没指定路径"）。用户清空这个框的意思是"这个实例不特殊
        // 指定"，那正是跟随全局。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            setChoice(dialog, Dlg::javaPathField(), true);
            lineEditOf(dialog, Dlg::javaPathEditName())->setText(QStringLiteral("   "));
            check(!dialog.override().javaPath.has_value(),
                  "U4 「自定义」但框里只有空格 → 归一成跟随全局",
                  // 注意别写 *opt：detail 这个实参**无论断言过不过都会求值**，
                  // 而解引用一个空 optional 是未定义行为。
                  dialog.override().javaPath.value_or(QStringLiteral("<无覆盖>")));
            // 反过来：额外参数自定义成空串**要留着** —— 全局带着 -XX:+UseG1GC，
            // 这个实例就是"不要额外参数"，那是一项真的覆盖。
            setChoice(dialog, Dlg::extraJvmArgsField(), true);
            lineEditOf(dialog, Dlg::extraJvmArgsEditName())->setText(QString());
            check(dialog.override().extraJvmArgs.has_value() &&
                      dialog.override().extraJvmArgs->isEmpty(),
                  "U4 但额外参数自定义成空串要留着（那是一种真的意思）");
        }

        // ---- U5 内存只剩一根拖动的条，自动档那一行撤掉了 ----
        //
        // （要求「内存分配和最大内存这玩意完全就是重复的，只保留那个
        // 拖动的」）：这一页**不再有**「内存分配」（自动（按系统内存））那一行。两条路
        // 仍然要钉住：
        // ① 这一页里找不到那个勾选框 / 那一格（防止哪天又被搭回来）；
        // ② 「跟随全局」时条和数字框仍然禁用（拖了也落不了值），切「自定义」才活。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            auto *maxSpin = dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName());
            auto *minSpin = dialog.findChild<QSpinBox *>(Dlg::memoryMinSpinName());
            check(dialog.findChild<QCheckBox *>(QStringLiteral("instanceMemoryAutoCheck")) ==
                          nullptr &&
                      dialog.findChild<QWidget *>(Dlg::choiceRowName(
                          QStringLiteral("autoMemory"))) == nullptr,
                  "U5 这一页不再有「内存分配」（自动 / 手动）那一行");
            check(!dialog.effectiveAutoMemory() && !maxSpin->isEnabled() && !minSpin->isEnabled(),
                  "U5 前景：这一行是「跟随全局」⇒ 两行内存**禁用**（拖了也落不了值）");
            check(dialog.override().autoMemory.has_value() == false,
                  "U5 前景：这一页也不再往覆盖里写 autoMemory");
            // 点了「自定义」就该能填 —— 别为了堵"静默丢弃"把路全堵死。
            setChoice(dialog, Dlg::xmxField(), true);
            check(maxSpin->isEnabled() && minSpin->isEnabled(),
                  "U5 切到「自定义」之后两行内存可用");
            maxSpin->setValue(2048);
            const Dlg::Override ov = dialog.override();
            check(ov.xmx.has_value() && *ov.xmx == 2048 && !ov.autoMemory.has_value(),
                  "U5 自定义的 Xmx 才算一项真覆盖（autoMemory 不写）",
                  ov.xmx.has_value() ? QString::number(*ov.xmx) : QStringLiteral("<无>"));

            // 全局是自动档时：这一页照样只有禁用 + 一句说明（自动档由设置页决定，这里
            // 没有能关掉它的入口）。
            Dlg::GlobalValues autoGlobal = global;
            autoGlobal.autoMemory = true;
            Dlg autoDialog(instanceDisplayName, instanceDirName, Dlg::Override(), autoGlobal,
                           QStringList(), true);
            // 同上：先把「启动」那一页建出来，再切回概览。
            autoDialog.showPage(Dlg::launchCategoryKey());
            autoDialog.showPage(Dlg::overviewCategoryKey());
            auto *autoMaxSpin =
                autoDialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName());
            check(autoDialog.effectiveAutoMemory() &&
                      autoMaxSpin != nullptr && !autoMaxSpin->isEnabled(),
                  "U5 全局自动档 ⇒ 这一页的条和数字框禁用（只能去设置页关自动）");
            check(hintOf(autoDialog, Dlg::xmxField()) == Dlg::memoryAutoDisabledHint(),
                  "U5 而且那行说明了为什么（自动档下这两个数按系统内存现算）",
                  hintOf(autoDialog, Dlg::xmxField()));
        }

        // ---- U5b 藏起来的那一行：存过的值照样透传，只有"比最大内存还大"的不透传 ----
        // Xms 那一行从界面上撤掉了，但**撤掉界面不等于替用户删掉他的设置**
        // —— 盘上存过的那份覆盖照样由 override() 透传回去。唯一必须拦的是 Xms > Xmx
        // 那一种：它是 JVM 直接拒绝启动的组合（Initial heap size set to a larger value
        // than the maximum heap size），而用户现在在界面上根本看不到自己存过这么一对。
        {
            Dlg::Override current;
            current.xmx = 2048;   // 这个实例自己钉了最大内存
            current.xms = 16384;  // 而盘上还留着一条比它大的最小内存（遗留值）
            Dlg dialog(instanceDisplayName, instanceDirName, current, global, QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            const Dlg::Override ov = dialog.override();
            check(ov.xmx.has_value() && *ov.xmx == 2048, "U5b 自定义的最大内存原样读回来",
                  ov.xmx.has_value() ? QString::number(*ov.xmx) : QStringLiteral("<无覆盖>"));
            check(ov.xms.has_value() && *ov.xms == 2048,
                  "U5b 超出去的最小内存被夹到最大内存上（JVM 会拒绝那对组合）",
                  ov.xms.has_value() ? QString::number(*ov.xms) : QStringLiteral("<无覆盖>"));

            // 反面：没超出去的那些**原样透传**（不能顺手把它抹掉 —— 那才是替用户删设置）
            Dlg::Override kept = current;
            kept.xms = 1024;
            Dlg dialog2(instanceDisplayName, instanceDirName, kept, global, QStringList(), true);
            const Dlg::Override ov2 = dialog2.override();
            check(ov2.xms.has_value() && *ov2.xms == 1024,
                  "U5b 没超出去的最小内存照旧透传（撤掉界面不等于删掉设置）",
                  ov2.xms.has_value() ? QString::number(*ov2.xms) : QStringLiteral("<无覆盖>"));
        }

        // ---- U6 追加参数：一条一行，空行丢掉，含空格的一条不许被切开 ----
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList({QStringLiteral("-Xss1M"), QStringLiteral("-Dfoo=bar baz")}),
                       true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            auto *append = dialog.findChild<QPlainTextEdit *>(Dlg::appendJvmArgsEditName());
            check(append != nullptr &&
                      append->toPlainText() == QStringLiteral("-Xss1M\n-Dfoo=bar baz"),
                  "U6 现有的追加参数一条一行灌进来（不是拼成一行）",
                  append != nullptr ? append->toPlainText() : QString());
            append->setPlainText(QStringLiteral("  -Xmx2G  \n\n-Dfoo=bar baz\n   \n-Xss4M"));
            check(dialog.appendJvmArgs() ==
                      QStringList({QStringLiteral("-Xmx2G"), QStringLiteral("-Dfoo=bar baz"),
                                   QStringLiteral("-Xss4M")}),
                  "U6 首尾空格去掉、空行丢掉、含空格的一条完整保留",
                  dialog.appendJvmArgs().join(QStringLiteral(" | ")));
            append->setPlainText(QString());
            check(dialog.appendJvmArgs().isEmpty(), "U6 空文本 = 没有追加参数");
        }

        // ---- U7 高级选项：默认折叠 ----
        //
        // NOTE: 分栏之后必须先**切到「启动」那一栏**再问。高级选项长在「启动」
        // 页里，而 QStackedLayout 会把非当前页 `hide()` 掉 —— 那一页的控件于是
        // "不visible"，`isHidden()` 虽然还只反映它自己的标志位（所以下面那几条碰巧
        // 还是过的），但这一段的**意思**是"在「启动」栏里它默认折叠"。不切栏的话
        // 量的是一个没露出来的页面，量对了也不说明产品对。
        // U7b 就不是"碰巧"了：那一整段的判据是窗口高度，而 QStackedLayout 的
        // minimumSize() **跳过所有 isHidden() 的页**（`QWidgetItem::isEmpty()`）。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            dialog.showPage(Dlg::launchCategoryKey());
            // 折叠行现在是**一颗 QPushButton**（整行可点、右边写「展开 / 收起」，不带箭头）。
            auto *toggle = dialog.findChild<QAbstractButton *>(Dlg::advancedToggleName());
            auto *box = dialog.findChild<QWidget *>(Dlg::advancedBoxName());
            check(toggle != nullptr && box != nullptr, "U7 高级选项那个折叠按钮和内容块都在");
            check(toggle != nullptr && toggle->text() == Dlg::advancedToggleText(),
                  "U7 按钮上的字是约定的那句");
            check(!dialog.advancedExpanded() && box->isHidden(),
                  "U7 默认**折叠**（里面几项都是改错了会让游戏起不来的东西）");
            toggle->setChecked(true);
            check(dialog.advancedExpanded() && !box->isHidden(), "U7 点开之后内容才露出来");
            toggle->setChecked(false);
            check(!dialog.advancedExpanded() && box->isHidden(), "U7 还能再折回去");
        }

        // ---- U7b 展开高级选项：窗口**一个字都不动**，多出来的内容让那一页自己滚 ----
        //
        // 展开高级选项再折回，窗口**一个字都不动**。Qt 只在"窗口比内容还小"时把窗口
        // **撑大**，内容变少时一动不动 —— 不补还原逻辑的话
        // 600x480 → 展开 600x503 → 折回**还是** 600x503（布局只要 310，多出来那一截
        // 全是空白）。产品在"展开前记下高度、折回时按它还原"。
        //
        // NOTE: 这一段必须**真的 show()**（U7 那一段没 show：布局在隐藏的窗上根本不重算）。
        // 开量之前先把窗缩到"折叠时最矮"那一档 —— 这样"多出来的内容一定超出一屏"
        // 是这个夹具保证的，跨机器都成立（不然这一条在"480 本来就够放"的机器上就是
        // 空转的）。
        //
        // NOTE: 还得先切到「启动」那一栏：高级选项在那一页里，而
        // `QStackedLayout::minimumSize()` 是**跳过非当前页**的。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList({QStringLiteral("-Xss1M")}), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            dialog.show();
            QApplication::processEvents();
            dialog.showPage(Dlg::launchCategoryKey());
            QApplication::processEvents();
            dialog.resize(dialog.width(), dialog.minimumSizeHint().height());
            QApplication::processEvents();
            auto *toggle = dialog.findChild<QAbstractButton *>(Dlg::advancedToggleName());
            // 当前那一页外面那层滚动区（剥壳的办法和上面 U0a 那条同一条）。
            // NOTE: 懒建之后堆叠页那一格是空位，滚动区套在它肚子里的那一页上（`scrollName()`）。
            QWidget *current = dialog.stack() != nullptr ? dialog.stack()->currentWidget() : nullptr;
            auto *scroll = current != nullptr
                               ? current->findChild<QScrollArea *>(
                                     Dlg::scrollName(Dlg::launchCategoryKey()))
                               : nullptr;
            check(scroll != nullptr && scroll->verticalScrollBar() != nullptr,
                  "U7b 前提：「启动」那一页外面套着它自己那层滚动区（下面两条靠它量）");
            const int collapsedHeight = dialog.height();
            const int collapsedWidth = dialog.width();
            const int collapsedMax =
                scroll != nullptr ? scroll->verticalScrollBar()->maximum() : -1;

            toggle->setChecked(true);
            QApplication::processEvents();
            const int expandedMax =
                scroll != nullptr ? scroll->verticalScrollBar()->maximum() : -1;
            check(dialog.height() == collapsedHeight,
                  "★ U7b 展开高级选项之后**窗口高度一个字都不动**（不被 "
                  "Qt 撑高、留一截空白）",
                  QStringLiteral("展开前 %1 / 现在 %2").arg(collapsedHeight).arg(dialog.height()));
            check(expandedMax > collapsedMax,
                  "★ U7b 多出来的那一块落在**这一页自己的滚动区**里（滚得动 = 用户够得着"
                  "底下那几项，而不是被切在看不见的地方）",
                  QStringLiteral("滚动范围 %1 → %2").arg(collapsedMax).arg(expandedMax));

            toggle->setChecked(false);
            // 两次：那一页套了几级布局，收敛要几拍（产品那边"由内到外 activate 一遍"是
            // 当场收敛的，这里多转两圈只是别让断言跑在布局前面）。
            QApplication::processEvents();
            QApplication::processEvents();
            check(dialog.height() == collapsedHeight,
                  "★ U7b 折回之后窗口高度还是那个数（既没留着空白、也没缩到别处去）",
                  QStringLiteral("展开前 %1 / 现在 %2").arg(collapsedHeight).arg(dialog.height()));
            check(dialog.width() == collapsedWidth, "U7b 全程只字未动宽度",
                  QStringLiteral("%1 / %2").arg(collapsedWidth).arg(dialog.width()));
            check(scroll != nullptr && scroll->verticalScrollBar()->maximum() == collapsedMax,
                  "★ U7b 折回之后那一块又收进去了（滚回原来那个范围）",
                  QStringLiteral("%1 / %2").arg(collapsedMax)
                      .arg(scroll != nullptr ? scroll->verticalScrollBar()->maximum() : -1));
            auto *box = dialog.findChild<QWidget *>(Dlg::advancedBoxName());
            check(box != nullptr && box->isHidden(), "U7b 内容块本身当然还是收着的");
            dialog.hide();
        }

        // ---- U8 灌值：盘上有覆盖时重开，必须停在「自定义」上 ----
        // 停在「跟随全局」的话，用户会以为自己的设置丢了，而点确定还会把它删掉。
        {
            Dlg::Override current;
            current.javaPath = QStringLiteral("D:/实例自己的/java.exe");
            current.autoMemory = false;
            Dlg::GlobalValues autoGlobal = global;
            autoGlobal.autoMemory = true;  // 全局是自动档，而这一行覆盖成手动
            Dlg dialog(instanceDisplayName, instanceDirName, current, autoGlobal,
                       QStringList({QStringLiteral("-Xss1M")}), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());

            check(isCustom(dialog, Dlg::javaPathField()) &&
                      !isFollow(dialog, Dlg::javaPathField()),
                  "U8 有覆盖的那一项停在「自定义」上");
            check(lineEditOf(dialog, Dlg::javaPathEditName())->text() ==
                      QStringLiteral("D:/实例自己的/java.exe"),
                  "U8 控件里填的是覆盖值",
                  lineEditOf(dialog, Dlg::javaPathEditName())->text());
            check(isFollow(dialog, Dlg::xmxField()),
                  "U8 没有覆盖的那一项停在「跟随全局」上");
            check(dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->value() == 4096,
                  "U8 而且它的起点是**全局的有效值**（用户是想微调，不是想从头填）",
                  QString::number(dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->value()));

            // `current.autoMemory = false` 这份**老 meta 覆盖不再生效** ——
            // 这一页没有能改它的入口了，有效值只看全局（见 effectiveAutoMemory()）。
            // 于是全局自动档下，切「自定义」也不能填 Xmx（自动档会把它重算掉）。
            check(dialog.effectiveAutoMemory() == true,
                  "U8 老 meta 里的 autoMemory=false 不再生效（这一页只听全局）");
            check(!dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->isEnabled() &&
                      !dialog.findChild<QSpinBox *>(Dlg::memoryMinSpinName())->isEnabled(),
                  "U8 全局自动档 ⇒ 两行内存禁用（拖了也落不了值）");
            setChoice(dialog, Dlg::xmxField(), true);
            check(!dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->isEnabled(),
                  "U8 自动档下切「自定义」也还是禁用（数字会被下次启动重算掉）");
            // 摆回「跟随全局」再读（下拉框就是摆回下标 0，见 U5 那条 NOTE）。
            setChoice(dialog, Dlg::xmxField(), false);
            const Dlg::Override back = dialog.override();
            check(!back.autoMemory.has_value() && !back.xmx.has_value(),
                  "U8 原样读回来：autoMemory 不再读出来、内存两项还是跟随",
                  QStringLiteral("autoMemory=%1 xmx=%2")
                      .arg(back.autoMemory.has_value() ? QStringLiteral("有值")
                                                       : QStringLiteral("<无>"),
                           back.xmx.has_value() ? QString::number(*back.xmx)
                                                : QStringLiteral("<无>")));
        }

        // ---- U8b 内存上限：由**调用方递进来**（设置页那一格算出来的那个数） ----
        // 上限不能是这一页自己算的（再读一次系统内存），否则"设置页能填 4096、
        // 实例页能填 16384"这种两边对不上的事做得到。
        // 只有一处算（MainWindowUI::memoryCapForTotalMb），由 MainWindow 从
        // 设置页那一格上取下来递进来（GlobalValues::memoryCapMb）。
        {
            const auto maxOf = [&](int capMb) {
                Dlg::GlobalValues g = global;
                g.memoryCapMb = capMb;
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), g, QStringList(),
                           true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                setChoice(dialog, Dlg::xmxField(), true);
                return dialog.findChild<QSpinBox *>(Dlg::memoryMaxSpinName())->maximum();
            };
            check(maxOf(8192) == 8192, "U8b 调用方给了上限就用它", QString::number(maxOf(8192)));
            // 不给（<= 0）时退回内置上限 —— 老调用点/验证工程不必都跟着改。
            check(maxOf(0) == MainWindowUI::memoryLimitMaxMb(),
                  "U8b 调用方没给（<= 0）就退回内置上限", QString::number(maxOf(0)));
            // 上限算法本身是**纯函数**、只有一处（往下取到 memoryStepMb() 的整数倍，
            // 有下限兜着）—— 这里把它的两头点一下，免得哪天有人顺手改了那儿。
            check(MainWindowUI::memoryCapForTotalMb(8191) == 8192 - MainWindowUI::memoryStepMb() &&
                      MainWindowUI::memoryCapForTotalMb(0) == MainWindowUI::memoryLimitMaxMb() &&
                      MainWindowUI::memoryCapForTotalMb(64) == MainWindowUI::memoryMaxMinMb(),
                  "U8b 上限算法：往下取整 / 读不到系统内存时给内置上限 / 有下限兜着",
                  QStringLiteral("%1 / %2 / %3")
                      .arg(MainWindowUI::memoryCapForTotalMb(8191))
                      .arg(MainWindowUI::memoryCapForTotalMb(0))
                      .arg(MainWindowUI::memoryCapForTotalMb(64)));
        }

        // ---- U9 「Java 版本不符时提醒」那一行：默认勾上，而且**不许漏进那五项** ----
        // 它是 wtfml.meta 里的另一个键（skipJavaVersionWarning），不是"值覆盖"：
        // 一旦漏进 Override，isEmpty() 就再也不为真，R5 那条精确五键的断言也会跟着失真。
        {
            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                       QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
            auto *warn = dialog.findChild<QCheckBox *>(Dlg::warnJavaMismatchCheckName());
            check(warn != nullptr, "U9 提醒勾选框按 objectName 找得回来");
            check(warn != nullptr && warn->isChecked(), "U9 默认**勾上**（缺省就是提醒）");
            check(dialog.warnJavaMismatch(), "U9 访问器读出来的也是 true");
            auto *advBox = dialog.findChild<QWidget *>(Dlg::advancedBoxName());
            check(advBox != nullptr && advBox->isAncestorOf(warn),
                  "U9 它在**高级选项**区里（不是一打开就摆在眼前的那几项）");
            const Dlg::Override ov = dialog.override();
            check(!ov.javaPath.has_value() && !ov.xmx.has_value() && !ov.xms.has_value() &&
                      !ov.autoMemory.has_value() && !ov.extraJvmArgs.has_value(),
                  "U9 它一个字段都没漏进那五项（覆盖层还是空的）");
            warn->setChecked(false);
            check(!dialog.warnJavaMismatch(), "U9 取消勾选之后读出来是 false");

            // 反过来：盘上记着"这个实例不再提醒"时重开，那一行必须是没勾的 ——
            // 否则用户关掉过一次，下次打开对话框又看到它勾着，等于开关没记住。
            Dlg dialog2(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                        QStringList(), false);
            // 同上：先把「启动」那一页建出来。
            dialog2.showPage(Dlg::launchCategoryKey());
            auto *warn2 = dialog2.findChild<QCheckBox *>(Dlg::warnJavaMismatchCheckName());
            check(warn2 != nullptr && !warn2->isChecked(), "U9 传 false 进来时它是没勾的");
            check(!dialog2.warnJavaMismatch(), "U9 访问器跟着是 false");
        }

        // ---- U9b 窗口那两行----
        //
        // 这两行比别处多一层绕：`Override` 里存的是**代号字符串**，代号表还是调用方**喂**
        // 进来的（ui/ 不许 include src/ —— 界面层不认识 GameWindowMode 这个枚举，也不认识
        // 那三个代号串；见 InstanceSettingsDialog.hpp 里的 WindowModeChoice）。
        // 所以这一节断的是"喂什么就显示什么、点了自定义才发值"这几条，**不是**代号长什么样
        // —— 那三个字面量钉在 verify 的 ZD5 那一节里。
        {
            using Dlg = InstanceSettingsDialog;
            const QList<Dlg::WindowModeChoice> modes = {
                {QStringLiteral("小窗"), QStringLiteral("windowed")},
                {QStringLiteral("最大化"), QStringLiteral("maximized")},
                {QStringLiteral("全屏"), QStringLiteral("fullscreen")}};
            Dlg::GlobalValues winGlobal = global;
            winGlobal.windowMode = QStringLiteral("windowed");
            winGlobal.windowModeLabel = QStringLiteral("小窗");
            winGlobal.windowWidth = 1280;
            winGlobal.windowHeight = 720;

            const auto comboOf = [](Dlg &d) {
                return d.findChild<QComboBox *>(Dlg::windowModeComboName());
            };
            const auto widthOf = [](Dlg &d) {
                return d.findChild<QSpinBox *>(Dlg::windowWidthSpinName());
            };
            const auto heightOf = [](Dlg &d) {
                return d.findChild<QSpinBox *>(Dlg::windowHeightSpinName());
            };
            const auto timesOf = [](Dlg &d) {
                return d.findChild<QLabel *>(Dlg::windowSizeTimesName());
            };

            // (1) 一个覆盖都没有：两行都停在「跟随全局」，下拉框停在全局那一档上。
            {
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), winGlobal,
                           QStringList(), true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                auto *combo = comboOf(dialog);
                auto *wSpin = widthOf(dialog);
                auto *hSpin = heightOf(dialog);
                check(combo != nullptr && wSpin != nullptr && hSpin != nullptr &&
                          timesOf(dialog) != nullptr,
                      "U9b 窗口那两行的四个控件都按 objectName 找得回来");
                check(choiceCombo(dialog, Dlg::windowModeField()) != nullptr &&
                          choiceCombo(dialog, Dlg::windowWidthField()) != nullptr,
                      "U9b 那两行各有一个「跟随全局 / 自定义」档位下拉框");
                // 两个档位下拉框**不是**同一格：模式那一行和分辨率那一行各管各的（模式跟随
                // 全局、大小自定义是完全正常的一种配法）。共用一格的话，改一处会连带另一处。
                {
                    auto *modeRow =
                        dialog.findChild<QWidget *>(Dlg::choiceRowName(Dlg::windowModeField()));
                    auto *sizeRow =
                        dialog.findChild<QWidget *>(Dlg::choiceRowName(Dlg::windowWidthField()));
                    check(modeRow != nullptr && sizeRow != nullptr && modeRow != sizeRow &&
                              choiceCombo(dialog, Dlg::windowModeField()) !=
                                  choiceCombo(dialog, Dlg::windowWidthField()),
                          "U9b 模式那一行和分辨率那一行是两个格子（各有一个档位下拉框）");
                }
                if (combo != nullptr && wSpin != nullptr && hSpin != nullptr) {
                    // 项是**喂进来的那份**、顺序就是喂进来的顺序、itemData 是代号。
                    QStringList got;
                    for (int i = 0; i < combo->count(); ++i) {
                        got << combo->itemData(i).toString();
                    }
                    check(combo->count() == modes.size() &&
                              got == QStringList({QStringLiteral("windowed"),
                                                  QStringLiteral("maximized"),
                                                  QStringLiteral("fullscreen")}),
                          "U9b 下拉框里的档位就是喂进来的那一份（顺序也是）",
                          got.join(QStringLiteral(" | ")));
                    check(combo->currentData().toString() == winGlobal.windowMode,
                          "U9b 没有覆盖时它停在**全局那一档**上（不是第 0 项）",
                          combo->currentData().toString());
                    // 停在「跟随全局」⇒ 这一行的控件是灰的（改了也发不出去，见 override()）。
                    check(!combo->isEnabled(), "U9b 跟随全局时下拉框是灰的");
                    check(!wSpin->isEnabled() && !hSpin->isEnabled(),
                          "U9b 跟随全局时那两个数字框是灰的");
                    check(hintOf(dialog, Dlg::windowModeField()) ==
                              Dlg::followHint(QStringLiteral("小窗")),
                          "U9b 「跟随全局」那句话写明了跟随的是**哪一档**",
                          hintOf(dialog, Dlg::windowModeField()));
                    check(hintOf(dialog, Dlg::windowWidthField()) ==
                              Dlg::followHint(QStringLiteral("1280×720")),
                          "U9b 分辨率那句写明跟随的是**多大**（不用退出去翻设置页）",
                          hintOf(dialog, Dlg::windowWidthField()));
                    check(!dialog.override().windowMode.has_value() &&
                              !dialog.override().windowWidth.has_value() &&
                              !dialog.override().windowHeight.has_value(),
                          "U9b 两行都没动过 ⇒ 一个字段都不发（覆盖层保持空）");
                }
            }

            // (1b) 那一行的几何：`宽 × 高` 三样**挨在一起**、整行靠左
            //
            // 唯一的洞是「×」那一颗 —— 四颗里只有它**没带对齐**。盒子布局里"没有对齐、
            // 又能长"的那一颗会把多出来的空间**全吃掉**（QLabel 的默认策略就是能长），
            // 于是屏幕上成了 `[跟随全局▾][1280▾] × [720▾]`：一行两个数
            // 被拉开半个屏。上面那段注释写着"整行按 sizeHint 靠左摆"，可代码里没有一处
            // 这么要求（文件里那条「注释与代码相反」的教训）。真正的钉子是末尾那个弹簧。
            //
            // NOTE: 判据用"相邻两颗的空档"而不是"右边还剩多少" —— 空档正是那个弹簧管的东西，
            // 而右边剩多少还跟着这一格有多宽走（窗一大就变，不该拿来做断言）。
            {
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), winGlobal,
                           QStringList(), true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                // 几何断言的前提：**得先把这一页摆出来**。两件事缺一不可 ——
                // · 没 `show()` 过的控件，x()/width() 全是那个 100×30 的默认值；
                // · 「窗口大小」在「启动」那一页上，而堆叠里**不是当前页的那些页根本不参与
                // 布局**（见 U0j 那段 NOTE）⇒ 光 show() 还是量不到，得先 `showPage()`
                // 切过去。量出来 -100 那种看着像界面坏了的数，就是这个原因。
                dialog.show();
                dialog.showPage(Dlg::launchCategoryKey());
                QApplication::processEvents();
                QApplication::processEvents();
                auto *wSpin = widthOf(dialog);
                auto *hSpin = heightOf(dialog);
                auto *times = timesOf(dialog);
                auto *sizeRow =
                    dialog.findChild<QWidget *>(Dlg::choiceRowName(Dlg::windowWidthField()));
                check(wSpin != nullptr && hSpin != nullptr && times != nullptr &&
                          sizeRow != nullptr,
                      "U9b 前提：「窗口大小」那一行的四颗（含 ×）都找得回来");
                if (wSpin != nullptr && hSpin != nullptr && times != nullptr &&
                    sizeRow != nullptr) {
                    // 布局给的空档是 8；这里留到 12 是给样式的边框/边距留一点余量。
                    const int gapBefore = times->x() - (wSpin->x() + wSpin->width());
                    const int gapAfter = hSpin->x() - (times->x() + times->width());
                    check(gapBefore >= 0 && gapBefore <= 12 && gapAfter >= 0 && gapAfter <= 12,
                          "★★ U9b 「宽 × 高」三样挨在一起（中间没有哪一颗被拉伸撑开）",
                          QStringLiteral("空档 %1 / %2（布局的间距是 8）")
                              .arg(gapBefore)
                              .arg(gapAfter));
                    check(hSpin->x() + hSpin->width() + 40 < sizeRow->width(),
                          "U9b 整行靠左、右边留着白（多出来的空间归末尾那个弹簧）",
                          QStringLiteral("右边界 %1 / 整行宽 %2")
                              .arg(hSpin->x() + hSpin->width())
                              .arg(sizeRow->width()));
                }
                // 上面那一发 show() 是真的把窗摆出来了 ⇒ 这里得收回去，别让它跟着跑到后面几节。
                dialog.hide();
            }

            // (2) 切到「自定义」：控件亮起来，override() 才把值发出去。
            {
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), winGlobal,
                           QStringList(), true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                auto *combo = comboOf(dialog);
                auto *wSpin = widthOf(dialog);
                auto *hSpin = heightOf(dialog);
                setChoice(dialog, Dlg::windowModeField(), true);
                setChoice(dialog, Dlg::windowWidthField(), true);
                check(combo != nullptr && combo->isEnabled() &&
                          wSpin != nullptr && wSpin->isEnabled() && hSpin != nullptr &&
                          hSpin->isEnabled() &&
                          (timesOf(dialog) == nullptr || timesOf(dialog)->isEnabled()),
                      "U9b 切到「自定义」之后三个控件连着 × 都亮了");
                if (combo != nullptr && wSpin != nullptr && hSpin != nullptr) {
                    check(wSpin->value() == 1280 && hSpin->value() == 720,
                          "U9b 数字框的起点是**全局的有效值**（用户是想微调，不是从头填）",
                          QStringLiteral("%1×%2").arg(wSpin->value()).arg(hSpin->value()));
                    combo->setCurrentIndex(combo->findData(QStringLiteral("fullscreen")));
                    wSpin->setValue(1600);
                    hSpin->setValue(900);
                    const Dlg::Override ov = dialog.override();
                    check(ov.windowMode.has_value() &&
                              *ov.windowMode == QStringLiteral("fullscreen") &&
                              ov.windowWidth.has_value() && *ov.windowWidth == 1600 &&
                              ov.windowHeight.has_value() && *ov.windowHeight == 900,
                          "U9b 自定义时读出来的是改完之后的那一份（代号原样透传）",
                          QStringLiteral("%1 %2×%3")
                              .arg(ov.windowMode.value_or(QStringLiteral("<无>")))
                              .arg(ov.windowWidth.value_or(0))
                              .arg(ov.windowHeight.value_or(0)));
                }
            }

            // (3) 盘上有覆盖时重开：停在「自定义」上、控件里是覆盖值。
            // 停在「跟随全局」的话用户会以为设置丢了，而点确定还会把它删掉。
            {
                Dlg::Override current;
                current.windowMode = QStringLiteral("maximized");
                current.windowWidth = 1920;
                current.windowHeight = 1080;
                Dlg dialog(instanceDisplayName, instanceDirName, current, winGlobal, QStringList(),
                           true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                auto *combo = comboOf(dialog);
                auto *wSpin = widthOf(dialog);
                check(isCustom(dialog, Dlg::windowModeField()) &&
                          isCustom(dialog, Dlg::windowWidthField()),
                      "U9b 有覆盖的那两行都停在「自定义」上");
                check(combo != nullptr && combo->currentData().toString() ==
                                              QStringLiteral("maximized"),
                      "U9b 下拉框停在覆盖的那一档上",
                      combo != nullptr ? combo->currentData().toString() : QStringLiteral("<null>"));
                check(wSpin != nullptr && wSpin->value() == 1920,
                      "U9b 数字框里是覆盖值");
                // 原样读回去（用户什么都没动，点确定不该改盘上那份）。
                const Dlg::Override back = dialog.override();
                check(back.windowMode.value_or(QString()) == QStringLiteral("maximized") &&
                          back.windowWidth.value_or(0) == 1920 &&
                          back.windowHeight.value_or(0) == 1080,
                      "U9b 一进来就读回去：和盘上那份逐项相同");
            }

            // (4) 盘上那个代号产品认不出来（换过名、或者手写过）：停在第一档，不崩、不留空。
            // 真实后果是用户看见「小窗」而盘上写着别的 —— 但那一份本来就谁也读不懂，
            // **可断言的是"界面不空、也不越界"**（真正的兜底在合成那一侧，见 verify ZD5）。
            {
                Dlg::Override current;
                current.windowMode = QStringLiteral("最大窗口");
                Dlg dialog(instanceDisplayName, instanceDirName, current, winGlobal, QStringList(),
                           true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                auto *combo = comboOf(dialog);
                check(combo != nullptr && combo->currentIndex() == 0 &&
                          !combo->currentData().toString().isEmpty(),
                      "U9b 认不出来的代号 ⇒ 退回第 0 档（不是空着一格）",
                      combo != nullptr ? combo->currentData().toString()
                                       : QStringLiteral("<null>"));
            }

            // (5) 调用方没喂档位表（老调用点 / 别的验证工程那样构造）：模式那一行**根本不
            // 出现**，而分辨率那一行照旧 —— 这样加了这一档之后，没跟上的调用点也不会
            // 冒出一个空下拉框。
            {
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), winGlobal,
                           QStringList(), true);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                check(comboOf(dialog) == nullptr &&
                          dialog.findChild<QWidget *>(
                              Dlg::choiceRowName(Dlg::windowModeField())) == nullptr,
                      "U9b 没喂档位表时那一行不出现（老调用点不必跟着改）",
                      comboOf(dialog) == nullptr ? QStringLiteral("没有下拉框")
                                                 : QStringLiteral("有下拉框"));
                check(widthOf(dialog) != nullptr && heightOf(dialog) != nullptr,
                      "U9b 而分辨率那一行照旧在（它不依赖调用方喂任何东西）");
            }

            // (6) 滚轮防护：三个控件都装了（下拉框滚一格就换一档，数字框滚一格就改数）。
            {
                Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), winGlobal,
                           QStringList(), true, modes);
            // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：这一节要摸
            // 「启动」那一页上的控件，先切过去把它建出来，再切回**概览** —— 打开对话框停的
            // 就是概览，这一节里量几何的那些判据都是在那上面量的。
            dialog.showPage(Dlg::launchCategoryKey());
            dialog.showPage(Dlg::overviewCategoryKey());
                const QString comboProblem = wheelGuardProblem(comboOf(dialog));
                const QString widthProblem = wheelGuardProblem(widthOf(dialog));
                const QString heightProblem = wheelGuardProblem(heightOf(dialog));
                check(comboProblem.isEmpty(), "U9b 这个窗的「窗口模式」下拉框也装了滚轮防护",
                      comboProblem);
                check(widthProblem.isEmpty() && heightProblem.isEmpty(),
                      "U9b 「窗口大小」那两个数字框也装了滚轮防护", widthProblem + heightProblem);
            }
        }
    }

    // ------------------------------------------------------------------
    // U9c. 脏状态的签名（`InstanceSettingsSignature::signatureOf()`）。
    //
    // 这条判据以前埋在对话框的私有函数里，验证工程够不着；现在它是纯函数，可以直接钉：
    // **值一样 ⇒ 签名一样**（否则每开一次这一页就白写一次盘），**任何一格变了 ⇒ 一定不同**
    // （否则改了不写盘：界面上看着改了，下次开又是老样子）。
    {
        namespace Sig = InstanceSettingsSignature;
        const auto baseInputs = [] {
            Sig::Inputs in;
            in.javaPath = QStringLiteral("C:/java/bin/java.exe");
            in.xmx = 4096;
            in.xms = 1024;
            in.autoMemory = false;
            in.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
            in.windowMode = QStringLiteral("windowed");
            in.windowWidth = 1280;
            in.windowHeight = 720;
            in.appendArgs = {QStringLiteral("--add-opens=a"), QStringLiteral("--add-opens=b")};
            in.warnJavaMismatch = false;
            in.iconSignature = QStringLiteral("local|C:/img/icon.png");
            in.pageChecks = {{QStringLiteral("overview"), true}, {QStringLiteral("servers"), false}};
            in.groupId = QStringLiteral("g-1");
            in.pinned = true;
            return in;
        };
        const QStringList reference = Sig::signatureOf(baseInputs());
        check(reference == Sig::signatureOf(baseInputs()),
              "★ U9c 同样的值两次算出来逐字一样（没改就别说改了）");
        check(reference.join(QLatin1Char('\n')).contains(QStringLiteral("--add-opens=a")),
              "★ U9c 追加参数按条进签名（不是拼成一坨）", reference.join(QLatin1Char('|')));

        int differing = 0;
        const auto expectDiffers = [&](const Sig::Inputs &changed, const char *label) {
            const bool differs = Sig::signatureOf(changed) != reference;
            check(differs, label);
            if (differs) {
                ++differing;
            }
        };
        {
            Sig::Inputs changed = baseInputs();
            changed.xmx = 6144;
            expectDiffers(changed, "★ U9c 最大内存改了 ⇒ 签名变");
        }
        {
            // "跟随全局"和"自定义成一个值"必须是两份签名 —— `std::optional` 的空/有值那一档。
            Sig::Inputs changed = baseInputs();
            changed.xmx.reset();
            expectDiffers(changed, "★ U9c 从「自定义 4096」切到「跟随全局」⇒ 签名变");
        }
        {
            Sig::Inputs changed = baseInputs();
            changed.appendArgs = {QStringLiteral("--add-opens=b"), QStringLiteral("--add-opens=a")};
            expectDiffers(changed, "★ U9c 追加参数只换了顺序 ⇒ 也算变了（本来就是两件事）");
        }
        {
            Sig::Inputs changed = baseInputs();
            changed.pageChecks = {{QStringLiteral("servers"), false},
                                  {QStringLiteral("overview"), true}};
            expectDiffers(changed, "★ U9c 「显示哪几页」的顺序变了 ⇒ 签名跟着变（按界面顺序拼）");
        }
        {
            Sig::Inputs changed = baseInputs();
            changed.iconSignature = QStringLiteral("local|C:/img/other.png");
            expectDiffers(changed, "★ U9c 换了一张本地图 ⇒ 签名变（路径也进签名）");
        }
        {
            Sig::Inputs changed = baseInputs();
            changed.pinned = false;
            expectDiffers(changed, "★ U9c 取消置顶 ⇒ 签名变");
        }
        {
            Sig::Inputs changed = baseInputs();
            changed.groupId = QStringLiteral("g-2");
            expectDiffers(changed, "★ U9c 换分组 ⇒ 签名变（按 id，不按组名）");
        }
        check(differing == 7, "★ U9c 七种改法一条都没漏", QString::number(differing));
    }
    // ------------------------------------------------------------------
    // U9d. 左栏"显示哪几页"那两条纯判断（`InstanceSettingsPageChoices`）。
    //
    // 以前它们在对话框里，验证工程够不着；现在能直接钉两条规矩：认不出的键丢掉（前向兼容），
    // 顺序按调用方给的清单（不能按 QHash 遍历顺序，否则每次都可能不一样）。
    {
        const QStringList optional = {QStringLiteral("save"), QStringLiteral("server"),
                                      QStringLiteral("mod")};
        const QJsonObject stored{{QStringLiteral("save"), false},
                                 {QStringLiteral("server"), true},
                                 {QStringLiteral("gone"), false},      // 以后删掉的那一页
                                 {QStringLiteral("mod"), QStringLiteral("no")}};  // 不是 bool
        const QJsonObject normalized = InstanceSettingsPageChoices::normalized(stored, optional);
        const bool saveStaysFalse = !normalized.value(QStringLiteral("save")).toBool(true);
        const bool serverStaysTrue = normalized.value(QStringLiteral("server")).toBool(false);
        check(normalized.size() == 2 && saveStaysFalse && serverStaysTrue,
              "★ U9d 归一化只收认得的、值是 bool 的那几键（未知页与非 bool 值都丢掉）",
              QString::fromUtf8(QJsonDocument(normalized).toJson(QJsonDocument::Compact)));

        auto order = [](const QStringList &list) { return list.join(QLatin1Char(',')); };
        const QStringList pinned = {QStringLiteral("overview"), QStringLiteral("launch")};
        QHash<QString, bool> available;
        available.insert(QStringLiteral("mod"), false);   // 这一页此刻用不上
        QJsonObject overrides;
        overrides.insert(QStringLiteral("save"), false);  // 用户勾掉了
        const QStringList shown = InstanceSettingsPageChoices::visible(pinned, optional, available,
                                                                      overrides);
        check(order(shown) == QStringLiteral("overview,launch,server"),
              "★★ U9d 钉死的两页永远在前；用不上的与勾掉的都不在，顺序照 optionalKeys",
              order(shown));
        check(order(InstanceSettingsPageChoices::visible(pinned, optional, {}, {}))
                  == QStringLiteral("overview,launch,save,server,mod"),
              "★ U9d 没有可用性/勾选信息时：钉死的在前、其余按顺序全显示",
              order(InstanceSettingsPageChoices::visible(pinned, optional, {}, {})));
    }
 return 0;
}




