// Shared layout primitives are compiled once; page modules do not duplicate them.
#include "MainWindowUI.hpp"

#include "Animations.hpp"  // 换页 / 展开折叠 / 上拉面板那几段动效
#include "ElidedLabel.hpp"
#include "InstanceGroupDelegate.hpp"
#include "MemorySlider.hpp"
#include "OverlayHost.hpp"
#include "PopupPlacement.hpp"
#include "SkinModel.hpp"
#include "TaskProgressBar.hpp"
#include "Theme.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "TitleBar.hpp"
#include "VersionIcons.hpp"
#include "VersionTreeDelegate.hpp"
#include "WindowChrome.hpp"

#include <QCheckBox>
#include <QColor>
#include <QEvent>
#include <QFont>
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
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "MainWindowPageLayout.hpp"

namespace MainWindowPageLayout {
QLabel *makeHomeTitle(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    font = Theme::textFont(font, Theme::TextRole::Heading);
    label->setFont(font);
    return label;
}

QLabel *makeHomeHint(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}
// Always reserve the scrollbar gutter: expanding a category must not move the
// right-aligned arrow/clear targets or change the width available to row text.
void stabilizeDownloadTree(QTreeWidget *tree) {
    tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto *bar = tree->verticalScrollBar();
    const auto syncIdle = [bar] {
        const bool idle = bar->maximum() <= bar->minimum();
        if (bar->property("wtfmlIdle").isValid() && bar->property("wtfmlIdle").toBool() == idle) return;
        bar->setProperty("wtfmlIdle", idle);
        bar->style()->unpolish(bar);
        bar->style()->polish(bar);
        bar->update();
    };
    QObject::connect(bar, &QScrollBar::rangeChanged, tree, syncIdle);
    syncIdle();
}

/// 给 @p widget 挂一个"面"（`wtfmlSurface`）。名字与值都从 Theme 那一组**唯一出处**来，
/// 这里只是把 `const char *` 那一下收成一行（属性名要 `QByteArray` 活得比这一次调用长）。
void markSurface(QWidget *widget, const QString &value) {
    const QByteArray name = Theme::surfacePropertyName().toUtf8();
    widget->setProperty(name.constData(), value);
}

/// 给 @p label 挂一个"文字角色"（`wtfmlTextRole`）。同上。
void markTextRole(QWidget *label, const QString &value) {
    const QByteArray name = Theme::textRolePropertyName().toUtf8();
    label->setProperty(name.constData(), value);
}

void addSearchIcon(QLineEdit *edit) {
    QAction *glass = edit->addAction(QIcon(), QLineEdit::LeadingPosition);
    const auto tint = [glass] {
        glass->setIcon(VersionIcons::line(QStringLiteral("search"), 16, Theme::tokens().secondary));
    };
    tint();
    Theme::follow(edit, tint);
}

// 侧栏列表与容器共用一层底色；代理绘制选中/悬停时也拿相同的 Base，避免白色内框。
void styleSidebarList(QListWidget *list) {
    list->setProperty("wtfmlSidebarRail", true);
    list->setFrameShape(QFrame::NoFrame);
    const auto apply = [list] {
        QPalette palette = QApplication::palette();
        palette.setColor(QPalette::Base, Theme::tokens().main);
        palette.setColor(QPalette::Window, Theme::tokens().main);
        list->setPalette(palette);
        list->viewport()->setPalette(palette);
        list->setAutoFillBackground(false);
        list->viewport()->setAutoFillBackground(false);
        list->viewport()->update();
    };
    apply();
    Theme::follow(list, apply);
}

/**
 * @brief 把页面搭成那套统一骨架（`PageScaffold`）。
 * @param page 选项卡页面。
 * @param tab 页面代号（"home" / "instance" / "download" / "settings"），进 objectName，
 * **也决定份数**。
 * @return 两栏的控件与布局；调用方往 `layout` 里塞东西即可。
 *
 * 【这套骨架长什么样】（那张 ASCII）
 *
 * ┌──────────────────────────────────────────────────┐
 * │ 侧栏 216px │ 页面标题 操作 │
 * │ │ 页面说明 / 路径 / 当前状态 │
 * │ 导航或列表 ├───────────────────────────────────┤
 * │ │ 主内容区（一个"正文面"） │
 * └──────────────────────────────────────────────────┘
 *
 * 四条规则落在代码里就是：
 * ① **页面本身不画边框**：整页只有两个面（侧栏 + 正文），别的层一律透明；
 * ② **侧栏只用一个轻微底色**（`wtfmlSurface="sidebar"`）区分，不画矩形边框；
 * ③ **正文只有一个面**（`wtfmlSurface="workspace"`），小节走"标题 + 小节卡片"
 * （`wtfmlSurface="section"`）；
 * ④ **页面级操作永远在右上角**：塞进 `columns.right.actions`（页头那一行的最右），
 * 确认操作由调用方摆在自己的"底栏"上（`wtfmlSurface="footer"`）。
 *
 * 【哪些页走哪一档】主页那一栏是"选实例 + 启动"、那是**正文**，所以它不套侧栏面；
 * 它的右栏（「我的角色」）是**定宽**的一栏（`homeRightColumnWidth()`），
 * 左栏吃剩下的全部（份额见头文件里 `homeLeftColumnStretch()` 那段）。另外三页的左栏是
 * **导航**（一列分类 / 一列文件夹），走固定宽度的侧栏面。
 *
 * 【`layout` 落在哪儿】正文那一栏：`widget`（透明外框）→ 页头 → `surface`（正文面）→
 * `layout`。也就是说调用方 `columns.right.layout->addWidget(...)` 塞进去的东西**都在
 * 页头下面、正文面里面** —— 这正是"主内容区"该在的位置。侧栏那一栏没有页头，
 * `surface == widget`、`layout` 就是它自己的布局。
 *
 * 【只带名字的栏才是验证工程要的】栏做成真控件而不是裸布局，是为了验证工程能按
 * objectName 找回来量几何；页头里那两行字与操作那一条也各有名字（见
 * `pageHeaderName()` / `pageTitleName()` / `pageSubtitleName()` / `pageActionsName()`）。
 */
TwoColumns buildTwoColumnPage(QWidget *page, const QString &tab) {
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(kPageMargin, kPageMargin, kPageMargin, kPageMargin);
    row->setSpacing(kColumnSpacing);

    const bool home = tab == QLatin1String("home");

    // 栏本身是个控件（不是裸布局），为的是验证工程能按 objectName 找回来量几何。
    // 自定义背景图铺在**整页那一层**（`Theme::Part::PageBackground`，见 MainWindowUI.cpp），
    // 栏自己不画任何东西 —— 看到的都是底下那一层（页面 / 选项卡自己画的底）。
    const auto makeColumn = [page](const QString &name) {
        Column column;
        column.widget = new QWidget(page);
        column.widget->setObjectName(name);
        column.layout = new QVBoxLayout(column.widget);
        // 栏本身不留边距：页面那一层已经留过 kPageMargin 了，再留一层就是双层边距
        column.layout->setContentsMargins(0, 0, 0, 0);
        column.layout->setSpacing(10);
        return column;
    };

    TwoColumns columns;
    columns.left = makeColumn(MainWindowUI::leftColumnName(tab));
    columns.right = makeColumn(MainWindowUI::rightColumnName(tab));
    if (home) {
        // 右栏**钉死**在人物预览那一栏的宽度上（唯一出处见头文件
        // `homeRightColumnWidth()`）—— 主页不按份数分宽，左栏吃剩下的全部。
        columns.right.widget->setFixedWidth(MainWindowUI::homeRightColumnWidth());
        columns.right.widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    } else {
        // ---- 侧栏那一栏：固定宽度 + 一个轻微底色 ----
        //
        // 固定宽度取代了"按份数分宽"（见 kSidebarWidth 那段）。
        // 面是 QFrame 而不是裸 QWidget：样式表给的底色只对 QFrame 这一族生效
        // （裸 QWidget 得自己画）。
        auto *sidebar = new QFrame(columns.left.widget);
        const bool rail = tab == QLatin1String("instance") || tab == QLatin1String("download")
                          || tab == QLatin1String("settings");
        sidebar->setObjectName(tab + QStringLiteral("SidebarSurface"));
        sidebar->setProperty("wtfmlSidebarRail", rail);
        sidebar->setFrameShape(QFrame::NoFrame);
        markSurface(sidebar, Theme::sidebarSurfaceValue());
        auto *sidebarLayout = new QVBoxLayout(sidebar);
        const int padding = rail ? 12 : kSurfacePadding;
        sidebarLayout->setContentsMargins(padding, padding, padding, padding);
        sidebarLayout->setSpacing(10);
        columns.left.surface = sidebar;
        // 侧栏里那些东西落在**面**里（侧栏没有页头，面就是这一栏的全部）
        columns.left.layout->addWidget(sidebar, 1);
        columns.left.layout = sidebarLayout;

        // ---- 正文那一栏：页头 + 一个"正文面" ----
        //
        // 页头**在面外面**（面里只有内容）：标题和说明是"这一页是什么"，内容面是
        // "这一页有什么" —— 把标题装进面里的话，那一圈边就同时框住了标题和内容，
        // 又回到了"框套框"的观感（"减少框"第一条要减掉的就是它）。
        auto *header = new QWidget(columns.right.widget);
        header->setObjectName(MainWindowUI::pageHeaderName(tab));
        auto *headerLayout = new QVBoxLayout(header);
        // 左缘 / 上沿取一级页统一落点（页面外边距已在外层留过，这里只补差）
        headerLayout->setContentsMargins(Theme::pageTitleInsetLeft() - kPageMargin, Theme::pageTitleInsetTop() - kPageMargin,
                                         kSurfacePadding, 0);
        headerLayout->setSpacing(kHeaderSpacing);

        auto *titleRow = new QHBoxLayout;
        titleRow->setSpacing(kColumnSpacing);
        columns.right.title = new QLabel(header);
        columns.right.title->setObjectName(MainWindowUI::pageTitleName(tab));
        markTextRole(columns.right.title, Theme::pageTitleTextRoleValue());
        {   // 页标题：手写体 Light 26（tokens-r2 --fs-title），四处手写体之一。
            QFont titleFont = columns.right.title->font();
            titleFont.setBold(false);
            Theme::markDisplay(columns.right.title, Theme::textFont(titleFont, Theme::TextRole::Display));
        }
        titleRow->addWidget(columns.right.title);
        titleRow->addStretch(1);
        columns.right.actions = new QHBoxLayout;
        columns.right.actions->setSpacing(kFilterRowSpacing);
        titleRow->addLayout(columns.right.actions);
        headerLayout->addLayout(titleRow);

        columns.right.subtitle = new QLabel(header);
        columns.right.subtitle->setObjectName(MainWindowUI::pageSubtitleName(tab));
        markTextRole(columns.right.subtitle, Theme::secondaryTextRoleValue());
        columns.right.subtitle->setWordWrap(true);
        // 和主页那行说明同一条规矩：纯说明文字**不许**参与横向度量（折行 QLabel 报出来的
        // sizeHint 是"整句排成一行"的宽度，让它决定这一栏多宽是本末倒置）。
        columns.right.subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        columns.right.subtitle->setVisible(false);
        headerLayout->addWidget(columns.right.subtitle);

        auto *workspace = new QFrame(columns.right.widget);
        markSurface(workspace, Theme::workspaceSurfaceValue());
        auto *workspaceLayout = new QVBoxLayout(workspace);
        workspaceLayout->setContentsMargins(kSurfacePadding, kSurfacePadding, kSurfacePadding,
                                            kSurfacePadding);
        workspaceLayout->setSpacing(10);
        columns.right.surface = workspace;

        columns.right.header = header;
        columns.right.layout->addWidget(header);
        columns.right.layout->addWidget(workspace, 1);
        columns.right.layout = workspaceLayout;
    }
    // 主页那一栏左宽右窄，靠份数（见上面那段）；另几页的侧栏是**固定宽度**，
    // 份数给它 0（它自己那一位由 `setFixedWidth()` 定死），多出来的宽度全归正文那一栏。
    // NOTE: 这两句直接读 `leftColumnStretch(tab)`/`rightColumnStretch(tab)` —— 那一对
    // 函数**自己**就编码了"只有主页按份数、另三页是 0:1"（见头文件里那段），
    // 所以这里没有三元表达式，改份数还是只有那一处。
    row->addWidget(columns.left.widget, MainWindowUI::leftColumnStretch(tab));
    row->addWidget(columns.right.widget, MainWindowUI::rightColumnStretch(tab));
    if (!home) {
        columns.left.widget->setFixedWidth(kSidebarWidth);
        // 尺寸策略也要一起钉：`setFixedWidth()` 只写死了 min/max，策略留在
        // Preferred —— 屏幕上看着一样，但"这一栏跟不跟窗口长"这件事的判据是策略
        // （按份数分宽那一版是 Preferred/Expanding，两者只在策略上分得开）。ui_check 那条
        // "左栏的横向尺寸策略是 Fixed"量的是这一句；少它的话，将来谁把 setFixedWidth 挪走
        // 换成 setMinimumWidth，屏幕上要到某个窗口宽度才看得出走样。
        columns.left.widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }
    return columns;
}

/// 给正文那一栏的页头填标题与说明（`subtitle` 传空串就整行收起来，连位置都不占）。
void fillPageHeader(Column &column, const QString &title, const QString &subtitle) {
    if (column.title != nullptr) {
        column.title->setText(title);
    }
    if (column.subtitle != nullptr) {
        column.subtitle->setText(subtitle);
        column.subtitle->setVisible(false); // Root navigation already identifies the page.
    }
}

/// 起一节（标题 + 一张空的内容卡片），并在 @p row 那一行把它摆进 @p grid。
/// @param row 按引用收：卡片占掉一行，函数自己推进 —— 忘不掉了。
SettingsSection addSettingsSection(QGridLayout *grid, int &row, const QString &title,
                                   QWidget *parent) {
    SettingsSection section;
    section.title = title;
    section.card = new QFrame(parent);
    // 面挂在**属性**上（不是 objectName）：一页里这样的卡片有好几张，名字只能靠它认
    // （产品、样式表、验证工程三方共用这一对字符串，见 Theme::surfacePropertyName()）。
    markSurface(section.card, Theme::sectionSurfaceValue());
    section.card->setProperty("settingsSectionTitle", title);
    auto *layout = new QVBoxLayout(section.card);
    // 内边距 **16 四面**、行距 **10**。
    // 两个数都落进令牌那三档（区块间距 16 / 控件间距 10）：卡片自己那一圈属于"区块"，
    // 卡里"行名 → 控件"那种成对的间距属于"控件"。这一档统一走令牌的三档节奏
    // （两边都是"松紧"，只有断言能钉住 —— ui_check 的 K2 量的是这两个数）。
    // 扁平（7a）：小节不再是一张卡片，只是一个手写体小标题 + 下面一列「标签 | 控件」，
    // 节与节之间靠留白（grid 的行距）分开。
    layout->setContentsMargins(0, 4, 0, 12);
    layout->setSpacing(12);
    auto *titleLabel = new QLabel(title, section.card);
    markTextRole(titleLabel, Theme::sectionTitleTextRoleValue());
    titleLabel->setFont(Theme::textFont(titleLabel->font(), Theme::TextRole::Heading));
    layout->addWidget(titleLabel);
    grid->addWidget(section.card, row, 0, 1, 2);
    ++row;
    section.layout = layout;
    return section;
}

namespace {
/// 设置行（设计稿 .set）：底下一条 1px 极淡分隔（divider 的四成，只提示换行、不画成格子），
/// 最后一行由 section 那一格的留白收住。
class SettingRow : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        QColor line = Theme::tokens().divider;
        line.setAlphaF(line.alphaF() * 0.4);
        painter.fillRect(QRect(0, height() - 1, width(), 1), line);
    }
};
}  // namespace

