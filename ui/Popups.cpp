/**
 * @file Popups.cpp
 * @brief 浮层那一套（tooltip / 下拉弹层 / 菜单）的实现在这里，对外只有 `Popups.hpp` 那几个口子。
 *
 * 这一层**不碰窗口**：`Glass` 那一份失焦磨砂、`WindowChrome` 的边框 / 圆角都不归这里管。
 * 这里只借 `Glass::systemBackdrop()` 问一句"系统背板现在能不能用"，然后给**浮层自己的
 * 顶层窗口**挂 DWM 瞬态亚克力（`DWMSBT_TRANSIENTWINDOW`）；挂不上（Win10 / 关透明 /
 * 高对比度）就只剩自绘的那层着色玻璃 —— 观感仍是"一块磨砂"，只是背后不是真模糊。
 */

#include "Popups.hpp"

#include "Animations.hpp"
#include "Glass.hpp"
#include "GlassPopover.hpp"
#include "Theme.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {

// 规格（`.tongxi/设计稿/r2`，tokens-r2 §3 / §6）
constexpr qreal kTooltipRadius = 8;                       // 提示比弹层小一号
constexpr qreal kPopupRadius = GlassPopover::kRadius;     // 14：和窗口里那些浮层卡片同一个圆角
constexpr int kPopupFadeMs = GlassPopover::kFadeMs;       // 150ms：同上，同一个淡入时长
constexpr int kPopupPadding = 6;        // 弹层内边距
constexpr int kTooltipDelayMs = 500;    // 停 500ms 才出
constexpr int kTooltipWakeMs = 1000;    // 收起后 1 秒内换到相邻控件：不等，立刻出
constexpr int kTooltipMaxTextWidth = 420;  // 正文最宽 420：到了就换行，字少时框跟着变窄（框高只随行数走）
constexpr QPoint kTooltipCursorOffset(14, 22);  // 落在鼠标右下方，错开指针本身
constexpr int kRowHeight = 30;          // 下拉项高：照 PCL 那种紧凑（它 26），本仓正文 14px 留够点中的余地
constexpr int kPopupGap = 4;            // 弹层顶边离框底边多远（挂在框正下方，不盖框）
constexpr int kItemTextInset = 12;      // 项里文字左右各让出多少（paint 里 box 缩 4 + 文字缩 8）
constexpr int kMaxVisibleItems = 8;     // 最多露 8 项，超出在弹层里滚
constexpr int kSearchAfterItems = 15;   // 超过这么多项，顶部加搜索框
constexpr int kThumbFadeMs = 600;       // 细条停 600ms 淡出
constexpr int kFadeEdgePx = 16;         // 上下渐隐那一条的高度
constexpr int kShadowSpread = 18;       // 浮层阴影向四周铺开的宽度
constexpr int kShadowDropY = 4;         // 阴影整体下移

// ---------------------------------------------------------------------------
// 真·透桌面那一档：给浮层自己的 HWND 挂 DWM 瞬态亚克力
// ---------------------------------------------------------------------------

void applyTransientBackdrop(QWidget *popup) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QLatin1String("windows")) {
        return;  // offscreen 那类平台给的 winId 不是真 HWND
    }
    // 不直接链 dwmapi：这个文件也被验证工程编，它们各自只链了 Qt。取一次函数指针就够，
    // 系统没有这幅 API（Win10 以前）就静默跳过，只留下自绘那层玻璃。
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) {
        return;
    }
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    // `reinterpret_cast` 直接从 FARPROC 转函数指针会被 `-Wcast-function-type` 拦下（产品那份
    // 把警告当错误），走一趟 memcpy 把函数指针的位原样搬过来。
    static SetAttribute setAttribute = [](HMODULE module) -> SetAttribute {
        SetAttribute fn = nullptr;
        if (module != nullptr) {
            FARPROC raw = GetProcAddress(module, "DwmSetWindowAttribute");
            std::memcpy(&fn, &raw, sizeof(fn));
        }
        return fn;
    }(dwm);
    if (setAttribute == nullptr) {
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(popup->winId());
    if (hwnd == nullptr) {
        return;
    }
    const int transient = 3;  // DWMSBT_TRANSIENTWINDOW：弹出层 / 菜单专门那一档亚克力
    setAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &transient, sizeof(transient));
    const int round = 2;      // DWMWCP_ROUND
    setAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
#else
    Q_UNUSED(popup);
#endif
}

// ---------------------------------------------------------------------------
// 淡入（+ 位移）：`Anim::enabled()` 关着时直接落到位
// ---------------------------------------------------------------------------

/// @return 位移那一条动画（没有位移 / 动效关着时为空），调用方要知道"还在滑"时用。
QPropertyAnimation *fadeIn(QWidget *widget, int ms, int risePixels) {
    if (widget == nullptr) {
        return nullptr;
    }
    if (!Anim::enabled()) {
        widget->setWindowOpacity(1.0);
        return nullptr;
    }
    const QPoint target = widget->pos();
    widget->setWindowOpacity(0.0);
    auto *fade = new QPropertyAnimation(widget, "windowOpacity", widget);
    fade->setDuration(ms);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
    if (risePixels == 0) {
        return nullptr;
    }
    widget->move(target + QPoint(0, risePixels));
    auto *slide = new QPropertyAnimation(widget, "pos", widget);
    slide->setDuration(ms);
    slide->setStartValue(widget->pos());
    slide->setEndValue(target);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
    return slide;
}

