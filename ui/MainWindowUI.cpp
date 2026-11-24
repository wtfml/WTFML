/**
 * @file MainWindowUI.cpp
 * @brief 实现 UI 构建器类，实际创建主窗口的控件、布局并设置窗口属性。
 * @note 完成主窗口所有 UI 元素的初始化与排列，供 MainWindow 构造函数调用。
 */

#include "MainWindowUI.hpp"
#include "MainWindowPageLayout.hpp"

#include "TaskCenterPanel.hpp"
#include "AgentPage.hpp"
#include "Animations.hpp"  // 换页 / 展开折叠 / 上拉面板那几段动效
#include "BrandMark.hpp"
#include "DesignDefaults.hpp"
#include "ElidedLabel.hpp"
#include "Glass.hpp"
#include "GlassPopover.hpp"
#include "InstanceGroupDelegate.hpp"
#include "ItemViewContextMenu.hpp"
#include "ListRowDelegate.hpp"
#include "MemorySlider.hpp"
#include "OverlayHost.hpp"
#include "PopupPlacement.hpp"
#include "Popups.hpp"
#include "SettingsPageParts.hpp"
#include "RoundAvatar.hpp"
#include "SkinModel.hpp"
#include "TaskProgressBar.hpp"
#include "Theme.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "ThemeBackground.hpp"
#include "TitleBar.hpp"
#include "VersionIcons.hpp"
#include "VersionTreeDelegate.hpp"
#include "WindowChrome.hpp"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <algorithm>
#include <functional>

using namespace MainWindowPageLayout;

namespace {

/// 盯着主窗口的 Resize / WindowStateChange，一来就调一次 @p changed（「恢复默认」置灰 + 记窗口大小）。
class WindowSizeWatcher : public QObject {
public:
    WindowSizeWatcher(QWidget *window, std::function<void()> changed) : QObject(window), m_changed(std::move(changed)) {
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange) m_changed();
        return false;
    }

private:
    std::function<void()> m_changed;
};


/// 实例页「文件夹」浮层的底：`Qt::Popup` 顶层窗，走 `Popups::preparePopup()` + `paintSurface()`。
class FolderPopupSurface final : public QFrame {
public:
    explicit FolderPopupSurface(QWidget *owner) : QFrame(owner, Qt::Popup) { Popups::preparePopup(this); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Popups::paintSurface(painter, QRectF(rect()), GlassPopover::kRadius);
    }
};

/// 文件夹浮层那张列表的行底：内缩的圆角块（圆角 8，和菜单行 / 智能体实例下拉同一档）。
/// 行内容是 setItemWidget() 塞的 FolderRowWidget，条目上没字，这里只画底。悬停按那块行控件
/// 认（`underMouse()`）：鼠标落在两行字 / 齿轮上时列表自己的悬停下标收不到移动事件。
class FolderRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const QWidget *row = view != nullptr ? view->indexWidget(index) : nullptr;
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hover = (row != nullptr && row->underMouse()) || (option.state & QStyle::State_MouseOver) != 0;
        if (!selected && !hover) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(selected ? Theme::tokens().selected : Theme::tokens().hover);
        painter->drawRoundedRect(QRectF(option.rect.adjusted(4, 2, -4, -2)), 8, 8);
        painter->restore();
    }
};

/// 启动区里「当前实例 · 名字 · 元信息」那一块（设计稿 .inst）：平时无底，悬停 / 弹层开着时铺一层
/// 场景暗角的雾底（圆角 14），点开实例切换弹层。是 QAbstractButton 不是 QPushButton：后者的
/// sizeHint 只算自己的字，不看里面那套布局，名字一长就被压窄。
class HomeInstBlock final : public QAbstractButton {
public:
    explicit HomeInstBlock(QWidget *parent) : QAbstractButton(parent) {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }
    QSize sizeHint() const override { return layout() != nullptr ? layout()->sizeHint() : QSize(); }
    QSize minimumSizeHint() const override { return layout() != nullptr ? layout()->minimumSize() : QSize(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        // 弹层开着时保持那层底（设计稿 .inst.is-open），弹层那边写 wtfmlOpen 属性。
        const bool hot = property("wtfmlOpen").toBool() || isDown() || underMouse();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (hot) {
            QColor fill = Glass::sceneShown() ? t.heroVeil : t.hover;
            if (Glass::sceneShown()) fill.setAlphaF(isDown() ? 0.56 : 0.46);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(box, 14, 14);
        }
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 13, 13);
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QAbstractButton::enterEvent(event);
        // 名字那一格对鼠标透明：它收了省略号时的全名提示由这一整块代出。进门就同步——
        // 玻璃提示的过滤器在 event() 之前读 toolTip，晚了就落回 Qt 原生提示框。
        if (auto *name = findChild<QLabel *>(QStringLiteral("homeCurrentName"))) setToolTip(name->toolTip());
        update();
    }
    void leaveEvent(QEvent *event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }
};

/// 主页压在风景上的字色：场景画着时是 heroInk / heroInk2（设计稿 --hero-ink / --hero-ink-2），
/// 高对比度（没有场景）回到正文 / 次级色。
QColor heroInk(bool secondary) {
    const Theme::Tokens t = Theme::tokens();
    if (!Glass::sceneShown()) return secondary ? t.secondary : t.text;
    return secondary ? t.heroInk2 : t.heroInk;
}

/// 让 @p w 的字跟着 heroInk 走（换主题 / 场景开关时重取）。
void followHeroInk(QWidget *w, bool secondary) {
    const auto apply = [w, secondary]() {
        QPalette p = w->palette();
        p.setColor(QPalette::WindowText, heroInk(secondary));
        p.setColor(QPalette::ButtonText, heroInk(secondary));
        p.setColor(QPalette::Link, heroInk(secondary));  // 文字按钮（Ghost）的字走 Link
        w->setPalette(p);
    };
    apply();
    Theme::follow(w, apply);
}

/// 主页「每日一言」：细笔画手写体直接压在风景上（亮雪山那块尤其发虚），字号随句长在区间里挑（见
/// MainWindowUI::fitHomeQuoteFont()）。不加底框，
/// 画的时候按这一格背后**此刻实测**的明暗两端兜对比度 ≥ 3:1（和导航轨品牌标同一路数：往 heroInk 挪），
/// 再垫一圈很淡的暗角色光晕把细笔画托出来；背后连 heroInk 都不够 3:1 时光晕加重。中 / 英 / 日三种句子同一套。
class HomeQuoteLabel final : public ElidedLabel {
public:
    using ElidedLabel::ElidedLabel;
    /// 宽度一变就重挑字号（窗口拉伸、左栏让位都走这里）。
    std::function<void()> widthChanged;

protected:
    void resizeEvent(QResizeEvent *event) override {
        ElidedLabel::resizeEvent(event);
        if (widthChanged && event->size().width() != event->oldSize().width()) widthChanged();
    }

    void paintEvent(QPaintEvent *) override {
        const QRect box = contentsRect();
        const QString shown = fontMetrics().elidedText(text(), elideMode(), box.width());
        const int flags = int(alignment()) | Qt::TextSingleLine;
        QColor ink = palette().color(foregroundRole());
        QPainter p(this);
        if (Glass::sceneShown()) {
            const Theme::Tokens t = Theme::tokens();
            QList<QColor> under{t.page};
            if (const Glass::Backdrop *backdrop = Glass::Backdrop::of(this)) under = backdrop->colorsUnder(this);
            const auto worst = [&under](const QColor &c) {
                double low = 21.0;
                for (const QColor &bg : under) low = qMin(low, Glass::contrast(Glass::over(c, bg), bg));
                return low;
            };
            for (int step = 0; step < 5 && worst(ink) < 3.0; ++step) {
                ink = QColor::fromRgbF(ink.redF() + (t.heroInk.redF() - ink.redF()) * 0.25,
                                       ink.greenF() + (t.heroInk.greenF() - ink.greenF()) * 0.25,
                                       ink.blueF() + (t.heroInk.blueF() - ink.blueF()) * 0.25,
                                       ink.alphaF() + (1.0 - ink.alphaF()) * 0.25);
            }
            if (worst(ink) < 3.0) ink = t.heroInk;
            QColor halo = t.heroVeil;
            halo.setAlphaF(worst(ink) < 3.0 ? 0.30 : 0.14);
            p.setPen(halo);
            static const QPoint kRing[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (const QPoint &d : kRing) p.drawText(box.translated(d), flags, shown);
        }
        p.setPen(ink);
        p.drawText(box, flags, shown);
    }
};

/// 把 @p font 换成手写楷体、按像素定号并加字距（设计稿 --hand）。
QFont handPx(QFont font, Theme::HandFont which, int px, qreal spacing) {
    font = Theme::handFont(font, which);
    font.setPixelSize(px);
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    return font;
}

/// 问候和名字之间那一格，按标题字号的比例算（文楷那 3~4 px 字距另算在问候末字后面）。
constexpr qreal kHeroGapEm = 0.4;

/// 名字（或「WTFML」）那一段的字体，按整段判：全是正版名那套字符（ASCII 字母、数字、下划线）
/// 用英文手写体 Dancing Script，放大 1.2 倍（它的 x 高只有文楷七成，同号显小）、字距 0（连笔字）；
/// 含中文就整段文楷。Dancing Script 注册不上也退回文楷。
QFont heroWordFont(const QFont &han, const QString &word) {
    const QString script = Theme::handFamily(Theme::HandFont::Script);
    const bool latin = !word.isEmpty() && std::all_of(word.cbegin(), word.cend(), [](QChar c) {
        return c == QLatin1Char('_') || (c.unicode() < 0x80 && c.isLetterOrNumber());
    });
    if (!latin || script.isEmpty()) return han;
    QFont font = han;
    font.setFamilies(QStringList{script} + han.families());
    font.setPixelSize(qRound(han.pixelSize() * 1.2));
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    return font;
}

/// 主页大标题：单行问候，不带标点。拆成「问候 + 一格空隙 + 名字」两段画，共用文楷的基线；
/// 放不下时只把名字收成「名…」，问候留着。宽度交给布局（Ignored），名字长短不会把左栏抻开。
class HomeGreetingLabel final : public ElidedLabel {
public:
    using ElidedLabel::ElidedLabel;
    void setGreeting(const QString &text, const QString &name) {
        // updateBottomBar() 每次进度刷新都会走到这里：没变就不重画。
        if (text == this->text() && name == m_name) return;
        m_name = name;
        setText(text);
        update();
    }
    QString lead() const { return text().left(split()).trimmed(); }
    QString word() const { return text().mid(split()); }
    QFont wordFont() const { return heroWordFont(font(), word()); }
    QString shownWord() const {
        const int room = contentsRect().width() - QFontMetrics(font()).horizontalAdvance(lead()) - gap();
        const QString shown = QFontMetrics(wordFont()).elidedText(word(), Qt::ElideRight, room);
        // 窄到连「名…」都放不下：照样留「…」，句子形状不散。
        return shown.isEmpty() && !word().isEmpty() ? QStringLiteral("…") : shown;
    }
    QString shown() const { return lead().isEmpty() ? shownWord() : lead() + QLatin1Char(' ') + shownWord(); }
    int shownWidth() const {
        return QFontMetrics(font()).horizontalAdvance(lead()) + gap() +
               QFontMetrics(wordFont()).horizontalAdvance(shownWord());
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        const QRect box = contentsRect();
        QPointF pen(box.left(), box.top() + QFontMetrics(font()).ascent());
        painter.setFont(font());
        painter.drawText(pen, lead());
        pen.rx() += QFontMetrics(font()).horizontalAdvance(lead()) + gap();
        painter.setFont(wordFont());
        painter.drawText(pen, shownWord());
    }

private:
    /// 名字从哪儿起：带名字时就是句尾那个名字；没名字时取最后一个空格之后（「欢迎来到 WTFML」
    /// 的 WTFML）；一个空格都没有就整句当一段（「夜深了」）。
    qsizetype split() const {
        const QString t = text();
        if (!m_name.isEmpty() && t.endsWith(m_name)) return t.size() - m_name.size();
        return t.lastIndexOf(QLatin1Char(' ')) + 1;
    }
    int gap() const { return lead().isEmpty() ? 0 : qRound(font().pixelSize() * kHeroGapEm); }

    QString m_name;
};

/// 主页压在风景上的那几颗"玻璃"按钮：账号胶囊、启动区的切换 / 设置、实例带上的卡片和那颗
/// 悬停才出来的播放钮。底色是场景暗角色（heroVeil）按设计稿的浓度叠上去，不描边（只有快速启动
/// 空槽描虚线）；没有场景时退回中性淡底。
/// 颜色在绘制时现取，换主题不必重挂样式表；尺寸仍归 QSS（min-height / max-height）。
class HeroButton final : public QPushButton {
public:
    /// Capsule：启动卡片右边那颗「取消 / 关闭」——圆头、淡底浓度同 Square（0.40），中间写字。
    enum class Shape { Pill, Square, Tile, Play, Capsule };
    HeroButton(Shape shape, QWidget *parent) : QPushButton(parent), m_shape(shape) {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
    }
    /// 卡片上那颗只在悬停 / 键盘焦点时出现的子控件（设计稿：悬停才出播放钮）。
    void setRevealOnHover(QWidget *child) {
        m_reveal = child;
        syncReveal();
    }
    void setStripWidths(QWidget *identity, int normalWidth, int compactWidth) {
        m_stripIdentity = identity;
        m_normalStripWidth = normalWidth;
        m_compactStripWidth = compactWidth;
        setFixedWidth(normalWidth);
    }
    int stripWidth(bool compact) const { return compact ? m_compactStripWidth : m_normalStripWidth; }
    void setStripCompact(bool compact) {
        if (compact == m_compactStrip) return;
        m_compactStrip = compact;
        m_stripIdentity->setVisible(!compact);
        setFixedWidth(stripWidth(compact));
        syncReveal();
    }
    void setDashed(bool dashed) { m_dashed = dashed; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        const bool scene = Glass::sceneShown();
        const bool hot = isEnabled() && (underMouse() || isDown());
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = m_shape == Shape::Pill || m_shape == Shape::Play || m_shape == Shape::Capsule
                                 ? box.height() / 2
                                 : 14.0;
        if (m_shape == Shape::Play) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? t.primaryPressed : underMouse() ? t.primaryHover : t.primary);
            p.drawEllipse(box);
        } else {
            qreal alpha = m_shape == Shape::Pill                                    ? 0.30
                          : m_shape == Shape::Square || m_shape == Shape::Capsule ? 0.40
                                                                                  : 0.0;
            if (m_shape == Shape::Tile && (hot || hasFocus())) alpha = 0.42;
            if (hot) alpha += isDown() ? 0.18 : 0.10;
            if (alpha > 0 || m_dashed) {
                QColor fill = scene ? t.heroVeil : t.subtle;
                if (scene) fill.setAlphaF(qMin(1.0, alpha));
                else if (hot) fill = t.hover;
                p.setBrush(alpha > 0 ? QBrush(fill) : Qt::NoBrush);
                QPen edge(m_dashed ? heroInk(true) : (scene ? t.glassEdge : t.stroke));
                if (m_dashed) {
                    QColor c = edge.color();
                    c.setAlphaF(c.alphaF() * 0.45);
                    edge.setColor(c);
                    edge.setStyle(Qt::DashLine);
                }
                edge.setWidthF(1.0);
                // 只有快速启动空槽描虚线；其余只留底色（「去硬描边」：底色差就分得出"一颗"）。
                const bool stroked = m_dashed;
                p.setPen(stroked ? edge : QPen(Qt::NoPen));
                p.drawRoundedRect(box, radius, radius);
            }
        }
        if (!icon().isNull()) {
            const QSize s = iconSize();
            const QRect r(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
            icon().paint(&p, r);
        }
        if (m_shape == Shape::Capsule) {
            p.setPen(heroInk(false));
            p.setFont(font());
            p.drawText(rect(), Qt::AlignCenter, text());
        }
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
            p.drawRoundedRect(r, radius - 1, radius - 1);
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QPushButton::enterEvent(event);
        syncReveal();
    }
    void leaveEvent(QEvent *event) override {
        QPushButton::leaveEvent(event);
        syncReveal();
    }
    void focusInEvent(QFocusEvent *event) override {
        QPushButton::focusInEvent(event);
        syncReveal();
    }
    void focusOutEvent(QFocusEvent *event) override {
        QPushButton::focusOutEvent(event);
        syncReveal();
    }

private:
    void syncReveal() {
        if (m_reveal != nullptr) m_reveal->setVisible(m_compactStrip || underMouse() || hasFocus());
        update();
    }
    Shape m_shape;
    QPointer<QWidget> m_reveal;
    QWidget *m_stripIdentity = nullptr;
    int m_normalStripWidth = 0;
    int m_compactStripWidth = 0;
    bool m_compactStrip = false;
    bool m_dashed = false;
};

/// 启动卡片失败态的「查看详情」：一段能点的主题色文字，无底无框，悬停多一道下划线。
/// 颜色取 primary：accentInk 在三套主题上都和 heroInk 太近，看不出哪个能点；浅色主题压深一档保对比。
class HeroLink final : public QPushButton {
public:
    explicit HeroLink(QWidget *parent) : QPushButton(parent) {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont f = font();
        f.setUnderline(underMouse());
        p.setFont(f);
        p.setPen(Theme::isDark() ? Theme::tokens().primary : Theme::tokens().primary.darker(130));
        p.drawText(rect(), Qt::AlignCenter, text());
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QPushButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override {
        QPushButton::leaveEvent(event);
        update();
    }
};

/// 启动卡片那一行（规格：.tongxi 样张 card-spec.md §2）：
/// 「手写 W 76×52 | 22 | 说明 / 进度两行（加载 300 宽、失败 236 宽）| … | 胶囊 84×36」，整行恒宽 512、高 52，
/// 胶囊（取消 / 关闭）三种状态下位置、尺寸不动；失败态「查看详情」落在胶囊左边、进度条尾巴原本那段。
/// 两行网格：第一行 y 4–24（说明），第二行 y 24–44（进度条 / 停在…），右边操作的中线都压在第二行中线 y 34。
/// 整行尺寸固定，直接按坐标摆；换状态只换字和显隐，不重排。
class HomeLaunchRow final : public QWidget {
public:
    static constexpr int kWidth = 512, kHeight = 52;
    static constexpr int kMarkW = 76, kColumnX = 76 + 22, kColumnW = 300, kFailColumnW = 236;
    static constexpr int kLinkX = kColumnX + kFailColumnW + 20, kLinkW = 60;
    static constexpr int kPillX = kColumnX + kColumnW + 30, kPillW = 84, kPillH = 36;
    static constexpr int kLine1Y = 4, kLine2Y = 24, kLineH = 20, kFailIndent = 22;

