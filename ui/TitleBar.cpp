/**
 * @file TitleBar.cpp
 * @brief 左导航轨与窗口按钮条的摆法与画法。
 */

#include "TitleBar.hpp"

#include "Animations.hpp"
#include "BrandMark.hpp"
#include "Glass.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QEasingCurve>
#include <QFile>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <QShowEvent>

// `Q_INIT_RESOURCE` 不能写在命名空间里（见 ui/AppIcon.hpp）。
static void initIconResources() { Q_INIT_RESOURCE(icons); }

namespace {

// 设计稿第二批 tokens.md 第 1、2 节。
constexpr int kRailWidth = 208;
constexpr int kRailMiniWidth = 64;
constexpr int kRailPadding = 12;
constexpr int kRailMiniPadding = 10;
constexpr int kRailTopPadding = 14;
constexpr int kNarrowBreakpoint = 1024;
constexpr int kNavItemWidth = kRailWidth - 2 * kRailPadding;
constexpr int kNavItemHeight = 42;
constexpr int kNavSpacing = 4;
constexpr int kNavFontPixels = 14;  // 样张 r2：导航用系统字 14 / 500，手写体只留给品牌与主页大标题
constexpr int kNavIconSize = 20;
constexpr int kNavIconGap = 14;
constexpr int kNavPaddingX = 14;
/// 品牌那一行：高 44，下面再空 18 才是第一颗导航。
constexpr int kBrandHeight = 44;
constexpr int kBrandGap = 18;
constexpr int kLogoSize = 26;  // 手写 W 横向撑满这一格（字形本身扁，高约 17）
constexpr int kCaptionHeight = 40;
constexpr int kWindowButtonWidth = 46;
constexpr int kWindowButtonsRightMargin = 0;
constexpr int kRadius = 10;
constexpr int kHoverAnimationMs = 120;
constexpr int kSlideAnimationMs = 180;
/// 比这个窄就按图标轨画（宽度动画过程中也按这条判）。
constexpr int kCompactButtonWidth = 80;
const QColor kCloseHover(0xC4, 0x2B, 0x1C);
const QColor kClosePressed(0xA8, 0x25, 0x18);

QColor blendedColor(const QColor &from, const QColor &to, qreal progress) {
    const qreal t = qBound(0.0, progress, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t,
                            from.alphaF() + (to.alphaF() - from.alphaF()) * t);
}

TitleBarButton::NavIcon iconForIndex(int index) {
    // 按**序号**认，不按文字：条上的字是 `MainWindowUI` 那五个标签的文本，会跟着界面语言变
    //（装了英文翻译时就是英文）。按文字认等于每加一门语言都要来这里加一列，漏了就是几个
    // "没有图标"的按钮。
    //
    // 顺序契约：和 `MainWindowUI` 里那五个 `addTab` 的先后一致（home / instance / download /
    // settings / agent，见 `homeTabIndex()` 那一组常量）。加语言不动这里；**重排页面**才要动。
    switch (index) {
    case 0: return TitleBarButton::NavIcon::Home;
    case 1: return TitleBarButton::NavIcon::Instances;
    case 2: return TitleBarButton::NavIcon::Download;
    case 3: return TitleBarButton::NavIcon::Settings;
    case 4: return TitleBarButton::NavIcon::Agent;
    default: break;
    }
    return TitleBarButton::NavIcon::None;
}

QString iconName(TitleBarButton::NavIcon icon) {
    switch (icon) {
    case TitleBarButton::NavIcon::Home: return QStringLiteral("home");
    case TitleBarButton::NavIcon::Instances: return QStringLiteral("manage");
    case TitleBarButton::NavIcon::Download: return QStringLiteral("download");
    case TitleBarButton::NavIcon::Settings: return QStringLiteral("settings");
    case TitleBarButton::NavIcon::Agent: return QStringLiteral("agent");
    case TitleBarButton::NavIcon::Toolbox: return QStringLiteral("toolbox");
    case TitleBarButton::NavIcon::None: break;
    }
    return QString();
}

int railPaddingFor(int width) { return width < (kRailWidth + kRailMiniWidth) / 2 ? kRailMiniPadding : kRailPadding; }

/// 开程序时品牌标写一遍：书写中线（24 px 专版坐标，w24.py 从 256 版中线映射，遮罩全放出覆盖 99%）。
struct WritePoint { float x, y, r, s, t; };
const WritePoint kWritePoints[] = {
#include "BrandWriteData.inc"
};

/// 写到第 @p progress（0..1，按书写时刻）时，沿中线放出的遮罩（圆头变宽笔）。
void paintWriteMask(QPainter &mask, qreal progress) {
    const int n = int(sizeof(kWritePoints) / sizeof(kWritePoints[0]));
    // 书写时刻 → 弧长（t 单调），手写节奏（三分之二幂律）已经在 t 里
    qreal s = kWritePoints[n - 1].s;
    for (int i = 1; i < n; ++i) {
        if (kWritePoints[i].t >= progress) {
            const WritePoint &a = kWritePoints[i - 1], &b = kWritePoints[i];
            const qreal u = b.t > a.t ? (progress - a.t) / (b.t - a.t) : 1.0;
            s = a.s + (b.s - a.s) * u;
            break;
        }
    }
    for (int i = 1; i < n && kWritePoints[i - 1].s < s; ++i) {
        const WritePoint &a = kWritePoints[i - 1], &b = kWritePoints[i];
        QPointF to(b.x, b.y);
        qreal r = b.r;
        if (b.s > s) {
            const qreal u = (s - a.s) / (b.s - a.s);
            to = QPointF(a.x, a.y) + (to - QPointF(a.x, a.y)) * u;
            r = a.r + (b.r - a.r) * u;
        }
        mask.setPen(QPen(Qt::black, 2 * r, Qt::SolidLine, Qt::RoundCap));
        mask.drawLine(QPointF(a.x, a.y), to);
    }
}

/// 品牌标「手写的 W」：设计稿 24 px 专版（`assets/app/wtfml-24.svg`，一条只有 M/L/Z 的填充路径），
/// 读成 24×24 坐标里的 QPainterPath，画多大缩多大、颜色由调用方给。
const QPainterPath &brandMarkPath() {
    initIconResources();
    static const QPainterPath path = BrandMark::pathFromSvg(QStringLiteral(":/assets/app/wtfml-24.svg"));
    return path;
}

using BrandMark::ensureGraphicContrast;
using BrandMark::worstContrast;

/// 窗口按钮的墨：在 @p under（这颗按钮底下此刻实测的两端）上不够 3:1 时，往这一套主题里
/// heroInk / heroVeil（一深一浅）中对比度更高的那头小步挪，刚够就停（和品牌标同一个逼近）。
/// 默认风景本来就够，原样返回。
QColor legibleCaptionInk(const QColor &ink, const QList<QColor> &under) {
    const Theme::Tokens t = Theme::tokens();
    if (worstContrast(ink, under) >= 3.0) return ink;
    const bool light = worstContrast(t.heroInk, under) >= worstContrast(t.heroVeil, under);
    return ensureGraphicContrast(ink, light ? t.heroInk : t.heroVeil, under);
}

}  // namespace

