/**
 * @file PresetManagePage.cpp
 * @brief 实现 PresetManagePage.hpp。
 *
 * 两页一个形状，都照着这一栏里已有的样子：
 *   第 0 页（列表）= 和「客户端实例」「服务端」同一页形 —— 顶上一整条搜索框 + 下面一排
 *     类别 chip（类别多，挤在一行会把搜索框压没），中间一整棵不画外框的树（行高 58、
 *     行首图标 26、按类别分组），最底下那颗建东西的主操作。
 *   第 1 页（细节）= 和实例设置那一页同一页形 —— 顶上一行「← 返回」+ 这是哪一份，下面
 *     整片底色上是"图标 + 名字 + 说明"和几小节（预设内容 / 从实例提取 / 应用到实例），
 *     主操作浮在底下正中间。
 */

#include "PresetManagePage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"
#include "LoadingState.hpp"
#include "TextMarks.hpp"
#include "Animations.hpp"
#include "InstanceGroupDelegate.hpp"
#include "MainWindowPageLayout.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QIcon>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <functional>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

using namespace MainWindowPageLayout;

namespace {

/// 单独开出来时的大小（内嵌到覆盖层里时由那一层给整个内容区，见 OverlayHost）。
constexpr int kDialogWidth = 980;
constexpr int kDialogHeight = 640;
/// 段表 / 计划表的高度：三四行可见，再多就滚。
constexpr int kSectionListHeight = 118;
constexpr int kPlanListHeight = 132;
/// 身份区那颗图标（实例设置页顶上那颗就是这个尺寸）。
constexpr int kIdentityIconSize = 48;
/// 细节页左边那条导航（实例设置页左栏也是这一档宽）。
constexpr int kDetailNavWidth = 220;
constexpr int kDetailNavRowHeight = 44;
/// 预设行上那三个数据槽（外面喂进来的值，页面里只用不翻）。
///
/// ★ 号码从 `Qt::UserRole + 20` 起，别往 +1 / +2 上挪：`MainWindowUI::groupIdRole()`
/// 是 `Qt::UserRole + 1`、资源那几张表还有 `+3`，撞上去的症状是"每一行都被当成组头"
/// （选中不生效、右边那一栏永远空着）。`rowBrokenRole()` 必须和这里那一个逐字一致。
constexpr int kRoleId = Qt::UserRole;
constexpr int kRoleBroken = Qt::UserRole + 21;
constexpr int kRoleDefault = Qt::UserRole + 22;
/// 搜索用的一份"名字 + 说明"（原先那个代理模型上的过滤串，现在直接挂在这一格上）。
constexpr int kRoleSearchText = Qt::UserRole + 23;
static_assert(kRoleBroken == PresetManagePage::rowBrokenRole(),
              "rowBrokenRole() 是给外面认行用的那把钥匙，两边不许各写各的");  // i18n-ignore
/// 「读不回来」那一组的钥匙。它不是预设类别（类别钥匙由外面给），才用一个不可能撞上的串。
QString brokenGroupKey() {
    return QStringLiteral("!broken");
}

bool itemIsBroken(const QTreeWidgetItem *item) {
    return item != nullptr && item->data(0, kRoleBroken).toBool();
}

bool itemIsGroup(const QTreeWidgetItem *item) {
    return item != nullptr && item->data(0, MainWindowUI::groupIdRole()).isValid();
}

/// 小一号 + PlaceholderText 那档墨色。和 JavaManageDialog / MainWindowUI 里那几个
/// 匿名 namespace 里的同名帮手是同一套形状（跨不过来，只能各留一份）。
void applyHintStyle(QLabel *hint) {
    hint->setWordWrap(true);
    QFont hintFont = hint->font();
    hintFont = Theme::textFont(hintFont, Theme::TextRole::Caption);
    hint->setFont(hintFont);
    QPalette hintPalette = hint->palette();
    hintPalette.setColor(QPalette::WindowText, hintPalette.color(QPalette::PlaceholderText));
    hint->setPalette(hintPalette);
}

}  // namespace


PresetManagePage::PresetManagePage(QWidget *parent) : QDialog(parent) {
    setWindowTitle(titleText());
    resize(kDialogWidth, kDialogHeight);
    buildUi();
    refreshActions();
    refreshCaptureEnabled();
}

QString PresetManagePage::titleText() const {
    return QCoreApplication::translate("PresetManagePage", "实例预设");
}

void PresetManagePage::buildUi() {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(kFilterRowSpacing);

    // 这一页和同一栏里的「客户端实例」「服务端」是**同一个形状**：一层列表 + 一层细节。
    //   第 0 层 = 搜索框 + 类别 chip + 一棵按类别分组的树（和实例列表逐字同形）；
    //   第 1 层 = 选中那一份的内容 / 从实例提取 / 应用到实例（和下载页那张安装页
    //            同一个形状：顶上一行「← 返回」，中间是可滚动的卡片，底下那颗主动作
    //            浮在正中间）。
    // 两层之间只靠"点一行"和「← 返回」走 —— 和实例页点一行进实例设置是同一条路。
    m_layers = new QStackedWidget(this);
    m_layers->setObjectName(QStringLiteral("presetLayers"));
    outer->addWidget(m_layers, 1);

    buildListLayer();
    buildDetailLayer();

    // 状态那一行在两层**下面**：读盘失败、提取结果都不该被翻来翻去的图层挡住。
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(statusLabelName());
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->hide();
    outer->addWidget(m_statusLabel);
}

/// 第 0 层：搜索 + 类别 + 一棵按类别分组的树 + 底下那颗「新建预设…」。
///
/// 顶上那两行和实例列表那一行同一个口径（见 `MainWindowUI` 里 `instanceFilterRow`
/// 那一段）：搜索框 chip 高、chip 那一排挨着摆；只是预设的类别比实例多，两样分成两排
/// （搜索框独占一行，chip 在下面一行）。树也一样：不画外框、左边不画展开三角、
/// 行高就是实例页那一档（58）、行首图标 26。
void PresetManagePage::buildListLayer() {
    m_listLayer = new QWidget(m_layers);
    m_listLayer->setObjectName(QStringLiteral("presetListPage"));
    auto *layout = new QVBoxLayout(m_listLayer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kFilterRowSpacing);

    // ---- 搜索框一行、类别 chip 一行 ----
    //
    // 八个类别 chip 摆在同一行里会把搜索框挤得只剩几十像素（用户量的就是这一条），
    // 所以分两排：上面一条**整幅宽**的搜索框，下面一排 chip。chip 再多也只是往右排，
    // 不再抢搜索框的宽度。
    auto *filterBox = new QWidget(m_listLayer);
    filterBox->setObjectName(QStringLiteral("presetFilterRow"));
    auto *filterLayout = new QVBoxLayout(filterBox);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(kFilterRowSpacing);

    auto *searchRow = new QWidget(filterBox);
    auto *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(kFilterRowSpacing);
    m_searchEdit = new QLineEdit(searchRow);
    m_searchEdit->setObjectName(QStringLiteral("presetSearchEdit"));
    m_searchEdit->setPlaceholderText(tr("搜索名称或说明"));
    m_searchEdit->setClearButtonEnabled(true);
    // 和实例页那个框一样高（chip 那一档）：同一栏里两页最上面那一行不该差着高度。
    m_searchEdit->setFixedHeight(Theme::chipHeight());
    m_searchEdit->setProperty("wtfmlChipHeight", true);
    searchLayout->addWidget(m_searchEdit, 1);
    filterLayout->addWidget(searchRow);

    auto *chipRow = new QWidget(filterBox);
    m_chipRow = new QHBoxLayout(chipRow);
    m_chipRow->setContentsMargins(0, 0, 0, 0);
    m_chipRow->setSpacing(kFilterRowSpacing);
    filterLayout->addWidget(chipRow);
    layout->addWidget(filterBox);
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this] { rebuildPresetList(QString(), true); });

    // 「新建为」那个下拉框：新建的类别现在由**内容**决定（编辑器保存时按启用了哪几部分
    // 推出来），这一格只剩「提取为新预设」那条老路在用。它不占界面（外面也一直藏着它）。
    m_newCategoryCombo = new QComboBox(this);
    m_newCategoryCombo->setObjectName(newCategoryComboName());
    m_newCategoryCombo->hide();

    // 「没有符合筛选条件的预设」那一行：位置和实例页那一句一样（筛选行下面、树上面）——
    // 筛空了的时候那句话直接跟着筛选行读下来才对得上"是上面那些条件把它滤没的"。
    m_noMatchLabel = new QLabel(m_listLayer);
    m_noMatchLabel->setObjectName(QStringLiteral("presetNoMatchLabel"));
    markTextRole(m_noMatchLabel, Theme::secondaryTextRoleValue());
    m_noMatchLabel->setAlignment(Qt::AlignCenter);
    m_noMatchLabel->setWordWrap(true);
    // 纯说明**不许**参与横向度量（折行 QLabel 报出来的 sizeHint 是"整句排成一行"的宽度）。
    m_noMatchLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_noMatchLabel->setVisible(false);
    layout->addWidget(m_noMatchLabel);

    // 一张**按类别分组**的列表：组头是类别名（粗体 + 右端箭头，和实例页 / 服务端页
    // 那两张表同一个代理），组里一行一份预设。窗格本身和实例树同一条规矩：不画外框、
    // 左边不画展开三角（展开/收起画在行尾那个箭头上）、每行等高、开着悬停跟踪。
    m_presetList = new QTreeWidget(m_listLayer);
    m_presetList->setObjectName(presetListName());
    m_presetList->setHeaderHidden(true);
    m_presetList->setFrameShape(QFrame::NoFrame);
    m_presetList->setRootIsDecorated(false);
    m_presetList->setUniformRowHeights(true);
    m_presetList->setIndentation(14);
    // 这一份树必须设 iconSize（理由同实例树那一段）：不设就用样式给的默认值，
    // 26 px 画出来的图标会被缩到默认那一档。
    m_presetList->setIconSize(QSize(MainWindowUI::instanceTreeIconSize(),
                                    MainWindowUI::instanceTreeIconSize()));
    m_presetList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_presetList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_presetList->setAlternatingRowColors(false);
    m_presetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 悬停底色要它（代理里那个 `&& hovered` 是另一半）：视图不填 `State_MouseOver`
    // 就一次都不会出现，而且不报错。
    m_presetList->setMouseTracking(true);
    // 行高走代理的默认值（`MainWindowUI::instanceTreeRowHeight()` = 58）——和实例页那一档
    // 逐像素一样，两页的行才看着是同一种行。
    auto *listDelegate = new InstanceGroupDelegate(m_presetList);
    // 行尾那三颗（齿轮 / 置顶箭头 / 垃圾桶）**不挂**：这一份页面上"对一条做什么"
    // 在细节那一层里（复制 / 设为默认 / 删除 / 上移下移），挂着的话行尾会多出三格
    // 点下去没有去处的图标。
    listDelegate->setInlineRowActions(false);
    m_presetList->setItemDelegate(listDelegate);
    Anim::animateExpansion(m_presetList);
    layout->addWidget(m_presetList, 1);
    connect(m_presetList, &QTreeWidget::currentItemChanged, this,
            &PresetManagePage::onCurrentPresetChanged);
    // 组头点一下 = 展开 / 收起（它不可选中，所以不会抢走"现在看的是哪一份"）；
    // 点中一份预设 = 进细节那一层（和实例页点一行进实例设置同一步）。
    m_presetList->setExpandsOnDoubleClick(false);
    connect(m_presetList, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (itemIsGroup(item)) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                if (item != nullptr && !itemIsBroken(item)) {
                    showDetailLayer(true);
                }
            });

    // 底下那颗新建：和「服务端」那一页底下那颗「新建服务端…」同一个位置、同一档样式
    // （造一个新东西出来 = 这一页的主操作，走主题色）。
    m_newButton = new QPushButton(tr("新建预设…"), m_listLayer);
    m_newButton->setObjectName(newButtonName());
    m_newButton->setAutoDefault(false);
    m_newButton->setToolTip(QCoreApplication::translate("PresetManagePage", "建一份空预设；建好之后在编辑页里填内容。"));
    Theme::markButton(m_newButton, Theme::ButtonKind::Accent);
    layout->addWidget(m_newButton);
    connect(m_newButton, &QPushButton::clicked, this, [this] {
        emit createRequested(m_newCategoryCombo->currentData().toString());
    });

    m_layers->addWidget(m_listLayer);
}

