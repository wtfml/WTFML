#include "controllers/ServerVersionPicker.hpp"
#include "controllers/AgentController.hpp"
#include "controllers/AgentLauncherHostImpl.hpp"
#include "AgentPage.hpp"
#include "agent/AgentInstanceLock.hpp"
#include "util/AppPaths.hpp"
/**
 * @file MainWindow.cpp
 * @brief 实现主应用程序窗口及用户交互逻辑。
 * @note 本文件实现了 UI 事件处理函数并委托给各个管理器的方法。
 *
 * 本文件将 UI 控件连接到启动器，处理用户命令，
 * 并协调身份验证、下载和启动操作。
 */

#include "MainWindow.hpp"
#include "TextMarks.hpp"
#include "presets/PresetTargetSnapshot.hpp"
#include "controllers/ResourceSearchController.hpp"
#include "controllers/InstanceUpdateCheckController.hpp"
#include "controllers/UpdateCenterController.hpp"
#include "controllers/ServerSettingsPageController.hpp"
#include "controllers/InstallPageController.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerMenuController.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/ResourceInstallController.hpp"
#include "manage/UpdatePolicy.hpp"
#include "launcher/LaunchScriptExport.hpp"
#include "util/SecretRedactor.hpp"
#include "util/LaunchScriptCredentials.hpp"
#include "manage/BackupFiles.hpp"
#include "operations/InstanceUpgrade.hpp"

#include "..\ui\Animations.hpp"  // 换游戏文件夹时那一栏淡一下
#include "..\ui\ConfigFileEditorPage.hpp"
#include "..\ui\ConfigFileText.hpp"
#include "..\ui\GroupDialog.hpp"
#include "..\ui\InstanceGroupDelegate.hpp"
#include "..\ui\InstanceFilesPage.hpp"
#include "..\ui\InstanceSettingsDialog.hpp"
#include "..\ui\IntegrityReportPage.hpp"  // 完整性检查报告页
#include "..\ui\JavaMismatchDialog.hpp"
#include "..\ui\JavaManageDialog.hpp"
#include "..\ui\AccountSkinDialog.hpp"
#include "..\ui\ResourceDetailPage.hpp"
// 轻量回滚点：`updateInstanceFiles()` 动手之前
// 摆一份、`onUpdateCenterUndoRequested()` 照着它退回去。
#include "manage/UpdateRollback.hpp"
#include "..\ui\GameLogWindow.hpp"
#include "..\ui\MainWindowUI.hpp"
// 任务中心面板：底栏中间那格点开之后向上弹出来的那块。
// 这一边给它两样东西 —— 一行长什么样（`TaskCenter::Item`，那层是**纯显示**的 DTO）和
// 「清除已完成」被点了怎么办（见 setupConnections() 里 setTaskCenterContent() 那一段）。
#include "..\ui\TaskCenterPanel.hpp"
#include "..\ui\TaskDetailPage.hpp"
// 那一页本身（存档那支笔点开的「世界设置」）。**它摆什么、怎么摆只有它自己知道**，
// 这一边只负责把盘上读出来的东西翻译成 `WorldSettingsPage::WorldView`（见
// worldViewOf() 那段），以及把页面上改过的翻译回去。
#include "..\ui\WorldSettingsPage.hpp"
// 内容区上面那一层：设置界面和各种提示都开在它里面，不再另开独立窗口。
// 见它的文件头 —— ui/ 那边只管"开在哪儿、长什么样"，不认识业务。
#include "..\ui\OverlayHost.hpp"
#include <QDialog>
#include <QLayout>
// 安装页「预设」那一行点开的紧凑弹窗：四类各挑一份、看一眼计划。
// 它只画外面喂进去的值（`ui/` 不读盘），所以这一边负责把 controller 算好的东西递进去。
#include "..\ui\PresetInstallPicker.hpp"
// QMessageBox 的替身：`InlineMessage::warning(overlay, 标题, 正文)` 顶掉
// `QMessageBox::warning(this, 标题, 正文)`。调用点一行一行对得上，
// 语义一个字没变 —— 变的只是"这一句说在哪"。
#include "..\ui\InlineMessage.hpp"
// 主页右栏那个小人怎么画：几何、投影、默认外观、头像裁切全在它里面（纯函数 + 值）。
#include "..\ui\SkinModel.hpp"
#include "..\ui\Theme.hpp"
// 加载器那棵树底下每一条版本号前面挂的**它自己那个加载器的标志**（见
// fillLoaderVersionItems()）：那几条是"Fabric 能装哪些版本"，前面顶个草方块会让人
// 以为是原版号。树的顶层那五条由 MainWindowUI 那边给图标，这里只管子条目。
#include "..\ui\VersionIcons.hpp"
#include "..\ui\VersionTreeDelegate.hpp"
#include "..\ui\WindowChrome.hpp"
// 树上"这一行被激活了"的唯一接线口（鼠标单击 + 键盘回车，一次手势只进来一次）。
// 三棵树（版本树 / 加载器树 / 下载页那几张资源表）都走它，见那个文件头。
#include "..\ui\RowActivation.hpp"

// 「背景图」那一节挑回来的那张图：**拷进数据目录**再记路径（见那个文件头那段）。
// `ui/` 那边不许 include `src/`，所以收图这一步只能在 MainWindow 这一层做
// （`MainWindowUI::ThemeImageRow` 只管摆控件）。
#include "util/ImageDecode.hpp"
#include "util/ThemeImage.hpp"

// 添加以下头文件（防止hpp变动导致文件失效）
#include <QApplication>
#include <QCheckBox>
#include <QShortcut>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
// 「配置文件」那一页记盘上的"那一份"要一秒不差地带上修改时间（见 ConfigFileText::Stamp），
// 备份文件夹名也是拿它拼的。
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
// 玩家外观那一套（皮肤/披风贴图）和存档图标都按 QImage 处理。
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
// 拖进来的文件是从 QMimeData 上那串 url 读出来的（见 MainWindow::dropEvent）。
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
// 底栏面板里那几行的图标要按"底栏那几行字的墨色"上色（图标是画的，颜色由调用方给）。
#include <QPalette>
// 下载页那几张资源表前面那个缩略图（onResourceIconReady 里还要造一个全透明的占位）。
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
// 那几处"先写临时文件、再原子替换"的落盘（启动脚本 / 导出 / 备份那几处）。
#include <QSaveFile>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
// 整合包那一页下完 .mrpack 之后要"排到下一轮"再走导入（不能在那个信号栈里直接开新批次，
// 见 onResourceDownloadFinished 里那段 NOTE）。
#include <QThread>
// 「文件管理」那几页的图标反查跑在它上面（见 requestInstanceFileIcons 里那段 NOTE）。
#include <QTimer>
#include <QUrl>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QtConcurrent/QtConcurrentRun>
// 日志窗那两颗导出键的保存框默认落在桌面（见 exportDefaultDirectory()）。
#include <QStandardPaths>

// 底栏右面板那三行要按 saved_at 排序（std::stable_sort：同一秒里存的两个账号要保持
// 列表里原来的先后，不然每次点开顺序都可能不一样）。
#include <algorithm>
// 「配置文件」那一页挑最近那一份备份（`std::greater<QString>`：备份文件夹名倒序 == 时间
// 倒序，见 ConfigFileText::backupFolderName() 那段 NOTE）。
#include <functional>
#include <memory>

#include "auth/AccountStore.hpp"
#include "auth/AuthManager.hpp"
#include "auth/SkinApi.hpp"
#include "controllers/JavaRuntimeController.hpp"
#include "controllers/JavaSettingsController.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/LauncherPreferencesController.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/IntegrityController.hpp"
#include "controllers/ToolboxController.hpp"
#include "controllers/BackupController.hpp"
#include "controllers/ServerPingController.hpp"
#include "controllers/FolderController.hpp"
#include "controllers/ModpackExportController.hpp"
#include "controllers/InstanceFileController.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/FileOperationRecoveryController.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "controllers/InstanceDialogController.hpp"
#include "controllers/InstanceEditorController.hpp"
#include "controllers/ImportController.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/ConnectionsController.hpp"
#include "controllers/InstanceIconController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/TaskDetailController.hpp"
#include "controllers/DailyQuoteController.hpp"
#include "controllers/HomeQuickController.hpp"
#include "controllers/InstanceMaintenanceController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "controllers/ServerConsoleController.hpp"
#include "controllers/ServerSettingsController.hpp"
#include "controllers/ServerManagementActions.hpp"
#include "controllers/ThemeSettingsController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "controllers/PlayerSkinController.hpp"
// 「主窗口各控件的视图模型」那一层（实例树 / 分组行 / 底栏面板行 / 版本下拉框）。
#include "controllers/MainWindowViewData.hpp"
// 下载页那几类资源的取数（搜索 / 版本列表 / 图标）。头文件那边只前置声明了这个类
// （那里用不到它的定义），这里要用真的。
#include "download/ResourceStore.hpp"
#include "launcher/AsciiPathAlias.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/DefaultSkin.hpp"
// 拖进来的那一份文件是哪一类（纯判断，见那个头文件）。
#include "manage/DroppedResource.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/ModpackExport.hpp"
#include "manage/ModpackImport.hpp"
// 离线账号自己挑的那张皮肤（「账号与外观」→「换一张…」）。
#include "manage/OfflineSkin.hpp"
#include "manage/PlayerSkinStore.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/VersionManager.hpp"
#include "manage/WorldFiles.hpp"
// 游戏异常退出那几句怎么得出来：纯函数，规则全在它自己那个文件里。
#include "diag/GameExitDiagnosis.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
// 任务账本 —— 底栏中间那格和任务中心面板都读它。
// 账本由 ActivityCenterController 持有；MainWindow 通过 controller 更新执行进度。
#include "tasks/TaskManager.hpp"
#include "util/AppPaths.hpp"
#include "util/ErrorReport.hpp"
// 「服务器列表」那一页要读 servers.dat（NBT，旧存档还是 gzip 过的）。
#include "util/NbtUtil.hpp"
// 同一页的「编辑」「添加」「删除」：改名字和地址、加一台、拿掉一台都要
// **写** servers.dat —— NbtUtil 那一层故意只读，写侧单独一层（格式、按值匹配、gzip
// 保留的理由都在它的文件头）。`ServerListFile::pathFor()` 也是这一份路径的唯一出处。
#include "manage/ServerListFile.hpp"
// 服务端工作区那一层：判据层（状态 / EULA / properties）和
// 扫盘那一层（`<数据目录>/servers` 底下谁在住）。界面那一页只摆控件，判据全在这儿。
#include "download/ServerInstall.hpp"
#include "server/ServerModel.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRestart.hpp"  // 崩溃自动重启那几条判据（纯头）
#include "server/ServerResourcePack.hpp"  // 本地资源包那一份 zip 的判据 + SHA-1
#include "server/ServerContent.hpp"  // 服务端内容（模组 / 数据包）的名字与分类判据
// 服务端安装那四段的**判据层**（"还差哪一份元数据、取回来怎么读、分成几步"）：
// 纯函数，`verify` 的 ZY 一节直接钉它。这里只用到那几个翻译。
#include "server/ServerInstallRun.hpp"
#include "server/ServerRun.hpp"  // 服务端"跑"的那几条判据（纯头，见它的文件头）
// 同一页的「刷新」：真的连上去问一遍每台现在什么样（图标 / MOTD / 人数 / 延迟）。
#include "util/ServerPing.hpp"
#include "util/SystemMemory.hpp"
// 「导入整合包…」要读压缩包根目录里的 modrinth.index.json（ZipUtil::readEntry）。
#include "util/ZipUtil.hpp"