QString TitleBar::defaultBrandText() { return QStringLiteral("WTFML"); }
int TitleBar::railWidth() { return kRailWidth; }
int TitleBar::collapsedRailWidth() { return kRailMiniWidth; }
int TitleBar::narrowBreakpoint() { return kNarrowBreakpoint; }
int TitleBar::barHeight() { return kCaptionHeight; }

void TitleBar::styleCaptionButtons(QAbstractButton *minimize, QAbstractButton *maximize,
                                   QAbstractButton *close, qreal hero) {
    const Theme::Tokens t = Theme::tokens();
    const QColor captionInk = blendedColor(t.secondary, t.heroInk2, hero);
    const QColor captionHoverInk = blendedColor(t.text, t.heroInk, hero);
    for (QAbstractButton *b : {minimize, maximize}) {
        if (auto *button = qobject_cast<TitleBarButton *>(b)) {
            button->setHoverColors(t.hover, captionHoverInk);
            button->setPressColor(t.fillPress);
            button->setInkColor(captionInk);
        }
    }
    if (auto *button = qobject_cast<TitleBarButton *>(close)) {
        button->setHoverColors(kCloseHover, QColor(Qt::white));
        button->setPressColor(kClosePressed);
        button->setInkColor(captionInk);
    }
}
int TitleBar::buttonSize() { return kCaptionHeight; }
int TitleBar::windowButtonWidth() { return kWindowButtonWidth; }
int TitleBar::buttonInset() { return 0; }
int TitleBar::navItemWidth() { return kNavItemWidth; }
int TitleBar::navItemHeight() { return kNavItemHeight; }
int TitleBar::navFontPixelSize() { return kNavFontPixels; }
int TitleBar::windowButtonsRightMargin() { return kWindowButtonsRightMargin; }