/// 第 1 层：选中那一份的细节（内容 / 从实例提取 / 应用到实例）。
///
/// 形状照搬**实例设置**那一页：顶上一行「← 返回 + 这是哪一份」，下面是左导航 + 右页表
///（概览 / 从实例提取 / 应用到实例），主动作「应用」在左栏底下。
void PresetManagePage::buildDetailLayer() {
    m_detailLayer = new QWidget(m_layers);
    m_detailLayer->setObjectName(QStringLiteral("presetDetailPage"));
    auto *layout = new QVBoxLayout(m_detailLayer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- 顶上那一行：回得去 + 这一份叫什么 + 右端那一颗 ----
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    m_backButton = new QPushButton(tr("← 返回"), m_detailLayer);
    m_backButton->setObjectName(QStringLiteral("presetBackButton"));
    Theme::markButton(m_backButton, Theme::ButtonKind::Neutral);
    m_backButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    top->addWidget(m_backButton);
    // 这一行是"上一层的说明"，平着写（不加粗、不放大）—— 安装页顶上那句
    // 「将基于原版 26.3 安装」也是这一档；页面上唯一的粗标题是小节卡片的那几条。
    m_detailTitle = new QLabel(m_detailLayer);
    m_detailTitle->setObjectName(QStringLiteral("presetDetailTitle"));
    top->addWidget(m_detailTitle);
    top->addStretch(1);
    // 「保存」（内嵌时由控制器换成「编辑预设」）：名字框里打完字顺手回车是最常见的
    // 动作，所以它长在名字框那一行的尾巴上。
    m_saveDetailsButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "保存"), m_detailLayer);
    m_saveDetailsButton->setObjectName(saveDetailsButtonName());
    m_saveDetailsButton->setAutoDefault(false);
    Theme::markButton(m_saveDetailsButton, Theme::ButtonKind::Neutral);
    top->addWidget(m_saveDetailsButton);
    layout->addLayout(top);
    connect(m_backButton, &QPushButton::clicked, this, [this] { showDetailLayer(false); });
    connect(m_saveDetailsButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (id.isEmpty()) return;
        emit detailsEdited(id, m_nameEdit->text(), m_descriptionEdit->text());
    });

    // ---- 下面两栏：左边是**这一份自己的导航**，右边是选中的那一页 ----
    //
    // 和实例设置那一页同一个分工：左边一条竖着的页表 + 底下那颗主动作（实例那边是
    // 「启动实例」，这边是「应用」），右边是当前这一页的内容，摊在页面底色上。
    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(18);
    layout->addLayout(columns, 1);

    auto *navPanel = new QFrame(m_detailLayer);
    navPanel->setObjectName(QStringLiteral("presetDetailNavPanel"));
    markSurface(navPanel, Theme::sectionSurfaceValue());
    navPanel->setFixedWidth(kDetailNavWidth);
    auto *navLayout = new QVBoxLayout(navPanel);
    navLayout->setContentsMargins(10, 10, 10, 12);
    navLayout->setSpacing(10);
    m_detailNav = new QListWidget(navPanel);
    m_detailNav->setObjectName(QStringLiteral("presetDetailNav"));
    m_detailNav->setFrameShape(QFrame::NoFrame);
    m_detailNav->setStyleSheet(QStringLiteral("QListWidget{background:transparent;border:none;}"));
    m_detailNav->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailNav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_detailNav->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_detailNav->setFocusPolicy(Qt::NoFocus);
    for (const QString &text : {QCoreApplication::translate("PresetManagePage", "概览"), QCoreApplication::translate("PresetManagePage", "从实例提取"),
                                QCoreApplication::translate("PresetManagePage", "应用到实例")}) {
        auto *item = new QListWidgetItem(text, m_detailNav);
        item->setSizeHint(QSize(0, kDetailNavRowHeight));
    }
    m_detailNav->setCurrentRow(0);
    navLayout->addWidget(m_detailNav, 1);
    m_applyButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "应用"), navPanel);
    m_applyButton->setObjectName(applyButtonName());
    m_applyButton->setAutoDefault(false);
    Theme::markButton(m_applyButton, Theme::ButtonKind::Primary);
    QFont applyFont = m_applyButton->font();
    applyFont = Theme::textFont(applyFont, Theme::TextRole::Body);  // 按钮一律正文 14 系统字（tokens-r2 §1）
    applyFont.setWeight(QFont::Medium);
    m_applyButton->setFont(applyFont);
    m_applyButton->setMinimumHeight(48);
    navLayout->addWidget(m_applyButton);
    columns->addWidget(navPanel);

    // 右边：三页叠着一个栈（和实例设置页右栏那一个用法相同）。
    m_detailPages = new QStackedWidget(m_detailLayer);
    m_detailPages->setObjectName(QStringLiteral("presetDetailPages"));
    const auto makePage = [this](const std::function<void(QVBoxLayout *)> &fill) {
        auto *scroll = new Theme::GutterScrollArea(m_detailPages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *body = new QWidget(scroll);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 18);
        bodyLayout->setSpacing(18);
        fill(bodyLayout);
        scroll->setWidget(body);
        return scroll;
    };
    m_detailPages->addWidget(makePage([this](QVBoxLayout *body) {
        body->addWidget(buildIdentityBlock(body->parentWidget()));
        body->addWidget(buildPresetContentSection(body->parentWidget()));
        body->addStretch(1);
    }));
    m_detailPages->addWidget(makePage([this](QVBoxLayout *body) {
        body->addWidget(buildCaptureSection(body->parentWidget()));
        body->addStretch(1);
    }));
    m_detailPages->addWidget(makePage([this](QVBoxLayout *body) {
        body->addWidget(buildApplySection(body->parentWidget()));
        body->addStretch(1);
    }));
    columns->addWidget(m_detailPages, 1);
    connect(m_detailNav, &QListWidget::currentRowChanged, m_detailPages,
            &QStackedWidget::setCurrentIndex);
    connect(m_applyButton, &QPushButton::clicked, this, [this] {
        if (!m_planCanApply || m_busy) return;
        const QString presetId = currentPresetId();
        const QString instanceId = comboInstanceId(m_applyInstanceCombo);
        if (presetId.isEmpty() || instanceId.isEmpty()) return;
        if (m_planNeedsConfirm && !m_applyArmed) {
            armApply();
            return;
        }
        const bool confirmed = m_planNeedsConfirm || m_planReplaceConfirmed;
        disarmApply();
        emit applyRequested(presetId, instanceId, confirmed);
    });

    m_layers->addWidget(m_detailLayer);
}

