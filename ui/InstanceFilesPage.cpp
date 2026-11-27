/**
 * @file InstanceFilesPage.cpp
 * @brief InstanceFilesPage 的实现（见 .hpp 里那段"六页共用一个形状"）。
 */
#include "InstanceFilesPage.hpp"
#include "ItemViewContextMenu.hpp"
#include "../ui/ShellOpenWith.hpp"
#include <QFileInfo>
#include <QMenu>
#include <QProcess>

#include "ElidedLabel.hpp"
#include "InstanceFilesPageInternals.hpp"
#include "MainWindowUI.hpp"  // WheelGuard：状态下拉框不许被滚轮改
#include "Theme.hpp"         // 上面那两颗一般性的按钮（黑字黑边）

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include "FlowLayout.hpp"
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>  // 滚动那一趟要重摆「详情」那一格（见构造里那段）
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>  // isCurrentPage()：往上找最外面那个堆叠容器
#include <QTimer>
#include <QVBoxLayout>

using namespace InstanceFilesPageInternals;

namespace {

/// 那条操作条四周的留白、以及它里面控件之间的间距（px）。
constexpr int kSelectionBarPadding = 8;
constexpr int kSelectionBarSpacing = 8;

}  // namespace

InstanceFilesPage::InstanceFilesPage(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(8);

    // ---------- 最上面那一排：一条搜索框----------
    //
    // 搜索框在「下载新 / 刷新 / 打开文件夹」那一排按钮**之上**，状态选择在那一排**之下**。
    //
    // 作用范围是**这一页的列表**：`mods/` 底下几十上百个 jar，找一个名字靠眼睛扫是最费的
    // 一件事，而这一页每一趟都是全列的。筛的是"这一条在界面上叫什么"（以及盘上那个文件名
    // ——禁用着的那条盘上带着 `.disabled`，用户手里那个文件确实是这么叫的），
    // 子串、不分大小写（`Sodium.jar` 敲 `sod`、`odium`、`.JAR` 都该中）。
    m_searchRow = new QWidget(this);
    {
        auto *row = new QHBoxLayout(m_searchRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        m_search = new QLineEdit(m_searchRow);
        m_search->setObjectName(searchEditName());
        m_search->setPlaceholderText(searchPlaceholder());
        // 有个"清除"小叉（和「配置文件」那一页左边的条目筛选同一个写法）：这一条是
        // **立刻生效**的筛选，而筛完之后那一格按「刷新」是清不掉的（刷新只重读目录）
        // —— 没有小叉的话用户得自己全选删掉，而那正是"以为筛坏了"的那一刻。
        m_search->setClearButtonEnabled(true);
        // 立刻生效，不走延时器：它只是把行重列一遍，不读盘、不改正文（和 ConfigFileEditorPage
        // 里那条条目筛选同理）。一个目录几十上百条，重建的代价当场就回来了。
        connect(m_search, &QLineEdit::textChanged, this,
                [this](const QString &text) { setSearchText(text); });
        row->addWidget(m_search, 1);
    }
    outer->addWidget(m_searchRow);

    // ---------- 上面那一排：路径 + 刷新 + 打开文件夹 ----------
    m_pathRow = new QWidget(this);
    auto *pathRow = new QHBoxLayout(m_pathRow);
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(8);
    // 这一格写这一页在看哪个目录：文字由 refresh() 灌（目录从 setDirectory() 来，
    // 只有那一处知道）。没有它，上面那一排就是「…… [刷新] [打开文件夹]」，用户
    // 不知道这一页在看哪个目录，而这一页叫「文件管理」。
    // 用 ElidedLabel 而不是普通 QLabel：路径一定比这一格长（实例名本身能到 64 个字），
    // 普通 QLabel 装不下是**硬裁**（最后一级目录名被切掉一半），而这一段文字两头都有用
    // （盘符那头说明是哪个游戏目录，尾巴那头是 mods / saves 哪一页）⇒ 从**中间**省略。
    m_pathLabel = new ElidedLabel(m_pathRow);
    m_pathLabel->setObjectName(pathLabelName());
    m_pathLabel->setElideMode(Qt::ElideMiddle);
    makeSecondary(m_pathLabel);
    // 路径可能很长（实例名本身就能到 64 个字），而它这一格**不许**把整页顶宽：
    // 横向按 Ignored 算 = 布局完全不管它要多大，只给它剩下来的地方，多出来的字被裁掉。
    // （和实例页左栏那条折行 QLabel 顶大整窗 sizeHint 是同一个坑的另一面 —— 那边是
    // 不能让它要宽度，这边是不能让它给宽度。）
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pathLabel->setMinimumWidth(0);
    // NOTE: 这里不挂 `setTextInteractionFlags(Qt::TextSelectableByMouse)`：ElidedLabel
    // 的文字是 paintEvent 自己画的，选中高亮画不出来 —— 用户拖一遍什么反应都没有，
    // 比不提供还费解。整条完整路径由**悬停提示**给（见 refresh() 里 setToolTip 那句），
    // 那是这一格唯一"看得全"的路。
    pathRow->addWidget(m_pathLabel, 1);

    // 顶上那颗「下载新 X」。摆在**路径右边、刷新左边**：它是这一页上
    // 「往里加东西」的那一颗，比刷新/打开文件夹更靠前；而路径仍旧贴着最左边 —— 那一格带
    // 的是"这一页在看哪儿"的身份，把它挪开，这一排就没有主语了。
    //
    // NOTE: 构造时是藏着的（没有文字 = 没有这一颗，见 setDownloadText()）。**每一页都建**
    // 它（这几页是同一段循环搭出来的，只多一个 if 反而要多一处"哪几页有"的知识）。
    m_download = new QPushButton(m_pathRow);
    m_download->setObjectName(downloadButtonName());
    m_download->setVisible(false);
    connect(m_download, &QPushButton::clicked, this, &InstanceFilesPage::downloadRequested);
    pathRow->addWidget(m_download);

    // 顶上那两颗（只有存档那一页露着，见 setImportText / setBatchText）。
    // 摆在**路径右边、下载/刷新左边**：和「下载新 X」同一档 —— 它们也是"这一页上的
    // 动作"，够不着这一页在看哪个目录那件事。
    m_batch = new QPushButton(m_pathRow);
    m_batch->setObjectName(batchButtonName());
    // 「批量编辑」= 打开另一层（一般性）；旁边那两颗「下载新 X」「导入世界」是确认性。
    Theme::markButton(m_batch, Theme::ButtonKind::Neutral);
    m_batch->setVisible(false);
    connect(m_batch, &QPushButton::clicked, this, &InstanceFilesPage::batchEditRequested);
    pathRow->addWidget(m_batch);

    m_import = new QPushButton(m_pathRow);
    m_import->setObjectName(importButtonName());
    m_import->setVisible(false);
    connect(m_import, &QPushButton::clicked, this, &InstanceFilesPage::importRequested);
    pathRow->addWidget(m_import);

    // 顶上那颗「加入选中的世界」（只有存档那一页露着，见 setJoinText）。
    // 摆在「导入世界」右边、刷新左边 —— 和它左边那几颗同一档（"这一页上的动作"）。
    // **不带对象名去查状态**：它发的是"哪一条路径"（见 .hpp 里 joinRequested 那段），
    // 这一层不掺和"那个版本认不认这个参数"。
    m_join = new QPushButton(m_pathRow);
    m_join->setObjectName(joinButtonName());
    m_join->setVisible(false);
    connect(m_join, &QPushButton::clicked, this,
            [this] { emit joinRequested(currentPath()); });
    pathRow->addWidget(m_join);

    m_refresh = new QPushButton(refreshText(), m_pathRow);
    m_refresh->setObjectName(refreshButtonName());
    Theme::markRefreshButton(m_refresh);  // F5 认它
    Theme::markButton(m_refresh, Theme::ButtonKind::Ghost);
    // 刷新是**这一页自己的事**（重新列一遍目录，只有读），不用绕到 MainWindow 去。
    connect(m_refresh, &QPushButton::clicked, this, &InstanceFilesPage::refresh);
    pathRow->addWidget(m_refresh);

    m_openFolder = new QPushButton(openFolderText(), m_pathRow);
    m_openFolder->setObjectName(openFolderButtonName());
    Theme::markButton(m_openFolder, Theme::ButtonKind::Ghost);
    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        const QStringList dirs = scanDirectories();
        emit openFolderRequested(dirs.isEmpty() ? m_directory : dirs.first());
    });
    pathRow->addWidget(m_openFolder);

    outer->addWidget(m_pathRow);

    // ---------- 路径那一排的下面：「结构/原理图」那一页那几排「来源」----------
    //
    // 两排下拉框**横着并排：上面一排是存档的，下面一排是投影的**。
    // 结构方块那半边跟着存档走、投影那半边跟着实例走（见 .hpp 里 setStructureSources 那段）
    // ⇒ 这一页要能同时列两个来源，屏幕上就得先有"看哪几个来源"这一件事。
    //
    // NOTE: 这个容器**每一页都建**（和上面那几颗按钮同一个理由：这几页是同一段循环搭出来的，
    // 多一个 if 就多一处"哪几页有"的知识），但**里面一排都不摆**、自己也藏着 ——
    // 只有「结构/原理图」那一页的调用方会调 setStructureSources()。别的七页因此
    // 一个像素都不变。
    m_structureRows = new QWidget(this);
    m_structureRows->setVisible(false);
    m_structureLayout = new QVBoxLayout(m_structureRows);
    m_structureLayout->setContentsMargins(0, 0, 0, 0);
    m_structureLayout->setSpacing(6);
    outer->addWidget(m_structureRows);

    // ---------- 按钮那排的下面：一个状态选择----------
    //
    // 状态选择在那一排按钮的**下方**，四格是全部 / 启用 / 禁用 / 可更新。顺序就是它们
    // 写在这儿的样子（`stateFilterText()` 是那些字的唯一出处）。
    //
    // 「可更新」这一格**不是每一页都有**（见 setCanFilterUpdates）：它靠的是一趟更新
    // 检测的结果，而只有下载页里真有对应一栏的那四页查得动（原理图 / 存档 / 配置文件
    // 里的文件根本不在任何资源平台的清单里）。
    m_stateRow = new QWidget(this);
    {
        auto *row = new QHBoxLayout(m_stateRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        // 左边那个标签：光一个「全部」摆在那儿，用户不知道它管的是列表的哪一半。
        // （和下载页那一排筛选的下拉框一样，每一格前面都有个标签。）
        // **只有没开批量的那几页还看得到它** —— 开了批量的页摆的是右边
        // 那四颗按钮，而那四颗自己就写着「全部(352) 启用(351) …」，再加一个「显示」
        // 是多余的。
        m_stateLabel = new QLabel(stateLabelText(), m_stateRow);
        m_stateLabel->setObjectName(stateLabelName());
        makeSecondary(m_stateLabel);
        row->addWidget(m_stateLabel);

        m_stateCombo = new QComboBox(m_stateRow);
        m_stateCombo->setObjectName(stateComboName());
        // 滚轮守护：**这一栏和别的下拉框一样会被鼠标轮子误伤**（列表里滚动时指针划过它，
        // 条件就被改了，而屏幕上是"列表自己少了一半"）。全仓同一处（见 WheelGuard）。
        WheelGuard::install(m_stateCombo);
        connect(m_stateCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            setStateFilter(filterAt(m_stateCombo, index));
        });
        // 四格摆进去（信号在这条路上被挡着，见 fillStateCombo）。
        fillStateCombo();
        row->addWidget(m_stateCombo);

        // **四颗横向按钮**（全部 / 启用 / 禁用 / 可更新）。和上面那个下拉框
        // **互相显隐**（见 applyState），
        // 两套说的是同一件事（`m_stateFilter` 是唯一出处）。
        //
        // NOTE: **四颗都建着**，不能筛更新的那几页把「可更新」那一颗藏起来（见
        // fillStateButtons）—— 和「下载新 X」「导入世界」同一套做法：建/不建会让
        // "这一页该有什么"散成两处知识。
        // NOTE: `setAutoExclusive(true)` = 这四颗是一组单选（同一父控件即一组）。
        // **光靠它还不够**：它保证的是"点已经亮着的那一颗不会把它点灭"，而"点一颗
        // 就换条件"那一发走的是下面那个 clicked。
        m_stateButtonsRow = new QWidget(m_stateRow);
        {
            auto *buttons = new QHBoxLayout(m_stateButtonsRow);
            buttons->setContentsMargins(0, 0, 0, 0);
            buttons->setSpacing(8);
            for (int i = 0; i < 4; ++i) {
                const auto filter = static_cast<StateFilter>(i);
                auto *button = new QPushButton(m_stateButtonsRow);
                button->setObjectName(stateButtonName(filter));
                button->setCheckable(true);
                button->setAutoExclusive(true);
                button->setText(stateFilterButtonText(filter, 0));
                button->setToolTip(tr("仅显示%1。").arg(stateFilterText(filter)));
                // 一般性那一档（黑字黑边）：这一排是"筛成什么样"，不是这一页的主语，
                // 不该和顶上那几颗主行动按钮抢眼睛（和「刷新」「打开文件夹」同一档）。
                Theme::markButton(button, Theme::ButtonKind::Neutral);
                // 点一下 = 把条件落到这一档上。**没有别的副作用** —— 这一颗只筛条件，
                // 不发起更新检测（没查过的时候这一颗根本摆不出来，也就没有"点它"这回事）。
                connect(button, &QPushButton::clicked, this,
                        [this, filter] { setStateFilter(filter); });
                m_stateButtons.append(button);
                buttons->addWidget(button);
            }
        }
        row->addWidget(m_stateButtonsRow);
        // 靠左：这一排只管"筛成什么样"，不是这一页的主语（右边留给将来别的东西）。
        row->addStretch(1);
    }
    outer->addWidget(m_stateRow);

    // ---------- 列表 ----------
    m_list = new QListWidget(this);
    m_list->setObjectName(listName());
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setUniformItemSizes(true);
    // 不画外框、不铺底（底色规则见 Theme 样式表里 #instanceFilesList 那条），行底由代理画成圆角淡底。
    m_list->setFrameShape(QFrame::NoFrame);
    // 每一行由 RowDelegate 画（见 .hpp 文件头），整页只有这一个视图、没有行控件。
    m_list->setItemDelegate(createRowDelegate());
    // 悬停那一行 / 那一格要靠 MouseMove 认（见 eventFilter），视口得开着鼠标跟踪。
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    // 行宽跟着视口走，不许出现横向滚动条，否则那一排按钮会跟着来回滑。
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 整页只有这张列表滚动（视口页）：滑条常驻最右一列，行右侧留出内容间距。
    Theme::applyViewportScrollGutter(m_list);
    // 用 itemActivated 而不是 itemDoubleClicked：前者是 Qt 说的「用户激活了这一条」
    // —— 双击**和**选中之后按回车都会发它（各平台按自己的习惯），而那正是"打开一个
    // 文件"该有的两种手势。
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item == nullptr) {
            return;
        }
        const QString path = item->data(pathRole()).toString();
        if (path.isEmpty()) {
            return;
        }
        emit activateRequested(path, QFileInfo(path).isDir());
    });

    // 「选中了哪几条」的**唯一记账处**（见 .hpp 里 m_selectedPaths 那段）。
    // 用户点行身、Ctrl+A、条上那三颗"改选择"的按钮 —— 最后都会走到这一发上。
    // NOTE: 它**不看是哪一路来的**：所以这一处是唯一出处，不必在每条路上各记一遍。
    // 记账那一趟本身收进了 syncSelectedPaths() —— 重列末尾要办的正是同一件事
    // （那一段把信号挡着，只能自己喊），两处各写一遍就是第二份真相。
    // 读屏那条路（无障碍「切换」动作）改的是勾选位：照开关被点了一下办 —— 真去改盘上的名字；
    // 勾选位本身由改名之后那一趟刷新重摆（代理的 editorEvent 不接点击，鼠标 / 空格走不到这里）。
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (item == nullptr || !m_canToggle || !item->flags().testFlag(Qt::ItemIsUserCheckable)) return;
        const bool wantOn = item->checkState() == Qt::Checked;
        if (wantOn == item->data(disabledRole()).toBool()) requestToggle(item->data(pathRole()).toString());
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        syncSelectedPaths();
        updateSelectionBar();
        updateInfoPanel();
    });
    // 同一发信号再挂一条 —— 「加入选中的世界」那颗的灰/亮只跟"有没有选中
    // 一条"走（见 updateJoinEnabled()）。**单独一条连接**而不是塞进上面那个 lambda：
    // 那一条讲的是批量管理那一套（只在那四页上谈得上），这一条在存档那一页上。
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            [this] { updateJoinEnabled(); });
    // 那条悬浮操作条的父控件是 `m_list` ⇒ 它（或者它的 viewport）变大小了得自己重算位置
    // （见 eventFilter）。
    if (m_list->viewport() != nullptr) {
        m_list->viewport()->installEventFilter(this);

    // 右键（空白处；行上右键是"开详情页"，见 eventFilter）：对选中的单个文件给出
    // 「打开方式…」和「在资源管理器中显示」。打开方式只调系统对话框，不自己执行。
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    ItemViewContextMenu::install(m_list);  // 菜单键 / Shift+F10：弹在当前行
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QString path;
        if (const QListWidgetItem *item = m_list->itemAt(pos)) {
            path = item->data(pathRole()).toString();
        }
        if (path.isEmpty()) {
            const auto selected = m_list->selectedItems();
            if (selected.size() == 1) path = selected.first()->data(pathRole()).toString();
        }
        QMenu menu(this);
        QAction *openWith = menu.addAction(tr("打开方式…"));
        openWith->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
        QAction *reveal = menu.addAction(tr("在资源管理器中显示"));
        reveal->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
        QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
        if (chosen == nullptr || path.isEmpty()) return;
        if (chosen == openWith) {
            ShellOpenWith::openWithDialog(this, path);
        } else if (chosen == reveal) {
            QProcess::startDetached(QStringLiteral("explorer.exe"),
                                    {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
        }
    });
    }
    // **滚动之后按光标重认悬停那一行**：滚轮一动，光标底下那一行往往就换人了，而鼠标一个像素
    // 都没动 ⇒ 没有 MouseMove 可等（见 syncRowHover）。
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncRowHover(); });
    m_selectionTools = new QWidget(this);
    m_selectionTools->setObjectName(QStringLiteral("modSelectionTools"));
    m_selectionTools->hide();
    outer->addWidget(m_selectionTools);
    m_contentHost = new QWidget(this);
    m_contentHost->setMinimumWidth(0);
    auto *contentLayout = new QHBoxLayout(m_contentHost);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addWidget(m_list);
    m_contentHost->installEventFilter(this);
    outer->addWidget(m_contentHost, 1);

    // ---------- 列表下面那条"这一份是什么"----------
    //
    // 原理图 / 结构那一页要给元数据、方块统计和那句版本兼容提示。
    // **这一层算不出那些**（要读 gzip NBT、要认 Litematica 的位打包，全在 src/ 里；
    // 这一层不 include src/）—— 它只摆一格标签，选中一条文件时发 infoRequested()，
    // 由接的人算好再 `setInfoText()` 递回来。
    //
    // 默认藏着：别的七页一个像素都不变（`setInfoPanelEnabled(true)` 才露）。
    m_info = new QLabel(this);
    m_info->setObjectName(infoLabelName());
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setVisible(false);
    makeSecondary(m_info);
    auto *infoRow = new QHBoxLayout;
    infoRow->addWidget(m_info, 1);
    m_infoDetails = new QPushButton(tr("方块详情"), this);
    m_infoDetails->setObjectName(QStringLiteral("instanceSchematicDetails"));
    Theme::markButton(m_infoDetails, Theme::ButtonKind::Neutral);
    m_infoDetails->hide();
    infoRow->addWidget(m_infoDetails);
    outer->addLayout(infoRow);
    connect(m_infoDetails, &QPushButton::clicked, this, [this] {
        if (!m_infoPath.isEmpty()) emit infoDetailsRequested(m_infoPath);
    });

    // 连点合并：在一个文件上停 150 ms 才真去读。读一份大投影要几百毫秒（gzip 还要
    // 起一次 PowerShell 解压），用户快速翻行时不该每一行都读一遍。
    m_infoTimer = new QTimer(this);
    m_infoTimer->setSingleShot(true);
    m_infoTimer->setInterval(150);
    connect(m_infoTimer, &QTimer::timeout, this, [this] {
        if (!m_infoPanelEnabled || m_infoPath.isEmpty()) {
            return;
        }
        emit infoRequested(m_infoPath);
    });

    // ---------- 底下那条**悬浮**操作条----------
    //
    // 父控件是 **`m_list` 本身**、**不进任何布局**：进了布局就变成"列表下面另起一行"，
    // 而这一条要**悬浮在列表下缘**。位置由 repositionSelectionBar() 算，重算的时机是
    // viewport 的 Resize（见 eventFilter）。
    //
    // 父控件必须是 `m_list`，不能是 `m_list->viewport()`：挂在 viewport 底下的话，
    // 滚动滚轮时这一条会跟着跑。根子在 Qt 那边：
    // `QAbstractItemViewPrivate::scrollContentsBy()`（`qabstractitemview_p.h`）滚的是
    // `viewport->scroll(dx, dy)`，而 **`QWidget::scroll()` 是"连孩子一起挪"的**
    // （`QWidgetPrivate::scroll_sys()` 第一句就是 `scrollChildren(dx, dy)`，Qt 6 里那句
    // 注释写着 QTBUG-138381）—— 于是每滚一格，这条浮层就跟着挪一格，滚到头就飘出视口。
    // 挂在 `m_list` 底下就**从结构上没有这个毛病**：被滚的只有 viewport 那一棵，
    // 而它是 `m_list` 的**兄弟**，不是它的孩子。
    // ⚠ 代价是坐标要多加一个 `viewport->pos()`（见 repositionSelectionBar），因为
    // 现在算出来的还是 viewport 坐标系里的位置。压在上面靠 `raise()`（applyState 里）。
    //
    // NOTE: 它**一直建着**（哪怕这一页没开批量）—— 显不显由 updateSelectionBar() 说了算。
    // 建/不建会让"这一页有没有这一条"变成第二处知识，而且 `setBatchMode(true)` 是
    // 在构造之后才叫的（`InstanceSettingsDialog` 那一轮 setter），那时候再补建控件
    // 就得把这一整段搬进一个函数里。
    m_selectionBar = new QWidget(m_list);
    m_selectionBar->setObjectName(selectionBarName());
    m_selectionBar->setVisible(false);
    // 底色：**压在列表上面的一块浮层**，所以走 `Part::Panel`（那一档的出厂色比窗口底色再深
    // 一档，正是"从底下那堆东西里分出来"）。**不写死颜色** —— 主题一换跟着走。
    // 为什么不是 `Part::Card`：那一档的出厂色就是 `Base`，而列表的底**也是** `Base` ——
    // 同色等于这条操作条根本看不出来（只有按钮浮在行上面）。Panel 才是"压上来的一层"。
    m_selectionBar->setAutoFillBackground(true);
    m_selectionBar->setPalette([]{ // 现算一份：`QApplication::palette()` 是当前主题那一份
        QPalette barPalette = QApplication::palette();
        barPalette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::Panel));
        return barPalette;
    }());
    {
        auto *bar = new QHBoxLayout(m_selectionBar);
        bar->setContentsMargins(kSelectionBarPadding, kSelectionBarPadding,
                                kSelectionBarPadding, kSelectionBarPadding);
        bar->setSpacing(kSelectionBarSpacing);
        m_selectionCount = new QLabel(m_selectionBar);
        m_selectionCount->setObjectName(selectionCountName());
        m_selectionCount->setText(selectionCountText(0));
        // 这一格**要多少给多少**：这一条**不进布局、宽度是
        // repositionSelectionBar() 现算的**，窗口窄的时候得压得住 —— 而承接压缩的该是
        // 那七颗按钮（它们少几个像素也就是边距窄一点），不是这句「已选择 N 个文件」
        // （它被压了就是少一个字，屏幕上是「已选择 2 个文」，而且 Qt **不给 QLabel
        // 省略号**，看着像写错了）。`Fixed` 就是"按 sizeHint 给，不多也不少"。
        // NOTE: 七颗的 `minimumWidth` 已经在下面那一段放到 0 了（不给就压不动）。
        m_selectionCount->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        m_selectionCount->setMinimumWidth(0);
        bar->addWidget(m_selectionCount);

        for (int i = 0; i < selectionActionCount(); ++i) {
            const auto action = static_cast<SelectionAction>(i);
            auto *button = new QPushButton(selectionActionText(action), m_selectionBar);
            button->setObjectName(selectionButtonName(action));
            // 「窄窗口下这一条怎么让位」：给每一颗一个**按自己那两个字的
            // 宽度算出来的下限**（`QPushButton::minimumSizeHint()` 默认把 `padding: 6px`
            // 那 12 + 边框 2 也算进去，比这个还大 —— 七颗一加起来就顶得这一条比视口还宽，
            // 于是布局开始按比例压，**被压掉的是最左边那句「已选择 N 个文件」**，屏幕上
            // 看着像"字写错了"）。压到"刚好放得下这两个字"为止就有了下限，再窄也不会
            // 把字截掉，而多出来的那点宽度按 Qt 的规矩平分给七颗。
            button->setMinimumWidth(button->fontMetrics().horizontalAdvance(
                                        selectionActionText(action)) +
                                    14);  // 6*2 padding + 1*2 border —— 和 Theme 里那份 QSS 一致
            // 三档颜色的分工：**更新 = 主题色那一颗**（它是这条上"正经要做的事"），
            // **删除 = 红的那一颗**（危险，"点下去会没"），其余一般性（黑字黑边）。
            // 走 `Theme::markButton()` —— 全仓按钮外观的唯一出处，不许自己写颜色。
            Theme::markButton(button, action == SelectionAction::Update  ? Theme::ButtonKind::Accent
                                      : action == SelectionAction::Delete ? Theme::ButtonKind::Danger
                                                                          : Theme::ButtonKind::Neutral);
            switch (action) {
            case SelectionAction::Update:
                button->setToolTip(selectionUpdateTip());
                connect(button, &QPushButton::clicked, this, [this] {
                    emit batchUpdateRequested(selectedPathList());
                });
                break;
            case SelectionAction::Disable:
            case SelectionAction::Enable: {
                const bool disable = action == SelectionAction::Disable;
                button->setToolTip(selectionToggleTip(disable));
                connect(button, &QPushButton::clicked, this, [this, disable] {
                    const QStringList paths = selectedTogglePaths(disable);
                    requestToggle(paths, disable);
                });
                break;
            }
            case SelectionAction::SelectAll:
                button->setToolTip(selectionPickTip(action));
                // 全选 = `selectAll()`：它选的就是"列表里现在有的那些行"，而列表里现在有的
                // 正是**筛出来的那些**（见 rebuildRows）。选择一变，
                // itemSelectionChanged 会把账和这一条一起刷新。
                connect(button, &QPushButton::clicked, m_list, &QListWidget::selectAll);
                break;
            case SelectionAction::InvertAll:
                button->setToolTip(selectionPickTip(action));
                connect(button, &QPushButton::clicked, this, [this] {
                    if (m_list->count() == 0) return;
                    // One selection-model transaction emits one change. Updating
                    // each row separately recomputed the entire action panel N times.
                    const QItemSelection range(m_list->model()->index(0, 0),
                        m_list->model()->index(m_list->count() - 1, 0));
                    m_list->selectionModel()->select(range,
                        QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
                });
                break;
            case SelectionAction::ClearAll:
                button->setToolTip(selectionPickTip(action));
                connect(button, &QPushButton::clicked, m_list, &QListWidget::clearSelection);
                break;
            case SelectionAction::Delete:
                button->setToolTip(tr("删除选中文件，执行前确认。"));
                connect(button, &QPushButton::clicked, this, [this] {
                    emit batchDeleteRequested(selectedPathList());
                });
                break;
            case SelectionAction::Count:
                break;
            }
            m_selectionButtons.append(button);
            bar->addWidget(button);
        }
    }
    // 这一条**常驻**（对话框一开就在那儿）⇒ 主题换了得自己重算底色（`Theme::follow` 那段
    // NOTE 说得很清楚：现建的那些不用登记，常驻的必须登记）。
    Theme::follow(m_selectionBar, [this] {
        QPalette barPalette = QApplication::palette();
        barPalette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::Panel));
        m_selectionBar->setPalette(barPalette);
    });

    // ---------- 空 / 用不上时那一段话 ----------
    m_notice = new QLabel(this);
    m_notice->setObjectName(noticeLabelName());
    m_notice->setWordWrap(true);
    m_notice->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    makeSecondary(m_notice);
    outer->addWidget(m_notice, 1);

    // 目录统计那一趟的节拍器（见 fillDirectoryStats / m_statQueue 那段）。单发 + 0 毫秒：
    // 每排一次就在**下一个事件循环**里干一行，干完自己再排下一次。
    m_statTimer = new QTimer(this);
    m_statTimer->setSingleShot(true);
    m_statTimer->setInterval(0);
    connect(m_statTimer, &QTimer::timeout, this, &InstanceFilesPage::fillDirectoryStats);

    fillStateButtons();

    // 摆到"目录还没接上"那一档：构造函数里 setDirectory() 还没被叫过，这会儿既没东西可列、
    // 也没得可点，露出一个空框比说一句话更让人以为坏了。
    applyState();
}

