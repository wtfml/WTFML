/**
 * @file AutoHeight.hpp
 * @brief 嵌在滚动页里的多行框 / 列表：高度随内容撑开，自己不出竖滚动条，由外层那一条滚。
 *
 * 高度只从内容算（文本版面、`sizeHintForRow`），内容、宽度、字体、样式、语言变了都重算。
 * 键盘移动光标 / 当前行时，让外层滚动区跟过去（自己没有行程了，Qt 那套自动跟随到不了外层）。
 */
#pragma once

#include <QAbstractItemModel>
#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>

#include <cmath>
#include <functional>

namespace AutoHeight {

/// 把 widget 坐标系里的 rect 滚进最近的外层滚动区。@p bottomMargin：底下要让开的高度（浮动按钮）。
inline void reveal(QWidget *widget, const QRect &rect, int bottomMargin = 8) {
    for (QWidget *parent = widget->parentWidget(); parent != nullptr; parent = parent->parentWidget()) {
        auto *area = qobject_cast<QScrollArea *>(parent);
        if (area == nullptr || area->widget() == nullptr || !area->widget()->isAncestorOf(widget)) continue;
        const QRect target(widget->mapTo(area->widget(), rect.topLeft()), rect.size());
        area->ensureVisible(target.center().x(), target.bottom(), 0, bottomMargin);
        area->ensureVisible(target.center().x(), target.top(), 0, 8);
        return;
    }
}

namespace detail {

class Fitter : public QObject {
public:
    Fitter(QAbstractScrollArea *target, std::function<int()> contentHeight, int floor, bool stretch = false)
        : QObject(target), m_target(target), m_content(std::move(contentHeight)), m_floor(floor),
          m_stretch(stretch) {
        target->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        target->installEventFilter(this);
    }

    void fit() {
        // 析构途中（hide、model 清空）还会进来：那时控件的子类部分已经没了。
        if (!qobject_cast<QAbstractScrollArea *>(parent())) return;
        if (m_fitting) return;  // setFixedHeight 会同步回到 Resize
        m_fitting = true;
        m_target->ensurePolished();
        // 外框 = 控件高减视口高（样式表的 border / padding、横滚动条都在里面）；没显示时几何还是默认值，按框宽估。
        const int chrome = m_target->isVisible()
                               ? m_target->height() - m_target->viewport()->height()
                               : 2 * m_target->frameWidth();
        // 叠页里没露出来的那一页不算数：它的版面宽度是旧的，量出来的高度会把整叠撑大。
        auto *stack = qobject_cast<QStackedWidget *>(m_target->parentWidget());
        const bool buried = stack != nullptr && stack->currentWidget() != m_target;
        apply(buried ? m_floor : qMax(m_floor, m_content() + chrome));
        m_target->verticalScrollBar()->setValue(0);
        m_fitting = false;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::Resize:
            // 这一发早于控件自己的 resizeEvent，视口还是旧几何：只排到下一轮再算。
            QTimer::singleShot(0, this, [this] { fit(); });
            break;
        case QEvent::Show:
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::LanguageChange:
        case QEvent::Hide:
            fit();
            // 视口与文本版面在这一发之后才更新到新宽度，再算一次。
            QTimer::singleShot(0, this, [this] { fit(); });
            break;
        default: break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void apply(int height) {
        if (m_stretch) {  // 只钉下限：版面有富余时框照旧跟着长
            if (m_target->minimumHeight() != height) m_target->setMinimumHeight(height);
        } else if (m_target->minimumHeight() != height || m_target->maximumHeight() != height) {
            m_target->setFixedHeight(height);
        }
    }

    QAbstractScrollArea *m_target;
    std::function<int()> m_content;
    int m_floor;
    bool m_stretch;
    bool m_fitting = false;
};

}  // namespace detail

/// @p floor：内容少时的最小高度（保持原来的样子）。
/// @p stretch：只设下限、不钉死，给跟着所在卡片拉高的框用。
inline void install(QPlainTextEdit *edit, int floor, bool stretch = false) {
    auto *fitter = new detail::Fitter(edit, [edit] {
        // QPlainTextEdit 的 documentSize() 高度按"行"算，像素高要逐段量。
        qreal height = 2 * edit->document()->documentMargin();
        auto *layout = edit->document()->documentLayout();
        for (QTextBlock block = edit->document()->begin(); block.isValid(); block = block.next()) {
            height += layout->blockBoundingRect(block).height();
        }
        return int(std::ceil(height)) + 1;
    }, floor, stretch);
    QObject::connect(edit, &QPlainTextEdit::textChanged, fitter, [fitter] { fitter->fit(); });
    QObject::connect(edit, &QPlainTextEdit::cursorPositionChanged, fitter, [fitter, edit] {
        if (!edit->hasFocus()) return;
        QTimer::singleShot(0, fitter, [edit] {
            reveal(edit->viewport(), edit->cursorRect());
        });
    });
    fitter->fit();
}

inline void install(QTextEdit *edit, int floor) {
    auto *fitter = new detail::Fitter(edit, [edit] {
        return int(std::ceil(edit->document()->size().height()));
    }, floor);
    QObject::connect(edit->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                     fitter, [fitter] { fitter->fit(); });
    QObject::connect(edit, &QTextEdit::cursorPositionChanged, fitter, [fitter, edit] {
        if (!edit->hasFocus()) return;
        QTimer::singleShot(0, fitter, [edit] {
            reveal(edit->viewport(), edit->cursorRect());
        });
    });
    fitter->fit();
}

/// 单列竖排列表。要在 model 就位后调用（`QListWidget` 构造完即可）。
inline void install(QListWidget *list, int floor = 0) {
    auto *fitter = new detail::Fitter(list, [list] {
        int height = 2 * list->spacing();
        for (int row = 0; row < list->count(); ++row) {
            if (!list->isRowHidden(row)) height += list->sizeHintForRow(row) + 2 * list->spacing();
        }
        return height;
    }, floor);
    const auto refit = [fitter] { fitter->fit(); };
    QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, fitter, refit);
    QObject::connect(list->model(), &QAbstractItemModel::rowsRemoved, fitter, refit);
    QObject::connect(list->model(), &QAbstractItemModel::modelReset, fitter, refit);
    QObject::connect(list->model(), &QAbstractItemModel::dataChanged, fitter, refit);
    QObject::connect(list->model(), &QAbstractItemModel::layoutChanged, fitter, refit);
    QObject::connect(list, &QListWidget::currentItemChanged, fitter, [fitter, list](QListWidgetItem *item) {
        if (item == nullptr || !list->hasFocus()) return;
        QTimer::singleShot(0, fitter, [list] {
            if (auto *current = list->currentItem()) {
                reveal(list->viewport(), list->visualItemRect(current));
            }
        });
    });
    fitter->fit();
}

}  // namespace AutoHeight
