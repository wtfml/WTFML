/**
 * @file ToolboxPage.cpp
 * @brief 工具箱页的实现（目录页 → 工具页两层的形状见头文件）。
 */

#include "ToolboxPage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"
#include "FlowLayout.hpp"
#include "ArmorTrimPreview.hpp"
#include "DownloadToolPanel.hpp"
#include "GlassPopover.hpp"
#include "MotdText.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QStandardItemModel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

#include "MainWindowUI.hpp"
#include "Theme.hpp"

namespace {

/// 九件工具的那张表（**键 / 名字 / 目录里那一句的唯一出处**，次序 = 目录页与切换浮层的次序）。
struct ToolInfo {
    ToolboxPage::Tool tool;
    const char *key;
    const char *name;
    const char *summary;
};

const ToolInfo kToolInfos[] = {
    {ToolboxPage::Tool::Anvil, "anvil", QT_TRANSLATE_NOOP("ToolboxPage", "附魔助手"), QT_TRANSLATE_NOOP("ToolboxPage", "算最省等级的合并顺序")},
    {ToolboxPage::Tool::Coord, "coord", QT_TRANSLATE_NOOP("ToolboxPage", "坐标换算"), QT_TRANSLATE_NOOP("ToolboxPage", "主世界与下界坐标互换")},
    {ToolboxPage::Tool::Beacon, "beacon", QT_TRANSLATE_NOOP("ToolboxPage", "信标计算"), QT_TRANSLATE_NOOP("ToolboxPage", "底座方块与效果范围")},
    {ToolboxPage::Tool::Brewing, "brewing", QT_TRANSLATE_NOOP("ToolboxPage", "酿造助手"), QT_TRANSLATE_NOOP("ToolboxPage", "从水瓶起的配方与效果")},
    {ToolboxPage::Tool::Text, "text", QT_TRANSLATE_NOOP("ToolboxPage", "彩色文本"), QT_TRANSLATE_NOOP("ToolboxPage", "§ 文本与 JSON 互转")},
    {ToolboxPage::Tool::Banner, "banner", QT_TRANSLATE_NOOP("ToolboxPage", "旗帜设计"), QT_TRANSLATE_NOOP("ToolboxPage", "叠图案，算织布机步骤")},
    {ToolboxPage::Tool::Trim, "trim", QT_TRANSLATE_NOOP("ToolboxPage", "纹饰搭配"), QT_TRANSLATE_NOOP("ToolboxPage", "盔甲纹饰与材料搭配")},
    {ToolboxPage::Tool::Download, "download", QT_TRANSLATE_NOOP("ToolboxPage", "高速下载"), QT_TRANSLATE_NOOP("ToolboxPage", "直链下载，可断点续传")},
    {ToolboxPage::Tool::Server, "server", QT_TRANSLATE_NOOP("ToolboxPage", "延迟测试"), QT_TRANSLATE_NOOP("ToolboxPage", "服务器状态与延迟")},
};

const ToolInfo *infoOf(ToolboxPage::Tool tool) {
    for (const ToolInfo &info : kToolInfos) {
        if (info.tool == tool) {
            return &info;
        }
    }
    return &kToolInfos[0];
}

// ---------------------------------------------------------------------------
// 目录与切换浮层里的十枚线稿图标
// ---------------------------------------------------------------------------
//
// 现画，不落盘：48×48 的画布按 2 倍像素密度交出去（逻辑 24×24），线宽 3.4 在这个
// 坐标系里 = 屏幕上 1.7 px —— 和界面里正文笔画差不多粗细。色由调用方给：面板那边
// 拿主题色的 `Ink` 档（深色主题下换 `Pure`），不在这里另立一条配色。
//
// NOTE: 九件一件一个 switch 分支，**没有 default 省略** —— 以后往 `Tool` 里加一件，
// 编译器会在这里点名。
QIcon drawToolIcon(ToolboxPage::Tool tool, const QColor &ink) {
    constexpr int kSide = 48;
    QPixmap pixmap(kSide, kSide);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(ink, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (tool) {
    case ToolboxPage::Tool::Download:
        // 往托盘里落一支箭头。
        painter.drawLine(QPointF(24, 6), QPointF(24, 27));
        painter.drawLine(QPointF(16, 20), QPointF(24, 28));
        painter.drawLine(QPointF(32, 20), QPointF(24, 28));
        painter.drawLine(QPointF(10, 30), QPointF(10, 39));
        painter.drawLine(QPointF(10, 39), QPointF(38, 39));
        painter.drawLine(QPointF(38, 39), QPointF(38, 30));
        break;
    case ToolboxPage::Tool::Server:
        // 两层服务器机架，和坐标定位/无线网络区分。
        painter.drawRoundedRect(QRectF(9, 9, 30, 12), 2, 2);
        painter.drawRoundedRect(QRectF(9, 27, 30, 12), 2, 2);
        painter.drawPoint(QPointF(15, 15));
        painter.drawPoint(QPointF(15, 33));
        painter.drawLine(QPointF(25, 15), QPointF(33, 15));
        painter.drawLine(QPointF(25, 33), QPointF(33, 33));
        break;
    case ToolboxPage::Tool::Anvil:
        // 铁砧：平整工作面、收腰底座和独立砧角。
        {
            QPainterPath anvil;
            anvil.moveTo(9, 12); anvil.lineTo(39, 12);
            anvil.lineTo(36, 21); anvil.lineTo(29, 25);
            anvil.lineTo(29, 32); anvil.lineTo(35, 38);
            anvil.lineTo(13, 38); anvil.lineTo(19, 32);
            anvil.lineTo(19, 25); anvil.lineTo(9, 22);
            anvil.lineTo(5, 17); anvil.lineTo(9, 17);
            anvil.closeSubpath(); painter.drawPath(anvil);
        }
        break;
    case ToolboxPage::Tool::Coord:
        // 地图针：一滴 + 中间一个洞。
        {
            QPainterPath pin;
            pin.moveTo(24, 41);
            pin.cubicTo(13, 29, 12, 22, 12, 18);
            pin.cubicTo(12, 11, 17, 7, 24, 7);
            pin.cubicTo(31, 7, 36, 11, 36, 18);
            pin.cubicTo(36, 22, 35, 29, 24, 41);
            painter.drawPath(pin);
            painter.drawEllipse(QPointF(24, 18), 4.5, 4.5);
        }
        break;
    case ToolboxPage::Tool::Text:
        // 文字 T + 色卡，减少小尺寸中的碎点。
        painter.drawLine(QPointF(9, 11), QPointF(35, 11));
        painter.drawLine(QPointF(22, 11), QPointF(22, 32));
        painter.drawLine(QPointF(16, 32), QPointF(28, 32));
        painter.drawRoundedRect(QRectF(9, 38, 30, 4), 1, 1);
        painter.drawLine(QPointF(19, 38), QPointF(19, 42));
        painter.drawLine(QPointF(29, 38), QPointF(29, 42));
        break;
    case ToolboxPage::Tool::Brewing:
        // 圆底烧瓶 + 液面。
        painter.drawLine(QPointF(18, 7), QPointF(30, 7));
        painter.drawLine(QPointF(20, 7), QPointF(20, 16));
        painter.drawLine(QPointF(28, 7), QPointF(28, 16));
        {
            QPainterPath flask;
            flask.moveTo(20, 16);
            flask.lineTo(12, 32);
            flask.quadTo(10, 41, 24, 41);
            flask.quadTo(38, 41, 36, 32);
            flask.lineTo(28, 16);
            painter.drawPath(flask);
            painter.drawLine(QPointF(16, 29), QPointF(32, 29));
        }
        break;
    case ToolboxPage::Tool::Banner:
        // Minecraft 竖旗，而非横向的燕尾旗。
        painter.drawLine(QPointF(24, 32), QPointF(24, 42));
        painter.drawLine(QPointF(17, 42), QPointF(31, 42));
        painter.drawLine(QPointF(10, 8), QPointF(38, 8));
        {
            QPainterPath flag;
            flag.moveTo(13, 8);
            flag.lineTo(35, 8);
            flag.lineTo(35, 34);
            flag.lineTo(24, 29);
            flag.lineTo(13, 34);
            flag.closeSubpath();
            painter.drawPath(flag);
        }
        break;
    case ToolboxPage::Tool::Beacon:
        // 发光立方体和阶梯底座。
        painter.drawLine(QPointF(24, 5), QPointF(24, 11));
        painter.drawRect(QRectF(18, 16, 12, 12));
        painter.drawLine(QPointF(12, 34), QPointF(36, 34));
        painter.drawLine(QPointF(7, 41), QPointF(41, 41));
        break;
    case ToolboxPage::Tool::Trim:
        // 一件上衣：盔甲纹饰。
        {
            QPainterPath shirt;
            shirt.moveTo(18, 9);
            shirt.lineTo(10, 11);
            shirt.lineTo(10, 22);
            shirt.lineTo(15, 25);
            shirt.lineTo(15, 39);
            shirt.lineTo(33, 39);
            shirt.lineTo(33, 25);
            shirt.lineTo(38, 22);
            shirt.lineTo(38, 11);
            shirt.lineTo(30, 9);
            shirt.cubicTo(28, 13, 20, 13, 18, 9);
            shirt.closeSubpath();
            painter.drawPath(shirt);
            painter.drawLine(QPointF(20, 23), QPointF(24, 29));
            painter.drawLine(QPointF(24, 29), QPointF(28, 23));
        }
        break;
    }
    painter.end();
    pixmap.setDevicePixelRatio(2.0);
    return QIcon(pixmap);
}

/// 一行小字（次要信息）。挂 objectName 是为了验证工程找得到它。
QLabel *makeMutedLabel(QWidget *parent, const QString &objectName = QString()) {
    auto *label = new QLabel(parent);
    if (!objectName.isEmpty()) {
        label->setObjectName(objectName);
    }
    label->setProperty(Theme::textRolePropertyName().toUtf8().constData(),
                       Theme::secondaryTextRoleValue());
    label->setWordWrap(true);
    return label;
}

/// 一张**小节卡片**（`wtfmlSurface="section"` 那一档面：浅一档的底 + 圆角、不画边）。
///
/// 这个面**只有 `QFrame` 认得**（样式表那条选择器是 `QFrame[wtfmlSurface="section"]`）——
/// 挂在一个 `QLabel` / `QPlainTextEdit` 上等于什么都没发生。所以"要有卡片底"的地方一律
/// 先起一张 `QFrame`、把东西装进去，别指望给内容控件挂个属性就长出来。
bool darkPage() { return Theme::tokens().page.lightness() < 128; }

/// 柔和面板（#32）：圆角 14、半透明淡底、不描边。深色底上 5% 的白，浅色底上 40% 的白 ——
/// 磨砂底透得过来，不会出现一块纯白。结果区、预览区、输出框都装在它里面。
/// （section 那档面在 7a 以后是全透明的，挂属性等于没有底，所以这里自己画。）
class SoftPanel final : public QFrame {
public:
    explicit SoftPanel(QWidget *parent) : QFrame(parent) {
        setFrameShape(QFrame::NoFrame);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 20);  // 底边留得比顶边多：文字行盒自带的空白在上面，底边看起来才一样宽
        layout->setSpacing(8);
        Theme::follow(this, [this] { update(); });
    }
    QVBoxLayout *box() const { return static_cast<QVBoxLayout *>(layout()); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(darkPage() ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 102));
        p.drawRoundedRect(QRectF(rect()), 14, 14);
    }
};

QFrame *makeSectionCard(QWidget *parent) {
    return new SoftPanel(parent);
}

/// 一块**只读**的输出框（结果、生成出来的命令、摘要）。
///
/// 用等宽字体：这里面放的是十六进制摘要、JSON、`/tellraw` 命令 —— 都不是自然语言，等宽才
/// 看得清"哪一位对哪一位"（用户拿着两串摘要对眼睛扫的时候，比例字体会让他数错）。
QPlainTextEdit *makeOutputBox(QFrame *card, const QString &objectName = QString()) {
    auto *box = new QPlainTextEdit(card);
    if (!objectName.isEmpty()) {
        box->setObjectName(objectName);
    }
    box->setReadOnly(true);
    box->setFrameShape(QFrame::NoFrame);
    box->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    box->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // 高度按两行钉一档（**不设 maximumHeight**：窗口拉高时这些框跟着长，读长命令舒服）。
    // 固定三行高（长内容在框里滚）：四个框一样高，页面不会被某一格撑出一大块空。
    box->setFixedHeight(box->fontMetrics().lineSpacing() * 3 + 12);
    // 卡片自己已经是那个面了 —— 框里不要再铺一层底（两层底会变成一块深色的疙瘩）。
    box->setStyleSheet(QStringLiteral("QPlainTextEdit { background: transparent; }"));
    return box;
}

void copyToClipboard(const QString &text) {
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
}

void updateResultCopy(QLabel *result, const QString &plainText) {
    result->setProperty("copyText", plainText);
    auto *button = result->parentWidget()->findChild<QPushButton *>(result->objectName() + QStringLiteral("Copy"));
    if (button != nullptr) button->setEnabled(!plainText.trimmed().isEmpty());
}

// ---------------------------------------------------------------------------
// #32：九页共用的三段式零件 —— 页头 / 输入区（标签列定宽）/ 结果区（柔和面板）
// ---------------------------------------------------------------------------

/// 输入区左边那一列标签的宽度（四个字 + 余量），几行的控件因此竖着对齐。
constexpr int kFormLabelWidth = 72;

/// 信标页光柱配色那一块（玻璃清单 + 光柱示意）的高度。
constexpr int kBeamBodyHeight = 160;
/// 旗帜页旗面预览与图层清单的高度（六行清单刚好放下）。
constexpr int kBannerBodyHeight = 196;

/// 面板左上那一行小标题（次要色、比正文小一档）。
QLabel *makePanelCaption(QWidget *parent, const QString &text) {
    auto *label = makeMutedLabel(parent);
    label->setText(text);
    label->setWordWrap(false);
    QFont font = label->font();
    font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

/// 小图标钮上画的那几种记号（QPainter 画，不吃字体里的字形）。
enum class Glyph { Copy, Up, Down, Remove, Undo, Plus, Minus };

QIcon glyphIcon(Glyph glyph, const QColor &ink) {
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    switch (glyph) {
    case Glyph::Copy:
        p.drawRoundedRect(QRectF(5.75, 5.75, 7.5, 7.5), 1.8, 1.8);
        path.moveTo(3, 10.25);
        path.lineTo(3, 4.5);
        path.quadTo(3, 3, 4.5, 3);
        path.lineTo(10.25, 3);
        break;
    case Glyph::Up:
        path.moveTo(4.5, 10);
        path.lineTo(8, 6.5);
        path.lineTo(11.5, 10);
        break;
    case Glyph::Down:
        path.moveTo(4.5, 6.5);
        path.lineTo(8, 10);
        path.lineTo(11.5, 6.5);
        break;
    case Glyph::Remove:
        path.moveTo(4.75, 4.75);
        path.lineTo(11.25, 11.25);
        path.moveTo(11.25, 4.75);
        path.lineTo(4.75, 11.25);
        break;
    case Glyph::Undo:
        path.moveTo(6.25, 3.5);
        path.lineTo(3.75, 6);
        path.lineTo(6.25, 8.5);
        path.moveTo(3.75, 6);
        path.lineTo(9.5, 6);
        path.cubicTo(13.25, 6, 13.25, 12.5, 9.5, 12.5);
        path.lineTo(7, 12.5);
        break;
    case Glyph::Plus:
        path.moveTo(8, 4);
        path.lineTo(8, 12);
        path.moveTo(4, 8);
        path.lineTo(12, 8);
        break;
    case Glyph::Minus:
        path.moveTo(4, 8);
        path.lineTo(12, 8);
        break;
    }
    p.drawPath(path);
    p.end();
    return QIcon(pixmap);
}

/// 一颗实心色点（清单行首：这一层是什么颜色）。
QIcon colorDotIcon(const QColor &color) {
    if (!color.isValid()) return QIcon();
    QPixmap pixmap(28, 28);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(Theme::swatchBorder(color), 1.0));
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, 12, 12));
    p.end();
    return QIcon(pixmap);
}

/// 小图标钮：Ghost（没底没边，悬停才显底）。text 为空 = 只有图标的方钮。
QPushButton *makeGlyphButton(QWidget *parent, const QString &objectName, Glyph glyph, const QString &tooltip,
                             const QString &text = QString(), int height = 28) {
    auto *button = new QPushButton(text, parent);
    if (!objectName.isEmpty()) button->setObjectName(objectName);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setAutoDefault(false);
    button->setCursor(Qt::PointingHandCursor);
    Theme::markButton(button, Theme::ButtonKind::Ghost);
    const auto paint = [button, glyph] { button->setIcon(glyphIcon(glyph, Theme::tokens().secondary)); };
    paint();
    Theme::follow(button, paint);
    button->setIconSize(QSize(16, 16));
    // 尺寸写进这颗自己的样式表：全局那条按钮最小高度每次 polish 都会把 setFixedSize 的下限改回去。
    const int content = height - 2;  // Ghost 留着 1px 透明边
    button->setStyleSheet(text.isEmpty()
        ? QStringLiteral("QPushButton { padding: 0px; min-width: %1px; max-width: %1px; min-height: %1px; max-height: %1px; }").arg(content)
        : QStringLiteral("QPushButton { padding: 0px 8px; min-height: %1px; max-height: %1px; }").arg(content));
    return button;
}

/// 数字框的补充设置：外观（淡底、圆角、没有上下钮、焦点淡环）**全在应用级样式表里**
/// （`Theme.cpp` 那一族 `QLineEdit, QComboBox, QAbstractSpinBox`），这里只钉两件样式表
/// 说不清的事：没有上下钮之后的宽度按内容算、数字居中。
/// NOTE: 别再给数字框挂 `setStyleSheet()` —— 控件级样式表会盖掉全局那条 3px 焦点环。
void styleNumberSpin(QAbstractSpinBox *spin) {
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignCenter);
}

QWidget *makeNumberField(QWidget *parent, QAbstractSpinBox *spin, bool steppers, int width = 88) {
    auto *field = new QWidget(parent);
    auto *row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    spin->setParent(field);
    styleNumberSpin(spin);
    spin->setFixedWidth(width);
    if (steppers) {
        auto *minus = makeGlyphButton(field, spin->objectName() + QStringLiteral("Minus"), Glyph::Minus,
                                      QCoreApplication::translate("ToolboxPage", "减一"), QString(), 30);
        auto *plus = makeGlyphButton(field, spin->objectName() + QStringLiteral("Plus"), Glyph::Plus,
                                     QCoreApplication::translate("ToolboxPage", "加一"), QString(), 30);
        minus->setFocusPolicy(Qt::NoFocus);
        plus->setFocusPolicy(Qt::NoFocus);
        QObject::connect(minus, &QPushButton::clicked, spin, &QAbstractSpinBox::stepDown);
        QObject::connect(plus, &QPushButton::clicked, spin, &QAbstractSpinBox::stepUp);
        row->addWidget(minus);
        row->addWidget(spin);
        row->addWidget(plus);
    } else {
        row->addWidget(spin);
    }
    field->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return field;
}

/// 数字框粘贴拦截：粘进来的文字不是一个数（比如 F3+C 复制的 /execute … tp 命令）就交给 @p onCommand，
/// 不当作数字塞进框里；纯数字照常粘。
class TpPasteFilter final : public QObject {
public:
    TpPasteFilter(QDoubleSpinBox *spin, std::function<void(const QString &)> onCommand)
        : QObject(spin), m_onCommand(std::move(onCommand)) {
        if (auto *edit = spin->findChild<QLineEdit *>()) edit->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::KeyPress) return false;
        const auto *key = static_cast<QKeyEvent *>(event);
        if (!key->matches(QKeySequence::Paste)) return false;
        const QString text = QGuiApplication::clipboard()->text().trimmed();
        bool number = false;
        QLocale().toDouble(text, &number);
        if (text.isEmpty() || number || !text.contains(QLatin1String("tp"))) return false;
        m_onCommand(text);
        return true;
    }

private:
    std::function<void(const QString &)> m_onCommand;
};

/// 容器查询：@p watched 的宽度 < @p threshold 时，把 @p box 的方向翻成上下叠放，否则左右并排。
class NarrowFlip final : public QObject {
public:
    NarrowFlip(QWidget *watched, QBoxLayout *box, int threshold)
        : QObject(watched), m_box(box), m_threshold(threshold) {
        watched->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Resize && m_box != nullptr) {
            const int width = static_cast<QWidget *>(object)->width();
            m_box->setDirection(width < m_threshold ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        }
        return false;
    }

private:
    QPointer<QBoxLayout> m_box;
    int m_threshold;
};