// ---------------------------------------------------------------------------
// 浮层阴影：Windows 给 `Qt::Popup` 顶层窗画的原生阴影（`CS_DROPSHADOW`）只会按矩形铺，
// 圆角外面露出一圈方的。原生那条在 `preparePopup()` 里关掉，换成这里一块跟圆角同形的柔和阴影：
// 一个点击穿透的透明顶层窗，叠在浮层正后方，跟着它走（位置 / 大小 / 显示 / 隐藏）。
// ---------------------------------------------------------------------------

class PopupShadow : public QWidget {
public:
    PopupShadow()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                               Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
        setObjectName(QStringLiteral("wtfmlPopupShadow"));
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        // 深色页上阴影要更浓才看得出来。
        const int peak = Theme::tokens().page.lightness() < 128 ? 120 : 52;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        // 阴影本体 = 浮层的矩形整体下移 kShadowDropY；贴边处最浓（大半被浮层盖住），向外按三次方淡出。
        const QRectF core = QRectF(rect()).adjusted(kShadowSpread, kShadowSpread + kShadowDropY, -kShadowSpread,
                                                    -kShadowSpread + kShadowDropY);
        for (int i = kShadowSpread; i >= 0; --i) {
            const qreal fall = 1.0 - qreal(i) / kShadowSpread;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, qRound(peak * fall * fall * fall)));
            const QRectF ring = core.adjusted(-i, -i, i, i);
            painter.drawRoundedRect(ring, kPopupRadius + i, kPopupRadius + i);
        }
    }
};

/// 挂在浮层上：浮层上屏就把阴影窗叠到它后面，之后跟着挪、跟着缩，藏起来就一起藏。
class PopupShadowFollower : public QObject {
public:
    explicit PopupShadowFollower(QWidget *popup) : QObject(popup), m_popup(popup) {
        setObjectName(QStringLiteral("wtfmlPopupShadowFollower"));
        popup->installEventFilter(this);
    }

