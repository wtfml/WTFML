/**
 * @file FlowLayout.hpp
 * @brief 流式布局：一行放不下就折到下一行，子项按各自的 sizeHint 摆（行内垂直居中），不拉伸。
 *
 * 用在"项数不定、又不许出横向滚动条"的地方（工具箱的选项行、资源详情页的版本 chip）。
 * 父布局靠 heightForWidth 拿到折行后的高度，所以放它的容器不要钉死高度。
 * addStretch() 加的伸缩项吃掉本行余宽：一行放得下时摆法同带 stretch 的 QHBoxLayout。
 * alignRight() 让每一行靠右（右对齐的按钮组折行后仍靠右）。
 */
#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QSpacerItem>

class FlowLayout final : public QLayout {
public:
    explicit FlowLayout(int hSpacing = 8, int vSpacing = 8) : m_h(hSpacing), m_v(vSpacing) {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override { qDeleteAll(m_items); }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    void alignRight() { m_right = true; }
    void addStretch() { addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum)); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override {
        return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize size;
        for (QLayoutItem *item : m_items) size = size.expandedTo(item->minimumSize());
        const QMargins m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

private:
    int arrange(const QRect &rect, bool testOnly) const {
        const QMargins m = contentsMargins();
        const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = area.x(), y = area.y(), lineHeight = 0;
        QList<QLayoutItem *> line;
        // 换行或收尾时摆一行：伸缩项平分余宽，矮的控件在行内垂直居中。
        const auto place = [&] {
            if (testOnly) return;
            int stretches = 0;
            for (QLayoutItem *item : line) stretches += item->spacerItem() != nullptr;
            const int spare = stretches ? qMax(0, area.right() + 1 - (x - m_h)) / stretches : 0;
            int at = area.x() + (m_right ? qMax(0, area.right() + 1 - (x - m_h)) : 0);
            for (QLayoutItem *item : line) {
                const QSize size = item->spacerItem() ? QSize(spare, 0) : item->sizeHint();
                item->setGeometry(QRect(QPoint(at, y + qMax(0, lineHeight - size.height()) / 2), size));
                at += size.width() + m_h;
            }
        };
        for (QLayoutItem *item : m_items) {
            const bool stretch = item->spacerItem() != nullptr;  // QSpacerItem 的 isEmpty() 恒为真
            if (!stretch && item->isEmpty()) continue;
            const QSize hint = stretch ? QSize(0, 0) : item->sizeHint();
            if (x > area.x() && x + hint.width() > area.right() + 1) {
                place();
                line.clear();
                x = area.x();
                y += lineHeight + m_v;
                lineHeight = 0;
            }
            line.append(item);
            x += hint.width() + m_h;
            lineHeight = qMax(lineHeight, hint.height());
        }
        place();
        return y + lineHeight - rect.y() + m.top() + m.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_h, m_v;
    bool m_right = false;
};