/// 输入区：左边一列标签定宽，后面是控件；一行放一对或两对（标签, 控件）。
struct FormGrid {
    QGridLayout *grid = new QGridLayout;
    QWidget *parent = nullptr;
    int row = 0;
    explicit FormGrid(QWidget *owner) : parent(owner) {
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(10);
        grid->setColumnMinimumWidth(0, kFormLabelWidth);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
    }
    /// 两对都是小数字框那种：两对挨着摆在左边，富余的宽度留在最右（不把第二对推到最远）。
    void compact() const {
        grid->setColumnStretch(1, 0);
        grid->setColumnStretch(3, 0);
        grid->setColumnStretch(4, 1);
    }
    QLabel *label(const QString &text) const {
        auto *l = makeMutedLabel(parent);
        l->setText(text);
        l->setWordWrap(false);
        return l;
    }
    /// 数字框那种定宽的小块靠左摆，下拉框之类占满这一格（同一列几行等宽）。
    void place(QWidget *field, int column, int span = 1) const {
        const bool fixed = field->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed;
        grid->addWidget(field, row, column, 1, span, fixed ? Qt::AlignLeft | Qt::AlignVCenter : Qt::Alignment());
    }
    /// 一对：占满整行；两对：左右各一半。
    void add(const QString &text, QWidget *field, const QString &text2 = QString(), QWidget *field2 = nullptr) {
        grid->addWidget(label(text), row, 0);
        if (field2 == nullptr) {
            place(field, 1, 4);
        } else {
            place(field, 1);
            auto *second = label(text2);
            second->setMinimumWidth(kFormLabelWidth - 24);
            second->setContentsMargins(grid->columnStretch(4) > 0 ? 28 : 0, 0, 0, 0);
            grid->addWidget(second, row, 2);
            place(field2, 3);
        }
        ++row;
    }
    /// 一行标签 + 一串控件（放不下就折行，不出横向滚动）。
    QLayout *addFlowRow(const QString &text, const QList<QWidget *> &fields) {
        // 折行时标签只跟第一行：高度取控件的行高、字在里面垂直居中，再贴顶放。
        QLabel *name = label(text);
        int lineHeight = 0;
        for (const QWidget *field : fields) lineHeight = qMax(lineHeight, field->sizeHint().height());
        name->setFixedHeight(qMax(lineHeight, name->sizeHint().height()));
        name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(name, row, 0, Qt::AlignTop);
        auto *line = new FlowLayout(10, 8);
        for (QWidget *field : fields) line->addWidget(field);
        grid->addLayout(line, row, 1, 1, 4);
        ++row;
        return line;
    }
    /// 一行标签 + 一串控件（横着排，富余的宽度留在右边）。
    QHBoxLayout *addRow(const QString &text, const QList<QWidget *> &fields) {
        grid->addWidget(label(text), row, 0);
        auto *line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(10);
        for (QWidget *field : fields) line->addWidget(field);
        line->addStretch(1);
        grid->addLayout(line, row, 1, 1, 4);
        ++row;
        return line;
    }
};

/// 一行按钮：左边主要的一组，右边次要的一组（中间留空）。
QHBoxLayout *makeButtonRow(const QList<QWidget *> &left, const QList<QWidget *> &right = {}) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    for (QWidget *w : left) row->addWidget(w);
    row->addStretch(1);
    for (QWidget *w : right) row->addWidget(w);
    return row;
}

/// 结果区：一块柔和面板，左上小标题、右上「复制」小钮；里面先一行结论（大字），再是细项。
/// headline 的 objectName 就是原来那个结果标签的名字（复制钮 = 名字 + "Copy"，细项 = 名字 + "Detail"）。
struct ResultBox {
    SoftPanel *card = nullptr;
    QHBoxLayout *head = nullptr;
    QLabel *headline = nullptr;
    QLabel *details = nullptr;
    QPushButton *copy = nullptr;
};

ResultBox makeResultBox(QWidget *parent, const QString &caption, const QString &objectName, bool withCopy = true) {
    ResultBox box;
    box.card = new SoftPanel(parent);
    box.head = new QHBoxLayout;
    box.head->setContentsMargins(0, 0, 0, 0);
    box.head->setSpacing(4);
    box.head->addWidget(makePanelCaption(box.card, caption));
    box.head->addStretch(1);
    box.card->box()->addLayout(box.head);
    box.headline = new QLabel(box.card);
    box.headline->setObjectName(objectName);
    box.headline->setTextFormat(Qt::PlainText);
    box.headline->setWordWrap(true);
    box.headline->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont big = box.headline->font();
    big.setPixelSize(Theme::textSize(Theme::TextRole::Heading));  // 结果标题 = 小节档 16/600
    big.setWeight(QFont::DemiBold);
    box.headline->setFont(big);
    box.headline->hide();
    box.card->box()->addWidget(box.headline);
    box.details = new QLabel(box.card);
    box.details->setObjectName(objectName + QStringLiteral("Detail"));
    box.details->setTextFormat(Qt::RichText);
    box.details->setWordWrap(true);
    box.details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    box.card->box()->addWidget(box.details);
    if (withCopy) {
        box.copy = makeGlyphButton(box.card, objectName + QStringLiteral("Copy"), Glyph::Copy,
                                   QCoreApplication::translate("ToolboxPage", "把结果复制到剪贴板"), QCoreApplication::translate("ToolboxPage", "复制"));
        box.copy->setEnabled(false);
        box.head->addWidget(box.copy);
        QLabel *headline = box.headline;
        QObject::connect(box.copy, &QPushButton::clicked, headline, [headline] {
            const auto text = headline->property("copyText").toString();
            if (!text.isEmpty()) copyToClipboard(text);
        });
    }
    return box;
}

/// 细项那几行：空行 = 分段；以「：」结尾的一行 = 小标题（次要色、小一号）；其余一行一行原样（转义，不吃 HTML）。
QString reportDetailHtml(const QStringList &lines) {
    const QString muted = Theme::tokens().secondary.name(QColor::HexRgb);
    QString html;
    bool gap = false;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            gap = true;
            continue;
        }
        const QString top = html.isEmpty() ? QStringLiteral("0") : gap ? QStringLiteral("10px") : QStringLiteral("4px");
        gap = false;
        if (line.endsWith(QCoreApplication::translate("ToolboxPage", "：")) || line.endsWith(QLatin1Char(':')))
            html += QStringLiteral("<div style='margin-top:%1;color:%2;font-size:13px'>%3</div>")
                        .arg(top, muted, line.toHtmlEscaped());
        else
            html += QStringLiteral("<div style='margin-top:%1'>%2</div>").arg(top, line.toHtmlEscaped());
    }
    return html;
}

/// 把一份纯文本报告摆进结果区：第一行 → 结论大字，其余 → 细项；复制的是整份原文。
/// 空报告 = 整块结果区不画。
void setReport(QLabel *headline, const QString &text) {
    updateResultCopy(headline, text);
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    const QString first = lines.isEmpty() ? QString() : lines.takeFirst().trimmed();
    headline->setText(first);
    headline->setVisible(!first.isEmpty());
    auto *details = headline->parentWidget()->findChild<QLabel *>(headline->objectName() + QStringLiteral("Detail"),
                                                                  Qt::FindDirectChildrenOnly);
    if (details == nullptr) return;
    const QString html = reportDetailHtml(lines);
    details->setText(html);
    details->setVisible(!html.isEmpty());
    // 没内容整块结果区都不画。
    if (QWidget *card = headline->parentWidget()) card->setVisible(!first.isEmpty() || !html.isEmpty());
}

/// 预览底台：一块圆角的柔和底（正文色压到很淡），白旗面 / 白光柱摆上去也不会变成一块纯白。
void paintPreviewStage(QPainter &p, const QSizeF &size) {
    QColor tint = Theme::tokens().text;
    tint.setAlpha(30);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(tint);
    p.drawRoundedRect(QRectF(QPointF(0, 0), size), 10, 10);
}

/// 预览图：图居中摆在柔和底台上，四周描一圈极淡的细边（白旗面在浅色底上也分得出边）。
QPixmap framedPreview(const QImage &image, const QSize &size, qreal dpr, Qt::TransformationMode mode = Qt::FastTransformation) {
    QPixmap out((QSizeF(size) * dpr).toSize());
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    if (image.isNull()) {
        // 还没有图：只留一圈虚线轮廓（旗面那个 1:2 的比例），告诉人图会出现在这儿。
        QPainter p(&out);
        paintPreviewStage(p, size);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Theme::tokens().stroke, 1.2, Qt::DashLine);
        p.setPen(pen);
        const QSizeF box = QSizeF(size).scaled(QSizeF(size) * 0.8, Qt::KeepAspectRatio);
        const QSizeF shape = QSizeF(1, 2).scaled(box, Qt::KeepAspectRatio);
        p.drawRoundedRect(QRectF(QPointF((size.width() - shape.width()) / 2, (size.height() - shape.height()) / 2), shape), 4, 4);
        return out;
    }
    const QSize fitted = image.size().scaled((QSizeF(size) * 0.72).toSize(), Qt::KeepAspectRatio);
    const QRect target(QPoint((size.width() - fitted.width()) / 2, (size.height() - fitted.height()) / 2), fitted);
    QPainter p(&out);
    paintPreviewStage(p, size);
    p.setRenderHint(QPainter::SmoothPixmapTransform, mode == Qt::SmoothTransformation);
    p.drawImage(target, image);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor edge = Theme::tokens().text;
    edge.setAlpha(46);
    p.setPen(QPen(edge, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(target).adjusted(-0.5, -0.5, 0.5, 0.5));
    return out;
}

/// 光柱示意：透明底上一根圆角光柱，从下往上按段着色，外面描一圈极淡的细边。
/// 没放玻璃时是白光柱 —— 画成半透明的白，摆在柔和底台上，不在浅色底上留一块纯白。
void paintBeamPreview(QLabel *preview, const QList<QColor> &segments) {
    preview->setProperty("beamSegments", QVariant::fromValue(segments));  // 换主题时照原样重画
    const QSize size = preview->size();
    const qreal dpr = preview->devicePixelRatioF();
    QPixmap image((QSizeF(size) * dpr).toSize());
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintPreviewStage(painter, size);
    const QRectF column((size.width() - 22) / 2.0, 12.5, 22, size.height() - 25);
    QPainterPath shape;
    shape.addRoundedRect(column, 7, 7);
    painter.save();
    painter.setClipPath(shape);
    const int count = qMax(1, int(segments.size()));
    for (int i = 0; i < count; ++i) {
        const qreal top = column.bottom() - (i + 1) * column.height() / count;
        const qreal bottom = column.bottom() - i * column.height() / count;
        painter.fillRect(QRectF(column.left(), top, column.width(), bottom - top),
                         segments.isEmpty() ? QColor(255, 255, 255, 170) : segments[i]);
    }
    painter.restore();
    QColor edge = Theme::tokens().text;
    edge.setAlpha(46);
    painter.setPen(QPen(edge, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
    painter.end();
    preview->setPixmap(image);
}

// ---------------------------------------------------------------------------
// 目录页：自适应网格 + 一格
// ---------------------------------------------------------------------------

constexpr int kSidePadding = 28;      ///< 页面左右留白（和设置页同一档）
constexpr int kHeaderBleed = 8;       ///< 页头比内容两侧各多出的宽度（面包屑按钮的内边距）
constexpr int kCellMinWidth = 240;    ///< 目录格最小列宽
constexpr int kCellHeight = 64;
constexpr int kCellGap = 6;

/// 自适应列数的网格：每格最小 240，按宽度自动分列（auto-fill），格子等宽。
class CellGridLayout final : public QLayout {
public:
    explicit CellGridLayout(QWidget *parent) : QLayout(parent) {}
    ~CellGridLayout() override { qDeleteAll(m_items); }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override {
        return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }
    QSize sizeHint() const override { return QSize(kCellMinWidth, heightForWidth(kCellMinWidth)); }
    QSize minimumSize() const override { return QSize(kCellMinWidth, kCellHeight); }

private:
    static int columnsFor(int width) { return qMax(1, (width + kCellGap) / (kCellMinWidth + kCellGap)); }
    int arrange(const QRect &rect, bool apply) const {
        if (m_items.isEmpty()) return 0;
        const int columns = columnsFor(rect.width());
        const int cellWidth = (rect.width() - kCellGap * (columns - 1)) / columns;
        const int rows = (int(m_items.size()) + columns - 1) / columns;
        if (apply) {
            for (int i = 0; i < m_items.size(); ++i) {
                m_items.at(i)->setGeometry(QRect(rect.x() + (i % columns) * (cellWidth + kCellGap),
                                                 rect.y() + (i / columns) * (kCellHeight + kCellGap),
                                                 cellWidth, kCellHeight));
            }
        }
        return rows * kCellHeight + (rows - 1) * kCellGap;
    }
    QList<QLayoutItem *> m_items;
};

/// 目录里的一格：图标块 36 + 名称 + 一句用途；无卡片底，悬停淡底；键盘 Tab 到达画淡环。
class ToolCell final : public QPushButton {
public:
    ToolCell(ToolboxPage::Tool tool, QWidget *parent)
        : QPushButton(ToolboxPage::toolName(tool), parent), m_tool(tool), m_summary(ToolboxPage::toolSummary(tool)) {
        setObjectName(ToolboxPage::toolCellName(tool));
        setFlat(true);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(text());
        setAccessibleDescription(m_summary);
        // 高度写进自己的样式表：全局按钮样式在 polish 时会改写 min/max 高。
        setStyleSheet(QStringLiteral("QPushButton { border: none; padding: 0px; background: transparent;"
                                     " min-height: %1px; max-height: %1px; }").arg(kCellHeight));
    }
    QSize sizeHint() const override { return QSize(kCellMinWidth, kCellHeight); }
    QSize minimumSizeHint() const override { return QSize(160, kCellHeight); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Theme::Tokens t = Theme::tokens();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.fillPress);
            p.drawRoundedRect(box, 14, 14);
        } else if (underMouse() || hasFocus()) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.hover);
            p.drawRoundedRect(box, 14, 14);
        }
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box.adjusted(1, 1, -1, -1), 13, 13);
        }
        const QRect tile(14, (height() - 36) / 2, 36, 36);
        p.setPen(Qt::NoPen);
        p.setBrush(t.subtle);
        p.drawRoundedRect(QRectF(tile), 10, 10);
        ToolboxPage::toolIcon(m_tool, Theme::textTone(t.accentInk, t.deep)).paint(&p, tile.adjusted(7, 7, -7, -7));
        const int x = tile.right() + 14;
        const int w = qMax(0, width() - x - 12);
        QFont nameFont = font();
        nameFont.setPixelSize(Theme::textSize(Theme::TextRole::Body));
        nameFont.setWeight(QFont::Medium);
        QFont summaryFont = font();
        summaryFont.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
        const QFontMetrics nameMetrics(nameFont), summaryMetrics(summaryFont);
        const int top = (height() - nameMetrics.height() - summaryMetrics.height() - 2) / 2;
        p.setFont(nameFont);
        p.setPen(t.text);
        p.drawText(QRect(x, top, w, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   nameMetrics.elidedText(text(), Qt::ElideRight, w));
        p.setFont(summaryFont);
        p.setPen(t.secondary);
        p.drawText(QRect(x, top + nameMetrics.height() + 2, w, summaryMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   summaryMetrics.elidedText(m_summary, Qt::ElideRight, w));
    }
    void focusInEvent(QFocusEvent *event) override {
        QPushButton::focusInEvent(event);
        update();
    }
    void focusOutEvent(QFocusEvent *event) override {
        QPushButton::focusOutEvent(event);
        update();
    }

private:
    ToolboxPage::Tool m_tool;
    QString m_summary;
};

// ---------------------------------------------------------------------------
// 面包屑上的工具名 → 切换浮层（复用 GlassPopover）
// ---------------------------------------------------------------------------

constexpr int kSwitchWidth = 236;
constexpr int kSwitchRow = 34;

class SwitchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(0, kSwitchRow); }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const Theme::Tokens t = Theme::tokens();
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRect r = option.rect;
        const bool on = index.data(Qt::UserRole + 1).toBool();
        const bool hot = (option.state & QStyle::State_MouseOver) || (option.state & QStyle::State_Selected);
        if (on || hot) {
            p->setPen(Qt::NoPen);
            p->setBrush(on ? t.selected : t.fillPress);
            p->drawRoundedRect(QRectF(r), 8, 8);
        }
        const QColor ink = on ? Theme::textTone(t.accentInk, t.deep) : t.text;
        const auto tool = static_cast<ToolboxPage::Tool>(index.data(Qt::UserRole).toInt());
        ToolboxPage::toolIcon(tool, on ? ink : t.secondary).paint(p, QRect(r.left() + 12, r.center().y() - 8, 16, 16));
        QFont f = option.font;
        f.setPixelSize(14);
        f.setWeight(QFont::Medium);
        p->setFont(f);
        p->setPen(ink);
        p->drawText(QRect(r.left() + 40, r.top(), r.width() - 52, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    index.data(Qt::DisplayRole).toString());
        p->restore();
    }
};

class ToolSwitchPopover final : public GlassPopover {
public:
    ToolSwitchPopover(QWidget *anchor, ToolboxPage::Tool current, std::function<void(ToolboxPage::Tool)> onPick)
        : GlassPopover(anchor), m_onPick(std::move(onPick)) {
        setObjectName(ToolboxPage::switchPopoverName());
        auto *box = new QVBoxLayout(this);
        box->setContentsMargins(6, 6, 6, 6);
        box->setSpacing(0);
        m_list = new QListWidget(this);
        m_list->setObjectName(QStringLiteral("toolboxSwitchList"));
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setMouseTracking(true);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setItemDelegate(new SwitchDelegate(m_list));
        m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"
                                             "QListWidget::item { background: transparent; border: none; }"));
        m_list->viewport()->setAutoFillBackground(false);
        for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
            auto *item = new QListWidgetItem(ToolboxPage::toolName(tool), m_list);
            item->setData(Qt::UserRole, int(tool));
            item->setData(Qt::UserRole + 1, tool == current);
        }
        m_list->setFixedHeight(int(m_list->count()) * kSwitchRow);
        box->addWidget(m_list);
        setFixedWidth(kSwitchWidth);
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { pick(item); });
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { pick(item); });
    }

protected:
    void place() override {
        QWidget *host = parentWidget();
        if (host == nullptr || m_anchor == nullptr) return;
        const QPoint a = m_anchor->mapTo(host, QPoint(0, 0));
        const int height = layout()->sizeHint().height();
        const int x = qBound(8, a.x(), qMax(8, host->width() - kSwitchWidth - 8));
        setGeometry(x, a.y() + m_anchor->height() + 4, kSwitchWidth, height);
        raise();
    }
    void showEvent(QShowEvent *event) override {
        GlassPopover::showEvent(event);
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole + 1).toBool()) m_list->setCurrentRow(i);
        }
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            m_list->setFocus();
            QApplication::sendEvent(m_list, event);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            pick(m_list->currentItem());
            return;
        }
        GlassPopover::keyPressEvent(event);
    }

private:
    void pick(QListWidgetItem *item) {
        if (item == nullptr) return;
        const auto tool = static_cast<ToolboxPage::Tool>(item->data(Qt::UserRole).toInt());
        auto fn = m_onPick;
        close();
        if (fn) fn(tool);
    }
    QListWidget *m_list = nullptr;
    std::function<void(ToolboxPage::Tool)> m_onPick;
};

/// 面包屑上的一段：手写体 26，无边框，悬停淡底。
QPushButton *makeCrumbButton(QWidget *parent, const QString &objectName, const QString &text, bool current) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setAttribute(Qt::WA_Hover);
    QFont font = Theme::handFont(button->font(), Theme::HandFont::KaiLight);
    font.setPixelSize(26);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    button->setFont(font);
    const auto paint = [button, current] {
        const Theme::Tokens t = Theme::tokens();
        button->setStyleSheet(QStringLiteral(
            "QPushButton { border: none; border-radius: 10px; padding: 0px 8px; background: transparent;"
            " color: %1; min-height: 36px; max-height: 36px; }"
            "QPushButton:hover, QPushButton:focus { background: %2; }"
            "QPushButton:pressed { background: %3; }")
            .arg((current ? t.text : t.secondary).name(QColor::HexArgb), t.hover.name(QColor::HexArgb),
                 t.fillPress.name(QColor::HexArgb)));
    };
    paint();
    Theme::follow(button, paint);
    return button;
}

