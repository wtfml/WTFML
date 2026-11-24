/**
 * @file ResourceDetailPage.cpp
 * @brief 实现资源详情页。
 * @note 页面结构与信号约定见 ResourceDetailPage.hpp。
 */

#include "ResourceDetailPage.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "Animations.hpp"
#include "FlowLayout.hpp"
#include "LoadingState.hpp"
#include "MainWindowUI.hpp"
#include "RowActivation.hpp"
#include "Theme.hpp"
#include "VersionTreeDelegate.hpp"

namespace {

// 这一页自己的几个尺寸。**都不跟着窗口走**：嵌进覆盖层那一层的时候这一页拿到的是内容区
// 的整幅宽（见 OverlayHost::showPage），"整页"看着才像一页；单独拿出来用时构造函数
// 里那个 resize() 只是给验证工程一个像样的初始大小。
constexpr int kPageWidth = 820;
constexpr int kPageHeight = 560;
/// 头卡上那个图标多大（正方形）。PCL 那一页也是这么大一圈。
constexpr int kIconSize = 64;
/// chip 之间的横竖间距（折行后两行之间也是这个值）。
constexpr int kChipRowSpacing = 6;

/// 这条标签**现在会画出来的字**（截断之后的那一串）。paintEvent 和验证工程读的那个读数
/// 都走这一份 —— 两处各算一遍的话，"量到的那串"和"屏幕上的那串"迟早不是一回事。
///
/// NOTE: 它得摆在 ElidedLabel **前面**：成员函数的函数体是按"类定义之后"那一处编译的，
/// 但名字查找只认**写在类前面**的那些声明。
QString elidedFor(const QLabel *label);

/// 放不下时把尾巴截成省略号的 QLabel。
///
/// **为什么非它不可**：资源名和简介都能长过这一格（CurseForge 上的简介常常是整段话），
/// 而 QLabel 在控件比文字窄的时候是**硬裁**的 —— 最后一个字被切掉半个、右边什么都没有，
/// 看着像画坏了（`VersionTreeDelegate::paint()` 的注释讲的是同一件事）。
/// QLabel 自己没有 elide 相关的 API（翻遍 qlabel.h 都没有），所以只能自己接 paintEvent。
///
/// NOTE: 和 `MainWindowUI.cpp` 里底栏用的那一个**是同一种东西、不是同一份代码**（那个在
/// 它的匿名命名空间里，这一层看不见）。这边只留了这两行字需要的那两档：字号与字重
/// 由外面设，截哪一头固定是尾巴。
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget *parent = nullptr) : QLabel(parent) {}

    /// 宽度报 **0**：这一格**可以被压窄**，压窄之后字被截成省略号（见 paintEvent）。
    ///
    /// NOTE: 不覆写的话 QLabel 报的是**整句话的宽度**（单行标签的 minimumSizeHint 就是它），
    /// 而"最小宽度"是布局里唯一一个能把父窗口顶开的量：症状是这一页的最小宽度等于
    /// 最长那句简介的宽度，窄窗口下页面底下多出一根横向滚动条，怎么拖窗口都消不掉
    /// （`OverlayHost` 那个整页滚动区的 widgetResizable 认的正是这个量）。
    QSize minimumSizeHint() const override {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const QRect box = contentsRect();
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(box, alignment() | Qt::TextSingleLine, elidedFor(this));
    }
};

QString elidedFor(const QLabel *label) {
    return label->fontMetrics().elidedText(label->text(), Qt::ElideRight,
                                          qMax(0, label->contentsRect().width()));
}

/// 底色是 Base 的卡（头卡用它）。层级靠底色和留白，不画额外框线。
QFrame *makeCard(QWidget *parent) {
    auto *card = new QFrame(parent);
    // UI 7b：页头不画卡片底，直接落在整窗磨砂底上（设计稿 resource.html）。
    card->setFrameShape(QFrame::NoFrame);
    return card;
}

/// chip 那一排的形状与颜色**由 `Theme::markChipButton()` 一处说了算**
/// （实例页那排搜索筛子是第二个读者），底下走 `ui/Theme.cpp` 的 `chipStyleSheet()`。
/// 这一页唯一那句话：chip 的外观不走 QSS 之外的东西（其余外观都交给 Fusion + 代理，
/// 见 UI 样式基线）。

}  // namespace

