/**
 * @file InlineMessage.cpp
 * @brief 实现内嵌提示/确认面板。
 * @note 替代 QMessageBox；行为约定见 InlineMessage.hpp。
 */

#include "InlineMessage.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QPainter>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

namespace {
/// `setList()` 那张列表最多露几行（再多就在列表内部滚）。见 setList() 里那段 ★。
constexpr int kListVisibleRows = 6;

// ---------- 取色卡片（`InlineMessage::pickColor()`）----------

/// 色板：4 行 × 8 块。**第一行是中性那一档**（黑 → 白，最后一块是出厂主题色 —— 用户想回到
/// 默认不用去记那个色号）；第二行是几个色相的"标准"那一档，第三行是它们的深一档，第四行是
/// 浅一档。
///
/// 为什么自己钉一张表而不是拿 `QColor::fromHsv()` 现算一整圈：色板要"每一块都好看"，
/// 而现算出来的那一圈里有几块（黄绿的中间档、饱和的青色）在浅色界面上是糊的。
const char *const kPalette[] = {
    "#000000", "#262626", "#4d4d4d", "#808080", "#b8b8b8", "#e6e6e6", "#ffffff", "#3d8068",
    "#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#8e24aa", "#d81b60",
    "#b71c1c", "#e65100", "#f9a825", "#2e7d32", "#00695c", "#1565c0", "#4527a0", "#ad1457",
    "#ef9a9a", "#ffcc80", "#fff59d", "#a5d6a7", "#80deea", "#90caf9", "#ce93d8", "#f48fb1",
};
constexpr int kPaletteColumns = 8;
/// 色板里每块多大、离多远。**8 列正好铺得下** `OverlayHost::promptContentWidth()`：
/// 8 * 48 + 7 * 6 = 426（内容宽 442）—— 再宽一格就要被卡片裁掉了。
constexpr int kSwatchWidth = 48;
constexpr int kSwatchHeight = 24;
constexpr int kSwatchSpacing = 6;
/// 那个"你现在选的是哪个色"的预览块。和设置页那一行的色块一样大（64×24），
/// 两处看着是同一件东西。
constexpr int kPreviewWidth = 64;
constexpr int kPreviewHeight = 24;
/// 色号那一格多宽。装得下 `#aarrggbb`（九个字符）还有富余，又不至于宽得像要给一句话用。
constexpr int kColorEditWidth = 120;

/// 认色号。收三种写法：`#rrggbb` / `#aarrggbb`、**不带 `#` 的六位或八位十六进制**
/// （用户从别处粘过来的色号常常没有）、以及 `QColor` 认得的颜色名（red / steelblue）。
/// @return 认不出给一个无效色（调用方按"还没打完"处理）。
QColor parseColorText(const QString &text) {
    QString value = text.trimmed();
    if (value.isEmpty()) {
        return QColor();
    }
    if (!value.startsWith(QLatin1Char('#')) && (value.size() == 6 || value.size() == 8)) {
        bool allHex = true;
        for (const QChar ch : value) {
            const QChar lower = ch.toLower();
            const bool hex = (lower >= QLatin1Char('0') && lower <= QLatin1Char('9')) ||
                             (lower >= QLatin1Char('a') && lower <= QLatin1Char('f'));
            if (!hex) {
                allHex = false;
                break;
            }
        }
        if (allHex) {
            value.prepend(QLatin1Char('#'));
        }
    }
    const QColor color = QColor::fromString(value);
    return color.isValid() ? color : QColor();
}

/// 把一块预览摆成 @p color（无效色 = 一块极淡的空底 —— "这里还不是一个色"）。
void paintPreview(QLabel *preview, const QColor &color) {
    // 有色：色块边保留（`swatchBorder()`，浅色块压在浅底上要它才分得开）。
    // 无色：不画空心框（「去硬描边」），只垫一层 fill-press 淡底，1px 透明边保住几何。
    const QColor fill = color.isValid() ? color : Theme::tokens().fillPress;
    const QColor edge = color.isValid() ? Theme::swatchBorder(color) : QColor(Qt::transparent);
    preview->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid %2;"
                                          " border-radius: %3px;")
                               .arg(fill.name(QColor::HexArgb), edge.name(QColor::HexArgb))
                               .arg(Theme::controlRadius()));
}
}  // namespace