// ---------------------------------------------------------------------------
// 附魔行：固定列宽网格（附魔 1fr(min 200) · 等级 72 · 物品 128 · 已用次数 88 · 删除 32）
// 四个框统一高 36；内容宽 < 640 时附魔独占第一行，其余三个在第二行。
// ---------------------------------------------------------------------------

constexpr int kAnvilNarrowBelow = 640;
constexpr int kAnvilLevelWidth = 72;
constexpr int kAnvilKindWidth = 128;
constexpr int kAnvilUsesWidth = 88;
constexpr int kAnvilRemoveWidth = 32;

QSpinBox *makeUnitSpin(QWidget *parent, int minimum, int maximum, const QString &unit, int width) {
    auto *spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setSuffix(unit);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spin->setFixedSize(width, Theme::inputHeight());
    return spin;
}

class AnvilRowWidget final : public QWidget {
public:
    QComboBox *enchant = nullptr;
    QSpinBox *level = nullptr;
    QComboBox *kind = nullptr;
    QSpinBox *uses = nullptr;
    QPushButton *remove = nullptr;

    explicit AnvilRowWidget(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_Hover);
        enchant = new QComboBox(this);
        enchant->setFixedHeight(Theme::inputHeight());
        enchant->setMinimumWidth(200);
        enchant->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        level = makeUnitSpin(this, 1, 9, QCoreApplication::translate("ToolboxPage", " 级"), kAnvilLevelWidth);  // 附魔等级没有超过 5 的，给到 9 留余地
        level->setValue(1);
        kind = new QComboBox(this);
        kind->setFixedSize(kAnvilKindWidth, Theme::inputHeight());
        uses = makeUnitSpin(this, 0, 9, QCoreApplication::translate("ToolboxPage", " 次"), kAnvilUsesWidth);
        uses->setToolTip(QCoreApplication::translate("ToolboxPage", "在铁砧上用过几次。"));
        remove = makeGlyphButton(this, QStringLiteral("anvilRemoveRow"), Glyph::Remove, QCoreApplication::translate("ToolboxPage", "删除这一行"),
                                 QString(), kAnvilRemoveWidth);
        // 悬停才出现；键盘到达时（焦点在行里）也出现。
        auto *fade = new QGraphicsOpacityEffect(remove);
        fade->setOpacity(0.0);
        remove->setGraphicsEffect(fade);
        m_fade = fade;
        for (QWidget *child : {static_cast<QWidget *>(enchant), static_cast<QWidget *>(level),
                               static_cast<QWidget *>(kind), static_cast<QWidget *>(uses),
                               static_cast<QWidget *>(remove)}) {
            child->installEventFilter(this);
        }
        auto *grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);
        m_grid = grid;
        arrange(false);
    }

    void setNarrow(bool narrow) {
        if (narrow == m_narrow) return;
        arrange(narrow);
    }
    void setRemovable(bool removable) {
        remove->setEnabled(removable);
        syncRemoveVisibility();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        Q_UNUSED(watched);
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
            event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
            QTimer::singleShot(0, this, [this] { syncRemoveVisibility(); });
        }
        return false;
    }
    void enterEvent(QEnterEvent *event) override {
        QWidget::enterEvent(event);
        syncRemoveVisibility();
    }
    void leaveEvent(QEvent *event) override {
        QWidget::leaveEvent(event);
        QTimer::singleShot(0, this, [this] { syncRemoveVisibility(); });
    }

private:
    void syncRemoveVisibility() {
        const bool show = remove->isEnabled() && (underMouse() || hasFocusWithin());
        m_fade->setOpacity(show ? 1.0 : 0.0);
    }
    bool hasFocusWithin() const {
        QWidget *focus = QApplication::focusWidget();
        return focus != nullptr && isAncestorOf(focus);
    }
    void arrange(bool narrow) {
        m_narrow = narrow;
        while (m_grid->count() > 0) delete m_grid->takeAt(0);
        for (int column = 0; column < 6; ++column) {
            m_grid->setColumnStretch(column, 0);
            m_grid->setColumnMinimumWidth(column, 0);
        }
        if (!narrow) {
            m_grid->addWidget(enchant, 0, 0);
            m_grid->addWidget(level, 0, 1);
            m_grid->addWidget(kind, 0, 2);
            m_grid->addWidget(uses, 0, 3);
            m_grid->addWidget(remove, 0, 4);
            m_grid->setColumnStretch(0, 1);
            m_grid->setColumnMinimumWidth(0, 200);
        } else {
            // 附魔占前四列整行；等级 / 物品 / 已用次数在第二行；删除钮在第一行右端。
            m_grid->addWidget(enchant, 0, 0, 1, 4);
            m_grid->addWidget(remove, 0, 4);
            m_grid->addWidget(level, 1, 0);
            m_grid->addWidget(kind, 1, 1);
            m_grid->addWidget(uses, 1, 2);
            m_grid->setColumnStretch(3, 1);
        }
    }
    QGridLayout *m_grid = nullptr;
    QGraphicsOpacityEffect *m_fade = nullptr;
    bool m_narrow = false;
};

/// 列头一行（宽时显示）：列宽与行内网格一致，窄时隐藏。
class AnvilHeadRow final : public QWidget {
public:
    explicit AnvilHeadRow(QWidget *parent) : QWidget(parent) {
        auto *grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        const QStringList names{QCoreApplication::translate("ToolboxPage", "附魔"), QCoreApplication::translate("ToolboxPage", "等级"), QCoreApplication::translate("ToolboxPage", "物品"),
                                QCoreApplication::translate("ToolboxPage", "已用次数")};
        const int widths[] = {0, kAnvilLevelWidth, kAnvilKindWidth, kAnvilUsesWidth};
        for (int i = 0; i < 4; ++i) {
            auto *label = makeMutedLabel(this);
            label->setText(names.at(i));
            label->setWordWrap(false);
            label->setContentsMargins(2, 0, 0, 0);
            grid->addWidget(label, 0, i);
            if (widths[i] > 0) grid->setColumnMinimumWidth(i, widths[i]);
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnMinimumWidth(0, 200);
        grid->setColumnMinimumWidth(4, kAnvilRemoveWidth);
    }
};

/// 装一行行输入的容器：按自己的宽度决定每行是否折成两行（相当于容器查询）。
class AnvilRowsHost final : public QWidget {
public:
    explicit AnvilRowsHost(QWidget *parent) : QWidget(parent) {
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(8);
        m_head = new AnvilHeadRow(this);
        m_layout->addWidget(m_head);
    }
    void addRow(AnvilRowWidget *row) {
        m_layout->addWidget(row);
        row->setNarrow(m_narrow);
        m_rows.append(row);
    }
    void relayout() { applyNarrow(width() < kAnvilNarrowBelow, true); }
    bool narrow() const { return m_narrow; }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        applyNarrow(event->size().width() < kAnvilNarrowBelow, false);
    }

private:
    void applyNarrow(bool narrow, bool force) {
        if (narrow == m_narrow && !force) return;
        m_narrow = narrow;
        m_head->setVisible(!narrow);
        m_layout->setSpacing(narrow ? 16 : 8);
        for (const QPointer<AnvilRowWidget> &row : std::as_const(m_rows)) {
            if (row) row->setNarrow(narrow);
        }
    }
    QVBoxLayout *m_layout = nullptr;
    AnvilHeadRow *m_head = nullptr;
    QList<QPointer<AnvilRowWidget>> m_rows;
    bool m_narrow = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// 九件工具那张表（键 / 名字 / 用途）
// ---------------------------------------------------------------------------

QString ToolboxPage::toolKey(Tool tool) {
    return QString::fromLatin1(infoOf(tool)->key);
}

ToolboxPage::Tool ToolboxPage::toolFromKey(const QString &key, bool *ok) {
    for (const ToolInfo &info : kToolInfos) {
        if (key == QLatin1String(info.key)) {
            if (ok != nullptr) {
                *ok = true;
            }
            return info.tool;
        }
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return Tool::Anvil;
}

QString ToolboxPage::toolName(Tool tool) {
    return QCoreApplication::translate("ToolboxPage", infoOf(tool)->name);
}

QString ToolboxPage::toolSummary(Tool tool) {
    return QCoreApplication::translate("ToolboxPage", infoOf(tool)->summary);
}

QList<ToolboxPage::Tool> ToolboxPage::tools() {
    QList<Tool> list;
    for (const ToolInfo &info : kToolInfos) list.append(info.tool);
    return list;
}

QIcon ToolboxPage::toolIcon(Tool tool, const QColor &ink) {
    return drawToolIcon(tool, ink);
}

QString ToolboxPage::toolCellName(Tool tool) {
    return QStringLiteral("toolboxCell_%1").arg(toolKey(tool));
}

QString ToolboxPage::toolPanelName(Tool tool) {
    return QStringLiteral("toolboxPanel_%1").arg(toolKey(tool));
}

QString ToolboxPage::toolPageName(Tool tool) {
    return toolPanelName(tool) + QStringLiteral("Scroll");
}

// ---------------------------------------------------------------------------
// 搭页：页头 + 两层（目录 / 工具）
// ---------------------------------------------------------------------------

ToolboxPage::ToolboxPage(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("toolboxPage"));
    setWindowTitle(pageTitle());
    resize(900, 560);  // 单独拿出来用（验证工程）时的默认尺寸；嵌在选项卡里时拿内容区整幅

    m_tools = tools();

    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    box->addWidget(buildHeader());

    m_levels = new QStackedWidget(this);
    m_levels->setObjectName(QStringLiteral("toolboxLevels"));
    m_levels->addWidget(buildCatalog());

    m_stack = new QStackedWidget(m_levels);
    m_stack->setObjectName(QStringLiteral("toolboxStack"));
    // 工具页不在这里搭：第一次进哪件才搭哪件（`ensureTool()`），目录页先出。
    m_levels->addWidget(m_stack);
    box->addWidget(m_levels, 1);
    showCatalog();
}

QWidget *ToolboxPage::buildHeader() {
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("toolboxHeader"));
    auto *outer = new QHBoxLayout(header);
    // 页头两侧各比内容多出 8px：面包屑按钮自带 8px 内边距，这样按钮上的字和内容左缘对齐；
    // 目录页的标题与「记住输入」自己补回这 8px。左缘、上沿走一级页统一落点（Theme::pageTitleInset*）。
    // 内容区右边常留一条滚动条的位置（GutterScrollArea），页头也让出来，这样两者右缘对得上。
    outer->setContentsMargins(Theme::pageTitleInsetLeft() - kHeaderBleed, Theme::pageTitleInsetTop(),
                              kSidePadding - kHeaderBleed + Theme::scrollBarThickness(), 14);
    m_headerInner = new QWidget(header);
    m_headerInner->setMaximumWidth(kCatalogMaxWidth + 2 * kHeaderBleed);
    m_headerInner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    outer->addStretch(1);  // 和内容一样居中
    outer->addWidget(m_headerInner, 1000);
    outer->addStretch(1);
    auto *inner = new QVBoxLayout(m_headerInner);
    inner->setContentsMargins(0, 0, 0, 0);
    inner->setSpacing(0);

    // 目录页：页标题 + 右端「记住输入」开关。
    m_catalogHead = new QWidget(m_headerInner);
    auto *catalogRow = new QHBoxLayout(m_catalogHead);
    catalogRow->setContentsMargins(kHeaderBleed, 0, kHeaderBleed, 0);
    catalogRow->setSpacing(10);
    auto *title = new QLabel(pageTitle(), m_catalogHead);
    title->setObjectName(QStringLiteral("toolboxTitle"));
    title->setProperty(Theme::textRolePropertyName().toUtf8().constData(), Theme::pageTitleTextRoleValue());
    {
        QFont font = Theme::handFont(title->font(), Theme::HandFont::KaiLight);
        font.setPixelSize(26);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
        title->setFont(font);
    }
    title->setMinimumHeight(36);
    catalogRow->addWidget(title);
    catalogRow->addStretch(1);
    auto *rememberLabel = new QLabel(QCoreApplication::translate("ToolboxPage", "记住输入"), m_catalogHead);
    rememberLabel->setProperty(Theme::textRolePropertyName().toUtf8().constData(), Theme::secondaryTextRoleValue());
    m_remember = new QCheckBox(m_catalogHead);
    m_remember->setObjectName(QStringLiteral("toolboxRememberInputs"));
    m_remember->setText(QCoreApplication::translate("ToolboxPage", "记住输入"));  // 开关只画胶囊；文字作无障碍名
    m_remember->setToolTip(QCoreApplication::translate("ToolboxPage", "关闭后清除已记住的输入。"));
    m_remember->setChecked(true);
    Theme::markSwitch(m_remember);
    rememberLabel->setToolTip(m_remember->toolTip());
    catalogRow->addWidget(rememberLabel);
    catalogRow->addWidget(m_remember);
    inner->addWidget(m_catalogHead);

    // 工具页：面包屑「工具箱 / 附魔助手」。「工具箱」回目录，工具名弹切换浮层。
    m_crumbHead = new QWidget(m_headerInner);
    auto *crumbRow = new QHBoxLayout(m_crumbHead);
    crumbRow->setContentsMargins(0, 0, 0, 0);
    crumbRow->setSpacing(2);
    m_crumbRoot = makeCrumbButton(m_crumbHead, crumbRootName(), pageTitle(), false);
    m_crumbRoot->setToolTip(QCoreApplication::translate("ToolboxPage", "回到工具箱目录"));
    crumbRow->addWidget(m_crumbRoot);
    auto *slash = new QLabel(QStringLiteral("/"), m_crumbHead);
    {
        QFont font = Theme::handFont(slash->font(), Theme::HandFont::KaiLight);
        font.setPixelSize(26);
        slash->setFont(font);
        slash->setProperty(Theme::textRolePropertyName().toUtf8().constData(), Theme::secondaryTextRoleValue());
    }
    crumbRow->addWidget(slash);
    m_crumbTool = makeCrumbButton(m_crumbHead, crumbToolName(), QString(), true);
    m_crumbTool->setToolTip(QCoreApplication::translate("ToolboxPage", "切换工具"));
    crumbRow->addWidget(m_crumbTool);
    crumbRow->addStretch(1);
    inner->addWidget(m_crumbHead);

    connect(m_crumbRoot, &QPushButton::clicked, this, [this] { showCatalog(); });
    connect(m_crumbTool, &QPushButton::clicked, this, [this] { openSwitchPopover(); });
    return header;
}

QWidget *ToolboxPage::buildCatalog() {
    auto *scroll = new Theme::GutterScrollArea(this);
    scroll->setObjectName(catalogName());
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *holder = new QWidget(scroll);
    auto *row = new QHBoxLayout(holder);
    row->setContentsMargins(Theme::pageTitleInsetLeft(), 4, kSidePadding, 40);
    row->setSpacing(0);
    m_catalogBody = new QWidget(holder);
    m_catalogBody->setMaximumWidth(kCatalogMaxWidth);
    m_catalogBody->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *grid = new CellGridLayout(m_catalogBody);
    for (Tool tool : m_tools) {
        auto *cell = new ToolCell(tool, m_catalogBody);
        connect(cell, &QPushButton::clicked, this, [this, tool] { showTool(tool); });
        grid->addWidget(cell);
    }
    row->addStretch(1);  // 宽窗时居中
    row->addWidget(m_catalogBody, 1000);
    row->addStretch(1);
    scroll->setWidget(holder);
    return scroll;
}

void ToolboxPage::syncHeader() {
    m_catalogHead->setVisible(m_inCatalog);
    m_crumbHead->setVisible(!m_inCatalog);
    m_headerInner->setMaximumWidth((m_inCatalog ? kCatalogMaxWidth : kToolMaxWidth) + 2 * kHeaderBleed);
    if (!m_inCatalog) m_crumbTool->setText(toolName(m_current));
}

void ToolboxPage::showCatalog() {
    if (!m_inCatalog && m_current == Tool::Server) clearServerResults();
    GlassPopover::closeAllIn(window());
    m_inCatalog = true;
    m_levels->setCurrentIndex(0);
    syncHeader();
    emit catalogShown();
}

QWidget *ToolboxPage::ensureTool(Tool tool) {
    if (QWidget *built = m_pages.value(tool)) return built;
    QWidget *panel = nullptr;
    switch (tool) {
    case Tool::Anvil: panel = buildAnvilPanel(); break;
    case Tool::Text: panel = buildTextPanel(); break;
    case Tool::Coord: panel = buildCoordPanel(); break;
    case Tool::Server: panel = buildServerPanel(); break;
    case Tool::Beacon: panel = buildBeaconPanel(); break;
    case Tool::Brewing: panel = buildBrewingPanel(); break;
    case Tool::Banner: panel = buildBannerPanel(); break;
    case Tool::Trim: panel = buildTrimPanel(); break;
    case Tool::Download: panel = buildDownloadPanel(); break;
    }
    panel->setObjectName(toolPanelName(tool));
    panel->layout()->setContentsMargins(0, 4, 0, 0);
    static_cast<QBoxLayout *>(panel->layout())->setSpacing(14);
    // 长工具页在自身滚动，不把整个窗口的最小高度撑大；横向永不出滚动条（窄时由各片自己重排）。
    auto *scroll = new Theme::GutterScrollArea(m_stack);
    scroll->setObjectName(toolPageName(tool));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *holder = new QWidget(scroll);
    auto *row = new QHBoxLayout(holder);
    row->setContentsMargins(Theme::pageTitleInsetLeft(), 0, kSidePadding, 40);
    row->setSpacing(0);
    panel->setParent(holder);
    panel->setMaximumWidth(kToolMaxWidth);
    row->addStretch(1);  // 宽窗时居中
    row->addWidget(panel, 1000);
    row->addStretch(1);
    scroll->setWidget(holder);
    m_stack->addWidget(scroll);
    m_pages.insert(tool, scroll);
    // 先让 controller 灌表、恢复输入、算出初值，再换到这一页（`showTool()` 接着做）。
    emit toolBuilt(tool);
    return scroll;
}

void ToolboxPage::showTool(Tool tool) {
    if (!m_inCatalog && m_current == Tool::Server && tool != Tool::Server) clearServerResults();
    QWidget *page = ensureTool(tool);
    m_current = tool;
    m_inCatalog = false;
    m_stack->setCurrentWidget(page);
    m_levels->setCurrentIndex(1);
    syncHeader();
    // 目录格、切换浮层、程序里调 `showTool()` 都只走到这里，所以"换了哪一件"必须从这里发出去 ——
    // 纹饰那一片要借它去读预览资源，高速下载那一片要借它统计缓存。
    emit toolShown(m_current);
}

void ToolboxPage::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    if (!m_inCatalog && m_current == Tool::Server) clearServerResults();
}

void ToolboxPage::openSwitchPopover() {
    if (QWidget *open = window()->findChild<QWidget *>(switchPopoverName())) {
        open->close();  // 再点一次工具名 = 收起
        return;
    }
    auto *popover = new ToolSwitchPopover(m_crumbTool, m_current, [this](Tool tool) { showTool(tool); });
    popover->popUp();
}

QWidget *ToolboxPage::buildDownloadPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    m_download = new DownloadToolPanel(panel);
    layout->addWidget(m_download);
    return panel;
}