// ---------- 版本行左边那个字母方块 ----------

namespace ResourceDetailMarks {

int badgeSize() {
    // **和下载页那四张表的缩略图同出一处**（见头文件）。改那边这一格跟着走 —— 两处都是
    // "列表行前面那一格"，不一样大的话并列看过去一眼就看得出来。
    return MainWindowUI::resourceTreeIconSize();
}

QColor channelColor(int channel) {
    switch (channel) {
    case 0:
        return QColor(0x2e, 0x7d, 0x32);  // 正式版：绿
    case 1:
        return QColor(0xef, 0x6c, 0x00);  // 测试版：橙
    case 2:
        return QColor(0xc6, 0x28, 0x28);  // 早期测试版：红
    default:
        // 认不出来的档给灰的。**不是"不画"**：同组里别的行前面都有方块，这一行没有的话
        // 它的正文会往左挪一截，看着像列表坏了。
        return QColor(0x75, 0x75, 0x75);
    }
}

QString channelLetter(int channel) {
    switch (channel) {
    case 0:
        return QStringLiteral("R");
    case 1:
        return QStringLiteral("B");
    case 2:
        return QStringLiteral("A");
    default:
        return QString();
    }
}

QColor channelLetterColor() {
    return QColor(0xff, 0xff, 0xff);
}

QPixmap channelBadge(int channel, int size) {
    if (size <= 0) {
        return QPixmap();
    }
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(channelColor(channel));
    // 圆角半径 = 边长的八分之一（32 px 上是 4）：够圆润，又还看得出是个方块 ——
    // 和 chip 那种"胶囊"是两回事，这一枚在列表里是个标记，不是按钮。
    const qreal radius = qMax(2.0, size / 8.0);
    painter.drawRoundedRect(QRectF(0, 0, size, size), radius, radius);
    const QString letter = channelLetter(channel);
    if (!letter.isEmpty()) {
        QFont font = painter.font();
        font.setBold(true);
        // 半个方块高：32 px 上是 16 px 的字，方块里那一枚字母撑得开又不贴边。
        font.setPixelSize(qMax(8, size / 2));
        painter.setFont(font);
        painter.setPen(channelLetterColor());
        painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, letter);
    }
    return pixmap;
}

}  // namespace ResourceDetailMarks

// ---------- 这一页 ----------

using ResourceDetail::Chip;
using ResourceDetail::Group;
using ResourceDetail::Header;
using ResourceDetail::Row;