/// 顶上那块"图标 + 名字 + 说明 + 一行小字"，和实例设置页那一排同一个位置、同一个配方：
/// 左边一格大图标，右边一整条名字框；小字（类别 / 段数）在框下面，平着写。
QWidget *PresetManagePage::buildIdentityBlock(QWidget *parent) {
    auto *block = new QWidget(parent);
    auto *layout = new QHBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    m_identityIcon = new QLabel(block);
    m_identityIcon->setObjectName(QStringLiteral("presetDetailIcon"));
    m_identityIcon->setFixedSize(kIdentityIconSize, kIdentityIconSize);
    m_identityIcon->setPixmap(VersionIcons::puzzleMark(
        kIdentityIconSize, palette().color(QPalette::WindowText)));
    layout->addWidget(m_identityIcon, 0, Qt::AlignTop);

    auto *fields = new QVBoxLayout;
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(8);
    m_nameEdit = new QLineEdit(block);
    m_nameEdit->setObjectName(nameEditName());
    m_nameEdit->setPlaceholderText(QCoreApplication::translate("PresetManagePage", "预设名称"));
    InputSizing::shortValue(m_nameEdit, InputSizing::kNameChars, InputSizing::Script::Han);
    fields->addWidget(m_nameEdit);
    m_descriptionEdit = new QLineEdit(block);
    m_descriptionEdit->setObjectName(descriptionEditName());
    m_descriptionEdit->setPlaceholderText(QCoreApplication::translate("PresetManagePage", "说明（可选）"));
    InputSizing::longValue(m_descriptionEdit, QCoreApplication::translate("PresetManagePage", "预设说明：一两句话，长度不定"));
    fields->addWidget(m_descriptionEdit);
    m_presetMeta = new QLabel(block);
    m_presetMeta->setObjectName(QStringLiteral("presetMetaLabel"));
    applyHintStyle(m_presetMeta);
    fields->addWidget(m_presetMeta);
    layout->addLayout(fields, 1);

    // Enter 也算「保存」（名字框里打完字顺手回车是最常见的动作）。
    connect(m_nameEdit, &QLineEdit::returnPressed, m_saveDetailsButton, &QPushButton::click);
    connect(m_descriptionEdit, &QLineEdit::returnPressed, m_saveDetailsButton,
            &QPushButton::click);
    return block;
}

/// 「预设内容」那一小节：段表 + 这一份的增删改（名字 / 说明在上面那块身份区里）。
QWidget *PresetManagePage::buildPresetContentSection(QWidget *parent) {
    QVBoxLayout *card = nullptr;
    auto *section = makePageSection(tr("预设内容"), parent, &card);

    m_sectionList = new QListWidget(section);
    m_sectionList->setObjectName(sectionListName());
    m_sectionList->setSelectionMode(QAbstractItemView::NoSelection);
    m_sectionList->setFocusPolicy(Qt::NoFocus);
    m_sectionList->setFixedHeight(kSectionListHeight);
    // 不套第二个白框、也不带自己的外框：这一份的段就摊在页面底色上
    // （实例设置页那一页的勾选组也没有白底）。
    m_sectionList->setFrameShape(QFrame::NoFrame);
    m_sectionList->setStyleSheet(QStringLiteral("QListWidget{background:transparent;border:none;}"));
    m_sectionList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    card->addWidget(m_sectionList);

    // 这一份的增删改：**对选中那一份做什么**，所以长在这一份的页上（不是列表底下
    // 那一排 —— 列表那一层只有"新建"这一件对整份清单的事）。
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    m_duplicateButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "复制"), section);
    m_duplicateButton->setObjectName(duplicateButtonName());
    m_duplicateButton->setAutoDefault(false);
    m_duplicateButton->setToolTip(QCoreApplication::translate("PresetManagePage", "复制预设及其文件。"));
    m_defaultButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "设为默认"), section);
    m_defaultButton->setObjectName(defaultButtonName());
    m_defaultButton->setAutoDefault(false);
    m_defaultButton->setToolTip(QCoreApplication::translate("PresetManagePage", "安装实例时优先选中这一份。"));
    m_deleteButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "删除"), section);
    m_deleteButton->setObjectName(deleteButtonName());
    m_deleteButton->setAutoDefault(false);
    Theme::markButton(m_deleteButton, Theme::ButtonKind::Danger);
    m_deleteButton->setToolTip(
        QCoreApplication::translate("PresetManagePage", "连这一份的目录一起删掉，删掉的东西会进回收站，可以从那里还原。"));
    m_moveUpButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "上移"), section);
    m_moveUpButton->setObjectName(moveUpButtonName());
    m_moveUpButton->setAutoDefault(false);
    m_moveDownButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "下移"), section);
    m_moveDownButton->setObjectName(moveDownButtonName());
    m_moveDownButton->setAutoDefault(false);
    // 五颗平分这一行：实例设置页那几排按钮是平铺开的，不是挤在左边。
    for (QPushButton *button : {m_duplicateButton, m_defaultButton, m_moveUpButton,
                                m_moveDownButton, m_deleteButton}) {
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        actions->addWidget(button);
    }
    card->addLayout(actions);

    connect(m_duplicateButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (!id.isEmpty()) emit duplicateRequested(id);
    });
    connect(m_deleteButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (id.isEmpty()) return;
        if (!m_deleteArmed) {
            armDelete();
            return;
        }
        disarmDelete();
        emit deleteRequested(id);
    });
    connect(m_defaultButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (id.isEmpty()) return;
        const auto *item = m_presetList->currentItem();
        const bool nowDefault = item != nullptr && item->data(0, kRoleDefault).toBool();
        emit defaultRequested(id, !nowDefault);
    });
    connect(m_moveUpButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (!id.isEmpty()) emit moveRequested(id, -1);
    });
    connect(m_moveDownButton, &QPushButton::clicked, this, [this] {
        const QString id = currentPresetId();
        if (!id.isEmpty()) emit moveRequested(id, 1);
    });

    auto *editMods = new QPushButton(tr("编辑模组清单"), section);
    editMods->setObjectName(QStringLiteral("presetEditModsButton"));
    Theme::markButton(editMods, Theme::ButtonKind::Neutral);
    editMods->setVisible(false);
    card->addWidget(editMods, 0, Qt::AlignLeft);
    connect(editMods, &QPushButton::clicked, this, [this] {
        if (!currentPresetId().isEmpty()) emit editModsRequested(currentPresetId());
    });
    auto *editOptions = new QPushButton(tr("编辑游戏设置与按键"), section);
    editOptions->setObjectName(QStringLiteral("presetEditOptionsButton"));
    Theme::markButton(editOptions, Theme::ButtonKind::Neutral);
    editOptions->setVisible(false);
    card->addWidget(editOptions, 0, Qt::AlignLeft);
    connect(editOptions, &QPushButton::clicked, this, [this] {
        if (!currentPresetId().isEmpty()) emit editOptionsRequested(currentPresetId());
    });
    return section;
}

