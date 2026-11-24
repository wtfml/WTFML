// Download page construction, filtering and header visibility.
#include "MainWindowUI.hpp"
#include "InputSizing.hpp"

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
#include "ThemeBackground.hpp"
#include "TitleBar.hpp"
#include "VersionIcons.hpp"
#include "VersionTreeDelegate.hpp"
#include "WindowChrome.hpp"
#include "RowActivation.hpp"
#include <QPointer>
#include <QScrollBar>

#include <QButtonGroup>
#include <QMenu>
#include <QColor>
#include <QCoreApplication>
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
using namespace MainWindowPageLayout;

QWidget *MainWindowUI::setupDownloadPage() {
    // ---------- 下载：左 = 分类入口（仿 PCL）；右 = 那个分类的内容 ----------
    //
    // 左栏是 PCL 那种**一列分类**：游戏 / 整合包 / 模组 / 数据包 / 资源包 / 光影
    // （「服务端」见头文件那张表）。点哪个，右栏那份 QStackedWidget 就换到哪一页。
    //
    // NOTE: 后面几页都是真的：「游戏」那一页挑加载器、挑版本、下载，一个控件都不少；
    // 其余几页是"一个搜索框 + 一棵三层树"（资源 → 游戏版本 → 版本号），在这里一段
    // 循环里搭出来（见下面那段）。（「数据包」见头文件那张表那段。）
    QWidget *downloadTab = new ThemeBackground(Theme::Part::PageBackground, tabWidget);
    downloadTab->setObjectName(downloadTabName());
    // NOTE: 这一份**不是 const**（另两页那两份是）：页头那两处是搭完之后才由这一页填的
    // （`fillPageHeader()` 收的是 `Column&`）。
    TwoColumns download = buildTwoColumnPage(downloadTab, QStringLiteral("download"));
    {
        const QStringList categoryKeys = downloadCategoryKeys();
        const QStringList categoryTexts = downloadCategoryTexts();

        // ---- 页头：标题「下载」+ 说明（当前停在哪一类、那一类能干什么）+ 右上角那颗刷新 ----
        fillPageHeader(download.right, downloadPageTitle(),
                       downloadHeaderSubtitle(downloadCategoryGameIndex()));
        downloadHeaderSubtitleLabel = download.right.subtitle;
        // 页头工具钮：和左边「下载源」同高同底的圆钮（字进悬停提示），两颗并排对得齐。
        downloadHeaderRefreshButton = new QPushButton(download.right.header);
        downloadHeaderRefreshButton->setObjectName(downloadHeaderRefreshButtonName());
        Theme::markRefreshButton(downloadHeaderRefreshButton);  // F5 认它
        Theme::markButton(downloadHeaderRefreshButton, Theme::ButtonKind::Ghost);
        Theme::markIconButton(downloadHeaderRefreshButton);
        downloadHeaderRefreshButton->setFixedSize(28, 28);
        downloadHeaderRefreshButton->setProperty("wtfmlComboTrigger", true);
        downloadHeaderRefreshButton->setProperty("wtfmlIconOnly", true);
        downloadHeaderRefreshButton->setToolTip(downloadHeaderRefreshText());
        downloadHeaderRefreshButton->setAccessibleName(downloadHeaderRefreshText());
        downloadHeaderRefreshButton->setCursor(Qt::PointingHandCursor);
        {
            const auto tint = [b = downloadHeaderRefreshButton] {
                b->setIcon(VersionIcons::line(QStringLiteral("refresh"), 18, Theme::tokens().secondary));
            };
            tint();
            Theme::follow(downloadHeaderRefreshButton, tint);
        }
        {
            // 页头右边「下载源 · 优先官方源」：一颗胶囊触发器（`wtfmlComboTrigger`），
            // **不带 ∨**（8 号反馈：这种右边的小箭头不要再出现）—— 整块可点、悬停加深。
            // 选项不另存一份：点开时现读设置页那只下拉框，选中就改它（持久化、生效都走它原来那条路）。
            auto *source = new QPushButton(download.right.header);
            source->setObjectName(downloadSourceButtonName());
            source->setProperty("wtfmlComboTrigger", true);
            Theme::markButton(source, Theme::ButtonKind::Ghost);
            source->setCursor(Qt::PointingHandCursor);
            const auto sync = [this, source] {
                const QString name = downloadSourceCombo != nullptr ? downloadSourceCombo->currentText() : QString();
                source->setText(name.isEmpty() ? QCoreApplication::translate("MainWindowUI", "下载源")
                                               : QCoreApplication::translate("MainWindowUI", "下载源 · %1").arg(name));
                source->setToolTip(QCoreApplication::translate("MainWindowUI", "优先源速度缓慢时，自动改用另一个源。"));
            };
            sync();
            QObject::connect(source, &QPushButton::clicked, source, [this, source, sync] {
                if (downloadSourceCombo == nullptr || downloadSourceCombo->count() == 0) return;
                QMenu menu(source);
                for (int i = 0; i < downloadSourceCombo->count(); ++i) {
                    QAction *action = menu.addAction(downloadSourceCombo->itemText(i));
                    action->setCheckable(true);
                    action->setChecked(i == downloadSourceCombo->currentIndex());
                    action->setData(i);
                }
                if (QAction *chosen = menu.exec(source->mapToGlobal(QPoint(0, source->height())))) {
                    downloadSourceCombo->setCurrentIndex(chosen->data().toInt());
                }
                sync();
            });
            // 设置页那只下拉框读配置时挡了信号，这里换到下载页时再对一次字。
            QObject::connect(tabWidget, &QTabWidget::currentChanged, source, [sync](int) { sync(); });
            download.right.actions->addWidget(source);
        }
        download.right.actions->addWidget(downloadHeaderRefreshButton);
        // 页头那一整块（进安装页时连标题带按钮一起收起来，见 getDownloadHeader() 那段）。
        downloadHeader = download.right.header;

        downloadCategoryList = new QListWidget(download.left.surface);
        downloadCategoryList->setObjectName(downloadCategoryListName());
        downloadCategoryList->setUniformItemSizes(true);
        downloadCategoryList->setFrameShape(QFrame::NoFrame);
        styleSidebarList(downloadCategoryList);
        downloadCategoryList->setItemDelegate(
            new VersionTreeDelegate(downloadCategoryList, /*leafArrow=*/true,
                                    /*iconSize=*/0, /*navRow=*/true));
        // 这一列那五个刷新按钮**只在鼠标悬停到那一行时才显示**。代理读的是视图填的
        // QStyle::State_MouseOver，而**视图默认不填**：干净的 QListWidget 视口
        // WA_Hover=0 / mouseTracking=0，真鼠标移动一个字都不改；
        // 开了下面这一句之后，鼠标真挪过去才会把"光标底下是哪一行"交给代理（windows 与
        // offscreen 两个平台结果一样）。
        // NOTE: 只开给这一份列表：那一格（rowRefreshRole()）全仓只有这里有，版本树、加载器树、
        // 资源表上都没有，开了只是让它们多收几条鼠标移动事件。
        // 这一句和 VersionTreeDelegate::paint() 里那段 showRefresh 是一对 —— 少哪一句
        // 都是"刷新按钮再也不出现"，两边各自的断言都还是绿的。
        downloadCategoryList->setMouseTracking(true);
        QFont categoryFont = downloadCategoryList->font();
        categoryFont = Theme::textFont(categoryFont, Theme::TextRole::Body);
        downloadCategoryList->setFont(categoryFont);
        for (int i = 0; i < categoryKeys.size(); ++i) {
            auto *item = new QListWidgetItem(categoryTexts.value(i), downloadCategoryList);
            // 身份进 userData：条目文字是显示内容，会随产品文案走，不能拿它当身份。
            item->setData(Qt::UserRole, categoryKeys.value(i));
            // 每个条目右边都有那个刷新按钮（一条不落）。点它的那一下由
            // MainWindow::eventFilter 接 —— 判据是 VersionTreeDelegate::refreshRect()。
            // NOTE: 这里是**两参数**那个 setData（QListWidgetItem 认的是 role，没有列）——
            // 三参数那一个是 QTreeWidgetItem 的，写在这儿编不过。
            {
                item->setData(rowRefreshRole(), true);
            }
        }
        // 高度贴着内容：几个两三个字的条目，被拉成一整条空白看着像坏了。行高**交给样式
        // 算**（sizeHintForRow），别写死数字 —— 换字体/换样式就错（UI 样式基线那件事）。
        int rowsHeight = 0;
        for (int i = 0; i < downloadCategoryList->count(); ++i) {
            rowsHeight += downloadCategoryList->sizeHintForRow(i);
        }
        downloadCategoryList->setFixedHeight(rowsHeight +
                                             2 * downloadCategoryList->frameWidth() + 2);
        // 「← 返回实例设置」那一颗。
        //
        // 返回按钮放在左上角（就是左边栏的顶上）。它由
        // **六栏共用一颗**：
        // ① 「回到刚才那个实例」与用户此刻翻到哪一栏**无关**，本来就不该一栏一颗；
        // ② 它在左栏顶上，翻栏时**不动**，不会跟着右栏一起刷没了。
        // **默认收着**（连位置都不占）：只有这一次是从某个实例的设置页点进来的才露面。
        downloadBackButton = new QPushButton(downloadBackButtonText(), download.left.surface);
        downloadBackButton->setObjectName(downloadBackButtonName());
        downloadBackButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        downloadBackButton->setToolTip(downloadBackTooltip());
        // 「← 返回」= 往回走（一般性）。
        Theme::markButton(downloadBackButton, Theme::ButtonKind::Neutral);
        downloadBackButton->setVisible(false);
        download.left.layout->addWidget(downloadBackButton);

        download.left.layout->addWidget(downloadCategoryList);
        download.left.layout->addStretch(1);

        // ---- UI 7b（设计稿 download-*）：分类改成页头下面一排 chip ----
        // 左栏那列分类**保留但收起**（身份、刷新角色、MainWindow 那边按列表行翻页的数据流都不动），
        // chip 只是它的另一张脸：点 chip = 设列表当前行；列表当前行变了 = 亮对应那颗 chip。
        // 「← 返回实例设置」跟着搬到这一排最前面（左栏收起后它得有地方露面）。
        download.left.widget->setVisible(false);
        Theme::markDisplay(download.right.title, Theme::textFont(download.right.title->font(), Theme::TextRole::Display));
        auto *chipRow = new QWidget(download.right.widget);
        chipRow->setObjectName(downloadCategoryChipsName());
        {
            auto *row = new QHBoxLayout(chipRow);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(8);
            downloadBackButton->setParent(chipRow);
            downloadBackButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            row->addWidget(downloadBackButton);
            auto *group = new QButtonGroup(chipRow);
            group->setExclusive(true);
            for (int i = 0; i < categoryTexts.size(); ++i) {
                auto *chip = new QPushButton(categoryTexts.value(i), chipRow);
                chip->setObjectName(downloadCategoryChipName(i));
                chip->setCheckable(true);
                Theme::markChipButton(chip);
                group->addButton(chip, i);
                row->addWidget(chip);
            }
            row->addStretch(1);
            QObject::connect(group, &QButtonGroup::idClicked, downloadCategoryList,
                             [list = downloadCategoryList](int id) { list->setCurrentRow(id); });
            QObject::connect(downloadCategoryList, &QListWidget::currentRowChanged, group, [group](int row) {
                if (QAbstractButton *chip = group->button(row)) chip->setChecked(true);
            });
        }
        download.right.layout->addWidget(chipRow);

        downloadStack = new QStackedWidget(download.right.widget);
        downloadStack->setObjectName(downloadStackName());
        download.right.layout->addWidget(downloadStack, 1);

        // ---- 第 0 页「游戏」与其余那几页各自在自己的函数里（见 .hpp 里那两条说明）----
        // 顺序是契约：第 0 页先加，其余几页按同一张表的顺序往后加，
        // 于是"第 i 个分类 = 第 i 页"这条对应关系成立（验证工程会数一遍两边的条数）。
        setupDownloadGamePage();
        setupDownloadServerPage();
        setupDownloadResourcePages();

        // 页头的显隐跟着三处翻页走（判据见 `syncDownloadHeaderVisibility()` 那段）：
        // · 外层翻分类（游戏 / 服务端 / 整合包 / 模组 …）；
        // · 「游戏」那一层在版本树 / 安装页之间翻；
        // · 「服务端」那一层在版本列表 / 安装表单之间翻。
        // 三条入口都接上：「退回版本树」（`MainWindow::backToVersionTree()`）只动内层、
        // 不碰外层；从别的分类回到「游戏」又只动外层。只接一条就会漏掉另一半。
        // NOTE: 每一发都只传 `int`，判据自己读三个堆叠页的 currentIndex()，不依赖信号里
        // 的下标（外面那一发给的是分类下标，里面那两发是层下标，含义不一样）。
        const auto syncHeader = [header = downloadHeader, downloadStack = downloadStack,
                                 gameStack = gameStack, serverStack = serverStack,
                                 chipRow = chipRow](int) {
            syncDownloadHeaderVisibility(header, downloadStack, gameStack, serverStack, chipRow);
        };
        for (QStackedWidget *stack : {downloadStack, gameStack, serverStack}) {
            QObject::connect(stack, &QStackedWidget::currentChanged, stack, syncHeader);
        }
        syncHeader(0);

        // 打开下载页时停在「游戏」。
        downloadCategoryList->setCurrentRow(downloadCategoryGameIndex());
    }
    return downloadTab;
}