// ---------- 这一页能做什么、那几颗按钮写着什么 ----------
// 在 `ui/InstanceFilesPageCapabilities.cpp`（同一个类，另一个翻译单元）。


// ---------- 「结构 / 原理图」那一页顶上的两排来源 ----------
// 在 `ui/InstanceFilesPageStructures.cpp`（同一个类，另一个翻译单元）。


QStringList InstanceFilesPage::scanDirectories() const {
    if (m_structureSources.isEmpty()) {
        // 别的七页：就是那一个目录。
        return m_directory.isEmpty() ? QStringList() : QStringList{m_directory};
    }
    QStringList dirs;
    for (int row = 0; row < m_structureSources.size(); ++row) {
        const QString path = selectedStructurePath(row);
        if (!path.isEmpty() && !dirs.contains(path)) {
            dirs.append(path);
        }
    }
    return dirs;
}

bool InstanceFilesPage::isCurrentPage() const {
    // 往上找**最外面**那个堆叠容器，看它现在摆的是不是我（或者我的祖先）。
    // - 取最外面那一个：问的是"用户在实例设置里看的是哪一页"，那一层就是
    // `InstanceSettingsDialog::m_stack`。往里那几层（`save` / `config` 自己套的两格堆叠
    // —— 列表 / 编辑器）不是这个问题：取最近的话，摆着「概览」时那两页**照样**会被判成
    // "当前"，欠账那一套对它们就白设了（而这两个目录正是行最多的那几个之一）。
    // - 判据是"当前那一格是不是我或我的祖先"：这一页外面还套着一层 `QScrollArea`
    // （见 `InstanceSettingsDialog::addPage()`），拿 `==` 比会永远为假。
    // 不认 `isVisible()`：offscreen 下（三个验证工程）那个函数**恒为 false**，在"还没
    // show 出来"的窗口里也恒为 false —— 拿它当判据等于这一页永远不列。
    const QStackedWidget *stack = nullptr;
    for (const QWidget *w = this; w != nullptr; w = w->parentWidget()) {
        if (const auto *found = qobject_cast<const QStackedWidget *>(w)) {
            stack = found;
        }
    }
    if (stack == nullptr) {
        return true;  // 不在堆叠容器里：没有"当前页"这回事（单独用这个控件的那一档）
    }
    const QWidget *current = stack->currentWidget();
    return current == this || (current != nullptr && current->isAncestorOf(this));
}

