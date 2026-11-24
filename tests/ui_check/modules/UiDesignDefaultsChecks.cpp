// 设计系统默认生效（ui/DesignDefaults，步骤 120）：光秃秃的列表 / 树 / 表 / 文本框 / 栈
// 什么都不接也拿到自家默认；退出口那个属性管用；手动接过换页的栈不会一次换页播两发。
#include "Support.hpp"
#include "DesignDefaults.hpp"
#include "InstanceSettingsDialog.hpp"
#include "PresetOptionsEditor.hpp"
#include "SettingsPageParts.hpp"
#include "TitleBar.hpp"

#include <QComboBox>
#include <QEvent>
#include <QEventLoop>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QWidget *curtain(QWidget *page) { return page->findChild<QWidget *>(QStringLiteral("wtfmlPageCurtain")); }
bool curtainShown(QWidget *page) { return curtain(page) != nullptr && curtain(page)->isVisible(); }

class OwnDelegate : public QStyledItemDelegate {
    using QStyledItemDelegate::QStyledItemDelegate;
};

bool defaultsOn(QAbstractItemView *view) {
    return DesignDefaults::isRowDelegate(view->itemDelegate()) && view->frameShape() == QFrame::NoFrame &&
           !view->viewport()->autoFillBackground();
}

void viewChecks() {
    QWidget window;
    window.resize(640, 480);
    auto *layout = new QVBoxLayout(&window);
    auto *list = new QListWidget;
    list->addItems({QStringLiteral("一"), QStringLiteral("二")});
    auto *tree = new QTreeView;
    auto *model = new QStandardItemModel(2, 2, tree);
    tree->setModel(model);
    auto *table = new QTableWidget(2, 3);
    auto *text = new QPlainTextEdit;
    auto *own = new QListWidget;
    own->setItemDelegate(new OwnDelegate(own));
    auto *native = new QListWidget;
    DesignDefaults::optOut(native);  // 测试：退出口
    auto *combo = new QComboBox;
    combo->addItems({QStringLiteral("甲"), QStringLiteral("乙")});
    for (QWidget *w : {static_cast<QWidget *>(list), static_cast<QWidget *>(tree), static_cast<QWidget *>(table),
                       static_cast<QWidget *>(text), static_cast<QWidget *>(own), static_cast<QWidget *>(native),
                       static_cast<QWidget *>(combo)})
        layout->addWidget(w);
    window.show();
    QApplication::processEvents();

    check(defaultsOn(list), "★ DD1 光秃秃的 QListWidget：默认行代理 + NoFrame + 视口透明");
    check(defaultsOn(tree), "★ DD1 光秃秃的 QTreeView：默认行代理 + NoFrame + 视口透明");
    check(defaultsOn(table) && !table->showGrid(), "DD1 光秃秃的 QTableWidget：默认行代理 + NoFrame + 视口透明 + 不画网格");
    check(!table->isCornerButtonEnabled() &&
              table->horizontalHeader()->defaultAlignment() == (Qt::AlignLeft | Qt::AlignVCenter),
          "DD1b 光秃秃的 QTableWidget：关左上角角按钮，表头字靠左");
    check(list->sizeHintForRow(0) >= DesignDefaults::minRowHeight(), "DD1 默认行代理行高不低于 minRowHeight",
          QString::number(list->sizeHintForRow(0)));
    check(!DesignDefaults::isRowDelegate(table->horizontalHeader()->itemDelegate()),
          "DD1 表头不是条目视图那一类，不被换代理");
    check(text->frameShape() == QFrame::NoFrame && !text->viewport()->autoFillBackground(),
          "★ DD2 光秃秃的 QPlainTextEdit：去原生框、视口透明（输入面归 QSS）");
    check(!DesignDefaults::isRowDelegate(own->itemDelegate()), "DD3 自家代理不被替换");
    check(!DesignDefaults::isRowDelegate(native->itemDelegate()) && native->frameShape() != QFrame::NoFrame,
          "★ DD4 挂了退出属性的列表留原生代理和边框");
    combo->showPopup();
    QApplication::processEvents();
    check(!DesignDefaults::isRowDelegate(combo->view()->itemDelegate()), "DD5 下拉弹层里的列表不被换代理");
    combo->hidePopup();
    // 焦点环那条会反复 unpolish / polish：再 polish 一次不叠第二层。
    QAbstractItemDelegate *before = list->itemDelegate();
    list->style()->unpolish(list);
    list->style()->polish(list);
    check(list->itemDelegate() == before, "DD6 重复 polish 不换掉已换上的默认代理");
}

