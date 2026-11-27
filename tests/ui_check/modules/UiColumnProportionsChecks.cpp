#include "Support.hpp"
int UiColumnProportionsChecks(QComboBox * &installed, QPushButton * &startBtn, QListWidget * &folderList, QTreeWidget * &instanceList, QTabWidget * &tabs, const std::function<QString(QWidget *)> &surfaceValueOf, const std::function<bool(QWidget *, const QString &)> &inSurface, const std::function<QFrame *(QWidget *, const QString &)> &surfaceOf) {

    {
        // 份数的唯一出处就是 MainWindowUI 里那几个函数，验证工程拿它们当期望值算 ——
        // 界面和测试各写一份份数的话，改比例时一定会有一边忘了改。
        const int defaultLeftStretch = MainWindowUI::leftColumnStretch();
        const int defaultRightStretch = MainWindowUI::rightColumnStretch();
        check(defaultLeftStretch == 3 && defaultRightStretch == 7,
              "栏**内**那张表格的默认份数是 3:7（设置页每一栏、实例设置里那几张表格）",
              QString("%1:%2").arg(defaultLeftStretch).arg(defaultRightStretch));
        check(MainWindowUI::rightColumnIsWider(), "栏内那张表格里右列更宽");
        // 主页反着来（左宽右窄）。★ ：这一对份数现在只是**入参**了 ——
        // 主页右栏是定宽的（`homeRightColumnWidth()`，见下面那一段量的），
        // 这里只钉"左栏那边仍然标着更宽"这件事，别拿这两个数去算栏宽。
        check(MainWindowUI::leftColumnIsWider(QStringLiteral("home")) &&
                  MainWindowUI::leftColumnStretch(QStringLiteral("home")) == 7 &&
                  MainWindowUI::rightColumnStretch(QStringLiteral("home")) == 3,
              "主页标的是左宽右窄（7:3 那对份数还在，但宽度规则已经是右栏定宽）",
              QString("%1:%2")
                  .arg(MainWindowUI::leftColumnStretch(QStringLiteral("home")))
                  .arg(MainWindowUI::rightColumnStretch(QStringLiteral("home"))));
        // 另三页**按固定宽度分**：左栏是**固定宽度的侧栏**（`sidebarWidth()`，用户那份
        // 文档："侧栏 208 px"），份数给 0 —— 宽度全归正文那一栏。所以这里量的是
        // **真实的栏宽**，不是份数：
        // · 左栏 == sidebarWidth()（一个像素都不多不少）；
        // · 右栏 == 页面宽 - 左栏 - 两条页边距 - 中间那条间距（也就是"剩下的全给它"）。
        check(!MainWindowUI::leftColumnIsWider(QStringLiteral("instance")) &&
                  !MainWindowUI::leftColumnIsWider(QStringLiteral("download")) &&
                  !MainWindowUI::leftColumnIsWider(QStringLiteral("settings")),
              "实例/下载/设置三页都没跟着反过来（左栏是那条窄侧栏）");

        const QStringList tabKeys = {QStringLiteral("home"), QStringLiteral("instance"),
                                     QStringLiteral("download")};  // 设置页是单栏，没有左右两栏
        for (int i = 0; i < tabKeys.size(); ++i) {
            const QString key = tabKeys.at(i);
            const bool home = MainWindowUI::leftColumnIsWider(key);
            tabs->setCurrentIndex(i);
            QApplication::processEvents();
            QWidget *page = tabs->widget(i);
            QWidget *left = page->findChild<QWidget *>(MainWindowUI::leftColumnName(key));
            QWidget *right = page->findChild<QWidget *>(MainWindowUI::rightColumnName(key));
            check(left != nullptr && right != nullptr,
                  QStringLiteral("%1 页有按名字找得到的左右两栏").arg(key));
            if (left == nullptr || right == nullptr) {
                continue;
            }
            // UI 7b：实例页、下载页的左栏收起（文件夹进页头浮层、分类改页头 chip），右栏单独铺满。
            if (!left->isVisibleTo(page)) {
                check((key == QStringLiteral("instance") || key == QStringLiteral("download")) &&
                          right->x() == Theme::pageInsetLeft() &&
                          page->width() - (right->x() + right->width()) == Theme::pageInsetRight(),
                      QStringLiteral("%1 页：左栏收起，右栏单独铺满（左右页边距走整页那一档）").arg(key),
                      QString("%1 vs %2").arg(right->x()).arg(page->width() - right->x() - right->width()));
                continue;
            }
            check(left->x() >= 0 && left->x() + left->width() <= right->x(),
                  QStringLiteral("%1 页：左栏整个在右栏左边").arg(key),
                  QString("left=[%1,%2] right.x=%3")
                      .arg(left->x())
                      .arg(left->x() + left->width())
                      .arg(right->x()));
            if (home) {
                check(left->width() > right->width() &&
                          right->width() == MainWindowUI::homeRightColumnWidth(),
                      QStringLiteral("home 页：右栏钉在人物预览那一栏的宽度上（%1 px），"
                                     "左栏吃剩下的全部")
                          .arg(MainWindowUI::homeRightColumnWidth()),
                      QString("%1 : %2").arg(left->width()).arg(right->width()));
                check(right->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed,
                      "home 页：右栏的横向尺寸策略是 Fixed（宽度定死，不跟窗口长）",
                      QString::number(int(right->sizePolicy().horizontalPolicy())));
            } else {
                check(left->width() == MainWindowUI::sidebarWidth(),
                      QStringLiteral("%1 页：左栏就是那条固定宽度的侧栏（%2 px）")
                          .arg(key)
                          .arg(MainWindowUI::sidebarWidth()),
                      QString("left.width=%1").arg(left->width()));
                // 反面：它**不是**一栏按份数分宽的（改回去会是一条跟着窗口拉伸的窄栏，
                // 拉宽窗口时跟着长 —— 这正是那份文档要修掉的"缺少主次关系"）。
                // 判据取**尺寸策略**：`setFixedWidth()` 之后这一栏的横向策略就是 Fixed，
                // 按份数分宽那一版是 Preferred/Expanding。
                check(left->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed,
                      QStringLiteral("%1 页：左栏的横向尺寸策略是 Fixed（宽度定死，不跟窗口长）")
                          .arg(key),
                      QString::number(int(left->sizePolicy().horizontalPolicy())));
            }
            check(left->x() == page->width() - (right->x() + right->width()),
                  QStringLiteral("%1 页：左右页边距相等（两栏加中间间距正好铺满）").arg(key),
                  QString("%1 vs %2").arg(left->x()).arg(page->width() - right->x() -
                                                         right->width()));
            // 三页（非主页）的左栏那个面就是侧栏那一档（`wtfmlSurface="sidebar"`），
            // 而且那个面**自己不画边框**（用户文档："不画完整矩形边框"）。
            // NOTE: 面是左栏**里面**那张 `QFrame`（见 buildTwoColumnPage() 里 `makeColumn()`
            // 那一段）—— 所以这里问的是"左栏里有没有这么一张面"，不是"左栏自己是不是"。
            if (!home) {
                check(surfaceOf(page, Theme::sidebarSurfaceValue()) != nullptr &&
                          surfaceOf(left, Theme::sidebarSurfaceValue()) != nullptr,
                      QStringLiteral("%1 页：左栏那个面是 `wtfmlSurface=\"sidebar\"`").arg(key),
                      surfaceValueOf(left));
                auto *sideFrame = surfaceOf(page, Theme::sidebarSurfaceValue());
                check(sideFrame != nullptr && sideFrame->frameShape() == QFrame::NoFrame,
                      QStringLiteral("%1 页：侧栏那个面不画边框（只靠底色区分）").arg(key),
                      sideFrame != nullptr ? QString::number(int(sideFrame->frameShape()))
                                           : QStringLiteral("<null>"));
            }
        }

        // 左栏放什么：★ 起主页**不再摆**那颗版本下拉框和那颗大启动按钮 ——
        // 两者都还在（业务层照旧往里灌数据、接信号）但 `hide()` 了。可见的入口换成：
        // 左栏底下那张「当前实例」卡片里的「启动」，以及底部快捷区那颗「列表」。
        tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
        QApplication::processEvents();
        QWidget *homePage = tabs->widget(MainWindowUI::homeTabIndex());
        QWidget *homeLeft =
            homePage->findChild<QWidget *>(MainWindowUI::leftColumnName(QStringLiteral("home")));
        QWidget *homeRight =
            homePage->findChild<QWidget *>(MainWindowUI::rightColumnName(QStringLiteral("home")));
        check(homeLeft != nullptr && isDescendantOf(installed, homeLeft) &&
                  isDescendantOf(startBtn, homeLeft),
              "主页左栏里（还）是版本选择和启动按钮 —— 兼容控件留在那一栏里，只是不摆了");
        check(startBtn != nullptr &&
                  startBtn->property("wtfmlButtonKind").toString() ==
                      Theme::buttonKindKey(Theme::ButtonKind::Primary),
              "主页的「启动游戏」是整页唯一的实心主动作");
        check(startBtn != nullptr && !startBtn->isVisible() &&
                  installed != nullptr && !installed->isVisible(),
              "主页上不再摆它们了：版本下拉框和那颗大启动按钮都是收着的",
              QStringLiteral("combo visible=%1 / 启动按钮 visible=%2")
                  .arg(installed != nullptr ? int(installed->isVisible()) : -1)
                  .arg(startBtn != nullptr ? int(startBtn->isVisible()) : -1));
        // 主页右栏装的是「我的角色」：顶上一张账号卡片 + 3D 小人 +（默认收起来的那行
        // 说明）。这一栏的宽度**不由份数决定** —— 它被 `setFixedWidth()` 钉死（见上面
        // 那一段量的 `homeRightColumnWidth()`），里面那行说明把自己从横向度量里摘出去
        // 只是"顺手"，不再是"这一栏宽度成立的前提"。
        auto *skinViewer =
            homeRight != nullptr
                ? homeRight->findChild<SkinViewer *>(MainWindowUI::skinViewerName())
                : nullptr;
        auto *skinHint =
            homeRight != nullptr
                ? homeRight->findChild<QLabel *>(MainWindowUI::skinHintLabelName())
                : nullptr;
        check(skinViewer != nullptr && skinHint != nullptr,
              "主页右栏里是皮肤预览和它下面那行说明",
              skinViewer == nullptr ? QStringLiteral("<没有 viewer>") : QStringLiteral("<没有说明>"));
        // 兜底那套是界面层喂的：不登录也得有个小人（不是一块空白）。
        check(skinViewer != nullptr && !skinViewer->skin().isNull() &&
                  skinViewer->skin().size() == QSize(64, 64),
              "那个小人开窗就有外观（默认外观兜底，不留白）");
        check(skinHint != nullptr && skinHint->text() == MainWindowUI::skinHintNotLoggedInText(),
              "没登录时那句话说的是默认外观",
              skinHint != nullptr ? skinHint->text() : QStringLiteral("<null>"));

        // 实例页（UI 7b）：文件夹列表在页头胶囊点开的浮层里，实例列表在右栏正文面。
        QWidget *instancePage = tabs->widget(1);
        QWidget *folderPopup = instancePage->findChild<QWidget *>(MainWindowUI::instanceFolderPopupName());
        check(folderPopup != nullptr && instanceList != nullptr && isDescendantOf(folderList, folderPopup) &&
                  !isDescendantOf(instanceList, folderPopup),
              "实例页的文件夹列表在页头浮层里，实例列表不在浮层里");
    }
 return 0;
}