    ~PopupShadowFollower() override { delete m_shadow; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched != m_popup) {
            return false;
        }
        switch (event->type()) {
        case QEvent::Show:
            sync();
            // Qt 在 Show 之后才把浮层摆到最上面：再排一拍把阴影压回它后面。
            QTimer::singleShot(0, this, [this] { sync(); });
            break;
        case QEvent::Move:
        case QEvent::Resize:
            if (m_popup->isVisible()) {
                sync();
            }
            break;
        case QEvent::Hide:
        case QEvent::Close:
            if (m_shadow != nullptr) {
                m_shadow->hide();
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    void sync() {
        if (m_popup.isNull() || !m_popup->isVisible()) {
            return;
        }
        if (m_shadow == nullptr) {
            m_shadow = new PopupShadow;
        }
        const QRect target = m_popup->geometry().adjusted(-kShadowSpread, -kShadowSpread, kShadowSpread, kShadowSpread);
        if (m_shadow->geometry() != target) {
            m_shadow->setGeometry(target);
        }
        const bool fresh = !m_shadow->isVisible();
        if (fresh) {
            m_shadow->show();
            fadeIn(m_shadow, kPopupFadeMs, 0);  // 和浮层同一个淡入时长
        }
#ifdef Q_OS_WIN
        // 叠在浮层正后方：插在它后面，不抢焦点、不动位置。
        const HWND top = reinterpret_cast<HWND>(m_popup->winId());
        const HWND under = reinterpret_cast<HWND>(m_shadow->winId());
        if (top != nullptr && under != nullptr) {
            SetWindowPos(under, top, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
#endif
    }

    QPointer<QWidget> m_popup;
    PopupShadow *m_shadow = nullptr;
};

// ---------------------------------------------------------------------------
// tooltip：自己画的一条（系统那个黄底方角气泡不再出现）
// ---------------------------------------------------------------------------

class TooltipBubble : public QWidget {
public:
    TooltipBubble()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
        setObjectName(QStringLiteral("wtfmlTooltip"));
        Popups::preparePopup(this);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setFont(Theme::textFont(font(), Theme::TextRole::Caption));
    }

    /// @p maxWidth 正文最宽多少（已按屏幕可用区收过）；一行放不下就按它换行，放得下的框就跟着文字变窄。
    void setBody(const QString &text, int maxWidth) {
        m_text = text;
        const QFontMetrics metrics(font());
        QRect box = metrics.boundingRect(QRect(0, 0, maxWidth, 4000), kBodyFlags, text);
        if (!text.contains(QLatin1Char('\n'))) {
            // 一行放得下：框就是这一行的宽；放不下：撑到上限、按它换行（换行后的紧贴宽度比上限窄一个字，
            // 别把它当成"一行放得下"又撑回整行宽）。
            const int oneLine = metrics.horizontalAdvance(text) + 1;
            box.setWidth(oneLine <= maxWidth ? oneLine : maxWidth);
        }
        resize(box.width() + 20, box.height() + 12);  // 内边距 10 / 6
    }

    QString body() const { return m_text; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Popups::paintSurface(painter, QRectF(rect()), kTooltipRadius);
        painter.setPen(Theme::tokens().text);
        painter.drawText(rect().adjusted(10, 6, -10, -6), Qt::AlignLeft | Qt::AlignVCenter | kBodyFlags, m_text);
    }

private:
    // 词边界换行；整段没有空格的（路径、长英文串）也能在任意处断开。
    static constexpr int kBodyFlags = Qt::TextWordWrap | Qt::TextWrapAnywhere;
    QString m_text;
};

TooltipBubble *bubble() {
    // 应用活多久它活多久（`Qt::ToolTip` 顶层窗口，没有父级）。
    static TooltipBubble *instance = new TooltipBubble;
    return instance;
}

/// 屏幕可用区：鼠标所在那块屏（没有就主屏）。
QRect tooltipScreenArea(const QPoint &cursor) {
    QScreen *screen = QGuiApplication::screenAt(cursor);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

/// 落在鼠标右下方；右边放不下翻到鼠标左边，下边放不下翻到鼠标上边，最后收进可用区。
QPoint placeNearCursor(const QSize &size, const QPoint &cursor) {
    const QRect available = tooltipScreenArea(cursor);
    QPoint target = cursor + kTooltipCursorOffset;
    if (target.x() + size.width() > available.right() - 4) {
        target.setX(cursor.x() - kTooltipCursorOffset.x() - size.width());
    }
    if (target.y() + size.height() > available.bottom() - 4) {
        target.setY(cursor.y() - 8 - size.height());
    }
    target.setX(qBound(available.left() + 4, target.x(), qMax(available.left() + 4, available.right() - size.width() - 4)));
    target.setY(qBound(available.top() + 4, target.y(), qMax(available.top() + 4, available.bottom() - size.height() - 4)));
    return target;
}

// ---------------------------------------------------------------------------
// tooltip 的调度：进控件 500ms 才出、离开立刻收、1 秒内换到相邻控件不等
// ---------------------------------------------------------------------------

class TooltipDriver : public QObject {
public:
    explicit TooltipDriver(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlTooltipDriver"));
        app.installEventFilter(this);
        m_delay.setSingleShot(true);
        m_delay.setInterval(kTooltipDelayMs);
        QObject::connect(&m_delay, &QTimer::timeout, this, [this] {
            if (m_pending.isNull()) {
                return;
            }
            Popups::showTooltip(m_pending->toolTip(), QCursor::pos(), m_pending.data());
        });
        m_hideGuard.setSingleShot(true);
        m_hideGuard.setInterval(0);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::Enter: {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget == nullptr || !widget->isVisible()) {
                return false;
            }
            const QString text = widget->toolTip();
            if (text.isEmpty() || Popups::isTextButton(widget)) {
                return false;  // 文字按钮不出提示（按 text() 判，见 `isTextButton`）
            }
            m_pending = widget;
            if (QDateTime::currentMSecsSinceEpoch() - m_lastHideMs < kTooltipWakeMs) {
                Popups::showTooltip(text, QCursor::pos(), widget);  // 换到相邻控件：不等
            } else {
                m_delay.start();
            }
            return false;
        }
        case QEvent::ToolTip: {
            // 系统那条气泡**一律不出**：文字按钮没有提示，别的一律换成自绘那一条。
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget == nullptr) {
                return false;
            }
            auto *help = static_cast<QHelpEvent *>(event);
            const QString text = textAt(widget, help->pos());
            if (Popups::isTextButton(widget)) {
                return true;
            }
            if (text.isEmpty()) {
                return false;  // 条目自带的提示由代理自己叫 `showTooltip()`，这里别抢
            }
            m_delay.stop();
            Popups::showTooltip(text, help->globalPos(), widget);
            return true;
        }
        case QEvent::MouseMove:
            // 提示亮着时跟着鼠标走（只挪位置，文字不变）：停在哪就出现在哪。
            if (bubble()->isVisible() && watched != bubble() && watched->isWidgetType()) {
                Popups::moveTooltipTo(QCursor::pos());
            }
            return false;
        case QEvent::Leave:
        case QEvent::Hide:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            m_delay.stop();
            hideNow();
            return false;
        default:
            return false;
        }
    }

private:
    /// 这一格现在该说哪句：条目看 `ToolTipRole`，别的控件看 `toolTip`。
    static QString textAt(QWidget *widget, const QPoint &pos) {
        if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
            const QModelIndex index = view->indexAt(pos);
            return index.isValid() ? index.data(Qt::ToolTipRole).toString() : QString();
        }
        // 列表 / 树的视口：事件打在 viewport 上，条目要从父级那只 view 问。
        if (widget->parentWidget() != nullptr) {
            if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget())) {
                if (widget == view->viewport()) {
                    const QModelIndex index = view->indexAt(pos);
                    return index.isValid() ? index.data(Qt::ToolTipRole).toString() : QString();
                }
            }
        }
        return widget->toolTip();
    }

    void hideNow() {
        TooltipBubble *tip = bubble();
        if (tip->isVisible()) {
            tip->hide();
            m_lastHideMs = QDateTime::currentMSecsSinceEpoch();
        }
    }

    QTimer m_delay;
    QTimer m_hideGuard;
    QPointer<QWidget> m_pending;
    qint64 m_lastHideMs = 0;
};

// ---------------------------------------------------------------------------
// 下拉弹层的项：30 高；当前值 = 强调淡底 + 强调字，悬停 / 键盘停在的那一项 = 更淡一档的强调底
// （PCL 同款两档：当前项和"手指着的"那一项一眼分得开）。不画勾不画条。
// ---------------------------------------------------------------------------