InlineMessageBox::InlineMessageBox(MessageKind kind, const QString &title, const QString &text,
                                   const QStringList &buttons, int defaultIndex,
                                   int destructiveIndex, QWidget *parent)
    : QDialog(parent), m_kind(kind) {
    setObjectName(QStringLiteral("inlineMessageBox"));
    // 它自己**从来不是窗口**（覆盖层摆上去之前会先把 Qt::Dialog 摘掉），但键盘事件得能到
    // 它身上 —— Esc = 拒绝、回车打默认按钮，这两条都是 QDialog 白给的，前提是焦点在它这儿。
    // 对"不是窗口"的 QDialog 来说焦点策略默认是 NoFocus，所以这一句不能省。
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // 标题正文色；是哪一类由左边那块语义色图标说（出错 = 浅红底的叉），红不再压在字上。
    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(titleLabelName());
    m_header = makeHeader(this, m_titleLabel, kind);
    layout->addLayout(m_header);

    m_textLabel = new QLabel(text, this);
    m_textLabel->setObjectName(textLabelName());
    m_textLabel->setWordWrap(true);
    // 正文 14px、次要字色（设计稿 .dlg p：14/22 ink-2）。字号要在下面量高度之前定。
    QFont bodyFont = m_textLabel->font();
    bodyFont.setPixelSize(14);
    m_textLabel->setFont(bodyFont);
    QPalette bodyPalette = m_textLabel->palette();
    bodyPalette.setColor(QPalette::WindowText, bodyPalette.color(QPalette::PlaceholderText));
    m_textLabel->setPalette(bodyPalette);
    // 正文允许选中复制：正文里常带路径 / 版本号 / 报错原文，手抄一条路径是不现实的。
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 宽度**钉死**。它同时管两件事：所有提示一样宽，以及"这段字换几行"
    // 是个确定的数 —— 卡片的高矮才算得准（指望 heightForWidth 一路穿过两层布局传上来，
    // 是会失手的那种）。
    // 正文超过卡片高度时**滚动**，别把按钮挤出卡片：长错误（复制整棵目录失败、
    // 多个文件占用）会很长，wordWrap 的 QLabel 没有上限，卡片会被顶出屏幕。
    const int textWidth = OverlayHost::promptContentWidth();
    m_textLabel->setFixedWidth(textWidth - 14);  // 给竖直滚动条留一格
    // QLabel::heightForWidth() 在没上屏前不稳定；这里直接用字体度量算出换行后的高度。
    const QFontMetrics metrics(m_textLabel->font());
    const int neededHeight = metrics
        .boundingRect(QRect(0, 0, textWidth - 14, 100000), Qt::TextWordWrap, m_textLabel->text())
        .height();
    // QScrollArea 不认 heightForWidth：把量出来的高度钉成最小高度，滚动条才有范围。
    m_textLabel->setMinimumHeight(neededHeight + 4);
    auto *textScroll = new QScrollArea(this);
    textScroll->setObjectName(textScrollName());
    textScroll->setFrameShape(QFrame::NoFrame);
    textScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    textScroll->setWidgetResizable(true);
    textScroll->setWidget(m_textLabel);
    textScroll->setFixedWidth(textWidth);
    // 和上面最小高度同一个数：差那 2px 就会白白冒出一根滚动条。
    textScroll->setFixedHeight(qBound(24, neededHeight + 4, 260));
    layout->addWidget(textScroll);

    QWidget *row = new QWidget(this);
    row->setObjectName(buttonRowName());
    row->setProperty("wtfmlDialogActions", true);  // 一排一样高（Theme 里那条规则）
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(10);
    rowLayout->addStretch(1);
    // 一颗都没给 = "就说一句"，补一颗「确定」。**默认按钮也是它**（回车 = 知道了我走了）。
    const QStringList labels = buttons.isEmpty() ? QStringList{tr("确定")} : buttons;
    QPushButton *defaultButton = nullptr;
    QList<QPushButton *> actions;
    for (int i = 0; i < labels.size(); ++i) {
        auto *button = new QPushButton(labels.at(i), row);
        actions.append(button);
        button->setObjectName(buttonName(i));
        // 三档颜色按**位置**分，不按文字（"取消"这三个字认不得 —— 按钮表是调用方给的，
        // 上面那张世界编辑菜单里就有四颗都叫"做什么"的）：
        // - `destructiveIndex` 那一颗 = 危险红（"不可撤销"那一档）；
        // - 默认那一颗 = 主动作（实心主题色，设计稿 .dlg .acts 最右那颗）；
        // - 其余 = 一般性。
        //
        // 这条规矩的前提是"调用方标对了 `destructiveIndex`"。真正的闸是 `defaultIndex`
        // （危险那两颗一律不给默认：误按回车不该把事办了），颜色只是第二道。
        if (i == destructiveIndex) {
            Theme::markButton(button, Theme::ButtonKind::Danger);
        } else if (i == defaultIndex) {
            Theme::markButton(button, Theme::ButtonKind::Primary);
        } else {
            Theme::markButton(button, Theme::ButtonKind::Neutral);
        }
        connect(button, &QPushButton::clicked, this, [this, i] {
            m_clicked = i;
            accept();
        });
        if (i == defaultIndex) {
            button->setDefault(true);
            defaultButton = button;  // 默认那颗摆到最右（设计稿：主动作在最右，带焦点环）
        } else {
            rowLayout->addWidget(button);
        }
    }
    if (defaultButton != nullptr) {
        rowLayout->addWidget(defaultButton);
    }
    if (destructiveIndex >= 0 && destructiveIndex < actions.size() && actions.size() > 1) {
        int actionWidth = 0;
        for (QPushButton *button : actions) {
            actionWidth = qMax(actionWidth, button->sizeHint().width());
        }
        for (QPushButton *button : actions) {
            button->setFixedWidth(actionWidth);
        }
    }
    layout->addWidget(row);
    relinkTabOrder();
}