    explicit HomeLaunchRow(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("homeLaunchCard"));
        setFixedSize(kWidth, kHeight);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        mark = new LaunchMark(this);
        mark->setObjectName(QStringLiteral("homeLaunchMark"));
        mark->setGeometry(0, 0, kMarkW, kHeight);
        const auto label = [this](const QString &name, int px, QFont::Weight weight, bool secondary) {
            auto *l = new ElidedLabel(this);
            l->setObjectName(name);
            l->setMinimumWidth(1);
            QFont f = l->font();
            f.setPixelSize(px);
            f.setWeight(weight);
            l->setFont(f);
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
            followHeroInk(l, secondary);
            return l;
        };
        step = label(QStringLiteral("homeLaunchStep"), 14, QFont::Medium, false);
        count = label(QStringLiteral("homeLaunchCount"), 13, QFont::Normal, true);
        count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        reason = label(QStringLiteral("homeLaunchReason"), 14, QFont::Medium, false);
        stopAt = label(QStringLiteral("homeLaunchStopAt"), 13, QFont::Normal, true);
        warn = new QLabel(this);
        warn->setObjectName(QStringLiteral("homeLaunchWarn"));
        warn->setAttribute(Qt::WA_TransparentForMouseEvents);
        warn->setScaledContents(true);
        const auto applyWarn = [this]() {
            const Theme::Tokens t = Theme::tokens();
            warn->setPixmap(VersionIcons::line(QStringLiteral("warn"), 16, Theme::textTone(t.dangerInk, t.danger), 1.8));
        };
        applyWarn();
        Theme::follow(warn, applyWarn);
        bar = new TaskProgressBar(this);
        bar->setObjectName(QStringLiteral("homeLaunchBar"));
        bar->setFullUsesAccent(true);  // 成功走满仍是主题色，和 W、整张卡片一致（成功 / 失败另由眉题与 W 区分）
        details = new HeroLink(this);
        details->setObjectName(QStringLiteral("homeLaunchDetails"));
        details->setText(QCoreApplication::translate("MainWindowUI", "查看详情"));
        QFont linkFont = details->font();
        linkFont.setPixelSize(14);
        linkFont.setWeight(QFont::DemiBold);
        details->setFont(linkFont);
        details->setGeometry(kLinkX, kHeight - kPillH, kLinkW, kPillH);
        pill = new HeroButton(HeroButton::Shape::Capsule, this);
        pill->setObjectName(QStringLiteral("homeLaunchPill"));
        QFont pillFont = pill->font();
        pillFont.setPixelSize(14);
        pillFont.setWeight(QFont::Medium);
        pill->setFont(pillFont);
        pill->setGeometry(kPillX, kHeight - kPillH, kPillW, kPillH);  // 底边贴行底（同原主按钮），中线 = 第二行中线
        place(false);
    }

    /// 按状态摆字、显隐（胶囊永远在同一处）。
    void apply(const MainWindowUI::LaunchCard &card, const QString &instanceName) {
        using Phase = MainWindowUI::LaunchCard::Phase;
        const bool failed = card.phase == Phase::Failed;
        place(failed);
        step->setVisible(!failed);
        bar->setVisible(!failed);
        count->setVisible(!failed && card.phase == Phase::Running && card.total > 0);
        warn->setVisible(failed);
        reason->setVisible(failed);
        stopAt->setVisible(failed);
        details->setVisible(failed);
        pill->setVisible(card.phase != Phase::Succeeded);  // 成功态不给操作，只藏不挪
        pill->setText(failed ? QCoreApplication::translate("MainWindowUI", "关闭")
                             : QCoreApplication::translate("MainWindowUI", "取消"));
        pill->setAccessibleName(pill->text());
        details->setAccessibleName(details->text());
        QString line1;
        if (failed) {
            reason->setText(card.reason);
            stopAt->setText(QCoreApplication::translate("MainWindowUI", "停在「%1」").arg(card.stage));
            line1 = card.reason;
            mark->finish(false);
        } else if (card.phase == Phase::Succeeded) {
            step->setText(QCoreApplication::translate("MainWindowUI", "游戏窗口已打开"));
            bar->setRange(0, 1);
            bar->setValue(1);
            line1 = step->text();
            mark->finish(true);
        } else {
            step->setText(card.stage);
            line1 = card.stage;
            if (card.total > 0) {
                const qint64 done = qBound<qint64>(0, card.done, card.total);
                count->setText(card.bytes ? QStringLiteral("%1 / %2 MB")
                                                .arg(qRound64(done / 1048576.0))
                                                .arg(qMax<qint64>(1, qRound64(card.total / 1048576.0)))
                                          : QStringLiteral("%1 / %2").arg(done).arg(card.total));
                bar->setRange(0, 1000);
                bar->setValue(int(done * 1000 / card.total));
                line1 += QLatin1Char(' ') + count->text();
            } else {
                bar->setRange(0, 0);
            }
            mark->startWriting();
        }
        const QString eyebrow = failed ? QCoreApplication::translate("MainWindowUI", "启动失败")
                                : card.phase == Phase::Succeeded ? QCoreApplication::translate("MainWindowUI", "游戏已启动")
                                                                 : QCoreApplication::translate("MainWindowUI", "正在启动游戏");
        setAccessibleName(QCoreApplication::translate("MainWindowUI", "%1 %2：%3").arg(eyebrow, instanceName, line1));
    }

    LaunchMark *mark = nullptr;
    ElidedLabel *step = nullptr, *count = nullptr, *reason = nullptr, *stopAt = nullptr;
    QLabel *warn = nullptr;
    TaskProgressBar *bar = nullptr;
    HeroLink *details = nullptr;
    HeroButton *pill = nullptr;

private:
    void place(bool failed) {
        const int w = failed ? kFailColumnW : kColumnW;
        step->setGeometry(kColumnX, kLine1Y, w, kLineH);
        count->setGeometry(kColumnX, kLine1Y, w, kLineH);
        const int barH = 4;
        bar->setGeometry(kColumnX, kLine2Y + (kLineH - barH) / 2, w, barH);
        warn->setGeometry(kColumnX, kLine1Y + 2, 16, 16);
        reason->setGeometry(kColumnX + kFailIndent, kLine1Y, w - kFailIndent, kLineH);
        stopAt->setGeometry(kColumnX + kFailIndent, kLine2Y, w - kFailIndent, kLineH);
    }
};

/// 主页右上那颗账号胶囊里的圆头像（32，和胶囊两头的半圆同心）。没登录（pixmap 空）画淡底 + 人形线条。
class HomeAccountAvatar final : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box(rect());
        if (!pixmap().isNull()) {
            RoundAvatar::paint(p, box, pixmap().toImage());
            return;
        }
        const Theme::Tokens t = Theme::tokens();
        p.setPen(Qt::NoPen);
        p.setBrush(t.subtle);
        p.drawEllipse(box);
        const qreal side = box.width() * 0.5;
        VersionIcons::paintLine(p, QStringLiteral("user"),
                                QRectF(box.center() - QPointF(side / 2, side / 2), QSizeF(side, side)), heroInk(true));
    }
};

/// 主页右上的账号胶囊：圆头像 + 名字 + 一行很淡的小状态，点开账号弹层。
/// 一直铺一层场景暗角色（heroVeil）的淡底、不描边 —— 风景再花，名字也压得住（浓度见
/// `MainWindowUI::homeAccountPillVeilAlpha()`）；悬停 / 按下 / 弹层开着再浓一档。
/// 宽度随内容走，封顶 kHomeAccountPillWidth（QPushButton 的 sizeHint 不看布局，这里改成看）。
class HomeAccountPill final : public QPushButton {
public:
    explicit HomeAccountPill(QWidget *parent) : QPushButton(parent) {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override {
        const QSize s = layout() != nullptr ? layout()->sizeHint() : QSize();
        return QSize(qMin(s.width(), kHomeAccountPillWidth), kHomeAccountPillHeight);
    }
    QSize minimumSizeHint() const override { return QSize(kHomeAccountPillHeight, kHomeAccountPillHeight); }

protected:
    bool event(QEvent *e) override {
        // 名字换了 => 里面那套布局重算 => 让外面重新问一遍 sizeHint（宽度跟着名字走）。
        if (e->type() == QEvent::LayoutRequest) updateGeometry();
        return QPushButton::event(e);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        const bool hot = property("wtfmlOpen").toBool() || isDown() || underMouse();
        const QRectF box(rect());
        const qreal radius = box.height() / 2;
        QColor fill;
        if (Glass::sceneShown()) {
            fill = t.heroVeil;
            fill.setAlphaF(qMin(1.0, MainWindowUI::homeAccountPillVeilAlpha(Theme::isDark()) +
                                         (isDown() ? 0.18 : hot ? 0.10 : 0.0)));
        } else {
            fill = hot ? t.hover : t.subtle;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(box, radius, radius);
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QPushButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override {
        QPushButton::leaveEvent(event);
        update();
    }
};

/// 实例带上的「最近 / 收藏」页签（设计稿 r2 .htabs span）：系统字 14，28 高的小胶囊；
/// 平时 heroInk2，选中铺弱强调底、字用强调色。
class HomeStripTab final : public QPushButton {
public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(text()) + 24, 28);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        if (isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.selected);
            p.drawRoundedRect(QRectF(rect()), 9, 9);
        }
        p.setPen(isChecked() ? Theme::textTone(t.accentInk, t.deep) : heroInk(!underMouse()));
        p.setFont(font());
        p.drawText(rect(), Qt::AlignCenter, text());
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
    }
};

/// 页签那一排里的「收起 / 展开」箭头：直接画在风景上，平时和悬停都不铺底，只在悬停时墨色从
/// heroInk2 换成 heroInk（和 HomeStripTab 一致）。展开时箭头朝上（点了往上收），收起时朝下；
/// 开合时箭头随折叠动画翻过去。
class HomeStripToggle final : public QPushButton {
public:
    explicit HomeStripToggle(QWidget *parent) : QPushButton(parent) {
        m_turn = new QVariantAnimation(this);
        m_turn->setDuration(200);
        m_turn->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_turn, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_angle = v.toReal();
            update();
        });
    }
    void setCollapsed(bool collapsed, bool animate) {
        const qreal target = collapsed ? 180.0 : 0.0;
        m_turn->stop();
        if (animate && Anim::enabled() && isVisible()) {
            m_turn->setStartValue(m_angle);
            m_turn->setEndValue(target);
            m_turn->start();
        } else {
            m_angle = target;
            update();
        }
    }
    QSize sizeHint() const override { return QSize(28, 28); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(heroInk(!underMouse()));
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.translate(QRectF(rect()).center());
        p.rotate(m_angle);
        const QPointF chevron[] = {{-5.0, 2.5}, {0.0, -2.5}, {5.0, 2.5}};
        p.drawPolyline(chevron, 3);
        p.resetTransform();
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QPushButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override {
        QPushButton::leaveEvent(event);
        update();
    }

private:
    QVariantAnimation *m_turn = nullptr;
    qreal m_angle = 0.0;
};

/// 实例带那一排卡片的"取景框"：卡片按自己的宽度一张挨一张排，排不下就从右边裁掉，
/// 最后 120px 渐隐（设计稿 `.cards { overflow: hidden; mask-image: … }`）。
/// 里面那一排是个普通的 QHBoxLayout（`row()`），重建卡片的代码照旧往里塞。
///
/// 卡片**不摊开、不居中**：每张卡按自己名字的宽度定尺寸（`kHomeTileWidth` ~
/// `kHomeTileMaxWidth`，见 `rebuildRow()` 里那段），从这一排的左缘一张挨一张摆，
/// 剩下的宽度就空在右边 —— 和设计稿 `.cards` 的"一排卡片、超出的裁掉"是一回事。
class HomeStripRow final : public QWidget {
public:
    explicit HomeStripRow(QWidget *parent) : QWidget(parent), m_content(new QWidget(this)) {
        m_row = new QHBoxLayout(m_content);
        // 左边让 4 px：卡片自己的内边距是 8，页签那一排的外边距是 12 —— 补上这 4 px，
        // 第一张卡的封面左缘就和上面「最近 / 收藏」页签的左缘落在同一条线上。
        m_row->setContentsMargins(4, 0, 0, 0);
        m_row->setSpacing(6);
        m_content->installEventFilter(this);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    QHBoxLayout *row() const { return m_row; }
    QSize sizeHint() const override { return QSize(0, m_content->sizeHint().height()); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        relayout();
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_content &&
            (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
            // 卡片是重建的（`rebuildRow()` 里清空再塞），那一排的自然宽会变：铺满 / 让位都
            // 得按新的数目重算。只在几何真的变了的时候动，免得 LayoutRequest 自己喂自己。
            relayout();
            if (event->type() == QEvent::Resize && m_content->height() != height()) updateGeometry();
            syncFade();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void relayout() {
        QList<HeroButton *> tiles;
        int normalWidth = m_row->contentsMargins().left() + m_row->contentsMargins().right();
        for (int i = 0; i < m_row->count(); ++i) {
            if (auto *tile = dynamic_cast<HeroButton *>(m_row->itemAt(i)->widget())) {
                if (!tiles.isEmpty()) normalWidth += m_row->spacing();
                tiles.append(tile);
                normalWidth += tile->stripWidth(false);
            }
        }
        qsizetype normalCount = tiles.size();
        // 默认三张沿用原版式。更多卡片按优先顺序保留完整样式。
        while (tiles.size() > 3 && normalCount > 0 && normalWidth > width()) {
            const auto *tile = tiles.at(--normalCount);
            normalWidth -= tile->stripWidth(false) - tile->stripWidth(true);
        }
        for (qsizetype i = 0; i < tiles.size(); ++i) tiles.at(i)->setStripCompact(i >= normalCount);
        const QSize hint = m_content->sizeHint();
        const int width = qMax(hint.width(), 0);
        const int height = qMax(hint.height(), this->height());
        if (m_content->geometry() != QRect(0, 0, width, height)) {
            m_content->setGeometry(0, 0, width, height);
        }
        syncFade();
    }
    void syncFade() {
        const bool overflow = m_content->width() > width() && width() > 160;
        auto *effect = qobject_cast<QGraphicsOpacityEffect *>(graphicsEffect());
        if (!overflow) {
            if (effect != nullptr) setGraphicsEffect(nullptr);
            return;
        }
        if (effect == nullptr) {
            effect = new QGraphicsOpacityEffect(this);
            effect->setOpacity(1.0);
            setGraphicsEffect(effect);
        }
        QLinearGradient mask(0, 0, width(), 0);
        mask.setColorAt(0.0, Qt::black);
        mask.setColorAt(qMax(0.0, 1.0 - 120.0 / width()), Qt::black);
        mask.setColorAt(1.0, Qt::transparent);
        effect->setOpacityMask(mask);
    }
    QWidget *m_content;
    QHBoxLayout *m_row;
};

/// 只在窗口尺寸变化时跑一段函数（主页窄窗那一档：标题收一号、立绘缩小）。
class ResizeHook final : public QObject {
public:
    ResizeHook(QWidget *watched, std::function<void()> fn) : QObject(watched), m_fn(std::move(fn)) {
        watched->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) m_fn();
        return false;
    }

private:
    std::function<void()> m_fn;
};

/// 实例带卡片下面那行小字（设计稿 .cv）："Minecraft 1.21.4 / Fabric 0.16.9" → "Fabric 1.21.4"。
QString homeTileSubtitle(const QString &detail) {
    const QString line = detail.section(QLatin1Char('\n'), 0, 0);
    const QString game = line.section(QStringLiteral(" / "), 0, 0).remove(QStringLiteral("Minecraft ")).trimmed();
    const QString loader = line.section(QStringLiteral(" / "), 1, 1).section(QLatin1Char(' '), 0, 0).trimmed();
    if (game.isEmpty()) return line;
    return loader.isEmpty() ? game : loader + QLatin1Char(' ') + game;
}

/// 启动区那一行元信息（设计稿 .meta）：加载器 · Minecraft 版本 · 上次游玩。
/// 启动卡片出来时末项换成 @p tail（登录方式，card-spec §1）。
QString homeLaunchMeta(const MainWindowUI::HomeQuickItem &item, const QString &tail = {}) {
    const QString versionLine = item.detail.section(QLatin1Char('\n'), 0, 0);
    const QString activityLine = item.detail.section(QLatin1Char('\n'), 1, 1);
    QStringList parts;
    const QString game = versionLine.section(QStringLiteral(" / "), 0, 0).trimmed();
    const QString loader = versionLine.section(QStringLiteral(" / "), 1).trimmed();
    if (!loader.isEmpty()) parts << loader;
    if (!game.isEmpty()) parts << game;
    const QString when = tail.isEmpty() ? activityLine.section(QStringLiteral(" / "), 0, 0).trimmed() : tail;
    if (!when.isEmpty()) parts << when;
    return parts.join(QStringLiteral("  ·  "));
}

/// 实例带卡片左边那块 48×48 封面：圆角暗底 + 居中的实例图标。
/// 没挑图标的实例：按名字稳定配色的等距方块（设计稿 cover）。
QPixmap homeTileCover(const QIcon &icon, const QString &name, int side) {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    QPixmap cover(QSize(side, side) * dpr);
    cover.setDevicePixelRatio(dpr);
    cover.fill(Qt::transparent);
    QPainter p(&cover);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme::Tokens t = Theme::tokens();
    QColor fill = Glass::sceneShown() ? t.heroVeil : t.subtle;
    if (Glass::sceneShown()) fill.setAlphaF(0.40);
    p.setPen(Qt::NoPen);  // 不描边（「去硬描边」）：淡底块就是形状
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(0.5, 0.5, side - 1, side - 1), 12, 12);
    const int inner = side * 7 / 12;
    const QPixmap mark = icon.isNull() ? VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(name), inner)
                                      : icon.pixmap(inner, inner);
    p.drawPixmap(QRect((side - inner) / 2, (side - inner) / 2, inner, inner), mark);
    return cover;
}
} // namespace

MainWindowUI::MainWindowUI(QMainWindow *parent) { setupUi(parent); }

const MainWindowUI::ThemeImageRow &MainWindowUI::getThemeImageRow(int index) const {
    // 越界**不崩**：那几行的下标是调用方自己数出来的（一共两行），而这一层在
    // 构造之后才被问到 —— 给一个空行（指针全是 nullptr）比断言更好，调用方本来就要判空
    // （见头文件那句"调用方自己判空"）。
    static const ThemeImageRow kEmpty;
    if (index < 0 || index >= m_themeImageRows.size()) {
        return kEmpty;
    }
    return m_themeImageRows.at(index);
}