/// 往一节里加一项（设计稿 .set）：左边「名字 14 + 说明 12.5」，右边控件靠右；行底一条分隔线。
/// 控件容器里挂了 `settingsHelp` 的静态说明搬到左边名字下面；会变的那几句（内存、报错）
/// 留在控件底下。复选框自己带名字（@p labelText 等于小节名）⇒ 整个控件摆在左边。
void addSettingItem(const SettingsSection &section, const QString &labelText, QWidget *control,
                    QWidget *parent) {
    auto *row = new SettingRow(parent);
    row->setProperty("settingsItemRow", true);
    row->setMinimumHeight(60);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 10, 0, 10);
    rowLayout->setSpacing(24);
    control->setParent(row);
    QString name = labelText != section.title ? labelText : QString();
    // 一行只有一个勾选框 = 开关行：勾选框上那句话挪到左格当行名，它自己变成右侧 40×22 开关。
    if (name.isEmpty()) {
        if (auto *check = control->findChild<QCheckBox *>(QString(), Qt::FindDirectChildrenOnly)) {
            name = check->text();
            Theme::markSwitch(check);
        }
    }
    if (name.isEmpty()) {
        rowLayout->addWidget(control, 1);
        section.layout->addWidget(row);
        return;
    }
    auto *left = new QWidget(row);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(3);
    QLabel *label = new QLabel(name, left);
    label->setProperty("settingsItemLabel", true);
    label->setWordWrap(true);
    {
        QFont font = label->font();
        font.setPixelSize(14);
        font.setWeight(QFont::Medium);
        label->setFont(font);
    }
    leftLayout->addWidget(label);
    if (QLayout *box = control->layout()) {
        for (int i = 0; i < box->count();) {
            auto *help = qobject_cast<QLabel *>(box->itemAt(i)->widget());
            if (help != nullptr && help->property("settingsHelp").toBool()) {
                box->removeWidget(help);
                help->setParent(left);
                QFont font = help->font();
                font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
                help->setFont(font);
                leftLayout->addWidget(help);
                continue;
            }
            if (QWidget *widget = box->itemAt(i)->widget()) {
                box->setAlignment(widget, Qt::AlignRight | Qt::AlignVCenter);
                if (qobject_cast<QLineEdit *>(widget) != nullptr) widget->setMinimumWidth(260);
                if (auto *hint = qobject_cast<QLabel *>(widget)) hint->setAlignment(Qt::AlignRight);
            }
            ++i;
        }
    }
    left->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rowLayout->addWidget(left, 1, Qt::AlignVCenter);
    rowLayout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    section.layout->addWidget(row);
}