/// 「从实例提取」那一小节：一按展开十样勾选 + 来源实例 + 两颗按钮。
QWidget *PresetManagePage::buildCaptureSection(QWidget *parent) {
    QVBoxLayout *card = nullptr;
    auto *section = makePageSection(tr("从实例提取"), parent, &card);

    auto *captureToggle = new QPushButton(tr("勾选要收的几样"), section);
    captureToggle->setObjectName(QStringLiteral("presetCaptureToggle"));
    captureToggle->setCheckable(true);
    captureToggle->setAutoDefault(false);
    Theme::markButton(captureToggle, Theme::ButtonKind::Neutral);
    card->addWidget(captureToggle, 0, Qt::AlignLeft);
    auto *captureBody = new QWidget(section);
    captureBody->setObjectName(QStringLiteral("presetCaptureBody"));
    auto *captureLayout = new QVBoxLayout(captureBody);
    captureLayout->setContentsMargins(0, 0, 0, 0);
    card->addWidget(captureBody);
    captureBody->hide();
    connect(captureToggle, &QPushButton::toggled, captureBody, &QWidget::setVisible);

    auto *captureGrid = new QGridLayout;
    captureGrid->setObjectName(QStringLiteral("presetCaptureGrid"));
    captureGrid->setContentsMargins(0, 0, 0, 0);
    captureGrid->setHorizontalSpacing(12);
    captureGrid->setVerticalSpacing(6);
    auto addCheck = [this, captureGrid, section](int row, int column, const QString &text,
                                               const QString &tooltip, bool checked,
                                               QCheckBox **out) {
        auto *box = new QCheckBox(text, section);
        box->setChecked(checked);
        box->setToolTip(tooltip);
        m_captureTips.insert(box, tooltip);
        captureGrid->addWidget(box, row, column);
        *out = box;
        connect(box, &QCheckBox::toggled, this, &PresetManagePage::refreshCaptureEnabled);
        return box;
    };
    captureGrid->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "游戏设置"), section), 0, 0);
    addCheck(0, 1, QCoreApplication::translate("PresetManagePage", "基础设置"),
             QCoreApplication::translate("PresetManagePage", "FOV、界面缩放、语言、音量、渲染距离等。"),
             true, &m_captureOptions);
    addCheck(0, 2, QCoreApplication::translate("PresetManagePage", "标准按键"),
             QCoreApplication::translate("PresetManagePage", "options.txt 里的 key_*。"),
             true, &m_captureKeys);
    captureGrid->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "服务器"), section), 1, 0);
    addCheck(1, 1, QCoreApplication::translate("PresetManagePage", "服务器列表"),
             QCoreApplication::translate("PresetManagePage", "servers.dat；追加到目标实例，已有地址不重复。"),
             true, &m_captureServers);
    captureGrid->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "配置文件"), section), 2, 0);
    addCheck(2, 1, QStringLiteral("config/"),
             QCoreApplication::translate("PresetManagePage", "整份按「缺了才拷」落下去，不会盖掉目标上已经有的同名项。"),
             true, &m_captureConfig);
    addCheck(2, 2, QStringLiteral("defaultconfigs/"),
             QCoreApplication::translate("PresetManagePage", "新实例第一次启动时模组会照它生成配置。"), false,
             &m_captureDefaultConfigs);
    captureGrid->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "资源包 / 光影"), section), 3, 0);
    addCheck(3, 1, QCoreApplication::translate("PresetManagePage", "资源包"),
             QCoreApplication::translate("PresetManagePage", "resourcepacks/。文件通常较大，默认排除。"), false,
             &m_captureResourcePacks);
    addCheck(3, 2, QCoreApplication::translate("PresetManagePage", "光影包"),
             QCoreApplication::translate("PresetManagePage", "shaderpacks/。文件通常较大，默认排除。"), false, &m_captureShaderPacks);
    captureGrid->addWidget(new QLabel(QStringLiteral("MaLiLib"), section), 4, 0);
    addCheck(4, 1, QCoreApplication::translate("PresetManagePage", "热键"),
             QCoreApplication::translate("PresetManagePage", "按结构认这一族，一个模组一段；默认只收绑好的热键。"),
             true, &m_captureMalilib);
    addCheck(4, 2, QCoreApplication::translate("PresetManagePage", "连值一起收"),
             QCoreApplication::translate("PresetManagePage", "包含开关和其他设置。"),
             false, &m_captureMalilibValues);
    captureGrid->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "数据包"), section), 5, 0);
    addCheck(5, 1, QCoreApplication::translate("PresetManagePage", "存档数据包"),
             QCoreApplication::translate("PresetManagePage", "saves/<世界>/datapacks/；目标实例没有该存档时先记为「待应用」。"),
             false, &m_captureDataPacks);
    captureGrid->setColumnStretch(3, 1);
    captureLayout->addLayout(captureGrid);

    auto *captureRow = new QHBoxLayout;
    captureRow->setContentsMargins(0, 0, 0, 0);
    captureRow->setSpacing(8);
    captureRow->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "来源实例"), section));
    m_captureInstanceCombo = new QComboBox(section);
    m_captureInstanceCombo->setObjectName(captureInstanceComboName());
    m_captureInstanceCombo->setMinimumWidth(220);
    captureRow->addWidget(m_captureInstanceCombo, 1);
    captureLayout->addLayout(captureRow);

    auto *captureButtons = new QHBoxLayout;
    captureButtons->setContentsMargins(0, 0, 0, 0);
    captureButtons->setSpacing(8);
    m_captureButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "更新当前预设"), section);
    m_captureButton->setObjectName(captureButtonName());
    m_captureButton->setAutoDefault(false);
    m_captureButton->setToolTip(
        QCoreApplication::translate("PresetManagePage", "提取的内容会替换现有预设中的同名部分，其他部分保留。"));
    Theme::markButton(m_captureButton, Theme::ButtonKind::Accent);
    captureButtons->addWidget(m_captureButton);
    m_newPresetNameEdit = new QLineEdit(section);
    m_newPresetNameEdit->setObjectName(newPresetNameEditName());
    m_newPresetNameEdit->setPlaceholderText(QCoreApplication::translate("PresetManagePage", "新预设名称（可选）"));
    InputSizing::shortValue(m_newPresetNameEdit, InputSizing::kNameChars, InputSizing::Script::Han);
    m_newPresetNameEdit->setMaximumWidth(220);
    captureButtons->addWidget(m_newPresetNameEdit);
    m_captureAsNewButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "提取为新预设"), section);
    m_captureAsNewButton->setObjectName(captureAsNewButtonName());
    m_captureAsNewButton->setAutoDefault(false);
    Theme::markButton(m_captureAsNewButton, Theme::ButtonKind::Neutral);
    captureButtons->addWidget(m_captureAsNewButton);
    captureButtons->addStretch(1);
    captureLayout->addLayout(captureButtons);

    connect(m_captureButton, &QPushButton::clicked, this, [this] {
        const QString presetId = currentPresetId();
        const QString instanceId = comboInstanceId(m_captureInstanceCombo);
        if (presetId.isEmpty() || instanceId.isEmpty()) return;
        emit captureRequested(presetId, instanceId, captureChoice());
    });
    connect(m_captureAsNewButton, &QPushButton::clicked, this, [this] {
        const QString instanceId = comboInstanceId(m_captureInstanceCombo);
        if (instanceId.isEmpty()) return;
        emit captureAsNewRequested(instanceId, m_newPresetNameEdit->text().trimmed(),
                                   m_newCategoryCombo->currentData().toString(),
                                   captureChoice());
    });
    connect(m_captureInstanceCombo, &QComboBox::currentIndexChanged, this,
            &PresetManagePage::refreshCaptureEnabled);
    return section;
}

/// 「应用到实例」那一小节：先选目标实例、再看一眼计划、最后按底下那颗「应用」。
QWidget *PresetManagePage::buildApplySection(QWidget *parent) {
    QVBoxLayout *card = nullptr;
    auto *section = makePageSection(tr("应用到实例"), parent, &card);

    auto *applyRow = new QHBoxLayout;
    applyRow->setContentsMargins(0, 0, 0, 0);
    applyRow->setSpacing(8);
    applyRow->addWidget(new QLabel(QCoreApplication::translate("PresetManagePage", "目标实例"), section));
    m_applyInstanceCombo = new QComboBox(section);
    m_applyInstanceCombo->setObjectName(applyInstanceComboName());
    m_applyInstanceCombo->setMinimumWidth(220);
    applyRow->addWidget(m_applyInstanceCombo, 1);
    m_previewButton = new QPushButton(QCoreApplication::translate("PresetManagePage", "预览计划"), section);
    m_previewButton->setObjectName(previewButtonName());
    m_previewButton->setAutoDefault(false);
    m_previewButton->setToolTip(
        QCoreApplication::translate("PresetManagePage", "查看更改和冲突，不写入实例。"));
    Theme::markButton(m_previewButton, Theme::ButtonKind::Neutral);
    applyRow->addWidget(m_previewButton);
    auto *reportButton = new QPushButton(tr("执行报告"), section);
    reportButton->setObjectName(QStringLiteral("presetExecutionReportButton"));
    Theme::markButton(reportButton, Theme::ButtonKind::Neutral);
    applyRow->addWidget(reportButton);
    auto *resumeButton = new QPushButton(tr("继续未完成"), section);
    resumeButton->setObjectName(QStringLiteral("presetResumeButton"));
    Theme::markButton(resumeButton, Theme::ButtonKind::Neutral);
    applyRow->addWidget(resumeButton);
    applyRow->addStretch(1);
    card->addLayout(applyRow);
    connect(m_previewButton, &QPushButton::clicked, this, [this] {
        if (m_planLoading) {
            emit previewCancelRequested();
            return;
        }
        const QString presetId = currentPresetId();
        const QString instanceId = comboInstanceId(m_applyInstanceCombo);
        if (presetId.isEmpty() || instanceId.isEmpty()) return;
        clearPlan();
        emit planRequested(presetId, instanceId);
    });
    // 换实例 / 换预设都会让上一份计划作废（计划是"对着那个实例那一刻"算出来的）。
    connect(m_applyInstanceCombo, &QComboBox::currentIndexChanged, this, [this] {
        clearPlan();
        emit applyTargetChanged(comboInstanceId(m_applyInstanceCombo));
        refreshActions();
    });
    connect(reportButton, &QPushButton::clicked, this, [this] {
        if (!m_busy) emit executionReportRequested(currentPresetId(), comboInstanceId(m_applyInstanceCombo));
    });
    connect(resumeButton, &QPushButton::clicked, this, [this, resumeButton] {
        if (m_busy || currentPresetId().isEmpty() || comboInstanceId(m_applyInstanceCombo).isEmpty()) return;
        const QString selection = currentPresetId() + QLatin1Char(':') + comboInstanceId(m_applyInstanceCombo);
        if (resumeButton->property("armedSelection").toString() != selection) {
            emit executionReportRequested(currentPresetId(), comboInstanceId(m_applyInstanceCombo));
            resumeButton->setProperty("armedSelection", selection);
            resumeButton->setText(tr("再次选择以继续未完成的操作"));
            QTimer::singleShot(4000, resumeButton, [resumeButton] {
                resumeButton->setProperty("armedSelection", QString()); resumeButton->setText(tr("继续未完成"));
            });
            return;
        }
        resumeButton->setProperty("armedSelection", QString()); resumeButton->setText(tr("继续未完成"));
        emit resumeRequested(currentPresetId(), comboInstanceId(m_applyInstanceCombo));
    });

    m_planList = new QListWidget(section);
    m_planList->setObjectName(planListName());
    m_planList->setSelectionMode(QAbstractItemView::NoSelection);
    m_planList->setFocusPolicy(Qt::NoFocus);
    m_planList->setFixedHeight(kPlanListHeight);
    card->addWidget(m_planList);

    m_planHint = new QLabel(section);
    applyHintStyle(m_planHint);
    card->addWidget(m_planHint);
    return section;
}

