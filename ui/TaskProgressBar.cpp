/**
 * @file TaskProgressBar.cpp
 * @brief 自绘进度条的实现（取舍见头文件）。
 */

#include "TaskProgressBar.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

#include "Animations.hpp"
#include "Theme.hpp"

namespace {

/// 不确定态那一截占整条多宽（比例）。约"三分之一条子"—— 一眼看得出它在动，又不至于
/// 大半条都是它。
constexpr qreal kBusyBandFraction = 0.32;

/// 那截带子滚一整圈多久（毫秒）。
constexpr qint64 kBusyPeriodMs = 1333;

/// 重画定时器的间隔（毫秒，≈ 一帧 60 Hz）。
constexpr int kBusyTickMs = 16;

/// 现在滚到哪儿（0..1）。按单调时钟算、不记在控件里：任务中心每次刷新都重建这根条，
/// 记在控件里的话下载时 200 ms 一刷，带子永远回到起点。关掉动效时恒 0（定住的那一格）。
qreal busyPhase() {
    static const QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return Anim::enabled() ? qreal(clock.elapsed() % kBusyPeriodMs) / qreal(kBusyPeriodMs) : 0.0;
}

}  // namespace

TaskProgressBar::TaskProgressBar(QWidget *parent) : QProgressBar(parent) {
    // 自绘的那一条**不画百分比文字**（数在标题行 / 右边那一格）。这一句同时对上
    // `ui_modernize.py audit` 的 "progress text enabled"（上游把文字打开的那一位）。
    setTextVisible(false);
    m_busyTimer = new QTimer(this);
    m_busyTimer->setInterval(kBusyTickMs);
    connect(m_busyTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void TaskProgressBar::syncBusyTimer() {
    const bool busy = minimum() == maximum();
    const bool need = busy && isVisible() && Anim::enabled();
    if (need == m_busyTimer->isActive()) {
        return;
    }
    if (need) {
        m_busyTimer->start();
    } else {
        m_busyTimer->stop();
    }
}

void TaskProgressBar::showEvent(QShowEvent *event) {
    QProgressBar::showEvent(event);
    syncBusyTimer();
}

void TaskProgressBar::hideEvent(QHideEvent *event) {
    QProgressBar::hideEvent(event);
    syncBusyTimer();
}

void TaskProgressBar::paintEvent(QPaintEvent *) {
    const int h = height();
    const int w = width();
    if (h <= 0 || w <= 0) {
        return;
    }
    // 顺手把"该不该有定时器"再对一遍：`setRange(0, 0)` 这种改动到不了显隐事件里。
    syncBusyTimer();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 色取**应用那一份**调色板，不取这一格自己的 —— 底栏那一条装在一颗挂着 QSS 的按钮
    // 里，那颗按钮的 QPalette 里 `Window` 是黑的，照它算会掉进深色那一档（同一个顾虑
    // 见 `MainWindowUI` 里给底栏选色那几处）。取应用那一份就不必操心谁先收到事件。
    const QPalette palette = QApplication::palette();
    const QColor track = Theme::progressTrackColor(palette);
    // 走满的那一根是「完成」：设计稿用成功绿。
    const QColor fill = !m_fullAccent && maximum() > minimum() && value() >= maximum() ? Theme::tokens().ok
                                                                       : Theme::progressFillColor(palette);

    if (m_ring) {
        const qreal side = qMin(w, h);
        const qreal r = side * 11.0 / 28.0;
        const QRectF circle(w / 2.0 - r, h / 2.0 - r, 2 * r, 2 * r);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Theme::tokens().stroke, side * 3.0 / 28.0));
        painter.drawEllipse(circle);
        painter.setPen(QPen(fill, side * 2.5 / 28.0, Qt::SolidLine, Qt::RoundCap));
        if (minimum() == maximum()) {
            // 测不出总量：一段三成长的弧绕圈转。
            painter.drawArc(circle, int((90 - 360 * busyPhase()) * 16), int(-0.3 * 360 * 16));
            return;
        }
        const int span = maximum() - minimum();
        const qreal ratio = span > 0 ? qBound(0.0, qreal(value() - minimum()) / qreal(span), 1.0) : 0.0;
        if (ratio > 0) painter.drawArc(circle, 90 * 16, int(-ratio * 360 * 16));
        return;
    }
    const qreal radius = h / 2.0;
    const QRectF rail(0, 0, w, h);
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(rail, radius, radius);

    // 不确定态：`minimum() == maximum()`（底栏"有东西在跑但说不清到哪"那一档）。
    if (minimum() == maximum()) {
        const qreal band = w * kBusyBandFraction;
        const qreal x = -band + (w + band * 2) * busyPhase();
        QLinearGradient gradient(x, 0, x + band, 0);
        QColor clear = fill;
        clear.setAlpha(0);
        gradient.setColorAt(0.0, clear);
        gradient.setColorAt(0.5, fill);
        gradient.setColorAt(1.0, clear);
        painter.setBrush(gradient);
        painter.drawRoundedRect(QRectF(x, 0, band, h), radius, radius);
        return;
    }

    // 确定态：按 `value()` 占 `range` 的比例把前一段涂满。两头圆角，露出来的那一截左端
    // 也是圆的（贴住轨道左边那半圆）。
    const int span = maximum() - minimum();
    if (span <= 0) {
        return;
    }
    const qreal ratio = qBound(0.0, qreal(value() - minimum()) / qreal(span), 1.0);
    const qreal fillWidth = w * ratio;
    if (fillWidth <= 0.0) {
        return;
    }
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, qMax(fillWidth, radius * 2.0), h), radius, radius);
}