/// @p area 里第一列有墨的 x（和这一片左缘那个底色差得开就算墨）；没有返回 -1。
int firstInkX(const QImage &image, const QRect &area) {
    const QRect r = area.intersected(image.rect());
    const QColor ground = image.pixelColor(r.left() + 1, r.center().y());
    for (int x = r.left(); x <= r.right(); ++x)
        for (int y = r.top(); y <= r.bottom(); ++y) {
            const QColor c = image.pixelColor(x, y);
            if (qMax(qAbs(c.red() - ground.red()), qMax(qAbs(c.green() - ground.green()), qAbs(c.blue() - ground.blue()))) > 60)
                return x;
        }
    return -1;
}

/// 表头字和单元格字左缘对齐：每一列（首列、中间列、末列）都量一遍，不按某张表打补丁。
void headerAlignChecks(QApplication &app) {
    Theme::apply(app);  // 表头那条 padding 在应用 QSS 里
    QWidget window;
    window.resize(640, 240);
    auto *layout = new QVBoxLayout(&window);
    auto *table = new QTableWidget(2, 3);
    table->setHorizontalHeaderLabels({QStringLiteral("甲乙"), QStringLiteral("甲乙"), QStringLiteral("甲乙")});
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 3; ++col) table->setItem(row, col, new QTableWidgetItem(QStringLiteral("甲乙")));
    table->verticalHeader()->hide();
    layout->addWidget(table);
    window.show();
    QApplication::processEvents();
    const QImage shot = window.grab().toImage();
    QHeaderView *header = table->horizontalHeader();
    QStringList detail;
    bool aligned = true;
    for (int col = 0; col < 3; ++col) {
        const QPoint headerAt = header->mapTo(&window, QPoint(header->sectionViewportPosition(col), 0));
        const int headX = firstInkX(shot, QRect(headerAt, QSize(header->sectionSize(col), header->height())));
        const QRect cell = table->visualRect(table->model()->index(0, col));
        const int cellX = firstInkX(shot, QRect(table->viewport()->mapTo(&window, cell.topLeft()), cell.size()));
        aligned &= headX >= 0 && cellX >= 0 && qAbs(headX - cellX) <= 1;
        detail << QStringLiteral("列%1 表头 %2 / 格 %3").arg(col).arg(headX).arg(cellX);
    }
    check(aligned, "★ DD11 表头字和单元格字每一列左缘对齐（同一个 cellTextInset）", detail.join(QStringLiteral("；")));
}

/// @p area 里最后一列有墨的 x；没有返回 -1。
int lastInkX(const QImage &image, const QRect &area) {
    const QRect r = area.intersected(image.rect());
    const QColor ground = image.pixelColor(r.left() + 1, r.center().y());
    for (int x = r.right(); x >= r.left(); --x)
        for (int y = r.top(); y <= r.bottom(); ++y) {
            const QColor c = image.pixelColor(x, y);
            if (qMax(qAbs(c.red() - ground.red()), qMax(qAbs(c.green() - ground.green()), qAbs(c.blue() - ground.blue()))) > 60)
                return x;
        }
    return -1;
}

/// 表头这一节和 (row 0, @p col) 那一格在 @p shot 里的墨：左缘 / 右缘。
struct ColumnInk { int headLeft, headRight, cellLeft, cellRight; };
ColumnInk columnInk(QWidget *window, const QImage &shot, QAbstractItemView *view, QHeaderView *header, int col) {
    const QPoint headerAt = header->mapTo(window, QPoint(header->sectionViewportPosition(col), 0));
    const QRect headRect(headerAt, QSize(header->sectionSize(col), header->height()));
    const QRect cell = view->visualRect(view->model()->index(0, col));
    const QRect cellRect(view->viewport()->mapTo(window, cell.topLeft()), cell.size());
    return {firstInkX(shot, headRect), lastInkX(shot, headRect), firstInkX(shot, cellRect), lastInkX(shot, cellRect)};
}