QString MainWindowUI::runningDurationText(qint64 startedAtMs, qint64 nowMs) {
    // 负数（时钟被往回调过、或者 startedAtMs 是个没写过的 0）当"刚开始"：这一行是个说明，
    // 不该出现"已运行 -3 分钟"这种算术漏出来的话。
    const qint64 seconds = qMax<qint64>(0, (nowMs - startedAtMs) / 1000);
    if (seconds < 60) {
        return QCoreApplication::translate("MainWindowUI", "已运行不到 1 分钟");
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QCoreApplication::translate("MainWindowUI", "已运行 %1 分钟").arg(minutes);
    }
    const qint64 hours = minutes / 60;
    if (hours < 24) {
        const qint64 rest = minutes % 60;
        // 整点就不缀那半句："已运行 2 小时 0 分钟"读起来是台机器写的。
        return rest == 0 ? QCoreApplication::translate("MainWindowUI", "已运行 %1 小时").arg(hours)
                         : QCoreApplication::translate("MainWindowUI", "已运行 %1 小时 %2 分钟")
                               .arg(hours)
                               .arg(rest);
    }
    const qint64 days = hours / 24;
    // 再往上一档（"N 个月"）没有意义：一个实例连着跑好几天已经是要不要收掉的问题了，
    // 具体几天几小时够用。
    return QCoreApplication::translate("MainWindowUI", "已运行 %1 天 %2 小时")
        .arg(days)
        .arg(hours % 24);
}

bool MainWindowUI::isHomeTabCurrent() const {
    // 主页恒定是第一页（下面 addTab 的顺序就是契约），所以比下标就够，
    // 不用再存一份页面指针。
    return tabWidget != nullptr && tabWidget->currentIndex() == homeTabIndex();
}

/// 切选项卡的公共那一段：顶上导航和导航轨底部的「工具箱」都走它（唯一出处）。
/// 返回 -1 = 被导航守卫拦下；0 = 已翻页；1 = 点的就是当前这一颗（已收覆盖层）。
int MainWindowUI::applyTabRequest(int index) {
    if (navigationGuard && !navigationGuard()) return -1;
    const int sourceIndex = tabWidget->currentIndex();
    const bool reselected = index == sourceIndex;
    // 点上方那四大钮时要**收起任务中心**，不能卡在别的页上没反应。
    // 任务中心是**整页覆盖**那一档：它在自己的选项卡上是摊开的，切到别的选项卡上
    // 只是"收着"（OverlayHost::parked()），而那一位**还占着覆盖层**。于是：
    // · 在别的选项卡上点导航 —— `dismiss()` 对"收着"的页什么都不做（它按设计不
    // 替用户收看不见的页），任务中心原样占着那一位；
    // · 再回到原来那一页 —— 它又摊开，看着像"哪颗都没反应、页面卡住了"。
    // 所以点导航这一下先把它**收掉**（evict 连"收着"那一档一起收），再照常翻页 /
    // dismiss。只认任务中心：别的覆盖页（资源详情之类）仍按同一条规矩停放、切回来还在。
    // NOTE: `pageOpen()` 那一条是"**最上面**那一层是页"的意思 —— 页上面压着一句提示
    // 的时候不在这儿收（那句话是要人回答的，不能因为切个页就替他答"取消"）。
    if (taskCenterPanel() != nullptr && overlayHost != nullptr && overlayHost->promptOpen() &&
        overlayHost->promptFullSize()) {
        overlayHost->dismiss();
    } else if (taskCenterPanel() != nullptr && overlayHost != nullptr && overlayHost->pageOpen()) {
        overlayHost->evict();
    }
    // 顶端四个按钮点下去要**自动到达对应选项卡的首页**（比如正开着 sodium 的下载页，
    // 点启动选项卡就切过去、下载那一层自动收起）；在下载页点顶上那个「下载」（不是
    // 下载确认键）也回到下载页首页，四个都适用。于是这一颗有两种意思：
    //
    // ① 点的**不是**现在这一颗 ⇒ 翻页。切走之后那页不会丢，只是"收着"（见
    // OverlayHost.hpp 那段），切回来还是原来那个界面、连滚到哪儿都在；
    // ② 点的**就是**现在这一颗 ⇒ "回到这个功能的首页"：把上面那一层收掉 —— 页面那一档
    // 走"返回箭头"那一发（收页要经过页面自己的 reject，理由见 dismiss()）。
    if (reselected) {
        if (overlayHost != nullptr) {
            overlayHost->dismiss();
        }
    } else {
        tabWidget->setCurrentIndex(index);
    }
    return reselected ? 1 : 0;
}

void MainWindowUI::openToolboxTab(const QString &key) {
    const int result = applyTabRequest(toolboxTabIndex());
    if (result < 0) return;
    // 空串 = 停在上次那一层；已经在工具箱页上再点一次 = 回目录。
    if (m_toolboxToolHandler) {
        m_toolboxToolHandler(!key.isEmpty() ? key : (result == 1 ? QStringLiteral("catalog") : QString()));
    }
}

void MainWindowUI::setupUi(QMainWindow *parent) {
    parent->setWindowTitle(QStringLiteral("WTF Minecraft Launcher"));
    // 默认 1024×620（MainWindow 随后换成上次记住的大小）；屏幕放不下按可用区缩，不缩到最小值以下。
    parent->resize(fittedWindowSize(parent, QSize(kMainWindowDefaultWidth, kMainWindowDefaultHeight)));
    parent->setMinimumSize(kMainWindowMinWidth, kMainWindowMinHeight);

    // 不给最大化：去掉这个标志，系统就没有 WS_MAXIMIZEBOX，Win+↑、拖到屏顶、双击标题栏都不会最大化
    // （左右半屏贴靠不受影响）。窗口大小只靠拖边缩放，最小尺寸见 kMainWindowMinWidth。
    parent->setWindowFlags(parent->windowFlags() & ~Qt::WindowMaximizeButtonHint);

    // 中央控件就是整窗那一层底（系统背板 / 风景预模糊底 / 纯色，见 ui/Glass.hpp）。
    // 去套娃：底只有这一层，导航轨和页面都直接铺在它上面，不再各自垫面板。
    auto *backdrop = new Glass::Backdrop(parent);
    QWidget *centralWidget = backdrop;
    parent->setCentralWidget(centralWidget);

    // 左 = 导航轨；右 = 顶上一条透明的窗口按钮条 + 页面。
    auto *shellLayout = new QHBoxLayout(centralWidget);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);
    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ================= 顶上那条自己画的标题栏 =================
    // 系统那条白色标题栏由 WindowChrome 去掉（客户区铺满整窗，见它的文件头），
    // 顶上这一行就是替它的：左边草方块 + 启动器名字（默认是 `TitleBar::defaultBrandText()`
    // 那五个字母，设置页「个性化」栏的「外观」里可以改），
    // 然后是**五个功能的导航按钮**（下面 addTab 完之后 setNavItems() 灌进去），
    // 右边最小化与关闭。第一行就是它、第二行才是标签页 —— 顺序由 addWidget 的先后决定。
    //
    // NOTE: 覆盖层（OverlayHost）挂在 tabWidget 上、条在 tabWidget 上面那一行，所以开设置页
    // 时这条**不会被盖住** —— 和"底下那一条不用覆盖"是同一条规矩；
    // "顶上四个选项卡也不覆盖"由"四个功能在条上、条不被覆盖"接手。
    bar = new TitleBar(centralWidget);
    shellLayout->addWidget(bar);
    shellLayout->addLayout(mainLayout, 1);
    bar->captionStrip()->setParent(centralWidget);
    mainLayout->addWidget(bar->captionStrip());
    // 条只发信号、不自己动手（见 TitleBar.hpp）：谁接这两下是这一层的事。
    // NOTE: 这个类**不是 QObject**（没有 tr()、没有自己的信号槽），所以得写全 `QObject::connect`。
    QObject::connect(bar, &TitleBar::minimizeRequested, parent, &QMainWindow::showMinimized);
    QObject::connect(bar, &TitleBar::closeRequested, parent, &QMainWindow::close);

    // ================= 选项卡：主页 / 管理 / 下载 / 设置 / 智能体 =================
    // 四页各自只放自己那摊控件，都排成左右两栏：左栏是要操作的那点东西
    // （选版本、按钮），右栏是列表或一整摊设置项。份数按页不同，见 leftColumnIsWider()。
    // 注意"选版本"这件事现在分在两个地方：「已安装版本」那个**下拉框**在主页、只管启动；
    // 下载页上没有下拉框，是**一棵版本树**（点分类展开、点一个版本号进安装页装它）。
    // 两边本来就是互不相干的数据源（一个是盘上装好的、一个是清单里能下的）。
    tabWidget = new QTabWidget(centralWidget);
    tabWidget->setObjectName(tabWidgetName());
    // 退出默认换页动效：顶层选项卡自己接 pageAppear（整页停放时不盖幕布，见下面 currentChanged 那段）。
    DesignDefaults::optOut(tabWidget);
    // QTabWidget 默认把所有页面里最大的 minimumSizeHint 当成自己的下限。下载页那棵
    // 固定行数的版本树会把它顶到 568px，再加标题栏和底栏后整窗被强制到 710px；即使
    // 当前停在主页也缩不下去，只能在上半部留下大块空白。竖直方向忽略这个跨页提示，
    // 让当前窗口尺寸说了算；各页已有自己的滚动区域，空间不足时由页面内部处理。
    tabWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);

    // ---------- 四页：主页 / 实例 / 下载 / 设置 ----------
    //
    // 四个页面都是 `ThemeBackground`：其他页面的背景也能换图（`Theme::Part::PageBackground`）。
    // 没设图时它画的就是这一页的底色，一个像素都不差（见 ui/ThemeBackground.hpp 的文件头）。
    // 每一页都在自己的 `setup*Page()` 里搭（见下面那几个函数），这里只按顺序摆出来。
    QWidget *homeTab = setupHomePage();
    QWidget *instanceTab = setupInstancePage();
    QWidget *downloadTab = setupDownloadPage();
    QWidget *settingsTab = setupSettingsTab();
    agentPage = new AgentPage(tabWidget);

    // NOTE: 顺序就是契约：homeTabIndex() == 0 靠的就是"主页第一个 addTab"。
    // NOTE: 这五个是 **要翻的**（`QCoreApplication::translate` 的上下文是 "MainWindowUI"，
    // 译文在 `assets/i18n/wtfml_en.ts`）。标签文字同时是顶上导航条那五个按钮的字
    // （下面 `bar->setNavItems(tabText…)` 抄的就是这一份），所以一处翻译两处生效。
    tabWidget->addTab(homeTab, QCoreApplication::translate("MainWindowUI", "主页"));
    tabWidget->addTab(instanceTab, QCoreApplication::translate("MainWindowUI", "管理"));
    tabWidget->addTab(downloadTab, QCoreApplication::translate("MainWindowUI", "下载"));
    tabWidget->addTab(settingsTab, QCoreApplication::translate("MainWindowUI", "设置"));
    // D1：智能体**放最后**；启动默认仍停在主页（下面 setCurrentIndex(homeTabIndex())）。
    tabWidget->addTab(agentPage, QCoreApplication::translate("MainWindowUI", "智能体"));
    // 工具箱：第六个选项卡，**不上顶上那条导航**（入口是导航轨底部那颗「工具箱」，见 setupBottomBar）。
    // 页本身由 ToolboxController 首次打开时挂进来（避免没用过工具箱的人白搭九片控件）。
    {
        auto *toolboxHost = new ThemeBackground(Theme::Part::PageBackground, tabWidget);
        toolboxHost->setObjectName(toolboxTabName());
        auto *toolboxLayout = new QVBoxLayout(toolboxHost);
        toolboxLayout->setContentsMargins(0, 0, 0, 0);
        toolboxLayout->setSpacing(0);
        tabWidget->addTab(toolboxHost, QCoreApplication::translate("MainWindowUI", "工具箱"));
    }
    tabWidget->setCurrentIndex(homeTabIndex());
    mainLayout->addWidget(tabWidget, 1);

    // ================= 那四个选项卡放在顶上那一条上 =================
    // 四个选项卡放在最顶上，通过顶端按钮打开四个功能。
    //
    // 做法是"条上一排按钮 + 把 QTabWidget 自己那条标签栏藏起来"，而不是把 QTabBar 搬出来：
    // 标签栏的几何归 QTabWidget 管（和内容区那条接缝、圆角都是它画的），搬出来只会得到一个
    // 位置还会被拽回去的条。藏起来的后果是好的那一半 —— Qt 的 `initStyleOption()` 里那句
    // `if (d->tabs->isVisibleTo(this))` 一判，`tabBarSize` 归零，于是 `contentsRect` 变成
    // 整个 QTabWidget：**内容页铺满整块**（所以覆盖层也跟着铺满，见 OverlayHost）。
    tabWidget->tabBar()->hide();
    // 标签**就是** tabWidget 自己的那一份：名字、顺序、条数都只有一处出处。
    // NOTE: 这一句必须在上面四个 addTab **之后** —— tabText() 这会儿才有东西。
    QStringList navLabels;
    for (int i = 0; i < toolboxTabIndex(); ++i) {  // 工具箱那一页不上导航
        navLabels << tabWidget->tabText(i);
    }
    bar->setNavItems(navLabels);
    bar->setCurrentNav(tabWidget->currentIndex());
    // 条只发信号、不自己动手（见 TitleBar.hpp）：翻页是这一层的事。
    QObject::connect(bar, &TitleBar::navRequested, tabWidget, [this](int index) {
        const int sourceIndex = tabWidget->currentIndex();
        const int result = applyTabRequest(index);
        if (result < 0) return;
        // 翻页 / dismiss 都已经发出后再报业务语义。比让 MainWindow 也监听
        // navRequested、并假设“UI 那条先 connect 所以先跑”更稳定。
        emit bar->navigationApplied(sourceIndex, index, result == 1);
    });
    // 反方向也要通：别处（底栏、主页那几个入口）翻页之后，条上标着的也得跟着换。
    QObject::connect(tabWidget, &QTabWidget::currentChanged, bar, &TitleBar::setCurrentNav);
    QObject::connect(tabWidget, &QTabWidget::currentChanged, tabWidget, [this](int index) {
        if (index != homeTabIndex()) {
            closeHomeInstancePicker();
        }
    });
    auto *closePickerShortcut = new QShortcut(QKeySequence(Qt::Key_Escape),
                                              tabWidget->widget(homeTabIndex()));
    closePickerShortcut->setObjectName(QStringLiteral("homePickerCloseShortcut"));
    closePickerShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(closePickerShortcut, &QShortcut::activated, tabWidget,
                     [this] { closeHomeInstancePicker(); });

    // ================= 换页那一下 =================
    // 换页动效（「切换页面」）：新那一块**先等一拍、再淡进来**，
    // 顺带从下面抬 `Anim::pageRiseDistance()` 那么一小段。规矩（尤其是"一个像素的几何都
    // 不动"）见 ui/Animations.hpp。
    //
    // 这里动的**是内容区整个控件**（`tabWidget` 自己），**不是** `currentWidget()` 那一页。
    // 换页时整页的卡片要淡进来、不能瞬间出现。原因在这一层底下：那张卡片和它的外框**是 `QTabWidget`
    // 自己画的** —— 四个页面都是 `ThemeBackground(Part::PageBackground)`，而它在"没设图"
    // 时 `paintEvent()` 直接 return（见那个文件头上那一大段）。所以只给页面装效果的话，
    // 卡片和外框不在这张图里，淡的是"卡片上浮着的一层内容"，虚的那几帧能看见卡片那层底
    // 从中间透出来。
    //
    // 改的是**抬**那一半：淡的仍然是整块（卡片一起淡），但**往下挪着画的只有
    // 它里面的页面**（`ui/Animations.cpp` 里那个文件内的 `liftTarget()` 把它认出来）。
    // 有框的时候框会瞬移，所以抬的是内容、不是框 —— 卡片那圈外框换页前后都在同一
    // 个地方，整块一起挪就是**框自己跳了一下再滑回来**；内容是新上来的，没有参照物，反倒
    // 像正常的滑入。整段理由与做法见 `Animations.hpp` 里 `pageAppear()` 那段。
    //
    // NOTE: 幕布底下写回的是祖先们原样那层底（含整窗磨砂的半透明着色），不垫不透明色 ——
    // 垫 `QPalette::Base` 那一版在真机上换页时磨砂整块变实心再变回来。
    // NOTE: 挂的是 `currentChanged`，不是某一次点击 —— 四颗导航、底栏、主页那几个入口
    // 都会翻页，只有挂在这儿才一处管住全部（"唯一出处"那条规矩）。
    // NOTE: 只在**已经露着头**的时候动。搭界面这一路会挨个 `addTab()`，`currentChanged`
    // 在那时候就会响，而那些页还没上屏、几何也没定下来，给它们装效果纯属白干
    // （`tabWidget` 是 `centralWidget` 里生出来的，这条判据在它身上同样说得通）。
    QObject::connect(tabWidget, &QTabWidget::currentChanged, tabWidget, [this](int index) {
        Anim::cancelLayoutTransition(tabWidget->parentWidget());
        if (overlayHost && overlayHost->promptOpen() && overlayHost->promptFullSize()) {
            if (TaskCenterPanel *panel = taskCenterPanel()) {
                QWidget *workspace = panel->parentWidget();
                overlayHost->closeOverlay();
                overlayHost->setPromptBlockedWidget(bottomBar);
                workspace->deleteLater();
            }
        }
        if (!Anim::enabled() || !tabWidget->isVisible()) return;

        // 让同一个切页信号上的后续槽先更新内容（如设置页的 Java 与内存），再排版和拍首帧。
        QMetaObject::invokeMethod(tabWidget, [this, index] {
            const QPointer<QTabWidget> tabs(tabWidget);
            if (!Anim::enabled() || tabs->currentIndex() != index || !tabs->isVisible()) return;
            const QPointer<QWidget> page(tabs->currentWidget());
            if (!page || !page->isVisible()) return;

            // 切回停放着整页的选项卡时，由 OverlayHost 播放整页淡入。
            const auto canAnimate = [this] {
                return overlayHost == nullptr || !overlayHost->pageOpen() || overlayHost->parked();
            };
            if (!canAnimate()) return;
            DesignDefaults::settlePageLayout(page);
            if (!tabs || !page || tabs->currentWidget() != page || !page->isVisible() || !canAnimate()) return;
            Anim::pageAppear(tabs);
        }, Qt::QueuedConnection);
    });

    // 内层那几层换页（下载页右栏、游戏栏、设置页右栏、实例页客户端 / 服务端）不用再接：
    // 所有 `QStackedWidget` 换页默认就有 `pageAppear()`（`ui/DesignDefaults.hpp`）。

    setupBottomBar(bar);
    bar->setFooter(bottomBar);
    // 导航轨底部「工具箱」的高亮 = 工具箱选项卡正开着。
    toolboxButton->setChecked(tabWidget->currentIndex() == toolboxTabIndex());
    QObject::connect(tabWidget, &QTabWidget::currentChanged, toolboxButton,
                     [button = QPointer<QPushButton>(toolboxButton)](int index) {
                         if (button) button->setChecked(index == toolboxTabIndex());
                     });
    // 图标轨：任务卡只剩进度环、运行状态那格只剩图标和进程数（常驻，空闲时图标弱化）、工具箱只剩图标（tokens.md 第 7 节）。
    const auto applyRailCollapsed = [this](bool collapsed) {
        if (auto *text = taskSummaryButton->findChild<QWidget *>(QStringLiteral("taskSummaryText"))) {
            text->setVisible(!collapsed);
        }
        if (auto *card = taskSummaryButton->layout()) {
            card->setContentsMargins(collapsed ? QMargins(8, 8, 8, 8) : QMargins(10, 8, 10, 8));
        }
        m_railCollapsed = collapsed;
        // 图标轨里任务卡只剩进度环：没有任务时也把环留着，否则整颗按钮会缩成一块白框。
        if (progressBar != nullptr) {
            progressBar->setVisible(collapsed || m_lastTaskSummary.hasTask);
        }
        if (taskFailureBadge != nullptr) {
            taskFailureBadge->setVisible(collapsed && m_lastTaskSummary.unreadFailure);
        }
    };
    QObject::connect(bar, &TitleBar::collapsedChanged, bottomBar, applyRailCollapsed);
    applyRailCollapsed(bar->isCollapsed());  // 轨一建好就是图标轨（每次启动都收起），不等第一次切换
    backdrop->setHeroWidget(homeTab);
    // 主页上窗口按钮压在清晰风景上换主页字色：跟着风景层此刻的不透明度渐变（进出主页 250ms 淡变，
    // 主页上开了整页覆盖层时风景收起，也跟着淡回普通字色），不在换页那一刻硬切。品牌标、品牌字不变。
    bar->setHeroOpacity(backdrop->heroOpacity());
    QObject::connect(backdrop, &Glass::Backdrop::heroOpacityChanged, bar, &TitleBar::setHeroOpacity);

    // ---------- 内容区上面那一层 ----------
    // 设置界面和各种提示都开在这儿，不另开窗口（见 OverlayHost.hpp 开头那段）。
    // NOTE: 建在**最后**：它不吃布局，几何靠盯住 tabWidget 的 Resize/Move 现算，所以
    // 搭它的时候上面那四个 addTab 必须已经跑完了。
    overlayHost = new OverlayHost(tabWidget, centralWidget);
    // **提示开着时把底栏一起按住**。这一句必须在底栏建好之后（上面的
    // setupBottomBar 已经跑完），理由见 OverlayHost::setPromptBlockedWidget()：
    // 底栏不在覆盖层底下（这一层挂在 tabWidget 上），"被盖住 = 点不着"那条判据管不到它，
    // 于是提示开着时底栏照样能点开上拉面板 → 面板里点「改设置」被挡住 → 而那句解释
    // 为什么的话又要被"提示叠提示不做"吞掉 ⇒ 用户点了一下，屏幕上什么都没有。
    overlayHost->setPromptBlockedWidget(bottomBar);

    // ---------- 外框 ----------
    // 标题栏收进客户区 + 系统背板（见 WindowChrome.hpp 开头那两段）。拖窗区两条：
    // 右上那条窗口按钮条、导航轨的空白处。
    windowChrome = new WindowChrome(parent, bar->captionStrip(), parent);
    windowChrome->addDragStrip(bar);
    // 主窗口不要 Windows 的贴靠：拖窗自己搬、不进系统移动循环（见 WindowChrome.hpp【自己拖窗】）。
    windowChrome->setOwnMove(true);

    // 窗口大小一变：刷新「恢复默认」的置灰，并把该记的大小交给 MainWindow 落盘。
    new WindowSizeWatcher(parent, [this, parent] {
        if (windowSizeResetButton != nullptr) windowSizeResetButton->setEnabled(!isDefaultWindowSize(parent));
        if (m_windowSizeHandler) m_windowSizeHandler();
    });
    if (windowSizeResetButton != nullptr) windowSizeResetButton->setEnabled(!isDefaultWindowSize(parent));
}

