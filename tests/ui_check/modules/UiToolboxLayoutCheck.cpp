// 工具箱版式验收：目录页 → 工具页两层、面包屑与切换浮层、十个工具页在 最小窗口 / 1280×800 / 窄内容宽
// 下都不出横向滚动条、不被裁；附魔行四个框同高、窄时折成两行。
// 设环境变量 WTFML_TOOLBOX_SHOTS=<目录> 时另外出验收图（深色 / 青瓷 × 两种窗口尺寸）。不弹真窗口。
#include "Support.hpp"
#include "MainWindowPageLayout.hpp"
#include "DownloadToolPanel.hpp"
#include "FlowLayout.hpp"
#include "GlassPopover.hpp"
#include "ToolboxSamples.hpp"

#include <QDir>
#include <QFontDatabase>
#include <QMainWindow>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>

namespace {

/// 页里所有可见子控件都落在 @p panel 的宽度之内（没有被右侧裁掉）。
bool noChildClipped(QWidget *panel, QString *why) {
    for (QWidget *child : panel->findChildren<QWidget *>()) {
        if (!child->isVisibleTo(panel) || child->width() <= 0) continue;
        if (child->objectName().startsWith(QStringLiteral("qt_"))) continue;
        const QPoint topLeft = child->mapTo(panel, QPoint(0, 0));
        if (topLeft.x() + child->width() > panel->width() + 1 || topLeft.x() < -1) {
            if (why != nullptr)
                *why = QStringLiteral("%1/%2 超出 %3 > %4").arg(child->metaObject()->className(), child->objectName())
                           .arg(topLeft.x() + child->width()).arg(panel->width());
            return false;
        }
    }
    return true;
}

/// 一件控件**画出来之后**，那一行字离左、右两边各多少逻辑像素（取中线上下四分之一带里
/// 第一个和最后一个墨点）。判据落在像素上而不是 `alignment()` 上：要守的是"看起来居中"，
/// 设了哪个标志是手段，不是结论。
///
/// NOTE: 图像取**整窗** `grab()` 再裁这块，不取控件自己 —— 这块的底是半透明的（主题给的是
/// 带 alpha 的 fill），单独 grab 出来合成在"什么都没有"上，深浅两套主题下都变成一片白，
/// 字和底分不开（实测 dark 下最大色差只有 21 级）。整窗那张才是屏幕上真实的那一张。
bool textSideGaps(QWidget *widget, QWidget *root, int *leftGap, int *rightGap) {
    const qreal dpr = widget->devicePixelRatioF() > 0 ? widget->devicePixelRatioF() : 1.0;
    const QPoint origin = widget->mapTo(root, QPoint(0, 0));
    const QRect rect = QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
                             qRound(widget->width() * dpr), qRound(widget->height() * dpr)).adjusted(-1, -1, 1, 1);
    const QImage image = root->grab(rect).toImage().convertToFormat(QImage::Format_RGB32);
    if (image.isNull() || image.width() < 32 || image.height() < 12) return false;
    const int mid = image.height() / 2;
    const int band = qMax(2, image.height() / 4);
    const QRgb fill = image.pixel(4, mid);  // 四边是 3px 透明边，从 4 起落在自己的底上
    const auto lum = [](QRgb c) { return 299 * qRed(c) + 587 * qGreen(c) + 114 * qBlue(c); };
    int first = -1;
    int last = -1;
    for (int x = 4; x < image.width() - 4; ++x) {
        bool ink = false;
        for (int y = mid - band; y <= mid + band && !ink; ++y)
            ink = qAbs(lum(image.pixel(x, y)) - lum(fill)) > 40 * 1000;
        if (!ink) continue;
        if (first < 0) first = x;
        last = x;
    }
    if (first < 0) return false;
    *leftGap = qRound(first / dpr);
    *rightGap = qRound((image.width() - 1 - last) / dpr);
    return true;
}

/// FlowLayout 的伸缩项与靠右：一行放得下时同带 stretch 的横排，放不下折行。
void checkFlowLayout() {
    QWidget host;
    auto *flow = new FlowLayout(8, 8);
    host.setLayout(flow);
    auto *left = new QWidget(&host);
    auto *right = new QWidget(&host);
    left->setFixedSize(100, 30);
    right->setFixedSize(100, 20);
    flow->addWidget(left);
    flow->addStretch();
    flow->addWidget(right);
    flow->setGeometry(QRect(0, 0, 400, 100));
    check(left->pos() == QPoint(0, 0) && right->pos() == QPoint(300, 5), "折行布局：伸缩项把后面的顶到右边、行内居中");
    flow->setGeometry(QRect(0, 0, 150, 100));
    check(right->pos() == QPoint(0, 38) && flow->heightForWidth(150) == 58, "折行布局：放不下就折到下一行");
    flow->alignRight();
    flow->setGeometry(QRect(0, 0, 151, 100));
    check(right->x() == 51, "折行布局：靠右时折下去的行仍靠右", QString::number(right->x()));
}

}  // namespace

