/**
 * @file DesignDefaults.cpp
 * @brief 设计系统默认生效那一层（为什么这么做见头文件）。
 */

#include "DesignDefaults.hpp"

#include "Animations.hpp"
#include "Theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QHeaderView>
#include <QItemDelegate>
#include <QLayout>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QStackedWidget>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTextEdit>
#include <QTreeView>

#include <typeinfo>

namespace {

/// 样式画 `CE_ItemViewItem` 时自己在文字前让出的那一截（`QCommonStyle` 的 viewItem 文字边距）。
int styleTextMargin(const QWidget *widget) {
    const QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
    return style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
}
/// 我们要额外让的那一截：加上样式自己那截，文字正好落在 `cellTextInset()`。
int extraTextPad(const QWidget *widget) { return qMax(0, DesignDefaults::cellTextInset() - styleTextMargin(widget)); }
/// 已经接过换页那条 connect 的栈（polish 会反复进来）。
constexpr const char *kStackWired = "wtfmlDefaultPageAppear";

bool iconMode(const QStyleOptionViewItem &option) {
    const auto *list = qobject_cast<const QListView *>(option.widget);
    return list != nullptr && list->viewMode() == QListView::IconMode;
}
/// 这一格是不是一行的左头 / 右头。树会填 `viewItemPosition`；表格不填，按列号算。
void edges(const QStyleOptionViewItem &option, const QModelIndex &index, bool *left, bool *right) {
    switch (option.viewItemPosition) {
    case QStyleOptionViewItem::Beginning: *left = true; *right = false; return;
    case QStyleOptionViewItem::Middle: *left = false; *right = false; return;
    case QStyleOptionViewItem::End: *left = false; *right = true; return;
    case QStyleOptionViewItem::OnlyOne: *left = true; *right = true; return;
    case QStyleOptionViewItem::Invalid: break;
    }
    if (qobject_cast<const QTableView *>(option.widget) != nullptr && index.model() != nullptr) {
        *left = index.column() == 0;
        *right = index.column() == index.model()->columnCount(index.parent()) - 1;
    }
}

bool isQtDefaultDelegate(const QAbstractItemDelegate *delegate) {
    // 按 typeid 认：本仓不少自家代理没有 Q_OBJECT，按 className 会全认成基类。
    return delegate != nullptr &&
           (typeid(*delegate) == typeid(QStyledItemDelegate) || typeid(*delegate) == typeid(QItemDelegate));
}

/// 下拉 / 补全 / 浮层里的列表各有自己的底和代理，这一层不进去。
bool insidePopup(const QWidget *widget) {
    const QWidget *window = widget->window();
    return window != nullptr && (window->windowType() == Qt::Popup || window->windowType() == Qt::ToolTip);
}

void applyItemView(QAbstractItemView *view) {
    if (qobject_cast<QHeaderView *>(view) != nullptr || insidePopup(view)) return;
    if (isQtDefaultDelegate(view->itemDelegate())) view->setItemDelegate(new DesignDefaults::RowDelegate(view));
    view->setFrameShape(QFrame::NoFrame);
    // 去线条化：表格不画网格，行靠悬停 / 选中那块底分开。左上角那颗角按钮（全选）不要：
    // 它画成一块原生白方块，行号列也大多是 Qt 默认带出来的。表头字和格里的字一样靠左。
    if (auto *table = qobject_cast<QTableView *>(view)) {
        table->setShowGrid(false);
        table->setCornerButtonEnabled(false);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (QWidget *viewport = view->viewport()) {
        viewport->setAutoFillBackground(false);
        viewport->setAttribute(Qt::WA_Hover, true);
    }
}

void applyStack(QStackedWidget *stack) {
    if (stack->property(kStackWired).toBool()) return;
    stack->setProperty(kStackWired, true);
    // 退出口在每次换页时现查：属性可以在 polish 之后才挂上。
    QObject::connect(stack, &QStackedWidget::currentChanged, stack, [stack](int) {
        if (!Anim::enabled() || DesignDefaults::optedOut(stack)) return;
        // QTabWidget 里那个栈：选项卡自己退出了（自己接 pageAppear），这一层就不再动它。
        if (const auto *tabs = qobject_cast<const QTabWidget *>(stack->parentWidget());
            tabs != nullptr && DesignDefaults::optedOut(tabs)) {
            return;
        }
        const QPointer<QStackedWidget> owner(stack);
        const QPointer<QWidget> page(stack->currentWidget());
        if (!page || !page->isVisible()) return;

        DesignDefaults::settlePageLayout(page);
        if (!owner || !page || owner->currentWidget() != page || !page->isVisible()) return;
        Anim::pageAppear(page);
    });
}

}  // namespace

namespace DesignDefaults {

void settlePageLayout(QWidget *page) {
    const QPointer<QWidget> guard(page);
    // 「新页可见」后，滚动区还会更新内容宽度，表单收到新宽度后再排一轮。
    for (int pass = 0; pass < 4 && guard && guard->isVisible(); ++pass) {
        if (QLayout *layout = guard->layout()) layout->activate();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
}

void optOut(QWidget *widget) {
    if (widget != nullptr) widget->setProperty(optOutProperty(), true);
}

bool optedOut(const QWidget *widget) {
    return widget != nullptr && widget->property(optOutProperty()).toBool();
}

QSize RowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (iconMode(option)) return size;
    const int customMin = option.widget != nullptr
                              ? option.widget->property("wtfmlMinRowHeight").toInt()
                              : 0;
    const int floorHeight = customMin > 0 ? customMin : minRowHeight();
    const int scrollGap = option.widget != nullptr
                              ? option.widget->property("wtfmlScrollContentGap").toInt()
                              : 0;
    const bool lastColumn = index.model() != nullptr
                            && index.column() == index.model()->columnCount(index.parent()) - 1;
    const int rightGap = (scrollGap > 0 && lastColumn) ? scrollGap : 0;
    return QSize(size.width() + 2 * extraTextPad(option.widget) + rightGap, qMax(size.height(), floorHeight));
}

void RowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const bool selected = opt.state.testFlag(QStyle::State_Selected);
    const bool hovered = opt.state.testFlag(QStyle::State_MouseOver) && index.flags().testFlag(Qt::ItemIsEnabled);
    bool roundLeft = true, roundRight = true;
    edges(opt, index, &roundLeft, &roundRight);
    const bool tile = iconMode(opt);
    const int scrollGap = opt.widget != nullptr
                              ? opt.widget->property("wtfmlScrollContentGap").toInt()
                              : 0;
    if (scrollGap > 0 && (roundRight || tile) && opt.rect.width() > scrollGap) {
        opt.rect.adjust(0, 0, -scrollGap, 0);
    }
    paintRowBlock(painter, opt.rect, selected, hovered, roundLeft || tile, roundRight || tile);
    // 底已经画好了：样式那边只印内容（不铺选中底、不画焦点框），墨按这块底换。
    opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    opt.palette = Theme::selectionPalette(opt.palette, selected);
    // 每一格（不只首尾）都让到 cellTextInset()：表头每一节的 padding 是同一个数，两边从同一条竖线起。
    if (!tile) {
        const int pad = extraTextPad(opt.widget);
        opt.rect.adjust(pad, 0, -pad, 0);
    }
    QStyle *style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
}

void alignHeaderLabel(QStyleOptionHeader *option, const QWidget *header) {
    const auto *headerView = qobject_cast<const QHeaderView *>(header);
    if (headerView == nullptr || option->orientation != Qt::Horizontal) return;
    const auto *view = qobject_cast<const QAbstractItemView *>(headerView->parentWidget());
    const QAbstractItemModel *model = headerView->model();
    if (view == nullptr || model == nullptr) return;
    if (!model->headerData(option->section, Qt::Horizontal, Qt::TextAlignmentRole).isValid()) {
        const QVariant cell = model->index(0, option->section).data(Qt::TextAlignmentRole);
        if (cell.isValid())
            option->textAlignment = (Qt::Alignment(cell.toInt()) & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter;
    }
    const auto *tree = qobject_cast<const QTreeView *>(view);
    if (tree != nullptr && tree->rootIsDecorated() && headerView->visualIndex(option->section) == 0)
        option->rect.setLeft(option->rect.left() + tree->indentation());
    const int scrollGap = view->property("wtfmlScrollContentGap").toInt();
    if (scrollGap > 0 && headerView->visualIndex(option->section) == headerView->count() - 1
        && option->rect.width() > scrollGap) {
        option->rect.adjust(0, 0, -scrollGap, 0);
    }
}

bool isRowDelegate(const QAbstractItemDelegate *delegate) {
    return delegate != nullptr && typeid(*delegate) == typeid(RowDelegate);
}

void paintRowBlock(QPainter *painter, const QRect &cell, bool selected, bool hovered, bool roundLeft, bool roundRight) {
    if (!selected && !hovered) return;
    const qreal r = Theme::controlRadius();
    const QRectF block = QRectF(cell).adjusted(roundLeft ? 3 : -r - 1, 2, roundRight ? -3 : r + 1, -2);
    const Theme::Tokens t = Theme::tokens();
    QColor fill = selected ? t.focus : t.text;
    fill.setAlpha(selected ? (t.page.lightness() < 128 ? 34 : 30) : 14);
    painter->save();
    painter->setClipRect(cell);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(block, r, r);
    painter->restore();
}

void apply(QWidget *widget) {
    if (widget == nullptr || optedOut(widget)) return;
    if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
        applyItemView(view);
    } else if (auto *stack = qobject_cast<QStackedWidget *>(widget)) {
        applyStack(stack);
    } else if (qobject_cast<QPlainTextEdit *>(widget) != nullptr || qobject_cast<QTextEdit *>(widget) != nullptr) {
        // 输入面由 QSS 那条 `QPlainTextEdit, QTextEdit` 画；这里只撤原生框和视口那层不透明底。
        auto *area = static_cast<QAbstractScrollArea *>(widget);
        area->setFrameShape(QFrame::NoFrame);
        area->viewport()->setAutoFillBackground(false);
    }
}

}  // namespace DesignDefaults
