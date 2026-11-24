#include "ConnectionsController.hpp"
#include <QCoreApplication>
#include "MainWindow.hpp"
#include "controllers/ResourceInstallController.hpp"
#include "Animations.hpp"
#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "SettingsPageParts.hpp"
#include "OverlayHost.hpp"
#include "InstanceSettingsDialog.hpp"
#include "InstanceFilesPage.hpp"
#include "TaskCenterPanel.hpp"
#include "TitleBar.hpp"
#include "RowActivation.hpp"
#include "auth/AuthManager.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/InstallPageController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/JavaSettingsController.hpp"
#include "controllers/PlayerSkinController.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/ServerMenuController.hpp"
#include "download/ResourceStore.hpp"
#include "download/DownloadRate.hpp"
#include <memory>
#include <QTimer>
#include <QDesktopServices>
#include <QMenu>
#include <QUrl>
#include "ItemViewContextMenu.hpp"
#include "ListRowDelegate.hpp"
#include "util/AppPaths.hpp"
#include <QElapsedTimer>
#include "launcher/LauncherManager.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/VersionManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "server/ServerInstallRun.hpp"
#include "tasks/TaskManager.hpp"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QSignalBlocker>

ConnectionsController::ConnectionsController(MainWindow *owner) : QObject(owner), m_owner(owner) {}

namespace {
QString instanceIdOf(const QTreeWidgetItem *item) {
    if (item == nullptr || item->data(0, MainWindowUI::groupIdRole()).isValid()) return {};
    return item->data(0, MainWindowUI::instanceIdRole()).toString();
}
void syncComboToTree(QComboBox *combo, const QTreeWidget *tree) {
    const QString id = instanceIdOf(tree->currentItem());
    if (id.isEmpty()) return;
    const int index = combo->findData(id);
    if (index >= 0 && combo->currentIndex() != index) combo->setCurrentIndex(index);
}
void syncTreeToCombo(const QComboBox *combo, QTreeWidget *tree) {
    const QString id = combo == nullptr ? QString() : combo->currentData().toString();
    const QSignalBlocker blocker(tree);
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        if (instanceIdOf(*it) == id && !id.isEmpty()) { tree->setCurrentItem(*it); return; }
    }
    tree->setCurrentItem(nullptr);
}
QList<Tasks::TaskStep> taskStepsFromNames(const QStringList &names, int runningIndex) {
    QList<Tasks::TaskStep> steps;
    for (int i = 0; i < names.size(); ++i) {
        Tasks::TaskStep step;
        step.name = names.at(i);
        step.state = i < runningIndex ? Tasks::TaskStepState::Done
                   : i == runningIndex ? Tasks::TaskStepState::Running
                                       : Tasks::TaskStepState::Pending;
        steps.append(step);
    }
    return steps;
}
}