/// 在两层之间翻。`detail` 真 = 进"这一份预设"那一层（列表收起来），假 = 回列表。
/// 没有选中一份可动手的预设时**进不去**（组头、读不回来的、空列表都按回列表处理）。
void PresetManagePage::showDetailLayer(bool detail) {
    if (m_layers == nullptr || m_detailLayer == nullptr || m_listLayer == nullptr) {
        return;
    }
    if (detail && !selectionValid()) {
        detail = false;
    }
    if (detail && m_detailTitle != nullptr) {
        // 标题要的是**那份预设自己的名字**（行文字尾巴上可能挂着「（默认）」）。
        QString name;
        const QString id = currentPresetId();
        QString iconPath;
        for (const PresetItem &preset : m_presets) {
            if (preset.id == id) {
                name = preset.name;
                iconPath = preset.iconPath;
                break;
            }
        }
        if (name.isEmpty()) {
            const auto *item = m_presetList->currentItem();
            name = item != nullptr ? item->text(0) : QString();
        }
        m_detailTitle->setText(name);
        // 顶上那颗图标跟着这一份走（没有自带图标就是拼图，和列表行首同一套判据）。
        if (m_identityIcon != nullptr) {
            QPixmap pixmap(iconPath);
            if (pixmap.isNull()) {
                pixmap = VersionIcons::puzzleMark(
                    kIdentityIconSize, palette().color(QPalette::WindowText));
            }
            m_identityIcon->setPixmap(pixmap.scaled(kIdentityIconSize, kIdentityIconSize,
                                                    Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
        }
    }
    m_layers->setCurrentWidget(detail ? m_detailLayer : m_listLayer);
}

// ---------------------------------------------------------------------------
// 外面灌进来的值
// ---------------------------------------------------------------------------

void PresetManagePage::setPresets(const QList<PresetItem> &presets,
                                  const QList<BrokenItem> &broken, const QString &selectId) {
    m_presets = presets;
    m_broken = broken;
    rebuildPresetList(selectId, false);
    m_currentId = currentPresetId();
}

void PresetManagePage::rebuildPresetList(const QString &selectId, bool emitSelection) {
    const QString previousId = m_currentId;
    const QString keep = selectId.isEmpty() ? m_currentId : selectId;
    const QString query = m_searchEdit->text().trimmed();
    m_fillingList = true;
    m_presetList->clear();

    // 先按类别分桶。桶与桶之间按 `setCategories()` 给的那个次序摆（和外面那四类
    // + 旧版 / 综合 / 草稿的固定次序一致），不认识的钥匙排在后面、按第一次出现的次序
    // —— 页面不认识钥匙背后的意思，它只负责"同一类的摆在一起"。
    QHash<QString, QList<const PresetItem *>> buckets;
    QStringList keys;
    for (const PresetItem &preset : m_presets) {
        if (!buckets.contains(preset.categoryKey)) {
            buckets.insert(preset.categoryKey, {});
            keys.append(preset.categoryKey);
        }
        buckets[preset.categoryKey].append(&preset);
    }
    QStringList ordered;
    for (const QString &key : m_groupOrder) {
        if (buckets.contains(key)) ordered.append(key);
    }
    for (const QString &key : keys) {
        if (!ordered.contains(key)) ordered.append(key);
    }

    // 行首那一格图标：这一份自带图标就用它，没有就用拼图（和文件页里"这一份是资源"
    // 同一个记号，见 VersionIcons::puzzleMark 那段）。实例页每行都有图标，
    // 这一页要是空着，两张列表一眼就不像同一张表。
    const QIcon fallbackIcon(VersionIcons::puzzleMark(
        MainWindowUI::instanceTreeIconSize(),
        m_presetList->palette().color(QPalette::WindowText)));

    auto makeGroup = [this](const QString &text) {        auto *group = new QTreeWidgetItem(m_presetList);
        group->setText(0, text);
        // 组头这一格是 `InstanceGroupDelegate` 认"这是组"的判据（和实例页同一把钥匙）。
        group->setData(0, MainWindowUI::groupIdRole(), text);
        group->setFlags(Qt::ItemIsEnabled);
        group->setFirstColumnSpanned(true);
        return group;
    };

    for (const QString &key : ordered) {
        QString text = m_groupText.value(key);
        if (text.isEmpty() && !buckets.value(key).isEmpty()) {
            text = buckets.value(key).first()->categoryText;
        }
        if (text.isEmpty()) text = key;
        auto *group = makeGroup(text);
        for (const PresetItem *preset : buckets.value(key)) {
            auto *item = new QTreeWidgetItem(group);
            QString label = preset->name;
            if (preset->isDefault) {
                label += QCoreApplication::translate("PresetManagePage", "（默认）");
            }
            item->setText(0, label);
            item->setData(0, kRoleId, preset->id);
            item->setData(0, kRoleBroken, false);
            item->setData(0, kRoleDefault, preset->isDefault);
            item->setData(0, kRoleSearchText,
                          preset->name + QLatin1Char(' ') + preset->description);
            QIcon rowIcon;
            if (!preset->iconPath.isEmpty()) rowIcon = QIcon(preset->iconPath);
            if (rowIcon.isNull()) rowIcon = fallbackIcon;
            item->setIcon(0, rowIcon);
            QString tip = preset->description;
            if (!preset->categoryText.isEmpty()) {
                if (!tip.isEmpty()) tip += QLatin1Char('\n');
                tip += QCoreApplication::translate("PresetManagePage", "类别：%1").arg(preset->categoryText);
            }
            if (!preset->updatedText.isEmpty()) {
                if (!tip.isEmpty()) tip += QLatin1Char('\n');
                tip += preset->updatedText;
            }
            tip += QCoreApplication::translate("PresetManagePage", "\n%1 段").arg(preset->sectionCount);
            item->setToolTip(0, tip);
        }
    }

    // 读不回来的那几份：它们没写类别，硬塞进某一类里等于替它们猜一个身份 —— 所以单开
    // 一组摆在最后。照样列出来但不可选：用户至少知道"它还在，只是读不动"。
    // 这一组**不跟着搜索走**（它们没有可信的名字可比），和原先"在全部那一档里一直露着"
    // 是同一条规矩。
    if (!m_broken.isEmpty()) {
        auto *group = makeGroup(tr("无法读取"));
        // 这一组的钥匙要**真的**写成 `brokenGroupKey()`：下面按搜索藏行时靠它把这一组
        // 摘出去（组里那几行没有可比的名字，跟着搜索一起消失才是撒谎）。
        group->setData(0, MainWindowUI::groupIdRole(), brokenGroupKey());
        for (const BrokenItem &one : m_broken) {
            auto *item = new QTreeWidgetItem(group);
            QString name = one.id;
            if (name.size() > 12) name = name.left(12) + QStringLiteral("…");
            item->setText(0, TextMarks::warning(QCoreApplication::translate("PresetManagePage", "读取失败：%1").arg(name)));
            item->setData(0, kRoleId, one.id);
            item->setData(0, kRoleBroken, true);
            item->setFlags(Qt::NoItemFlags);
            item->setToolTip(0, one.reason);
        }
    }

    // 筛选：只藏行，不重新建行（一遍一遍重建会把滚动位置和展开状态一起丢掉）。
    // 两道条件是**且**的关系：搜索词管行、类别 chip 管组（实例页那边也是这个分工）。
    const QString categoryKey = m_categoryFilterKey;
    for (int g = 0; g < m_presetList->topLevelItemCount(); ++g) {
        auto *group = m_presetList->topLevelItem(g);
        if (group->data(0, MainWindowUI::groupIdRole()).toString() == brokenGroupKey()) {
            // 这一组不跟着搜索走（它们没有可信的名字可比）；但点了某一类之后它也不该
            // 再露着 —— 它没有类别，谈不上"属于这一类"。
            group->setHidden(!categoryKey.isEmpty());
            continue;
        }
        const bool groupMatches = categoryKey.isEmpty() ||
                                  group->data(0, MainWindowUI::groupIdRole()).toString() == categoryKey;
        int visible = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            auto *child = group->child(i);
            const bool match = groupMatches &&
                               (query.isEmpty() ||
                                child->data(0, kRoleSearchText)
                                    .toString()
                                    .contains(query, Qt::CaseInsensitive));
            child->setHidden(!match);
            if (match) ++visible;
        }
        group->setHidden(visible == 0);
    }

    // 选中的那一份不在列表里（或者被搜索藏掉了）就退回第一条看得见的。
    QTreeWidgetItem *target = nullptr;
    QTreeWidgetItem *first = nullptr;
    for (int g = 0; g < m_presetList->topLevelItemCount() && target == nullptr; ++g) {
        auto *group = m_presetList->topLevelItem(g);
        if (group->isHidden()) continue;
        group->setExpanded(true);
        for (int i = 0; i < group->childCount(); ++i) {
            auto *child = group->child(i);
            if (child->isHidden() || itemIsBroken(child)) continue;
            if (first == nullptr) first = child;
            if (child->data(0, kRoleId).toString() == keep) {
                target = child;
                break;
            }
        }
    }
    m_presetList->setCurrentItem(target != nullptr ? target : first);
    m_fillingList = false;

    // 「没有符合筛选条件的预设」那一句：筛空了才露（和实例页那一句同一个位置和说法）。
    if (m_noMatchLabel != nullptr) {
        int visibleRows = 0;
        for (int g = 0; g < m_presetList->topLevelItemCount(); ++g) {
            const auto *group = m_presetList->topLevelItem(g);
            if (group->isHidden()) continue;
            for (int i = 0; i < group->childCount(); ++i) {
                if (!group->child(i)->isHidden()) ++visibleRows;
            }
        }
        m_noMatchLabel->setText(m_presets.isEmpty() ? tr("还没有预设")
                                                    : tr("没有符合筛选条件的预设"));
        m_noMatchLabel->setVisible(visibleRows == 0);
    }

    if (currentPresetId().isEmpty()) {
        m_currentId.clear();
        m_nameEdit->clear();
        m_descriptionEdit->clear();
        m_presetMeta->clear();
        m_sectionList->clear();
        m_presetMeta->setText(m_presets.isEmpty()
            ? tr("尚无预设")
            : tr("未找到匹配的预设"));
    }
    refreshActions();

    // 正停在细节那一层时被外面刷新了一下（删掉一份 / 改了名）：选中的那一份没了就退回
    // 列表，还在就把标题换成它的新名字。
    if (m_layers != nullptr && m_layers->currentWidget() == m_detailLayer) {
        showDetailLayer(true);
    }

    // 用户自己换了搜索词：新选中的那一份要跟着刷新右栏（外面靠这个信号知道"现在看的是谁"）。
    const QString nowId = currentPresetId();
    if (emitSelection && nowId != previousId) {
        m_currentId = nowId;
        clearPlan();
        emit presetSelected(nowId);
    }
}

void PresetManagePage::setCategories(const QList<CategoryOption> &categories) {
    m_categoryOptions = categories;
    // 分组次序与组名：空钥匙 = 「全部」那一项，它不是一组。
    m_groupOrder.clear();
    m_groupText.clear();
    for (const CategoryOption &option : categories) {
        if (option.key.isEmpty()) continue;
        m_groupOrder.append(option.key);
        m_groupText.insert(option.key, option.text);
    }

    // 「新建为」：只列真正能新建的那些（「全部」那一项不进下拉框）。
    const QString keepNew = m_newCategoryCombo->currentData().toString();
    {
        const QSignalBlocker blocker(m_newCategoryCombo);
        m_newCategoryCombo->clear();
        for (const CategoryOption &option : categories) {
            if (!option.key.isEmpty() && !option.filterOnly) {
                m_newCategoryCombo->addItem(option.text, option.key);
            }
        }
        const int index = m_newCategoryCombo->findData(keepNew);
        if (index >= 0) {
            m_newCategoryCombo->setCurrentIndex(index);
        }
    }

    // 类别 chip：亮着的永远是**一颗**（空钥匙 = 「全部」）。摆在搜索框右边、和实例页
    // 那一排 chip 同一个位置。
    rebuildCategoryChips();

    // 组名换了（或者第一次来）⇒ 组头跟着换一遍。没数据时什么都不用建。
    if (!m_presets.isEmpty() || !m_broken.isEmpty()) {
        rebuildPresetList(m_currentId, false);
    }
}

/// 类别 chip 按 `setCategories()` 的次序重摆一遍。每颗一个 `clicked` → 换成"只亮这一颗"。
/// 摆之前记住现在亮着哪一颗：外面重复调 `setCategories()` 时不该把用户的筛选重置掉。
void PresetManagePage::rebuildCategoryChips() {
    if (m_chipRow == nullptr) {
        return;
    }
    // 先看一遍外面给的那张表里有没有**空钥匙**那一项（「全部」）：没有就补一颗，
    // 否则点了某一类之后就没有路回"看全部"了。
    QStringList keys;
    QStringList texts;
    bool hasAll = false;
    for (const CategoryOption &option : m_categoryOptions) {
        if (option.key.isEmpty()) hasAll = true;
        keys.append(option.key);
        texts.append(option.text);
    }
    if (!hasAll) {
        keys.prepend(QString());
        texts.prepend(tr("全部"));
    }

    for (auto *chip : m_categoryChips) {
        m_chipRow->removeWidget(chip);
        delete chip;
    }
    m_categoryChips.clear();
    m_categoryChipKeys.clear();

    if (!keys.contains(m_categoryFilterKey)) {
        m_categoryFilterKey.clear();
    }
    // 亮着哪一颗由 `syncCategoryChips()` 一处说了算（摆的时候、点的时候都走它）。
    for (int i = 0; i < keys.size(); ++i) {
        auto *chip = new QPushButton(texts.at(i), m_chipRow->parentWidget());
        chip->setObjectName(categoryChipName(keys.at(i)));
        chip->setCheckable(true);
        chip->setAutoDefault(false);
        // chip 那一档（24 px 高、圆角、勾上是实底主题色）—— 和实例页那一排
        // **同一个出处** `Theme::markChipButton()`，两页的筛子长得一样靠的就是它。
        Theme::markChipButton(chip);
        m_chipRow->addWidget(chip);
        m_categoryChips.append(chip);
        m_categoryChipKeys.append(keys.at(i));
        const int index = i;
        connect(chip, &QPushButton::clicked, this, [this, index] {
            if (index >= m_categoryChipKeys.size()) {
                return;
            }
            const QString key = m_categoryChipKeys.at(index);
            // 一颗一颗都是开关（和实例页那四颗同一种手感）：
            //   点没亮的某类 = 只看这一类（别类自动灭）；
            //   再点亮的这一类 = 摘掉这一类，回到「全部」；
            //   点「全部」= 直接清掉筛选。
            if (key.isEmpty() || key == m_categoryFilterKey) {
                m_categoryFilterKey.clear();
            } else {
                m_categoryFilterKey = key;
            }
            syncCategoryChips();
            rebuildPresetList(QString(), true);
        });
    }
    syncCategoryChips();
}

/// 亮不亮只看 `m_categoryFilterKey` 一处：「全部」那一颗在筛选钥匙为空时亮，
/// 别的恰好在钥匙对上时亮。
void PresetManagePage::syncCategoryChips() {
    for (int i = 0; i < m_categoryChips.size(); ++i) {
        const QString key = m_categoryChipKeys.at(i);
        m_categoryChips.at(i)->setChecked(key.isEmpty() ? m_categoryFilterKey.isEmpty()
                                                        : key == m_categoryFilterKey);
    }
}

void PresetManagePage::setCaptureCheckEnabled(QCheckBox *box, quint32 bit, quint32 mask,
                                              const QString &note) {
    if (box == nullptr) {
        return;
    }
    const bool allowed = (mask & bit) != 0;
    if (!allowed) {
        box->setChecked(false);
    }
    box->setEnabled(allowed && !m_busy && m_detailsAvailable);
    box->setVisible(allowed);
    QString tip = m_captureTips.value(box, box->toolTip());
    if (!allowed && !note.isEmpty()) {
        tip += QStringLiteral("\n%1").arg(note);
    }
    box->setToolTip(tip);
}

void PresetManagePage::setCaptureAvailability(quint32 mask, const QString &note) {
    m_captureMask = mask;
    m_captureNote = note;
    setCaptureCheckEnabled(m_captureOptions, CaptureVanillaOptions, mask, note);
    setCaptureCheckEnabled(m_captureKeys, CaptureVanillaKeys, mask, note);
    setCaptureCheckEnabled(m_captureServers, CaptureServers, mask, note);
    setCaptureCheckEnabled(m_captureConfig, CaptureConfig, mask, note);
    setCaptureCheckEnabled(m_captureDefaultConfigs, CaptureDefaultConfigs, mask, note);
    setCaptureCheckEnabled(m_captureResourcePacks, CaptureResourcePacks, mask, note);
    setCaptureCheckEnabled(m_captureShaderPacks, CaptureShaderPacks, mask, note);
    setCaptureCheckEnabled(m_captureMalilib, CaptureMalilib, mask, note);
    setCaptureCheckEnabled(m_captureMalilibValues, CaptureMalilibValues, mask, note);
    setCaptureCheckEnabled(m_captureDataPacks, CaptureDataPacks, mask, note);
    if (auto *toggle = findChild<QPushButton *>(QStringLiteral("presetCaptureToggle"))) {
        toggle->setVisible(mask != 0);
        if (auto *body = findChild<QWidget *>(QStringLiteral("presetCaptureBody")))
            body->setVisible(mask != 0 && toggle->isChecked());
    }
    refreshCaptureEnabled();
}

void PresetManagePage::showPreset(const QString &id, const QString &name,
                                  const QString &description, const QString &categoryText,
                                  const QList<SectionItem> &sections) {
    m_detailsAvailable = true;
    m_currentId = id;
    LoadingState::finish(m_sectionList);
    m_nameEdit->setText(name);
    m_descriptionEdit->setText(description);
    // 细节那一层顶上的标题就是这一份的名字（外面改了名之后这一发跟着换）。
    if (m_detailTitle != nullptr) {
        m_detailTitle->setText(name);
    }

    QStringList parts;
    if (!categoryText.isEmpty()) {
        parts.append(QCoreApplication::translate("PresetManagePage", "类别：%1").arg(categoryText));
    }
    parts.append(QCoreApplication::translate("PresetManagePage", "%1 段").arg(sections.size()));
    m_presetMeta->setText(parts.join(QStringLiteral(" / ")));

    m_sectionList->clear();
    if (sections.isEmpty()) {
        auto *item = new QListWidgetItem(tr("暂无内容"),
                                         m_sectionList);
        item->setFlags(Qt::NoItemFlags);
    } else {
        for (const SectionItem &section : sections) {
            const QString title = section.title.isEmpty() ? section.kindText : section.title;
            QString text = QCoreApplication::translate("PresetManagePage", "%1 / %2 / %3 项")
                               .arg(title, section.modeText)
                               .arg(section.itemCount);
            if (!section.summary.isEmpty()) {
                text += QCoreApplication::translate("PresetManagePage", "（%1）").arg(section.summary);
            }
            auto *item = new QListWidgetItem(text, m_sectionList);
            item->setToolTip(QCoreApplication::translate("PresetManagePage", "%1（%2）").arg(title, section.kindText));
        }
    }
    clearPlan();
    refreshActions();
}

void PresetManagePage::fillInstances(QComboBox *combo, const QList<InstanceItem> &instances,
                                     const QString &selectId) {
    const QString keep = selectId.isEmpty() ? comboInstanceId(combo) : selectId;
    const QSignalBlocker blocker(combo);
    combo->clear();
    int selectRow = -1;
    for (const InstanceItem &instance : instances) {
        QString text = instance.name;
        if (instance.running) {
            text += QCoreApplication::translate("PresetManagePage", "（正在运行）");
        }
        combo->addItem(text, instance.id);
        if (instance.id == keep) {
            selectRow = combo->count() - 1;
        }
    }
    if (selectRow < 0 && combo->count() > 0) {
        selectRow = 0;
    }
    if (selectRow >= 0) {
        combo->setCurrentIndex(selectRow);
    }
}

void PresetManagePage::setInstances(const QList<InstanceItem> &instances,
                                    const QString &selectId) {
    fillInstances(m_captureInstanceCombo, instances, selectId);
    fillInstances(m_applyInstanceCombo, instances, selectId);
    // The same instance id may now refer to another game root or run state.
    clearPlan();
    emit applyTargetChanged(selectedApplyInstanceId());
    refreshCaptureEnabled();
    refreshActions();
}

void PresetManagePage::setPlanLoading(bool loading) {
    if (m_planLoading == loading) return;
    m_planLoading = loading;
    clearPlan();
    m_previewButton->setText(loading ? tr("取消预览") : tr("预览计划"));
    m_previewButton->setToolTip(loading ? tr("取消本次预览，不会修改实例。")
        : tr("预览会改动的段落与条目，这一步不写文件。"));
    // 计划那一格还空着：整格盖加载态（和上面预设详情同一个组件），底下那行提示不再写状态字。
    if (loading) LoadingState::begin(m_planList, tr("正在读取实例并生成计划…"));
    else LoadingState::finish(m_planList);
    refreshActions();
}

void PresetManagePage::showPlan(const PlanView &plan) {
    setPlanLoading(false);
    m_planList->clear();
    m_planCanApply = plan.canApply;
    m_planNeedsConfirm = plan.needsConfirm;
    m_planReplaceConfirmed = plan.replaceConfirmed;
    m_applyArmed = false;
    m_applyToken++;

    if (!plan.ok) {
        auto *item = new QListWidgetItem(
            plan.problem.isEmpty() ? QCoreApplication::translate("PresetManagePage", "无法计算本次要执行的步骤。") : plan.problem,
            m_planList);
        item->setFlags(Qt::NoItemFlags);
        m_planHint->setText(QString());
        m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
        refreshActions();
        return;
    }

    for (const PlanLine &line : plan.rows) {
        auto *item = new QListWidgetItem(line.risky ? TextMarks::warning(line.text) : line.text,
                                         m_planList);
        item->setFlags(Qt::NoItemFlags);
    }
    for (const QString &conflict : plan.keyConflicts) {
        auto *item = new QListWidgetItem(TextMarks::warning(QCoreApplication::translate("PresetManagePage", "键位冲突：%1").arg(conflict)),
                                         m_planList);
        item->setFlags(Qt::NoItemFlags);
    }

    QString hint = plan.summary;
    if (!plan.blockedReason.isEmpty()) {
        hint = plan.blockedReason;
    } else if (plan.needsConfirm) {
        hint = QCoreApplication::translate("PresetManagePage", "预设包含覆盖操作。选择「应用」后需要再次确认。");
    } else if (!plan.canApply) {
        hint = QCoreApplication::translate("PresetManagePage", "本次没有可应用的内容。");
    }
    m_planHint->setText(hint);
    m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
    refreshActions();
}

void PresetManagePage::showApplyResult(const ApplyView &result) {
    m_planList->clear();
    m_planCanApply = false;
    m_planNeedsConfirm = false;
    m_planReplaceConfirmed = false;
    m_applyArmed = false;
    m_applyToken++;

    if (result.needsConfirm) {
        auto *item = new QListWidgetItem(QCoreApplication::translate("PresetManagePage", "尚未确认覆盖。再次选择「应用」将执行覆盖。"),
                                         m_planList);
        item->setFlags(Qt::NoItemFlags);
        m_planHint->setText(QCoreApplication::translate("PresetManagePage", "选择「应用」确认覆盖。覆盖前会创建备份。"));
        m_planCanApply = true;
        m_planNeedsConfirm = true;
        m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
        refreshActions();
        return;
    }

    if (!result.problem.isEmpty()) {
        const QString head = result.lines.isEmpty()
                                 ? result.problem
                                 : QCoreApplication::translate("PresetManagePage", "%1：%2").arg(result.problem,
                                                                result.lines.first());
        auto *item = new QListWidgetItem(head, m_planList);
        item->setFlags(Qt::NoItemFlags);
    }
    for (const QString &line : result.lines) {
        auto *item = new QListWidgetItem(line, m_planList);
        item->setFlags(Qt::NoItemFlags);
    }
    if (!result.backupDir.isEmpty()) {
        auto *item = new QListWidgetItem(QCoreApplication::translate("PresetManagePage", "备份：%1").arg(result.backupDir),
                                         m_planList);
        item->setFlags(Qt::NoItemFlags);
        item->setToolTip(QCoreApplication::translate("PresetManagePage", "还原时把这一份里的文件按原路径拷回实例目录。"));
    }
    m_planHint->setText(result.ok ? QCoreApplication::translate("PresetManagePage", "应用完成。")
                                  : QCoreApplication::translate("PresetManagePage", "应用没有全部完成，看上面哪一段没做成。"));
    m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
    refreshActions();
}

void PresetManagePage::showStatus(const QString &text) {
    m_statusLabel->setText(text);
    m_statusLabel->setVisible(!text.isEmpty());
}

void PresetManagePage::clearPlan() {
    m_planList->clear();
    m_planHint->clear();
    m_planCanApply = false;
    m_planNeedsConfirm = false;
    m_planReplaceConfirmed = false;
    m_applyArmed = false;
    m_applyToken++;
    m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
    refreshActions();
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

void PresetManagePage::showPresetLoading(const QString &id) {
    m_detailsAvailable = false;
    m_currentId = id;
    m_nameEdit->clear();
    m_descriptionEdit->clear();
    m_sectionList->clear();
    m_presetMeta->clear();  // 读取中只看正中那一个加载态，这行不再重复一遍
    LoadingState::begin(m_sectionList, tr("正在读取预设…"));
    clearPlan();
    for (const auto &name : {QStringLiteral("presetEditModsButton"), QStringLiteral("presetEditOptionsButton")})
        if (auto *button = findChild<QPushButton *>(name)) button->hide();
    refreshActions();
}

void PresetManagePage::showPresetReadError(const QString &reason) {
    m_detailsAvailable = false;
    m_presetMeta->setText(tr("无法读取预设内容"));
    // 原因和「重试」就摆在内容那一格上（重试 = 原样再选一次这一份，控制器重新读盘）。
    LoadingState::fail(m_sectionList, tr("无法读取此预设：%1").arg(reason),
                       [this] { emit presetSelected(m_currentId); });
    refreshActions();
}

void PresetManagePage::setBusy(bool busy) {
    m_busy = busy;
    refreshActions();
    refreshCaptureEnabled();
}

QString PresetManagePage::currentPresetId() const {
    const auto *item = m_presetList->currentItem();
    if (item == nullptr || itemIsBroken(item) || itemIsGroup(item)) {
        return QString();
    }
    return item->data(0, kRoleId).toString();
}

QString PresetManagePage::selectedApplyInstanceId() const {
    return comboInstanceId(m_applyInstanceCombo);
}

PresetManagePage::CaptureChoice PresetManagePage::captureChoice() const {
    CaptureChoice choice;
    choice.vanillaOptions = m_captureOptions->isChecked();
    choice.vanillaKeys = m_captureKeys->isChecked();
    choice.servers = m_captureServers->isChecked();
    choice.config = m_captureConfig->isChecked();
    choice.defaultConfigs = m_captureDefaultConfigs->isChecked();
    choice.resourcePacks = m_captureResourcePacks->isChecked();
    choice.shaderPacks = m_captureShaderPacks->isChecked();
    choice.malilib = m_captureMalilib->isChecked();
    // 「连值一起收」只在「热键」也勾着的时候才有意义（不勾这一族就一段都不收）。
    choice.malilibValues = m_captureMalilib->isChecked() && m_captureMalilibValues->isChecked();
    choice.dataPacks = m_captureDataPacks->isChecked();
    return choice;
}

void PresetManagePage::refreshActions() {
    const bool valid = selectionValid();
    const bool editable = valid && !m_busy && m_detailsAvailable;
    const bool draft = currentPresetId().startsWith(QStringLiteral("draft:"));
    for (const auto &name : {QStringLiteral("presetEditModsButton"), QStringLiteral("presetEditOptionsButton")})
        if (auto *button = findChild<QPushButton *>(name)) button->setEnabled(editable);

    m_newButton->setEnabled(!m_busy);
    m_duplicateButton->setEnabled(editable && !draft);
    m_deleteButton->setEnabled(editable);
    // 「设为默认」只对**四类**有意义（默认项现在每类各一个）。综合预设（好几类拼的）、
    // 旧版混合预设、草稿都没有"这一类"可言 —— 按钮灰着并说明原因，别让它看起来像能用。
    // 判据不用在这儿认识那四个名字：`setCategories()` 灌进来时 `filterOnly = false` 的
    // 那几项就是四类（「全部 / 旧版 / 综合预设 / 草稿」都标着 filterOnly）。
    m_defaultButton->setEnabled(editable && !draft && defaultAppliesToSelection());
    m_defaultButton->setToolTip(defaultAppliesToSelection() || !valid
                                    ? QString()
                                    : QCoreApplication::translate("PresetManagePage", "综合预设和旧版混合预设不设默认：默认只对四类预设生效。"));
    m_saveDetailsButton->setEnabled(editable);
    m_nameEdit->setEnabled(editable);
    m_descriptionEdit->setEnabled(editable);

    m_presetList->setEnabled(true);

    // 上移 / 下移：能挪的判据是"同一个组里还有前一份 / 后一份"。跨组挪没有意义 ——
    // 列表按类别分组，挪到别组去屏幕上什么都不会变。
    m_moveUpButton->setEnabled(editable && !draft && siblingPreset(-1) != nullptr);
    m_moveDownButton->setEnabled(editable && !draft && siblingPreset(1) != nullptr);

    const auto *item = m_presetList->currentItem();
    const bool isDefault = item != nullptr && item->data(0, kRoleDefault).toBool();
    m_defaultButton->setText(isDefault ? QCoreApplication::translate("PresetManagePage", "取消默认")
                                       : QCoreApplication::translate("PresetManagePage", "设为默认"));

    if (!m_deleteArmed) {
        m_deleteButton->setText(QCoreApplication::translate("PresetManagePage", "删除"));
    }

    m_previewButton->setEnabled(!draft && (m_planLoading || (editable && !comboInstanceId(m_applyInstanceCombo).isEmpty())));
    m_applyButton->setEnabled(!m_busy && m_detailsAvailable && m_planCanApply && valid);
    if (!m_applyArmed) {
        m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
    }

    refreshCaptureEnabled();
}

void PresetManagePage::refreshCaptureEnabled() {
    if (m_captureButton == nullptr) {
        return;  // 构造里第一轮 refreshActions() 可能先到（见 buildUi 末尾）
    }
    // 每一格能不能点 = 「这一类收不收它」（`m_captureMask`）×「忙不忙」。
    // 只按忙碌状态写的话，切回这一份预设之后被禁掉的格子会重新亮起来 —— 那就是
    // "不串类别"在界面上破了一个口子。
    const quint32 mask = m_captureMask;
    auto enable = [this, mask](QCheckBox *box, quint32 bit, bool extra = true) {
        if (box != nullptr) {
            box->setEnabled((mask & bit) != 0 && !m_busy && m_detailsAvailable && extra);
        }
    };
    enable(m_captureOptions, CaptureVanillaOptions);
    enable(m_captureKeys, CaptureVanillaKeys);
    enable(m_captureServers, CaptureServers);
    enable(m_captureConfig, CaptureConfig);
    enable(m_captureDefaultConfigs, CaptureDefaultConfigs);
    enable(m_captureResourcePacks, CaptureResourcePacks);
    enable(m_captureShaderPacks, CaptureShaderPacks);
    enable(m_captureMalilib, CaptureMalilib);
    enable(m_captureMalilibValues, CaptureMalilibValues, m_captureMalilib->isChecked());
    enable(m_captureDataPacks, CaptureDataPacks);
    if (auto *grid = findChild<QGridLayout *>(QStringLiteral("presetCaptureGrid"))) {
        for (int row = 0; row < grid->rowCount(); ++row) {
            bool visible = false;
            for (int col = 1; col <= 2; ++col) {
                const auto *item = grid->itemAtPosition(row, col);
                if (item && item->widget() && !item->widget()->isHidden()) visible = true;
            }
            if (auto *label = grid->itemAtPosition(row, 0); label && label->widget()) label->widget()->setVisible(visible);
        }
    }

    const bool hasPreset = !currentPresetId().isEmpty();
    const bool hasInstance = !comboInstanceId(m_captureInstanceCombo).isEmpty();
    const bool any = !captureChoice().isEmpty();
    m_captureButton->setEnabled(!m_busy && m_detailsAvailable && hasPreset && hasInstance && any);
    m_captureAsNewButton->setEnabled(!m_busy && m_detailsAvailable && hasInstance && any);
}

void PresetManagePage::armDelete() {
    m_deleteArmed = true;
    m_deleteButton->setText(QCoreApplication::translate("PresetManagePage", "再次选择以确认删除"));
    const int token = ++m_deleteToken;
    QTimer::singleShot(4000, this, [this, token] {
        if (token == m_deleteToken && m_deleteArmed) {
            disarmDelete();
        }
    });
}

void PresetManagePage::disarmDelete() {
    m_deleteArmed = false;
    m_deleteToken++;
    m_deleteButton->setText(QCoreApplication::translate("PresetManagePage", "删除"));
}

void PresetManagePage::armApply() {
    m_applyArmed = true;
    m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "再次选择以确认覆盖"));
    const int token = ++m_applyToken;
    QTimer::singleShot(4000, this, [this, token] {
        if (token == m_applyToken && m_applyArmed) {
            disarmApply();
        }
    });
}