/// 表头每一节跟着那一列的内容走：树的第一节和缩进后的字齐、数字列右对齐时表头也右对齐、
/// 自家代理的两张真表（预设游戏设置表、实例服务器列表）第一列和表头齐。
void headerFollowsColumnChecks() {
    QStringList detail;
    bool indentOk = true, rightOk = true, ownOk = true;
    {
        QWidget window;
        window.resize(640, 360);
        auto *layout = new QVBoxLayout(&window);
        auto *tree = new QTreeWidget;
        tree->setHeaderLabels({QStringLiteral("甲乙"), QStringLiteral("甲乙")});
        auto *top = new QTreeWidgetItem(tree, {QStringLiteral("甲乙"), QStringLiteral("甲乙")});
        new QTreeWidgetItem(top, {QStringLiteral("甲乙"), QStringLiteral("甲乙")});
        top->setExpanded(true);
        auto *numbers = new QTreeWidget;
        numbers->setRootIsDecorated(false);
        numbers->setHeaderLabels({QStringLiteral("甲乙"), QStringLiteral("数量")});
        auto *row = new QTreeWidgetItem(numbers, {QStringLiteral("甲乙"), QStringLiteral("123")});
        row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        auto *table = new QTableWidget(1, 2);
        table->verticalHeader()->hide();
        table->setHorizontalHeaderLabels({QStringLiteral("甲乙"), QStringLiteral("数量")});
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("甲乙")));
        auto *count = new QTableWidgetItem(QStringLiteral("123"));
        count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(0, 1, count);
        layout->addWidget(tree);
        layout->addWidget(numbers);
        layout->addWidget(table);
        window.show();
        QApplication::processEvents();
        const QImage shot = window.grab().toImage();
        const ColumnInk first = columnInk(&window, shot, tree, tree->header(), 0);
        indentOk = first.headLeft >= 0 && qAbs(first.headLeft - first.cellLeft) <= 1;
        detail << QStringLiteral("树首列 表头 %1 / 格 %2").arg(first.headLeft).arg(first.cellLeft);
        const ColumnInk treeNum = columnInk(&window, shot, numbers, numbers->header(), 1);
        const ColumnInk tableNum = columnInk(&window, shot, table, table->horizontalHeader(), 1);
        rightOk = treeNum.headRight >= 0 && qAbs(treeNum.headRight - treeNum.cellRight) <= 2 &&
                  tableNum.headRight >= 0 && qAbs(tableNum.headRight - tableNum.cellRight) <= 2;
        detail << QStringLiteral("树数字列右缘 表头 %1 / 格 %2").arg(treeNum.headRight).arg(treeNum.cellRight)
               << QStringLiteral("表数字列右缘 表头 %1 / 格 %2").arg(tableNum.headRight).arg(tableNum.cellRight);
    }
    {
        PresetOptionsEditor editor({});
        editor.resize(600, 440);
        editor.show();
        QApplication::processEvents();
        auto *table = editor.findChild<QTableWidget *>(QStringLiteral("presetOptionsEditorTable"));
        const QImage shot = editor.grab().toImage();
        // 只量首列：第二列已经是滑条 / 下拉框这类输入控件，第一笔墨是控件边框或滑轨，
        // 不是文字，拿来和表头字比会把控件内缩算成“没对齐”。DD14 说的是“第一列”。
        if (table != nullptr) {
            const ColumnInk ink = columnInk(&editor, shot, table, table->horizontalHeader(), 0);
            // 首列那一格最左边是勾选框，不是文字：勾选框的指示器比表头字左缘再让 2px 是
            // QStyle 自己画的 inset，不拿它当「表头没对齐」。服务器列表首列同样按 2px 判。
            ownOk &= ink.headLeft >= 0 && qAbs(ink.headLeft - ink.cellLeft) <= 2;
            detail << QStringLiteral("预设设置表首列 表头 %1 / 格 %2").arg(ink.headLeft).arg(ink.cellLeft);
        }
        ownOk &= table != nullptr;
    }
    {
        using Dlg = InstanceSettingsDialog;
        Dlg dialog(QStringLiteral("Test"), QStringLiteral("test"), Dlg::Override(), Dlg::GlobalValues(), {}, true);
        dialog.resize(820, 560);
        dialog.show();
        dialog.showPage(QStringLiteral("server"));
        dialog.setServerList({{QStringLiteral("甲乙服"), QStringLiteral("mc.example.org")}}, QString());
        QApplication::processEvents();
        auto *tree = dialog.findChild<QTreeWidget *>(Dlg::serverTreeName());
        const QImage shot = dialog.grab().toImage();
        if (tree != nullptr) {
            const ColumnInk ink = columnInk(&dialog, shot, tree, tree->header(), 0);
            ownOk &= ink.headLeft >= 0 && qAbs(ink.headLeft - ink.cellLeft) <= 2;
            detail << QStringLiteral("服务器列表首列 表头 %1 / 图标 %2").arg(ink.headLeft).arg(ink.cellLeft);
        }
        ownOk &= tree != nullptr;
    }
    const QString joined = detail.join(QStringLiteral("；"));
    check(indentOk, "★ DD12 树的第一节表头字和缩进后的格字左缘对齐", joined);
    check(rightOk, "★ DD13 右对齐的数字列，表头字也右对齐、右缘和格字齐", joined);
    check(ownOk, "★ DD14 自家代理的表（预设游戏设置 / 服务器列表）第一列和表头左缘对齐", joined);
}