ResourceDetailPage::ResourceDetailPage(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("资源详情"));
    // 这一页自己要收键盘：那颗「显示快照版」还配着一个裸 Alt（见 keyPressEvent），
    // 而**嵌进覆盖层的时候焦点不在这一页身上** —— `openPage()` 把焦点给了页眉那颗返回
    // 箭头（覆盖层那一层的控件），键事件只沿着"父控件链"往上传，返回箭头和这一页是
    // 兄弟，它在的那条链上根本没有这一页。所以：允许拿焦点（下面那一行）+ 开页时把焦点
    // 摆回来（showEvent 里那段）。
    setFocusPolicy(Qt::StrongFocus);
    // 只影响"单独拿出来用"（验证工程那几处）：嵌进覆盖层的时候，这一页拿到的是内容区的
    // 整幅宽（见 OverlayHost::showPage 里那句 area->layout()->addWidget(dialog)）。
    resize(kPageWidth, kPageHeight);

    auto *outer = new QVBoxLayout(this);
    // 页眉（返回箭头 + 标题）由覆盖层那一层画，这一页从卡开始 —— 边上留的那一圈和设置页
    // 一样是 12（覆盖层自己还有 9，见 OverlayHost::kOverlayPageMargin）。
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    // ---------- 头卡 ----------
    QFrame *card = makeCard(this);
    card->setObjectName(headerCardName());
    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(0, 4, 0, 8);
    cardLayout->setSpacing(18);

    m_icon = new QLabel(card);
    m_icon->setObjectName(iconLabelName());
    // 尺寸**钉死**：图是异步回来的（走 ResourceStore 那一套），回来之前也得占着同样大一格
    // —— 不然图一到位，右边那几行字整体往左跳一下。
    m_icon->setFixedSize(kIconSize, kIconSize);
    m_icon->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(m_icon, 0, Qt::AlignTop);

    auto *textColumn = new QVBoxLayout;
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(4);

    m_title = new ElidedLabel(card);
    m_title->setObjectName(titleLabelName());
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont = Theme::textFont(titleFont, Theme::TextRole::Heading);
    m_title->setFont(titleFont);
    textColumn->addWidget(m_title);

    m_description = new ElidedLabel(card);
    m_description->setObjectName(descriptionLabelName());
    // 按钮挪到右上之后，窄窗下别把简介挤成 0 宽（至少留得下几个字 + 省略号）。
    m_description->setMinimumWidth(120);
    // 简介只画一行、放不下就截（全文挂在鼠标提示上，见 setHeader）。让它换行的话，
    // CurseForge 那种整段的简介会把这张卡撑成半屏高 —— 而这一页的主角是下面那份版本列表。
    textColumn->addWidget(m_description);

    m_meta = new ElidedLabel(card);
    m_meta->setObjectName(metaLabelName());
    QFont metaFont = m_meta->font();
    metaFont = Theme::textFont(metaFont, Theme::TextRole::Caption);
    m_meta->setFont(metaFont);
    {
        // 次要信息用次要那一档颜色（和列表里第二行小字同一个角色，见 VersionTreeDelegate）。
        QPalette metaPalette = m_meta->palette();
        metaPalette.setColor(QPalette::WindowText,
                             metaPalette.color(QPalette::PlaceholderText));
        m_meta->setPalette(metaPalette);
    }
    textColumn->addWidget(m_meta);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    // 三颗按钮**都在**，只是按"拼不拼得出网址"来显示 —— 拼不出地址的那一颗不画
    // （见 Header 里那两段 NOTE：点了没反应的按钮比没有这颗按钮更糟）。
    m_open = new QPushButton(card);
    m_open->setObjectName(openButtonName());
    Theme::markButton(m_open, Theme::ButtonKind::Neutral);
    m_open->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_open->setVisible(false);
    connect(m_open, &QPushButton::clicked, this,
            [this] { emit openExternalRequested(m_header.openUrl); });
    buttonRow->addWidget(m_open);

    m_mcmod = new QPushButton(tr("在 MC 百科搜索"), card);
    m_mcmod->setObjectName(mcmodButtonName());
    // 这几颗都是「看一眼 / 拿一份出去」（一般性）—— 转到网页、
    // 复制名字，一件都不改盘上的东西。
    Theme::markButton(m_mcmod, Theme::ButtonKind::Ghost);
    m_mcmod->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_mcmod->setVisible(false);
    connect(m_mcmod, &QPushButton::clicked, this,
            [this] { emit openExternalRequested(m_header.mcmodUrl); });
    buttonRow->addWidget(m_mcmod);

    m_copyName = new QPushButton(tr("复制名称"), card);
    m_copyName->setObjectName(copyNameButtonName());
    Theme::markButton(m_copyName, Theme::ButtonKind::Ghost);
    m_copyName->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_copyName->setVisible(false);
    connect(m_copyName, &QPushButton::clicked, this,
            [this] { emit copyNameRequested(m_header.nameToCopy); });
    buttonRow->addWidget(m_copyName);
    textColumn->addStretch(1);

    auto *buttonColumn = new QVBoxLayout;
    buttonColumn->setSpacing(4);
    buttonColumn->addLayout(buttonRow);
    m_modSettings = new QPushButton(modSettingsText(), card);
    m_modSettings->setObjectName(modSettingsButtonName());
    Theme::markButton(m_modSettings, Theme::ButtonKind::Neutral);
    m_modSettings->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_modSettings->setEnabled(false);
    m_modSettings->setToolTip(tr("打开配置文件管理；共享配置需自行选择。"));
    connect(m_modSettings, &QPushButton::clicked, this, [this] {
        if (m_modSettingsShown) emit modSettingsRequested();
    });
    m_modSettings->hide();
    buttonColumn->addWidget(m_modSettings, 0, Qt::AlignRight);
    cardLayout->addLayout(textColumn, 1);
    cardLayout->addLayout(buttonColumn);
    buttonColumn->setAlignment(Qt::AlignTop);
    outer->addWidget(card);

    // ---------- 那一排游戏版本 chip ----------
    //
    // chip 是大版本组（1.21、1.20……），数量有限，但窄窗口下一行也可能放不下：用折行布局
    // 往下折，**不出横向滚动条**，页面宽度也不会被 chip 顶开。
    m_chipRow = new QWidget(this);
    m_chipRow->setObjectName(chipRowName());
    m_chipRow->setLayout(new FlowLayout(kChipRowSpacing, kChipRowSpacing));
    // 那一排 chip 的**右边**挂着「显示快照版」，而且它和那一排是同一件事（见
    // ResourceDetailPage.hpp 里那段）：放在 chip 区**外面**、贴着第一行，
    // chip 折出第二行第三行时它不跟着挪。
    auto *chipLine = new QHBoxLayout;
    chipLine->setContentsMargins(0, 0, 0, 0);
    chipLine->setSpacing(8);
    chipLine->addWidget(m_chipRow, 1, Qt::AlignTop);

    m_showSnapshots = new QCheckBox(tr("显示快照版"), this);
    m_showSnapshots->setObjectName(showSnapshotsCheckBoxName());
    // 鼠标提示里点明快捷键。
    m_showSnapshots->setToolTip(tr("按 Alt 键也能切换"));
    m_showSnapshots->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_showSnapshots->setMinimumHeight(Theme::chipHeight());  // 和第一行 chip 同高，文字才对齐
    connect(m_showSnapshots, &QCheckBox::toggled, this,
            [this](bool checked) { emit showSnapshotsChanged(checked); });
    chipLine->addWidget(m_showSnapshots, 0, Qt::AlignTop);

    outer->addLayout(chipLine);

    // ---------- chip 那一排与版本列表中间那一行----------
    //
    // "游戏版本选择"就是上面那一排 chip、"模组版本选择"就是下面那棵树 —— 于是这一行夹在
    // 它俩中间（chipLine 的 addLayout 之后、m_list 的 addWidget 之前）。
    //
    // **默认收着**：这一页是同一个对象、建一次留着的，而"有没有前置"要等版本列表回来
    // 才算得出来（`setDependencies()` 那一下才露）。一开始就摆一条空的会占掉一格高度，
    // 然后等那一行字回来再"长高"一下 —— 那是能看见的一跳。
    m_dependencies = new QLabel(this);
    m_dependencies->setObjectName(dependenciesLabelName());
    m_dependencies->setWordWrap(true);
    m_dependencies->setVisible(false);
    outer->addWidget(m_dependencies);

    // ---------- 版本列表 ----------
    //
    // 三层设置和下载页那四张表**逐项相同**（表头藏起来、行高常数、根上那个小三角不画、
    // 同一个字号偏移、同一个代理），只有两处不一样：
    // ① 图标换成那一枚字母方块（代理见到条目给了 DecorationRole 就画它）；
    // ② 叶子那一层**不画**右边的 `>`（点是"下它"这个动作，不是"还有下一页"）。
    m_list = new QTreeWidget(this);
    m_list->setObjectName(versionListName());
    m_list->setHeaderHidden(true);
    m_list->setUniformRowHeights(true);
    m_list->setRootIsDecorated(false);
    m_list->setColumnCount(1);
    m_list->setItemDelegate(new VersionTreeDelegate(m_list, /*leafArrow=*/false,
                                                    ResourceDetailMarks::badgeSize()));
    // 悬停底色由代理画（`Theme::rowHoverFill()`），但那一位 `State_MouseOver` 是**视图**填的。
    // 少了这一句，代理里那段一个字都不会生效 —— 不报错、断言也不一定先红，屏幕上就是"悬停没反应"。
    m_list->setMouseTracking(true);
    m_list->setExpandsOnDoubleClick(false);
    QFont listFont = m_list->font();
    listFont = Theme::textFont(listFont, Theme::TextRole::Body);
    m_list->setFont(listFont);
    m_list->setIconSize(QSize(ResourceDetailMarks::badgeSize(),
                              ResourceDetailMarks::badgeSize()));
    outer->addWidget(m_list, 1);
    // 展开/折叠动画（规则见 ui/Animations.hpp）。
    Anim::animateExpansion(m_list);

    // 组头那一行是**开关**（点一下展开、再点一下收起），版本那一行是**动作**（下它）——
    // 这两档的分别正是 connectRowActivation() 那个判据在管的事（点得快的时候开关行
    // 后面那几下会被 Qt 压掉，得由它补上）。
    connectRowActivation(
        m_list, [this](QTreeWidgetItem *item) { onRowActivated(item); },
        [](QTreeWidgetItem *item) { return item != nullptr && item->parent() == nullptr; });
}