using namespace MainWindowViewData;

namespace {
/// 主页「每日一言」的文字种类：判据层那套（`util/DailyQuote.hpp`）映射到界面层那套
/// （`Theme::QuoteScript`）。两张表不共用类型是分层铁律：ui/ 不许 include src/。
Theme::QuoteScript quoteScriptForTheme(DailyQuote::Script script) {
    switch (script) {
    case DailyQuote::Script::Han: return Theme::QuoteScript::Han;
    case DailyQuote::Script::Kana: return Theme::QuoteScript::Kana;
    case DailyQuote::Script::Latin: return Theme::QuoteScript::Latin;
    }
    return Theme::QuoteScript::Han;
}

/**
 * @brief 「这件事对账本上那件任务的收尾责任」——从建出来到 `disarm()` 之间任何一个早退
 * （`return`）都由析构里那一发兜住。
 *
 * 【为什么非要它】
 *
 * 一件漏收尾的任务，症状是"底栏中间那格从此一直显示它、任务中心里它永远在'当前工作'那一栏"
 * （账本里再没有人会去碰它了）。而那种洞**只在走到那一条早退时才露出来**：`onStartButtonClicked()`
 * 从"点了启动"到"进程起来"之间有七八个 `return`（版本信息缺失 / 继承链不全 / natives 起不来 /
 * ASCII 别名建不出 / 启动前检查没过 / 用户在那个 Java 不匹配的框里点了取消），靠人记着每个
 * 前面都补一句 `finishTask(...)`，迟早漏一个 —— 漏掉的那一条平时一次都测不到。
 * 把责任挂在栈上就没有"漏一处"这回事：作用域一退出，没 `disarm()` 过的就一律按失败收尾。
 *
 * 【什么时候要 `disarm()`】
 *
 * 不是"没出错"就完事，而是"这件事**已经按别的说法收过尾了**"——三种：
 * · 成功（进程真的起来了 ⇒ 由 `onProcessOutput()` 那边收）；
 * · 用户自己取消（该记 `cancel()` 而不是失败，免得账本上多一次"失败的启动"）；
 * · 转交给别人（比如后面那条异步的路自己会收）。
 */
class TaskAbortGuard {
public:
    explicit TaskAbortGuard(std::function<void()> onAbort) : m_onAbort(std::move(onAbort)) {}
    ~TaskAbortGuard() {
        if (m_armed && m_onAbort) {
            m_onAbort();
        }
    }
    /// 这一趟的收尾**已经由别处负责了**（见类上那段），撤销兜底。
    void disarm() { m_armed = false; }

private:
    std::function<void()> m_onAbort;
    bool m_armed = true;
};

}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_ui(new MainWindowUI(this)),
      m_settings(new LauncherSettings(this)),
      m_launcherManager(new LauncherManager(this)),
      m_authManager(new AuthManager(this)),
      m_folderManager(new FolderManager(m_settings, this)),
      m_versionManager(new VersionManager(this)),
      m_resourceStore(new ResourceStore(this)),
      m_serverWorkspace(new ServerWorkspaceController(this)),
      m_javaSettings(new GlobalJavaSettings(this)),
      m_javaRuntimeController(new JavaRuntimeController(this)),
      m_groupStore(new InstanceGroupStore(this)),
      m_skinStore(new PlayerSkinStore(this)),
      m_skinApi(new SkinApi(this)),
      m_playerSkin(new PlayerSkinController(m_authManager, m_skinStore, this)) {
    // 安装页那一层（要装哪个原版、选中哪条加载器、名称框里那份"用户改过没有"）。
    // NOTE: 它的构造里**特意不碰那棵树的界面** —— 第一次重画是进安装页的 `openPage()`；
    // 构造只把「选中」这一位初始化成 `MainWindowUI::modLoaderNoneIndex()`。
    // 智能体（Stage 1A）：页面已经由 MainWindowUI 建好，这里只接控制器。
    // 没有页面时控制器不建 —— 但产品里 MainWindowUI 总是会建第五页。
    if (m_ui->getAgentPage() != nullptr) {
        m_agentController = new AgentController(m_ui->getAgentPage(), m_ui->overlay(), this);
        // Stage 2：启动器工具（诊断/设置/模组/备份/升级检查）走这一层；智能体核心只认
        // `AgentLauncherHost` 那张纯虚表，不认识 VersionManager / MainWindow。
        m_agentLauncherHost = std::make_unique<AgentLauncherHostImpl>(this);
        m_agentController->setLauncherHost(m_agentLauncherHost.get());
        m_agentController->setInstanceContextProvider([this] {
            AgentInstanceInfo info;
            QComboBox *combo = m_ui->getInstalledVersionCombo();
            if (combo == nullptr || combo->currentIndex() < 0) return info;
            info.id = combo->currentData().toString();
            if (info.id.isEmpty()) return info;
            info.name = instanceDisplayName(info.id);
            info.gameRootId = AgentInstanceLocks::gameRootId(AppPaths::minecraftDir());
            info.exists = QFileInfo(AppPaths::versionDir(info.id)).isDir();
            return info;
        });
        // 「实例」下拉的选项：主页实例下拉里那一份清单（当前游戏目录下的全部实例）。
        m_agentController->setInstanceListProvider([this] {
            QList<AgentInstanceInfo> list;
            QComboBox *combo = m_ui->getInstalledVersionCombo();
            const QString root = AgentInstanceLocks::gameRootId(AppPaths::minecraftDir());
            for (int i = 0; combo != nullptr && i < combo->count(); ++i) {
                AgentInstanceInfo info;
                info.id = combo->itemData(i).toString();
                if (info.id.isEmpty()) continue;
                info.name = instanceDisplayName(info.id);
                info.gameRootId = root;
                info.exists = QFileInfo(AppPaths::versionDir(info.id)).isDir();
                list.append(info);
            }
            return list;
        });
        connect(m_ui->getTabWidget(), &QTabWidget::currentChanged, m_agentController,
                [this](int index) {
                    if (index != MainWindowUI::agentTabIndex()) {
                        m_agentController->onPageHidden();
                    } else {
                        m_agentController->onPageShown();
                    }
                });
    }
    m_installPage = new InstallPageController(this);
    // 服务端**起 / 停 / 退出之后怎么办**那一套（见 `controllers/ServerRunController.hpp`）。
    // 它不拥有进程 —— 进程的账在 `m_serverWorkspace` 里，这一层只按 id 去问。
    m_serverRun = new ServerRunController(this);
    // 「服务端」那一页的右键菜单：摆哪几项 + 点下去做什么（见
    // `controllers/ServerMenuController.hpp` 那段"为什么这一层值得单独站着"）。
    m_serverMenu = new ServerMenuController(this);
    // 下载收尾那六种来路（见 `controllers/DownloadCompletionController.hpp`）。
    m_downloadCompletion = new DownloadCompletionController(this);
    // 「这一批下载的账」：任务号 / 谁能重试 / 父任务 / 资源名 / 取消标志 / 进度节流。
    m_downloadTaskLedger = new DownloadTaskLedgerController(this);
    setAcceptDrops(true);
    // 「资源详情」那一页的控制器**构造里就建**（只是状态 + 一个 QObject，页面对象仍然懒建）。
    // 不能跟着页面一起懒建：图标那一发、版本列表那一发回来时都可能比"开过页"早到，
    // 而它们按 `m_resourcePage->state().open` 认人 —— 那一位没有"还没建"这一档。
    m_resourcePage = new ResourcePageController(this);
    // 「把这一份资源装进那个实例」：落点 / 谁在动盘 / 存不存别处 / 加载器那道闸。
    m_resourceInstall = new ResourceInstallController(this);
    m_downloadFilter = new DownloadFilterController(this);
    m_instanceUpdateCheck = new InstanceUpdateCheckController(this);
    // 「更新中心」那一页：账 + 页面对象 + 开页时记的实例 id 全在这一层（构造里就建，
    // 页面仍然懒建 —— 理由同上面 `m_resourcePage` 那段）。
    m_updateCenter = new UpdateCenterController(this);
    m_resourceSearch = new ResourceSearchController(this);
    // ---------- 任务账本 ----------
    // **唯一**记录"现在有哪些事在跑、各自跑到哪儿了"的地方。它自己一个网络都不开、一个
    // 进程都不起（执行者照旧是 DownloadManager / VersionManager / LauncherManager）——
    // 这一层负责把它们的信号翻成 begin() / setProgress() / succeed()（不把任务中心
    // 做成第二套业务逻辑）。
    m_activityCenter = new ActivityCenterController(this);
    m_serverSettingsController = new ServerSettingsController(m_serverWorkspace);
    // 「服务端设置」那一页：页对象 + 现在说的是谁 + 能不能改 + 端口那一次的结论全在这一层
    // （构造里就建，页面仍然懒建 —— 理由同上面 `m_resourcePage` 那段）。
    m_serverSettings = new ServerSettingsPageController(this);
    m_themeSettingsController = new ThemeSettingsController(m_ui, m_settings, this, this);
    m_javaSettingsController = new JavaSettingsController(
        m_ui, m_javaSettings, m_javaRuntimeController, this,
        JavaSettingsController::Host{[this](const QString &text) { appendLog(text); }}, this);
    // 「实例预设」那一页：页面、增删改、提取、计划、备份、落盘
    // 全在 PresetController 里；这儿只把"实例叫什么 / 在不在跑 / 日志往哪儿写"三样递过去，
    // 以及实例页第三档那个宿主（页面由 controller 自己塞进去）。
    // 兼容性提供方在构造时就固定端点；不能等后面的 applySettingsToUi() 才注入。
    m_versionManager->setModrinthApiBaseOverride(m_settings->modrinthApiBaseOverride());
    // 后台扫盘的回复。接在版本管理器上（不是某个控制器）：实例下拉框、实例树、服务端页、
    // 预设页的实例下拉框、主页"最近运行"都由 `applyInstalledVersions()` 一次收尾。
    connect(m_versionManager, &VersionManager::installedVersionsScanned, this,
            [this](const QStringList &installed, const QStringList &warnings) {
                if (m_installedRefreshToken != m_pendingInstalledRefreshToken) {
                    return;  // 这之后又有人同步刷过一遍，这份比屏幕上那份旧
                }
                applyInstalledVersions(installed, warnings);
            });
    m_presetController = new PresetController(
        m_versionManager, m_ui->getInstancePresetPage(),
        PresetController::Host{
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &id) { return m_launcherManager->isVersionRunning(id); },
            [this](const QString &text) { appendLog(text); },
            // 模组预设那一趟在后台往 `mods/` 里写文件：期间这一份不能启动、不能删
            // （和「复制实例」「还原」同一个集合，见 installingVersionIds()）。
            [this](const QString &id, bool busy) {
                if (busy) {
                    markFileOperationBusy(id);
                } else {
                    releaseFileOperationBusy(id);
                }
            },
            [this](const QString &id) {
                // 导入整合包那一路也会把 overrides 整棵解压进实例根目录，和预设写的是
                // 同一片地方。它不在搬文件那个计数集合里，单独并进来。
                //
                // NOTE: 只认**导入**这一档，不用 `InstallOccupancy::modifying()`：这是窄的
                // 那道闸（宽的那一道在下面），安装 / 补全期间落预设是允许的。
                return m_fileOperationOccupancy.contains(id)
                       || m_installOccupancy.is(InstallOccupancy::Batch::Import, id);
            },
            // `applyPlan()` 专用、宽的那一份 —— 正被安装 / 补全文件的
            // 实例也不能这时候落预设。它只从预设页那一颗「应用」进来，不存在
            // "安装向导收尾自己挡自己"的问题（那一步走的是下面窄的那道闸）。
            [this](const QString &id) {
                return installingVersionIds().contains(id);
            },
            // 收尾之后重灌实例清单：这一趟期间别的刷新（下载收尾、改设置）会把这一份
            // 按 `installingVersionIds()` 滤掉，不重刷它就一直不在列表里。
            [this]() { refreshInstalledVersions(); },
            // 预设编辑器那一页铺在内容区上（和实例设置同一条路，见 OverlayHost::showPage：
            // 摆上去就返回，收页之后才叫 onClosed）。
            // 归属传 -1 = 现在这一颗选项卡：这一页只从实例页「预设」那一档开得出来。
            [this](QDialog *dialog, const QString &title, int ownerTab,
                   std::function<void(int)> onClosed) {
                OverlayHost *overlay = m_ui->overlay();
                if (!overlay->showPage(dialog, title, ownerTab)) {  // surface-registry-ignore: 转发口，页面在各调用方的入口登记
                    return false;
                }
                // 收页结果接在 `pageClosed()` 上：确定 / 取消 / 返回箭头 / Esc /
                // 被别的页顶掉 —— 全汇到那一处；结果从对话框自己那份 `result()` 读
                // （确定 = Accepted，和原来 `execPage()` 的返回值同一套）。
                //
                // NOTE: 接在 `dialog` 上（不是 `this` 上）当上下文：这一页收掉之后
                // 对话框就 `deleteLater()` 了，连接跟着一起没；接在 this 上的话，每开
                // 一次就多留一条永远匹配不上的连接。
                connect(
                    overlay, &OverlayHost::pageClosed, dialog,
                    [dialog, onClosed](QWidget *page) {
                        if (page == dialog && onClosed) {
                            onClosed(dialog->result());
                        }
                    },
                    Qt::SingleShotConnection);
                return true;
            },
            // 页头那句「N 份预设」：和「当前文件夹：…（N 个实例）」是页头同一格，
            // 换档时由 MainWindowUI 自己换着写（见 setInstanceMode）。
            [this](const QString &text) { m_ui->setPresetSubtitle(text); }},
        this);
    m_presetController->setEditorPrompt([this](const QString &title, const QString &text, const QStringList &buttons) {
        return InlineMessage::ask(m_ui->overlay(), MessageKind::Question, title, text, buttons, buttons.size()-1, buttons.size()-1);
    });
    m_ui->navigationGuard = [this] { return !m_presetController || m_presetController->leaveEditor(); };
    // 模组兼容检查里 CurseForge 那一侧要用同一个 key（设置里填过才算可用）。
    m_presetController->setCurseForgeApiKey(m_settings->curseForgeApiKey());
    m_launcherPreferences = new LauncherPreferencesController(
        m_ui, m_settings, m_javaSettings, m_versionManager, m_resourceStore,
        LauncherPreferencesController::Host{
            [this](const QString &text) { appendLog(text); },
            [this] { refreshInstalledVersions(); }, [this] { refreshAvailableVersions(); }},
        this);
    m_crashReports = new CrashReportController(
        m_activityCenter, m_ui->overlay(), this,
        CrashReportController::Host{[this](const QString &text) { appendLog(text); }}, this);
    // 工具箱那九件小工具：页面、算术、线程全在那个 controller 里，这儿只有
    // "建它"和"把导航轨底部那颗『工具箱』接到它身上"两句话。
    m_toolboxController = new ToolboxController(
        m_ui->overlay(), m_ui->toolboxTab(),
        ToolboxController::Host{
            [this](const QString &text) { appendLog(text); }, m_activityCenter},
        this);
    m_ui->setToolboxToolHandler(
        [this](const QString &key) { m_toolboxController->open(key); });
    m_integrityController = new IntegrityController(
        m_versionManager, m_activityCenter, m_ui->overlay(), this,
        IntegrityController::Host{
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &text) { appendLog(text); },
            [this](std::function<void()> open) { openPageWhenIdle(open); },
            [this](const QString &id) { repairInstanceFiles(id); }},
        this);
    m_backupController = new BackupController(
        m_versionManager, m_activityCenter, m_ui->overlay(),
        BackupController::Host{
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &path) { return instanceIdOfPath(path); },
            [this](const QString &id) { return m_launcherManager->isVersionRunning(id); },
            [this](const QString &id) { return installingVersionIds().contains(id); },
            [this](const QString &text) { appendLog(text); },
            [this](InstanceSettingsDialog *dialog, const QString &worldDir) {
                showWorldSettingsPage(dialog, worldDir);
            },
            [this](const QString &serverId) { m_serverSettings->refreshIfShowing(serverId); },
            // 还原一份实例期间它算「正在搬文件」：不能启动、不能删（见 restoreInstance()）。
            [this](const QString &id, bool busy) {
                if (busy) {
                    markFileOperationBusy(id);
                } else {
                    releaseFileOperationBusy(id);
                }
            },
            [this] { refreshInstalledVersions(); },
            // 服务端世界正在备份 / 还原：那一个服务端不能启动、不能删（见
            // `ServerRunController::fileBusy()` / `removeServer()` 那两道闸）。服务端不是
            // 实例，记不进 `installingVersionIds()`。
            [this](const QString &id, bool busy) { m_serverRun->setFileBusy(id, busy); }},
        this);
    m_serverPingController = new ServerPingController(
        ServerPingController::Host{
            [this](const QString &id) { return serverListOf(id); }}, this);
    // 控制器的分工：服务端控制台、实例文件图标反查、启动历史、游戏会话、
    // 任务详情和实例设置写侧已经各自持有页面/状态/业务判断。MainWindow 只保留跨域协调所需的
    // Host 回调和兼容入口；具体所有权与生命周期约束写在各 Controller 的文件头，不在这里重复。
    m_serverConsole = new ServerConsoleController(
        m_serverWorkspace, m_ui, this,
        ServerConsoleController::Host{
            [this](const QString &text) { appendLog(text); },
            [this](const QString &id) { m_serverRun->stop(id); },
            [this](const QString &id) { m_serverRun->forceKill(id); }},
        this);
    m_folderController = new FolderController(
        m_folderManager, m_launcherManager, m_ui, this,
        FolderController::Host{
            [this](const QString &path) { return folderTitle(path); },
            [this] { refreshFolderList(); }, [this] { refreshInstalledVersions(); },
            [this](const QString &text) { appendLog(text); }},
        this);
    m_modpackExportController = new ModpackExportController(
        m_versionManager, m_settings, m_ui->overlay(),
        ModpackExportController::Host{
            [this](const QString &id) { return installingVersionIds().contains(id); },
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &text) { appendLog(text); },
            [this](const QString &status) {
                m_syncWaitStatus = status;
                updateBottomBar();
            },
            // 任务中心那一格。★ 收尾**不走 `MainWindow::finishTask()`**：
            // 那一位会顺手把"这一批下载还能不能重试"塞进失败的那一项，而导出根本没有那个
            // 失败清单 —— 挂上去会露一颗按下去重做别人下载的「重试」。
            [this](Tasks::TaskKind kind, const QString &title, const QString &targetId,
                   const QList<Tasks::TaskStep> &steps) {
                return m_activityCenter->begin(kind, title, targetId, steps);
            },
            [this](Tasks::TaskId id, const QString &detail) {
                m_activityCenter->tasks()->setDetail(id, detail);
            },
            [this](Tasks::TaskId id, qint64 done, qint64 total) {
                m_activityCenter->tasks()->setProgress(id, done, total);
            },
            [this](Tasks::TaskId id, int index, Tasks::TaskStepState state) {
                m_activityCenter->tasks()->setStepState(id, index, state);
            },
            [this](Tasks::TaskId id, bool success, const QString &errorSummary) {
                m_activityCenter->finish(id, success, errorSummary, false);
            },
            [this](Tasks::TaskId id) { m_activityCenter->tasks()->cancel(id); }},
        this);
    m_instanceFileController = new InstanceFileController(
        m_ui->overlay(), InstanceFileController::Host{
            [this](const QString &text) { appendLog(text); },
            [this](const QString &path) -> QString {
                const QString id = instanceIdOfPath(path);
                if (id.isEmpty()) return QString();
                if (m_launcherManager != nullptr && m_launcherManager->isVersionRunning(id)) {
                    return tr("「%1」正在运行。请退出游戏后再修改此文件。")
                        .arg(instanceDisplayName(id));
                }
                if (installingVersionIds().contains(id)) {
                    return tr("「%1」正在被另一项文件操作占用（复制 / 备份 / 还原 / 安装），"
                              "请等待任务完成后再修改此文件。")
                        .arg(instanceDisplayName(id));
                }
                return QString();
            },
            [this](const QString &path) { return instanceIdOfPath(path); },
            [this](const QString &id, bool busy) {
                if (busy) {
                    markFileOperationBusy(id);
                } else {
                    releaseFileOperationBusy(id);
                }
            }},
        this);
    m_instanceTransferController = new InstanceTransferController(
        m_versionManager, m_launcherManager, m_ui->overlay(),
        InstanceTransferController::Host{
            [this](const QString &id) { return installingVersionIds().contains(id); },
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &id, const QString &key) {
                return instanceFileDirs(id).value(key);
            },
            [this] { refreshInstalledVersions(); },
            [this](const QString &text) { appendLog(text); },
            [this](const QString &id, bool busy) {
                if (busy) {
                    markFileOperationBusy(id);
                } else {
                    releaseFileOperationBusy(id);
                }
            },
            // 任务中心那一格：「复制实例」后台那一趟。收尾同样**不走**
            // `MainWindow::finishTask()` —— 复制没有"可重试的下载批次"。
            [this](Tasks::TaskKind kind, const QString &title, const QString &targetId,
                   const QList<Tasks::TaskStep> &steps) {
                return m_activityCenter->begin(kind, title, targetId, steps);
            },
            // 升级父任务底下的子任务：源实例更新这一趟的"复制 / 快照"不该各占一行。
            [this](Tasks::TaskId parent, Tasks::TaskKind kind, const QString &title,
                   const QString &targetId, const QList<Tasks::TaskStep> &steps) {
                return m_activityCenter->tasks()->beginChild(parent, kind, title, targetId,
                                                              steps, Tasks::TaskCapabilities());
            },
            [this](Tasks::TaskId id, const QString &detail) {
                m_activityCenter->tasks()->setDetail(id, detail);
            },
            [this](Tasks::TaskId id, qint64 done, qint64 total) {
                m_activityCenter->tasks()->setProgress(id, done, total);
            },
            [this](Tasks::TaskId id, qint64 bytesDone, qint64 bytesTotal) {
                m_activityCenter->tasks()->setBytes(id, bytesDone, bytesTotal);
            },
            [this](Tasks::TaskId id, int index, Tasks::TaskStepState state) {
                m_activityCenter->tasks()->setStepState(id, index, state);
            },
            [this](Tasks::TaskId id, const Tasks::TaskCapabilities &caps) {
                m_activityCenter->tasks()->setCapabilities(id, caps);
            },
            [this](Tasks::TaskId id, bool success, const QString &errorSummary) {
                m_activityCenter->finish(id, success, errorSummary, false);
            },
            [this](Tasks::TaskId id, const QString &token) {
                m_activityCenter->tasks()->setRecovery(id, token, !token.isEmpty(), QString());
            },
            [this](const OperationReport::Report &report) {
                return m_fileRecovery != nullptr && m_fileRecovery->registerReport(report);
            },
            // 用户按了「取消」、后台导入真的停下来之后：收成 Cancelled 而不是 Failed
            // （取消不是失败，底栏不该亮红点）。
            [this](Tasks::TaskId id) { m_activityCenter->tasks()->cancel(id); }},
        this);
    m_fileRecovery = new FileOperationRecoveryController(
        m_ui->overlay(),
        FileOperationRecoveryController::Host{
            [this](const QString &id) { return instanceDisplayName(id); },
            [this](const QString &id) {
                return (m_launcherManager != nullptr && m_launcherManager->isVersionRunning(id))
                       || installingVersionIds().contains(id);
            },
            [this] { refreshInstalledVersions(); },
            [this](const QString &text) { appendLog(text); },
            [this](Tasks::TaskKind kind, const QString &title, const QString &targetId,
                   const QList<Tasks::TaskStep> &steps) {
                return m_activityCenter->begin(kind, title, targetId, steps);
            },
            [this](Tasks::TaskId id, const QString &detail) {
                m_activityCenter->tasks()->setDetail(id, detail);
            },
            [this](Tasks::TaskId id, int index, Tasks::TaskStepState state) {
                m_activityCenter->tasks()->setStepState(id, index, state);
            },
            [this](Tasks::TaskId id, bool ok, const QString &error) {
                m_activityCenter->finish(id, ok, error, false);
            },
            [this](Tasks::TaskId id, const QString &token) {
                m_activityCenter->tasks()->setRecovery(id, token, !token.isEmpty(), QString());
            },
            // 续做成功之后把原来那条任务上的恢复入口摘掉（按报告钥匙找那一条，R5）。
            [this](const QString &token) {
                for (const Tasks::TaskSnapshot &task : m_activityCenter->tasks()->all()) {
                    if (task.recoveryToken == token) {
                        m_activityCenter->tasks()->setRecovery(task.id, QString(), false,
                                                               QString());
                    }
                }
            }},
        this);
    // 上一次会话留下的、已经没有报告在用的本模块暂存，启动时收掉（只碰自己的目录名）。
    m_fileRecovery->cleanupStaleStaging(AppPaths::minecraftDir());
    m_instanceDialogController = new InstanceDialogController(this);
    m_instanceEditorController = new InstanceEditorController(this);
    m_importController = new ImportController(this);
    // GameSessionController 要写启动历史，所以必须等 m_launchHistory 就位后再创建。
    m_launchController = new LaunchController(this);
    m_connectionsController = new ConnectionsController(this);
    m_instanceIconController = new InstanceIconController(
        m_settings, m_resourceStore,
        InstanceIconController::Host{
            [this](const QString &versionId) { return instanceFileDirs(versionId); },
            [this](const QString &versionId) { return instanceDisplayName(versionId); },
            [this](const QString &text) { appendLog(text); },
            [this](const QString &iconKey, const ResourceCatalog::Hit &hit) {
                m_resourceHits.insert(iconKey, hit);
            }},
        this);
    // ---------- 「上一次启动」那本账 ----------
    // 它在这儿建（不是初始化列表里），和上面那份任务账本同一个理由：它要读盘，得等前面
    // 几样都就位了；而且必须**赶在第一次 `updateHomeRecent()` 之前** —— 那之后凡是
    // "实例列表变了"的路（重扫实例树那一条在构造里就会走）都会去问它要账本。
    m_launchHistory = new LaunchHistoryController(this);
    // 账本一变就重画主页那两排快速入口。发的位置那一侧管着（读回来 / 记了一条 / 真改到了
    // 一条的结局那三处；结局没对上任何一条的空转不发），这边只管接。
    connect(m_launchHistory, &LaunchHistoryController::changed, this,
            &MainWindow::updateHomeRecent);
    connect(m_launchHistory, &LaunchHistoryController::logMessage, this,
            [this](const QString &text) { appendLog(text); });
    connect(m_launcherManager, &LauncherManager::gameStarted, this, [this](int sessionId) {
        m_launchHistory->startPlayTime(sessionId, m_launcherManager->session(sessionId).versionId,
                                       *m_versionManager, *m_groupStore);
    });
    m_gameSessionController = new GameSessionController(
        m_launcherManager, m_settings, m_versionManager, m_launchHistory, m_ui,
        GameSessionController::Host{
            [this](const QString &text) { appendLog(text); },
            [this](Tasks::TaskId id) {
                const auto snap = m_activityCenter->tasks()->snapshot(id);
                for (int index = 0; index < snap.steps.size(); ++index)
                    if (snap.steps.at(index).state == Tasks::TaskStepState::Running
                        || index == snap.steps.size() - 1)
                        m_activityCenter->tasks()->setStepState(id, index, Tasks::TaskStepState::Done);
                if (snap.totalUnits <= 0) {
                    m_activityCenter->tasks()->setProgress(id, 1, 1);
                }
            },
            // 进程起来的这一刻把账本上那件「启动 X」和会话号对上。
            [this](Tasks::TaskId id, int sessionId) {
                m_activityCenter->bindSession(id, sessionId);
            },
            [this](Tasks::TaskId id, bool ok, const QString &note) { finishTask(id, ok, note); },
            // 用户自己停掉、一行日志都没出的那一次启动：按"取消"收，不记失败（见
            // `GameSessionController::Host::cancelLaunchTask` 的说明）。
            [this](Tasks::TaskId id) {
                if (id != Tasks::invalidTaskId()) m_activityCenter->tasks()->cancel(id);
            },
            [this] { updateBottomBar(); },
            [this] { updateInstanceTreeRunState(); },
            [this](const QString &versionId) { return instanceDisplayName(versionId); },
            [this](int sessionId, int exitCode, bool crashed) {
                reportGameCrash(sessionId, exitCode, crashed);
            },
            [this](int sessionId) { exportGameLog(sessionId); },
            [this](int sessionId) { exportGameErrorReport(sessionId); },
            [this] { return frameGeometry(); },
            [this] {
                showNormal();
                raise();
                activateWindow();
            }},
        this);
    m_taskDetail = new TaskDetailController(
        m_activityCenter, m_crashReports, m_versionManager, m_gameSessionController,
        m_ui->overlay(), this,
        TaskDetailController::Host{
            [this](const QString &text) { appendLog(text); },
            [this](const Tasks::TaskSnapshot &task) { return cancelWouldWork(task); },
            [this](const QString &key) { cancelTaskFromDetail(key); },
            [this](const QString &key) { retryTaskFromDetail(key); },
            [this](const QString &id) { m_importController->openImportReport(id); },
            [this](const QString &key) {
                bool ok = false;
                const Tasks::TaskId id = key.toULongLong(&ok);
                if (ok && m_toolboxController && m_toolboxController->ownsDownloadTask(id)) {
                    m_toolboxController->resumeDownloadTask(id); return;
                }
                if (ok && m_downloadBatches && m_downloadBatches->owns(id)) { m_downloadBatches->resume(id); return; }
                if (ok && m_presetController && m_presetController->ownsModTask(id)) { m_presetController->resumeModDownload(id); return; }
                if (!ok || m_fileRecovery == nullptr) return;
                const Tasks::TaskSnapshot task = m_activityCenter->tasks()->snapshot(id);
                if (!task.recoveryToken.isEmpty()) m_fileRecovery->resume(task.recoveryToken);
            },
            [this](const QString &key) {
                const auto id = key.toULongLong();
                if (m_downloadBatches && m_downloadBatches->owns(id)) m_downloadBatches->pause(id);
                else if (m_presetController && m_presetController->ownsModTask(id)) m_presetController->pauseModDownload(id);
                else if (m_toolboxController) m_toolboxController->pauseDownloadTask(id);
            }},
        this);
    connect(m_serverWorkspace, &ServerWorkspaceController::message, this,
            [this](const QString &text) { appendLog(text); });
    connect(m_serverWorkspace, &ServerWorkspaceController::statusChanged, this,
            &MainWindow::onServerStatusChanged);
    connect(m_serverWorkspace, &ServerWorkspaceController::lineAppended, this,
            &MainWindow::onServerLineAppended);
    connect(m_serverWorkspace, &ServerWorkspaceController::forceKillAvailable, this,
            &MainWindow::onServerForceKillAvailable);
    connect(m_serverWorkspace, &ServerWorkspaceController::exited, m_serverRun,
            &ServerRunController::onExited);
    // 收起态那一格的重画时机里有一档是"刚完成那一项露到头了"（「完成任务短暂显示后
    // 收进历史」）—— 那个到期**没有任何信号会来**，只能自己起一个单发定时器。
    m_taskSummaryTimer = new QTimer(this);
    m_taskSummaryTimer->setSingleShot(true);
    connect(m_taskSummaryTimer, &QTimer::timeout, this, &MainWindow::updateBottomBarTaskSummary);
    // 账本一变就重画底栏那**两**格（中间那格 + 右下那格）。**不去分辨"变的是哪一项"**：
    // 收起态显示谁是把整张账本放在一起算出来的（`Tasks::collapsedSummary()`），只重画
    // "变了的那一行"在这里没有对应的东西可画（那一格只有一个整体）。
    //
    // NOTE: 整条底栏（`updateBottomBar()`）**不许**接在这上面：账本每来一帧进度都会发这个
    // 信号（下载那份是 200 ms 一帧），接上去等于每帧连头像和账号名一起重刷。
    connect(m_activityCenter, &ActivityCenterController::activityChanged, this, [this]() {
        updateBottomBarTaskSummary();
        updateBottomBarRunState();
    });
    // 「已完成」那一栏变了（收尾 / 清除 / 读盘）：面板要是开着，跟着重画一遍。
    connect(m_activityCenter, &ActivityCenterController::historyChanged, this, [this]() {
        m_ui->refreshTaskCenterPanel();
    });
    // ---------- 「实例设置」写侧那一套 ----------
    // 它自己一块界面都不碰（**一个字节的中文都没有**），要问、要说的那几件事全走下面这六个
    // 回调：这几件恰恰是"只有主窗口做得成"的事（谁在跑要问 LauncherManager、那句话怎么说要
    // 挑中文、重扫实例树是主窗口的活）。
    //
    // 为什么这几个回调是**同步**的：`notice` 是在 controller 那一句代码的位置上当场执行的，
    // 顺序变了用户看到的东西就变（提示卡片是一张盖一张的）。所以这里一行都不许改成排队/异步。
    m_instanceSettings = new InstanceSettingsController(
        m_versionManager, m_groupStore, m_settings,
        InstanceSettingsController::Host{
            // 谁在跑。**必须问 LauncherManager**，不能自己看那个"正在启动"的标志位 ——
            // 一个会话起来之后标志位就落了，而游戏还在跑。
            [this](const QString &versionId) {
                return m_launcherManager->isVersionRunning(versionId);
            },
            // 谁在体检。那一位是主窗口手上的（`m_integrityScanningVersion`），controller
            // 那一层够不着也不该够着。
            [this](const QString &versionId) { return integrityScanningVersion(versionId); },
            // 谁正被别的作业占着（安装 / 导入 / 复制 / 快照 / 还原）。问的是那一份合集
            // —— `installingVersionIds()` 是"谁动不得"的唯一出处，启动和删除读的也是它。
            [this](const QString &versionId) {
                return installingVersionIds().contains(versionId);
            },
            [this](const InstanceSettingsController::Notice &notice) {
                showInstanceSettingsNotice(notice);
            },
            [this](const QString &text) { appendLog(text); },
            // 名字/图标都变了 ⇒ 实例树得重扫（那棵树上那一格画的就是这两个）。
            [this]() { refreshInstalledVersions(); },
            // 改完名要显式选中新那一个：刷新时下拉框那边是"保住原来选中的 id"，而那个 id
            // 已经不在了，于是它会停在空选。
            [this](const QString &newId) {
                QComboBox *combo = m_ui->getInstalledVersionCombo();
                const int index = combo->findData(newId);
                if (index >= 0) {
                    combo->setCurrentIndex(index);  // 这条连接是活的，按钮文字会跟着刷新
                }
                updateStartButtonText();
            },
            // 分组/置顶那三条规则走的是**现有那两个入口**（不是在这儿另写一遍
            // `wtfml.folders` 的读写）：它们各自会发 groupsChanged 把树重灌，而且
            // "没有置顶组时什么都不做""认不出来就落未分组"这些判断都已经在那边了。
            [this](const QString &versionId, const QString &groupId) {
                setInstanceHomeGroup(versionId, groupId);
            },
            [this](const QString &versionId) { pinInstance(versionId); },
            [this](const QString &versionId) { unpinInstance(versionId); }},
        this);
    // 任务中心那块面板要画什么、以及「清除已完成」被点了做什么。**每次点开/重画现问一遍**
    // （和底栏那两块面板一个路数：面板自己不会跟着账本刷新，缓存上一次那几行迟早说假话）。
    //
    // NOTE: provider 那一头有一处**副作用**（"列出来"就是"看过了"，见 taskCenterItems()），
    // 所以它不能是 const 的、更不能被别人当"读一下"随便调。
    m_ui->setTaskCenterContent([this]() { return taskCenterItems(); },
                               [this]() { m_activityCenter->clearFinished(); });
    // 点任务中心里的**一行** = 开任务详情页（点击任务进入内嵌 TaskDetailPage）。
    m_ui->setTaskCenterRowActivatedHandler([this](const QString &key) { openTaskDetailPage(key); });
    // 主页启动卡片（#65）：数据只有任务中心那一份（阶段 / 进度 / 失败原因），卡片和任务中心说的是同一件事。
    connect(m_activityCenter->tasks(), &Tasks::TaskManager::taskChanged, this,
            [this](Tasks::TaskId id) { syncLaunchCard(id); });
    m_ui->setLaunchCardActionHandler([this](quint64 id, MainWindowUI::LaunchCardAction action) {
        if (action == MainWindowUI::LaunchCardAction::Details) {
            openTaskDetailPage(QString::number(id));
            return;
        }
        // 卡片上唯一一处取消：走 #61 的统一入口（进程起来前停启动流程，起来后停会话），任务记「已取消」。
        cancelLaunch(m_activityCenter->tasks()->snapshot(id));
    });
    // 行尾那颗「取消」：走的是和详情页里那一颗**完全同一条路**（`onTaskDetailAction`）——
    // 两处各写一遍的话，"取消"的语义迟早会分家（一处删文件、一处不删）。
    m_ui->setTaskCenterRowActionHandler(
        [this](const QString &key, TaskCenter::RowAction action) {
            switch (action) {
            case TaskCenter::RowAction::Pause:
                onTaskDetailAction(key, TaskDetailPage::Action::Pause);
                break;
            case TaskCenter::RowAction::MoveUp:
            case TaskCenter::RowAction::MoveDown:
                m_activityCenter->moveQueued(key.toULongLong(), action == TaskCenter::RowAction::MoveUp ? -1 : 1);
                break;
            case TaskCenter::RowAction::Viewed:
                m_activityCenter->markViewed(key);
                break;
            case TaskCenter::RowAction::Seen:
                m_activityCenter->markSeen(key);
                break;
            case TaskCenter::RowAction::Cancel:
                onTaskDetailAction(key, TaskDetailPage::Action::Cancel);
                break;
            case TaskCenter::RowAction::Retry:
                onTaskDetailAction(key, TaskDetailPage::Action::Retry);
                break;
            case TaskCenter::RowAction::OpenTarget:
                onTaskDetailAction(key, TaskDetailPage::Action::OpenTarget);
                break;
            case TaskCenter::RowAction::RepairImport:
                onTaskDetailAction(key, TaskDetailPage::Action::RepairImport);
                break;
            case TaskCenter::RowAction::Recover:
                onTaskDetailAction(key, TaskDetailPage::Action::Recover);
                break;
            case TaskCenter::RowAction::OpenLog:
                onTaskDetailAction(key, TaskDetailPage::Action::OpenLog);
                break;
            case TaskCenter::RowAction::ExportReport:
                onTaskDetailAction(key, TaskDetailPage::Action::ExportReport);
                break;
            case TaskCenter::RowAction::OpenCrashDir:
                onTaskDetailAction(key, TaskDetailPage::Action::OpenCrashDir);
                break;
            }
        });
    m_ui->setHomeQuickActionHandler(
        [this](const QString &key, MainWindowUI::HomeQuickAction action) {
            if (action == MainWindowUI::HomeQuickAction::SwitchInstance) {
                openHomeInstancePicker();
                return;
            }
            if (key.isEmpty() || m_versionManager == nullptr
                || !m_versionManager->installedVersions().contains(key)) {
                updateHomeRecent();
                return;
            }
            if (action == MainWindowUI::HomeQuickAction::Edit) {
                openInstanceSettings(key);
                return;
            }
            if (action == MainWindowUI::HomeQuickAction::Stop) {
                // 停的是这个实例名下在跑的每一份会话（多开时一起停）。
                for (const LauncherManager::GameSession &game : m_launcherManager->runningGames()) {
                    if (game.versionId == key) stopGameSession(game.sessionId);
                }
                return;
            }
            // Select = 设为当前实例；Launch = 设为当前再启动（实例带上那颗 ▶ 也走这里）。
            QComboBox *combo = m_ui->getInstalledVersionCombo();
            const int row = combo == nullptr ? -1 : combo->findData(key);
            if (row >= 0) {
                combo->setCurrentIndex(row);
                if (action == MainWindowUI::HomeQuickAction::Launch) onStartButtonClicked();
            }
        });
    // ---------- 主页「每日一言」 ----------
    // 建在 setupConnections() 之前：设置页那个开关的槽要它。取句全在后台异步，
    // 起点是下面 applySettingsToUi() 之后的 refresh()（关着就一个请求都不发）。
    m_dailyQuote = new DailyQuoteController(this, m_settings);
    m_dailyQuote->shown = [this](const DailyQuote::Quote &quote, bool visible) {
        m_ui->setHomeHeroQuote(visible ? quote.text : QString(),
                               visible ? DailyQuote::attribution(quote) : QString(),
                               quoteScriptForTheme(quote.script));
    };
    setupConnections();
    // 键盘入口：窗口级过滤器挂在自己身上（键从焦点控件冒泡上来才看得到），
    // 见 MainWindowEventFilter.cpp 开头那一段。
    installEventFilter(this);
    // F5：不绑某一段逻辑，只去点"当前页上那颗刷新"（见 `refreshTopmostPage()`）。
    m_refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
    connect(m_refreshShortcut, &QShortcut::activated, this, &MainWindow::refreshTopmostPage);
    // 下载页那五页的筛子先灌上（来源 / 类型 / 加载器那三项这一刻就定得下来；「版本」那一栏
    // 等在线清单一到才有的灌，见 onVersionsFetched）。界面那边只搭空框 —— ui/ 不 include
    // src/，"每一项是什么"只能由这一层灌进去。
    fillDownloadFilters();
    // 设置页先按盘上的值摆好（不扫 Java，那一段等第一次切到设置页，见
    // refreshJavaList()）。必须在下面 fetchAvailableVersions() **之前**：
    // 清单请求要不要走镜像就是在这一步交给 VersionManager 的。
    applySettingsToUi();
    // 主窗口按上次记住的大小开（只记大小不记位置）。改大小节流 400 ms 落盘：进程被直接杀掉
    // 最多丢最后一次改动；正常关窗 closeEvent 里再补一次。
    MainWindowUI::restoreWindowSize(this, m_settings->mainWindowSize());
    m_windowSizeSaveTimer = new QTimer(this);
    m_windowSizeSaveTimer->setSingleShot(true);
    m_windowSizeSaveTimer->setInterval(400);
    connect(m_windowSizeSaveTimer, &QTimer::timeout, this,
            [this] { m_settings->setMainWindowSize(MainWindowUI::rememberedWindowSize(this)); });
    m_ui->setWindowSizeHandler([this] { m_windowSizeSaveTimer->start(); });
    connect(m_ui->getWindowSizeResetButton(), &QPushButton::clicked, this, [this] {
        MainWindowUI::resetWindowSize(this);
        m_windowSizeSaveTimer->stop();
        m_settings->setMainWindowSize(MainWindowUI::rememberedWindowSize(this));
    });
    // 主页那一行到这一步才决定：盘上关着就什么都不发；开着先显示缓存 / 池 / 兜底，
    // 网络回来再替换（不阻塞首帧）。
    m_dailyQuote->refresh();
    // 上次用的是个**离线账号**：把名字接回 AuthManager。**必须在下面 updateBottomBar()
    // 之前** —— AuthManager 构造函数只读活动槽，而离线账号在盘上**没有凭据**，它读出来的
    // 是占位的 "Player"：不接回来的话底栏和 --username 里是个假名字。
    //
    // NOTE: 「这一趟走离线」这件事**只由"当前账号是离线账号"决定**（见 offlineIdentity()），
    // 主页没有离线模式勾选框 —— 不接名字的话启动那条路会去要凭据，而盘上一份都没有，
    // 用户拿到的是"请先登录"，一个死胡同。
    {
        const QString active = AccountStore::activeUuid();
        if (AccountStore::isOffline(active)) {
            m_authManager->setOfflinePlayer(AccountStore::nameOf(active));
        }
    }
    // 「游戏文件夹」：先把当前那一份交给 AppPaths —— 从这一句起，扫版本、下资源、
    // 找 Java 用的都是它。**必须在 refreshInstalledVersions() 之前**，那个函数扫的
    // 就是这个根目录。左边那张列表此刻还没有条目可选，所以要挡着信号灌。
    applyCurrentFolderToPaths();
    refreshFolderList();
    // 上次那本任务账读回来（只存摘要与错误；**没跑完的标成"已中断"，不假装继续**）。
    //
    // 账本是启动器级的固定文件，不随当前游戏文件夹切换。这样正在下载/启动时切换目录，
    // 内存里的任务不会在下一次落盘时被写进另一份 .minecraft。
    m_activityCenter->loadHistory();
    m_presetController->setRecoveryHost(m_activityCenter, [this] { return m_downloadTaskLedger->task(); });
    m_downloadBatches = new DownloadBatchController(m_activityCenter, m_versionManager->downloads(),
        [this] {
            if (m_serverInstall && m_serverInstall->ownsDownloadBatch()) return m_serverInstallTaskId;
            if (!m_versionManager->ownsFileBatch()) return Tasks::invalidTaskId();
            return m_downloadTaskLedger->task() != Tasks::invalidTaskId()
                       ? m_downloadTaskLedger->task() : m_updateBatch.task;
        }, this);
    m_downloadBatches->setContinuation([this] { return snapshotDownloadContinuation(); },
        [this](Tasks::TaskId id, const QJsonObject &intent) { return continueDownloadInstallation(id, intent, true); },
        [](const QJsonObject &intent, const std::atomic_bool *cancel) { return Presets::unchangedInitializationContinuation(intent, cancel); },
        // 「要写预设的那几趟」要拍的那棵配置树：**在后台算**（见 `requestContinuationTargets`）。
        // 只读意图里的值，不碰界面/启动器状态，所以可以放心在 worker 里跑。
        [](const QJsonObject &intent, const std::atomic_bool *cancel) {
            const auto root = intent.value("root").toString(), instance = intent.value("instance").toString();
            if (!QDir::isAbsolutePath(root) || instance.isEmpty()) return QJsonObject();
            return Presets::snapshotInitializationTargets(QDir(root).filePath(QStringLiteral("versions/") + instance), cancel);
        });
    m_downloadBatches->setRestoreAllowed([this] {
        return !m_downloadInFlight && runningVersionIds().isEmpty() && !m_launchController->isStarting()
            && m_serverWorkspace->runningServerIds().isEmpty()
            && installingVersionIds().isEmpty() && (!m_serverInstall || !m_serverInstall->isRunning());
    });
    m_downloadBatches->load();
    // 「忽略这一版 / 固定这一版」那一本账：**启动时读一次**。
    //
    // 读不回来（文件被改坏了 / 权限不对）**不拦任何事**：那是"以后别再提示我"这一档的
    // 记忆，读丢了的最坏结果是那几条又提示了一次 —— 比"启动时弹一个框说这本账读不动、
    // 然后用户什么也做不了"轻得多。所以只写日志。
    {
        QString policyProblem;
        m_updateCenter->policy() = UpdatePolicy::load(UpdatePolicy::storePath(), &policyProblem);
        if (!policyProblem.isEmpty()) {
            appendLog(tr("无法读取「更新中心」的记录：%1").arg(policyProblem));
        }
    }
    // 本地已安装的先填上（同步扫盘）；在线清单是异步拉的，回来之后由
    // onVersionsFetched() 填"可安装"下拉框 —— 在那之前它显示占位文案。
    // Pending upgrade journals use rename-only recovery, before scanning installed instances.
    // 2.1.5a：游戏根可能被改名/搬走，所以扫描**已配置的游戏根**，每个根用 journal 里的
    // 根指纹验证。只扫文件夹列表，不全盘搜索；指纹对不上的事务不动盘。
    {
        QStringList upgradeRoots = m_folderManager != nullptr ? m_folderManager->folders()
                                                              : QStringList();
        upgradeRoots.append(AppPaths::minecraftDir());
        for (const QString &message : InstanceUpgrade::recoverInterruptedInRoots(upgradeRoots)) {
            appendLog(message);
        }
    }
    // 当前实例在这一趟里就定下来（上次启动 → 上次选中 → 第一个，见 resolveCurrentInstance()），
    // 必须在 fetchAvailableVersions() 之前，免得等网络时用户手选的那个被抢掉。
    refreshInstalledVersions();
    // 启动历史账本：读回来并刷新主页“最近游玩”排（读完发一声 `changed()`，接在
    // `updateHomeRecent()` 上；那一发是**同步**的，所以下面几行看到的已经是刷新过的界面）。
    //
    // 位置有一处讲究：必须在 `refreshInstalledVersions()` **之后** —— 那张卡上「继续游玩」
    // 还是「启动实例」要看"那一份实例还在不在已安装列表里"，列表空着的时候问它只会得到
    // "不在"，卡片就会显示成降级的那一档（明明能玩）。
    m_launchHistory->load();
    // 主页问候：先拿旧的「上次打开日期」和这一刻有没有实例定好，再把今天写回去
    // （下面 updateBottomBar() 里第一次算出来的就是这次该说的那句）。
    m_greetingLastOpened = m_settings->lastOpenedDate();
    m_greetingHadInstances = m_ui->getInstalledVersionCombo()->count() > 0;
    m_settings->setLastOpenedDate(QDate::currentDate());
    // 开着窗跨了时段（下午 → 晚上），切回来时换一句。不挂定时器。
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) updateHomeGreeting();
    });
    // 底栏那三格的初始值：登录名可能已经从存下来的令牌里恢复了，进度两格先藏起来，
    // 运行状态也要等第一次真的启动才有内容。
    updateBottomBar();
    // 主页右栏那个小人 + 底栏头像。放在 updateBottomBar() 之后是因为它要问
    // AuthManager"登过没有"，而那份状态是在它自己的构造函数里从盘上读回来的。
    // **必须在 fetchAvailableVersions() 之前**：那一下要发网络请求，外观这条线不该
    // 排在它后面（离线那条路更是完全不让它走）。
    startPlayerSkinLoad();
    // 底栏那两块上拉面板里画什么、行尾的按钮点了做什么（面板本身在 MainWindowUI 里，
    // 它不查数据）。**每次点开现算一遍**：面板弹出来那一刻的一张快照，它自己不会跟着
    // 刷新（实例随时会结束、账号随时会多一个），所以这里不许缓存上一次那几行。
    //
    // NOTE: 行里带着的 key 是"**这一行是谁**"（左面板 = 会话号、右面板 = 账号 uuid），不是
    // 行号 —— 面板弹出来到某个按钮被点之间隔着用户看的那两眼，那中间列表是会变的
    // （见 MainWindowUI::PanelRow::key 那段）。
    m_ui->setBottomBarPanelContent(
        [this](bool account) -> QList<MainWindowUI::PanelRow> {
            if (account) {
                // 「最后登录的三个账户」—— 3 这个数只在这一处。
                const QList<SavedAccount> saved = AccountStore::accounts();
                // 每个人的脸**一次算完**再交给 recentAccountPanelRows()：那个函数是纯函数
                // （它不认识缓存目录、也不该认识）。这里给**全部**账号各算一张、不是只算
                // 要显示的那三张 —— 挑哪三张是那个函数的事（它按 saved_at 倒序取前三个），
                // 在调它之前再排一遍序就是两处各算一遍。
                QHash<QString, QPixmap> faces;
                faces.reserve(saved.size());
                for (const SavedAccount &one : saved) {
                    faces.insert(one.uuid, accountFacePixmap(one));
                }
                QList<MainWindowUI::PanelRow> rows =
                    recentAccountPanelRows(saved, kAccountPanelLimit, faces);
                // 末尾两行菜单项（设计稿 .acpop .mi）：一个账号都没有时只摆「登录」。
                const QColor ink = QApplication::palette().color(QPalette::PlaceholderText);
                const auto menuRow = [&ink](const QString &key, const QString &title, const QString &icon) {
                    MainWindowUI::PanelRow row;
                    row.key = key;
                    row.title = title;
                    row.menuItem = true;
                    if (!icon.isEmpty()) row.icon = VersionIcons::line(icon, 18, ink, 1.8);
                    return row;
                };
                if (saved.isEmpty()) {
                    rows.append(menuRow(kAccountPanelLoginKey, tr("登录"), QString()));
                } else {
                    rows.append(menuRow(kAccountPanelAddKey, MainWindowUI::accountAddText(), QStringLiteral("plus")));
                    rows.append(menuRow(kAccountPanelManageKey, MainWindowUI::accountSkinButtonText(),
                                        QStringLiteral("user")));
                    // 登出只有这一个入口（「管理账号与皮肤」里只有删除）：有当前身份时才摆。
                    if (!AccountStore::activeUuid().isEmpty()) {
                        rows.append(menuRow(kAccountPanelLogoutKey, MainWindowUI::accountLogoutText(),
                                            QStringLiteral("logout")));
                    }
                }
                return rows;
            }
            return runningGamePanelRows(
                runningProcessEntries(), m_versionManager,
                // 图标那一档颜色 = 底栏那几行字的墨色（画的图标不带自己的颜色）。
                m_ui->getBottomBar()->palette().color(QPalette::WindowText),
                QDateTime::currentMSecsSinceEpoch(),
                [history = QPointer<LaunchHistoryController>(m_launchHistory)](int sessionId) {
                    if (!history) return QString();
                    const auto time = history->runningPlayTime(sessionId);
                    if (!time) return QString();
                    return tr("本次 %1 · 累计 %2").arg(LaunchHistory::playTimeText(time->currentMs),
                                                      LaunchHistory::playTimeText(time->totalMs));
                });
        },
        [this](bool account, const QString &key, int action) {
            onBottomBarPanelAction(account, key, action);
        });

    // 主题一换，底栏要按当前状态**重画一遍**：那颗三态方块和头像都是**位图**（颜色已经画进
    // 像素里了），底栏自己那几位（模式文字、次要文字的颜色）也是建的时候从调色板取的。
    // `updateBottomBar()` 正是"底栏全部内容按当前状态重画"那一个函数，它每次状态变化都会
    // 被调到、本身是幂等的 —— 挂它就够了。
    //
    // NOTE: 挂的是**底栏**那个控件（常驻、和窗口同寿）⇒ 这一条不会比窗口活得长。
    Theme::follow(m_ui->getBottomBar(), [this]() { updateBottomBar(); });

    m_versionManager->fetchAvailableVersions();
}