void ConnectionsController::setup() {
    // UI 按钮信号
    connect(m_owner->m_ui->getStartButton(), &QPushButton::clicked, m_owner, &MainWindow::onStartButtonClicked);
    // 主页实例带的收起状态：启动时直接还原（不播动画），之后每次开合落盘。
    m_owner->m_ui->setHomeQuickCollapsed(m_owner->m_settings->homeQuickCollapsed(), false);
    m_owner->m_ui->setHomeQuickCollapsedHandler(
        [owner = m_owner](bool collapsed) { owner->m_settings->setHomeQuickCollapsed(collapsed); });
    if (QSpinBox *spin = m_owner->m_ui->getHomeFavoriteCountSpin()) {
        spin->setValue(m_owner->m_settings->homeFavoriteCount());
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), m_owner, [this](int value) {
            m_owner->m_settings->setHomeFavoriteCount(value);
            m_owner->updateHomeRecent();
        });
    }
    if (QSpinBox *spin = m_owner->m_ui->getHomeRecentCountSpin()) {
        spin->setValue(m_owner->m_settings->homeRecentCount());
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), m_owner, [this](int value) {
            m_owner->m_settings->setHomeRecentCount(value);
            m_owner->updateHomeRecent();
        });
    }
    // NOTE: 这里不接「离线模式」勾选框：让身份变的动作是**换账号 / 加一个离线账号**，
    // 而那两个槽自己就会调 m_owner->updateBottomBar()，所以这里不需要对应的接线
    // （底栏名字下面那行写的是"这次启动用哪种身份"）。
    // 「账号与外观…」的隐藏兼容入口 + 设置页入口。这两条仍然直通那个窗。
    //
    // 主页那张账号卡片（`getAccountSkinButton()`）**不接在这一串里** ——
    // 它点开的是"最近用过的账号"那块面板（接线在 MainWindowUI 自己那儿，见
    // `toggleAccountsPanel()`），而进「账号与外观」窗的那条路在面板**末行**那一颗。
    // 两边都留着的话，点一下卡片会同时冒出面板和那个窗。
    connect(m_owner->m_ui->getAccountSkinSettingsButton(), &QPushButton::clicked, m_owner,
            &MainWindow::onAccountSkinButtonClicked);
    connect(m_owner->m_ui->getAddFolderButton(), &QPushButton::clicked, m_owner,
            &MainWindow::onAddFolderClicked);
    // NOTE: 「移除」只在行尾齿轮菜单里，走
    // m_owner->onFolderActionRequested() → m_owner->removeFolderFromList()。
    // NOTE: 实例页右栏只剩「分组设置…」这一颗（挪到「实例列表」那个框外面的右上角）。
    // 「实例设置…」与「重命名…」不在这里接线：前者走"点这一条实例行"（行里的
    // 任意处）和行尾那颗常显的齿轮，两条都落在 MainWindow::eventFilter 里；
    // 后者在「实例设置」那一页，走 m_owner->openInstanceSettings() 里那条路。
    connect(m_owner->m_ui->getGroupInstanceButton(), &QPushButton::clicked, m_owner,
            &MainWindow::onGroupInstanceClicked);
    // NOTE: 停游戏只有底栏左面板里那行电源那条路
    // （m_owner->onBottomBarPanelAction() → m_owner->stopGameSession()）。
    // 「导入整合包…」在左栏「游戏文件夹」那一组（「添加文件夹…」下面）。
    connect(m_owner->m_ui->getImportModpackButton(), &QPushButton::clicked, m_owner,
            &MainWindow::onImportModpackClicked);
    // ---------- 实例页顶部的模式切换 + 服务端那一页 ----------
    //
    // 三颗模式按钮互斥（点一颗就把另外两颗的勾摘掉）—— `m_owner->setInstanceMode()` 是"停在哪一页 +
    // 谁勾着"的唯一出处，所以这里只把"用户点了哪一颗"交给它，不自己动 setChecked。
    {
        const int modeCount = 3;
        for (int i = 0; i < modeCount; ++i) {
            const auto mode = static_cast<MainWindowUI::InstanceMode>(i);
            QPushButton *button = m_owner->m_ui->getInstanceModeButton(mode);
            if (button == nullptr) {
                continue;
            }
            connect(button, &QPushButton::clicked, m_owner,
                    [this, mode] { m_owner->setInstanceMode(mode); });
        }
        if (QPushButton *newServer = m_owner->m_ui->getNewServerButton()) {
            connect(newServer, &QPushButton::clicked, m_owner, &MainWindow::openNewServerPage);
        }
        // 服务端列表和客户端列表同一套手势：单击一行 = 进这个服务端的设置；悬停三格 =
        // 文件夹 / 更多（同右键菜单，含控制台）/ 启动·停止·修复；双击不再开控制台
        // （单击已经跳走，双击的第一下就把页换了）。
        if (QTreeWidget *serverList = m_owner->m_ui->getServerList()) {
            connectRowActivation(serverList, [this](QTreeWidgetItem *item) {
                const QString id = item->data(0, Qt::UserRole).toString();
                if (!id.isEmpty()) {
                    m_owner->openServerSettingsPage(id);
                }
            });
            // 悬停三格由 MainWindow::eventFilter 在视口上认下（几何同 ListRowDelegate::actionRect）。
            serverList->viewport()->installEventFilter(m_owner);
            if (auto *rowDelegate = qobject_cast<ListRowDelegate *>(serverList->itemDelegate())) {
                connect(rowDelegate, &ListRowDelegate::actionTriggered, m_owner,
                        [this, serverList](const QModelIndex &index, ListRowDelegate::Action action) {
                    const QString id = index.data(Qt::UserRole).toString();
                    if (id.isEmpty()) return;
                    if (action == ListRowDelegate::Action::Folder) {
                        m_owner->openServerFolder(id);
                    } else if (action == ListRowDelegate::Action::Launch) {
                        m_owner->activateServerRow(id);
                    } else {
                        const QRect more = ListRowDelegate::actionRect(serverList->visualRect(index),
                                                                       ListRowDelegate::Action::More);
                        m_owner->m_serverMenu->showFor(
                            id, serverList->viewport()->mapToGlobal(more.bottomLeft()));
                    }
                });
            }
            // 右键那一发（`setContextMenuPolicy(Qt::CustomContextMenu)` 在 MainWindowUI 里设的）。
            connect(serverList, &QTreeWidget::customContextMenuRequested, m_owner->m_serverMenu,
                    &ServerMenuController::showAt);
        }
    }
    // 左边列表选中哪一份 .minecraft，启动器就换到哪一份上（PCL 的卡片是同一个交互）。
    connect(m_owner->m_ui->getFolderList(), &QListWidget::currentRowChanged, m_owner,
            &MainWindow::onFolderSelectionChanged);
    // 行尾那颗齿轮弹出来的菜单：界面只报"哪一份文件夹、第几项"，做哪件事是这边的事。
    m_owner->m_ui->setFolderActionHandler([this](const QString &path, int action) {
        m_owner->onFolderActionRequested(path, action);
    });
    // FolderManager 才是改状态的那一方：加载/删除目录、当前目录不存在时的退回，
    // 都会从它这儿发信号出来，界面只管跟着重画。
    connect(m_owner->m_folderManager, &FolderManager::foldersChanged, m_owner, [this] {
        m_owner->refreshFolderList();
        // 列表变了 = 可借的来源也变了（加进来的 / 移走的），重新推一遍。
        m_owner->syncDownloadReuseSources();
    });
    connect(m_owner->m_folderManager, &FolderManager::directoryAccessDenied, m_owner,
        [this](const QString &problem) {
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("目录正在使用"), problem);
            m_owner->refreshFolderList();
        });
    connect(m_owner->m_folderManager, &FolderManager::currentFolderChanged, m_owner, [this] {
        m_owner->applyCurrentFolderToPaths();
        m_owner->refreshFolderList();
        // 换了一整份 .minecraft，三件跟着"上一份"走的事要重置：
        // - "目录名和 jar 名对不上"的告警只报一次，新目录得重新能报；
        // - 本机 Java 列表是**按根目录**扫的（自管 Java 在 <数据目录>/java 下），
        // 下次进设置页要重扫一遍。★ ：那份结果整个换掉（scanLocalJava
        // 直接赋值给 m_owner->m_localJavaVersions），所以下面连着它一起丢 —— 不能往一个
        // 下拉框里"往里加"，否则上一个根目录里那份自管 Java 会一直留在框里（它指着的是个
        // 真实存在的 java.exe，不碍事，但会误导下一次选择）。
        // - ★ 第三件：**版本 json 的两张内存缓存表**。它们的键只有版本
        // id（= 目录名），而两份 .minecraft 里各有一个 `1.20.1` 是常态 —— 不丢的话，
        // 切过来再启动同名的那个实例，classpath / natives / javaVersion / assetIndex
        // 全按**上一份目录**里那份 json 算，而游戏自己去读新目录的 json，症状是
        // "换文件夹之后启动的不是这个版本"。详见
        // VersionManager::clearVersionJsonCache()。
        m_owner->m_reportedUnrecognizedVersions = false;
        m_owner->m_javaSettingsController->resetDiscovery();
        m_owner->m_versionManager->clearVersionJsonCache();
        // 版本列表、实例列表、按钮文字、底栏全都对着新根目录重来一遍。
        // 分组**不需要**在这里重置或重读：InstanceGroupStore 每个访问器都会比一下
        // "这次问的是哪一份 .minecraft"，不一样就自己重读（见它的文件头）。
        m_owner->refreshInstalledVersions();
        // 预设跟着游戏文件夹走（`Presets::presetsRoot()` = 当前文件夹下的 `presets/`），
        // 换了一整份 .minecraft 就是换了一整套预设：页面这会儿正开着的话当场重灌。
        m_owner->m_presetController->refreshList();
        // 换游戏文件夹也算"点击左边栏、右边栏跟着变"（用户 ：「除了切换页面，
        // 点击左边栏导致右边栏发生变化的时候也要加动画」）。
        // 挂在这儿、而不是挂在左栏那一次点击上：这一发**就是**"右边那一列真的换了一整份"
        // 的判据 —— 同一个文件夹再点一下，`FolderManager::setCurrentFolder()` 直接返回、
        // 这条根本不响，所以它天生不会"点了一下没换也淡一遍"。
        // NOTE: 窗口还没露面那一趟（开程序时认当前文件夹）由 `isVisible()` 挡掉 —— 和顶上
        // 四颗选项卡那条接线逐字同一个判据（见 MainWindowUI.cpp 里 `appearOnSwitch`）。
        // 底不垫色：写回的是树和它祖先们原样那层（磨砂透着），见 `Anim::pageAppear()`。
        // 抬的是树**的视口**（`liftTarget()`，`ui/Animations.cpp` 里那个），不是树自己：这棵树自带那圈框（还有
        // 顶上那条表头），树本身换文件夹前后**都在原地** —— 整棵跟着挪就是"框瞬移"
        // （用户 报的那件事）。见 `Animations.hpp` 里 `pageAppear()` 那段。
        if (QTreeWidget *list = m_owner->m_ui->getInstanceList()) {
            if (list->isVisible()) {
                Anim::pageAppear(list);
            }
        }
    });
    // 分组数据变了（对话框里确定之后、改名之后 rekey）。重灌实例页那张树。
    connect(m_owner->m_groupStore, &InstanceGroupStore::groupsChanged, m_owner,
            &MainWindow::refreshInstalledVersions);
    // 用户在"已安装版本"下拉框里换一个版本，按钮第二行立刻跟着变。
    // NOTE: 这条只管用户手选。fillVersionCombo() 重灌下拉框时信号是被挡住的，
    // 那条路径由 m_owner->refreshInstalledVersions() 自己收尾，两条路合起来才不会漏。
    // NOTE: 接的是 currentIndexChanged 而不是 currentTextChanged。后者在"两个条目
    // 渲染成同一串文字"之间切换时**不发射** —— QComboBoxPrivate::updateCurrentText()
    // 发现文本没变就提前 return（qcombobox.cpp:2956-2963）。而按下标看就不会漏：
    // 关掉「版本名」档之后，`我的整合包-Forge` 和 `别的名字-Forge` 完全可能显示成
    // 一样，那时按钮第二行和 tooltip 会停在旧值上。
    connect(m_owner->m_ui->getInstalledVersionCombo(), &QComboBox::currentIndexChanged, m_owner,
            [this] {
                // 用户手选的才记成「上次选中」；刷新时自动挑的那一下（wtfmlAutoPick）不记。
                QComboBox *installed = m_owner->m_ui->getInstalledVersionCombo();
                if (!installed->property("wtfmlAutoPick").toBool()) {
                    // 手选了 ⇒ 不再等那个被挤掉的实例回来（见 refreshInstalledVersions()）。
                    installed->setProperty("wtfmlDisplacedId", QVariant());
                    if (const QString id = MainWindowViewData::selectedVersionId(installed); !id.isEmpty()) {
                        m_owner->m_settings->setLastSelectedVersion(id);
                    }
                }
                m_owner->updateStartButtonText();
                m_owner->updateHomeRecent();
                // 底栏左下角那条状态要看"选中的是不是正在跑的那个实例"，
                // 所以换版本也得跟着刷一遍。
                m_owner->updateBottomBar();
                // 实例页右边那张树是同一份数据的另一种看法，跟着一起亮。
                syncTreeToCombo(m_owner->m_ui->getInstalledVersionCombo(), m_owner->m_ui->getInstanceList());
                // 离线账号那张脸**跟着选中的版本走**：游戏按客户端 JAR 里那套规矩分配默认
                // 皮肤，两套规矩算出来的是两张不同的图（见 DefaultSkin.hpp 文件头），所以
                // 换版本就得重算一遍。**只有离线账号会做实事**：别的账号在
                // m_owner->startPlayerSkinLoad() 里当场就返回了（那是网络那条线，和版本无关）。
                // 算过的那一份在盘上，重算是两次 QFile::exists，不会卡。
                if (m_owner->offlineIdentity()) {
                    m_owner->startPlayerSkinLoad();
                }
            });
    // 树 → 下拉框。点树里的一行 = 在上面把它选中（用户要求的行为）。
    // 选中组头时不动作（syncComboToTree 里的守卫）—— 点一个组的标题不是在挑版本。
    connect(m_owner->m_ui->getInstanceList(), &QTreeWidget::currentItemChanged, m_owner, [this] {
        syncComboToTree(m_owner->m_ui->getInstalledVersionCombo(), m_owner->m_ui->getInstanceList());
    });
    // 点**组头**那一下 = 展开/收起，而且当场记下来 —— 下次打开还照这样画（见 InstanceGroupStore::
    // Group::expanded）。点实例那一行仍旧只是"把它选中"（上面那条 currentItemChanged）。
    //
    // 整条行都是热区：那一行除了"展开/收起"没有别的事可做，而"点了没反应"是用户唯一会
    // 当成启动器坏了的情况（和版本树那几条分类同一条规矩）。
    //
    // NOTE: 先算再做（`!item->isExpanded()` 在 setExpanded() 之前求值）—— 和加载器树那
    // 一句同一个写法、同一个教训（见那里的 NOTE）。
    const auto onInstanceTreeActivated = [this](QTreeWidgetItem *item) {
        if (item == nullptr) {
            return;
        }
        if (!item->data(0, MainWindowUI::rowGroupGearRole()).toBool()) {
            // **实例行**：点这一行（空白处、左边那个图标、版本名 —— 除了行尾那三颗按钮
            // 之外的任何地方）= 进这个实例的设置。行尾那三颗由 eventFilter 先吃掉，落不到
            // 这里来（见那一段；它们三个各自是"别的事"，不该顺带把设置页也开了）。
            //
            // NOTE: 这一下**同时**把它选中（视图在按下那一刻就把 currentItem 挪过来了），
            // 那条路照旧、它同步到主页那个下拉框。两件事不冲突：选中是给"启动"用的，
            // 进设置是这一下点名的另一件事 —— 用户明确要的是"点这一行就进设置"。
            m_owner->openInstanceSettings(item->data(0, MainWindowUI::instanceIdRole()).toString());
            return;
        }
        const bool expand = !item->isExpanded();
        item->setExpanded(expand);
        // 筛着的时候**不落盘**（「清空搜索恢复原展开状态」）：筛着的时候
        // 每一组都是被我们摆成展开的，用户这一下点的是"这一次先收起来看看"，不是"以后
        // 默认都收着"。落盘的话，清空搜索之后那一组的**原**展开状态就被这一次点击悄悄
        // 改掉了 —— 而清空搜索之后要回到的正是它原来的样子。
        if (!m_owner->instanceFilterQuery().isActive()) {
            m_owner->rememberGroupExpanded(item->data(0, MainWindowUI::groupIdRole()).toString(), expand);
        }
    };
    // 接线口和另外三棵树同一条（鼠标单击 + 键盘回车），另外交待**哪些行是"开/关"**：
    // 点得快的第二发（以及后面每一发）到的是双击事件，只有开关行才补那一发。
    // 组头就是开关行；实例行是"选中它"，算做事（那是 currentItemChanged 那条路的事，
    // 双击不该办两遍）。
    connectRowActivation(m_owner->m_ui->getInstanceList(), onInstanceTreeActivated,
                         [](QTreeWidgetItem *item) {
                             return item != nullptr
                                    && item->data(0, MainWindowUI::rowGroupGearRole()).toBool();
                         });
    // 组行行尾那颗**齿轮**：它不是"点这一行"，所以不走上面那条路 —— 走**视口上的事件
    // 过滤器**，判据是 InstanceGroupDelegate::gearRect()（和代理画它的那一格是同一份
    // 几何）。过滤器是为了**吃掉**那一下：不然点齿轮会顺手把这一组展开/收起
    // （和加载器树上那个「取消」小叉、下载页那几个刷新圈完全同一个路数）。
    m_owner->m_ui->getInstanceList()->viewport()->installEventFilter(m_owner);
    // 实例行悬停快捷操作（ListRowDelegate::actionTriggered，由 eventFilter 在按下时发出）：
    // 文件夹 = 打开实例目录；启动 = 选中它并启动；更多 = 实例设置 / 收藏 / 删除（原行尾三颗）。
    if (auto *rowDelegate = qobject_cast<ListRowDelegate *>(m_owner->m_ui->getInstanceList()->itemDelegate())) {
        connect(rowDelegate, &ListRowDelegate::actionTriggered, m_owner,
                [this](const QModelIndex &index, ListRowDelegate::Action action) {
            const QString id = index.data(MainWindowUI::instanceIdRole()).toString();
            if (id.isEmpty()) return;
            if (action == ListRowDelegate::Action::Folder) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::versionDir(id)));
                return;
            }
            if (action == ListRowDelegate::Action::Launch) {
                QComboBox *combo = m_owner->m_ui->getInstalledVersionCombo();
                const int row = combo == nullptr ? -1 : combo->findData(id);
                if (row >= 0) {
                    combo->setCurrentIndex(row);
                    m_owner->onStartButtonClicked();
                }
                return;
            }
            QTreeWidget *tree = m_owner->m_ui->getInstanceList();
            auto *menu = new QMenu(tree);
            menu->setObjectName(MainWindowUI::instanceRowMenuName());
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->addAction(MainWindowUI::rowInstanceSettingsTipText(), m_owner,
                            [this, id] { m_owner->openInstanceSettings(id); });
            const bool canPin = index.data(MainWindowUI::rowPinRole()).toBool();
            const bool canUnpin = index.data(MainWindowUI::rowUnpinRole()).toBool();
            if (canPin || canUnpin) {
                menu->addAction(canUnpin ? MainWindowUI::rowUnpinTipText() : MainWindowUI::rowPinTipText(), m_owner,
                                [this, id, canUnpin] { canUnpin ? m_owner->unpinInstance(id) : m_owner->pinInstance(id); });
            }
            menu->addSeparator();
            menu->addAction(MainWindowUI::rowDeleteInstanceTipText(), m_owner,
                            [this, id] { m_owner->confirmDeleteInstance(id); });
            const QRect more = ListRowDelegate::actionRect(tree->visualRect(index), ListRowDelegate::Action::More);
            menu->popup(tree->viewport()->mapToGlobal(more.bottomLeft()));
        });
    }
    // 菜单键 / Shift+F10（鼠标右键同走这里）：实例行弹「更多」那份菜单，组行弹齿轮那份菜单，
    // 都落在当前行附近（落点见 ItemViewContextMenu）。
    {
        QTreeWidget *instanceTree = m_owner->m_ui->getInstanceList();
        instanceTree->setContextMenuPolicy(Qt::CustomContextMenu);
        ItemViewContextMenu::install(instanceTree);
        connect(instanceTree, &QTreeWidget::customContextMenuRequested, m_owner,
                [this, instanceTree](const QPoint &pos) {
            QTreeWidgetItem *item = instanceTree->itemAt(pos);
            if (item == nullptr) return;
            if (item->data(0, MainWindowUI::rowGroupGearRole()).toBool()) {
                m_owner->openGroupMenuFor(item);
                return;
            }
            if (auto *rowDelegate = qobject_cast<ListRowDelegate *>(instanceTree->itemDelegate())) {
                emit rowDelegate->actionTriggered(instanceTree->indexFromItem(item), ListRowDelegate::Action::More);
            }
        });
    }
    // 组行行尾那颗齿轮里那几项点了做什么（界面只报"哪一组、第几项"）。
    m_owner->m_ui->setGroupActionHandler([this](const QString &groupId, int action) {
        m_owner->onGroupActionRequested(groupId, action);
    });
    // 齿轮里「把选中的实例移入分组…」弹出的**第二个**框（挑目标）点了哪一组。
    m_owner->m_ui->setGroupPickHandler([this](const QString &groupId) {
        m_owner->setSelectedInstanceHomeGroup(groupId);
    });
    // ---------- 实例列表的搜索与筛选 ----------
    //
    // 两条线都汇到 `m_owner->rebuildInstanceTree()`：**它自己现读**那几个控件（
    // `m_owner->instanceFilterQuery()`），所以这儿不必把"点了什么/打了什么"当参数传下去 ——
    // 条件只有一个出处，就是那几个控件此刻的样子。
    //
    // NOTE: 不接 `textEdited` + 防抖那一套：这条路**不扫盘**（原料是
    // `m_owner->m_instancePageParts`，见那一格），重灌一次只是建几十个树条目。真机 61 个
    // 实例实测不卡 —— 需要防抖的话它会是这条路先冒出来的问题。
    if (QLineEdit *search = m_owner->m_ui->getInstanceSearchEdit()) {
        connect(search, &QLineEdit::textChanged, m_owner, [this] { m_owner->rebuildInstanceTree(); });
    }
    for (int i = 0; i < 4; ++i) {
        const auto which = static_cast<MainWindowUI::InstanceFilterButton>(i);
        QPushButton *button = m_owner->m_ui->getInstanceFilterButton(which);
        if (button == nullptr) {
            continue;
        }
        connect(button, &QPushButton::clicked, m_owner, [this, which] {
            // 原版 / 模组**互斥**：点一颗就把另一颗摘掉（用户点的是"我要看哪一类"，
            // 不是"再加一条条件"）。另外两颗（可更新 / 最近运行）各管各的、不动别人。
            const bool pair = which == MainWindowUI::InstanceFilterButton::Vanilla
                              || which == MainWindowUI::InstanceFilterButton::Modded;
            if (pair) {
                const auto other = which == MainWindowUI::InstanceFilterButton::Vanilla
                                       ? MainWindowUI::InstanceFilterButton::Modded
                                       : MainWindowUI::InstanceFilterButton::Vanilla;
                if (QPushButton *sibling = m_owner->m_ui->getInstanceFilterButton(other)) {
                    // 挡信号：这一趟收尾**自己会**重灌一遍树，让 setChecked 绕一圈回去
                    // 只会多灌一次（`toggled` 没接线，`clicked` 才是。这里挡它是因为
                    // 将来谁把线接到 toggled 上时不会莫名其妙灌两遍）。
                    const QSignalBlocker blocker(sibling);
                    sibling->setChecked(false);
                }
            }
            m_owner->rebuildInstanceTree();
        });
    }
    // ---------- 下载页「游戏」那两层：版本树 → 安装页 ----------
    //
    // 点树里**一个版本号**（子条目）进安装页；点**分类**（顶层那四条）只展开/折叠 ——
    // 用户点的是那一行最右边那个 >/V，箭头就画在行上。判据是"有没有父条目"：分类存的是
    // 分类键、版本号存的是版本号，两者都不是空串，光看 userData 分不清这两种。
    //
    // 整个行都是热区（不要求用户精确点在那 12 像素的箭头上）：那一行除了"展开"没有
    // 别的事可做，而"点了没反应"是用户唯一会当成启动器坏了的情况。
    //
    // 共用一份 lambda 而不是一个槽函数：槽声明里带 QTreeWidgetItem* 就得让 moc 也认得
    // 这个类型（等于把这个头拖进 QtWidgets 的树控件），而这里类型本来就是完整的。
    const auto openFromTreeItem = [this](QTreeWidgetItem *item) {
        if (item == nullptr) {
            return;
        }
        if (item->parent() == nullptr) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        const QString versionId =
            item->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
        if (!versionId.isEmpty()) {
            m_owner->openInstallPage(versionId);
        }
    };
    // 鼠标和键盘走**同一个**接线口（见 ui/RowActivation.hpp）：两条路都指向上面那份 lambda，
    // 而且一次手势只进来一次。两条各接一条不行 —— Windows 上双击两发都到，于是
    // m_owner->openInstallPage() 会跑两遍（第二遍把四个在飞的加载器版本请求 abort 掉重发），
    // 分类那一行的展开/收起也会被翻两次（看上去就是"双击没反应"）。
    // 第三件事只有这一棵树要交待：**哪些行的激活是"开/关"**。分类那四条（顶层）是，
    // 点得快的时候第二发（以及后面每一发）到的是双击事件、itemClicked 不发它 —— 不补上
    // 用户看到的就是"一直点，有几次没反应"。版本号那一条是"进安装页"，算做事：双击仍然
    // 只办一次（不然安装页开两遍）。
    connectRowActivation(m_owner->m_ui->getVersionTree(), openFromTreeItem,
                         [](QTreeWidgetItem *item) { return item->parent() == nullptr; });
    connect(m_owner->m_ui->getInstallBackButton(), &QPushButton::clicked, m_owner,
            &MainWindow::backToVersionTree);
    connect(m_owner->m_ui->getInstallButton(), &QPushButton::clicked, m_owner->m_installPage,
            &InstallPageController::installClicked);
    // 实例预设那一行：它是**加载器树里的一条**，点它是
    // 开那张四类弹窗（在 onLoaderTreeClicked 里按 UserRole 认出来）—— 没有单独的控件
    // 要接线。
    // 名称框：每敲一个字都要重算那句小字（重名提示要"实时"——要求）。
    connect(m_owner->m_ui->getInstallNameEdit(), &QLineEdit::textChanged, m_owner,
            &MainWindow::onInstallNameEdited);
    // 加载器那棵树（安装页）：**点一下**那条里的两件事分开办 ——
    // · 点顶层（一个加载器）：**只展开/收起**那一条。展开是"看看有哪些版本"，
    // 看一眼不等于选了它 —— **选中是点版本号那一下的事**；
    // · 点「不装（纯原版）」：它底下没有版本可展开，点它就是选中它。它也是换加载器的
    // 唯一退路 —— 选中某一条之后其余几条会变灰（见 m_owner->updateLoaderRows()），
    // 没有它就走不回去了；
    // · 点子条目（一个版本号）：选中它爹那条加载器 + 把这一条钉在这个版本上，**收起**
    // 那条，并把版本号写回它的第二行小字。
    // 共用一份 lambda 而不是槽：槽签名里带 QTreeWidgetItem* 就得让 moc 也认得这个类型
    // （版本树那边同一个理由，见 openFromTreeItem 上面那段）。
    const auto onLoaderTreeClicked = [this](QTreeWidgetItem *item) {
        if (item == nullptr) {
            return;
        }
        // 灰的那几条（选中某个加载器之后其余几条、"正在获取…"/"列表没取到"那两个占位）
        // **不是能点的东西**：点它什么都不该发生。放在最前面是因为"点不动"这件事对
        // 顶层和子条目是同一个道理。
        if (item->isDisabled()) {
            return;
        }
        if (item->parent() == nullptr) {
            const int index =
                item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt();
            // 「预设」那一行：不是加载器（UserRole 是哨兵），点它开弹窗、**不展开**。
            // 这一段必须在「不装（纯原版）」那条判断之前 —— 否则它会被当成某个加载器。
            if (index == MainWindowUI::installPresetRowRole()) {
                m_owner->openInstallPresetPicker();
                return;
            }
            if (index == MainWindowUI::modLoaderNoneIndex()) {
                m_owner->m_installPage->selectLoader(index);
                return;
            }
            // NOTE: 这里**先算再做**（`!item->isExpanded()` 在动状态之前求值）：
            // 反过来的话读到的是**已被改过**的 isExpanded()（这一支里若夹着
            // m_owner->selectLoader()，它会顺手把这一条撑开）。这一支除了 setExpanded
            // 一行都不做，写法仍按这个来 —— 它是这一句话唯一安全的写法。
            const bool expand = !item->isExpanded();
            item->setExpanded(expand);
            return;
        }
        QTreeWidgetItem *loader = item->parent();
        const int index =
            loader->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt();
        // userData 里可能是那两档的键、也可能是一个具体版本号（见
        // MainWindowUI::loaderVersionLatestText 上面那段）—— 这里原样存下来，认它的是
        // 数据层（LoaderMeta::loaderRequestFromChoice，安装那条路开头解码一次）。
        const QString version =
            item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toString();
        // 走到这里就是"选中了这条加载器的这个版本"（上面那句 isDisabled() 已经把
        // "正在获取…"/"列表没取到"那两个灰占位挡在外面了 —— 不挡的话"列表没取到"会被
        // 当成一个选中的版本号填进默认名里）。
        m_owner->m_installPage->setPinnedLoaderVersion(index, version);
        m_owner->m_installPage->selectLoader(index);
        loader->setExpanded(false);
        // 钉的版本号是默认名的一部分（`1.20.1-Forge_47.4.10`），换一条就得重算。
        m_owner->m_installPage->updateInstallName();
    };
    // 接线口和版本树那一条完全一样（鼠标单击 + 键盘回车），另外交待**哪些行是"开/关"**：
    // 点得快的第二发到的是双击事件，只有开关行才补那一发。按上面那三条判据划：
    // 顶层那几条里除「不装」以外都是开关（「不装」点它是选中，算做事），
    // 灰的那几条点不动（handler 开头就 return 了，不算）；子条目（版本号）是选中，算做事。
    connectRowActivation(
        m_owner->m_ui->getLoaderTree(), onLoaderTreeClicked, [](QTreeWidgetItem *item) {
            if (item->parent() != nullptr || item->isDisabled()) {
                return false;
            }
            const int role = item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt();
            // 哨兵那一行（「预设」）是**做事**（开弹窗），不是开关：双击不能补第二发。
            if (role == MainWindowUI::installPresetRowRole()) {
                return false;
            }
            return role != MainWindowUI::modLoaderNoneIndex();
        });
    // 行里那个「取消」小叉（选中那条加载器右边那个 ×，见 m_owner->updateLoaderRows()）：它不是
    // "点这一行"，所以不走上面两条路 —— 走**视口上的事件过滤器**，判据是
    // VersionTreeDelegate::clearRect()（和代理画它的那一格是同一份几何）。
    // NOTE: 过滤器是为了**吃掉**那一下：Qt 那边 itemClicked 是在鼠标松开时发的，而
    // "点中了行里的一个按钮"和"点中了这一行"必须是两件事 —— 不然点叉会顺手把那一列
    // 展开/收起，用户看到的是"我按了取消，它却收起来了"。
    m_owner->m_ui->getLoaderTree()->viewport()->installEventFilter(m_owner);
    // 下载页左栏那份分类列表的行尾也有"行里的一个按钮"：每个条目右边那个刷新
    // （见 MainWindow::refreshDownloadCategory()）。同一个理由挂同一个过滤器 —— 那一下
    // 必须被**吃掉**，不然它会顺着 Qt 那条"点中了这一行"的路再走一遍（切页，以及空页
    // 自己搜的那一发），和我们自己要做的事叠在一起。"切不切、搜不搜"由
    // m_owner->refreshDownloadCategory() 一处说了算。
    if (QListWidget *categories = m_owner->m_ui->getDownloadCategoryList()) {
        categories->viewport()->installEventFilter(m_owner);
    }
    // 「下载」页左栏那份分类（游戏 / 整合包 / 模组 / 资源包 / 光影）：点哪个，右栏那份
    // QStackedWidget 就翻到哪一页。**"第几个分类 = 第几页"这条对应关系是界面自己搭出来的**
    // （同一个表、同一个顺序一页一页加进去），这里只做一次越界保护：分类表和页数将来要是
    // 对不上，宁可停在原来那页，也不要 setCurrentIndex(-1) 把右栏整片清空 —— 那种"点一下
    // 界面就空了"的毛病，用户只会当成崩了。
    connect(m_owner->m_ui->getDownloadCategoryList(), &QListWidget::currentRowChanged, m_owner,
            [this](int row) {
                QStackedWidget *stack = m_owner->m_ui->getDownloadStack();
                if (row >= 0 && row < stack->count()) {
                    stack->setCurrentIndex(row);
                }
                // 页头那行说明跟着当前这一类走（写的是"当前状态"）。
                // 摆在这儿是**唯一**一处 —— 翻类有两条路（点条目、页头那颗刷新切过去），
                // 两条最后都会走到 `currentRowChanged` 上，各写一遍迟早会有一边漏。
                m_owner->m_ui->setDownloadHeaderRow(row);
                if (row == MainWindowUI::downloadCategoryServerIndex()
                    && !m_owner->m_suppressGameInstallServerJump) {
                    QStackedWidget *gameStack = m_owner->m_ui->getGameStack();
                    if (gameStack != nullptr
                        && gameStack->currentIndex() == MainWindowUI::downloadGameInstallIndex()
                        && !m_owner->m_installPage->gameVersion().isEmpty()) {
                        m_owner->openServerPageForGameVersion(m_owner->m_installPage->gameVersion());
                        return;
                    }
                }
                const QStringList keys = MainWindowUI::downloadCategoryKeys();
                ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
                if (row >= 0 && row < keys.size()
                    && ResourceCatalog::kindFromCategoryKey(keys.at(row), &kind)) {
                    m_owner->ensureResourcePageLoaded(kind);
                }
            });
    if (QStackedWidget *serverStack = m_owner->m_ui->getServerStack()) {
        connect(serverStack, &QStackedWidget::currentChanged, m_owner, [this](int index) {
            if (index != MainWindowUI::downloadServerFormIndex()) {
                m_owner->cancelPendingServerVersionProbe();
            }
        });
    }
    if (QStackedWidget *downloadStack = m_owner->m_ui->getDownloadStack()) {
        connect(downloadStack, &QStackedWidget::currentChanged, m_owner, [this](int index) {
            if (index != MainWindowUI::downloadCategoryServerIndex()) {
                m_owner->cancelPendingServerVersionProbe();
            }
        });
    }
    // 左栏顶上那颗「← 返回实例设置」。**整个下载页一颗**，摆在左栏顶上
    // —— 所以这里不是一圈循环，就接一次。
    //
    // NOTE: 那颗键要回的是"当初从哪个实例点进来"，而那个实例只有一个出处
    // （`m_owner->m_downloadReturn`，由 openDownloadCategory 摆、由这个槽消费掉）。
    // 把实例号塞进信号里也能做，但那样"哪一位是活的"就有两份，而"按下时用的那一位"
    // 和"露出来的那一颗"必须永远是同一个。
    if (QPushButton *back = m_owner->m_ui->getDownloadBackButton()) {
        connect(back, &QPushButton::clicked, m_owner,
                &MainWindow::returnToInstanceSettingsFromDownload);
    }
    // 下载页**页头右上角**那颗刷新（ 加的）。用户那份改版文档把刷新归到
    // "当前分类自己的页面头部"—— 于是它和分类行行尾那一颗做的是同一件事（`m_owner->refreshDownloadCategory()`），
    // 只是作用对象不用再从"点在哪一行"推：就是**现在停着的那一类**。
    // NOTE: 传的是 `currentRow()` 而不是捕获下来的某一行 —— 那颗键一直活着，翻类之后
    // 它必须跟着新那一类走。
    if (QPushButton *refresh = m_owner->m_ui->getDownloadHeaderRefreshButton()) {
        connect(refresh, &QPushButton::clicked, m_owner, [this]() {
            QListWidget *categories = m_owner->m_ui->getDownloadCategoryList();
            m_owner->refreshDownloadCategory(categories != nullptr ? categories->currentRow() : -1);
        });
    }
    // 下载页「服务端」那一栏里那颗「新建服务端…」。
    // **和实例页那颗是同一个动作**（`MainWindow::openNewServerPage()`），这里只是把
    // 那条路也摆到下载页上 —— 别另开一条"在下载页里建服务端"的实现。
    if (QPushButton *newServer = m_owner->m_ui->getDownloadNewServerButton()) {
        connect(newServer, &QPushButton::clicked, m_owner, &MainWindow::openNewServerPage);
    }
    // 顶上那四颗导航。
    //
    // **只管 MainWindowUI 管不着的那一件**：翻页和"点的是当前这一颗就把覆盖层收起来"那两条
    // 仍旧由 MainWindowUI 自己接的那一条负责（见那边搭导航那段）。这边补的一条只做
    // 下载页那一件 —— 那一层够不着"下载页的首页是哪一栏、筛子该怎么清"。
    //
    // UI 先完成翻页 / 收页，再用一条带完整上下文的信号通知业务层。
    // 不再依赖“哪条 connect 先执行”这种 Qt 隐式时序。
    connect(m_owner->m_ui->titleBar(), &TitleBar::navigationApplied, m_owner,
            &MainWindow::onNavigationApplied);
    // ---------- 下载页后面那几页：搜索框 + 三层资源表 ----------
    //
    // 这几页走的是同一条路，所以这里是**一圈循环**：每一页接上"回车就搜"和"点一下就动"，
    // 三个回复信号接的全是同一个槽（槽里按请求钥匙认回来的是哪一页）。
    //
    // NOTE: 那两个 lambda 收的是 Kind 而不是 res 的键 —— 用值捕获，每页一份，
    // 不会在循环结束后全都指向最后一页。
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        const QString key = ResourceCatalog::kindKey(kind);
        QLineEdit *edit = m_owner->m_ui->getDownloadSearchEdit(key);
        QTreeWidget *tree = m_owner->m_ui->getDownloadResourceTree(key);
        if (edit == nullptr || tree == nullptr) {
            // MainWindowUI 那边这几页是一段循环搭出来的，这里少一个说明"分类键"这套约定
            // 两边对不上了。**说出来**，不然表现是"某一页点了没反应"，没人查得到。
            qWarning() << "下载页缺少控件，分类键：" << key;
            continue;
        }
        // 回车（returnPressed）才搜，不是边打字边搜：一次搜索是一条请求，而这个接口按分钟
        // 限流；边打边搜等于把额度花在"用户打到一半的那些词"上。
        // NOTE: 信号带一个 QString（编辑框里的文字），这个槽不收参数，多余的那个会被丢掉
        // （Qt 的新式 connect 允许槽少收参数）。
        connect(edit, &QLineEdit::returnPressed, m_owner, [this, kind] { m_owner->startResourceSearch(kind); });

        // ---------- 旁边那一行筛子----------
        //
        // 「搜索」那颗键：和名称框里按回车是**同一件事**。
        if (QPushButton *searchButton = m_owner->m_ui->getDownloadFilterSearchButton(key)) {
            connect(searchButton, &QPushButton::clicked, m_owner,
                    [this, kind] { m_owner->startResourceSearch(kind); });
        }
        // 「清空条件」：把名称框和四个筛子退回"不筛"，然后**重搜一发**（清完不动的话，
        // 界面和列表就对不上 —— 六个框全空着，底下还摆着上一批按老条件筛出来的结果）。
        if (QPushButton *resetButton = m_owner->m_ui->getDownloadFilterResetButton(key)) {
            connect(resetButton, &QPushButton::clicked, m_owner, [this, kind] {
                m_owner->resetDownloadFilters(kind);
                m_owner->startResourceSearch(kind);
            });
        }
        // 「版本」那一格也可输入，在里面按回车**同样等同于按搜索键**。
        //
        // NOTE: 必须接 `lineEdit()->returnPressed`，不能接下拉框自己的 `returnPressed` ——
        // 后者只在"框里那串字正好是列表里选中的那一项"时才发（Qt 的
        // QComboBox::returnPressed 就是 `lineEdit()->returnPressed` 加了一道
        // "有没有选中项"的闸，见它文档里那句）。而这一格存在的理由正是**用户自己
        // 打一个清单里没有的版本号**，那一发会被那道闸吃掉：回车下去什么都没发生。
        // NOTE: `lineEdit()` 只有在 `setEditable(true)` 之后才不是空的（界面那边已经设了，
        // 见 MainWindowUI 里 makeFilterCombo 那一段），所以这里要判一下。
        if (QComboBox *versionCombo = m_owner->m_ui->getDownloadFilterVersionCombo(key)) {
            if (QLineEdit *versionEdit = versionCombo->lineEdit()) {
                connect(versionEdit, &QLineEdit::returnPressed, m_owner, [this, kind, key] {
                    // 先记一笔，理由见下面那个 activated 里那段（同一按回车会来两发）。
                    m_owner->m_versionEnterSearches.insert(key);
                    QTimer::singleShot(0, m_owner,
                                       [this, key] { m_owner->m_versionEnterSearches.remove(key); });
                    m_owner->startResourceSearch(kind);
                });
            }
            // 「版本」是从下拉里挑的（而不是手打的字）⇒ 挑完就搜。
            //
            // NOTE: 这一发**和上面那一发会撞**：这一格是**可输入的**，在框里按回车时 Qt
            // 先发 `QLineEdit::returnPressed`（上面那条路），紧接着发
            // `editingFinished`，而 QComboBox 自己接了后者 —— 框里那串字正好是清单里
            // 某一项时它补一次 setCurrentIndex + activated（见 Qt 6.10 的
            // QComboBoxPrivate::editingFinished 与 QLineEditPrivate::controlEditingFinished
            // 那两句的先后）。两发都通到"搜一发"，不拦的话同一按回车会搜两次、
            // 烧两发限流额度。判据就是上面 `returnPressed` 记的那一笔 —— **同一回合里
            // 只认第一发**（那一笔由 0 毫秒的单次定时器抹掉）。
            connect(versionCombo, &QComboBox::activated, m_owner, [this, kind, key](int) {
                if (m_owner->m_versionEnterSearches.contains(key)) {
                    return;
                }
                m_owner->startResourceSearch(kind);
            });
        }
        // ---------- 四个筛子：在下拉里挑了一项 ⇒ 立刻重搜----------
        //
        // 分工是：**筛子（下拉）挑一项 = 立刻搜；框里打的字 = 只有回车那一下搜**。
        //
        // NOTE: 这四格接的是 `activated` 而不是 `currentIndexChanged`。后者在**程序**改
        // 的时候也发（灌清单、换来源时把加载器/类型退回「全部」、清空条件），而"用户
        // 在下拉里挑了一项"只有 activated 说得清（Qt 只在 itemSelected /
        // editingFinished 里发它）。两个好处：灌数据不会自己搜自己，而
        // `setCurrentIndex()` 这种"改到一半"的中间态也不会漏出去一发。
        //
        // 「来源」换了一项：这一页的加载器 / 类型跟着换一份选项（两边的键不是一套），
        // 换完**就按新来源搜一发**。
        if (QComboBox *sourceCombo = m_owner->m_ui->getDownloadFilterSourceCombo(key)) {
            // 顺序是**对的**：用户挑那一项时 Qt 先 setCurrentIndex（⇒ 下面这条先把选项
            // 换掉）再发 activated（⇒ 这一发读到的已经是新选项），见 QComboBoxPrivate::
            // itemSelected 那两句的先后。
            connect(sourceCombo, &QComboBox::currentIndexChanged, m_owner,
                    [this, kind] { m_owner->refreshDownloadFilterOptions(kind); });
            connect(sourceCombo, &QComboBox::activated, m_owner,
                    [this, kind](int) { m_owner->startResourceSearch(kind); });
        }
        // 「模组加载器」（**只有模组那一页有这个控件**，别的页回空指针）和「类型」。
        if (QComboBox *loaderCombo = m_owner->m_ui->getDownloadFilterLoaderCombo(key)) {
            connect(loaderCombo, &QComboBox::activated, m_owner,
                    [this, kind](int) { m_owner->startResourceSearch(kind); });
        }
        if (QComboBox *typeCombo = m_owner->m_ui->getDownloadFilterTypeCombo(key)) {
            connect(typeCombo, &QComboBox::activated, m_owner,
                    [this, kind](int) { m_owner->startResourceSearch(kind); });
        }
        // 点一行：三种行的行为不一样，判据在 m_owner->onResourceItemActivated() 里（那里是唯一出处）。
        // 鼠标和键盘走同一个接线口（见 ui/RowActivation.hpp）—— 这里**必须**只进来一次：
        // 第三层那一行"点它就是下它"，而双击的两发里第二发会撞上"已有下载任务正在进行"，
        // 界面于是先弹一句假的「下载失败」，顺手把待导入记录也吃掉了（真的整合包下完就
        // 再也不会走导入）。
        // NOTE: 起那笔记录只在"这一批真的开起来了"之后才登记（见
        // downloadResourceVersion 里那句），假失败那一发带的是同一个 label，
        // 但它根本没开出批次，登记这一步压根没走到 —— 吃不到它了。
        // 这里仍然只该进来一次：假失败本身是假的，双击仍只该下一次。
        // 哪几行是"开/关"：**上面两层**（资源、游戏版本）是，第三层那一行（版本号）
        // 是"下它"，算做事 —— 双击仍然只下一次。这个判据和 m_owner->onResourceItemActivated()
        // 里那个 m_owner->resourceRowDepth() 说的是同一件事（那边 2 才是下），两处必须一起看。
        connectRowActivation(tree, [this](QTreeWidgetItem *item) { m_owner->onResourceItemActivated(item); },
                             [this](QTreeWidgetItem *item) {
                                 // 写"0 或 1"而不是"<= 1"：树上认不出来的行回 -1，
                                 // 那个也**不是**开关行。
                                 const int depth = m_owner->resourceRowDepth(item);
                                 return depth == 0 || depth == 1;
                             });
        // 点行身 = 打开详情页，点箭头 = 展开/收起。这一发走**事件过滤器**
        // 而不是上面那个接线口：接线的那个只给"点中了哪个条目"、不给"点在条目里的哪儿"
        // （箭头是行里的一格），和加载器树上那个「取消」小叉是同一个形状的判据
        // —— 见 MainWindow::eventFilter 里"下载页那四张资源表"那一段。
        //
        // NOTE: 装的是**视口**（鼠标事件按光标位置投递给视口）—— 和实例树、加载器树、
        // 左栏那份分类列表三处一模一样。
        // 这一句**必须排在上面那句 connectRowActivation 之后**：Qt 的事件过滤器是
        // "后装的先跑"，而上面那个接线口会给**开关行**的每一发双击补一次激活
        // （见 RowActivation.hpp 里那段"点得快的时候每一下都要算"）。排在它前面的话，
        // "双击行身"那一发会先被它当成展开/收起办掉、再被我们吃掉 —— 症状是"点开这一页
        // 之前，底下那棵树自己展开又收起了一下"。
        // （那个接线口自己也挂在视口上，所以这两句的先后**就是**那两条过滤器的先后。）
        tree->viewport()->installEventFilter(m_owner);
        // 图标是**懒要**的：条目建出来之后才 requestIcon()，不然图标回来时没有行可以放
        // （而且 store 命中缓存时是同步发的，见 requestIcon 的 NOTE）。
    }
    // 下载页「整合包」那一页那颗「导入整合包…」走的**就是**实例页那颗同一个槽：
    // 同一条路、同一套"选文件 → 问实例名 → 挑 Java"，没有理由抄两份。
    // 两颗按钮的区别只在它们在哪儿，做的事是同一件。
    if (QPushButton *downloadImport = m_owner->m_ui->getDownloadFilterImportModpackButton()) {
        connect(downloadImport, &QPushButton::clicked, m_owner, &MainWindow::onImportModpackClicked);
    }
    connect(m_owner->m_resourceStore, &ResourceStore::searchFinished, m_owner,
            &MainWindow::onResourceSearchFinished);
    connect(m_owner->m_resourceStore, &ResourceStore::versionsFinished, m_owner,
            &MainWindow::onResourceVersionsFinished);
    connect(m_owner->m_resourceStore, &ResourceStore::iconReady, m_owner,
            &MainWindow::onResourceIconReady);
    // 同一个 `versionsFinished` 上**再挂两个接的人**（前置模组那一趟、
    // 更新检测那一趟）。Qt 允许多个槽接同一个信号，而**那三处各自按自己的钥匙认人**
    // （详情页那把、`ModDependencies::Lookup::requestKey`、更新检测那把）—— 所以
    // 谁的回复谁接走，不会互相顶掉。**不要**为了"省两个连接"把它们并成一个槽：
    // 那样这个函数会变成三段 if，而三件事的生命周期根本不一样（详情页那一页关掉就没了、
    // 前置那一趟跟着一次点击、更新检测那一趟跟着一次检查）。
    connect(m_owner->m_resourceStore, &ResourceStore::versionsFinished,
            m_owner->m_resourceInstall, &ResourceInstallController::onVersionsFinished);
    connect(m_owner->m_resourceStore, &ResourceStore::versionsFinished, m_owner,
            &MainWindow::onUpdateCheckVersionsFinished);
    connect(m_owner->m_resourceStore, &ResourceStore::latestVersionsFinished, m_owner,
            &MainWindow::onUpdateCheckLatestFinished);
    connect(m_owner->m_resourceStore, &ResourceStore::projectsFinished,
            m_owner->m_resourceInstall, &ResourceInstallController::onProjectsFinished);
    // 详情页那一行前置要的名字。**和上面那一趟分开两个槽**，理由同 `versionsFinished`
    // 那三条 —— 两件事各有各的钥匙，各自认人（见 :: 后面那段 ★）。
    connect(m_owner->m_resourceStore, &ResourceStore::projectsFinished, m_owner,
            [owner = m_owner](const QString &requestKey, const QList<ResourceCatalog::Hit> &hits,
                              const QString &problem) {
                // 那一页的状态在控制器里，而控制器是**懒建**的（点过详情页才有）——
                // 没建出来就等于这一页没开，这一发没人要。
                if (owner->m_resourcePage != nullptr) {
                    owner->m_resourcePage->onProjectsFinished(requestKey, hits, problem);
                }
            });
    // ---------- 设置页 ----------
    // 「Java 版本」那一格现在是一颗按钮：点开「Java 管理」那一页去看/去装。
    // 那一页自己只发信号（它不认识 Java），点哪一行、点哪颗按钮都在下面这几个槽里落地。
    connect(m_owner->m_ui->getJavaManageButton(), &QPushButton::clicked, m_owner,
            &MainWindow::openJavaManage);
    // 「实例预设」那一页 搬进了实例页的模式行（第三档），设置页里那颗门跟着撤了 ——
    // 接线在下面「模式切换」那一段（三颗 chip 同一个槽）。
    // 「实例指定的 Java 版本不符时提醒」这个总闸。管的是**启动时那个三岔口**
    // （onStartButtonClicked 里问的那一次），不是这一页的显示。
    connect(m_owner->m_ui->getJavaWarnMismatchCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onJavaWarnMismatchChanged);
    // 内存：「最大内存」那一行的**一根条 + 一个数字框 + 一颗自动/手动下拉框**。
    // 自动分配重新有入口了。那颗下拉框只读不写数值，真正的重算 / 回滚
    // 在 JavaSettingsController 里（`setMemoryModeFromUi()`）。
    // 条和数字框说的是同一件事 ⇒ 走同一个槽，谁改的在那儿看当前值就知道（见 onMemoryChanged：
    // "另一个还停在旧值上"的那一侧会被挪过去）。条多走一道 onMemorySliderMoved 只是为了把
    // **拖动中**（还没松手）那一串 valueChanged 和"松手了、该落盘了"分开。
    connect(m_owner->m_ui->getMemoryModeCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onMemoryModeChanged);
    connect(m_owner->m_ui->getMemoryMaxSpin(), &QSpinBox::valueChanged, m_owner,
            &MainWindow::onMemoryChanged);
    connect(m_owner->m_ui->getMemoryMaxSlider(), &QSlider::valueChanged, m_owner,
            &MainWindow::onMemorySliderMoved);
    // 松手这一发**必须**单独接：拖动中 valueChanged 已经响过了，松手时位置没再变就不会
    // 再来一发 —— 只接 valueChanged 的话，拖到哪儿、界面上数字也变了，盘上却一个字节
    // 都没写（下一次启动用的还是老值）。这一发就是"拖完了、可以落盘了"。
    connect(m_owner->m_ui->getMemoryMaxSlider(), &QSlider::sliderReleased, m_owner,
            &MainWindow::onMemoryChanged);
    connect(m_owner->m_ui->getLauncherBehaviourCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onLauncherBehaviourChanged);
    // 「语言」：落盘 + 提示重启（见 MainWindow::onLanguageChanged）。
    connect(m_owner->m_ui->getLanguageCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onLanguageChanged);
    connect(m_owner->m_ui->getDownloadSourceCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onDownloadSourceChanged);
    // 「同时最多下几个」。和上面那颗同一个形状：改完立刻生效。
    connect(m_owner->m_ui->getDownloadConcurrencyCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onDownloadConcurrencyChanged);
    // 「下载速度上限」：同一个形状 —— 改完立刻生效（正在下的那一批也慢下来）。
    connect(m_owner->m_ui->getDownloadSpeedLimitCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onDownloadSpeedLimitChanged);
    // 「代理」：档位改完立刻装到进程上；手动地址那一格按回车 /
    // 点到别处（editingFinished）才提交 —— 每敲一个字就重装一次代理没有意义，而且
    // 半截地址会让请求短暂走错路。
    connect(m_owner->m_ui->getDownloadProxyCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onDownloadProxyModeChanged);
    connect(m_owner->m_ui->getDownloadProxyManualEdit(), &QLineEdit::editingFinished, m_owner,
            &MainWindow::onDownloadProxyManualChanged);
    // 「下载新模组时直接放进当前实例」：和上面那一项同一条路 —— 设置里改完
    // 立刻生效（下载那一刻现读），不用重启启动器。
    connect(m_owner->m_ui->getDownloadAutoIntoInstanceCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onDownloadAutoIntoInstanceChanged);
    // 「下载缺前置时提醒」：同一条路 —— 改完立刻生效（点下载那一刻现读）。
    connect(m_owner->m_ui->getWarnMissingDependenciesCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onWarnMissingDependenciesChanged);
    // 「启动选项」那三项。三项都是"下一次启动才读"的设置（日志那个例外：见
    // showGameLogLine 的说明），所以槽里只落盘，不去碰任何正在跑的东西。
    connect(m_owner->m_ui->getShowGameLogCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onShowGameLogChanged);
    // 「每日一言」（设置 → 个性化 → 外观）：关掉立刻不显示也不请求，打开立刻取。
    connect(m_owner->m_ui->getDailyQuoteCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onDailyQuoteChanged);
    connect(m_owner->m_ui->getProcessPriorityCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onProcessPriorityChanged);
    // 「窗口模式」（小窗 / 最大化 / 全屏， 加的）。改完不只是落盘，还要**回头
    // 把下面那两个数字框按档位灰/亮** —— 那一格在「最大化」时是不生效的（见那一档的说明），
    // 亮着会让人以为改了有用。所以槽里既落盘也调 m_owner->updateGameWindowSizeEnabled()。
    connect(m_owner->m_ui->getGameWindowModeCombo(), &QComboBox::currentIndexChanged, m_owner,
            &MainWindow::onGameWindowModeChanged);
    // 两个数字框共用一个槽（和内存那两个 spin 同一套写法：谁改的在那里区分不了，
    // 也不该区分 —— 两个值一起存）。
    connect(m_owner->m_ui->getGameWindowWidthSpin(), &QSpinBox::valueChanged, m_owner,
            &MainWindow::onGameWindowSizeChanged);
    connect(m_owner->m_ui->getGameWindowHeightSpin(), &QSpinBox::valueChanged, m_owner,
            &MainWindow::onGameWindowSizeChanged);
    // 「启动提醒」那两条。和上面三项一样是"下一次启动才读"，槽里只落盘。
    connect(m_owner->m_ui->getWarnDuplicateLaunchCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onWarnDuplicateLaunchChanged);
    connect(m_owner->m_ui->getWarnWorldLossCheck(), &QCheckBox::toggled, m_owner,
            &MainWindow::onWarnWorldLossChanged);
    // JVM 参数刻意**不**接 textChanged：边打字边存会把打到一半的半条参数写进配置，
    // 而它是直接塞进命令行的东西，半条参数会让游戏起不来。改为失焦（或 Ctrl+Enter）即存。
    connect(m_owner->m_ui->getExtraJvmArgsCommit(), &SettingsParts::CommitOnFocusOut::committed, m_owner,
            &MainWindow::onApplyExtraJvmArgsClicked);
    // 「背景图」那一节（主页左栏 / 页面背景）。四颗控件**每改一下都立刻生效、也立刻落盘**
    // —— 改的是"界面长什么样"，改完看不见效果等于没改。
    // NOTE: 下标只用来取这一行，**认它是哪一块靠 `partKey`**（见 MainWindowUI::ThemeImageRow）。
    // 越界不崩：`getThemeImageRow()` 给一个空行，那边每一处都判了空。
    for (int i = 0; i < MainWindowUI::themeImageRowCount(); ++i) {
        const MainWindowUI::ThemeImageRow &imageRow = m_owner->m_ui->getThemeImageRow(i);
        if (imageRow.choose == nullptr) {
            continue;
        }
        connect(imageRow.choose, &QPushButton::clicked, m_owner,
                [this, i] { m_owner->onThemeImageChooseClicked(i); });
        connect(imageRow.clear, &QPushButton::clicked, m_owner,
                [this, i] { m_owner->onThemeImageClearClicked(i); });
        connect(imageRow.fit, &QComboBox::currentIndexChanged, m_owner,
                [this, i] { m_owner->onThemeImageFitChanged(i); });
        connect(imageRow.opacity, &QSlider::valueChanged, m_owner,
                [this, i] { m_owner->onThemeImageOpacityChanged(i); });
    }
    // 「主题」那一节（配色 / 主题色 / 文字不透明度 / 语义色 / 整份导入导出）。
    // 和上面那一节同一个手感：**改一下就生效、也立刻落盘**。
    // NOTE: 越界不崩：`getThemeSemanticButton()` 给 nullptr，那边判空了；而"一共几个语义色"
    // 和那四个槽里的 switch 是同一张表（`MainWindowUI::themeSemanticCount()`）。
    if (m_owner->m_ui->getThemePresetCombo() != nullptr) {
        connect(m_owner->m_ui->getThemePresetCombo(), &QComboBox::currentIndexChanged, m_owner,
                &MainWindow::onThemePresetChanged);
    }
    if (m_owner->m_ui->getThemeAccentButton() != nullptr) {
        connect(m_owner->m_ui->getThemeAccentButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeAccentClicked);
    }
    if (m_owner->m_ui->getThemeAccentResetButton() != nullptr) {
        connect(m_owner->m_ui->getThemeAccentResetButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeAccentResetClicked);
    }
    for (int i = 0; i < MainWindowUI::themeSemanticCount(); ++i) {
        if (QPushButton *swatch = m_owner->m_ui->getThemeSemanticButton(i)) {
            connect(swatch, &QPushButton::clicked, m_owner,
                    [this, i] { m_owner->onThemeSemanticClicked(i); });
        }
    }
    if (m_owner->m_ui->getThemeSemanticResetButton() != nullptr) {
        connect(m_owner->m_ui->getThemeSemanticResetButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeSemanticResetClicked);
    }
    if (m_owner->m_ui->getThemeResetAllButton() != nullptr) {
        connect(m_owner->m_ui->getThemeResetAllButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeResetAllClicked);
    }
    if (m_owner->m_ui->getThemeExportButton() != nullptr) {
        connect(m_owner->m_ui->getThemeExportButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeExportClicked);
    }
    if (m_owner->m_ui->getThemeImportButton() != nullptr) {
        connect(m_owner->m_ui->getThemeImportButton(), &QPushButton::clicked, m_owner,
                &MainWindow::onThemeImportClicked);
    }
    if (m_owner->m_ui->getThemeTextOpacitySlider() != nullptr) {
        connect(m_owner->m_ui->getThemeTextOpacitySlider(), &QSlider::valueChanged, m_owner,
                &MainWindow::onThemeTextOpacityChanged);
    }
    // CurseForge 的 API key：**编辑完才存**（editingFinished = 回车或者焦点离开）。
    // 理由写在 m_owner->onCurseForgeApiKeyEdited() 里。
    connect(m_owner->m_ui->getCurseForgeApiKeyEdit(), &QLineEdit::editingFinished, m_owner,
            &MainWindow::onCurseForgeApiKeyEdited);

    // 选项卡切换：底栏左下角的显示规则依赖"当前是不是在主页"，切页就得重算；
    // 第一次切到设置页还要顺带扫一遍 Java。
    connect(m_owner->m_ui->getTabWidget(), &QTabWidget::currentChanged, m_owner,
            &MainWindow::onMainTabChanged);

    // 登录三态都要在界面上有反应 —— 少接一条，「登录成功」这件事就在启动器上
    // 一点痕迹都没有：用户点完登录、在浏览器里看到「登录成功」，回到启动器还是原样，
    // 像什么都没发生。
    connect(m_owner->m_authManager, &AuthManager::loginStarted, m_owner, &MainWindow::onLoginStarted);
    connect(m_owner->m_authManager, &AuthManager::loginSucceeded, m_owner, &MainWindow::onLoginSucceeded);
    connect(m_owner->m_authManager, &AuthManager::loginFailed, m_owner, &MainWindow::onLoginFailed);
    // 刷新令牌失效 / 重新登录成功会改「需要重新登录」标记；账号页开着时列表要跟着变。
    connect(m_owner->m_authManager, &AuthManager::needsReloginChanged, m_owner,
            &MainWindow::refreshAccountRows);

    // 玩家外观那三条（Store 的三个信号）接在 PlayerSkinController 自己身上
    // （见那个 const 函数里的接线），因为"档案到了怎么办"已经是它的判断（认人、落盘、
    // 要不要重画）。这一层只收它的两个出口：
    // ① skinChanged —— 现在该画这一套了（右栏小人 + 底栏头像 + 那个窗的 3D 预览）；
    // ② hintChanged —— 事实变了，那句话重挑一遍（`m_owner->updateSkinHint()`）。
    // 外加一条日志通道：那几行"取不到/记不下"走在 m_owner->appendLog() 这条公共路上
    // （验证工程是数那条路上的行数）。
    connect(m_owner->m_playerSkin, &PlayerSkinController::skinChanged, m_owner,
            &MainWindow::onPlayerSkinChanged);
    connect(m_owner->m_playerSkin, &PlayerSkinController::hintChanged, m_owner, &MainWindow::updateSkinHint);
    connect(m_owner->m_playerSkin, &PlayerSkinController::logMessage, m_owner,
            [this](const QString &text) { m_owner->appendLog(text); });

    // 游戏进程信号。三条都带 sessionId（哪一份会话说的）—— 同时开着好几份时，日志里
    // 那句"哪来的"和底栏那格"还有谁在跑"全靠它归属。
    connect(m_owner->m_launcherManager, &LauncherManager::processOutput, m_owner, &MainWindow::onProcessOutput);
    connect(m_owner->m_launcherManager, &LauncherManager::processError, m_owner, &MainWindow::onProcessError);
    connect(m_owner->m_launcherManager, &LauncherManager::gameFinished, m_owner, &MainWindow::onGameFinished);
    // 一份会话在**真的起来之前**就被收回（启动那三条早退）。上面那条 processError 会替它
    // 建出一扇日志窗、记一笔底账，而这一号此后不会再有任何信号 —— 收尾归这一条。
    connect(m_owner->m_launcherManager, &LauncherManager::sessionDiscarded, m_owner,
            &MainWindow::onSessionDiscarded);

    // 版本管理器信号
    // NOTE: 只能接真正在下载的那条链路：MainWindow 自己 new 出来的 DownloadManager
    // 上面从来没有任何地方发起过下载 —— 真正的下载全走 VersionManager
    // 内部的 DownloadManager。接错了 "下载完成" 那个弹窗就永远不触发，
    // 用户看着刷屏的日志以为还没下完就去点启动。
    connect(m_owner->m_versionManager, &VersionManager::versionsFetched, m_owner,
            &MainWindow::onVersionsFetched);
    connect(m_owner->m_versionManager, &VersionManager::versionDetailsFetched, m_owner,
            &MainWindow::onVersionDetailsFetched);
    connect(m_owner->m_versionManager, &VersionManager::versionDownloadStats, m_owner,
            &MainWindow::onVersionDownloadStats);
    // The shared game downloader also serves repair, resource updates and servers.
    // Account physical transfers once, never sum file-name-based progress or cache hits.
    struct LiveRate {
        QElapsedTimer clock;
        DownloadRate estimate;
        Tasks::TaskId task = Tasks::invalidTaskId();
        qint64 received = 0;
    };
    const auto rate = std::make_shared<LiveRate>();
    rate->clock.start();
    const auto ownerTask = [this] {
        return m_owner->m_downloadTaskLedger->owner() != Tasks::invalidTaskId()
            ? m_owner->m_downloadTaskLedger->owner() : m_owner->m_serverInstallTaskId;
    };
    const auto switchTask = [rate](Tasks::TaskId id) {
        if (rate->task == id) return;
        rate->task = id;
        rate->received = 0;
        rate->estimate.reset(rate->clock.elapsed());
    };
    connect(m_owner->m_versionManager->downloads(), &DownloadManager::transferBytes, this,
            [rate, ownerTask, switchTask](qint64 bytes) {
                switchTask(ownerTask());
                rate->received += qMax<qint64>(0, bytes);
            });
    auto *rateTimer = new QTimer(this);
    rateTimer->setInterval(1000);
    connect(rateTimer, &QTimer::timeout, this, [this, rate, ownerTask, switchTask] {
        const auto id = ownerTask();
        switchTask(id);
        if (!m_owner->m_versionManager->downloads()->isBusy()) {
            m_owner->m_activityCenter->setTransferSpeed(id, -1);
            rate->received = 0;
            rate->estimate.reset(rate->clock.elapsed());
            return;
        }
        const qint64 now = rate->clock.elapsed();
        rate->estimate.sample(now, rate->received);
        m_owner->m_activityCenter->setTransferSpeed(id, rate->estimate.bytesPerSecond(now));
    });
    rateTimer->start();
    connect(m_owner->m_versionManager, &VersionManager::versionDownloadFinished,
            m_owner->m_downloadCompletion, &DownloadCompletionController::onFinished);
    // 下载页那几类资源（模组/数据包/资源包/光影）里挑中的那个版本下完了。**和上面那条分开**
    // （理由见 VersionManager 里那个信号的 NOTE）：这条路上的 label 不是"哪个版本"，
    // 接进来的是另一个槽，那个槽不碰任何实例的安装状态。
    connect(m_owner->m_versionManager, &VersionManager::resourceDownloadFinished, m_owner,
            &MainWindow::onResourceDownloadFinished);
    // **每一个文件**下完之后那一条结果（成功 / 失败 + 原因）。
    // 任务的账本上一步一个资源（「更新任务显示每个资源的结果，不只显示总数」）
    // 就是靠它填的。不是更新那一批的时候这一路什么都不做（见那个信号的 NOTE）。
    connect(m_owner->m_versionManager, &VersionManager::resourceDownloadFileFinished, m_owner,
            &MainWindow::onUpdateBatchFileFinished);
    // 加载器安装中间那几步（拉列表、挑版本、先补原版）只写日志、不弹窗：那几件事都在
    // "一次点击"里面，每步弹一个框等于点着玩。真正要用户做决定的（还没做、失败了）
    // 才走 versionDownloadFinished 那条弹窗的路。
    connect(m_owner->m_versionManager, &VersionManager::loaderInstallMessage, m_owner,
            [this](const QString &text) { m_owner->appendLog(text); });
    // 整合包导入的中间几句（装加载器、下清单那批文件、解覆盖文件）同上：只写日志、不弹窗。
    // 真正要用户做决定的（装完了、失败了）走 versionDownloadFinished 那条弹窗的路。
    connect(m_owner->m_versionManager, &VersionManager::modpackImportMessage, m_owner,
            [this](const QString &text) { m_owner->appendLog(text); });
    connect(m_owner->m_versionManager, &VersionManager::modpackImportStageChanged, m_owner,
            [this](VersionManager::ModpackStage stage, const QString &note) {
                const Tasks::TaskId id = m_owner->m_downloadTaskLedger->task();
                if (id == Tasks::invalidTaskId()) {
                    return;
                }
                Tasks::TaskManager *tasks = m_owner->m_activityCenter->tasks();
                const Tasks::TaskSnapshot snap = tasks->snapshot(id);
                if (snap.steps.isEmpty()) {
                    return;
                }
                // 一键安装带「下载整合包文件 / 解析清单」前两步（共 6 步），本地导入从第 0 步开始（共 4 步）。
                const int offset = snap.steps.size() >= 6 ? 2 : 0;
                int active = offset;
                switch (stage) {
                case VersionManager::ModpackStage::LoaderAndVanilla:
                    active = offset;
                    break;
                case VersionManager::ModpackStage::ModpackFiles:
                    active = offset + 1;
                    break;
                case VersionManager::ModpackStage::ExtractingOverrides:
                    active = offset + 2;
                    break;
                case VersionManager::ModpackStage::Verifying:
                    active = offset + 3;
                    break;
                }
                for (int i = 0; i < snap.steps.size(); ++i) {
                    if (i < active) {
                        tasks->setStepState(id, i, Tasks::TaskStepState::Done);
                    } else if (i == active) {
                        tasks->setStepState(id, i, Tasks::TaskStepState::Running, note);
                    }
                }
                if (!note.isEmpty()) {
                    tasks->setDetail(id, note);
                }
            });
    // 装"一个带名字的实例"中间那几句（正在装原版 / 正在装加载器）同上：只写日志、不弹窗。
    connect(m_owner->m_versionManager, &VersionManager::instanceInstallMessage, m_owner,
            [this](const QString &text) { m_owner->appendLog(text); });
    // 某个加载器在那个游戏版本上有哪些版本可选（安装页上点开一条加载器 → 拉列表 → 填
    // 它右边那个组合框）。**每次请求都会回一次**，失败也回 —— 失败时那句原因要写进
    // 组合框的占位文案里（不能让用户对着一个永远空的框等下去）。
    // 这里"枚举 → 下标"的翻译也在这条链路上做一次：槽收的是下标（见头里的说明），
    // 而信号带过来的是枚举，中间只有 VersionManager::loaderIndexForKind 这一处。
    connect(m_owner->m_versionManager, &VersionManager::loaderVersionsFetched, m_owner,
            [this](const QString &gameVersion, VersionManager::LoaderKind kind, bool success,
                   const LoaderMeta::LoaderVersionList &versions, const QString &errorMessage) {
                const int loaderIndex = VersionManager::loaderIndexForKind(kind);
                if (loaderIndex < 0) {
                    return;  // 枚举里新来的那条界面还没跟上（表没改全），当没收到
                }
                m_owner->onLoaderVersionsFetched(gameVersion, loaderIndex, success, versions, errorMessage);
            });

    // ---------- 服务端安装那四段 ----------
    //
    // 它拿的是 `VersionManager` 手上**那一份**下载器：下载器只有一份批次状态（镜像降级 /
    // 按主机守卫 / 看门狗都在它身上），自己另起一份 = 同一时间有两条路在改同一批文件。
    // 元数据那几次 GET 由控制器用**它自己**的 network manager 发（理由见那个头文件里的 NOTE）。
    m_owner->m_serverInstall = new ServerInstallRun::ServerInstallController(m_owner);
    m_owner->m_serverInstall->setDownloadManager(m_owner->m_versionManager->downloads());
    // 原版那一档的地址在版本 json 的 `downloads.server` 里 —— 盘上已经有这个版本的客户端时
    // 一个网络请求都不用发（"先装了客户端、再开一个同版本的服务端"是最常见的那条路）。
    m_owner->m_serverInstall->setLocalVersionJson([this](const QString &versionId) {
        const QJsonObject local = m_owner->m_versionManager->getVersionJson(versionId);
        return !local.isEmpty() ? local : m_owner->m_probedServerVersionJsons.value(versionId);
    });
    connect(m_owner->m_serverInstall, &ServerInstallRun::ServerInstallController::message, m_owner,
            [this](const QString &text) { m_owner->appendLog(text); });
    // 这几条连线**只接一次**（在构造里），靠 `m_owner->m_serverInstallTaskId` 认"现在这一趟是哪一项"
    // —— 接在 `m_owner->installServer()` 里的话，第二次安装会接上第二遍（同一件事被记两回账）。
    connect(m_owner->m_serverInstall, &ServerInstallRun::ServerInstallController::stepsChanged, m_owner,
            [this](const QStringList &names, int runningIndex) {
                if (m_owner->m_serverInstallTaskId == Tasks::invalidTaskId()) {
                    return;
                }
                m_owner->m_serverInstallStepCount = names.size();
        m_owner->m_activityCenter->tasks()->setSteps(m_owner->m_serverInstallTaskId,
                                        taskStepsFromNames(names, runningIndex));
                m_owner->m_activityCenter->tasks()->setProgress(m_owner->m_serverInstallTaskId, qMax(0, runningIndex),
                                           qMax(1, names.size()));
            });
    connect(m_owner->m_serverInstall, &ServerInstallRun::ServerInstallController::stepChanged, m_owner,
            [this](int index, bool ok, const QString &note) {
                if (m_owner->m_serverInstallTaskId == Tasks::invalidTaskId()) {
                    return;
                }
                m_owner->m_activityCenter->tasks()->setStepState(m_owner->m_serverInstallTaskId, index,
                                            ok ? Tasks::TaskStepState::Done
                                               : Tasks::TaskStepState::Failed,
                                            note);
                if (ok) {
                    m_owner->m_activityCenter->tasks()->setProgress(m_owner->m_serverInstallTaskId, index + 1,
                                               qMax(1, m_owner->m_serverInstallStepCount));
                }
            });
    connect(m_owner->m_serverInstall, &ServerInstallRun::ServerInstallController::bytesChanged, m_owner,
            [this](qint64 done, qint64 total) {
                if (m_owner->m_serverInstallTaskId == Tasks::invalidTaskId()) {
                    return;
                }
                m_owner->m_activityCenter->tasks()->setBytes(m_owner->m_serverInstallTaskId, done, total);
            });
    connect(m_owner->m_serverInstall, &ServerInstallRun::ServerInstallController::finished, m_owner,
            [this](ServerInstallRun::Outcome outcome, const QString &errorSummary) {
                const Tasks::TaskId id = m_owner->m_serverInstallTaskId;
                m_owner->m_serverInstallTaskId = Tasks::invalidTaskId();
                // 服务端那一页开着的时候重扫一遍：装好的那一行从「未安装」变成「可启动」
                // （判据是 `ServerRegistry::describe()`，和任务中心那句"装好了"同一个出处）。
                if (m_owner->m_instanceMode == MainWindowUI::InstanceMode::Server) {
                    m_owner->refreshServerList();
                }
                if (id == Tasks::invalidTaskId()) {
                    return;
                }
                switch (outcome) {
                case ServerInstallRun::Outcome::Succeeded:
                    m_owner->finishTask(id, true);
                    break;
                case ServerInstallRun::Outcome::Cancelled:
                    // 取消是一种**正常**的收场（"取消"是单独一档），不是失败 ——
                    // 所以不能借 `m_owner->finishTask(id, false, …)` 那条路（那会把它记成失败、
                    // 还点亮底栏那颗"没看过的失败"红点）。账本自己那颗入口就在这儿。
                    m_owner->m_activityCenter->tasks()->cancel(id);
                    break;
                case ServerInstallRun::Outcome::Failed:
                    m_owner->finishTask(id, false, errorSummary);
                    // 「错误既进入日志，也在用户当前流程中可见」。日志那一半在
                    // `message` 那条路上（控制器 `fail()` 里已经发过），这里补弹窗那一半 ——
                    // 用户常常是点了「创建」就去干别的了，不弹一下他看不见。
                    InlineMessage::critical(m_owner->m_ui->overlay(), m_owner->tr("无法安装这个服务端"), errorSummary);
                    break;
                }
            });
}