TitleBarButton::TitleBarButton(Glyph glyph, const QColor &hoverColor, const QColor &hoverInk, QWidget *parent)
    : QAbstractButton(parent), m_glyph(glyph), m_hoverColor(hoverColor), m_hoverInk(hoverInk) {
    m_ink = palette().color(QPalette::WindowText);
    if (glyph == Glyph::Minimize || glyph == Glyph::Maximize || glyph == Glyph::Close) {
        setFixedSize(kWindowButtonWidth, kCaptionHeight);
    } else if (glyph == Glyph::Logo) {
        setFixedSize(kLogoSize, kLogoSize);
    }
    // Qt 不会自己产生 HoverEnter/HoverLeave：WA_Hover + mouseTracking 两条缺一不可。
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    // 只有品牌标 W 进 Tab 序（它是轨上唯一不在别处有键盘入口的开关）；Tab 才给焦点，鼠标点不抢焦点。
    setFocusPolicy(glyph == Glyph::Logo ? Qt::TabFocus : Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    m_hoverAnimation = new QVariantAnimation(this);
    m_hoverAnimation->setDuration(kHoverAnimationMs);
    m_hoverAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hoverAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_hoverProgress = value.toReal();
        update();
    });
}

void TitleBarButton::setNavIcon(NavIcon icon) {
    m_icon = icon;
    update();
}

void TitleBarButton::setBaseColor(const QColor &color) {
    m_baseColor = color;
    update();
}

void TitleBarButton::setHoverColors(const QColor &hoverColor, const QColor &hoverInk) {
    m_hoverColor = hoverColor;
    m_hoverInk = hoverInk;
    update();
}

void TitleBarButton::setPressColor(const QColor &color) {
    m_pressColor = color;
    update();
}

void TitleBarButton::setInkColor(const QColor &color) {
    m_ink = color;
    update();
}

void TitleBarButton::setCurrent(bool current) {
    if (m_current == current) return;
    m_current = current;
    update();
}

void TitleBarButton::setRestoreGlyph(bool restore) {
    if (m_restore == restore) return;
    m_restore = restore;
    update();
}

void TitleBarButton::setWriteProgress(qreal progress) {
    progress = qBound(0.0, progress, 1.0);
    if (m_writeProgress == progress) return;
    m_writeProgress = progress;
    update();
}

bool TitleBarButton::event(QEvent *event) {
    // 焦点是窗口激活时自动落上来的：头一下 Tab / Shift+Tab 就地亮出焦点框（留在 W），不跳过它去下一个。
    if (event->type() == QEvent::KeyPress && hasFocus() && !m_keyboardFocus) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
            m_keyboardFocus = true;
            update();
            return true;
        }
    }
    const bool enter = event->type() == QEvent::HoverEnter;
    if (enter || event->type() == QEvent::HoverLeave) {
        m_hovered = enter;
        m_hoverAnimation->stop();
        if (!Anim::enabled()) {
            m_hoverProgress = enter ? 1.0 : 0.0;
            update();
        } else {
            m_hoverAnimation->setStartValue(m_hoverProgress);
            m_hoverAnimation->setEndValue(enter ? 1.0 : 0.0);
            m_hoverAnimation->start();
        }
    }
    return QAbstractButton::event(event);
}