// ---------- 窗口的生命周期（析构里的四趟后台活 / 外框消息 / 关窗那一刻）----------
// 在 `src/MainWindowLifecycle.cpp`（同一个类，另一个翻译单元）。


void MainWindow::setupConnections() {
    m_connectionsController->setup();
}

// ---------- 底栏那一行的投影（同步等待 / 全局忙态 / 运行方块 / 三根进度条）----------
// 在 `src/MainWindowBottomBar.cpp`（同一个类，另一个翻译单元）。


void MainWindow::appendLog(const QString &text) {
    // 界面里**没有**日志面板：命令行里本来就有
    // LauncherManager 的全套输出，多加一块面板只是把同一批东西抄第二遍。
    //
    // NOTE: 所以这条通道是"给人事后看"的，不是"必须让人看见"的。**必须让用户看到**
    // 的消息（缺 Java、下载失败、未选版本、找不到 version.json）一律走
    // `InlineMessage`（`ui/InlineMessage.hpp`，全仓已无一处 `QMessageBox`）挡住
    // 操作，绝不能只写这里。
    qDebug().noquote() << QStringLiteral("[WTFML]") << SecretRedactor::redact(text);
}

// ---------- 实例树 / 已安装版本 / «启动» 那一行 ----------
// 在 `src/MainWindowInstanceTree.cpp`（同一个类，另一个翻译单元）。