/// 给设置页里的一行控件套一层"控件 + 说明文字"的容器（说明统一小一号、灰色）。
/// @return 容器；调用方再往它的 layout 里塞真控件 —— 说明文字在**最后**加，
/// 所以这里只把空布局还回去。
QWidget *makeHintedControl(QWidget *parent) {
    QWidget *box = new QWidget(parent);
    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    return box;
}

HintedControlWithLayout makeHintedControlWithLayout(QWidget *parent) {
    QWidget *widget = makeHintedControl(parent);
    return {widget, qobject_cast<QVBoxLayout *>(widget->layout())};
}

InlineRow makeInlineRow(QWidget *parent) {
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    return {widget, layout};
}

/// 往 makeHintedControl() 造出来的容器里加一句说明，并把标签还回去。
/// 文字会随情况变的那几句用它（比如「内存分配」下面那句要说系统现在有多少内存）。
QLabel *addDynamicHint(QWidget *box, const QString &text, QWidget *parent) {
    QLabel *hint = new QLabel(text, parent);
    hint->setWordWrap(true);
    QFont hintFont = hint->font();
    hintFont = Theme::textFont(hintFont, Theme::TextRole::Caption);
    hint->setFont(hintFont);
    // 使用动态文字角色，不能把创建时的浅色主题文字固化在局部调色板中。
    markTextRole(hint, Theme::secondaryTextRoleValue());
    qobject_cast<QVBoxLayout *>(box->layout())->addWidget(hint);
    return hint;
}

