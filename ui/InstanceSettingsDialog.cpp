/**
 * @file InstanceSettingsDialog.cpp
 * @brief 搭出「实例设置…」对话框的控件，并把它们读成一份覆盖值。
 * @note 这里**不落盘、不碰 src/ 下的代码**：值从控件读，落盘在 MainWindow 那边。
 *
 * 这一层唯一的"业务判断"是内存那两行在自动档下的表现（禁用 + 不写覆盖），
 * 理由写在 .hpp 的 override() 上。
 *
 * 【这一份是四份里的哪一份】
 *
 * 类是同一个类，实现按"一页一摊"分在四个 .cpp 里：这一份是**外壳与接线**（构造、左栏导航、
 * 堆叠页、脏状态、拖放、拒绝关页），另外三份是「概览」（`InstanceSettingsOverviewPage.cpp`）、
 * 「启动」（`InstanceSettingsLaunchPage.cpp`）和文件那几页（`InstanceSettingsFilePages.cpp`）。
 * 搭控件用的小工具在 `InstanceSettingsDialogInternals.hpp`（原来它们在下面那个匿名命名空间里，
 * 拆开之后别的 .cpp 看不见 —— 匿名命名空间是内部链接）。
 */
#include "InstanceSettingsDialog.hpp"
#include "InstanceSettingsSignature.hpp"
#include "InstanceSettingsPageChoices.hpp"
#include "ResourcePackPage.hpp"
#include "WheelSafeComboBox.hpp"

#include "Animations.hpp"
#include "ElidedLabel.hpp"
#include "Theme.hpp"   // 浮层的底色（Theme::Part::Panel）

#include <QAbstractItemView>  // eventFilter()：那两件导航控件共同的兜底（focusInEvent 替人挑第一条）
#include <QCheckBox>
#include <QElapsedTimer>
#include <QDebug>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>   // 左栏那一列的宽度（见 navListWidth()）
#include <QGuiApplication>
#include <QMimeData>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QUrl>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QShowEvent>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QDirIterator>
#include <QSet>
#include <QImageReader>
#include <QVBoxLayout>
#include <utility>
#include <algorithm>
#include <functional>

#include "ConfigFileEditorPage.hpp"
#include "InstanceFilesPage.hpp"
#include "MainWindowUI.hpp"
#include "MemorySlider.hpp"
#include "RowActivation.hpp"
#include "VersionIcons.hpp"
// 第 1 格那个世界设置。头文件那边已经 include 了它（进参是个嵌套类型），
// 这一句是给"这一份 .cpp 自己用得上它的完整定义"留的。
#include "WorldSettingsPage.hpp"
#include "VersionTreeDelegate.hpp"
#include "InstanceSettingsDialogInternals.hpp"

namespace {

QString maintenancePageKey(const QString &key) {
    if (key == QStringLiteral("integrity") || key == QStringLiteral("repair") ||
        key == QStringLiteral("integritymenu") || key == QStringLiteral("diagnosticsmenu"))
        return QStringLiteral("diagnosis");
    if (key == QStringLiteral("export") || key == QStringLiteral("snapshots"))
        return QStringLiteral("operationsmenu");
    return key;
}

/// 这七个框一律**不伸缩 + 一条下限**（见 `addOverrideRow()` 里那两行）：默认吃拉伸的话，
/// 同一页上会量出 59 / 143 / 307 三种宽度（「Java 路径」那一行挤、其它几行摊满），而"档位"
/// 是同一件事的同一个控件。不伸缩之后七格一样宽，右边那个值控件因此也从同一条竖线上起头。
///
/// 为什么是"下限 + 不伸缩"而不是 `setFixedWidth()`：这七格里的字一模一样
/// （就那两项），所以 `sizeHint()` 天然一样大 —— 钉住策略就够了。哪天真换了更大的字体，
/// `sizeHint()` 跟着长、七格**还是一样宽**，写死像素反而会把字切掉。
constexpr int kChoiceComboWidth = 110;
/// 左栏那一列导航在算出来的宽度上再加多少余量（左右各一半）。
/// `sizeHintForColumn` 给的是"字有多宽"，条目的字贴着边看着像被切了。
constexpr int kNavListExtraWidth = 28;
/// 两栏之间空多少。和整页外边距那一档（12）同一档。
constexpr int kColumnSpacing = 16;
/// 页眉实例名的字号下限：从 Display 26 往下收到这一号，再放不下才省略。
constexpr int kHeaderTitleMinPixelSize = 20;
/// 单独拿出来用时的初始大小。宽是"一列导航 + 一块还放得下两列表格的内容"量出来的
/// （见 .cpp 构造函数里那句 NOTE：嵌进覆盖层时这个数不作数）。
constexpr int kDialogWidth = 820;
constexpr int kDialogHeight = 560;

/// 把左栏那棵导航树摆进两栏那一行。
///
/// NOTE: 外面套一层**竖排 + 底部 stretch**，不是直接把列表 addWidget 进去 —— 列表自己是
/// 定高的（"贴着内容"那条），而横排布局会把这一格的**高**整个给它，定高控件在那
/// 一格里的落点是 Qt 说了算。套一层之后贴顶是写出来的，不靠猜。设置页左栏那一列
/// 也是这个写法。
void addNavColumn(QHBoxLayout *columns, QWidget *list, QWidget *footer = nullptr) {
    auto *column = new QWidget(list->parentWidget());
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(list, 1);
    if (footer != nullptr) {
        layout->addWidget(footer);
    }
    columns->addWidget(column);
}

/// 堆叠页第 @p index 格是**哪一页**（认不出来回空串）。`stackPageIndexOf()` 的反面。
///
/// NOTE: 摆在这一列左栏旁边，是因为"左栏现在该亮哪一条"要从"右栏现在摆着哪一页"反查
/// （见 rebuildEntryList()）—— 反过来再存一份"现在在哪一页"的影子状态，迟早会和
/// 堆叠页对不上。
QString pageKeyAtStackIndex(int index) {
    if (index < 0) {
        return QString();
    }
    const QStringList pinned = InstanceSettingsDialog::pinnedPageKeys();
    if (index < pinned.size()) {
        return pinned.at(index);
    }
    const QStringList files = InstanceSettingsDialog::filePageKeys();
    const int fileIndex = index - InstanceSettingsDialog::filePageFirstIndex();
    if (fileIndex >= 0 && fileIndex < files.size()) return files.at(fileIndex);
    return InstanceSettingsDialog::lifecyclePageKeys().value(fileIndex - files.size());
}


}  // namespace

using namespace InstanceSettingsDialogInternals;

InstanceSettingsDialog::InstanceSettingsDialog(const QString &instanceName,
                                               const QString &instanceId,
                                               const Override &current,
                                               const GlobalValues &global,
                                               const QStringList &appendJvmArgs,
                                               bool warnJavaMismatch,
                                               const QList<WindowModeChoice> &windowModes,
                                               QWidget *parent, bool embedded)
    : QDialog(parent), m_global(global), m_instanceId(instanceId) {
    // 在创建子控件前确定最终父容器与类型，避免挂载时整棵控件树重新继承样式。
    if (embedded) setWindowFlags(Qt::Widget);
    setWindowTitle(tr("实例设置"));
    setAcceptDrops(true);
    // 比分组对话框宽得多：从左到右是**两栏**（左栏一列页 + 右栏一块内容），600 放不下。
    // NOTE: 嵌进覆盖层的时候这个尺寸**不作数** —— 那一层给它的是内容区的整幅宽（见
    // OverlayHost::showPage 里那句 `area->layout()->addWidget(dialog)`）。
    // 这一句只影响单独把它拿出来用的时候（验证工程那几处）。
    resize(kDialogWidth, kDialogHeight);

    // 盘上那一份「现状」留着：**启动页第一次切过去才建**，建之前 override() /
    // appendJvmArgs() / warnJavaMismatch() 读的就是它（见 .hpp 里那几个成员）。
    // autoMemory 这一页从不产出（见 .hpp 里 override() 那段），清掉免得顺手写回去。
    m_currentOverride = current;
    m_currentOverride.autoMemory.reset();
    for (const QString &line : appendJvmArgs) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            m_pendingAppendJvmArgs.append(trimmed);
        }
    }
    m_pendingWarnJavaMismatch = warnJavaMismatch;

    auto *outer = new QVBoxLayout(this);
    // 左右不再留边：覆盖层自己两侧已经各留了 40，这一页再叠 12 会把右栏挤窄（UI问题 #6）。
    outer->setContentsMargins(0, 12, 0, 12);
    outer->setSpacing(10);

    // UI 7b 页眉（设计稿 instance-*）：图标 + 实例名 + 一行元信息 | 打开目录（扁平）+ 启动实例（这一页唯一的实心钮）。
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("instanceSettingsHeader"));
    auto *headerLine = new QHBoxLayout(header);
    headerLine->setContentsMargins(4, 0, 4, 4);
    headerLine->setSpacing(14);
    m_headerIcon = new QLabel(header);
    m_headerIcon->setObjectName(QStringLiteral("instanceHeaderIcon"));
    m_headerIcon->setFixedSize(48, 48);
    m_headerIcon->setScaledContents(true);
    headerLine->addWidget(m_headerIcon);
    auto *headerText = new QVBoxLayout;
    headerText->setSpacing(2);
    m_headerTitle = new ElidedLabel(header);
    m_headerTitle->setText(instanceName);
    m_headerTitle->setObjectName(QStringLiteral("instanceHeaderTitle"));
    Theme::markDisplay(m_headerTitle, Theme::textFont(m_headerTitle->font(), Theme::TextRole::Display));
    // 长名先收字号（Display 26 → 20），再放不下才省略、悬停看全名；高度按 26 钉死，页眉不随名字变高矮。
    m_headerTitle->setFitPixelRange(Theme::textSize(Theme::TextRole::Display), kHeaderTitleMinPixelSize);
    headerText->addWidget(m_headerTitle);
    m_headerMeta = new QLabel(header);
    m_headerMeta->setObjectName(headerMetaName());
    {
        QPalette pal = m_headerMeta->palette();
        pal.setColor(QPalette::WindowText, Theme::tokens().secondary);
        m_headerMeta->setPalette(pal);
    }
    m_headerMeta->hide();  // 由 setHeaderMeta() 填；没填就不占行
    headerText->addWidget(m_headerMeta);
    headerLine->addLayout(headerText, 1);
    auto *folder = new QToolButton(header);
    folder->setObjectName(QStringLiteral("instanceHeaderFolder"));
    folder->setAutoRaise(true);
    folder->setIcon(VersionIcons::line(QStringLiteral("folder"), 18, Theme::tokens().text));
    folder->setIconSize(QSize(18, 18));
    folder->setToolTip(tr("打开实例目录"));
    connect(folder, &QToolButton::clicked, this, [this]() { emit openInstanceFolderRequested(m_instanceDir); });
    headerLine->addWidget(folder);
    outer->addWidget(header);

    // ---------- 两栏（见 .hpp 里 pinnedPageKeys() 上面那一段）----------
    auto *columns = new QHBoxLayout;
    columns->setSpacing(kColumnSpacing);

    // 左栏那一列：**内容不在这儿填** —— 它是按这个实例算出来的（哪几页用得上 + 用户勾掉了
    // 哪几页），构造末尾的 rebuildEntryList() 才灌第一遍。这儿只把"长什么样"摆好。
    //
    // NOTE: **不设** rowRefreshRole()。那一格里画的刷新圈是下载页专有的（这一列没有
    // "重新拉一遍"这回事），代理见到没设就不画。
    m_pageList = new QListWidget(this);
    m_pageList->setObjectName(pageListName());
    m_pageList->setUniformItemSizes(false);
    m_pageList->setItemDelegate(new VersionTreeDelegate(m_pageList));
    // 悬停底色由代理画（`Theme::rowHoverFill()`），但那一位 `State_MouseOver` 是**视图**填的。
    // 少了这一句，代理里那段一个字都不会生效 —— 不报错、断言也不一定先红，屏幕上就是"悬停没反应"。
    m_pageList->setMouseTracking(true);
    QFont listFont = m_pageList->font();
    listFont = Theme::textFont(listFont, Theme::TextRole::Body);
    m_pageList->setFont(listFont);
    // 只为一件事：**别让"拿到焦点"变成一次翻页**（见 eventFilter() 上面那一整段）。
    m_pageList->installEventFilter(this);
    auto *launchInstance = new QPushButton(launchInstanceText(), this);
    launchInstance->setObjectName(launchInstanceButtonName());
    launchInstance->setToolTip(launchInstanceHint());
    launchInstance->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLine->addWidget(launchInstance);
    Theme::markButton(launchInstance, Theme::ButtonKind::Primary);
    connect(launchInstance, &QPushButton::clicked, this,
            [this]() { emit launchInstanceRequested(); });
    m_pageList->hide(); // 保留 objectName 供验证工程查；界面导航统一由树承担。
    m_navigationTree = new QTreeWidget(this);
    m_navigationTree->setObjectName(navigationTreeName());
    m_navigationTree->setHeaderHidden(true);
    m_navigationTree->setColumnCount(1);
    m_navigationTree->setIndentation(16);
    m_navigationTree->setFixedWidth(navigationTreeWidth());
    m_navigationTree->setMinimumHeight(0);
    m_navigationTree->setFrameShape(QFrame::NoFrame);
    // 透明：左栏直接落在整窗磨砂底上（不另画一块面板）。
    m_navigationTree->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; }"));
    m_navigationTree->viewport()->setAutoFillBackground(false);
    m_navigationTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_navigationTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                if (item != nullptr && !item->data(0, Qt::UserRole).toString().isEmpty())
                    showPage(item->data(0, Qt::UserRole).toString());
            });
    m_navigationTree->installEventFilter(this);
    // 组头那几行（「内容管理」「世界与联机」「创作工具」「维护」）：**整行**点下去就是
    // 展开 / 收起。原来只有最左边那颗小箭头管用 —— 组头不可选中、整行又没有别的动作，
    // 指着那行字点下去什么都不发生，用户只会以为界面坏了（截图点名要改的就是这一处）。
    // 接线口和实例树、下载页那几棵树是**同一处出处**：一次手势只算一下、点得快时后面
    // 每一发照样算、回车同一条路（见 ui/RowActivation.hpp）。
    //
    // NOTE: 叶子那几行**不归它管** —— 点一条叶子 = 选中它，翻页走上面那条
    // `currentItemChanged`（键盘上下键同一条路），这里再补一次就是同一页摆两遍。
    // 所以"这是不是开关行"的判据只有一条：有孩子。
    connectRowActivation(m_navigationTree,
                         [](QTreeWidgetItem *item) {
                             if (item != nullptr && item->childCount() > 0) {
                                 // 先算再做：`!isExpanded()` 必须在这发之前取，反过来的话
                                 // 读到的是已经被翻过的状态（和实例树那条同一个写法）。
                                 item->setExpanded(!item->isExpanded());
                             }
                         },
                         [](QTreeWidgetItem *item) {
                             return item != nullptr && item->childCount() > 0;
                         });
    addNavColumn(columns, m_navigationTree);
    QWidget::setTabOrder(m_navigationTree, launchInstance);

    m_stack = new QStackedWidget(this);
    // 由外层分配可用空间，避免隐藏页面的尺寸提示撑大整页。
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->setObjectName(stackName());
    columns->addWidget(m_stack, 1);
    // 左栏换页的动效不用接：`QStackedWidget` 换页默认就有 `pageAppear()`（`ui/DesignDefaults.hpp`）。
    // 两栏吃掉整页剩下的竖直空间（底下那排按钮按自己的高排）。
    outer->addLayout(columns, 1);


    // ---------- 堆叠页：先按 pageCount() 的顺序把每一格占住 ----------
    //
    // 页里的控件是**第一次切过去才建**的（见 ensurePage()）：原来打开这个对话框要
    // 290–993 ms，绝大部分是 23 页控件树（每页还各套一层滚动区），而用户一开就看见的
    // 只有概览。空位必须先占住 —— 堆叠页的下标是 stackPageIndexOf() 算出来的，页按
    // **顺序**占位。
    for (const QString &key : pinnedPageKeys()) addPageSlot(key);
    for (const QString &key : filePageKeys()) addPageSlot(key);
    for (const QString &key : lifecyclePageKeys()) addPageSlot(key);

    // 每一页怎么造，登记成 builder。灌值那几段跟着页走（`applyLaunchValues()` 就是原来
    // 构造函数里那一串 setChoiceCustom / setValue）：页晚建，值就晚灌一遍。
    m_pageBuilders.insert(overviewCategoryKey(), [this, instanceId] { setupOverviewPage(instanceId); });
    m_pageBuilders.insert(launchCategoryKey(),
                          [this, current, global, appendJvmArgs, warnJavaMismatch, windowModes] {
                              setupLaunchPage(global, appendJvmArgs, warnJavaMismatch, windowModes);
                              applyLaunchValues(current, global);
                          });
    for (const QString &key : filePageKeys())
        m_pageBuilders.insert(key, [this, key] { buildFilePage(key); });
    for (const QString &key : lifecyclePageKeys())
        m_pageBuilders.insert(key, [this, key] { buildLifecyclePage(key); });

    // ---------- 左栏那一列的接线 ----------
    //
    // 左栏一变：换页。就这一件事（见 .hpp 里 pinnedPageKeys() 那一段）。
    //
    // NOTE: 越界保护的理由和设置页那条一样 —— 左栏的条目和堆叠页要是对不上，宁可停在原来
    // 那页，也不要 setCurrentIndex(-1) 把右栏整片清空（"点一下界面就空了"用户只会
    // 当成崩了）。
    connect(m_pageList, &QListWidget::currentRowChanged, this,
            [this](int row) { applyEntry(row); });
    // 打开就是第一条（概览）—— 左栏亮着哪一条、右栏就是哪一页，两边不能各说各的。
    // 那一列的内容和这一发都由构造末尾的 rebuildEntryList() 办（它是**算出来的**，
    // 见 .hpp 里 pinnedPageKeys() 那一段）。

    // ---------- 灌值 ----------
    // 「有值 = 自定义，没值 = 跟随全局」这一条两边都要对上：磁盘上有一项覆盖时，
    // 重开这个对话框必须停在「自定义」上，否则用户会以为自己的设置丢了。
    //
    // 启动页那几项（Java 路径 / 内存 / 额外参数 / 窗口模式与大小）的灌值搬进了
    // `applyLaunchValues()`：那一页是第一次切过去才建的，值就跟着页一起灌。概览页那几行
    // 不在这儿 —— 它一开就建好了。

    // 图标那一行的初值走 setIcon()（调用方在后头调，见 .hpp 里那段"为什么用 setter"）。
    // 下面 ensurePage() 建出概览页之后才画得了：没被 setIcon() 叫过的时候（验证工程里
    // 那些只验结构的构造点）八颗按钮也得是**八颗都没勾**的样子，而不是停在随机状态上。

    // ---------- 「改了就写」那套 ----------
    //
    // NOTE: 有一个例外：**「配置文件」那一格里改过还没保存的时候**，
    // `reject()` 会把这一关挡下来先问一句（见那边）。那一格是这一页上唯一一处
    // "改动没有实时落盘"的地方（它的「保存」是一颗按钮），所以也只有它需要问。
    //
    // NOTE: **这个对话框自己从不 accept**（没有「确定」那一类默认按钮）。收页时读的是
    // `QDialog::result()`（`pageClosed()` 那一发，和原来 `execPage()` 的返回值同一套），
    // 这里永远是 Rejected —— 而那正是"这一页关掉了"的意思，收尾不看它做什么。
    // 页眉、左右两栏那几颗控件先接上（那会儿概览页还没建，它自己在下一条里接）。
    watchEditors();
    // **默认那一页当场建**：打开对话框停的就是概览，它必须先有东西可摆。其余页
    // 第一次切过去才建（见 ensurePage()）。
    ensurePage(overviewCategoryKey());
    // 「备份与操作」那一页要**当众搬走**概览上那几颗按钮（快照 / 复制 / 导出 / 删除，
    // 见 buildLifecyclePage() 里 operationsmenu 那一段）：它不建，那几颗就留在概览上，
    // 一开对话框就多出一排。这一页只有几行字和三排按钮，建起来很便宜。
    ensurePage(QStringLiteral("operationsmenu"));
    // 图标那一行按"默认"画一遍 —— 要有概览页那八颗按钮才画得出来，所以排在它后面。
    refreshIconRow();
    // 左栏那一列灌第一遍（内容和第一页都归它管）。**排在 watchEditors() 之后**：它会在
    // 那一列上挑一条（程序化 setCurrentRow()），接上接线之后才走"选中 → 换页"那条正常路。
    rebuildEntryList();
    // 起点 = 现在这一份（灌值那一串 setChecked/setText 都已经跑完）。不记这一笔的话，
    // 打开页面之后的第一次交互会把"整份值"当成一次改动写一遍盘。
    m_lastSignature = valueSignature();
}