// ---------- 名字 ----------

QString ResourceDetailPage::headerCardName() { return QStringLiteral("resourceDetailHeaderCard"); }
QString ResourceDetailPage::iconLabelName() { return QStringLiteral("resourceDetailIcon"); }
QString ResourceDetailPage::titleLabelName() { return QStringLiteral("resourceDetailTitle"); }
QString ResourceDetailPage::descriptionLabelName() {
    return QStringLiteral("resourceDetailDescription");
}
QString ResourceDetailPage::metaLabelName() { return QStringLiteral("resourceDetailMeta"); }
QString ResourceDetailPage::openButtonName() { return QStringLiteral("resourceDetailOpen"); }
QString ResourceDetailPage::mcmodButtonName() { return QStringLiteral("resourceDetailMcmod"); }
QString ResourceDetailPage::copyNameButtonName() {
    return QStringLiteral("resourceDetailCopyName");
}
QString ResourceDetailPage::chipRowName() { return QStringLiteral("resourceDetailChips"); }
QString ResourceDetailPage::chipButtonNamePrefix() {
    return QStringLiteral("resourceDetailChip");
}
QString ResourceDetailPage::showSnapshotsCheckBoxName() {
    return QStringLiteral("resourceDetailShowSnapshots");
}
QString ResourceDetailPage::modSettingsButtonName() {
    return QStringLiteral("resourceDetailModSettings");
}
QString ResourceDetailPage::modSettingsText() { return tr("模组设置"); }
QString ResourceDetailPage::dependenciesLabelName() {
    return QStringLiteral("resourceDetailDependencies");
}
QString ResourceDetailPage::versionListName() { return QStringLiteral("resourceDetailVersions"); }
int ResourceDetailPage::rowKeyRole() { return Qt::UserRole; }