void PresetManagePage::disarmApply() {
    m_applyArmed = false;
    m_applyToken++;
    m_applyButton->setText(QCoreApplication::translate("PresetManagePage", "应用"));
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------

void PresetManagePage::onCurrentPresetChanged(QTreeWidgetItem *current,
                                              QTreeWidgetItem *previous) {
    Q_UNUSED(previous);
    if (m_fillingList) {
        return;
    }
    refreshActions();
    // 组头不可选中（见 buildUi），所以走到这儿的一定是组里那一行；真为空 / 读不回来 /
    // 组头时按"什么都没选"处理 —— 右边那一栏跟着清空，不给拿组头当代替品的机会。
    if (current == nullptr || itemIsBroken(current) || itemIsGroup(current)) {
        m_currentId.clear();
        m_nameEdit->clear();
        m_descriptionEdit->clear();
        m_sectionList->clear();
        m_presetMeta->setText(tr("未选择预设"));
        clearPlan();
        refreshActions();
        // 细节那一层是"某一份的页"：没有可动手的那一份就退回列表（不留一张空页）。
        showDetailLayer(false);
        emit presetSelected(QString());
        return;
    }
    const QString id = current->data(0, kRoleId).toString();
    m_currentId = id;
    clearPlan();
    emit presetSelected(id);
}

bool PresetManagePage::selectionValid() const {
    const auto *item = m_presetList->currentItem();
    return item != nullptr && !itemIsBroken(item) && !itemIsGroup(item);
}

bool PresetManagePage::defaultAppliesToSelection() const {
    const auto *item = m_presetList->currentItem();
    if (item == nullptr || item->parent() == nullptr || itemIsBroken(item) || itemIsGroup(item)) {
        return false;
    }
    // 行所在的组头**存的是组名，不是钥匙**（`groupIdRole()` 那一格是 `InstanceGroupDelegate`
    // 认"这是组"用的，见 `rebuildPresetList()` 的 `makeGroup()`）—— 所以按 `m_groupText`
    // 反过来对：哪一项组名跟它一样、又不是筛选专用的，就是它的类别。
    // "是不是四类"这一层**不认识**，只认灌进来的选项：`filterOnly = false` 的那几项才是
    // 能新建的四类（「全部 / 旧版 / 综合预设 / 草稿」都标着 `filterOnly`）。
    const QString groupText = item->parent()->data(0, MainWindowUI::groupIdRole()).toString();
    for (const CategoryOption &option : m_categoryOptions) {
        if (option.filterOnly || option.key.isEmpty()) {
            continue;
        }
        if (m_groupText.value(option.key) == groupText) {
            return true;
        }
    }
    return false;
}

QTreeWidgetItem *PresetManagePage::siblingPreset(int delta) const {
    const auto *item = m_presetList->currentItem();
    if (item == nullptr || item->parent() == nullptr) return nullptr;
    auto *parent = item->parent();
    for (int i = parent->indexOfChild(const_cast<QTreeWidgetItem *>(item)) + delta;
         i >= 0 && i < parent->childCount(); i += delta) {
        auto *candidate = parent->child(i);
        if (!candidate->isHidden() && !itemIsBroken(candidate)) return candidate;
    }
    return nullptr;
}

QString PresetManagePage::comboInstanceId(const QComboBox *combo) {
    if (combo == nullptr || combo->currentIndex() < 0) {
        return QString();
    }
    return combo->currentData().toString();
}