void TitleBarButton::focusInEvent(QFocusEvent *event) {
    QAbstractButton::focusInEvent(event);
    // Qt 藏起有焦点的控件（鼠标关浮层、换页）时也把焦点按 TabFocusReason 交给链上下一个，
    // 所以光看理由不够：还要"最后一次输入是键盘"。后者读 Theme 的焦点过滤器在 FocusIn 之前挂的
    // wtfmlKeyboardFocus（只在此刻读一次，不跟着后来的按键变，窗口激活后按回车才不会收放导航）。
    const bool tabReason = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
    m_keyboardFocus = tabReason && property("wtfmlKeyboardFocus").toBool();
    update();
}

void TitleBarButton::focusOutEvent(QFocusEvent *event) {
    QAbstractButton::focusOutEvent(event);
    m_keyboardFocus = false;
    update();
}

void TitleBarButton::keyPressEvent(QKeyEvent *event) {
    // 窗口激活时自动落到 W 上的焦点不接键：不然一启动按回车 / 空格就把导航收放了。
    if (!m_keyboardFocus) {
        event->ignore();
        return;
    }
    // 空格 QAbstractButton 自己认；回车也当一次点击（品牌标 W 的收展导航）。
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        click();
        return;
    }
    QAbstractButton::keyPressEvent(event);
}

void TitleBarButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (m_glyph == Glyph::Logo) {
        // 颜色先按主题取（BrandMark::stops()），再补对比度：底色取这一格在**任一页**可能落到的
        // （主页清晰风景 + 别的页磨砂底），墨色固定用轨的正文色，所以换页、风景淡入淡出时颜色不跳。
        // 自定义背景图很亮 / 很暗时字靠着色层兜底，这枚小图形靠往墨色挪。
        const QPair<QColor, QColor> stops = BrandMark::stops();
        QList<QColor> under{Theme::tokens().page};
        if (const Glass::Backdrop *backdrop = Glass::Backdrop::of(this)) under = backdrop->colorsUnderAnyPage(this);
        if (Theme::partHasBackground(Theme::Part::TitleBar)) {
            const QColor railFill = Theme::partBackground(Theme::Part::TitleBar);
            for (QColor &c : under) c = Glass::over(railFill, c);
        }
        // 按字形外框等比撑满这一格（SVG 画布四周留着白，照 24 缩会小一圈）。
        const QRectF box = brandMarkPath().boundingRect();
        const qreal scale = qMin(width() / box.width(), height() / box.height());
        p.translate((width() - box.width() * scale) / 2.0, (height() - box.height() * scale) / 2.0);
        p.scale(scale, scale);
        p.translate(-box.topLeft());
        QLinearGradient gradient(0, 4.8, 0, 19.2);  // 设计稿 24 专版的渐变区间（路径坐标）
        gradient.setColorAt(0, ensureGraphicContrast(stops.first, m_ink, under));
        gradient.setColorAt(1, ensureGraphicContrast(stops.second, m_ink, under));
        if (m_writeProgress < 1.0) {
            // 写到一半：原填充画进图层，再用遮罩 DestinationIn 裁（不用 setClipPath，它不抗锯齿）。
            const qreal dpr = devicePixelRatioF();
            QImage layer(size() * dpr, QImage::Format_ARGB32_Premultiplied);
            layer.setDevicePixelRatio(dpr);
            layer.fill(Qt::transparent);
            QImage mask = layer;
            const QTransform glyph = p.transform();
            if (m_writeProgress > 0.0) {
                QPainter m(&mask);
                m.setRenderHint(QPainter::Antialiasing, true);
                m.setTransform(glyph);
                paintWriteMask(m, m_writeProgress);
                QPainter l(&layer);
                l.setRenderHint(QPainter::Antialiasing, true);
                l.setTransform(glyph);
                l.fillPath(brandMarkPath(), gradient);
                l.resetTransform();
                l.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                l.drawImage(0, 0, mask);
            }
            p.resetTransform();
            p.drawImage(0, 0, layer);
            return;
        }
        p.fillPath(brandMarkPath(), gradient);
        if (hasKeyboardFocus()) {
            // 键盘焦点：W 底下一笔主题色的手写下划线（像签名收笔），不画框。
            // 只认 Tab / Shift+Tab 选来的焦点，窗口激活时自动落上来的不画。
            p.resetTransform();
            const qreal bottom = (height() + box.height() * scale) / 2.0;  // 字形下缘（格子上下各留约 4.5）
            // 两条二次曲线围成一片两头尖、中间约 2 px、右端上挑的笔画（比等粗的线更像笔写的）。
            const QPointF start(width() * 0.12, bottom + 1.8);
            const QPointF end(width() * 0.9, bottom + 0.4);
            QPainterPath swash(start);
            swash.quadTo(QPointF(width() * 0.5, bottom + 2.4), end);
            swash.quadTo(QPointF(width() * 0.5, bottom + 6.4), start);
            p.fillPath(swash, Theme::focusRingColor(palette()));
        }
        return;
    }
    if (!isEnabled()) p.setOpacity(0.6);
    // 当前那颗的底由轨自己画（会滑），这里只画悬停 / 按下那层中性灰。
    QColor base = blendedColor(m_baseColor, m_current ? QColor(Qt::transparent) : m_hoverColor, m_hoverProgress);
    if (isDown() && !m_current && m_pressColor.isValid()) base = m_pressColor;
    if (base.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(base);
        if (m_glyph == Glyph::Close) {
            // 窗口右上角那颗：只有右上角跟窗口一起圆（8），其余直角。
            QPainterPath shape;
            const QRectF r(rect());
            shape.moveTo(r.topLeft());
            shape.lineTo(r.right() - 8, r.top());
            shape.quadTo(r.topRight(), QPointF(r.right(), r.top() + 8));
            shape.lineTo(r.bottomRight());
            shape.lineTo(r.bottomLeft());
            shape.closeSubpath();
            p.drawPath(shape);
        } else if (m_glyph == Glyph::Minimize || m_glyph == Glyph::Maximize) {
            p.drawRect(rect());
        } else {
            p.drawRoundedRect(QRectF(rect()), kRadius, kRadius);
        }
    }
    // 窗口按钮压在用户自定义的亮背景图上时，墨按底下实测的颜色补到 3:1（关闭那颗悬停是红底白字，不动）。
    QColor rest = m_ink, hoverInk = m_hoverInk;
    if (m_glyph == Glyph::Minimize || m_glyph == Glyph::Maximize || m_glyph == Glyph::Close) {
        if (const Glass::Backdrop *backdrop = Glass::Backdrop::of(this)) {
            const QList<QColor> under = backdrop->colorsUnder(this);
            rest = legibleCaptionInk(m_ink, under);
            if (m_glyph != Glyph::Close) hoverInk = legibleCaptionInk(m_hoverInk, under);
        }
    }
    const QColor ink = !isEnabled() ? Theme::tokens().disabled
                       : m_current  ? rest
                                    : blendedColor(rest, hoverInk, m_hoverProgress);
    switch (m_glyph) {
    case Glyph::Text: {
        const bool compact = width() < kCompactButtonWidth;
        const qreal iconX = compact ? (width() - kNavIconSize) / 2.0 : kNavPaddingX;
        const QRectF iconBox(iconX, (height() - kNavIconSize) / 2.0, kNavIconSize, kNavIconSize);
        VersionIcons::paintLine(p, iconName(m_icon), iconBox, ink);
        if (compact) return;
        p.setFont(font());
        p.setPen(ink);
        const int textLeft = int(iconBox.right()) + kNavIconGap;
        p.drawText(QRect(textLeft, 0, width() - textLeft - 8, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(font()).elidedText(text(), Qt::ElideRight, width() - textLeft - 8));
        return;
    }
    case Glyph::Minimize:
        VersionIcons::paintLine(p, QStringLiteral("min"), QRectF((width() - 16) / 2.0, (height() - 16) / 2.0, 16, 16), ink, 1.4 * 24 / 16);
        return;
    case Glyph::Maximize:
        VersionIcons::paintLine(p, m_restore ? QStringLiteral("restore") : QStringLiteral("max"),
                                QRectF((width() - 15) / 2.0, (height() - 15) / 2.0, 15, 15), ink, 1.4 * 24 / 15);
        return;
    case Glyph::Close:
        VersionIcons::paintLine(p, QStringLiteral("close"), QRectF((width() - 16) / 2.0, (height() - 16) / 2.0, 16, 16), ink, 1.4 * 24 / 16);
        return;
    case Glyph::Logo:
        return;
    }
}

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("titleBar"));
    setFixedWidth(kRailMiniWidth);  // 每次启动都从图标轨开始（m_userCollapsed 默认 true）
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_pillAnimation = new QVariantAnimation(this);
    m_pillAnimation->setDuration(kSlideAnimationMs);
    m_pillAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_pillAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_pillProgress = value.toReal();
        update();
    });
    m_widthAnimation = new QVariantAnimation(this);
    m_widthAnimation->setDuration(kSlideAnimationMs);
    {
        // 设计稿动效曲线 cubic-bezier(.2,.8,.2,1)。
        QEasingCurve curve(QEasingCurve::BezierSpline);
        curve.addCubicBezierSegment(QPointF(0.2, 0.8), QPointF(0.2, 1.0), QPointF(1.0, 1.0));
        m_widthAnimation->setEasingCurve(curve);
        m_pillAnimation->setEasingCurve(curve);
    }
    connect(m_widthAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        layoutForWidth(value.toInt());
    });

    // 右上那条：透明，两颗窗口按钮（最小化、关闭）贴顶贴右（调用方把它摆进右栏顶上）。
    m_caption = new QWidget(this);
    m_caption->setObjectName(QStringLiteral("titleBarCaption"));
    m_caption->setFixedHeight(kCaptionHeight);
    auto *captionLayout = new QHBoxLayout(m_caption);
    captionLayout->setContentsMargins(0, 0, kWindowButtonsRightMargin, 0);
    captionLayout->setSpacing(0);
    captionLayout->addStretch(1);
    m_minimize = new TitleBarButton(TitleBarButton::Glyph::Minimize, QColor(), QColor(), m_caption);
    m_minimize->setObjectName(QStringLiteral("titleBarMinimize"));
    m_minimize->setToolTip(tr("最小化"));
    m_close = new TitleBarButton(TitleBarButton::Glyph::Close, QColor(), QColor(), m_caption);
    m_close->setObjectName(QStringLiteral("titleBarClose"));
    m_close->setToolTip(tr("关闭"));
    captionLayout->addWidget(m_minimize, 0, Qt::AlignTop);
    captionLayout->addWidget(m_close, 0, Qt::AlignTop);
    connect(m_minimize, &QAbstractButton::clicked, this, &TitleBar::minimizeRequested);
    connect(m_close, &QAbstractButton::clicked, this, &TitleBar::closeRequested);

    // 品牌行：手写的 W（点它收起 / 展开导航；窄窗强制图标轨时点了不动）+ 名字（画的，不可点）。
    m_logo = new TitleBarButton(TitleBarButton::Glyph::Logo, QColor(), QColor(), this);
    m_logo->setObjectName(QStringLiteral("titleBarLogo"));
    connect(m_logo, &QAbstractButton::clicked, this, [this]() {
        if (!m_narrow) setCollapsed(!m_userCollapsed);
    });

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(kRailPadding, kRailTopPadding + kBrandHeight + kBrandGap, kRailPadding, kRailTopPadding);
    m_layout->setSpacing(0);
    m_navLayout = new QVBoxLayout;
    m_navLayout->setContentsMargins(0, 0, 0, 0);
    m_navLayout->setSpacing(kNavSpacing);
    m_layout->addLayout(m_navLayout);
    m_layout->addStretch(1);
    layoutForWidth(kRailMiniWidth);
    applyColors();
}