/// 两页的栈；页上各一块标签撑面积。
QStackedWidget *twoPages(QWidget *parent) {
    auto *stack = new QStackedWidget(parent);
    for (const auto &name : {QStringLiteral("页一"), QStringLiteral("页二")}) stack->addWidget(new QLabel(name));
    return stack;
}

class DeferredSettingsPage : public QScrollArea {
protected:
    void showEvent(QShowEvent *event) override {
        QScrollArea::showEvent(event);
        m_needsLayout = true;
        QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest));
    }

    bool event(QEvent *event) override {
        if (event->type() == QEvent::LayoutRequest && m_needsLayout) {
            m_needsLayout = false;
            widget()->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        return QScrollArea::event(event);
    }

private:
    bool m_needsLayout = false;
};

class SettingsRowPaintProbe : public SettingsParts::SettingsRow {
public:
    using SettingsRow::SettingsRow;
    int firstPaintWidth = -1;
    bool firstPaintStacked = false;

protected:
    void paintEvent(QPaintEvent *event) override {
        if (firstPaintWidth < 0) {
            firstPaintWidth = width();
            firstPaintStacked = stacked();
        }
        SettingsRow::paintEvent(event);
    }
};

void stackFirstSnapshotCheck() {
    QStackedWidget stack;
    stack.resize(SettingsParts::kStackBelow + 160, 360);
    stack.addWidget(new QWidget);
    auto *page = new DeferredSettingsPage;
    auto *form = new QWidget;
    form->setMaximumWidth(SettingsParts::kStackBelow - 40);
    auto *layout = new QVBoxLayout(form);
    auto *row = new SettingsRowPaintProbe({QStringLiteral("界面语言"), {}, {}}, new QComboBox, form);
    layout->addWidget(row);
    layout->addStretch();
    page->setWidgetResizable(true);
    page->setWidget(form);
    stack.addWidget(page);
    stack.show();
    QApplication::processEvents();

    // 「首帧」由切页同步抓取。处理事件前先读探针，避免量到幕布下面后来排好的页面。
    stack.setCurrentWidget(page);
    const bool snapped = curtainShown(page);
    const int firstWidth = row->firstPaintWidth;
    const bool firstStacked = row->firstPaintStacked;
    QApplication::processEvents();
    check(snapped && !firstStacked && firstWidth >= SettingsParts::kStackBelow && firstWidth == row->width(),
          "DD15 设置子页首次换页快照已按最终宽度排成横排",
          QStringLiteral("首帧行宽 %1，最终行宽 %2，首帧叠放 %3")
              .arg(firstWidth).arg(row->width()).arg(firstStacked));
}

