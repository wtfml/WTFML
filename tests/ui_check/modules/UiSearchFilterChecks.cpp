#include "Support.hpp"

#include "UiInstanceFilesTestHelpers.hpp"

// U10P：顶上那条搜索 + 按钮下面那栏状态。
//
// 要求：「模组等的下载新/刷新/打开文件夹的**上方**加一个搜索，三个按钮的**下方**一个选择
// 全部/启用/禁用/可更新」。这一段量四件事：两排控件在不在、摆的位置对不对（上下是拿
// **y 坐标**量的，不是"我记得写在源码哪一行"）、筛得对不对（搜索 / 状态 / 两样同时）、
// 以及"筛完一条不剩"和"目录本来就是空的"是不是两个状态。
//
// 这一段原来住在 `UiContentPagesChecks.cpp` 里、跟着那一大段共用的 `dialog`（当前页就是
// 这一段挑的）。分出来以后 `dialog` 按引用传；读行里那几格改用 `uiItemText()` /
// `uiItemPath()` / `uiItemFlag()`（`UiInstanceFilesTestHelpers.hpp`）。
int UiSearchFilterChecks(InstanceSettingsDialog &dialog, const QString &modsDir) {
    using Dlg = InstanceSettingsDialog;
    const auto showPage = [](Dlg &d, const QString &key) { d.showPage(key); };
    const auto itemText = [](InstanceFilesPage *page, int row) { return uiItemText(page, row); };
    const auto itemFlag = [](InstanceFilesPage *page, int row, int role) {
        return uiItemFlag(page, row, role);
    };
    const auto itemPath = [](InstanceFilesPage *page, int row) { return uiItemPath(page, row); };
    // ---- U10P 顶上那条搜索 + 按钮下面那栏状态----
    //
    // 要求：「模组等的下载新/刷新/打开文件夹的**上方**加一个搜索，三个按钮的
    // **下方**一个选择 全部/启用/禁用/可更新」。这一段量四件事：两排控件在不在、
    // 摆的位置对不对（上下是拿 **y 坐标**量的，不是"我记得写在源码哪一行"）、
    // 筛得对不对（搜索 / 状态 / 两样同时）、以及"筛完一条不剩"和"目录本来就是空的"
    // 是不是两个状态。
    {
        showPage(dialog, QStringLiteral("mod"));
        InstanceFilesPage *page = dialog.filesPage(QStringLiteral("mod"));
        QLineEdit *search = page != nullptr ? page->searchEdit() : nullptr;
        const auto stateButton = [page](InstanceFilesPage::StateFilter filter) -> QPushButton * {
            return page != nullptr ? page->stateButtonAt(filter) : nullptr;
        };
        QPushButton *state = stateButton(InstanceFilesPage::StateFilter::All);
        check(search != nullptr && state != nullptr &&
                  search->objectName() == InstanceFilesPage::searchEditName() &&
                  state->objectName() == InstanceFilesPage::stateButtonName(
                                            InstanceFilesPage::StateFilter::All) &&
                  search->placeholderText() == InstanceFilesPage::searchPlaceholder(),
              "U10P 模组那一页有一条搜索框、一排状态按钮（名字和那格灰字都走那几个唯一出处）");
        check(page->stateCombo() != nullptr && !page->stateCombo()->isVisibleTo(page) &&
                  page->stateLabel() != nullptr && !page->stateLabel()->isVisibleTo(page) &&
                  page->stateLabel()->objectName() == InstanceFilesPage::stateLabelName() &&
                  page->stateLabel()->text() == InstanceFilesPage::stateLabelText(),
              "★ U10P 开了批量的这一页上**那个下拉框和那个「显示」标签收着**（文字还是走"
              "唯一出处 stateLabelText() —— 没开批量的那几页照样看得到它）");

        // 模组页头（#136 照 PCL）：搜索在最上面（筛选胶囊排得下就同排），常用操作一行在它下面、
        // 列表正上方；旧的目录工具排收掉了。
        const int searchY = search != nullptr ? search->mapTo(page, QPoint(0, 0)).y() : -1;
        auto *headerMore = page->findChild<QPushButton *>(QStringLiteral("modHeaderMore"));
        const int stateY = state != nullptr ? state->mapTo(page, QPoint(0, 0)).y() : -1;
        const int actionsY = headerMore != nullptr ? headerMore->mapTo(page, QPoint(0, 0)).y() : -1;
        const int listY = page->list()->mapTo(page, QPoint(0, 0)).y();
        check(searchY >= 0 && searchY <= stateY && stateY < actionsY && actionsY < listY && headerMore &&
                  headerMore->isVisibleTo(page) && !page->refreshButton()->isVisibleTo(page),
              "★★ U10P 搜索在上、常用操作一行在列表正上方，列表不被顶部工具覆盖",
              QStringLiteral("搜索 y=%1 / 状态 y=%2 / 操作 y=%3 / 列表 y=%4")
                  .arg(searchY)
                  .arg(stateY)
                  .arg(actionsY)
                  .arg(listY));

        // ---- U10P-2 那四颗：字（含条数）、顺序、默认档 ----
        //
        // 顺序走 `selectionActionCount` 那一套的兄弟 —— 四颗是**按下标**摆的
        // （`static_cast<StateFilter>(i)`，见 .cpp 构造那段），所以"第几颗是哪个档"
        // 就是枚举自己的顺序。文字里带**条数**：这一页三条（两条启用、一条禁用、
        // 还没查过更新）⇒ 全部(3) 启用(2) 禁用(1) 可更新(0)。
        QStringList stateTexts;
        for (int i = 0; i < 4; ++i) {
            QPushButton *button =
                stateButton(static_cast<InstanceFilesPage::StateFilter>(i));
            stateTexts.append(button != nullptr ? button->text() : QStringLiteral("<没有>"));
        }
        check(stateTexts ==
                  QStringList({InstanceFilesPage::stateFilterButtonText(
                                   InstanceFilesPage::StateFilter::All, 3),
                               InstanceFilesPage::stateFilterButtonText(
                                   InstanceFilesPage::StateFilter::Enabled, 2),
                               InstanceFilesPage::stateFilterButtonText(
                                   InstanceFilesPage::StateFilter::Disabled, 1),
                               InstanceFilesPage::stateFilterButtonText(
                                   InstanceFilesPage::StateFilter::Update, 0)}),
              "★ U10P 那四颗就是「全部(3) 启用(2) 禁用(1) 可更新(0)」—— 顺序和条数都对"
              "（条数说的是**这个目录里有多少条是这样**，和筛没筛无关；还没查过的时候"
              "那一颗整颗收着，见 U10P-6）",
              stateTexts.join(QStringLiteral(" | ")));
        check(state->isChecked() &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::All &&
                  page->list()->count() == 3,
              "U10P 一进来亮着的是「全部」（三条原样列着，不筛）",
              QStringLiteral("%1 条").arg(page->list()->count()));

        // ---- U10P-3 搜索：子串、不分大小写、盘上那个名字也认 ----
        //
        // 一律走 `setText()`（= 用户敲字那条路，textChanged 立刻生效）—— 直接调
        // setSearchText() 量不到"接没接上"，而那一句才是这一段的重点。
        search->setText(QStringLiteral("sod"));
        check(page->list()->count() == 1 &&
                  itemText(page, 0) == QStringLiteral("sodium.jar"),
              "★ U10P 搜 `sod` 只剩 sodium.jar（子串就中）",
              QStringLiteral("%1 条").arg(page->list()->count()));
        search->setText(QStringLiteral("SOD"));
        check(page->list()->count() == 1,
              "★ U10P 不分大小写（`SOD` 照样中）—— 名字里那句话是怎么写的用户不一定记得");
        search->setText(QStringLiteral(".disabled"));
        check(page->list()->count() == 1 &&
                  itemPath(page, 0) == modsDir + QStringLiteral("/禁用掉的.jar.disabled"),
              "★ U10P 盘上那个名字也认（禁用着的那条盘上叫 `.jar.disabled`，用户手里"
              "那个文件就是这么叫的）",
              itemPath(page, 0));
        search->setText(QStringLiteral("iris"));
        check(page->list()->count() == 1 &&
                  itemPath(page, 0) == modsDir + QStringLiteral("/iris-1.6.5.jar"),
              "★ U10P 每一条搜出来的仍然是**盘上那个真路径**（行是重列出来的，不是藏起来的）");
        search->clear();
        check(page->list()->count() == 3 && page->searchText().isEmpty(),
              "U10P 清掉搜索词，三条都回来");

        // ---- U10P-4 状态：选哪一档就只剩哪一档 ----
        //
        // 走 `setCurrentIndex()`（= 用户点那一栏的路，currentIndexChanged）—— 同上，
        // 直接调 setStateFilter() 量不到"接没接上"。
        // 走 `click()`（= 用户点那一颗的路）—— 直接调 setStateFilter() 量不到"接没接上"。
        const auto selectState = [&stateButton](InstanceFilesPage::StateFilter filter) {
            QPushButton *button = stateButton(filter);
            if (button == nullptr || !button->isEnabled()) {
                return false;
            }
            button->click();
            return true;
        };
        check(selectState(InstanceFilesPage::StateFilter::Disabled) &&
                  page->list()->count() == 1 &&
                  itemFlag(page, 0, InstanceFilesPage::disabledRole()),
              "★ U10P 点「禁用」只剩那一条禁用着的（走角色位判，不看文字）",
              QStringLiteral("%1 条").arg(page->list()->count()));
        check(page->stateFilter() == InstanceFilesPage::StateFilter::Disabled &&
                  stateButton(InstanceFilesPage::StateFilter::Disabled) != nullptr &&
                  stateButton(InstanceFilesPage::StateFilter::Disabled)->isChecked() &&
                  !state->isChecked(),
              "★ U10P 点上那一颗自己就亮起来、原来亮着的那一颗灭掉（四颗是一组单选）");
        check(selectState(InstanceFilesPage::StateFilter::Enabled) &&
                  page->list()->count() == 2,
              "★ U10P 选「启用」剩那两条（`.jar.disabled` 那条不算）",
              QStringLiteral("%1 条").arg(page->list()->count()));
        // 两样**同时**生效（是与关系）：名字里有 iris **而且**是启用着的。
        search->setText(QStringLiteral("iris"));
        check(page->list()->count() == 1 &&
                  itemText(page, 0) == QStringLiteral("iris-1.6.5.jar"),
              "★ U10P 搜索和状态**同时**生效（换一档不把搜索词清掉）",
              QStringLiteral("%1 条").arg(page->list()->count()));
        search->clear();

        // ---- U10P-5 筛完一条不剩 ≠ 目录是空的 ----
        search->setText(QStringLiteral("zzz-一个文件都不叫这个"));
        check(!page->list()->isVisibleTo(page) && page->noticeLabel()->isVisibleTo(page) &&
                  page->noticeLabel()->text() == InstanceFilesPage::noMatchText(),
              "★ U10P 筛完一条不剩：列表收起来、说的是「没有符合条件的条目。」—— 和目录"
              "本来就空那句（「这个文件夹还是空的。」）**不是同一句**",
              page->noticeLabel()->text());
        check(search->isVisibleTo(page) && state->isVisibleTo(page) &&
                  headerMore && headerMore->isVisibleTo(page),
              "★★ U10P 筛选为空时搜索、状态和维护菜单仍可访问，能清除条件和刷新");
        // 清搜索词（状态栏这会儿还停在上面选的「启用」上 ⇒ 回来的是那两条，不是三条 ——
        // 这正是"两样同时生效"的另一面）。
        search->clear();
        check(page->list()->count() == 2 && page->list()->isVisibleTo(page),
              "U10P 搜索词清掉，列表立刻回来（状态栏那一档没跟着被清掉）",
              QStringLiteral("%1 条").arg(page->list()->count()));
        selectState(InstanceFilesPage::StateFilter::All);

        // ---- U10P-6 「可更新」那一颗：靠的是那一趟检测的结果 ----
        //
        // 这一页这会儿是三条：两条启用、一条禁用、**还没查过更新**。
        check(stateButton(InstanceFilesPage::StateFilter::Update) != nullptr &&
                  stateButton(InstanceFilesPage::StateFilter::Update)->isHidden() &&
                  !stateButton(InstanceFilesPage::StateFilter::Disabled)->isHidden(),
              "★★ U10P 还没查过更新时，「可更新」那一颗**整颗收着**（用户原话「没有检查"
              "的时候先隐藏可更新按钮」）—— 旁边「禁用(1)」那颗照旧露着，说明收的是"
              "它自己、不是整排都藏了",
              stateButton(InstanceFilesPage::StateFilter::Update)->text());
        // 于是条件也**落不到**那一档上（`setStateFilter()` 撞上一颗摆不出来的档会把条件
        // 退回「全部」—— 两条路走的是同一个 `stateFilterVisible()`）。
        {
            selectState(InstanceFilesPage::StateFilter::Update);
            check(page->stateFilter() == InstanceFilesPage::StateFilter::All &&
                      page->list()->count() == 3,
                  "★★ U10P 那一颗收着的时候，条件**落不到「可更新」上**（退回「全部」）"
                  "—— 不然屏幕上就是一条都不剩的列表，而两套控件里没有一格亮着",
                  QStringLiteral("%1 条").arg(page->list()->count()));
        }
        check(selectState(InstanceFilesPage::StateFilter::All) &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::All,
              "U10P 收回「全部」上");
        // 喂一份结果：走对话框那条真路（`setFileUpdates` 只递给开得了这一格的那几页）。
        dialog.setFileUpdates({modsDir + QStringLiteral("/sodium.jar")}, true);
        check(!stateButton(InstanceFilesPage::StateFilter::Update)->isHidden() &&
                  stateButton(InstanceFilesPage::StateFilter::Update)->text() ==
                      InstanceFilesPage::stateFilterButtonText(
                          InstanceFilesPage::StateFilter::Update, 1),
              "★ U10P 查出有新版本的那几条之后，那一颗**自己冒出来**、条数就是查出来的条数",
              stateButton(InstanceFilesPage::StateFilter::Update)->text());
        check(selectState(InstanceFilesPage::StateFilter::Update) &&
                  page->list()->count() == 1 &&
                  itemPath(page, 0) == modsDir + QStringLiteral("/sodium.jar"),
              "★ U10P 点上去「可更新」只剩查出来有新版本的那几条",
              QStringLiteral("%1 条").arg(page->list()->count()));
        // 批量更新部分失败（#143）：两条可更新、成了一条 ⇒ 仍停在「可更新」，数量是失败条数。
        dialog.setFileUpdates({modsDir + QStringLiteral("/sodium.jar"),
                               modsDir + QStringLiteral("/iris-1.6.5.jar")}, true);
        const int updatableBefore = page->list()->count();
        dialog.setFileUpdates({modsDir + QStringLiteral("/sodium.jar")}, true);
        check(updatableBefore == 2 &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::Update &&
                  page->list()->count() == 1 &&
                  stateButton(InstanceFilesPage::StateFilter::Update)->text() ==
                      InstanceFilesPage::stateFilterButtonText(
                          InstanceFilesPage::StateFilter::Update, 1),
              "★★ U10P 停在「可更新」时名单减到一条：仍停在这一档，数量跟着减（#143）",
              QStringLiteral("%1 → %2 条").arg(updatableBefore).arg(page->list()->count()));
        // 「一条都不剩」再喂一遍：那一颗**缩回去**，而条件必须**退回「全部」** ——
        // 留着的话屏幕上是一条都不剩的列表，而四颗里没有任何一颗亮着。
        dialog.setFileUpdates({}, true);
        check(stateButton(InstanceFilesPage::StateFilter::Update)->isHidden() &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::All &&
                  page->list()->count() == 3 && page->list()->isVisibleTo(page),
              "★★ U10P 那一份名单变空之后：那颗收起来、条件退回「全部」、三条原样回来"
              "（不是留一个筛空了列表、却没有任何一颗亮着的界面）",
              QStringLiteral("%1 条").arg(page->list()->count()));

        // ---- U10P-7 行里那个 ↑ 和禁用那一条的删除线（★★ ）----
        //
        // 要求：「**可更新的模组在名称后面显示一个上箭头（和名字一个字号），禁用的
        // 模组名字标灰，删除线覆盖**」。
        //
        // 这一页三条：`iris-1.6.5.jar`（启用）、`sodium.jar`（启用）、
        // `禁用掉的.jar.disabled`（禁用）。喂一份名单说 sodium 有新版本 ⇒ 只有它那一行
        // 该长出 ↑，而禁用那一行的名字该是划掉的。
        {
            dialog.setFileUpdates({modsDir + QStringLiteral("/sodium.jar")}, true);
            check(page->list()->count() == 3,
                  "U10P 前提：那份名单回来之后三条**照旧都列着**（↑ 是长在行上的，不筛"
                  "列表 —— 筛是上面那颗「可更新」的事）",
                  QStringLiteral("%1 条").arg(page->list()->count()));
            const auto rowOf = [page, &itemPath](const QString &path) {
                for (int i = 0; i < page->list()->count(); ++i) {
                    if (itemPath(page, i) == path) {
                        return i;
                    }
                }
                return -1;
            };
            const int rowUpdate = rowOf(modsDir + QStringLiteral("/sodium.jar"));
            const int rowPlain = rowOf(modsDir + QStringLiteral("/iris-1.6.5.jar"));
            const int rowOff = rowOf(modsDir + QStringLiteral("/禁用掉的.jar.disabled"));
            check(rowUpdate >= 0 && rowPlain >= 0 && rowOff >= 0,
                  "U10P 前提：三条各按**盘上那个真路径**各找到一行（下面按行取控件）",
                  QStringLiteral("%1 / %2 / %3").arg(rowUpdate).arg(rowPlain).arg(rowOff));
            // 名字那一格：条目上的名字 + 代理画它用的字体 / 墨色（RowDelegate::paint 那一套：
            // 禁用的那条换次要墨、划删除线，别的是正文墨）。
            struct NameCell {
                QString shown;
                bool disabled;
                QPalette palette;
                QString text() const { return shown; }
                bool strikeOut() const { return disabled; }
                QColor ink() const {
                    return palette.color(disabled ? QPalette::PlaceholderText : QPalette::WindowText);
                }
            };
            const auto nameAt = [page](int row) -> std::shared_ptr<NameCell> {
                QListWidgetItem *item = row >= 0 ? page->list()->item(row) : nullptr;
                if (item == nullptr) return nullptr;
                return std::make_shared<NameCell>(
                    NameCell{item->data(Qt::AccessibleTextRole).toString(),
                             item->data(InstanceFilesPage::disabledRole()).toBool(),
                             page->list()->palette()});
            };
            const auto name = nameAt(rowUpdate);
            const auto plainName = nameAt(rowPlain);
            check(name != nullptr && plainName != nullptr &&
                      name->text() == InstanceFilesPage::nameTextWithUpdateMark(
                                          QStringLiteral("sodium.jar"), true) &&
                      plainName->text() ==
                          InstanceFilesPage::nameTextWithUpdateMark(
                              QStringLiteral("iris-1.6.5.jar"), false),
                  "★★ U10P 那个 ↑ **只长在有新版本的那一行上**，而且就是名字那一格**同一串"
                  "字的尾巴**（用户原话「在名称后面显示一个上箭头」+「和名字一个字号」"
                  "—— 同一格同一个字体，「紧挨着」和「一样大」两条都是结构保证的，"
                  "不是调出来的）",
                  QStringLiteral("有新版本那行「%1」/ 没有的那行「%2」")
                      .arg(name != nullptr ? name->text() : QStringLiteral("<null>"),
                           plainName != nullptr ? plainName->text()
                                                : QStringLiteral("<null>")));
            const auto offName = nameAt(rowOff);
            check(offName != nullptr && !offName->text().contains(
                                            InstanceFilesPage::updateArrowText()),
                  "★★ U10P 禁用着的那一条**没有**那个 ↑（这一趟压根不查它 —— "
                  "见 MainWindow::checkInstanceUpdates() 里「禁用着的不查」那条）",
                  offName != nullptr ? offName->text() : QStringLiteral("<null>"));
            const auto colorOf = [](const std::shared_ptr<NameCell> &cell) {
                return cell != nullptr ? cell->ink() : QColor();
            };
            check(offName != nullptr && offName->strikeOut(),
                  "★★ U10P 被禁用那一条的名字**划了一道删除线**（用户原话「禁用的模组名字"
                  "标灰，删除线覆盖」）—— 走的是字体上的 strikeOut，不是画上去的一条线",
                  QStringLiteral("划没划：%1").arg(offName != nullptr
                                                      ? offName->strikeOut()
                                                      : false));
            check(name != nullptr && !name->strikeOut() && offName != nullptr &&
                      colorOf(offName) != colorOf(name),
                  "★★ U10P 而且**只有禁用那一条**名字是灰的、也只有它被划掉（旁边那条"
                  "启用的：正文色、没有删除线）—— 灰走的是和「大小 / 时间」同一档的次要"
                  "色（全仓唯一去处 QPalette::PlaceholderText）",
                  QStringLiteral("启用那条 %1 / 禁用那条 %2")
                      .arg(colorOf(name).name(), colorOf(offName).name()));
            // 再钉一条纯函数上的：没有新版本的行**一个字都不多**（别哪天在那儿也缀上一个
            // 空格 —— 那一格是拿 width 算省略号的，多一个空格就是另一串省略结果）。
            check(InstanceFilesPage::nameTextWithUpdateMark(QStringLiteral("iris-1.6.5.jar"),
                                                           false) ==
                      QStringLiteral("iris-1.6.5.jar"),
                  "★★ U10P 没查出来新版本的那些行原样就是那个名字（唯一出处那边一行都不加）");
            // 喂回去（这一节后面那一段量的另一页，别把这一页留在"查过"那一档上）。
            dialog.setFileUpdates({}, true);
        }

        // ---- U10P-8 同页筛选：**只淡新冒出来的那几行**，整页不重播（★ ）----
        //
        // 要求那一句：「同页筛选/列表变化：只动画**新增**、删除和状态，**不重播整页**」。
        // 这一段的判据全是**结构**上的（不是"看着好像淡了一下"）：
        // / 这一趟真**新冒出来**的行，每一个身上挂着一个效果、而且那一发动画的时长
        // 就是 `rowAppearMs()`（效果挂在行上，动画是效果的子对象 —— 这儿的够法
        // 和 ⑬ 那一段够幕布、够换页动画是同一招：只认公开类型 + `findChildren`）；
        // / 这一趟**本来就在**的行，一个效果都没有 —— 这就是「不重播整页」：旧的那几行
        // 要是也被淡了，屏幕上就是整页在闪；
        // / 整个过程里行和列表的**几何一个像素都不动**。
        //
        // 判据里"哪几行是新的"是**构造出来的**：先筛到一条不剩（`m_shownIdentities`
        // 清空），再筛回来 ⇒ 那三条在重建这一趟里全是新的。反过来，紧接着换到「禁用」
        // 那一档时那一条**本来就在名单里**⇒ 一条都不该淡。
        {
            const bool animWas = Anim::enabled();   // 这个工程平时关着，收尾要摆回去
            Anim::setEnabled(true);
            // 「这一行正在被淡进来吗」：代理按 rowAppearMs() 那一段时间给这一行降不透明度
            // （只淡内容，行底照常），判据就是页面上那份「这一趟新冒出来的身份」。
            // 淡的时候代理只在这一行自己的矩形里画 —— 不会糊出行外面去。
            const auto rowAnimIsFade = [page](int row) { return page->rowFadingIn(row); };
            // 前提：筛到一条不剩（一趟之后"上一趟摆过的身份"就是空的）
            search->setText(QStringLiteral("zzz-一个文件都不叫这个"));
            const bool emptied = page->list()->count() == 0;
            // 再筛回来：三条全是新的 ⇒ 三条都该在淡
            search->clear();
            const bool refilled = page->list()->count() == 3;
            int faded = 0;
            for (int row = 0; row < page->list()->count(); ++row) {
                if (rowAnimIsFade(row)) {
                    ++faded;
                }
            }
            // 这一趟**故意不把事件循环再跑起来**：要求的那两条（"新冒出来的在淡"
            // 和"本来就在的不淡"）都是**接线上**的事，重列那一发是同步的，当场就量得
            // 到。"动画跑起来那一段时间里几何不动"由 ⑬ 那一段自己量（它本来就有一整
            // 段真帧），这一段不必再往这一段 U10 里塞一发事件循环。
            check(emptied && refilled && faded == 3,
                  "★★ U10P 筛到一条不剩再筛回来：重新冒出来的那三条**都**在淡（时长就是 "
                  "`rowAppearMs()`，代理只在那一行自己的矩形里画 —— 用户点名的「不许有元素超出」）",
                  QStringLiteral("筛空%1 / 筛回来%2 条 / 在淡的 %3 条")
                      .arg(emptied ? QStringLiteral("成") : QStringLiteral("没成"))
                      .arg(page->list()->count())
                      .arg(faded));
            // 反面：换一档**不引入任何新行**（那一条本来就在上一趟的名单里）⇒ 一条都不淡。
            // 这一条正是「不重播整页」：列表整个重建过，可屏幕上没有哪一行在闪。
            selectState(InstanceFilesPage::StateFilter::Disabled);
            const bool keptFading = page->list()->count() == 1 && rowAnimIsFade(0);
            check(page->list()->count() == 1 && !keptFading,
                  "★★ U10P 而换一档时**本来就在的那几行一个效果都不挂**（列表照样整个重"
                  "建过，可屏幕上没有哪一行在闪 —— 「不重播整页」量的就是这一条）",
                  QStringLiteral("换档后 %1 条、那一条身上%2")
                      .arg(page->list()->count())
                      .arg(keptFading ? QStringLiteral("在淡") : QStringLiteral("干净")));
            // 收尾把这一页摆回原样（下面那几段量的还是这三条、这一档）：动画开关也要
            // 摆回去 —— 这个工程往后整段都是按"关着"搭界面的。
            selectState(InstanceFilesPage::StateFilter::All);
            search->clear();
            Anim::setEnabled(animWas);
        }

        // ---- U10P-6b 换一页，把「收空档」的另一个方向也钉住 ----
        //
        // 上面那几条量的都是「可更新」那一档；这一条换成**状态**那两档：资源包那一页
        // 两条都是启用着的（一个 zip + 一个目录）、也没有查出来可更新的 ⇒ 「禁用」「可
        // 更新」两颗都收着，而「全部」恒在（用户点名的那半句：「…全部的默认显示」）。
        {
            showPage(dialog, QStringLiteral("resourcepack"));
            InstanceFilesPage *packs = dialog.filesPage(QStringLiteral("resourcepack"));
            const bool shapeOk = packs != nullptr && packs->list()->count() == 2;
            check(shapeOk, "U10P 前提：资源包那一页摆着两条（一个 zip + 一个目录），"
                           "而且都启用着",
                  packs != nullptr ? QStringLiteral("%1 条").arg(packs->list()->count())
                                   : QStringLiteral("<没这一页>"));
            if (shapeOk) {
                check(!packs->stateButtonAt(InstanceFilesPage::StateFilter::All)->isHidden() &&
                          packs->stateButtonAt(InstanceFilesPage::StateFilter::Enabled)
                              ->isHidden() &&
                          packs->stateButtonAt(InstanceFilesPage::StateFilter::Disabled)
                              ->isHidden() &&
                          packs->stateButtonAt(InstanceFilesPage::StateFilter::Update)
                              ->isHidden(),
                      "★★ U10P 一条都没有的那几档（启用 / 禁用 / 可更新）**整颗收着**，"
                      "只剩「全部(2)」—— 「启用」跟着「禁用」收，是因为这一页一条禁用都"
                      "没有（用户点名的那条）");
            }
            // 回到模组那一页（这一段剩下的几条都在它身上量）。
            showPage(dialog, QStringLiteral("mod"));
        }

        // ---- U10P-7 哪几页有「可更新」：判据和「下载新 X」同一张表 ----
        const QStringList withUpdate = {
            QStringLiteral("mod"), QStringLiteral("resourcepack"),
            QStringLiteral("datapack"), QStringLiteral("shader")};
        bool updateOk = true;
        QString updateNote;
        for (const QString &key : Dlg::filePageKeys()) {
            if (key == Dlg::serverPageKey()) {
                continue;
            }
            InstanceFilesPage *other = dialog.filesPage(key);
            if (other == nullptr) {
                updateOk = false;
                updateNote += QStringLiteral("%1=<没这一页> ").arg(key);
                continue;
            }
            // 开了批量的那四页：那一档在不在看 `canFilterUpdates()`；
            // 没开批量的那几页：还是数下拉框里几格（两套控件都留着）。
            const int got = other->batchMode() ? (other->canFilterUpdates() ? 4 : 3)
                                               : (other->stateCombo() != nullptr
                                                      ? other->stateCombo()->count()
                                                      : -1);
            const int want = withUpdate.contains(key) ? 4 : 3;
            if (got != want) {
                updateOk = false;
                updateNote += QStringLiteral("%1=%2 ").arg(key).arg(got);
            }
        }
        check(updateOk,
              "★ U10P 「可更新」只有下载页里真有那一栏的那四页有（模组/资源包/数据包/光影）"
              "—— 原理图 / 存档 / 配置文件那几页上，多一格永远筛不出东西的选项等于摆一个坏键",
              updateNote);
        // 选着「可更新」的时候把那一档拿掉（`setCanFilterUpdates(false)`）：条件必须
        // **退回「全部」** —— 留着的话屏幕上是一条都不剩的列表，而那一栏里没有任何
        // 一格亮着（列表和条件对不上，且没有任何东西会自己纠正它）。
        // 这一段得**先让那一档有内容**（不然它本来就被收着、点不动 ——
        // 收起来的档 `setStateFilter()` 也会落到「全部」，量不到"退档"这一发）。
        dialog.setFileUpdates({modsDir + QStringLiteral("/sodium.jar")}, true);
        check(selectState(InstanceFilesPage::StateFilter::Update) &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::Update,
              "U10P 先让「可更新」有内容、点上去（下面那一发才有得退）");
        page->setCanFilterUpdates(false);
        check(page->stateCombo()->count() == 3 &&
                  stateButton(InstanceFilesPage::StateFilter::Update)->isHidden() &&
                  page->stateFilter() == InstanceFilesPage::StateFilter::All &&
                  page->list()->count() == 3,
              "★★ U10P 把「可更新」那一档拿掉时，正选着它会**退回「全部」**并把列表放回来"
              "（不是留下一个筛空了列表、却没有任何一颗亮着的界面）");
        page->setCanFilterUpdates(true);
        check(!stateButton(InstanceFilesPage::StateFilter::Update)->isHidden() &&
                  page->list()->count() == 3,
              "U10P 再打开：那一档回来、而**条数还在**（那份名单不会因为换了个开关就没了）");
        dialog.setFileUpdates({}, true);
        // 收尾：别把条件留在这一页上给后面那几段用（这一页是这一整段共用的那个 dialog）。
        search->clear();
        selectState(InstanceFilesPage::StateFilter::All);
    }
    return 0;
}