// ---------- 行里那些小按钮的事件路由 ----------
// 在 `src/MainWindowEventFilter.cpp`（同一个类，另一个翻译单元）。


void MainWindow::onLoaderVersionsFetched(const QString &gameVersion, int loaderIndex, bool success,
                                         const LoaderMeta::LoaderVersionList &versions,
                                         const QString &errorMessage) {
    m_installPage->onLoaderVersionsFetched(gameVersion, loaderIndex, success, versions,
                                           errorMessage);
}

// ---------------------------------------------------------------------------
// 下载页后面那几页：搜索 → 挑游戏版本 → 挑版本号 → 装进实例
// ---------------------------------------------------------------------------
//
// 这几页（整合包 / 模组 / 数据包 / 资源包 / 光影）走的是**同一条路**，区别只有"搜哪一类"
// 和"下到哪儿去"。
// 三层树里每一层都是用户点开才动：
// 第一层 资源 —— 进页面时空搜索词问一次（= 这一类里最热门的），回车重搜
// 第二层 游戏版本 —— 搜索响应里本来就带着这个资源支持哪些游戏版本，**不用再问接口**
// 第三层 版本号 —— 点开第二层那一下才问（一个资源 × 一个游戏版本 = 一条请求）
// 点第三层那一下 = 下它（下到实例的 mods/ 、datapacks/ 之类；整合包那条走缓存 + 导入）。
//
// 三层都是**平条目**（不是自绘控件），用的是版本树/加载器树那同一个代理，所以长得一样
// （见 VersionTreeDelegate 第三个用户那段）。