QString ResourceDetailPage::titleText() const {
    return m_header.title.isEmpty() ? tr("资源详情") : m_header.title;
}

// ---------- 头卡 ----------

void ResourceDetailPage::setHeader(const Header &header) {
    m_header = header;
    m_title->setText(header.title);
    m_title->setToolTip(header.title);
    m_description->setText(header.description);
    // 全文挂鼠标提示：这一行是**截过的**，截掉的那部分只有这儿看得到。
    m_description->setToolTip(header.description);
    m_meta->setText(header.metaLine);
    m_meta->setToolTip(header.metaLine);
    if (!header.openUrl.isEmpty()) {
        m_open->setText(tr("转到 %1").arg(header.openLabel));
        m_open->setVisible(true);
    } else {
        m_open->setVisible(false);
        // **文字也要清掉**，不能光藏。这一颗的文字是 openButtonText() 的唯一
        // 出处，而那个访问器的约定是"没摆 = 空串"（见 .hpp）—— 光藏不清的话，先看过一个
        // 有来源页的资源、再翻到一个没有的（CurseForge 那一类），那一格**看不见**但
        // openButtonText() 照旧答「转到 Modrinth」：界面和读它的人各说各的，
        // 而验证工程正是照着那句话断言"这一页有没有可跳转的来源"。
        // NOTE: 判"摆没摆"**不能**改成去看 isVisible() —— 这一页自己收起来的时候，
        // 子控件跟着一起 isVisible()=false，那样答出来的空串是假的。
        m_open->setText(QString());
    }
    m_mcmod->setVisible(!header.mcmodUrl.isEmpty());
    m_copyName->setVisible(!header.nameToCopy.isEmpty());
    // 这一页自己的窗口标题也换掉：覆盖层那一层显示的是 titleText()，两处同一个出处。
    setWindowTitle(titleText());
}