class PopupItemDelegate : public QStyledItemDelegate {
public:
    /// @p combo 给了就按它的 currentIndex 认"当前值"；补全列表那种没有当前值的不给，选中那一行就是当前。
    explicit PopupItemDelegate(QObject *parent, QComboBox *combo = nullptr)
        : QStyledItemDelegate(parent), m_combo(combo) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QFontMetrics metrics(Theme::textFont(option.font, Theme::TextRole::Body));
        const int textWidth = metrics.horizontalAdvance(index.data(Qt::DisplayRole).toString());
        return QSize(textWidth + 2 * kItemTextInset, kRowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const Theme::Tokens tokens = Theme::tokens();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect box = option.rect.adjusted(4, 1, -4, -1);  // 项之间留 2px：紧凑，但两块高亮不粘成一条
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hover = (option.state & QStyle::State_MouseOver) != 0;
        // ★ 下拉弹层里 Qt 让"选中"跟着鼠标 / ↑↓ 走，所以 State_Selected 是"指着的那一项"，
        // 不是下拉框现在的值 —— 当前值要问下拉框自己。
        const bool current = m_combo.isNull() ? selected : index.row() == m_combo->currentIndex();
        const bool pointed = m_combo.isNull() ? hover : (selected || hover);
        painter->setPen(Qt::NoPen);
        if (current) {
            painter->setBrush(tokens.selected);
            painter->drawRoundedRect(box, 6, 6);
        } else if (pointed) {
            painter->setBrush(tokens.accentSoft2);
            painter->drawRoundedRect(box, 6, 6);
        }
        painter->setFont(Theme::textFont(option.font, Theme::TextRole::Body));
        painter->setPen(current ? option.palette.color(QPalette::Link) : tokens.text);
        painter->drawText(box.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString());
        painter->restore();
    }

private:
    QPointer<QComboBox> m_combo;
};

/// 弹层上下那 16px 的渐隐：项滚到边上时淡进那层玻璃，不硬切。
class EdgeFade : public QWidget {
public:
    EdgeFade(QWidget *parent, bool top) : QWidget(parent), m_top(top) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QColor solid = Theme::tokens().glass;
        solid.setAlpha(255);  // 渐隐到"这一层的底"，不是渐隐到黑
        QColor clear = solid;
        clear.setAlpha(0);
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(m_top ? 1.0 : 0.0, clear);
        gradient.setColorAt(m_top ? 0.0 : 1.0, solid);
        QPainter painter(this);
        painter.fillRect(rect(), gradient);
    }

private:
    bool m_top = true;
};

// ---------------------------------------------------------------------------
// 下拉弹层的外壳：磨砂底、圆角 14、3px 细条（滚动时露 / 停 600ms 淡出）、
// 超过 15 项时顶上一条搜索框
// ---------------------------------------------------------------------------

class ComboDecorator : public QObject {
public:
    ComboDecorator(QComboBox *combo, QAbstractItemView *view)
        : QObject(combo), m_combo(combo), m_view(view) {
        m_thumbIdle.setSingleShot(true);
        m_thumbIdle.setInterval(kThumbFadeMs);
        QObject::connect(&m_thumbIdle, &QTimer::timeout, this, [this] { fadeThumb(false); });
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_view) {
            if (event->type() == QEvent::Show) {
                decoratePopup();
            }
            return false;
        }
        if (watched == m_search) {
            return filterSearchKey(event);
        }
        if (m_popup.isNull() || watched != m_popup.data()) {
            return false;
        }
        if (event->type() == QEvent::Show) {
            // Qt 每次弹出都会把那两块重新摆出来，而且在 show 之后还要再跑一趟
            // `updateScrollers()`：当场藏一次不够，排到下一拍再藏一次。
            hideScrollers();
            if (Anim::enabled()) {
                m_popup->setWindowOpacity(0.0);  // 摆到位、起动效之前别在 Qt 摆的位置上闪一帧
            }
            m_combo->update();  // 框换展开态（细边转强调色，Theme.cpp ComboOutlinePainter 画）
            QTimer::singleShot(0, this, [this] {
                if (m_popup.isNull() || !m_popup->isVisible()) return;
                hideScrollers();
                fitPopup();
                // 从框下沿往下滑出来（翻到上方时从框上沿往上），同时淡入。
                m_slide = fadeIn(m_popup.data(), kPopupFadeMs, m_above ? kPopupGap : -kPopupGap);
            });
        } else if (event->type() == QEvent::Hide) {
            m_combo->update();
        } else if (event->type() == QEvent::Paint) {
            // 先铺那一层磨砂玻璃，再让弹层自己把项画上去（容器自身被样式表清成透明）。
            QPainter painter(m_popup.data());
            painter.setRenderHint(QPainter::Antialiasing, true);
            Popups::paintSurface(painter, QRectF(m_popup->rect()), kPopupRadius);
        } else if (event->type() == QEvent::Resize) {
            hideScrollers();
            layoutFades();
            // Qt 自己再摆一次（它按"整张表"算）时会把视图压回去 —— 再收紧成按项数算的那一版。
            fitPopup();
        }
        return false;
    }