QSize MainWindowUI::fittedWindowSize(QSize wanted, QSize available) {
    if (available.isValid() && !available.isEmpty()) wanted = wanted.boundedTo(available);
    return wanted.expandedTo(QSize(kMainWindowMinWidth, kMainWindowMinHeight));
}

QSize MainWindowUI::fittedWindowSize(const QWidget *window, QSize wanted) {
    const QScreen *screen = window->screen();
    return fittedWindowSize(wanted, screen != nullptr ? screen->availableGeometry().size() : QSize());
}

void MainWindowUI::restoreWindowSize(QWidget *window, QSize saved) {
    if (saved.isEmpty()) saved = QSize(kMainWindowDefaultWidth, kMainWindowDefaultHeight);
    window->resize(fittedWindowSize(window, saved));
}

void MainWindowUI::resetWindowSize(QWidget *window) {
    if (window->isFullScreen()) window->showNormal();
    const QSize size = fittedWindowSize(window, QSize(kMainWindowDefaultWidth, kMainWindowDefaultHeight));
    window->resize(size);
    if (const QScreen *screen = window->screen()) {
        // move() 摆的是带窗框的外沿：按外沿大小居中（无边框窗两者一样）。
        QRect frame(QPoint(), size + window->frameGeometry().size() - window->geometry().size());
        frame.moveCenter(screen->availableGeometry().center());
        window->move(frame.topLeft());
    }
}

bool MainWindowUI::isDefaultWindowSize(const QWidget *window) {
    return !window->isFullScreen()
           && window->size() == fittedWindowSize(window, QSize(kMainWindowDefaultWidth, kMainWindowDefaultHeight));
}

void MainWindowUI::syncGameWindowSizeResetButton() {
    if (gameWindowSizeResetButton == nullptr || gameWindowWidthSpin == nullptr
        || gameWindowHeightSpin == nullptr) {
        return;
    }
    // 那一整行被窗口模式灰掉（最大化 / 全屏时那两个数不参与启动）时，这颗也跟着灰：
    // 亮着一颗按下去什么都不改的钮比灰着更难懂。
    const bool rowEnabled = gameWindowWidthSpin->isEnabled() && gameWindowHeightSpin->isEnabled();
    const bool isDefault = gameWindowWidthSpin->value() == windowDefaultWidth()
                           && gameWindowHeightSpin->value() == windowDefaultHeight();
    gameWindowSizeResetButton->setEnabled(rowEnabled && !isDefault);
}

void MainWindowUI::updateHomeQuoteFont(bool narrow) {
    homeQuoteNarrow = narrow;
    fitHomeQuoteFont();
}

void MainWindowUI::fitHomeQuoteFont() {
    if (homeHeroQuote == nullptr) return;
    const int lo = homeQuoteNarrow ? kHomeQuoteNarrowMinPixelSize : kHomeQuoteMinPixelSize;
    const int hi = homeQuoteNarrow ? kHomeQuoteNarrowMaxPixelSize : kHomeQuoteMaxPixelSize;
    // 行高按上限、按三种文字里最高的那一种预留：换句（连带换语种）时下面的版面不跳。
    int line = 0;
    for (const Theme::QuoteScript s : {Theme::QuoteScript::Han, Theme::QuoteScript::Latin, Theme::QuoteScript::Kana})
        line = qMax(line, QFontMetrics(Theme::quoteFont(homeHeroQuote->font(), s, hi)).height());
    const QMargins m = homeHeroQuote->contentsMargins();
    homeHeroQuote->setFixedHeight(m.top() + line + m.bottom());
    // 放得下整句的最大一号；到下限还放不下就用下限，交给省略号。
    const int px = Theme::fitPixelSize(Theme::quoteFont(homeHeroQuote->font(), homeQuoteScript, hi),
                                       homeHeroQuote->text(), homeHeroQuote->contentsRect().width(), hi, lo);
    const QFont font = Theme::quoteFont(homeHeroQuote->font(), homeQuoteScript, px);
    if (homeQuoteScript == Theme::QuoteScript::Kana) {
        // 日文走系统字体链，不算手写体：不挂 wtfmlDisplay（ui_check 按它查手写体有没有漏）。
        homeHeroQuote->setFont(font);
        homeHeroQuote->setProperty(Theme::displayPropertyName().toUtf8().constData(), false);
        return;
    }
    Theme::markDisplay(homeHeroQuote, font);
}

void MainWindowUI::setHomeHeroTitle(const QString &text, const QString &name) {
    if (auto *title = static_cast<HomeGreetingLabel *>(homeHeroTitle)) title->setGreeting(text, name);
}

QString MainWindowUI::homeHeroTitleShown() const {
    return homeHeroTitle == nullptr ? QString() : static_cast<HomeGreetingLabel *>(homeHeroTitle)->shown();
}

QFont MainWindowUI::homeHeroTitleNameFont() const {
    return homeHeroTitle == nullptr ? QFont() : static_cast<HomeGreetingLabel *>(homeHeroTitle)->wordFont();
}

int MainWindowUI::homeHeroTitleShownWidth() const {
    return homeHeroTitle == nullptr ? 0 : static_cast<HomeGreetingLabel *>(homeHeroTitle)->shownWidth();
}

void MainWindowUI::setHomeHeroQuote(const QString &text, const QString &tooltip,
                                    Theme::QuoteScript script) {
    if (homeHeroQuote == nullptr) return;
    homeQuoteScript = script;
    homeHeroQuote->setText(text);
    const QWidget *host = homeHeroQuote->window();
    updateHomeQuoteFont(host != nullptr && host->width() < 1024);
    homeHeroQuote->setToolTip(tooltip);
    homeHeroQuote->setVisible(!text.isEmpty());
}