// ---------------------------------------------------------------------------
// 一、附魔助手
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildAnvilPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *rowsHost = new AnvilRowsHost(panel);
    rowsHost->setObjectName(QStringLiteral("anvilRows"));
    m_anvilRowsHost = rowsHost;
    layout->addWidget(rowsHost);

    auto *rowButtons = new QHBoxLayout;
    rowButtons->setContentsMargins(0, 6, 0, 0);
    rowButtons->setSpacing(10);
    auto *addRow = makeGlyphButton(panel, QStringLiteral("anvilAddRow"), Glyph::Plus,
                                   QCoreApplication::translate("ToolboxPage", "在末尾加一行"), QCoreApplication::translate("ToolboxPage", "加一行"), 36);
    auto *renameLabel = new QLabel(QCoreApplication::translate("ToolboxPage", "顺带改名"), panel);
    renameLabel->setProperty(Theme::textRolePropertyName().toUtf8().constData(), Theme::secondaryTextRoleValue());
    auto *rename = new QCheckBox(panel);
    rename->setObjectName(QStringLiteral("anvilRename"));
    rename->setText(QCoreApplication::translate("ToolboxPage", "顺带改名"));  // 开关只画胶囊；文字作无障碍名
    Theme::markSwitch(rename);
    // 铁砧改名只加在一次操作上：合的时候顺手改（+1 级），或合完单独改一次；规则在结果那一句里说明。
    rename->setToolTip(QCoreApplication::translate("ToolboxPage", "按「要改名」算，多 1 级。"));
    renameLabel->setToolTip(rename->toolTip());
    m_anvilRename = rename;
    auto *plan = new QPushButton(QCoreApplication::translate("ToolboxPage", "计算"), panel);
    plan->setObjectName(QStringLiteral("anvilPlan"));
    Theme::markButton(plan, Theme::ButtonKind::Primary);
    plan->setAutoDefault(false);
    rowButtons->addWidget(addRow);
    rowButtons->addStretch(1);
    rowButtons->addWidget(renameLabel);
    rowButtons->addWidget(rename);
    rowButtons->addSpacing(4);
    rowButtons->addWidget(plan);
    layout->addLayout(rowButtons);

    // 结果：有结果（或报错）才出现。
    m_anvilResultBox = new QWidget(panel);
    m_anvilResultBox->setObjectName(QStringLiteral("anvilResultBox"));
    auto *resultLayout = new QVBoxLayout(m_anvilResultBox);
    resultLayout->setContentsMargins(0, 18, 0, 0);
    resultLayout->setSpacing(8);
    auto *resultHead = new QHBoxLayout;
    resultHead->setContentsMargins(0, 0, 0, 0);
    resultHead->setSpacing(12);
    auto *resultTitle = new QLabel(QCoreApplication::translate("ToolboxPage", "合并顺序"), m_anvilResultBox);
    {
        QFont font = resultTitle->font();
        font.setPixelSize(Theme::textSize(Theme::TextRole::Heading));
        font.setWeight(QFont::DemiBold);
        resultTitle->setFont(font);
    }
    resultHead->addWidget(resultTitle);
    m_anvilStepCount = makeMutedLabel(m_anvilResultBox, QStringLiteral("anvilStepCount"));
    m_anvilStepCount->setWordWrap(false);
    resultHead->addWidget(m_anvilStepCount);
    resultHead->addStretch(1);
    resultLayout->addLayout(resultHead);
    m_anvilError = makeMutedLabel(m_anvilResultBox, QStringLiteral("anvilError"));
    m_anvilError->hide();
    resultLayout->addWidget(m_anvilError);
    m_anvilResult = new QLabel(m_anvilResultBox);
    m_anvilResult->setObjectName(QStringLiteral("anvilResult"));
    m_anvilResult->setTextFormat(Qt::RichText);
    m_anvilResult->setWordWrap(true);
    m_anvilResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_anvilResult->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    resultLayout->addWidget(m_anvilResult);
    m_anvilResultBox->hide();
    layout->addWidget(m_anvilResultBox);
    layout->addStretch(1);

    connect(addRow, &QPushButton::clicked, this, [this]() { appendAnvilRow(); });
    connect(plan, &QPushButton::clicked, this, [this]() { emit anvilPlanRequested(); });

    // 一进来就摆一行 —— 空表看着像没搭好。
    appendAnvilRow();
    return panel;
}

void ToolboxPage::appendAnvilRow() {
    auto *host = static_cast<AnvilRowsHost *>(m_anvilRowsHost);
    if (host == nullptr || m_anvilRowWidgets.size() >= 64) {
        return;
    }
    auto *row = new AnvilRowWidget(host);
    // 下拉框与数字框的合法值域由控件自己管着（不让用户敲一串文本再回来判"这算不算一条附魔"）。
    for (const auto &pair : m_anvilChoices) {
        row->enchant->addItem(pair.second, pair.first);
    }
    row->kind->addItem(QCoreApplication::translate("ToolboxPage", "附魔书"), QStringLiteral("book"));
    row->kind->addItem(QCoreApplication::translate("ToolboxPage", "装备"), QStringLiteral("gear"));
    host->addRow(row);
    m_anvilRowWidgets.append(row);
    connect(row->kind, &QComboBox::currentIndexChanged, this, [this](int) { refreshAnvilRows(); });
    connect(row->remove, &QPushButton::clicked, this, [this, row]() {
        const int index = int(m_anvilRowWidgets.indexOf(row));
        if (index < 0 || m_anvilRowWidgets.size() <= 1) return;
        m_anvilRowWidgets.removeAt(index);
        row->hide();
        row->deleteLater();
        refreshAnvilRows();
    });
    refreshAnvilRows();
}

void ToolboxPage::refreshAnvilRows() {
    for (int index = 0; index < m_anvilRowWidgets.size(); ++index) {
        auto *row = static_cast<AnvilRowWidget *>(m_anvilRowWidgets.at(index));
        // 「同上一件」= 这一条附魔并进上一行那一件（一本书带两条附魔、一件装备本来有好几条）。
        // 第一行没有上一行，下拉里不提供。
        const int same = row->kind->findData(QStringLiteral("same"));
        {
            const QSignalBlocker blocker(row->kind);
            if (index == 0 && same >= 0) {
                if (row->kind->currentIndex() == same) row->kind->setCurrentIndex(0);
                row->kind->removeItem(same);
            } else if (index > 0 && same < 0) {
                row->kind->addItem(QCoreApplication::translate("ToolboxPage", "同上一件"), QStringLiteral("same"));
            }
        }
        // 并进上一行时，「已用次数」归上一行那一件管，这一格灰掉。
        row->uses->setEnabled(row->kind->currentData().toString() != QStringLiteral("same"));
        row->setRemovable(m_anvilRowWidgets.size() > 1);
    }
    if (auto *host = static_cast<AnvilRowsHost *>(m_anvilRowsHost)) host->relayout();
}

void ToolboxPage::setAnvilRows(const QList<AnvilRow> &rows) {
    for (QWidget *old : m_anvilRowWidgets) {
        old->hide();
        old->deleteLater();
    }
    m_anvilRowWidgets.clear();
    for (const auto &entry : rows.mid(0, 64)) {
        appendAnvilRow();
        auto *row = static_cast<AnvilRowWidget *>(m_anvilRowWidgets.last());
        const int index = row->enchant->findData(entry.id);
        if (index < 0) {
            m_anvilRowWidgets.removeLast();
            row->hide();
            row->deleteLater();
            continue;
        }
        row->enchant->setCurrentIndex(index);
        row->level->setValue(entry.level);
        const bool same = m_anvilRowWidgets.size() > 1 && entry.sameAsPrevious;
        row->kind->setCurrentIndex(row->kind->findData(same ? QStringLiteral("same")
                                                       : entry.isBook ? QStringLiteral("book") : QStringLiteral("gear")));
        row->uses->setValue(entry.uses);
    }
    if (m_anvilRowWidgets.isEmpty()) appendAnvilRow();
    refreshAnvilRows();
}

void ToolboxPage::setEnchantmentChoices(const QList<QPair<QString, QString>> &idAndName) {
    m_anvilChoices = idAndName;
    // 已经摆着的那几行也得跟上（表是构造之后才喂进来的）。
    for (QWidget *widget : m_anvilRowWidgets) {
        auto *row = static_cast<AnvilRowWidget *>(widget);
        const QString previous = row->enchant->currentData().toString();
        row->enchant->clear();
        for (const auto &pair : idAndName) {
            row->enchant->addItem(pair.second, pair.first);
        }
        const int index = row->enchant->findData(previous);
        if (index >= 0) {
            row->enchant->setCurrentIndex(index);
        }
    }
}

bool ToolboxPage::anvilRename() const {
    return m_anvilRename != nullptr && m_anvilRename->isChecked();
}

QList<ToolboxPage::AnvilRow> ToolboxPage::anvilRows() const {
    QList<AnvilRow> rows;
    for (int index = 0; index < m_anvilRowWidgets.size(); ++index) {
        const auto *row = static_cast<const AnvilRowWidget *>(m_anvilRowWidgets.at(index));
        AnvilRow entry;
        entry.id = row->enchant->currentData().toString();
        entry.level = row->level->value();
        const QString kind = row->kind->currentData().toString();
        entry.isBook = kind != QStringLiteral("gear");
        entry.uses = row->uses->value();
        entry.sameAsPrevious = index > 0 && kind == QStringLiteral("same");
        if (!entry.id.isEmpty()) {
            rows.append(entry);
        }
    }
    return rows;
}

void ToolboxPage::setAnvilView(const AnvilView &view) {
    if (m_anvilError == nullptr || m_anvilResult == nullptr) {
        return;
    }
    m_anvilError->setText(view.error);
    m_anvilError->setVisible(!view.error.isEmpty());
    if (!view.ok) {
        m_anvilResult->clear();
        m_anvilResult->hide();
        m_anvilStepCount->clear();
        m_anvilResultBox->setVisible(!view.error.isEmpty());
        return;
    }
    m_anvilResultBox->show();
    m_anvilResult->show();
    m_anvilStepCount->setText(view.steps.isEmpty() ? QString() : QCoreApplication::translate("ToolboxPage", "%1 步").arg(view.steps.size()));
    // 先一行结论大字，再是说明（次要色），最后一步一步的列表。
    QString html;
    if (!view.summary.isEmpty()) {
        html.append(QStringLiteral("<div style='font-size:16px;font-weight:600;margin-bottom:6px'>%1</div>")
                        .arg(view.summary.toHtmlEscaped()));
    }
    if (!view.notes.isEmpty()) {
        QStringList notes;
        for (const QString &note : view.notes) notes << note.toHtmlEscaped();
        html.append(QStringLiteral("<div style='color:%1;margin-bottom:6px'>%2</div>")
                        .arg(Theme::tokens().secondary.name(QColor::HexRgb), notes.join(QStringLiteral("<br>"))));
    }
    html.append(QStringLiteral("<ol style='margin:0 0 0 -16px'>"));
    for (const QString &step : view.steps) {
        html.append(QStringLiteral("<li style='margin-top:3px'>%1</li>").arg(step.toHtmlEscaped()));
    }
    html.append(QStringLiteral("</ol>"));
    m_anvilResult->setText(html);
}

// ---------------------------------------------------------------------------
// 二、文本生成器
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildTextPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    auto *textBody = new QHBoxLayout;
    textBody->setContentsMargins(0, 0, 0, 0);
    textBody->setSpacing(14);
    auto *textRight = new QVBoxLayout;
    textRight->setContentsMargins(0, 0, 0, 0);
    textRight->setSpacing(8);
    auto *inputCard = new SoftPanel(panel);
    auto *inputBox = inputCard->box();
    inputBox->setSpacing(10);
    inputBox->addWidget(makePanelCaption(inputCard, QCoreApplication::translate("ToolboxPage", "编辑预览")));
    m_textPreview = new QTextEdit(inputCard);
    m_textPreview->setObjectName(QStringLiteral("textPreview"));
    m_textPreview->setAcceptRichText(false);
    m_textPreview->setPlaceholderText(QCoreApplication::translate("ToolboxPage", "输入或粘贴文本，再为选中文字设置颜色和格式。"));
    m_textPreview->setFixedHeight(96);
    connect(m_textPreview, &QTextEdit::textChanged, this, &ToolboxPage::textPreviewEdited);

    // 色码 + 字形那两排（按钮全部由 controller 喂进来的表生成 —— 十六色那张表在
    // `TextFormat` 里，这一页不另抄一份）。
    m_textPaletteRow = new QWidget(inputCard);
    m_textPaletteRow->setObjectName(QStringLiteral("textPalette"));
    m_textPaletteLayout = new QGridLayout(m_textPaletteRow);
    m_textPaletteLayout->setContentsMargins(0, 0, 0, 0);
    m_textPaletteLayout->setHorizontalSpacing(12);
    m_textPaletteLayout->setVerticalSpacing(6);
    m_textPaletteLayout->setColumnMinimumWidth(0, 40);
    m_textPaletteLayout->setColumnStretch(1, 1);
    inputBox->addWidget(m_textPaletteRow);

    // 1.16 起那份自定颜色：十六色表里没有的颜色。这一格只收 `#RRGGBB` 六位 ——
    // `§x§R§R§G§G§B§B` 那串由 controller 算（规矩在 `TextFormat` 里，这一页不抄）。
    m_textHex = new QLineEdit(inputCard);
    m_textHex->setObjectName(QStringLiteral("textHex"));
    m_textHex->setPlaceholderText(QStringLiteral("#ff8800"));
    m_textHex->setMaxLength(7);
    m_textHex->setFixedWidth(104);
    m_textHex->setToolTip(QCoreApplication::translate("ToolboxPage", "需 1.16 起。"));
    auto *hexInsert = makeGlyphButton(inputCard, QStringLiteral("textHexInsert"), Glyph::Plus,
                                      m_textHex->toolTip(), QCoreApplication::translate("ToolboxPage", "插自定颜色"), 32);
    connect(hexInsert, &QPushButton::clicked, this,
            [this]() { emit textHexColorRequested(m_textHex->text().trimmed()); });
    connect(m_textHex, &QLineEdit::returnPressed, this,
            [this]() { emit textHexColorRequested(m_textHex->text().trimmed()); });

    auto *fromJson = new QPushButton(QCoreApplication::translate("ToolboxPage", "应用 JSON"), inputCard);
    fromJson->setObjectName(QStringLiteral("textFromJson"));
    fromJson->setToolTip(QCoreApplication::translate("ToolboxPage", "把下面 JSON 里的内容读回原文。"));
    Theme::markButton(fromJson, Theme::ButtonKind::Ghost);
    auto *clear = new QPushButton(QCoreApplication::translate("ToolboxPage", "清空"), inputCard);
    clear->setObjectName(QStringLiteral("textClear"));
    Theme::markButton(clear, Theme::ButtonKind::Ghost);
    for (QPushButton *button : {fromJson, clear}) button->setAutoDefault(false);
    // 左：自定颜色；右：次要的三颗转换动作成一组。
    auto *textActions = new QVBoxLayout;
    textActions->setSpacing(8);
    auto *customGroup = new QHBoxLayout;
    customGroup->setSpacing(8);
    customGroup->addWidget(m_textHex);
    customGroup->addWidget(hexInsert);
    customGroup->addStretch(1);
    auto *convertGroup = new QHBoxLayout;
    convertGroup->setSpacing(8);
    convertGroup->addWidget(clear);
    convertGroup->addStretch(1);
    textActions->addLayout(customGroup);
    textActions->addLayout(convertGroup);
    inputBox->addLayout(textActions);
    inputBox->addWidget(m_textPreview);

    m_textNotice = makeMutedLabel(inputCard, QStringLiteral("textNotice"));
    m_textNotice->hide();
    inputBox->addWidget(m_textNotice);
    inputBox->addStretch(1);
    textBody->addWidget(inputCard, 6);

    // 源码区：§ 文本即时同步，JSON 由用户确认应用，MOTD 只读。
    const struct {
        const char *objectName;
        QString title;
        QString tooltip;
    } kOutputs[] = {
        {"textSource", QCoreApplication::translate("ToolboxPage", "§ 源码"), QCoreApplication::translate("ToolboxPage", "输入或粘贴 § 文本，同步更新预览。")},
        {"textJson", "JSON", QCoreApplication::translate("ToolboxPage", "命令里的 text 字段、物品名称用它。")},
        {"textMotd", "motd", QCoreApplication::translate("ToolboxPage", "整行写进 server.properties，非 ASCII 已转成 \\uXXXX。")},
    };
    QFrame *outCard = makeSectionCard(panel);
    auto *outBox = static_cast<QVBoxLayout *>(outCard->layout());
    outBox->setSpacing(8);
    auto *outHead = new QHBoxLayout;
    outHead->setContentsMargins(0, 0, 0, 0);
    outHead->setSpacing(8);
    auto *segment = new QWidget(outCard);
    segment->setObjectName(QStringLiteral("textOutputSegments"));
    auto *segmentLayout = new QHBoxLayout(segment);
    segmentLayout->setContentsMargins(3, 3, 3, 3);
    segmentLayout->setSpacing(2);
    QList<QPushButton *> segments;
    auto *outStack = new QStackedWidget(outCard);
    outStack->setObjectName(QStringLiteral("textOutputStack"));
    for (const auto &spec : kOutputs) {
        auto *button = new QPushButton(spec.title, segment);
        button->setToolTip(spec.tooltip);
        segmentLayout->addWidget(button);
        segments.append(button);
        QPlainTextEdit *box = makeOutputBox(outCard, QString::fromLatin1(spec.objectName));
        box->setMinimumHeight(60);
        box->setMaximumHeight(QWIDGETSIZE_MAX);  // 高度跟着卡片，不钉三行
        box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        outStack->addWidget(box);
        if (qstrcmp(spec.objectName, "textSource") == 0) {
            m_textSource = box;
            m_textSource->setReadOnly(false);
            connect(m_textSource, &QPlainTextEdit::textChanged, this, [this] {
                emit textSourceChanged(m_textSource->toPlainText());
            });
        } else if (qstrcmp(spec.objectName, "textJson") == 0) {
            m_textJson = box;
            m_textJson->setReadOnly(false);
        } else {
            m_textMotd = box;
        }
    }
    Theme::markSegmented(segment, segments);
    segments.first()->setChecked(true);
    for (int i = 0; i < segments.size(); ++i) {
        connect(segments.at(i), &QPushButton::clicked, outStack, [outStack, i] { outStack->setCurrentIndex(i); });
    }
    outHead->addWidget(segment);
    outHead->addStretch(1);
    auto *copy = makeGlyphButton(outCard, QStringLiteral("textOutputCopy"), Glyph::Copy,
                                 QCoreApplication::translate("ToolboxPage", "复制当前这一种写法"), QCoreApplication::translate("ToolboxPage", "复制"));
    outHead->addWidget(copy);
    outBox->addLayout(outHead);
    outBox->addWidget(outStack, 1);
    outBox->addWidget(fromJson, 0, Qt::AlignRight);
    fromJson->hide();
    connect(outStack, &QStackedWidget::currentChanged, fromJson, [outStack, fromJson, this] {
        fromJson->setVisible(outStack->currentWidget() == m_textJson);
    });
    textRight->addWidget(outCard, 1);
    textBody->addLayout(textRight, 5);
    layout->addLayout(textBody);
    new NarrowFlip(panel, textBody, 720);  // 内容宽 < 720（并排的最小宽约 690）：左右两块上下叠放
    connect(copy, &QPushButton::clicked, this, [outStack]() {
        if (auto *box = qobject_cast<QPlainTextEdit *>(outStack->currentWidget())) copyToClipboard(box->toPlainText());
    });
    layout->addStretch(1);

    connect(fromJson, &QPushButton::clicked, this,
            [this]() { emit textJsonRequested(m_textJson->toPlainText()); });
    connect(clear, &QPushButton::clicked, m_textPreview, &QTextEdit::clear);
    return panel;
}

QString ToolboxPage::textSource() const {
    return m_textSource != nullptr ? m_textSource->toPlainText() : QString();
}

void ToolboxPage::setTextSource(const QString &text) {
    if (m_textSource != nullptr) {
        m_textSource->setPlainText(text);  // 这一步自己会发 textChanged → 那一发重算一遍
    }
}