namespace {
/// 对话框头左边那块 32px 图标块：底色与默认图标按 @p kind，@p icon 非空时只换图标。
QPixmap headerChip(MessageKind kind, const QString &icon) {
    const Theme::Tokens t = Theme::tokens();
    QColor fill = t.accentSoft2;
    QColor ink = Theme::textTone(t.accentInk, t.primary);
    QString name = icon.isEmpty() ? QStringLiteral("info") : icon;
    if (kind == MessageKind::Warning) {
        fill = t.warnSoft;
        ink = Theme::textTone(t.warnInk, t.warn);
        if (icon.isEmpty()) name = QStringLiteral("warn");
    } else if (kind == MessageKind::Critical) {
        fill = t.dangerSoft;
        ink = Theme::textTone(t.dangerInk, t.danger);
        if (icon.isEmpty()) name = QStringLiteral("xc");
    }
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap chip(QSize(32, 32) * dpr);
    chip.setDevicePixelRatio(dpr);
    chip.fill(Qt::transparent);
    QPainter painter(&chip);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, 32, 32), 10, 10);
    VersionIcons::paintLine(painter, name, QRectF(7, 7, 18, 18), ink, 1.8);
    return chip;
}
}  // namespace

void InlineMessageBox::setHeaderIcon(const QString &icon) {
    if (auto *chip = findChild<QLabel *>(QStringLiteral("inlineMessageIcon"), Qt::FindDirectChildrenOnly))
        chip->setPixmap(headerChip(m_kind, icon));
}

QHBoxLayout *InlineMessageBox::makeHeader(QWidget *owner, QLabel *title, MessageKind kind,
                                          const QString &icon) {
    const QPixmap chip = headerChip(kind, icon);
    auto *chipLabel = new QLabel(owner);
    chipLabel->setObjectName(QStringLiteral("inlineMessageIcon"));
    chipLabel->setFixedSize(32, 32);
    chipLabel->setPixmap(chip);

    title->setFont(Theme::textFont(title->font(), Theme::TextRole::Heading));
    title->setWordWrap(true);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 2);
    header->setSpacing(12);
    header->addWidget(chipLabel, 0, Qt::AlignTop);
    header->addWidget(title, 1, Qt::AlignVCenter);
    return header;
}

void InlineMessageBox::relinkTabOrder() {
    // 理由见头文件里那一段（焦点链默认跟创建顺序，而后面那几行都是后插的）。
    //
    // 顺序按**布局**走，不按几何走：说"先上后下、先左后右"到底就是布局里那一串的
    // 先后（竖排 = 从上到下，里面那张横排 = 从左到右）。几何在这里量不得 —— 这几处
    // 调用都发生在 `show()` **之前**，那会儿布局还没跑过，每个控件都停在默认位置
    // （`(0,0,100,30)`），量出来的顺序恒等于创建顺序 —— 那不是要的布局顺序。
    QVector<QWidget *> focusables;
    const auto collect = [&focusables](auto &&self, QLayout *layout) -> void {
        if (layout == nullptr) {
            return;
        }
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem *item = layout->itemAt(i);
            QWidget *widget = item->widget();
            if (widget != nullptr) {
                if ((widget->focusPolicy() & Qt::TabFocus) != 0) {
                    focusables.append(widget);
                }
                // 这一个自己还带布局（按钮行那张横排）⇒ 把它里面那几格按顺序接上。
                self(self, widget->layout());
            } else if (QLayout *inner = item->layout()) {
                self(self, inner);
            }
        }
    };
    collect(collect, layout());
    for (int i = 1; i < focusables.size(); ++i) {
        QWidget::setTabOrder(focusables.at(i - 1), focusables.at(i));
    }
}