int UiToolboxLayoutCheck(QApplication &app) {
    Anim::setEnabled(false);
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".toolbox-layout-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLToolboxLayout"));
    app.setApplicationName(QStringLiteral("ToolboxLayout"));
    app.setStyle(Theme::createStyle());
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));

    checkFlowLayout();
    const QString shotDir = qEnvironmentVariable("WTFML_TOOLBOX_SHOTS");
    if (!shotDir.isEmpty()) QDir().mkpath(shotDir);
    int saved = 0;
    const auto pump = [&app] {
        app.processEvents();
        app.processEvents();
    };

    for (const QString &id : {QStringLiteral("dark"), QStringLiteral("emerald")}) {
        Theme::setSpec(Theme::presetSpec(id));
        Theme::apply(app);
        QMainWindow window;
        MainWindowUI ui(&window);
        window.show();
        QTabWidget *tabs = ui.getTabWidget();
        check(tabs->count() == 6 && MainWindowUI::toolboxTabIndex() == 5 &&
                  tabs->tabText(MainWindowUI::toolboxTabIndex()) == QStringLiteral("工具箱") &&
                  tabs->widget(MainWindowUI::toolboxTabIndex())->objectName() == MainWindowUI::toolboxTabName(),
              "工具箱是第六个选项卡");
        check(ui.titleBar()->navCount() == 5, "顶上导航仍是五颗，工具箱不上导航", QString::number(ui.titleBar()->navCount()));
        QWidget *host = ui.toolboxTab();
        auto *page = new ToolboxPage(host);
        host->layout()->addWidget(page);
        wireToolboxSamples(*page);
        check(ui.getToolboxButton() != nullptr && !ui.getToolboxButton()->isChecked(), "不在工具箱页时导航轨底部那颗不高亮");
        ui.openToolboxTab();  // 没装 handler：只切选项卡
        check(tabs->currentIndex() == MainWindowUI::toolboxTabIndex() && ui.getToolboxButton()->isChecked(),
              "点「工具箱」直接进工具箱页并高亮，没有弹出框");
        check(window.findChild<QFrame *>(QStringLiteral("toolboxPanel")) == nullptr &&
                  window.findChild<QPushButton *>(QStringLiteral("toolboxExpandButton")) == nullptr,
              "没有工具箱弹出框，也没有「全部展开为页面」");

        for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)}) {
            window.resize(size);
            pump();
            const QString sizeText = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
            const auto shot = [&](const QString &name) {
                pump();
                if (shotDir.isEmpty()) return;
                const QString file = QDir(shotDir).filePath(QStringLiteral("toolbox-%1-%2-%3.png").arg(name, id, sizeText));
                if (window.grab().save(file)) ++saved;
            };

            // ---- 目录页 ----
            page->showCatalog();
            pump();
            check(page->inCatalog(), "目录页：停在目录");
            if (auto *catalog = page->findChild<QScrollArea *>(ToolboxPage::catalogName());
                catalog != nullptr && size == QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight)) {
                check(catalog->verticalScrollBar()->maximum() == 0 && catalog->horizontalScrollBar()->maximum() == 0,
                      QStringLiteral("目录页 %1：没有滚动范围").arg(sizeText),
                      QStringLiteral("纵 %1 横 %2").arg(catalog->verticalScrollBar()->maximum()).arg(catalog->horizontalScrollBar()->maximum()));
            }
            int cells = 0;
            QList<int> xs;
            for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
                auto *cell = page->findChild<QPushButton *>(ToolboxPage::toolCellName(tool));
                if (cell != nullptr && cell->isVisible()) {
                    ++cells;
                    if (!xs.contains(cell->mapTo(page, QPoint(0, 0)).x())) xs.append(cell->mapTo(page, QPoint(0, 0)).x());
                    check(cell->width() >= 240 - 1 || xs.size() == 1, "目录格不窄于 240", QString::number(cell->width()));
                }
            }
            check(cells == 9, "目录页：九个工具一格不少", QString::number(cells));
            check(xs.size() >= 2 && xs.size() <= 4, "目录页：自适应成多列", QString::number(xs.size()));
            check(page->findChild<QCheckBox *>(QStringLiteral("toolboxRememberInputs")) != nullptr &&
                      page->findChild<QCheckBox *>(QStringLiteral("toolboxRememberInputs"))->isVisible(),
                  "目录页右上有「记住输入」");
            check(!page->findChild<QPushButton *>(ToolboxPage::crumbRootName())->isVisible(), "目录页没有面包屑");
            shot(QStringLiteral("cat"));

            // ---- 十个工具页 ----
            for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
                auto *cell = page->findChild<QPushButton *>(ToolboxPage::toolCellName(tool));
                if (cell != nullptr) cell->click();
                else page->showTool(tool);
                pump();
                const QString key = ToolboxPage::toolKey(tool);
                check(!page->inCatalog() && page->currentTool() == tool, "点目录格进工具页", key);
                auto *crumbTool = page->findChild<QPushButton *>(ToolboxPage::crumbToolName());
                auto *crumbRoot = page->findChild<QPushButton *>(ToolboxPage::crumbRootName());
                check(crumbTool != nullptr && crumbTool->text() == ToolboxPage::toolName(tool) && crumbRoot != nullptr &&
                          crumbRoot->isVisible(),
                      "面包屑「工具箱 / 工具名」", key);
                // 面包屑文字（按钮左内边距 8）与工具内容左缘对齐。
                if (crumbRoot != nullptr && crumbRoot->isVisible()) {
                    auto *scrollArea = page->findChild<QScrollArea *>(ToolboxPage::toolPageName(tool));
                    QWidget *content = page->findChild<QWidget *>(ToolboxPage::toolPanelName(tool));
                    if (scrollArea != nullptr && content != nullptr) {
                        const int textLeft = crumbRoot->mapTo(page, QPoint(0, 0)).x() + 8;
                        const int contentLeft = content->mapTo(page, QPoint(0, 0)).x();
                        check(qAbs(textLeft - contentLeft) <= 1, "面包屑文字与内容左缘对齐", QStringLiteral("%1 vs %2 %3 | crumbRoot.x=%4 head.x=%5 inner.x=%6").arg(textLeft).arg(contentLeft).arg(key).arg(crumbRoot->mapTo(page, QPoint(0,0)).x()).arg(crumbRoot->parentWidget()->mapTo(page, QPoint(0,0)).x()).arg(crumbRoot->parentWidget()->parentWidget()->mapTo(page, QPoint(0,0)).x()));
                    }
                }
                auto *scroll = page->findChild<QScrollArea *>(ToolboxPage::toolPageName(tool));
                check(scroll != nullptr && scroll->isVisible(), "工具页可见", key);
                if (tool == ToolboxPage::Tool::Banner) {
                    // 走查 j05：说明句原先与「加载贴图…」并排，可用宽只剩 252，四十个字折成三行、
                    // 按钮悬在折行文字右边。判据落在**画出来之后**的几何上：说明句占满卡宽、
                    // 一两行读完，按钮在它下面另起一行、左对齐。
                    auto *notice = page->findChild<QLabel *>(QStringLiteral("bannerVisualNotice"));
                    auto *loadButton = page->findChild<QPushButton *>(QStringLiteral("bannerLoadTextures"));
                    check(notice != nullptr && loadButton != nullptr && notice->parentWidget() == loadButton->parentWidget(),
                          "旗帜页：说明句与「加载贴图…」在同一张图层卡里", sizeText);
                    if (notice != nullptr && loadButton != nullptr && notice->parentWidget() == loadButton->parentWidget()) {
                        QWidget *card = notice->parentWidget();
                        const QMargins margins = card->layout()->contentsMargins();
                        const int inner = card->width() - margins.left() - margins.right();
                        const int lines = notice->height() / qMax(1, notice->fontMetrics().lineSpacing());
                        const QPoint noticeAt = notice->mapTo(card, QPoint(0, 0));
                        const QPoint buttonAt = loadButton->mapTo(card, QPoint(0, 0));
                        check(notice->width() * 5 >= inner * 4,
                              QStringLiteral("旗帜页 %1：说明句占满卡宽（≥ 80%）").arg(sizeText),
                              QStringLiteral("说明句 %1 / 卡内宽 %2").arg(notice->width()).arg(inner));
                        check(lines <= 2, QStringLiteral("旗帜页 %1：说明句一两行读完").arg(sizeText),
                              QStringLiteral("%1 行，宽 %2").arg(lines).arg(notice->width()));
                        check(buttonAt.x() == noticeAt.x() && buttonAt.y() >= noticeAt.y() + notice->height(),
                              QStringLiteral("旗帜页 %1：「加载贴图…」在说明句下方的左端").arg(sizeText),
                              QStringLiteral("按钮 (%1,%2) 说明句 (%3,%4) 高 %5")
                                  .arg(buttonAt.x()).arg(buttonAt.y()).arg(noticeAt.x()).arg(noticeAt.y()).arg(notice->height()));
                    }
                }
                if (tool == ToolboxPage::Tool::Download) {
                    // 走查 j07：数字框里那行字右对齐，33px 余量全落在左边（看着像左空 48、右空 15）。
                    // 判据量的是 grab() 出来的像素：文字的左右留白差 ≤ 4px。
                    auto *limit = page->findChild<QSpinBox *>(QStringLiteral("downloadCacheLimit"));
                    auto *trimButton = page->findChild<QPushButton *>(QStringLiteral("downloadCacheAction1"));
                    auto *expireButton = page->findChild<QPushButton *>(QStringLiteral("downloadCacheAction2"));
                    check(limit != nullptr && trimButton != nullptr && expireButton != nullptr &&
                              limit->height() == trimButton->height() && trimButton->height() == expireButton->height(),
                          QStringLiteral("高速下载 %1：缓存上限数字框与两颗整理按钮等高").arg(sizeText),
                          QStringLiteral("数字框 %1 / 按钮 %2 %3")
                              .arg(limit != nullptr ? limit->height() : -1)
                              .arg(trimButton != nullptr ? trimButton->height() : -1)
                              .arg(expireButton != nullptr ? expireButton->height() : -1));
                    int leftGap = -1;
                    int rightGap = -1;
                    const bool measured = limit != nullptr && textSideGaps(static_cast<QWidget *>(limit), &window, &leftGap, &rightGap);
                    check(measured && qAbs(leftGap - rightGap) <= 4,
                          QStringLiteral("高速下载 %1：数字框文字左右留白差 ≤ 4px").arg(sizeText),
                          QStringLiteral("左 %1 / 右 %2").arg(leftGap).arg(rightGap));
                }
                if (scroll != nullptr) {
                    QWidget *panel = page->findChild<QWidget *>(ToolboxPage::toolPanelName(tool));
                    QString why;
                    check(scroll->horizontalScrollBar()->maximum() == 0, QStringLiteral("%1 %2：没有横向滚动范围").arg(key, sizeText),
                          QString::number(scroll->horizontalScrollBar()->maximum()));
                    check(panel != nullptr && panel->minimumSizeHint().width() <= scroll->viewport()->width(),
                          QStringLiteral("%1 %2：内容最小宽 ≤ 视口宽").arg(key, sizeText),
                          QStringLiteral("%1 > %2").arg(panel ? panel->minimumSizeHint().width() : -1).arg(scroll->viewport()->width()));
                    check(panel != nullptr && noChildClipped(panel, &why),
                          QStringLiteral("%1 %2：没有控件被右侧裁掉").arg(key, sizeText), why);
                    // 每个工具页在最小窗口下整页放得下，不出纵向滚动条（旗帜页 585 起放得下，最小高 590 留 5px 余量）。
                    if (size == QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight)) {
                        check(scroll->verticalScrollBar()->maximum() == 0,
                              QStringLiteral("%1 %2：没有纵向滚动范围").arg(key, sizeText),
                              QStringLiteral("max=%1 viewport=%2 content=%3").arg(scroll->verticalScrollBar()->maximum())
                                  .arg(scroll->viewport()->height()).arg(scroll->widget()->height()));
                    }
                }
                shot(key);
            }

            // ---- 附魔页：四个框统一高、宽窗口一行 ----
            page->showTool(ToolboxPage::Tool::Anvil);
            pump();
            QList<QComboBox *> combos = page->findChild<QWidget *>(QStringLiteral("anvilRows"))->findChildren<QComboBox *>();
            QList<QSpinBox *> spins = page->findChild<QWidget *>(QStringLiteral("anvilRows"))->findChildren<QSpinBox *>();
            combos.removeIf([](QComboBox *c) { return !c->isVisible(); });  // 刚被替换掉、等着删的旧行
            spins.removeIf([](QSpinBox *c) { return !c->isVisible(); });
            bool sameHeight = !combos.isEmpty() && !spins.isEmpty();
            for (QWidget *w : QList<QWidget *>(combos.cbegin(), combos.cend()) + QList<QWidget *>(spins.cbegin(), spins.cend()))
                sameHeight = sameHeight && w->height() == Theme::inputHeight();
            check(sameHeight, "附魔行：附魔 / 等级 / 物品 / 已用次数四个框统一高 36", sizeText);
            const bool oneLine = !combos.isEmpty() && combos.first()->mapTo(page, QPoint(0, 0)).y() == spins.first()->mapTo(page, QPoint(0, 0)).y();
            check(oneLine == (size.width() >= 900), "附魔行：宽窗口一行排下", sizeText);

            // ---- 切换浮层 ----
            auto *crumbTool = page->findChild<QPushButton *>(ToolboxPage::crumbToolName());
            crumbTool->click();
            pump();
            QWidget *popover = window.findChild<QWidget *>(ToolboxPage::switchPopoverName());
            check(popover != nullptr && popover->isVisible() && popover->width() == 236, "点工具名弹出切换浮层（宽 236）", sizeText);
            check(popover != nullptr && popover->findChild<QListWidget *>() != nullptr &&
                      popover->findChild<QListWidget *>()->count() == 9,
                  "切换浮层列出九个工具");
            shot(QStringLiteral("switch"));
            if (auto *list = popover != nullptr ? popover->findChild<QListWidget *>() : nullptr) {
                list->itemClicked(list->item(1));
                pump();
                check(page->currentTool() == ToolboxPage::tools().at(1) && !page->inCatalog() &&
                          window.findChild<QWidget *>(ToolboxPage::switchPopoverName()) == nullptr,
                      "点浮层里的工具直接切过去并收起浮层");
            }
            page->findChild<QPushButton *>(ToolboxPage::crumbRootName())->click();
            pump();
            check(page->inCatalog(), "点面包屑「工具箱」回到目录");
        }
    }

    // ---- 窄内容宽（独立页，内容宽 504 < 640）：重排而不是出滚动条 ----
    {
        Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
        Theme::apply(app);
        for (const int width : {560, 700, 836, 1216}) {
            ToolboxPage narrow;
            wireToolboxSamples(narrow);
            narrow.resize(width, 620);
            narrow.show();
            pump();
            for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
                narrow.showTool(tool);
                pump();
                auto *scroll = narrow.findChild<QScrollArea *>(ToolboxPage::toolPageName(tool));
                QWidget *panel = narrow.findChild<QWidget *>(ToolboxPage::toolPanelName(tool));
                QString why;
                const QString label = QStringLiteral("%1 @%2").arg(ToolboxPage::toolKey(tool)).arg(width);
                check(scroll != nullptr && scroll->horizontalScrollBar()->maximum() == 0,
                      QStringLiteral("窄宽 %1：没有横向滚动范围").arg(label),
                      QStringLiteral("max=%1 panelMin=%2 viewport=%3").arg(scroll != nullptr ? scroll->horizontalScrollBar()->maximum() : -1)
                          .arg(panel != nullptr ? panel->minimumSizeHint().width() : -1)
                          .arg(scroll != nullptr ? scroll->viewport()->width() : -1));
                check(panel != nullptr && noChildClipped(panel, &why),
                      QStringLiteral("窄宽 %1：没有控件被裁").arg(label), why);
                if (!shotDir.isEmpty() && (width == 560 || width == 700) && narrow.grab().save(QDir(shotDir).filePath(
                        QStringLiteral("toolbox-narrow-%1-dark-%2x620.png").arg(ToolboxPage::toolKey(tool)).arg(width)))) ++saved;
            }
            narrow.showTool(ToolboxPage::Tool::Anvil);
            pump();
            auto *rows = narrow.findChild<QWidget *>(QStringLiteral("anvilRows"));
            auto combos = rows->findChildren<QComboBox *>();
            auto spins = rows->findChildren<QSpinBox *>();
            combos.removeIf([](QComboBox *c) { return !c->isVisible(); });
            spins.removeIf([](QSpinBox *c) { return !c->isVisible(); });
            const bool oneLine = combos.first()->mapTo(&narrow, QPoint(0, 0)).y() == spins.first()->mapTo(&narrow, QPoint(0, 0)).y();
            check(oneLine == (rows->width() >= 640), QStringLiteral("附魔行折行阈值 640：内容宽 %1").arg(rows->width()));
            if (rows->width() < 640) {
                check(combos.first()->width() >= 200, "窄时附魔框独占一行并 ≥ 200");
            }
        }
    }
    if (!shotDir.isEmpty()) std::printf("saved %d shots to %s\n", saved, qPrintable(shotDir));
    return 0;
}