private:
    void decoratePopup() {
        QWidget *popup = m_view->window();
        if (popup == nullptr) {
            return;
        }
        if (m_popup.data() != popup) {
            m_popup = popup;
            popup->installEventFilter(this);
            Popups::preparePopup(popup);  // 上屏了才挂得上 DWM 背板：每次换容器补一趟
            popup->setAutoFillBackground(false);
            m_view->setFrameShape(QFrame::NoFrame);
            // 这一份只给弹层里那张表：底归玻璃给，别自己铺一块不透明的。
            m_view->setStyleSheet(QStringLiteral("QListView { background: transparent; border: none; }"));
            if (auto *layout = qobject_cast<QBoxLayout *>(popup->layout())) {
                layout->setContentsMargins(kPopupPadding, kPopupPadding, kPopupPadding, kPopupPadding);
                layout->setSpacing(0);
            }
            // ★ 限高钉在**视图**上：Fusion 原来那种"当前项盖在框上"的弹法下 `setMaxVisibleItems()`
            // 不管用（45 项就是整屏）。#337 起样式已改成挂在框下方，这里照旧钉死，不依赖 Qt 那一支。
            // 弹层容器按视图的尺寸算，所以把视图按住就等于弹层按住。
            m_view->setMaximumHeight(kMaxVisibleItems * kRowHeight + 2);
            ensureThumb();
            ensureFades();
            hideScrollers();
        }
        ensureSearch();
        layoutFades();
        m_view->setFocus();
    }

    /// 右缘那条 3px 细条：常态透明，滚动时淡入、停 600ms 淡出（S2）。
    void ensureThumb() {
        QScrollBar *bar = m_view->verticalScrollBar();
        if (bar == nullptr || m_thumbBar == bar) {
            return;
        }
        m_thumbBar = bar;
        // 形状（3px 宽、圆角 1px）在应用级样式表的 `QComboBox QAbstractItemView QScrollBar`
        // 那几条里；这里只管"什么时候露"：常态透明度 0，滚动时 120ms 淡入、停 600ms 淡出。
        m_thumbEffect = new QGraphicsOpacityEffect(bar);
        m_thumbEffect->setOpacity(0.0);
        bar->setGraphicsEffect(m_thumbEffect);
        QObject::connect(bar, &QScrollBar::valueChanged, this, [this] { fadeThumb(true); });
    }

    void fadeThumb(bool visible) {
        if (m_thumbEffect == nullptr) {
            return;
        }
        if (!Anim::enabled()) {
            m_thumbEffect->setOpacity(visible ? 1.0 : 0.0);
        } else {
            auto *fade = new QPropertyAnimation(m_thumbEffect, "opacity", m_thumbEffect);
            fade->setDuration(visible ? 120 : 300);
            fade->setStartValue(m_thumbEffect->opacity());
            fade->setEndValue(visible ? 1.0 : 0.0);
            fade->start(QAbstractAnimation::DeleteWhenStopped);
        }
        if (visible) {
            m_thumbIdle.start();
        }
    }

    /// Qt 给装不下的弹层在上下加了两块"翻一屏"的箭头（`QComboBoxPrivateScroller`，
    /// 私有类，认不了类型就按类名认）。这种小箭头一个都不要：滚轮 / 拖动 / ↑↓ 已经够了。
    void hideScrollers() {
        if (m_popup.isNull()) {
            return;
        }
        const auto children = m_popup->findChildren<QWidget *>();
        for (QWidget *child : children) {
            if (QString::fromLatin1(child->metaObject()->className())
                    .contains(QLatin1String("QComboBoxPrivateScroller"))) {
                child->setVisible(false);
            }
        }
    }

    /// Qt 把弹层撑到"装得下全部项"（45 项就是整屏）。这里按 8 项收回，并且**高度按项数算准**。
    ///
    /// ★ 不能只缩不涨：容器的高度是 Qt 按"视图要多大"摆的，而 `decoratePopup()` 给容器加了
    /// 一圈内边距 —— 那几像素是从视图身上扣掉的，Qt 算的时候并不知道。于是 4 项就少露半行
    /// （用户那台 125% 下量到末项被裁 15 物理像素）。所以这里钉**视图**：视图拿到
    /// rows × kRowHeight，容器高度再由布局自己算（内边距、顶上那条搜索框都在 sizeHint 里），
    /// 不再拿魔数补。屏幕装不下才往下收，收的是视图那一截（剩下的靠弹层里滚动）。
    void fitPopup() {
        if (m_popup.isNull() || m_view.isNull() || m_fitting) {
            return;
        }
        m_fitting = true;
        int visible = 0;
        auto *list = qobject_cast<QListView *>(m_view.data());
        for (int row = 0; row < m_combo->count(); ++row) {
            if (list == nullptr || !list->isRowHidden(row)) {
                ++visible;
            }
        }
        const int rows = qBound(1, qMin(visible, kMaxVisibleItems), kMaxVisibleItems);
        const QScreen *screen = m_combo->screen();
        const QRect available = screen != nullptr ? screen->availableGeometry() : QRect();
        m_view->setFixedHeight(rows * kRowHeight);
        auto *layout = qobject_cast<QBoxLayout *>(m_popup->layout());
        const auto totalHeight = [this, layout] {
            if (layout == nullptr) {
                return m_view->height() + 2 * kPopupPadding;
            }
            layout->activate();
            auto *frame = qobject_cast<QFrame *>(m_popup.data());
            return layout->sizeHint().height() + 2 * (frame != nullptr ? frame->frameWidth() : 0);
        };
        int height = totalHeight();
        // 挂在框正下方（PCL / HMCL / XMCL 都是这样）：顶边 = 框底 + kPopupGap，左缘对齐框左缘。
        // 下面放不下、上面更宽裕才翻到框上方；两边都放不下就挑大的那边、收视图那一截。
        const QRect anchor(m_combo->mapToGlobal(QPoint(0, 0)), m_combo->size());
        m_above = false;
        if (available.isValid()) {
            const int roomBelow = available.bottom() - anchor.bottom() - kPopupGap;
            const int roomAbove = anchor.top() - available.top() - kPopupGap;
            m_above = height > roomBelow && roomAbove > roomBelow;
            const int allowed = qMax(kRowHeight, m_above ? roomAbove : roomBelow);
            if (height > allowed && rows > 1) {
                const int viewHeight = qMax(kRowHeight, m_view->height() - (height - allowed));
                m_view->setFixedHeight(viewHeight);
                height = totalHeight();
            }
        }
        // 宽度：弹层是 Qt 按**外面那个下拉框**的宽摆的（它不看项有多宽），而项的文字左右
        // 各让出 12px、弹层自己还有 6px 内边距 —— 一行放不下的时候就切字（不带省略号）。
        // 这里按最宽那一项把弹层撑开，屏幕放不下才收。
        int width = qMax(m_popup->width(), anchor.width());  // 至少和框一样宽
        const int widestItem = m_view->sizeHintForColumn(0);
        if (widestItem > 0) {
            const auto *frame = qobject_cast<QFrame *>(m_popup.data());
            const int frameWidth = frame != nullptr ? 2 * frame->frameWidth() : 0;
            const int scrollWidth = visible > m_view->height() / kRowHeight
                ? m_view->verticalScrollBar()->sizeHint().width() : 0;
            width = qMax(width, widestItem + 2 * kPopupPadding + frameWidth + scrollWidth);
        }
        if (available.isValid()) width = qMin(width, available.width());
        if (m_popup->height() != height || m_popup->width() != width) {
            m_popup->resize(width, height);
        }
        // 滑入那 150ms 里别把它拽回终点（动画自己会落到这里算的那个位置）。
        const bool sliding = !m_slide.isNull() && m_slide->state() == QAbstractAnimation::Running;
        if (available.isValid() && !sliding) {
            // 比框宽的部分向右撑；右边出屏才整块往左让。
            const int left = qBound(available.left(), anchor.left(),
                                    qMax(available.left(), available.right() - width + 1));
            const int wantTop = m_above ? anchor.top() - kPopupGap - height : anchor.bottom() + 1 + kPopupGap;
            const int top = qBound(available.top(), wantTop, qMax(available.top(), available.bottom() - height + 1));
            if (m_popup->frameGeometry().topLeft() != QPoint(left, top)) m_popup->move(left, top);
        }
        m_fitting = false;
    }

    void ensureFades() {
        if (m_fadeTop == nullptr) {
            m_fadeTop = new EdgeFade(m_popup.data(), true);
            m_fadeBottom = new EdgeFade(m_popup.data(), false);
        }
        m_fadeTop->show();
        m_fadeBottom->show();
        m_fadeTop->raise();
        m_fadeBottom->raise();
    }

    void layoutFades() {
        if (m_fadeTop == nullptr || m_popup.isNull()) {
            return;
        }
        QWidget *list = m_view;
        const QRect area = list->geometry();
        const int height = qMin(kFadeEdgePx, qMax(0, area.height() / 3));
        m_fadeTop->setGeometry(area.left(), area.top(), area.width(), height);
        m_fadeBottom->setGeometry(area.left(), area.bottom() - height + 1, area.width(), height);
    }

    void ensureSearch() {
        if (m_combo->count() <= kSearchAfterItems) {
            return;  // 短列表不要搜索框
        }
        if (m_search.isNull()) {
            auto *popup = m_popup.data();
            m_search = new QLineEdit(popup);
            m_search->setObjectName(QStringLiteral("wtfmlPopupSearch"));
            m_search->setPlaceholderText(QObject::tr("搜索…"));
            m_search->setClearButtonEnabled(true);
            m_search->installEventFilter(this);
            if (auto *layout = qobject_cast<QBoxLayout *>(popup->layout())) {
                layout->insertWidget(0, m_search);
            }
            QObject::connect(m_search, &QLineEdit::textChanged, this,
                             [this](const QString &query) { applyFilter(query); });
        }
        m_search->show();
        m_search->setFocus();
        if (m_search->text().isEmpty()) {
            applyFilter(QString());
        } else {
            m_search->clear();
        }
    }

    /// 搜索框拿着焦点时，↑↓ / 回车还得能当列表用。
    bool filterSearchKey(QEvent *event) {
        if (event->type() != QEvent::KeyPress) {
            return false;
        }
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
            m_combo->hidePopup();
            return true;
        }
        if (key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_PageDown ||
            key == Qt::Key_PageUp) {
            QApplication::sendEvent(m_view, event);
            return true;
        }
        return false;
    }

    void applyFilter(const QString &query) {
        auto *list = qobject_cast<QListView *>(m_view.data());
        if (list == nullptr) {
            return;  // 换了别的视图（树）就不做过滤，搜索框也不该出现
        }
        const QString needle = query.trimmed();
        int firstVisible = -1;
        for (int row = 0; row < m_combo->count(); ++row) {
            const bool match =
                needle.isEmpty() || m_combo->itemText(row).contains(needle, Qt::CaseInsensitive);
            list->setRowHidden(row, !match);
            if (match && firstVisible < 0) {
                firstVisible = row;
            }
        }
        if (firstVisible >= 0) {
            m_view->setCurrentIndex(m_combo->model()->index(firstVisible, m_combo->modelColumn()));
        }
        fitPopup();
        hideScrollers();
    }

    QPointer<QComboBox> m_combo;
    QPointer<QAbstractItemView> m_view;
    QPointer<QWidget> m_popup;
    bool m_fitting = false;  ///< 正在按项数摆高：`resize()` 会回灌 Resize 事件，别自己套自己
    bool m_above = false;    ///< 这一次翻到了框上方（下方放不下）
    QPointer<QPropertyAnimation> m_slide;
    QPointer<QScrollBar> m_thumbBar;
    QPointer<QGraphicsOpacityEffect> m_thumbEffect;
    QPointer<QLineEdit> m_search;
    QPointer<QWidget> m_fadeTop;
    QPointer<QWidget> m_fadeBottom;
    QTimer m_thumbIdle;
};