/// 「这一条在第几层」。0 = 资源 / 1 = 游戏版本 / 2 = 版本号；不在树上回 -1。
///
/// NOTE: 代理那边认层用的是"有没有爹"（isChannel），这边认层用的是"往上数几层" ——
/// 两处必须说的是同一件事，而"往上数几层"这句话在这里写一次（见头文件那段）。
int MainWindow::resourceRowDepth(const QTreeWidgetItem *item) {
    if (item == nullptr) {
        return -1;
    }
    int depth = 0;
    const QTreeWidgetItem *walk = item;
    while (walk->parent() != nullptr) {
        walk = walk->parent();
        ++depth;
    }
    return depth;
}

QTreeWidgetItem *MainWindow::addResourcePlaceholderRow(QTreeWidget *tree, const QString &text) {
    if (tree == nullptr) {
        return nullptr;
    }
    auto *item = new QTreeWidgetItem(tree);
    item->setText(MainWindowUI::versionTreeKeyColumn(), text);
    // 灰的（禁用）—— 和加载器树里「正在获取…」「列表没取到」那两行是同一种样子、
    // 也是同一种意思："这一行不是能点的东西"。判据见 onResourceItemActivated 开头。
    // NOTE: 不用 setFlags(Qt::ItemIsEnabled) 那一套"能看不能选"：代理是看 enabled 位
    // 整行淡 50% 的（见 VersionTreeDelegate 里 rowOpacity），禁用位才是"灰"的唯一出处。
    item->setDisabled(true);
    return item;
}

