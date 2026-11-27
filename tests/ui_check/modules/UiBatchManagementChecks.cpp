#include "Support.hpp"
#include "UiInstanceFilesTestHelpers.hpp"

// U10Q：批量管理（多选 / Ctrl+A / 悬浮操作条 / 灰化）。
//
// 要求：「单击改为选中，同时在该界面按 ctrl+a 有全选效果，在选中任意数量模组时在列表
// 下方的位置（**不是表尾而是悬浮**）显示几个按钮：更新 禁用 启用 全选 反选 取消 删除，
// 如果选中的所有 mod 都没有更新 / 都已经启用 / 禁用，则对应的按钮变成灰色不可选状态」。
//
// 这一节量的是**这一层能验的那一半**：开关摆对了没有、选中集怎么攒、条子在不在、
// 三颗的灰化规则算得对不对。**真动盘**那一半（更新 / 禁用 / 删除落到 MainWindow）在
// mw_check 的 T28 一带。
//
// 这一段原来住在 `UiContentPagesChecks.cpp` 里、跟着那一大段的对话框夹具。分出来以后
// 要的三样（两个名字 + 全局那份值）都是**显式形参**，生命周期见 docs/dev/TESTING.md
// 「夹具生命周期（ui_check 的大模块）」。
int UiBatchManagementChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                            const InstanceSettingsDialog::GlobalValues &global) {
    using Dlg = InstanceSettingsDialog;
    const auto showPage = [](Dlg &d, const QString &key) { d.showPage(key); };
    // ---- U10Q 批量管理：多选 / Ctrl+A / 悬浮操作条 / 灰化----
    //
    // 要求：「单击改为选中，同时在该界面按 ctrl+a 有全选效果，在选中任意
    // 数量模组时在列表下方的位置（**不是表尾而是悬浮**）显示几个按钮：更新 禁用 启用 全选
    // 反选 取消 删除，如果选中的所有 mod 都没有更新 / 都已经启用 / 禁用，则对应的按钮变成
    // 灰色不可选状态」。
    //
    // 这一节量的是**这一层能验的那一半**：开关摆对了没有、选中集怎么攒、条子在不在、
    // 三颗的灰化规则算得对不对。**真动盘**那一半（更新 / 禁用 / 删除落到 MainWindow）在
    // mw_check 的 T28 一带。
    {
        QTemporaryDir batchScratch;
        const QString bRoot = batchScratch.path();
        const auto mkfileB = [](const QString &path) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QByteArray(2048, 'z'));
            }
        };
        // 三条：两条启用着、一条禁用着（`.disabled` 后缀就是这个意思，见 disabledSuffix）。
        mkfileB(bRoot + QStringLiteral("/mods/甲.jar"));
        mkfileB(bRoot + QStringLiteral("/mods/乙.jar"));
        mkfileB(bRoot + QStringLiteral("/mods/丙.jar") + InstanceFilesPage::disabledSuffix());
        const QString firstMod = bRoot + QStringLiteral("/mods/甲.jar");
        const QString secondMod = bRoot + QStringLiteral("/mods/乙.jar");
        const QString disabledMod =
            bRoot + QStringLiteral("/mods/丙.jar") + InstanceFilesPage::disabledSuffix();
        // 再摆一个**不开批量**的页面（存档那一页）作对照。
        mkfileB(bRoot + QStringLiteral("/saves/世界甲/level.dat"));
        QHash<QString, QString> bDirs;
        bDirs.insert(QStringLiteral("mod"), bRoot + QStringLiteral("/mods"));
        bDirs.insert(QStringLiteral("save"), bRoot + QStringLiteral("/saves"));

        Dlg batch(instanceDisplayName, instanceDirName, Dlg::Override(), global,
                  QStringList(), true);
        batch.setInstanceDir(bRoot);
        batch.setFileDirs(bDirs);
        batch.showPage(QStringLiteral("mod"));
        batch.show();

        InstanceFilesPage *mods = batch.filesPage(QStringLiteral("mod"));
        check(mods != nullptr && mods->list() != nullptr && mods->batchMode(),
              "U10Q 前提：模组那一页是「开了批量」的那一页");
        if (mods != nullptr && mods->list() != nullptr) {
            QListWidget *list = mods->list();
            const auto rowOf = [list](const QString &path) {
                for (int row = 0; row < list->count(); ++row) {
                    QListWidgetItem *item = list->item(row);
                    if (item != nullptr &&
                        item->data(InstanceFilesPage::pathRole()).toString() == path) {
                        return row;
                    }
                }
                return -1;
            };
            const int modRowA = rowOf(firstMod);
            const int modRowB = rowOf(secondMod);
            const int modRowDisabled = rowOf(disabledMod);
            check(list->count() == 3 && modRowA >= 0 && modRowB >= 0 && modRowDisabled >= 0,
                  "U10Q 前提：模组那一页三条都列出来了（两条启用、一条禁用）",
                  QString::number(list->count()));


            // ---- ①b 单击 = 替换选择（ExtendedSelection：单击一条、Ctrl+单击增减）----
            //
            // 这一发**合成真鼠标事件**（`QApplication::sendEvent` 直接发给 viewport：
            // 行控件是 viewport 的孩子、盖在行上面，走真命中测试的话事件会落到那个
            // `QWidget` 上而不是视图）。
            //
            // NOTE: 拖出 / Ctrl+框选那一套仍在 `InstanceFilesPageEventFilter.cpp`。

            const auto clickRow = [list, &rowOf](const QString &path) {
                const int row = rowOf(path);
                if (row < 0 || list->item(row) == nullptr) {
                    return;
                }
                const QPoint at = list->visualItemRect(list->item(row)).center();
                const QPointF local(at);
                const QPointF global = list->viewport()->mapToGlobal(at);
                QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(list->viewport(), &release);
            };
            list->clearSelection();
            clickRow(firstMod);
            check(list->selectedItems().size() == 1 &&
                      list->selectedItems().first()->data(InstanceFilesPage::pathRole())
                              .toString() == firstMod,
                  "★★ U10Q 点第一发：**这一条选上了**（用户要的第一步）",
                  QString::number(list->selectedItems().size()));
            clickRow(firstMod);
            check(list->selectedItems().isEmpty() && !mods->selectionBar()->isVisible(),
                  "U10Q 再点唯一选中行：取消选择并收起操作面板",
                  QString::number(list->selectedItems().size()));
            clickRow(firstMod);
            clickRow(secondMod);
            check(list->selectedItems().size() == 1 &&
                      list->selectedItems().first()->data(InstanceFilesPage::pathRole())
                              .toString() == secondMod &&
                      mods->selectionCountLabel() != nullptr &&
                      mods->selectionCountLabel()->text() ==
                          InstanceFilesPage::selectionCountText(1),
                  "★★ U10Q 点另一条：**选择换成了这一条**（单击替换），计数写 1",
                  mods->selectionCountLabel() != nullptr ? mods->selectionCountLabel()->text()
                                                         : QString());
            list->clearSelection();

            // ---- ② 什么都没选时那条**不露** ----
            QWidget *bar = mods->selectionBar();
            QLabel *countLabel = mods->selectionCountLabel();
            check(bar != nullptr && countLabel != nullptr,
                  "U10Q 前提：那条悬浮操作条和它左边那句计数都在");
            check(bar != nullptr && !bar->isVisible(),
                  "★★ U10Q 一条都没选中时那条操作条**不露**（凭空压着列表下缘就是错的）");

            // ---- ③ Ctrl+A：合成一发（用户点名的那一条）----
            //
            // NOTE: 这一发走的是 `QAbstractItemView::keyPressEvent()` 里那条
            // `QKeySequence::SelectAll` —— 全仓没有哪一处自己写过 Ctrl+A。
            QKeyEvent selectAllPress(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
            QApplication::sendEvent(list, &selectAllPress);
            check(list->selectedItems().size() == list->count(),
                  "★★ U10Q 在列表里按 Ctrl+A **全选**（用户点名：「按 ctrl+a 有全选效果」）",
                  QStringLiteral("%1/%2").arg(list->selectedItems().size()).arg(list->count()));
            check(bar != nullptr && bar->isVisible() &&
                      countLabel != nullptr &&
                      countLabel->text() == InstanceFilesPage::selectionCountText(list->count()),
                  "★★ U10Q 选中之后那条操作条**露出来**，而且左边那句写的就是选中数",
                  countLabel != nullptr ? countLabel->text() : QString());

            // ---- ④ 灰化规则：拿一次只选一条来逐个量 ----
            //
            // 四条判据（见 .hpp 里 SelectionAction 那段 / .cpp 里 updateSelectionBar）：
            // 更新 = 选中的里**至少一条**在 m_outdatedPaths 里；
            // 禁用 = 至少一条是启用着的；启用 = 至少一条是禁用着的；
            // 全选 / 反选 / 取消 / 删除 恒可用。
            const auto buttonOf = [mods](InstanceFilesPage::SelectionAction action) {
                return mods->selectionButtonAt(static_cast<int>(action));
            };
            const auto selectOnly = [list, &rowOf](const QString &path) {
                list->clearSelection();
                const int row = rowOf(path);
                if (row >= 0) {
                    list->item(row)->setSelected(true);
                }
            };

            // 只选那条**启用着**的、而且没有更新结果 ⇒ 更新灰、禁用亮、启用灰。
            mods->setUpdateResult(QStringList(), false);
            selectOnly(firstMod);
            check(countLabel != nullptr &&
                      countLabel->text() == InstanceFilesPage::selectionCountText(1),
                  "U10Q 只选一条时那句计数写 1",
                  countLabel != nullptr ? countLabel->text() : QString());
            check(buttonOf(InstanceFilesPage::SelectionAction::Update) != nullptr &&
                      !buttonOf(InstanceFilesPage::SelectionAction::Update)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::Disable) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::Disable)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::Enable) != nullptr &&
                      !buttonOf(InstanceFilesPage::SelectionAction::Enable)->isEnabled(),
                  "★★ U10Q 选中的都是启用着的、且都没有更新 ⇒ 「更新」和「启用」是**灰的**、"
                  "「禁用」亮着（用户点名的灰化规则）");

            // 只选那条**禁用着**的 ⇒ 反过来了：启用亮、禁用灰。
            selectOnly(disabledMod);
            check(buttonOf(InstanceFilesPage::SelectionAction::Disable) != nullptr &&
                      !buttonOf(InstanceFilesPage::SelectionAction::Disable)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::Enable) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::Enable)->isEnabled(),
                  "★★ U10Q 选中的是那条**禁用着**的 ⇒ 「启用」亮、「禁用」灰（同一条规则反过来）");

            // 给它一个「这一条有新版」⇒ 「更新」就亮了。
            mods->setUpdateResult({disabledMod}, true);
            selectOnly(disabledMod);
            check(buttonOf(InstanceFilesPage::SelectionAction::Update) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::Update)->isEnabled(),
                  "★★ U10Q 选中的那条被查出来**有新版本** ⇒ 「更新」亮起来（没有更新才是灰的）");
            // 而**没查出来的那一条**选上时「更新」仍旧是灰的（至少一条，不是全部）。
            selectOnly(firstMod);
            check(buttonOf(InstanceFilesPage::SelectionAction::Update) != nullptr &&
                      !buttonOf(InstanceFilesPage::SelectionAction::Update)->isEnabled(),
                  "★★ U10Q 换成那条**没有新版**的 ⇒ 「更新」又灰回去（判据是「选中的里有"
                  "至少一条有更新」）");

            // 全选 / 反选 / 取消 / 删除 四颗**恒可用**（前两颗只动选择集，后两颗按下去
            // 还会问一句 —— 灰着反倒像"删不了"）。
            check(buttonOf(InstanceFilesPage::SelectionAction::SelectAll) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::SelectAll)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::InvertAll) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::InvertAll)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::ClearAll) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::ClearAll)->isEnabled() &&
                      buttonOf(InstanceFilesPage::SelectionAction::Delete) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::Delete)->isEnabled(),
                  "★★ U10Q 全选 / 反选 / 取消 / 删除**恒可用**（它们不该跟着灰化规则走）");

            // 模组页开了「操作面板」那一档（`setActionPanelEnabled(true)`）：
            // 七颗按钮一个都没少（`selectionActionCount()` 仍是 7，选择/灰化规则不变），
            // 但**摆放换了地方**（#136 照 PCL 排到列表上方一行）——
            // / 启用 / 禁用 / 更新 / 删除 / 清除选择在选中后换出来的那条批量操作条里；
            // / 全选 / 反选常驻在这一行右端（批量条外面）
            //   （启用 / 禁用各自按 `selectedTogglePaths()` 计算可用状态 —— 见 U10R）。
            // 判据用 `isAncestorOf()` 而不是 `isVisibleTo()`：后者只顺着**自己**那条
            // 可见链往上走，走到给定控件为止 —— 给它的那个控件根本不是祖先时，它照样
            // 可能回 true（"看不见"这件事它答不出来）。
            const auto inPanel = [bar](QPushButton *button) {
                return button != nullptr && bar->isAncestorOf(button) && button->isVisible();
            };
            const auto outOfPanel = [bar](QPushButton *button) {
                return button != nullptr && !bar->isAncestorOf(button);
            };
            check(InstanceFilesPage::selectionActionCount() == 7 &&
                      inPanel(buttonOf(InstanceFilesPage::SelectionAction::Update)) &&
                      inPanel(buttonOf(InstanceFilesPage::SelectionAction::Delete)),
                  "★★ U10Q 开了操作面板：七颗都还在，「更新」「删除」在批量操作条里",
                  QString::number(InstanceFilesPage::selectionActionCount()));
            check(buttonOf(InstanceFilesPage::SelectionAction::Enable) != nullptr &&
                      buttonOf(InstanceFilesPage::SelectionAction::Disable) != nullptr &&
                      inPanel(buttonOf(InstanceFilesPage::SelectionAction::Enable)) &&
                      inPanel(buttonOf(InstanceFilesPage::SelectionAction::Disable)) &&
                      inPanel(buttonOf(InstanceFilesPage::SelectionAction::ClearAll)) &&
                      outOfPanel(buttonOf(InstanceFilesPage::SelectionAction::SelectAll)) &&
                      outOfPanel(buttonOf(InstanceFilesPage::SelectionAction::InvertAll)),
                  "★★ U10Q 启用/禁用/清除选择在批量操作条里；全选/反选常驻在这一行右端");
            check(mods->findChild<QPushButton *>(QStringLiteral("modStateAction")) == nullptr,
                  "★★ U10Q 面板不再创建启用/禁用菜单");

            // ---- ⑤ 「更新」这一颗按下去报的**是选中那些的路径** ----
            QStringList reportedPaths;
            bool reportedDisable = true;
            const QMetaObject::Connection updConn = QObject::connect(
                mods, &InstanceFilesPage::batchUpdateRequested, mods,
                [&reportedPaths](const QStringList &paths) { reportedPaths = paths; });
            mods->setUpdateResult({firstMod}, true);
            selectOnly(firstMod);
            list->item(rowOf(secondMod))->setSelected(true);
            if (buttonOf(InstanceFilesPage::SelectionAction::Update) != nullptr) {
                buttonOf(InstanceFilesPage::SelectionAction::Update)->click();
            }
            check(reportedPaths.size() == 2 && reportedPaths.contains(firstMod) &&
                      reportedPaths.contains(secondMod),
                  "★★ U10Q 点「更新」报的是**选中的那一批路径**（一条一条走，不是只报第一行）",
                  reportedPaths.join(QStringLiteral(" | ")));
            QObject::disconnect(updConn);

            const QMetaObject::Connection togConn = QObject::connect(
                mods, &InstanceFilesPage::batchToggleRequested, mods,
                [&reportedPaths, &reportedDisable](const QStringList &paths, bool disable) {
                    reportedPaths = paths;
                    reportedDisable = disable;
                });
            reportedPaths.clear();
            selectOnly(firstMod);
            if (buttonOf(InstanceFilesPage::SelectionAction::Disable) != nullptr) {
                buttonOf(InstanceFilesPage::SelectionAction::Disable)->click();
            }
            check(reportedPaths == QStringList{firstMod} && reportedDisable,
                  "★★ U10Q 点「禁用」报的是(路径, disable=true)（「启用」那一颗是 false）",
                  reportedPaths.join(QStringLiteral(" | ")));
            QObject::disconnect(togConn);

            // ---- ⑥ 「全选」/「取消」两颗自己动选择集（用户点名那两颗）----
            list->clearSelection();
            if (buttonOf(InstanceFilesPage::SelectionAction::SelectAll) != nullptr) {
                buttonOf(InstanceFilesPage::SelectionAction::SelectAll)->click();
            }
            check(list->selectedItems().size() == list->count(),
                  "U10Q 点「全选」= 现在列出来的那些全选上",
                  QString::number(list->selectedItems().size()));
            // 「反选」= 现在列出来的那些里，没选的选上、选着的取消。全选之后再按一下
            // 必然是**一条都不剩** —— 只把"选着的取消"做了一半的实现会在这儿露出来。
            if (buttonOf(InstanceFilesPage::SelectionAction::InvertAll) != nullptr) {
                buttonOf(InstanceFilesPage::SelectionAction::InvertAll)->click();
            }
            check(list->selectedItems().isEmpty(),
                  "★★ U10Q 全选之后点「反选」⇒ 一条都不剩（每一条都真的翻了个面）",
                  QString::number(list->selectedItems().size()));
            if (buttonOf(InstanceFilesPage::SelectionAction::ClearAll) != nullptr) {
                buttonOf(InstanceFilesPage::SelectionAction::ClearAll)->click();
            }
            check(list->selectedItems().isEmpty() && bar != nullptr && !bar->isVisible(),
                  "★★ U10Q 点「取消」清空选择 ⇒ 那条操作条跟着收回去");

            // ---- ⑦ 行尾那颗「详情」：认得出资源的那一条亮着，认不出的那一条灰着 ----
            //
            // 要求：「把点击条目进入对应详情页的功能变成一个详情按钮，
            // 放到开关和删除中间」。「认不认得出」是这一格能不能点的**唯一**判据（见
            // setResourceForPath 那段：在 m_resourceTips 里 = 认出来了）。这一节只喂
            // 一条手工的账；真去问 Modrinth、真开那一页在 mw_check / 真机那两处。
            check(uiDetailAt(mods, rowOf(firstMod)) != nullptr &&
                      !uiDetailAt(mods, rowOf(firstMod))->isEnabled(),
                  "★★ U10Q 没认出来的那一行，那颗「详情」是**灰的**（认不出是哪个资源，"
                  "点进去只会是一张空页）");
            check(mods->setResourceForPath(firstMod, QStringLiteral("测试资源")) &&
                      uiDetailAt(mods, rowOf(firstMod)) != nullptr &&
                      uiDetailAt(mods, rowOf(firstMod))->isEnabled(),
                  "★★ U10Q 认出来的那一行那颗「详情」**亮起来**（setResourceForPath 的"
                  "返回值就是「认没认出来」这件事本身）");

            // ---- ⑧ refresh() 之后选择**按路径保住**（ 真机揪出来的缺陷） ----
            //
            // 症状：在真机上点「启用」把一条改回来之后，那一行**没选上**、底下那条操作条
            // 也一起消失。根子 = `refresh()` 里那句 `m_list->clear()` **没挡信号**，而
            // 它接的是「按当前选中重算 m_selectedPaths」—— 此刻表已经空了，于是那份账在
            // **自己的重列过程中**被抹平；紧接着 rebuildRows() 想按路径把选择还回去，
            // 手里已经什么都没有了。
            // 这正是 `wtfml-comment-vs-code` 那一类：rebuildRows() 的注释声称"重列
            // 这一步防住了"，漏的却是它外面那一发 clear()。两处都得挡。
            mods->setUpdateResult(QStringList(), false);
            selectOnly(secondMod);
            check(list->selectedItems().size() == 1, "U10Q 前提：先选中一条");
            mods->refresh();
            check(list->selectedItems().size() == 1 &&
                      list->selectedItems().first()->data(InstanceFilesPage::pathRole())
                              .toString() == secondMod &&
                      countLabel != nullptr && bar != nullptr && bar->isVisible() &&
                      countLabel->text() == InstanceFilesPage::selectionCountText(1),
                  "★★ U10Q 走过一趟 `refresh()`（改名 / 刷新 / 筛一下都必经它）之后，"
                  "选择**按路径还在**、那条操作条也还露着",
                  QStringLiteral("refresh 之后选中 %1 条")
                      .arg(list->selectedItems().size()));

            // ---- ⑨ 不开批量的那一页：**单选** + 没有详情那一格 + 仍旧是下拉框 ----
            batch.showPage(QStringLiteral("save"));
            InstanceFilesPage *saves = batch.filesPage(QStringLiteral("save"));
            check(saves != nullptr && saves->list() != nullptr && !saves->batchMode(),
                  "U10Q 前提：存档那一页是**没开批量**的那一页");
            if (saves != nullptr && saves->list() != nullptr) {
                check(saves->list()->selectionMode() == QAbstractItemView::ExtendedSelection,
                      "★★ U10Q 存档页自己的批量工具条开了**多选**（批量管理那一套仍只铺那四页）",
                      QString::number(static_cast<int>(saves->list()->selectionMode())));
                check(saves->list()->count() > 0 ? uiDetailAt(saves, 0) == nullptr : true,
                      "★★ U10Q 没开批量的页上**没有**那颗「详情」（它只在开了批量的页上建）");
                check(saves->stateCombo() != nullptr && saves->stateButtonAt(
                          InstanceFilesPage::StateFilter::All) != nullptr &&
                          !saves->stateCombo()->isVisibleTo(saves) &&
                          !saves->stateButtonAt(InstanceFilesPage::StateFilter::All)
                               ->isVisibleTo(saves),
                      "U10Q 存档页不显示启用/禁用筛选，也不显示资源批量筛选按钮");
                check(saves->selectionBar() != nullptr &&
                          !saves->selectionBar()->isVisible(),
                      "★★ U10Q 没开批量的页上那条操作条**不露**（哪怕把行选上）");
            }
        }

        // ---- ⑩ 那条操作条**滚轮搬不动它**（ 用户报的那个）----
        //
        // 要求：「选中以后出现下方框，但是滚动滚轮以后框会跟着跑了」。
        // 根子在 Qt：`QAbstractItemViewPrivate::scrollContentsBy()` 滚的是
        // `viewport->scroll(dx, dy)`，而 `QWidget::scroll()` 是**连孩子一起挪**的
        // （`QWidgetPrivate::scroll_sys()` 第一句 `scrollChildren(dx, dy)`）。所以只要
        // 这条浮层是 **viewport 的孩子**，它就会跟着滚。产品那边的修法是**换爹**：
        // 挂到 `m_list` 底下（viewport 的**兄弟**），于是被滚的那一棵里没有它。
        //
        // 这一节量两件事，**顺序不能反**：
        // ① 结构：`parentWidget() == list`（**不是 viewport**）—— 这是根因那一句；
        // ② 行为：滚到底之后 `geometry()` 一个像素都没动 —— 这是用户看见的那一句。
        // 两条都要，因为①可能被"再加一层中间容器"改对而②仍旧对，反过来②也可能因为
        // 产品在 scroll 之后顺手 reposition 一次而假绿（那样"滚轮一挪它就跳回原位"
        // 看不出区别，但下一帧还是会抖）。
        {
            QTemporaryDir scrollScratch;
            const QString sRoot = scrollScratch.path();
            const auto mkfileS = [](const QString &path) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile file(path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(QByteArray(64, 'w'));
                }
            };
            // 够多行，才滚得动（每行 46，视口就那么高，给 60 条稳稳溢出）。
            //
            // NOTE: 这一节**故意用光影页**，不用模组页。模组页开了「操作面板」那一档
            // （setActionPanelEnabled(true)，见下面的 U10R 那一批），操作条被换爹到
            // 列表上方那一行里；而这一节要量的正是**没换爹**的那个形状 ——
            // 「浮在列表下缘、挂 list 底下、滚轮搬不动」。光影页是同一套批量管理，
            // 没有面板那一档，正好是这条判据该落的地方。
            for (int i = 0; i < 60; ++i) {
                mkfileS(sRoot +
                        QStringLiteral("/shaderpacks/光影%1.zip").arg(i, 3, 10, QLatin1Char('0')));
            }
            QHash<QString, QString> sDirs;
            sDirs.insert(QStringLiteral("shader"), sRoot + QStringLiteral("/shaderpacks"));
            Dlg sc(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(),
                   true);
            sc.setInstanceDir(sRoot);
            sc.setFileDirs(sDirs);
            sc.resize(900, 620);
            sc.showPage(QStringLiteral("shader"));
            sc.show();
            InstanceFilesPage *sp = sc.filesPage(QStringLiteral("shader"));
            check(sp != nullptr && sp->list() != nullptr && sp->selectionBar() != nullptr,
                  "U10Q 前提：滚轮那一节的页 / 列表 / 那条操作条都在");
            if (sp != nullptr && sp->list() != nullptr && sp->selectionBar() != nullptr) {
                QListWidget *slist = sp->list();
                QWidget *sbar = sp->selectionBar();
                check(slist->count() == 60 && slist->verticalScrollBar() != nullptr &&
                          slist->verticalScrollBar()->maximum() > 0,
                      "U10Q 前提：这一页有 60 条、而且**真的滚得动**"
                      "（滚不动的话下面那两条是空转的）",
                      QStringLiteral("count=%1 max=%2")
                          .arg(slist->count())
                          .arg(slist->verticalScrollBar() != nullptr
                                   ? slist->verticalScrollBar()->maximum()
                                   : -1));
                // ① 结构那一句：**爹是视图本身，不是 viewport**。
                check(sbar->parentWidget() == slist &&
                          sbar->parentWidget() != slist->viewport(),
                      "★★ U10Q 那条操作条挂在 **`list` 底下**（不是 viewport）—— "
                      "这就是「滚轮搬不动它」的根因那一句（viewport 的孩子会被 "
                      "`viewport->scroll()` 连人一起挪，见 qwidget.cpp 的 scroll_sys）",
                      sbar->parentWidget() != nullptr
                          ? sbar->parentWidget()->objectName()
                          : QStringLiteral("<null>"));
                // 选一条 ⇒ 它露出来、位置算好。
                slist->item(0)->setSelected(true);
                check(sbar->isVisible(),
                      "U10Q 前提：选中一条之后那条操作条露出来了");
                const QRect before = sp->selectionBarGeometry();
                check(before.isValid() && before.height() > 0,
                      "U10Q 前提：那条操作条这会儿有真的大小（0×0 的话下面量不出动静）",
                      QStringLiteral("%1,%2 %3×%4")
                          .arg(before.x())
                          .arg(before.y())
                          .arg(before.width())
                          .arg(before.height()));
                // ② 滚到底。
                slist->verticalScrollBar()->setValue(slist->verticalScrollBar()->maximum());
                const QRect after = sp->selectionBarGeometry();
                check(after == before,
                      "★★ U10Q 滚轮滚到底：那条操作条的 `geometry()` **一个像素都没动**"
                      "（用户报的「滚动滚轮以后框会跟着跑了」）",
                      QStringLiteral("%1,%2 → %3,%4")
                          .arg(before.x())
                          .arg(before.y())
                          .arg(after.x())
                          .arg(after.y()));
                // 而且它仍旧贴着**视口**下缘居中（换爹之后坐标要多加 viewport->pos()，
                // 加错的话它整条会歪到视图左上角去）。
                QWidget *sviewport = slist->viewport();
                const int expectedLeft =
                    sviewport != nullptr ? sviewport->pos().x() +
                                               (sviewport->width() - after.width()) / 2
                                         : after.x();
                const int expectedTop = sviewport != nullptr
                                            ? sviewport->pos().y() + sviewport->height() -
                                                  after.height() - 12  // kSelectionBarMargin
                                            : after.y();
                check(sviewport != nullptr && after.x() == expectedLeft &&
                          after.y() == expectedTop,
                      "★★ U10Q 而且它**仍旧贴着视口下缘、横向居中**（换爹之后那笔"
                      "`viewport->pos()` 加错了就会整条歪到左上角）",
                      QStringLiteral("x=%1(期望%2) y=%3(期望%4)")
                          .arg(after.x())
                          .arg(expectedLeft)
                          .arg(after.y())
                          .arg(expectedTop));
            }
        }
    }
    // ---------- 大列表：1000 条首绘 / 滚到底 ----------
    {
        QTemporaryDir bigScratch;
        const QString bigRoot = bigScratch.path();
        QDir().mkpath(bigRoot + QStringLiteral("/mods"));
        for (int i = 0; i < 1000; ++i) {
            QFile file(bigRoot + QStringLiteral("/mods/模组%1.jar").arg(i, 4, 10, QLatin1Char('0')));
            if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(32, 'a'));
        }
        QHash<QString, QString> bigDirs;
        bigDirs.insert(QStringLiteral("mod"), bigRoot + QStringLiteral("/mods"));
        Dlg big(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(), true);
        big.setFileDirs(bigDirs);
        QElapsedTimer clock;
        clock.start();
        big.showPage(QStringLiteral("mod"));
        big.show();
        InstanceFilesPage *page = big.filesPage(QStringLiteral("mod"));
        QListWidget *list = page != nullptr ? page->list() : nullptr;
        while (list != nullptr && list->count() < 1000 && clock.elapsed() < 10000)
            QApplication::processEvents();
        const qint64 firstPaint = clock.elapsed();
        check(list != nullptr && list->count() == 1000,
              "大列表首绘：1000 条都建出来了",
              list != nullptr ? QString::number(list->count()) : QStringLiteral("无列表"));
        check(firstPaint < 10000, "1000 条文件首绘在 10 秒内", QString::number(firstPaint));
        clock.restart();
        if (list != nullptr) {
            list->scrollToBottom();
            QApplication::processEvents();
        }
        // 滚到底：行是代理画的，没有行控件要补、要收 —— 最后一行直接就在视口里。
        if (list != nullptr && list->verticalScrollBar() != nullptr) {
            list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
            QApplication::processEvents();
        }
        const qint64 scrollMs = clock.elapsed();
        check(list != nullptr && list->count() == 1000 &&
                  list->visualItemRect(list->item(list->count() - 1))
                      .intersects(list->viewport()->rect()),
              "滚到底后最后一行就在视口里（代理画，不用等行控件补上来）");
        check(list != nullptr &&
                  list->viewport()->findChildren<QWidget *>().size() < 10,
              "★ 1000 条文件：视口里**没有**一行一个的控件（每一行都是代理画的）",
              list != nullptr ? QString::number(list->viewport()->findChildren<QWidget *>().size())
                              : QStringLiteral("无列表"));
        check(scrollMs < 3000, "1000 条文件滚到底在 3 秒内", QString::number(scrollMs));
        qInfo() << "[perf] large file list: first paint" << firstPaint << "ms, scroll-to-bottom" << scrollMs << "ms";

    }

    return 0;
}