/// 主页那一页：左 = 挑一个已装好的版本、启动、三种快捷入口；右 = 我的角色（3D 预览）。
///
/// 原先是 `setupUi()` 中间那两百多行。搬出来只是为了让"哪一页长什么样"有个名字：
/// `setupUi()` 现在只剩"按契约顺序把四页摆出来"和最后那一段接线。
QWidget *MainWindowUI::setupHomePage() {
    QWidget *homeTab = new ThemeBackground(Theme::Part::PageBackground, tabWidget);
    homeTab->setObjectName(homeTabName());
    auto *homePageLayout = new QVBoxLayout(homeTab);
    homePageLayout->setContentsMargins(0, 0, 0, 0);
    homePageLayout->setSpacing(0);
    // 设计稿 home.html 的版式（页面在 40 高的拖动条下面）：标题和启动区离页左 44，账号胶囊
    // 离页右 28、离顶 12；启动区底到实例带顶留 70；实例带离页左 32、离页底 22。
    // 两栏的页边距左右相等（验收量这一条），取 kHomeHeroMargin，左栏里再缩 kHomeHeroInset。
    homeHeroArea = new QWidget(homeTab);
    homeHeroArea->setObjectName(QStringLiteral("homeHeroArea"));
    const TwoColumns home = buildTwoColumnPage(homeHeroArea, QStringLiteral("home"));
    if (QLayout *row = homeHeroArea->layout()) {
        row->setContentsMargins(kHomeHeroMargin, 12, kHomeHeroMargin, 78 - kHomeLaunchGlowRoom);
    }
    // 左栏不缩：启动区那块可点的面要往左多出 kHomeHeroInset 的悬停底，标题自己缩进这一截。
    home.left.layout->setContentsMargins(0, 0, 0, 0);
    home.left.layout->setSpacing(0);
    home.right.layout->setSpacing(0);
    homePageLayout->addWidget(homeHeroArea, 1);
    auto *homeBottomArea = new QWidget(homeTab);
    homeBottomArea->setObjectName(QStringLiteral("homeBottomArea"));
    auto *homeBottomLayout = new QVBoxLayout(homeBottomArea);
    homeBottomLayout->setContentsMargins(32, 0, 0, 22);
    homeBottomLayout->setSpacing(12);
    homePageLayout->addWidget(homeBottomArea);
    {
        home.left.layout->addSpacing(40);
        // 问候语由 MainWindow::updateHomeGreeting() 灌（规则在 src/util/HomeGreeting）；
        // 灌之前先写「欢迎来到 WTFML」，不留空。
        auto *welcome = new HomeGreetingLabel(home.left.widget);
        welcome->setGreeting(QCoreApplication::translate("MainWindowUI", "欢迎来到 WTFML"), QString());
        welcome->setObjectName(homeHeroTitleName());
        welcome->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        welcome->setContentsMargins(kHomeHeroInset, 0, 0, 0);
        homeHeroTitle = welcome;
        home.left.layout->addWidget(welcome);
        followHeroInk(welcome, false);
        // 大标题正下方那一行「每日一言」：一天一句，零实例与有实例都显示。
        // 位置就是被删掉的英文副题那一格（边距 kHomeHeroInset + 4, 12）。没句子时**隐藏**，
        // 版面和没有这一行时一模一样（设置里关掉也走这条路）。
        auto *quote = new HomeQuoteLabel(home.left.widget);
        quote->setObjectName(homeHeroQuoteName());
        quote->setContentsMargins(kHomeHeroInset + 4, 12, 0, 0);
        // 宽度交给布局（Ignored），文字长短不会把左栏抻开；装不下自己收省略号。
        quote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        // 字号随句长变，顶边对齐：和大标题的间距不跟着字号走。
        quote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        quote->widthChanged = [this] { fitHomeQuoteFont(); };
        quote->setToolTip(QString());
        quote->setVisible(false);
        homeHeroQuote = quote;
        home.left.layout->addWidget(quote);
        followHeroInk(quote, true);
        // 窄于 1024 的窗口：标题收一号（50 → 40），一言换到窄窗那档字号区间，立绘缩到七成二（设计稿 @media）。
        const auto applyScale = [this, homeTab, welcome]() {
            const bool narrow = homeTab->window()->width() < 1024;
            QFont titleFont = handPx(welcome->font(), Theme::HandFont::KaiLight,
                                     narrow ? 40 : kHomeHeroTitlePixelSize, narrow ? 3 : 4);
            // 名字里文楷没有的字（é、Я、假名）先落系统楷体，笔意还接得上；再没有才到系统字体。
            QStringList families = titleFont.families();
            if (families.size() >= 2) families.insert(1, QStringLiteral("KaiTi"));
            titleFont.setFamilies(families);
            Theme::markDisplay(welcome, titleFont);
            updateHomeQuoteFont(narrow);
            if (skinViewer != nullptr) {
                skinViewer->setMaximumHeight(narrow ? kHomeSkinViewerHeight * 72 / 100 : kHomeSkinViewerHeight);
            }
        };
        new ResizeHook(homeTab, applyScale);
        applyScale();
    }
    {
        // 下拉框继续作为启动逻辑的内部选择模型，但主页上不出现。用户从启动区的「切换实例」
        // 或实例带的「全部实例」进入完整选择页；这样不用复制启动状态机。
        installedVersionCombo = new QComboBox(home.left.widget);

        // 空状态用"没选中任何项(currentIndex() == -1) + placeholderText"表示，
        // 不往列表里塞 "自定义版本" 那种假条目：控件自己会把占位文案画成灰字。
        // NOTE: 只要占位文案非空，往空下拉框里 addItems() 就不会自动选中第 0 项
        // （QComboBoxPrivate::rowsInserted 里那条 setCurrentIndex(0) 带了
        // !placeholderText.isEmpty() 的条件）。所以选中项全由 MainWindow 定：fillVersionCombo()
        // 保住原来那个，保不住就按 resolveCurrentInstance() 挑一个（有实例必有当前实例）。
        // 这里只是初始值；灌完数据后由 MainWindow 在两个串之间切换。
        installedVersionCombo->setPlaceholderText(noInstalledVersionPlaceholder());

        // 版本号长度不定（26.2-snapshot-1 之类），默认的 AdjustToContentsOnFirstShow 会在
        // 第一次显示（那时还是空的）时定一个宽度然后一直沿用，之后灌进上千个长版本号就容易
        // 把窗口顶宽。按"最短内容长度"算，宽度确定，灌数据时也不会跳。
        //
        // NOTE: "宽度确定"有个前提，很容易踩：**占位文案一旦超过 minimumContentsLength
        // 就会被算进 sizeHint**。（Qt 6.10.1，Fusion）13 字的占位会把框顶宽
        // 12px，而**条目文本和当前选中文本不影响**（空 / 加了一行超长条目 / 选中
        // 那行超长条目，算出来都是同一个宽度）。
        // 所以：给这两个下拉框设的每一条占位文案都必须 ≤12 字。现在共四处 ——
        // 本文件两处 + MainWindow.cpp 两处（那句取数失败的文案是 10 字，超过 12 字
        // 会让框在失败时突然变宽）。要放长句就写到旁边的 hint
        // 标签上去，QLabel 没有这个约束。
        installedVersionCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        installedVersionCombo->setMinimumContentsLength(12);
        // 滚轮防护：见 MainWindowUI.hpp 里的 WheelGuard
        WheelGuard::install(installedVersionCombo);
        installedVersionCombo->setObjectName(installedVersionComboName());
        installedVersionCombo->hide();

        startButton = new QPushButton(QCoreApplication::translate("MainWindowUI", "启动游戏"), home.left.widget);
        // 文字是两行（"启动游戏\n<显示名>"，由 MainWindow::updateStartButtonText() 维护；
        // 高度固定给足，免得两行被压成一行半。
        startButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        startButton->setMinimumHeight(kStartButtonMinHeight);
        QFont startFont = startButton->font();
        startFont.setBold(true);
        startFont = Theme::textFont(startFont, Theme::TextRole::Heading);
        startButton->setFont(startFont);
        Theme::markButton(startButton, Theme::ButtonKind::Primary);
        // 启动入口在左下那块启动区（`homeCurrentLaunch`）。保留这个兼容控件供既有业务连接
        // 使用，但不参与主页布局，否则会出现两颗功能完全相同的按钮。
        startButton->hide();

        // 启动用哪种身份**只由当前账号是什么**决定 —— 离线就去账号窗里加一个
        // 离线账号、切过去，没有第二个开关。判据的唯一出处是 MainWindow::offlineIdentity()。

        // 左下启动区（设计稿 r2 .launch）：当前实例的名字 + 元信息（整块可点，开实例切换弹层，
        // 见 openHomeInstancePicker()）、「启动游戏」+ 设置。压在风景左下（上面那段弹簧把它推下去）。
        homeLaunchSpacer = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
        home.left.layout->addItem(homeLaunchSpacer);
        homeQuickCurrentSection = new QWidget(home.left.widget);
        homeQuickCurrentSection->setObjectName(QStringLiteral("homeQuickCurrentSection"));
        homeQuickCurrentSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        homeQuickCurrentRow = new QHBoxLayout(homeQuickCurrentSection);
        homeQuickCurrentRow->setContentsMargins(0, 0, 0, 0);
        homeQuickCurrentRow->setSpacing(0);
        home.left.layout->addWidget(homeQuickCurrentSection);

        // 下半区：「最近 / 收藏」实例带（设计稿 .strip）。
        homeLaunchArea = new QWidget(homeBottomArea);
        homeLaunchArea->setObjectName(QStringLiteral("homeLaunchArea"));
        homeLaunchArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *homeLaunchLayout = new QVBoxLayout(homeLaunchArea);
        homeLaunchLayout->setContentsMargins(0, 0, 0, 0);
        homeLaunchLayout->setSpacing(12);
        homeQuickSection = new QWidget(homeLaunchArea);
        homeQuickSection->setObjectName(QStringLiteral("homeQuickSection"));
        homeQuickSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *quickColumn = new QVBoxLayout(homeQuickSection);
        quickColumn->setContentsMargins(0, 0, 0, 0);
        quickColumn->setSpacing(0);  // 页签与卡片的 6 px 放在折叠体顶边距里，收起时一起收走，落定那一下不跳
        auto *quickTabs = new QHBoxLayout;
        quickTabs->setContentsMargins(12, 0, 24, 0);
        quickTabs->setSpacing(6);
        const auto makeQuickTab = [this, quickTabs](const QString &text, const QString &name,
                                                    HomeQuickMode mode) {
            auto *button = new HomeStripTab(text, homeQuickSection);
            button->setObjectName(name);
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            button->setProperty("wtfmlHomeQuickTab", true);
            button->setCursor(Qt::PointingHandCursor);
            QFont tabFont = button->font();
            tabFont.setPixelSize(14);
            button->setFont(tabFont);
            // 收起时点页签：先展开再切过去。
            QObject::connect(button, &QPushButton::clicked, button, [this, mode]() {
                setHomeQuickCollapsed(false);
                setHomeQuickMode(mode);
            });
            quickTabs->addWidget(button);
            return button;
        };
        // 「当前」不再是实例带里的一页（当前实例就是上面那块启动区，一直摆着）；这颗页签
        // 留着给既有接口，不显示。
        homeQuickCurrentTab = makeQuickTab(QCoreApplication::translate("MainWindowUI", "当前"),
                                            QStringLiteral("homeQuickCurrentTab"),
                                            HomeQuickMode::Current);
        homeQuickCurrentTab->hide();
        homeQuickRecentTab = makeQuickTab(QCoreApplication::translate("MainWindowUI", "最近"),
                                           QStringLiteral("homeQuickRecentTab"),
                                           HomeQuickMode::Recent);
        homeQuickFavoriteTab = makeQuickTab(QCoreApplication::translate("MainWindowUI", "收藏"),
                                             QStringLiteral("homeQuickFavoriteTab"),
                                             HomeQuickMode::Favorite);
        homeQuickToggle = new HomeStripToggle(homeQuickSection);
        homeQuickToggle->setObjectName(QStringLiteral("homeQuickToggle"));
        homeQuickToggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        homeQuickToggle->setCursor(Qt::PointingHandCursor);
        QObject::connect(homeQuickToggle, &QPushButton::clicked, homeQuickToggle,
                         [this]() { setHomeQuickCollapsed(!m_homeQuickCollapsed); });
        quickTabs->addWidget(homeQuickToggle);
        syncHomeQuickToggleText();
        quickTabs->addStretch(1);
        // 「管理 ›」：去实例页（导航，不在主页里展开列表）。
        homeInstanceSelectButton = new QPushButton(QCoreApplication::translate("MainWindowUI", "管理"), homeQuickSection);
        homeInstanceSelectButton->setObjectName(homeInstanceSelectButtonName());
        homeInstanceSelectButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        homeInstanceSelectButton->setLayoutDirection(Qt::RightToLeft);  // 箭头在字右边
        Theme::markButton(homeInstanceSelectButton, Theme::ButtonKind::Ghost);
        {
            QFont manageFont = homeInstanceSelectButton->font();
            manageFont.setPixelSize(14);
            homeInstanceSelectButton->setFont(manageFont);
        }
        QObject::connect(homeInstanceSelectButton, &QPushButton::clicked, homeInstanceSelectButton,
                         [this]() { tabWidget->setCurrentIndex(instancesTabIndex()); });
        // 不挂 › 图标（体验反馈 8）：「管理」两个字本身就说明点下去去哪。
        followHeroInk(homeInstanceSelectButton, true);
        quickTabs->addWidget(homeInstanceSelectButton);
        quickColumn->addLayout(quickTabs);
        // 两排卡片装在同一个折叠体里（同一时间只显示其中一排）；收起 / 展开走 FoldMotion。
        homeQuickCards = new QWidget(homeQuickSection);
        homeQuickCards->setObjectName(QStringLiteral("homeQuickCards"));
        auto *cardsLayout = new QVBoxLayout(homeQuickCards);
        cardsLayout->setContentsMargins(0, 6, 0, 0);
        cardsLayout->setSpacing(0);
        quickColumn->addWidget(homeQuickCards);
        homeQuickFold = new SettingsParts::FoldMotion(homeQuickCards);
        const auto makeQuickPage = [cardsLayout, this](const QString &name, QWidget **sectionOut,
                                                       QHBoxLayout **rowOut) {
            auto *section = new HomeStripRow(homeQuickCards);
            section->setObjectName(name);
            cardsLayout->addWidget(section);
            *sectionOut = section;
            *rowOut = section->row();
        };
        makeQuickPage(QStringLiteral("homeFavoriteSection"), &homeFavoriteSection,
                      &homeFavoriteRow);
        makeQuickPage(QStringLiteral("homeQuickRecentSection"), &homeQuickRecentSection,
                      &homeQuickRecentRow);
        homeLaunchLayout->addWidget(homeQuickSection);
        homeBottomLayout->addWidget(homeLaunchArea);
    }
    // ---------- 主页右栏：账号胶囊 + 站在风景里的立绘 ----------
    // 设计稿：账号胶囊贴右上（页面在 40 高的拖动条下面，窗口按钮压不到它）；立绘不进任何盒子，
    // 脚踩一圈落地影站在右下，脚底和启动按钮底边齐。
    {
        auto *profileCard = new QWidget(home.right.widget);
        profileCard->setObjectName(QStringLiteral("homeProfileCard"));
        profileCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto *profileLayout = new QVBoxLayout(profileCard);
        profileLayout->setContentsMargins(0, 0, 0, 0);
        profileLayout->setSpacing(0);

        // 账号唯一的入口。名字 / 小状态 / 头像由 MainWindow::updateBottomBar() 灌。
        accountSkinButton = new HomeAccountPill(profileCard);
        accountSkinButton->setObjectName(accountSkinButtonName());
        accountSkinButton->setAccessibleName(QCoreApplication::translate("MainWindowUI", "账号"));
        accountSkinButton->setToolTip(accountSkinButtonText());
        auto *accountRow = new QHBoxLayout(accountSkinButton);
        accountRow->setContentsMargins(6, 0, 16, 0);
        accountRow->setSpacing(10);
        homeAvatarLabel = new HomeAccountAvatar(accountSkinButton);
        homeAvatarLabel->setObjectName(QStringLiteral("homeAvatarLabel"));
        homeAvatarLabel->setFixedSize(32, 32);
        homeAvatarLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        accountRow->addWidget(homeAvatarLabel);
        auto *identity = new QVBoxLayout;
        identity->setContentsMargins(0, 0, 0, 0);
        identity->setSpacing(0);
        identity->addStretch(1);
        homeAccountLabel = new ElidedLabel(accountSkinButton);
        homeAccountLabel->setObjectName(QStringLiteral("homeAccountLabel"));
        {
            QFont nameFont = Theme::textFont(homeAccountLabel->font(), Theme::TextRole::Body);
            nameFont.setWeight(QFont::DemiBold);
            homeAccountLabel->setFont(nameFont);
        }
        homeAccountLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        followHeroInk(homeAccountLabel, false);
        identity->addWidget(homeAccountLabel);
        homeAccountTypeLabel = new ElidedLabel(accountSkinButton);
        homeAccountTypeLabel->setObjectName(QStringLiteral("homeAccountTypeLabel"));
        homeAccountTypeLabel->setFont(Theme::textFont(homeAccountTypeLabel->font(), Theme::TextRole::Caption));
        homeAccountTypeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        followHeroInk(homeAccountTypeLabel, true);
        identity->addWidget(homeAccountTypeLabel);
        // 业务层灌数据之前先按「没登录」摆：名字写「登录」、小状态收起。
        homeAccountLabel->setText(homeAccountSignInText());
        homeAccountTypeLabel->hide();
        identity->addStretch(1);
        accountRow->addLayout(identity, 1);
        QObject::connect(accountSkinButton, &QPushButton::clicked, accountSkinButton, [this]() { toggleAccountsPanel(); });
        profileLayout->addWidget(accountSkinButton, 0, Qt::AlignRight | Qt::AlignTop);

        skinViewer = new SkinViewer(profileCard);
        skinViewer->setObjectName(skinViewerName());
        // 宽钉在右栏宽；高按 SkinViewer 的人物比例算（heightForWidth），竖向放不下时被压矮、
        // 人跟着缩，窄窗再封顶到七成二（见上面 applyScale）。底边贴右栏底 = 启动按钮底边。
        skinViewer->setFixedWidth(kHomeSkinViewerWidth);
        skinViewer->setMinimumHeight(140);
        skinViewer->setMaximumHeight(kHomeSkinViewerHeight);
        // 立刻喂一套外观。这个控件**任何时候都不该是空白的** —— 没登录、还没取到、
        // 取不到，这三种情况看到的都是这套默认外观，所以兜底放在这里而不是等业务层来灌：
        // 少一处"谁忘了设"的可能。业务层拿到真皮肤时再覆盖它。
        skinViewer->setSkin(SkinModel::defaultSteveSkin(), QImage(), false);
        profileLayout->addWidget(skinViewer, 1, Qt::AlignHCenter | Qt::AlignBottom);

        skinHintLabel = new QLabel(skinHintNotLoggedInText(), profileCard);
        skinHintLabel->setObjectName(skinHintLabelName());
        skinHintLabel->setWordWrap(true);
        // 横向上把自己从布局的度量里摘出去：wordWrap 的 QLabel 报出来的 sizeHint 是
        // "整句排成一行"的宽度，这一栏是定宽的，不能让一句说明去顶它。
        skinHintLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        // 状态仍由业务层维护，供无障碍/测试读取；主页不显示这句冗余说明。
        skinHintLabel->hide();
        home.right.layout->addWidget(profileCard, 1);
    }
    // NOTE: 控件一个都不放也得把这栏建出来 —— 验证工程靠 leftColumnName()/rightColumnName()
    // 找这两栏量几何，主页右栏没了就等于主页的比例没地方量。
    return homeTab;
}