void settingsNavigationSnapshotCheck(QApplication &app) {
    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(1280, 800);
    auto *tabs = ui.getTabWidget();
    const int settingsIndex = MainWindowUI::settingsTabIndex();
    auto *settings = tabs->widget(settingsIndex);
    auto *form = settings->findChild<QWidget *>(MainWindowUI::settingsFormName(QStringLiteral("launch")));
    auto *bar = window.findChild<TitleBar *>();
    auto *layout = form != nullptr ? qobject_cast<QVBoxLayout *>(form->layout()) : nullptr;
    check(layout != nullptr && bar != nullptr, "DD16 设置页和主导航已就绪");
    if (layout == nullptr || bar == nullptr) return;

    // 模拟进入设置后，同一个切页信号的后续槽更新内容和布局。
    form->setMaximumWidth(SettingsParts::kStackBelow - 40);
    auto *row = new SettingsRowPaintProbe({QStringLiteral("首帧检查"), {}, {}}, new QComboBox, form);
    layout->insertWidget(0, row);
    QObject::connect(tabs, &QTabWidget::currentChanged, form, [form, settingsIndex](int index) {
        if (index == settingsIndex) form->setMaximumWidth(MainWindowUI::settingsFormMaxWidth());
    });
    window.show();
    app.processEvents();

    emit bar->navRequested(settingsIndex);
    QCoreApplication::sendPostedEvents(tabs, QEvent::MetaCall);
    const bool snapped = curtainShown(tabs);
    const int firstWidth = row->firstPaintWidth;
    const bool firstStacked = row->firstPaintStacked;
    app.processEvents();
    check(snapped && !firstStacked && firstWidth >= SettingsParts::kStackBelow && firstWidth == row->width(),
          "DD16 从主导航首次进入设置时，首帧使用更新后的表单布局",
          QStringLiteral("首帧行宽 %1，最终行宽 %2，首帧叠放 %3")
              .arg(firstWidth).arg(row->width()).arg(firstStacked));
}

void stackChecks() {
    QWidget window;
    window.resize(640, 480);
    auto *layout = new QVBoxLayout(&window);
    QStackedWidget *bare = twoPages(&window);
    QStackedWidget *quiet = twoPages(&window);
    DesignDefaults::optOut(quiet);  // 测试：退出口
    QStackedWidget *manual = twoPages(&window);
    // 旧写法：手动接了一条一样的换页动效。
    QObject::connect(manual, &QStackedWidget::currentChanged, manual, [manual](int) {
        if (QWidget *page = manual->currentWidget(); page != nullptr && page->isVisible()) Anim::pageAppear(page);
    });
    auto *tabs = new QTabWidget(&window);
    tabs->addTab(new QLabel(QStringLiteral("卡一")), QStringLiteral("一"));
    tabs->addTab(new QLabel(QStringLiteral("卡二")), QStringLiteral("二"));
    auto *tabsQuiet = new QTabWidget(&window);
    tabsQuiet->addTab(new QLabel(QStringLiteral("卡一")), QStringLiteral("一"));
    tabsQuiet->addTab(new QLabel(QStringLiteral("卡二")), QStringLiteral("二"));
    DesignDefaults::optOut(tabsQuiet);  // 测试：选项卡退出（自己接 pageAppear 的那种）
    for (QWidget *w : {static_cast<QWidget *>(bare), static_cast<QWidget *>(quiet), static_cast<QWidget *>(manual),
                       static_cast<QWidget *>(tabs), static_cast<QWidget *>(tabsQuiet)})
        layout->addWidget(w);
    window.show();
    QApplication::processEvents();

    bare->setCurrentIndex(1);
    check(curtainShown(bare->widget(1)), "★ DD7 光秃秃的 QStackedWidget 换页：新那一页盖上换页幕布");
    quiet->setCurrentIndex(1);
    check(curtain(quiet->widget(1)) == nullptr, "★ DD8 挂了退出属性的栈换页硬切");
    tabs->setCurrentIndex(1);
    check(curtainShown(tabs->widget(1)), "DD9 QTabWidget 里那个栈换页同样有动效");
    tabsQuiet->setCurrentIndex(1);
    check(curtain(tabsQuiet->widget(1)) == nullptr, "DD9 选项卡退出时它里面那个栈也不动");

    // 手动接线 + 默认：第一次换页把幕布建出来、跑完；第二次换页时数这一拍起了几发动画。
    manual->setCurrentIndex(1);
    wait(Anim::appearMs() + 150);
    manual->setCurrentIndex(0);
    manual->setCurrentIndex(1);
    QWidget *c = curtain(manual->widget(1));
    const int runs = c != nullptr ? int(c->findChildren<QVariantAnimation *>().size()) : -1;
    check(runs == 1, "★ DD10 手动接过换页的栈：一次换页只起一发（不重播）", QString::number(runs));
    wait(Anim::appearMs() + 150);
    check(c != nullptr && !c->isVisible(), "DD10 那一发照常收尾");
}

}  // namespace

int UiDesignDefaultsChecks(QApplication &app) {
    viewChecks();
    Anim::setEnabled(true);
    stackChecks();
    stackFirstSnapshotCheck();
    settingsNavigationSnapshotCheck(app);
    Anim::setEnabled(false);
    headerAlignChecks(app);
    headerFollowsColumnChecks();
    return 0;
}