// ---------------------------------------------------------------------------
// 全仓 QComboBox：一只一次，挂在 Polish 上（以后新加的下拉框自动跟上）
// ---------------------------------------------------------------------------

class ComboWatcher : public QObject {
public:
    explicit ComboWatcher(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlComboWatcher"));
        app.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Polish) {
            return false;
        }
        auto *combo = qobject_cast<QComboBox *>(watched);
        if (combo == nullptr || combo->property("wtfmlPopupStyled").toBool()) {
            return false;
        }
        combo->setProperty("wtfmlPopupStyled", true);
        combo->setMaxVisibleItems(kMaxVisibleItems);
        QAbstractItemView *view = combo->view();
        if (view == nullptr) {
            return false;
        }
        // 这个代理类在匿名命名空间里（没有 Q_OBJECT，moc 认不到），认它靠挂在身上的属性。
        auto *delegate = view->itemDelegate();
        if (delegate == nullptr || !delegate->property("wtfmlPopupDelegate").toBool()) {
            auto *popupDelegate = new PopupItemDelegate(view, combo);
            popupDelegate->setProperty("wtfmlPopupDelegate", true);
            view->setItemDelegate(popupDelegate);
        }
        view->setMouseTracking(true);
        view->viewport()->setMouseTracking(true);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // ★ 两件事都得赶在弹层自己的 HWND 建出来之前说（`QWidget` 只在**首次**上屏时
        // 按当时的属性、旗标造平台窗口）。这里还是 Polish：`combo->view()` 已经把
        // `QComboBoxPrivateContainer` 造就了、但还没 show 过。
        //   · 无边框：Windows 上 Qt 只给「无边框**且**半透明」的窗口建 WS_EX_LAYERED
        //     （逐像素 alpha），而这个容器只有 Qt::Popup、没有 FramelessWindowHint；
        //   · 半透明：属性要是等到 `ComboDecorator` 收到 Show 才挂，窗口早按不透明造成。
        // 少了这两件，四角那块没人画过的像素就是黑（96 把出厂材质从亚克力
        // 改成风景底之后，新装用户一打开就能看见）。
        if (QWidget *popup = view->window(); popup != nullptr && popup != combo) {
            Popups::preparePopup(popup);
        }
        auto *decorator = new ComboDecorator(combo, view);
        view->installEventFilter(decorator);
        return false;
    }
};

