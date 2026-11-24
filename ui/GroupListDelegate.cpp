/**
 * @file GroupListDelegate.cpp
 * @brief 实现分组设置列表的行绘制。
 * @note 行尾上下移是否可用由条目计算，本层只负责几何和绘制。
 */

#include "GroupListDelegate.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QModelIndex>
#include <QPainter>
#include <QStyle>
#include "Popups.hpp"

#include "DesignDefaults.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

GroupListDelegate::GroupListDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

int GroupListDelegate::rowGap() {
    // 留一条肉眼看得出来、又不至于把两行拉成两个块的距离（46 高的行上 6 px）。
    return 6;
}

QRect GroupListDelegate::contentRect(const QRect &itemRect) {
    // 条目矩形 → 画出来的那一块。**只有高度**上收：底下那 `rowGap()` 是一条永远空着的带子，
    // 左右一点都不动（横向的留白是版面那一圈 `versionTreeRowPadding()` 的事，两码事）。
    const int height = qMax(0, qMin(itemRect.height(), MainWindowUI::versionTreeRowHeight()));
    return QRect(itemRect.left(), itemRect.top(), itemRect.width(), height);
}

QRect GroupListDelegate::moveBoxRect(const QRect &itemRect) {
    const QRect content = contentRect(itemRect);
    const int cell = MainWindowUI::versionTreeArrowSize() + 6;
    // 方框 = 上下两格竖着码起来。宽度也得跟着行高那一套收：手摆的矩形（验证工程里那个
    // 12 px 的矮行）进来时不能算出比行还宽/还高的框，不然画出去的那一截会压到别的行上。
    const int width = qMax(0, qMin(cell, content.width()));
    const int height = qMax(0, qMin(2 * cell, content.height()));
    return QRect(content.right() + 1 - MainWindowUI::versionTreeRowPadding() - width,
                 content.top() + (content.height() - height) / 2, width, height);
}

QRect GroupListDelegate::moveUpRect(const QRect &itemRect) {
    const QRect box = moveBoxRect(itemRect);
    // 上半：从框顶起算，高度取一半 —— 两半合起来正好是整框，中间那条线不重不漏。
    return QRect(box.left(), box.top(), box.width(), box.height() / 2);
}

QRect GroupListDelegate::moveDownRect(const QRect &itemRect) {
    const QRect box = moveBoxRect(itemRect);
    const int half = box.height() / 2;
    // 下半：接在上半底下，剩下的高度全给它（框高是奇数时多出来的那一像素落在下面，
    // 保证两半拼起来 == 整框）。
    return QRect(box.left(), box.top() + half, box.width(), box.height() - half);
}

QRect GroupListDelegate::glyphRect(const QRect &cell) {
    const int size =
        qMax(0, qMin(MainWindowUI::versionTreeArrowSize(), qMin(cell.width(), cell.height())));
    const int dx = (cell.width() - size) / 2;
    const int dy = (cell.height() - size) / 2;
    return QRect(cell.left() + dx, cell.top() + dy, size, size);
}

QRect GroupListDelegate::moveUpGlyphRect(const QRect &itemRect) {
    return glyphRect(moveUpRect(itemRect));
}

QRect GroupListDelegate::moveDownGlyphRect(const QRect &itemRect) {
    return glyphRect(moveDownRect(itemRect));
}

bool GroupListDelegate::canMoveUp(const QModelIndex &index) {
    return index.isValid() && index.row() > 0;
}

bool GroupListDelegate::canMoveDown(const QModelIndex &index) {
    if (!index.isValid()) {
        return false;
    }
    const QAbstractItemModel *model = index.model();
    // 行序就是组序（这张表里只有分组），所以"下面还有没有一行"就是全部判据。
    return model != nullptr && index.row() < model->rowCount(index.parent()) - 1;
}

bool GroupListDelegate::showsMoveBox(const QModelIndex &index) {
    return canMoveUp(index) || canMoveDown(index);
}