void ResourceDetailPage::setIcon(const QPixmap &icon) {
    if (icon.isNull()) {
        m_icon->clear();
        return;
    }
    m_icon->setPixmap(icon.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation));
}

QString ResourceDetailPage::openButtonText() const { return m_open->text(); }
QString ResourceDetailPage::shownDescription() const { return elidedFor(m_description); }
QString ResourceDetailPage::descriptionToolTip() const { return m_description->toolTip(); }

// ---------- chip ----------

void ResourceDetailPage::setChips(const QList<Chip> &chips, const QString &preselectedKey) {
    m_chips = chips;
    rebuildChips();
    // 默认按哪一颗由调用方说（它知道用户在下载页选的是哪个游戏版本）——
    // 这儿**不猜**、也不发信号：信号是"用户点了"的意思，开页那一发会去重算一遍分组，
    // 而调用方摆好 chip 之后本来就要自己 setGroups()（那才是第一份内容）。
    m_currentChip = preselectedKey;
    applyChipChecked(preselectedKey);
}

void ResourceDetailPage::rebuildChips() {
    QLayout *layout = m_chipRow->layout();
    if (layout == nullptr) {
        return;
    }
    // 整排重建（和实例设置页里那几行同一个做法）：**当场 delete，不 deleteLater** ——
    // 名字按下标给，按名字找控件的验证工程会先撞上悬着的旧那颗。
    for (QPushButton *button : m_chipButtons) {
        delete button;
    }
    m_chipButtons.clear();
    for (int i = 0; i < m_chips.size(); ++i) {
        const Chip &chip = m_chips.at(i);
        auto *button = new QPushButton(chip.text, m_chipRow);
        button->setObjectName(chipButtonNamePrefix() + QString::number(i));
        button->setCheckable(true);
        // 高度、圆角外观、换主题重挂三件事一起（见上面那段）。
        Theme::markChipButton(button);
        // 身份挂属性上（和 rowKeyRole 那条同一个理由：文字是给人看的，会随文案改）。
        button->setProperty("chipKey", chip.key);
        connect(button, &QPushButton::clicked, this, [this, button] { onChipClicked(button); });
        layout->addWidget(button);
        m_chipButtons.append(button);
    }
}

void ResourceDetailPage::onChipClicked(QPushButton *button) {
    if (button == nullptr) {
        return;
    }
    const QString key = button->property("chipKey").toString();
    if (key == m_currentChip) {
        // 点的是**已经按着的那一颗**：把勾选状态摆回去就行（Qt 点一下会自己翻成没按），
        // 别再重算一遍分组 —— 重算会让下面那份列表整个重建、用户眼前闪一下。
        applyChipChecked(m_currentChip);
        return;
    }
    m_currentChip = key;
    applyChipChecked(key);
    emit chipChanged(key);
}

void ResourceDetailPage::applyChipChecked(const QString &key) {
    // 一排 chip 是**互斥**的（和单选钮一样），一个都不按是不合法的状态：点「全部」那一颗
    // 就是"不筛"。所以这儿总能挑出一颗来按 —— 挑不中（调用方给了个表里没有的 key）
    // 就按"全部"那一颗，别再往下一层层猜。
    QPushButton *fallback = nullptr;
    QPushButton *wanted = nullptr;
    for (QPushButton *button : m_chipButtons) {
        const QString chipKey = button->property("chipKey").toString();
        if (chipKey.isEmpty() && fallback == nullptr) {
            fallback = button;
        }
        if (chipKey == key && wanted == nullptr) {
            wanted = button;
        }
    }
    QPushButton *target = wanted != nullptr ? wanted : fallback;
    for (QPushButton *button : m_chipButtons) {
        button->setChecked(button == target);
    }
    if (target != nullptr) {
        // 挑中的不是要的那一颗 ⇒ "当前值"跟着纠正（不然界面按着一颗、currentChip()
        // 说的是另一颗）。
        //
        // NOTE: 判据得比**挑中的那一颗**，不能比 `key != m_currentChip` —— 那两个入参
        // 在三个调用点上都已经被调成相等了（setChips 先赋再调、onChipClicked 那两处
        // 也一样），拿它当判据等于这一句永远不执行，而它要修的正是"调用方给了一个
        // 表里没有的 key"那一档（界面上按着「全部」、currentChip() 却回那个空 key）。
        const QString picked = target->property("chipKey").toString();
        if (picked != m_currentChip) {
            m_currentChip = picked;
        }
    }
}