QLineEdit *InlineMessageBox::setLineEdit(const QString &initial) {
    if (m_lineEdit != nullptr) {
        m_lineEdit->setText(initial);  // 已经插过一行了：只换内容，不再插第二行。
        return m_lineEdit;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    Q_ASSERT(layout != nullptr);  // ctor 里那个布局是本类的实现细节，不是可选的。
    m_lineEdit = new QLineEdit(initial, this);
    m_lineEdit->setObjectName(lineEditName());
    // 和正文一样**钉住宽度**：卡片的高矮才算得准（理由见 ctor 里那段）。
    m_lineEdit->setFixedWidth(OverlayHost::promptContentWidth());
    // 正文里常带路径，这一格也一样让人能选中复制（和正文那个标签同一个取法）。
    m_lineEdit->setContextMenuPolicy(Qt::DefaultContextMenu);
    // 插在**按钮行前面** —— ctor 里那个布局的顺序是「标题 / 正文 / 按钮行」，
    // 所以"最后一个"就是按钮行。（写成 `count() - 1` 而不是写死 2：将来正文上面再加
    // 一行什么，这一句不用跟着改。）
    layout->insertWidget(layout->count() - 1, m_lineEdit);
    // 插在按钮行**前面**，可它建得比按钮行晚 ⇒ 不重排的话 Tab 会往回跳（见 relinkTabOrder）。
    relinkTabOrder();
    // NOTE: 这里**不**叫 setFocus() —— 叫了也不管用（`setParent()` 之后会被清掉，
    // 见 showEvent() 那段）。真正定光标的是 `showEvent()` 里那一下。
    return m_lineEdit;
}

QString InlineMessageBox::lineEditText() const {
    return m_lineEdit != nullptr ? m_lineEdit->text() : QString();
}

QListWidget *InlineMessageBox::setList(const QStringList &items, int initialRow) {
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    Q_ASSERT(layout != nullptr);  // 同 setLineEdit()：布局是本类的实现细节。
    if (m_list == nullptr) {
        m_list = new QListWidget(this);
        m_list->setObjectName(listName());
        // 和正文/输入框一样**钉住宽度**（理由见 ctor 里那段）。
        m_list->setFixedWidth(OverlayHost::promptContentWidth());
        // 不要横向滚动条：这一张列表里放的是实例名/文件夹名，长了让它自己截（和列表页
        // 那些行的做法一致），横着滚会让卡片多出一条从来没用的东西。
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // 插在**按钮行前面**（同 setLineEdit 那一句，写成 count()-1 不写死下标）。
        layout->insertWidget(layout->count() - 1, m_list);
        relinkTabOrder();  // 同 setLineEdit：它建得比按钮行晚，Tab 得跟着重排。
    }
    m_list->clear();
    m_list->addItems(items);
    if (initialRow >= 0 && initialRow < m_list->count()) {
        m_list->setCurrentRow(initialRow);
    }
    // 高度**不跟条数长**：最多露 kListVisibleRows 行，再多就在列表内部滚。
    // NOTE: 行高问 `sizeHintForRow(0)` —— 它才是"这个列表画一行要多少"的出处（字体、
    // 内边距、将来可能有的行装饰都在里面）；拿 fontMetrics().height() 自己算，
    // 第一行就会被切掉一点。取不到（一行都没有）时退回字体那一档。
    const int rowHeight = m_list->count() > 0 ? m_list->sizeHintForRow(0)
                                             : fontMetrics().height() + 8;
    const int rows = qBound(1, items.size(), kListVisibleRows);
    m_list->setFixedHeight(rows * rowHeight + 2 * m_list->frameWidth() + 2);
    return m_list;
}

int InlineMessageBox::listRow() const {
    return m_list != nullptr ? m_list->currentRow() : -1;
}

int InlineMessageBox::listVisibleRows() {
    return kListVisibleRows;
}

void InlineMessageBox::setNote(const QString &text) {
    if (text.isEmpty()) {
        // 空串 = **不补**。已经补过一行的话把它撤掉：否则"这次不带附注"会留着一个空标签，
        // 卡片底下多出一段白白的高度。
        if (m_noteLabel != nullptr) {
            delete m_noteLabel;
            m_noteLabel = nullptr;
        }
        return;
    }
    if (m_noteLabel == nullptr) {
        auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
        Q_ASSERT(layout != nullptr);  // 同 setLineEdit()：布局是本类的实现细节。
        m_noteLabel = new QLabel(this);
        m_noteLabel->setObjectName(noteLabelName());
        m_noteLabel->setWordWrap(true);
        // 和正文一样钉住宽度：卡片的高矮才算得准（理由见 ctor 里那段）。
        m_noteLabel->setFixedWidth(OverlayHost::promptContentWidth());
        m_noteLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        // 小一号 + 浅一档。字号在**这份调色板自己的字体**上算（不是写死点数）：
        // 系统字号调大调小、或者以后换主题，这一行都跟着正文一起动，不会变成"正文 11pt
        // 而附注还是 10pt"那种歪掉的比例。
        QFont noteFont = Theme::textFont(m_noteLabel->font(), Theme::TextRole::Caption);
        m_noteLabel->setFont(noteFont);
        // 浅一档的颜色走 QPalette::PlaceholderText —— 全仓"比正文浅一档的次要操作"就是它
        // （版本树那个取消小叉、导航按钮那几颗没选中的，用的都是同一个）。
        // NOTE: 这里要**先 setFont 再取调色板**，理由和别处那个坑一样：调色板是从字体上算的。
        QPalette notePalette = m_noteLabel->palette();
        notePalette.setColor(QPalette::WindowText, notePalette.color(QPalette::PlaceholderText));
        m_noteLabel->setPalette(notePalette);

        // 插在**按钮行前面**：布局顺序是「标题 / 正文 / 按钮行」，所以"最后一个"就是按钮行。
        // 写成 `count() - 1` 而不是写死下标，和 setLineEdit() 同一个理由（将来上面再加一行，
        // 这两处都不用跟着改）。
        layout->insertWidget(layout->count() - 1, m_noteLabel);
        relinkTabOrder();  // 小字本身 Tab 不到，可它会把下面那排控件的落点顶下去。
    }
    m_noteLabel->setText(text);
}

QString InlineMessageBox::noteText() const {
    return m_noteLabel != nullptr ? m_noteLabel->text() : QString();
}

QPushButton *InlineMessageBox::buttonAt(int index) const {
    if (index < 0) {
        return nullptr;
    }
    if (auto *row = findChild<QWidget *>(buttonRowName())) {
        return row->findChild<QPushButton *>(buttonName(index));
    }
    return nullptr;
}

QCheckBox *InlineMessageBox::setCheckBox(const QString &text, int acceptButtonIndex) {
    if (m_checkBox == nullptr) {
        auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
        Q_ASSERT(layout != nullptr);  // 同 setLineEdit()：布局是本类的实现细节。
        m_checkBox = new QCheckBox(text, this);
        m_checkBox->setObjectName(checkBoxName());
        // **一开始不勾**（我们不替任何人预勾）。这一句是多余的，但写出来是为了让"谁把它
        // 改成 setChecked(true) 会让整个承诺作废"这件事在代码里看得见。
        m_checkBox->setChecked(false);
        // 插在**按钮行前面**（写成 count()-1 不写死下标，同 setLineEdit 那一句）。
        layout->insertWidget(layout->count() - 1, m_checkBox);
        relinkTabOrder();  // 同 setLineEdit：那个勾建得比按钮行晚，Tab 得跟着重排。

        if (QPushButton *accept = buttonAt(acceptButtonIndex)) {
            // 这是这一格真正的用途（见头文件）：没勾之前那一颗是灰的。
            // 绑法走 `toggled`，而且**当场按现在的状态摆一次** —— 只接信号不摆初值的话，
            // 那个按钮会以"能按"的样子出现，直到用户手一抖勾一下再取消才变灰。
            accept->setEnabled(m_checkBox->isChecked());
            connect(m_checkBox, &QCheckBox::toggled, accept, &QPushButton::setEnabled);
        }
    } else {
        m_checkBox->setText(text);
    }
    return m_checkBox;
}

bool InlineMessageBox::checkBoxChecked() const {
    return m_checkBox != nullptr && m_checkBox->isChecked();
}

QLabel *InlineMessageBox::setLink(const QString &text, const QString &url) {
    if (m_linkLabel == nullptr) {
        auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
        Q_ASSERT(layout != nullptr);  // 同 setLineEdit()：布局是本类的实现细节。
        m_linkLabel = new QLabel(this);
        m_linkLabel->setObjectName(linkName());
        m_linkLabel->setWordWrap(true);
        // 和正文一样钉住宽度：卡片的高矮才算得准（理由见 ctor 里那段）。
        m_linkLabel->setFixedWidth(OverlayHost::promptContentWidth());
        // `false` —— 见头文件那段：点了只发链接激活那条信号，开浏览器是调用方的事
        // （那一层要写日志、开不了还要弹一句）。改成 true 会绕过这两个线索。
        m_linkLabel->setOpenExternalLinks(false);
        // 光靠鼠标悬停变手型还不够：这一行得能被键盘 Tab 到、能用回车激活（Tab 到之后按
        // 回车，QLabel 自己会把 linkActivated 发出来）。所以是 TextBrowserInteraction，
        // 不是只给鼠标用的 TextSelectableByMouse。
        m_linkLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_linkLabel->setFocusPolicy(Qt::StrongFocus);
        m_linkLabel->setTextFormat(Qt::RichText);
        connect(m_linkLabel, &QLabel::linkActivated, this,
                [this](const QString &url) { emit linkActivated(url); });
        // 插在**按钮行前面**（写成 count()-1 不写死下标，同 setLineEdit 那一句）。
        layout->insertWidget(layout->count() - 1, m_linkLabel);
        relinkTabOrder();  // 这一行是 StrongFocus、Tab 够得着，也得跟着重排。
    }
    m_linkUrl = url;
    // 用 `toHtmlEscaped()` 把显示的那几个字转义掉 —— 它是**内容**，不是标记。不转的话
    // 带 `&` 或 `<` 的一句话会被 Qt 当成富文本吃掉一段（比如"允许与<服务器>"那种）。
    m_linkLabel->setText(
        QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), text.toHtmlEscaped()));
    return m_linkLabel;
}

