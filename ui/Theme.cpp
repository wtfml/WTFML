/**
 * @file Theme.cpp
 * @brief Theme 的实现：预设 / 覆盖项怎么算成一个调色板，以及"地方"和"语义色"现在是什么颜色。
 */

#include "Theme.hpp"

#include "DesignDefaults.hpp"
#include "Glass.hpp"
#include "Popups.hpp"

#include <QAbstractItemView>
#include <QTreeView>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QButtonGroup>
#include <QCache>
#include <QFileInfo>
#include <QLabel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QCheckBox>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOptionComboBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

/// 标题字体在 icons.qrc 里（静态库里的 qrc 没人引用会被链接器丢掉，见 ui/Glass.cpp 同一段）。
static void initTitleFontResources() { Q_INIT_RESOURCE(icons); }

namespace Theme {
bool platformColorSchemeDark();

Tokens tokens(const QString &id) {
    if (id == QLatin1String("system")) {
        return tokens(platformColorSchemeDark() ? QStringLiteral("dark")
                                                : QStringLiteral("emerald"));
    }
    // 三主题色值照抄设计稿第二批 tokens.md（窗口底 = 模糊风景 + page 着色，控件层叠在上面）。
    const auto rgba = [](int r, int g, int b, qreal a) { QColor c(r, g, b); c.setAlphaF(a); return c; };
    if (id == QLatin1String("dark")) {
        // 暮紫：浅薰衣草主按钮配深字（8.6:1），强调字同色。
        static const Tokens dark = {
            .primary = QColor("#B7B3EE"), .deep = QColor("#B7B3EE"), .primaryHover = QColor("#C6C3F4"),
            .primaryPressed = QColor("#A5A1E2"),
            .page = QColor("#10121A"), .main = QColor("#10121A"), .card = QColor("#1F212F"),
            .subtle = rgba(255, 255, 255, .07), .hover = rgba(255, 255, 255, .055), .selected = rgba(183, 179, 238, .22),
            .input = rgba(255, 255, 255, .07),
            .text = QColor("#E8E9F1"), .secondary = QColor("#A9ADBF"), .disabled = QColor("#7B8095"),
            .stroke = rgba(255, 255, 255, .13), .divider = rgba(255, 255, 255, .07), .focus = rgba(183, 179, 238, .60),
            .thumb = rgba(255, 255, 255, .20), .track = QColor(Qt::transparent),
            .logWindow = QColor("#242C32"), .logToolbar = QColor("#303D46"), .logBase = QColor("#171E23"),
            .logHint = QColor("#99A9B3"), .logBorder = QColor("#4C6573"), .logDivider = QColor("#34434B"),
            .onPrimary = QColor("#1C1B30"), .accentSoft2 = rgba(183, 179, 238, .09), .fillPress = rgba(255, 255, 255, .09),
            .fillSoftHover = rgba(255, 255, 255, .11),
            .ok = QColor("#86CFA6"), .warn = QColor("#E6BE73"), .danger = QColor("#F09393"),
            .okSoft = rgba(134, 207, 166, .14), .warnSoft = rgba(230, 190, 115, .14), .dangerSoft = rgba(240, 147, 147, .14),
            .dangerSolid = QColor("#A8585E"), .dangerSolidHover = QColor("#B4646A"), .dangerSolidPressed = QColor("#984F55"),
            .onDanger = QColor("#FFF4F4"),
            .glass = rgba(36, 38, 54, .74), .glassEdge = rgba(255, 255, 255, .10), .scrim = rgba(8, 9, 14, .55),
            .heroInk = QColor("#F4F3FA"), .heroInk2 = rgba(236, 235, 248, .78), .heroVeil = QColor(10, 11, 18),
            .heroScrim = .30, .heroTop = .42, .railFog = 0.0, .groundShade = QColor(6, 6, 14), .groundGlint = .10,
            .accentInk = QColor("#D4D1F8"), .okInk = QColor("#86CFA6"), .warnInk = QColor("#E6BE73"), .dangerInk = QColor("#F09393")};
        return dark;
    }
    if (id == QLatin1String("ocean")) {
        // 雾蓝。
        static const Tokens ocean = {
            .primary = QColor("#4A7396"), .deep = QColor("#436C8F"), .primaryHover = QColor("#43698A"),
            .primaryPressed = QColor("#395A77"),
            .page = QColor("#F1F5F9"), .main = QColor("#F1F5F9"), .card = QColor("#F9FBFD"),
            .subtle = rgba(255, 255, 255, .58), .hover = rgba(26, 52, 80, .05), .selected = rgba(56, 124, 190, .17),
            .input = rgba(255, 255, 255, .58),
            .text = QColor("#1F2C3A"), .secondary = QColor("#465769"), .disabled = QColor("#85929F"),
            .stroke = rgba(26, 50, 78, .16), .divider = rgba(26, 50, 78, .09), .focus = rgba(74, 115, 150, .55),
            .thumb = rgba(26, 50, 78, .22), .track = QColor(Qt::transparent),
            .logWindow = QColor("#EEF2F6"), .logToolbar = QColor("#F3F8FD"), .logBase = QColor("#FDFEFF"),
            .logHint = QColor("#6B7B8B"), .logBorder = QColor("#91B8E0"), .logDivider = QColor("#BCD2EC"),
            .onPrimary = QColor("#FFFFFF"), .accentSoft2 = rgba(74, 115, 150, .08), .fillPress = rgba(26, 52, 80, .09),
            .fillSoftHover = rgba(255, 255, 255, .80),
            .ok = QColor("#2E875A"), .warn = QColor("#A16D1A"), .danger = QColor("#BC5246"),
            .okSoft = rgba(46, 135, 90, .12), .warnSoft = rgba(161, 109, 26, .12), .dangerSoft = rgba(188, 82, 70, .12),
            .dangerSolid = QColor("#A4524A"), .dangerSolidHover = QColor("#964A43"), .dangerSolidPressed = QColor("#86413B"),
            .onDanger = QColor("#FFFFFF"),
            .glass = rgba(249, 251, 253, .80), .glassEdge = rgba(255, 255, 255, .75), .scrim = rgba(30, 44, 60, .22),
            .heroInk = QColor("#172433"), .heroInk2 = rgba(23, 36, 51, .82), .heroVeil = QColor(240, 245, 250),
            .heroScrim = .78, .heroTop = .80, .railFog = .45, .groundShade = QColor(20, 40, 34), .groundGlint = .35,
            .accentInk = QColor("#2D4961"), .okInk = QColor("#1B4F34"), .warnInk = QColor("#5E3F0E"), .dangerInk = QColor("#74312A")};
        return ocean;
    }
    // 青瓷。
    static const Tokens emerald = {
        .primary = QColor("#457F70"), .deep = QColor("#3D7A69"), .primaryHover = QColor("#3E7466"),
        .primaryPressed = QColor("#34625A"),
        .page = QColor("#F2F7F5"), .main = QColor("#F2F7F5"), .card = QColor("#FAFCFB"),
        .subtle = rgba(255, 255, 255, .58), .hover = rgba(24, 60, 48, .05), .selected = rgba(46, 150, 116, .19),
        .input = rgba(255, 255, 255, .58),
        .text = QColor("#1D2A26"), .secondary = QColor("#44554E"), .disabled = QColor("#879490"),
        .stroke = rgba(24, 56, 46, .16), .divider = rgba(24, 56, 46, .09), .focus = rgba(69, 127, 112, .55),
        .thumb = rgba(24, 56, 46, .22), .track = QColor(Qt::transparent),
        .logWindow = QColor("#EEF3F0"), .logToolbar = QColor("#F6FAF8"), .logBase = QColor("#FCFEFD"),
        .logHint = QColor("#64776D"), .logBorder = QColor("#9CCDBB"), .logDivider = QColor("#B2D6C5"),
        .onPrimary = QColor("#FFFFFF"), .accentSoft2 = rgba(69, 127, 112, .08), .fillPress = rgba(24, 60, 48, .09),
        .fillSoftHover = rgba(255, 255, 255, .80),
        .ok = QColor("#2E875A"), .warn = QColor("#A16D1A"), .danger = QColor("#BC5246"),
        .okSoft = rgba(46, 135, 90, .12), .warnSoft = rgba(161, 109, 26, .12), .dangerSoft = rgba(188, 82, 70, .12),
        .dangerSolid = QColor("#A4524A"), .dangerSolidHover = QColor("#964A43"), .dangerSolidPressed = QColor("#86413B"),
        .onDanger = QColor("#FFFFFF"),
        .glass = rgba(250, 252, 251, .80), .glassEdge = rgba(255, 255, 255, .75), .scrim = rgba(30, 50, 44, .22),
        .heroInk = QColor("#16241F"), .heroInk2 = rgba(22, 36, 31, .82), .heroVeil = QColor(240, 246, 243),
        .heroScrim = .78, .heroTop = .80, .railFog = .45, .groundShade = QColor(20, 40, 34), .groundGlint = .35,
        .accentInk = QColor("#234A3F"), .okInk = QColor("#1B4F34"), .warnInk = QColor("#5E3F0E"), .dangerInk = QColor("#74312A")};
    return emerald;
}

/// 平台现在是不是深色那一档。
///
/// 为什么要单独问一句，而不是继续拿 `isDark()`（= 应用调色板的明度）：从
/// 起「跟着系统」也会把一份**我们自己算的**调色板铺到应用上（这样按钮 / 输入框那些
/// 样式表颜色才能从调色板里读现成的，换配色不用重挂整份样式表）。铺过之后应用调色板
/// 就是"上一次算出来的那一份"，再拿它判深浅会自己咬自己。所以这一位先问平台的
/// `colorScheme()`；平台不认（offscreen 就是 Unknown）才退回基样式那份标准调色板的明度。
bool platformColorSchemeDark() {
    if (QGuiApplication::instance() != nullptr) {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) {
            return true;
        }
        if (scheme == Qt::ColorScheme::Light) {
            return false;
        }
    }
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    return app != nullptr && app->style() != nullptr &&
           app->style()->standardPalette().color(QPalette::Window).lightness() < 128;
}

QString handFamily(HandFont which) {
    static bool loaded = false;
    static QString families[3];
    if (!loaded && QGuiApplication::instance() != nullptr) {
        loaded = true;
        initTitleFontResources();
        const char *paths[3] = {":/assets/fonts/LXGWWenKai-Regular.ttf",
                                ":/assets/fonts/LXGWWenKai-Light.ttf",
                                ":/assets/fonts/DancingScript-Regular.ttf"};
        for (int i = 0; i < 3; ++i) {
            const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(paths[i]));
            const QStringList names = id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
            if (!names.isEmpty()) families[i] = names.first();
        }
    }
    return families[int(which)];
}

QStringList quoteFamilies(QuoteScript script) {
    const QString base = QApplication::font().family();
    switch (script) {
    case QuoteScript::Kana:
        return {QStringLiteral("Yu Mincho"), QStringLiteral("Yu Gothic UI"),
                QStringLiteral("Meiryo UI"), base};
    case QuoteScript::Han: {
        const QString kai = handFamily(HandFont::KaiLight);
        return kai.isEmpty() ? QStringList{base} : QStringList{kai, base};
    }
    case QuoteScript::Latin: {
        const QString latin = handFamily(HandFont::Script);
        return latin.isEmpty() ? QStringList{base} : QStringList{latin, base};
    }
    }
    return {base};
}

QFont quoteFont(QFont font, QuoteScript script, int pixelSize) {
    font.setFamilies(quoteFamilies(script));
    font.setPixelSize(pixelSize);
    font.setBold(false);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    return font;
}

int fitPixelSize(QFont font, const QString &text, int width, int hi, int lo) {
    for (int px = hi; px > lo; --px) {
        font.setPixelSize(px);
        if (QFontMetrics(font).horizontalAdvance(text) <= width) return px;
    }
    return lo;
}

QFont handFont(QFont font, HandFont which) {
    const QString family = handFamily(which);
    if (family.isEmpty()) return font;
    // 西文也走这一支（文楷自带手写感的拉丁字形）；它缺的字落回系统字体。只给 markDisplay() 那四处用。
    font.setFamilies({family, QApplication::font().family()});
    font.setBold(false);
    return font;
}

Tokens tokens() {    const QString preset = current().presetId;
    Tokens t = tokens(preset == QLatin1String("system")
                          ? (platformColorSchemeDark() ? QStringLiteral("dark")
                                                       : QStringLiteral("emerald"))
                          : preset);
    const auto overrideRole = [](Role role, QColor &color) {
        const QColor value = current().roles.value(int(role));
        if (value.isValid()) color = value;
    };
    overrideRole(Role::Window, t.page);
    overrideRole(Role::Base, t.input);
    overrideRole(Role::Base, t.card);
    overrideRole(Role::AlternateBase, t.main);
    overrideRole(Role::WindowText, t.text);
    overrideRole(Role::PlaceholderText, t.secondary);
    overrideRole(Role::Button, t.subtle);
    overrideRole(Role::Highlight, t.selected);
    overrideRole(Role::Mid, t.divider);
    return t;
}

namespace {

// Qt 的 :focus 同时包含鼠标和键盘焦点，而这一套界面只对**键盘到达**画焦点环（F1）：
// 按钮、输入框、下拉框、数字框都一样 —— 鼠标点进来只把底提亮一档，不画线。
//
// 落法：应用级一个过滤器记着"最后一次输入是不是键盘"（非修饰键的 `KeyPress` 置上，
// 任何 `MouseButtonPress` 摘掉），焦点进来时按这一条挂 `wtfmlKeyboardFocus`
// （样式表按它选那个 3px 淡环、按钮和自绘环的控件也读它）；`FocusOut` 摘掉。
// **判据不能是 `QFocusEvent::reason()`**：Qt 把焦点从藏起来的那一页交接给新页、以及代码里
// `setFocus(Qt::TabFocusReason)`，报的都是 `TabFocusReason` —— 和用户真按 Tab 一模一样。
// 不清除真实焦点、不改 Tab 顺序，也不影响输入框和列表的焦点行为。
// 条目视图（列表 / 树）带着键盘焦点时，当前行上有一圈可见标记（和别的控件的焦点环同一个色）。
//
// 行是各个代理自己画的，样式那条 `PE_FrameFocusRect` 管不到它们，所以标记是视口上的一块
// 透明小控件：只在「视图有键盘焦点（`wtfmlKeyboardFocus`）且有当前行」时露出，盖在当前行上；
// 视口一重画（滚动、展开、换当前行、挂 / 摘属性都会引起）就按当前行重算一次位置。
// 鼠标点进来、或焦点离开，属性被摘掉，标记随之收起。
class ItemViewFocusMarker : public QWidget {
public:
    static void refresh(QAbstractItemView *view) {
        if (view == nullptr || view->viewport() == nullptr) return;
        auto *marker = static_cast<ItemViewFocusMarker *>(
            view->viewport()->findChild<QWidget *>(QStringLiteral("wtfmlItemViewFocusMarker"), Qt::FindDirectChildrenOnly));
        if (marker == nullptr) {
            if (!itemViewKeyboardFocus(view)) return;  // 没有要露的东西，也就不必建
            marker = new ItemViewFocusMarker(view);
        }
        marker->sync();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(focusRingColor(palette()));
        pen.setWidthF(2.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_view->viewport() && (event->type() == QEvent::Paint || event->type() == QEvent::Resize) &&
            !m_pending) {
            m_pending = true;  // 画完这一帧再摆：过滤器跑在绘制之前
            QTimer::singleShot(0, this, [this] {
                m_pending = false;
                sync();
            });
        }
        return false;
    }

private:
    explicit ItemViewFocusMarker(QAbstractItemView *view) : QWidget(view->viewport()), m_view(view) {
        setObjectName(QStringLiteral("wtfmlItemViewFocusMarker"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
        view->viewport()->installEventFilter(this);
    }
    void sync() {
        QRect target;
        if (m_view != nullptr && itemViewKeyboardFocus(m_view)) {
            const QModelIndex current = m_view->currentIndex();
            if (current.isValid()) {
                QRect row = m_view->visualRect(current);
                // 树的行要铺满视口宽（visualRect 只给第 0 列那一格）。
                if (qobject_cast<QTreeView *>(m_view) != nullptr) row = QRect(0, row.y(), m_view->viewport()->width(), row.height());
                target = row.adjusted(1, 0, -1, 0).intersected(m_view->viewport()->rect());
            }
        }
        if (target.isEmpty()) {
            hide();
            return;
        }
        const bool wasHidden = isHidden();
        if (geometry() != target) setGeometry(target);
        if (wasHidden) {
            show();
            raise();
        }
    }
    QPointer<QAbstractItemView> m_view;
    bool m_pending = false;
};

class ControlFocusVisibility : public QObject {
public:
    explicit ControlFocusVisibility(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlControlFocusVisibility"));
        app.installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            m_keyboard = false;
            setVisible(qobject_cast<QAbstractButton *>(QApplication::focusWidget()), false);
            setRingOwner(QApplication::focusWidget(), false);
            setVisible(button, false);
            setKeyboard(watched, false);
        } else if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                clampSpin(qobject_cast<QWidget *>(watched));
            }
            if (key != Qt::Key_Shift && key != Qt::Key_Control && key != Qt::Key_Alt && key != Qt::Key_Meta) {
                m_keyboard = true;
                setVisible(qobject_cast<QAbstractButton *>(QApplication::focusWidget()), true);
                setRingOwner(QApplication::focusWidget(), true);
            }
            // A native frameless window can receive its first Tab before Qt has
            // assigned any child focus. Recover that entry only; existing editors,
            // popup/dialog focus and normal Tab traversal keep their own handling.
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            auto *receiver = qobject_cast<QWidget *>(watched);
            auto *host = receiver ? receiver->window() : nullptr;
            if ((key == Qt::Key_Tab || key == Qt::Key_Backtab) &&
                !(keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                (QApplication::focusWidget() == nullptr || QApplication::focusWidget() == host) && host != nullptr && host->isWindow()) {
                const bool backward = key == Qt::Key_Backtab || keyEvent->modifiers().testFlag(Qt::ShiftModifier);
                QWidget *candidate = backward ? host->previousInFocusChain() : host->nextInFocusChain();
                while (candidate != host) {
                    if (candidate->isVisible() && candidate->isEnabled() &&
                        (candidate->focusPolicy() & Qt::TabFocus) && candidate->window() == host) {
                        candidate->setFocus(backward ? Qt::BacktabFocusReason : Qt::TabFocusReason);
                        if (QApplication::focusWidget() == candidate) { event->accept(); return true; }
                        break;
                    }
                    candidate = backward ? candidate->previousInFocusChain() : candidate->nextInFocusChain();
                }
                // If the window itself was assigned focus before its child chain
                // joined, enter the visible descendant chain directly.
                auto children = host->findChildren<QWidget *>();
                if (backward) std::reverse(children.begin(), children.end());
                for (auto *child : children) {
                    if (child->isVisible() && child->isEnabled() && (child->focusPolicy() & Qt::TabFocus) && child->window() == host) {
                        child->setFocus(backward ? Qt::BacktabFocusReason : Qt::TabFocusReason);
                        if (QApplication::focusWidget() && QApplication::focusWidget() != host) { event->accept(); return true; }
                    }
                }
            }
        } else if (event->type() == QEvent::FocusIn) {
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason == Qt::MouseFocusReason) {
                m_keyboard = false;
            }
            // 三个分支同一条判据（"最后一次输入是不是键盘"）：按钮看样式表、输入类控件由
            // `setKeyboard()` 标里外两层、自绘环的控件读同一个属性。**别按 `reason()` 判** ——
            // 换页交接和真按 Tab 报的是同一个理由，那样鼠标换页后新页第一个控件就会亮一圈。
            setVisible(button, m_keyboard);
            setRingOwner(qobject_cast<QWidget *>(watched), m_keyboard);
            setKeyboard(watched, m_keyboard);
        } else if (event->type() == QEvent::FocusOut) {
            clampSpin(qobject_cast<QWidget *>(watched));
            setKeyboard(watched, false);
            setRingOwner(qobject_cast<QWidget *>(watched), false);
        }
        return false;
    }
private:
    /// 输入类控件：`widgetStyleSheet()` 里那条 `[wtfmlKeyboardFocus="true"]:focus` 就按这几种写。
    static bool isInput(QWidget *widget) {
        return qobject_cast<QLineEdit *>(widget) != nullptr ||
               qobject_cast<QComboBox *>(widget) != nullptr ||
               qobject_cast<QAbstractSpinBox *>(widget) != nullptr ||
               qobject_cast<QTextEdit *>(widget) != nullptr ||
               qobject_cast<QPlainTextEdit *>(widget) != nullptr;
    }
    static void mark(QWidget *widget, bool on) {
        if (widget == nullptr || (widget->property("wtfmlKeyboardFocus").isValid() &&
                                  widget->property("wtfmlKeyboardFocus").toBool() == on)) return;
        widget->setProperty("wtfmlKeyboardFocus", on);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
    /// 焦点落在哪一格：数字框 / 可编辑下拉框的 `focusProxy` 是**里面**那一格 `QLineEdit`，
    /// 而 3px 淡环要画在外面那一圈上 —— 所以里头那一格和外面那一层一起标。
    /// 里面那一格身上虽然也挂着属性，但它的边框被 `QAbstractSpinBox QLineEdit` 那条复位成
    /// `none`，画不出第二条环。
    static void setKeyboard(QObject *watched, bool on) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!isInput(widget)) return;
        mark(widget, on);
        if (auto *parent = widget->parentWidget();
            qobject_cast<QAbstractSpinBox *>(parent) != nullptr || qobject_cast<QComboBox *>(parent) != nullptr) {
            mark(parent, on);
        }
        if (qobject_cast<QAbstractSpinBox *>(widget) != nullptr || qobject_cast<QComboBox *>(widget) != nullptr) {
            const auto edits = widget->findChildren<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
            for (auto *edit : edits) mark(edit, on);
        }
    }
    /// 数字框「越界回最近合法值」：Qt 出厂只做**回退**（打进 99999 按 Tab 会退回上一个值），
    /// 这里补成**钳到边界**（99999 → 最大值）。只处理「文本能解析成一个数、但越界」那一档：
    /// 打了一半（空 / 只有负号）或特殊值文本（`跟随全局`）解析不出来，仍交给 Qt 自己回退。
    ///
    /// ★ 这次只**读**文本、算出该落到的值，真正的 `setValue()` 排到下一拍（`QTimer::singleShot`
    /// 带控件当上下文，控件没了就自动不打）：焦点转移那一刻直接改值，会顺着
    /// `valueChanged` 把页面 / 对话框（服务端设置页那几颗表内编辑框就是）在别人的
    /// FocusOut 事件里拆掉，mw_check 当场段错误 —— 这是实测撞过的，别改回同步。
    static void clampSpin(QObject *watched) {
        auto *spin = qobject_cast<QAbstractSpinBox *>(watched);
        if (spin == nullptr) {
            // 焦点其实在数字框**里面那一格** QLineEdit 上。
            auto *edit = qobject_cast<QLineEdit *>(watched);
            if (edit == nullptr || edit->parentWidget() == nullptr) return;
            spin = qobject_cast<QAbstractSpinBox *>(edit->parentWidget());
            if (spin == nullptr) return;
        }
        // `QAbstractSpinBox::lineEdit()` 是 protected，这里按直接子对象找那一格。
        auto *edit = spin->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
        if (edit == nullptr) return;
        QString bare = edit->text();
        QVariant target;
        if (auto *ints = qobject_cast<QSpinBox *>(spin); ints != nullptr) {
            bare.remove(ints->prefix());
            bare.remove(ints->suffix());
            bool ok = false;
            const int typed = QLocale().toInt(bare.trimmed(), &ok);
            if (!ok) return;
            const int clamped = qBound(ints->minimum(), typed, ints->maximum());
            if (clamped == ints->value()) return;
            target = clamped;
        } else if (auto *real = qobject_cast<QDoubleSpinBox *>(spin); real != nullptr) {
            bare.remove(real->prefix());
            bare.remove(real->suffix());
            bool ok = false;
            const double typed = QLocale().toDouble(bare.trimmed(), &ok);
            if (!ok) return;
            const double clamped = qBound(real->minimum(), typed, real->maximum());
            if (qFuzzyCompare(clamped, real->value())) return;
            target = clamped;
        } else {
            return;
        }
        const QPointer<QAbstractSpinBox> guard(spin);
        QTimer::singleShot(0, spin, [guard, target]() {
            if (guard.isNull()) return;
            if (auto *ints = qobject_cast<QSpinBox *>(guard.data())) {
                ints->setValue(target.toInt());
            } else if (auto *real = qobject_cast<QDoubleSpinBox *>(guard.data())) {
                real->setValue(target.toDouble());
            }
        });
    }
    static void setVisible(QAbstractButton *button, bool visible) {
        if (button == nullptr || (button->property("wtfmlKeyboardFocus").isValid() &&
                                  button->property("wtfmlKeyboardFocus").toBool() == visible)) return;
        button->setProperty("wtfmlKeyboardFocus", visible);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    /// 自绘焦点环的控件（挂了 `wtfmlFocusRingOwner`，如设置页分段页签）：和按钮同一条判据。
    /// 条目视图（列表 / 树）同样按这一条判据挂属性：行尾按钮和当前行标记都读它。
    static void setRingOwner(QWidget *widget, bool visible) {
        auto *view = qobject_cast<QAbstractItemView *>(widget);
        if (widget == nullptr || (view == nullptr && !widget->property("wtfmlFocusRingOwner").toBool()) ||
            widget->property("wtfmlKeyboardFocus").toBool() == visible) return;
        widget->setProperty("wtfmlKeyboardFocus", visible);
        widget->update();
        if (view != nullptr) {
            view->viewport()->update();  // 行是画在视口上的
            ItemViewFocusMarker::refresh(view);
        }
    }
    bool m_keyboard = false;
};