void TitleBar::setFooter(QWidget *footer) {
    if (m_footer != nullptr) {
        m_layout->removeWidget(m_footer);
        m_footer->deleteLater();
    }
    m_footer = footer;
    if (footer != nullptr) m_layout->addWidget(footer);
}

void TitleBar::applyColors() {
    if (m_applyingColors) return;
    m_applyingColors = true;
    const Theme::Tokens t = Theme::tokens();
    QPalette railPalette = QApplication::palette();
    // 只改可用 / 非活动两组：Disabled 那一档灰要留着（轨底的实例状态方块「没在跑」就取它）。
    railPalette.setColor(QPalette::Active, QPalette::WindowText, t.text);
    railPalette.setColor(QPalette::Inactive, QPalette::WindowText, t.text);
    setPalette(railPalette);
    setAutoFillBackground(false);
    styleCaptionButtons(m_minimize, nullptr, m_close, m_heroOpacity);
    // 品牌标补对比度时往这一处的字色挪：固定正文色，不随主页换（换页不变色）。
    if (auto *logo = qobject_cast<TitleBarButton *>(m_logo)) logo->setInkColor(t.text);
    for (int i = 0; i < m_navButtons.size(); ++i) {
        if (auto *button = qobject_cast<TitleBarButton *>(m_navButtons.at(i))) {
            button->setHoverColors(t.hover, t.text);
            button->setPressColor(t.fillPress);
            button->setInkColor(i == m_currentNav ? t.deep : t.secondary);
        }
    }
    m_applyingColors = false;
    update();
}

