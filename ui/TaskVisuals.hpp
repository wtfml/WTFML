/**
 * @file TaskVisuals.hpp
 * @brief 任务详情页和任务中心（整页 / 浮动面板）共用的那几样画法。
 *
 * 7c 的任务详情页定了样子：状态 / 步骤行是「线条图标 + 名字 + 右侧注」，出错是浅红圆角块，
 * 底下一排操作「主钮 + 最多两颗扁平钮 + 更多…」。任务中心那一侧以前自己拿 √ ● ○ 字符拼，
 * 两处看着像两个程序。这里只放**画法**，不放数据：两边各自照 `TaskCenter::Item` 摆。
 */
#pragma once

#include <QFontMetrics>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSet>

#include "TaskCenterPanel.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

namespace TaskVisuals {

/// 一行「图标 + 字（+ 右侧注）」。`shown` 为空时就是普通 QLabel，图标画在 `glyphAt`；
/// `text()` 留给读屏 / 复制 / 验证工程（可以和屏幕上画的字不一样）。
class GlyphLabel : public QLabel {
public:
    using QLabel::QLabel;
    QString glyph;
    QColor glyphColor;
    int glyphSize = 16;
    int glyphGap = 8;
    QPoint glyphAt{0, 0};
    QString shown;
    QColor shownColor;
    QString note;
    QColor noteColor;
    QColor fill;
    int radius = 0;
    bool divider = false;