void ToolboxPage::setTextPalette(const QList<PaletteEntry> &entries) {
    if (m_textPaletteLayout == nullptr || m_textPaletteRow == nullptr) {
        return;
    }
    // 整排重来（喂进来的表是唯一出处，不叠加）。
    while (QLayoutItem *item = m_textPaletteLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    // 嵌套的行布局被 takeAt 删掉时不带走控件 —— 上一轮的钮 / 标签在这里一起收走。
    for (QWidget *old : m_textPaletteRow->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        old->hide();
        old->deleteLater();
    }
    // 两排：「颜色」一排十六颗圆色点，「格式」一排文字 chip（粗体 / 斜体 …… / 复位）。
    auto *colorRow = new FlowLayout(2, 2);
    auto *styleRow = new FlowLayout(6, 6);
    for (const PaletteEntry &entry : entries) {
        auto *button = new QPushButton(m_textPaletteRow);
        button->setObjectName(QStringLiteral("textCode_%1").arg(entry.code));
        button->setToolTip(QCoreApplication::translate("ToolboxPage", "%1（§%2）").arg(entry.name, entry.code));
        button->setAccessibleName(button->toolTip());
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        if (entry.isColor) {
            // 颜色码那一颗：画成一颗它自己颜色的圆点，字母用反差色（颜色就是它的样子）。
            const QColor color(entry.rgbHex);
            QPixmap dot(48, 48);
            dot.setDevicePixelRatio(2.0);
            dot.fill(Qt::transparent);
            {
                QPainter p(&dot);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(QPen(Theme::swatchBorder(color), 1.0));
                p.setBrush(color);
                p.drawEllipse(QRectF(1.5, 1.5, 21, 21));
                QFont font = button->font();
                font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));  // 21 px 圆点里一个字符，13 放得下
                font.setWeight(QFont::DemiBold);
                p.setFont(font);
                p.setPen(color.lightness() < 140 ? QColor(255, 255, 255) : QColor(0, 0, 0));
                p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, entry.code.toUpper());
            }
            Theme::markButton(button, Theme::ButtonKind::Ghost);
            button->setIcon(QIcon(dot));
            button->setIconSize(QSize(24, 24));
            button->setStyleSheet(QStringLiteral(
                "QPushButton { padding: 0px; min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; }"));
            colorRow->addWidget(button);
        } else {
            // 格式码那一颗：它没有"自己的色"，就按普通 chip 画，字写它的意思。
            Theme::markChipButton(button);
            button->setFixedHeight(28);
            button->setStyleSheet(QStringLiteral("padding: 2px 10px; min-height: 18px;"));
            button->setText(entry.code == QStringLiteral("r") ? QCoreApplication::translate("ToolboxPage", "复位") : entry.name);
            styleRow->addWidget(button);
        }
        const QString code = entry.code;
        connect(button, &QPushButton::clicked, this, [this, entry, code] {
            if (entry.isColor) {
                applyTextPreviewColor(QColor(entry.rgbHex), QStringLiteral("§%1").arg(code));
                return;
            }
            QTextCharFormat format;
            const QTextCharFormat current = m_textPreview->currentCharFormat();
            if (code == QStringLiteral("l")) format.setFontWeight(current.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
            if (code == QStringLiteral("o")) format.setFontItalic(!current.fontItalic());
            if (code == QStringLiteral("n")) format.setFontUnderline(!current.fontUnderline());
            if (code == QStringLiteral("m")) format.setFontStrikeOut(!current.fontStrikeOut());
            if (code == QStringLiteral("k")) format.setProperty(QTextFormat::UserProperty + 1, !current.boolProperty(QTextFormat::UserProperty + 1));
            if (code == QStringLiteral("r")) {
                format.setForeground(m_textPreview->palette().brush(QPalette::Text));
                format.setProperty(QTextFormat::UserProperty, QStringLiteral(""));
                format.setProperty(QTextFormat::UserProperty + 1, false);
                format.setFontWeight(QFont::Normal);
                format.setFontItalic(false);
                format.setFontUnderline(false);
                format.setFontStrikeOut(false);
            }
            m_textPreview->mergeCurrentCharFormat(format);
            m_textPreview->setFocus();
        });
    }
    const auto caption = [this](const QString &text) {
        auto *label = makeMutedLabel(m_textPaletteRow);
        label->setText(text);
        label->setWordWrap(false);
        return label;
    };
    m_textPaletteLayout->addWidget(caption(QCoreApplication::translate("ToolboxPage", "颜色")), 0, 0, Qt::AlignTop);
    m_textPaletteLayout->addLayout(colorRow, 0, 1);
    m_textPaletteLayout->addWidget(caption(QCoreApplication::translate("ToolboxPage", "格式")), 1, 0, Qt::AlignTop);
    m_textPaletteLayout->addLayout(styleRow, 1, 1);
}


void ToolboxPage::setTextPreview(const QString &html) {
    if (m_textPreview != nullptr) {
        const QSignalBlocker blocker(m_textPreview);
        m_textPreview->setHtml(html);
    }
}

void ToolboxPage::setTextPreviewRuns(const QList<TextRun> &runs) {
    if (m_textPreview == nullptr) return;
    const QSignalBlocker blocker(m_textPreview);
    m_textPreview->clear();
    QTextCursor cursor(m_textPreview->document());
    for (const TextRun &run : runs) {
        QTextCharFormat format;
        if (run.color.isValid()) format.setForeground(run.color);
        format.setProperty(QTextFormat::UserProperty, run.colorCode);
        format.setProperty(QTextFormat::UserProperty + 1, run.obfuscated);
        format.setFontWeight(run.bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(run.italic);
        format.setFontUnderline(run.underline);
        format.setFontStrikeOut(run.strike);
        cursor.insertText(run.text, format);
    }
}

QList<ToolboxPage::TextRun> ToolboxPage::textPreviewRuns() const {
    QList<TextRun> runs;
    if (m_textPreview == nullptr) return runs;
    for (QTextBlock block = m_textPreview->document()->begin(); block.isValid(); block = block.next()) {
        if (block != m_textPreview->document()->begin()) {
            TextRun newline;
            newline.text = QStringLiteral("\n");
            runs.append(newline);
        }
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextFragment text = fragment.fragment();
            const QTextCharFormat format = text.charFormat();
            runs.append({text.text(), format.stringProperty(QTextFormat::UserProperty), {},
                         format.fontWeight() >= QFont::Bold, format.fontItalic(), format.fontUnderline(),
                         format.fontStrikeOut(), format.boolProperty(QTextFormat::UserProperty + 1)});
        }
    }
    return runs;
}

void ToolboxPage::applyTextPreviewColor(const QColor &color, const QString &code) {
    if (m_textPreview == nullptr) return;
    QTextCharFormat format;
    format.setForeground(MotdText::readableOn(color, m_textPreview->palette().color(QPalette::Base)));
    format.setProperty(QTextFormat::UserProperty, code);
    m_textPreview->mergeCurrentCharFormat(format);
    m_textPreview->setFocus();
}

void ToolboxPage::setTextOutputs(const QString &legacy, const QString &json) {
    if (m_textSource != nullptr && !legacy.isNull() && m_textSource->toPlainText() != legacy) {
        const QSignalBlocker blocker(m_textSource);
        m_textSource->setPlainText(legacy);
    }
    if (m_textJson != nullptr && m_textJson->toPlainText() != json) {
        m_textJson->setPlainText(json);
    }
}

void ToolboxPage::setTextMotd(const QString &motd) {
    if (m_textMotd != nullptr) {
        m_textMotd->setPlainText(motd);
    }
}

void ToolboxPage::insertTextAtCursor(const QString &text) {
    if (m_textSource != nullptr) {
        m_textSource->insertPlainText(text);  // 插在光标处：改的往往是中间那一段
    }
}

void ToolboxPage::setTextNotice(const QString &notice) {
    if (m_textNotice == nullptr) {
        return;
    }
    m_textNotice->setText(notice);
    m_textNotice->setVisible(!notice.isEmpty());
}

// ---------------------------------------------------------------------------
// 三、坐标换算
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildCoordPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    // ---------- 输入区：维度 / 三格坐标 / 从 tp 命令认 ----------
    FormGrid form(panel);
    m_coordDimension = new QComboBox(panel);
    m_coordDimension->setObjectName(QStringLiteral("coordDimension"));
    m_coordDimension->setMinimumWidth(200);
    m_coordDimension->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    form.add(QCoreApplication::translate("ToolboxPage", "所在维度"), m_coordDimension);

    const auto makeSpin = [panel](const QString &objectName, double value) {
        auto *spin = new QDoubleSpinBox(panel);
        spin->setObjectName(objectName);
        spin->setDecimals(2);
        spin->setRange(-30000000.0, 30000000.0);
        spin->setValue(value);
        return spin;
    };
    m_coordX = makeSpin(QStringLiteral("coordX"), 0.0);
    m_coordY = makeSpin(QStringLiteral("coordY"), 64.0);
    m_coordZ = makeSpin(QStringLiteral("coordZ"), 0.0);
    const auto axis = [panel](const QString &text) {
        auto *label = makeMutedLabel(panel);
        label->setText(text);
        label->setWordWrap(false);
        return label;
    };
    auto *convert = new QPushButton(QCoreApplication::translate("ToolboxPage", "换算"), panel);
    convert->setObjectName(QStringLiteral("coordConvert"));
    convert->setAutoDefault(false);
    Theme::markButton(convert, Theme::ButtonKind::Primary);
    // 「粘贴命令」：读剪贴板里 F3+C 那一行，当场认维度 + 坐标再换算。
    // 原先那一行「从命令读」输入框 + 「认一下」按钮在 bfeca25 收文案时删掉了，只剩"往坐标
    // 框里 Ctrl+V"这条没人看得出来的暗路（用户报的就是它没了）。
    auto *pasteCommand = new QPushButton(QStringLiteral("粘贴命令"), panel);
    pasteCommand->setObjectName(QStringLiteral("coordPasteTp"));
    pasteCommand->setToolTip(
        QStringLiteral("读剪贴板里 F3+C 复制的那一行，认出维度和坐标并直接换算。"));
    pasteCommand->setAutoDefault(false);
    Theme::markButton(pasteCommand, Theme::ButtonKind::Neutral);
    // 「换算」是这一块唯一的主钮，紧跟在三格坐标后面（同一行、靠右）。
    // 每个轴「字母 + 数字框」成一组，窄时整组折行，不把字母和框拆开。
    const auto axisGroup = [&](const QString &text, QDoubleSpinBox *spin) {
        auto *group = new QWidget(panel);
        auto *row = new QHBoxLayout(group);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        row->addWidget(axis(text));
        row->addWidget(makeNumberField(group, spin, false, 120));
        return group;
    };
    // 「换算」是这一块唯一的主钮，紧跟在三格坐标后面。
    form.addFlowRow(QCoreApplication::translate("ToolboxPage", "坐标"),
                    {axisGroup(QStringLiteral("X"), m_coordX), axisGroup(QStringLiteral("Y"), m_coordY),
                     axisGroup(QStringLiteral("Z"), m_coordZ), pasteCommand, convert});

    // 坐标框直接接受粘贴的 tp 命令（F3+C 复制的那一行）：粘进来的不是数字就交给 controller 认。
    for (QDoubleSpinBox *spin : {m_coordX, m_coordY, m_coordZ}) {
        new TpPasteFilter(spin, [this](const QString &text) { emit teleportParseRequested(text); });
    }
    layout->addLayout(form.grid);

    m_tpNotice = makeMutedLabel(panel, QStringLiteral("coordTpNotice"));
    m_tpNotice->setContentsMargins(kFormLabelWidth + 12, 0, 0, 0);
    m_tpNotice->hide();
    layout->addWidget(m_tpNotice);


    // ---------- 结果区：结论一行 + 其余几行 + tp 命令 ----------
    ResultBox result = makeResultBox(panel, QCoreApplication::translate("ToolboxPage", "换算结果"), QStringLiteral("coordResult"));
    m_coordResult = result.headline;
    setReport(m_coordResult, QString());

    auto *commandRow = new QHBoxLayout;
    commandRow->setContentsMargins(0, 6, 0, 0);
    commandRow->setSpacing(6);
    m_tpCommand = new QLineEdit(result.card);
    m_tpCommand->setObjectName(QStringLiteral("coordTpCommand"));
    m_tpCommand->setReadOnly(true);
    InputSizing::longValue(m_tpCommand, QCoreApplication::translate("ToolboxPage", "只读结果：整条 tp 命令，方便选中复制"));
    m_tpCommand->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *makeCommand = new QPushButton(QCoreApplication::translate("ToolboxPage", "生成 tp 命令"), result.card);
    makeCommand->setObjectName(QStringLiteral("coordMakeTp"));
    makeCommand->setAutoDefault(false);
    Theme::markButton(makeCommand, Theme::ButtonKind::Ghost);
    auto *copyCommand = makeGlyphButton(result.card, QStringLiteral("coordCopyTp"), Glyph::Copy,
                                        QCoreApplication::translate("ToolboxPage", "复制这条 tp 命令"));
    commandRow->addWidget(m_tpCommand, 1);
    commandRow->addWidget(makeCommand);
    commandRow->addWidget(copyCommand);
    result.card->box()->addLayout(commandRow);
    layout->addWidget(result.card);
    layout->addStretch(1);

    connect(pasteCommand, &QPushButton::clicked, this, [this]() {
        const QString text = QGuiApplication::clipboard()->text().trimmed();
        if (text.isEmpty()) {
            setTeleportParse(false,
                             QStringLiteral("剪贴板里没有东西。先在游戏里按 F3+C 复制坐标，再点这里。"));
            return;
        }
        emit teleportParseRequested(text);
    });
    connect(convert, &QPushButton::clicked, this, [this]() {
        emit coordinateRequested(m_coordDimension->currentIndex(), m_coordX->value(),
                                 m_coordY->value(), m_coordZ->value());
    });
    connect(makeCommand, &QPushButton::clicked, this, [this]() {
        emit teleportCommandRequested(m_coordDimension->currentIndex(), m_coordX->value(),
                                      m_coordY->value(), m_coordZ->value());
    });
    connect(copyCommand, &QPushButton::clicked, this,
            [this]() { copyToClipboard(m_tpCommand->text()); });
    return panel;
}

void ToolboxPage::setDimensionChoices(const QStringList &names) {
    if (m_coordDimension == nullptr) {
        return;
    }
    const int previous = m_coordDimension->currentIndex();
    m_coordDimension->clear();
    m_coordDimension->addItems(names);
    if (previous >= 0 && previous < m_coordDimension->count()) {
        m_coordDimension->setCurrentIndex(previous);
    }
}

int ToolboxPage::dimensionIndex() const {
    return m_coordDimension != nullptr ? m_coordDimension->currentIndex() : 0;
}

double ToolboxPage::coordinate(int axis) const {
    const QDoubleSpinBox *spin = axis == 0 ? m_coordX : (axis == 1 ? m_coordY : m_coordZ);
    return spin != nullptr ? spin->value() : 0.0;
}

void ToolboxPage::setCoordinates(int dimensionIndex, double x, double y, double z) {
    if (m_coordDimension != nullptr && dimensionIndex >= 0
        && dimensionIndex < m_coordDimension->count()) {
        m_coordDimension->setCurrentIndex(dimensionIndex);
    }
    if (m_coordX != nullptr) {
        m_coordX->setValue(x);
    }
    if (m_coordY != nullptr) {
        m_coordY->setValue(y);
    }
    if (m_coordZ != nullptr) {
        m_coordZ->setValue(z);
    }
}

void ToolboxPage::setCoordinateRows(const QStringList &rows) {
    if (m_coordResult == nullptr) {
        return;
    }
    // 第一行（目标维度那一格）当结论大字，其余几行当细项；复制的是整几行原文。
    setReport(m_coordResult, rows.join(QLatin1Char('\n')));
}

void ToolboxPage::setTeleportParse(bool ok, const QString &message) {
    if (m_tpNotice == nullptr) {
        return;
    }
    m_tpNotice->setText(message);
    m_tpNotice->setVisible(!message.isEmpty());
    // 认出来了那一条是好消息，用正文色；没认出来才是"要留意的"（这里不改色 ——
    // 这一页只说清"认出来了没有"，是否要紧由那句话自己说）。
    Q_UNUSED(ok);
}

void ToolboxPage::setTeleportCommand(const QString &command) {
    if (m_tpCommand != nullptr) {
        m_tpCommand->setText(command);
    }
}

// ---------------------------------------------------------------------------
// 四、服务器在线检测
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildServerPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    FormGrid form(panel);
    m_serverInput = new QLineEdit(panel);
    m_serverInput->setObjectName(QStringLiteral("serverAddress"));
    m_serverInput->setPlaceholderText(QCoreApplication::translate("ToolboxPage", "host:port，默认 25565"));
    InputSizing::shortValue(m_serverInput, 40);  // 域名:端口，常见不超过 30 个字符
    m_serverCheck = new QPushButton(QCoreApplication::translate("ToolboxPage", "检测"), panel);
    m_serverCheck->setObjectName(QStringLiteral("serverCheck"));
    m_serverCheck->setAutoDefault(false);
    Theme::markButton(m_serverCheck, Theme::ButtonKind::Primary);
    m_serverCancel = new QPushButton(QCoreApplication::translate("ToolboxPage", "取消"), panel);
    m_serverCancel->setObjectName(QStringLiteral("serverCancel"));
    m_serverCancel->setAutoDefault(false);
    Theme::markButton(m_serverCancel, Theme::ButtonKind::Ghost);
    m_serverCancel->setEnabled(false);
    m_serverCancel->hide();  // 只在检测中出现
    auto *line = form.addRow(QCoreApplication::translate("ToolboxPage", "服务器地址"), {m_serverInput, m_serverCheck, m_serverCancel});
    line->setStretchFactor(m_serverInput, 100);
    delete line->takeAt(line->count() - 1);
    line->addStretch(1);  // 地址框封顶后，「检测」紧跟在它后面，别被拉成一长条
    layout->addLayout(form.grid);

    // 结果区：左边服务器图标，右边 MOTD 与几行细项。
    auto *card = new SoftPanel(panel);
    card->box()->addWidget(makePanelCaption(card, QCoreApplication::translate("ToolboxPage", "检测结果")));
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);
    m_serverIcon = new QLabel(card);
    m_serverIcon->setObjectName(QStringLiteral("serverIcon"));
    m_serverIcon->setFixedSize(64, 64);
    m_serverIcon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    m_serverIcon->setScaledContents(false);
    m_serverIcon->hide();
    body->addWidget(m_serverIcon, 0, Qt::AlignTop);

    m_serverResult = new QLabel(card);
    m_serverResult->setObjectName(QStringLiteral("serverResult"));
    m_serverResult->setTextFormat(Qt::RichText);
    m_serverResult->setWordWrap(true);
    m_serverResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_serverResult->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    body->addWidget(m_serverResult, 1);
    card->box()->addLayout(body);
    card->hide();  // 没有结果不画
    layout->addWidget(card);
    auto *history = new SoftPanel(panel);
    history->box()->addWidget(makePanelCaption(history, QStringLiteral("本次检测记录")));
    m_serverHistory = new QListWidget(history);
    m_serverHistory->setObjectName(QStringLiteral("serverHistory"));
    m_serverHistory->setUniformItemSizes(true);
    m_serverHistory->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_serverHistory->setFixedHeight(112);
    history->box()->addWidget(m_serverHistory);
    history->hide();
    layout->addWidget(history);
    layout->addStretch(1);

    connect(m_serverCheck, &QPushButton::clicked, this,
            [this]() { emit serverCheckRequested(m_serverInput->text().trimmed()); });
    // 和按钮共享禁用状态，检测中敲回车不能偷偷再发起一趟请求。
    connect(m_serverInput, &QLineEdit::returnPressed, m_serverCheck, &QPushButton::click);
    connect(m_serverCancel, &QPushButton::clicked, this,
            [this]() { emit serverCheckCancelRequested(); });
    return panel;
}

QString ToolboxPage::serverAddress() const {
    return m_serverInput != nullptr ? m_serverInput->text().trimmed() : QString();
}

void ToolboxPage::setServerAddress(const QString &address) {
    if (m_serverInput != nullptr) {
        m_serverInput->setText(address);
    }
}

QString ToolboxPage::serverPlayerCountText(int online, int max) {
    if (online < 0) {
        return QString();  // 服务器没说在线人数：那一行整个不写（见头文件）。
    }
    if (max > 0) {
        return QStringLiteral("%1 / %2").arg(online).arg(max);
    }
    return QString::number(online);
}