void TitleBar::changeEvent(QEvent *event) {
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
        applyColors();
        break;
    default:
        break;
    }
    QWidget::changeEvent(event);
}

void TitleBar::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!m_watchingWindow && window() != this) {
        m_watchingWindow = true;
        window()->installEventFilter(this);
    }
    if (window() != this) {
        const bool narrow = window()->width() < kNarrowBreakpoint;
        if (narrow != m_narrow) {
            m_narrow = narrow;
            applyWidth(false);
        }
    }
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == window()) {
        if (event->type() == QEvent::Resize) {
            const bool narrow = window()->width() < kNarrowBreakpoint;
            if (narrow != m_narrow) {
                const bool wasCollapsed = isCollapsed();
                m_narrow = narrow;
                if (wasCollapsed != isCollapsed()) applyWidth(isVisible());
                else layoutForWidth(width());
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TitleBar::setCollapsed(bool collapsed) {
    if (m_userCollapsed == collapsed) return;
    const bool wasCollapsed = isCollapsed();
    m_userCollapsed = collapsed;
    if (wasCollapsed != isCollapsed()) applyWidth(isVisible());
    else layoutForWidth(width());
}

void TitleBar::applyWidth(bool animate) {
    const int target = isCollapsed() ? kRailMiniWidth : kRailWidth;
    m_widthAnimation->stop();
    if (animate && Anim::enabled() && width() != target) {
        m_widthAnimation->setStartValue(width());
        m_widthAnimation->setEndValue(target);
        m_widthAnimation->start();
    } else {
        layoutForWidth(target);
    }
    emit collapsedChanged(isCollapsed());
}

void TitleBar::layoutForWidth(int w) {
    setFixedWidth(w);
    const int pad = railPaddingFor(w);
    m_layout->setContentsMargins(pad, kRailTopPadding + kBrandHeight + kBrandGap, pad, kRailTopPadding);
    for (QAbstractButton *button : std::as_const(m_navButtons)) button->setFixedSize(w - 2 * pad, kNavItemHeight);
    const bool compact = w < (kRailWidth + kRailMiniWidth) / 2;
    // 品牌行：展开时 padding 0 10，图标轨时 0 11（W 对准导航图标）。
    // 中线对准导航图标：图标轨时导航图标居中，展开时图标中心在 pad + 14 + 10。
    const int logoX = compact ? (w - kLogoSize) / 2 : pad + kNavPaddingX + kNavIconSize / 2 - kLogoSize / 2;
    m_logo->move(logoX, kRailTopPadding + (kBrandHeight - kLogoSize) / 2);
    const QString action = isCollapsed() ? tr("展开导航") : tr("收起导航");
    m_logo->setCursor(m_narrow ? Qt::ArrowCursor : Qt::PointingHandCursor);
    m_logo->setToolTip(m_narrow ? tr("窗口加宽后才能展开") : action);
    m_logo->setAccessibleName(action);
    m_logo->setAccessibleDescription(m_narrow ? tr("窗口加宽后才能展开") : QString());
    update();
}

void TitleBar::setHeroOpacity(qreal opacity) {
    if (m_heroOpacity == opacity) return;
    m_heroOpacity = opacity;
    styleCaptionButtons(m_minimize, nullptr, m_close, m_heroOpacity);
}

void TitleBar::setNavItems(const QStringList &labels) {
    for (QAbstractButton *button : m_navButtons) {
        m_navLayout->removeWidget(button);
        button->setParent(nullptr);
        button->deleteLater();
    }
    m_navButtons.clear();
    m_currentNav = -1;
    QFont navFont = font();
    navFont.setPixelSize(kNavFontPixels);
    navFont.setWeight(QFont::Medium);
    const int pad = railPaddingFor(width());
    for (int i = 0; i < labels.size(); ++i) {
        auto *button = new TitleBarButton(TitleBarButton::Glyph::Text, QColor(), QColor(), this);
        button->setObjectName(QStringLiteral("titleBarNav%1").arg(i));
        button->setLabel(labels.at(i));
        button->setNavIcon(iconForIndex(i));
        button->setFont(navFont);
        button->setFixedSize(width() - 2 * pad, kNavItemHeight);
        button->setAccessibleName(labels.at(i));
        button->setToolTip(labels.at(i));
        m_navButtons.append(button);
        m_navLayout->addWidget(button);
        connect(button, &QAbstractButton::clicked, this, [this, i]() { emit navRequested(i); });
    }
    applyColors();
}

void TitleBar::setCurrentNav(int index) {
    const int previous = m_currentNav;
    m_currentNav = (index >= 0 && index < m_navButtons.size()) ? index : -1;
    m_pillAnimation->stop();
    if (previous >= 0 && previous != m_currentNav && Anim::enabled() && isVisible()) {
        m_pillFrom = previous;
        m_pillProgress = 0.0;
        m_pillAnimation->setStartValue(0.0);
        m_pillAnimation->setEndValue(1.0);
        m_pillAnimation->start();
    } else {
        m_pillFrom = -1;
        m_pillProgress = 1.0;
    }
    for (int i = 0; i < m_navButtons.size(); ++i) {
        if (auto *button = qobject_cast<TitleBarButton *>(m_navButtons.at(i))) button->setCurrent(i == m_currentNav);
    }
    applyColors();
}

QAbstractButton *TitleBar::navButton(int index) const {
    return index >= 0 && index < m_navButtons.size() ? m_navButtons.at(index) : nullptr;
}

void TitleBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme::Tokens t = Theme::tokens();
    // 轨本身是磨砂玻璃；用户在个性化里给「标题栏」单独设过底色时才铺那一层。
    if (Theme::partHasBackground(Theme::Part::TitleBar))
        p.fillRect(rect(), Theme::partBackground(Theme::Part::TitleBar));

    // 选中那一格：accent-soft 圆角 10 淡底（不画指示条，tokens-r2 §6），从上一颗滑过来。
    if (QAbstractButton *to = navButton(m_currentNav)) {
        QRectF rect = to->geometry();
        if (QAbstractButton *from = navButton(m_pillFrom); from != nullptr && m_pillProgress < 1.0) {
            const QRectF a = from->geometry();
            rect.moveTop(a.top() + (rect.top() - a.top()) * m_pillProgress);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(t.selected);
        p.drawRoundedRect(rect, kRadius, kRadius);
    }

    // 品牌名（西文手写体 30）；图标轨时只剩 W。
    // NOTE: 固定是商标，不给改（UI 问题 #3）—— 出处只有 `defaultBrandText()` 这一处。
    if (width() < (kRailWidth + kRailMiniWidth) / 2) return;
    const QString brand = defaultBrandText();
    if (brand.isEmpty()) return;
    QFont brandFont = Theme::handFont(font(), Theme::HandFont::Script);
    brandFont.setPixelSize(30);
    brandFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    p.setFont(brandFont);
    p.setPen(t.text);  // 品牌字不随主页换色：heroInk 与 text 三主题里明暗同档，压在风景上照样读得清
    const int left = m_logo->geometry().right() + 1 + 10;
    const int right = width() - kRailPadding;
    const QRect textRect(left, kRailTopPadding + 2, right - left, kBrandHeight);
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(brandFont).elidedText(brand, Qt::ElideRight, textRect.width()));
}