    QSize sizeHint() const override {
        if (shown.isEmpty()) return QLabel::sizeHint();
        const QMargins m = contentsMargins();
        const QFontMetrics fm(font());
        int w = m.left() + glyphSize + glyphGap + fm.horizontalAdvance(shown) + m.right() + 2;
        if (!note.isEmpty()) w += 24 + fm.horizontalAdvance(note);
        return {w, qMax(minimumHeight(), fm.height() + m.top() + m.bottom())};
    }
    QSize minimumSizeHint() const override {
        return shown.isEmpty() ? QLabel::minimumSizeHint() : QSize(0, sizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            if (fill.isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(fill);
                p.drawRoundedRect(QRectF(rect()), radius, radius);
            }
            if (divider) p.fillRect(QRect(0, height() - 1, width(), 1), Theme::tokens().divider);
        }
        if (shown.isEmpty()) {
            QLabel::paintEvent(event);
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            VersionIcons::paintLine(p, glyph, QRectF(glyphAt, QSizeF(glyphSize, glyphSize)), glyphColor);
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect r = contentsRect();
        const qreal gy = r.top() + (r.height() - glyphSize) / 2.0;
        VersionIcons::paintLine(p, glyph, QRectF(r.left(), gy, glyphSize, glyphSize), glyphColor);
        const QFontMetrics fm(font());
        p.setFont(font());
        int right = r.right();
        if (!note.isEmpty()) {
            const int nw = qMin(fm.horizontalAdvance(note), r.width() / 2);
            p.setPen(noteColor.isValid() ? noteColor : palette().color(QPalette::PlaceholderText));
            p.drawText(QRect(right - nw + 1, r.top(), nw, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                       fm.elidedText(note, Qt::ElideMiddle, nw));
            right -= nw + 24;
        }
        const int x = r.left() + glyphSize + glyphGap;
        p.setPen(shownColor.isValid() ? shownColor : palette().color(QPalette::WindowText));
        p.drawText(QRect(x, r.top(), right - x + 1, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(shown, Qt::ElideRight, right - x + 1));
    }
};

/// 一种状态 / 一步画成哪个图标、什么颜色（两处共用，颜色每次现取 = 跟着主题走）。
struct Glyph {
    QString name;
    QColor color;
};

inline Glyph stateGlyph(TaskCenter::State state, const QPalette &palette) {
    const Theme::Tokens t = Theme::tokens();
    switch (state) {
    case TaskCenter::State::Failed:
    case TaskCenter::State::Interrupted:
        return {QStringLiteral("xc"), Theme::textTone(t.dangerInk, t.danger)};
    case TaskCenter::State::Succeeded:
        return {QStringLiteral("okc"), Theme::textTone(t.okInk, t.ok)};
    case TaskCenter::State::Running:
    case TaskCenter::State::WaitingUser:
        return {QStringLiteral("dots"), Theme::textTone(t.accentInk, t.primary)};
    case TaskCenter::State::Paused:
        return {QStringLiteral("pause"), palette.color(QPalette::PlaceholderText)};
    case TaskCenter::State::Cancelled:
        return {QStringLiteral("stop"), palette.color(QPalette::PlaceholderText)};
    case TaskCenter::State::Queued:
        break;
    }
    return {QStringLiteral("clock"), palette.color(QPalette::PlaceholderText)};
}

inline Glyph stepGlyph(TaskCenter::StepState state, const QPalette &palette) {
    const Theme::Tokens t = Theme::tokens();
    switch (state) {
    case TaskCenter::StepState::Done: return {QStringLiteral("okc"), Theme::textTone(t.okInk, t.ok)};
    case TaskCenter::StepState::Failed: return {QStringLiteral("xc"), Theme::textTone(t.dangerInk, t.danger)};
    case TaskCenter::StepState::Running: return {QStringLiteral("dots"), Theme::textTone(t.accentInk, t.primary)};
    case TaskCenter::StepState::Pending: break;
    }
    return {QStringLiteral("clock"), palette.color(QPalette::PlaceholderText)};
}

/// 一颗只画图标的小方块（列表行最左边那个状态图标）。
inline GlyphLabel *makeGlyphIcon(const Glyph &glyph, int size, QWidget *parent) {
    auto *icon = new GlyphLabel(parent);
    icon->glyph = glyph.name;
    icon->glyphColor = glyph.color;
    icon->glyphSize = size;
    icon->setFixedSize(size, size);
    return icon;
}

/**
 * 底部那排操作：一颗主钮（实心）+ 最多 `maxShown` 颗次要钮（扁平），其余收进「更多…」菜单。
 *
 * 收进菜单的按钮**照旧存在**（只是藏起来）：菜单项点了就 `click()` 它，所以信号、能力位、
 * 验证工程按名字找按钮都不变 —— 判断"这一项有没有提供某个操作"用 `offers()`，不用 isVisible()。
 */
class ActionOverflow {
public:
    explicit ActionOverflow(QWidget *parent, const QString &objectName) {
        m_more = new QPushButton(QCoreApplication::translate("TaskVisuals", "更多…"), parent);
        m_more->setObjectName(objectName);
        m_more->setCursor(Qt::PointingHandCursor);
        Theme::markButton(m_more, Theme::ButtonKind::Ghost);
        m_menu = new QMenu(m_more);
        m_menu->setObjectName(objectName + QStringLiteral("Menu"));
        // 只捕按钮和菜单本身（它们跟着父控件走）：这个对象可能先于按钮被换掉。
        QObject::connect(m_more, &QPushButton::clicked, m_more, [more = m_more, menu = m_menu] {
            menu->popup(more->mapToGlobal(QPoint(0, -menu->sizeHint().height() - 4)));
        });
        m_more->hide();
    }
    QPushButton *button() const { return m_more; }
    QMenu *menu() const { return m_menu; }
    bool offers(const QPushButton *button) const {
        return button != nullptr && (!button->isHidden() || m_folded.contains(button));
    }

    /// @param primary 实心那一颗（可空）。@param secondaries 按优先级排好的次要钮（只放"该提供"的）。
    /// @param budget 这一排最多多宽（≤0 = 不限宽，只按 maxShown 收）；主钮、「更多…」都算在里头。
    void arrange(QPushButton *primary, const QList<QPushButton *> &secondaries, int maxShown, int budget = 0,
                 int spacing = 8) {
        m_folded.clear();
        int used = 0;
        if (primary != nullptr) {
            mark(primary, Theme::ButtonKind::Primary);
            primary->show();
            used += primary->sizeHint().width() + spacing;
        }
        QList<QPushButton *> shown;
        for (QPushButton *b : secondaries) {
            mark(b, Theme::ButtonKind::Ghost);
            const int w = b->sizeHint().width() + spacing;
            if (shown.size() < maxShown && (budget <= 0 || used + w <= budget)) {
                shown.append(b);
                used += w;
            } else {
                m_folded.insert(b);
            }
        }
        // 有东西进了菜单、而「更多…」本身摆不下：退回最后露出的那一颗。
        if (!m_folded.isEmpty() && budget > 0 && !shown.isEmpty()
            && used + m_more->sizeHint().width() > budget) m_folded.insert(shown.takeLast());
        for (QPushButton *b : secondaries) b->setVisible(shown.contains(b));
        m_menu->clear();
        for (QPushButton *b : secondaries) {
            if (!m_folded.contains(b)) continue;
            QAction *action = m_menu->addAction(b->text());
            action->setObjectName(b->objectName() + QStringLiteral("Action"));
            QObject::connect(action, &QAction::triggered, b, [b] { b->click(); });
        }
        m_more->setVisible(!m_folded.isEmpty());
    }

private:
    static void mark(QPushButton *b, Theme::ButtonKind kind) {
        if (b->property("wtfmlButtonKind").toString() != Theme::buttonKindKey(kind)) Theme::markButton(b, kind);
    }
    QPushButton *m_more = nullptr;
    QMenu *m_menu = nullptr;
    QSet<const QPushButton *> m_folded;
};

}  // namespace TaskVisuals