/// 焦点不在输入框/多行文本/列表和树/下拉框/数字框/滑块上（包括无焦点或焦点在页眉/导航栏）时，
/// 方向键（上/下）和翻页键（PageUp/PageDown）把最上层可见页面的滚动区滚一步/一页。
class PageScrollKeyFilter : public QObject {
public:
    static void install(QApplication &app) {
        const QString name = QStringLiteral("wtfmlPageScrollKeyFilter");
        if (app.findChild<QObject *>(name, Qt::FindDirectChildrenOnly) != nullptr) {
            return;
        }
        auto *filter = new PageScrollKeyFilter(&app);
        filter->setObjectName(name);
        app.installEventFilter(filter);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::KeyPress) {
            return false;
        }
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int key = keyEvent->key();
        if (key != Qt::Key_Up && key != Qt::Key_Down &&
            key != Qt::Key_PageUp && key != Qt::Key_PageDown) {
            return false;
        }
        const Qt::KeyboardModifiers mods =
            keyEvent->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                     Qt::AltModifier | Qt::MetaModifier);
        if (mods != Qt::NoModifier || QApplication::activePopupWidget() != nullptr) {
            return false;
        }

        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget == nullptr) {
            return false;
        }
        QWidget *win = widget->window();
        if (win == nullptr) {
            return false;
        }

        QWidget *focus = QApplication::focusWidget();
        if (focus == nullptr || focus->window() != win) {
            focus = win->focusWidget();
        }
        if (isArrowKeyControl(focus)) {
            if (qobject_cast<QScrollArea *>(watched) != nullptr) {
                event->ignore();
                return true;
            }
            return false;
        }

        if (auto *area = qobject_cast<QScrollArea *>(watched)) {
            QScrollBar *bar = area->verticalScrollBar();
            if (bar != nullptr && bar->maximum() > bar->minimum()) {
                return false;
            }
            if (QScrollArea *topArea = findTopmostScrollArea(win, focus);
                topArea != nullptr && topArea != area) {
                triggerScroll(topArea, key);
                event->accept();
                return true;
            }
            return false;
        }

        if (watched == win ||
            (qobject_cast<QAbstractButton *>(watched) != nullptr &&
             !watched->property("wtfmlKeyCaptureArmed").toBool())) {
            if (QScrollArea *topArea = findTopmostScrollArea(win, focus)) {
                triggerScroll(topArea, key);
                event->accept();
                return true;
            }
        }
        return false;
    }

private:
    explicit PageScrollKeyFilter(QObject *parent) : QObject(parent) {}

    static bool isArrowKeyControl(QWidget *w) {
        if (w == nullptr) {
            return false;
        }
        const auto matches = [](QWidget *x) {
            return qobject_cast<QLineEdit *>(x) != nullptr ||
                   qobject_cast<QTextEdit *>(x) != nullptr ||
                   qobject_cast<QPlainTextEdit *>(x) != nullptr ||
                   qobject_cast<QAbstractItemView *>(x) != nullptr ||
                   qobject_cast<QComboBox *>(x) != nullptr ||
                   qobject_cast<QAbstractSpinBox *>(x) != nullptr ||
                   qobject_cast<QAbstractSlider *>(x) != nullptr;
        };
        if (matches(w)) {
            return true;
        }
        if (QWidget *parent = w->parentWidget()) {
            if (auto *area = qobject_cast<QAbstractScrollArea *>(parent);
                area != nullptr && area->viewport() == w && matches(area)) {
                return true;
            }
            if (qobject_cast<QComboBox *>(parent) != nullptr ||
                qobject_cast<QAbstractSpinBox *>(parent) != nullptr) {
                return true;
            }
        }
        return false;
    }

    static QWidget *topmostVisibleRoot(QWidget *win) {
        if (win == nullptr) {
            return nullptr;
        }
        const auto children = win->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            QWidget *child = *it;
            if (child != nullptr && child->isVisible() &&
                child->property("wtfmlGlassPopover").toBool()) {
                return child;
            }
        }
        if (auto *overlay = win->findChild<QWidget *>(QStringLiteral("overlayHost"))) {
            if (overlay->isVisible()) {
                if (auto *prompt = overlay->findChild<QWidget *>(QStringLiteral("overlayPromptHost"));
                    prompt != nullptr && prompt->isVisible()) {
                    return prompt;
                }
                return overlay;
            }
        }
        if (auto *tabs = win->findChild<QTabWidget *>(QStringLiteral("mainTabWidget"))) {
            if (QWidget *current = tabs->currentWidget(); current != nullptr && current->isVisible()) {
                return current;
            }
        }
        return win;
    }

    static QScrollArea *findTopmostScrollArea(QWidget *win, QWidget *focus) {
        QWidget *root = topmostVisibleRoot(win);
        if (root == nullptr) {
            return nullptr;
        }
        QList<QScrollArea *> candidates;
        if (auto *selfArea = qobject_cast<QScrollArea *>(root);
            selfArea != nullptr && selfArea->isVisible()) {
            QScrollBar *bar = selfArea->verticalScrollBar();
            if (bar != nullptr && bar->maximum() > bar->minimum()) {
                candidates.append(selfArea);
            }
        }
        for (QScrollArea *area : root->findChildren<QScrollArea *>()) {
            if (area == nullptr || !area->isVisible()) {
                continue;
            }
            QScrollBar *bar = area->verticalScrollBar();
            if (bar != nullptr && bar->maximum() > bar->minimum()) {
                candidates.append(area);
            }
        }
        if (candidates.isEmpty()) {
            return nullptr;
        }
        if (focus != nullptr) {
            QScrollArea *deepestForFocus = nullptr;
            for (QScrollArea *area : std::as_const(candidates)) {
                if (area == focus || area->isAncestorOf(focus)) {
                    if (deepestForFocus == nullptr || deepestForFocus->isAncestorOf(area)) {
                        deepestForFocus = area;
                    }
                }
            }
            if (deepestForFocus != nullptr) {
                return deepestForFocus;
            }
        }
        QScrollArea *best = nullptr;
        qint64 bestViewportArea = -1;
        for (QScrollArea *area : std::as_const(candidates)) {
            bool enclosesAnother = false;
            for (QScrollArea *other : std::as_const(candidates)) {
                if (other != area && area->isAncestorOf(other)) {
                    enclosesAnother = true;
                    break;
                }
            }
            if (enclosesAnother) {
                continue;
            }
            const QWidget *vp = area->viewport();
            const qint64 vpArea = vp != nullptr ? qint64(vp->width()) * vp->height() : 0;
            if (best == nullptr || vpArea > bestViewportArea) {
                best = area;
                bestViewportArea = vpArea;
            }
        }
        return best;
    }

    static void triggerScroll(QScrollArea *area, int key) {
        QScrollBar *bar = area != nullptr ? area->verticalScrollBar() : nullptr;
        if (bar == nullptr) {
            return;
        }
        switch (key) {
        case Qt::Key_Up:
            bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
            break;
        case Qt::Key_Down:
            bar->triggerAction(QAbstractSlider::SliderSingleStepAdd);
            break;
        case Qt::Key_PageUp:
            bar->triggerAction(QAbstractSlider::SliderPageStepSub);
            break;
        case Qt::Key_PageDown:
            bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
            break;
        default:
            break;
        }
    }
};

// ---------------------------------------------------------------------------
// 那几张表：每一项都是"画面上哪儿"的唯一出处
// ---------------------------------------------------------------------------

/// 一块骨架今天的样子：`from` 那一档颜色压暗 `shade`，
///
/// - `shade == 0` ⇒ 直接用 `from` 那一档（卡片：今天就是 `Base`）；
/// - `literal` ⇒ 默认值是个写死的色（四周变暗那一层），不从 `from` 推；
/// - `accent` ⇒ 默认值是**主题色那一支**（`accentShade(AccentShade::Pure)`），
/// 既不看 `from` 也不看 `shade`（只有标题栏是）。
struct PartInfo {
    const char *key;
    const char *name;
    int shade;
    QPalette::ColorRole from;
    bool literal;
    bool accent;
};

/// 顺序 = `Part` 的枚举顺序。`static_assert` 在下面盯着。
/// NOTE: 最后那一位（出厂字色是不是白墨）在 `partLightInk()` 里另有一张表 —— 它只有标题栏
/// 用得到，塞进这一张只会让上面五行都跟着多一个恒假的字段。
const PartInfo kPartInfos[] = {
    {"titleBar", QT_TRANSLATE_NOOP("Theme", "标题栏"), 105, QPalette::Window, false, true},
    {"bottomBar", QT_TRANSLATE_NOOP("Theme", "底栏"), 105, QPalette::Window, false, false},
    {"panel", QT_TRANSLATE_NOOP("Theme", "浮层面板"), 112, QPalette::Window, false, false},
    {"card", QT_TRANSLATE_NOOP("Theme", "卡片"), 0, QPalette::Base, false, false},
    {"scrim", QT_TRANSLATE_NOOP("Theme", "四周变暗"), 0, QPalette::Window, true, false},
    // 这一块的 `from` / `shade` 只是"万一有人给它设了个底色"时的兜底算法（今天没有任何
    // 调用点取它的底）—— 它真正的底是 `QTabWidget` 自己画的那一层，**不在调色板里**
    // （见 `partHasBackground()` 那段）。加这一项是为了能单独铺一张图。没设图时它一个
    // 像素都不改。
    {"pageBackground", QT_TRANSLATE_NOOP("Theme", "页面背景"), 0, QPalette::Window, false, false},
};
static_assert(sizeof(kPartInfos) / sizeof(kPartInfos[0]) == static_cast<size_t>(partCount()),
              "kPartInfos 和 Part 的项数对不上 —— 加一块骨架时两处都要动");  // i18n-ignore

/// 一张图的摆法表（顺序 = `ImageFit` 的枚举顺序）。
struct ImageFitInfo {
    const char *key;
    const char *name;
};

const ImageFitInfo kImageFitInfos[] = {
    {"cover", QT_TRANSLATE_NOOP("Theme", "填满并裁剪")},
    {"contain", QT_TRANSLATE_NOOP("Theme", "完整放下")},
    {"stretch", QT_TRANSLATE_NOOP("Theme", "拉伸填满")},
    {"tile", QT_TRANSLATE_NOOP("Theme", "平铺")},
};
static_assert(sizeof(kImageFitInfos) / sizeof(kImageFitInfos[0]) ==
                  static_cast<size_t>(imageFitCount()),
              "kImageFitInfos 和 ImageFit 的项数对不上 —— 加一种摆法时两处都要动");  // i18n-ignore

/// 四周变暗那一层的出厂色。**与 `OverlayHost::scrimColor()` 的字面量一字不差**：
/// 半透明黑，盖在什么底色上都成立。
/// 四周变暗那一层：跟预设的 `scrim` 走（深色压暗、浅色带一点蓝灰），不透明度只取它的 0.6。
/// 用户反馈弹窗一出整块发黑：这一层只该"轻轻压一档"，磨砂风景还得透得出来。
QColor scrimLiteral() {
    QColor color = tokens().scrim;
    // 浅色预设的 scrim 本来就淡（.22），再打六折几乎看不出弹窗下面压了一层：浅色照原值。
    color.setAlphaF(color.alphaF() * (isDark() ? 0.6 : 1.0));
    return color;
}

/// 按钮身上那个"身份"属性名（`Theme::markButton()` 挂的、`widgetStyleSheet()` 里选择器
/// 认的就是它）。**只有这一处出处** —— 两边各写一遍字符串的话，改名字就成了"属性挂上了、
/// 样式一条都没匹配上"，而屏幕上看只是"这颗按钮颜色不对"，查起来要命。
constexpr const char kButtonKindProperty[] = "wtfmlButtonKind";

/// 按钮身上那个"尺寸档"属性（`Theme::markIconButton()` 挂的）。今天只有一个值：图标按钮。
/// **只有这一处出处**，理由同上（改名字 = 属性挂上了、选择器一条都没匹配到）。
constexpr const char kButtonSizeProperty[] = "wtfmlButtonSize";
constexpr const char kIconButtonValue[] = "icon";

/// 页面骨架那几层"面"挂在控件上的属性名 / 属性值。
///
/// 设计上第一条规则是"减少框"：整页只留**两个**面
/// （侧栏 + 正文），其余靠排版分层。这一套属性就是那两个面（外加"小节卡片"和"页脚"
/// 两档）的**唯一出处** —— 产品那边 `setProperty()` 写的是这两个常量，样式表里那几条
/// 选择器读的也是它们。两边各写一遍字符串的话，改名字就成了"属性挂上了、样式一条都没
/// 匹配上"，而屏幕上看只是"这一块没有底色"。
constexpr const char kSurfaceProperty[] = "wtfmlSurface";
/// 文字角色（页面标题 / 小节标题 / 次要说明）挂在 QLabel 上的属性名。
constexpr const char kTextRoleProperty[] = "wtfmlTextRole";

/// 它们那几个值。**只有这一处**（产品、样式表、验证工程三方共用，见头文件那段）。
constexpr const char kSurfaceSidebar[] = "sidebar";
constexpr const char kSurfaceWorkspace[] = "workspace";
constexpr const char kSurfaceSection[] = "section";
constexpr const char kSurfaceFooter[] = "footer";
constexpr const char kTextRolePageTitle[] = "pageTitle";
constexpr const char kTextRoleSectionTitle[] = "sectionTitle";
constexpr const char kTextRoleSecondary[] = "secondary";

/// 把 @p color 按 @p percent 罩在白上（`percent` = 0 给全白、100 给 @p color 本色）。
QColor mixWithWhite(const QColor &color, int percent) {
    const int p = qBound(0, percent, 100);
    const auto mix = [p](int top) { return (255 * (100 - p) + top * p + 50) / 100; };
    return QColor(mix(color.red()), mix(color.green()), mix(color.blue()));
}

/// 把 @p color 按 @p percent 混进**黑**里（`percent` = 0 给全黑、100 给 @p color 本色）。
///
/// 和 `mixWithWhite()` 是同一族（同一个式子，只是那一头是 `0` 而不是 `255`）——
/// **主题色那一支里"深一点"那两档走的就是它**，不走 `QColor::darker()`。
QColor mixWithBlack(const QColor &color, int percent) {
    const int p = qBound(0, percent, 100);
    const auto mix = [p](int c) { return (c * p + 50) / 100; };
    return QColor(mix(color.red()), mix(color.green()), mix(color.blue()));
}

/// 把 @p color 按 @p percent 混进 @p base（`percent` = 0 给 @p base 本色、100 给 @p color
/// 本色）。和 `rowHoverFill()` / `selectionFill()` 用的是**同一个式子**（含那一位 `+50`）
/// —— 三处都是"从这一块自己的底色出发往主题色那头混"，差 1 就没法逐字节比了。
QColor mixToward(const QColor &base, const QColor &color, int percent) {
    const int p = qBound(0, percent, 100);
    const auto mix = [p](int b, int a) { return (b * (100 - p) + a * p + 50) / 100; };
    return QColor(mix(base.red(), color.red()), mix(base.green(), color.green()),
                  mix(base.blue(), color.blue()));
}

/// 能单独改的调色板角色表（顺序 = `Role` 的枚举顺序）。
struct RoleInfo {
    const char *key;
    const char *name;
    QPalette::ColorRole role;
};

const RoleInfo kRoleInfos[] = {
    {"window", QT_TRANSLATE_NOOP("Theme", "内容区底色"), QPalette::Window},
    {"base", QT_TRANSLATE_NOOP("Theme", "输入框/列表底色"), QPalette::Base},
    {"alternateBase", QT_TRANSLATE_NOOP("Theme", "隔行底色"), QPalette::AlternateBase},
    {"text", QT_TRANSLATE_NOOP("Theme", "输入框/列表里的字"), QPalette::Text},
    {"windowText", QT_TRANSLATE_NOOP("Theme", "正文"), QPalette::WindowText},
    {"button", QT_TRANSLATE_NOOP("Theme", "按钮底色"), QPalette::Button},
    {"buttonText", QT_TRANSLATE_NOOP("Theme", "按钮上的字"), QPalette::ButtonText},
    {"highlight", QT_TRANSLATE_NOOP("Theme", "选中/悬停"), QPalette::Highlight},
    {"highlightedText", QT_TRANSLATE_NOOP("Theme", "选中项文字"), QPalette::HighlightedText},
    {"placeholderText", QT_TRANSLATE_NOOP("Theme", "次要文字"), QPalette::PlaceholderText},
    {"mid", QT_TRANSLATE_NOOP("Theme", "分隔线"), QPalette::Mid},
};
static_assert(sizeof(kRoleInfos) / sizeof(kRoleInfos[0]) == static_cast<size_t>(roleCount()),
              "kRoleInfos 和 Role 的项数对不上 —— 加一个角色时两处都要动");  // i18n-ignore

/// 文字不透明度落在哪几个角色上。**只有带字的角色**：`Highlight` 那种是"一块底"，
/// 跟着变淡的话选中那一块就淡了，那不是"文字透明度"。
///
/// NOTE: `ToolTipText` 也在这儿，它**不在** `kRoleInfos` 里（用户调不到它，但它是字，
/// 全局那一位得管它，不然提示框里的字会比别处清楚）。
const QPalette::ColorRole kInkRoles[] = {
    QPalette::WindowText, QPalette::Text,        QPalette::ButtonText,
    QPalette::HighlightedText, QPalette::PlaceholderText, QPalette::ToolTipText,
};

QPalette::ColorGroup colorGroups[] = {QPalette::Active, QPalette::Inactive, QPalette::Disabled};

// ---------------------------------------------------------------------------
// 全局那一份
// ---------------------------------------------------------------------------

Spec &mutableSpec() {
    static Spec spec = defaultSpec();
    return spec;
}


int clampOpacity(int value) {
    return qBound(0, value, 100);
}

/// 调色板里"正文/次要文字从哪儿取"那两档的当前值（`partForeground()` 的兜底用）。
QColor appColor(QPalette::ColorRole role) {
    if (QGuiApplication::instance() == nullptr) {
        return QColor();
    }
    return QGuiApplication::palette().color(role);
}

// ---------------------------------------------------------------------------
// 存盘的键
// ---------------------------------------------------------------------------

const char *const kPresetKey = "Theme/Preset";
const char *const kTextOpacityKey = "Theme/TextOpacity";
/// **单数**：主题色（"这一套界面是什么颜色"）。底下那个复数前缀是**语义色**
/// （"红黄绿各是哪一个红黄绿"）。两个前缀只差一个 s，别看串了：往 `Theme/Accents/` 里写
/// 一个 "Accent" 是不生效的（`load()` 读的是这个单数键）。
const char *const kAccentKey = "Theme/Accent";
const char *const kRolesPrefix = "Theme/Roles/";
const char *const kPartsPrefix = "Theme/Parts/";
const char *const kAccentsPrefix = "Theme/Accents/";

/// 色存成 `#aarrggbb`（`QColor::HexArgb`）—— 注册表里能手看、能手改，和别的设置一个待遇。
QString colorToString(const QColor &color) {
    return color.isValid() ? color.name(QColor::HexArgb) : QString();
}

QColor colorFromString(const QString &text) {
    const QColor color = QColor::fromString(text);
    // 认不出的字符串（手改歪了）当"没设"：`fromString()` 对垃圾给一个无效色。
    return color.isValid() ? color : QColor();
}

QString roleSettingKey(Role role) {
    return QLatin1String(kRolesPrefix) + QLatin1String(kRoleInfos[static_cast<int>(role)].key);
}

QString partSettingKey(Part part, const char *field) {
    return QLatin1String(kPartsPrefix) + QLatin1String(kPartInfos[static_cast<int>(part)].key) +
           QLatin1Char('/') + QLatin1String(field);
}

QString accentSettingKey(const char *field) {
    return QLatin1String(kAccentsPrefix) + QLatin1String(field);
}

// ---------------------------------------------------------------------------
// 我们那一份代理样式（`wtfml-fusion`）
// ---------------------------------------------------------------------------

/**
 * @brief 产品+三个验证工程共用的那一份代理样式。**它接手一件事**：
 *
 * **选中那一块**（`PE_PanelItemViewItem` / `PE_PanelItemViewRow` 两条原语 +
 * `CE_ItemViewItem` 里换墨）—— 选中那一块不用平台那档蓝色，而是画成
 * "这一行自己的底色混一档主题色"（和侧栏导航同款，不画指示条）。
 * 为什么落在这一层：调色板的 `Highlight` 是平台给的蓝，而"出厂那一份不碰调色板"这条
 * 不能破（破=把"跟着系统深浅"钉死，见 `apply()` 那段）。样式这一层一份代码就管住
 * 全仓每一张表，连不用我们那两个代理的表、以及下拉框弹出来那张表也一起管住了。
 *
 * 【下拉框那颗 `∨` 已经不画了】原来这里挂了个 `ComboChevronPainter` 在 `::drop-down`
 * 那一格里补一个 V（7b 的规格）。8 号反馈之后那条规格撤了：`::drop-down` 宽度写 0，
 * 整块可点，右上角不再有任何箭头，那个过滤器连同它画的 V 一起删掉。
 */
namespace {

}  // namespace

class WtfmlProxyStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    /// tokens-r2 §1：按钮用系统字 14 px、字重 500（手写体只留品牌 / 主页英雄区 / 页标题 / 空状态）。
    /// 代码里自己 setFont 过的（主页大按钮之类）不动。
    void polish(QWidget *w) override
    {
        QProxyStyle::polish(w);
        // 设计系统默认生效（列表代理 / 视口透明 / 栈换页 / 文本框去框），见 DesignDefaults.hpp。
        DesignDefaults::apply(w);
        auto *button = qobject_cast<QPushButton *>(w);
        if (button == nullptr || button->testAttribute(Qt::WA_SetFont)) return;
        QFont font = button->font();
        font.setPixelSize(textSize(TextRole::Body));
        font.setWeight(QFont::Medium);
        button->setFont(font);
    }
    using QProxyStyle::polish;

    /// "这一条原语该不该由我们画成那块浅灰"—— 是的话给出那一份 option，不是给 nullptr。
    ///
    /// 判据和基样式里那两条逐字一样（`if (showDecorationSelected && State_Selected)`），
    /// 不自己发明第二条：`showDecorationSelected` 是"选中那一块铺整行还是只铺文字那一格"
    /// 这个开关，样式说 false 的地方基样式本来就不铺，我们补上去等于凭空多画一块。
    static const QStyleOptionViewItem *selectedItemOption(const QStyleOption *opt)
    {
        const auto *vopt = qstyleoption_cast<const QStyleOptionViewItem *>(opt);
        if (vopt == nullptr || !vopt->showDecorationSelected ||
            (vopt->state & QStyle::State_Selected) == 0) {
            return nullptr;
        }
        // 只接管"铺**平台那一档**选中色"的地方。谁**自己**把选中色换掉了，就交回给基样式
        // —— 下拉框弹出来那张表正是（`widgetStyleSheet()` 里那条 `selection-background-color`
        // 是刻意做成的"主题色打底 + 黑墨"）。判据是"这一份 option 里的
        // `Highlight` 和**应用调色板**里那一档是不是同一个色"：一样 ⇒ 它没自己换过。
        //
        // 比的是 **Active 那一组**，不是 `color(role)` 那个 Current：窗口一失焦，控件身上
        // 的 `Highlight` 就落到 Inactive 那一组（Fusion 给的是另一种淡蓝），拿 Current 比
        // 会得到"窗口一失焦我们就撒手"—— 那个 bug 只在失焦时出现，屏幕上看着像"有时灰
        // 有时蓝"。比 8 位的那三个数（`.rgb()`），不用 `QColor::operator==`：Qt 6 那个比
        // 的是内部 16 位分量，两处各构造一次就可能不相等（这条坑见 `ui_check` 里那段）。
        if (vopt->palette.color(QPalette::Active, QPalette::Highlight).rgb() !=
            platformSelectionColor().rgb()) {
            return nullptr;
        }
        return vopt;
    }

    /// 平台那一档选中色（= 应用调色板里那一档；我们从不去改它，见 `apply()` 那段说明）。
    static QColor platformSelectionColor()
    {
        return QGuiApplication::palette().color(QPalette::Active, QPalette::Highlight);
    }

    /// 数字框右边那一列上下箭头：Fusion 拿 `Button` 色现算按钮底时用 `setHsv()` 把 alpha 抹成
    /// 255，我们那档半透明白（subtle）就变成一整列不透明的白块（深色主题下最扎眼的一条硬边）。
    /// 这里先把 `Button` 换成「页面底 + 半透明白」合成后的不透明色，按钮底就和框里那片同色。
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p,
                            const QWidget *w) const override
    {
        if (cc == CC_SpinBox && opt != nullptr) {
            if (const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(opt)) {
                QStyleOptionSpinBox copy(*spin);
                const QColor page = copy.palette.color(QPalette::Window);
                const QColor over = copy.palette.color(QPalette::Base);
                const qreal a = over.alphaF();
                const QColor flat = QColor::fromRgbF(page.redF() * (1 - a) + over.redF() * a,
                                                     page.greenF() * (1 - a) + over.greenF() * a,
                                                     page.blueF() * (1 - a) + over.blueF() * a);
                copy.palette.setColor(QPalette::Button, flat);
                QProxyStyle::drawComplexControl(cc, &copy, p, w);
                return;
            }
        }
        QProxyStyle::drawComplexControl(cc, opt, p, w);
    }

    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p,
                       const QWidget *w) const override
    {
        // 树的展开小三角一律不画（体验反馈 8「右边的小箭头不要再出现」）：可展开的行
        // 靠整行点击 + 组头样式 / 「展开 · 收起」文字 / 数量说明表达，见 tokens-r2 §6。
        if (pe == PE_IndicatorBranch)
            return;
        if ((pe == PE_IndicatorCheckBox || pe == PE_IndicatorItemViewItemCheck) && opt && p) {
            // 设计稿：18 px、圆角 5；空心 1.5 px ink-3 描边，悬停垫中性灰、描边转 ink-2；
            // 勾上 = 强调色实心 + on-accent 勾；禁用整体 .45。
            const Tokens t = tokens();
            const bool enabled = opt->state & State_Enabled;
            const bool on = opt->state & (State_On | State_NoChange);
            const bool hover = enabled && (opt->state & State_MouseOver);
            const bool down = enabled && (opt->state & State_Sunken);
            const QRectF box = QRectF(opt->rect).adjusted(0.75, 0.75, -0.75, -0.75);
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            if (!enabled) p->setOpacity(0.45);
            if (on) {
                p->setPen(Qt::NoPen);
                p->setBrush(accentShade(AccentShade::Pure));
            } else {
                p->setPen(QPen(hover ? t.secondary : t.disabled, 1.5));
                p->setBrush(down ? t.fillPress : hover ? t.hover : QColor(Qt::transparent));
            }
            p->drawRoundedRect(box, 5, 5);
            p->setPen(QPen(t.onPrimary, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const auto point = [&box](qreal x, qreal y) {
                return QPointF(box.left() + box.width() * x, box.top() + box.height() * y);
            };
            if (opt->state & State_NoChange) p->drawLine(point(0.28, 0.5), point(0.72, 0.5));
            else if (on) {
                QPainterPath tick; tick.moveTo(point(0.25, 0.52));
                tick.lineTo(point(0.43, 0.70)); tick.lineTo(point(0.76, 0.32));
                p->drawPath(tick);
            }
            p->restore();
            return;
        }
        // 选中那一块：调色板里那一档（`Highlight`）是**平台给的蓝**，而"出厂那一份不碰
        // 调色板"这条不能破（破了就把"跟着系统深浅"钉死了，见 `apply()` 那段）⇒ 由这里
        // 接过来自己画成"这一行自己的底色混一档主题色"（和侧栏导航同款，不画指示条）——
        // 理由在 `ui/Theme.hpp` 那段里。
        //
        // 两条原语各管一半，缺一条屏幕上就留一块蓝：
        // - `PE_PanelItemViewItem` = 条目那一格（每张表都有）；
        // - `PE_PanelItemViewRow` = **左边那一截分支区**（`QTreeView::drawRow()` 在把画笔
        // 交给代理之前自己铺的那一条，矩形是"整条行高 × 缩进宽度"）。
        // 代理侧还有一份同样的补涂（`VersionTreeDelegate::paint()` 里那段），因为这两条
        // 原语是不是真的被走到由**视图**决定 —— 补涂那一份是保底，两边同一个色。
        if (pe == PE_PanelItemViewItem || pe == PE_PanelItemViewRow) {
            if (const QStyleOptionViewItem *vopt = selectedItemOption(opt)) {
                // 实例树（选中带只有卡片那一圈）：分支区那一截不涂。7a 令牌里
                // 选中底是半透明的，先涂再让代理拿底色「擦」已经擦不掉了，只能从源头不画。
                const bool cardOnly = w != nullptr && w->property("wtfmlCardSelection").toBool();
                // 默认行代理自己画内缩圆角块，分支区那一截也不涂（否则块左边多一截方的）。
                const auto *view = qobject_cast<const QAbstractItemView *>(w);
                const bool rowBlock = view != nullptr && DesignDefaults::isRowDelegate(view->itemDelegate());
                if (pe == PE_PanelItemViewRow && (cardOnly || rowBlock))
                    return;
                p->fillRect(vopt->rect, selectionFill(vopt->palette.color(QPalette::Base)));
                return;
            }
        }

        // 键盘焦点环（全部关键操作具备键盘焦点、可见焦点环）。
        //
        // 基样式（Fusion）画的是**一圈虚线**，而我们的按钮挂了应用级样式表之后那圈虚线
        // 常常被自己的边盖掉/看不见 —— "按 Tab 键跳到哪儿了"就成了一个说不出口的状态。
        // 这里把它接过来画成**两像素的主题色圆圈**，颜色只问一处（`focusRingColor()`）：
        // 固定预设用自家 `tokens().focus`（那一支本来就是照着自家底色挑的）；只有
        // `system` 要同时活在两种底上，才按深浅分两支（浅底 `Ink`、深底 `Hover`）。
        //
        // **条目视图（列表 / 树 / 表格）不接管、而且是"一笔都不画"**：
        // 交回基样式的话，Fusion 就在"当前项"那一格外面套一圈**浅色细框**
        // （左边三页的左栏那一列上量到的是 1 px 的 `#BDD9F0` 圆角框）。列表这边"现在
        // 停在哪儿"本来就由我们那套选中底（`selectionFill()` 那块淡底）说清楚
        // 了，这一圈框是多余的第二层，而且它画在**整格**上、比里面那块内缩的圆角选中块
        // 大一圈，正是"框和实际按钮大小不同 ⇒ 难看"。所以直接返回，什么都不画。
        // `w == nullptr` 时交回去：没有控件就没有"谁拿着焦点"这回事（验证工程直接调进来时就是这样）。
        if (pe == PE_FrameFocusRect) {
            if (qobject_cast<const QAbstractButton *>(w) != nullptr &&
                !w->property("wtfmlKeyboardFocus").toBool()) return;
            if (qobject_cast<const QAbstractItemView *>(w) != nullptr) return;
            if (w == nullptr || (opt->state & State_HasFocus) == 0) {
                QProxyStyle::drawPrimitive(pe, opt, p, w);
                return;
            }
            const bool wholeWidgetButton =
                qobject_cast<const QPushButton *>(w) != nullptr
                || qobject_cast<const QToolButton *>(w) != nullptr;
            QColor ring = focusRingColor(opt->palette);
            // 文字透明度调低过的话（`textOpacity`）那一整圈也跟着淡一档 —— 它挂在
            // 调色板的墨色上就是那个意思，跟着走别自己算。
            const int inkAlpha = opt->palette.color(QPalette::WindowText).alpha();
            if (inkAlpha < 255) {
                ring.setAlpha(inkAlpha);
            }
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setBrush(Qt::NoBrush);
            QPen pen(ring);
            pen.setWidthF(2.0);
            p->setPen(pen);
            // 内缩 1 px：两像素的笔有一半落在外面，贴着边缘画会被裁掉一半（看上去就成一像素了）。
            // The inset focus ring shares the control's center of curvature.
            const QRectF ringRect = wholeWidgetButton
                ? QRectF(w->rect()).adjusted(1, 1, -1, -1)
                : QRectF(opt->rect).adjusted(1, 1, -1, -1);
            const int radius = qMax(0, (wholeWidgetButton ? buttonRadius() : controlRadius()) - 1);
            p->drawRoundedRect(ringRect, radius, radius);
            p->restore();
            return;
        }

        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }


    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p,
                     const QWidget *w) const override
    {
        // 选中那一行的**字**：底色那一块已经换成"这一行自己的底色混一档主题色"了（见上面
        // 那两条原语），而基样式紧接着印字用的是 `HighlightedText` —— 那一档还是"照蓝底
        // 配的"（深色系统主题下是白、浅色下是近黑）。这一块底上必须换一支**配套**的墨：
        // 浅色主题下平台给的近黑勉强能用，深色主题下那支白字压上去就没了（见
        // `selectionInk()` 里那两档怎么翻）。这套做法和整套理由见 `ui/Theme.hpp` 那段。
        //
        // 改的是**这一份拷贝**、不是控件身上那份：换色只对这一次绘制生效，别的地方读
        // `HighlightedText`（它们是照平台那一套算的）一点都不受影响。
        //
        // 只有 `CE_ItemViewItem` 走这一支。两个代理自己手画正文的那几棵树不经过这儿
        // （它们在 `paint()` 里读的是自己那份局部调色板，`VersionTreeDelegate` /
        // `InstanceGroupDelegate` 各有一处同样的换墨），实例行那种"把文字交给样式印"
        // 的走这儿 —— 三条路上屏幕看到的必须是同一支墨。
        if (ce == CE_ItemViewItem) {
            if (const QStyleOptionViewItem *vopt = selectedItemOption(opt)) {
                QStyleOptionViewItem patched(*vopt);
                // 换墨那一套只有一处出处（`selectionPalette()`），和三个手画正文的代理
                // 读的是同一个函数 —— 两条路上屏幕看到的必须是同一支墨。
                patched.palette = selectionPalette(patched.palette, /*selected=*/true);
                QProxyStyle::drawControl(ce, &patched, p, w);
                return;
            }
        }
        if (ce == CE_HeaderLabel) {
            if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(opt)) {
                QStyleOptionHeaderV2 patched;
                if (const auto *v2 = qstyleoption_cast<const QStyleOptionHeaderV2 *>(opt)) patched = *v2;
                else static_cast<QStyleOptionHeader &>(patched) = *header;
                DesignDefaults::alignHeaderLabel(&patched, w);
                QProxyStyle::drawControl(ce, &patched, p, w);
                return;
            }
        }
        QProxyStyle::drawControl(ce, opt, p, w);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 名字 / 键
// ---------------------------------------------------------------------------

QString partKey(Part part) {
    const int index = static_cast<int>(part);
    return index >= 0 && index < partCount() ? QLatin1String(kPartInfos[index].key) : QString();
}

QString partName(Part part) {
    const int index = static_cast<int>(part);
    return index >= 0 && index < partCount()
               ? QCoreApplication::translate("Theme", kPartInfos[index].name)
               : QString();
}

bool partFromKey(const QString &key, Part *out) {
    for (int i = 0; i < partCount(); ++i) {
        if (key == QLatin1String(kPartInfos[i].key)) {
            if (out != nullptr) {
                *out = static_cast<Part>(i);
            }
            return true;
        }
    }
    return false;
}

QString imageFitKey(ImageFit fit) {
    const int index = static_cast<int>(fit);
    return index >= 0 && index < imageFitCount() ? QLatin1String(kImageFitInfos[index].key)
                                                 : QString();
}

QString imageFitName(ImageFit fit) {
    const int index = static_cast<int>(fit);
    return index >= 0 && index < imageFitCount()
               ? QCoreApplication::translate("Theme", kImageFitInfos[index].name)
               : QString();
}

bool imageFitFromKey(const QString &key, ImageFit *out) {
    for (int i = 0; i < imageFitCount(); ++i) {
        if (key == QLatin1String(kImageFitInfos[i].key)) {
            if (out != nullptr) {
                *out = static_cast<ImageFit>(i);
            }
            return true;
        }
    }
    return false;
}

QPalette::ColorRole qtRole(Role role) {
    const int index = static_cast<int>(role);
    return index >= 0 && index < roleCount() ? kRoleInfos[index].role : QPalette::WindowText;
}

QString roleKey(Role role) {
    const int index = static_cast<int>(role);
    return index >= 0 && index < roleCount() ? QLatin1String(kRoleInfos[index].key) : QString();
}

QString roleName(Role role) {
    const int index = static_cast<int>(role);
    return index >= 0 && index < roleCount()
               ? QCoreApplication::translate("Theme", kRoleInfos[index].name)
               : QString();
}

bool roleFromKey(const QString &key, Role *out) {
    for (int i = 0; i < roleCount(); ++i) {
        if (key == QLatin1String(kRoleInfos[i].key)) {
            if (out != nullptr) {
                *out = static_cast<Role>(i);
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 主题色那一支
// ---------------------------------------------------------------------------

QColor defaultAccentColor() {
    // 冰川碧湖那一支主题色（`tokens("emerald").primary`）。出厂主题色现在跟预设走
    // （`defaultSpec()` = 暮色雪山，主题色是它自己的 primary）；这一支只剩挑色框里那行
    // 示例色在用（`InlineMessage::pickColor()`）。
    // 全仓只有这一处写这六个十六进制位 —— 顶栏、按钮的字与边、当前那一颗导航底下那条
    // 线，量的都是它算出来的某一档（见 `accentShade()`），**不许在别处再写一遍**。
    return QColor(QStringLiteral("#457F70"));
}

QColor rowHoverFill(const QColor &base) {
    if (current().presetId != QLatin1String("system") && current().accent == tokens().primary)
        return tokens().hover;
    // 认不出（盘上手改歪了）就退回当前预设那一支 —— 和 `accentShade()` 同一个兜底。
    QColor accent = current().accent;
    if (!accent.isValid()) {
        accent = tokens().primary;
    }
    // 混法**必须和 `mixWithWhite()` 逐字同式**（`(底*(100-p) + 主题*p + 50) / 100`）：
    // 浅色下 `base` 就是白，这一份和 `accentShade(AccentShade::Hover)` 因此**逐字节相等**
    // —— ui_check 钉着这条（"行悬停和按钮悬停是同一个色"就是靠它）。四舍五入那一位
    // （`+50`）不改的话两边会对不上，而那种差 1 的错在屏幕上完全看不出来。
    const int p = qBound(0, rowHoverAccentPercent(), 100);
    const auto mix = [p](int b, int a) { return (b * (100 - p) + a * p + 50) / 100; };
    return QColor(mix(base.red(), accent.red()), mix(base.green(), accent.green()),
                  mix(base.blue(), accent.blue()));
}

QColor neutralFill(const QColor &base, const QColor &ink, int percent) {
    // 「中性那一档」：底色往墨色那头混。**唯二的读者是日志窗那一排开关和它行间那条线**
    // （见 .hpp 里那段"开放深浅色之后写死的那几个灰站不住"）—— 浅色下偏差 ≤5，
    // 深色下自动变深灰。
    // 混法和 `mixWithWhite()` / `rowHoverFill()` **逐字同式**（那一位 `+50` 也照抄）：
    // 三个地方算出来的是同一族数，屏幕上才不会出现"两处差 1"的斑。
    // @param percent = 往墨那头走了几成：`0` 给 @p base 原样、`100` 给全墨。
    const int p = qBound(0, percent, 100);
    const auto mix = [p](int b, int i) { return (b * (100 - p) + i * p + 50) / 100; };
    return QColor(mix(base.red(), ink.red()), mix(base.green(), ink.green()),
                  mix(base.blue(), ink.blue()));
}

QColor selectionFill(const QColor &base) {
    if (current().presetId != QLatin1String("system") && current().accent == tokens().primary)
        return tokens().selected;
    // 混法**和 `rowHoverFill()` 逐字同式**（同一个 `mixToward()`），只差比例 —— 两边都是
    // "从这一行自己的底色出发往主题色那头混"。浅色下底就是白，所以这一档和
    // `accentShade(AccentShade::Hover)` 也只差那两个百分比。
    QColor accent = current().accent;
    if (!accent.isValid()) {
        accent = tokens().primary;
    }
    return mixToward(base, accent, selectionAccentPercent());
}

namespace {
// 选中那块底**在屏幕上**亮不亮：7a 令牌里它是半透明的，要先叠到页面底上再量明度
// （半透明的绿本身明度不到 128，直接量会把浅底误判成深底、配上亮墨）。
bool selectionLooksLight(const QColor &base) {
    const QColor top = selectionFill(base);
    if (top.alpha() == 255) return top.lightness() > 128;
    const QColor under = tokens().page;
    const qreal a = top.alphaF();
    const QColor shown(qRound(top.red() * a + under.red() * (1 - a)),
                       qRound(top.green() * a + under.green() * (1 - a)),
                       qRound(top.blue() * a + under.blue() * (1 - a)));
    return shown.lightness() > 128;
}
} // namespace

QColor selectionInk(const QColor &base) {
    // 墨**跟着那一块底的明暗翻**（规矩：底改浅了墨必须跟着翻过去 ——
    // 反方向上"亮墨压浅底 = 等于没字"照样成立）。浮上来的一直是**主题色那一支**里
    // 对应的那一档，不是黑/白两档写死的灰。
    //
    // 浅底（默认那身浅色主题：20% 主题色罩在白上）：`AccentShade::Ink` 压上去约 7.4:1。
    // **不能**用 `AccentShade::Pure` —— 本色压在自己 20% 的浅底上只有 2.4:1，那行字就糊了。
    // 深底（深色预设：约 `#31433a`）：`Ink` 那一档压上去只剩 1.2:1（等于没字）⇒ 换成
    // 那一支里"深底上写字"的那一档（主题色罩 25% 白 —— 罩白比例**越小越亮**），约 7.5:1。
    if (selectionLooksLight(base)) {
        return accentShade(AccentShade::Ink);
    }
    return mixWithWhite(accentShade(AccentShade::Pure), 25);
}

QColor selectionSubInk(const QColor &base) {
    // 次要那一档（版本树第二行那行小字、占位文字、行里那些小图标）：浅底上约 5:1、
    // 深底上约 5.7:1 —— 比正文那一档低一档（罩白的比例大一档，于是更靠主题色、
    // 也就更暗），但一样读得清。
    if (selectionLooksLight(base)) {
        return QColor(0x55, 0x55, 0x55);
    }
    return mixWithWhite(accentShade(AccentShade::Pure), 45);
}

QPalette selectionPalette(const QPalette &palette, bool selected) {
    QPalette out = palette;
    if (!selected) {
        return out;
    }
    // 这一行**自己的底色**（`QAbstractItemView` 构造里给视口设的 backgroundRole 就是它）：
    // 底和墨都从它推 —— 深色预设下底是 `#323232`，不从这个出发就会闪成一片近白。
    const QColor base = palette.color(QPalette::Base);
    // 换上去的墨沿用**原来那一档的 alpha**（用户把"文字透明度"调低过的话，选中那一行的字
    // 也该一样淡 —— `kInkRoles` 里就有这几位，那个 alpha 正是这个意思）。
    const auto inkOn = [](const QColor &want, const QColor &was) {
        QColor ink = want;
        if (was.alpha() < 255) {
            ink.setAlpha(was.alpha());
        }
        return ink;
    };
    out.setColor(QPalette::WindowText,
                 inkOn(selectionInk(base), palette.color(QPalette::WindowText)));
    out.setColor(QPalette::Text, inkOn(selectionInk(base), palette.color(QPalette::Text)));
    out.setColor(QPalette::HighlightedText,
                 inkOn(selectionInk(base), palette.color(QPalette::HighlightedText)));
    out.setColor(QPalette::PlaceholderText,
                 inkOn(selectionSubInk(base), palette.color(QPalette::PlaceholderText)));
    // 这一行真正铺的那一层是那块浅主题色底（不是调色板里那一档蓝）—— 有几处按"底色"重算
    // 文字对比度（版本树上那两行 MOTD），读的就是 `Highlight`。
    out.setColor(QPalette::Highlight, selectionFill(base));
    return out;
}

QColor accentShade(AccentShade shade) {
    // 当前那一份里的主题色；认不出（盘上手改歪了）就退回当前预设那一支。
    QColor pure = current().accent;
    if (!pure.isValid()) {
        pure = tokens().primary;
    }
    switch (shade) {
    case AccentShade::Pure:
        return pure;
    case AccentShade::Ink:
        return mixWithBlack(pure, 60);
    case AccentShade::Border:
        // 那一圈边：**主题色的 80%**（出厂色下 `#316653`，对白底 6.7:1）。
        // 比字浅一档（"边只要看得见、字要读得清"），但对白底仍远超界面部件那条 3:1。
        // 跟着 `Ink` 一起抬：纯色 / 边 / 字是 128 / 102 / 77，两两差着 20 上下，
        // 屏幕上真能分出三层；边夹在"抬上来的字"和纯色之间，不会挤成一坨。
        return mixWithBlack(pure, 80);
    case AccentShade::Hover:
    case AccentShade::Pressed:
        // 悬停 / 按下那两层底：**主题色罩在白上**（12% / 28%），不是把主题色调亮或调暗 ——
        // 调亮方向会撞到"白底"、调暗方向就成了另一种颜色，而这两层底上写的还是那一档深绿
        // （`Ink`），底色必须一直待在很浅那一头，那行字才一直读得清。
        return mixWithWhite(pure, shade == AccentShade::Hover ? 12 : 28);
    default:
        break;
    }
    return pure;
}

// ---------------------------------------------------------------------------
// 按钮那一套外观
// ---------------------------------------------------------------------------

QColor neutralInkColor() {
    // 「一般性」那一档的黑。**唯一出处**：次要按钮（Neutral）的字和边、未勾选的勾选框
    // 那个框和那行字，量的都是它。不用纯黑（`#000000`）：纯黑压在纯白上是 21:1，
    // 比正文（`#000000`）还重 —— 一颗"取消"不该比它旁边那颗"确定"更抢眼。
    return tokens().text;
}

// 「灰掉」那一档的三个色（详见头文件里那段）。★ 三处**必须一起看**：底浅一档、字浅两档、
// 边浅浅地留一道 —— 单看哪一个都只是"一个灰色"，合起来才是"这一格现在按不动，但形状还在"。
QColor disabledInkColor() {
    // 单独设计 Disabled 状态：`#8a8a8a` 对
    // `disabledFillColor()` 是 3.2:1 —— 一页里字号最小的那几颗（12~13px）灰掉之后
    // 不至于"看不清"而是"读得清"。既还是明显灰的（对白底 3.4:1、对次要用那支纯黑
    // 15:1），又读得出来写的是什么。
    // 判据不是"我觉得好看"：ui_check 里那条**现算对比度**，下限就是 3:1。
    return tokens().disabled;
}

QColor disabledBorderColor() {
    // 轮廓那一圈：比字再浅一档。抹掉它的话，灰掉的一格在白底上只剩几个灰字 ——
    // "这儿本来有一颗按钮"这件事就看不出来了（而那正是禁用态要传达的一半信息）。
    return tokens().divider;
}

QColor disabledFillColor() {
    // 底：贴白但明显不是白（对着 `#ffffff` 的页面底一眼能看出这一格是"塌"下去的）。
    return tokens().subtle;
}

QColor focusRingColor(const QPalette &palette) {
    if (current().presetId != QLatin1String("system")) return tokens().focus;
    // 深浅两档就是主题色那一支里的两头（浅底用"白底上写字"的那一档、深底用"往白里混 12%"
    // 的那一档）。**和别处"深色下用亮的那一档"是同一个判断**（`isDarkPalette()`），
    // 别在这儿另立一条"看主题名"的规矩。
    return isDarkPalette(palette) ? accentShade(AccentShade::Hover)
                                  : accentShade(AccentShade::Ink);
}

bool itemViewKeyboardFocus(const QWidget *view) {
    return view != nullptr && view->hasFocus() && view->property("wtfmlKeyboardFocus").toBool();
}

bool isKeyboardCurrentRow(const QStyleOptionViewItem &option, const QModelIndex &index) {
    const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
    if (view == nullptr || !index.isValid() || !itemViewKeyboardFocus(view)) return false;
    const QModelIndex current = view->currentIndex();
    return current.isValid() && current.siblingAtColumn(0) == index.siblingAtColumn(0);
}

QColor progressTrackColor(const QPalette &palette) {
    Q_UNUSED(palette);
    // 设计稿 .bar：底 line-2、前景 accent-fill（三个主题都一样取令牌）。
    return tokens().stroke;
}

QColor progressFillColor(const QPalette &palette) {
    Q_UNUSED(palette);
    return tokens().primary;
}

QString buttonKindKey(ButtonKind kind) {
    switch (kind) {
    case ButtonKind::Primary:
        return QStringLiteral("primary");
    case ButtonKind::Neutral:
        return QStringLiteral("neutral");
    case ButtonKind::Ghost:
        return QStringLiteral("ghost");
    case ButtonKind::Danger:
        return QStringLiteral("danger");
    case ButtonKind::DangerSolid:
        return QStringLiteral("dangerSolid");
    case ButtonKind::Accent:
        break;
    }
    // 普通按钮**不挂属性**（挂一个 "accent" 只是让构造出来的控件都带一位恒真的值）。
    return QString();
}

// ---------------------------------------------------------------------------
// 「面」与「文字角色」那两个属性
// ---------------------------------------------------------------------------

// 这一组就是上面那几个 `constexpr char` 的出口（**唯一出处**：产品、样式表、验证工程
// 三方都读它，理由见头文件里那段）。写成函数而不是把常量放进头文件，是为了它们能和
// 别的 `QString` 访问器一样用在 `setProperty()` / `findChild()` 那一族调用里。
QString surfacePropertyName() { return QString::fromLatin1(kSurfaceProperty); }
QString sidebarSurfaceValue() { return QString::fromLatin1(kSurfaceSidebar); }
QString workspaceSurfaceValue() { return QString::fromLatin1(kSurfaceWorkspace); }
QString sectionSurfaceValue() { return QString::fromLatin1(kSurfaceSection); }
QString footerSurfaceValue() { return QString::fromLatin1(kSurfaceFooter); }
QString textRolePropertyName() { return QString::fromLatin1(kTextRoleProperty); }
QString pageTitleTextRoleValue() { return QString::fromLatin1(kTextRolePageTitle); }
QString sectionTitleTextRoleValue() { return QString::fromLatin1(kTextRoleSectionTitle); }
QString secondaryTextRoleValue() { return QString::fromLatin1(kTextRoleSecondary); }
QString displayPropertyName() { return QStringLiteral("wtfmlDisplay"); }

QString refreshActionPropertyName() { return QStringLiteral("wtfmlRefreshAction"); }

void markRefreshButton(QWidget *button) {
    if (button != nullptr) {
        button->setProperty(refreshActionPropertyName().toUtf8().constData(), true);
    }
}

void markDisplay(QWidget *widget, const QFont &font) {
    if (widget == nullptr) return;
    widget->setFont(font);
    widget->setProperty("wtfmlDisplay", true);
}

// ---------------------------------------------------------------------------
// 滚动条
// ---------------------------------------------------------------------------

namespace {

/// 把手那一支中性灰的**实色**那一半（alpha 由下面三个函数各自加）。
///
/// 底色浅 ⇒ 一支深灰；底色深 ⇒ 一支浅灰。**同一支灰**在两种底上都不是"看不见"就是
/// "一块黑疙瘩"，所以这一位必须跟着深浅走。
QColor scrollHandleBase() {
    return isDark() ? QColor(0xff, 0xff, 0xff) : QColor(0x0f, 0x0f, 0x0f);
}

/// 同一支灰的三档浓淡（0~255 的 alpha）：平时 / 悬停 / 拖。
QColor scrollHandleAt(int alpha) {
    QColor color = scrollHandleBase();
    color.setAlpha(alpha);
    return color;
}

/// QSS 里那个 `rgba(...)`。**不写 `#AARRGGBB`**：Qt 样式表认的是 `#RRGGBB` 和 `rgba()`
/// 两种，八位的十六进制进到那边是"认不出就忽略"，静默退成没有这一条。
QString rgbaCss(const QColor &color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

}  // namespace

QColor scrollHandleColor() {
    if (current().presetId != QLatin1String("system")) return tokens().thumb;
    // 110/255 ≈ 43%：压在 `#fcfcfc` 上量出来是一支 `#919191` 上下的灰 —— 一眼看得见，
    // 又不会把旁边那行字比下去。深底上那支浅灰同理。
    return scrollHandleAt(110);
}

QColor scrollHandleHoverColor() {
    return scrollHandleAt(165);
}

QColor scrollHandlePressedColor() {
    return scrollHandleAt(205);
}

namespace {

/// 滚动条那两段规则的**唯一模板**（竖的 + 横的，同一串参数）。
///
/// 应用级样式表那一份用 `palette(...)` 当占位（换配色不用重挂整份表），
/// 每一根真滚动条身上那一份用 `rgba(...)` 精确色（`scrollHandleColor()` 那三档带 alpha）。
/// 两份用同一个模板，免得"竖的改了、横的没改"那种漂。
QString scrollBarRules(const QString &handle, const QString &handleHover,
                       const QString &handlePressed) {
    return QStringLiteral(
               "QScrollBar:vertical { background: transparent; width: %1px; margin: 0px; }"
               "QScrollBar::handle:vertical { background: %2; min-height: %3px;"
               " border-radius: %4px; margin: %5px; }"
               "QScrollBar::handle:vertical:hover { background: %6; }"
               "QScrollBar::handle:vertical:pressed { background: %7; }"
               "QScrollBar::handle:vertical[wtfmlIdle=\"true\"] { background: transparent; }"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
               "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
               " background: transparent; }"
               "QScrollBar:horizontal { background: transparent; height: %1px; margin: 0px; }"
               "QScrollBar::handle:horizontal { background: %2; min-width: %3px;"
               " border-radius: %4px; margin: %5px; }"
               "QScrollBar::handle:horizontal:hover { background: %6; }"
               "QScrollBar::handle:horizontal:pressed { background: %7; }"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
               "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
               " background: transparent; }")
        .arg(QString::number(scrollBarThickness()), handle,
             QString::number(scrollBarMinHandleLength()),
             QString::number(scrollBarHandleRadius()),
             QString::number(scrollBarHandleMargin()), handleHover, handlePressed);
}

/// 每一根滚动条自己那一份"主题代数"（见 `styleScrollBar()`）。
int &scrollBarThemeGeneration() {
    static int generation = 0;
    return generation;
}

constexpr const char kScrollBarThemeProperty[] = "wtfmlScrollTheme";

/// 把**当前**那一支滚动条样式挂到 @p bar 身上（控件级样式表，压过应用级那一份）。
///
/// 为什么滚动条要单独走控件级：它那三档是**带 alpha 的中性灰**（`scrollHandleColor()`），
/// 而 `palette(...)` 只能给出一个实色角色；把它塞进调色板又会占掉别的语义档。
/// 滚动条的数量是两位数，换主题时挨个重挂一遍不贵（按钮 / 输入框那几千个走的是
/// 调色板 + 一次 `repolishWidgets()`）。
///
/// 新生出来的滚动条（换页之后冒出来那一根）由 `ScrollBarStyler` 在 `Show` 上补一次。
void styleScrollBar(QScrollBar *bar) {
    if (bar == nullptr) {
        return;
    }
    const int generation = scrollBarThemeGeneration();
    const QVariant stamp = bar->property(kScrollBarThemeProperty);
    if (stamp.isValid() && stamp.toInt() == generation) {
        return;
    }
    bar->setProperty(kScrollBarThemeProperty, generation);
    bar->setStyleSheet(scrollBarRules(rgbaCss(scrollHandleColor()),
                                      rgbaCss(scrollHandleHoverColor()),
                                      rgbaCss(scrollHandlePressedColor())));
}

/// 新滚动条一露头就补上当前那一份样式（应用级那份只是个 `palette(...)` 的近似值）。
class ScrollBarStyler : public QObject {
public:
    explicit ScrollBarStyler(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlScrollBarStyler"));
        app.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            if (auto *bar = qobject_cast<QScrollBar *>(watched)) {
                styleScrollBar(bar);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

}  // namespace

/// 挂过 `markButton()` 的按钮身上那一位"控件级配色已经登记过"的印记。
constexpr const char kButtonKindStyledProperty[] = "wtfmlKindStyled";
constexpr const char kButtonKindFollowProperty[] = "wtfmlKindFollow";

/// 主动作 / 危险这两档的**悬停与按下**用的是比底色再深一档的同色，而调色板里
/// 只备得起"本色 / 亮一档 / 深一档"那几个通用档位。这两档的按钮一共几十颗，
/// 所以走**控件级样式表**：只影响它们自己，不动整份应用样式表（那才是卡顿的来源）。
QColor primaryHoverFill() {
    const QColor pure = accentShade(AccentShade::Pure);
    if (pure == tokens().primary) return tokens().primaryHover;  // 预设主题色：设计稿给的那一档
    return isDark() ? pure.lighter(108) : pure.darker(108);
}

QColor primaryPressedFill() {
    const QColor pure = accentShade(AccentShade::Pure);
    if (pure == tokens().primary) return tokens().primaryPressed;
    return isDark() ? pure.darker(108) : pure.darker(118);
}

QString kindButtonStyleSheet(const QString &kind) {
    // 设计稿：主按钮实心强调色 + on-accent 字；危险默认柔和淡红（悬停 α.22 / 按下 α.28）；
    // 实心暗哑红只给对话框里不可逆确认。按下一律固定色，不靠亮度滤镜。
    const Tokens t = tokens();
    const QString block = QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border-color: transparent; }"
        "QPushButton:hover:enabled { background-color: %3; }"
        "QPushButton:pressed:enabled { background-color: %4; }"
        "QPushButton[wtfmlKeyboardFocus=\"true\"]:focus { border-color: %5; }"
        "QPushButton:disabled { background-color: %6; color: %7; border-color: transparent; }");
    const auto css = [](const QColor &c) { return c.name(QColor::HexArgb); };
    const auto alpha = [](QColor c, qreal a) { c.setAlphaF(a); return c; };
    if (kind == QLatin1String("primary")) {
        return block.arg(css(accentShade(AccentShade::Pure)), css(t.onPrimary), css(primaryHoverFill()),
                         css(primaryPressedFill()), css(t.focus), css(t.subtle), css(t.disabled));
    }
    if (kind == QLatin1String("danger"))
        return block.arg(css(t.dangerSoft), css(textTone(t.dangerInk, t.danger)), css(alpha(t.danger, .22)), css(alpha(t.danger, .28)),
                         css(t.danger), css(t.hover), css(t.disabled));
    if (kind == QLatin1String("dangerSolid"))
        return block.arg(css(t.dangerSolid), css(t.onDanger), css(t.dangerSolidHover), css(t.dangerSolidPressed),
                         css(t.focus), css(t.subtle), css(t.disabled));
    return QString();
}

void syncKindButtonStyle(QWidget *button) {
    if (button == nullptr) {
        return;
    }
    const QString kind = button->property(kButtonKindProperty).toString();
    QString sheet = kindButtonStyleSheet(kind);
    if (button->property("wtfmlModAction").toBool()) {
        const Tokens t = tokens();
        QColor border = t.secondary;
        border.setAlphaF(.38);
        sheet += QStringLiteral("QPushButton:disabled { background-color: transparent; color: %1; border: 1px dashed %2; }")
                     .arg(t.disabled.name(QColor::HexArgb), border.name(QColor::HexArgb));
    }
    if (sheet.isEmpty()) {
        if (button->property(kButtonKindStyledProperty).toBool()) {
            button->setProperty(kButtonKindStyledProperty, false);
            button->setStyleSheet(QString());
        }
        return;
    }
    button->setProperty(kButtonKindStyledProperty, true);
    button->setStyleSheet(sheet);
}

void markButton(QWidget *button, ButtonKind kind) {
    if (button == nullptr) {
        return;
    }
    // Text actions need a visible surface; icon-only and navigation buttons
    // retain their lighter presentation. Keep the semantic kind unchanged.
    bool textAction = false;
    if (auto *push = qobject_cast<QPushButton *>(button)) {
        for (const QChar character : push->text()) {
            if (character.isLetterOrNumber()) { textAction = true; break; }
        }
    }
    button->setProperty("wtfmlTextAction", textAction);
    const QString key = buttonKindKey(kind);
    if (key.isEmpty()) {
        button->setProperty(kButtonKindProperty, QVariant());
    } else {
        button->setProperty(kButtonKindProperty, key);
    }
    syncKindButtonStyle(button);
    // 主动作 / 危险这两档挂过一次 `follow()` 就够了：它读按钮自己身上那个属性，
    // 以后换主题 / 换档都在同一条回调里重算（不受这一趟是哪一档影响）。
    if ((kind == ButtonKind::Primary || kind == ButtonKind::Danger || kind == ButtonKind::DangerSolid ||
         button->property("wtfmlModAction").toBool()) &&
        !button->property(kButtonKindFollowProperty).toBool()) {
        button->setProperty(kButtonKindFollowProperty, true);
        follow(button, [button]() { syncKindButtonStyle(button); });
    }
}

QString iconButtonSizeValue() {
    return QString::fromLatin1(kIconButtonValue);
}

namespace {
const char kPinnedIconKeeperName[] = "wtfmlPinnedIconSizeKeeper";

/**
 * @brief 图标档按钮"钉死的边长"的补钉：`setFixedSize()` 钉好的最小高度会被应用级 QSS
 *        的 `min-height`（polish 那一趟，content-box ⇒ 还要加 padding/border）改回 QSS
 *        那一档，而 polish **不发** StyleChange，`follow()` 收不到 ⇒ 下一次布局就把
 *        64x24 摆成 64x19（min 14 / max 24，ui_check W14 偶发就是这样）。
 *
 *        补钉分两步：事件上先钉一次（这一帧就摆正），再排一发 0 毫秒的定时器 ——
 *        QSS 重铺是**同一趟里随后**发生的（Show 之后还会来一发 PolishRequest /
 *        PaletteChange），当场钉好的那一下会被它再打回去。实测只钉事件不收尾的话
 *        `processEvents()` 回来又变回 14。
 *
 *        没钉过（最大值仍是 QWIDGETSIZE_MAX）的按钮一概不动。
 */
class PinnedIconKeeper : public QObject {
public:
    explicit PinnedIconKeeper(QWidget *button) : QObject(button), m_button(button) {
        setObjectName(QString::fromLatin1(kPinnedIconKeeperName));
        button->installEventFilter(this);
        m_rearm.setInterval(0);
        m_rearm.setSingleShot(true);
        connect(&m_rearm, &QTimer::timeout, this, [this]() { applyPinnedSize(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        Q_UNUSED(watched);
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::StyleChange:
        case QEvent::Polish:
        case QEvent::PolishRequest:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
            applyPinnedSize();
            m_rearm.start();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void applyPinnedSize() {
        const QSize pinned = m_button->maximumSize();
        if (pinned.width() < QWIDGETSIZE_MAX && pinned.height() < QWIDGETSIZE_MAX) {
            m_button->setFixedSize(pinned);
        }
    }

    QWidget *m_button;
    QTimer m_rearm;
};
} // namespace

void markIconButton(QWidget *button) {
    if (button == nullptr) {
        return;
    }
    button->setProperty(kButtonSizeProperty, QString::fromLatin1(kIconButtonValue));
    if (button->findChild<QObject *>(QString::fromLatin1(kPinnedIconKeeperName),
                                     Qt::FindDirectChildrenOnly) == nullptr) {
        new PinnedIconKeeper(button);
    }
}

QString chipStyleSheet(const QPalette &palette) {
    // 胶囊：不描边（「去硬描边」），靠次按钮那档淡底立住；深色下那档太淡（白 7%），
    // 提到 11% 形状才清楚。悬停次按钮悬停底 + ink；按下 fill-press；选中 = 强调色淡底 + 强调字。
    // 四边留 1px 透明边，高度不变。
    const auto c = [&palette](QPalette::ColorRole role) { return palette.color(role).name(QColor::HexArgb); };
    QColor fill = palette.color(QPalette::Button);
    if (isDarkPalette(palette)) fill.setAlphaF(qMax(fill.alphaF(), 0.11));
    QColor hover = palette.color(QPalette::Shadow);
    if (isDarkPalette(palette)) hover.setAlphaF(qMax(hover.alphaF(), 0.16));
    // 高度写进 QSS：图标按钮那条 `min-height: 0` 复位会把 C++ 的 setFixedHeight 下限冲掉。
    // 内容盒 = chipHeight − 上下 1px 边 − 上下 2px padding。
    return QStringLiteral("QPushButton { border: 1px solid transparent; border-radius: %1px; padding: 2px 14px;"
                          " background-color: %2; color: %3; min-height: %9px; max-height: %9px; }"
                          "QPushButton:hover:!checked { background-color: %4; color: %5; }"
                          "QPushButton:pressed:!checked { background-color: %6; color: %5; }"
                          "QPushButton:checked { background-color: %7; color: %8; }"
                          "QPushButton:disabled { color: %10; background: transparent; }")
        .arg(chipHeight() / 2)
        .arg(fill.name(QColor::HexArgb), c(QPalette::PlaceholderText), hover.name(QColor::HexArgb),
             c(QPalette::WindowText), c(QPalette::Midlight), c(QPalette::Highlight), c(QPalette::Link))
        .arg(chipHeight() - 6)
        .arg(tokens().disabled.name(QColor::HexArgb));
}

void markBadge(QLabel *label, BadgeTone tone) {
    if (label == nullptr) return;
    label->setFixedHeight(22);
    label->setAlignment(Qt::AlignCenter);
    QFont f = label->font();
    f.setPixelSize(textSize(TextRole::Caption));
    label->setFont(f);
    const auto apply = [label, tone]() {
        const Tokens t = tokens();
        QColor bg = t.subtle, ink = t.secondary;
        switch (tone) {
        case BadgeTone::Neutral: break;
        case BadgeTone::Accent: bg = t.selected; ink = textTone(t.accentInk, t.deep); break;
        case BadgeTone::Ok: bg = t.okSoft; ink = textTone(t.okInk, t.ok); break;
        case BadgeTone::Warn: bg = t.warnSoft; ink = textTone(t.warnInk, t.warn); break;
        case BadgeTone::Danger: bg = t.dangerSoft; ink = textTone(t.dangerInk, t.danger); break;
        }
        label->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: %2; border-radius: 11px; padding: 0 9px; }")
                                 .arg(bg.name(QColor::HexArgb), ink.name(QColor::HexArgb)));
    };
    apply();
    follow(label, apply);
}

namespace {
class SwitchPainter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Paint) return false;
        auto *box = qobject_cast<QCheckBox *>(watched);
        if (box == nullptr) return false;
        const Tokens t = tokens();
        QPainter p(box);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF track(0.5, 0.5, 39, 21);
        const bool on = box->isChecked();
        if (!box->isEnabled()) p.setOpacity(0.45);
        p.setPen(Qt::NoPen);  // 关着也不描边：fill-press 底 + 圆点就认得出轨道
        p.setBrush(on ? t.primary : t.fillPress);
        p.drawRoundedRect(track, 10.5, 10.5);
        const qreal d = 16;
        const QRectF knob(on ? track.right() - 3 - d : track.left() + 3, track.center().y() - d / 2, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(on ? t.onPrimary : t.secondary);
        p.drawEllipse(knob);
        if (box->hasFocus() && box->property("wtfmlKeyboardFocus").toBool()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(t.focus, 2));
            p.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        }
        return true;
    }
};
}  // namespace

void markSwitch(QCheckBox *box) {
    if (box == nullptr || box->property("wtfmlSwitch").toBool()) return;
    static SwitchPainter *painter = new SwitchPainter(qApp);
    box->setProperty("wtfmlSwitch", true);
    if (box->accessibleName().isEmpty()) box->setAccessibleName(box->text());
    box->setFixedSize(40, 22);
    box->setCursor(Qt::PointingHandCursor);
    box->installEventFilter(painter);
}

void markSegmented(QWidget *container, const QList<QPushButton *> &segments) {
    if (container == nullptr) return;
    container->setAttribute(Qt::WA_StyledBackground, true);
    container->setProperty("wtfmlSegmented", true);
    auto *group = new QButtonGroup(container);
    group->setExclusive(true);
    QFont f = container->font();
    f.setPixelSize(textSize(TextRole::Body));
    f.setWeight(QFont::Medium);
    for (QPushButton *segment : segments) {
        segment->setProperty("wtfmlSegment", true);
        segment->setCheckable(true);
        segment->setFont(f);
        segment->setCursor(Qt::PointingHandCursor);
        group->addButton(segment);
    }
}

void markChipButton(QPushButton *button) {
    if (button == nullptr) {
        return;
    }
    button->setFixedHeight(chipHeight());
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    markIconButton(button);
    button->setStyleSheet(chipStyleSheet(QApplication::palette()));
    // 取应用 palette 而不是 button->palette()：换主题时 follow 回调早于调色板下发到子控件，读按钮自己的会拿到旧主题的字色。
    // QSS 里嵌的是色名 ⇒ 主题一换得重算。chip 是**常驻**控件（不像资源详情页那一排是整排
    // 重建的），没人会替它重挂，所以这一条必须自己接。
    follow(button, [button]() { button->setStyleSheet(chipStyleSheet(QApplication::palette())); });
}

QColor swatchBorder(const QColor &color) {
    // 深色块给白边、浅色块给黑边。两边都带一点 alpha：这一圈是"给它一个形状"，不是内容，
    // 全实的话浅色页面上会是一圈扎眼的黑。
    //
    // NOTE: 判据取的是**这块色自己的明度**（不是当前主题的深浅）：一块近黑的主题色搁在浅色
    // 页面上，跟着主题走照样是"一个没有形状的洞"。
    return color.lightness() < 128 ? QColor(0xff, 0xff, 0xff, 0xa0)
                                   : QColor(0x00, 0x00, 0x00, 0xa0);
}

/// 现在这一块是哪个色（存在按钮自己的动态属性上）。
///
/// 为什么要存一份：`follow()` 那条重算**不能捕获调用方当时给的那个 `color`** —— 用户换过
/// 一个色之后，同一个按钮身上那条老登记还在，主题一变它会把**旧色**贴回去。存一份、
/// 重算时读回来，改色这条路上就只有一个出处。
const char *const kSwatchColorProperty = "wtfmlSwatchColor";
const char *const kSwatchFollowProperty = "wtfmlSwatchFollow";

QString swatchStyleSheet(const QColor &color) {
    const QColor edge = swatchBorder(color);
    // 悬停 / 按下**只动边框**：底色就是它要表达的那件事，跟着鼠标换掉等于这块色块什么都不是。
    // - 悬停 → 边框换成主题色本色（"这一块能点"）；
    // - 按下 → 边框换成主题色更深的那一档（这一下真的按下去了）。
    // 这几条 `:hover` / `:pressed` 是**必须写的**：应用级那份样式表里有同名的伪类规则，
    // 不写的话鼠标一划过去，块里的色会被换成"按钮悬停"那一层淡主题色。
    return QStringLiteral("QPushButton { background-color: %1; border: 1px solid %2;"
                          " border-radius: %3px; padding: 0px; }"
                          "QPushButton:hover { background-color: %1; border-color: %4; }"
                          "QPushButton:pressed { background-color: %1; border-color: %5; }")
        .arg(color.name(QColor::HexArgb), edge.name(QColor::HexArgb))
        .arg(controlRadius())
        .arg(accentShade(AccentShade::Pure).name(QColor::HexArgb),
             accentShade(AccentShade::Ink).name(QColor::HexArgb));
}

void markColorSwatch(QPushButton *button, const QColor &color) {
    if (button == nullptr) {
        return;
    }
    markIconButton(button);
    const QSize pinned = button->minimumSize() == button->maximumSize() ? button->minimumSize()
                                                                       : QSize();
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty(kSwatchColorProperty, color);
    button->setStyleSheet(swatchStyleSheet(color));
    if (pinned.isValid() && pinned.width() < QWIDGETSIZE_MAX && pinned.height() < QWIDGETSIZE_MAX) {
        button->setFixedSize(pinned);
    }
    if (!button->property(kSwatchFollowProperty).toBool()) {
        button->setProperty(kSwatchFollowProperty, true);
        // QSS 里嵌的是色名，而**悬停那一圈跟着主题色走** ⇒ 主题一换得重算一遍（块里那个色
        // 不跟着动 —— 它正是用户选的那个）。
        follow(button, [button]() {
            button->setStyleSheet(
                swatchStyleSheet(button->property(kSwatchColorProperty).value<QColor>()));
            // 样式表重挂那一趟同样会把最小高度打回去 —— 这里也得跟着再钉一次（不然
            // "换了主题之后色块矮了一截"会在下一次布局时冒出来）。
            const QSize again = button->maximumSize();
            if (again.width() < QWIDGETSIZE_MAX && again.height() < QWIDGETSIZE_MAX) {
                button->setFixedSize(again);
            }
        });
    }
}

// ---------------------------------------------------------------------------
// 两个控件工厂
// ---------------------------------------------------------------------------

QPushButton *makeFoldRow(QWidget *parent, const QString &title) {
    auto *button = new QPushButton(title, parent);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // 属性选择器把这一颗从通用 QPushButton 那几档里摘出来（那几档的圆角淡底 / 勾上铺色不合折叠行）。
    button->setProperty("wtfmlFoldRow", true);
    // 标题仍走按钮自己的 `text()`（QSS 里 `text-align: left` 靠左画；无障碍名、验证工程照旧读它）；
    // 右侧那两个字是一颗子 `QLabel` —— 整行任何位置按下都是按这颗按钮。
    auto *state = new QLabel(button);
    state->setAttribute(Qt::WA_TransparentForMouseEvents);
    state->setForegroundRole(QPalette::PlaceholderText);
    auto *row = new QHBoxLayout(button);
    row->setContentsMargins(0, 0, 14, 0);
    row->addStretch(1);
    row->addWidget(state);
    const auto sync = [state](bool open) { state->setText(open ? QObject::tr("收起") : QObject::tr("展开")); };
    sync(false);
    QObject::connect(button, &QPushButton::toggled, button, sync);
    return button;
}

QWidget *makeNumberStepper(QWidget *parent, QAbstractSpinBox *spin, int spinWidth) {
    auto *field = new QWidget(parent);
    auto *row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    spin->setParent(field);
    spin->setAlignment(Qt::AlignCenter);
    // 没有上下钮之后 sizeHint 不再留那一格；显式 NoButtons 让宽度也按这个来算。
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    if (spinWidth > 0) {
        spin->setFixedWidth(spinWidth);
    }
    const auto step = [field](const QString &text, const QString &tip) {
        auto *button = new QPushButton(text, field);
        button->setFocusPolicy(Qt::NoFocus);  // Tab 只停在数字框上，−/+ 是它的附件
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFixedWidth(30);
        markButton(button, ButtonKind::Ghost);
        QFont font = button->font();
        font.setPixelSize(16);
        button->setFont(font);
        return button;
    };
    auto *minus = step(QStringLiteral("−"), QObject::tr("减一"));
    auto *plus = step(QStringLiteral("+"), QObject::tr("加一"));
    row->addWidget(minus);
    row->addWidget(spin);
    row->addWidget(plus);
    QObject::connect(minus, &QPushButton::clicked, spin, &QAbstractSpinBox::stepDown);
    QObject::connect(plus, &QPushButton::clicked, spin, &QAbstractSpinBox::stepUp);
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return field;
}

QString paletteRef(const char *role) {
    return QStringLiteral("palette(%1)").arg(QLatin1String(role));
}

QString widgetStyleSheet() {
    // 这一份样式表里的**颜色**一律写成 `palette(角色)`，不再把十六进制
    // 色名嵌进字符串。QSS 那边的 `palette(...)` 是"按控件自己的调色板现取"，于是
    // 换配色只需要换调色板，样式表字符串**一个字节都不变** —— `installButtonSheet()`
    // 那一层"没变就不重挂"才真正成立。
    //
    // 写成 `.arg(t.text.name())` 那一族的话，每换一次配色都会让整份样式表变成另一串，
    // `QApplication::setStyleSheet()` 于是把每一个控件重新 polish 一遍；上千个控件就要
    // 1.9 秒，界面上就是"改完配色卡住一会"。走 `palette(...)` 的代价只有一次
    // `repolishWidgets()`（同量级 ~100 ms）。
    //
    // 这些角色在 `buildPalette()` 里**全部**备好（包括 `Accent` / `Light` / `LinkVisited`
    // 这些平时没人用的档位）。别在这儿写死色名 —— 写死一个，就会回到"整份重挂"那条路。
    const QString ink = paletteRef("button-text");
    const QString border = paletteRef("link");
    const QString pure = paletteRef("accent");
    // 主题色写出来的那一档字（勾上那一格）。取 `link`：各预设都把它备成
    // 「压在自家底色上读得清的主题色」—— 浅色预设是深绿（对白底 5.3:1），深色预设是
    // 浅蓝。`accent`（纯色）在深色预设的深底上会糊，所以不能拿它写字。
    const QString accentInk = paletteRef("link");
    const QString hover = paletteRef("light");
    const QString pressed = paletteRef("highlight");
    // 次要 / 危险那两档的色。红**故意不取 `dangerColor()`**：那一支是"按深浅现推"的；
    // 而按钮使用很浅的固定底色，深色预设给出的浅红在这里对比度不够。所以这一档直接钉死
    // 那个对浅底仍有 4.8:1 的深红，它和 `errorTextColor()` 浅底那一档是同一个。
    // 红色那一档由 `buildPalette()` 按当前深浅写进 `BrightText`；浅红底在控件级样式里。
    const QString red = paletteRef("bright-text");
    const QString black = paletteRef("text");
    const QString disabledInk = paletteRef("link-visited");
    const QString disabledBorder = paletteRef("mid");
    const QString disabledFill = paletteRef("button");
    const QString accentFill = paletteRef("button");
    const QString neutralFill = paletteRef("button");
    // 危险那一档的完整配色（含浅红底）在 `kindButtonStyleSheet()` 里给每一颗 Danger
    // 按钮挂控件级样式；这一份应用级规则只是它没走到那条路时的兜底（中性底，不刺眼）。
    const QString dangerFill = paletteRef("button");

    // `padding: 6px` 不是随手写的：Fusion 的 `PM_ButtonMargin` 就是 6，边框 1 ⇒
    // 2*(6+1) = 14，和样式自己算的**一模一样**。差一点点的话界面上是看不出来，但一堆
    // "这一行放得下几颗按钮"的断言会整片地漂。
    // 圆角走 `buttonRadius()`、**高度走 `actionHeight()`**。
    // `min-height` 钉成 32：否则一颗按钮多高全看系统字号 + `padding: 6px` 算出来多少
    // （字号一换就在 30~33 之间飘，"一行里两颗一样高"是碰巧的）。钉成 32 之后它是个
    // **确定的数**，`Theme::actionHeight()` 那个常量也就是从这儿生效的。
    constexpr int kButtonChrome = 2 * 6 + 2 * 1;
    QString qss = QStringLiteral(
                      "QPushButton {"
                      " background-color: %3;"
                      " color: %1;"
                      " border: 1px solid transparent;"
                      " border-radius: %6px;"
                      " min-height: %7px;"
                      " padding: 6px; }"
                      // 默认那一颗（对话框里回车就按下去的那颗）：淡淡一层主题色的底。
                      // **不能跟悬停同色** —— 同色的话"鼠标停在哪儿"就看不出默认是哪一颗了。
                      "QPushButton:default:enabled { background-color: %4; }"
                      "QPushButton:hover:enabled { background-color: %4; }"
                      "QPushButton:pressed:enabled { background-color: %5; }"
                      "QPushButton[wtfmlKeyboardFocus=\"true\"]:focus { border-color: %2; }")
                      .arg(ink, border, accentFill, hover, pressed)
                      .arg(buttonRadius())
                      .arg(actionHeight() - kButtonChrome);
    // 主动作是页面上唯一一颗实心按钮。悬停和按下都向更深处走，白字对比度不会因为交互
    // 状态而降低；普通确认 / 危险 / 次要按钮仍是白底描边，不会整页争夺注意力。
    const auto primaryBlock = [](const QString &key, const QString &fill,
                                 const QString &hoverFill, const QString &pressedFill) {
        // 主动作单独高一档（36）。它是页面上唯一一颗实心
        // 按钮，"这一页最重要的那一下"值得多 4px；其余那颗留在 32 上，一页里两档就够。
        return QStringLiteral("QPushButton[%1=\"%2\"], QPushButton[%1=\"%2\"]:default:enabled { background-color: %3; color: #ffffff;"
                              " border-color: %3; border-radius: %6px; min-height: %7px;"
                              " padding-left: %8px; padding-right: %8px; min-width: %9px; }"
                              "QPushButton[%1=\"%2\"]:hover:enabled { background-color: %4;"
                              " border-color: %4; }"
                              "QPushButton[%1=\"%2\"]:pressed:enabled { background-color: %5;"
                              " border-color: %5; }")
            .arg(QString::fromLatin1(kButtonKindProperty), key, fill, hoverFill, pressedFill)
            .arg(buttonRadius())
            .arg(primaryActionHeight() - kButtonChrome)
            .arg(primaryButtonPadX())
            .arg(primaryButtonMinWidth() - 2 * primaryButtonPadX() - 2);  // min-width 是内容盒：扣内边距与左右各 1px 边框
    };
    // 主动作那两颗（悬停 / 按下）由 `kindButtonStyleSheet()` 给每一颗 Primary 挂控件级
    // 样式；这一份应用级规则只是兜底（同色），别拿它当"悬停会变深"的唯一出处。
    qss += primaryBlock(QStringLiteral("primary"), pure, pure, pure);

    // 危险 / 次要两档用轻填充建立形状，不再常驻一圈深色框；透明边仍占着 1px，
    // 键盘焦点出现时才换成有意义的语义色，因此不会改变按钮几何。
    const auto kindBlock = [](const QString &key, const QString &color, const QString &fill,
                              const QString &hoverFill, const QString &pressedFill) {
        return QStringLiteral("QPushButton[%1=\"%2\"] { color: %3; background-color: %4;"
                              " border-color: transparent; }"
                              "QPushButton[%1=\"%2\"][wtfmlKeyboardFocus=\"true\"]:focus { border-color: %3; }"
                              "QPushButton[%1=\"%2\"]:hover:enabled { background-color: %5; }"
                              "QPushButton[%1=\"%2\"]:pressed:enabled { background-color: %6; }")
            .arg(QString::fromLatin1(kButtonKindProperty), key, color, fill, hoverFill, pressedFill);
    };
    // 危险那一档的悬停 / 按下用的是比底色再深一档的红，调色板里没有第三、第四个红档；
    // 这一对由 `markButton()` 给那一颗按钮挂的**控件级**样式补上（只影响挂 Danger 的
    // 那几十颗，不影响整份应用样式表）。这里留同一档底色，保证没走那条路时也不会花。
    qss += kindBlock(QStringLiteral("danger"), red, dangerFill, dangerFill, dangerFill);
    qss += kindBlock(QStringLiteral("neutral"), black, neutralFill, hover, pressed);

    // ---------- Ghost（统一按钮视觉层级）----------
    //
    // 「低频辅助动作」那一档：**只有字**。和 `Neutral` 的分别不是颜色而是**形状** ——
    // 它没有底、没有边，所以它挪到卡片边上不抢眼（这是这一档存在的全部意义）。
    //
    // 边**不是删掉，是留成透明的 1px**。这一条是这一段最容易写错的地方：写
    // `border: none` 看着"更干净"，可它会把控件矮 2px、窄 2px（基样式那条
    // `QPushButton` 规则给的就是 1px 边）—— 于是同一个布局里换一档 kind，行里所有
    // 东西都跟着挪 1px。留着透明的边，三档的几何**一模一样**。
    // NOTE: 悬停/按下那两层浅灰和 `Neutral` 用的是同一对（`#f0f0f0` / `#e0e0e0`）：
    // 两档的区别在"平时有没有形"，不在"按下去什么色"。
    // NOTE: 勾上那一档给的是主题色那层淡底（和基样式里 `:default` 同一支色）—— 一颗
    // checkable 的 Ghost（比如"只看选中的"那种小开关）得让人看出它现在是"亮着"的。
    const QString ghostTint = pressed;
    qss += QStringLiteral("QPushButton[%1=\"ghost\"] { background-color: transparent;"
                          " border-color: transparent; color: %2; }"
                          "QPushButton[%1=\"ghost\"]:hover:enabled { background-color: %3; }"
                          "QPushButton[%1=\"ghost\"]:pressed:enabled { background-color: %4; }"
                          "QPushButton[%1=\"ghost\"]:checked { background-color: %5; }")
               .arg(QString::fromLatin1(kButtonKindProperty), black, hover, pressed, ghostTint);

    // Non-toggle text actions (refresh, open folder, export, reset, etc.)
    // share the neutral fill so their click target is visible at rest.
    qss += QStringLiteral("QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:enabled"
                          " { background-color: %2; color: %3; }"
                          "QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:hover:enabled"
                          " { background-color: %4; }"
                          "QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:pressed:enabled"
                          " { background-color: %5; }")
               .arg(QString::fromLatin1(kButtonKindProperty), neutralFill, black, hover, pressed);

    // 整页覆盖层顶上的面包屑「‹ 返回」：次要字色、贴字的小内边距（设计稿 .crumb）。
    qss += QStringLiteral("QPushButton[wtfmlCrumb=\"true\"] { color: %1; padding: 0px 8px; min-height: 0px; }")
               .arg(paletteRef("placeholder-text"));
    // 对话框底下那排按钮一样高：主动作那颗是 36，其余跟它齐（设计稿 .dlg .acts）。
    qss += QStringLiteral("QWidget[wtfmlDialogActions=\"true\"] QPushButton { min-height: %1px; }")
               .arg(primaryActionHeight() - kButtonChrome);

    // 「灰掉」这一条**必须摆在上面那两块之后**。
    // Qt 的样式表按**优先级**排：`QPushButton[wtfmlButtonKind="neutral"]` 和
    // `QPushButton:disabled` 都是"元素 + 一个属性/伪类"，**优先级一模一样** ——
    // 平手的规矩就是**谁写在后面谁赢**。`:disabled` 摆在最前面的话，中性/危险那两种
    // 按钮一灰掉，底变成 `disabledFill` 了（说明确实 disabled 生效了），**字和边却还是
    // `#202020`** —— 屏幕上看着就是一颗能按的按钮，只有主题色那颗（Accent，没有 kind
    // 属性、不参与这场平手）灰得对。禁用一颗中性按钮，底 `#F7F7F7` /
    // 字 `#202020`。所以这一条**最后**追加，把两种 kind 的字和边一并按下去。
    qss += QStringLiteral(
               "QPushButton:disabled { background-color: %1; color: %2;"
               " border-color: transparent; }")
               .arg(disabledFill, disabledInk);

    // 图标按钮那一档**必须摆在最后**：它和 `[wtfmlButtonKind="primary"]`
    // 优先级一样（都是"元素 + 一个属性"），平手时后面写的赢。它只有一句话 ——
    // "我按自己钉死的边长来"，把上面那条 `min-height` 按回去。
    // 写死 22 的 `QPushButton` 在 `min-height: 18px` 下实际是 **32**
    // —— `setFixedSize()` 拦不住它，只能靠这一条。`min-height: 0` 是有效的复位。
    // 主按钮那条的左右内边距 / 最小宽也在这里按回去（图标档是方块，不吃留白）。
    qss += QStringLiteral("QPushButton[%1=\"%2\"] { min-height: 0px; }"
                          "QPushButton[%3=\"primary\"][%1=\"%2\"] { min-width: 0px;"
                          " padding-left: 6px; padding-right: 6px; }")
               .arg(QString::fromLatin1(kButtonSizeProperty), QString::fromLatin1(kIconButtonValue),
                    QString::fromLatin1(kButtonKindProperty));

    // 快速启动卡自己按内容档位钉高度。和图标按钮相同，先把通用 QPushButton 的
    // min-height / padding 复位，否则 QSS polish 会把 C++ 里给卡片的高度压回普通按钮。
    //
    // ★ `compact` 那一档是主页实例列表展开时用的（上方启动块要让位）。它必须在这里有一档
    // 自己的 min-height —— 只靠 C++ 的 `setFixedHeight(52)` 拦不住：QSS 的 `min-height`
    // 是在**每一次 polish** 时 `setMinimumHeight()` 下去的（`QStyleSheetStyle::setGeometry()`），
    // 而悬停 / 按下 / 换主题都会来一发 polish，于是"点一下卡片，压小的那一截又长回 82"。
    //
    // ★★ 这里的数就是 C++ 里那个固定高度：7a 的按钮没有边框，`boxSize()` 不再额外加上下
    // 那 2px（旧壳有 1px 边时这里得写 50 才出 52）。写错一档，每次 polish 都会差 2px ——
    // 就是"点一下还是会长 / 缩一点点"。
    qss += QStringLiteral(
        "QPushButton[wtfmlQuickCard=\"true\"] { padding: 0px; border: none; }"
        "QPushButton[wtfmlQuickCardSize=\"normal\"] { min-height: 64px; max-height: 64px; }"
        "QPushButton[wtfmlQuickCardSize=\"compact\"] { min-height: 52px; max-height: 52px; }"
        "QPushButton[wtfmlQuickCardSize=\"icon\"] { min-height: 56px; max-height: 56px; }");
    // 主页压在风景上的几颗（设计稿 home.html）：账号胶囊 44、启动区「启动游戏」52（.btn.lg）、
    // 卡片上那颗 30 的播放钮。底色由 HeroButton 自己画，这里只钉尺寸。
    qss += QStringLiteral(
        "QPushButton[wtfmlAccountHeader=\"true\"] { padding: 0px; border: none; min-height: 44px; max-height: 44px; }"
        "QPushButton[wtfmlHeroStart=\"true\"] { min-height: 52px; max-height: 52px; padding: 0px 28px; border: none;"
        " border-radius: 14px; }"
        "QPushButton[wtfmlHeroSquare=\"true\"] { padding: 0px; border: none; min-height: 52px; max-height: 52px; }"
        "QPushButton[wtfmlHeroPlay=\"true\"] { padding: 0px; border: none; min-height: 30px; max-height: 30px; }");

    // 主页快速启动的空槽：它是布局占位和“去实例页添加”的入口，不应长得像已经能启动的
    // 实例。半透明感在 QWidget 上用一层极浅主题色表达；虚线边只用于空槽，不把普通卡片
    // 又带回“框框套框框”的旧观感。
    qss += QStringLiteral(
               "QPushButton[wtfmlQuickEmpty=\"true\"] { background-color: %1; color: %2;"
               " border: 1px dashed %3; }"
               "QPushButton[wtfmlQuickEmpty=\"true\"]:hover:enabled { background-color: %4; }"
               "QPushButton[wtfmlQuickEmpty=\"true\"]:pressed:enabled { background-color: %5; }")
               .arg(paletteRef("alternate-base"), ink, paletteRef("dark"), hover, pressed);

    // 主页次动作使用浅主题色表面，避免和 Disabled 共用一脸系统灰。它仍比实心 Primary
    // 弱很多，因此“启动”保持唯一主动作，账号、设置和实例卡则明确表现为可以点击。
    qss += QStringLiteral(
               "QPushButton[wtfmlHomeAction=\"true\"] { background-color: %1; color: %2;"
               " border-color: transparent; }"
               "QPushButton[wtfmlHomeAction=\"true\"]:hover:enabled { background-color: %3; }"
               "QPushButton[wtfmlHomeAction=\"true\"]:pressed:enabled { background-color: %4; }"
               "QPushButton[wtfmlHomeAction=\"true\"][wtfmlKeyboardFocus=\"true\"]:focus { border-color: %5; }")
               .arg(paletteRef("button"), ink, hover, pressed, border);

    // -----------------------------------------------------------------------
    // 输入框
    // -----------------------------------------------------------------------
    //
    // 搜索框的框框跟随主题色，文本保留黑色。所以**边**走主题色
    // （`Border` 那一档，对白底 6.7:1 —— 边框要看得见，不必像字那么重），**底一律白、
    // 字一律黑**（不去跟深浅：这一套界面上的按钮和输入框都是"白纸"，跟了深浅反而会在
    // 深色系统主题下出现白底黑字旁边一堆深灰控件）。
    //
    // 聚焦那一档换成 `Ink`（9.5:1）：边框从"看得见"变成"这一格是活的" —— 只靠加粗或者
    // 换灰是说不清"哪一格有光标"的，而换一档更深的同色系一眼就是它。
    // 高度：下拉框和所有输入框都略微改高一点。
    // 下面两条**要一起看**，它们必须落成一样高（并排摆着差 1px 一眼就看出来）：
    //
    // `QLineEdit` / `QComboBox`：`padding: 1px 3px` + `min-height: 20px`
    // `QAbstractSpinBox`： `min-height: 18px`
    //
    // 高度收口：**给内容盒统一下限** + 竖向 padding 1px：
    // 输入框 / 下拉框：20 + 2×1 + 2×1（边）= 24
    // 数字框： 18 + 6（它自己那一圈框，基样式画的）= 24
    // 正常字号下两边都是 24px；大字号下允许控件随字体一起长高，不能为对齐裁字。
    //
    // 水平那一位**故意还是 3px**：只改高度，横着那 1px 没有理由动 —— 动了就是
    // "这一页上所有输入框的字整体右移 1px"。
    qss += QStringLiteral(
               "QLineEdit { background-color: #ffffff; color: #000000; border: 1px solid %1;"
               " border-radius: %6px; padding: 1px 3px;"
               " min-height: 20px; }"
               "QLineEdit:focus { border-color: %2; }"
               "QLineEdit:disabled { background-color: %3; color: %4; border-color: %5; }")
               .arg(border, ink, disabledFill, disabledInk, disabledBorder)
               .arg(controlRadius());
    // 数字框的高度**不在这儿钉**：文件末尾那一族（`QLineEdit, QComboBox, QAbstractSpinBox`）
    // 给它和输入框、下拉框同一个盒模型（3px 透明边 + 1px padding + 同一个 min-height）。
    // 早先这里单独补过一条 `min-height: inputHeight() - 6`（数字框那 6px 框是基样式画的），
    // 现在框归样式表自己画，那条要是不删就会把数字框的**下限**顶高 2px —— 三样等高的钉子在
    // 它上面破掉。
    // 表头：不画白底、不描边，字用次要色（工具箱那几张表）。左右 padding 和默认行代理每一格的
    // 文字内边距是同一个数（DesignDefaults::cellTextInset()），表头字和那一列的字从同一条竖线起。
    qss += QStringLiteral("QHeaderView { background: transparent; border: none; }"
                          "QHeaderView::section { background: transparent; border: none;"
                          " color: %1; padding: 4px %2px; }"
                          "QTableCornerButton::section { background: transparent; border: none; }")
               .arg(paletteRef("placeholder-text"))
               .arg(DesignDefaults::cellTextInset());
    // **数字框 / 下拉框里面那一格也是 `QLineEdit`** —— 上面那条会连它一起框上，画出来
    // 就是"框里还套着一个框"（自旋框自己已经有一圈边了）。这一条把它按回去。
    // 用**后代**选择器（Qt 的 `>` 子选择器不可靠，后代选择器一定生效）。
    // **高度那两位也要按回去**（`min-height: 0px` 是有记录的复位写法，见
    // 下面 `markIconButton()` 那一条）。不按回去的话：里面那一格被钉成"内容高 20"，
    // 而它比数字框自己要的 18 还大 ⇒ **反过来把数字框撑到 26** —— 三样等高的意图从
    // 另一头破掉。
    qss += QStringLiteral("QAbstractSpinBox QLineEdit, QComboBox QLineEdit {"
                          " border: none; background: transparent; padding: 0;"
                          " min-height: 0px; max-height: 16777215px; }");

    // -----------------------------------------------------------------------
    // 下拉框
    // -----------------------------------------------------------------------
    //
    // 下拉框和输入框一样都走主题色。所以**和上面那条一模一样的四个色** ——
    // 白底 / 纯黑字 / `Border` 那一档的边 / 聚焦换 `Ink`。两处各写一份颜色是**有意**的：
    // 它们在这一页上是同一族控件（一个框住一段字、一个框住一个选择），差一档并排摆着
    // 一眼就看得出来。改这一条的时候顺手看一眼上面那条。
    //
    // 下拉框比输入框多一层：**弹出来那张表是它的子控件**。Qt 样式表里的 `color` 会
    // **往下传**，而那张表有自己的底色 —— 不点名的话，深色系统主题下弹出来的就是
    // "黑的字压在深色的底上"。所以最底下那条 `QComboBox QAbstractItemView` 把表和
    // 选中那一格也一起写成白底黑字（选中那一格用主题色打底、**黑墨**：黑压 `Pure`
    // 是 13.5:1，白压 `Pure` 只有 1.56:1）。
    //
    // NOTE: 8 号反馈之后下拉框**不再画任何箭头**：`::drop-down` 宽度 0、`border: none`，
    // 那一格什么都不画（整块仍可点 —— 非可编辑的 QComboBox 点框内任何位置都会弹）。
    // 原来那个补 `∨` 的 `ComboChevronPainter` 已经删了；这两处（这里和文件末尾那一族）
    // 写的是同一个规格，别只改一边。
    qss += QStringLiteral(
               "QComboBox { background-color: #ffffff; color: #000000; border: 1px solid %1;"
               " border-radius: %7px; padding: 1px 3px;"
               " min-height: 20px; }"
               "QComboBox:focus { border-color: %2; }"
               "QComboBox:disabled { background-color: %3; color: %4; border-color: %5; }"
               "QComboBox::drop-down { width: 0px; border: none; }"
               "QComboBox QAbstractItemView { background-color: #ffffff; color: #000000;"
               " border: 1px solid %1;"
               " selection-background-color: %6; selection-color: #000000; outline: none; }")
               .arg(border, ink, disabledFill, disabledInk,
                    disabledBorder, pure)
               .arg(controlRadius());

    // -----------------------------------------------------------------------
    // 勾选框
    // -----------------------------------------------------------------------
    //
    // Checkbox indicators are drawn by WtfmlProxyStyle at the actual device scale.
    // Do not override ::indicator here: QSS would bypass the custom checkmark.
    qss += QStringLiteral("QCheckBox { color: %1; }"
                          "QCheckBox:checked { color: %2; }"
                          "QCheckBox:disabled { color: %3; }")
               .arg(black, accentInk, disabledInk);

    // -----------------------------------------------------------------------
    // 滚动条
    // -----------------------------------------------------------------------
    //
    // 滚动条做成一个嵌在页面边上的条，不要直接一个白色条。
    //
    // 这一条一次说清三件事：
    //
    // 1. **没有底**（`background: transparent`）：Fusion 出厂那一条是"一格浅灰的槽",
    // 看着就是一根白条 —— 那正是要避免的。底色不画之后，滚动条那一条露出来的是
    // **它自己底下那一层**（页面 / 卡片的底），于是把手看着是"嵌在页面边上"，
    // 而不是"旁边另开了一栏"。
    // 2. **两头的小箭头不要**（`add-line` / `sub-line` 给 0）：这一套界面里没有人点那个
    // 三角翻一格。★ 但 `add-page` / `sub-page`（在槽上点一下翻一屏）**必须留着** ——
    // 给它 0 就是把"点槽翻页"这件事一起删掉了，只是"不画"，不是"没有"。
    // 3. **把手是一个胶囊**：宽 = `scrollBarThickness() - 2 * scrollBarHandleMargin()`，
    // 圆角 = 半宽（`scrollBarHandleRadius()`），墨色跟着深浅各取一支带 alpha 的灰
    // （`scrollHandleColor()` 那段讲了为什么必须跟着深浅走）。
    //
    // `margin` 是这儿**唯一一处"留白"**：它把把手从滚动条那一条的两边各推进来
    // `scrollBarHandleMargin()`，看着才是"嵌在边上的一条"而不是"顶满一整条"。
    // 横的那一条**必须跟着一起写**：这一套界面里横向滚动条是少，但少不等于没有（表里
    // 某一列放不下就出来了）。只写竖的，横的那一条会**整条退回 Fusion 出厂样子** ——
    // 那时候屏幕上"竖的变了、横的没变"，比两处都不改还显眼。两段**同一个 `.arg()`**，
    // 就是不许它们漂开。
    const QString handle = paletteRef("placeholder-text");
    const QString handleHover = paletteRef("mid");
    const QString handlePressed = paletteRef("dark");
    qss += scrollBarRules(handle, handleHover, handlePressed);

    // -----------------------------------------------------------------------
    // 页面骨架那几层"面"
    // -----------------------------------------------------------------------
    //
    // 页面骨架：整页只留**侧栏 + 正文**两个面，其余靠排版分层（不用一堆
    // QGroupBox 套起来）。这两块的色**从调色板现问**（`QPalette::Base` / `Mid`）——
    // 样式表里写的是 `palette(...)`，换主题只铺调色板，**不重挂样式表**（见这里上面
    // `widgetStyleSheet()` 开头那段）。
    //
    // 侧栏是"轻微底色"（正文色混主题色 6%）：一眼分得出"这一列是导航"，又不至于像一块
    // 独立的卡片。正文面就是 `Base`（今天那份卡片色）加一圈极细的边 —— 边**只有一档**
    // （`Mid`），不是主题色：整页上一个面而已，边上再点一笔主题色就变成第三个强调点了。
    // 小节卡片（`section`）比正文面**再淡一档**（6%），它装在正文面里面，"一块白压在白上"
    // 是看不见的（"标题 + 内容卡片"要的是卡片看得出来是一块）。
    // 页脚（`footer`）**不铺底**、只画上面那条分隔线：它是正文面的一部分，铺底会把它切
    // 成"又一个面"。"固定底栏"是**位置**上的固定（钉在正文面底部），不是又一个盒子。
    // 这几条**只管挂了这个属性的控件**（属性选择器），别处一个像素都不动。控件本身得是
    // QFrame（`QLabel`/裸 `QWidget` 不铺样式表给的底色）。
    //
    // **这里故意没有**下面这几条（不是漏了）：
    // `QListView[wtfmlNav="sidebar"]::item { min-height: 42px; padding: 0 12px;
    // border-radius: 7px; }` / `::item:selected { ... border-left: 3px solid ...; }`
    // —— 那两个"侧栏导航"（下载页左栏那六条分类、设置页左栏那五条）用的都是
    // `VersionTreeDelegate`，**整行是代理自己画的**（底色、文字、行高全在 paint() 里），
    // 样式表里那几条 `::item` 规则一条都匹配不到那层底色：写上去就是死代码 ——
    // 挂在那里让人以为"侧栏选中样式配好了"，而实际画出来的仍然只有代理那一支。
    // 所以那一档落成了**代理里的 `navRow` 模式**（`new VersionTreeDelegate(..., true)`），
    // 色一个字节都没另写：`selectionFill()`（这一列自己的底色混 20% 主题色）、
    // `selectionInk()`（主题色那一支里配套的那一档墨），都在 ui/Theme.hpp 里 ——
    // **全仓的列表选中态都是这一套**（和侧栏导航同款）。
    // 行高和圆角同理：那份列表的行高一直是 `versionTreeRowHeight()`（46，比 42 那条下限还高），
    // 而代理画的是一整行**不带圆角**的方块 —— 多出来的圆角只能由代理画（今天没画）。
    // 去套娃（7a）：sidebar / workspace / section 三层面**一律不铺底、不画边**，内容直接
    // 落在窗口那一层底上（`Glass::Backdrop`）。分组靠留白、小节标题和 1px 细线，属性照旧挂着
    // 只是为了让别处（滚动区、列表）能按"在哪一块里"认出来。
    {
        const QString sheetSecondary = paletteRef("placeholder-text");
        qss += QStringLiteral(
                   "QFrame[%1=\"sidebar\"], QFrame[%1=\"workspace\"], QFrame[%1=\"section\"],"
                   " QFrame[%1=\"footer\"], QFrame[wtfmlSidebarRail=\"true\"]"
                   " { background-color: transparent; border: none; }"
                   "QListWidget[wtfmlSidebarRail=\"true\"] { background: transparent; border: none; }"
                   "QLabel[%2=\"secondary\"] { color: %3; }")
                   .arg(QString::fromLatin1(kSurfaceProperty), QString::fromLatin1(kTextRoleProperty),
                        sheetSecondary);
    }
    // -----------------------------------------------------------------------
    // 三套预设给输入类控件 / 正文面那些"贴上去的颜色"
    // -----------------------------------------------------------------------
    //
    // 这一块**排在最后** ⇒ 按 QSS 的规矩（同优先级后写的赢）它盖在上面那几条之上。
    // 所以每加一句都得回上面看一眼**它盖掉的是哪一条**，以及那一条是不是钉过的规格。
    //
    // 两条**故意不在这里**：
    // · `QScrollBar`：滚动条有自己的唯一出处（上面那一段），「**不画底**」是拿像素钉死的
    // 那一条（ui_check ⑭：槽上量到的色必须**等于它底下那一层**）。这里加一句
    // `QScrollBar { background-color: track }` 就会因后写而赢 ⇒ 横竖两条当场长出
    // 一根底色条，⑭ 那一组（把手留白、把手宽度、那条"不画底"）跟着一起红。
    // · `QAbstractSpinBox`：数字框那一圈框是**基样式自己画的**（6px），它的高度是
    // `min-height: 18px` + 那 6px 凑出来的 24（见上面那段★★）。在这儿给它补一句
    // `border: 1px solid` 就会把那一圈的宽度改掉（量出来 23）—— 三样等高（24/24/24）
    // 那条钉子当场破。它的颜色跟着**调色板**走（`buildPalette()` 已经按预设铺过
    // Mid/Dark/Base/Text），不需要这一层再管。
    //
    // 圆角走 `controlRadius()`：**别在样式表里另写一个数** —— 换主题时那个数跟着主题一起
    // 变，`ui_modernize.py` 那类按名字定位的东西也认这个令牌。
    qss += QStringLiteral(
        "QLineEdit, QComboBox, QComboBox QAbstractItemView {"
        " background-color: %1; color: %2; border: 1px solid %3; border-radius: %7px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: %4; }"
        "QLineEdit:disabled, QComboBox:disabled { color: %5; background-color: %6; }"
        "QComboBox QAbstractItemView { selection-background-color: %8; selection-color: %2; }"
        "QTabWidget::pane { background-color: %9; border: none; }")
        .arg(paletteRef("base"), paletteRef("text"), paletteRef("base"), paletteRef("link"),
             paletteRef("placeholder-text"), paletteRef("button"),
             QString::number(controlRadius()), paletteRef("highlight"), paletteRef("window"));
    // 页面底、滚动区、页面里的列表 / 树：一律透明，直接排在窗口那一层底上
    // （弹出框里的列表不是这几块面的子孙，不受影响）。
    qss += QStringLiteral(
        "QTabWidget::pane { background-color: transparent; border: none; }"
        "QFrame[%1=\"workspace\"] QAbstractItemView, QFrame[%1=\"sidebar\"] QAbstractItemView"
        " { background-color: transparent; }"
        // 实例设置里那几页文件列表：直接排在对话框底上（圆角行底由它自己的代理画）。
        "QListWidget#instanceFilesList { background-color: transparent; border: none; }"
        "QScrollArea, QScrollArea > QWidget#qt_scrollarea_viewport,"
        " QScrollArea > QWidget#qt_scrollarea_viewport > QWidget { background: transparent; }")
               .arg(QString::fromLatin1(kSurfaceProperty));
    // 主页上那几格次动作（账号胶囊、实例卡）和「添加实例」空槽：淡底，不是不透明色块。
    qss += QStringLiteral(
        "QPushButton[wtfmlHomeAction=\"true\"] { background-color: palette(button); border: none; }"
        "QPushButton[wtfmlHomeAction=\"true\"]:hover:enabled { background-color: palette(shadow); }"
        "QPushButton[wtfmlQuickEmpty=\"true\"] { background-color: palette(light); border: none; }"
        "QPushButton[wtfmlQuickEmpty=\"true\"]:hover:enabled { background-color: palette(button); }");
    // ---- 设计稿控件层（14 种控件 × 6 态）：放在最后，同优先级时盖住上面的旧规则 ----
    // 悬停一律中性灰（light = fill-hover，shadow = fill-soft-hover），按下固定色（midlight =
    // fill-press），选中 = 强调色淡底（highlight）+ 强调字（link）。颜色仍全走 palette(...)。
    const QString kind = QString::fromLatin1(kButtonKindProperty);
    qss += QStringLiteral(
               "QPushButton, QPushButton[%1=\"neutral\"] { background-color: palette(button); color: palette(button-text); }"
               "QPushButton:hover:enabled, QPushButton[%1=\"neutral\"]:hover:enabled { background-color: palette(shadow); }"
               "QPushButton:pressed:enabled, QPushButton[%1=\"neutral\"]:pressed:enabled { background-color: palette(midlight); }"
               "QPushButton:checked, QPushButton[%1=\"neutral\"]:checked { background-color: palette(highlight); color: palette(link); }"
               "QPushButton:disabled, QPushButton[%1=\"neutral\"]:disabled { background-color: palette(light);"
               " color: palette(link-visited); }"
               // 挂了 neutral 的 QToolButton（「更改图标」浮层里图上字下那几格）与上面胶囊同一档；
               // 不挂就是原生白底，深色主题下字看不清。
               "QToolButton[%1=\"neutral\"] { background-color: palette(button); color: palette(button-text);"
               " border: 1px solid transparent; border-radius: %4px; padding: 6px; }"
               "QToolButton[%1=\"neutral\"]:hover:enabled { background-color: palette(shadow); }"
               "QToolButton[%1=\"neutral\"]:pressed:enabled { background-color: palette(midlight); }"
               "QToolButton[%1=\"neutral\"]:checked { background-color: palette(highlight); color: palette(link); }"
               // 文字按钮：无底强调字；可勾的（页签）平时 ink-2，勾上强调色淡底。
               "QPushButton[%1=\"ghost\"], QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:enabled"
               " { background-color: transparent; color: palette(link); }"
               "QPushButton[%1=\"ghost\"][checkable=\"true\"] { color: palette(placeholder-text); }"
               "QPushButton[%1=\"ghost\"]:hover:enabled,"
               " QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:hover:enabled"
               " { background-color: palette(light); }"
               "QPushButton[%1=\"ghost\"]:pressed:enabled,"
               " QPushButton[%1=\"ghost\"][wtfmlTextAction=\"true\"][checkable=\"false\"]:pressed:enabled"
               " { background-color: palette(midlight); }"
               "QPushButton[%1=\"ghost\"]:checked { background-color: palette(highlight); color: palette(link); }"
               "QPushButton[%1=\"ghost\"]:disabled { background-color: transparent; color: palette(link-visited); }"
               // 图标按钮：无底；悬停 / 按下中性，选中强调色淡底。
               "QPushButton[%2=\"%3\"] { background-color: transparent; }"
               "QPushButton[%2=\"%3\"]:hover:enabled { background-color: palette(light); }"
               "QPushButton[%2=\"%3\"]:pressed:enabled { background-color: palette(midlight); }"
               "QPushButton[%2=\"%3\"]:checked { background-color: palette(highlight); }")
               .arg(kind, QString::fromLatin1(kButtonSizeProperty), QString::fromLatin1(kIconButtonValue))
               .arg(buttonRadius());
    // 输入框 / 下拉 / 数字框：**同一族**（fill-soft 底、不描线、圆角 10、等高）。
    // 焦点按 F1：鼠标点进来只把底提亮一档，不画线；键盘 Tab / Backtab 到达才加 3px 淡环
    // （`wtfmlKeyboardFocus` 由 `ControlFocusVisibility` 挂，见那个过滤器）。
    //
    // ★ 四边**常驻 3px 透明边**：环出现时只换 `border-color`，盒子不长大、字不挪一格。
    // 内容盒 = inputHeight − 上下 3px 边 − 上下 1px padding，默认仍是 36；三种控件同一个盒模型，
    // 所以并排摆着一样高（那是 W13 的钉子）。
    qss += QStringLiteral(
        "QLineEdit, QComboBox, QAbstractSpinBox { background-color: palette(base);"
        " color: palette(text);"
        " border: 3px solid transparent; border-radius: %1px; padding: 1px 8px;"
        " min-height: %3px; }"
        "QLineEdit:hover:enabled, QComboBox:hover:enabled, QAbstractSpinBox:hover:enabled"
        " { background-color: palette(shadow); }"
        // 和 chip 并排的搜索框（`wtfmlChipHeight`）：QSS 的 min-height 会冲掉 setFixedHeight，高度也写在这儿。
        "QLineEdit[wtfmlChipHeight=\"true\"] { min-height: %2px; max-height: %2px; }"
        // 鼠标焦点：底提亮一档，四边仍是透明（**不画线**）。
        "QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus, QComboBox:on"
        " { background-color: palette(shadow); border-color: transparent; }"
        "QComboBox:on { background-color: palette(midlight); }"
        // 键盘焦点（只有 Tab / Backtab 到达才算）：3px 淡环。`highlight` = 选中淡底，
        // 就是「强调色压在本程序底上」那一档，三套预设加换主题色都跟着走。
        "QLineEdit[wtfmlKeyboardFocus=\"true\"]:focus,"
        " QComboBox[wtfmlKeyboardFocus=\"true\"]:focus,"
        " QAbstractSpinBox[wtfmlKeyboardFocus=\"true\"]:focus"
        " { border-color: palette(highlight); }"
        // 报错：淡环换危险色（下方那行小字仍由各页自己的报错展示给，这里只管这一圈）。
        "QLineEdit[wtfmlError=\"true\"], QLineEdit[inputError=\"true\"]"
        " { border-color: palette(bright-text); }"
        // ★ 禁用态也不画框：靠这一条把 border-color 改回透明。
        // 不能只依赖上面那条 `QLineEdit, QComboBox, QAbstractSpinBox`：文件前半还有一份
        // 旧白框时代的 `QComboBox:disabled { border-color: palette(mid) }`，它带伪类、
        // 优先级高于不带伪类的那条 —— 于是禁用下拉外面就多出一圈 3px 灰边
        // （用户看成“灰虚线框”那个）。这里随后写、选择器一模一样，才压得住。
        "QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled"
        " { background-color: palette(light); color: palette(link-visited);"
        " border-color: transparent; }"
        // 多行文本框：和输入框同一族（fill-soft 底、透明 3px 边、同圆角），键盘焦点同一圈淡环。
        // 原生框与视口底由 DesignDefaults 撤掉；自己挂了样式表的（聊天输入、日志）照旧以那份为准。
        "QPlainTextEdit, QTextEdit { background-color: palette(base); color: palette(text);"
        " border: 3px solid transparent; border-radius: %1px; padding: 2px 4px; }"
        "QPlainTextEdit[wtfmlKeyboardFocus=\"true\"]:focus, QTextEdit[wtfmlKeyboardFocus=\"true\"]:focus"
        " { border-color: palette(highlight); }"
        // 下拉不再有「文字 + ∨」那颗箭头：整块可点（点框内任何位置都会弹），那一格宽度归零。
        "QComboBox::drop-down { width: 0px; border: none; }"
        // ★ 数字框的内容盒再低 3px：`QStyleSheetStyle` 给 `CT_SpinBox` 算尺寸时会在
        // 我们这套盒模型之外**再加一圈基样式的框**（实测 36 → 39）。减掉这 3px 才是
        // 「三样一样高 36」，W13 那条钉子量的是摆进布局之后的 `height()`。
        "QAbstractSpinBox { min-height: %4px; }"
        // 上下箭头不画，但滚轮 / ↑↓ / PgUp / PgDn 照旧 —— 那是 QAbstractSpinBox 自己的键盘处理。
        "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button"
        " { width: 0px; height: 0px; border: none; background: transparent; }"
        // QToolButton 上的菜单小三角一律不画（本仓没有哪颗钮挂 menu）。
        "QToolButton::menu-indicator { width: 0px; height: 0px; image: none; }"
        // 下拉触发器（本仓那种「文字 + ∨」的钮改成的那颗）：胶囊（半径 = 高度一半）+ fill-soft，整块可点。
        // ★ 半径别写 999：Qt 的 QSS 遇到比半高还大的半径会整个放弃圆角，画成直角。
        "QPushButton[wtfmlComboTrigger=\"true\"] { background-color: palette(button);"
        " border-radius: 14px; padding: 0px 14px; min-height: 28px; max-height: 28px; }"
        // 同一排里的纯图标钮（页头刷新）：同高同底，圆形。
        "QPushButton[wtfmlComboTrigger=\"true\"][wtfmlIconOnly=\"true\"] { padding: 0px; }"
        // 设置页「Java 版本」那一行的值：和输入框同一族（淡底、圆角），整块可点。
        "QLabel[wtfmlFieldLabel=\"true\"] { background-color: palette(base); color: palette(text);"
        " border-radius: %1px; padding: 9px 12px; }"
        "QLabel[wtfmlFieldLabel=\"true\"]:hover { background-color: palette(shadow); }"
        "QPushButton[wtfmlComboTrigger=\"true\"]:hover:enabled { background-color: palette(shadow); }"
        "QPushButton[wtfmlComboTrigger=\"true\"]:pressed:enabled { background-color: palette(midlight); }"
        // 折叠行：整行一颗按钮，不带箭头；勾上（展开着）不再铺强调色淡底，只靠右边那两个字说明。
        "QPushButton[wtfmlFoldRow=\"true\"] { background-color: transparent; border: none; padding: 0px;"
        " min-height: 34px; max-height: 34px; color: palette(window-text); }"
        "QPushButton[wtfmlFoldRow=\"true\"]:checked { background-color: transparent; color: palette(window-text); }"
        "QPushButton[wtfmlFoldRow=\"true\"]:hover:enabled { background-color: palette(light); }"
        "QPushButton[wtfmlFoldRow=\"true\"]:pressed:enabled { background-color: palette(midlight); }"
        // 弹层里那根滚动条是 S2 那条 3px 细条：常态不显（`ComboDecorator` 用透明度动效
        // 让它只在滚动时露出来），所以这里只钉宽度、圆角和墨色，不画槽底。
        "QComboBox QAbstractItemView QScrollBar:vertical { width: 3px; background: transparent;"
        " margin: 0px; border: none; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical { background: palette(highlight);"
        " border-radius: 1px; min-height: 24px; }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical,"
        " QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height: 0px; border: none; }"
        "QComboBox QAbstractItemView QScrollBar::add-page:vertical,"
        " QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background: transparent; }"
        // 下拉弹层那张表也是透明的：磨砂底由 `ComboDecorator` 铺在弹层窗口上，项由
        // `PopupItemDelegate` 自己画（悬停淡底 / 选中强调淡底），都不许再铺一块不透明底。
        "QComboBox QAbstractItemView { background-color: transparent; color: palette(text);"
        " border: none; selection-background-color: transparent; selection-color: palette(text); }"
        "QCheckBox, QCheckBox:checked { color: palette(window-text); }"
        "QCheckBox:disabled { color: palette(link-visited); }"
        "QCheckBox::indicator { width: 18px; height: 18px; }"
        "QToolTip { background-color: palette(tool-tip-base); color: palette(tool-tip-text);"
        " border: none; padding: 5px 9px; }"
        "QProgressBar { background-color: palette(dark); border: none; border-radius: 2px; color: palette(text); }"
        "QProgressBar::chunk { background-color: palette(accent); border-radius: 2px; }"
        "QSlider:horizontal { min-height: 20px; }"  // 圆钮 16px 外加抗锯齿边，控件盒不到 18px 时底部会被裁平
        "QSlider::groove:horizontal { height: 3px; background: palette(dark); border-radius: 1px; }"
        "QSlider::sub-page:horizontal { height: 3px; background: palette(accent); border-radius: 1px; }"
        "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -7px 0; border-radius: 8px;"
        " background: palette(accent); }"
        "QSlider::handle:horizontal:hover { background: palette(link); }"
        "QSlider::handle:horizontal:disabled, QSlider::sub-page:horizontal:disabled"
        " { background: palette(link-visited); }")
        .arg(controlRadius())
        .arg(chipHeight() - 8)    // 内容盒 = chipHeight − 上下 3px 边 − 上下 1px padding
        .arg(inputHeight() - 8)   // 同上，默认 36
        .arg(inputHeight() - 11); // 数字框：再让 3px 给基样式多算的那一圈框
    // 分段（设计稿 .seg）：容器 fill-hover；段 ink-2，悬停 fill-hover + ink，按下 fill-press，勾上 accent-soft + accent。
    qss += QStringLiteral(
        "QWidget[wtfmlSegmented=\"true\"] { background-color: palette(light); border-radius: 12px; }"
        "QPushButton[wtfmlSegment=\"true\"] { min-height: 30px; max-height: 30px; padding: 0 14px; border: none;"
        " border-radius: 9px; background-color: transparent; color: palette(placeholder-text); }"
        "QPushButton[wtfmlSegment=\"true\"]:hover:enabled { background-color: palette(light); color: palette(window-text); }"
        "QPushButton[wtfmlSegment=\"true\"]:pressed:enabled { background-color: palette(midlight); color: palette(window-text); }"
        "QPushButton[wtfmlSegment=\"true\"]:checked { background-color: palette(highlight); color: palette(link); }"
        "QPushButton[wtfmlSegment=\"true\"]:disabled { background-color: transparent; color: palette(link-visited); }");
    // 右键 / 「…」菜单：玻璃面、不描硬边、圆角，项悬停淡底（窗口透明由 ControlFocusVisibility 在 Polish 时打开）。
    // 菜单（右键 / 「…」）：一律走 palette(...)，**不写死颜色** —— 这一份应用级样式表换主题时
    // 字符串必须不变（见 installButtonSheet()），写死 token 会让每次换主题都在换调色板**之前**
    // 重挂一遍，跟着主题算色的控件拿到的还是旧色（U32.4 那排 chip 就是这么停在深色上的）。
    // 底**不在这儿画**：磨砂玻璃由 `Popups::paintSurface()` 在菜单自己的 Paint 之前铺
    // （见 ui/Popups.cpp）。这里写成一个不透明底色就会把它整块盖掉。
    qss += QStringLiteral(
        "QMenu { background-color: transparent; color: palette(window-text); border: none;"
        " border-radius: 10px; padding: 6px; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 6px; background-color: transparent; }"
        "QMenu::item:selected { background-color: palette(light); color: palette(window-text); }"
        "QMenu::item:disabled { color: palette(link-visited); }"
        "QMenu::separator { height: 1px; background-color: palette(mid); margin: 4px 8px; }"
        "QMenu::icon { padding-left: 8px; }");
    return qss;
}

QStyle *createStyle() {
    if (qApp != nullptr) {
        PageScrollKeyFilter::install(*qApp);
    }
    QStyle *style = new WtfmlProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    // 名字是给 `ui_shot` 那行 `style: …` 看的 —— 那一行存在的意义就是"图是在哪个样式下画的
    // 必须能从产物本身看出来"。`QProxyStyle` 自己不设 `objectName`，不设的话那一行会印成
    // 空的，等于把这道哨兵悄悄撤了（`QStyleFactory::create()` 会给它造的那一份设成 "fusion"，
    // 所以我们这里接过来改个名，见 `Theme.hpp` 那段）。
    style->setObjectName(QStringLiteral("wtfml-fusion"));
    return style;
}

// ---------------------------------------------------------------------------
// 预设
// ---------------------------------------------------------------------------

QStringList presetIds() {
    // 这一个是设置页「配色」那一格的下拉顺序：出厂默认那一档排第一。
    return {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean"),
            QStringLiteral("system")};
}

QString presetName(const QString &id) {
    // 显示名照着各自那张内置风景起（`assets/scenes/`）：dark = 暮色雪山（麦克唐纳湖的暮光雪山），
    // emerald = 冰川碧湖（梦莲湖），ocean = 落日海崖（雷斯岬）。只换显示名，内部 id 与顺序不动 ——
    // 盘上存的是 id，老设置照常读回。
    if (id == QLatin1String("light") || id == QLatin1String("emerald")) {
        return QCoreApplication::translate("Theme", "冰川碧湖");
    }
    if (id == QLatin1String("dark")) return QCoreApplication::translate("Theme", "暮色雪山");
    if (id == QLatin1String("ocean")) return QCoreApplication::translate("Theme", "落日海崖");
    return QCoreApplication::translate("Theme", "跟随系统");
}

bool isPresetId(const QString &id) {
    return presetIds().contains(id) || id == QLatin1String("light");
}

Spec presetSpec(const QString &id) {
    Spec spec;
    spec.presetId = isPresetId(id) ? id : defaultSpec().presetId;
    spec.accent = tokens(spec.presetId).primary;
    return spec;
}

Spec defaultSpec() {
    // 新安装默认使用暮色雪山：主题色跟着预设那一支走（和 `presetSpec()` 同一个来源）。
    // 盘上存过预设 / 主题色 / 覆盖项的继续按存的那一份读回。
    Spec spec;
    spec.presetId = QStringLiteral("dark");
    spec.accent = tokens(spec.presetId).primary;
    return spec;
}

// ---------------------------------------------------------------------------
// 当前这一份
// ---------------------------------------------------------------------------

Spec normalize(const Spec &spec) {
    Spec clean;
    clean.presetId = spec.presetId == QLatin1String("light") ? QStringLiteral("emerald")
                       : isPresetId(spec.presetId) ? spec.presetId : defaultSpec().presetId;
    clean.textOpacity = clampOpacity(spec.textOpacity);
    // 主题色：认不得（盘上手改歪了 / 没写过）一律退回出厂那一支 —— 它没有"空"这个状态，
    // 界面上随时都有顶栏那一块要画。
    clean.accent = spec.accent.isValid() ? spec.accent : tokens(clean.presetId).primary;
    for (auto it = spec.roles.constBegin(); it != spec.roles.constEnd(); ++it) {
        // 越界的下标（盘上手改出来的）直接丢：认不出的角色没有"画面上是哪儿"可言。
        if (it.key() < 0 || it.key() >= roleCount() || !it.value().isValid()) {
            continue;
        }
        clean.roles.insert(it.key(), it.value());
    }
    for (auto it = spec.parts.constBegin(); it != spec.parts.constEnd(); ++it) {
        if (it.key() < 0 || it.key() >= partCount()) {
            continue;
        }
        PartStyle style = it.value();
        style.hasBackground = style.hasBackground && style.background.isValid();
        style.hasForeground = style.hasForeground && style.foreground.isValid();
        style.textOpacity = style.textOpacity < 0 ? -1 : clampOpacity(style.textOpacity);
        // 图这一位：**路径**是唯一说得准"有没有"的东西（`hasImage` 由它推出来，不是另立
        // 一位让两边对不上 —— 空字符串天然就是"没设过"）。
        // 摆法认不出的（盘上手改歪了）退回默认那一种，不整条丢掉 —— 图还在，只是摆法回到
        // 「铺满」；为了一个摆法把用户挑的图整个忽略掉，那是把"读不懂"变成了"没设过"。
        style.imagePath = style.imagePath.trimmed();
        style.hasImage = !style.imagePath.isEmpty();
        if (style.hasImage) {
            if (static_cast<int>(style.fit) < 0 || static_cast<int>(style.fit) >= imageFitCount()) {
                style.fit = ImageFit::Cover;
            }
            style.imageOpacity = clampOpacity(style.imageOpacity);
            style.imageName = style.imageName.trimmed();
        } else {
            style.imagePath.clear();
            // 没图了，"当初挑的那张叫什么"也就没有意义了（留着的话下次读回来这一块会
            // 带着一个孤儿名字）。
            style.imageName.clear();
        }
        // 一项覆盖都没有的那一块**不留**：留着的话"这一块设过"这件事在盘上是假的
        // （读回来之后 `parts.contains()` 说它有，而它什么都没改）。
        if (!style.hasBackground && !style.hasForeground && style.textOpacity < 0 &&
            !style.hasImage) {
            continue;
        }
        clean.parts.insert(it.key(), style);
    }
    clean.danger = spec.danger.isValid() ? spec.danger : QColor();
    clean.warning = spec.warning.isValid() ? spec.warning : QColor();
    clean.ok = spec.ok.isValid() ? spec.ok : QColor();
    clean.errorText = spec.errorText.isValid() ? spec.errorText : QColor();
    return clean;
}

const Spec &current() {
    return mutableSpec();
}

void setSpec(const Spec &spec) {
    mutableSpec() = normalize(spec);
}

// ---------------------------------------------------------------------------
// 铺开
// ---------------------------------------------------------------------------

bool isDarkPalette(const QPalette &palette) {
    // 判据取的是**窗口底色的明度**（不是某个"主题叫什么"的标志位）：深底用浅红、
    // 浅底用深红，两种主题下都是"一眼看出是红字"而不是"一团糊在底色里"。
    return palette.color(QPalette::Window).lightness() < 128;
}

bool isDark() {
    return isDarkPalette(QGuiApplication::palette());
}

QPalette buildPalette(const Spec &spec, const QPalette &base) {
    QPalette palette = base;
    const Tokens t = tokens(spec.presetId);
    palette.setColor(QPalette::Window, t.page);
    palette.setColor(QPalette::Base, t.input);
    palette.setColor(QPalette::AlternateBase, t.main);
    palette.setColor(QPalette::WindowText, t.text);
    palette.setColor(QPalette::Text, t.text);
    palette.setColor(QPalette::Button, t.subtle);
    palette.setColor(QPalette::ButtonText, t.text);
    palette.setColor(QPalette::Highlight, t.selected);
    palette.setColor(QPalette::HighlightedText, t.text);
    palette.setColor(QPalette::PlaceholderText, t.secondary);
    palette.setColor(QPalette::Mid, t.divider);
    palette.setColor(QPalette::Dark, t.stroke);
    palette.setColor(QPalette::ToolTipBase, t.card);
    palette.setColor(QPalette::ToolTipText, t.text);
    palette.setColor(QPalette::Link, textTone(t.accentInk, t.deep));  // 强调色写的字（chip / 分段选中、文字按钮）
    for (const auto role : kInkRoles) palette.setColor(QPalette::Disabled, role, t.disabled);

    for (auto it = spec.roles.constBegin(); it != spec.roles.constEnd(); ++it) {
        if (it.key() < 0 || it.key() >= roleCount() || !it.value().isValid()) {
            continue;
        }
        palette.setColor(qtRole(static_cast<Role>(it.key())), it.value());
    }

    // 样式表里那些"跟着主题走"的颜色全部走 `palette(...)` 引用，
    // 所以调色板除了那几档 `Role`，还得把样式表要念的这几档一起备好。**只在这里备**：
    // 它们是样式表的"变量"，不是用户能单独挑的 `Role`（见头文件里 `Role` 那段）。
    //
    // 为什么非走这一步：`QApplication::setStyleSheet()` 重挂整份样式表会把**每一个**
    // 控件重新 polish 一遍 —— 6800 个控件量到 1.9 秒，界面就卡在那儿。调色板当变量
    // 之后样式表字符串一个字节都不变，换配色只铺调色板 + 重 polish（~100 ms）。
    const QColor accent = spec.accent.isValid() ? spec.accent : t.primary;
    palette.setColor(QPalette::Accent, accent);
    palette.setColor(QPalette::Light, t.hover);
    palette.setColor(QPalette::Midlight, t.fillPress);
    palette.setColor(QPalette::BrightText, t.danger);
    // 灰掉那一档的字和边借 `LinkVisited` / `Shadow` 两格（Fusion 不拿它们画控件）：
    // 它们必须是真的 `t.disabled` / `t.divider`，不能拿 `PlaceholderText` 顶 ——
    // 那一档是"次要文字"，比灰掉的字深，混用会让两档在屏幕上分不出来。
    palette.setColor(QPalette::LinkVisited, t.disabled);
    palette.setColor(QPalette::Shadow, t.fillSoftHover);  // 次按钮 / 输入框悬停（样式表念它）

    if (spec.textOpacity != 100) {
        for (QPalette::ColorRole role : kInkRoles) {
            for (QPalette::ColorGroup group : colorGroups) {
                QColor color = palette.color(group, role);
                if (!color.isValid()) {
                    continue;
                }
                // **乘**上去而不是盖掉：`PlaceholderText` 本来就带一点透明度，
                // 直接赋值等于把它从半透明变成"和正文一样清楚"。
                color.setAlpha(color.alpha() * spec.textOpacity / 100);
                palette.setColor(group, role, color);
            }
        }
    }
    return palette;
}

void notifyWidgets() {
    if (QGuiApplication::instance() == nullptr) {
        return;
    }
    QList<QPointer<QWidget>> widgets;
    const QList<QWidget *> all = QApplication::allWidgets();
    widgets.reserve(all.size());
    for (QWidget *widget : all) {
        widgets.append(widget);
    }
    for (const QPointer<QWidget> &widget : widgets) {
        if (widget.isNull()) {
            continue;
        }
        QEvent event(QEvent::PaletteChange);
        QCoreApplication::sendEvent(widget.data(), &event);
    }
}

/// 把那一套控件外观（按钮 / 输入框 / 勾选框）挂到应用上。**只在字符串真变了的时候挂**。
///
/// 应用身上挂着样式表的时候，`QApplication::setPalette()` 那套"发给每个控件"的传播会
/// **漏掉一部分控件** —— 最明显的是 `QCheckBox`：从深色换回"跟着系统"之后，它记着的
/// 还是深色那一份调色板（白字 + 深色勾选框），在浅底上那行字就**看不见了**。显式
/// `polish()` 一圈能冲掉那层缓存（~100 ms），比重新挂整份样式表（6800 个控件
/// ~1.9 秒）小一个数量级。
void installButtonSheet(QApplication &app) {
    const QString buttons = widgetStyleSheet();
    static QString installed;
    if (installed == buttons) {
        return;
    }
    installed = buttons;
    app.setStyleSheet(buttons);
}

namespace {

/// 当前是不是正在 `apply()` 里面（给"系统深浅变了"那条回调防自咬用）。
bool &applyingTheme() {
    static bool applying = false;
    return applying;
}

/// 把"该重新取色了"这件事落到每一个控件身上（`polish()`）。
void repolishWidgets() {
    if (QGuiApplication::instance() == nullptr) {
        return;
    }
    QList<QPointer<QWidget>> widgets;
    const QList<QWidget *> all = QApplication::allWidgets();
    widgets.reserve(all.size());
    for (QWidget *widget : all) {
        widgets.append(widget);
    }
    for (const QPointer<QWidget> &widget : widgets) {
        if (widget.isNull()) {
            continue;
        }
        // 滚动条身上挂的是控件级精确样式（见 `styleScrollBar()`）：那份颜色不来自
        // `palette(...)`，刚刚已经由 `styleScrollBars()` 重挂过了，不必再白跑一趟。
        if (qobject_cast<QScrollBar *>(widget.data()) != nullptr) {
            continue;
        }
        // 自己 `setPalette()` 过的控件（标题栏 / 底栏 / 侧栏 / 卡片…）**收不到**
        // `QApplication::setPalette()` 那套自动传播 —— Qt 只把"应用调色板变了"发给
        // 没有本地调色板的控件。它们各自存着一份算好的色，靠 `changeEvent()` 重算；
        // 这条路径不发 StyleChange，所以这里点名补一发。
        // NOTE: 判据用 `WA_SetPalette`，不乱发 —— 没本地调色板的控件 Qt 已经发过了。
        if (widget->testAttribute(Qt::WA_SetPalette)) {
            QEvent event(QEvent::PaletteChange);
            QCoreApplication::sendEvent(widget.data(), &event);
            if (widget.isNull()) {
                continue;
            }
        }
        widget->style()->polish(widget.data());
    }
}

/// 换主题时把每一根滚动条的控件级样式重算一遍。
///
/// 只算**露着的**那些（见下面那条为什么）。换主题的一下里，一屏上真露着的滚动条
/// 通常只有一两根，而全应用几百根一起重挂的价钱是 15–26 ms
/// （perf 探针的 `THEME-PROBE` 拆分，2026-10-31），和"重 polish 所有控件"同量级。
void styleScrollBars() {
    ++scrollBarThemeGeneration();
    const QList<QWidget *> all = QApplication::allWidgets();
    for (QWidget *widget : all) {
        if (auto *bar = qobject_cast<QScrollBar *>(widget)) {
            // 藏着的**不在这儿算**：它们身上的代数标记还是旧的，露出来的那一刻
            // ScrollBarStyler 收 Show 事件补一发（`styleScrollBar()` 先看标记，
            // 旧标记 ⇒ 真重挂）。少这一趟不影响它们该有的颜色，只是把活挪到它
            // 第一次真的要画的时候。
            if (bar->isVisible()) {
                styleScrollBar(bar);
            }
        }
    }
}

/// 「跟着系统」那一档：平台深浅一换，重新铺一遍我们那份调色板。
///
/// 为什么需要它："跟着系统"也会铺一份我们自己算的调色板（样式表里
/// 的 `palette(...)` 需要它）。铺过之后 Qt 就不再自动把平台调色板同步过来了，所以
/// 平台深浅的变化得自己接住。
class ColorSchemeWatcher : public QObject {
public:
    explicit ColorSchemeWatcher(QApplication &app) : QObject(&app) {
        setObjectName(QStringLiteral("wtfmlColorSchemeWatcher"));
        connect(app.styleHints(), &QStyleHints::colorSchemeChanged, this,
                [](Qt::ColorScheme) {
                    if (applyingTheme() || current().presetId != QLatin1String("system")) {
                        return;
                    }
                    // 已经和平台一致就不用再铺（`apply()` 自己也会调 `setColorScheme()`，
                    // 那一下可能把这条信号带回自己头上）。
                    if (platformColorSchemeDark() ==
                        isDarkPalette(QGuiApplication::palette())) {
                        return;
                    }
                    apply(*qApp);
                });
    }
};

}  // namespace

void apply(QApplication &app) {
    if (applyingTheme()) {
        return;
    }
    applyingTheme() = true;
    if (app.font().pixelSize() != textSize(TextRole::Body))
        app.setFont(textFont(app.font(), TextRole::Body));
    if (app.findChild<QObject *>(QStringLiteral("wtfmlControlFocusVisibility")) == nullptr)
        new ControlFocusVisibility(app);
    PageScrollKeyFilter::install(app);
    if (app.findChild<QObject *>(QStringLiteral("wtfmlScrollBarStyler")) == nullptr)
        new ScrollBarStyler(app);
    if (app.findChild<QObject *>(QStringLiteral("wtfmlColorSchemeWatcher")) == nullptr)
        new ColorSchemeWatcher(app);
    // 浮层那一层（自绘 tooltip / 下拉弹层 / 菜单外壳）：自己按 objectName 幂等安装。
    Popups::install(app);
    Glass::install(app);

    const Spec &spec = current();
    const bool followSystem = spec.presetId == QLatin1String("system");

    // -1) 按钮 / 输入框那一套外观：应用级样式表。**换主题时不重挂**（字符串不变），
    // 只在这一份字符串真的变了（第一次铺、或者以后改了结构）时挂一次。
    installButtonSheet(app);

    // 1) 深浅那一档。"跟着系统"仍然交给平台（原生对话框那些也跟着走）；只是我们自己
    // 那份调色板按平台的深浅算，见 `platformColorSchemeDark()`。
    if (followSystem) {
        app.styleHints()->unsetColorScheme();
    } else {
        app.styleHints()->setColorScheme(spec.presetId == QLatin1String("dark")
                                             ? Qt::ColorScheme::Dark
                                             : Qt::ColorScheme::Light);
    }

    // 2) 基色一律现问样式要。**不能拿"现在的应用调色板"当基色**：铺过一次深色预设之后
    // 它就是深色那一份了，再叠一次"跟系统"的覆盖项会得到深底 + 浅色覆盖的怪东西。
    // 这一刻深浅那一档已经定好，所以 `standardPalette()` 问到的就是那一档。
    const QPalette base = app.style()->standardPalette();
    const QPalette before = app.palette();
    app.setPalette(buildPalette(spec, base));

    if (app.palette() != before) {
        // 这一趟**真**把调色板换掉了 ⇒ 让每一份 `palette(...)` 重新解析一遍。
        styleScrollBars();
        repolishWidgets();
    } else {
        // 调色板**没变**的时候 Qt 一个通知都不发，而"换了一张背景图 / 改了摆法 /
        // 调了不透明度 / 换了某一块的底色"这些东西**都不在调色板里**
        // （`buildPalette()` 只读 preset / roles / textOpacity）。于是 `apply()`
        // 跑完了、规格也换了，屏幕上却还是上一张图 —— `ThemeBackground` 记着两份位图，
        // 只有收到 "调色板变了" 才会重画（见它构造里那句 `follow`）。
        // 症状：换图之后要切到别的选项卡再切回来、或者拖一下窗口改尺寸，才看得见新图。
        notifyWidgets();
    }
    applyingTheme() = false;
}

// ---------------------------------------------------------------------------
// 每一块 / 每一个语义色
// ---------------------------------------------------------------------------

QColor partBackground(Part part) {
    const int index = static_cast<int>(part);
    if (index < 0 || index >= partCount()) {
        return QColor();
    }
    const auto it = current().parts.constFind(index);
    if (it != current().parts.constEnd() && it->hasBackground) {
        return it->background;
    }
    const PartInfo &info = kPartInfos[index];
    if (current().presetId != QLatin1String("system")) {
        const Tokens t = tokens();
        if (part == Part::TitleBar) return current().accent == t.primary ? t.deep : current().accent;
        if (part == Part::BottomBar) return t.subtle;
        if (part == Part::Panel || part == Part::Card) return t.card;
    }
    if (info.accent) {
        // 顶栏：主题色那一支。用户单独给这一块设过底色的话上面那条已经
        // 返回了 —— 覆盖永远赢过出厂值。
        return accentShade(AccentShade::Pure);
    }
    if (info.literal) {
        return scrimLiteral();
    }
    const QColor from = appColor(info.from);
    return info.shade == 0 ? from : from.darker(info.shade);
}

bool partHasBackground(Part part) {
    const int index = static_cast<int>(part);
    if (index < 0 || index >= partCount()) {
        return false;
    }
    const auto it = current().parts.constFind(index);
    return it != current().parts.constEnd() && it->hasBackground;
}

bool partLightInk(Part part) {
    const int index = static_cast<int>(part);
    // 白墨这一位跟着"顶栏底色 = 主题色那一支"走：绿条上的字必须够白才读得出来
    // （顶栏上那几个窗口按钮跟着用白墨）。它是**出厂值**，
    // 用户单独给这一块设过前景色的话那个覆盖照样赢（`partForeground()` 里那条在前）。
    return index >= 0 && index < partCount() && kPartInfos[index].accent;
}

QColor partForeground(Part part, const QColor &fallback) {
    const int index = static_cast<int>(part);
    if (index < 0 || index >= partCount()) {
        return fallback;
    }
    // 没人给这一块设过前景色时，出厂字色就是这一位上写着的那一支（顶栏是**白墨**，
    // 别的块才是调用方递进来的那个兜底色）。
    const QColor factoryInk = kPartInfos[index].accent ? QColor(Qt::white) : fallback;
    const auto it = current().parts.constFind(index);
    if (it == current().parts.constEnd()) {
        return factoryInk;
    }
    QColor color = it->hasForeground ? it->foreground : factoryInk;
    // 这一块**自己**那一位才在这儿生效；全局那一位落在调色板的文字角色上（`buildPalette()`），
    // 而 `fallback` 多半就是从调色板取来的 ⇒ 在这儿再乘一次就是乘了两遍。
    if (it->textOpacity >= 0 && color.isValid()) {
        color.setAlpha(color.alpha() * it->textOpacity / 100);
    }
    return color;
}

PartImage partImage(Part part) {
    PartImage image;
    const int index = static_cast<int>(part);
    if (index < 0 || index >= partCount()) {
        return image;
    }
    const auto it = current().parts.constFind(index);
    if (it == current().parts.constEnd() || !it->hasImage) {
        return image;
    }
    image.path = it->imagePath;
    image.name = it->imageName;
    image.fit = it->fit;
    image.opacity = clampOpacity(it->imageOpacity);
    return image;
}

QImage cachedImage(const QString &path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    // `路径|大小|修改时间`：和 `ThemeBackground` 那一串同一个口径，换了一张图时后两位会变。
    const QString key = path + QLatin1Char('|') + QString::number(info.size()) +
                        QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
    // 按张数封顶：一张 4K 图解码后约 33 MB，留住的是"最近用过的那几张"，不是整个相册。
    static QCache<QString, QImage> cache(6);
    if (const QImage *hit = cache.object(key)) {
        return *hit;
    }
    const QImage image(path);
    if (!image.isNull()) {
        cache.insert(key, new QImage(image));
    }
    return image;
}

QColor scrimColor() {
    return partBackground(Part::Scrim);
}

namespace {

/// 语义色的出厂值：一深一浅两档。**参数是"算不算深色"**，不是调色板 —— 三个语义色
/// 都只用得到这一位。
QColor dangerFor(bool dark) {
    return dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xc0, 0x28, 0x28);
}

}  // namespace

QColor dangerColor(const QPalette &palette) {
    const QColor override = current().danger;
    if (override.isValid()) {
        return override;
    }
    return dangerFor(isDarkPalette(palette));
}

QColor warningColor(const QPalette &palette) {
    const QColor override = current().warning;
    if (override.isValid()) {
        return override;
    }
    return isDarkPalette(palette) ? QColor(0xff, 0xd2, 0x4d) : QColor(0xe0, 0xa0, 0x00);
}

QColor okColor(const QPalette &palette) {
    const QColor override = current().ok;
    if (override.isValid()) {
        return override;
    }
    return isDarkPalette(palette) ? QColor(0x5e, 0xd1, 0x7a) : QColor(0x1f, 0x9d, 0x4d);
}

QColor errorTextColor(const QPalette &palette) {
    const QColor override = current().errorText;
    if (override.isValid()) {
        return override;
    }
    // 浅底那一档是写死的 #c0392b（对浅色主题的窗口底色 #f0f0f0
    // 对比度 4.77:1，够 WCAG 正文那条 4.5:1）。深底那一档没法照搬：同一个 #c0392b 落到
    // 深色主题的底上只有 2.26:1 —— 名字写错了却看不出底下那行写了什么，等于没有。
    // 所以借用同一个界面里已经调好的那个浅红（`dangerColor()`）。
    // NOTE: 两处红**故意**不合并成一个语义色：浅底那一档 #c0392b 和 #c02828 各有各的
    // 对比度来历（见 `MainWindowUI::installNameErrorColor()` 那段注释）。
    return isDarkPalette(palette) ? dangerFor(true) : QColor(0xc0, 0x39, 0x2b);
}

// ---------------------------------------------------------------------------
// 落盘
// ---------------------------------------------------------------------------

namespace {

/// `load()` 那一刻盘上是什么（只收 `Theme/` 这一组），外加那份配置文件的身份。
///
/// 有了它，`save()` 才能只写**这一次真改过的项**：两个安装目录各自改不同的项时，
/// 后写的那个不会把对方刚改的那一项抹回自己启动时看到的样子（和
/// `LauncherSettings` / `GlobalJavaSettings` 同一个路数，见
/// docs/features/DIRECTORY_SESSION_LOCK.md）。
struct PersistedThemeValues {
    QString scope;
    QHash<QString, QVariant> values;
    QHash<QString, QVariant> semantic;
};

PersistedThemeValues &persistedThemeValues() {
    static PersistedThemeValues persisted;
    return persisted;
}

/// 配置文件的身份：换了文件（测试换临时目录、`-settings`）基线就得作废。
QString settingsScope(QSettings &settings) {
    return settings.fileName() + QLatin1Char('|') + settings.organizationName() +
           QLatin1Char('|') + settings.applicationName();
}

const char *const kThemeGroupPrefix = "Theme/";

}  // namespace

static QHash<QString, QVariant> themeSettingValues(const Spec &spec) {
    const Spec clean = normalize(spec);

    // 先算出"这一次该长什么样"，再和基线逐项比 —— 不再整组清掉重写。
    QHash<QString, QVariant> desired;
    desired.insert(QLatin1String(kPresetKey), clean.presetId);
    desired.insert(QLatin1String(kTextOpacityKey), clean.textOpacity);
    desired.insert(QLatin1String(kAccentKey), colorToString(clean.accent));
    for (auto it = clean.roles.constBegin(); it != clean.roles.constEnd(); ++it) {
        desired.insert(roleSettingKey(static_cast<Role>(it.key())), colorToString(it.value()));
    }
    for (auto it = clean.parts.constBegin(); it != clean.parts.constEnd(); ++it) {
        const PartStyle &style = it.value();
        const Part part = static_cast<Part>(it.key());
        if (style.hasBackground) {
            desired.insert(partSettingKey(part, "Background"), colorToString(style.background));
        }
        if (style.hasForeground) {
            desired.insert(partSettingKey(part, "Foreground"), colorToString(style.foreground));
        }
        if (style.textOpacity >= 0) {
            desired.insert(partSettingKey(part, "TextOpacity"), style.textOpacity);
        }
        if (style.hasImage) {
            // 路径按 `QDir::toNativeSeparators` 写（注册表里能手看）—— 读回来再统一成
            // `/`，见 `load()`。这一来一回是**故意的**：注册表里那串得是 `\`，用户在
            // regedit 里一眼认得出那是自己盘上的哪一条；而这一层内部一律用 `/`。
            desired.insert(partSettingKey(part, "Image"),
                           QDir::toNativeSeparators(style.imagePath));
            desired.insert(partSettingKey(part, "ImageFit"), imageFitKey(style.fit));
            desired.insert(partSettingKey(part, "ImageOpacity"), style.imageOpacity);
            // 用户挑的那张**原来叫什么**：设置页那一行「现在铺的是：…」写的是它，不是我们
            // 存进来那个槽位名（见 `PartStyle::imageName`）。空的不写 —— 读回来是空的就
            // 退回落盘那个文件的名字。
            if (!style.imageName.isEmpty()) {
                desired.insert(partSettingKey(part, "ImageName"), style.imageName);
            }
        }
    }
    if (clean.danger.isValid()) {
        desired.insert(accentSettingKey("Danger"), colorToString(clean.danger));
    }
    if (clean.warning.isValid()) {
        desired.insert(accentSettingKey("Warning"), colorToString(clean.warning));
    }
    if (clean.ok.isValid()) {
        desired.insert(accentSettingKey("Ok"), colorToString(clean.ok));
    }
    if (clean.errorText.isValid()) {
        desired.insert(accentSettingKey("ErrorText"), colorToString(clean.errorText));
    }
    return desired;
}

void save(const Spec &spec) {
    QSettings settings;
    const auto desired = themeSettingValues(spec);

    PersistedThemeValues &persisted = persistedThemeValues();
    const QString scope = settingsScope(settings);
    if (persisted.scope != scope) {
        // 没有基线（这个进程还没 `load()` 过，或者刚换了配置文件）：主题自己那几项整组清掉再写，
        // 这样"上一次设过、这一次没设"的那几项不会在读回来时复活。
        // 这一支只在启动早期/测试切文件时走，跨进程互不覆盖那条保证本来就无从谈起。
        //
        // **只清主题自己的键**：`Theme/` 这一组里还有 Glass 的材质 / 着色 / 模糊
        // （`Theme/Backdrop`、`Theme/BackdropTint`、`Theme/BackdropBlur`，见 ui/Glass.cpp），
        // 整组删会把用户挑的那三项抹回出厂。
        // 主题自己的键就是这三组加三个主键。以后主题再加键，加在这一支里，别退回整组删。
        for (const char *prefix : {kRolesPrefix, kPartsPrefix, kAccentsPrefix}) {
            // 这三个组只装主题的键（Roles / Parts / Accents），整组删掉连注册表里那层空键一起收。
            QString group = QLatin1String(prefix);
            group.chop(1);  // 去掉尾部那个 '/'
            settings.remove(group);
        }
        settings.remove(QLatin1String(kPresetKey));
        settings.remove(QLatin1String(kTextOpacityKey));
        settings.remove(QLatin1String(kAccentKey));
        // `Theme` 那一组只在确实空了的时候才收（Glass 的键还在就不动它）。
        bool themeGroupHasKeys = false;
        for (const QString &key : settings.allKeys()) {
            if (key.startsWith(QLatin1String(kThemeGroupPrefix))) {
                themeGroupHasKeys = true;
                break;
            }
        }
        if (!themeGroupHasKeys) {
            QString group = QLatin1String(kThemeGroupPrefix);
            group.chop(1);
            settings.remove(group);
        }
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
            settings.setValue(it.key(), it.value());
        }
    } else {
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
            const auto prior = persisted.semantic.constFind(it.key());
            const bool untouched = prior != persisted.semantic.constEnd() && prior.value() == it.value();
            // 比较用户读到的语义值（包括未落盘的默认值）。未改项一律不写，
            // 包括其它窗口刚删除的项：删除也是“恢复默认”的有效修改。
            if (untouched) {
                continue;
            }
            settings.setValue(it.key(), it.value());
        }
        // 上一次设过、这一次没设的项：真要删掉（否则"改回默认"重开一次又复活）。
        // 但只在盘上还是我们当初看到的那一份时才删 —— 别的进程刚改成另一个值就别插手。
        for (auto it = persisted.values.constBegin(); it != persisted.values.constEnd(); ++it) {
            // 未识别的未来版本键不是本窗口编辑过的项，不能顺手清理。
            if (!persisted.semantic.contains(it.key())) {
                continue;
            }
            if (desired.contains(it.key()) || !settings.contains(it.key())) {
                continue;
            }
            if (settings.value(it.key()) == it.value()) {
                settings.remove(it.key());
            }
        }
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        // 写盘失败不推进基线，下一次保存仍会重试本次改动。
        return;
    }
    // 记住本窗口已经提交过的意图；未改项的磁盘值可能属于另一个窗口，不能读回来当成本窗口意图。
    persisted.scope = scope;
    persisted.values = desired;
    persisted.semantic = desired;
}

Spec load() {
    QSettings settings;
    // 记账：盘上**现在**是什么（只收 `Theme/` 这一组）。`save()` 靠它分辨"这一项我改过没有"，
    // 从而只写改过的那几项（见那份结构上面的注释）。原始值保留用于删除前的冲突比较；
    // 未识别的键不进入语义基线，保存时不会删除。
    {
        PersistedThemeValues &persisted = persistedThemeValues();
        persisted.scope = settingsScope(settings);
        persisted.values.clear();
        for (const QString &key : settings.allKeys()) {
            if (key.startsWith(QLatin1String(kThemeGroupPrefix))) {
                persisted.values.insert(key, settings.value(key));
            }
        }
    }
    Spec spec = defaultSpec();
    spec.presetId = settings.value(QLatin1String(kPresetKey), spec.presetId).toString();
    spec.accent = tokens(spec.presetId).primary;
    bool textOpacityOk = false;
    const int textOpacity =
        settings.value(QLatin1String(kTextOpacityKey), spec.textOpacity).toInt(&textOpacityOk);
    if (textOpacityOk) {
        spec.textOpacity = textOpacity;
    }
    // 主题色：没写过就是出厂那一支 —— 它没有"空"这个状态。
    const QColor accent = colorFromString(settings.value(QLatin1String(kAccentKey)).toString());
    if (accent.isValid()) {
        spec.accent = accent;
    }
    for (int i = 0; i < roleCount(); ++i) {
        const Role role = static_cast<Role>(i);
        const QColor color = colorFromString(
            settings.value(roleSettingKey(role)).toString());
        if (color.isValid()) {
            spec.roles.insert(i, color);
        }
    }
    for (int i = 0; i < partCount(); ++i) {
        const Part part = static_cast<Part>(i);
        PartStyle style;
        style.background = colorFromString(
            settings.value(partSettingKey(part, "Background")).toString());
        style.hasBackground = style.background.isValid();
        style.foreground = colorFromString(
            settings.value(partSettingKey(part, "Foreground")).toString());
        style.hasForeground = style.foreground.isValid();
        const QVariant opacity = settings.value(partSettingKey(part, "TextOpacity"));
        if (opacity.isValid()) {
            // 同上：认不出（手改成 "abc"）不许当成 0 —— -1 才是"这一块没调过"。
            bool opacityOk = false;
            const int value = opacity.toInt(&opacityOk);
            if (opacityOk) {
                style.textOpacity = value;
            }
        }
        // 图：路径统一成 `/`（写盘时是 `\`，见 `save()`）。**不认识就不留下这一条**
        // （下一句由 `normalize()` 收尾：空的路径 = 没设过）。
        style.imagePath = QDir::fromNativeSeparators(
            settings.value(partSettingKey(part, "Image")).toString());
        style.hasImage = !style.imagePath.isEmpty();
        if (style.hasImage) {
            ImageFit fit = ImageFit::Cover;
            imageFitFromKey(settings.value(partSettingKey(part, "ImageFit")).toString(), &fit);
            style.fit = fit;
            const QVariant imageOpacity = settings.value(partSettingKey(part, "ImageOpacity"));
            // 没写过这一项（手改注册表删掉了）⇒ 默认 100，不是 0 —— 0 是一张看不见的图，
            // 那个"空白"会被当成坏了。**认不出也一样**（"abc" 的 toInt 也是 0）。
            bool imageOpacityOk = false;
            const int imageOpacityValue = imageOpacity.toInt(&imageOpacityOk);
            style.imageOpacity =
                (imageOpacity.isValid() && imageOpacityOk) ? imageOpacityValue : 100;
            // 「当初挑的那张叫什么」：没有这一项（手改注册表删了）就留空，
            // 界面那一侧会退回落盘那个文件的名字 —— 不许因为少一位就写不出话。
            style.imageName = settings.value(partSettingKey(part, "ImageName")).toString();
        }
        if (style.hasBackground || style.hasForeground || style.textOpacity >= 0 ||
            style.hasImage) {
            spec.parts.insert(i, style);
        }
    }
    spec.danger = colorFromString(settings.value(accentSettingKey("Danger")).toString());
    spec.warning = colorFromString(settings.value(accentSettingKey("Warning")).toString());
    spec.ok = colorFromString(settings.value(accentSettingKey("Ok")).toString());
    spec.errorText = colorFromString(settings.value(accentSettingKey("ErrorText")).toString());
    // 盘上那一份是能手改的、也可能是以后降级回退留下的：**认不出的一律退回出厂**，
    // 不猜也不报错（和 LauncherSettings::load() 那几处同一个路数）。
    const auto clean = normalize(spec);
    persistedThemeValues().semantic = themeSettingValues(clean);
    return clean;
}

// ---------------------------------------------------------------------------
// 导入 / 导出
// ---------------------------------------------------------------------------

namespace {

/// JSON 里那几位字段名。**和 `QSettings` 里那套一一对应**（`preset` / `textOpacity` /
/// `accent` / `roles` / `parts` / `semantic`）—— 两个格式说的是同一件事，看哪一份都行。
constexpr const char *kJsonVersionKey = "wtfmlTheme";
constexpr int kJsonVersion = 1;

QColor colorFromJson(const QJsonValue &value) {
    // 两种写法都收：一个裸字符串（手写时最省事），或者 `{"color": "#…"}`（导出给的形状）。
    if (value.isString()) {
        return colorFromString(value.toString());
    }
    if (value.isObject()) {
        return colorFromString(value.toObject().value(QStringLiteral("color")).toString());
    }
    return QColor();
}

QJsonObject specFields(const Spec &spec) {
    QJsonObject out;
    out.insert(QStringLiteral("preset"), spec.presetId);
    out.insert(QStringLiteral("textOpacity"), spec.textOpacity);
    out.insert(QStringLiteral("accent"), colorToString(spec.accent));
    if (!spec.roles.isEmpty()) {
        QJsonObject roles;
        for (auto it = spec.roles.constBegin(); it != spec.roles.constEnd(); ++it) {
            roles.insert(roleKey(static_cast<Role>(it.key())), colorToString(it.value()));
        }
        out.insert(QStringLiteral("roles"), roles);
    }
    if (!spec.parts.isEmpty()) {
        QJsonObject parts;
        for (auto it = spec.parts.constBegin(); it != spec.parts.constEnd(); ++it) {
            const Part part = static_cast<Part>(it.key());
            const PartStyle &style = it.value();
            QJsonObject one;
            if (style.hasBackground) {
                one.insert(QStringLiteral("background"), colorToString(style.background));
            }
            if (style.hasForeground) {
                one.insert(QStringLiteral("foreground"), colorToString(style.foreground));
            }
            if (style.textOpacity >= 0) {
                one.insert(QStringLiteral("textOpacity"), style.textOpacity);
            }
            if (style.hasImage) {
                // 写的是**导出这台机器上的那一条路径**（理由见 .hpp 里那段：图不在这份
                // 文件里）。换台机器导入之后那几张图找不到，由 `MainWindow` 数出来说一句。
                one.insert(QStringLiteral("image"), style.imagePath);
                one.insert(QStringLiteral("imageName"), style.imageName);
                one.insert(QStringLiteral("imageFit"), imageFitKey(style.fit));
                one.insert(QStringLiteral("imageOpacity"), style.imageOpacity);
            }
            parts.insert(partKey(part), one);
        }
        out.insert(QStringLiteral("parts"), parts);
    }
    QJsonObject semantic;
    if (spec.danger.isValid()) {
        semantic.insert(QStringLiteral("danger"), colorToString(spec.danger));
    }
    if (spec.warning.isValid()) {
        semantic.insert(QStringLiteral("warning"), colorToString(spec.warning));
    }
    if (spec.ok.isValid()) {
        semantic.insert(QStringLiteral("ok"), colorToString(spec.ok));
    }
    if (spec.errorText.isValid()) {
        semantic.insert(QStringLiteral("errorText"), colorToString(spec.errorText));
    }
    if (!semantic.isEmpty()) {
        out.insert(QStringLiteral("semantic"), semantic);
    }
    return out;
}

}  // namespace

QJsonObject specToJson(const Spec &spec) {
    const Spec clean = normalize(spec);
    QJsonObject out = specFields(clean);
    out.insert(QLatin1String(kJsonVersionKey), kJsonVersion);
    return out;
}

bool specFromJson(const QJsonObject &json, Spec *out, QString *error) {
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    // 认得出来是一份主题文件吗 —— 判据是"里面至少有一样我们认识的东西"（有任意一项就收）。
    // 不要求那个版本号标记：手写一份只写 `{"accent": "#ff3d8068"}` 也该能用，而
    // "导入了别的文件"这件事仍然挡得住（那些人一个认识的字段都没有）。
    static const char *const fields[] = {"preset", "textOpacity", "accent", "roles", "parts",
                                         "semantic"};
    bool recognised = false;
    for (const char *field : fields) {
        if (json.contains(QLatin1String(field))) {
            recognised = true;
            break;
        }
    }
    if (!recognised) {
        return fail(QCoreApplication::translate("Theme", "未找到受支持的主题字段：preset、accent、roles、parts 或 semantic。"));
    }

    Spec spec = defaultSpec();  // 从出厂那一份出发：**缺的那几项就是出厂**，不是"没有"
    const QString preset = json.value(QStringLiteral("preset")).toString();
    if (!preset.isEmpty()) {
        spec.presetId = preset;  // 认不出的 id 由 normalize() 退回"跟着系统"
    }
    if (json.contains(QStringLiteral("textOpacity"))) {
        spec.textOpacity = json.value(QStringLiteral("textOpacity")).toInt(spec.textOpacity);
    }
    if (json.contains(QStringLiteral("accent"))) {
        const QColor accent = colorFromJson(json.value(QStringLiteral("accent")));
        if (accent.isValid()) {
            spec.accent = accent;
        }
    }

    const QJsonObject roles = json.value(QStringLiteral("roles")).toObject();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        Role role;
        const QColor color = colorFromJson(it.value());
        // 认不出的名字（不是我们这十来个角色之一）静默丢掉：`normalize()` 也是这个路数，
        // 而且主题文件是能手改的，为一个错字把整份导入拒掉对用户没有好处。
        if (roleFromKey(it.key(), &role) && color.isValid()) {
            spec.roles.insert(static_cast<int>(role), color);
        }
    }

    const QJsonObject parts = json.value(QStringLiteral("parts")).toObject();
    for (auto it = parts.constBegin(); it != parts.constEnd(); ++it) {
        Part part;
        if (!partFromKey(it.key(), &part) || !it.value().isObject()) {
            continue;
        }
        const QJsonObject one = it.value().toObject();
        PartStyle style;
        style.background = colorFromJson(one.value(QStringLiteral("background")));
        style.hasBackground = style.background.isValid();
        style.foreground = colorFromJson(one.value(QStringLiteral("foreground")));
        style.hasForeground = style.foreground.isValid();
        if (one.contains(QStringLiteral("textOpacity"))) {
            style.textOpacity = one.value(QStringLiteral("textOpacity")).toInt(-1);
        }
        style.imagePath = one.value(QStringLiteral("image")).toString();
        style.hasImage = !style.imagePath.isEmpty();
        if (style.hasImage) {
            ImageFit fit = ImageFit::Cover;
            if (!imageFitFromKey(one.value(QStringLiteral("imageFit")).toString(), &fit)) {
                fit = ImageFit::Cover;  // 认不出的摆法退回默认那一种（和 normalize() 一样）
            }
            style.fit = fit;
            style.imageName = one.value(QStringLiteral("imageName")).toString();
            style.imageOpacity = one.contains(QStringLiteral("imageOpacity"))
                                     ? one.value(QStringLiteral("imageOpacity")).toInt(100)
                                     : 100;
        }
        spec.parts.insert(static_cast<int>(part), style);
    }

    const QJsonObject semantic = json.value(QStringLiteral("semantic")).toObject();
    spec.danger = colorFromJson(semantic.value(QStringLiteral("danger")));
    spec.warning = colorFromJson(semantic.value(QStringLiteral("warning")));
    spec.ok = colorFromJson(semantic.value(QStringLiteral("ok")));
    spec.errorText = colorFromJson(semantic.value(QStringLiteral("errorText")));

    if (out != nullptr) {
        *out = normalize(spec); // 收拾干净 —— 和盘上读回来那份走同一道闸
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// follow()
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief "主题一变就重算一遍"这条登记。
 *
 * 它是个事件过滤器而不是"调色板变了"这个信号 —— Qt 没有那样的信号，但**每个控件**都会
 * 收到 `ApplicationPaletteChange`（应用级）和 `PaletteChange`（控件级）。
 */
class PaletteFollower : public QObject {
public:
    PaletteFollower(QWidget *owner, std::function<void()> fn)
        : QObject(owner), m_fn(std::move(fn)) {
        owner->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::ApplicationPaletteChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
            if (!m_running) {
                m_running = true;
                m_fn();
                m_running = false;
            }
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_fn;
    bool m_running = false;
};

}  // namespace

void followImpl(QWidget *owner, std::function<void()> fn) {
    // parent 就是 owner ⇒ owner 没了这一条自己掉，不用谁来收。
    new PaletteFollower(owner, std::move(fn));
}

}  // namespace Theme