bool InstanceSettingsDialog::leaveBlockedByUnsavedEdits() const {
    // 这两条**就是** reject() 里那两道闸，一个字的第二种写法都不许有（见 .hpp）。
    if (m_configEditor != nullptr && m_configEditor->isDirty()) {
        return true;
    }
    if (m_worldSettings != nullptr && m_worldSettings->isDirty()) {
        return true;
    }
    return false;
}

QStringList InstanceSettingsDialog::droppedFilePaths(const QMimeData *mime) {
    QStringList paths;
    if (mime == nullptr || !mime->hasUrls()) {
        return paths;
    }
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        // 只要**本地文件**：`toLocalFile()` 对 http(s) 链接、`data:` 之类一律回空串。
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        // 目录不收（拖文件夹进来要另说一件事：是"把里面所有 jar 都装进去"还是别的），
        // 不存在的东西也不收 —— 半路被人移走了的话，后面那条复制路会报一句莫名其妙的话。
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            continue;
        }
        paths.append(path);
    }
    return paths;
}

void InstanceSettingsDialog::dragEnterEvent(QDragEnterEvent *event) {
    // 本进程里拖出来的文件不接（见 MainWindow::dragEnterEvent 同一条理由）。
    if (event->source() != nullptr) {
        event->ignore();
        return;
    }
    // 准入判据就一条：拖的是不是**本地文件**（见 droppedFilePaths()）。
    // 不认的话**什么都不做**（不 accept）—— 于是光标是"禁止"那个形状，用户一眼知道
    // 这一页不收它，而不是拖上去松手之后什么都没有发生。
    if (droppedFilePaths(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void InstanceSettingsDialog::dropEvent(QDropEvent *event) {
    if (event->source() != nullptr) {
        event->ignore();
        return;
    }
    const QStringList paths = droppedFilePaths(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    // 判断与落盘全在 MainWindow（见 .hpp）。NOTE: 这里**不能**顺手把事件也交给基类 ——
    // 交出去之后父链上再有人接一发，同一个文件会被装两遍。
    emit filesDropped(paths);
}

void InstanceSettingsDialog::reject() {
    // 走这条路的**不是**编辑器自己那颗「返回」（那颗发 backRequested()、由 MainWindow
    // 接到 leaveConfigEditor() 上），而是**覆盖层页眉那颗返回箭头** —— 它接的是
    // `dialog->reject()`（见 OverlayHost::showPage 里那段），点一下关掉的是**整个
    // 「实例设置」**。而那一格里可能正压着改了一半的东西。
    //
    // 这一层没有覆盖层、弹不出"要丢掉吗"那一句，所以只能**先挡住这一关**、把这一发交
    // 出去（见 .hpp 里 reject() 那一段）。接的人问完，用户答"丢掉"之后拿
    // `done(QDialog::Rejected)` 关 —— 绕开这儿，不然又回到"问一句、再问一句"。
    if (m_configEditor != nullptr && m_configEditor->isDirty()) {
        emit closeBlockedByUnsavedConfigFile();
        return;
    }
    // NOTE: 两条**分开问、不合并**：两格里同时压着东西的情况不存在（这两格各占一个
    // QStackedWidget，同一时刻只可能露出其中一格），而合并成一句只会让"丢掉的是
    // 哪一份"变得说不清。
    //
    // 这两条判据挪进了 `leaveBlockedByUnsavedEdits()`，只因为**还有第二个
    // 人要在做同一件事之前先问一句**（拖进来一个整合包 ⇒ 程序自己发起的那次关页，
    // 见 MainWindow）。两处各写一遍"哪两条算脏"迟早会分岔。这里只把"是哪一格脏"翻成
    // 两条不同的信号 —— 那件事仍然只有这一处知道。
    if (m_configEditor != nullptr && m_configEditor->isDirty()) {
        emit closeBlockedByUnsavedConfigFile();
        return;
    }
    if (m_worldSettings != nullptr && m_worldSettings->isDirty()) {
        emit closeBlockedByUnsavedWorldSettings();
        return;
    }
    QDialog::reject();
}

QString InstanceSettingsDialog::titleText() const {
    return tr("实例设置");
}

void InstanceSettingsDialog::setInstanceName(const QString &name) {
    m_instanceId = name;
    if (m_instanceNameEdit == nullptr) {
        return;
    }
    // **不当成"用户改了这一格"**：程序化 `setText()` 本来也不发 editingFinished，这里
    // 也不需要挡什么 —— 这一句只是让控件和"磁盘上现在叫什么"重新对上（改名成功之后是
    // 新名字，失败之后退回老名字）。
    m_instanceNameEdit->setText(name);
}

void InstanceSettingsDialog::setInstanceDisplayName(const QString &displayName) {
    if (m_headerTitle == nullptr) return;
    m_headerTitle->setText(displayName);
    m_headerTitle->refitFont();
}

void InstanceSettingsDialog::setHeaderMeta(const QString &meta) {
    if (m_headerMeta == nullptr) return;
    m_headerMeta->setText(meta);
    m_headerMeta->setVisible(!meta.isEmpty());
}

QString InstanceSettingsDialog::instanceName() const {
    // 现场读控件（和 override() 一样，没有影子状态）：验证工程摆好那一格直接读它。
    // **不 trim**：去掉空格是多一层判断，而"只有空格差别"那一档由调用方（MainWindow）
    // 按 trimmed() 之后的结果比 —— 那是它的事，这里原样给。
    return m_instanceNameEdit != nullptr ? m_instanceNameEdit->text() : QString();
}

// ---------- 分栏（见 .hpp 里 pinnedPageKeys() 上面那一段）----------

QWidget *InstanceSettingsDialog::addPageSlot(const QString &key) {
    auto *slot = new QWidget(m_stack);
    slot->setObjectName(QStringLiteral("instanceSettingsSlot-") + key);
    m_stack->addWidget(slot);
    return slot;
}

QWidget *InstanceSettingsDialog::pageSlot(const QString &key) const {
    const int index = stackPageIndexOf(key);
    if (m_stack == nullptr || index < 0 || index >= m_stack->count()) {
        return nullptr;
    }
    return m_stack->widget(index);
}

void InstanceSettingsDialog::ensurePage(const QString &key) {
    // **不在这里改键**：并进去的那几个老页键（`maintenancePageKey()` 那张表）在堆叠页里
    // 仍有自己那一格，跳页的调用方（`showPage()` / `applyPageKey()`）早就换成目标键了。
    // 在这里再换一次，那几个老键自己那一页就永远建不出来 —— 而"建得出来、只是没人摆"正是
    // 那一格的意思（验证工程按 panelName() 认它）。
    const QString pageKey = key;
    if (pageKey.isEmpty() || m_builtPages.contains(pageKey)) {
        return;
    }
    const auto it = m_pageBuilders.find(pageKey);
    if (it == m_pageBuilders.end()) {
        return;
    }
    const auto build = it.value();
    // 先记成"建过"再跑：builder 里可能绕回 applyPageKey()（比如建「存档」时那颗
    // 「世界备份」接的是 showPage()），那时不能再进来一次造第二份。
    m_pageBuilders.erase(it);
    m_builtPages.insert(pageKey);
    {
        // 这一页的建造成本 = "第一次切过去的首帧"那一笔的大头。超 20 ms 就记一行 ——
        // 预算 50 ms（同 `[UI timing] lifecycle page refresh` 那几条一样的写法）。
        QElapsedTimer buildClock;
        buildClock.start();
        build();
        if (buildClock.elapsed() > 20) {
            qInfo() << "[UI timing] settings page build" << pageKey << buildClock.elapsed() << "ms";
        }
    }
    if (!filePagesPending()) {
        // 「文件管理」那几页**全建完了**：留给它们补灌的那几份就用不上了（见 filePagesPending()）。
        m_pendingFileIcons.clear();
        m_pendingFileResources.clear();
        m_hasPendingUpdates = false;
        m_pendingOutdatedPaths.clear();
    }
    // 新造出来的控件要接上"改了就写"那几条线 —— 构造末尾那一次 watchEditors() 扫不到它们。
    //
    // NOTE: 扫的是**整个对话框**，不是这一页。有几样浮层是挂在对话框自己身上的（概览页
    // 「更改图标」那一排候选、「分组…」那几行 —— `MenuPopup(this)` 的 this 是对话框，
    // 不是页），只扫这一页就漏掉它们：点一下图标候选，值变了却没人发 settingsEdited，
    // 盘上一直不动（T18⑥ 钉的就是这一条）。`watchPage()` 里那几条线都是
    // Qt::UniqueConnection，重复扫同几个控件不会再接一遍，所以这里每建一页都扫一遍整个对话框是安全的。
    watchPage(this);
    // 谁要在这一页的控件上接线，等的是这一发（见 .hpp 里 pageBuilt()）。
    emit pageBuilt(pageKey);
}

QWidget *InstanceSettingsDialog::addPage(const QString &key) {
    // **一页一层滚动区**（不是整页套一层）—— 和设置页的 addSettingsPanel() 同一套写法、
    // 同一条理由（见 MainWindowUI.cpp 里那段）。
    //
    // NOTE: 页面自己是 `scroll->widget()`（objectName 还是 panelName(key)）—— 堆叠页里
    // 加进去的仍然是**一页一个控件**，所以那些按 index 认页的代码（filePageFirstIndex()
    // 那套）一位都不动。验证工程按 panelName() 找回来的也还是那一页。
    //
    // NOTE: 外层那一格是 addPageSlot() 在构造里挖的空位 —— 堆叠页的下标那时就定好了，
    // 这一页里的东西是第一次切过去才往里填（见 ensurePage()）。
    QWidget *slot = pageSlot(key);
    if (slot == nullptr) {
        return nullptr;
    }
    auto *scroll = new Theme::GutterScrollArea(slot);
    scroll->setObjectName(scrollName(key));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 横向永远不滚：页面跟着右栏宽度收，控件自己省略 / 换行（UI问题 #6：本该放得下的地方不许出横滑条）。
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *page = new QWidget(scroll);
    page->setObjectName(panelName(key));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    scroll->setWidget(page);
    auto *slotLayout = new QVBoxLayout(slot);
    slotLayout->setContentsMargins(0, 0, 0, 0);
    slotLayout->setSpacing(0);
    slotLayout->addWidget(scroll);
    return page;
}

QGridLayout *InstanceSettingsDialog::addForm(const QString &key, QWidget *page) {
    auto *form = new QWidget(page);
    form->setObjectName(formName(key));
    auto *grid = new QGridLayout(form);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    // 和设置页栏内那一层同一套比例（两边每一行都是上下结构、每格跨两列，所以这一对
    // 份数在两边都只摆表格的形状，见各自那张 grid 上面那段说明）。
    grid->setColumnStretch(0, MainWindowUI::leftColumnStretch());
    grid->setColumnStretch(1, MainWindowUI::rightColumnStretch());
    if (auto *layout = qobject_cast<QVBoxLayout *>(page->layout())) {
        layout->addWidget(form);
        // 表格底下留一段空白：内容少的时候别把它抻开（几行字被拉开行距看着像坏了）。
        layout->addStretch(1);
    }
    return grid;
}

int InstanceSettingsDialog::navListWidth(const QListWidget *list) {
    // 这一列的宽度**不随"现在显示着哪几页"变**：只按表里**这一刻**有的那几条算的话，
    // 用户在「高级选项」里勾掉「结构/原理图」（左栏里最宽的那一条）之后，这一列当场窄掉
    // 十来像素、**右栏整幅跟着横着跳一下**；同一个对话框在两个实例上开出来也会不一样宽
    // （"用不上的那一页"本身就不在表里）。宽度是这一页的**骨架**，骨架不该跟着内容晃。
    // 所以把"这一页随时可能变出来的那几条"也算进来（钉死的那两条 + `optionalPageKeys()`
    // 那七条，与这一刻勾没勾、可不可用都无关）——**同一个对话框永远是同一个宽度**，
    // 而且只会更宽、绝不会窄（多出来那几条里就有最宽的那一条）。
    //
    // NOTE: 这一列里"装饰"占掉的那一块（样式给条目的边距）由表自己量出来，再用同一个数去
    // 估不在表里的那几条 —— 别写死一个像素数，换字体/换样式就不是那个数了。
    // 这里的字度量必须和**代理量行宽时用的那个字体**一致。
    // 左边这一列用的是 `VersionTreeDelegate`，它给**顶层条目**（这一列全是顶层）换的是
    // **粗体**（见 `VersionTreeDelegate::sizeHint` 那句 `if (!index.parent().isValid())`）。
    // 列表自己的字体不是粗体：用它去量，`decoration = sizeHintForColumn − 最宽那条的文字宽`
    // 里就会混进「粗体比普通体宽出来的那一点」；而"最宽的那一条"在两种字体下**可能不是
    // 同一条** ⇒ 勾掉几页之后 decoration 差个把像素、右栏跟着横着抖 1 px。两边都在粗体下
    // 标量之后，差值就剩下一圈**纯粹的样式装饰**（常数），这一列的宽度只由"一共有哪些页"
    // 决定。
    QFont rowFont = list->font();
    rowFont.setBold(true);
    const QFontMetrics fm(rowFont);
    // 一列（QListWidget 只有第 0 列），而 `sizeHintForColumn(0)` 量的就是**所有行里最宽的
    // 那一条** ⇒ 这里**一次**调用就够；别学 QTableWidget 那种 `for (i < count())` 的写法
    // （列号越界时那个函数回 -1，被 `qMax` 吃掉，结果一样，但看着像在数行）。
    const int measured = list->sizeHintForColumn(0);
    int width = qMax(0, measured);
    int widestShownText = 0;
    for (int i = 0; i < list->count(); ++i) {
        widestShownText = qMax(widestShownText, fm.horizontalAdvance(list->item(i)->text()));
    }
    const int decoration = qMax(0, measured - widestShownText);
    const QStringList everyKey = pinnedPageKeys() + optionalPageKeys();
    for (const QString &key : everyKey) {
        width = qMax(width, fm.horizontalAdvance(pageText(key)) + decoration);
    }
    return width + kNavListExtraWidth;
}

QString InstanceSettingsDialog::pageText(const QString &key) {
    if (key == QStringLiteral("launch")) return tr("启动设置");
    if (key == QStringLiteral("schematic")) return tr("原理图");
    const int lifecycleIndex = lifecyclePageKeys().indexOf(key);
    if (lifecycleIndex >= 0) return lifecyclePageTexts().value(lifecycleIndex);
    // 两张按下标对齐的表各查一遍（键表是**身份**，别拿文字去反查）。
    const QStringList pinned = pinnedPageKeys();
    const int pinnedIndex = pinned.indexOf(key);
    if (pinnedIndex >= 0) {
        return pinnedPageTexts().value(pinnedIndex);
    }
    const int fileIndex = filePageKeys().indexOf(key);
    if (fileIndex >= 0) {
        return filePageTexts().value(fileIndex);
    }
    return QString();
}

QStringList InstanceSettingsDialog::shownEntryKeys() const {
    // 左栏那一列的**唯一出处**：钉死的那两条 + 七条里"用得上且没被用户勾掉"的。
    // 判断本身在 `InstanceSettingsPageChoices::visible()`（纯函数，U9d 钉着）。
    return InstanceSettingsPageChoices::visible(pinnedPageKeys(), optionalPageKeys(),
                                                m_pageAvailable, m_pageOverrides);
}

QJsonObject InstanceSettingsDialog::pageChoices() const {
    // 存下来的**只可能是 false**（见 .hpp 里那一段）：勾选框只在那一页可用时能勾，而
    // 不可用就没得选 —— 所以这一格是"藏起来的那几页"的名单，不是一份对称的选择表。
    QJsonObject out = m_pageOverrides;
    for (const QString &key : optionalPageKeys()) {
        // 每条先按"这一刻的样子"重算一遍：可用但没勾 ⇒ 记 false；其余（可用且勾着、
        // 或者这条根本用不上）⇒ 一个键都不写。这么写，"勾上"就是把键去掉。
        out.remove(key);
        const QCheckBox *check = m_pageChecks.value(key);
        if (check != nullptr && m_pageAvailable.value(key, true) && !check->isChecked()) {
            out.insert(key, false);
        }
    }
    return out;
}

void InstanceSettingsDialog::setPageChoices(const QJsonObject &stored) {
    // 只收认得出来的键（前向兼容：以后删掉一页之后，盘上那条不该跟着在这儿复活）。
    // 归一化本身在 `InstanceSettingsPageChoices::normalized()`（纯函数，U9d 钉着）。
    m_pageOverrides = InstanceSettingsPageChoices::normalized(stored, optionalPageKeys());
    rebuildEntryList();
    m_lastSignature = valueSignature();
}

void InstanceSettingsDialog::applyEntry(int row) {
    const QListWidgetItem *item = m_pageList->item(row);
    if (item == nullptr || item->data(Qt::UserRole).toString().isEmpty()) {
        return;
    }
    applyPageKey(item->data(Qt::UserRole).toString());
}

void InstanceSettingsDialog::applyPageKey(const QString &requestedKey) {
    const QString key = maintenancePageKey(requestedKey);
    const int index = stackPageIndexOf(key);
    if (index < 0) {
        return;
    }
    // 这一页第一次切过来 ⇒ **现在就建**（打开对话框那一刻只建了概览）。建完堆叠页那一格里
    // 才有东西可摆 —— 构造里挖的空位自己什么都不画。
    ensurePage(key);
    // **"进这一页"和"同一页再摆一次"是两件事**：showPage() 那条路里"选中那一条"这一发
    // 底下不该再做一遍。refresh() 重复一次只是白列一遍目录（还好，而且"再摆一次就重新读
    // 一遍盘"本来就是这一页要的），但 serverPageEntered 多发一次是**白连一轮**（用户看到
    // 的是"每次翻页都多等一次超时"）—— 所以挡的是后者。
    // 用户手里碰不到这一位：点同一条时 QListWidget 本来就不发信号（见 .hpp 那条 NOTE）。
    const bool alreadyHere = m_stack->currentIndex() == index;
    m_stack->setCurrentIndex(index);
    refreshLifecycleFiles(key);
    if (key == QStringLiteral("diagnosis")) emit diagnosisRequested();
    if (key == QStringLiteral("migration") || key == QStringLiteral("upgrade")) {
        const QString targetId = transferTarget(key);
        if (key == QStringLiteral("upgrade") || !targetId.isEmpty()) emit transferTargetChosen(key, targetId);
    }
    if (key == QStringLiteral("resourcepack") && m_resourcePackPage != nullptr) {
        auto *stack = qobject_cast<QStackedWidget *>(m_resourcePackPage->parentWidget());
        if (stack != nullptr && stack->currentWidget() == m_resourcePackPage)
            emit resourcePackPageEntered();
    }
    {
        const QSignalBlocker blocker(m_navigationTree);
        m_navigationTree->setCurrentItem(nullptr);
        QString navigationKey = key;
        if (key == QStringLiteral("worldbackup")) navigationKey = QStringLiteral("save");
        if (key == QStringLiteral("crash")) navigationKey = QStringLiteral("logs");
        for (QTreeWidgetItemIterator it(m_navigationTree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == navigationKey) {
                m_navigationTree->setCurrentItem(*it);
                for (auto *parent = (*it)->parent(); parent != nullptr; parent = parent->parent())
                    parent->setExpanded(true);
                m_navigationTree->scrollToItem(*it);
                break;
            }
        }
    }
    // 切到这一页就**当场重列一遍**：打开这个对话框的这段时间里，盘上那些文件夹完全可能
    // 被别的东西动过（游戏正在写日志、用户自己开了个资源管理器）。列目录只有读，很便宜，
    // 而"我看见的和盘上不一样"是这一页最没道理的一种错。
    if (InstanceFilesPage *page = m_filesPages.value(key)) {
        page->refresh();
    }
    // 点进去就自动测速。**排在 refresh() 之后**：这一页刚露出来
    // 时先有那张表（表里那几行是这一页本来就有的东西），再谈"连上去问一遍"。
    //
    // NOTE: 这一发走的是**键**不是行号 ⇒ 左栏那一列怎么变（勾掉一条、可用性变了）都不影响
    // 它：只要有人摆到「服务器列表」那一页上，它就该发。
    if (key == QStringLiteral("mod") && !alreadyHere) {
        QTimer::singleShot(0, this, [this] { emit checkUpdatesRequested(); });
    }
    if (key == serverPageKey() && !alreadyHere) {
        emit serverPageEntered();
    }
}

void InstanceSettingsDialog::rebuildEntryList() {
    // 现在摆着的是哪一页（重建之后要把选中还原回去）。堆叠页是唯一那份"现在是哪一页"，
    // 左栏这一列只是它的画 —— 所以从它反查，别自己存一份。
    const QString currentKey = pageKeyAtStackIndex(m_stack->currentIndex());
    const QStringList keys = shownEntryKeys();
    rebuildNavigationTree();
    {
        // 重灌这一列时的每一句都是"程序化改控件"，压着信号：选中的还原由下面那一发
        // applyPageKey() 自己办（`setCurrentRow()` 在"本来就亮着这一条"时不发信号，
        // 光靠信号会漏）。
        const QSignalBlocker blocker(m_pageList);
        m_pageList->clear();
        const QList<QPair<QString, QStringList>> groups = {
            {tr("基础设置"), pinnedPageKeys()},
            {tr("内容管理"), {QStringLiteral("mod"), QStringLiteral("resourcepack"),
                              QStringLiteral("datapack"), QStringLiteral("shader")}},
            {tr("世界与联机"), {QStringLiteral("save"), QStringLiteral("server")}},
            {tr("创作工具"), {QStringLiteral("schematic")}}
        };
        for (const auto &group : groups) {
            QStringList visible;
            for (const QString &key : group.second) {
                if (keys.contains(key)) visible.append(key);
            }
            if (visible.isEmpty()) continue;
            auto *heading = new QListWidgetItem(group.first, m_pageList);
            heading->setFlags(Qt::NoItemFlags);
            heading->setForeground(palette().color(QPalette::PlaceholderText));
            heading->setSizeHint(QSize(0, 28));
            for (const QString &key : visible) {
                auto *item = new QListWidgetItem(QStringLiteral("  ") + pageText(key), m_pageList);
                item->setData(Qt::UserRole, key);
            }
        }
        // 尺寸**每次都重算** —— 这一列的内容是会变的（勾掉一条就少一格），照抄构造时
        // 那份宽高会留下"标签被裁掉"或者"右边空一大截"。行高**交给样式算**
        // （sizeHintForRow），别写死数字：换字体/换样式就错。和设置页那一列同一条规矩。
        // 导航随可用高度滚动，分组再多也不能把底部启动按钮挤出窗口。
        m_pageList->setMinimumHeight(0);
        m_pageList->setMaximumHeight(QWIDGETSIZE_MAX);
        m_pageList->setFixedWidth(navListWidth(m_pageList));
        // 当前那一页还在表里就选回去；不在（被勾掉 / 变不可用 / 本来就没有条目）就是 -1。
        int selectedRow = -1;
        for (int i = 0; i < m_pageList->count(); ++i) {
            if (m_pageList->item(i)->data(Qt::UserRole).toString() == currentKey) {
                selectedRow = i;
                break;
            }
        }
        m_pageList->setCurrentRow(selectedRow);
    }
    refreshPageChecks();

    // 选中那一发要么没发（行号本来就对）、要么压根没有那一条，所以这儿补一次：
    // · 当前那一页还在表里 ⇒ 按它换一次页（和自己点了那一条是同一条路）；
    // · 当前是「配置文件」⇒ **什么都不做**：它本来就是一个没有条目的页（用户
    // 说收起来），重建（比如可用性变了一下）不该把用户从那一页上赶走；
    // · 其余（那一页被勾掉 / 变不可用了）⇒ 回概览：右栏摆着一页、左栏里却没有它，
    // 是最容易让人以为"点坏了"的一种状态。
    if (keys.contains(currentKey)) {
        applyPageKey(currentKey);
    } else if (currentKey == configPageKey() || lifecyclePageKeys().contains(currentKey)) {
        applyPageKey(currentKey);
        return;
    } else {
        for (int i = 0; i < m_pageList->count(); ++i) {
            if (!m_pageList->item(i)->data(Qt::UserRole).toString().isEmpty()) {
                m_pageList->setCurrentRow(i);
                break;
            }
        }
        if (!keys.isEmpty()) {
            applyPageKey(keys.first());
        }
    }
}

bool InstanceSettingsDialog::eventFilter(QObject *watched, QEvent *event) {
    // ---- 左栏那一列不许因为"拿到焦点"就自己挑一条 ----
    //
    // 这是 Qt 的一个**兜底行为**，不是我们写的：`QAbstractItemView::focusInEvent()` 里
    // 有这么一段（6.10.1 的 qabstractitemview.cpp）——
    //
    // if (model && !d->currentIndexSet && !currentIndexValid) {
    // QModelIndex index = moveCursor(MoveNext, ...); // 第一条
    // if (index.isValid() && ... && event->reason() != Qt::MouseFocusReason)
    // selectionModel()->setCurrentIndex(index, NoUpdate);
    // }
    //
    // 也就是"**视图拿到焦点、而当前条目是无效的** ⇒ 替你选中第一条"。落到这一页上：
    // 「配置文件」是一个**没有左栏条目**的页（见 configPageKey()），摆到它上面时左栏
    // 选中的就是 -1；接下来用户按一下 Tab 进来、或者窗口重新激活（`ActiveWindowFocusReason`
    // —— 开关一次别的窗口回来就会走到），Qt 就把选中挑到第 0 条上，而这一列那一下
    // **是翻页**（`currentRowChanged` → applyEntry() → applyPageKey()）⇒ 右栏摆着配置文件，
    // 左栏自己跳回了概览。**没有用户意图的一次翻页**，正是这一整页最没道理的一种错。
    //
    // 鼠标点出来的焦点不在其列（上面那句 `reason() != Qt::MouseFocusReason` 挡着），
    // 所以只有键盘 / 窗口激活这条路会撞上 —— 而它照样是用户会走的路。
    //
    // NOTE: 吃掉这一发只是"不让它自己挑"，**焦点照旧落在它身上**（焦点是 Qt 在发这个事件
    // 之前就设好的）。按一下方向键时 `QListView::moveCursor()` 对"当前条目无效"本来
    // 就有兜底（回第一条），所以键盘一样能走，只是"没碰过它之前什么都不高亮"。
    // 判据带上 `!currentIndex().isValid()`：当前条目有效时 focusInEvent() 什么都不会做，
    // 那种时候这一发没有吃掉的道理。
    const auto autoPickPending = [](const QAbstractItemView *view) {
        return view != nullptr && !view->currentIndex().isValid();
    };
    if (event->type() == QEvent::FocusIn &&
        ((watched == m_pageList && autoPickPending(m_pageList)) ||
         (watched == m_navigationTree && autoPickPending(m_navigationTree)))) {
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void InstanceSettingsDialog::refreshPageChecks() {
    // 勾选框是**画**，不是用户改的 ⇒ 逐格压着信号摆（`setChecked()` 本来也不发 clicked ——
    // 写盘那条线接的是 clicked，见 watchEditors() —— 这里压一下是"以后万一有人给它接一条
    // 值变化就写盘的线"时的保险；左栏那一列在这儿**不碰**，别顺手把它的信号也压了）。
    for (const QString &key : optionalPageKeys()) {
        QCheckBox *check = m_pageChecks.value(key);
        if (check == nullptr) {
            continue;
        }
        const bool available = m_pageAvailable.value(key, true);
        // 不可用的那一条**不勾且勾不上**，原因走 tooltip（悬停文本解释）。灰着而不是
        // 藏起来：用户得看得见"有这么一页、这个实例上用不上、为什么"。
        const QSignalBlocker checkBlocker(check);
        check->setChecked(available && m_pageOverrides.value(key).toBool(true));
        check->setEnabled(available);
        check->setToolTip(available ? pageShownHint() : pageUnavailableText(key));
    }
}

// ---------- 文件管理那八页（见 .hpp 里 filePageKeys() 上面那一段）----------

QStringList InstanceSettingsDialog::shortcutTexts() {
    return {tr("版本目录"), tr("日志"), tr("游戏目录"), tr("崩溃报告"), tr("配置")};
}

QStringList InstanceSettingsDialog::shortcutPathKeys() {
    // 和 shortcutTexts() **逐位对齐**。这些键由 MainWindow 从 setInstanceDir() /
    // setFileDirs() 灌进来（它才知道那些目录在哪儿）。
    //
    // `game` / `crash` 两个**不在** `fileDirs()` 那九条里（那一份是"文件管理那几页"的
    // 目录，多两个进去会让那几页认错根）—— MainWindow 把它们**并进**灌给 setFileDirs()
    // 的那一份里（多出来的键没人读就是死数据，比改 instanceFileDirs() 的形状安全得多，
    // 那个函数还有一个断言在数它是九条）。`logs` / `config` 本来就在那九条里。
    return {QStringLiteral("dir"),        QStringLiteral("logs"),
            QStringLiteral("game"),       QStringLiteral("crash"),
            QStringLiteral("config")};
}

void InstanceSettingsDialog::setFolderPresentKeys(const QStringList &keys) {
    // 传进来的就是**在盘上的那些**（调用方问的盘）。不认识的键照收不管 —— 多一个键
    // 只是让它不参与任何一段（`shortcutPathKeys()` 里没有它，两段都轮不到它）。
    m_shortcutPresentKeys = keys;
    // 展开状态**不重置**：这个函数可能被调很多次（每次 openInstanceSettings 都会来一发），
    // 用户手点开的那一截不该因为这个缩回去。收着的那些位子本来就由 present 决定。
    relayoutShortcuts();
}

QList<QWidget *> InstanceSettingsDialog::shortcutHiddenButtons() const {
    QList<QWidget *> hidden;
    // 展开着就没有"收着"的了（它们在外面，只是下面那一段）。
    if (m_shortcutMoreExpanded) {
        return hidden;
    }
    const QStringList keys = shortcutPathKeys();
    for (int i = 0; i < keys.size(); ++i) {
        if (!m_shortcutPresentKeys.contains(keys.at(i))) {
            hidden.append(m_shortcutButtons.value(i));
        }
    }
    return hidden;
}

void InstanceSettingsDialog::toggleMoreShortcuts() {
    m_shortcutMoreExpanded = !m_shortcutMoreExpanded;
    relayoutShortcuts();
    // NOTE: **不发 settingsEdited()** —— 展开/收起只动这一页的摆法，一个字节都不进 wtfml.meta。
    // 发了的话，点一下「更多目录…」就等于把"实例设置改过"的账记上，关页时还会弹那道
    // "要保存吗"—— 而那里根本没有东西要保存。
}

/// 摆法那一份唯一出处（两段：外面 + 「更多目录」+ 收着的）。**建完按钮、以及每一次
/// present / 展开态变了之后**都走它。
///
/// NOTE: 收着的那些**照样在网格里**（只是 `setVisible(false)`），不是摘出去 —— 它们的次序
/// 在表里，也就是**永远排在最后那一段**（缺的键按表序补在外面的后面）。于是收起来
/// 的时候网格里看不见的那几格全在尾巴上，前面几行不会出现空洞。哪天这张表的缺项
/// 不再是"一段尾巴"（比如中间空一个），这一条就不成立了。
void InstanceSettingsDialog::relayoutShortcuts() {
    if (m_shortcutGrid == nullptr) {
        return;
    }
    // 先清网格：`takeAt()` 摘下来的那一项只是"摆法"，控件自己还挂在 m_shortcutCell 上。
    while (QLayoutItem *item = m_shortcutGrid->takeAt(0)) {
        if (auto *widget = item->widget(); widget && widget->objectName() == QStringLiteral("instanceActionGridFiller"))
            delete widget;
        delete item;
    }

    const QStringList keys = shortcutPathKeys();
    bool anyAbsent = false;
    for (const QString &key : keys) {
        if (!m_shortcutPresentKeys.contains(key)) {
            anyAbsent = true;
            break;
        }
    }

    // 「更多目录…」自己没有存在的理由时**整颗不建**（不摆一颗按下去什么都不变的按钮）。
    if (!anyAbsent) {
        m_shortcutMoreExpanded = false;
        delete m_shortcutMoreButton;
        m_shortcutMoreButton = nullptr;
    } else if (m_shortcutMoreButton == nullptr) {
        auto *more = new QPushButton(shortcutMoreText(), m_shortcutCell);
        Theme::markButton(more, Theme::ButtonKind::Neutral);
        more->setObjectName(shortcutMoreButtonName());
        more->setToolTip(tr("目录尚不存在，打开时自动创建。"));
        connect(more, &QPushButton::clicked, this, &InstanceSettingsDialog::toggleMoreShortcuts);
        m_shortcutMoreButton = more;
    }

    QList<QWidget *> ordered;
    // 第一段：现在真有东西的（表序）。
    for (int i = 0; i < keys.size(); ++i) {
        if (m_shortcutPresentKeys.contains(keys.at(i))) {
            if (QWidget *button = m_shortcutButtons.value(i)) {
                button->setVisible(true);
                ordered.append(button);
            }
        }
    }
    // 中间那一颗开关（有得收才摆）。
    if (m_shortcutMoreButton != nullptr) {
        m_shortcutMoreButton->setText(m_shortcutMoreExpanded ? shortcutMoreLessText()
                                                            : shortcutMoreText());
        m_shortcutMoreButton->setVisible(true);
        ordered.append(m_shortcutMoreButton);
    }
    // 第二段：现在还没有的那些（表序）。展开着就摆在外面，收着就只占格子。
    for (int i = 0; i < keys.size(); ++i) {
        if (!m_shortcutPresentKeys.contains(keys.at(i))) {
            if (QWidget *button = m_shortcutButtons.value(i)) {
                button->setVisible(m_shortcutMoreExpanded);
                ordered.append(button);
            }
        }
    }
    // 定高 / 最窄 / 横向可抻 都在这一手里（幂等，重摆几次都一样）。
    layoutActionButtons(m_shortcutGrid, ordered);
}

QString InstanceSettingsDialog::filesHint(const QString &key) {
    if (key == QStringLiteral("shortcut")) return tr("打开实例目录及相关文件夹。");
    if (key == QStringLiteral("schematic") || key == QStringLiteral("structure")) return tr("查看投影和结构文件。");
    if (key == QStringLiteral("config")) return tr("双击文件或按 Enter 编辑，双击文件夹进入。");
    if (key == QStringLiteral("save")) return tr("管理世界、备份和导入存档。");
    if (key == QStringLiteral("server")) return tr("编辑服务器名称和地址；右键地址可复制。");
    return QString();
}

QString InstanceSettingsDialog::filesEmptyText(const QString &key) {
    if (key == QStringLiteral("mod")) {
        return tr("暂无模组，可通过“%1”添加。").arg(filesDownloadText(key));
    }
    if (key == QStringLiteral("resourcepack")) {
        return tr("暂无资源包，可通过“%1”添加。").arg(filesDownloadText(key));
    }
    if (key == QStringLiteral("datapack")) {
        return tr("暂无数据包，可通过“%1”添加。").arg(filesDownloadText(key));
    }
    if (key == QStringLiteral("shader")) {
        return tr("暂无光影，可通过“%1”添加。").arg(filesDownloadText(key));
    }
    if (key == QStringLiteral("schematic")) {
        return tr("暂无结构或原理图");
    }
    if (key == QStringLiteral("config")) {
        // 「还没有」在这里有**两种**原因，都说得出口：这个实例从来没进过游戏（模组还没
        // 跑过一次，也就还没写配置），或者它确实一个能配的模组都没有。第二种更常见 ——
        // 原版客户端本来就没有 config 这个目录。
        return tr("暂无配置文件，启动游戏后可自动生成。");
    }
    if (key == QStringLiteral("save")) {
        return tr("暂无存档");
    }
    if (key == QStringLiteral("server")) {
        return serverEmptyText();
    }
    return QString();
}

QString InstanceSettingsDialog::shaderUnavailableText() {
    return tr("使用光影需要 Iris 或 OptiFine，请先安装兼容版本。");
}

QString InstanceSettingsDialog::schematicUnavailableText() {
    return tr("使用原理图需要安装 Litematica 等兼容模组。");
}

QString InstanceSettingsDialog::modUnavailableText() {
    return tr("当前实例未安装模组加载器，无法加载 mods 目录中的模组。");
}

QString InstanceSettingsDialog::datapackUnavailableText() {
    return tr("当前游戏版本不支持数据包，需要 Minecraft 1.13 或更新版本。");
}

QString InstanceSettingsDialog::filesDownloadText(const QString &key) {
    // 「光影包」那一页上写的是「下载新光影」—— 这四个字要和
    // **落地那一栏**对得上：下载页左栏那一栏叫「光影」（MainWindowUI::downloadCategoryTexts()），
    // 而实例设置左边那一列里这一页仍旧叫「光影包」。
    if (key == QStringLiteral("mod")) {
        return tr("下载新模组");
    }
    if (key == QStringLiteral("resourcepack")) {
        return tr("下载新资源包");
    }
    if (key == QStringLiteral("datapack")) {
        return tr("下载新数据包");
    }
    if (key == QStringLiteral("shader")) {
        return tr("下载新光影");
    }
    return QString();
}

QString InstanceSettingsDialog::filesDownloadTip(const QString &key) {
    const QString category = filesDownloadCategoryKey(key);
    if (category.isEmpty()) {
        return QString();
    }
    const int index = MainWindowUI::downloadCategoryKeys().indexOf(category);
    const QStringList texts = MainWindowUI::downloadCategoryTexts();
    if (index < 0 || index >= texts.size()) {
        return QString();
    }
    // 提示里得说清"这一页会关掉"：用户按它是为了**换个地方**，不是就地多出一个东西。
    return tr("打开“%1”下载页。").arg(texts.at(index));
}

QString InstanceSettingsDialog::filesDownloadCategoryKey(const QString &key) {
    // 认的就是这四个页键，回的**就是**下载页那一栏的键（见 .hpp 里那张说明：两套键现在
    // 恰好同名，可这张表是"这两处是一回事"的唯一出处）。
    if (key == QStringLiteral("mod") || key == QStringLiteral("resourcepack") ||
        key == QStringLiteral("datapack") || key == QStringLiteral("shader")) {
        return key;
    }
    return QString();
}

QString InstanceSettingsDialog::filesDownloadPageKey(const QString &categoryKey) {
    if (categoryKey.isEmpty()) {
        return QString();
    }
    for (const QString &key : filePageKeys()) {
        if (filesDownloadCategoryKey(key) == categoryKey) {
            return key;
        }
    }
    return QString();
}

bool InstanceSettingsDialog::filesNameOpensResource(const QString &key) {
    // 这四页（模组 / 资源包 / 数据包 / 光影包）出来的条目点得进详情页：匹配上了才点得动，
    // 模组也一样。名字点开和资源包三页走的是**同一条路**，唯一的分别落在那一页右上角
    // 那一格上（`MainWindow::DetailSource::InstanceFiles` + 种类是模组 ⇒ 摆「模组设置」，
    // 见 ResourceDetailPage::setModSettingsSlot）。
    //
    // 要接别的页时改的只有这一处（setFileResource() 那个循环和构造里那条 connect 都读它）。
    //
    // NOTE: 原理图 / 存档 / 配置文件不在：前两个在下载页里没有"它自己的那一页"可去
    // （原理图压根没有那一栏、存档不是一个能下载的资源），配置文件不是资源。
    return key == QStringLiteral("mod") || key == QStringLiteral("resourcepack") ||
           key == QStringLiteral("datapack") || key == QStringLiteral("shader");
}

QString InstanceSettingsDialog::filesResourceTip(const QString &title) {
    if (title.isEmpty()) {
        // 没认出来 ⇒ 没有这一颗（见 setFileResource()）。
        return QString();
    }
    // 和 filesDownloadTip() 同一句话的规矩：**说清这一页会关掉** —— 用户点它是为了换个
    // 地方，不是就地多出一个东西。
    return tr("打开“%1”下载页。").arg(title);
}

// 那几页底下不挂说明文字，所以这里也没有一段讲"行尾那颗 ⓘ"的小字。要再挂说明，写的时候
// **照着 `setBatchMode()` 里那段**写，别照抄记忆里的版本 —— 多选是 `MultiSelection`
// （点一下切换，一个修饰键都不用），写成"按住 Ctrl"就是错的。

QString InstanceSettingsDialog::pageUnavailableText(const QString &key) {
    // **唯一分发处**：页里那句「用不上」和勾选表的悬停提示都走它。
    if (key == QStringLiteral("mod")) {
        return modUnavailableText();
    }
    if (key == QStringLiteral("shader")) {
        return shaderUnavailableText();
    }
    if (key == QStringLiteral("schematic")) {
        return schematicUnavailableText();
    }
    if (key == QStringLiteral("datapack")) {
        return datapackUnavailableText();
    }
    return QString();
}

QString InstanceSettingsDialog::pageShownHint() {
    return tr("在导航中显示此页；隐藏不会删除文件。");
}

void InstanceSettingsDialog::setInstanceDir(const QString &dir) {
    // 只是记下来：快捷方式那九颗按钮**点的时候现场读**（见构造函数里那段 NOTE），
    // 所以这里不用重画什么。
    m_instanceDir = dir;
}

void InstanceSettingsDialog::setFileDirs(const QHash<QString, QString> &dirs) {
    QElapsedTimer prepareClock;
    prepareClock.start();
    m_fileDirs = dirs;
    // `config/` 的根**在这儿**认下来（不是在构造函数里）—— 那会儿这一份还没灌进来，
    // 抓到的永远是空串，"不许越过 config/"那道墙就形同虚设了（见 .hpp 里 m_configRoot
    // 那一 NOTE）。
    m_configRoot = m_fileDirs.value(QStringLiteral("config"));
    // 文件页是按需建的：已经建出来的各列一遍；还没建的由构建时的那一发补上。
    // 所以灌完必须让它们各列一遍，否则每一页都停在"这个文件夹还是空的"上。
    const QList<QString> keys = m_filesPages.keys();
    for (const QString &key : keys) {
        if (InstanceFilesPage *page = m_filesPages.value(key)) {
            // setDirectory() 一律回到**根** —— 换了一个实例再回来，不该还停在上一个实例
            // 翻到的那个子目录里（那个路径在新实例上多半根本不存在）。
            page->setDirectory(m_fileDirs.value(key));
        }
    }
    updateConfigUpButton();
}

void InstanceSettingsDialog::openConfigEditor(const QString &path,
                                              const ConfigFileText::Loaded &loaded,
                                              const ConfigFileText::Stamp &stamp) {
    if (m_configStack == nullptr || m_configEditor == nullptr) {
        return;
    }
    m_configEditor->setFile(path, loaded, stamp);
    m_configStack->setCurrentWidget(m_configEditor);
}

void InstanceSettingsDialog::setConfigBackupAvailable(bool available) {
    if (m_configEditor == nullptr) {
        return;
    }
    // NOTE: 顺序要紧 —— `setFile()` 会把这一位清成 false（换了一份文件，上一条答案不作数），
    // 所以这一句必须**排在 openConfigEditor() 之后**。反过来的话，用户看到的是一颗
    // 永远画不出来的「还原上一次」。
    m_configEditor->setBackupAvailable(available);
}

void InstanceSettingsDialog::openWorldSettings(const WorldSettingsPage::WorldView &view) {
    if (m_worldStack == nullptr || m_worldSettings == nullptr) {
        return;
    }
    m_worldSettings->setWorld(view);
    m_worldStack->setCurrentWidget(m_worldSettings);
    emit worldBreadcrumbChanged(m_worldSettings->breadcrumbTitle());
}

void InstanceSettingsDialog::openWorldSettingsUnreadable(const QString &dirName,
                                                         const QString &reason) {
    if (m_worldStack == nullptr || m_worldSettings == nullptr) {
        return;
    }
    m_worldSettings->setUnreadable(dirName, reason);
    m_worldStack->setCurrentWidget(m_worldSettings);
    emit worldBreadcrumbChanged(m_worldSettings->breadcrumbTitle());
}

void InstanceSettingsDialog::closeWorldSettings() {
    if (m_worldStack == nullptr) {
        return;
    }
    // 回第 0 格，并让那一页重新列一遍目录 —— 和 closeConfigEditor() 同一条规矩，但
    // **键不一样**（那一个是 `config`，照抄会列错一页、而且屏幕上看不出来：
    // `m_filesPages.value()` 给的是那一页的指针，刷新它一样会"动一下屏幕"）。
    if (InstanceFilesPage *files = m_filesPages.value(QStringLiteral("save"))) {
        files->refresh();
    }
    m_worldStack->setCurrentIndex(0);
    emit worldBreadcrumbChanged(QString());
}

bool InstanceSettingsDialog::worldSettingsUnreadable() const {
    return m_worldSettings == nullptr || m_worldSettings->isUnreadable();
}

void InstanceSettingsDialog::closeConfigEditor() {
    if (m_configStack == nullptr) {
        return;
    }
    // 回第 0 格，并让那一页**重新列一遍当前目录**：用户待在编辑器那一格的时候，这个目录
    // 可能在外面（「打开文件夹」开出去的那个窗口）被动过，而这一页只在切进来那一下列一次。
    // 重列一次最省事，也一定不会显示一份旧的清单。
    //
    // NOTE: 走的是 `refresh()` 而**不是** `setDirectory()` —— 后者会回到根，而用户可能是从
    // `config/ftbquests/` 底下点开一个文件的，返回时该还停在那儿。
    if (InstanceFilesPage *files = m_filesPages.value(QStringLiteral("config"))) {
        files->refresh();
    }
    m_configStack->setCurrentIndex(0);
    updateConfigUpButton();
}

void InstanceSettingsDialog::onConfigEntryActivated(const QString &path, bool isDir) {
    if (!isDir) {
        // 文件：**这一层读不了盘**（也没有 OverlayHost，读失败要弹的那一句得有人弹），
        // 所以绕出去让 MainWindow 读了再拿 openConfigEditor() 递回来。
        emit openConfigFileRequested(path);
        return;
    }
    // 目录：**这一层自己就办得了** —— 换一个目录再列一遍，纯显示层的事（不碰盘、不联网，
    // 也不问 MainWindow）。所以"点进去看"这一下是同步的、没有往返。
    if (InstanceFilesPage *files = m_filesPages.value(QStringLiteral("config"))) {
        files->setDirectory(path);
        updateConfigUpButton();
    }
}

void InstanceSettingsDialog::leaveConfigDir() {
    InstanceFilesPage *files = m_filesPages.value(QStringLiteral("config"));
    if (files == nullptr) {
        return;
    }
    const QString current = QDir::cleanPath(files->directory());
    const QString root = QDir::cleanPath(m_configRoot);
    if (root.isEmpty() || current == root) {
        return;
    }
    QDir up(current);
    if (!up.cdUp()) {
        files->setDirectory(root);
        updateConfigUpButton();
        return;
    }
    const QString parent = QDir::cleanPath(up.absolutePath());
    // 到了根的下一层再点「上级」就**停在根上**，不往上走（`config/` 上面是这个实例自己，
    // 再上面是整个 .minecraft —— 走到那儿"这是配置文件"这件事就走丢了）。
    files->setDirectory(parent.startsWith(root + QLatin1Char('/')) ? parent : root);
    updateConfigUpButton();
}

void InstanceSettingsDialog::updateConfigUpButton() {
    if (m_configUp == nullptr) {
        return;
    }
    InstanceFilesPage *files = m_filesPages.value(QStringLiteral("config"));
    if (files == nullptr) {
        m_configUp->setVisible(false);
        return;
    }
    const QString root = QDir::cleanPath(m_configRoot);
    const QString current = QDir::cleanPath(files->directory());
    // 两个条件缺一不可：根认出来了（setFileDirs 来过），而且现在**在根以下**。
    // 目录还没接上（构造那一会儿）、或者 config/ 根本不存在（原版实例）时，两边都是
    // 空串 —— 那时候也不该画这颗按钮。
    m_configUp->setVisible(!root.isEmpty() && current != root &&
                           current.startsWith(root + QLatin1Char('/')));
}

void InstanceSettingsDialog::setShaderUsable(bool usable) {
    setPageUsable(QStringLiteral("shader"), usable);
}

void InstanceSettingsDialog::setSchematicUsable(bool usable) {
    setPageUsable(QStringLiteral("schematic"), usable);
}

void InstanceSettingsDialog::setModUsable(bool usable) {
    // 「模组」那一页没有"用不上"这个换页状态可言 —— 它背后照样是一个普通的
    // InstanceFilesPage（mods/ 目录），只是原版实例装了也没人读。所以这一条只影响
    // "左栏里有没有它、勾选框能不能勾"，页本身照旧（用户把它勾出来的话看的是真目录）。
    setPageUsable(QStringLiteral("mod"), usable, /*markPage=*/false);
}

void InstanceSettingsDialog::setDatapackUsable(bool usable) {
    // 和「模组」同理：低版本上 datapacks/ 这个目录没人读，但页本身还是那个文件列表。
    setPageUsable(QStringLiteral("datapack"), usable, /*markPage=*/false);
}

void InstanceSettingsDialog::setPageUsable(const QString &key, bool usable, bool markPage) {
    if (markPage) {
        // 页里那句「用不上」是**整页替换**：这一页在这个实例上用不上时，列表和那排按钮
        // 一起收起来，换成一句为什么（判据在 MainWindow，见 .hpp 里 setModUsable() 那段）。
        if (InstanceFilesPage *page = m_filesPages.value(key)) {
            page->setUnavailable(usable ? QString() : pageUnavailableText(key));
        }
    }
    m_pageAvailable.insert(key, usable);
    // 可用性一变，左栏那一列（有没有这一条）和勾选框（能不能勾、勾没勾、tooltip）都要
    // 跟着变 —— 两件事都在 rebuildEntryList() 里。
    rebuildEntryList();
}

QTreeWidgetItem *InstanceSettingsDialog::selectedServerItem() const {
    if (m_serverTree == nullptr) {
        return nullptr;
    }
    // 编辑 / 加入 / 收藏只认「恰好选中一行」；多选时只剩删除可用。
    // 被筛掉的行不能再被操作（切收藏筛选之后）。
    const auto selected = selectedServerItems();
    return selected.size() == 1 ? selected.first() : nullptr;
}

QList<QTreeWidgetItem *> InstanceSettingsDialog::selectedServerItems() const {
    QList<QTreeWidgetItem *> items;
    if (m_serverTree == nullptr) {
        return items;
    }
    for (auto *item : m_serverTree->selectedItems())
        if (!item->isHidden()) items.append(item);
    return items;
}

void InstanceSettingsDialog::refreshFilePages() {
    const QList<InstanceFilesPage *> pages = m_filesPages.values();
    for (InstanceFilesPage *page : pages) {
        if (page != nullptr) {
            page->refresh();
        }
    }
    if (m_resourcePackPage != nullptr) {
        auto *stack = qobject_cast<QStackedWidget *>(m_resourcePackPage->parentWidget());
        if (stack != nullptr && stack->currentWidget() == m_resourcePackPage)
            emit resourcePackPageEntered();
    }
}

void InstanceSettingsDialog::setFileUpdates(const QStringList &outdatedPaths, bool checked) {
    // 只有**开得了「可更新」那一格**的那几页接（判据逐字同构造里那一条：下载页里真有
    // 对应一栏的那四页）。别的页上那一格根本不在，喂给它只是让它白白记着一份用不上的
    // 名单（而有名单就会有"下一次刷新之后它还在"这类问题）。
    if (filePagesPending()) {
        m_pendingOutdatedPaths = outdatedPaths;
        m_pendingUpdatesChecked = checked;
        m_hasPendingUpdates = true;
    }
    for (auto it = m_filesPages.constBegin(); it != m_filesPages.constEnd(); ++it) {
        if (it.value() == nullptr || filesDownloadText(it.key()).isEmpty()) {
            continue;
        }
        it.value()->setUpdateResult(outdatedPaths, checked);
    }
}

void InstanceSettingsDialog::setFileIcon(const QString &path, const QPixmap &icon) {
    const QList<InstanceFilesPage *> pages = m_filesPages.values();
    for (InstanceFilesPage *page : pages) {
        if (page != nullptr) {
            // 返回值**不看**：不在这一页的那一条本来就会回 false，而它在那六页里必然
            // 只有一页有（六个目录互不重叠）。找不着才是要说话的那一档，而那一档由
            // "图标是异步回来的、这一条可能已经被删了"解释掉了 —— 什么都没有要做的。
            page->setIconForPath(path, icon);
        }
    // 还有页没建 ⇒ 存着，那一页建出来时会自己补上（见 buildFilePage 末尾那几条 for）。
    if (filePagesPending()) m_pendingFileIcons.insert(path, icon);
    }
}

bool InstanceSettingsDialog::filePagesPending() const {
    for (const QString &fileKey : filePageKeys()) {
        if (!m_builtPages.contains(fileKey)) {
            return true;
        }
    }
    return false;
}

void InstanceSettingsDialog::ensureFilePage(const QString &key) {
    // 这一页的壳（addPage 那一层）里才登记着"内容怎么造"（见 buildFilePage），所以先确保
    // 那一页建出来，再谈它里面的文件列表。
    ensurePage(key);
    if (m_filesPages.contains(key)) return;
    const auto it = m_filePageBuilders.find(key);
    if (it == m_filePageBuilders.end()) return;
    const auto build = it.value();
    m_filePageBuilders.erase(it);
    build();
}

InstanceFilesPage *InstanceSettingsDialog::filesPage(const QString &key) {
    ensureFilePage(key);
    return m_filesPages.value(key);
}

void InstanceSettingsDialog::setFileResource(const QString &path, const QString &title) {
    // 走**带键**的遍历（不像 setFileIcon() 那样 `values()` 一把梭）：哪几页接这件事是
    // 有讲究的（见 filesNameOpensResource()）—— 同一份路径当然只会在一个目录里，
    // 所以"过滤掉哪几页"这件事靠的不是路径，而是**规矩**：不在 filesNameOpensResource()
    // 那张表里的页连 tip 都不会喂给它（于是那一行连链接的样子都不会有）。
    const QString tip = filesResourceTip(title);
    for (auto it = m_filesPages.constBegin(); it != m_filesPages.constEnd(); ++it) {
        if (it.value() == nullptr || !filesNameOpensResource(it.key())) {
            continue;
        }
        // 返回值**不看**，理由同 setFileIcon()：不在这页的那一条本来就会回 false，而
        // "盘上已经没有这个文件了"那一档由"下一趟刷新会把它抹掉"解释掉了。
        // 空 tip 也要往下走 —— 那是"把上一趟戴上的链接摘掉"（同一个路径换了内容、
        // 认不出来了；这一趟的负结果得覆盖上一趟的正结果）。
        it.value()->setResourceForPath(path, tip);
    }
    if (filePagesPending()) m_pendingFileResources.insert(path, tip);
}

// ---------- 概览栏那三行（见 .hpp 里 setIcon() 上面那一段）----------

QStringList InstanceSettingsDialog::iconCandidateTexts() {
    // 顺序与 VersionIcons::iconCandidateKeys() 一一对应（前四个是版本分类那四张，后四个
    // 是四个加载器）。名字和版本树 / 安装页上用的是同一批词。
    return {tr("草方块"), tr("覆雪草块"), tr("土块"), tr("幸运方块"),
            tr("Fabric"), tr("Forge"), tr("NeoForge"), tr("OptiFine")};
}

QString InstanceSettingsDialog::ungroupedGroupText() {
    // 转手给 MainWindowUI —— 那一句话的唯一出处在那儿（实例树那边也是同一个词）。
    return MainWindowUI::ungroupedGroupText();
}

int InstanceSettingsDialog::iconPreviewSize() { return kIconPreviewSize; }

void InstanceSettingsDialog::setLastLaunch(const QDateTime &at) {
    if (m_lastLaunchLabel == nullptr) return;
    m_lastLaunchLabel->setText(at.isValid()
        ? tr("最后启动：%1").arg(at.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
        : tr("从未启动"));
    m_lastLaunchLabel->setToolTip(tr("以成功创建游戏进程的时间为准，不代表进入世界的时间。"));
}

void InstanceSettingsDialog::setPlayTime(const QString &total, const QString &last) {
    if (m_playTimeLabel == nullptr) return;
    m_playTimeLabel->setText(tr("累计游玩：%1 · 上次：%2").arg(total, last.isEmpty() ? tr("未记录") : last));
    m_playTimeLabel->setToolTip(tr("按本地客户端进程运行时间计，格式为小时:分钟:秒。"
                                  "同一实例多开时，重叠的时间只计一次。"));
}

void InstanceSettingsDialog::setIcon(const QString &icon, const QPixmap &defaultMark,
                                     const QPixmap &localMark) {
    m_iconChoice = icon;
    m_defaultIconMark = defaultMark;
    m_localIconMark = localMark;
    // 初值不是"本地那张"就把那一份丢掉：留着它只会让人以为挑过了。
    if (m_iconChoice != localIconValue()) {
        m_pickedIconPath.clear();
    }
    refreshIconRow();
}

void InstanceSettingsDialog::setGroupChoices(const QList<QPair<QString, QString>> &groups,
                                             const QString &currentGroupId) {
    if (m_groupMenu == nullptr) {
        return;
    }
    auto *box = qobject_cast<QVBoxLayout *>(m_groupMenu->layout());
    if (box == nullptr) {
        return;
    }
    // 浮层里那几行**整个重建** —— 行数是照这份清单来的（用户在「分组设置」里加组 / 删组
    // 之后再调一次这里，旧的几行就没意义了）。
    //
    // NOTE: **当场 delete，不是 deleteLater**。悬着的那几行和新的几行**同名**（objectName
    // 按下标给），而按名字找控件的验证工程会先撞上旧那个 —— 那个窗口只在"事件循环
    // 转起来之前"存在，短到没法调试。代价是这一招**不能在一行自己的槽里调**
    // （那一行还站在栈上）：产品里只有 `InstanceDialogController` 在 `showPage()` 之前调一次。
    for (QPushButton *row : m_groupRows) {
        delete row;
    }
    m_groupRows.clear();
    m_groupIds.clear();

    const auto addRow = [this, box](const QString &text, const QString &groupId) {
        auto *button = new QPushButton(text, m_groupMenu);
        // 名字按下标给（0 起 = 递进来的顺序）：验证工程按它取控件，而组名是用户起的、
        // 随时会变，拿文字当身份必错（和实例页那块「挑目标」浮层同一条）。
        button->setObjectName(groupMenuRowName(m_groupRows.size()));
        button->setStyleSheet(menuEntryStyle(m_groupMenu->palette()));
        button->setCursor(Qt::PointingHandCursor);
        const int index = m_groupRows.size();
        connect(button, &QPushButton::clicked, this, [this, index]() { chooseGroup(index); });
        box->addWidget(button);
        m_groupRows.append(button);
        m_groupIds.append(groupId);
    };
    // 「未分组」排在最前：它是这一列的"没有归属"，和别的组不是一类东西，摆在末尾会让人
    // 以为它也是一个能排序的组。
    addRow(ungroupedGroupText(), QString());
    for (const QPair<QString, QString> &group : groups) {
        addRow(group.first, group.second);
    }
    // 当前值按 **组 id** 找（组名会变），找不到就停在第一项「未分组」—— 那一组刚被删掉的
    // 实例自然落到未分组，和实例树那边同一条规矩，不为它写清理代码。
    const int index = m_groupIds.indexOf(currentGroupId);
    m_chosenGroupIndex = index >= 0 ? index : 0;
    refreshGroupMenuState();
}

void InstanceSettingsDialog::setPinned(bool pinned, const QString &pinnedGroupId) {
    m_pinnedGroupId = pinnedGroupId;
    if (m_pinCheck == nullptr) {
        return;
    }
    // 挡掉 toggled。**没有人接 `toggled`**（状态由调用方经 setPinned() 推），留着这层
    // 挡板是为了"以后谁接上"的时候两条路不会各摆一份状态 —— 和 refreshHints() 那两处
    // 同一个套路。
    const QSignalBlocker blocker(m_pinCheck);
    m_pinCheck->setChecked(pinned);
    // 这一份 .minecraft 里没有置顶组（用户把它删了）：勾上这一下真的没有去处
    // （MainWindow::pinInstance() 直接返回）⇒ **灰掉它并说清为什么**。灰着不说话是这一层
    // 另一条明令禁止的（用户只会以为界面坏了），所以 tooltip 必须给上。
    m_pinCheck->setEnabled(!pinnedGroupId.isEmpty());
    // 有组时换回收藏规则（步骤 133 起它挂在开关自己身上，清空就等于把它弄丢了）。
    m_pinCheck->setToolTip(
        pinnedGroupId.isEmpty()
            ? tr("暂无收藏组，请先在分组设置中指定。")
            : pinHint());
}

void InstanceSettingsDialog::chooseGroup(int index) {
    if (index < 0 || index >= m_groupIds.size()) {
        return;
    }
    m_chosenGroupIndex = index;
    refreshGroupMenuState();
    // 挑完就收（和图标那个浮层同一条：它是"选一个"，不是"切换一个开关"）。
    if (m_groupMenu != nullptr) {
        m_groupMenu->close();
    }
    // 这一发是**自己补的**：浮层里那几行是 setGroupChoices() 每次重建出来的，构造
    // 函数跑 watchEditors() 的时候它们还不存在（按类型的线接不到以后才建的控件）。
    // 反正这一个动作只有一个入口，接在这儿比"每次重建再补一遍接线"直白。
    notifyEdited();
}

void InstanceSettingsDialog::refreshGroupMenuState() {
    for (int i = 0; i < m_groupRows.size(); ++i) {
        const bool current = i == m_chosenGroupIndex;
        // 当前那一行**灰掉**：点它等于什么都没做，而"什么都没做"和"点了没反应"在用户
        // 眼里是一回事。这是实例页那块「挑目标」浮层早就定下的规矩（那边叫
        // `containsSelected`），两处的措辞也照抄：灰着必须配一句为什么。
        m_groupRows.at(i)->setEnabled(!current);
        m_groupRows.at(i)->setToolTip(current ? tr("已在此分组")
                                              : tr("移入此分组"));
    }
    if (m_groupButton == nullptr) {
        return;
    }
    // 按钮上那几个字是**固定的**（就是「分组…」），所以"现在归在哪一组"只能走 tooltip ——
    // 不写这一句的话，用户不点开就完全看不出当前值。
    const QString name = m_chosenGroupIndex >= 0 && m_chosenGroupIndex < m_groupRows.size()
                             ? m_groupRows.at(m_chosenGroupIndex)->text()
                             : ungroupedGroupText();
    m_groupButton->setToolTip(tr("现在归在「%1」里。点开可以换一组。").arg(name));
}

void InstanceSettingsDialog::popUpMenuBelow(QWidget *menu, const QWidget *anchor) {
    if (menu == nullptr || anchor == nullptr) {
        return;
    }
    // 只有 MenuPopup 知道怎么摆（贴着按钮下边缘、超出屏幕往里收）。这一句是"两个成员在
    // .hpp 里是 QWidget*"和"那个类是 .cpp 里的实现细节"之间的桥。
    //
    // NOTE: 用 dynamic_cast 而**不是** qobject_cast：MenuPopup 是这个 .cpp 里的一个
    // anonymous-namespace 类，**没有 Q_OBJECT**（它不需要信号槽），而 qobject_cast
    // 要求那个宏（编不过：static_assert HasQ_OBJECT_Macro）。给它补上 Q_OBJECT 是
    // 另一条路，代价是这个 .cpp 结尾要多一行 `#include "...moc"` —— 全仓库没有第二处
    // 这么干的，为一次向下转型不值得。dynamic_cast 认的是 C++ 的类型系统，够用。
    auto *popup = dynamic_cast<MenuPopup *>(menu);
    // 走不到：那两个成员在构造函数里各被赋值一次、都是 MenuPopup。写成断言而不是"没有就
    // 什么都不做"—— 后者是"点了没反应"，而这一页上每一处"点了没反应"都得配一句话。
    Q_ASSERT(popup != nullptr);
    if (popup != nullptr) {
        popup->popUpBelow(anchor);
    }
}

QString InstanceSettingsDialog::iconChoice() const { return m_iconChoice; }

QString InstanceSettingsDialog::pickedIconPath() const { return m_pickedIconPath; }

void InstanceSettingsDialog::markPickedIconSaved() {
    if (m_pickedIconPath.isEmpty()) {
        return;
    }
    // 那一份字节现在就是实例目录里的图标 ⇒ 它成了「本地那张」的画法。不换的话，这一格
    // 下一次重画会退回**上一张**本地图（`m_localIconMark` 是 setIcon() 灌的旧值）。
    const QPixmap picked(m_pickedIconPath);
    if (!picked.isNull()) {
        m_localIconMark = picked;
    }
    m_pickedIconPath.clear();
    // 签名跟着挪到"本地那张"这一档，否则下一次任何改动都会拿 `local` 去和
    // `local|源路径` 比，白触发一次写盘（内容其实一模一样）。
    m_lastSignature = valueSignature();
    refreshIconRow();
}

QString InstanceSettingsDialog::chosenGroupId() const {
    if (m_chosenGroupIndex < 0 || m_chosenGroupIndex >= m_groupIds.size()) {
        // 还没灌过候选（setGroupChoices() 没调过）⇒ 空串 = 未分组。调用方（MainWindow）
        // 拿它和"打开对话框时那一格"比，两边都是空串时结论是"用户没改过"，正确。
        return QString();
    }
    return m_groupIds.at(m_chosenGroupIndex);
}

bool InstanceSettingsDialog::pinned() const {
    return m_pinCheck != nullptr && m_pinCheck->isChecked();
}

QString InstanceSettingsDialog::actionsHint() {
    // 与导出按钮的 tooltip 共用文案；文件补全说明属于维护/修复页。
    return exportScriptHint();
}

void InstanceSettingsDialog::refreshIconRow() {
    if (m_iconPreview == nullptr) {
        return;
    }

    // 当前该画哪一张。三档的先后是有讲究的：
    // ① 手上有一张**刚挑的**图 → 画它（用户要立刻看见自己选了什么，而它这会儿还在原处）；
    // 读不动（不是图 / 半截文件）就往下走，退回这一档原本的样子。
    // ② 内置候选 → 那一张。
    // ③ 本地那一档 → 调用方解好的那张；它要是空的（文件没了）→ 退回默认那张。
    // ④ 默认 → 调用方算好的 defaultMark。
    QPixmap mark;
    if (!m_pickedIconPath.isEmpty()) {
        const QPixmap picked(m_pickedIconPath);
        if (!picked.isNull()) {
            mark = picked;
        }
    }
    if (mark.isNull()) {
        const QString key = VersionIcons::iconKeyFromValue(m_iconChoice);
        if (!key.isEmpty()) {
            mark = VersionIcons::iconMarkForKey(key, kIconPreviewSize);
        } else if (m_iconChoice == localIconValue()) {
            mark = m_localIconMark;
        }
    }
    if (mark.isNull()) {
        mark = m_defaultIconMark;
    }
    // 按**保持长宽比**缩到那一格：用户挑的图多半不是正方形，拉成方的会把人像 / 图标压扁。
    m_iconPreview->setPixmap(mark.isNull()
                                 ? QPixmap()
                                 : mark.scaled(kIconPreviewSize, kIconPreviewSize,
                                               Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (m_headerIcon != nullptr) m_headerIcon->setPixmap(m_iconPreview->pixmap());

    // 八颗按钮的勾选状态跟着走（"按下去了哪一颗"必须和预览说的是同一件事）。
    const QStringList keys = VersionIcons::iconCandidateKeys();
    const QString checkedKey = VersionIcons::iconKeyFromValue(m_iconChoice);
    for (int i = 0; i < m_iconButtons.size(); ++i) {
        m_iconButtons.at(i)->setChecked(keys.value(i) == checkedKey);
    }
}

void InstanceSettingsDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    // 任务中心关掉后重开这一页：背景不应该再播一次换页动画。
    if (window() != nullptr && window()->property("wtfmlSuppressSettingsAppear").toBool()) {
        window()->setProperty("wtfmlSuppressSettingsAppear", false);
        m_firstShowAnimated = true;
        return;
    }
    if (m_firstShowAnimated) return;
    m_firstShowAnimated = true;
    QWidget *page = m_stack != nullptr ? m_stack->currentWidget() : nullptr;
    if (page == nullptr) return;
    // 第一次露面（从「管理」列表点某个实例进来）也播一次换页动画：
    // 之前只有「已经开着再换页」才有，第一次进来是硬切。
    QTimer::singleShot(0, this, [this, page] {
        if (!isVisible() || m_stack == nullptr || m_stack->currentWidget() != page) return;
        Anim::pageAppear(page);
    });
}

void InstanceSettingsDialog::showPage(const QString &requestedKey) {
    const QString key = maintenancePageKey(requestedKey);
    ensureFilePage(key);
    const int index = stackPageIndexOf(key);
    if (index < 0) {
        return;
    }
    // 左栏那一条**先**摆好：有条目的是"选中那一条"（和用户点那一条是同一条路），没有条目
    // 的（「配置文件」这种没有入口的页、被用户勾掉了的页、这个实例上用不上的页）那一列里
    // 根本没有它 ⇒ row 是 -1，等于把选中清掉（留着上一次那一行的话，左栏亮着 A、右栏
    // 摆着 B —— 那正是"两栏各说各的"）。
    //
    // NOTE: 这一下**压着信号** —— 下面那一发 applyPageKey() 本来就会换页，让这一下也发
    // 一次的话，同一页会被摆两次（见 applyPageKey() 里那条 alreadyHere：
    // 多发的那一次在「服务器列表」上就是白连一轮）。
    {
        const QSignalBlocker blocker(m_pageList);
        int row = -1;
        for (int i = 0; i < m_pageList->count(); ++i) {
            if (m_pageList->item(i)->data(Qt::UserRole).toString() == key) {
                row = i;
                break;
            }
        }
        m_pageList->setCurrentRow(row);
    }
    // 无论上面那一发有没有真的换过去（`setCurrentRow()` 在"本来就亮着这一条"时不发信号），
    // 这里都摆一次。同一页重复摆的那一多半由 alreadyHere 挡掉。
    applyPageKey(key);
}

QString InstanceSettingsDialog::currentPageKey() const {
    // 判据只有右栏那一位（见 .hpp 那段）：反查函数本来就在、`rebuildEntryList()` 也在用。
    return m_stack == nullptr ? QString() : pageKeyAtStackIndex(m_stack->currentIndex());
}

void InstanceSettingsDialog::rebuildNavigationTree() {
    if (m_navigationTree == nullptr) return;
    QSet<QString> collapsed;
    const bool rebuilding = m_navigationTree->topLevelItemCount() > 0;
    for (QTreeWidgetItemIterator it(m_navigationTree); *it; ++it) {
        if ((*it)->childCount() > 0 && !(*it)->isExpanded())
            collapsed.insert((*it)->data(0, Qt::UserRole + 1).toString());
    }
    const QSignalBlocker blocker(m_navigationTree);
    m_navigationTree->clear();
    const auto add = [this](QTreeWidgetItem *parent, const QString &text, const QString &key) {
        auto *item = parent != nullptr ? new QTreeWidgetItem(parent)
                                      : new QTreeWidgetItem(m_navigationTree);
        item->setText(0, text);
        item->setData(0, Qt::UserRole, key);
        item->setToolTip(0, text);
        item->setSizeHint(0, QSize(0, 32));  // 一级二级同一行高；36 时最小窗里左栏要竖着滚
        if (key.isEmpty()) item->setFlags(Qt::ItemIsEnabled);
        if (parent == nullptr) {
            // 一级目录（「概览」「启动设置」和四个分组头）同一档：正文字号、加粗、正文色；二级页才是常规字重。
            QFont f = Theme::textFont(item->font(0), Theme::TextRole::Body);
            f.setBold(true);
            item->setFont(0, f);
        }
        return item;
    };
    const auto group = [&add](QTreeWidgetItem *parent, const QString &text, const QString &id) {
        auto *item = add(parent, text, QString());
        item->setData(0, Qt::UserRole + 1, id);
        return item;
    };
    const auto leaf = [&add](QTreeWidgetItem *parent, const QString &key) {
        return add(parent, InstanceSettingsDialog::pageText(key), key);
    };
    leaf(nullptr, overviewCategoryKey());
    leaf(nullptr, launchCategoryKey());
    auto *content = group(nullptr, tr("内容管理"), QStringLiteral("content"));
    for (const QString &key : {QStringLiteral("mod"), QStringLiteral("resourcepack"),
                               QStringLiteral("datapack"), QStringLiteral("shader")}) {
        // 保留用户明确关闭页面的选择；可用性不足在既有页面里解释，不吞掉导航入口。
        if (m_pageOverrides.value(key).toBool(true)) leaf(content, key);
    }
    auto *worlds = group(nullptr, tr("世界与联机"), QStringLiteral("worlds"));
    for (const QString &key : {QStringLiteral("save"), QStringLiteral("server")})
        if (m_pageOverrides.value(key).toBool(true)) leaf(worlds, key);
    auto *creative = group(nullptr, tr("创作工具"), QStringLiteral("creative"));
    for (const QString &key : {QStringLiteral("screenshots"), QStringLiteral("replay"),
                               QStringLiteral("schematic"), QStringLiteral("structure")})
        if (m_pageOverrides.value(key).toBool(true)) leaf(creative, key);
    auto *maintenance = group(nullptr, tr("维护"), QStringLiteral("maintenance"));
    leaf(maintenance, QStringLiteral("diagnosis"));
    leaf(maintenance, QStringLiteral("logs"));
    leaf(maintenance, QStringLiteral("operationsmenu"));
    leaf(maintenance, QStringLiteral("migration"));
    leaf(maintenance, QStringLiteral("upgrade"));
    for (QTreeWidgetItemIterator it(m_navigationTree); *it; ++it) {
        if ((*it)->childCount() > 0) {
            const QString id = (*it)->data(0, Qt::UserRole + 1).toString();
            const bool defaultExpanded = id != QStringLiteral("creative") && id != QStringLiteral("maintenance");
            (*it)->setExpanded(rebuilding ? !collapsed.contains(id) : defaultExpanded);
        }
    }
}

QWidget *InstanceSettingsDialog::addOverrideRow(QGridLayout *grid, int row, const QString &field,
                                                const QString &title, const QString &customHint) {
    // **两行**：上面那一行是这个设置的名字（独占一整行），下面那一行才是它的选项
    // （「跟随全局 / 自定义」那个档位下拉框 + 控件）。**调用方因此要 `row += 2`**
    // （见 addTitledRow() 那段）。
    QVBoxLayout *cellLayout = nullptr;
    QWidget *cell = addTitledRow(grid, row, title, this, &cellLayout);

    // 控件和档位下拉框在同一格，是为了内存那两行"整行禁用"能一次把两者都灰掉 ——
    // 只禁数字框会留下一个能选的「自定义」，选了却什么都不发生。
    auto *choiceRow = new QWidget(cell);
    choiceRow->setObjectName(choiceRowName(field));
    auto *choiceLayout = new QHBoxLayout(choiceRow);
    choiceLayout->setContentsMargins(0, 0, 0, 0);
    choiceLayout->setSpacing(8);

    // 两项的**顺序就是下标**：`choiceFollowIndex()`（跟随全局）在前、
    // `choiceCustomIndex()`（自定义）在后。那两个函数是档位的唯一出处，读侧
    // （isCustomChoice）、写侧（setChoiceCustom）和验证工程都走它们 —— 这里只是把
    // 它们摆成 `addItem()` 的先后，**不许在别处再写一遍 0 / 1**。
    //
    // NOTE: 下拉框**吃不到** `watchEditors()` 那四条按类型的线（它们接的是
    // `QAbstractButton::clicked` / `QLineEdit::editingFinished` …，QComboBox 一条
    // 都不沾），所以那边另有一循环接它。漏了的症状很具体：把档位切到「自定义」
    // 之后盘上还是老样子 —— 和「窗口模式」那个下拉框同一个坑
    // （见下面构造它那一段）。
    auto *combo = new QComboBox(choiceRow);
    combo->setObjectName(choiceComboName(field));
    combo->addItem(followText());
    combo->addItem(customText());
    // 下拉框也吃滚轮（在两项之间来回跳），而这一格上的滚轮多半是"想滚这一页"。
    // 和「窗口模式」那一个同一个理由。
    WheelGuard::install(combo);
    combo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    combo->setMinimumWidth(kChoiceComboWidth);
    choiceLayout->addWidget(combo);
    m_choiceCombos.insert(field, combo);

    cellLayout->addWidget(choiceRow);

    auto *hintLabel = new QLabel(cell);
    hintLabel->setObjectName(followHintName(field));
    hintLabel->setWordWrap(true);
    // 精修：「跟随全局（当前 …）」这类说明不占一行，进这一格的悬停提示（refreshHints()
    // 同时写两处）。跟随时右边那几个灰着的控件本身就摆着全局值。标签照旧在、照旧有字。
    hintLabel->hide();
    // 提示文字有**两种形态**：选「自定义」时说"这一项只对这个实例生效"，选「跟随
    // 全局」时说"当前跟随的是什么值"。这里先把自定义那一种记下来，refreshHints()
    // 在两种之间切。
    m_customHints.insert(field, customHint);
    cellLayout->addWidget(hintLabel);

    // 档位一变，那一行下面那句话和内存两行的可用性都可能跟着变。
    connect(combo, &QComboBox::currentIndexChanged, this,
            &InstanceSettingsDialog::refreshHints);

    return cell;
}

bool InstanceSettingsDialog::isCustomChoice(const QString &field) const {
    const QComboBox *combo = m_choiceCombos.value(field);
    // 认不出来的 field（这一格没建出来）回 false = 跟随全局 —— 见 .hpp 里那一段。
    return combo != nullptr && combo->currentIndex() == choiceCustomIndex();
}

void InstanceSettingsDialog::setChoiceCustom(const QString &field, bool custom) {
    QComboBox *combo = m_choiceCombos.value(field);
    if (combo == nullptr) {
        return;
    }
    // 挡信号：这一发是**灌值**，不是用户改的。不挡的话它一次会响两条线 ——
    // refreshHints（无害，下面 refreshHints() 反正要再算一遍）和写盘那条
    // （`watchEditors()` 的 QComboBox 循环 ⇒ notifyEdited），后者在构造里
    // `m_lastSignature` 还是空的时候就是"打开这一页顺手替用户写一次盘"。
    const QSignalBlocker block(combo);
    combo->setCurrentIndex(custom ? choiceCustomIndex() : choiceFollowIndex());
}

QHBoxLayout *InstanceSettingsDialog::choiceRowOf(const QString &field) const {
    if (auto *row = findChild<QWidget *>(choiceRowName(field))) {
        return qobject_cast<QHBoxLayout *>(row->layout());
    }
    return nullptr;
}

bool InstanceSettingsDialog::effectiveAutoMemory() const {
    // 这一页自己没有「内存分配」（自动 / 手动）的入口 —— 那个开关在设置页最右边那颗
    // 下拉框上。所以有效值只看**全局**那份；全局是自动档时，
    // 这一页的条和数字框跟着禁用，并显示「自动档下按系统内存现算」那句提示。
    // 盘上老实例存着的 autoMemory 覆盖仍然不生效（`resolveJavaSettings` 读侧忽略它）。
    return m_global.autoMemory;
}

bool InstanceSettingsDialog::advancedExpanded() const {
    // 「高级选项」那一行在启动页里；页还没建 = 从来没展开过（默认折叠）。
    return m_advancedToggle != nullptr && m_advancedToggle->isChecked();
}

void InstanceSettingsDialog::refreshHints() {
    // 启动页还没建出来（第一次切过去才建）就没有"提示"可刷 —— 建的那一下
    // `applyLaunchValues()` 末尾会自己刷一遍。
    if (!m_launchPageBuilt) {
        return;
    }
    const bool autoMemory = effectiveAutoMemory();

    // 内存那两行：自动档下**整格**禁用（档位下拉框 + 数字框），提示换成"为什么它由不得你"。
    m_xmxFollowCell->setEnabled(!autoMemory);
    m_xmsFollowCell->setEnabled(!autoMemory);

    // 手动档 + **「跟随全局」** 时，那一格里的条和数字框要**灰掉**。只在自动档下禁用是
    // 不够的：在"跟随全局"这一档上还能拖、能填 —— 而拖完那一发
    // `override()` 根本不会读（它只在选了「自定义」时才把 `m_memoryMaxSpin->value()` 装进
    // 结果，见下面那段），用户看着数字变了、松手、关页，什么都没发生。这正是设置页那条
    // 注释明确反对的"让人以为能改"。
    //
    // **整格**不能跟着禁（档位下拉框得留着），所以只点这两个控件；下拉框本身照旧能点，
    // 想要填数就点「自定义」，一按下去这两个就活了（`setEnabled` 的实效是"父与自身都为真"，
    // 所以自动档下这里的 true 也翻不过上面那句整格禁用）。
    //
    // NOTE: 「实例名 / Java 路径」那几行是"跟随全局时编辑框里就摆着全局值、改了等于自定义"
    // 的另一种拆法（那边靠「浏览…」那一按顺手切档），内存这一行**不用**那个拆法：
    // 条目上滚一下滚轮就在盘上凭空多出一份覆盖，这种事不能有。
    const bool memoryEditable = !autoMemory && isCustomChoice(xmxField());
    m_memoryMaxSlider->setEnabled(memoryEditable);
    m_memoryMaxSpin->setEnabled(memoryEditable);
    // 最小内存那一格整个是隐藏的，但控件照旧跟着同一个规矩 ——
    // 它现在唯一的作用就是把盘上那份值透传回去，多一个能改它的入口就是多一条静默改写的路。
    m_memoryMinSpin->setEnabled(memoryEditable);

    // 每一行下面那句话：选「跟随全局」时写清"当前跟随的是什么值"，选「自定义」时
    // 说明"这一项只对这个实例生效"。两种都要有 —— 只有"跟随全局"四个字的话，
    // 用户不知道跟随的到底是什么，还得退出去翻设置页。
    const auto setHint = [this](const QString &field, const QString &text) {
        if (auto *label = findChild<QLabel *>(followHintName(field))) {
            label->setText(text);
            if (QWidget *cell = label->parentWidget()) cell->setToolTip(text);
        }
    };
    const auto followOrCustom = [this, &setHint](const QString &field, bool isCustom,
                                                 const QString &currentText) {
        setHint(field, isCustom ? m_customHints.value(field) : followHint(currentText));
    };

    followOrCustom(javaPathField(), isCustomChoice(javaPathField()),
                   m_global.javaPath.isEmpty() ? tr("未指定") : m_global.javaPath);
    followOrCustom(extraJvmArgsField(), isCustomChoice(extraJvmArgsField()),
                   m_global.extraJvmArgs.isEmpty() ? tr("未设置") : m_global.extraJvmArgs);

    // ---------- 窗口模式 / 窗口大小 ----------
    //
    // 「跟随全局」时**这一格里的控件灰掉**（和内存那两行同一个做法，理由也在那边：
    // 能改、改完 override() 又不读，就是"让人以为能改"）。**不能整格禁**：那个档位
    // 下拉框得留着才能切回「自定义」。所以只点那几个控件，下拉框照旧能点。
    //
    // NOTE: 和内存那一行不同的是，这里**没有**"这一格整格禁用"的第二种情况（内存那两种是
    // "自动档"和"跟随全局"）—— 窗口这几项没有自动档。
    if (m_windowModeCombo != nullptr) {
        const bool custom = isCustomChoice(windowModeField());
        m_windowModeCombo->setEnabled(custom);
        // 那一句话：选「自定义」时说"只对这个实例生效"，选「跟随全局」时说清**跟随的是什么**
        // —— 全局那一档的显示名由调用方喂进来。没喂（空串）时就只说"跟随全局"四个字：
        // 硬套 followHint("") 会写出「跟随全局（当前 ）」那种带个空括号的话。
        setHint(windowModeField(),
                custom ? m_customHints.value(windowModeField())
                       : (m_global.windowModeLabel.isEmpty()
                              ? followText()
                              : followHint(m_global.windowModeLabel)));
    }
    if (m_windowWidthSpin != nullptr) {
        // 分辨率那一行占两个键、只有**一个**档位下拉框，它存在 `windowWidthField()` 那一格
        // 里（见 .hpp 里那一段）。
        const bool editable = isCustomChoice(windowWidthField());
        m_windowWidthSpin->setEnabled(editable);
        m_windowHeightSpin->setEnabled(editable);
        // 中间那个 × 跟着一起灰：只剩两个灰框中间夹一个亮着的 ×，看着像那一格坏了
        // （和设置页那一行同一个做法）。
        if (auto *times = findChild<QLabel *>(windowSizeTimesName())) {
            times->setEnabled(editable);
        }
        // 「跟随全局」那句话里要写出**当前跟随的是多大** —— 光有"跟随全局"四个字，用户
        // 还得退出去翻设置页。调用方没给尺寸（<= 0 那个哨兵）时只说"跟随全局"。
        const QString globalSize = (m_globalWindowWidth > 0 && m_globalWindowHeight > 0)
                                       ? QStringLiteral("%1×%2").arg(m_globalWindowWidth)
                                             .arg(m_globalWindowHeight)
                                       : QString();
        setHint(windowWidthField(),
                editable ? m_customHints.value(windowWidthField())
                         : (globalSize.isEmpty() ? followText() : followHint(globalSize)));
    }

    if (autoMemory) {
        // 自动档：这两行是灰的，提示只说这一件事（"当前 %1 MB"在这里没有意义 ——
        // 那两个数每次启动前都会重算）。
        setHint(xmxField(), memoryAutoDisabledHint());
        setHint(xmsField(), memoryAutoDisabledHint());
    } else {
        followOrCustom(xmxField(), isCustomChoice(xmxField()),
                       QStringLiteral("%1 MB").arg(m_global.xmx));
        followOrCustom(xmsField(), isCustomChoice(xmsField()),
                       QStringLiteral("%1 MB").arg(m_global.xms));
    }
}

InstanceSettingsDialog::Override InstanceSettingsDialog::override() const {
    // 启动页还没建 = 这一段里用户没碰过它 ⇒ 盘上那一份就是"现在的值"，原样透传。
    // 反过来读控件的话那几个指针还是空的。
    if (!m_launchPageBuilt) {
        return m_currentOverride;
    }
    Override result;

    if (isCustomChoice(javaPathField())) {
        // 去首尾空白：路径末尾多一个空格在 Windows 上就是个不存在的文件，
        // 而那会变成"启动时找不到 Java"。
        const QString path = m_javaPathEdit->text().trimmed();
        // **空的「自定义」= 跟随全局**，不是一个空路径覆盖。这一条不是"顺手清理"：
        // 实例覆盖了 javaPath 就等于手动档（见 GlobalJavaSettings::ensureJavaForVersion
        // 的 NOTE），而手动档 + 空路径是**必然启动失败**的（"还没指定路径"）。
        // 用户清空这个框的意思是"这个实例不特殊指定"，那正是「跟随全局」。
        // NOTE: 这一条**只对 javaPath 成立**。extraJvmArgs 自定义成空串是有意义的
        // （全局带着 -XX:+UseG1GC，这个实例就是不要额外参数），那里空串照存。
        if (!path.isEmpty()) {
            result.javaPath = path;
        }
    }

    // 自动档下这两项**不写**。界面这时是禁用的，所以正常操作走不到这儿；能走到的是
    // 磁盘上本来就存着这种组合的旧数据（自动档 + Xmx 覆盖）。存一份永远不会被读的
    // 覆盖就是骗人 —— 见 .hpp 里 override() 的 NOTE。
    if (!effectiveAutoMemory()) {
        if (isCustomChoice(xmxField())) {
            result.xmx = m_memoryMaxSpin->value();
        }
        if (isCustomChoice(xmsField())) {
            // 这一行在界面上整行隐藏，但盘上存过的值**照样透传** ——
            // 界面不显示不等于替用户删掉他的设置。唯一不能原样透传的是"比最大内存还大"的
            // 那些：Xms > Xmx 是 JVM 直接拒绝启动的组合（Initial heap size set to a
            // larger value than the maximum heap size），而这一行是隐藏的，用户根本看不到
            // 自己存过这么一对 —— 没有人在旁边替他看着。
            const int effectiveXmx = isCustomChoice(xmxField()) ? m_memoryMaxSpin->value()
                                                                : m_global.xmx;
            result.xms = qMin(m_memoryMinSpin->value(), effectiveXmx);
        }
    }

    if (isCustomChoice(extraJvmArgsField())) {
        result.extraJvmArgs = m_extraJvmArgsEdit->text().trimmed();
    }

    // 窗口模式：选「自定义」就把下拉框里当前那一项的**代号**收进去（itemData 存的就是它，
    // 见构造那一段）。取不到代号（`windowModes` 空表时这一行根本不存在 ⇒ 指针是空的、
    // 或者下拉框里一项都没有）就当作没覆盖 —— 和 javaPath 那条"空的「自定义」= 跟随全局"
    // 同一个方向：宁可什么都不写，也不要往 meta 里塞一个空代号（那会被读侧当成认不出来的
    // 值、静默退回默认档，比"没写"更难查）。
    if (m_windowModeCombo != nullptr && isCustomChoice(windowModeField())) {
        const QString token = m_windowModeCombo->currentData().toString();
        if (!token.isEmpty()) {
            result.windowMode = token;
        }
    }

    // 分辨率：**两个一起收、或者两个都不收**。它们共用一个档位下拉框（见 .hpp 里 override()
    // 那段），所以判据只看那一个 —— 不去看两个框各自有没有被动过（那既判不出来，也不是
    // 用户的意思：他选的是「自定义」，意思是这一对标就要按他填的走）。
    if (m_windowWidthSpin != nullptr && isCustomChoice(windowWidthField())) {
        result.windowWidth = m_windowWidthSpin->value();
        result.windowHeight = m_windowHeightSpin->value();
    }

    return result;
}

QStringList InstanceSettingsDialog::appendJvmArgs() const {
    // 页还没建 ⇒ 构造时那一份（同 override()）。
    if (!m_launchPageBuilt) {
        return m_pendingAppendJvmArgs;
    }
    QStringList result;
    // 一条一行（不是空格分隔）：见 .hpp 里那个成员的说明 —— 参数本身可能含空格，
    // 空格分隔会把 `-Dfoo=bar baz` 切成两条。
    const QStringList lines = m_appendJvmArgsEdit->toPlainText().split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            result.append(trimmed);
        }
    }
    return result;
}

bool InstanceSettingsDialog::warnJavaMismatch() const {
    // 启动页还没建 ⇒ 构造时那一份。**不许退回"默认 true"**：盘上存的是 false 的话，
    // 这里回 true 就等于把用户的开关掰回去（下一次写盘就真写回去了）。
    if (!m_launchPageBuilt) {
        return m_pendingWarnJavaMismatch;
    }
    // 控件没建出来（只验结构的构造点）时返回 true（= 提醒）：读侧默认"要提醒"这个方向是
    // **安全**的那个 —— 宁可多问一次，也不要静默放行一个版本不对的 Java。
    return m_warnJavaMismatchCheck == nullptr || m_warnJavaMismatchCheck->isChecked();
}

// ---------- 「改了就写」那套（见 .hpp 里 settingsEdited() 那一段）----------

void InstanceSettingsDialog::watchEditors() {
    // 构造函数末尾那一次：把**这一刻已经建出来的**控件接上（页眉那两颗按钮、左右两栏）。
    // 懒建的页在 ensurePage() → watchPage() 里各自补一遍。
    watchPage(this);
}

void InstanceSettingsDialog::watchPage(QWidget *page) {
    if (page == nullptr) {
        return;
    }
    // 四类各一条线。**按类型接、不按对象接**：往这一页加控件时不用记得回来补一行。
    // 代价是这里面有两处接不到的（程序化改值），它们各自补一发 —— 见下面那段 NOTE。
    for (QAbstractButton *button : page->findChildren<QAbstractButton *>()) {
        // 勾选框 / 各种按钮一并算上。「值变了没有」由 notifyEdited() 判，
        // 所以那些**不改值**的按钮（打开文件夹 / 导出脚本 / 高级选项那个折叠开关）
        // 多接一条线是白接，不会多写一次盘。
        connect(button, &QAbstractButton::clicked, this, &InstanceSettingsDialog::notifyEdited,
                Qt::UniqueConnection);
    }
    for (QLineEdit *edit : page->findChildren<QLineEdit *>()) {
        // 「实例名」那一格走改名那条路（renameRequested），**不接在这儿**：它改的是目录名，
        // 不能和别的值挤在同一次写盘里。
        if (edit == m_instanceNameEdit) {
            continue;
        }
        // `editingFinished`（回车 / 离开这一格）而不是 `textChanged`：路径和额外参数都是
        // "一段字"，打字打到一半就写盘等于每次按键改一次磁盘上的设置。改名那一格是同一条
        // 理由（见 .hpp 里 renameRequested()）。
        connect(edit, &QLineEdit::editingFinished, this, &InstanceSettingsDialog::notifyEdited,
                Qt::UniqueConnection);
    }
    // QSpinBox 的两条 valueChanged 重载里挑 int 那条（另一个是 QString，Qt 6 已经删了，
    // 但 QOverload 写成 int 这一条在两边都编得过）。
    for (QSpinBox *spin : page->findChildren<QSpinBox *>()) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                &InstanceSettingsDialog::notifyEdited, Qt::UniqueConnection);
    }
    // 下拉框：这一页上**所有** QComboBox 一起接。档位那七格和「窗口模式」都是下拉框，
    // 一个个手接就成了"每加一行都要记得回来补一条线" —— 收进这个循环之后和上面四条是
    // 同一个形状。
    // 症状（漏掉时）：切到「自定义」之后盘上还是老样子 —— 界面上看着改了、下次开这一页
    // 又是原来的档。**容易在测试里被盖住**：后面只要再动一个别的控件，那一发写盘会顺带
    // 把新档位写进去，于是"没接线"这个洞照样全绿（mw_check 的 T20.11b 就是为这个写的：
    // 它把换档位摆成那一页上**最后一个**动作）。
    //
    // NOTE: 这一循环**连这一页上的子页面一起接**（文件管理那几页的「筛选」下拉框、世界设置
    // 那一页的难度下拉框 —— 它们都是这一页的后代）。改了它们会白喊一声 notifyEdited()
    // —— 无害：那个函数按 `valueSignature()` 去重，而那几个值不在签名里，所以一次盘都
    // 不会写。按"这一页上的哪些下拉框"去筛是**多一条会过期的名单**，不值当。
    for (QComboBox *combo : page->findChildren<QComboBox *>()) {
        connect(combo, &QComboBox::currentIndexChanged, this,
                &InstanceSettingsDialog::notifyEdited, Qt::UniqueConnection);
    }
    for (QPlainTextEdit *text : page->findChildren<QPlainTextEdit *>()) {
        // 这个没有 editingFinished，只能用 textChanged —— 于是"追加参数"那一格是这一页上
        // **唯一一处按每个按键写盘**的（一条一行，本来就短）。值没变还是不发（空格、回车这
        // 种改动会被签名里那句 `appendJvmArgs()` 归一掉）。
        connect(text, &QPlainTextEdit::textChanged, this, &InstanceSettingsDialog::notifyEdited,
                Qt::UniqueConnection);
    }

    // 「实例名」那一格：回车 / 离开这一格 ⇒ 报"用户要改名"（不是"值变了"）。
    if (m_instanceNameEdit != nullptr && !m_renameWired && page->isAncestorOf(m_instanceNameEdit)) {
        m_renameWired = true;
        connect(m_instanceNameEdit, &QLineEdit::editingFinished, this, [this]() {
            emit renameRequested(m_instanceNameEdit->text());
        });
    }

    // NOTE: 【上面两条按类型的线接不到两种改动，各自在动手的地方补一发 notifyEdited()】
    // ①「浏览…」选完 java.exe：那是 `setText()`（程序化，不发 editingFinished）；
    // ②「选一张图片…」挑完图：`clicked` 在**框弹出来之前**就发完了，等用户挑完，
    // 那一发早过去了（值是在那之后才变的）。
    // 往这一页再加"点一下、然后弹个系统框问点什么"的按钮时，记住同一件事。
}

QStringList InstanceSettingsDialog::valueSignature() const {
    // 拼的就是**写盘那边会读的那几问**（`override()` / `appendJvmArgs()` /
    // `warnJavaMismatch()` / `iconChoice()` / `pickedIconPath()` / `chosenGroupId()` /
    // `pinned()` / `pageChoices()`）。判据本身在 `src/settings/InstanceSettingsSignature`
    // 那一层：它是纯函数、能被断言盯上；这一页只负责把值摆过去。
    const Override values = override();
    InstanceSettingsSignature::Inputs inputs;
    inputs.javaPath = values.javaPath;
    inputs.xmx = values.xmx;
    inputs.xms = values.xms;
    inputs.autoMemory = values.autoMemory;
    inputs.extraJvmArgs = values.extraJvmArgs;
    inputs.windowMode = values.windowMode;
    inputs.windowWidth = values.windowWidth;
    inputs.windowHeight = values.windowHeight;
    // 追加参数按条摆：`appendJvmArgs()` 已经把空行和首尾空白归一掉了。
    inputs.appendArgs = appendJvmArgs();
    inputs.warnJavaMismatch = warnJavaMismatch();
    // 图标：挑本地那张时**连路径一起算**（换一张图 = 换一次设置），否则只算那一档的键。
    inputs.iconSignature = m_pickedIconPath.isEmpty()
                               ? iconChoice()
                               : localIconValue() + QLatin1Char('|') + m_pickedIconPath;
    // 「显示哪些页」按 optionalPageKeys() 的**顺序**摆（不能按 QHash 的遍历顺序）。
    for (const QString &key : optionalPageKeys()) {
        const QCheckBox *check = m_pageChecks.value(key);
        inputs.pageChecks.append({key, check != nullptr && check->isChecked()});
    }
    // 分组与置顶：分组按 id（组名会变，id 才是身份），置顶只算那一位。
    inputs.groupId = chosenGroupId();
    inputs.pinned = pinned();
    return InstanceSettingsSignature::signatureOf(inputs);
}

void InstanceSettingsDialog::notifyEdited() {
    const QStringList now = valueSignature();
    if (now == m_lastSignature) {
        return;
    }
    m_lastSignature = now;
    emit settingsEdited();
}