void ToolboxPage::setServerView(const ServerView &view) {
    if (m_serverResult == nullptr) {
        return;
    }
    if (!view.icon.isNull()) {
        const qreal dpr = m_serverIcon->devicePixelRatioF();
        QPixmap icon = view.icon.scaled(m_serverIcon->size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        icon.setDevicePixelRatio(dpr);
        m_serverIcon->setPixmap(icon);
        m_serverIcon->show();
    } else {
        m_serverIcon->clear();
        m_serverIcon->hide();
    }
    const QString muted = Theme::tokens().secondary.name(QColor::HexRgb);
    QString html;
    // 细项：左边一列次要色的名字（定宽），右边是值。
    QString rows;
    const auto line = [&rows, &muted](const QString &name, const QString &value) {
        if (value.isEmpty()) {
            return;
        }
        rows.append(QStringLiteral("<tr><td style='color:%1;padding:3px 16px 0 0'>%2</td><td style='padding-top:3px'>%3</td></tr>")
                        .arg(muted, name.toHtmlEscaped(), value.toHtmlEscaped()));
    };
    if (!view.ok) {
        // 分类那一格用的是 `ServerPing::kindName()`（controller 摆进来的）：它比弹一句
        // socket 错误原文更好懂 —— 「连上了但没答话」和「连接被拒」不是同一件事。
        const QString headline = !view.kind.isEmpty()
                                     ? view.kind
                                     : (view.timedOut ? QCoreApplication::translate("ToolboxPage", "超时")
                                                      : QCoreApplication::translate("ToolboxPage", "检测失败"));
        html.append(QStringLiteral("<div style='font-size:16px;font-weight:600'>%1</div>").arg(headline.toHtmlEscaped()));
        html.append(QStringLiteral("<div style='color:%1;margin-top:4px'>%2</div>").arg(muted, view.error.toHtmlEscaped()));
    } else {
        // MOTD 那一行是**服务器自己配的富文本**（controller 已经把 § 码翻成 HTML 了）。
        html.append(QStringLiteral("<div style='font-size:14px'>%1</div>").arg(view.motd));
        line(QCoreApplication::translate("ToolboxPage", "版本"), view.version);
        line(QCoreApplication::translate("ToolboxPage", "在线"), view.players);
        line(QCoreApplication::translate("ToolboxPage", "延迟"), view.latency);
    }
    line(QCoreApplication::translate("ToolboxPage", "地址"), view.address);
    if (!rows.isEmpty()) {
        html.append(QStringLiteral("<table style='margin-top:8px' cellspacing='0' cellpadding='0'>%1</table>").arg(rows));
    }
    m_serverResult->setText(html);
    m_serverResult->parentWidget()->show();
    const QString key = view.addressKey.isEmpty() ? view.address.trimmed().toCaseFolded() : view.addressKey;
    if (m_serverHistory == nullptr || key.isEmpty()) return;
    constexpr int historyLimit = 20;
    ServerHistoryStat &stat = m_serverHistoryStats[key];
    ++stat.attempts;
    if (view.latencyMs >= 0) {
        if (stat.successes == 0 || view.latencyMs < stat.minMs) stat.minMs = view.latencyMs;
        if (stat.successes == 0 || view.latencyMs > stat.maxMs) stat.maxMs = view.latencyMs;
        ++stat.successes;
    }
    if (view.timedOut) ++stat.timeouts;
    const QString result = view.ok ? view.latency : !view.kind.isEmpty() ? view.kind
                          : view.timedOut ? QStringLiteral("超时") : QStringLiteral("检测失败");
    QString label = QStringLiteral("%1 · %2").arg(view.address, result);
    if (stat.attempts > 1) {
        if (stat.successes > 0) {
            label += QCoreApplication::translate("ToolboxPage", " · %1 次 %2–%3 ms")
                         .arg(stat.attempts)
                         .arg(stat.minMs)
                         .arg(stat.maxMs);
        }
        if (stat.timeouts > 0) {
            label += QCoreApplication::translate("ToolboxPage", " · 超时 %1 次").arg(stat.timeouts);
        }
    }
    QListWidgetItem *entry = nullptr;
    for (int row = 0; row < m_serverHistory->count(); ++row) {
        if (m_serverHistory->item(row)->data(Qt::UserRole).toString() == key) {
            entry = m_serverHistory->item(row);
            break;
        }
    }
    if (entry == nullptr) {
        if (m_serverHistory->count() >= historyLimit) delete m_serverHistory->takeItem(0);
        entry = new QListWidgetItem(m_serverHistory);
        entry->setData(Qt::UserRole, key);
    }
    entry->setText(label);
    entry->setToolTip(label);
    m_serverHistory->parentWidget()->show();
    m_serverHistory->scrollToItem(entry);
}

void ToolboxPage::clearServerResults() {
    if (m_serverResult != nullptr) {
        m_serverResult->clear();
        m_serverResult->parentWidget()->hide();
    }
    if (m_serverHistory != nullptr) {
        m_serverHistory->clear();
        m_serverHistory->parentWidget()->hide();
    }
    m_serverHistoryStats.clear();
    emit serverToolExited();
}

void ToolboxPage::setServerBusy(bool busy) {
    if (m_serverCheck != nullptr) {
        m_serverCheck->setEnabled(!busy);
        m_serverCheck->setText(busy ? QCoreApplication::translate("ToolboxPage", "检测中…") : QCoreApplication::translate("ToolboxPage", "检测"));
    }
    if (m_serverCancel != nullptr) {
        m_serverCancel->setEnabled(busy);
        m_serverCancel->setVisible(busy);
    }
}

// ---------------------------------------------------------------------------
// 五、信标计算
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildBeaconPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    // ---------- 输入区：两行、每行一对 ----------
    FormGrid form(panel);
    form.compact();
    m_beaconFrom = new QSpinBox(panel);
    m_beaconFrom->setObjectName(beaconFromSpinName());
    m_beaconFrom->setRange(0, 4);
    m_beaconFrom->setValue(0);
    m_beaconFrom->setSuffix(QCoreApplication::translate("ToolboxPage", " 级"));
    m_beaconFrom->setSpecialValueText(QCoreApplication::translate("ToolboxPage", "未搭建"));  // 0 = 还没搭
    m_beaconLevel = new QSpinBox(panel);
    m_beaconLevel->setObjectName(beaconLevelSpinName());
    m_beaconLevel->setRange(1, 4);
    m_beaconLevel->setValue(4);
    m_beaconLevel->setSuffix(QCoreApplication::translate("ToolboxPage", " 级"));
    form.add(QCoreApplication::translate("ToolboxPage", "现在已有"), makeNumberField(panel, m_beaconFrom, true, 64),
             QCoreApplication::translate("ToolboxPage", "想搭到"), makeNumberField(panel, m_beaconLevel, true, 64));

    // 第二排：底座材质 + 同一座金字塔上放几颗（「矿物折算」「多信标共用底座」）。
    m_beaconMaterial = new QComboBox(panel);
    m_beaconMaterial->setObjectName(beaconMaterialComboName());
    m_beaconMaterial->setMinimumWidth(132);
    m_beaconMaterial->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_beaconCount = new QSpinBox(panel);
    m_beaconCount->setObjectName(beaconCountSpinName());
    m_beaconCount->setRange(1, 9);
    m_beaconCount->setValue(1);
    m_beaconCount->setSuffix(QCoreApplication::translate("ToolboxPage", " 颗"));
    m_beaconCount->setToolTip(QCoreApplication::translate("ToolboxPage", "可共用底座，最多 9 颗。"));
    form.add(QCoreApplication::translate("ToolboxPage", "底座方块"), m_beaconMaterial,
             QCoreApplication::translate("ToolboxPage", "信标颗数"), makeNumberField(panel, m_beaconCount, true, 64));
    layout->addLayout(form.grid);

    // 下半：左边结果，右边光柱配色（玻璃清单 + 光柱示意），整页在 900×620 下不出纵向滚动条。
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);

    ResultBox result = makeResultBox(panel, QCoreApplication::translate("ToolboxPage", "结果"), beaconResultName());
    m_beaconResult = result.headline;
    setReport(m_beaconResult, QString());
    result.card->box()->addStretch(1);  // 文字贴着上沿，面板跟右边那块等高
    body->addWidget(result.card, 5);

    // ---------- 光柱配色：一块面板，上面选色 + 清单操作，下面左边玻璃清单、右边光柱示意（放在柔和底台上） ----------
    auto *beamCard = new SoftPanel(panel);
    auto *beamHead = new QHBoxLayout;
    beamHead->setContentsMargins(0, 0, 0, 0);
    beamHead->setSpacing(4);
    beamHead->addWidget(makePanelCaption(beamCard, QCoreApplication::translate("ToolboxPage", "光柱配色 · 从下往上")));
    beamHead->addStretch(1);
    beamCard->box()->addLayout(beamHead);

    auto *glassColor = new QComboBox(beamCard);
    glassColor->setObjectName(QStringLiteral("beamGlassColor"));
    glassColor->setMinimumWidth(110);
    auto *glassAdd = makeGlyphButton(beamCard, QStringLiteral("beamGlassAdd"), Glyph::Plus,
                                     QCoreApplication::translate("ToolboxPage", "把这一色玻璃加到最上面"), QCoreApplication::translate("ToolboxPage", "添加玻璃"), 32);
    auto *glassList = new QListWidget(beamCard);
    glassList->setObjectName(QStringLiteral("beamGlassList"));
    glassList->setFrameShape(QFrame::NoFrame);
    glassList->setIconSize(QSize(14, 14));
    glassList->setMinimumHeight(kBeamBodyHeight);
    glassList->setMaximumHeight(kBeamBodyHeight);
    glassList->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"
                                            "QListWidget::item { padding: 3px 6px; border-radius: 6px; }"));
    // 上移 / 下移 / 删除：一组小图标钮，靠右挨着「添加」那一行。
    const QList<QPair<Glyph, QString>> actions{{Glyph::Up, QCoreApplication::translate("ToolboxPage", "上移")},
                                               {Glyph::Down, QCoreApplication::translate("ToolboxPage", "下移")},
                                               {Glyph::Remove, QCoreApplication::translate("ToolboxPage", "删除")}};
    QList<QWidget *> group;
    const auto recalculate = [this, glassList] {
        QList<int> colors;
        for (int i = 0; i < glassList->count(); ++i) colors.append(glassList->item(i)->data(Qt::UserRole).toInt());
        emit beamRequested(colors);
    };
    for (int action = 0; action < 3; ++action) {
        auto *button = makeGlyphButton(beamCard, QStringLiteral("beamGlassAction%1").arg(action),
                                       actions[action].first, actions[action].second);
        group.append(button);
        connect(button, &QPushButton::clicked, this, [glassList, action, recalculate] {
            const int row = glassList->currentRow(); if (row < 0) return;
            if (action == 2) delete glassList->takeItem(row);
            else {
                const int to = row + (action == 0 ? -1 : 1);
                if (to < 0 || to >= glassList->count()) return;
                auto *item = glassList->takeItem(row); glassList->insertItem(to, item); glassList->setCurrentRow(to);
            }
            recalculate();
        });
    }
    beamCard->box()->addLayout(makeButtonRow({glassColor, glassAdd}, group));

    auto *glassBody = new QHBoxLayout;
    glassBody->setContentsMargins(0, 0, 0, 0);
    glassBody->setSpacing(12);
    glassBody->addWidget(glassList, 1);
    auto *beam = new QLabel(beamCard);
    beam->setObjectName(QStringLiteral("beamColorPreview"));
    beam->setFixedSize(72, kBeamBodyHeight);
    beam->setAlignment(Qt::AlignCenter);
    glassBody->addWidget(beam, 0, Qt::AlignTop);
    beamCard->box()->addLayout(glassBody);

    auto *beamReport = new QLabel(QCoreApplication::translate("ToolboxPage", "未放置染色玻璃：白色光柱。"), beamCard);
    beamReport->setObjectName(QStringLiteral("beamColorReport")); beamReport->setWordWrap(true);
    beamReport->setTextFormat(Qt::PlainText);
    beamReport->setTextInteractionFlags(Qt::TextSelectableByMouse);
    beamCard->box()->addWidget(beamReport);
    beamCard->box()->addStretch(1);
    auto *reportCopy = makeGlyphButton(beamCard, QStringLiteral("beamColorReportCopy"), Glyph::Copy,
                                       QCoreApplication::translate("ToolboxPage", "复制配色说明"), QCoreApplication::translate("ToolboxPage", "复制"));
    reportCopy->setEnabled(false);
    beamHead->addWidget(reportCopy);
    connect(reportCopy, &QPushButton::clicked, beamReport, [beamReport] {
        const auto text = beamReport->property("copyText").toString();
        if (!text.isEmpty()) copyToClipboard(text);
    });
    body->addWidget(beamCard, 6);
    layout->addLayout(body);
    // 内容宽 < 640：两块上下叠放。
    new NarrowFlip(panel, body, 640);
    layout->addStretch(1);

    connect(glassAdd, &QPushButton::clicked, this, [glassList, glassColor, recalculate] {
        if (glassList->count() >= 64 || glassColor->currentIndex() < 0) return;
        auto *item = new QListWidgetItem(glassColor->currentText() + QCoreApplication::translate("ToolboxPage", "玻璃"), glassList);
        item->setData(Qt::UserRole, glassColor->currentIndex());
        glassList->setCurrentItem(item); recalculate();
    });
    paintBeamPreview(beam, {});
    Theme::follow(beam, [beam] { paintBeamPreview(beam, beam->property("beamSegments").value<QList<QColor>>()); });

    const auto emitNow = [this]() {
        emit beaconRequested(m_beaconFrom->value(), m_beaconLevel->value(),
                             m_beaconMaterial->currentIndex(), m_beaconCount->value());
    };
    connect(m_beaconFrom, &QSpinBox::valueChanged, this, [emitNow](int) { emitNow(); });
    connect(m_beaconLevel, &QSpinBox::valueChanged, this, [emitNow](int) { emitNow(); });
    connect(m_beaconCount, &QSpinBox::valueChanged, this, [emitNow](int) { emitNow(); });
    connect(m_beaconMaterial, &QComboBox::currentIndexChanged, this, [emitNow](int) { emitNow(); });
    return panel;
}

void ToolboxPage::setBeamColors(const QStringList &names) {
    auto *combo = findChild<QComboBox *>(QStringLiteral("beamGlassColor"));
    if (combo != nullptr) { combo->clear(); combo->addItems(names); }
}

void ToolboxPage::setBeamLayers(const QList<int> &colors) {
    auto *list = findChild<QListWidget *>(QStringLiteral("beamGlassList"));
    auto *combo = findChild<QComboBox *>(QStringLiteral("beamGlassColor"));
    list->clear();
    for (int color : colors.mid(0, 64)) {
        if (color < 0 || color >= combo->count()) continue;
        auto *item = new QListWidgetItem(combo->itemText(color) + QCoreApplication::translate("ToolboxPage", "玻璃"), list);
        item->setData(Qt::UserRole, color);
    }
    emit beamRequested(colors.mid(0, 64));
}
void ToolboxPage::setBeamResult(const QList<QColor> &segments, const QString &report) {
    auto *label = findChild<QLabel *>(QStringLiteral("beamColorReport"));
    if (label != nullptr) {
        // 逐层那几行（「第 n 层 / … → 色」）由清单里每行的色点表达，说明里只留汇总；复制拿到的仍是整份。
        QStringList shown;
        for (const QString &line : report.split(QLatin1Char('\n')))
            if (!line.contains(QStringLiteral(" → "))) shown << line;
        label->setText(shown.join(QLatin1Char('\n')));
        label->setToolTip(report);
        updateResultCopy(label, report);
    }
    if (auto *list = findChild<QListWidget *>(QStringLiteral("beamGlassList"))) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setIcon(i < segments.size() ? colorDotIcon(segments.at(i)) : QIcon());
    }
    if (auto *preview = findChild<QLabel *>(QStringLiteral("beamColorPreview"))) paintBeamPreview(preview, segments);
}

void ToolboxPage::setBeaconMaterials(const QStringList &names) {
    if (m_beaconMaterial == nullptr) return;
    const QSignalBlocker blocker(m_beaconMaterial);
    m_beaconMaterial->clear();
    m_beaconMaterial->addItems(names);
    if (m_beaconMaterial->count() > 0 && m_beaconMaterial->currentIndex() < 0) {
        m_beaconMaterial->setCurrentIndex(0);
    }
}

int ToolboxPage::beaconMaterialIndex() const {
    return m_beaconMaterial != nullptr ? m_beaconMaterial->currentIndex() : -1;
}

int ToolboxPage::beaconCount() const {
    return m_beaconCount != nullptr ? m_beaconCount->value() : 1;
}

void ToolboxPage::setBeaconLevels(int fromLevel, int targetLevel) {
    if (m_beaconFrom != nullptr) {
        const QSignalBlocker blocker(m_beaconFrom);
        m_beaconFrom->setValue(fromLevel);
    }
    if (m_beaconLevel != nullptr) {
        const QSignalBlocker blocker(m_beaconLevel);
        m_beaconLevel->setValue(targetLevel);
    }
}

int ToolboxPage::beaconFromLevel() const {
    return m_beaconFrom != nullptr ? m_beaconFrom->value() : 0;
}

int ToolboxPage::beaconTargetLevel() const {
    return m_beaconLevel != nullptr ? m_beaconLevel->value() : 1;
}

void ToolboxPage::setBeaconResult(const QString &text) {
    if (m_beaconResult != nullptr) {
        setReport(m_beaconResult, text);
    }
}

// ---------------------------------------------------------------------------
// 六、酿造助手
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildBrewingPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    // 输入区拆成三行：做什么（药水 / 形态）、做多少（瓶数 / 目标版本）、加料（两个开关）。
    FormGrid form(panel);
    m_brewingPotion = new QComboBox(panel);
    m_brewingPotion->setObjectName(brewingPotionComboName());
    m_brewingForm = new QComboBox(panel);
    m_brewingForm->setObjectName(brewingFormComboName());
    form.add(QCoreApplication::translate("ToolboxPage", "要做"), m_brewingPotion, QCoreApplication::translate("ToolboxPage", "形态"), m_brewingForm);

    m_brewingBottles = new QSpinBox(panel);
    m_brewingBottles->setObjectName(brewingBottlesSpinName());
    m_brewingBottles->setRange(1, 64);
    m_brewingBottles->setValue(1);
    m_brewingBottles->setSuffix(QCoreApplication::translate("ToolboxPage", " 瓶"));
    auto *version = new QComboBox(panel);
    version->setObjectName(brewingVersionComboName());
    form.add(QCoreApplication::translate("ToolboxPage", "瓶数"), makeNumberField(panel, m_brewingBottles, true, 72),
             QCoreApplication::translate("ToolboxPage", "目标版本"), version);

    m_brewingExtended = new QCheckBox(QCoreApplication::translate("ToolboxPage", "延时（红石）"), panel);
    m_brewingExtended->setObjectName(brewingExtendedName());
    m_brewingLevel2 = new QCheckBox(QCoreApplication::translate("ToolboxPage", "升一级（荧石）"), panel);
    m_brewingLevel2->setObjectName(brewingLevel2Name());
    form.addRow(QCoreApplication::translate("ToolboxPage", "加料"), {m_brewingExtended, m_brewingLevel2})->setSpacing(24);
    layout->addLayout(form.grid);

    ResultBox result = makeResultBox(panel, QCoreApplication::translate("ToolboxPage", "结果"), brewingResultName());
    m_brewingResult = result.headline;
    setReport(m_brewingResult, QString());
    layout->addWidget(result.card);
    layout->addStretch(1);

    const auto emitNow = [this]() {
        emit brewingRequested(m_brewingPotion->currentIndex(), m_brewingExtended->isChecked(),
                              m_brewingLevel2->isChecked(), m_brewingForm->currentIndex(),
                              m_brewingBottles->value());
    };
    connect(m_brewingPotion, &QComboBox::currentIndexChanged, this,
            [emitNow](int) { emitNow(); });
    connect(m_brewingExtended, &QCheckBox::toggled, this, [emitNow](bool) { emitNow(); });
    connect(m_brewingLevel2, &QCheckBox::toggled, this, [emitNow](bool) { emitNow(); });
    connect(m_brewingForm, &QComboBox::currentIndexChanged, this,
            [emitNow](int) { emitNow(); });
    connect(m_brewingBottles, &QSpinBox::valueChanged, this, [emitNow](int) { emitNow(); });
    connect(version, &QComboBox::currentIndexChanged, this, [emitNow](int) { emitNow(); });
    return panel;
}

void ToolboxPage::setBrewingPotions(const QStringList &names) {
    if (m_brewingPotion == nullptr) {
        return;
    }
    // 程序摆的那一份**不发**信号（controller 填完表之后会自己算一次）。
    const QSignalBlocker blocker(m_brewingPotion);
    m_brewingPotion->clear();
    m_brewingPotion->addItems(names);
}

void ToolboxPage::setBrewingForms(const QStringList &names) {
    if (m_brewingForm == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_brewingForm);
    m_brewingForm->clear();
    m_brewingForm->addItems(names);
}