// ---------- 搜索 + 状态筛选 ----------
// 在 `ui/InstanceFilesPageFilter.cpp`（同一个类，另一个翻译单元）；
// `entryMatchesFilter()` 也在那边（它读的就是这三个条件）。


void InstanceFilesPage::setUnavailable(const QString &reason) {
    m_unavailable = reason;
    applyState();
}

QString InstanceFilesPage::formatSize(qint64 bytes) {
    // 单位一律走字面量而不是 tr()：它们是量纲不是话（换语言也不该跟着换），而且验证工程
    // 按这几句核对 —— 那几条断言得有一个**不会随语言变**的基准。
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

// ---------- 重列前后那一份滚动锚点 ----------
// 在 `ui/InstanceFilesPageScroll.cpp`（同一个类，另一个翻译单元）。


// ---------- 把目录列成条目（扫描 / 禁用后缀 / 目录与文件两档）----------
// 在 `ui/InstanceFilesPageRefresh.cpp`（同一个类，另一个翻译单元）。



QLabel *InstanceFilesPage::pathLabel() const {
    // 只在**这里**把 ElidedLabel 当成 QLabel 交出去（见 .hpp 里那段）：那一格的确是
    // ElidedLabel，但调用方要的只有 setText / text，不必知道它是怎么画出来的。
    return m_pathLabel;
}

// ---------- 列表下面那条"这一份是什么" ----------
// 在 `ui/InstanceFilesPageInfoPanel.cpp`（同一个类，另一个翻译单元）。


void InstanceFilesPage::setWorldLayout(QPushButton *backupButton) {
    if (m_worldLayout) return;
    m_worldLayout = true;
    setStateFilter(StateFilter::All);
    setCompactHeader(backupButton);
    m_search->setPlaceholderText(tr("搜索存档名称"));
    m_join->setText(tr("加入世界"));
    applyState();
}

void InstanceFilesPage::placeAboveList(QWidget *widget) {
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    if (widget == nullptr || outer == nullptr) return;
    outer->removeWidget(widget);
    outer->insertWidget(outer->indexOf(m_contentHost), widget);
}

void InstanceFilesPage::setCompactHeader(QPushButton *leadingAction) {
    if (m_compactHeader) return;
    m_compactHeader = true;
    // 路径挪去搜索行后这一排只剩按钮：换成可折行的布局，英文放不下时「刷新 / 打开文件夹」折下去。
    delete m_pathRow->layout();
    auto *actions = new FlowLayout(8, 8);
    for (QPushButton *button : {leadingAction, m_download, m_batch, m_import, m_join})
        if (button) actions->addWidget(button);
    actions->addStretch();
    auto *tail = new QWidget(m_pathRow);  // 这两颗成组，一起换行
    auto *tailRow = new QHBoxLayout(tail);
    tailRow->setContentsMargins(0, 0, 0, 0);
    tailRow->setSpacing(8);
    tailRow->addWidget(m_refresh);
    tailRow->addWidget(m_openFolder);
    actions->addWidget(tail);
    m_pathRow->setLayout(actions);
    auto *search = qobject_cast<QHBoxLayout *>(m_searchRow->layout());
    search->setStretchFactor(m_search, 2);
    search->addWidget(m_pathLabel, 1);
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    outer->removeWidget(m_pathRow);
    outer->insertWidget(0, m_pathRow);
    applyState();
}

// ---------- 这一页摆成哪一档（不可用 / 空 / 筛没了 / 有东西）----------
// 在 `ui/InstanceFilesPageState.cpp`（同一个类，另一个翻译单元）。


// ---------- 底下那条悬浮操作条----------

const InstanceFilesPage::Entry *InstanceFilesPage::entryForPath(const QString &path) const {
    if (path.isEmpty()) {
        return nullptr;
    }
    for (const Entry &entry : m_entries) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

QString InstanceFilesPage::currentPath() const {
    if (m_list == nullptr) {
        return QString();
    }
    // 用视图自己那份"现在选着什么"（不是 currentItem：那一格在**重列之后**可能还指着旧
    // 位置，而 selectedItems() 是重建时按路径还回去之后的结果，和 selectedPathList()
    // 同一个口径）。
    const QList<QListWidgetItem *> selected = m_list->selectedItems();
    if (selected.isEmpty() || selected.first() == nullptr) {
        return QString();
    }
    return selected.first()->data(pathRole()).toString();
}

QPushButton *InstanceFilesPage::stateButtonAt(StateFilter filter) const {
    const int index = static_cast<int>(filter);
    if (index < 0 || index >= m_stateButtons.size()) {
        return nullptr;
    }
    return m_stateButtons.at(index);
}

// ---------- 底下那行统计：每个目录多大、世界上次什么时候玩的 ----------
// 在 `ui/InstanceFilesPageDirectoryStats.cpp`（同一个类，另一个翻译单元）。


// ---------- 行控件上的事件路由（浮条重定位 / 悬停露详情 / 右键进详情页）----------
// 在 `ui/InstanceFilesPageEventFilter.cpp`（同一个类，另一个翻译单元）。