QString ResourceDetailPage::currentChip() const { return m_currentChip; }
QList<QPushButton *> ResourceDetailPage::chips() const { return m_chipButtons; }

bool ResourceDetailPage::showSnapshots() const {
    return m_showSnapshots != nullptr && m_showSnapshots->isChecked();
}

void ResourceDetailPage::setShowSnapshots(bool showSnapshots) {
    if (m_showSnapshots == nullptr || m_showSnapshots->isChecked() == showSnapshots) {
        return;
    }
    // 挡掉那一发 `toggled`：这是调用方在摆初始值（开页那一下），不是"用户拨了" ——
    // 和 setChips 不发 chipChanged 是同一条规矩（不挡的话开页会立刻绕回调用方一次，
    // 而那时候它手上那份 chip 表才刚刚摆好）。
    const QSignalBlocker block(m_showSnapshots);
    m_showSnapshots->setChecked(showSnapshots);
}

void ResourceDetailPage::setModSettingsSlot(bool show) {
    m_modSettingsShown = show;
    if (m_modSettings == nullptr) {
        return;
    }
    m_modSettings->setVisible(show);
    m_modSettings->setEnabled(show);
}

QString ResourceDetailPage::chipTextAt(int index) const {
    return index >= 0 && index < m_chipButtons.size() ? m_chipButtons.at(index)->text()
                                                     : QString();
}

QString ResourceDetailPage::chipKeyAt(int index) const {
    return index >= 0 && index < m_chipButtons.size()
               ? m_chipButtons.at(index)->property("chipKey").toString()
               : QString();
}

// ---------- 版本列表 ----------

void ResourceDetailPage::setGroups(const QList<Group> &groups) {
    if (groups.isEmpty()) {
        // 空表**不摆一片空白**：一个资源还没发过版本是正常的（刚建的项目），
        // 一片空白看着像"读失败了"。
        showPlaceholder(tr("此资源未发布任何版本"));
        return;
    }
    fillGroups(groups);
}

void ResourceDetailPage::fillGroups(const QList<Group> &groups) {
    LoadingState::finish(m_list);
    m_list->clear();
    m_placeholder.clear();
    for (const Group &group : groups) {
        auto *groupItem = new QTreeWidgetItem(m_list);
        groupItem->setText(0, group.title);
        for (const Row &row : group.rows) {
            auto *item = new QTreeWidgetItem(groupItem);
            item->setText(0, row.title);
            item->setData(0, MainWindowUI::rowSubTextRole(), row.subText);
            item->setData(0, MainWindowUI::rowIconRole(),
                          ResourceDetailMarks::channelBadge(row.channel,
                                                            ResourceDetailMarks::badgeSize()));
            item->setData(0, rowKeyRole(), row.key);
            if (!row.tooltip.isEmpty()) {
                item->setToolTip(0, row.tooltip);
            }
        }
        // 组头默认**收起**，点标题行才展开；只有调用方标了 `expanded` 的（从实例进来、
        // 细版本正好是实例版本的那几组）才摊开。
        // NOTE: 换一颗 chip 会把整份列表重建（分组的算法在业务层），展开状态也就跟着复位 ——
        // 这是有意接受的（要留住它得按组名记一份，而组名会随 chip 改）。用户自己点开、收起
        // 一个组不触发重建，也不改变顶排选中的大版本。
        groupItem->setExpanded(group.expanded);
    }
}

void ResourceDetailPage::setDependencies(const QString &text) {
    if (m_dependencies == nullptr) {
        return;
    }
    // 空串 = **整个收起来**（不是摆一条空的）：这一行的存在本身就是一条信息
    // （"这一版有前置"），而一条空白会把上面那排 chip 和下面那棵树推开一格。
    m_dependencies->setText(text);
    m_dependencies->setToolTip(text);
    m_dependencies->setVisible(!text.isEmpty());
}