QString InlineMessageBox::linkUrl() const {
    return m_linkLabel != nullptr ? m_linkUrl : QString();
}

QWidget *InlineMessageBox::setContent(QWidget *content, int preferredWidth) {
    if (content == nullptr) {
        return nullptr;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    Q_ASSERT(layout != nullptr);  // 同 setLineEdit()：布局是本类的实现细节。
    if (m_content != nullptr) {
        // 已经插过一块了：换内容（把旧那块摘下来还给调用方，同 OverlayHost 那条"还回去
        // 而不是删掉"的规矩 —— 调用方是它的主人）。
        layout->removeWidget(m_content);
        m_content->setParent(nullptr);
    }
    m_content = content;
    m_content->setParent(this);
    // 和另外那几条一样**钉住宽度**（见头文件里那段）：卡片的高矮才算得准，而且塞宽了是
    // 被卡片裁掉，不会把卡片撑开。
    if (preferredWidth > OverlayHost::promptWidth()) {
        setProperty("wtfmlPromptWidth", qBound(OverlayHost::promptWidth(), preferredWidth, 1000));
        m_content->setMinimumWidth(0);
        m_content->setMaximumWidth(QWIDGETSIZE_MAX);
        m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (m_textLabel->text().isEmpty()) m_textLabel->hide();
        if (!findChild<QPushButton *>(QStringLiteral("inlineMessageClose"))) {
        auto *close = new QPushButton(QStringLiteral("×"), this);
        close->setObjectName(QStringLiteral("inlineMessageClose"));
        close->setAccessibleName(tr("关闭")); close->setFixedSize(32, 32);
        Theme::markButton(close, Theme::ButtonKind::Ghost);
        m_header->addWidget(close);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        }
    } else m_content->setFixedWidth(OverlayHost::promptContentWidth());
    // 插在**按钮行前面**（写成 count()-1 不写死下标，同 setLineEdit 那一句）。
    layout->insertWidget(layout->count() - 1, m_content);
    // 那块里挂了 `wtfmlPromptActionsLead` 的一格（如预设弹窗的「本次应用」汇总）挪到按钮行左边。
    // shortcut: 换内容时不把它还回旧那块；今天挂它的只有 PresetInstallPicker，且只 setContent 一次。
    if (auto *row = findChild<QWidget *>(buttonRowName()); row != nullptr) {
        for (QWidget *lead : m_content->findChildren<QWidget *>()) {
            if (!lead->property("wtfmlPromptActionsLead").toBool()) continue;
            static_cast<QHBoxLayout *>(row->layout())->insertWidget(0, lead, 1);
            break;
        }
    }
    relinkTabOrder();  // 调用方那块里可能有好几格能 Tab 到，一起按看得见的顺序排。
    return m_content;
}

void InlineMessageBox::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (m_lineEdit == nullptr) {
        // `setContent()` 那一档：那块东西里**第一个输入框**接着这一下（同一条规矩、
        // 同一个理由 —— 用户点开"挑一个颜色"，直接打字就该进那一格）。
        if (m_content != nullptr) {
            if (auto *edit = m_content->findChild<QLineEdit *>()) {
                edit->setFocus(Qt::OtherFocusReason);
                edit->selectAll();
            }
        }
        return;
    }
    // 理由见头文件里那一段（setParent 会清焦点、QDialog 会把焦点交给默认按钮）。
    m_lineEdit->setFocus(Qt::OtherFocusReason);
    // 预填的内容整段选中：用户想整段换掉时直接打字就行，不用先全选一遍。改一两个字的人
    // 点一下/按方向键也能取消选中。
    m_lineEdit->selectAll();
}