QTreeWidgetItem *MainWindow::addResourcePlaceholderChild(QTreeWidgetItem *parent,
                                                        const QString &text) {
    if (parent == nullptr) {
        return nullptr;
    }
    auto *item = new QTreeWidgetItem(parent);
    item->setText(MainWindowUI::versionTreeKeyColumn(), text);
    item->setDisabled(true);
    return item;
}

// ---------- 下载页每一页顶上那一行筛子：实现在 controllers/DownloadFilterController ----------
//
// 留下的这几条是**薄壳**（主窗口别处按名字叫它们：灌筛子、开分类、拼搜索请求钥匙）。
// 那一份"读也只从这几个访问器读"的规矩没变，只是它们现在都在控制器里。
bool MainWindow::kindForSearchRequestKey(const QString &key, ResourceCatalog::Kind *out,
                                         ResourceCatalog::Source *outSource) const {
    return m_downloadFilter->kindForSearchRequestKey(key, out, outSource);
}

void MainWindow::ensureResourcePageLoaded(ResourceCatalog::Kind kind) {
    m_downloadFilter->ensureResourcePageLoaded(kind);
}

void MainWindow::refreshDownloadCategory(int row) {
    m_downloadFilter->refreshDownloadCategory(row);
}

ResourceCatalog::Source MainWindow::downloadFilterSource(ResourceCatalog::Kind kind) const {
    return m_downloadFilter->downloadFilterSource(kind);
}