QString ResourceDetailPage::dependenciesText() const {
    return m_dependencies != nullptr && m_dependencies->isVisible() ? m_dependencies->text()
                                                                   : QString();
}

void ResourceDetailPage::setPlaceholder(const QString &text) { showPlaceholder(text); }

void ResourceDetailPage::showPlaceholder(const QString &text) {
    LoadingState::finish(m_list);
    m_list->clear();
    m_placeholder = text;
    if (text.isEmpty()) {
        return;
    }
    // 顶层条目（代理给顶层画粗体）——和下载页那四张表的"没取到"那句是同一个形状。
    auto *item = new QTreeWidgetItem(m_list);
    item->setText(0, text);
}

QString ResourceDetailPage::placeholderText() const { return m_placeholder; }

void ResourceDetailPage::setLoading(const QString &text) {
    m_list->clear();
    m_placeholder = text;
    LoadingState::begin(m_list, text);
}

void ResourceDetailPage::setFailed(const QString &reason, std::function<void()> retry) {
    m_list->clear();
    m_placeholder = reason;
    LoadingState::fail(m_list, reason, std::move(retry));
}
QTreeWidget *ResourceDetailPage::versionList() const { return m_list; }

int ResourceDetailPage::groupCount() const {
    return m_list->topLevelItemCount();
}

int ResourceDetailPage::versionRowCount() const {
    int count = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        count += m_list->topLevelItem(i)->childCount();
    }
    return count;
}

QString ResourceDetailPage::groupTitleAt(int index) const {
    QTreeWidgetItem *item = m_list->topLevelItem(index);
    return item != nullptr ? item->text(0) : QString();
}

bool ResourceDetailPage::groupExpandedAt(int index) const {
    QTreeWidgetItem *item = m_list->topLevelItem(index);
    return item != nullptr && item->isExpanded();
}

void ResourceDetailPage::onRowActivated(QTreeWidgetItem *item) {
    if (item == nullptr) {
        return;
    }
    if (item->parent() == nullptr) {
        // 组头：开/关（**自己翻**，Qt 那个双击展开在接线时就关掉了，见 RowActivation.hpp）。
        item->setExpanded(!item->isExpanded());
        return;
    }
    const QString key = item->data(0, rowKeyRole()).toString();
    if (!key.isEmpty()) {
        emit rowActivated(key);
    }
}

// ---------- 几何 ----------

void ResourceDetailPage::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    // 三个省略号标签是按"当下这一格的宽度"算的（见 elidedFor），宽度变了要重画一次 ——
    // 不然窄窗口下会留着按宽窗口算出来的那一串（甩出去半行字）。
    m_title->update();
    m_description->update();
    m_meta->update();
}

void ResourceDetailPage::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    // 摆上去那一刻这一页才拿到最终宽度（覆盖层给的），所以这里再催一次。
    m_title->update();
    m_description->update();
    m_meta->update();
    // 焦点摆回这一页自己身上（理由见构造函数里那段 + keyPressEvent）。**只在焦点不在这一页
    // 里面的时候拿** —— 焦点已经在本页某个控件上（用户点了某颗 chip、或者上面那条提示关掉
    // 之后这一页又露出来）时不该把它抢走。
    QWidget *focus = QApplication::focusWidget();
    if (focus == nullptr || !isAncestorOf(focus)) {
        setFocus(Qt::OtherFocusReason);
    }
}

void ResourceDetailPage::keyPressEvent(QKeyEvent *event) {
    // 裸 Alt = 拨一下「显示快照版」（鼠标提示上也写着）。Ctrl/Shift/Meta 同按
    // 的那几发**不算** —— 那是别的快捷键，放给下面那一层。
    const Qt::KeyboardModifiers others =
        event->modifiers() & ~(Qt::AltModifier | Qt::KeypadModifier);
    if (event->key() == Qt::Key_Alt && others == Qt::NoModifier) {
        if (!event->isAutoRepeat() && m_showSnapshots != nullptr) {
            // 走 setChecked：`toggled` 那一发会照常发出去（这一下就是"用户拨了"）。
            m_showSnapshots->setChecked(!m_showSnapshots->isChecked());
        }
        event->accept();
        return;
    }
    // 别的一律交给基类（Esc 归它管）。
    QDialog::keyPressEvent(event);
}