QString GroupListDelegate::rowActionTip(const QModelIndex &index, const QRect &itemRect,
                                       const QPoint &pos) {
    if (!showsMoveBox(index)) {
        return QString();
    }
    // 先判上移、再判下移（和画它们、认它们那一格同一个顺序）。这两格现在是上下叠着的、
    // 一点都不挨着，顺序无所谓；将来真叠上了，先判的那个赢（和 VersionTreeDelegate::
    // rowActionTip() 同一条）。
    if (canMoveUp(index) && moveUpRect(itemRect).contains(pos)) {
        return tr("上移这一组（按住 Shift 直接放到最上面）");
    }
    if (canMoveDown(index) && moveDownRect(itemRect).contains(pos)) {
        return tr("下移这一组（按住 Shift 直接放到最下面）");
    }
    return QString();
}

bool GroupListDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                                  const QStyleOptionViewItem &option, const QModelIndex &index) {
    if (event != nullptr && event->type() == QEvent::ToolTip) {
        const QString tip = rowActionTip(index, option.rect, event->pos());
        if (!tip.isEmpty()) {
            Popups::showTooltip(tip, event->globalPos(), view);
            return true;
        }
    }
    // 兜底：条目自己写 `Qt::ToolTipRole` 的老路（基类就干这个）。
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

void GroupListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    const QRect content = contentRect(option.rect);
    // 下面所有几何都吃**条目矩形**（option.rect），映射在里面做一次（见 .hpp 那段）。
    const QRect row = option.rect;
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    // 选中 / 悬停底和别的列表是同一块（DesignDefaults::paintRowBlock），**铺的是画出来的那一块**
    // （不是整条条目矩形）：铺满整条的话相邻两行的底色会连成一片，`rowGap()` 那条缝就白留了。
    DesignDefaults::paintRowBlock(painter, content, selected, hovered);
    // 选中那一行的底色是浅灰（`#cccccc`，见 `ui/Theme.hpp` 的 `selectionFill()`）⇒
    // 这一行上手画的那两笔（组名、行尾方框里那两颗三角）取色的那一份也得跟着换：
    // "选中就取 `HighlightedText`"那条规矩本身没错，错的是**浅灰上那一档仍照蓝底配**
    // （深色主题下是白，白压 `#cccccc` 只有 1.6:1）。
    // 换哪几档只有一处出处（`Theme::selectionPalette()`），理由是"三个手画正文的代理
    // 抄三遍迟早漏一档"。
    const QPalette palette = Theme::selectionPalette(option.palette, selected);
    const int pad = MainWindowUI::versionTreeRowPadding();
    // 右边那个方框**无条件**扣掉（不管这一行此刻画不画它）：只扣画了的那几行的话，文字
    // 右边界就不一样了 —— 组名一长，看着就是每行各自截断在不同的地方。
    const int textLeft = content.left() + pad;
    const int textRight = moveBoxRect(row).left() - 6;
    const QRect textRect(textLeft, content.top(), qMax(0, textRight - textLeft), content.height());

    // 字色和默认行代理一样：底是半透明的那一块，字一直是这份 palette 里的正文色。
    const QColor ink = palette.color(QPalette::Text);
    painter->save();
    painter->setPen(ink);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      option.fontMetrics.elidedText(text, Qt::ElideRight, textRect.width()));

    // 行尾那两颗实心三角：**只有这一行真挪得动的那一侧才画**（不是画成灰的）
    // —— 判据和 eventFilter() 认那一下读的是同一对函数，画和点永远不会错开。
    // 步骤 137：不再画那圈框（原生线框，和设计系统里别的东西都不像）。平时是次要色，
    // 指到 / 选中这一行时提成正文色 —— 和组名同一份 palette，两档之外不引入第三种颜色。
    if (showsMoveBox(index)) {
        const QColor arrowInk =
            palette.color((selected || hovered) ? QPalette::Text : QPalette::PlaceholderText);
        const int size = MainWindowUI::versionTreeArrowSize();
        if (canMoveUp(index)) {
            const QRect glyph = moveUpGlyphRect(row);
            painter->drawPixmap(glyph.topLeft(),
                                VersionIcons::triangleUp(qMin(size, glyph.width()), arrowInk));
        }
        if (canMoveDown(index)) {
            const QRect glyph = moveDownGlyphRect(row);
            painter->drawPixmap(glyph.topLeft(),
                                VersionIcons::triangleDown(qMin(size, glyph.width()), arrowInk));
        }
    }
    painter->restore();
}