/// 下载页第 0 页「游戏」：里面再分两层（版本树 / 安装页）。
///
/// 搬出来是因为 `setupDownloadPage()` 这一页里其实是三摊东西（分类列表 / 这一页 /
/// 其余几页那一段循环），摊在一趟里谁也说不清"这一句是给谁的"。这一摊和下一摊
/// 之间只共享已经挂好的堆叠页与页头（`downloadStack` / `gameStack` / `downloadHeader`），
/// 没有局部变量要传。
void MainWindowUI::setupDownloadGamePage() {
    // ---- 第 0 页「游戏」：里面再分两层（版本树 / 安装页） ----
    //
    // 要求见头文件里 downloadGameStackName() 上面那段：四个分类各占一条 →
    // 点开是那条底下的版本号 → 点一个版本号进安装页；安装页顶上是名称框（带重名
    // 小字提示）、底下是四个加载器各一条（每条右边选它自己的版本）、最下面「安装」。
    // 左栏那列分类、右栏这份堆叠、底下状态栏都不动。
    QWidget *gamePage = new QWidget(downloadStack);
    gamePage->setObjectName(downloadGamePageName());
    QVBoxLayout *gameLayout = new QVBoxLayout(gamePage);
    gameLayout->setContentsMargins(0, 0, 0, 0);
    gameLayout->setSpacing(0);

    gameStack = new QStackedWidget(gamePage);
    gameStack->setObjectName(downloadGameStackName());
    gameLayout->addWidget(gameStack, 1);

    // ---- [0] 四条分类 + 它们底下的版本号 ----
    //
    // 四条的**内容**（哪四条、各叫什么、底下哪些版本）全部来自
    // VersionManager::availableVersionChannels()，界面这边一条判据都不写 ——
    // "什么算愚人节版"那件事只有 VersionCategory 一处（含那张写死的名单）。
    QWidget *treePage = new QWidget(gameStack);
    auto *treeLayout = new QVBoxLayout(treePage);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(10);
    // ---- 版本号筛选（补上「版本树没有过滤/搜索」那条缺口）----
    //
    // 742 条快照平铺在一棵树里，找一个版本号只能靠滚。这里给它一个和实例列表、
    // 文件页同一个规矩的框：空格分词、每个词都得出现在版本号里、不区分大小写。
    //
    // NOTE: 过滤**只动条目**（`QTreeWidgetItem::setHidden`），不动数据 ——
    // `MainWindow::refreshAvailableVersions` 重灌时照旧从头摆一遍，摆完再叫
    // `applyVersionFilter()` 按框里现在的字筛一回。两边各管一半，别把过滤塞进
    // MainWindow 那一趟（那样刷新一次就得把"刚才筛的是什么"也搬过去）。
    // NOTE: 结果那句话单独一个标签，**不塞进框里**：框里的字是用户打的，改它等于
    // 改用户的输入；而"找到 0 个"必须有个地方说，不然筛出来一片空白像坏了。
    QWidget *versionFilterBar = new QWidget(treePage);
    auto *versionFilterLayout = new QHBoxLayout(versionFilterBar);
    versionFilterLayout->setContentsMargins(0, 0, 0, 0);
    versionFilterLayout->setSpacing(10);
    versionFilterEdit = new QLineEdit(versionFilterBar);
    versionFilterEdit->setObjectName(versionFilterEditName());
    versionFilterEdit->setPlaceholderText(versionFilterPlaceholderText());
    addSearchIcon(versionFilterEdit);
    versionFilterLayout->addWidget(versionFilterEdit, 1);
    versionFilterHint = new QLabel(versionFilterBar);
    versionFilterHint->setObjectName(versionFilterHintName());
    // 空筛选时它藏着（连位置都不占）—— 平时这一行只有那个框。
    versionFilterHint->setVisible(false);
    // 这句最长也就十来个字，给个下限让"找到 1 个版本"和"找到 742 个版本"切换时
    // 输入框的宽度不跟着跳。
    versionFilterHint->setMinimumWidth(120);
    versionFilterLayout->addWidget(versionFilterHint);
    // 设计稿 download：搜索框定宽靠左，不再横贯整行。
    versionFilterEdit->setMaximumWidth(360);
    {
        // 分段：全部 / 正式版 / 快照 / 远古版（设计稿 download）。只藏 / 露那几条分类，
        // 和筛选框叠着生效（见 applyVersionFilter()）。各段的版本数在 tooltip 里。
        auto *segment = new QWidget(versionFilterBar);
        auto *segmentLayout = new QHBoxLayout(segment);
        segmentLayout->setContentsMargins(3, 3, 3, 3);
        segmentLayout->setSpacing(2);
        const QList<QPair<QString, QString>> channels = {
            {QString(), QCoreApplication::translate("MainWindowUI", "全部")},
            {QStringLiteral("release"), QCoreApplication::translate("MainWindowUI", "正式版")},
            {QStringLiteral("snapshot"), QCoreApplication::translate("MainWindowUI", "快照")},
            {QStringLiteral("old"), QCoreApplication::translate("MainWindowUI", "远古版")}};
        for (const auto &channel : channels) {
            auto *button = new QPushButton(channel.second, segment);
            button->setObjectName(versionChannelButtonName(channel.first));
            button->setCheckable(true);
            button->setChecked(channel.first.isEmpty());
            button->setProperty("wtfmlChannelKey", channel.first);
            segmentLayout->addWidget(button);
            versionChannelButtons.append(button);
            const QString key = channel.first;
            QObject::connect(button, &QPushButton::clicked, versionFilterBar, [this, key] {
                versionChannelFilter = key;
                for (QPushButton *other : std::as_const(versionChannelButtons))
                    other->setChecked(other->property("wtfmlChannelKey").toString() == key);
                applyVersionFilter();
            });
        }
        Theme::markSegmented(segment, versionChannelButtons);
        versionFilterLayout->addWidget(segment);
    }
    // 原来那段弹簧换成一个空标签：占的还是那块剩余空地（横向不报尺寸，不挤搜索框），
    // 刷新在线清单时上面挂一行加载态，版本树本身不动。
    versionRefreshStatus = new QLabel(versionFilterBar);
    versionRefreshStatus->setObjectName(versionRefreshStatusName());
    versionRefreshStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    versionFilterLayout->addWidget(versionRefreshStatus, 1);
    treeLayout->addWidget(versionFilterBar);
    // 打在框里就筛（不另要一颗「筛选」键）：这一层只是藏/露条目，便宜到可以每敲一个字
    // 跑一遍；资源页要按回车是因为那边真去发网络请求，两件事别混。
    QObject::connect(versionFilterEdit, &QLineEdit::textChanged, versionFilterBar,
                     [this] { applyVersionFilter(); });
    versionTree = new QTreeWidget(treePage);
    versionTree->setObjectName(versionTreeName());
    // 只有一列、只有一个版本号，表头没有信息量（"1" 那一格纯属噪音）。
    versionTree->setHeaderHidden(true);
    versionTree->setFrameShape(QFrame::NoFrame);
    // 742 条快照全是同一个行高，让它按第一行算一次就够（省的是滚动时的重算）。
    // NOTE: 行高现在是**常数**（versionTreeRowHeight()）—— 见代理的 sizeHint()：
    // 分类那一行是一行字、版本号那两行字，按内容算会两边不一样高，两行那种会被裁。
    versionTree->setUniformRowHeights(true);
    // 左边那个"小三角"不画（展开/收起的箭头我们画在**行最右边**，见下面的代理）。
    // NOTE: 走 rootIsDecorated(false) 而**不是**条目上的 setChildIndicatorPolicy ——
    // QTreeModel::hasChildren() 是按那个 policy 直接 return false 的，那样
    // 整个分类的版本号一条都排不进布局（树上只剩四行空的分类）。
    // 这里关掉的只是装饰格：分类从 x=0 起画，子条目照样缩进 indent 那么多。
    versionTree->setRootIsDecorated(false);
    versionTree->setColumnCount(1);
    stabilizeDownloadTree(versionTree);
    // 每一行长什么样（草方块 + 版本号 + 下面那行发布时间 + 最右边的 >/V）全在这个代理里。
    // 条目还是普通 QTreeWidgetItem（身份、展开状态、点击那条路一个都没变）。
    versionTree->setItemDelegate(new VersionTreeDelegate(versionTree));
    // 悬停那一行的加深底色（代理里那段 Theme::rowHoverFill()）**要的就是这一句**：
    // 代理画不画看 `QStyle::State_MouseOver`，而那一位是**视图**填进 option 里的，
    // 干净的视图默认不填（`WA_Hover=0` / `mouseTracking=0`）—— 少了它悬停底色一次
    // 都不会出现，而且**不报错、断言也不一定先红**，屏幕上就是"悬停没反应"。
    // 真平台 windows 与 offscreen 两个平台上都是这样：这一位不填，代理那段一个字都不生效。
    versionTree->setMouseTracking(true);
    // 字号放大一点。代理里那行小字是从这个字号往下推的，别在这儿单独设它。
    QFont treeFont = versionTree->font();
    treeFont = Theme::textFont(treeFont, Theme::TextRole::Body);
    versionTree->setFont(treeFont);
    // 图标尺寸：画是代理画的（它自己按 versionTreeIconSize 缩放），这里设的是**视图
    // 认下的那个尺寸** —— 装上安装页那棵加载器树之后，这两条竖的设置必须成对地
    // 一样（验证工程拿"两棵树这三条相等"钉"两边是同一个模式"）。
    versionTree->setIconSize(QSize(versionTreeIconSize(), versionTreeIconSize()));
    // 单击分类就展开/折叠（用户点的是最右边那个 >，那个箭头就画在那一行上）。
    // 双击**不**自己展开一次 —— 那会和单击叠起来，双击等于"展开又收起"。
    versionTree->setExpandsOnDoubleClick(false);
    // 展开/折叠动画：分类那一层展开的时候，底下的版本号是**滑**出来的。
    // 五张资源表（下面那个循环里）走同一句，见 ui/Animations.hpp。
    Anim::animateExpansion(versionTree);
    treeLayout->addWidget(versionTree, 1);
    gameStack->addWidget(treePage);

    // ---- [1] 安装页 ----
    QWidget *installPage = new QWidget(gameStack);
    installPage->setObjectName(installPageName());
    QVBoxLayout *installLayout = new QVBoxLayout(installPage);
    installLayout->setContentsMargins(0, 0, 0, 0);
    installLayout->setSpacing(10);

    // 顶上那一行：回得去 + 这一份是"基于哪个原版"装的。点的是树里哪个版本，到这里
    // 必须还看得见 —— 否则"我在给哪个版本装加载器"就全靠记忆了。
    auto *installTop = new QHBoxLayout();
    installTop->setSpacing(8);
    installBackButton = new QPushButton(installBackButtonText(), installPage);
    installBackButton->setObjectName(installBackButtonName());
    // 「← 返回」= 往回走（一般性）。
    Theme::markButton(installBackButton, Theme::ButtonKind::Neutral);
    installBackButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    installTop->addWidget(installBackButton);
    installTargetLabel = new QLabel(installPage);
    installTargetLabel->setObjectName(installTargetLabelName());
    installTop->addWidget(installTargetLabel);
    installTop->addStretch(1);
    installLayout->addLayout(installTop);

    // ---- 中间那一层：名称框 + 加载器卡片，**可滚动** ----
    //
    // NOTE: 这层用的是设置页那几页同一个配方（无边框 + widgetResizable + 横向不滚）
    // —— 滚动条仍然贴着正文面右边那条线，和别的页一个规矩。
    installScroll = new Theme::GutterScrollArea(installPage);
    installScroll->setObjectName(installScrollName());
    installScroll->setWidgetResizable(true);
    installScroll->setFrameShape(QFrame::NoFrame);
    installScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *installBody = new QWidget(installScroll);
    installBody->setObjectName(installScrollBodyName());
    auto *installBodyLayout = new QVBoxLayout(installBody);
    installBodyLayout->setContentsMargins(0, 0, 0, 0);
    // 和 installLayout 同一个行距（名称框与卡片之间那 10 px 不变）。
    installBodyLayout->setSpacing(installLayout->spacing());
    auto *installContent = new QWidget(installPage);
    installContent->setObjectName(QStringLiteral("installFloatingContent"));
    auto *installContentLayout = new QGridLayout(installContent);
    installContentLayout->setContentsMargins(0, 0, 0, 0);
    installContentLayout->addWidget(installScroll, 0, 0);
    installLayout->addWidget(installContent, 1);
    // The floating action must not cover the last selectable row at the scroll limit.
    installBodyLayout->setContentsMargins(0, 0, 0, qMax(48, installButtonMinHeight()) + 40);

    // 版本名称：实例的目录名（`versions/<这个名字>`）。默认名和名字不合法那句原因，
    // 都在 InstallPageController::updateInstallNameState() 一处刷（界面这边不持有
    // 业务状态：它不知道什么名字算不能用）。
    //
    // NOTE: 名字框底下**不再摆那句灰色说明**（"装好之后在实例页…" / 重名提示）——
    // 用户点名那是废话，和下载页别处"一句解释一行字"的口径一起删了。这里只留
    // 名字不能用时的红字。
    QWidget *nameBox = makeHintedControl(installPage);
    auto *nameLabel = new QLabel(installNameLabelText(), nameBox);
    nameBox->layout()->addWidget(nameLabel);
    installNameEdit = new QLineEdit(nameBox);
    installNameEdit->setObjectName(installNameEditName());
    InputSizing::shortValue(installNameEdit, InputSizing::kNameChars, InputSizing::Script::Han);  // 实例目录名，不限长，框留常见长度
    nameBox->layout()->addWidget(installNameEdit);
    // 名字不能用（有非法字符、空着）时的**原因**：一行红字。平时它空着、也藏起来 ——
    // 藏起来而不是"留一个空行"，是为了正常情况下名字框底下那几行的位置不动。
    installNameError = addDynamicErrorHint(nameBox, nameBox);
    installNameError->setObjectName(installNameErrorName());
    installNameError->setVisible(false);
    installBodyLayout->addWidget(nameBox);

    // ---- 实例预设（**不是一张独立的卡片**）----
    //
    // 形状是加载器列表里的**一行**（就放在 OptiFine 下面），点开是一张紧凑
    // 弹窗 —— 四类各挑一份、看一眼它要动什么，**不跳页**。不用「实例预设」卡片
    // （下拉框 + 查看内容 + 安装完成后应用）的原因：
    // · 一份下拉框只能装一份预设，而产品要的是四类各选一份；
    // · 「查看内容」和「安装完成后应用」两件本来就能并进那张弹窗（选择 + 计划）。
    // 那一行本身在下面「顶层五条」之后插（见 `installPresetRowRole()`）。

    // 加载器：**一棵树**，和上一页的版本树同一个模式（要求见头文件里那段）：
    // 界面上是四条加载器（+ 一条预设行）；「不装（纯原版）」只留内部那条（默认值、
    // 取消加载器后的落点），不摆成一个重复选项 —— 点一条展开它底下的版本号，
    // 点一个版本号就收起来、把选中的版本号写在那一行的第二行小字上。
    //
    // NOTE: 这一组的表、下标、文案沿用 modLoaderOptionNames()，形状是树，
    // 不是"单选钮 + 右边一个组合框"。
    //
    // 这一节用设置页那种「标题 + 小节卡片」（见 `addSettingsSection()`）。理由就是
    // 第三条 —— 右侧只有一个主要内容表面（不用 `QGroupBox` 套起来）：这一页
    // 整块已经长在正文那个面上（`wtfmlSurface="workspace"`），再套一个带边的
    // `QGroupBox` 就是"框套框"，而那圈边和正文面那圈边挨得只有 10 px，屏幕上就是
    // 两条平行的框。做成卡片之后这一节和设置页那些小节长得一模一样，缺口式标题也
    // 没有了。
    auto *loaderCard = new QFrame(installPage);
    markSurface(loaderCard, Theme::sectionSurfaceValue());
    auto *loaderLayout = new QVBoxLayout(loaderCard);
    // 卡片本身没有底色，左右不留内边距：小标题、加载器列表和上面的「版本名称」同一条左缘
    // （新建服务端页那张「服务端」卡片是同一个形状）。行内的内边距由列表自己给（下面 wtfmlRowLeftPad）。
    loaderLayout->setContentsMargins(0, 16, 0, 18);
    loaderLayout->setSpacing(12);
    {
        auto *loaderTitle = new QLabel(modLoaderLabelText(), loaderCard);
        markTextRole(loaderTitle, Theme::sectionTitleTextRoleValue());
        QFont titleFont = loaderTitle->font();
        titleFont.setBold(true);
        loaderTitle->setFont(titleFont);
        loaderLayout->addWidget(loaderTitle);
    }
    installBodyLayout->addWidget(loaderCard);

    loaderTree = new QTreeWidget(loaderCard);
    loaderTree->setObjectName(loaderTreeName());
    // 竖的那几条设置和版本树那边**一模一样**（表头藏起来、行高常数、左边那个小三角
    // 不画、同一个代理、同一个字号偏移）—— 按"和上一页相同的模式"来，
    // 两边长得不一样的话这条模式就只兑现了一半。
    loaderTree->setHeaderHidden(true);
    // 行内图标 / 文字退到名称框文字那条线上（名称框文字距框左缘 12，委托自己已留 7，补 6）。
    loaderTree->setProperty("wtfmlRowLeftPad", 6);
    // 树**自己不画那圈边**（和实例列表、文件夹列表、下载页左栏那三张
    // 同一条规矩）：它四周已经是上面那张小节卡片，再套一圈又是"框套框"。
    loaderTree->setFrameShape(QFrame::NoFrame);
    loaderTree->setUniformRowHeights(true);
    loaderTree->setRootIsDecorated(false);
    loaderTree->setColumnCount(1);
    stabilizeDownloadTree(loaderTree);
    // 末一个参数 false：这棵树的**叶子**（两档 + 具体版本号）右边不画那个 `>` ——
    // 那一层是"选中它"，底下没有下一页（见 VersionTreeDelegate 的构造函数）。
    // 顶层那五条照画（它们是真的能展开）。
    loaderTree->setItemDelegate(new VersionTreeDelegate(loaderTree, /*leafArrow=*/false));
    // 悬停底色要它，理由见上面版本树那第一处：视图不填 State_MouseOver 就不出底色，也不报错。
    loaderTree->setMouseTracking(true);
    loaderTree->setExpandsOnDoubleClick(false);
    // 展开/折叠动画。NOTE: 这棵树的高度是**跟着展开了几条**现算的
    // （下面那个 fitLoaderTreeHeight，挂在 expanded/collapsed 上），所以高度是"啪"地
    // 让出地方、版本号再滑进来 —— 两段不是同一拍，但是看着是对的（先空出来、再填上）。
    Anim::animateExpansion(loaderTree);
    // 展开/收起是"点那一条"完成的，而点中一条会把它整行画成选中色（和版本树那边
    // 一样）。这里**不要**再自己画高亮：选中色就是"你现在停在哪一条"。
    QFont loaderFont = loaderTree->font();
    loaderFont = Theme::textFont(loaderFont, Theme::TextRole::Body);
    loaderTree->setFont(loaderFont);
    // 五条加载器（+ 展开出来的版本号）都用同一个标志尺寸，和版本树上那 18 px 一致。
    loaderTree->setIconSize(QSize(versionTreeIconSize(), versionTreeIconSize()));
    loaderLayout->addWidget(loaderTree);

    // 顶层五条：四个加载器 + 最后那条「不装（纯原版）」。顺序、下标、文案全走
    // modLoaderOptionNames()/modLoaderOptionTexts() 那两张表（见头文件里那段 NOTE）。
    // 「不装」那条**建出来但藏起来**：下标 4 仍是内部身份（默认选中、取消加载器后的
    // 落点），只是界面上不长成一个和行尾 × 重复的选项。
    {
        const QStringList texts = modLoaderOptionTexts();
        for (int i = 0; i < texts.size(); ++i) {
            auto *item = new QTreeWidgetItem(loaderTree);
            item->setData(loaderTreeKeyColumn(), Qt::UserRole, i);
            item->setText(loaderTreeKeyColumn(), texts.at(i));
            // Preserve the internal vanilla identity/index, but do not render a duplicate choice.
            // 「不装（纯原版）」只留内部状态（下标 4 仍是默认值、也是取消加载器后的落点），
            // 界面上不摆成一条和行尾 × 重复的选项 —— 见上面那段 NOTE。
            if (i == modLoaderNoneIndex()) item->setHidden(true);
            // 每条前面那个标志（Fabric 的织纹 / Forge 的铁砧 / NeoForge 的狐狸 /
            // OptiFine 的放大镜；「不装」那条是草方块 —— 那条路装出来的就是原版）。
            // 画法是 ui/VersionIcons.cpp，那几个图标的画法表和这张表**同序**；
            // 给的是整数下标，越界时它自己会退回草方块。
            item->setIcon(loaderTreeKeyColumn(), QIcon(VersionIcons::loaderMark(i, versionTreeIconSize())));
            // 「不装（纯原版）」那条**没有**版本可选（原版没有加载器版本），所以它
            // 底下一直是空的、也不长展开箭头（代理按"有没有孩子"决定画不画）。
            // NOTE: 这一条**不是凑数**：它是"回到装纯原版"那条路，初始选中的就是它。
        }
    }
    // 「预设」那一行（在 OptiFine 下面）—— **不是加载器**，是一条开弹窗的行。
    // UserRole 是哨兵（`installPresetRowRole()`）：`loaderItem()` 按加载器下标找条目，
    // 看见这个值就不能把它当成第几条加载器；点它开的是那张四类弹窗。
    // 插在「不装（纯原版）」**前面**（`modLoaderNoneIndex()` 那个位置）：加载器那五条
    // 的 UserRole 一个都不动 —— 界面上多一行，数据层的身份不受影响。
    {
        auto *presetItem = new QTreeWidgetItem();
        presetItem->setData(loaderTreeKeyColumn(), Qt::UserRole, installPresetRowRole());
        presetItem->setText(loaderTreeKeyColumn(), installPresetRowText());
        // 拼图：和文件页那边"这一份是资源"同一个意思（见 VersionIcons::puzzleMark 那段）。
        presetItem->setIcon(
            loaderTreeKeyColumn(),
            QIcon(VersionIcons::puzzleMark(
                versionTreeIconSize(),
                QApplication::palette().color(QPalette::WindowText))));
        // 第二行小字是摘要（「未选择」/「已选择 N 类」），由 MainWindow 按用户的选择刷。
        presetItem->setData(loaderTreeKeyColumn(), rowSubTextRole(),
                            installPresetSummaryText(0));
        loaderTree->insertTopLevelItem(modLoaderNoneIndex(), presetItem);
    }
    // 这棵树的高度**跟着"现在展开了几条"变**：五条全收着就是 5 行高，展开一条之后
    // 按 5 + 那条的版本数往上长，长到 loaderTreeMaxRows() 行就停（再多出滚动条）。
    // 不这么做的话：写死 5 行高，展开之后版本号全挤在一条一行高的缝里看不见；
    // 写死"铺满整页"，那五条收着的时候底下空一大片、安装按钮还被推到窗口最底下。
    // 挂在 expanded/collapsed 上（而不是让 MainWindow 每次灌完数据记得调一下）：
    // 展开这件事本来就发生在这棵树自己身上。
    // NOTE: 这个 lambda 只捕获树的指针、**不捕获 this** —— 它挂在树上（context 也是树），
    // 而 MainWindowUI 本身不是 QObject（它只是搭界面的那个类，连着两句都用
    // 树自己当 context）。
    const auto fitLoaderTreeHeight = [tree = loaderTree]() {
        if (tree == nullptr) {
            return;
        }
        int rows = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *top = tree->topLevelItem(i);
            if (top->isHidden()) continue;
            rows += 1 + (top->isExpanded() ? top->childCount() : 0);
        }
        rows = qBound(1, rows, loaderTreeMaxRows());
        const int frame = 2 * tree->frameWidth();
        tree->setFixedHeight(rows * versionTreeRowHeight() + frame);
    };
    // NOTE: 写 QObject::connect 而不是 connect —— MainWindowUI **不是** QObject
    // （见类声明），成员那版 connect 在这儿不存在；直接写 connect 编译器会
    // 去找一个叫 connect 的普通函数。这两句用树自己当 context。
    QObject::connect(loaderTree, &QTreeWidget::itemExpanded, loaderTree,
                     [fitLoaderTreeHeight]() { fitLoaderTreeHeight(); });
    QObject::connect(loaderTree, &QTreeWidget::itemCollapsed, loaderTree,
                     [fitLoaderTreeHeight]() { fitLoaderTreeHeight(); });
    // NOTE: 「这一棵树现在该显示成什么样」（哪条写着"与 X 不兼容"、哪条写着选中的
    // 版本号）一件都不在这里做 —— 那是 InstallPageController::updateLoaderRows() 的活，
    // 它按"选中了哪个加载器"一处重算。这里只把五条搭出来。
    fitLoaderTreeHeight();
    // 中间那层滚动区收尾：名称框和卡片都铺完了，内容容器交给它；
    // 底下那条 stretch 让内容**上对齐**（内容装不满时不会在滚动区里竖直居中）。
    installBodyLayout->addStretch(1);
    installScroll->setWidget(installBody);

    installButton = new QPushButton(installButtonText(), installPage);
    installButton->setObjectName(installButtonName());
    installButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    installButton->setMinimumWidth(qMax(180, kDownloadButtonMinWidth));
    Theme::markButton(installButton, Theme::ButtonKind::Primary);
    // 正文 14、给一个最小高度（浮在内容区底部的主操作）。
    //
    // Latest design: a compact floating action at the bottom-centre of the right content pane.
    // Only its own rectangle intercepts input; the surrounding scroll area remains interactive.
    QFont installFont = installButton->font();
    installFont = Theme::textFont(installFont, Theme::TextRole::Body);  // 按钮一律正文 14 系统字（tokens-r2 §1）
    installFont.setWeight(QFont::Medium);
    installButton->setFont(installFont);
    installButton->setMinimumHeight(qMax(48, installButtonMinHeight()));
    auto *installFooter = new QFrame(installContent);
    installFooter->setObjectName(installFooterName());
    auto *footerLayout = new QHBoxLayout(installFooter);
    footerLayout->setContentsMargins(0, 12, 0, 0);
    footerLayout->setSpacing(kFilterRowSpacing);
    footerLayout->addWidget(installButton);
    installContentLayout->addWidget(installFooter, 0, 0, Qt::AlignHCenter | Qt::AlignBottom);
    installFooter->raise();
    gameStack->addWidget(installPage);

    // 第 0 页加在**最前面** —— 下面那个循环按同一个表、同一个顺序往后加，
    // 于是"第 i 个分类 = 第 i 页"这条对应关系成立（验证工程会数一遍两边的条数）。
    downloadStack->addWidget(gamePage);

}