/// 和 addDynamicHint() 同一个形状，但字是**红的** —— 名字框底下那行"名字不能用"的原因
/// 用的就是它（大小、换行按说明那一档）。
QLabel *addDynamicErrorHint(QWidget *box, QWidget *parent) {
    QLabel *hint = addDynamicHint(box, QString(), parent);
    markTextRole(hint, QString());
    const auto refreshError = [hint] {
        QPalette palette = qApp->palette();
        palette.setColor(QPalette::WindowText, MainWindowUI::installNameErrorColor(palette));
        hint->setPalette(palette);
    };
    refreshError();
    Theme::follow(hint, refreshError);
    return hint;
}

/// 往 makeHintedControl() 造出来的容器里加一句说明。
void addHint(QWidget *box, const QString &text, QWidget *parent) {
    addDynamicHint(box, text, parent);
}

/// 下载页页头该不该露着 —— **唯一出处**。
///
/// 安装页的顶上大标题和刷新键**点进去就隐藏、外面还保留**。「那页」是「下载 → 游戏」
/// 里面第二层那页**安装页**（挑加载器、填名字、按安装）；外面（版本树那一层、以及别的分类）
/// 页头照旧露着。
///
/// 「服务端」那一栏**也是两层**（版本列表 / 安装表单），和「游戏」一模一样 —— 用户要的
/// 就是"服务端下载和游戏下载一个样"。表单那一层同样把页头整块收起来。
///
/// NOTE: 只接收传进来的指针、**不捕获 MainWindowUI 自己** —— 它不是一个 QObject
/// （见类声明），而这几发 `currentChanged` 的 context 是那三个堆叠页本身。
/// 判据读的是堆叠页**此刻**的下标，所以"先翻分类、后翻内层"哪一发先到、
/// 或者中间多来一发，结果都一样。
void syncDownloadHeaderVisibility(QWidget *header, QStackedWidget *downloadStack,
                                  QStackedWidget *gameStack, QStackedWidget *serverStack,
                                  QWidget *categoryChips) {
    if (header == nullptr || downloadStack == nullptr || gameStack == nullptr) {
        return;
    }
    const bool onInstallPage =
        (downloadStack->currentIndex() == MainWindowUI::downloadCategoryGameIndex() &&
         gameStack->currentIndex() == MainWindowUI::downloadGameInstallIndex()) ||
        (serverStack != nullptr &&
         downloadStack->currentIndex() == MainWindowUI::downloadCategoryServerIndex() &&
         serverStack->currentIndex() == MainWindowUI::downloadServerFormIndex());
    header->setVisible(!onInstallPage);
    if (categoryChips != nullptr) {
        const int count = MainWindowUI::downloadCategoryTexts().size();
        for (int i = 0; i < count; ++i) {
            if (auto *chip = categoryChips->findChild<QPushButton *>(
                    MainWindowUI::downloadCategoryChipName(i))) {
                const bool keepOnInstall =
                    i == MainWindowUI::downloadCategoryGameIndex() ||
                    i == MainWindowUI::downloadCategoryServerIndex();
                chip->setVisible(!onInstallPage || keepOnInstall);
            }
        }
    }
}
}