QString MainWindow::downloadFilterGameVersion(ResourceCatalog::Kind kind) const {
    return m_downloadFilter->downloadFilterGameVersion(kind);
}

QString MainWindow::downloadFilterLoader(ResourceCatalog::Kind kind) const {
    return m_downloadFilter->downloadFilterLoader(kind);
}

QString MainWindow::downloadFilterCategory(ResourceCatalog::Kind kind) const {
    return m_downloadFilter->downloadFilterCategory(kind);
}

void MainWindow::fillDownloadFilterVersionOptions() {
    m_downloadFilter->fillDownloadFilterVersionOptions();
}

void MainWindow::refreshDownloadFilterOptions(ResourceCatalog::Kind kind) {
    m_downloadFilter->refreshDownloadFilterOptions(kind);
}

void MainWindow::fillDownloadFilters() {
    m_downloadFilter->fillDownloadFilters();
}

void MainWindow::resetDownloadFilters(ResourceCatalog::Kind kind) {
    m_downloadFilter->resetDownloadFilters(kind);
}


void MainWindow::startResourceSearch(ResourceCatalog::Kind kind) {
    m_resourceSearch->startResourceSearch(kind);
}

void MainWindow::fillResourceHitRows(QTreeWidget *tree, ResourceCatalog::Kind kind,
                                     const ResourceCatalog::SearchResult &result) {
    m_resourceSearch->fillResourceHitRows(tree, kind, result);
}

void MainWindow::onResourceSearchFinished(const QString &requestKey,
                                         const ResourceCatalog::SearchResult &result) {
    m_resourceSearch->onResourceSearchFinished(requestKey, result);
}

void MainWindow::onResourceIconReady(const QString &iconId, const QPixmap &icon) {
    m_resourceSearch->onResourceIconReady(iconId, icon);
}

void MainWindow::onResourceItemActivated(QTreeWidgetItem *item) {
    m_resourceSearch->onResourceItemActivated(item);
}

void MainWindow::fillResourceVersionRows(QTreeWidgetItem *gameItem,
                                        const QList<ResourceCatalog::VersionEntry> &versions,
                                        const QString &problem) {
    m_resourceSearch->fillResourceVersionRows(gameItem, versions, problem);
}

void MainWindow::onResourceVersionsFinished(const QString &requestKey,
                                           const QList<ResourceCatalog::VersionEntry> &versions,
                                           const QString &problem) {
    m_resourceSearch->onResourceVersionsFinished(requestKey, versions, problem);
}

void MainWindow::downloadResourceVersion(QTreeWidgetItem *item) {
    m_resourceSearch->downloadResourceVersion(item);
}

void MainWindow::installResourceVersion(ResourceCatalog::Kind kind, const QString &displayTitle,
                                        ResourceCatalog::VersionEntry version,
                                        ResourceCatalog::Source source) {
    // 薄壳：缺前置那一趟、以及"下到哪儿"全在 `ResourceInstallController`（见那边头文件
    // 第一段 —— 这一趟的形状和每一条判断的理由都写在那儿）。
    m_resourceInstall->install(kind, displayTitle, version, source);
}

// ---------- 装前置时"装进哪个实例、按哪个版本筛" ----------
// 在 `src/MainWindowResourceDependency.cpp`（同一个类，另一个翻译单元）。


// ---------- 实例更新检测与更新中心（检测 / 可更新名单 / 换文件 / 收尾）----------
// 在 `src/MainWindowUpdateCenter.cpp`（同一个类，另一个翻译单元）。


// ============================ 资源详情页 ============================
//
// 下载页点中一个模组后进入一个**整页**：上方是模组图像、名字、来源和转到
// modrinth/curseforge/mcmod.cn 的入口，中间一排版本，下方排列版本。
// "独立界面"落成**整页覆盖层**（`OverlayHost::showPage`，顶上四个选项卡和
// 底栏照旧露着，不另开窗）。
//
// 三件事的分工（写清楚免得下次改错地方）：
// - **界面的样子**全在 ui/ResourceDetailPage.{hpp,cpp}（它不 include 任何 src/ 下的头）；
// - **数据**全在 src/download/ResourceCatalog（拼网址、分组、时间那句、渠道那一档）；
// - **下面这一片**是接线：谁去问网络、点哪颗按钮干什么、哪一行是"下它"。
// 所以这里没有一行"这一页长什么样"的代码，那一页也没有一处"要不要联网"的决定。