/// 下载页「服务端」那一栏：和「游戏」**一个形状**（两层），不是另开一个界面。
///
/// 产品要求是"服务端下载要和游戏下载功能相一致，不要搞独立的新界面"。所以这一页
/// 加进 `downloadStack` 之后自己再分两层：
///   [0] **版本列表** —— 一句说明 + 筛子 + 一棵版本树，点一个版本进表单；
///   [1] **安装表单** —— 顶上「← 返回版本列表」，下面嵌 `NewServerPage`
///       （由 `MainWindow` 在第一次打开时塞进 `serverFormHost`；它不再自己开覆盖层）。
/// 表单那一层页头整块收起（判据见 `syncDownloadHeaderVisibility()`）。
///
/// 版本树的来源和资源页那几页是同一条路：克隆「游戏」那一棵（同一份版本清单），
/// 刷新走 `sourceDirty`，而且**只在列表这一层真的露着**的时候才克隆 —— 翻到表单上
/// 就不必再搬一遍整个清单。
void MainWindowUI::setupDownloadServerPage() {
    // ---- 外层：这一栏那一页 + 里面那个堆叠页 ----
    QWidget *serverPage = new QWidget(downloadStack);
    serverPage->setObjectName(downloadServerPageName());
    auto *serverLayout = new QVBoxLayout(serverPage);
    serverLayout->setContentsMargins(0, 0, 0, 0);
    serverLayout->setSpacing(0);
    serverStack = new QStackedWidget(serverPage);
    serverStack->setObjectName(downloadServerStackName());
    serverLayout->addWidget(serverStack);

    // ---- [0] 版本列表 ----
    //
    // 这一层和「游戏」那一页**逐件同形**：顶上一行筛子（左边搜索框、右边结果那句话），
    // 中间一整棵版本树，点一个版本进下一层。同一栏里两页的筛子长得不一样是最刺眼的
    // 不统一之一，所以那几条视图设置（字号 / 图标边长 / 悬停 / 展开动画）照抄过来。
    QWidget *listPage = new QWidget(serverStack);
    listPage->setObjectName(downloadServerListPageName());
    auto *listLayout = new QVBoxLayout(listPage);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(10);
    // 说明那一句（「装好之后去管理 → 服务端」）在**页头副标题**里，不占这一页：
    // 这一行留给"筛出来的结果"（和「游戏」那一页同一个位置、同一句话法）。
    auto *filterBar = new QWidget(listPage);
    auto *filterLayout = new QHBoxLayout(filterBar);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(10);
    auto *search = new QLineEdit(filterBar);
    search->setObjectName(QStringLiteral("serverVersionFilter"));
    search->setPlaceholderText(versionFilterPlaceholderText());
    addSearchIcon(search);
    filterLayout->addWidget(search, 1);
    auto *hint = new QLabel(filterBar);
    hint->setObjectName(downloadServerHintName());
    hint->setVisible(false);
    // 和「游戏」那一页同一个下限：结果在"找到 1 个"和"找到 742 个"之间切换时框宽不跳。
    hint->setMinimumWidth(120);
    filterLayout->addWidget(hint);
    listLayout->addWidget(filterBar);

    auto *versions = new QTreeWidget(listPage);
    versions->setObjectName(QStringLiteral("serverVersionTree"));
    versions->setHeaderHidden(true);
    versions->setFrameShape(QFrame::NoFrame);
    versions->setRootIsDecorated(false);
    versions->setColumnCount(1);
    versions->setUniformRowHeights(true);
    stabilizeDownloadTree(versions);
    versions->setItemDelegate(new VersionTreeDelegate(versions, true));
    versions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 悬停那一行的加深底色要它（另一半在代理里那个 `&& hovered`）。
    versions->setMouseTracking(true);
    QFont versionsFont = versions->font();
    versionsFont = Theme::textFont(versionsFont, Theme::TextRole::Body);
    versions->setFont(versionsFont);
    versions->setIconSize(QSize(versionTreeIconSize(), versionTreeIconSize()));
    versions->setExpandsOnDoubleClick(false);
    Anim::animateExpansion(versions);
    listLayout->addWidget(versions, 1);
    downloadNewServerButton = new QPushButton(newServerText(), listPage);
    downloadNewServerButton->setObjectName(downloadNewServerButtonName());
    downloadNewServerButton->hide(); // 版本号就是可见入口；这一颗只当"同一个动作"的接线点。
    const auto filter = [versions, search, hint] {
        const QString text = search->text().trimmed();
        const QStringList terms = text.split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        int matched = 0;
        for (int i = 0; i < versions->topLevelItemCount(); ++i) {
            auto *group = versions->topLevelItem(i);
            bool any = false;
            for (int j = 0; j < group->childCount(); ++j) {
                auto *item = group->child(j);
                bool matches = true;
                for (const auto &term : terms)
                    matches &= item->text(0).contains(term, Qt::CaseInsensitive);
                item->setHidden(!matches);
                if (matches) ++matched;
                any |= matches;
            }
            group->setHidden(!any);
            if (!terms.isEmpty() && any) group->setExpanded(true);
        }
        // 结果那句话：没有清单时说"刷新重试"，筛过就说找到几个 / 一个都没匹配上
        // （和「游戏」那一页的说法逐字一致 —— `versionFilterMatchText()` 那一处）。
        if (versions->topLevelItemCount() == 0) {
            hint->setText(QCoreApplication::translate("MainWindowUI", "暂无可用版本，请刷新重试。"));
            hint->setVisible(true);
            return;
        }
        hint->setVisible(!terms.isEmpty());
        hint->setText(terms.isEmpty()
                          ? QString()
                          : (matched == 0 ? versionFilterNoMatchText(text)
                                          : versionFilterMatchText(matched)));
    };
    QObject::connect(search, &QLineEdit::textChanged, versions, filter);
    auto *sync = new QTimer(versions);
    sync->setSingleShot(true);
    const QPointer<QTreeWidget> source = versionTree;
    QObject::connect(sync, &QTimer::timeout, versions,
                     [source, versions, filter, stack = QPointer<QStackedWidget>(serverStack), listPage,
                      outer = QPointer<QStackedWidget>(downloadStack), serverPage] {
        if (!source || !stack || !outer || !versions->property("sourceDirty").toBool()) return;
        // 两件事都得成立才算"这一层真的露着"：外面那一栏停在「服务端」，里面停在版本列表。
        // 少判外面那一条，翻到「游戏」页之后这一棵还会跟着版本树一起重建（白搬整个清单）。
        if (stack->currentWidget() != listPage || outer->currentWidget() != serverPage) return;
        versions->setProperty("sourceDirty", false);
        versions->setProperty("syncRevision", versions->property("syncRevision").toInt() + 1);
        versions->setUpdatesEnabled(false);
        QSet<QString> expanded;
        const bool initialized = versions->property("initialized").toBool();
        for (int i = 0; i < versions->topLevelItemCount(); ++i)
            if (versions->topLevelItem(i)->isExpanded())
                expanded.insert(versions->topLevelItem(i)->data(0, Qt::UserRole).toString());
        const QSignalBlocker blocker(versions);
        const int scroll = versions->verticalScrollBar()->value();
        versions->clear();
        for (int i = 0; i < source->topLevelItemCount(); ++i) {
            auto *original = source->topLevelItem(i);
            if (original->childCount() == 0) continue;
            auto *group = original->clone();
            versions->addTopLevelItem(group);
            group->setExpanded(initialized
                                   ? expanded.contains(group->data(0, Qt::UserRole).toString())
                                   : original->isExpanded());
        }
        if (versions->topLevelItemCount() > 0) versions->setProperty("initialized", true);
        versions->verticalScrollBar()->setValue(scroll);
        filter();
        versions->setUpdatesEnabled(true);
    });
    // 不在这里读 stack / outer：树析构时（QTreeWidget::clear → modelReset）它们的布局可能已先析构。
    // 露没露着由 sync 的超时回调判（那一发随 versions 一起析构，不会在拆窗时跑）。
    const auto invalidate = [versions, sync] {
        versions->setProperty("sourceDirty", true);
        sync->start(0);
    };
    QObject::connect(serverStack, &QStackedWidget::currentChanged, sync,
                     [sync, versions, stack = QPointer<QStackedWidget>(serverStack), listPage,
                      outer = QPointer<QStackedWidget>(downloadStack), serverPage](int) {
        if (stack && outer && stack->currentWidget() == listPage && outer->currentWidget() == serverPage &&
            versions->property("sourceDirty").toBool())
            sync->start(0);
    });
    // 外层翻回「服务端」那一栏时也要补一发（翻栏只动外面这一层）。
    QObject::connect(downloadStack, &QStackedWidget::currentChanged, sync,
                     [sync, versions, stack = QPointer<QStackedWidget>(serverStack), listPage,
                      outer = QPointer<QStackedWidget>(downloadStack), serverPage](int) {
        if (stack && outer && stack->currentWidget() == listPage && outer->currentWidget() == serverPage &&
            versions->property("sourceDirty").toBool())
            sync->start(0);
    });
    for (auto signal : {&QAbstractItemModel::modelReset})
        QObject::connect(versionTree->model(), signal, sync, invalidate);
    QObject::connect(versionTree->model(), &QAbstractItemModel::rowsInserted, sync, invalidate);
    QObject::connect(versionTree->model(), &QAbstractItemModel::rowsRemoved, sync, invalidate);
    QObject::connect(versionTree->model(), &QAbstractItemModel::dataChanged, sync, invalidate);
    versions->setProperty("sourceDirty", true);
    connectRowActivation(
        versions,
        [this](QTreeWidgetItem *item) {
            if (!item) return;
            if (!item->parent()) {
                item->setExpanded(!item->isExpanded());
                return;
            }
            const QString id = item->data(versionTreeKeyColumn(), Qt::UserRole).toString();
            if (id.isEmpty()) return;
            // 点一个版本 = 进安装表单那一层（和「游戏」点版本号进安装页同一步）。
            downloadNewServerButton->setProperty("selectedVersion", id);
            downloadNewServerButton->click();
        },
        [](QTreeWidgetItem *item) { return item && !item->parent(); });
    serverStack->addWidget(listPage);

    // ---- [1] 安装表单 ----
    QWidget *formPage = new QWidget(serverStack);
    formPage->setObjectName(downloadServerFormPageName());
    auto *formLayout = new QVBoxLayout(formPage);
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setSpacing(10);
    serverFormBackButton = new QPushButton(installBackButtonText(), formPage);
    serverFormBackButton->setObjectName(downloadServerBackButtonName());
    Theme::markButton(serverFormBackButton, Theme::ButtonKind::Neutral);
    serverFormBackButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *backRow = new QHBoxLayout;
    backRow->setContentsMargins(0, 0, 0, 0);
    backRow->addWidget(serverFormBackButton);
    // 和「游戏」安装页顶上一模一样：返回键右边那句话说的是"这一趟要装什么"。
    serverFormIntroLabel = new QLabel(formPage);
    serverFormIntroLabel->setObjectName(QStringLiteral("serverFormIntroLabel"));
    backRow->addWidget(serverFormIntroLabel);
    backRow->addStretch(1);
    formLayout->addLayout(backRow);
    serverFormHost = new QWidget(formPage);
    serverFormHost->setObjectName(downloadServerFormHostName());
    auto *hostLayout = new QVBoxLayout(serverFormHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    formLayout->addWidget(serverFormHost, 1);
    serverStack->addWidget(formPage);
    // 「← 返回版本列表」= 退回这一页的第一层（表单不自己关：它就住在这一层里）。
    QObject::connect(serverFormBackButton, &QPushButton::clicked, serverStack,
                     [stack = serverStack] { stack->setCurrentIndex(downloadServerListIndex()); });

    // 第 1 页加在「游戏」之后（顺序是契约：第 i 个分类 = 第 i 页）。
    downloadStack->addWidget(serverPage);
}

/// 下载页其余那几页：一行筛子 + 一棵三层树（资源 → 游戏版本 → 版本号）。
///
/// 这几页的控件**一模一样**，只有搜索框里那句提示文字不同，所以是循环几遍、不是抄几份。
/// 分类表每次现问（`downloadCategoryKeys()` 是唯一出处），不指望调用方递进来。
void MainWindowUI::setupDownloadResourcePages() {
    const QStringList categoryKeys = downloadCategoryKeys();

    // ---- 其余几页：一行筛子 + 一棵三层树（资源 → 游戏版本 → 版本号）----
    //
    // 这几页的控件**一模一样**，只有搜索框里那句提示文字不同，所以这里是循环几遍，
    // 不是抄几份。三层的那条路就是下面这条（见头文件里 downloadResourceTreeName()
    // 上面那段）：
    // 第一层 资源 → 点开是游戏版本 → 点开某个游戏版本**这时才去问接口** → 第三层版本号
    // 关键是"什么在哪儿灌"：这一层只**搭**（空树、空框），几条路的数据都由 MainWindow 灌
    // （和版本树/加载器树那两棵一样，界面这边一条业务判据都不写）。
    for (int i = 0; i < categoryKeys.size(); ++i) {
        if (i == downloadCategoryGameIndex()) {
            continue;
        }
        // 「服务端」那一栏自己一整页（两层，和「游戏」一个形状）—— 这里跳过资源页模板。
        // 见 `setupDownloadServerPage()`。
        if (i == downloadCategoryServerIndex()) {
            continue;
        }
        const QString categoryKey = categoryKeys.value(i);
        QWidget *page = new QWidget(downloadStack);
        page->setObjectName(categoryKey + QStringLiteral("Page"));
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(10);

        // ---- 顶上那一行筛子----
        //
        // 要求见头文件里 downloadFilterVersionComboName() 上面那段。摆法是**两行**：
        // 第一行：名称（那个搜索框，占满剩下的宽度）+「搜索」+「清空条件」
        // （**整合包那一页**还多一颗「导入整合包…」）
        // 第二行：版本 / 模组加载器 / 来源 / 类型 四个下拉框，每个前面带一个小标签
        // 为什么不摆成一行：七样东西摆不下 —— 900 px 宽的窗口里右栏只剩 600 px 上下，
        // 七个控件挤进去每个 80 px，下拉框会被压到只剩一个箭头，那还不如不给。
        // 为什么名称单独占第一行的左边：它是**自由输入**的（名称和版本都可自行输入），
        // 要占满剩下的宽度才好打字。
        QWidget *filterRow = new QWidget(page);
        auto *filterLayout = new QVBoxLayout(filterRow);
        filterLayout->setContentsMargins(0, 0, 0, 0);
        filterLayout->setSpacing(kFilterRowSpacing);
        auto *filterTop = new QHBoxLayout;
        filterTop->setSpacing(kFilterRowSpacing);
        auto *filterBottom = new QHBoxLayout;
        filterBottom->setSpacing(kFilterRowSpacing);

        // 名称那一格。**回车就搜**：上方预留的搜索框里，这两个框选中时按回车等同于按搜索键。
        // NOTE: 不加"清除"那个小叉（setClearButtonEnabled）：它在输入框里塞一个真的
        // 子按钮，而这个页面上的控件形状是被验证工程一条一条数着的；
        // 要清空有旁边那颗「清空条件」。
        // 提示文字是那页"能干什么"的一句话（downloadSearchHint，唯一出处）——
        // 空框里那句话正好在最该看见它的地方。
        // 那颗「← 返回实例设置」**不在这一行里**：返回按钮放在左上角（就是左边栏的顶上）。
        // 它现在是整个下载页
        // **共用的一颗**、摆在左栏顶上，见 setupUi 里那一句（那一颗有两件事
        // 只有那一层知道：左栏是共用的，而"从实例进来的"这件事本来就与哪一栏无关）。

        auto *search = new QLineEdit(filterRow);
        search->setObjectName(downloadSearchEditName(categoryKey));
        search->setPlaceholderText(downloadSearchHint(i));
        InputSizing::longValue(search, "搜索框：关键词长度不定，占满筛选行");  // i18n-ignore
        addSearchIcon(search);
        filterTop->addWidget(search, 1);

        // 四个下拉框共用一套竖设置：宽度按**内容**算（`AdjustToContents`）—— 框里最长那一项
        // 要整句放得下。原来钉的是"按最短内容长度算 + 88 的上限"，结果是「全部加载器」最后
        // 一个字被裁掉（88 里能画字的只有 66 px，那五个字要 70）。上限还在，只用来兜住
        // 「一整份在线版本清单」那种没边的长项（见头文件里 downloadFilterComboMaxWidth()）。
        const auto makeFilterCombo = [&](const QString &objectName) {
            auto *combo = new QComboBox(filterRow);
            combo->setObjectName(objectName);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
            combo->setMinimumContentsLength(downloadFilterMinimumContentsLength());
            combo->setMaximumWidth(downloadFilterComboMaxWidth());
            WheelGuard::install(combo);
            return combo;
        };
        // 一个"标签 + 下拉框"的小组。标签只为说明右边那个框是筛什么用的 —— 这四样
        // 没有一样能从框里的内容猜出来（尤其「全部」那一项在四个框里长得一模一样）。
        const auto addFilterLabel = [&](QHBoxLayout *row, const QString &text) {
            auto *group = new QHBoxLayout;
            group->setSpacing(4);
            auto *label = new QLabel(text, filterRow);
            label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
            group->addWidget(label);
            row->addLayout(group);
            return group;
        };

        // 版本：**可编辑的下拉框** —— 上面那张清单里挑一个，也能自己打一个（版本可自行输入）。
        // 清单由 MainWindow 灌（它才知道有哪些游戏版本），
        // 灌之前它就是空的一个框。回车这一条也由 MainWindow 接（可编辑下拉框内部那个
        // QLineEdit 是 `combo->lineEdit()`，这一层不接信号）。
        auto *versionGroup = addFilterLabel(filterBottom, downloadFilterVersionLabel());
        QComboBox *versionCombo = makeFilterCombo(downloadFilterVersionComboName(categoryKey));
        versionCombo->setEditable(true);
        // NOTE: 可输入的框上**必须**把"回车就把框里的字追进清单"这条关掉（Qt 的默认是
        // `InsertAtBottom`，而且 duplicatesEnabled 默认开着 ⇒ 连重复项都照追）。
        // 这一格存在的理由正是"手打一个清单里没有的版本号"（版本号是自由输入），
        // 于是每按一次回车就多一项：清单越用越脏，而追进去的那一项永远
        // 也搜不出东西来（它不在任何一家接口的版本表里）。
        versionCombo->setInsertPolicy(QComboBox::NoInsert);
        versionCombo->setPlaceholderText(downloadFilterVersionPlaceholder());
        versionGroup->addWidget(versionCombo);

        // 模组加载器：**只有模组那一页有**（模组加载器只在模组那一栏存在）。
        // 别的几页连控件都不建 —— 建了再藏起来不叫"只在模组那栏存在"（验证工程按名字
        // 能把它找出来）。判据是**那一页的下标**（downloadCategoryModIndex()，和
        // 「游戏」那条一个规矩）：这张表要再插一条，「模组」那个号跟着挪，别在调用点
        // 再写一遍 "mod" 那个字面量。
        QComboBox *loaderCombo = nullptr;
        if (i == downloadCategoryModIndex()) {
            auto *loaderGroup = addFilterLabel(filterBottom, downloadFilterLoaderLabel());
            loaderCombo = makeFilterCombo(downloadFilterLoaderComboName(categoryKey));
            loaderGroup->addWidget(loaderCombo);
        }

        // 来源 / 类型：两页都有。类型那一栏**整个会藏起来**（连标签一起）——
        // `categoryOptions()` 回空表的时候（目前只有 Modrinth 的数据包那一档），
        // 摆一个只有「全部」的框等于骗人。显隐由 MainWindow 按那张表决定，这一层
        // 只是把标签也一起放在能藏的地方（标签进 layout、留个指针不管不行，所以这里
        // 用一个容器把"标签 + 框"包成一件东西）。
        auto *sourceGroup = addFilterLabel(filterBottom, downloadFilterSourceLabel());
        QComboBox *sourceCombo = makeFilterCombo(downloadFilterSourceComboName(categoryKey));
        sourceGroup->addWidget(sourceCombo);

        QWidget *typeBox = new QWidget(filterRow);
        auto *typeLayout = new QHBoxLayout(typeBox);
        typeLayout->setContentsMargins(0, 0, 0, 0);
        typeLayout->setSpacing(4);
        addFilterLabel(typeLayout, downloadFilterTypeLabel());
        QComboBox *typeCombo = makeFilterCombo(downloadFilterTypeComboName(categoryKey));
        typeLayout->addWidget(typeCombo);
        filterBottom->addWidget(typeBox);

        filterBottom->addStretch(1);

        // 「搜索」和「清空条件」两颗键（一颗搜索键 + 一颗清空条件键）。
        // 这一层只把它们摆上；点下去干什么、以及"名字/版本那两个框里按回车"这条，
        // 都接在 MainWindow 那一侧（同一个槽）。
        auto *searchButton = new QPushButton(downloadFilterSearchText(), filterRow);
        searchButton->setObjectName(downloadFilterSearchButtonName(categoryKey));
        // NOTE: 这里**不设** setAutoDefault —— 那个开关只在 QDialog 里有意义（它管的是
        // "对话框里按回车走哪颗键"），而这一页住在 QMainWindow 里，设了也是张白纸。
        // "名字/版本框里按回车 = 按搜索键"是 MainWindow 显式接的
        // （两个框各自的 returnPressed → 同一个槽），不靠默认按钮。
        filterTop->addWidget(searchButton);
        auto *resetButton = new QPushButton(downloadFilterResetText(), filterRow);
        resetButton->setObjectName(downloadFilterResetButtonName(categoryKey));
        // 「清空搜索」是一般性（那颗「搜索」是确认性，所以它是绿的）。
        Theme::markButton(resetButton, Theme::ButtonKind::Neutral);
        filterTop->addWidget(resetButton);

        // 整合包那一页那颗「导入整合包…」（整合包那一页带一个导入功能）。
        // 字和实例页那颗共用（importModpackText()），走的路也是同一条。
        if (i == downloadCategoryModpackIndex()) {
            downloadImportModpackButton = new QPushButton(importModpackText(), filterRow);
            downloadImportModpackButton->setObjectName(downloadFilterImportModpackButtonName());
            filterTop->addWidget(downloadImportModpackButton);
        }

        filterLayout->addLayout(filterTop);
        filterLayout->addLayout(filterBottom);
        pageLayout->addWidget(filterRow);

        // 下面那棵树：三层，和上一页那两棵用**同一个代理、同一套竖设置**（表头藏起来、
        // 行高常数、左边那个小三角不画、同一个字号偏移），所以长得一样。
        // 只有两处不一样：
        // ① 图标大一点（resourceTreeIconSize()）—— 要看清缩略图，18 px 那个不行；
        // ② 叶子那一层悬停写「下载」（leafArrow=true）—— 第三层"点一个版本号"是**下它**，
        // 是个真动作，和加载器树上"选中它"不是一回事。
        QTreeWidget *tree = new QTreeWidget(page);
        tree->setObjectName(downloadResourceTreeName(categoryKey));
        tree->setHeaderHidden(true);
        tree->setUniformRowHeights(true);
        tree->setRootIsDecorated(false);
        tree->setColumnCount(1);
        stabilizeDownloadTree(tree);
        auto *resourceDelegate = new VersionTreeDelegate(tree, /*leafArrow=*/true, resourceTreeIconSize());
        resourceDelegate->setLeafVerb(QCoreApplication::translate("MainWindowUI", "下载"));  // 第三层点一下就是下它
        resourceDelegate->setResourceRows(true);
        tree->setItemDelegate(resourceDelegate);
        // 悬停底色要它，理由见版本树那第一处：视图不填 State_MouseOver 就不出底色，也不报错。
        tree->setMouseTracking(true);
        tree->setExpandsOnDoubleClick(false);
        QFont resourceFont = tree->font();
        resourceFont = Theme::textFont(resourceFont, Theme::TextRole::Body);
        tree->setFont(resourceFont);
        tree->setIconSize(QSize(resourceTreeIconSize(), resourceTreeIconSize()));
        // 展开/折叠动画：和版本树那两棵同一句（三个层次都共用同一个代理，
        // 动画也共用同一个开关），见 ui/Animations.hpp。
        Anim::animateExpansion(tree);
        pageLayout->addWidget(tree, 1);

        downloadResourceSearches.insert(categoryKey, search);
        downloadResourceTrees.insert(categoryKey, tree);
        downloadFilterVersionCombos.insert(categoryKey, versionCombo);
        downloadFilterSourceCombos.insert(categoryKey, sourceCombo);
        downloadFilterTypeCombos.insert(categoryKey, typeCombo);
        downloadFilterTypeBoxes.insert(categoryKey, typeBox);
        downloadFilterSearchButtons.insert(categoryKey, searchButton);
        downloadFilterResetButtons.insert(categoryKey, resetButton);
        // 加载器那一格**只有模组页才登记** —— 别的页没有这一项，按它们的键取回来是
        // nullptr（那正是"只在模组那栏存在"要的形状，见头文件里那段）。
        if (loaderCombo != nullptr) {
            downloadFilterLoaderCombos.insert(categoryKey, loaderCombo);
        }
        downloadStack->addWidget(page);
    }
    // 打开下载页时停在「游戏」。
    downloadCategoryList->setCurrentRow(downloadCategoryGameIndex());
}


/// 设置那一页的外壳（左栏分类 + 右栏堆叠页），内容在 `setupSettingsPage()` 里。

void MainWindowUI::setDownloadHeaderRow(int row) {
    if (downloadHeaderSubtitleLabel == nullptr) {
        return;  // 界面没搭出来（还没调 setupUi）——和别处那些 getter 一个规矩。
    }
    if (row < 0 || row >= downloadCategoryKeys().size()) {
        // 越界什么都不改：页头宁可停在上一句，也不要空成一行（空字符串会把它整行收起来，
        // 页头就"矮"了一截，看着像换了页）。
        return;
    }
    downloadHeaderSubtitleLabel->setText(downloadHeaderSubtitle(row));
    // 说明是**这一页的状态**，永远露着（`fillPageHeader()` 那套"空串就收起来"在这儿
    // 用不上：它管的是"这一页有没有说明"，而这里每一类都有一句）。
    downloadHeaderSubtitleLabel->setVisible(downloadCategoryKeys().value(row) == QStringLiteral("datapack"));
    if (downloadHeaderRefreshButton != nullptr) {
        downloadHeaderRefreshButton->setVisible(true);
    }
}

void MainWindowUI::applyVersionFilter() {
    if (versionTree == nullptr || versionFilterEdit == nullptr) {
        return;  // 界面没搭出来（和上面那个函数一个规矩）
    }
    const QString text = versionFilterEdit->text().trimmed();
    const QStringList terms =
        text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const bool active = !terms.isEmpty();
    const int column = versionTreeKeyColumn();

    // 第一次真正开筛之前，把"用户自己摆成什么样"记下来。之后清空筛选按它还原。
    // NOTE: 已经在筛的时候**不重记**：那一趟的展开状态是筛选自己撑开的，记下来等于
    // 把"筛完还原"变成"筛完全展开"。
    if (active && !versionFilterActive) {
        versionFilterExpandedBefore.clear();
        for (int i = 0; i < versionTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *top = versionTree->topLevelItem(i);
            const QString key = top->data(column, Qt::UserRole).toString();
            versionFilterExpandedBefore.insert(key, top->isExpanded());
        }
    }

    // 藏/露条目和展开/收起都是纯界面动作，没必要让"展开动画"跟着每敲一个字跑一遍
    // （742 条那个分类一次就是几百毫秒的动画队列）。信号挡掉不影响这里的 setHidden。
    const QSignalBlocker blocker(versionTree);
    QSet<QString> matchedIds;
    for (int i = 0; i < versionTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = versionTree->topLevelItem(i);
        const QString key = top->data(column, Qt::UserRole).toString();
        int visibleChildren = 0;
        for (int c = 0; c < top->childCount(); ++c) {
            QTreeWidgetItem *child = top->child(c);
            const QString id = child->data(column, Qt::UserRole).toString();
            bool match = !active;
            if (active) {
                match = true;
                for (const QString &term : terms) {
                    if (!id.contains(term, Qt::CaseInsensitive)) {
                        match = false;
                        break;
                    }
                }
            }
            child->setHidden(!match);
            if (match) {
                ++visibleChildren;
                matchedIds.insert(id);
            }
        }
        if (!versionChannelFilter.isEmpty() && key != versionChannelFilter) {
            top->setHidden(true);  // 分段只看一类：别的分类（含「最新版本」）整条收起
        } else if (!versionChannelFilter.isEmpty()) {
            top->setHidden(active && visibleChildren == 0);
            top->setExpanded(true);
        } else if (active) {
            // 有命中的分类自动撑开（不然"找到 2 个"却还藏在收着的分类里，等于没找到）；
            // 一个都不命中的分类整条收起来 —— 留一行空分类在结果里只是噪音。
            top->setHidden(visibleChildren == 0);
            top->setExpanded(visibleChildren > 0);
        } else {
            top->setHidden(false);
            // NOTE: 清单刷新是"清空再重灌"，重灌出来的分类默认是收着的。这时候按筛选前
            // 记下的状态摆回去；记不着的（筛选期间才出现的分类）保持它自己的样子。
            top->setExpanded(versionFilterExpandedBefore.value(key, top->isExpanded()));
        }
    }
    if (!active) {
        versionFilterExpandedBefore.clear();
    }
    versionFilterActive = active;
    // 分段上的计数（tooltip）：每类多少个版本；「全部」不算「最新版本」那几条重复的。
    int total = 0;
    QHash<QString, int> counts;
    for (int i = 0; i < versionTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = versionTree->topLevelItem(i);
        const QString key = top->data(column, Qt::UserRole).toString();
        counts.insert(key, top->childCount());
        if (key != QStringLiteral("latest")) total += top->childCount();
    }
    for (QPushButton *button : std::as_const(versionChannelButtons)) {
        const QString key = button->property("wtfmlChannelKey").toString();
        button->setToolTip(QCoreApplication::translate("MainWindowUI", "%1 个版本")
                               .arg(key.isEmpty() ? total : counts.value(key)));
    }

    if (versionFilterHint != nullptr) {
        versionFilterHint->setVisible(active);
        if (active) {
            versionFilterHint->setText(matchedIds.isEmpty()
                                           ? versionFilterNoMatchText(text)
                                           : versionFilterMatchText(matchedIds.size()));
        } else {
            versionFilterHint->clear();
        }
    }
}