// ---------------------------------------------------------------------------
namespace MainWindowPageLayout {

// 小节卡片 / 小节 / 正文滚动层（三样各只此一处）
// ---------------------------------------------------------------------------

QFrame *makeSectionCard(const QString &title, QWidget *parent, QVBoxLayout **layoutOut) {
    auto *card = new QFrame(parent);
    markSurface(card, Theme::sectionSurfaceValue());
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(12);
    auto *heading = new QLabel(title, card);
    markTextRole(heading, Theme::sectionTitleTextRoleValue());
    QFont font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    if (layoutOut != nullptr) {
        *layoutOut = layout;
    }
    return card;
}

QWidget *makePageSection(const QString &title, QWidget *parent, QVBoxLayout **layoutOut) {
    auto *section = new QWidget(parent);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto *heading = new QLabel(title, section);
    markTextRole(heading, Theme::sectionTitleTextRoleValue());
    QFont font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    if (layoutOut != nullptr) {
        *layoutOut = layout;
    }
    return section;
}

QWidget *makeSettingRow(QWidget *parent, QWidget *title, const QString &desc, QWidget *control, bool below) {
    auto *row = new QFrame(parent);
    row->setObjectName(QStringLiteral("wtfmlSettingRow"));
    const auto paintRow = [row] {  // 行底分隔压到 fill-press 的四成：只提示换行，不画成格子
        QColor line = Theme::tokens().fillPress;
        line.setAlphaF(line.alphaF() * 0.4);
        row->setStyleSheet(QStringLiteral("QFrame#wtfmlSettingRow { border: none; border-bottom: 1px solid %1; }")
                               .arg(line.name(QColor::HexArgb)));
    };
    paintRow();
    Theme::follow(row, paintRow);
    auto *line = new QHBoxLayout(row);
    line->setContentsMargins(0, 12, 0, 12);
    line->setSpacing(16);
    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    if (title != nullptr) {
        title->setParent(row);
        text->addWidget(title, 0, Qt::AlignLeft);
    }
    if (!desc.isEmpty()) {
        auto *hint = new QLabel(desc, row);
        hint->setObjectName(QStringLiteral("wtfmlSettingRowDesc"));
        hint->setWordWrap(true);
        hint->setMaximumWidth(560);
        hint->setFont(Theme::textFont(hint->font(), Theme::TextRole::Caption));
        const auto tone = [hint] {
            QPalette pal = hint->palette();
            pal.setColor(QPalette::WindowText, Theme::tokens().secondary);
            hint->setPalette(pal);
        };
        tone();
        Theme::follow(hint, tone);
        // 带 AlignLeft 的换行标签只拿 sizeHint 那点宽、高度不跟折行长，会被压成一条缝；直接放进竖排又会
        // 把整列的最大宽压成 560、被摆到中间。⇒ 横排里 + 一截弹簧：限宽、靠左、按宽折行算高。
        auto *hintRow = new QHBoxLayout;
        hintRow->setContentsMargins(0, 0, 0, 0);
        hintRow->addWidget(hint, 1);  // 先把字长到 560，余下的给弹簧
        hintRow->addStretch(0);
        text->addLayout(hintRow);
    }
    line->addLayout(text, 1);
    if (control != nullptr && below) {
        control->setParent(row);
        text->addSpacing(6);
        text->addWidget(control);
    } else if (control != nullptr) {
        control->setParent(row);
        line->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    return row;
}

QLabel *makeSettingRowTitle(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    QFont f = Theme::textFont(label->font(), Theme::TextRole::Body);
    f.setBold(true);
    label->setFont(f);
    return label;
}

QScrollArea *makePageScroll(QWidget *parent, int bottomMargin, QVBoxLayout **bodyOut) {
    auto *scroll = new Theme::GutterScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, bottomMargin);
    scroll->setWidget(body);
    if (bodyOut != nullptr) {
        *bodyOut = bodyLayout;
    }
    return scroll;
}

}  // namespace MainWindowPageLayout