/// 实例那一页：左 = 游戏文件夹列表；右 = 那个文件夹里的实例（客户端 / 服务端 / 预设三页）。
QWidget *MainWindowUI::setupInstancePage() {
    // ---------- 实例：左 = 游戏文件夹列表；右 = 那个文件夹里的实例 ----------
    QWidget *instanceTab = new ThemeBackground(Theme::Part::PageBackground, tabWidget);
    instanceTab->setObjectName(instanceTabName());
    // NOTE: 这一份**不是 const**（另几页那三份是）：页头那两行字和右上角那条操作是
    // `buildTwoColumnPage()` 搭完之后才由这一页填的（`fillPageHeader()` 收的是
    // `Column&`）。
    TwoColumns instance = buildTwoColumnPage(instanceTab, QStringLiteral("instance"));
    // ---- 页头：标题「实例」+ 副标题（路径 + 实例数量，即 `instanceFolderLabel`）----
    //
    // 那句副标题**就是** `instanceFolderLabel`：它一直说的是"这一页的实例是从哪一份
    // .minecraft 里扫出来的、一共有几个"，"页面说明 / 路径 /
    // 当前状态"要的正是它。所以页头那个说明标签**就是它**（objectName 是
    // `instanceFolderLabel`）—— 文案、灌数据那条路（MainWindow::refreshInstanceList）、
    // 验证工程找它用的名字都落在这一处（标题下面那行说明）。
    fillPageHeader(instance.right, instancePageTitle(), QString());
    // 设计稿 page-title：一级页标题用 Display 档（26px 手写体 Light）。
    Theme::markDisplay(instance.right.title, Theme::textFont(instance.right.title->font(), Theme::TextRole::Display));
    // 路径行：换成 ElidedLabel（QLabel 折行会把最小宽度下的长路径折成两行），中间省略，
    // 全文放进提示。
    {
        auto *elided = new ElidedLabel(instance.right.header);
        elided->setElideMode(Qt::ElideMiddle);
        markTextRole(elided, Theme::secondaryTextRoleValue());
        elided->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        instance.right.header->layout()->replaceWidget(instance.right.subtitle, elided);
        delete instance.right.subtitle;
        instance.right.subtitle = elided;
    }
    instanceFolderLabel = instance.right.subtitle;
    instanceFolderLabel->setObjectName(MainWindowUI::instanceFolderLabelName());
    // 三个标签下都有这一行，文字由 MainWindow 灌（界面这边不知道盘上有几份）。
    instanceFolderLabel->setVisible(true);
    // 右上角那颗页面级操作：「分组设置…」（它管的是**分组清单本身**，不是选中的那一个
    // 实例 —— 见下面那段，那是 就定下的语义）。
    groupInstanceButton = new QPushButton(groupInstanceText(), instance.right.header);
    groupInstanceButton->setObjectName(groupInstanceButtonName());
    // 「分组设置…」= 打开另一个窗（一般性），不是"干这件事"。
    Theme::markButton(groupInstanceButton, Theme::ButtonKind::Neutral);
    instance.right.actions->addWidget(groupInstanceButton);
    // 设计稿 manage.html：没有左栏。标题右边一颗「游戏文件夹」胶囊，点开是文件夹列表浮层
    // （原左栏的 folderList / 添加文件夹… 原样搬进去，数据流不变）；标题下保留路径行。
    instance.left.widget->setVisible(false);
    auto *folderPill = new QPushButton(instance.right.header);
    folderPill->setObjectName(instanceFolderPillName());
    folderPill->setCursor(Qt::PointingHandCursor);
    folderPill->setProperty("wtfmlFolderPill", true);
    folderPill->setIconSize(QSize(16, 16));
    const auto paintPill = [folderPill]() {
        const Theme::Tokens t = Theme::tokens();
        folderPill->setIcon(QIcon(VersionIcons::line(QStringLiteral("folder"), 16, t.secondary)));
        folderPill->setStyleSheet(QStringLiteral(
            "QPushButton { background: %2; color: %1; border: 1px solid transparent; border-radius: 10px;"
            " padding: 0 12px 0 10px; min-height: 34px; max-height: 34px; text-align: left; font-weight: 600; }"
            "QPushButton:hover { background: %3; }")
            .arg(t.text.name(QColor::HexArgb), t.subtle.name(QColor::HexArgb), t.fillSoftHover.name(QColor::HexArgb)));
    };
    paintPill();
    Theme::follow(folderPill, paintPill);
    // 胶囊右端不画 ∨（体验反馈 8 / tokens-r2 §6 下拉去箭头）：整块可点，悬停有底色。
    if (auto *titleRow = qobject_cast<QHBoxLayout *>(instance.right.header->layout()->itemAt(0)->layout())) {
        titleRow->insertWidget(1, folderPill, 0, Qt::AlignVCenter);
    }
    // 文件夹浮层：和其他浮层同一张磨砂面（原先铺的是一块不透明卡片色）。
    auto *folderPopup = new FolderPopupSurface(instanceTab);
    folderPopup->setObjectName(instanceFolderPopupName());
    auto *popupLayout = new QVBoxLayout(folderPopup);
    popupLayout->setContentsMargins(12, 12, 12, 12);
    popupLayout->setSpacing(8);
    instance.left.layout = popupLayout;
    instance.left.surface = folderPopup;
    QObject::connect(folderPill, &QPushButton::clicked, folderPopup, [folderPill, folderPopup]() {
        folderPopup->setFixedWidth(qMax(360, folderPill->width()));
        folderPopup->adjustSize();
        folderPopup->move(folderPill->mapToGlobal(QPoint(0, folderPill->height() + 6)));
        folderPopup->show();
    });

    {
        // ---- 侧栏：一个页面小标题「游戏目录」+ 文件夹列表 + 两颗按钮（都在侧栏底部）----
        QLabel *folderTitle = new QLabel(folderSectionTitleText(), instance.left.surface);
        folderTitle->setObjectName(folderSectionTitleName());
        markTextRole(folderTitle, Theme::sectionTitleTextRoleValue());
        {
            QFont titleFont = folderTitle->font();
            titleFont.setBold(true);
            folderTitle->setFont(titleFont);
        }
        instance.left.layout->addWidget(folderTitle);

        // 左边这张列表就是"启动器用哪一份 .minecraft"的选择器：选中哪一项，版本、资源、
        // 库、自管 Java 就全从那一份里找（PCL 左边那几张卡片是同一个意思）。
        // 自带的那一份永远排第一（见 FolderManager），所以列表不会是空的。
        //
        // NOTE: 每一行的内容**不是**条目自己画的（条目上没有文字），而是 setItemWidget()
        // 塞进去的一个 FolderRowWidget（见 setFolderRows()）——两行字 + 行尾一颗
        // 齿轮需要一个真控件才摆得下。省略因此完全由那一行里的 ElidedLabel 自己做。
        //
        // NOTE: 这里**不**设 `setTextElideMode(Qt::ElideMiddle)`。它是个
        // **死设置**：条目上一个字都没有，列表没有任何东西可省略，这一句不起
        // 作用（真正拦住横向滚动条的是下面那条 ScrollBarAlwaysOff）。留着更糟 ——
        // 它让人以为"路径从中间省略"这件事已经配好了，而真正画字的标签按**尾巴**
        // 省略（路径最后一级目录名正是唯一认得出是哪一份的那截）。
        // 省略放在那个标签上：见 FolderRowWidget 里的 `subtitle->setElideMode(...)`。
        folderList = new QListWidget(instance.left.surface);
        folderList->setObjectName(folderListName());
        folderList->setUniformItemSizes(true);
        folderList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        folderList->setSelectionMode(QAbstractItemView::SingleSelection);
        // 列表**不画自己那圈边**：侧栏面已经是一层底了，再套一圈就是"框套框"
        // （"减少框"第一条要减掉的正是它）。这条列表的行本来就是自绘的两行式行
        // （见 FolderRowWidget），有没有外框在观感上是"一块面板"和"一串行"的区别。
        folderList->setFrameShape(QFrame::NoFrame);
        styleSidebarList(folderList);
        // 选中 / 悬停画成内缩圆角块：只这张列表换代理，styleSidebarList() 的共用部分（下载页
        // 分类列表也走它）不动。高度按行数定，见 setFolderRows()。
        folderList->setItemDelegate(new FolderRowDelegate(folderList));
        folderList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        instance.left.layout->addWidget(folderList);

        // 「添加文件夹…」是浮层里的一行动作，长相跟菜单行走：无底无框、左对齐、强调色字 +
        // 「+」，悬停铺一块和列表行同样内缩的圆角底（原先那颗描边按钮落在磨砂面上几乎看不出边）。
        // 「移除」只在行尾齿轮菜单里那一项（同一个 MainWindow::removeFolderFromList()）。
        addFolderButton = new QPushButton(addFolderText(), instance.left.surface);
        addFolderButton->setObjectName(addFolderButtonName());
        addFolderButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        addFolderButton->setFixedHeight(34);
        addFolderButton->setCursor(Qt::PointingHandCursor);
        addFolderButton->setIconSize(QSize(16, 16));
        const auto styleAddFolder = [button = addFolderButton] {
            const Theme::Tokens t = Theme::tokens();
            const QColor ink = QApplication::palette().color(QPalette::Link);
            button->setIcon(QIcon(VersionIcons::line(QStringLiteral("plus"), 16, ink)));
            button->setStyleSheet(
                QStringLiteral("QPushButton { border: none; border-radius: 8px; background: transparent;"
                               " margin: 0 4px; padding: 0 4px; min-height: 0px; text-align: left; color: %1; }"
                               "QPushButton:hover:enabled { background: %2; }"
                               "QPushButton:pressed:enabled { background: %3; }")
                    .arg(ink.name(), t.hover.name(QColor::HexArgb), t.selected.name(QColor::HexArgb)));
        };
        styleAddFolder();
        Theme::follow(addFolderButton, styleAddFolder);
        instance.left.layout->addWidget(addFolderButton);
        // 浮层里点了一份文件夹 / 要添加文件夹：先收起浮层（切换本身照旧由 MainWindow 接）。
        QObject::connect(folderList, &QListWidget::itemClicked, folderPopup, &QWidget::hide);
        QObject::connect(addFolderButton, &QPushButton::clicked, folderPopup, &QWidget::hide);

        // 「导入整合包…」：从 Modrinth 格式的整合包（.zip / .mrpack）里**造一个新实例**。
        // 它和「添加文件夹…」是同一类动作（往这一列里添东西）—— 而右栏那三个都是
        // "对选中的实例做什么"，混在一排里用户得先猜它作用于选中项还是新建。
        importModpackButton = new QPushButton(importModpackText(), instance.left.surface);
        importModpackButton->setObjectName(importModpackButtonName());
        importModpackButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // 设计稿：「导入整合包…」在页头右上，和「分组设置…」一排。
        Theme::markButton(importModpackButton, Theme::ButtonKind::Neutral);
        importModpackButton->setParent(instance.right.header);
        importModpackButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        instance.right.actions->insertWidget(0, importModpackButton);

        importModpackButton->setToolTip(QCoreApplication::translate("MainWindowUI", "导入平台整合包或可识别的本地压缩包。"));

    }
    {
        // 「分组设置…」是**页头右上角那颗页面级操作**（见上面 `instance.right.actions`
        // 那一段）。
        //
        // 它不该跟"对选中实例做什么"的那一排站在一起：那几颗作用于**选中的那一个实例**，
        // 它作用于**分组清单本身**（有哪些组、叫什么名字、什么顺序、默认展开还是收着、
        // 哪一个是置顶组 —— 见 ui/GroupDialog）。"选中的实例"对它没有意义，混在一排里
        // 用户得先猜它是"对这条做什么"还是"对这一列做什么"。摆在页头、正文之上，说的是
        // "下面这一整块怎么组织" —— 「框外右上角」就是**页头的操作位**。
        //
        // ---- 正文：就只有这一棵树 ----
        //
        // 第一条规矩就是"减少框"—— 页面不画边框、正文只有一个面。
        // 树的四周本来就是正文面（`workspace`），再套一圈就是"框套框"。那句
        // 「当前文件夹：…」是**页头那行说明**（见上面），而且和标题同级、比框里那行
        // 更像"这一页的状态"。
        //
        // 主页那个「已安装版本」下拉框的同一份数据，只是摆在这儿配左边的文件夹看。
        // 点击 = 在主页把它选中（两边按 id 双向同步，见 MainWindow::setupConnections）。
        //
        // 是**树**不是列表：实例可以归到用户建的组里，组是父节点、实例是子节点。
        // 一个组都没有的时候它就是平铺的一层（照样不插组头），观感和列表一样。
        // ---------- 搜索与筛选 ----------
        //
        // 搜索实例名、版本号、加载器；筛选原版/模组实例、可更新、最近运行；
        // 搜索不改变分组归属；清空搜索恢复原展开状态。四句里**三句都不是界面这一层的
        // 事**：判据在 `src/manage/InstanceFilter.hpp`（纯函数），"不改分组归属 / 清空就
        // 还原展开状态"两条落在 `MainWindow::rebuildInstanceTree()`（它才知道分组和展开
        // 状态从哪儿来）。这一层只摆控件、只把"用户点了/打了什么"如实报出去。
        //
        // 摆进**正文面里**、树的上方（不是页头右上角那个"页面级操作"位）：搜索和筛选是
        // "下面这棵树怎么摆"，和那颗「分组设置…」不是一类东西 —— 后者管的是分组清单本身。
        // 这一整套（筛选行 + 空态 + 树）**装在一个页面里**，好和"服务端那一页"
        // 一起塞进 `instanceModeStack`。
        QWidget *clientPage = new QWidget(instance.right.surface);
        clientPage->setObjectName(QStringLiteral("instanceClientPage"));
        auto *clientLayout = new QVBoxLayout(clientPage);
        clientLayout->setContentsMargins(0, 0, 0, 0);
        clientLayout->setSpacing(kFilterRowSpacing);

        instanceFilterRow = new QWidget(clientPage);
        instanceFilterRow->setObjectName(instanceFilterRowName());
        {
            auto *row = new QHBoxLayout(instanceFilterRow);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(kFilterRowSpacing);
            instanceSearchEdit = new QLineEdit(instanceFilterRow);
            instanceSearchEdit->setObjectName(instanceSearchName());
            instanceSearchEdit->setPlaceholderText(instanceSearchPlaceholder());
            addSearchIcon(instanceSearchEdit);
            // 一个 ✕（一行一次清干净）。**不加**"清空搜索"那颗独立按钮：要的是
            // "清空搜索恢复原展开状态"，而那一格自己就带着这个动作，再摆一颗是同一件事
            // 说两遍。
            instanceSearchEdit->setClearButtonEnabled(true);
            instanceSearchEdit->setFixedWidth(280);  // 设计稿：搜索框靠右、定宽，chip 在左
            instanceSearchEdit->setFixedHeight(Theme::chipHeight());
            instanceSearchEdit->setProperty("wtfmlChipHeight", true);

            // 四颗 chip **全都建着**（和下载页左栏那六个分类、文件页那四颗筛子同一套做法）：
            // 会变的是"露不露"和"亮不亮"，不是"有没有这一颗"—— 建/不建会让"这一页该有什么"
            // 散成两处知识。四颗恒露着（「可更新」那一颗**不**按"查过没有"收起来：
            // 它是跨实例的筛子，收起来就没人知道"还能按更新筛"，见
            // `MainWindow::rebuildInstanceTree()` 里那段 —— 那条规矩是**文件页**的）。
            for (int i = 0; i < 4; ++i) {
                const auto which = static_cast<InstanceFilterButton>(i);
                auto *button = new QPushButton(instanceFilterText(which), instanceFilterRow);
                button->setObjectName(instanceFilterButtonName(which));
                button->setCheckable(true);
                button->setToolTip(instanceFilterTipText(which));
                // chip 那一档（24px 高、圆角、勾上是实底主题色）—— **和资源详情页那排
                // 游戏版本同一处出处**（`Theme::markChipButton()`）。
                Theme::markChipButton(button);
                instanceFilterButtons.append(button);
                row->addWidget(button);
            }
            row->addStretch(1);
            row->addWidget(instanceSearchEdit);
        }
        clientLayout->addWidget(instanceFilterRow);
        // Tab 顺序跟着设计稿的版面走（左→右）：页头胶囊 → 导入 → 分组设置；四颗 chip → 搜索框。
        QWidget::setTabOrder(folderPill, importModpackButton);
        QWidget::setTabOrder(importModpackButton, groupInstanceButton);
        QWidget::setTabOrder(instanceFilterButtons.last(), instanceSearchEdit);

        // 「没有符合筛选条件的实例」那一行：摆在这儿（筛选行下面、树上面）而不是树的
        // 下边 —— 筛空了的时候树是空的，那句话直接跟着筛选行读下来才对得上"是上面那些
        // 条件把它滤没的"。平时收着（`setVisible(false)`，连位置都不占）。
        instanceNoMatchLabel = new QLabel(instanceNoMatchText(), clientPage);
        instanceNoMatchLabel->setObjectName(instanceNoMatchLabelName());
        markTextRole(instanceNoMatchLabel, Theme::secondaryTextRoleValue());
        instanceNoMatchLabel->setAlignment(Qt::AlignCenter);
        instanceNoMatchLabel->setWordWrap(true);
        // 和别处那些说明文字同一条规矩：纯说明**不许**参与横向度量（折行 QLabel 报出来的
        // sizeHint 是"整句排成一行"的宽度）。
        instanceNoMatchLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        instanceNoMatchLabel->setVisible(false);
        clientLayout->addWidget(instanceNoMatchLabel);
        // 空文件夹那一句：同一个位置、同一档次要色，只是说的是另一件事（见 instanceEmptyText()）。
        // 设计稿 manage-empty：插画（不画虚线圈）+ 大标题 + 一句引导 + 一颗主操作，整块居中；
        // MainWindow::rebuildInstanceTree() 按"空文件夹且没在筛"开关整块（标签跟着容器）。
        auto *emptyState = new QWidget(clientPage);
        emptyState->setObjectName(instanceEmptyStateName());
        emptyState->setVisible(false);
        auto *emptyLayout = new QVBoxLayout(emptyState);
        emptyLayout->setContentsMargins(0, 0, 0, 40);
        emptyLayout->setSpacing(10);
        emptyLayout->addStretch(1);
        auto *emptyArt = new QLabel(emptyState);
        emptyArt->setAlignment(Qt::AlignCenter);
        const auto paintArt = [emptyArt]() {
            QPixmap art(160, 140);
            art.fill(Qt::transparent);
            QPainter p(&art);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.drawPixmap(68, 12, VersionIcons::isoBlock(4, 30));   // 沙
            p.drawPixmap(26, 46, VersionIcons::isoBlock(0, 62));   // 草
            p.drawPixmap(72, 66, VersionIcons::isoBlock(6, 46));   // 石
            emptyArt->setPixmap(art);
        };
        paintArt();
        Theme::follow(emptyArt, paintArt);
        emptyLayout->addWidget(emptyArt, 0, Qt::AlignHCenter);
        auto *emptyTitle = new QLabel(instanceEmptyTitleText(), emptyState);
        emptyTitle->setAlignment(Qt::AlignCenter);
        {
            Theme::markDisplay(emptyTitle, Theme::textFont(emptyTitle->font(), Theme::TextRole::Empty));
        }
        emptyLayout->addWidget(emptyTitle);
        instanceEmptyLabel = new QLabel(instanceEmptyText(), emptyState);
        instanceEmptyLabel->setObjectName(instanceEmptyLabelName());
        markTextRole(instanceEmptyLabel, Theme::secondaryTextRoleValue());
        instanceEmptyLabel->setAlignment(Qt::AlignCenter);
        instanceEmptyLabel->setWordWrap(true);
        instanceEmptyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        instanceEmptyLabel->setVisible(false);
        emptyLayout->addWidget(instanceEmptyLabel);
        auto *emptyActions = new QHBoxLayout;
        emptyActions->setSpacing(10);
        emptyActions->addStretch(1);
        auto *emptyDownload = new QPushButton(instanceEmptyDownloadText(), emptyState);
        Theme::markButton(emptyDownload, Theme::ButtonKind::Primary);
        QObject::connect(emptyDownload, &QPushButton::clicked, emptyState,
                         [this]() { tabWidget->setCurrentIndex(downloadTabIndex()); });
        emptyActions->addWidget(emptyDownload);
        // 「导入整合包…」只留页头右上那一颗（有无实例都在），空状态里不再重复放一颗。
        emptyActions->addStretch(1);
        emptyLayout->addSpacing(8);
        emptyLayout->addLayout(emptyActions);
        emptyLayout->addStretch(2);
        clientLayout->addWidget(emptyState, 1);

        instanceList = new QTreeWidget(clientPage);
        instanceList->setObjectName(instanceListName());
        instanceList->setHeaderHidden(true);
        // 这一棵树**自己那圈边不画**：它四周已经是正文那个面（`workspace`），
        // 再套一圈就是第一条要减掉的"框套框"。行本身（组行 / 实例行）都是自绘的，
        // 有没有外框在观感上差的是"一块面板"还是"一串行"。
        instanceList->setFrameShape(QFrame::NoFrame);
        // 左边**不画** Qt 自带那个展开三角：展开/收起这件事画在行的**右**端（一个箭头，
        // 见 InstanceGroupDelegate），和版本树那几条分类长得一样（组行和版本下载那一套
        // 几乎一致）。开着的话一行上会有两个箭头，而且左边那个还会把组名往里推。
        // NOTE: 正路就是 setRootIsDecorated(false) —— 拿 `DontShowIndicator` 那条 item flag
        // 顶替会让整条分类的子条目一起消失（版本树那边踩过）。
        instanceList->setRootIsDecorated(false);
        // 组头 36px、实例行 60px 两档高度，不能开 uniformRowHeights（会全按第一行的组头算）。
        instanceList->setUniformRowHeights(false);
        instanceList->setIndentation(0);  // 设计稿：组内实例行和组头左缘对齐
        instanceList->viewport()->setAttribute(Qt::WA_Hover, true);
        instanceList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        // 分组头那一行不是默认画法：组名粗体、右边一个箭头 + 一颗齿轮（"点一下打开/关闭"
        // 和"这一组的更多操作"都画在那两个格里）。代理只接管挂着 groupIdRole() 的行，
        // 实例行原样走基类（见 InstanceGroupDelegate 文件头）。
        instanceList->setItemDelegate(new InstanceGroupDelegate(instanceList));
        // 实例行前面那个图标（原版草方块 / Forge 铁砧 / Fabric…）：**这一份树必须设
        // iconSize** —— 不设就用样式给的默认值，而图标是按 instanceTreeIconSize() 画的，
        // 会被缩到默认那一档、和实例名那行字挤在一起。
        //
        // 这个边长是**实例列表自己**那一档（26，版本树那一档是 18）。它和
        // MainWindow::refreshInstanceList() 里现算那些
        // QPixmap 时递进去的边长**必须同一个数** —— 两处对不上的症状是图标被 QIcon
        // 重新采样一遍（屏幕上只是略糊，验证工程里那条"行首那张图 == 现画的那张"会红）。
        instanceList->setIconSize(QSize(instanceTreeIconSize(), instanceTreeIconSize()));
        // 悬停那两颗按钮（上箭头 / 垃圾桶）**必须**有这一句，而且它是缺一不可的两半里的
        // 一半（另一半是代理里那个 `&& hovered`，见 InstanceGroupDelegate::paint()）：
        // 干净的视图 `WA_Hover=0` / `mouseTracking=0`，**真鼠标移动一个字都不改**
        // `QStyle::State_MouseOver`（下载页左栏那六个刷新圈就是这个判据）。少这一句的
        // 表现是"那两颗按钮再也不出现"，而两边
        // 各自的断言还都是绿的。
        instanceList->setMouseTracking(true);
        // 展开/折叠动画：组行那一按，子条目是**滑**出来的。交给 Qt 自己
        // （`QTreeView::setAnimated`），规矩见 ui/Animations.hpp。
        Anim::animateExpansion(instanceList);
        clientLayout->addWidget(instanceList, 1);

        // ---------- 顶部模式切换 + 服务端那一页 ----------
        //
        // 这一排是这一页**最上面**那一行：两颗按钮（「客户端实例」/「服务端」），下面那个
        // `QStackedWidget` 按它换页。摆进正文面里（不是页头右上角那排页面级操作）—— 它
        // 切的是"下面这一页说的是什么"，和「分组设置…」不是一类东西。
        {
            auto *modeRow = new QWidget(instance.right.surface);
            modeRow->setObjectName(instanceModeRowName());
            auto *modeLayout = new QHBoxLayout(modeRow);
            modeLayout->setContentsMargins(0, 0, 0, 0);
            modeLayout->setSpacing(kFilterRowSpacing);
            for (int i = 0; i < 3; ++i) {
                const auto mode = static_cast<InstanceMode>(i);
                auto *button = new QPushButton(instanceModeText(mode), modeRow);
                button->setObjectName(instanceModeButtonName(mode));
                button->setCheckable(true);
                button->setToolTip(instanceModeTipText(mode));
                // chip 那一档（和筛选那几颗同一处出处）。**互斥**由 `MainWindow` 那一层认
                // （点一颗就把另一颗的勾摘掉）—— 这里不挂 `setAutoExclusive`：那是"单选钮"
                // 那一套，和这一页别处的手感不一致（见 InstanceFilterButton 那段）。
                instanceModeButtons.append(button);
            }
            // 设计稿分段控件（客户端实例 / 服务端 / 预设）：一个容器包三段。
            auto *segment = new QWidget(modeRow);
            auto *segmentLayout = new QHBoxLayout(segment);
            segmentLayout->setContentsMargins(3, 3, 3, 3);
            segmentLayout->setSpacing(2);
            for (QPushButton *button : std::as_const(instanceModeButtons)) segmentLayout->addWidget(button);
            Theme::markSegmented(segment, instanceModeButtons);
            modeLayout->insertWidget(0, segment);
            // 两颗靠左，剩下的空当由弹簧吃掉。
            modeLayout->addStretch(1);
            instance.right.layout->addWidget(modeRow);
        }
        // 焦点链：左栏底下那两颗按钮 → 这一排模式 → 搜索框。
        // 这一排是这一页**最上面**那一行，可它是**最后**建的（筛选行 / 树那一整套先建，
        // 这一排是最后建的），于是按 Tab 的顺序是"搜索框 → 四颗筛子 → 树 → 模式按钮"
        // ——焦点从下面那棵树又窜回到最上面这一行。串成"左栏 → 模式 → 搜索"，一路往下。
        // （左栏那两颗是建在这一排之前的，所以这里从它们接出去。）
        if (importModpackButton != nullptr && instanceModeButtons.size() == 3) {
            QWidget::setTabOrder(importModpackButton, instanceModeButtons.at(0));
            QWidget::setTabOrder(instanceModeButtons.at(0), instanceModeButtons.at(1));
            QWidget::setTabOrder(instanceModeButtons.at(1), instanceModeButtons.at(2));
            if (instanceSearchEdit != nullptr) {
                QWidget::setTabOrder(instanceModeButtons.at(2), instanceSearchEdit);
            }
        }

        instanceModeStack = new QStackedWidget(instance.right.surface);
        instanceModeStack->setObjectName(instanceModeStackName());
        instanceModeStack->addWidget(clientPage);  // 第 0 页 = 客户端实例

        // ---- 第 1 页：服务端 ----
        //
        // "一行一个服务端"的一份清单（灌数据在 MainWindow::refreshServerList()）。这一页
        // 是**平铺的**（服务端没有分组），所以条目就是最上面一层、没有组头。
        QWidget *serverPage = new QWidget(instanceModeStack);
        serverPage->setObjectName(QStringLiteral("instanceServerPage"));
        auto *serverLayout = new QVBoxLayout(serverPage);
        serverLayout->setContentsMargins(0, 0, 0, 0);
        serverLayout->setSpacing(kFilterRowSpacing);

        serverEmptyLabel = new QLabel(serverEmptyText(), serverPage);
        serverEmptyLabel->setObjectName(serverEmptyLabelName());
        markTextRole(serverEmptyLabel, Theme::secondaryTextRoleValue());
        serverEmptyLabel->setAlignment(Qt::AlignCenter);
        serverEmptyLabel->setWordWrap(true);
        serverEmptyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        serverEmptyLabel->setVisible(false);
        serverLayout->addWidget(serverEmptyLabel);

        serverList = new QTreeWidget(serverPage);
        serverList->setObjectName(serverListName());
        serverList->setHeaderHidden(true);
        serverList->setFrameShape(QFrame::NoFrame);
        serverList->setRootIsDecorated(false);
        serverList->setUniformRowHeights(true);
        serverList->setSelectionMode(QAbstractItemView::SingleSelection);
        // 和客户端列表同一种行：自绘代理（悬停三格），一列撑满，窄窗靠省略号收字、不出横向滚动条。
        ListRowDelegate::install(serverList);
        serverList->setIndentation(0);
        serverList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        serverList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        // 右键菜单与行尾「更多」是同一份（ServerMenuController）。
        serverList->setContextMenuPolicy(Qt::CustomContextMenu);
        ItemViewContextMenu::install(serverList);  // 菜单键 / Shift+F10：弹在当前行
        serverList->setMouseTracking(true);
        serverLayout->addWidget(serverList, 1);

        newServerButton = new QPushButton(newServerText(), serverPage);
        newServerButton->setObjectName(newServerButtonName());
        // 造一个新东西出来 = 这一页的主操作，走主题色那一档（和左栏「添加文件夹…」同一档）。
        Theme::markButton(newServerButton, Theme::ButtonKind::Accent);
        serverLayout->addWidget(newServerButton);

        instanceModeStack->addWidget(serverPage);  // 第 1 页 = 服务端

        // ---- 第 2 页：预设----
        //
        // 只是一个**宿主**：整页的内容（列表 / 提取 / 计划）由 `PresetController` 建好塞进来。
        // 这一层不 include 预设那一层，也不该知道它长什么样。
        instancePresetPage = new QWidget(instanceModeStack);
        instancePresetPage->setObjectName(instancePresetPageName());
        auto *presetHostLayout = new QVBoxLayout(instancePresetPage);
        presetHostLayout->setContentsMargins(0, 0, 0, 0);
        presetHostLayout->setSpacing(0);
        instanceModeStack->addWidget(instancePresetPage);  // 第 2 页 = 预设

        instance.right.layout->addWidget(instanceModeStack, 1);
        // 起手停在第 0 页（客户端实例）—— 顺带把那两颗按钮的勾摆正（`setInstanceMode()`
        // 是"停在哪一页 + 谁勾着"的唯一出处）。
        setInstanceMode(InstanceMode::Client);

        // 这一排的三颗按钮已经分别有入口：
        // · 「实例设置…」= **点这一条实例行**（空白、左边那个图标、版本名都算），
        //   行尾还会常显一颗齿轮 —— 两颗做的是同一件事；
        // · 「重命名…」在「实例设置」那一页（改实例名就在那一页里改）；
        // · 「分组设置…」是框**外面**的右上角那颗页面级操作（见那一段 NOTE）。
        //
        // 「停止游戏…」归底栏左面板里那一行的电源（见 MainWindow::stopGameSession()）：
        // 那一格写着"哪一个会话在跑"，停哪一份比在"对选中实例做什么"的排里更说得清。
        // 它只停**这一份**（会话号）—— 多开的那几份要逐个停。
        // 「导入整合包…」在左栏「游戏文件夹」那一组（在「添加文件夹…」下面），那一组
        // 就是侧栏本身，不是框。
        // 正文那一格由上面那棵树吃满 —— 它上面再没有别的东西了（页头在面外面）。
    }
    return instanceTab;
}