// ---------- 「资源详情」那一页：状态与那几条函数都搬进 controllers/ResourcePageController ----------
//
// 留下的这几条是**薄壳**：`onResourceDownloadFinished` 被 `ConnectionsController` 按名字
// 接走（`&MainWindow::onResourceDownloadFinished`），另两条被主窗口别处直接叫。页面状态
// 与"开页/拨勾选框/筛分组"那几条内部函数全在控制器里，这里只转发。
void MainWindow::setupResourceDetailPage() {
    if (m_resourcePage == nullptr) {
        m_resourcePage = new ResourcePageController(this);
    }
    m_resourcePage->setup();
}

bool MainWindow::openResourceDetailPage(const QString &iconKey, const QString &kindKey,
                                        DetailSource from, const QString &instanceId,
                                        std::function<void(int)> onClosed) {
    if (m_resourcePage == nullptr) {
        m_resourcePage = new ResourcePageController(this);
    }
    return m_resourcePage->open(iconKey, kindKey, from, instanceId, onClosed);
}

void MainWindow::fillResourcePageGroups() {
    if (m_resourcePage != nullptr) {
        m_resourcePage->fillGroups();
    }
}

void MainWindow::onResourceDownloadFinished(const QString &label, bool success,
                                            const QString &errorMessage) {
    if (m_resourcePage != nullptr) {
        m_resourcePage->onDownloadFinished(label, success, errorMessage);
    }
}
QString MainWindow::projectTitleKey(ResourceCatalog::Source source, const QString &projectId) {
    // `Hit::iconCacheId()` 是那个拼法（`<来源>-<项目号>`）的**唯一出处** —— 别在这儿再拼
    // 一遍字符串：拼法一改，图标缓存、实例里那份身份表、这个缓存三处就各认各的了。
    ResourceCatalog::Hit hit;
    hit.projectId = projectId;
    hit.source = source;
    return hit.iconCacheId();
}

void MainWindow::rememberProjectTitles(const QList<ResourceCatalog::Hit> &hits,
                                       ResourceCatalog::Source source) {
    for (const ResourceCatalog::Hit &hit : hits) {
        if (hit.projectId.isEmpty() || hit.title.isEmpty()) {
            continue;
        }
        m_projectTitles.insert(projectTitleKey(source, hit.projectId), hit.title);
    }
}

void MainWindow::onInstallNameEdited() {
    m_installPage->noteNameEdited();
}

// ---------------------------------------------------------------------------
// 安装页：实例预设
// ---------------------------------------------------------------------------

void MainWindow::openInstallPresetPicker() {
    // 薄壳：「预设」那一行的选择、弹窗、摘要都在 `InstallPageController`（见那边）。
    m_installPage->openPresetPicker();
}


// ---------- 「设置」选项卡那一片接线 ----------
// `applySettingsToUi()` 与各颗 `on…Changed()` 在 `src/MainWindowSettings.cpp`（同一个类，另一个翻译单元）。

// ---------- 启动那条路（按钮 / 参数 / 启动后行为）与主页「最近玩的」 ----------
// 在 `src/MainWindowLaunch.cpp`（同一个类，另一个翻译单元）。


// ---------- 「账号与外观」那一扇（登录 / 切号 / 皮肤 / 披风）----------
// 在 `src/MainWindowAccount.cpp`（同一个类，另一个翻译单元）。


void MainWindow::onAddFolderClicked() {
    m_folderController->add();
}

void MainWindow::removeFolderFromList(const QString &folder) {
    m_folderController->removeFromList(folder);
}

void MainWindow::onFolderActionRequested(const QString &path, int action) {
    m_folderController->action(path, action);
}

void MainWindow::onFolderRefreshRequested(const QString &path) {
    m_folderController->refresh(path);
}

void MainWindow::onFolderRenameRequested(const QString &path) {
    m_folderController->rename(path);
}

void MainWindow::onFolderOpenRequested(const QString &path) {
    m_folderController->open(path);
}

void MainWindow::onFolderDeleteRequested(const QString &path) {
    m_folderController->removeFromDisk(path);
}

// ---------- 实例这一行能做的事（改名 / 固定 / 删除 / 两处 Notice 投影）----------
// 在 `src/MainWindowInstanceActions.cpp`（同一个类，另一个翻译单元）。


QString MainWindow::instanceKey(const QString &versionId) const {
    // 稳定 id / 目录名的取舍必须和实例设置写侧共用同一条规则。
    return InstanceSettingsController::instanceKeyFor(m_versionManager, versionId);
}

// ---------- 实例分组（组行 / 「分组设置」那一扇 / 成员与改名删除）----------
// 在 `src/MainWindowGroup.cpp`（同一个类，另一个翻译单元）。


namespace {

/// 「结构/原理图」那一页顶上那两排「来源」（★ ，见
/// `InstanceFilesPage::setStructureSources()`）。
///
/// 存档那一排和投影那一排并排，**上面一排是存档的、下面一排是投影的**。其中"**哪几个
/// 存档真有结构**"要读盘（结构方块存下来的东西落在
/// 存档自己的 `generated/` 底下），那件事由 `InstancePagesController::worldStructures()`
/// 算 —— 这一层只把那份事实摆成那一页要的形状。
///
/// 摆出来的规矩：
/// - **没有结构的存档一个都不列**；
/// - **没装投影模组就没有「投影」那一排**；
/// - 一个存档都没有、可投影装着 ⇒ 存档那一排照旧露着，里面是那**一行灰字**
/// （结构下拉打开时显示"无可用的结构"）；
/// - 两样都没有 ⇒ 返回**空表**，那一页只剩"这个实例还没有结构 / 原理图"那句提示
/// （不显示任何框，只保留没有结构的提示文本）。
///
/// NOTE: 两排的**排头**（「存档」/「投影」）走 `InstanceFilesPage` 上那三个静态函数 ——
/// 界面上写着的那几个字只有一个出处，这一层不自己抄一份（验证工程也读它们）。
/// 所以 `InstancePagesController` 那边给的是"哪几个存档有结构"（两张平行的表），
/// 排头是在这儿戴上去的。
[[maybe_unused]] QList<InstanceFilesPage::StructureSource> structureSourcesFor(
    const InstancePagesController::WorldStructures &worlds, bool litematica,
    const QString &schematicsDir) {
    QList<InstanceFilesPage::StructureSource> sources;
    if (!worlds.labels.isEmpty() || litematica) {
        // 一个存档都没有结构、可投影装着 ⇒ 这一排**露着但空着**（那行灰字在下拉框里，
        // 见 InstanceFilesPage::setStructureSources()）。
        InstanceFilesPage::StructureSource worldSource;
        worldSource.title = InstanceFilesPage::structureWorldTitle();
        worldSource.labels = worlds.labels;
        worldSource.paths = worlds.paths;
        sources.append(worldSource);
    }
    if (litematica) {
        InstanceFilesPage::StructureSource projections;
        projections.title = InstanceFilesPage::structureSchematicTitle();
        projections.labels.append(InstanceFilesPage::structureSchematicText());
        projections.paths.append(schematicsDir);
        sources.append(projections);
    }
    return sources;
}

}  // namespace

// ---------- 进实例设置 / 快速游玩（那一扇窗怎么开、开在哪一页）----------
// 在 `src/MainWindowInstanceEntry.cpp`（同一个类，另一个翻译单元）。


// ---------- 下载页的来路（从实例点「下载模组」进来，以及「← 返回实例设置」）----------
// 在 `src/MainWindowDownloadOrigin.cpp`（同一个类，另一个翻译单元）。


// ---------- 从实例文件页点开一个资源（详情页 / 那一行对应的资源页）----------
// 在 `src/MainWindowFileResource.cpp`（同一个类，另一个翻译单元）。


// ---------- 实例设置那一扇窗背后的页面数据与动作（配置 / 世界 / 模组 / 服务器列表）----------
// 在 `src/MainWindowInstanceSettingsWiring.cpp`（同一个类，另一个翻译单元）。


// ---------- 覆盖层那一层「现在能不能开新页」（让位 / 排到下一轮 / 弹一句）----------
// 在 `src/MainWindowOverlayGate.cpp`（同一个类，另一个翻译单元）。


// ---------- 实例文件与实例设置的读写（读一页 / 改一批 / 删 / 拷 / 落盘）----------
// 在 `src/MainWindowInstanceFiles.cpp`（同一个类，另一个翻译单元）。


// ---------- 日志 / 崩溃 / 导出（日志窗、错误报告、启动脚本、整合包）----------
// 在 `src/MainWindowSessionExport.cpp`（同一个类，另一个翻译单元）。


// ---------- 把实例里缺的东西补齐：修复文件 / 预取资源 ----------
// 在 `src/MainWindowRepair.cpp`（同一个类，另一个翻译单元）。


// ---------- 实例体检与备份 / 恢复（完整性检查报告、世界备份、实例快照）----------
// 在 `src/MainWindowIntegrityBackup.cpp`（同一个类，另一个翻译单元）。


// ---------- 这次启动用哪个 Java、给多少内存 ----------
// 在 `src/MainWindowLaunchJava.cpp`（同一个类，另一个翻译单元）。


// ---------- 左侧「游戏文件夹」那一列（选中 / 刷新 / 标题 / 下载复用来源）----------
// 在 `src/MainWindowFolderColumn.cpp`（同一个类，另一个翻译单元）。


// ---------- 游戏会话的几发事件（输出 / 结束 / 日志窗 / 停止）----------
// 在 `src/MainWindowSessionEvents.cpp`（同一个类，另一个翻译单元）。


// ---------- 导入整合包 / 把文件拖进来 ----------
// 在 `src/MainWindowDropImport.cpp`（同一个类，另一个翻译单元）。


void MainWindow::onVersionDetailsFetched(const QString &versionId, const QJsonObject &versionJson) {
    if (versionJson.isEmpty()) {
        appendLog(tr("获取版本 %1 的信息失败。").arg(versionId));
        // 账本上那件任务也到此为止（这一步还没进到"下载文件"，但用户点的那件事已经黄了）。
        // NOTE: 收尾也在 setDownloadUiBusy(false) **之前**，理由和
        // `DownloadCompletionController::onFinished` 那边逐字一样。
        m_downloadTaskLedger->finish(tr("无法获取版本 %1 的信息。").arg(versionId));
        setDownloadUiBusy(false);
        InlineMessage::warning(m_ui->overlay(), tr("下载失败"),
                             tr("无法获取版本 %1 的信息，请检查网络后重试。").arg(versionId));
        return;
    }

    appendLog(tr("已获取版本信息，正在下载游戏文件…"));
    const QString versionsPath = AppPaths::versionDir(versionId);
    // 这条槽是 VersionManager 的 versionDetailsFetched 唤起来的，而那条信号是在
    // DownloadManager 收完版本 json 的那个槽里同步发出来的 —— 直接在这里起下载批次，
    // 批次就整个跑在那条栈上。排到下一轮（见 VersionManager::deferNextStep 的 NOTE）。
    m_versionManager->deferNextStep([this, versionId, versionsPath] {
        m_versionManager->downloadVersion(versionId, versionsPath);
    });
}