void ToolboxPage::setBrewingVersions(const QStringList &versions) {
    auto *combo = findChild<QComboBox *>(brewingVersionComboName());
    if (combo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto &version : versions) {
        combo->addItem(version.isEmpty() ? QCoreApplication::translate("ToolboxPage", "不限版本") : version, version);
    }
}

QString ToolboxPage::brewingVersion() const {
    const auto *combo = findChild<QComboBox *>(brewingVersionComboName());
    return combo != nullptr ? combo->currentData().toString() : QString();
}

void ToolboxPage::setBrewingResult(const QString &text) {
    if (m_brewingResult != nullptr) {
        setReport(m_brewingResult, text);
    }
}

int ToolboxPage::brewingPotionIndex() const {
    return m_brewingPotion != nullptr ? m_brewingPotion->currentIndex() : -1;
}

bool ToolboxPage::brewingExtended() const {
    return m_brewingExtended != nullptr && m_brewingExtended->isChecked();
}

bool ToolboxPage::brewingLevel2() const {
    return m_brewingLevel2 != nullptr && m_brewingLevel2->isChecked();
}

int ToolboxPage::brewingFormIndex() const {
    return m_brewingForm != nullptr ? m_brewingForm->currentIndex() : 0;
}

int ToolboxPage::brewingBottles() const {
    return m_brewingBottles != nullptr ? m_brewingBottles->value() : 1;
}

// ---------------------------------------------------------------------------
// 七、旗帜设计
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildBannerPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);


    // ---------- 输入区：一行 —— 版本、底色，右边保存 / 载入 ----------
    FormGrid form(panel);
    auto *version = new QComboBox(panel);
    version->setObjectName(QStringLiteral("bannerGameVersion"));
    version->setMinimumWidth(140);
    m_bannerBase = new QComboBox(panel);
    m_bannerBase->setObjectName(bannerBaseComboName());
    m_bannerBase->setMinimumWidth(120);
    // 版本与底色是一排（放不下就折行），保存 / 载入靠右。
    auto *baseGroup = new QWidget(panel);
    auto *baseGroupRow = new QHBoxLayout(baseGroup);
    baseGroupRow->setContentsMargins(0, 0, 0, 0);
    baseGroupRow->setSpacing(10);
    baseGroupRow->addWidget(form.label(QCoreApplication::translate("ToolboxPage", "底色")));
    baseGroupRow->addWidget(m_bannerBase);
    auto *versionFlow = new FlowLayout(16, 8);
    versionFlow->addWidget(version);
    versionFlow->addWidget(baseGroup);
    auto *versionLine = new QHBoxLayout;
    versionLine->setContentsMargins(0, 0, 0, 0);
    versionLine->setSpacing(10);
    versionLine->addLayout(versionFlow, 1);
    QLabel *versionLabel = form.label(QCoreApplication::translate("ToolboxPage", "目标版本"));
    versionLabel->setContentsMargins(0, 8, 0, 0);  // 与第一行下拉框的文字对齐
    form.grid->addWidget(versionLabel, 0, 0, Qt::AlignTop);
    form.grid->addLayout(versionLine, 0, 1, 1, 4);
    for (bool save : {true, false}) {
        auto *button = new QPushButton(save ? QCoreApplication::translate("ToolboxPage", "保存设计…") : QCoreApplication::translate("ToolboxPage", "载入设计…"), panel);
        button->setObjectName(save ? QStringLiteral("bannerSaveDesign") : QStringLiteral("bannerLoadDesign"));
        button->setToolTip(save ? QCoreApplication::translate("ToolboxPage", "保存 / 导出这面旗的设计") : QCoreApplication::translate("ToolboxPage", "载入 / 导入一份旗帜设计"));
        button->setAutoDefault(false);
        Theme::markButton(button, Theme::ButtonKind::Ghost);
        versionLine->addWidget(button, 0, Qt::AlignTop);
        connect(button, &QPushButton::clicked, this, [this, save] {
            if (save) emit bannerSaveRequested(); else emit bannerLoadRequested();
        });
    }
    connect(version, &QComboBox::currentIndexChanged, this, [this] { emitBannerNow(); });
    layout->addLayout(form.grid);

    // ---------- 下半：左边图层卡（选图案 + 颜色 → 加一层；旗面预览在左、清单在右），右边结果卡（分段看步骤 / 材料 / 指令） ----------
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);

    auto *layerCard = new SoftPanel(panel);
    auto *layerHead = new QHBoxLayout;
    layerHead->setContentsMargins(0, 0, 0, 0);
    layerHead->setSpacing(2);
    layerHead->addWidget(makePanelCaption(layerCard, QCoreApplication::translate("ToolboxPage", "图层（从下往上）")));
    layerHead->addStretch(1);
    m_bannerUp = makeGlyphButton(layerCard, bannerUpName(), Glyph::Up, QCoreApplication::translate("ToolboxPage", "上移"));
    m_bannerDown = makeGlyphButton(layerCard, bannerDownName(), Glyph::Down, QCoreApplication::translate("ToolboxPage", "下移"));
    m_bannerRemove = makeGlyphButton(layerCard, bannerRemoveName(), Glyph::Remove, QCoreApplication::translate("ToolboxPage", "删除这一层"));
    m_bannerUndo = makeGlyphButton(layerCard, bannerUndoName(), Glyph::Undo,
                                   QCoreApplication::translate("ToolboxPage", "撤销：把列表退回上一步（最多留最近二十步）"));
    for (QPushButton *button : {m_bannerUp, m_bannerDown, m_bannerRemove, m_bannerUndo}) layerHead->addWidget(button);
    layerCard->box()->addLayout(layerHead);

    // 新的一层：图案 · 颜色 · 加一层，一行放下。
    m_bannerPattern = new QComboBox(layerCard);
    m_bannerPattern->setObjectName(bannerPatternComboName());
    m_bannerPattern->setMinimumWidth(96);
    m_bannerColor = new QComboBox(layerCard);
    m_bannerColor->setObjectName(bannerColorComboName());
    m_bannerColor->setMinimumWidth(96);
    m_bannerAdd = new QPushButton(QCoreApplication::translate("ToolboxPage", "加一层"), layerCard);
    m_bannerAdd->setObjectName(bannerAddName());
    m_bannerAdd->setAutoDefault(false);
    Theme::markButton(m_bannerAdd, Theme::ButtonKind::Primary);
    auto *addRow = new QHBoxLayout;
    addRow->setContentsMargins(0, 0, 0, 0);
    addRow->setSpacing(8);
    addRow->addWidget(m_bannerPattern, 3);
    addRow->addWidget(m_bannerColor, 2);
    addRow->addWidget(m_bannerAdd);
    layerCard->box()->addLayout(addRow);

    auto *bannerPreviewRow = new QHBoxLayout;
    bannerPreviewRow->setContentsMargins(0, 0, 0, 0);
    bannerPreviewRow->setSpacing(14);
    auto *bannerPreview = new QLabel(layerCard);
    bannerPreview->setObjectName(QStringLiteral("bannerVisualPreview"));
    bannerPreview->setFixedSize(108, kBannerBodyHeight); bannerPreview->setAlignment(Qt::AlignCenter);
    bannerPreview->setPixmap(framedPreview(QImage(), bannerPreview->size(), bannerPreview->devicePixelRatioF()));
    // 底台与描边取主题色，换主题时用记下的那张图重画。
    Theme::follow(bannerPreview, [bannerPreview] {
        bannerPreview->setPixmap(framedPreview(bannerPreview->property("bannerImage").value<QImage>(),
                                               bannerPreview->size(), bannerPreview->devicePixelRatioF()));
    });
    bannerPreviewRow->addWidget(bannerPreview, 0, Qt::AlignTop);
    m_bannerList = new QListWidget(layerCard);
    m_bannerList->setObjectName(bannerListName());
    m_bannerList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bannerList->setDragDropMode(QAbstractItemView::InternalMove);
    m_bannerList->setDefaultDropAction(Qt::MoveAction);
    m_bannerList->setDropIndicatorShown(true);
    m_bannerList->setFrameShape(QFrame::NoFrame);
    m_bannerList->setIconSize(QSize(14, 14));
    m_bannerList->setFixedHeight(kBannerBodyHeight);
    m_bannerList->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; }"
                                               "QListWidget::item { padding: 4px 6px; border-radius: 6px; }"));
    bannerPreviewRow->addWidget(m_bannerList, 1);
    layerCard->box()->addLayout(bannerPreviewRow);

    auto *bannerNotice = makeMutedLabel(layerCard, QStringLiteral("bannerVisualNotice"));
    bannerNotice->setText(QCoreApplication::translate("ToolboxPage", "加载游戏贴图后显示预览"));
    bannerNotice->setMinimumWidth(0); bannerNotice->setTextFormat(Qt::PlainText);
    auto *loadTextures = new QPushButton(QCoreApplication::translate("ToolboxPage", "加载贴图…"), layerCard);
    loadTextures->setObjectName(QStringLiteral("bannerLoadTextures"));
    loadTextures->setToolTip(QCoreApplication::translate("ToolboxPage", "加载游戏 JAR / 资源包贴图"));
    Theme::markButton(loadTextures, Theme::ButtonKind::Ghost); loadTextures->setAutoDefault(false);
    // 说明句独占卡宽那一行，「加载贴图…」紧跟其下、左对齐：两句并排时说明句被压窄，
    // 四十个字折成三行、按钮悬在折行文字右边（走查 j05）。挤在一行省不下高度 ——
    // 900×620 下面板还有余量，见 `wtfml.toolbox-layout` 的纵向滚动条那条。
    auto *noticeRow = new QHBoxLayout;
    noticeRow->setContentsMargins(0, 0, 0, 0);
    noticeRow->addWidget(bannerNotice);
    layerCard->box()->addLayout(noticeRow);
    auto *textureRow = new QHBoxLayout;
    textureRow->setContentsMargins(0, 0, 0, 0);
    textureRow->addWidget(loadTextures, 0, Qt::AlignLeft);
    textureRow->addStretch(1);
    layerCard->box()->addLayout(textureRow);
    connect(loadTextures, &QPushButton::clicked, this, &ToolboxPage::bannerTexturesRequested);
    body->addWidget(layerCard, 1);

    // ---------- 结果卡：右上「复制指令」；结论一行，下面分段看 步骤 / 材料 / 指令（框高跟着左边那张卡） ----------
    ResultBox result = makeResultBox(panel, QCoreApplication::translate("ToolboxPage", "结果"), bannerResultName(), false);
    m_bannerResult = result.headline;
    m_bannerCopy = makeGlyphButton(result.card, QStringLiteral("toolboxBannerCopy"), Glyph::Copy,
                                   QCoreApplication::translate("ToolboxPage", "把 /give 指令复制走（1.13–1.20.4 的写法）"),
                                   QCoreApplication::translate("ToolboxPage", "复制指令"));
    m_bannerCopy->setEnabled(false);
    result.head->addWidget(m_bannerCopy);
    connect(m_bannerCopy, &QPushButton::clicked, this,
            [this]() { copyToClipboard(m_bannerCommand); });
    result.details->hide();  // 细项改由下面三个分段框装，这一格不再显示
    {
        const struct { const char *objectName; QString title; } kTabs[] = {
            {"bannerStepsBox", QCoreApplication::translate("ToolboxPage", "步骤")}, {"bannerMaterialsBox", QCoreApplication::translate("ToolboxPage", "材料")}, {"bannerCommandBox", QCoreApplication::translate("ToolboxPage", "指令")}};
        auto *segment = new QWidget(result.card);
        segment->setObjectName(QStringLiteral("bannerResultSegments"));
        auto *segmentLayout = new QHBoxLayout(segment);
        segmentLayout->setContentsMargins(3, 3, 3, 3);
        segmentLayout->setSpacing(2);
        auto *resultStack = new QStackedWidget(result.card);
        resultStack->setObjectName(QStringLiteral("bannerResultStack"));
        QList<QPushButton *> tabs;
        for (const auto &spec : kTabs) {
            auto *tab = new QPushButton(spec.title, segment);
            segmentLayout->addWidget(tab);
            tabs.append(tab);
            QPlainTextEdit *box = makeOutputBox(result.card, QString::fromLatin1(spec.objectName));
            box->setMinimumHeight(60);
            box->setMaximumHeight(QWIDGETSIZE_MAX);  // 高度跟着卡片，不钉三行
            box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            if (qstrcmp(spec.objectName, "bannerCommandBox") != 0) box->setFont(QApplication::font());  // 步骤 / 材料是人话，不用等宽
            resultStack->addWidget(box);
        }
        Theme::markSegmented(segment, tabs);
        tabs.first()->setChecked(true);
        for (int i = 0; i < tabs.size(); ++i)
            connect(tabs.at(i), &QPushButton::clicked, resultStack, [resultStack, i] { resultStack->setCurrentIndex(i); });
        auto *segmentRow = new QHBoxLayout;
        segmentRow->setContentsMargins(0, 0, 0, 0);
        segmentRow->addWidget(segment);
        segmentRow->addStretch(1);
        result.card->box()->addLayout(segmentRow);
        result.card->box()->addWidget(resultStack, 1);
    }
    setReport(m_bannerResult, QString());
    result.card->show();  // 空结果也留着这张卡：左右两块的宽度不随内容跳
    body->addWidget(result.card, 1);
    layout->addLayout(body);
    layout->addStretch(1);
    // 内容宽 < 640：两块上下叠放。
    new NarrowFlip(panel, body, 640);

    const auto pushUndo = [this]() {
        m_bannerUndoStack.append(m_bannerLayers);
        while (m_bannerUndoStack.size() > 20) {
            m_bannerUndoStack.removeFirst();
        }
    };
    connect(m_bannerList->model(), &QAbstractItemModel::rowsMoved, this,
            [this, pushUndo]() {
        if (m_bannerSyncing) return;
        QStringList reordered;
        for (int i = 0; i < m_bannerList->count(); ++i)
            reordered.append(m_bannerList->item(i)->data(Qt::UserRole).toString());
        // Do not rebuild the model inside rowsMoved: the drop handler still owns its indexes.
        for (int i = 0; i < m_bannerList->count(); ++i) {
            const auto parts = reordered.at(i).split(QChar(':'));
            m_bannerList->item(i)->setText(QCoreApplication::translate("ToolboxPage", "第 %1 层：%2（%3）")
                .arg(i + 1).arg(m_bannerPattern->itemText(parts.value(0).toInt()))
                .arg(m_bannerColor->itemText(parts.value(1).toInt())));
        }
        if (reordered == m_bannerLayers) return;
        pushUndo();
        m_bannerLayers = reordered;
        m_bannerUndo->setEnabled(true);
        const int row = m_bannerList->currentRow();
        m_bannerUp->setEnabled(row > 0);
        m_bannerDown->setEnabled(row >= 0 && row + 1 < m_bannerList->count());
        m_bannerRemove->setEnabled(row >= 0);
        emitBannerNow();
    });
    connect(m_bannerBase, &QComboBox::currentIndexChanged, this,
            [this](int) { emitBannerNow(); });
    connect(m_bannerAdd, &QPushButton::clicked, this, [this, pushUndo]() {
        if (m_bannerLayers.size() >= 6 || m_bannerPattern->currentIndex() < 0) return;
        pushUndo();
        m_bannerLayers.append(QStringLiteral("%1:%2")
                                  .arg(m_bannerPattern->currentIndex())
                                  .arg(m_bannerColor->currentIndex()));
        syncBannerList();
        m_bannerList->setCurrentRow(m_bannerLayers.size() - 1);
        emitBannerNow();
    });
    connect(m_bannerUp, &QPushButton::clicked, this, [this, pushUndo]() {
        const int row = m_bannerList->currentRow();
        if (row <= 0 || row >= m_bannerLayers.size()) {
            return;
        }
        pushUndo();
        m_bannerLayers.swapItemsAt(row, row - 1);
        syncBannerList();
        m_bannerList->setCurrentRow(row - 1);
        emitBannerNow();
    });
    connect(m_bannerDown, &QPushButton::clicked, this, [this, pushUndo]() {
        const int row = m_bannerList->currentRow();
        if (row < 0 || row + 1 >= m_bannerLayers.size()) {
            return;
        }
        pushUndo();
        m_bannerLayers.swapItemsAt(row, row + 1);
        syncBannerList();
        m_bannerList->setCurrentRow(row + 1);
        emitBannerNow();
    });
    connect(m_bannerRemove, &QPushButton::clicked, this, [this, pushUndo]() {
        const int row = m_bannerList->currentRow();
        if (row < 0 || row >= m_bannerLayers.size()) {
            return;
        }
        pushUndo();
        m_bannerLayers.removeAt(row);
        syncBannerList();
        emitBannerNow();
    });
    connect(m_bannerUndo, &QPushButton::clicked, this, [this]() {
        if (m_bannerUndoStack.isEmpty()) {
            return;
        }
        m_bannerLayers = m_bannerUndoStack.takeLast();
        syncBannerList();
        emitBannerNow();
    });
    connect(m_bannerList, &QListWidget::currentRowChanged, this, [this](int) {
        if (m_bannerSyncing) {
            return;
        }
        const int row = m_bannerList->currentRow();
        m_bannerUp->setEnabled(row > 0);
        m_bannerDown->setEnabled(row >= 0 && row + 1 < m_bannerList->count());
        m_bannerRemove->setEnabled(row >= 0);
    });
    syncBannerList();
    return panel;
}

void ToolboxPage::syncBannerList() {
    if (m_bannerList == nullptr) {
        return;
    }
    m_bannerSyncing = true;
    const int row = m_bannerList->currentRow();
    m_bannerList->clear();
    for (int index = 0; index < m_bannerLayers.size(); ++index) {
        const QStringList parts = m_bannerLayers.at(index).split(QChar(':'));
        const int pattern = parts.value(0).toInt();
        const int color = parts.value(1).toInt();
        const QString patternName =
            m_bannerPattern != nullptr ? m_bannerPattern->itemText(pattern) : QString();
        const QString colorName =
            m_bannerColor != nullptr ? m_bannerColor->itemText(color) : QString();
        m_bannerList->addItem(QCoreApplication::translate("ToolboxPage", "第 %1 层：%2（%3）")
                                  .arg(index + 1)
                                  .arg(patternName)
                                  .arg(colorName));
        m_bannerList->item(index)->setData(Qt::UserRole, m_bannerLayers.at(index));
        m_bannerList->item(index)->setIcon(colorDotIcon(m_bannerSwatches.value(color)));
    }
    if (row >= 0 && row < m_bannerList->count()) {
        m_bannerList->setCurrentRow(row);
    }
    m_bannerSyncing = false;

    const int current = m_bannerList->currentRow();
    if (m_bannerUp != nullptr) {
        m_bannerUp->setEnabled(current > 0);
    }
    if (m_bannerDown != nullptr) {
        m_bannerDown->setEnabled(current >= 0 && current + 1 < m_bannerList->count());
    }
    if (m_bannerRemove != nullptr) {
        m_bannerRemove->setEnabled(current >= 0);
    }
    if (m_bannerUndo != nullptr) {
        m_bannerUndo->setEnabled(!m_bannerUndoStack.isEmpty());
    }
    if (m_bannerAdd != nullptr) {
        m_bannerAdd->setEnabled(m_bannerLayers.size() < 6);  // 原版最多叠 6 层，到上限「加一层」置灰
    }
}

void ToolboxPage::emitBannerNow() {
    emit bannerRequested(m_bannerBase != nullptr ? m_bannerBase->currentIndex() : -1,
                         m_bannerLayers);
}

void ToolboxPage::setBannerColors(const QStringList &names, const QList<QColor> &swatches) {
    if (m_bannerBase == nullptr) {
        return;
    }
    const QSignalBlocker blockBase(m_bannerBase);
    const QSignalBlocker blockColor(m_bannerColor);
    m_bannerBase->clear();
    m_bannerBase->addItems(names);
    m_bannerColor->clear();
    m_bannerColor->addItems(names);
    m_bannerSwatches = swatches;
    m_bannerBase->setIconSize(QSize(14, 14));
    m_bannerColor->setIconSize(QSize(14, 14));
    for (int i = 0; i < names.size() && i < swatches.size(); ++i) {
        m_bannerBase->setItemIcon(i, colorDotIcon(swatches.at(i)));
        m_bannerColor->setItemIcon(i, colorDotIcon(swatches.at(i)));
    }
    syncBannerList();  // 行里那截颜色名跟着换一套说法
}

void ToolboxPage::setBannerPatterns(const QStringList &names) {
    if (m_bannerPattern == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_bannerPattern);
    m_bannerPattern->clear();
    m_bannerPattern->addItems(names);
    syncBannerList();
}