/// 下载那一页：左 = 一列分类（游戏 + 五类资源 + 服务端）；右 = 那个分类的内容。
///
/// 「游戏」那一页是版本树 + 安装页；其余几页在下面那段循环里搭（搜索框 + 三层树）。


void MainWindowUI::showInstanceFolderText() {
    if (instanceFolderLabel == nullptr) return;
    const QString &text = instanceMode == InstanceMode::Preset   ? presetSubtitleText
                          : instanceMode == InstanceMode::Server ? serverFolderText
                                                                 : instanceFolderText;
    instanceFolderLabel->setText(text);
    instanceFolderLabel->setToolTip(text);  // 路径被省略时，提示里是全文
}

void MainWindowUI::setInstanceFolderText(const QString &text) {
    instanceFolderText = text;
    showInstanceFolderText();
    if (tabWidget != nullptr)
        if (auto *pill = tabWidget->findChild<QPushButton *>(instanceFolderPillName())) pill->setToolTip(text);
}

void MainWindowUI::setServerFolderText(const QString &text) {
    serverFolderText = text;
    showInstanceFolderText();
}

void MainWindowUI::setPresetSubtitle(const QString &text) {
    presetSubtitleText = text;
    showInstanceFolderText();
}

void MainWindowUI::setServerFormIntro(const QString &text) {
    if (serverFormIntroLabel != nullptr) {
        serverFormIntroLabel->setText(text);
    }
}

void MainWindowUI::setInstanceMode(InstanceMode mode) {
    if (navigationGuard && !navigationGuard()) {
        const int current = instanceModeStack ? instanceModeStack->currentIndex() : 0;
        for (int i=0;i<instanceModeButtons.size();++i)
            if (instanceModeButtons[i]) instanceModeButtons[i]->setChecked(i==current);
        return;
    }
    const int index = static_cast<int>(mode);
    if (instanceModeStack == nullptr || index < 0 || index >= instanceModeStack->count()) {
        return;  // 界面没搭出来 / 越界：什么都不改（见头文件那段）
    }
    // 三颗按钮的勾和停着的那一页**一起**改（这是这个函数存在的全部意义）。
    instanceModeStack->setCurrentIndex(index);
    // Presets live in the selected game folder too (they serve that folder's instances),
    // so the game folder pill stays visible in every mode.
    if (auto *page = tabWidget->widget(instancesTabIndex())) {
        if (auto *title = page->findChild<QLabel *>(pageTitleName(QStringLiteral("instance"))))
            title->setText(mode==InstanceMode::Preset ? QCoreApplication::translate("MainWindowUI", "预设") : QCoreApplication::translate("MainWindowUI", "实例"));
    }
    if (groupInstanceButton) groupInstanceButton->setVisible(mode != InstanceMode::Preset);
    // 导入整合包只在客户端 / 服务端标签露面，预设标签收起。
    if (importModpackButton) importModpackButton->setVisible(mode != InstanceMode::Preset);
    instanceMode = mode;
    // 页头路径行三个标签都有：换档只是换一份底稿。
    showInstanceFolderText();
    for (int i = 0; i < instanceModeButtons.size(); ++i) {
        if (instanceModeButtons.at(i) != nullptr) {
            instanceModeButtons.at(i)->setChecked(i == index);
        }
    }
}