namespace InlineMessage {

int ask(OverlayHost *host, MessageKind kind, const QString &title, const QString &text,
        const QStringList &buttons, int defaultIndex, int destructiveIndex,
        bool dismissOnOutsideClick, const QString &note) {
    if (host == nullptr) {
        // 没有覆盖层可摆（构造中途、或者验证工程单独测某个控件）。**不许静默吞掉** ——
        // "该说的话没说"是最难查的那一类故障。写一行日志，调用方照旧拿到"什么都没选"。
        qWarning("InlineMessage: 没有 OverlayHost，这条提示没有显示（%s）", qUtf8Printable(title));  // i18n-ignore
        return -1;
    }
    if (host->promptOpen()) {
        // 纯报信的那一档（一颗按钮都没给 —— `warning()` / `information()` /
        // `critical()` 全走这儿）**排着，不当场丢**。后台任务收尾最常撞上这一格：用户
        // 正开着一个「要不要…」的提示，下载失败那句话不该只留一行 qWarning 就丢掉。
        // 它不向调用方要答案，晚一步弹出来不改变任何语义。
        //
        // 要人回答的提示**仍然不叠**（见 OverlayHost::openPrompt 里那段）：排队会让
        // 调用方从"阻塞到用户答"变成"立刻拿到一个空答案"，那是另一个问题。
        if (buttons.isEmpty()) {
            const QPointer<OverlayHost> guard(host);
            if (host->queueNotification([guard, kind, title, text, buttons, defaultIndex,
                                         destructiveIndex, dismissOnOutsideClick, note] {
                    if (guard) {
                        ask(guard, kind, title, text, buttons, defaultIndex, destructiveIndex,
                            dismissOnOutsideClick, note);
                    }
                })) {
                return -1;
            }
        }
        // 不许静默吞掉：调用方拿到的是"什么都没选"，而那和"用户按了 Esc"分不开 ——
        // 所以至少要在日志里留下痕迹，不然"那条下载失败的消息去哪了"没法查。
        qWarning("InlineMessage: 已经有一条提示开着，这一条没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return -1;
    }
    InlineMessageBox box(kind, title, text, buttons, defaultIndex, destructiveIndex);
    // 那行小字**在 execPrompt 之前**补上：卡片摆上去之后几何就定了，之后再插一行会让
    // 它开在原来的高度上、把按钮行挤出边框。
    box.setNote(note);
    host->execPrompt(&box, dismissOnOutsideClick);
    return box.clickedIndex();
}

void information(OverlayHost *host, const QString &title, const QString &text) {
    // 纯报信：点框外面 / 按 Esc 都算"知道了"。
    ask(host, MessageKind::Information, title, text, QStringList(), 0, -1, true);
}

void warning(OverlayHost *host, const QString &title, const QString &text) {
    ask(host, MessageKind::Warning, title, text, QStringList(), 0, -1, true);
}

void critical(OverlayHost *host, const QString &title, const QString &text) {
    ask(host, MessageKind::Critical, title, text, QStringList(), 0, -1, true);
}

bool question(OverlayHost *host, const QString &title, const QString &text, const QString &yesText,
              const QString &noText, bool defaultYes, bool destructive) {
    const QStringList buttons{yesText, noText};
    const int index = ask(host, MessageKind::Question, title, text, buttons, defaultYes ? 0 : 1,
                          destructive ? 0 : -1, false);
    return index == 0;
}

QString getText(OverlayHost *host, const QString &title, const QString &label,
                const QString &initial, bool *ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (host == nullptr) {
        // 和 ask() 一样**不许静默吞掉**：调用方会拿到空串 + ok=false（= "用户取消了"），
        // 于是"那个名字改没改"这种事在界面上就是"什么也没发生" —— 只有日志能说清为什么。
        qWarning("InlineMessage: 没有 OverlayHost，这个「问一句名字」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return QString();
    }
    if (host->promptOpen()) {
        // 提示叠提示不做（见 OverlayHost::openPrompt 里那段）。理由同 ask()。
        qWarning("InlineMessage: 已经有一条提示开着，这个「问一句名字」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return QString();
    }
    // 按钮表就是 `{确定, 取消}`、默认是**确定**（回车 = 认下这个名字，和 QInputDialog
    // 一样：那一档的默认按钮也是 Ok）。`破坏性`那一格不给 —— 没有危险动作。
    // NOTE: 这里不能用光秃秃的 `tr(...)` —— 这是一个自由函数，没有 QObject 那个 tr。
    // 走 QObject::tr 拿到的是同一个翻译上下文（"QObject"），和 InlineMessageBox
    // 自己那颗默认的「确定」不是同一个上下文；本仓没有 .ts 文件，所以两处现在都是
    // 原字面量，但**要改名就两处一起改**（这一句就是给那种时候留的路标）。
    InlineMessageBox box(MessageKind::Question, title, label,
                         QStringList{QObject::tr("确定"), QObject::tr("取消")}, 0, -1);
    box.setLineEdit(initial);
    host->execPrompt(&box, false);
    const bool accepted = box.clickedIndex() == 0;
    if (ok != nullptr) {
        *ok = accepted;
    }
    return box.lineEditText();
}

bool pickFromList(OverlayHost *host, const QString &title, const QString &text,
                  const QStringList &items, int initialRow, int *rowOut) {
    if (rowOut != nullptr) {
        *rowOut = -1;
    }
    if (items.isEmpty()) {
        // 见 .hpp：一行都没有的列表框长得和"有东西可挑"一模一样，**不弹**。
        // 这一路也写一行日志 —— 调用方本该自己先判空，走到这儿说明那一处漏了。
        qWarning("InlineMessage: 清单是空的，这个「挑一个」没有显示（%s）", qUtf8Printable(title));  // i18n-ignore
        return false;
    }
    if (host == nullptr) {
        qWarning("InlineMessage: 没有 OverlayHost，这个「挑一个」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return false;
    }
    if (host->promptOpen()) {
        qWarning("InlineMessage: 已经有一条提示开着，这个「挑一个」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return false;
    }
    // 按钮表和 getText() 逐字一样（`{确定, 取消}`、默认确定）—— 那一档也是
    // "认下这个选择"。破坏性那一格不给：挑一个目标不是危险动作。
    InlineMessageBox box(MessageKind::Question, title, text,
                         QStringList{QObject::tr("确定"), QObject::tr("取消")}, 0, -1);
    box.setList(items, initialRow);
    host->execPrompt(&box, false);
    if (box.clickedIndex() != 0) {
        return false;
    }
    const int row = box.listRow();
    if (row < 0) {
        // 点了确定但一行都没选中（用户可以 Ctrl+点一下把选中项取消掉）。这**不算选中了
        // 第 0 行** —— 替用户挑一个，比让他再选一次糟得多。
        return false;
    }
    if (rowOut != nullptr) {
        *rowOut = row;
    }
    return true;
}

QString colorEditName() {
    return QStringLiteral("inlineColorEdit");
}

QString colorSwatchName(int index) {
    return QStringLiteral("inlineColorSwatch%1").arg(index);
}

QString colorPreviewName() {
    return QStringLiteral("inlineColorPreview");
}

int colorSwatchCount() {
    return static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
}

QColor pickColor(OverlayHost *host, const QString &title, const QString &text,
                 const QColor &initial) {
    if (host == nullptr) {
        // 和上面那几条一样**不许静默吞掉**：调用方拿到一个无效色（= "用户取消了"），
        // 于是"我点了一下颜色没变"在日志里才有线索。
        qWarning("InlineMessage: 没有 OverlayHost，这个「挑一个颜色」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return QColor();
    }
    if (host->promptOpen()) {
        qWarning("InlineMessage: 已经有一条提示开着，这个「挑一个颜色」没有显示（%s）",  // i18n-ignore
                 qUtf8Printable(title));
        return QColor();
    }
    // 按钮表和 getText() / pickFromList() 逐字一样（`{确定, 取消}`、默认确定）。
    InlineMessageBox box(MessageKind::Question, title, text,
                         QStringList{QObject::tr("确定"), QObject::tr("取消")}, 0, -1);

    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- 第一行：预览块 + 色号输入框 ----
    auto *line = new QWidget(content);
    auto *lineLayout = new QHBoxLayout(line);
    lineLayout->setContentsMargins(0, 0, 0, 0);
    lineLayout->setSpacing(8);

    auto *preview = new QLabel(line);
    preview->setObjectName(colorPreviewName());
    preview->setFixedSize(kPreviewWidth, kPreviewHeight);

    auto *edit = new QLineEdit(line);
    edit->setObjectName(colorEditName());
    edit->setText(initial.isValid() ? initial.name(QColor::HexRgb) : QString());
    edit->setPlaceholderText(Theme::defaultAccentColor().name(QColor::HexRgb));
    edit->setMinimumWidth(kColorEditWidth);
    // 色号是**六到八个字符**，长度不设限反而让人以为能写别的（写长了认不出，确定会灰掉
    // ——那是对的，但不如一开始就写不进去）。
    edit->setMaxLength(9);

    lineLayout->addWidget(preview);
    lineLayout->addWidget(edit);
    lineLayout->addStretch(1);
    layout->addWidget(line);

    // ---- 第二行：色板 ----
    auto *grid = new QWidget(content);
    auto *gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setHorizontalSpacing(kSwatchSpacing);
    gridLayout->setVerticalSpacing(kSwatchSpacing);
    for (int i = 0; i < colorSwatchCount(); ++i) {
        auto *swatch = new QPushButton(grid);
        swatch->setObjectName(colorSwatchName(i));
        swatch->setFixedSize(kSwatchWidth, kSwatchHeight);
        // 键盘不给这 32 块（`NoFocus`）：Tab 一趟要穿过它们 32 次，"用键盘挑颜色"这件事
        // 由上面那一格色号承担（它就是这一档的键盘路径）。
        swatch->setFocusPolicy(Qt::NoFocus);
        Theme::markColorSwatch(swatch, QColor::fromString(QString::fromLatin1(kPalette[i])));
        QObject::connect(swatch, &QPushButton::clicked, edit,
                         [edit, i] { edit->setText(QString::fromLatin1(kPalette[i])); });
        gridLayout->addWidget(swatch, i / kPaletteColumns, i % kPaletteColumns);
    }
    gridLayout->setColumnStretch(kPaletteColumns, 1);
    layout->addWidget(grid);

    box.setContent(content);
    // 这一行小字要在 execPrompt **之前**补（见 setNote() 那段）。
    box.setNote(QObject::tr("色号可以手打：`#rrggbb`（六位）或 `#aarrggbb`（八位，带透明度），也可以不带 `#`。"));

    // 现在这一格认不认得出一个色 —— **唯一出处**：预览块和「确定」那颗按钮都靠它。
    QPushButton *accept = box.buttonAt(0);
    const auto syncFromEdit = [preview, accept](const QString &value) {
        const QColor color = parseColorText(value);
        paintPreview(preview, color);
        if (accept != nullptr) {
            // 认不出的色号点了也没有意义，而"点了确定、颜色没变"是那种最难解释的失败。
            accept->setEnabled(color.isValid());
        }
    };
    QObject::connect(edit, &QLineEdit::textChanged, &box, syncFromEdit);
    syncFromEdit(edit->text());

    host->execPrompt(&box, false);
    if (box.clickedIndex() != 0) {
        return QColor();
    }
    return parseColorText(edit->text());
}

}  // namespace InlineMessage