void ToolboxPage::setBannerResult(const QString &text) {
    if (m_bannerResult == nullptr) {
        return;
    }
    setReport(m_bannerResult, text);
    QWidget *card = m_bannerResult->parentWidget();
    card->show();  // 空结果也留着这张卡
    if (auto *details = card->findChild<QLabel *>(bannerResultName() + QStringLiteral("Detail"))) details->hide();
    // 报告按小标题拆成 步骤 / 材料 / 指令 三块（第一行是结论，已在上面）；认不出小标题时整段放进「步骤」。
    QStringList lead, steps, materials, notes, command;
    QStringList *current = &lead;
    bool first = true;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (first) {
            first = line.isEmpty();
            continue;
        }
        if (line.isEmpty()) continue;
        if (line == QCoreApplication::translate("ToolboxPage", "一步一步：")) current = &steps;
        else if (line == QCoreApplication::translate("ToolboxPage", "材料清单：")) current = &materials;
        else if (line.startsWith(QCoreApplication::translate("ToolboxPage", "指令（"))) current = &command;
        else if (line.startsWith(QCoreApplication::translate("ToolboxPage", "（")) && line.endsWith(QCoreApplication::translate("ToolboxPage", "）"))) notes << line;
        else *current << line;
    }
    const auto fill = [card](const char *name, const QStringList &lines) {
        if (auto *box = card->findChild<QPlainTextEdit *>(QString::fromLatin1(name))) box->setPlainText(lines.join(QLatin1Char('\n')));
    };
    fill("bannerStepsBox", lead + steps + (notes.isEmpty() ? QStringList() : QStringList{QString()} + notes));
    fill("bannerMaterialsBox", materials);
    fill("bannerCommandBox", command);
}

void ToolboxPage::setBannerPreview(const QImage &image, const QString &notice) {
    auto *preview = findChild<QLabel *>(QStringLiteral("bannerVisualPreview"));
    if (preview != nullptr) {
        // 居中摆在透明底上、描一圈细边；没贴图时只留那圈虚线轮廓（不出一块白）。
        preview->setProperty("bannerImage", image);
        preview->setPixmap(framedPreview(image, preview->size(), preview->devicePixelRatioF()));
    }
    auto *label = findChild<QLabel *>(QStringLiteral("bannerVisualNotice"));
    if (label != nullptr) {
        label->setText(notice);
        label->setVisible(!notice.isEmpty());
    }
}
void ToolboxPage::setBannerPatternIcons(const QList<QIcon> &icons) {
    if (m_bannerPattern == nullptr) return;
    m_bannerPattern->setIconSize(QSize(20, 40));
    for (int i = 0; i < m_bannerPattern->count(); ++i) m_bannerPattern->setItemIcon(i, icons.value(i));
}

void ToolboxPage::setBannerCommand(const QString &command) {
    m_bannerCommand = command;
    if (m_bannerCopy != nullptr) {
        m_bannerCopy->setEnabled(!command.isEmpty());
    }
}

int ToolboxPage::bannerBaseColor() const {
    return m_bannerBase != nullptr ? m_bannerBase->currentIndex() : -1;
}

QStringList ToolboxPage::bannerLayers() const { return m_bannerLayers; }

QString ToolboxPage::bannerVersion() const {
    return findChild<QComboBox *>(QStringLiteral("bannerGameVersion"))->currentData().toString();
}
void ToolboxPage::setBannerVersions(const QStringList &versions) {
    auto *combo = findChild<QComboBox *>(QStringLiteral("bannerGameVersion"));
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto &version : versions)
        combo->addItem(version.isEmpty() ? QCoreApplication::translate("ToolboxPage", "不限版本") : version, version);
}
void ToolboxPage::setBannerDesign(int base, const QStringList &layers, const QString &version) {
    auto *combo = findChild<QComboBox *>(QStringLiteral("bannerGameVersion"));
    const QSignalBlocker blockBase(m_bannerBase), blockVersion(combo);
    m_bannerBase->setCurrentIndex(base);
    combo->setCurrentIndex(qMax(0, combo->findData(version)));
    setBannerLayers(layers);
}
void ToolboxPage::setBannerPatternAvailability(const QList<bool> &available) {
    auto *model = qobject_cast<QStandardItemModel *>(m_bannerPattern->model());
    auto *view = qobject_cast<QListView *>(m_bannerPattern->view());
    if (!model || !view) return;
    const QSignalBlocker blocker(m_bannerPattern);
    int first = -1;
    for (int i = 0; i < m_bannerPattern->count(); ++i) {
        const bool enabled = available.value(i, true);
        model->item(i)->setEnabled(enabled);
        view->setRowHidden(i, !enabled);
        if (enabled && first < 0) first = i;
    }
    const int current = m_bannerPattern->currentIndex();
    if (current < 0 || !available.value(current, true)) m_bannerPattern->setCurrentIndex(first);
    m_bannerAdd->setEnabled(first >= 0 && m_bannerLayers.size() < 6);
}

void ToolboxPage::setBannerLayers(const QStringList &layers) {
    m_bannerLayers = layers;
    m_bannerUndoStack.clear();
    syncBannerList();
}

// ---------------------------------------------------------------------------
// 八、纹饰搭配
// ---------------------------------------------------------------------------

QWidget *ToolboxPage::buildTrimPanel() {
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
    // 精修：和其余八页同一个表头（标题 + 一句说明）。
    // 左：输入（版本 + 四件 × 三行下拉框）与结果；右：一块柔和面板装预览和资源操作。
    auto *body = new QHBoxLayout; body->setSpacing(16);
    auto *left = new QVBoxLayout; left->setSpacing(14);
    auto *form = new QGridLayout; form->setHorizontalSpacing(8); form->setVerticalSpacing(10);
    form->setColumnMinimumWidth(0, kFormLabelWidth - 24);
    const QStringList parts{tr("头盔"),tr("胸甲"),tr("护腿"),tr("靴子")};
    const QStringList rows{tr("材质"),tr("纹饰"),tr("材料")};
    const auto muted = [panel](const QString &text) {
        auto *label = makeMutedLabel(panel);
        label->setText(text);
        label->setWordWrap(false);
        return label;
    };
    for (int row=0; row<3; ++row) form->addWidget(muted(rows[row]),row+1,0);
    for (int slot=0; slot<4; ++slot) {
        auto *heading=makePanelCaption(panel,parts[slot]); heading->setAlignment(Qt::AlignCenter);
        form->addWidget(heading,0,slot+1); form->setColumnStretch(slot+1,1);
        const QList<QList<QComboBox *> *> groups{&m_trimSlotArmor,&m_trimSlotPattern,&m_trimSlotMaterial};
        const QStringList keys{"trimArmorSlot","trimPatternSlot","trimMaterialSlot"};
        for (int row=0; row<3; ++row) {
            auto *combo=new QComboBox(panel);
            combo->setObjectName(keys[row]+QString::number(slot));
            combo->setMinimumWidth(72); combo->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            groups[row]->append(combo); form->addWidget(combo,row+1,slot+1);
            connect(combo,&QComboBox::currentIndexChanged,this,[this](int) {
                if (!m_trimSyncing) restoreTrimEquipment(true);
            });
        }
    }
    // 「目标版本」在盔甲格子上方：它管的是下面八格纹饰的可用性（只在结果里提一句，
    // 不悄悄改动已经选好的搭配 —— 与旗帜 / 酿造同一套判据）。
    FormGrid versionForm(panel);
    versionForm.grid->setColumnMinimumWidth(0, kFormLabelWidth - 24);
    auto *trimVersion = new QComboBox(panel);
    trimVersion->setObjectName(trimVersionComboName());
    trimVersion->setMinimumWidth(160);
    versionForm.addRow(tr("目标版本"), {trimVersion});
    left->addLayout(versionForm.grid);
    left->addLayout(form);
    ResultBox result = makeResultBox(panel, tr("要准备"), trimResultName());
    m_trimResult = result.headline;
    setReport(m_trimResult, QString());
    left->addWidget(result.card); left->addStretch(1);
    body->addLayout(left,3);

    auto *previewCard = new SoftPanel(panel);
    previewCard->setMinimumWidth(240);
    auto *right = previewCard->box(); right->setSpacing(8);
    auto *previewHead = new QHBoxLayout;
    previewHead->setContentsMargins(0, 0, 0, 0);
    previewHead->addWidget(makePanelCaption(previewCard, tr("预览")));
    previewHead->addStretch(1);
    // 披风是选修项：默认不画（纹饰看的是盔甲本身），勾上才把账号现有的那件披上去。
    m_trimCape=new QCheckBox(tr("显示披风"),previewCard);
    m_trimCape->setObjectName(trimCapeCheckName());
    m_trimCape->setChecked(false);
    // 还没收到账号外观之前一律当"没有披风"：置灰比勾了却什么都不发生好。
    m_trimCape->setEnabled(false);
    connect(m_trimCape,&QCheckBox::toggled,this,[this](bool visible){ emit trimCapeVisibilityChanged(visible); });
    previewHead->addWidget(m_trimCape);
    right->addLayout(previewHead);
    m_trimPreview=new ArmorTrimPreview(previewCard); right->addWidget(m_trimPreview,1);
    m_trimResourceStatus=makeMutedLabel(previewCard, QStringLiteral("trimResourceStatus"));
    m_trimResourceStatus->setText(tr("未载入盔甲资源"));
    right->addWidget(m_trimResourceStatus);
    // 资源操作：三颗次要的小钮成一组靠右；「取消」只在读取 / 下载中出现。
    auto *actions=new QHBoxLayout; actions->setContentsMargins(0,0,0,0); actions->setSpacing(2);
    actions->addStretch(1);
    const QStringList titles{tr("读取本地"),tr("选择 JAR"),tr("下载资源"),tr("取消")};
    const QStringList tips{tr("从本机已装的游戏里读取盔甲贴图"),tr("手动选一个客户端 JAR"),
                           tr("联网下载这一版的盔甲资源"),tr("取消正在进行的读取 / 下载")};
    for(int i=0;i<titles.size();++i) {
        auto *button=new QPushButton(titles[i],previewCard);
        button->setObjectName(QStringLiteral("trimResourceAction%1").arg(i)); button->setAutoDefault(false);
        button->setToolTip(tips[i]);
        Theme::markButton(button,Theme::ButtonKind::Ghost);
        button->setStyleSheet(QStringLiteral("QPushButton { padding: 0px 8px; min-height: 28px; max-height: 28px; }"));
        connect(button,&QPushButton::clicked,this,[this,i]{emit trimResourcesRequested(i);});
        m_trimResourceButtons.append(button); actions->addWidget(button);
    }
    m_trimResourceButtons.last()->setEnabled(false); m_trimResourceButtons.last()->hide();
    right->addLayout(actions);
    body->addWidget(previewCard,2,Qt::AlignTop); layout->addLayout(body,1);
    // 内容宽 < 640：预览挪到下面（左右并排放不下）。
    new NarrowFlip(panel, body, 640);

    // Legacy controls are migration adapters only, never part of the visible editor.
    m_trimArmor=new QComboBox(panel); m_trimArmor->setObjectName(trimArmorComboName());
    m_trimPart=new QComboBox(panel); m_trimPart->setObjectName(trimPartComboName());
    m_trimPattern=new QComboBox(panel); m_trimPattern->setObjectName(trimPatternComboName());
    m_trimMaterial=new QComboBox(panel); m_trimMaterial->setObjectName(trimMaterialComboName());
    m_trimPieces=new QSpinBox(panel); m_trimPieces->setObjectName(trimPiecesSpinName());
    m_trimPieces->setRange(1,4); m_trimPieces->setValue(1);
    m_trimWholeSet=new QCheckBox(panel); m_trimWholeSet->setObjectName(trimWholeSetName());
    const QList<QWidget *> legacy{m_trimArmor,m_trimPart,m_trimPattern,m_trimMaterial,m_trimPieces,m_trimWholeSet};
    for(auto *widget:legacy) widget->hide();
    const auto oldChanged=[this] {
        restoreTrimEquipment(false);
        emit trimRequested(trimArmorIndex(),trimPartIndex(),trimPatternIndex(),trimMaterialIndex(),trimPieces(),trimWholeSet());
    };
    for(auto *combo:{m_trimArmor,m_trimPart,m_trimPattern,m_trimMaterial})
        connect(combo,&QComboBox::currentIndexChanged,this,[oldChanged](int){oldChanged();});
    connect(m_trimPieces,&QSpinBox::valueChanged,this,[oldChanged](int){oldChanged();});
    connect(m_trimWholeSet,&QCheckBox::toggled,this,[oldChanged](bool){oldChanged();});
    connect(trimVersion,&QComboBox::currentIndexChanged,this,[oldChanged](int){oldChanged();});
    return panel;
}

QList<ToolboxPage::TrimEquipmentSlot> ToolboxPage::trimEquipment() const {
    QList<TrimEquipmentSlot> result;
    for(int i=0;i<m_trimSlotArmor.size();++i)
        result.append({m_trimSlotArmor[i]->currentIndex()<0 ? -1 : m_trimSlotArmor[i]->currentData().toInt(),
                       m_trimSlotPattern[i]->currentIndex()<0 ? -1 : m_trimSlotPattern[i]->currentData().toInt(),
                       m_trimSlotMaterial[i]->currentData().toInt()});
    return result;
}

void ToolboxPage::restoreTrimEquipment(bool perSlot) {
    if(m_trimSyncing || m_trimSlotArmor.size()!=4) return;
    m_trimSyncing=true;
    const bool turtle=m_trimArmor->count()==7 && trimArmorIndex()==6;
    if(turtle) { const QSignalBlocker blocker(m_trimPart); m_trimPart->setCurrentIndex(0); }
    m_trimPart->setEnabled(!turtle&&!trimWholeSet()); m_trimPieces->setEnabled(!trimWholeSet());
    for(int slot=0;slot<4;++slot) {
        if(!perSlot) {
            const bool equipped=turtle ? slot==0 : trimWholeSet() || trimPartIndex()==slot;
            const QSignalBlocker a(m_trimSlotArmor[slot]),b(m_trimSlotPattern[slot]),c(m_trimSlotMaterial[slot]);
            m_trimSlotArmor[slot]->setCurrentIndex(m_trimSlotArmor[slot]->findData(equipped?trimArmorIndex():-1));
            m_trimSlotPattern[slot]->setCurrentIndex(m_trimSlotPattern[slot]->findData(trimPatternIndex()));
            m_trimSlotMaterial[slot]->setCurrentIndex(m_trimSlotMaterial[slot]->findData(trimMaterialIndex()));
        }
        auto *armor=m_trimSlotArmor[slot];
        if(slot>0 && armor->currentData().toInt()==6) {
            const QSignalBlocker blocker(armor); armor->setCurrentIndex(armor->findData(-1));
        }
        const bool equipped=armor->currentIndex()>=0 && armor->currentData().toInt()>=0;
        m_trimSlotPattern[slot]->setEnabled(equipped);
        m_trimSlotMaterial[slot]->setEnabled(equipped && m_trimSlotPattern[slot]->currentData().toInt()>=0);
    }
    m_trimSyncing=false;
    emit trimEquipmentChanged();
}

void ToolboxPage::setTrimPreview(const SkinModel::Scene &scene) {
    if (m_trimPreview) m_trimPreview->setScene(scene);
}

void ToolboxPage::setTrimCapeAvailable(bool available) {
    if (m_trimCape == nullptr) return;
    // 账号换了 / 披风没载进来：先把勾收回来（不带着一个「勾着却画不出来」的状态），
    // 再决定这一格能不能点。收回时挡信号 —— 那不是用户改的，controller 自己会重画。
    if (!available && m_trimCape->isChecked()) {
        const QSignalBlocker blocker(m_trimCape);
        m_trimCape->setChecked(false);
    }
    m_trimCape->setEnabled(available);
    m_trimCape->setToolTip(available ? QString() : tr("当前账号没有可用的披风。"));
}

bool ToolboxPage::trimShowCape() const {
    return m_trimCape != nullptr && m_trimCape->isEnabled() && m_trimCape->isChecked();
}

void ToolboxPage::setTrimResourceStatus(const QString &text, bool busy) {
    if (m_trimResourceStatus) m_trimResourceStatus->setText(text);
    for (int i = 0; i < m_trimResourceButtons.size(); ++i) {
        m_trimResourceButtons[i]->setEnabled(i == 3 ? busy : !busy);
        if (i == 3) m_trimResourceButtons[i]->setVisible(busy);
    }
}

namespace {
void fillTrimSlots(const QList<QComboBox *> &combos, const QStringList &names, const QString &emptyLabel,
                   bool armor = false) {
    for (int slot=0;slot<combos.size();++slot) {
        auto *combo=combos[slot]; const bool had=combo->count()>0;
        const int previous=combo->currentData().toInt(); const QSignalBlocker blocker(combo);
        combo->clear();
        // 一个框一次整批插进去：逐项 addItem 每项各发一轮 rowsInserted、各重算一次尺寸，
        // 十二个框近两百项都压在纹饰页第一次进的那一帧里（灌表那段实测 16 ms 上下）。
        QList<QStandardItem *> items;
        const auto add=[&items](const QString &text,int value) {
            auto *item=new QStandardItem(text); item->setData(value,Qt::UserRole); items.append(item);
        };
        if(!emptyLabel.isEmpty()) add(emptyLabel,-1);
        for(int i=0;i<names.size();++i) add(names[i],i);
        auto *model=qobject_cast<QStandardItemModel *>(combo->model());
        if(model!=nullptr) model->invisibleRootItem()->appendRows(items);
        else for(auto *item:std::as_const(items)) { combo->addItem(item->text(),item->data(Qt::UserRole)); delete item; }
        if(armor && slot>0 && names.size()==7 && model!=nullptr)
            if(auto *item=model->item(combo->findData(6))) item->setEnabled(false);
        combo->setCurrentIndex(combo->findData(had?previous:0));
    }
}
}

void ToolboxPage::setTrimArmorMaterials(const QStringList &names) {
    fillTrimSlots(m_trimSlotArmor,names,tr("未装备"),true);
    if (m_trimArmor == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trimArmor);
    m_trimArmor->clear();
    m_trimArmor->addItems(names);
}

void ToolboxPage::setTrimParts(const QStringList &names) {
    if (m_trimPart == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trimPart);
    m_trimPart->clear();
    m_trimPart->addItems(names);
}

void ToolboxPage::setTrimPatterns(const QStringList &names) {
    fillTrimSlots(m_trimSlotPattern,names,tr("无纹饰"));
    if (m_trimPattern == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trimPattern);
    m_trimPattern->clear();
    m_trimPattern->addItems(names);
}

void ToolboxPage::setTrimMaterials(const QStringList &names) {
    fillTrimSlots(m_trimSlotMaterial,names,QString());
    if (m_trimMaterial == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trimMaterial);
    m_trimMaterial->clear();
    m_trimMaterial->addItems(names);
}

void ToolboxPage::setTrimVersions(const QStringList &versions) {
    auto *combo = findChild<QComboBox *>(trimVersionComboName());
    if (combo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (const auto &version : versions) {
        combo->addItem(version.isEmpty() ? tr("不限版本") : version, version);
    }
}

QString ToolboxPage::trimVersion() const {
    const auto *combo = findChild<QComboBox *>(trimVersionComboName());
    return combo != nullptr ? combo->currentData().toString() : QString();
}

void ToolboxPage::setTrimResult(const QString &text) {
    if (m_trimResult != nullptr) {
        setReport(m_trimResult, text);
    }
}

bool ToolboxPage::trimWholeSet() const {
    return m_trimWholeSet != nullptr && m_trimWholeSet->isChecked();
}

int ToolboxPage::trimArmorIndex() const {
    return m_trimArmor != nullptr ? m_trimArmor->currentIndex() : -1;
}

int ToolboxPage::trimPartIndex() const {
    return m_trimPart != nullptr ? m_trimPart->currentIndex() : -1;
}

int ToolboxPage::trimPatternIndex() const {
    return m_trimPattern != nullptr ? m_trimPattern->currentIndex() : -1;
}

int ToolboxPage::trimMaterialIndex() const {
    return m_trimMaterial != nullptr ? m_trimMaterial->currentIndex() : -1;
}

int ToolboxPage::trimPieces() const {
    return m_trimPieces != nullptr ? m_trimPieces->value() : 1;
}