// ---------------------------------------------------------------------------
// 菜单与补全列表：同一个外壳（底由这里画，项由它们自己画）
// ---------------------------------------------------------------------------

class MenuGlass : public QObject {
public:
    explicit MenuGlass(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlMenuGlass"));
        app.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        const QEvent::Type type = event->type();
        if (type != QEvent::Polish && type != QEvent::Show && type != QEvent::Paint) {
            return false;
        }
        QWidget *popup = qobject_cast<QMenu *>(watched);
        if (popup == nullptr) {
            // 可编辑下拉框的补全列表（`QCompleter` 自建的 QListView）：它自己就是 `Qt::Popup` 顶层窗。
            auto *list = qobject_cast<QAbstractItemView *>(watched);
            if (list == nullptr || list->windowType() != Qt::Popup) {
                return false;
            }
            popup = list;
            if (type == QEvent::Polish && !list->property("wtfmlPopupList").toBool()) {
                list->setProperty("wtfmlPopupList", true);
                list->setFrameShape(QFrame::NoFrame);
                list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                // 内边距 3：QCompleter 算弹层高时只多给 6px（上下各 3），再大就冒滚动条。
                list->setStyleSheet(QStringLiteral(
                    "QAbstractItemView { background: transparent; border: none; padding: 3px; }"));
                list->viewport()->setAutoFillBackground(false);
                auto *delegate = new PopupItemDelegate(list);
                delegate->setProperty("wtfmlPopupDelegate", true);
                list->setItemDelegate(delegate);
            }
        }
        if (type == QEvent::Polish) {
            // Polish 先于 HWND：旗标和半透明这时挂才算数（`preparePopup()` 的注释）。
            Popups::preparePopup(popup);
        } else if (type == QEvent::Show) {
            fadeIn(popup, kPopupFadeMs, 4);
        } else {
            QPainter painter(popup);
            painter.setRenderHint(QPainter::Antialiasing, true);
            Popups::paintSurface(painter, QRectF(popup->rect()), kPopupRadius);
        }
        return false;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 对外那几个口子
// ---------------------------------------------------------------------------

void Popups::paintSurface(QPainter &painter, const QRectF &rect, qreal radius) {
    // 面只有一份出处：主页那张玻璃卡片（`GlassPopover`）。顶层浮层是 Qt 自己的弹出窗口、
    // 装不进窗口里，所以共用的是**画法**（圆角 + 玻璃 + 顶上 1px 高光）。底下一律垫页底色：
    // 弹层是无边框半透明的分层窗口，亚克力/云母下 DWM 背板也贴不上，不垫就透出底下的控件。
    GlassPopover::paintSurface(painter, rect, radius, true);
}

void Popups::preparePopup(QWidget *popup) {
    if (popup == nullptr) {
        return;
    }
    // Qt 给没挂这个旗标的 Popup 在 Windows 上注册带 CS_DROPSHADOW 的窗口类（方形阴影），要赶在 HWND 建出来之前关。
    const bool shadowed = popup->windowType() == Qt::Popup &&
                          QGuiApplication::platformName() == QLatin1String("windows");
    if (shadowed && !popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint) && !popup->isVisible()) {
        popup->setWindowFlag(Qt::NoDropShadowWindowHint, true);
    }
    if (shadowed && popup->findChild<QObject *>(QStringLiteral("wtfmlPopupShadowFollower"), Qt::FindDirectChildrenOnly) == nullptr) {
        new PopupShadowFollower(popup);
    }
    if (!popup->windowFlags().testFlag(Qt::FramelessWindowHint)) {
        // 没上屏之前改旗标都算数（QMenu 在 Polish 时平台窗口已经建了，Qt 会按新旗标重建）；
        // 已经显示出来再改，Qt 要先藏起来销毁重建，弹层会闪没。
        if (!popup->isVisible()) {
            popup->setWindowFlag(Qt::FramelessWindowHint, true);
        } else {
            qWarning("Popups::preparePopup: %s(%s) 显示后才补无边框，四角会露黑",  // i18n-ignore
                     popup->metaObject()->className(), qPrintable(popup->objectName()));
        }
    }
    popup->setAttribute(Qt::WA_TranslucentBackground, true);
    popup->setAttribute(Qt::WA_NoSystemBackground, true);
    // DWM 那层要真 HWND：`applyTransientBackdrop()` 里有 `winId()`，在弹层还没上屏时调它
    // 会**提前**把原生窗口造出来（挂在控件树里当子窗口，主窗口的「没有原生子窗口」那条就红了）。
    if (Glass::systemBackdrop() && popup->windowHandle() != nullptr) {
        applyTransientBackdrop(popup);
    }
}

void Popups::showTooltip(const QString &text, const QPoint &globalPos, const QWidget *anchor) {
    Q_UNUSED(anchor);  // 位置只看鼠标；参数留着，调用方不用跟着改
    if (text.isEmpty()) {
        return;
    }
    TooltipBubble *tip = bubble();
    tip->setBody(text, qMin(kTooltipMaxTextWidth, qMax(120, tooltipScreenArea(globalPos).width() - 60)));
    tip->move(placeNearCursor(tip->size(), globalPos));
    const bool wasVisible = tip->isVisible();
    tip->show();
    tip->raise();
    if (!wasVisible) {
        fadeIn(tip, 120, -2);  // 120ms 淡入 + 上移 2px
    }
}

void Popups::moveTooltipTo(const QPoint &globalPos) {
    TooltipBubble *tip = bubble();
    if (tip->isVisible()) {
        tip->move(placeNearCursor(tip->size(), globalPos));
    }
}

void Popups::hideTooltip() {
    TooltipBubble *tip = bubble();
    if (tip->isVisible()) {
        tip->hide();
    }
}

QRect Popups::tooltipGeometry() {
    return bubble()->geometry();
}

QString Popups::tooltipText() {
    TooltipBubble *tip = bubble();
    return tip->isVisible() ? tip->body() : QString();
}

bool Popups::tooltipVisible() {
    return bubble()->isVisible();
}

bool Popups::isTextButton(const QWidget *widget) {
    auto *button = qobject_cast<const QAbstractButton *>(widget);
    if (button == nullptr) {
        return false;
    }
    // 「文字按钮」= 屏幕上真的画着一句字的按钮。下面两种身上也有 text()，但那是给
    // 无障碍 / 验证工程读的，屏幕上只有一个图标：图标钮该有提示，别一起掐掉。
    //  · `Theme::markIconButton()` 过的（属性 `wtfmlButtonSize = "icon"`，实例文件那排 ⓘ / ✕）；
    //  · `QToolButton` 摆成 `ToolButtonIconOnly` 的。
    if (button->property("wtfmlButtonSize").toString() == QLatin1String("icon")) {
        return false;
    }
    if (auto *tool = qobject_cast<const QToolButton *>(button);
        tool != nullptr && tool->toolButtonStyle() == Qt::ToolButtonIconOnly) {
        return false;
    }
    return !button->text().trimmed().isEmpty();
}

void Popups::install(QApplication &app) {
    // Qt 自己那套"下拉卷帘"（系统开着组合框动画时）会先抓一张不透明截图卷出来，和我们的
    // 半透明弹层 + 滑入淡入打架：关掉，动效只走 ComboDecorator 那一份。
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    if (app.findChild<QObject *>(QStringLiteral("wtfmlTooltipDriver")) == nullptr) {
        new TooltipDriver(app);
    }
    if (app.findChild<QObject *>(QStringLiteral("wtfmlComboWatcher")) == nullptr) {
        new ComboWatcher(app);
    }
    if (app.findChild<QObject *>(QStringLiteral("wtfmlMenuGlass")) == nullptr) {
        new MenuGlass(app);
    }
}