void MainWindowUI::setHomeQuickRows(const QList<HomeQuickItem> &current,
                                    const QList<HomeQuickItem> &favorites,
                                    const QList<HomeQuickItem> &recent) {
    const auto clearRow = [](QHBoxLayout *row) {
        while (QLayoutItem *child = row->takeAt(0)) {
            delete child->widget();
            delete child;
        }
    };
    // ---------- 左下启动区（设计稿 r2 home.html .launch）----------
    // 「当前实例」眉题 + 实例名（手写 40）+ 元信息一行，三样合成一块可点的面（点开实例切换弹层）；
    // 下面「启动游戏」+ 实例设置。一个实例都没有时：「还没有实例」+「下载游戏」。
    if (homeQuickCurrentSection != nullptr && homeQuickCurrentRow != nullptr) {
        closeHomeInstancePicker();  // 它贴着的那一块马上要重建
        if (m_launchRow != nullptr) {
            m_launchRow->hide();
            m_launchRow->setParent(homeQuickCurrentSection);  // 卡片那一行不跟着重建，W 的动画不断
        }
        clearRow(homeQuickCurrentRow);
        m_homeCurrentItem = current.isEmpty() ? HomeQuickItem{} : current.first();
        if (!current.isEmpty()) {
            const HomeQuickItem &item = current.first();
            auto *card = new QWidget(homeQuickCurrentSection);
            card->setObjectName(QStringLiteral("homeCurrentInstanceCard"));
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto *column = new QVBoxLayout(card);
            // 底下留 kHomeLaunchGlowRoom：主按钮那圈柔光画在按钮外面，卡片边就是裁切边。
            column->setContentsMargins(0, 0, 0, kHomeLaunchGlowRoom);
            column->setSpacing(0);
            QWidget *block = nullptr;
            if (item.empty) {
                block = new QWidget(card);
            } else {
                auto *picker = new HomeInstBlock(card);
                picker->setObjectName(homeCurrentPickerName());
                picker->setAccessibleName(QCoreApplication::translate("MainWindowUI", "切换实例：%1").arg(item.name));
                QObject::connect(picker, &QAbstractButton::clicked, picker, [this, key = item.key]() {
                    if (m_homeQuickActionHandler) m_homeQuickActionHandler(key, HomeQuickAction::SwitchInstance);
                });
                block = picker;
            }
            block->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            auto *blockColumn = new QVBoxLayout(block);
            // 左边只留 kHomeHeroInset：文字和上面的大标题对齐，悬停底往左多出这一截（样张是 14）。
            blockColumn->setContentsMargins(kHomeHeroInset, 6, 14, 10);
            blockColumn->setSpacing(0);
            if (!item.empty) {
                auto *eyebrow = new QLabel(QCoreApplication::translate("MainWindowUI", "当前实例"), block);
                eyebrow->setObjectName(QStringLiteral("homeCurrentEyebrow"));
                QFont eyebrowFont = eyebrow->font();
                eyebrowFont.setPixelSize(13);
                eyebrowFont.setWeight(QFont::Medium);
                eyebrow->setFont(eyebrowFont);
                eyebrow->setAttribute(Qt::WA_TransparentForMouseEvents);
                // 先装：过滤器后装的先跑，followHeroInk 刷完 heroInk 之后这一条再按失败涂回危险色。
                Theme::follow(eyebrow, [this]() { syncLaunchCard(); });
                followHeroInk(eyebrow, true);
                blockColumn->addWidget(eyebrow);
            }
            auto *name = new ElidedLabel(block);
            name->setObjectName(QStringLiteral("homeCurrentName"));
            name->setText(item.empty ? QCoreApplication::translate("MainWindowUI", "还没有实例") : item.name);
            name->setMinimumWidth(1);
            // 零实例：「还没有实例」只是一句状态，用普通字阶（16），大标题才是这一屏唯一的手写大字。
            name->setMinimumHeight(item.empty ? 28 : 52);
            if (item.empty) {
                name->setFont(Theme::textFont(name->font(), Theme::TextRole::Heading));
            } else {
                Theme::markDisplay(name, handPx(name->font(), Theme::HandFont::Kai, kHomeCurrentNameMaxPixelSize, 3));
                // 长名先收字号，到下限还放不下才收省略号；行高按上限钉死，下面那行和按钮不挪。
                name->setFitPixelRange(kHomeCurrentNameMaxPixelSize, kHomeCurrentNameMinPixelSize);
            }
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            followHeroInk(name, false);
            blockColumn->addWidget(name);
            if (!item.empty) {
                // 设计稿 .meta：行首一枚加载器小图标（没装加载器 = 草方块，和 loaderMark() 越界一条规矩）。
                auto *metaRow = new QHBoxLayout;
                metaRow->setContentsMargins(0, 4, 0, 0);
                metaRow->setSpacing(6);
                const QString versionLine = item.detail.section(QLatin1Char('\n'), 0, 0);
                const QString loader = versionLine.section(QStringLiteral(" / "), 1).trimmed();
                const QStringList loaderNames = modLoaderOptionTexts();
                int loaderIndex = loaderNames.size();
                for (int i = 0; i < loaderNames.size(); ++i) {
                    if (!loader.isEmpty() && loader.startsWith(loaderNames.at(i), Qt::CaseInsensitive)) {
                        loaderIndex = i;
                        break;
                    }
                }
                auto *loaderIcon = new QLabel(block);
                loaderIcon->setObjectName(QStringLiteral("homeCurrentLoaderIcon"));
                loaderIcon->setFixedSize(14, 14);
                loaderIcon->setScaledContents(true);
                // 标志跟着主题走（深色主题下 Forge 那把铁砧是浅色那版，见 VersionIcons::loaderMark）。
                const auto applyLoaderMark = [loaderIcon, loaderIndex]() {
                    loaderIcon->setPixmap(VersionIcons::loaderMark(loaderIndex, 28));
                };
                applyLoaderMark();
                Theme::follow(loaderIcon, applyLoaderMark);
                loaderIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
                metaRow->addWidget(loaderIcon, 0, Qt::AlignVCenter);
                auto *meta = new ElidedLabel(block);
                meta->setObjectName(QStringLiteral("homeCurrentVersion"));
                meta->setText(homeLaunchMeta(item));
                meta->setMinimumWidth(1);
                QFont metaFont = meta->font();
                metaFont.setPixelSize(13);
                meta->setFont(metaFont);
                meta->setAttribute(Qt::WA_TransparentForMouseEvents);
                followHeroInk(meta, true);
                metaRow->addWidget(meta, 1);
                blockColumn->addLayout(metaRow);
            }
            column->addWidget(block, 0, Qt::AlignLeft);
            column->addSpacing(item.empty ? 20 : 18);
            auto *actions = new QHBoxLayout;
            actions->setContentsMargins(kHomeHeroInset, 0, 0, 0);
            actions->setSpacing(10);
            if (!item.empty) {
                if (m_launchRow == nullptr) {
                    auto *row = new HomeLaunchRow(card);
                    QObject::connect(row->pill, &QPushButton::clicked, row, [this]() {
                        onLaunchCardAction(LaunchCardAction::Cancel);
                    });
                    QObject::connect(row->details, &QPushButton::clicked, row, [this]() {
                        onLaunchCardAction(LaunchCardAction::Details);
                    });
                    m_launchRow = row;
                } else {
                    m_launchRow->setParent(card);
                }
                m_launchRow->hide();
                actions->addWidget(m_launchRow);  // 显隐由 syncLaunchCard() 管：卡片和下面两颗按钮二选一
            }
            auto *launch = new QPushButton(card);
            launch->setObjectName(QStringLiteral("homeCurrentLaunch"));
            launch->setProperty("wtfmlHeroStart", true);
            launch->setProperty("wtfmlHomeEmpty", item.empty);
            launch->setMinimumWidth(220);
            // 按自己的宽度来（设计稿 min-width 220），不吃掉这一行剩下的空。
            launch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            QFont launchFont = launch->font();
            launchFont.setPixelSize(16);
            launchFont.setWeight(QFont::DemiBold);
            launch->setFont(launchFont);
            Theme::markButton(launch, Theme::ButtonKind::Primary);
            const auto applyGlow = [launch]() {
                // 设计稿：主按钮下面一圈强调色的柔光（box-shadow 0 10 30 -8 accent-fill）。
                auto *glow = qobject_cast<QGraphicsDropShadowEffect *>(launch->graphicsEffect());
                if (glow == nullptr) {
                    glow = new QGraphicsDropShadowEffect(launch);
                    launch->setGraphicsEffect(glow);
                }
                QColor color = Theme::tokens().primary;
                color.setAlphaF(0.55);
                glow->setColor(color);
                glow->setBlurRadius(30);
                glow->setOffset(0, 10);
            };
            applyGlow();
            Theme::follow(launch, applyGlow);
            Theme::follow(launch, [this]() { syncHomeLaunchButton(); });
            QObject::connect(launch, &QPushButton::clicked, launch, [this, launch, key = item.key]() {
                // 动作看按钮此刻是哪一档（syncHomeLaunchButton 写在属性里）：启动 / 停止 / 下载 / 看进度。
                const QString mode = launch->property("wtfmlLaunchMode").toString();
                if (mode == QStringLiteral("download")) {
                    if (tabWidget != nullptr) tabWidget->setCurrentIndex(downloadTabIndex());
                } else if (mode == QStringLiteral("installing")) {
                    toggleActivityPopover();
                } else if (m_homeQuickActionHandler) {
                    m_homeQuickActionHandler(key, mode == QStringLiteral("stop") ? HomeQuickAction::Stop
                                                                               : HomeQuickAction::Launch);
                }
            });
            actions->addWidget(launch);
            if (!item.empty) {
                auto *edit = new HeroButton(HeroButton::Shape::Square, card);
                edit->setObjectName(QStringLiteral("homeCurrentEdit"));
                edit->setProperty("wtfmlHeroSquare", true);
                edit->setFixedSize(52, 52);
                edit->setToolTip(QCoreApplication::translate("MainWindowUI", "实例设置"));
                edit->setAccessibleName(edit->toolTip());
                edit->setIconSize(QSize(18, 18));
                const auto applyIcon = [edit]() {
                    edit->setIcon(QIcon(VersionIcons::line(QStringLiteral("gear"), 18, heroInk(false), 1.8)));
                };
                applyIcon();
                Theme::follow(edit, applyIcon);
                QObject::connect(edit, &QPushButton::clicked, edit, [this, key = item.key]() {
                    if (m_homeQuickActionHandler) m_homeQuickActionHandler(key, HomeQuickAction::Edit);
                });
                actions->addWidget(edit);
            }
            actions->addStretch(1);
            column->addLayout(actions);
            homeQuickCurrentRow->addWidget(card, 1);
            syncHomeLaunchButton();
            syncLaunchCard();
            card->layout()->activate();
            card->setFixedHeight(card->layout()->sizeHint().height());
        }
    }

    // ---------- 实例带（设计稿 .strip .card）----------
    // 196 宽，封面 48 + 名字 / 版本两行；平时无底，悬停（或键盘焦点）铺一层玻璃底并在右边出一颗
    // 播放钮。点卡片 = 设为当前实例；点播放钮 = 直接启动它。实例带不摆空卡。
    const auto rebuildRow = [this, &clearRow](QHBoxLayout *row, const QList<HomeQuickItem> &items) {
        if (row == nullptr) return;
        clearRow(row);
        for (const HomeQuickItem &item : items) {
            if (item.empty) continue;
            auto *card = new HeroButton(HeroButton::Shape::Tile, row->parentWidget());
            card->setObjectName(QStringLiteral("homeQuickCard_%1").arg(item.key));
            card->setAccessibleName(item.name);
            // QPushButton 的全局 QSS 也声明了 min-height；只写 setMinimumHeight() 会在 polish
            // 后被它压回普通按钮高度。属性选择器负责复位 QSS，这里再用布局尺寸表达卡片的
            // 高度。宽度**跟着名字走**（下面按内容量出来，196 下限、320 上限），尺寸策略是
            // Fixed —— 这一排**不把剩下的宽度分给卡片**，右边空着就空着。
            card->setProperty("wtfmlQuickCard", true);
            card->setProperty("wtfmlQuickCardSize", QStringLiteral("normal"));
            card->setFixedHeight(kHomeTileHeight);
            card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            Theme::markButton(card, Theme::ButtonKind::Neutral);

            auto *content = new QHBoxLayout(card);
            content->setContentsMargins(8, 0, 8, 0);
            content->setSpacing(12);
            auto *icon = new QLabel(card);
            icon->setFixedSize(48, 48);
            icon->setAttribute(Qt::WA_TransparentForMouseEvents);
            const auto applyCover = [icon, itemIcon = item.icon, name = item.name]() {
                icon->setPixmap(homeTileCover(itemIcon, name, 48));
            };
            applyCover();
            Theme::follow(icon, applyCover);
            content->addWidget(icon, 0, Qt::AlignVCenter);
            auto *identityWidget = new QWidget(card);
            auto *identity = new QVBoxLayout(identityWidget);
            identity->setSpacing(1);
            identity->setContentsMargins(0, 0, 0, 0);
            identity->addStretch(1);
            auto *nameRow = new QHBoxLayout;
            nameRow->setSpacing(6);
            auto *name = new ElidedLabel(card);
            name->setText(item.name);
            // 名字按自己宽度排，「当前」紧跟在后；放不下时名字先省略。
            name->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
            name->setMinimumWidth(1);
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            QFont nameFont = name->font();
            nameFont.setPixelSize(14);
            nameFont.setWeight(QFont::Medium);
            name->setFont(nameFont);
            followHeroInk(name, false);
            nameRow->addWidget(name);
            if (item.current) {
                auto *badge = new QLabel(QCoreApplication::translate("MainWindowUI", "当前"), card);
                badge->setObjectName(QStringLiteral("homeQuickCurrentBadge"));
                QFont badgeFont = badge->font();
                badgeFont.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
                badgeFont.setWeight(QFont::Medium);
                badge->setFont(badgeFont);
                badge->setAttribute(Qt::WA_TransparentForMouseEvents);
                const auto applyBadge = [badge]() {
                    QPalette p = badge->palette();
                    p.setColor(QPalette::WindowText, Theme::tokens().deep);
                    badge->setPalette(p);
                };
                applyBadge();
                Theme::follow(badge, applyBadge);
                nameRow->addWidget(badge);
            }
            nameRow->addStretch(1);
            identity->addLayout(nameRow);
            // ElidedLabel：QLabel 装不下是硬裁，版本串过长要收省略号；全名给卡片的提示。
            const QString subtitle = homeTileSubtitle(item.detail);
            auto *sub = new ElidedLabel(card);
            sub->setText(subtitle);
            sub->setObjectName(QStringLiteral("homeQuickTileVersion"));
            QFont subFont = sub->font();
            subFont.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
            sub->setFont(subFont);
            // 和名字一样按自己宽度排：卡片宽度取名字行与版本行里较宽的一行，放不下再收省略号。
            sub->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
            sub->setMinimumWidth(1);
            sub->setAttribute(Qt::WA_TransparentForMouseEvents);
            followHeroInk(sub, true);
            identity->addWidget(sub);
            identity->addStretch(1);
            content->addWidget(identityWidget, 1);
            auto *play = new HeroButton(HeroButton::Shape::Play, card);
            play->setObjectName(QStringLiteral("homeQuickPlay"));
            play->setProperty("wtfmlHeroPlay", true);
            play->setFixedSize(30, 30);
            QSizePolicy playPolicy = play->sizePolicy();
            playPolicy.setRetainSizeWhenHidden(true);
            play->setSizePolicy(playPolicy);
            play->setAccessibleName(QCoreApplication::translate("MainWindowUI", "启动 %1").arg(item.name));
            play->setFocusPolicy(Qt::NoFocus);  // 键盘：焦点在卡片上时它就露出来
            play->setIconSize(QSize(13, 13));
            const auto applyPlay = [play]() {
                play->setIcon(QIcon(VersionIcons::line(QStringLiteral("play"), 13, Theme::tokens().onPrimary, 2.0)));
            };
            applyPlay();
            Theme::follow(play, applyPlay);
            QObject::connect(play, &QPushButton::clicked, play, [this, key = item.key]() {
                if (m_homeQuickActionHandler) m_homeQuickActionHandler(key, HomeQuickAction::Launch);
            });
            content->addWidget(play, 0, Qt::AlignVCenter);
            card->setRevealOnHover(play);
            QObject::connect(card, &QPushButton::clicked, card, [this, key = item.key]() {
                if (m_homeQuickActionHandler) m_homeQuickActionHandler(key, HomeQuickAction::Select);
            });
            // 宽度按**内容**量：封面 + 名字（+「当前」）+ 播放钮。播放钮平时收着（悬停才露），
            // 收着时也留那一格（retainSizeWhenHidden），名字的可用宽度才不随悬停 / 按下 / 焦点变。
            // （以前手动加 `play->sizeHint().width()`：QPushButton 按图标算的是 17，它实际钉死
            // 30 宽，每次少算 13，名字刚好放下的卡片一悬停就收成省略号 —— issue #41。）
            const int compactWidth = content->contentsMargins().left() + content->contentsMargins().right()
                                     + icon->width() + content->spacing() + play->width();
            card->setStripWidths(identityWidget,
                qBound(kHomeTileWidth, content->sizeHint().width(), kHomeTileMaxWidth), compactWidth);
            card->setToolTip(subtitle.isEmpty() ? item.name : item.name + QLatin1Char('\n') + subtitle);
            row->addWidget(card);
        }
    };
    rebuildRow(homeFavoriteRow, favorites);
    rebuildRow(homeQuickRecentRow, recent);
    // 收藏/最近一张都没有时不留一颗点了只会看空白的标签；两样都没有时整条实例带收起来。
    const auto hasCards = [](const QList<HomeQuickItem> &items) {
        return std::any_of(items.cbegin(), items.cend(), [](const HomeQuickItem &i) { return !i.empty; });
    };
    if (homeQuickFavoriteTab != nullptr) homeQuickFavoriteTab->setVisible(hasCards(favorites));
    if (homeQuickRecentTab != nullptr) homeQuickRecentTab->setVisible(hasCards(recent));
    if (homeQuickSection != nullptr) homeQuickSection->setVisible(hasCards(favorites) || hasCards(recent));
    setHomeQuickMode(m_homeQuickMode);
}

void MainWindowUI::syncHomeQuickToggleText() {
    if (homeQuickToggle == nullptr) return;
    const QString text = m_homeQuickCollapsed
        ? QCoreApplication::translate("MainWindowUI", "展开最近与收藏")
        : QCoreApplication::translate("MainWindowUI", "收起最近与收藏");
    homeQuickToggle->setToolTip(text);
    homeQuickToggle->setAccessibleName(text);
}

void MainWindowUI::setHomeQuickCollapsed(bool collapsed, bool animate) {
    if (collapsed == m_homeQuickCollapsed) return;
    m_homeQuickCollapsed = collapsed;
    syncHomeQuickToggleText();
    if (auto *toggle = dynamic_cast<HomeStripToggle *>(homeQuickToggle)) toggle->setCollapsed(collapsed, animate);
    if (homeQuickFold != nullptr) homeQuickFold->setOpen(!collapsed, animate);
    if (m_homeQuickCollapsedHandler) m_homeQuickCollapsedHandler(collapsed);
}

void MainWindowUI::setHomeQuickMode(HomeQuickMode mode) {
    // 已经隐藏的类别不能成为当前页；这一层守住，设置更新和点击标签两条路都不会留下空面板。
    const bool recentShown = homeQuickRecentTab != nullptr && !homeQuickRecentTab->isHidden();
    const bool favoriteShown = homeQuickFavoriteTab != nullptr && !homeQuickFavoriteTab->isHidden();
    if (mode == HomeQuickMode::Favorite && !favoriteShown) {
        mode = HomeQuickMode::Current;
    }
    if (mode == HomeQuickMode::Recent && !recentShown) {
        mode = HomeQuickMode::Current;
    }
    m_homeQuickMode = mode;
    // 「当前」那一块是左下启动区，一直在；停在「当前」时实例带显示最近（没有最近就收藏）。
    HomeQuickMode strip = mode;
    if (strip == HomeQuickMode::Current) {
        strip = recentShown ? HomeQuickMode::Recent
                            : (favoriteShown ? HomeQuickMode::Favorite : HomeQuickMode::Current);
    }
    const auto apply = [strip](QPushButton *tab, QWidget *page, HomeQuickMode ownMode) {
        const bool active = strip == ownMode;
        if (tab != nullptr) {
            const QSignalBlocker blocker(tab);
            tab->setChecked(active);
            tab->setProperty("wtfmlHomeQuickTabActive", active);
            tab->update();
        }
        if (page != nullptr) {
            page->setVisible(active);
        }
    };
    apply(homeQuickCurrentTab, nullptr, HomeQuickMode::Current);
    apply(homeQuickRecentTab, homeQuickRecentSection, HomeQuickMode::Recent);
    apply(homeQuickFavoriteTab, homeFavoriteSection, HomeQuickMode::Favorite);
}

void MainWindowUI::setLaunchCard(const LaunchCard &card) {
    LaunchCard next = card;
    if (next.phase == LaunchCard::Phase::Failed && next.stage.isEmpty() && next.taskId == m_launchCard.taskId)
        next.stage = m_launchCard.stage;  // 失败那一刻没给阶段：停在上一份数据的那一段
    const bool firstSuccess = next.phase == LaunchCard::Phase::Succeeded
                              && !(m_launchCard.taskId == next.taskId && m_launchCard.phase == LaunchCard::Phase::Succeeded);
    m_launchCard = next;
    if (firstSuccess && homeQuickCurrentSection != nullptr) {
        QTimer::singleShot(launchCardSuccessHoldMs(), homeQuickCurrentSection, [this, id = next.taskId]() {
            if (m_launchCard.taskId != id || m_launchCard.phase != LaunchCard::Phase::Succeeded) return;
            m_launchCardDismissed = id;
            syncLaunchCard();
        });
    }
    syncLaunchCard();
}

bool MainWindowUI::launchCardShown() const {
    return m_launchRow != nullptr && m_launchCard.phase != LaunchCard::Phase::Idle
           && m_launchCard.taskId != m_launchCardDismissed && !m_homeCurrentItem.empty
           && !m_homeCurrentItem.key.isEmpty()
           && (m_launchCard.instanceKey.isEmpty() || m_launchCard.instanceKey == m_homeCurrentItem.key);
}

void MainWindowUI::syncLaunchCard() {
    if (homeQuickCurrentSection == nullptr) return;
    const bool shown = launchCardShown();
    const bool failed = shown && m_launchCard.phase == LaunchCard::Phase::Failed;
    if (auto *launch = homeQuickCurrentSection->findChild<QPushButton *>(QStringLiteral("homeCurrentLaunch")))
        launch->setVisible(!shown);
    if (auto *edit = homeQuickCurrentSection->findChild<QPushButton *>(QStringLiteral("homeCurrentEdit")))
        edit->setVisible(!shown);
    if (auto *row = static_cast<HomeLaunchRow *>(m_launchRow)) {
        if (shown) {
            row->apply(m_launchCard, m_homeCurrentItem.name);
        } else {
            row->mark->reset();
        }
        row->setVisible(shown);
    }
    if (auto *eyebrow = homeQuickCurrentSection->findChild<QLabel *>(QStringLiteral("homeCurrentEyebrow"))) {
        eyebrow->setText(!shown ? QCoreApplication::translate("MainWindowUI", "当前实例")
                         : failed ? QCoreApplication::translate("MainWindowUI", "启动失败")
                         : m_launchCard.phase == LaunchCard::Phase::Succeeded
                             ? QCoreApplication::translate("MainWindowUI", "游戏已启动")
                             : QCoreApplication::translate("MainWindowUI", "正在启动游戏"));
        const Theme::Tokens t = Theme::tokens();
        // 三个角色一起换（同 followHeroInk）：眉题在卡片按钮里，前景角色跟着父控件是 ButtonText。
        const QColor ink = failed ? Theme::textTone(t.dangerInk, t.danger) : heroInk(true);
        QPalette pal = eyebrow->palette();
        for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Link}) pal.setColor(role, ink);
        eyebrow->setPalette(pal);
    }
    if (auto *meta = static_cast<ElidedLabel *>(homeQuickCurrentSection->findChild<QLabel *>(QStringLiteral("homeCurrentVersion"))))
        meta->setText(homeLaunchMeta(m_homeCurrentItem, shown ? m_launchCard.login : QString()));
}

void MainWindowUI::onLaunchCardAction(LaunchCardAction action) {
    const quint64 id = m_launchCard.taskId;
    if (action == LaunchCardAction::Details) {
        if (m_launchCardHandler) m_launchCardHandler(id, action);
        return;
    }
    // 胶囊：加载中 = 取消（停掉这次启动），失败 = 关闭（只还原）。两种都当场还原成平常的启动行。
    const bool cancel = m_launchCard.phase == LaunchCard::Phase::Running;
    m_launchCardDismissed = id;
    syncLaunchCard();
    if (cancel && m_launchCardHandler) m_launchCardHandler(id, LaunchCardAction::Cancel);
}

void MainWindowUI::setHomeLaunchRunning(bool running) {
    if (m_homeLaunchRunning == running) return;
    m_homeLaunchRunning = running;
    syncHomeLaunchButton();
}

void MainWindowUI::syncHomeLaunchButton() {
    QPushButton *launch = homeQuickCurrentSection == nullptr
                              ? nullptr
                              : homeQuickCurrentSection->findChild<QPushButton *>(QStringLiteral("homeCurrentLaunch"));
    if (launch == nullptr) return;
    // 主按钮只做动作：有实例 = 启动 / 停止；没实例 = 下载游戏，正在装 = 安装中（点开看进度）。
    const bool empty = launch->property("wtfmlHomeEmpty").toBool();
    const BottomBarTaskSummary &task = m_lastTaskSummary;
    const bool installing = empty && task.hasTask
                            && (task.state == TaskCenter::State::Running || task.state == TaskCenter::State::Queued);
    QString mode, text, icon;
    if (!empty) {
        mode = m_homeLaunchRunning ? QStringLiteral("stop") : QStringLiteral("launch");
        text = m_homeLaunchRunning ? QCoreApplication::translate("MainWindowUI", "停止游戏") : QCoreApplication::translate("MainWindowUI", "启动游戏");
        icon = m_homeLaunchRunning ? QStringLiteral("power") : QStringLiteral("play");
    } else if (installing) {
        mode = QStringLiteral("installing");
        text = task.percent >= 0 ? QCoreApplication::translate("MainWindowUI", "安装中 %1%").arg(task.percent) : QCoreApplication::translate("MainWindowUI", "安装中");
        icon = QStringLiteral("download");
    } else {
        mode = QStringLiteral("download");
        text = QCoreApplication::translate("MainWindowUI", "下载游戏");
        icon = QStringLiteral("download");
    }
    launch->setProperty("wtfmlLaunchMode", mode);
    launch->setText(text);
    launch->setAccessibleName(text);
    const int side = icon == QStringLiteral("play") ? 18 : 20;
    const QColor ink = Theme::tokens().onPrimary;
    launch->setIcon(QIcon(icon == QStringLiteral("power") ? VersionIcons::power(side, ink)
                                                         : VersionIcons::line(icon, side, ink, 2.0)));
    launch->setIconSize(QSize(side, side));
}


