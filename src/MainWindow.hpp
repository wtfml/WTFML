/**
 * @file MainWindow.hpp
 * @brief 声明主启动器窗口及其UI交互。
 * @note 按钮事件与版本选择在此文件中统一管理。
 */

#pragma once

#include <QAtomicInt>
#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPair>
#include <QJsonObject>
#include <QMainWindow>
#include <QPixmap>
#include <functional>
#include <QPointer>
#include <QProcess>
#include <QSet>

#include "manage/PlayerSkinStore.hpp"
#include "auth/AccountStore.hpp"
#include "download/LoaderMeta.hpp"
#include "download/ModDependencies.hpp"
#include "download/ResourceCatalog.hpp"
#include "manage/ServerStatusCache.hpp"
#include "manage/ModpackImport.hpp"
#include "manage/InstanceIconLookup.hpp"
#include "launcher/QuickPlay.hpp"
#include "launcher/RunningProcesses.hpp"
#include "util/ServerPing.hpp"
#include "tasks/TaskManager.hpp"
#include "..\ui\TaskCenterPanel.hpp"
#include "..\ui\WindowMotion.hpp"
#include "..\ui\TaskDetailPage.hpp"
#include "..\ui\IntegrityReportPage.hpp"
#include "manage/VersionIntegrity.hpp"
namespace BackupFiles { enum class Kind; }
#include "diag/GameExitDiagnosis.hpp"
#include "diagnose/StartupDiagnosis.hpp"
#include "manage/LaunchHistory.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/LaunchHistoryController.hpp"
#include "controllers/InstanceSettingsController.hpp"
#include "controllers/DownloadFilterController.hpp"
#include "controllers/InstallOccupancy.hpp"
#include "controllers/FileOperationOccupancy.hpp"
#include "controllers/NavigationReturn.hpp"
#include "controllers/ResourcePageController.hpp"
#include "controllers/ServerListController.hpp"
#include "manage/InstanceFilter.hpp"
#include "..\ui\UpdateCenterPage.hpp"
#include "..\ui\NewServerPage.hpp"
#include "server/ServerInstallController.hpp"
#include "..\ui\ServerSettingsPage.hpp"
#include "server/ServerCheck.hpp"

#include "..\ui\MainWindowUI.hpp"
class QDragEnterEvent;  // 拖进来的文件（只用指针，所以前置声明够）
class QDropEvent;
class InstanceUpdateCheckController;
class ResourceSearchController;
class UpdateCenterController;
class ServerSettingsPageController;
class InstallPageController;
class ServerRunController;
class DownloadCompletionController;
class DownloadTaskLedgerController;
class ServerMenuController;
class ResourceInstallController;
class AgentController;
class AgentLauncherHostImpl;
class ServerSettingsController;
class ServerManagementActions;
class InstanceUpdateFlow;
class ThemeSettingsController;
class JavaSettingsController;
class PresetController;
class LauncherPreferencesController;
class CrashReportController;
class IntegrityController;
class ToolboxController;
class BackupController;
class ServerPingController;
class FolderController;
class ModpackExportController;
class InstanceFileController;
class InstanceTransferController;
class FileOperationRecoveryController;
class DownloadBatchController;
class InstanceDialogController;
class InstanceEditorController;
class ImportController;
class GameSessionController;
class LaunchController;
class ConnectionsController;
class DailyQuoteController;
class InstanceIconController;
class QTreeWidget;  // 下载页那几张资源表（只用指针和引用，所以前置声明够）
class QTreeWidgetItem;
class QNetworkAccessManager;
class QThread;  // 图标反查那一趟（见 m_instanceIconThread），同上只按指针持有
class QShortcut;
// onPlayerSkinChanged 收到的两张贴图。**只用 const 引用传**（图是
// PlayerSkinController 那边载好发过来的），所以前置声明够 —— 定义在 MainWindow.cpp 里
// （那边的 SkinModel 那条路要用）。
class QImage;
class LauncherManager;
class AuthManager;
class FolderManager;
class VersionManager;
// 下载页那几类资源的取数（src/download/ResourceStore.hpp）。同上，只按指针持有。
class ResourceStore;
class GlobalJavaSettings;
class LauncherSettings;
class InstanceGroupStore;
// 「分组…」那个对话框。这里只按 const 引用传，前置声明就够 —— 没必要为了一个参数类型
// 让每个 include 了 MainWindow.hpp 的地方都把 ui/ 拖进来。
class GroupDialog;
// 「实例设置…」那个对话框。同上，只按 const 引用传。
class InstanceSettingsDialog;
// 「账号与外观」那个对话框（多账号 + 换皮肤披风）。这里**只按指针**持有它，所以
// 前置声明就够 —— 它在 MainWindow 的构造函数之后才懒建（第一次点那个入口才建）。
class AccountSkinDialog;
// 「资源详情」那一整页（下载页点一个模组进去看的那一页）。同上：只按指针持有，
// 懒建（第一次点开才建）。
class ResourceDetailPage;
/// 任务详情页（`ui/TaskDetailPage.hpp`）。
class TaskDetailPage;
// 「Java 管理」那一整页（设置页那颗按钮开出来的四个版本）。同上：只按指针持有，
// 懒建（第一次点开才建），而且它**不长记性**（四行每次开页现灌）。
class JavaManageDialog;
class JavaRuntimeController;
class ActivityCenterController;
class ServerWorkspaceController;
class ServerConsoleController;
class TaskDetailController;
// 「游戏日志」窗（一份游戏一个，见 ui/GameLogWindow.hpp）。同上，只按指针持有，
// 而且是**按需**才建（收到第一行输出那一刻，见 showGameLogLine()）。
class GameLogWindow;
// 换皮肤披风那几条要令牌的接口（src/auth/SkinApi.hpp）。同上，只按指针持有。
class SkinApi;
// 玩家外观那一整套（挑哪一张 / 取回来 / 认领 / 记账 / 退回默认外观）——
// src/controllers/PlayerSkinController.hpp。同上，只按指针持有（它是 this 的子对象）。
// NOTE: 它的 `hintFacts()` 返回一个嵌套的聚合类型，而这里只**传 const 引用**（见
// updateSkinHint 的实现），所以前置声明够用 —— 但那个类型是按值返回的，用它的地方
// 必须 include 那边（只有 MainWindow.cpp 用）。
class PlayerSkinController;
class InstanceSettingsController;
// 「每实例设置」那两层的值类型（src/settings/EffectiveJavaSettings.hpp）。这里只按
// const 引用传，前置声明就够 —— 让每个 include 了 MainWindow.hpp 的地方都拖一份
// 值结构的定义没有意义。
struct EffectiveJavaSettings;
struct JavaSettingsOverride;
namespace ModpackExport {
enum class Format;
struct ExportPlan;
}  // namespace ModpackExport
// 一份主题规格（`ui/Theme.hpp`）。`applyThemeSpec()` 只按 const 引用收，
// 前置声明就够 —— 不必让每个 include 了 MainWindow.hpp 的地方都拖一份规格的定义
// （那份定义里还挂着 QHash/QPalette 一串）。
namespace Theme {
struct Spec;
}  // namespace Theme

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// 这一窗里那个界面对象（`MainWindowUI`，不是 QObject，findChild 找不着它）。
    ///
    /// **给验证工程摆界面状态用**（ui_shot 那一类要把底栏摆成某个样子截图：它没有真会话，
    /// 摆不出"有实例在跑"这个前提，只能直接调 `setRunPanelAvailable()` 之类）。
    /// 产品代码里**不要拿它抄近路** —— 那个类的方法本来就是给 MainWindow 调的，但绕开
    /// 这里去直接摆状态，等于把"谁说了算"挪到一个不该有决定权的地方。
    MainWindowUI *ui() const { return m_ui; }
    /// 智能体控制器（Stage 1A：无工具对话）。验证工程用它注入假模型客户端。
    AgentController *agentController() const { return m_agentController; }

    /**
     * @brief **给验证工程摆现场用**：把"上一次点启动是什么时刻"直接摆成某个值。
     * @param identity 身份键（`launchIdentityKey()` 那个规矩：正版是 uuid、离线是账号名）。
     * @param versionId 实例（= versions/ 下面那个目录名）。
     * @param ms 毫秒时间戳（`QDateTime::currentMSecsSinceEpoch()`）；**0 或负数 = 抹掉这一格**。
     *
     * 「十秒内重复启动」那个窗口只有两种量法：真等十秒（验证工程每一跑多花十秒），或者把
     * 这一格的输入直接摆出来。判据的**全部**输入就是这一个时刻，所以摆它 == 摆现场；而
     * "窗口外那一侧"和"时钟被往回拨"这两档，只有摆得出来才验得到（等是等不出来的）。
     *
     * NOTE: 产品代码里**不要**调它 —— 真正的写点只有 confirmLaunchWhileRunning() 末尾
     * 那一处（写在"两条都放行了之后"，见那里的说明）。
     */
    void seedLastLaunchAttemptForTest(const QString &identity, const QString &versionId, qint64 ms) {
        if (ms <= 0) {
            m_lastLaunchAttempts[identity].remove(versionId);
            return;
        }
        m_lastLaunchAttempts[identity][versionId] = ms;
    }

    /**
     * @brief **给验证工程量"忙"用**：这一份实例现在在不在"谁都不许启动 / 不许删 /
     * 不许再开一份搬文件"的名单上。
     *
     * 后台复制、模组迁移、升级副本都会把被碰到的实例记进 `installingVersionIds()`
     * （那是唯一出处）。界面上看得见的表现是"开设置页被挡下来"，但**收尾有没有把标记
     * 摘干净**只有问这一格才知道 —— 靠"再开一次设置页"去间接猜会弹一条提示，
     * 和别的收尾消息撞在一起时还会被 OverlayHost 丢掉（提示不叠提示）。
     *
     * NOTE: 产品代码里**不要**调它（同上面那几个 seed*ForTest）。
     */
    bool instanceBusyForTest(const QString &versionId) const {
        return installingVersionIds().contains(versionId);
    }

    /// **给验证工程用**：任务中心那一项此刻翻成界面样子是什么（只读）。
    /// 和 `taskCenterItems()` 同一个翻译点，所以验的就是产品真正会画的那一份。
    QList<TaskCenter::Item> taskCenterItemsForTest() { return taskCenterItems(); }

    /**
     * @brief **给验证工程用**：直接压 / 放一次"这一份实例正在搬文件"的计数。
     *
     * 这一笔是**引用计数**（同一份实例上可以叠两趟作业，先收尾的那一趟
     * 不许把另一趟的标记一起摘掉）。验证工程要能造出"叠了两趟、收掉一趟"这个现场，
     * 否则那个洞只能靠读代码相信。
     *
     * NOTE: 产品代码里**不要**调它。
     */
    void markFileOperationBusyForTest(const QString &versionId) { markFileOperationBusy(versionId); }
    void releaseFileOperationBusyForTest(const QString &versionId) {
        releaseFileOperationBusy(versionId);
    }

    /// **给验证工程用**：走真实的文件页启用 / 禁用入口（宿主闸也一起走）。
    bool toggleInstanceFileForTest(const QString &path, QString *problem);

    /**
     * @brief **给验证工程摆现场用**：只压 `m_downloadInFlight` 这一位（不碰界面、不记账）。
     *
     * 「下载还没完的时候拖一个整合包进来」那条路没有别的入口能造出来（按钮灰着，而拖放
     * 不经过按钮）—— 用 `setDownloadUiBusy()` 摆会顺手在账本上开一项下载任务、还会灰掉
     * 一整排控件，把别的断言一起搅了。这里只要那一位。
     *
     * NOTE: 产品代码里**不要**调它。
     */
    void seedDownloadInFlightForTest(bool busy) { m_downloadInFlight = busy; }

    /**
     * @brief **给验证工程摆现场用**：把某一号会话的"启动事实"直接摆出来。
     *
     * 那一份启动事实（`GameSessionController::m_sessionLaunchFacts`）正常只有一条写点
     * （`LaunchController` 里拿到会话号之后那一趟 `beginSession()`），
     * 而验证工程验不了那一段 —— 它要真的起一个游戏进程。可"异常退出那一项说了什么"完全由
     * 这一份事实决定，摆它 == 摆现场：摆上"要求 Java 21 / 实际用 17 / Xmx 比物理内存还大"，
     * 那三条提示就该一条不差地出现。
     *
     * NOTE: 产品代码里**不要**调它。
     */
    void seedSessionFactsForTest(int sessionId, const QString &versionId, const QString &displayName,
                                 int requiredJavaMajor, const QString &javaPath, int xmxMb,
                                 int xmsMb, const QString &loaderName,
                                 const QString &loaderVersion) {
        m_gameSessionController->seedSessionFacts(sessionId, versionId, displayName,
                                                  requiredJavaMajor, javaPath, xmxMb, xmsMb,
                                                  loaderName, loaderVersion);
    }

    /// 给验证工程摆现场：这一号会话是用户从启动器里主动停止的。
    void markGameStopRequestedForTest(int sessionId) {
        m_gameSessionController->markUserStoppedForTest(sessionId);
    }

    /// 取消**一件启动任务**（任务中心那颗「取消」与主页启动卡片（#65）的同一个入口）：
    /// 进程已经在跑就停那一份会话，还没起来就让启动流程在下一个检查点停下。
    /// 返回"真的做了点什么"（`cancelWouldWork()` 为假时什么都不做）。
    bool cancelLaunch(const Tasks::TaskSnapshot &task);

    /// 测试用：任务中心那一颗「取消」此刻会不会做点什么（产品里由详情页按它画按钮，
    /// 判据就是 `cancelWouldWork()`）。
    ///
    /// NOTE: 产品代码里**不要**调它。
    bool taskCancelWouldWorkForTest(const Tasks::TaskSnapshot &task) const {
        return cancelWouldWork(task);
    }

    /**
     * @brief **给验证工程摆现场用**：把"刚刚成功启动了一次"这件事直接记进「上一次启动」
     * 那本账。
     *
     * 产品那一侧的唯一写点在 `onStartButtonClicked()` 里"会话号 > 0"那一段 —— 真起一个
     * 游戏进程是验证工程做不到的事，而主页那张卡上会写出什么**完全由账上那一条决定**
     * （哪一份、什么时候、进的是哪儿、结局如何），所以摆账 == 摆现场。
     *
     * 摆进去的字节走的是**产品同一条写路**（同一个 `appendLaunchRecord()`、同一个落盘），
     * 不是测试另抄一份 —— 否则这一节验的就只是测试自己。
     *
     * @param versionId 实例 id（= versions/ 下面那个目录名）。
     * @param startedAtMs 启动时刻（毫秒）；**照产品记的是"进程真的起来"那一刻**。
     *
     * NOTE: 产品代码里**不要**调它。
     */
    void seedLaunchRecordForTest(const QString &versionId, qint64 startedAtMs) {
        appendLaunchRecord(versionId, QuickPlay::Target(), startedAtMs);
    }
    /// 同上，但这一趟是**快捷启动**（走 quick play 直接进某个世界 / 某台服务器）。
    void seedQuickPlayRecordForTest(const QString &versionId, QuickPlay::Kind kind,
                                    const QString &value, qint64 startedAtMs) {
        QuickPlay::Target target;
        target.kind = kind;
        target.value = value;
        appendLaunchRecord(versionId, target, startedAtMs);
    }
    /**
     * @brief **给验证工程摆现场用**：把账上那一条的结局改写成"这一趟结束了"。
     *
     * @param startedAtMs 启动时刻 —— 账上那一条就是靠它认的（产品那一侧也只有这一个键
     * 能从"退出那一刻"追回"启动那一刻"记的那一条）。
     * @param crashed 异常退出（`QProcess::CrashExit` 或非 0 退出码，判据是
     * `GameExitDiagnosis::isAbnormal()`）。
     * @param neverStarted 游戏**连第一行日志都没打出来**就结束了 ⇒ 这一趟不算"玩过"
     * （`Result::StartFailed`）。产品的判据是"退出时它还在那一层的 `m_launchingSessions`
     * 里"，验证工程摆不出真进程，所以这一位直接由调用方给。
     */
    void seedLaunchFinishForTest(qint64 startedAtMs, bool crashed, bool neverStarted) {
        m_launchHistory->recordFinish(startedAtMs,
                                      LaunchHistoryController::resultFor(crashed, neverStarted),
                                      QDateTime::currentMSecsSinceEpoch());
    }

    /**
     * @brief **给验证工程摆现场用**：把某一趟「检查更新」的结果直接摆出来。
     *
     * 产品那一侧只在一个地方写这份名单（`finishUpdateCheck()` 里那一句 insert），而走到
     * 那里要先跑一整趟反查 + 版本列表（真网络）。实例页的筛选**只读这一份名单**（键在
     * 不在 = 查过没有，见 m_instanceOutdatedPaths 那段），所以摆它 == 摆现场。
     *
     * NOTE: 只摆**名单**那一位（`m_instanceOutdatedPaths`）—— 和它一起写的
     * `m_instanceUpdateTargets`（"换成哪一版"）是给"点更新"那条路用的，实例页的
     * 筛子不读它，所以这里不去编一份假的。摆完顺手重灌一次树，那颗 chip 当场生效。
     * NOTE: 产品代码里**不要**调它。
     *
     * @param versionId 实例 id。
     * @param paths 查出来可更新的那些路径（**绝对路径**，禁用着的那条带 `.disabled`）；
     * 给空表 = "查过了、一条都没有"（和"没查过"是两句话）。
     */
    void seedOutdatedPathsForTest(const QString &versionId, const QStringList &paths) {
        m_instanceOutdatedPaths.insert(versionId, paths);
        rebuildInstanceTree();
    }
private slots:
    void onStartButtonClicked();
    // 登录三态（AuthManager 的三个信号）。登录是异步的：点完按钮到浏览器回调回来之间
    // 可能有几十秒（用户还得输账号密码），所以这三条都得在界面上有反应 ——
    // 尤其 loginFailed：不接它，失败只在浏览器那个页面里看得见。
    void onLoginStarted();
    void onLoginSucceeded(const QString &playerName);
    void onLoginFailed(const QString &errorMessage);

    // ---------- 「账号与外观」这个模块（主页右栏那个入口 → AccountSkinDialog） ----------
    // 入口按钮：第一次点会把这个窗建出来（懒建，没点过就不占内存），然后刷新一遍再显示。
    void onAccountSkinButtonClicked();
    // 用户在这个窗里选了「添加账号」→「微软账号」。走的是和设置页那个登录按钮**同一条路**
    // （AuthManager::loginMicrosoft()），只是入口在另一个窗上。
    void onAccountLoginRequested();
    // 用户在那个窗里填了个离线名字。合法性（空 / 有空白 / 太长）由
    // AccountStore::addOffline() 判（唯一出处），失败按产品约定弹窗。
    void onOfflineAccountRequested(const QString &name);
    void onAccountSwitchRequested(const QString &uuid);
    void onAccountRemoveRequested(const QString &uuid);
    // 用户选好了一张皮肤图。**尺寸那条规矩在这条路上判**（SkinApi::isAcceptableSkinSize，
    // 唯一出处），失败按产品约定弹窗。
    void onSkinUploadRequested(const QString &path, const QString &variant);
    // 离线账号自己挑的那张（「账号与外观」→「换一张…」）：只存本机、不联网、不要令牌。
    // 尺寸仍走 SkinApi::isAcceptableSkinSize()，失败弹窗。
    void onOfflineSkinPickRequested(const QString &path, const QString &variant);
    // 撤掉自己挑的那张，回到算出来的 / 内置 Steve 那一档。
    void onOfflineSkinResetRequested();
    void onCapeSelectRequested(const QString &capeId);
    void onCapeHideRequested();

    // ---------- 主页右栏那个小人 + 底栏头像 ----------
    //
    // **挑图 / 取图 / 认领 / 记账 / 退回默认外观**那一整套（连带那九个成员）
    // 在 `controllers/PlayerSkinController` —— 四个真决策（挑哪一张、回来的是不是
    // 刚才那一位、什么时候才记账、取不到怎么降级）见那边的文件头。主窗口这边只有
    // 三件事：把图交给那几个控件（onPlayerSkinChanged）、按事实挑一句话（updateSkinHint）、
    // 把"要重走一遍"转给那边（startPlayerSkinLoad）。**文案只在这一层**（那一层不认识
    // 中文，也不许认识 `ui/`）。
    //
    // NOTE: 下面那三个**薄壳槽**是给验证工程留的 —— mw_check 里 X 节和 AF5/AF6/AF7 就是按
    // 名字直接打它们来演"档案到了 / 贴图到了 / 挑好的那张回来了"（QMetaObject::
    // invokeMethod）。那是**全程不联网**验完整条外观路径的唯一办法，所以名字必须是
    // 这几个。逻辑一行都不在这儿，整份在 controller 里。
    // / 官方回的档案到了（转 `PlayerSkinController::onProfileReady`）。
    void onSkinProfileReady(const QString &uuid, const PlayerSkinProfile &profile);
    /// 一张贴图在本地就绪了（转 `PlayerSkinController::onTextureReady`）。
    void onSkinTextureReady(const QString &url, const QString &path);

    /// 外观那条路说"现在该画这一套了"（`PlayerSkinController::skinChanged`）。
    ///
    /// 收到的两张图一定是**画得出来的**（载不进来时那一层已经退回默认外观了），所以这里
    /// 只管摆，顺序固定：
    /// 右栏那个小人 → 底栏头像图（算一次存起来）→ 那个窗里的 3D 预览 → 右栏那句话 → 底栏。
    /// @param cape 空图 = 这一套没有披风（常态）。
    /// @param slim 纤细模型（手臂 3 宽）。
    void onPlayerSkinChanged(const QImage &skin, const QImage &cape, bool slim);

    /// 把一份外观贴到界面上。**这一层只负责转手** —— 真正的判断（载不进来要不要退回
    /// 默认外观）在 controller 的 `applyPlayerSkin()` 里。
    /// @param skinPath 空串 = 用默认外观。
    ///
    /// NOTE: 名字留着、而且是**槽**，是因为验证工程靠 QMetaObject::invokeMethod 驱动它
    /// （沙盒里现写两张 PNG 喂进来），不联网就能验"喂一份外观，三个窗口跟着走"。
    void applyPlayerSkin(const QString &skinPath, const QString &capePath, bool slim);

    /**
     * @brief 账号的头像：底栏账号面板里某一行的行首头像（24×24，就是 `bottomBarPanelIconSize()`），
     * 账号页每一行也用它（传大一点的 `pixelSize`）。
     * @param account 那一行是谁（用 `uuid` 认人 —— 名字可以重复）。
     * @param pixelSize 边长；0 = 面板那一档。
     * @return 一张一定有东西的图；实在什么都没有时是**自己画的首字母方块**。
     *
     * 选择 / 登录过的账号磁盘上一定有缓存，所以这条路**只认缓存**，
     * 联网 / 换令牌 / 读客户端 JAR 一个都不做，取不到就自己画（那不算出错）。
     *
     * 四个来源，按优先级：
     * ① 当前账号 —— 底栏那颗头像现成的（`m_avatarPixmap`），缩一下；
     * ② 正版账号 —— 缓存里那份档案的 `skinUrl` 对应的贴图（PlayerSkinStore 的两级缓存）；
     * ③ 离线账号 —— 他自己挑的本地皮肤（`OfflineSkin`），其次是 `DefaultSkin`
     * **算出来的**那张官方默认皮肤的缓存图（**只拼路径探测，
     * 绝不走 resolve()**：那一条要起 PowerShell 读 JAR，几百毫秒，不能挂在开面板上）；
     * ④ 都没有 —— `SkinModel::letterAvatar()` 画的首字母方块。
     *
     * NOTE: 每次开面板都要给**全部**账号各算一张（面板里只显示三个，但哪三个是那个
     * 函数按 `saved_at` 排出来的，不在这里再排一遍）。代价是几次本地文件探测，
     * 可以接受；**换来的好处**是这条路永远不会"卡在网络上"。
     */
    QPixmap accountFacePixmap(const SavedAccount &account, int pixelSize = 0) const;

    /// 猜"某个离线账号当初是按哪个实例算的官方默认皮肤"（见 accountFacePixmap() ③）。
    /// 这一位在 `PlayerSkinController` 那边（它要读"上一轮真的算过哪个实例"）；
    /// 这里只剩上面那个 ④ 在向它要几个候选版本号。
    void onAddFolderClicked();
    /// 改名的**执行部分**：校验名字 → 改磁盘 → 同步状态（LastLaunched / 选中项）→ 刷界面。
    /// 失败时弹窗并返回 false。
    ///
    /// **判断和那些状态在 `InstanceSettingsController::rename()`**，这里
    /// 只剩一个转发。留着这一层（而不是把调用点全改成问 controller）是因为验证工程按
    /// **名字**驱动它（`QMetaObject::invokeMethod("applyRename")`，见 mw_check 的 S 段）——
    /// 那是"这个槽驱动得起来"这条判据本身，不是实现细节。
    ///
    /// NOTE: 和"问新名字"那一步分成两步：这一半里藏着最容易
    /// 漏的那件事：改完名要同步 Version/LastLaunched，不然用户下次打开启动器，选中项
    /// 会空着，他会以为实例丢了。那条只有真的跑一遍才验得出来。
    /// 问名字那一格是能按 objectName 找得到、能真的打字的 `QLineEdit`
    /// （见 InlineMessage.hpp）；`QFileDialog` 那种系统对话框驱动不了。
    bool applyRename(const QString &oldId, const QString &newId);
    /// 把 controller 报上来的一件事（`InstanceSettingsController::Notice`）说给用户听。
    ///
    /// **中文只在这一层**：controller 那边只有"哪一档、是谁、数据层说的原因是什么"这几格
    /// 事实，标题与正文在这里按 `kind` 挑。这样那一层能被验证工程直接驱动（不需要一个界面
    /// 就能验"改名拦在哪一道"），而"怎么说"仍然只有一个出处。
    void showInstanceSettingsNotice(const InstanceSettingsController::Notice &notice);
    /// 把 `ServerListController` 报上来的一件事说给用户听（上面那条的**第二个实例**）。
    ///
    /// 同款边界：那一层只有"哪一档、数据层说的原因是什么"，标题与正文在这一层挑。
    /// 注意它是**局部对象**（不是成员）—— 见 `openInstanceSettingsNow()` 里那一段：
    /// 它那两条回调要用栈上的 `dialog` 和 `targetId`，所以只能活在那一趟里。
    void showServerListNotice(const ServerListController::Notice &notice);
    /// 「分组设置…」：管**分组本身**（新建 / 重命名 / 删除 / 上下移 / 默认展开 / 恢复默认）。
    ///
    /// NOTE: 它**不读选中的实例**（分组设置不针对单个实例）—— 从哪儿进来都是同一张表，
    /// 没选中实例也能开。把实例放进某个组
    /// 是**组行行尾那颗齿轮**的事（见 onGroupActionRequested()）。
    ///
    /// 不分组也能用 —— 对话框里那份工作副本只有在点「确定」之后才由 applyGroupList()
    /// 落盘，取消的话磁盘一个字节都不动。
    void onGroupInstanceClicked();
    /// 分组的**执行部分**：把对话框里摆好的那份组清单落到磁盘上。失败时弹窗并返回 false。
    ///
    /// NOTE: 拆成两步的理由和 applyRename() 一样 —— QDialog::exec() 会阻塞在事件循环里，
    /// 验证工程驱动不了它。而这一半里藏着真会错的东西（组清单和文件里那份怎么比、
    /// 写失败了怎么说），必须验得了。
    /// NOTE: 过的是**指针**不是引用：验证工程按名字驱动它（QMetaObject::invokeMethod +
    /// Q_ARG），而 Q_ARG 要的是一份可拷贝的值 —— QDialog 不可拷贝。调用方那个对象
    /// 活得好好的（栈上那个 dialog），指针只是"这一半不接管它的生命周期"。
    bool applyGroupList(const GroupDialog *dialog);
    /// 组行行尾那颗齿轮里某一项被点了。@param groupId 哪一组（虚拟的「未分组」是空串）。
    /// @param action MainWindowUI::GroupAction 那一套里的某一项。
    ///
    /// NOTE: 这几项**都是立刻生效**的（不等谁按确定）：齿轮菜单不是对话框，它是"点一下
    /// 就办事"的一列动作。要问一句的那两项（重命名、删除）自己弹自己的框。
    void onGroupActionRequested(const QString &groupId, int action);
    /// 把某一组里的实例**都移出分组**（齿轮菜单里「把这一组里的实例都移出分组」）。
    /// 组留着、实例一个都不删，它们只是变成未分组。
    /// @param groupName 那一组的名字（只用来写日志和弹窗 —— 名字得是**菜单弹出来那一刻**
    /// 显示的那个，所以由调用方带着，这里不再查一遍）。
    ///
    /// NOTE: 它动的是"树上挂在这一组底下的那些"实例 —— 那就是「这一组里的东西」在树上的含义。
    /// 每个实例都要**明确写一条"未分组"**：不写的话没表过态的那些下一次刷新又被
    /// 自动归类拉回来（理由写在实现里那段）。
    void clearGroupMembers(const QString &groupId, const QString &groupName);
    /// 把当前选中的那个实例的**老家**改成 @p groupId 这一组（空串 = 未分组）。
    /// 挑目标那个框的回调。
    ///
    /// 取值口径和另外两颗实例级按钮一样：**下拉框**说了算（它就是"当前选中的那个实例"），
    /// 不是树的 currentItem()（点一下组头它也会变）。
    /// @return 写成功了返回 true（失败弹窗并返回 false）。
    bool setSelectedInstanceHomeGroup(const QString &groupId);
    /// 把一个**指定**的实例的**老家**改成 @p groupId 这一组（空串 = 未分组）。
    ///
    /// NOTE: 拆出来的理由是实例设置对话框那条路 —— 它手上是**那个实例的 id**，而"当前选中的
    /// 是哪一个"（上面那个函数的口径）在对话框开着的时候完全可能已经被改过了。
    /// 两条路必须写同一份归属逻辑，不然"从菜单里移"和"从设置里移"迟早会不一样。
    ///
    /// NOTE: 【为什么叫「老家」而不是「移入分组」】「在不在置顶组里」和「归在哪一组」
    /// 是分开的两格：「显示在置顶组中」由勾选框管，这个函数**一个字都不动它**。
    /// 所以置顶着的实例被「移进 Y」之后仍然置着顶、仍然在置顶组里多摆一条，
    /// 只是老家换成了 Y —— 这就是"两边都显示"。
    bool setInstanceHomeGroup(const QString &versionId, const QString &groupId);
    /// 把某一组的组名改掉（齿轮菜单里「重命名这一组…」）。
    /// @return 改成功了返回 true。
    bool renameGroupTo(const QString &groupId, const QString &newName);
    /// 删掉某一组（齿轮菜单里「删除这一组…」）。组里的实例一个都不删。
    /// @return 删掉了返回 true。
    bool deleteGroupById(const QString &groupId);
    /// 把某一组的「展开还是收着」记下来（点组行那一格、或者齿轮菜单里那一项）。
    ///
    /// NOTE: 它写盘但**不发 groupsChanged**（理由见 InstanceGroupStore::setGroupExpanded()）
    /// —— 所以调用方要自己把那一条的展开状态摆好，这里只管记。
    void rememberGroupExpanded(const QString &groupId, bool expanded);
    /// 给某一组弹齿轮菜单（锚点 = 那一行行尾那颗齿轮占的格子）。
    ///
    /// NOTE: 它和 onGroupActionRequested() 之间过的是 MainWindowUI::GroupRow，而那个类型
    /// 在 MainWindowUI.hpp 里 —— 本文件**不 include 它**（见文件头那几条前置声明的
    /// 说明），所以"翻快照"那一步是 .cpp 里的一个文件级自由函数
    /// （groupRowsForMenu()），不在这里出现。
    void openGroupMenuFor(QTreeWidgetItem *item);
    /// 实例设置那一页的**入口**：给某一个实例打开它。进来的每一处都带着一个具体的
    /// instanceId —— 点实例页那一行（任意处）、那一行行尾那颗常显的齿轮、底栏左面板那行
    /// 的齿轮，三条路都走它。
    ///
    /// NOTE: 所有入口共用这一条（别在入口里另写一遍对话框）—— 里面那段"已经写了的覆盖
    /// 读回来当起点、当前全局是什么"的口径只有一份；**改名也在这条路上**：页面上改
    /// 「实例名」那一格、按确定之后先 applyRename() 再写设置，顺序的理由写在实现里。
    ///
    /// @param pageKey 开出来之后**停在哪一页**（空 = 不指定，停它自己的第一页「概览」）。
    /// 只有那两条"从别处回来"的路会传（见 `openFileResourcePage()` /
    /// `returnToInstanceSettingsFromDownload()`）—— 从模组点进去，返回
    /// 也在模组而非概览，其余同理。从实例列表**新**点开一个实例的那几处一律不传：
    /// 那不是"回来"，用户没在等哪一页。
    ///
    /// 这一页**永远归「实例」那一颗选项卡**（`OverlayHost::showPage()` 的 `ownerTab`），
    /// 开之前先把选项卡切过去。不钉的话，从下载页那颗「← 返回实例设置」进来时当前选项卡
    /// 是**下载**，这一页会被记成下载页的页面 —— 用户点「实例」看见的是实例列表、那一页
    /// 收着看不见，而它占着覆盖层那一位 ⇒ 此后每次开实例设置都被挡下，而且**每次都被
    /// 拽回下载页**（症状："卡住进不去实例设置、还老变到下载页"）。
    void openInstanceSettings(const QString &versionId, const QString &pageKey = QString());

    /// 「加入选中的世界 / 加入服务器」那两颗按钮按下去之后走的那一条。
    ///
    /// 世界和服务器各做一个临时占位（加入选中的世界仅限有这个参数的版本）—— 它不是
    /// "说一句还没做"的占位，而是**真的把游戏按这个参数开起来**，走的就是点「启动」那一条路。
    ///
    /// 这一半只做两件事：问一句"这个版本认不认"（`QuickPlay::supports()`，界面上那两颗
    /// 按钮在支持的版本上才露，这是兜底），然后**让位 + 排到下一轮**再开
    /// （`runWhenOverlayFree()`）—— 调用点在这个信号里，实例设置那一页正卡在它自己的嵌套
    /// 事件循环里，当场去启动等于"页还开着就开始下载/挑 Java"。
    ///
    /// @param versionId 哪一个实例（按钮所在那一页的实例）。
    /// @param kind 进存档还是连服务器。
    /// @param value Singleplayer 给**存档目录名**（不是路径）；Multiplayer 给服务器地址。
    void startQuickPlay(const QString &versionId, QuickPlay::Kind kind, const QString &value);
    /// 「这个实例（版本）认不认这条 quick play 参数」。**唯一判据出处**（`QuickPlay::supports()`
    /// 是它唯一读的那份逻辑）。
    ///
    /// Why 要收成一个函数：同一句话有两个读者 —— ① `startQuickPlay()` 的第一道闸；
    /// ② 界面上那两颗「加入」按钮**露不露**（见 `refreshInstanceDirs()` 里那两句
    /// `setServerJoinText()` / `setJoinText()`）。两处各读一遍版本 json 的话，判据迟早会
    /// 漂：按钮露着而 `startQuickPlay()` 说不支持（点了弹一句"用不了"），或者反过来
    /// （明明能用，按钮却藏在页面里）。
    ///
    /// NOTE: 读的是**合并过的** json（`getVersionJson()`）而不是加载器自己那一份 —— 快速游玩
    /// 那几条是**原版**的 `arguments.game` 条目，Forge / NeoForge 的 profile 里根本没有，
    /// 靠继承才看得到（见 QuickPlay.hpp 那段）。
    bool instanceSupportsQuickPlay(const QString &versionId, QuickPlay::Kind kind) const;
    /// `startQuickPlay()` 让位之后真正跑的那一半（分成两个函数的理由见
    /// `runWhenOverlayFree()` 那段 NOTE）。它把 `m_pendingQuickPlay` 排上、把「已安装版本」
    /// 下拉框切到 @p versionId，然后**调点「启动」那一条路**（`onStartButtonClicked()`）——
    /// 令牌 / 挑 Java / 启动提醒那几道闸因此一道都不绕过。
    void startQuickPlayNow(const QString &versionId, const QuickPlay::Target &target);
    /// 「导出启动脚本」：把"按这个实例现在的这套参数启动"写成**一个 .bat**。
    ///
    /// 用户选保存位置，出 .bat。问路径那一半在对话框里
    /// （那一步开的是 Windows 自己的保存框，见 ui/InstanceSettingsDialog.cpp 里那个调用点），
    /// **这一半是"拿一条路径真的去写"** —— 收两个字符串，所以验证工程可以直接
    /// `invokeMethod` 进来喂一条临时路径，不用去驱动那个保存框。
    ///
    /// @param versionId 实例 id（= 目录名）。
    /// @param path 用户挑好的完整路径（带 .bat）。
    ///
    /// NOTE: 脚本的内容必须和**真正启动那一次**逐字一致（两边调的是同一个
    /// launchArguments()，见 MainWindow.cpp 里那两处调用点），
    /// 否则"导出来的能跑"这句话就没有依据。唯一**故意**不同的地方是程序名那一格：
    /// 那边由 LauncherManager 从 PATH 兜底，这边写的是同一个兜底值（裸 `java`）。
    void exportLaunchScript(const QString &versionId, const QString &path);
    /**
     * @brief 「导出…」：把这个实例按**整合包规范**打包成一个能直接导入的文件。
     *
     * 和上面那颗 `exportLaunchScript` **不是一件事**（名字像，做的是两件事：那一颗出的是
     * .bat，这一颗出的是整合包）。同一条切法：问路径在对话框里（Windows 自己的保存框），
     * 这一半收一条路径把事情做完 —— 所以验证工程能直接喂一条临时路径把整趟跑完。
     *
     * 格式**从扩展名读**（`.mrpack` = Modrinth / `.zip` = CurseForge），不做第三档参数：
     * 那正是对话框里那两个筛选项已经在问的东西（见那个信号的 NOTE）。
     *
     * 这一趟要枚举实例目录、算哈希、联网反查、再打一个 zip。打包阶段放在后台线程，
     * 进度和结果仍回到主线程展示，不另开独立进度窗口。
     *
     * @param versionId 实例 id（= 目录名）。
     * @param path 用户挑好的完整路径（带 `.mrpack` 或 `.zip`）。
     */
    void exportModpack(const QString &versionId, const QString &path);
    /// 「补全文件」：校验这个实例的文件，缺哪个补哪个。
    ///
    /// 干活的在 VersionManager::repairVersionFiles()，这里管的是"该不该开始、开始之后
    /// 界面成什么样"：三道守卫（这个实例自己在忙 / 别的批次在跑 / 继承链不完整），
    /// 置「补全」那一档占用（`InstallOccupancy`）+ 界面忙态，收尾在
    /// DownloadCompletionController::onFinished() 里。
    void repairInstanceFiles(const QString &versionId);
    /// 导入成功后给这个实例预取 GTNH 首启资源（MCLib AssetDirector）。
    ///
    /// 什么都不做的四种情况：认不出消费者模组（绝大多数实例）、已经全在盘上、体积超上限、
    /// 另一个批次正在跑。真开一批时就置「预取」那一档占用 + 界面忙态，
    /// 收尾在 DownloadCompletionController::onFinished() 里（它也会给失败那颗「再试一次」）。
    ///
    /// 为什么挂在"导入成功之后"而不是启动前：启动那条路是同步的（里面还有嵌套事件循环），
    /// 往里塞一个几十 MB 的下载会把取消、关窗、重复点击那几道闸全部重排；而导入刚结束时
    /// 正好有一件已完成的父任务作参照，界面也能直接说"这一批在干嘛"。
    void maybePrefetchAssetDirector(const QString &versionId);

    // ------------------------------------------------------------------
    // 完整性检查与修复
    // ------------------------------------------------------------------
    // 和上面那颗「补全文件」是同一件事的两半：那一颗缺什么补什么（直接开一批下载），这几
    // 个先**体检**（只算不装，不联网、不改盘），扫完把结论摊在内容区一整页上，页上那颗
    // 「补齐缺失或损坏文件」再回到 repairInstanceFiles() 那条现成的补全路。
    //
    // 扫描在**后台线程**里跑（`QThread::create`）：清单在 UI 线程上取好（快），那几千条
    // 比对（含几十兆 jar 的哈希）交给 worker，进度按 `invokeMethod` 排回主线程记进任务账本。
    // 于是**扫描期间界面是活的**（扫描期间允许浏览别的页面）。

    /// 用户点了概览栏那颗「检查并修复」：起一趟体检（`TaskKind::Integrity` 那一项）。
    ///
    /// 三道守卫（和 repairInstanceFiles 同一套）：这个实例自己在忙 / 别的批次在跑 / 继承链
    /// 不完整；再加一道"这个实例已经在扫了"。
    void submitIntegrityCheck(const QString &versionId);
    /// 这个实例是不是正在被体检（删除 / 改名那两处要按它拦一下：同一实例不允许同时
    /// 扫描和删除/改名）。
    bool integrityScanningVersion(const QString &versionId) const;
    void openIntegrityReportPage(const QString &versionId);
    /// 把 `VersionIntegrity::Report` 翻成报告页认的那份形状（**唯一翻译点**，和
    /// `taskCenterItem()` 翻任务账本是同一个路数）。
    IntegrityReportPage::Report integrityReportForPage(const VersionIntegrity::Report &report,
                                                       const QString &instanceName) const;
    void onIntegrityReportAction(IntegrityReportPage::Action action);
    /// 把这一份体检结论拼成纯文本放进剪贴板（走 `VersionIntegrity::plainReport()`）。
    void copyIntegrityReport();
    /// 后台那一趟体检报上来的进度（**一定在 UI 线程上**，worker 走 invokeMethod 排过来的）。
    /// `groupIndex` 是"正在比哪一组"（0..3，见 `VersionIntegrity::allGroups()`）。
    void onIntegrityScanProgress(int checked, int total, int groupIndex);
    void onIntegrityScanFinished(const VersionIntegrity::Report &report);

    // 世界与实例备份的状态、线程与结果处理均由 BackupController 持有。
    void backupWorldNow(const QString &worldDir);
    void restoreWorldBackup(const QString &worldDir);
    void snapshotInstance(const QString &versionId);
    /// @param dialog 从实例设置页点进来时那一页（可为空）。还原会把整个实例目录换掉，
    /// 那一页控件的值是还原之前的 —— 所以还原期间先禁用、动过盘之后直接收掉
    /// （见 `BackupController::restoreInstance()`）。
    void restoreInstanceSnapshot(const QString &versionId,
                                 InstanceSettingsDialog *dialog = nullptr);
    /// 起一趟"还原一份备份"（先做安全快照 → 挪走旧的那一份 → 解压 → 对账）—— 界面**不直接
    /// 走它**（走上面那两条），它是 `BackupController::startRestore()` 的薄壳，留着是给
    /// **mw_check 按名字驱动**的那几条断言（T35.3 / T35.4，见 .cpp 里那条 ★★）。
    Tasks::TaskId startBackupRestore(const QString &zipPath, const QString &targetParentDir,
                                     const QString &title, const QString &targetId,
                                     const QString &successTitle);
    void openBackupFolder(BackupFiles::Kind kind);

    /// 「复制实例」：让用户选择复制到的文件夹，再给定复制后的实例名称。
    ///
    /// 切法和上面几颗一样：**问的那一半**（选文件夹、要名字）在 MainWindow.cpp 里那个
    /// 私有 lambda 里，**真正动手的这一半**收三个字符串 —— 所以验证工程可以直接
    /// `QMetaObject::invokeMethod` 进来喂一条临时路径，不用去驱动那两个原生对话框。
    ///
    /// 四道闸：名字合不合法 / 这一份正在跑 / 这一份正在装 /
    /// **目标不能落在这一份实例自己里面**（最后那条见 VersionManager::copyInstance 的说明）。
    ///
    /// @param sourceId 源实例 id（= 目录名）。
    /// @param destGameFolder 用户挑的那个**游戏文件夹**（副本落在它的 `versions/` 底下，
    /// 不是直接落在它里面 —— 理由见 VersionManager::copyInstance 的说明）。
    /// @param newName 副本叫什么（新的实例 id）。
    /// @return 做成了 true。失败时原因已经弹出来了。
    bool copyInstanceTo(const QString &sourceId, const QString &destGameFolder,
                        const QString &newName);
    /// 界面那一颗「复制实例」走的后台版（不在 UI 线程直接复制整个目录）。
    /// 四道闸和同步版逐字一致，只是拷树落在后台线程；结果通过任务中心和弹框告诉人。
    void copyInstanceToInBackground(const QString &sourceId, const QString &destGameFolder,
                                    const QString &newName);
    /// 「导入世界」真正动手的那一半（顶上那颗按钮，）：把一个世界目录拷进
    /// 这个实例的 `saves/`。
    ///
    /// 干活的在 WorldFiles::copyWorldInto()（纯文件操作、单测在 verify 工程里），这里管的是
    /// "落点是哪儿"（`AppPaths::versionDir(id) + "/saves"`）、失败怎么报、成了之后让那一页重列。
    ///
    /// @param instanceId 目标实例 id（= 目录名）。
    /// @param worldDir 用户挑的那个世界目录（要含 `level.dat`）。
    /// @param name 拷进去之后那个目录叫什么。
    /// @return 做成了 true。失败时原因已经弹出来了。
    bool importWorldInto(const QString &instanceId, const QString &worldDir, const QString &name);
    /// 「存档」那一页行尾那支笔里的「重命名…」真正动手的那一半。
    ///
    /// 干活的在 WorldFiles::renameWorld()，这里只管防卫和报错。
    ///
    /// @param instanceId 这个世界属于哪个实例（**只用来判"游戏在不在跑"**）。
    /// @param worldDir 要改的那个世界目录（含路径）。
    /// @param newName 新名字。
    /// @return 做成了 true。失败时原因已经弹出来了。
    ///
    /// NOTE: 那道"正在跑就不许改"的闸是**早于磁盘的一层保险**，不是唯一的一层：真在跑
    /// 的时候，游戏那边握着 `session.lock` 的句柄，`QDir::rename()` 本来也会失败
    /// （Windows 会拒）。但那句失败理由只能说到"是不是有别的程序开着"，用户不一定
    /// 能把"游戏在跑"和它联系起来 —— 所以这里先把话说死。
    bool renameInstanceWorld(const QString &instanceId, const QString &worldDir,
                             const QString &newName);
    /// 实例行行尾那颗**上箭头**：把这个实例移进置顶组（哪一组当家见
    /// InstanceGroupStore::pinnedGroupId()），**并记下它置顶之前待在哪一组**（见那个类的
    /// 文件头那一段）。没有置顶组时什么都不做。
    void pinInstance(const QString &versionId);
    /// 同一格反过来那一颗（**下箭头**）：把这个实例送回它**置顶之前**待的那一组。
    /// **没有记录**（用户自己用「移入分组」塞进去的、或者旧数据）时落未分组 ——
    /// 和「移出分组」逐字同一个落点。
    void unpinInstance(const QString &versionId);
    /// 实例行行尾那个**垃圾桶**，以及实例设置概览页那颗「删除实例」：先问一句（这一趟
    /// 不可逆）再调 deleteInstance()。返回"真的删掉了" —— 唯一的调用方（概览页那一颗）
    /// 要靠它决定"这一页还收不收"（实例都没了，页面上那几格已经没有意义）。
    bool confirmDeleteInstance(const QString &versionId);
    /// 删除实例的**执行部分**：把 versions/<id> 整个目录删掉 + 刷界面。失败返回 false。
    ///
    /// NOTE: 拆成两步的理由和 applyRename() / applyInstanceSettings() 一样 —— 问的那一半
    /// 在覆盖层里跑一个嵌套事件循环，验证工程驱动不了它；而"守卫 + 真的删"这一半
    /// 必须验得了（运行中不许删、删完列表要跟着变）。
    bool deleteInstance(const QString &versionId);
    /// 实例设置的**执行部分**：把对话框里摆好的覆盖写进这个实例的 wtfml.meta。
    /// 失败时弹窗并返回 false，磁盘保持原样。
    ///
    /// NOTE: 拆成两步的理由和 applyRename() / applyGroupList() 一样 ——
    /// QDialog::exec() 阻塞在事件循环里，offscreen 的验证工程驱动不了它。
    /// 而这一半里藏着真会错的东西（"跟随全局"的项要**删键**而不是写一个等于
    /// 全局的值、写盘前得先确保实例有稳定 id），必须验得了。
    /// @note 收的是**非 const** 引用：用户刚挑的那张图标真落进实例目录之后，要回头让
    /// 对话框忘掉那个源路径（见 `InstanceSettingsDialog::markPickedIconSaved()`）。
    bool applyInstanceSettings(const QString &versionId, InstanceSettingsDialog &dialog);

    // ---------- 实例设置「文件管理」那八页----------
    //
    // 那八页说的是**这个实例自己的目录**（`--gameDir` 就是 `versions/<id>/`，见
    // LauncherManager 里那两处），所以动它们不会碰到别的实例。
    //
    // 分工和「导出启动脚本」那两颗按钮一样：对话框只**显示**和**报"用户想干这件事"**，
    // 真正碰盘、真正弹话的都在这一层 —— 弹话要 OverlayHost，而它在 MainWindow 手里。
    // 动完盘之后由调用点喊 dialog.refreshFilePages() 让它重列一遍。

    /// 「文件管理」那六页各看哪个目录：**页键 → 绝对路径**。
    ///
    /// 四个能下载的类走 ResourceCatalog::folderForKind()（那是目录名的唯一出处，整合包
    /// 那一路也照着它写），原理图 / 存档 / 截图 / 日志不在下载页那几类里，目录名写在这儿。
    QHash<QString, QString> instanceFileDirs(const QString &versionId) const;

    /// 概览那排「常用目录」里**现在真有东西**的那几路。
    ///
    /// 判据那两条是：「目录存在」或「相关模组可识别」—— 逐条理由见
    /// `InstancePagesController::commonFolderKeys()`（判据在那边）。
    /// 它和 `instanceFileDirs()` **不是一回事**：那一份回答"这个实例的每一路目录在哪儿"
    /// （形状是九条，有断言在数），这一份回答"这十三条里哪几条现在值得摆在外面"。
    ///
    /// @param dirs 灌给 `InstanceSettingsDialog::setFileDirs()` 的那一份（含 `game` /
    /// `replay` / `crash` 三个额外键 —— 它们就在那儿被读的，这一层不自己再算一遍
    /// 路径，两个地方算同一个路径正是"两份值"的经典起手式）。
    QStringList instanceCommonFolderKeys(const QString &versionId,
                                         const QHash<QString, QString> &dirs) const;

    /// 切到下载页的 @p categoryKey 那一栏。
    ///
    /// 入口是「文件管理」里那四页顶上的「下载新模组 / 资源包 / 数据包 / 光影」—— 按了以后
    /// 导向下载页对应类别。做的两件事是**收掉当前那一页**（收页只有这一条路，
    /// 见 .cpp 里那段）和**把选项卡与左栏那一列摆到对应的那一栏上**。
    ///
    /// @param categoryKey `MainWindowUI::downloadCategoryKeys()` 里的一个键。认不出来时
    /// 什么都不做，但会 `qWarning()` 说一句（症状是"按了没反应"，静默的话没人查得到）。
    /// 切到下载页的某一栏（实例设置那几个文件页顶上那颗「下载新模组」走的就是它）。
    ///
    /// @param origin 非空 = 这一次是**从某个实例的设置页进来的**（`NavigationReturn` 里
    /// 记着归哪颗根页、哪个实例、停在哪一栏、文件页滚到哪儿）：左栏顶上那颗
    /// 「← 返回实例设置」会露出来，并且把「已安装版本」选到那个实例上、把两个筛子
    /// （游戏版本 / 模组加载器）灌成那个实例的（左上角保留一个返回键，返回到实例设置
    /// 里面 / 自动放进当前实例文件夹 / 自动在搜索框里填入对应的版本和加载器）。
    /// 别的入口一律传默认值（无效的那一份）。
    ///
    /// 回来的那一页**默认就是 `categoryKey`**：「下载新模组」那一栏就是实例设置里
    /// 那一页（见 `InstanceSettingsDialog::filesDownloadCategoryKey()` ——
    /// "这两处是一回事"的唯一出处就在它那儿）；「版本升级」那一路要回来的是**另一页**
    /// （upgrade），它自己把 `pageKey` 写进 @p origin —— 来路不再靠事后改一位。
    void openDownloadCategory(const QString &categoryKey,
                              const NavigationReturn &origin = NavigationReturn());
    /// `openDownloadCategory()` 的第二步：**确认那一页收干净了**再切选项卡、挑那一栏、灌
    /// 筛子。收不干净就什么都不做（理由写在 .cpp 里）。
    /// @param roundsLeft 还允许再排几轮（收页要跨事件循环轮次才落定，见 .cpp）。
    void continueOpenDownloadCategory(int row, const QString &categoryKey,
                                      const NavigationReturn &origin, int roundsLeft);
    /// 把某一页那两个筛子灌成**某个实例的**：版本 = 它的游戏版本，加载器 = 它的加载器。
    /// 「自动在搜索框里填入对应的版本和加载器」就是这一条；调它的是上面那个函数。
    void fillDownloadFiltersFromInstance(ResourceCatalog::Kind kind, const QString &instanceId);
    /// 回到刚才那个实例的设置页（左栏顶上那颗「返回实例设置」点的就是它），并把那颗收起来。
    void returnToInstanceSettingsFromDownload();
    /// 顶上那四颗导航里的一颗被点了（`TitleBar::navRequested`）。**只管一件 MainWindowUI
    /// 管不着的事**：点的是当前这一颗、而且现在是「下载」页时，把那一页**刷回它的首页**
    /// （点击顶部下载按钮就刷新页面回到主页）。翻页和"把覆盖层收起来"那两条
    /// 仍旧由 MainWindowUI 自己接的那一条负责（那边先接，所以它先跑）。
    void onNavigationApplied(int sourceIndex, int targetIndex, bool reselected);

    // ---------- 「配置文件」那一页的编辑器----------
    //
    // 三件都是"接的人碰盘、弹话"那一套（同上那一段的分工）。**为什么编辑器那一格在
    // 对话框里、而读和写在这儿**：那一层手上没有 OverlayHost（读失败、写失败都得弹一句），
    // 也没有盘（ui/ 不 include src/，AppPaths 够不着）。而"切格子"是纯显示层的事，留在
    // 对话框自己那边（见 InstanceSettingsDialog::openConfigEditor()）。
    //
    // NOTE: 三件的 `dialog` 都是**这一趟开出来的那一页**（`openInstanceSettings()` 里
    // 那份堆上的 `InstanceSettingsDialog`）—— 和上面那几条 lambda 捕获的是同一个，
    // 它活到收页为止。

    /// 用户点开了 config/ 底下的一条文件：读进来、解码，然后摆进编辑器那一格。
    ///
    /// NOTE: **解码不等于"看得懂"**（见 ConfigFileText 那一段：这一页压根不解析格式）——
    /// 它只做三件事：认 BOM、认行尾、认这一份是不是 UTF-8。前两样要原样存着，改完
    /// 写回去时一个字都不能变。
    void openConfigFile(InstanceSettingsDialog *dialog, const QString &path);

    /// 用户点了「保存」：把 @p bytes 写到 @p path 上，成功了再告诉那一格。
    ///
    /// NOTE: 走 `QSaveFile`（先写同目录下一个临时文件、最后原子改名）—— 直接 `QFile`
    /// 写一半失败的话，用户手上的配置文件就毁了，而这一页**没有"恢复默认"**
    /// （模组下次启动重写一份那要等它启动，也不保证每个模组都这么干）。
    ///
    /// 写之前有三道闸，顺序是硬的：
    /// ① **游戏在跑这个实例 ⇒ 直接不写**（游戏手里有一份内存副本，退出时整份写回来，
    /// 用户改的东西会被静默盖掉，看起来就像「我改了没用」）；
    /// ② **盘上那份"还是我打开时那一份"吗** —— 对不上就问一句（覆盖 / 读盘上那份 / 什么都
    /// 不做），**不许静静盖掉**别人的改动；
    /// ③ **写之前把盘上那一份原样留一份**（世代备份）—— 留不下就不写（见下面那段
    /// "为什么备份失败要挡住这一次保存"）。
    void saveConfigFile(InstanceSettingsDialog *dialog, const QString &path, const QByteArray &bytes);

    /// 用户点了「还原上一次」：把最近那一份备份读出来，摆回编辑器那一格。
    ///
    /// NOTE: **只摆进框里、不写盘**（见 ConfigFileEditorPage::restoreContent()）——「还原」
    /// 不是一次悄悄的写入，用户按「保存」才算数。所以这一趟只读备份，一个字节都不动
    /// 盘上那一份。
    void restoreConfigFile(InstanceSettingsDialog *dialog, const QString &path);

    /// 这个文件属于哪个实例：**从路径上认**（`<数据目录>/versions/<id>/…` ⇒ `<id>`）。
    ///
    /// NOTE: 不从对话框手里问"现在开着的是哪个实例" —— 那条路隔着一次改名（改完名
    /// `versions/<老id>` 整个不见了，而这一格手上那份文件还在老路径上）。路径自己
    /// 带着答案，问它就够了。认不出（不在任何 versions/<id>/ 底下）= 空串。
    QString instanceIdOfPath(const QString &path) const;

    /// 这个文件的**最近那一份备份**的绝对路径（没有 = 空串）。
    ///
    /// NOTE: 「有没有备份」和「取哪一份」是**同一个问题**（备份的文件夹名按字典序排就是按
    /// 时间排，见 ConfigFileText::backupFolderName()）—— 所以只有这一个函数，那一格
    /// 那颗按钮画不画也是问它。
    QString latestConfigBackup(const QString &instanceId, const QString &path) const;

    /// 这个实例的备份只留最近 @p keep 份，更老的删掉。
    ///
    /// NOTE: 这是全仓**唯一一处会主动删东西的缓存目录**（见 AppPaths::configBackupDir()）——
    /// 所以只认我们自己生成的那个文件夹名（ConfigFileText::backupFoldersToRemove()
    /// 里那道闸），用户自己放进去的东西一个都不碰。
    void pruneConfigBackups(const QString &instanceId, int keep);

    /// 用户想从编辑器退回文件列表（点了「返回」，或者按了 Esc）。
    ///
    /// **改过的东西不许悄悄没掉**：改过就弹一句问，用户说"丢掉"才真回去。这一问在这儿
    /// 而不是在编辑器里，理由是那一层没有 OverlayHost（见 InstanceSettingsDialog::
    /// closeConfigEditor() 那条 NOTE）。
    void leaveConfigEditor(InstanceSettingsDialog *dialog);

    /// 弹那句「改过还没保存，要丢掉吗」。
    ///
    /// @return true = 用户选了"丢掉"。
    ///
    /// NOTE: **两个调用点共用这一份字**（`leaveConfigEditor()` 退到列表、以及顶上那颗返回
    /// 箭头要关整页那一趟）—— 同一件事在两处问出两套说法，用户会以为是两件事。
    /// NOTE: 默认那一颗是**「留在这儿」**（`defaultYes = false`）：手一滑按到 Esc、或者点
    /// 到卡片外面，不该把用户刚改的东西丢掉。
    bool confirmDiscardConfigEdits();

    // ---------- 「世界设置」（存档那支笔点开的那一页）----------
    //
    // 世界设置按存档文件编辑游戏规则等。三步的走法和"每一步都重读一遍盘"的理由见
    // .cpp 里那一大段。
    //
    // 点那支笔**不问备份那一句**：直接落到下面这一个函数上。备份仍有两处：页里那颗
    // 「备份一份」，以及展开「修改存档文件」那个折叠框时那一句警告。

    /// 那支笔上的「世界设置…」：读盘 → 翻译 → 摆上去。**坏档也在这里判**（读不出来就摆成
    /// "读不出来"那一档，顶上写「这个世界可能坏了：<目录>」、正文整块收着）。
    ///
    /// @param worldDir **存档目录的绝对路径**（那支笔发出来的就是它）。
    void showWorldSettingsPage(InstanceSettingsDialog *dialog, const QString &worldDir);

    /// 用户点了「返回」：改过就弹一句问，答"丢掉"才切回世界列表。
    void leaveWorldSettings(InstanceSettingsDialog *dialog);

    /// 用户点了「保存」。四道闸 + 写回，顺序见 .cpp。
    void saveWorldSettings(InstanceSettingsDialog *dialog);

    /// 把 `<worldDir>/level.dat` **原样抄一份**到世代备份里（并清理超出的代数）。
    ///
    /// @param problemOut 没留成时的原因（给人看的一句话；成功时清空）。
    /// @return 留成了没有。
    ///
    /// 抄的是**盘上那一份的字节**，不是"我们解出来又压回去的那一份"：备份是原件的逐字节
    /// 复制，"还原"才有确定的意思（也绕开了 gzip 头里那个 mtime 对不上的事）。
    bool backupWorldLevelData(const QString &worldDir, QString *problemOut);

    /// 这个世界的备份里**最近的那一份**（`level.dat` 的绝对路径）；一份都没有返回空串。
    QString latestWorldBackup(const QString &worldDir) const;

    /// 弹那句「改过还没保存，要丢掉吗」（**世界设置那一页的版本**）。
    ///
    /// @return true = 用户选了"丢掉"。
    ///
    /// NOTE: 和 `confirmDiscardConfigEdits()` 长得一样但是**两份**，不合并：两句话说的是
    /// 两样东西（那个是"文件"，这个是"世界"），而且默认那颗都是"别丢"——合并省下的
    /// 那两行字，代价是以后改一处话术要想起另一处。
    bool confirmDiscardWorldEdits();

    /// 这个实例的 mods/ 里有没有"名字里带 @p nameToken 这个词"的 **.jar**（忽略大小写）。
    ///
    /// NOTE: 判据是**文件名里的词**，不是"这个模组是谁"—— 我们没有模组元数据（那要读
    /// jar 里的 fabric.mod.json）。够用：Iris 的 jar 叫 `iris-<版本>.jar`，
    /// Litematica 的叫 `litematica-<加载器>-<版本>.jar`。被禁用（`.jar.disabled`）
    /// 的不算 —— 那正是"禁用"的意思。
    bool instanceHasMod(const QString &versionId, const QString &nameToken) const;

    /// 这个实例能不能用光影包：mods/ 里装了 **Iris / Oculus / OptiFine**（都是"加载
    /// shaderpacks/ 的东西"），或者这个实例装的加载器**就是 OptiFine**（它自己认
    /// shaderpacks/）。判据由这一层算好递给对话框 —— ui/ 够不着 wtfml.meta 和 mods/ 目录。
    ///
    /// NOTE: Oculus 是 Forge 上那个 Iris 移植，而 OptiFine 在 1.12.2 及以前是**当普通
    /// 模组装进 mods/ 下的（`OptiFine_1.12.2_HD_U_G5.jar`）—— 这两样都必须认，否则装了
    /// 光影的实例照样会说"用不上"。
    bool instanceShaderUsable(const QString &versionId) const;

    /// 这个实例用不用得上「模组」那一页 —— 两条路任一条命中就算用得上：
    /// ① 它基于一个模组加载器（Fabric / Forge / NeoForge / OptiFine / Quilt）；
    /// ② `mods/` 里**已经有 jar**。
    ///
    /// ① 判据**不能**只看 `displayParts().loaderName` 非空：那一格**只有我们自己的
    /// 安装器会写** —— PCL 那类"整份 json 合进实例目录"的实例读出来是空的，于是那一页
    /// 在**每一个**这样的实例上都灰着。现在走 VersionManager::instanceModLoader()，
    /// 那个函数的两条路见它的说明。
    ///
    /// ② 和 ① 是**两件事**，所以它挂在这一层、不塞进 instanceModLoader()：那边回答的是
    /// "基于哪个加载器"，而往 mods/ 里丢一个 jar 不构成任何加载器证据。"这一页对他有没有
    /// 用"才是这里的问题 —— 有的实例 json 认不出加载器、可 mods/ 里躺着 jar，
    /// 那一页显然是该给的。
    bool instanceModsUsable(const QString &versionId) const;

    /// 读一次 servers.dat 递给对话框（解析在 src/util/NbtUtil 里）。
    ///
    /// NOTE: 「读不动」和「读到了但一条都没有」分开给（见 setServerList()）——
    /// 前者要说清为什么，后者才能说"你还没加过服务器"。
    void loadServerList(InstanceSettingsDialog *dialog, const QString &versionId) const;

    /// 覆盖层那一层现在空着吗（"一层只放得下一页"那条规矩的**只读**版本）。
    ///
    /// NOTE: 它**只回答、不弹话**；"先关掉现在这一页"那种话由 `runWhenOverlayFree()`
    /// 在收不掉时再说。
    bool overlayAcceptsNewPage() const;

    /// 这一层**空着**的时候，`open` 当场就跑（绝大多数情况）；被占着的时候，先把占着的那
    /// 一层**静默收掉**，再把 `open` 排到下一轮去。
    ///
    /// 页面被递归开启、或任何"先关闭当前页面再继续"的情况，都直接静默关闭那个页面
    /// 并继续。
    ///
    /// 收页那一发（`OverlayHost::evict()`）是**请求**：`showPage()` 开出来的那一页在同一次
    /// 调用里就收干净了，但调这一发的人多半正站在那一页自己的槽里（当场再开一页等于把旧页
    /// 从脚下抽走），提示那一档（`execPrompt()`）还卡着嵌套循环、收尾要到它的调用栈退出去
    /// 才落地 ⇒ 所以 `open` **必须**排到下一轮（这条和"重下=校验跳过"那种判断不一样，
    /// 它是个时序事实）。排的时候**再从这一层的头上过一遍**（递归的自己）：那一轮里可能
    /// 又有别的东西把它占上了。
    ///
    /// @param open 真要开的那一整段（**不含**自己那一道判据 —— 让位这件事这儿已经做完了）。
    /// @param roundsLeft 还能再排几轮（`kOverlayOpenRounds`）。**不写"转到收干净为止"**：
    /// 收不掉的时候再转多少轮也收不掉，转下去只是空转（同 `kDownloadOpenRounds`）。
    ///
    /// NOTE: 这条**只给用户自己点的那些**用。后台弄完的事要开一页，走下面的
    /// `openPageWhenIdle()` —— 那一档**不驱逐**（理由见那边）。
    void runWhenOverlayFree(const std::function<void()> &open,
                            int roundsLeft = kOverlayOpenRounds);

    /// 收不掉的时候那条兜底：两句话（一条给"占着的是提示"、一条给"占着的是整页"）。
    ///
    /// NOTE: **正常情况走不到这儿** —— `evict()` 收不掉只有一种可能：那一层正卡在自己的
    /// 嵌套事件循环里等着、这一发不该落在它头上。真有那么一天，也得说句人话
    /// （"点了没反应"正是这条路要治的毛病）。
    void warnOverlayBusy();

    /// **后台**做完的事要开一页时走这个（更新检测回来了 / 更新装完了 / 体检跑完了）。
    ///
    /// 和 `runWhenOverlayFree()` 的差别只有一条，但很要紧：**不驱逐**。那一档是"用户
    /// 刚点了什么，把挡路的收掉再继续"；而这里是后台弄完的事 —— 用户这会儿可能正看着
    /// 另一页，把它收掉就是"我什么都没点，屏幕上的东西自己没了"（实例设置那种页还会
    /// 顺带把用户刚翻到的那一页、滚到哪儿全丢了）。
    ///
    /// 空着就当场开；被占着就排着，等这一层收干净之后再开
    /// （`OverlayHost::queuePageOpen()`）。用户**自己**点的那条路照旧走
    /// `runWhenOverlayFree()`，两条不许合并。
    void openPageWhenIdle(const std::function<void()> &open);

    /// `runWhenOverlayFree()` 让开之后真正跑的那一半。
    /// NOTE: 分成两个函数是为了让"让位"那一段只有一处（`runWhenOverlayFree`），而各个入口
    /// 自己那几百行一行都不用重新缩进。
    void openInstanceSettingsNow(const QString &versionId, const QString &pageKey);
    void openJavaManageNow();
    void openGroupDialogNow();

    /// 这个实例的服务器列表（名字 + 地址，按文件里的顺序）。servers.dat 读不动时是空的。
    ///
    /// NOTE: 和 serverListProblem() 分两个函数是因为**它们读的是同一份文件**，而调用方
    /// 要的东西不一样：setServerList() 两样都要（列表 + 读不动的那句话），
    /// 而 pingServers() 只要地址那一半。合起来收一个"结果结构"也不难，只是这一层
    /// 的文件读得很便宜（几 KB），宁可写成两个一眼看得出在干什么的函数。
    QList<QPair<QString, QString>> serverListOf(const QString &versionId) const;
    /// 上面那份列表读不动的原因（空串 = 读到了）。见 loadServerList() 那条 NOTE。
    QString serverListProblem(const QString &versionId) const;

    /// 请求服务器列表测速；运行状态与缓存回灌由 ServerPingController 持有。
    void pingServers(InstanceSettingsDialog *dialog, const QString &versionId);

    /// 启用 / 禁用一条：把文件名改成 `<原名>.disabled`，或者反过来。
    /// @return **动过盘没有** —— 真了调用点才需要让那一页重列（没动过还重列会让用户
    /// 以为"刚才那一下生效了"）。
    bool toggleInstanceFile(const QString &path, QString *problem = nullptr);

    /// 删掉一条（**先问一句**；文件夹连同里面的东西一起删）。@return 同 toggleInstanceFile()。
    bool deleteInstanceFile(const QString &path);

    /// 删掉一条**不问**那一句 —— `deleteInstanceFile()` 问完调的就是它，批量删除
    /// （deleteInstanceFiles()）也是（那一批**只问一次**）。@return 动过盘没有。
    /// @param problem 同 `toggleInstanceFile()` 那个出参（非空 = 别弹框，把缘故写在这儿）。
    bool removeInstanceFileUnasked(const QString &path, QString *problem = nullptr);

    /// 把「文件管理」那几页里那些**资源文件**的图标要回来。
    ///
    /// 这一整套（那一趟的状态、会话缓存、"这一条是哪个资源"那张表、往页上摆）在
    /// `src/controllers/InstanceIconController.*`。这一层只剩**这一个入口**：实例设置页
    /// 开出来时喊它一声（调用点在 `InstanceDialogController`）。其余几样（缓存钥匙 /
    /// 候选文件表 / 这个实例认过没有 / 这一条是哪个资源）都由那边直接提供，
    /// 不再从这一层绕一圈。
    void requestInstanceFileIcons(InstanceSettingsDialog *dialog, const QString &versionId);

    /// 实例设置里**点一条文件的名字**（说明见 .cpp 里那段）⇒ 开那个资源
    /// 自己的那一页。
    ///
    /// @param versionId 这一下是从**哪个实例**的设置页里发出来的（那一页正看着的实例的
    /// 目录名）。它只有一个用处：那一页关掉之后把**实例设置放回去**（见
    /// `openFileResourcePage()`）—— 点下载界面返回后应该退回到实例设置
    /// 而不是实例列表。
    /// @param pageKey 发起这一下的那一页的页键（模组 / 资源包 / 数据包 / 光影 / …）。
    /// 从它认"是哪一类资源"（唯一出处仍是 `filesDownloadCategoryKey()`）。
    /// @param path 那一行对应的文件（绝对路径）。
    ///
    /// 走的是**下载页点一行**那条路（`openResourceDetailPage()`）：同一份 `m_resourceHits`
    /// 里那条 Hit，同一页、同一套 chip。
    /// 没匹配上的（没认出来是哪个资源的）什么都不做 —— 那一行的名字本来也没变成链接。
    ///
    /// NOTE: 收实例设置那一页 + 排到下一轮事件循环的理由写在 .cpp 里（和
    /// `openDownloadCategory()` 逐字同一套）。
    void openInstanceFileResource(const QString &versionId, const QString &pageKey,
                                  const QString &path);
    /// 开那一页，并在用户**按返回/Esc 收掉它**之后把实例设置放回去（见
    /// `openInstanceFileResource()` 里那段）。两处调用（覆盖层还开着 / 已经关了）都走它，
    /// "回来之后干什么"因此只有一处出处。
    void openFileResourcePage(const QString &versionId, const QString &iconKey,
                              const QString &kindKey);
    /// 「导入整合包…」：选文件 → 读清单 → 问名字 → 换地址 → 确认 → 交给 applyModpackImport()。
    ///
    /// 读的是压缩包根目录里那一份清单（ZipUtil::readEntry，**不整包解开**：用户随手选一个
    /// 几百 MB 的包，为了知道它不是整合包先解压几分钟是白等）。**两种格式各读一个名字**：
    /// Modrinth 是 `modrinth.index.json`，CurseForge 是 `manifest.json`。
    void onImportModpackClicked();
    /// 导入的**执行部分**：把清单解析出来、该拦的拦掉、算出安装器要的 Java，然后开始导入。
    /// @param zipPath 用户选的那个整合包文件。
    /// @param instanceName 落地的实例名（= `versions/<id>` 那个目录名）。
    /// @param manifestJson 压缩包里那份清单的**原始字节**（哪个清单看 format）。
    /// 传字节而不是传解析结果，是为了让这一半能被验证工程驱动（自定义类型过不了
    /// QMetaObject::invokeMethod）；里面会再解析一遍 —— 纯函数，94 KB 的清单也就
    /// 一瞬，换来的是"确认框里说的那个包"和"真的去装的那个包"是同一串字节。
    /// @param format 这份字节是哪个清单。**CurseForge 那一档在这里还要再换一次地址**
    /// （那一发的详情见 VersionManager::resolveCurseForgeFiles）—— 于是导入一个
    /// CF 包会问两次接口，换来的还是上面那句"同一串字节"。
    /// @return 真的开始导入了返回 true（失败/被拦下时弹窗并返回 false）。
    ///
    /// NOTE: 拆成两步的理由和 applyRename() 一样 —— 这一半里藏着真会错的东西（重名要拦、
    /// 不支持的加载器要拦、忙态几时置上、结束之后列表要刷）。
    /// （问名字那一格是能驱动的内嵌提示；**QFileDialog 那一步驱动不了** ——
    /// 它开的是 Windows 自己的文件选择框，不是我们的窗，见那个调用点上面那段。）
    bool applyModpackImport(const QString &zipPath, const QString &instanceName,
                            const QByteArray &manifestJson, ModpackImport::Format format);
    /// 用户在左边那张列表里换了「游戏文件夹」。真正干活的是 FolderManager 发出的
    /// currentFolderChanged()，这里只负责把选中项翻译成一次 setCurrentFolder()。
    void onFolderSelectionChanged();
    /// 某一份会话吐了一行 stdout。@param sessionId 哪一份（0 = 连会话都没建起来，
    /// 那种消息是"又开一个被拦下了"这类，不挂在任何实例名下）。
    void onProcessOutput(int sessionId, const QString &text);
    void onProcessError(int sessionId, const QString &text);
    /// 这一号会话在真的起来之前就被收回了（启动那三条早退），界面据此把刚为它建出来的
    /// 那一扇日志窗和按号记的底账收掉（见 GameSessionController::onSessionDiscarded()）。
    void onSessionDiscarded(int sessionId);
    void onGameFinished(int sessionId, int exitCode, QProcess::ExitStatus exitStatus);
    /// 停掉**一份**会话（**现在只有底栏左面板里那行电源走它**）：先问、再停 ——
    /// @param sessionId 那一行是谁（面板弹出来时抄下来的会话号，见 PanelRow::key）。
    ///
    /// NOTE: 产品里**没有"一次全停"的入口**：多开的几份要一行一行停。别照着旧记性
    /// 找一组不存在的 "stop 全部" 函数。
    /// NOTE: 会话号可能在"面板弹出来到现在"这几秒里已经结束了 —— 那种情况**不弹窗**，
    /// 只写一行日志（用户没做错什么，弹一个框只会让他以为出事了）。
    void stopGameSession(int sessionId);
    /// 「关掉这个正在跑的游戏」那个人确认过了没有（默认那一边是"别停"）。
    /// @param displayName 实例显示名。
    /// @param sessions 这一下会停掉**几个**游戏进程（现在恒为 1 —— 唯一那个调用者
    /// stopGameSession() 只停一行；留着这个参数是因为"停几份"问话里要说清）。
    ///
    /// 那句"游戏不会存档"是**产品级约定**（见 .cpp 里那段）—— 这个框就是为了让它只有
    /// 一份出处：两处各写一句迟早会漏掉一处。
    bool confirmStopGame(const QString &displayName, int sessions);
    // ---------- 下载页「游戏」那两层：版本树 → 安装页 ----------
    //
    // 说明见 MainWindowUI::downloadGameStackName() 上面那段。这一层只做"界面上的
    // 走动 + 把用户选的东西翻译成一次请求"，真正干活的是 VersionManager 那三个
    // installNamed*()（它们自己会把原版补齐、写薄 json、发**唯一**那个终态信号）。
    // / 点树里一条**版本号**（子条目）→ 进安装页。顶层那四条是分类，点了不做任何事
    // / （展开/折叠交给 Qt 自己）—— 那条判据在 setupConnections() 里那个 lambda 上
    // / （那边 QTreeWidgetItem 是完整类型）。
    // / 进安装页：@param gameVersion 树里点中的那个原版号（这一份要基于它装）。
    // / 也**允许验证工程直接调**（参数只有字符串，QMetaObject::invokeMethod 认得）。
    void openInstallPage(const QString &gameVersion);
    /// 「← 返回版本列表」：回树那一层。**什么都不清** —— 树里那份展开状态、名称框里
    /// 敲过的字都留着（用户按返回是想再看一眼别的版本，不是想放弃刚才那些输入）。
    void backToVersionTree();
    /// 名称框里每敲一个字：记下"用户动过这个名字了"，再重算那句小字（重名提示要"实时"）。
    void onInstallNameEdited();
    /// 点了「安装」。名字校验、重名那一问、Java 挑选、忙态，都在这里收口。
    /// requestLoaderVersions() 的回复：填进**那一条**底下的子条目。
    ///
    /// 参数收的是**下标**（不是 VersionManager::LoaderKind）：这是给 moc 看的槽，
    /// 签名里出现别处的枚举就得把 VersionManager.hpp 整个拖进这个头（再说枚举还得
    /// 先 qRegisterMetaType）。枚举→下标那次翻译在 setupConnections() 那个 lambda 里做
    /// （VersionManager::loaderIndexForKind），一处收口，和"下标→枚举"同一个方向表。
    void onLoaderVersionsFetched(const QString &gameVersion, int loaderIndex, bool success,
                                 const LoaderMeta::LoaderVersionList &versions,
                                 const QString &errorMessage);

    void onVersionsFetched(const QStringList &versions);
    void onVersionDetailsFetched(const QString &versionId, const QJsonObject &versionJson);
    void onVersionDownloadStats(int done, int total, int failed, const QString &currentFile);
    /// 「导入整合包…」那个按钮：只是**选文件**，选完交给 importModpackFromFile()。
    /// 拆成两个函数是因为下载页「整合包」那一页也要走同一条路（那边是"下回来一个 .mrpack"，
    /// 路径是从下载结果来的，没有文件对话框那一步）—— 整条导入流程只有一份。
    ///
    /// @param namePrefill 问名字那一格的**预填值**。空串 = 按包名推一个
    /// （`ModpackImport::suggestInstanceName(pack.name)`，也就是那颗按钮那一档）。
    /// 拖进来一个整合包时预填的是**文件名**（不是包名）—— 拖进来的东西用户刚在资源管理器里看过，
    /// 文件名是他认得的那一串，包名反而未必。
    void importModpackFromFile(const QString &zipPath, const QString &namePrefill = QString());

    /// 下载页整合包一键安装入口：整合包压缩包已在缓存目录就绪、实例名也已在点击时确认，
    /// 直接沿用现有的 @p taskId 推进任务中心步骤并进入导入。
    void importModpackOneClick(const QString &zipPath, const QString &confirmedInstanceName,
                               Tasks::TaskId taskId = Tasks::invalidTaskId());

    void markOneClickModpack(const QString &instanceId) { m_oneClickModpackIds.insert(instanceId); }
    bool takeOneClickModpack(const QString &instanceId) { return m_oneClickModpackIds.remove(instanceId); }

    // ---------- 拖进来的文件----------
    //
    // 落点规则：实例设置开着时拖入 .jar/.zip 之类，先认是整合包还是资源包等；
    // 模组且正好在模组设置那一页就直接塞进去，不是对应页就问一句「将在此实例中安装
    // 资源包/…，是否继续」；整合包则让用户选择安装的文件夹及实例名（预填文件名）并离开
    // 实例设置（相当于点了返回）。主窗口上（没有哪一页开着）拖入模组等时，选择要安装到的
    // 目标实例。
    //
    // 切成三条：**认类型**在 `DroppedResource`（纯判断）、**复制进目录**在
    // `installLocalResourceFile()`、**问什么**在这两个 handle* 里。两条入口各一个
    // （实例设置那一页开着 / 没开着），是因为"装到哪去"这个问题只有前者不用问。

    /// 实例设置那一页上报"落了这几个文件"（`InstanceSettingsDialog::filesDropped`）。
    /// @param page 那一页（要用它读 `currentPageKey()`、装完让它刷一次文件页）。
    /// @param versionId 这一页说的是哪个实例（目录名）。
    void handleInstanceSettingsDrop(QWidget *page, const QString &versionId,
                                    const QStringList &paths);

    /// 拖到**主窗口**上的（没有哪一页开着）：装到哪个实例里要问用户。
public:
    void handleWindowDrop(const QStringList &paths);
    /// 主页启动卡片（#65）的数据：把任务中心里那一项启动任务翻成卡片那几样（阶段名、件数、
    /// 失败原因）。阶段名照 card-spec §3 的定稿字；失败但没有原因的（用户自己取消）回 Idle。
    static MainWindowUI::LaunchCard launchCardFor(const Tasks::TaskSnapshot &task, bool offline);
    /// 关联/命令行带进来的文件：原理图类打开摘要，其余按拖入处理。
    void openExternalFile(const QString &path);
private slots:

    /// 把一个本地文件装进 @p versionId 这个实例的 @p kind 那一类目录里（没有目录就建）。
    ///
    /// 目标里已经有同名文件时会**问一句要不要盖掉**（`kind` 那一类的说法写在那句话里）。
    /// @param problemOut 失败时说人话的原因；成功时清空。
    /// @return 装上去了没有（用户答"不盖"也算 false —— 那一次没装）。
    bool installLocalResourceFile(const QString &versionId, ResourceCatalog::Kind kind,
                                  const QString &path, QString *problemOut);

    /// 拖进来一个整合包：**先问游戏文件夹**、再问实例名（预填 @p namePrefill），
    /// 然后走既有的导入流程（`importModpackFromFile`）。
    ///
    /// 问游戏文件夹那一问（让用户选择安装的文件夹及实例名）——
    /// 手法和「复制实例」那一处**逐字相同**（原生目录选择框 + 问名字），理由见那一处
    /// 那段：`ui/` 不碰 `AppPaths`，"该默认到哪儿"只有这一层知道。
    void importDroppedModpack(const QString &zipPath, const QString &namePrefill);
    /// 下载页那几类资源里挑中的那个版本下完了（见 downloadResourceFiles）。
    /// label 是「Sodium 0.6.13」这种"下的是什么"，界面上只写日志 + 弹窗，**不碰任何实例的
    /// 安装状态**（那是 DownloadCompletionController::onFinished 的事，两者语义不同，见 VersionManager 里
    /// 那个信号的 NOTE）。
    void onResourceDownloadFinished(const QString &label, bool success,
                                    const QString &errorMessage);

    // ---------- 设置页「背景图」那两行 ----------
    //
    // 主页左栏和页面底各一块。动的是**主题规格**
    // （`ui/Theme.hpp`），不是 LauncherSettings —— 见 .cpp 里那一段开头写的"改内存里那份
    // → 落盘 → 立刻铺开"这三步。
    //
    // 这五条**放在 `private slots:` 里**（它们本来只被控件上的信号驱动，做不做得成槽对产品
    // 毫无区别）：验证工程要能按名字把 `refreshThemeImageRows()` 调起来，量"用户挑完一张图
    // 之后这两行长什么样" —— 挑图那颗按钮底下是 QFileDialog，测试里点不了，所以那一条只能
    // 从这儿进去（mw_check 的 T29）。
    // / 「选择图片…」：问用户要一张图 → 拷进数据目录 → 记进规格 → 立刻生效。
    // / @param index 第几行（`MainWindowUI::getThemeImageRow()` 那张表的下标）。
    void onThemeImageChooseClicked(int index);
    /// 「清除」：把数据目录里那一份删掉，顺带把这一块的覆盖项整个撤掉。
    void onThemeImageClearClicked(int index);
    /// 摆法 / 不透明度：**立刻生效**（拖滑块时那块底跟着变，预览就是界面本身）。
    void onThemeImageFitChanged(int index);
    void onThemeImageOpacityChanged(int index);
    /// 把 `Theme::current()` 里那两块图的样子画到这两行控件上（含"现在铺的是哪一张"那句
    /// 话、以及没设图时把后面那几颗灰掉）。**灌值那一段和每次改完都要调它一次。**
    void refreshThemeImageRows();

    // ---------- 「主题」那一节（设置页「个性化」） ----------
    //
    // 配色（跟随系统 / 浅色 / 深色）、主题色、文字不透明度、四个语义色，外加恢复默认与
    // 导入/导出。和「背景图」那一节动的是**同一份规格**（`Theme::Spec`）—— 所以两边改完
    // 走的是同一个 `applyThemeSpec()`。
    //
    // 同样放在 `private slots:` 里：验证工程要能按名字把 `refreshThemeControls()` 调起来
    // （`pickColor()` 那一格底下是覆盖层，测试里点不了 —— 和背景图那条路一个道理）。

    /// 「配色」那个下拉框换了一档：改 `presetId` 之后铺开（覆盖项**不动** —— 覆盖是叠在
    /// 预设上面的，换了预设还是那一个，"深色 + 我改的那几项"读起来才对）。
    void onThemePresetChanged();
    /// 点主题色那块色：开取色卡片，挑完立刻生效。
    void onThemeAccentClicked();
    /// 主题色右边那颗「恢复默认」：回到出厂那一支绿。
    void onThemeAccentResetClicked();
    /// 第 @p index 个语义色（下标见 `MainWindowUI::themeSemanticKey()`）。
    void onThemeSemanticClicked(int index);
    /// 四个语义色旁边那颗「跟随预设」：把四个覆盖项一起清掉（之后按深浅现推）。
    void onThemeSemanticResetClicked();
    /// 「恢复全部默认」：整份规格回出厂（含「背景图」那一节那两块），并删掉存下来的那几张图。
    void onThemeResetAllClicked();
    /// 「导出主题…」/「导入主题…」：一份 JSON 文件（`Theme::specToJson()` / `specFromJson()`）。
    void onThemeExportClicked();
    void onThemeImportClicked();
    /// 「文字不透明度」那根条。**拖一下就生效、也落盘**（和背景图那根不透明度条一个路数）。
    void onThemeTextOpacityChanged(int value);
    /// 把 `Theme::current()` 的样子画到这一节的控件上（配色下拉框 / 四块色 / 那根条）。
    /// **灌值那一段和每次改完都要调它一次**（挡着信号，理由同上面那一条）。
    void refreshThemeControls();

    /// 改主题规格的**唯一那三步**：收拾干净 → 落盘 → 立刻铺开，再把这一栏的控件重画一遍。
    ///
    /// 为什么非要有这么一个函数：这一栏里**两节**（主题 / 背景图）改的都是同一份规格，
    /// 而"改完立刻生效"这件事少一步就是"改了没生效、下次启动才出现" —— 那是这一栏最要命
    /// 的失败模式。四步写在一个地方，"三处各抄一遍、其中一处漏了第三步"就不可能发生。
    /// @param spec 要铺开的那一份（**整份**，不是差量 —— 规格是整份写回去的）。
    void applyThemeSpec(const Theme::Spec &spec);
protected:
    /// 只为一件事挂着：外框那两条 Win32 消息（WM_NCCALCSIZE / WM_NCHITTEST，见
    /// `ui/WindowChrome.hpp` 开头那两段）。接到手的是**这个窗口**的消息，Qt 已经替我们把
    /// HWND 对上号了，所以这里只是转手，没别的。
    ///
    /// NOTE: 这一条**必须在 MainWindow 上**（不能改挂到别的控件上）：Qt 只把消息交给顶层
    /// 部件 —— `QWidgetWindow::nativeEvent()` 那一句就是 `m_widget->nativeEvent(...)`，
    /// 而 `m_widget` 是那个顶层窗。
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

    /// 关窗口 = 退出启动器（全仓没有别的退出路径：没有托盘图标、没有 qApp->quit()）。
    ///
    /// 游戏还在跑的时候这里要拦一下，理由见 .cpp 里那段：MainWindow 是 main() 里的栈对象，
    /// 它一析构，链子上那个 QProcess 的析构就会把游戏 **kill()** 掉。
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief 有东西被拖到**窗口**上。
     *
     * 这一层接的是"没有哪一页开着"的那一档（实例设置开着时是**那一页**先接住，见
     * `InstanceSettingsDialog::dragEnterEvent()`）：拖进来的文件该装到**哪个实例**里，
     * 这一档没有上下文可推，只能问用户。
     *
     * NOTE: 判据（认不认这一发拖放、拖的是哪几个文件）只有一处 —— `DroppedResource` 判
     * 类型、`InstanceSettingsDialog::droppedFilePaths()` 判"是不是本地文件"。
     * 这一个大类不自己再写一遍，免得两处对"什么算一个能装的文件"给出不同答案。
     */
    void dragEnterEvent(QDragEnterEvent *event) override;

    /// @copydoc dragEnterEvent
    void dropEvent(QDropEvent *event) override;

    /// 只为一件事挂着：安装页那棵加载器树的视口。选中那条加载器右边那个「取消」小叉
    /// （`rowClearRole()`）是**行里的一个按钮**、不是"点这一行"，而 `itemClicked` 只给条目
    /// 不给"点在哪一格" —— 所以那一下得在这里判、在这里吃掉（返回 true），不然点叉会顺手
    /// 把那一列展开/收起（见 .cpp 里那段 NOTE）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class InstanceDialogController;
    friend class AgentLauncherHostImpl;
    friend class ServerManagementActions;
    friend class InstanceUpdateFlow;
    friend class InstanceEditorController;
    friend class ImportController;
    friend class LaunchController;
    friend class ConnectionsController;
    friend class ResourcePageController;
    friend class ResourceInstallController;
    friend class DownloadFilterController;
    friend class InstanceUpdateCheckController;
    friend class ResourceSearchController;
    friend class UpdateCenterController;
    friend class ServerSettingsPageController;
    friend class InstallPageController;
    friend class ServerRunController;
    friend class DownloadCompletionController;
    friend class DownloadTaskLedgerController;
    friend class ServerMenuController;
    void setupConnections();

    /// 键盘入口（实现在 MainWindowEventFilter.cpp）：主页回车启动。
    /// 返回 true = 这一发归窗口处理掉了（调用方把事件吃掉）。
    bool handleHomeEnter();
    /// F5：点当前最上层可见页面上那颗刷新按钮（`Theme::markRefreshButton()` 认人）。
    void refreshTopmostPage();
    /// 启动中按 Esc：游戏进程还没起来时走 `cancelLaunch()`（和点卡片上那颗取消同一条路）。
    /// 浮层/弹出层/下拉开着、或进程已经起来时返回 false（这一发不归窗口）。
    bool handleEscCancelLaunch();

    // ---------- 「世界设置」那两格状态----------
    //
    // 这两条**必须落在 `private:` 里**，跟上面那一堆"世界设置"的函数（它们在
    // `private slots:` 那一段，因为 MainWindow 的槽都挤在那儿）分开：moc 只认小节里的
    // **函数声明**，一条数据成员摆在 `slots:` 底下会当场报
    // `Not a signal or slot declaration`（编译都过不去）。
    // / 世界设置那一格现在看的是哪一个存档目录（**空串 = 没开着**）。
    // /
    // / NOTE: 存这一份而不是每次去问对话框：那一页上"保存"那一发信号**不带任何值**
    // / （见 WorldSettingsPage::saveRequested()），写哪个世界只有这里知道。
    QString m_worldDir;
    /// 打开世界设置那一刻盘上 `level.dat` 的大小 / 修改时间 ——"这中间别人动过没有"
    /// 那道闸比的就是这两样（和 `ConfigFileText::Stamp` 是同一件事，只是拆开存）。
    ///
    /// NOTE: 这里为什么**不直接存一个 `ConfigFileText::Stamp`**：那个类型在 `ui/` 底下，
    /// 而这个头文件一条 `ui/` 的 include 都没有（整个 `src/` 只在 .cpp 里往 `ui/`
    /// 看，见 CMakeLists 里那两行 include 目录）。为了一个"记着大小和时间"的小结构
    /// 破这个例不值得 —— `mtimeMs == 0` 就是 Stamp 那边 `valid() == false` 的意思
    /// （文件在打开的那一刻就不在）。
    qint64 m_worldStampSize = 0;
    qint64 m_worldStampMtimeMs = 0;

    // ---------- 下载页后面那几页：搜索 + 三层资源表 ----------
    //
    // 整合包 / 模组 / 数据包 / 资源包 / 光影这几页走的是**同一条路**，只有"搜哪一类"不同，所以
    // 实现是一份、按 ResourceCatalog::Kind 分。这条路是：
    // 进页面 → 搜一次（空搜索词 = 这一类的热门）→ 摆第一层**资源**
    // → 点开一个资源 = 摆第二层**游戏版本**（**不用再问接口**：搜索响应里本来就带着
    // "这个资源支持哪些游戏版本"）
    // → 点开一个游戏版本 = 这时才去问 → 摆第三层**版本号**
    // → 点一个版本号 = 下它、装进实例
    //
    // NOTE: 这几个刻意**不是**槽：参数里带着 ResourceCatalog 那几种类型，声明成槽就得
    // 让 moc 生成一套元调用，而这条路全都用新式 connect（函数指针）连，不需要它。
    // / 进这一页时调：**只有第一次进才搜**（树上已经有东西就不动 —— 用户来回换分类
    // / 不该每次都重新问，那既慢、又是在拿限流额度换同一份东西）。
    void ensureResourcePageLoaded(ResourceCatalog::Kind kind);

    /// 下载页左栏某个分类条目右边那个刷新按钮被点了一下（每一项右边都有这一颗）。
    /// `row` 是分类列表的行号（**和 downloadCategoryKeys() 同一个下标**）。
    ///
    /// 它办的事按分类分两种（见实现里的注释）：
    /// · 「游戏」→ 重新拉一次**在线版本清单**（那棵版本树四条分类的内容就是它）；
    /// · 其余几类 → 拿搜索框里现在那个词**再搜一次**（和按回车是同一件事）。
    /// 两种都**不动图标缓存**：重新摆出来的那批条目走的还是 ResourceStore::requestIcon，
    /// 命中内存/盘上那份就一个请求都不发（图标要留缓存）。
    void refreshDownloadCategory(int row);
    /// 按那一页搜索框里的词搜一次（回车时，或者刚进页面时那句空的）。
    void startResourceSearch(ResourceCatalog::Kind kind);
    /// 树上"这一行被激活了"（鼠标单击、双击的第一下、键盘回车）—— 接线口见
    /// ui/RowActivation.hpp：一次手势只进来一次，所以这里**不需要**再考虑重入。
    void onResourceItemActivated(QTreeWidgetItem *item);
    /// 第三层那一下：把这个版本下回来、装进实例。
    void downloadResourceVersion(QTreeWidgetItem *item);
    /// 真正下它的那一段（上面那个树版本、和详情页那一行，两条路都走到这儿）。
    ///
    /// 树那一条把身份、游戏版本、清单都从条目上读出来再调它；详情页那一条直接给值
    /// （那一页没有树条目）。**加载器那道闸、「没有可下载的文件」那句话、整合包下完接导入
    /// 那笔登记**全在这一个函数里 —— 两条路必须是同一份规矩，抄一份迟早抄岔。
    ///
    /// @param kind 是哪一类资源（决定落到哪个目录、要不要过加载器那道闸）
    /// @param displayTitle 界面上这一个资源叫什么（下载 label 的前半截，另一半是版本号）
    /// @param version 要下的那一版（`primaryFile()` 是它的文件清单）。
    /// **按值收**，不是按引用：两条来路递给它的都是容器里的元素（下载页那张
    /// `m_resourceVersions` 的 QHash、详情页的 `m_resourcePage.versions`），而它
    /// 下面会开嵌套事件循环（"存哪儿"的 `QFileDialog`、加载器那道闸的
    /// `InlineMessage`）—— 那一期间任何一发回信插进那张 QHash 都可能让它 rehash，
    /// 引用当场悬空。按值收 = 进门先拷一份，容器怎么变都不关它的事。
    /// @param source 这一条资源是**从哪一家搜出来的**（前置也去同一家问 —— 两家的项目号
    /// 不是一套）。★ 来源必须跟着走：前置要问"那个号叫什么、有哪些版本"。
    void installResourceVersion(ResourceCatalog::Kind kind, const QString &displayTitle,
                                ResourceCatalog::VersionEntry version,
                                ResourceCatalog::Source source);

    // ---------- 资源详情页----------
    //
    // 下载页选中一个模组后进入一整页详情：上方写模组图像、名字、来源，并有转到
    // modrinth/curseforge/mcmod.cn 的选项，中间一排版本，下方排列版本。
    // 「独立界面」落成**整页覆盖层**（`OverlayHost::showPage`，不另开窗）。
    //
    // 界面那一层（ui/ResourceDetailPage）一个字节都不上网，进参出参全是值；下面这四个
    // 函数是它和业务层之间那层皮。
    void setupResourceDetailPage();
    /// 进下载页那条路最多允许**再排几轮**去把实例设置那一页收干净（见
    /// `continueOpenDownloadCategory()`）。3 是个宽裕的上限：正常情况第一轮就已经收干净了
    /// （那一发 `dismiss()` 的收尾在下一轮事件循环之前就落定），只有"上面还压着一条提示"
    /// 那一档才真用得上第二轮。**不写"转到收干净为止"**：收不掉时再转多少轮也收不掉
    /// （那一页收着 = `dismiss()` 不放行），转下去只是空转。
    static constexpr int kDownloadOpenRounds = 3;
    /// `runWhenOverlayFree()` 最多再排几轮（同一个量级、同一条理由 —— 见上面那段）。
    /// 3 是宽裕的上限：正常那一档第一轮就开出来了（本来就没人占着），被占着时第二轮也
    /// 一定见得着空的那一层；只有"上面还压着一条提示"才真用得上第三轮。
    static constexpr int kOverlayOpenRounds = 3;
    /// 这一页是**怎么打开**的。只有一处需要知道：右上角那一格摆什么
    /// —— 从实例的「文件管理」页点一个**模组**进来时摆那颗「模组设置」，
    /// 其余一律照旧摆「显示快照版」。
    ///
    /// 为什么单独立一个枚举而不是让调用方自己去 `setModSettingsSlot()`：那一页是
    /// **同一个对象、建一次留着**的，两个入口共用它 ⇒ "摆什么"必须在**每次开页都显式
    /// 摆一次**，而摆的地方只能是这里（`showPage` 之前）。写成枚举参数的话，**下一个入口
    /// 一加进来就编译不过**，不会悄悄沿用上一次那一格。
    ///
    /// 模组点进来的详情页和资源包差不多，但右上角「显示快照版」那里换成「模组设置」
    /// （先占位）；下载页那条路永远是 `Download`（照旧摆「显示快照版」），这一位不许
    /// 跟着种类走。
    /// 枚举本体在 `ResourcePageController` 里（那一页是唯一读它的地方）；这里留一个
    /// 别名，主窗口里那两处调用点（`DetailSource::Download` / `InstanceFiles`）不用改。
    using DetailSource = ResourcePageController::DetailSource;
    /// 打开这一条资源的详情页（`iconKey` / `kindKey` 是条子上那两个角色）。
    ///
    /// 摆上去就返回（`OverlayHost::showPage()`），**不卡调用方**。`onClosed` 在这一页
    /// 收掉时叫一次，参数和原来 `execPage()` 的返回值同一套：**返回箭头和 Esc 都是
    /// `QDialog::Rejected`**，`Accepted` 是页上那颗「模组设置」。
    ///
    /// @return 这一页**摆上去了没有**。`false` = 没开出来（覆盖层正忙 / 那一行已经
    /// 不在清单里），调用方按它决定"要不要把来路放回去"（见 `openFileResourcePage()`）。
    ///
    /// NOTE: **仍然必须排在事件投递之后**再调（调用点走 `QTimer::singleShot(0, …)`）：
    /// 调用它的地方（树的事件过滤器）正处在一发鼠标事件的投递途中，视图那一侧的状态机
    /// 还没落地 —— 和 `openPage()` 那条"排到下一轮再开"是同一个理由。
    /// @param instanceId 非空 = 这一页是**从那个实例进来的**：那一排 chip 会预选到那个实例
    /// 的游戏版本上，而版本组里那个实例的加载器排在前面（进入时版本自动选到当前实例
    /// 对应的那个，对应的模组加载器的版本放前面，资源包等同理，展示为对应版本适用的）。
    /// 空 = 不预选（照旧停在「全部」那颗上）。
    ///
    /// NOTE: `from` 这一位**不是可选的**（不给默认值）：加一个新入口时忘了说"是不是从某个
    /// 实例来的"，编译就过不去 —— 和上面 `DetailSource` 那条设计理由逐字相同。
    /// `onClosed` 可以不给（下载页那条路不关心结果）。
    bool openResourceDetailPage(const QString &iconKey, const QString &kindKey, DetailSource from,
                                const QString &instanceId,
                                std::function<void(int)> onClosed = {});
    /// 详情页上方那个「显示快照版」勾选框被拨了（点它、或者按 Alt 键）。
    ///
    /// 它换的是**那一排 chip 的表**（折着的那一份 ↔ 原样那一份），所以三件事一起做：
    /// 存下这一位、把 chip 那一排整个换一份（按着的那一颗换算成同一大版本在另一档里的
    /// 说法）、再按新的这一份重算筛 + 分组。**不发请求** —— 版本列表是开页时就整份取回来
    /// 的，折与不折都是在本地的这一份上算。
    /// 把那一排 chip 摆成「这一份声明表 + 这一档（折 / 不折）」该有的样子：第一颗是
    /// 「全部」，其余按 `gameVersionChoices` 摆；@p wanted 是要按下去的那一颗（表里没有、
    /// 或者给的是空串，页面自己会挑「全部」）。
    ///
    /// **两个调用点**（开页、拨勾选框）共用这一份，免得两处各拼一遍、拼出两种表来。
    /// NOTE: 做成"摆到页面上去"而不是"返回一份表"，是为了让这个头**一个 ui/ 的类型都不用
    /// 带进来**（这一整个头对 ui/ 那边只按指针持有，见上面那几行前置声明）。
    /// 按当下这颗 chip 从**全量**那份清单里筛出来、分组、摆进页面（空的那一对也走它）。
    void fillResourcePageGroups();
    /// `m_projectTitles` 那把键（`Hit::iconCacheId()` 那个拼法 —— `<来源>-<项目号>`，
    /// **唯一出处是它**，别在这儿再拼一遍字符串）。
    static QString projectTitleKey(ResourceCatalog::Source source, const QString &projectId);
    /// 把一发问回来的名字写进 `m_projectTitles`。**名字空的那几条不写**（写空串等于把
    /// "还没问过"说成"问过了、它没名字"，而下一个查它的人就再也不去问了）。
    void rememberProjectTitles(const QList<ResourceCatalog::Hit> &hits,
                               ResourceCatalog::Source source);
    /// 详情页上点了某一个具体版本（`Row::key` = `VersionEntry::id`）= 下它。

    // ---------- 前置模组：装一个模组时带上它要的那几个 ----------
    //
    // 那一整趟（缺哪些 → 问名字 → 问版本 → 弹框 → 并成一批下）连同它那一份状态
    // （`DependencyCheck`）都在 `controllers/ResourceInstallController`，理由与形状见
    // 那边头文件第一段。这里留下的只有 `installResourceVersion()` 一条薄壳 ——
    // 详情页与搜索页按名字叫它。
    //
    /// 资源下载目标的初始选择：详情页的来路、下载页的来路、主页当前实例。
    QString resourceDownloadDefaultInstanceId() const;

    /// "这个前置要装进哪个实例"：游戏版本 + 加载器 + 来源（`ModDependencies::Target`）。
    /// 加载器名走 `instanceLoaderName()`（PCL 那类合并实例只有它能读出来，见那一处）。
    ModDependencies::Target dependencyTargetFor(const QString &instanceId,
                                                ResourceCatalog::Source source);
    /// 这个实例的 mods 里**已经认出来的**那些资源（键 = `<来源>-<项目号>`）。
    ///
    /// 这份判据的**强度就是"这个会话认过多少"**：那张"这一条是哪个资源"的表是文件管理页
    /// 那一趟反查填的，没进过那一页就是空的（表在 `InstanceIconController` 里，
    /// 搬过去的）。所以多出来一个 @p known 出参：**空表**和"确实一个都没装"是两件事，
    /// 界面上那两句话不一样（见 `known` 的用法）。
    QSet<QString> installedModKeys(const QString &instanceId, bool *known) const;
    // ---------- 实例更新检测----------
    //
    // 实例设置那一页带更新检测。判据只有一条：盘上那个文件的 sha1 落在
    // 项目版本列表的第几条上（见 `ModDependencies::judgeUpdate()`）。
    //
    // 【为什么要先反查一遍】
    //
    // 盘上那些 jar 是**谁**、**哪一版**，只有反查说得出来（`InstanceIconLookup`：哈希 →
    // 项目号 → 项目对象，同一趟顺手把 sha1 带出来，见 `Found::sha1`）。所以这一趟的形状是
    // "枚举 mods → 反查一次 → 有几个项目就再问几发版本列表 → 判"，**全在后台线程**吗？
    // 不是：版本列表那几发走的是主线程上的 ResourceStore（界面这一层本来就等结果），
    // 只有"读几十兆 jar 算哈希"那一段在后台线程（沿用 `startInstanceFileIconLookup` 那套）。
    void checkInstanceUpdates(const QString &instanceId, InstanceSettingsDialog *dialog,
                              const QStringList &selectedPaths = QStringList());

    /// 把"这个实例**上一趟查出来可更新**的那些文件"递给那几页（见
    /// InstanceFilesPage::setUpdateResult）。@p dialog 是**现在开着的**那一页（空 =
    /// 没开着，什么都不做）。
    ///
    /// 从两处喊：①开这一页的时候（拿下面那份会话缓存 —— 用户查过一次之后关掉再进来，
    /// 「可更新」还得筛得出东西）；②一趟检测收尾时（这时候才刚有结果）。
    void pushInstanceFileUpdates(InstanceSettingsDialog *dialog, const QString &instanceId);
    /// 反查那一趟回来了（更新检测这条路上的收尾：逐个项目问版本列表）。
    void onUpdateCheckLookupFinished(const InstanceIconLookup::Result &result,
                                     const QString &instanceId);
    void onUpdateCheckVersionsFinished(const QString &requestKey,
                                       const QList<ResourceCatalog::VersionEntry> &versions,
                                       const QString &problem);
    /// 发那一发"问这个项目的版本列表"（**一把钥匙一发**）。
    ///
    /// 单独一个函数的缘由是它有**两个调用点**：反查那一趟回来时对 `itemsByKey` 里每把钥匙
    /// 发一遍，以及批量那一发回来时给"退回问 CurseForge"的那些补发（见
    /// `UpdateCheckRun::fallbackItems` 那段）。两处要发的东西是逐字同一句（项目号得从
    /// `<来源>-<项目号>` 那把 `iconKey` 上读），抄第二份迟早有一边忘了带上来源。
    void requestUpdateCheckVersions(const QString &requestKey);
    /// **批量那一发**回来了（`POST /version_files/update` —— Modrinth 认出来的那一拨，
    /// 见 `UpdateCheckRun::batchItems` 那段）。判完之后和单发那条路走**同一个收尾**
    /// （`finishUpdateCheck()`）。
    void onUpdateCheckLatestFinished(
        const QString &requestKey,
        const QHash<QString, ResourceCatalog::VersionEntry> &latest, const QString &problem);
    void finishUpdateCheck(QString instanceId);

    // ---------- 更新中心 ----------
    //
    // 要做的是：把实例概览已有的"检查更新"结果做成可操作列表；支持逐项/多选更新、忽略某个
    // 版本、固定版本；更新前创建轻量回滚点，失败时可恢复原文件；更新任务显示每个资源的结果，
    // 不只显示总数；禁用资源保持跳过，不擅自启用或更新。
    //
    // 【为什么是一页、不是一个弹框】
    //
    // 一段文本（`InlineMessage::information`）关掉就没了，更不能动手。而逐条更新 / 忽略 /
    // 固定 / 撤销四件事全都得**看着那一行**按下去 —— 文本上没法按。所以是
    // `ui/UpdateCenterPage`（形状照完整性报告页那一套）。
    //
    // 【判据在哪一层】
    //
    // "这一条现在要不要提示"（忽略 / 固定两档）**不在界面里判** —— `src/manage/UpdatePolicy`
    // 是唯一出处，界面只画它算好的那一档（见那一页文件头那段）。回滚那一层同理
    // （`src/manage/UpdateRollback`）。
    // / 开「更新中心」那一页（`OverlayHost::showPage`，收在内容区里）。
    // /
    // / @param instanceId 看哪个实例的结论。**必须传**（不是"当前选中的那个"）：这一页可以从
    // / 实例设置那一页点进来 —— 那时候"选中的实例"就是它，但也可以从底栏 / 主页绕进来，
    // / 那时候去猜一个实例是错的。
    void openUpdateCenter(const QString &instanceId);
    /// 把上面那一页要的那份 `Report` 造出来 —— **唯一翻译点**（和 `integrityReportForPage()`
    /// / `taskCenterItem()` 是同一个路数）。忽略 / 固定那两档在这一处算好，页面自己不再判。
    UpdateCenterPage::Report updateCenterReportForPage(const QString &instanceId) const;
    /// 按**现在**这本账把"这个实例哪几条要更新"重算一遍，写回 `m_instanceOutdatedPaths`，
    /// 并把开着的更新中心那一页重摆一次。忽略 / 固定一动就要走它 —— 名单和界面必须**同一次**
    /// 动，不然用户会看到"已经忽略了、但计数里还算着它"。
    ///
    /// NOTE: 上面这四条都是**壳**，实现在 `controllers/UpdateCenterController`（那一页的全部
    /// 状态也在那儿：这本账、页面对象、开页时记下的实例 id）。留着名字是因为检测收尾那条路
    /// 还要按主窗口叫它们（`InstanceUpdateCheckController::finishUpdateCheck()`）。
    void rebuildInstanceOutdatedPaths(const QString &instanceId);
    /// 那一本账写回盘（`<数据目录>/wtfml.updates`）。写不成**只写日志**：用户按的是一颗
    /// "以后别提示我"，弹一个"存不进去"的框除了让他分心以外没有下一步可做（那一份记录在
    /// 这一次会话里仍然是生效的）。
    bool saveUpdatePolicy();
    /// 某一批「更新」下完之后那一行结果（逐条）。**每个资源一条**——
    /// 下载那一侧的那个信号收在这里，翻成任务账本里那一步。
    void onUpdateBatchFileFinished(const QString &label, const QString &fileName, bool success,
                                   const QString &errorMessage);
    /// 这一批「更新」收尾了（全下完了 / 半路失败）。成功的那几条要从"可更新"名单里摘掉，
    /// 并按**每个资源的结果**写一行日志 + 一份能看的清单。
    void finishUpdateBatch(bool success, const QString &errorMessage);

    // ---------- 文件管理那几页的批量按钮----------
    //
    // 选中任意数量模组时，列表下方（悬浮，不是表尾）显示几颗按钮：
    // 更新 禁用 启用 全选 反选 取消 删除。全选 / 反选 / 取消三颗**全在界面那一边**
    // （它们只动选中集，一个字节的盘都不碰），所以只有三件事落到这一层：
    // 更新 / 禁用·启用一把 / 删除一把。
    //
    // 【为什么「更新」是一批下、而不是一条一条走 installResourceVersion()】
    //
    // **做不到一条一条**：`VersionManager::downloadResourceFiles()` 头一道闸就是
    // `downloadManager->isBusy()`（见那儿那句 qWarning），第二发会被**直接拒掉**并回一句
    // "已有下载任务正在进行"。也就是说那一圈里只有第一条真下得动，剩下 N-1 条全是失败弹框。
    //
    // 所以更新走的是一整批，前置检查也留在外面 —— 那是**刻意**的：前置是在**装**的时候
    // 要解决的事，而这一颗按钮动的是**已经装好的**那几条（它们当初装进来时前置就过过一遍
    // 了）。真要补前置，用户走的是下载页那条路（那儿有那个框）。
    // / 「更新」：把这一批路径各自换成一趟检测查出来的**最新那一版**，并成**一批**下。
    // /
    // / ★：这一批是**就地换**的 —— 每一条落回**它自己原来那个目录、
    // / 用原来那个文件名**（`ResourceFile::dir` / `filename` 都从盘上那一条算）。
    // / **不能**按接口给的文件名下到"这一页那个目录"里：两家接口给的文件名常常和盘上那个
    // / 不一样（`sodium-fabric-0.6.13.jar` vs `sodium-0.6.13.jar`）⇒ 换完之后**多出一个
    // / jar、旧的还在**，而且"更新"按几次就多几份。
    // /
    // / @param instanceId 哪个实例（`m_instanceUpdateTargets` 的键）。
    // / @param pageKey `instanceFileDirs()` 的键（`mod` / `resourcepack` / `datapack` /
    // / `shader`）—— 界面那边只说"是哪一页"。**空串 = 不是从某一页来的**（更新中心
    // / 那一批是跨类勾出来的），那时候落点全按每条自己那个目录算，不做校验。
    void updateInstanceFiles(const QString &instanceId, const QString &pageKey,
                             const QStringList &paths);
    /// 「禁用」/「启用」：@p disable 为真 = 禁用。**只动该动的那几条**（选中集里本来就
    /// 混着已启用和已禁用 —— 灰化规则是"至少有一条能切"，不是"全都是同一种"）。
    void toggleInstanceFiles(const QStringList &paths, bool disable);
    /// 「删除」：**只问一次**，然后逐条落盘。
    void deleteInstanceFiles(const QStringList &paths);
    /// ResourceStore 的三个回复。
    void onResourceSearchFinished(const QString &requestKey,
                                  const ResourceCatalog::SearchResult &result);
    void onResourceVersionsFinished(const QString &requestKey,
                                    const QList<ResourceCatalog::VersionEntry> &versions,
                                    const QString &problem);
    void onResourceIconReady(const QString &iconId, const QPixmap &icon);

    /// 从请求钥匙反查是哪一类（钥匙是 ResourceCatalog::searchRequestKey() 拼的）。
    /// 拼和认都走那一个函数，所以这里不解析字符串。
    ///
    /// `outSource` 收的是"这把钥匙是发给哪个来源的"（可为空指针）。**回来的结果必须看它**：
    /// 用户可能在等结果的这段时间里把来源换成另一个，两个来源的请求是各自飞的，不看它就会
    /// 把上一家那批摆在一个写着另一家的页面上（见 onResourceSearchFinished）。
    bool kindForSearchRequestKey(const QString &key, ResourceCatalog::Kind *out,
                                 ResourceCatalog::Source *outSource = nullptr) const;
    /// 这个条目在第几层（0 = 资源 / 1 = 游戏版本 / 2 = 版本号；不是树上那三层就回 -1）。
    ///
    /// NOTE: **唯一出处**。代理里认层用的是"有没有爹"（isChannel），这里认层用的是
    /// "往上数几层"——两处必须说的是同一件事，而这句话在这里写一次。
    static int resourceRowDepth(const QTreeWidgetItem *item);
    /// 摆一行**占位**（"正在搜索…"、"没有搜到…"、"这个游戏版本上没有可用的版本"）。
    /// 占位行的定义就是**没有身份**（UserRole 空）—— 点它什么都不会发生，见
    /// onResourceItemActivated() 开头那个判据。树那一版加在顶层，子那一版加在某一条底下。
    static QTreeWidgetItem *addResourcePlaceholderRow(QTreeWidget *tree, const QString &text);
    static QTreeWidgetItem *addResourcePlaceholderChild(QTreeWidgetItem *parent,
                                                       const QString &text);
    /// 把搜索结果摆进第一层（每条底下顺手把第二层的游戏版本一起建出来）。
    /// `kind` 要写进每一条的身份里（MainWindowUI::resourceKindRole()）—— 点第三层那一下
    /// 靠它算"下到哪儿去"（见那个函数的注释）。
    void fillResourceHitRows(QTreeWidget *tree, ResourceCatalog::Kind kind,
                             const ResourceCatalog::SearchResult &result);
    /// 把版本列表摆进第三层（先清掉"正在获取…"那一行）。
    void fillResourceVersionRows(QTreeWidgetItem *gameItem,
                                 const QList<ResourceCatalog::VersionEntry> &versions,
                                 const QString &problem);

    // ---------- 每一页顶上那一行筛子----------
    //
    // 说明见 MainWindowUI 里 `downloadFilterVersionComboName()` 上面那段。界面那一层
    // 只把**空框**搭出来（ui/ 不 include src/，见那边的文件头），所以"每一项是什么字、
    // 是什么键"全在这儿灌：来源是 `ResourceCatalog::allSources()`、类型是 `categoryOptions()`、
    // 模组加载器是 `loaderOptions()`、版本是 VersionManager 那份在线清单。
    //
    // 装配是**两个方向**的，别混：
    // · 往下（灌）：`fillDownloadFilters()` 一次把五页都灌满；来源一变、
    // 版本清单一到，再灌一次（`refreshDownloadFilterOptions()` / onVersionsFetched）。
    // · 往上（读）：`downloadFilterXxx()` 那四个访问器是**唯一出处** ——
    // startResourceSearch() 和"清空条件"都从它们读，别在各处 `currentText()` 一遍
    // （读法不一致的症状是"界面显示筛了、发出去的请求没筛"，而且不报错）。
    // / 把五页的筛子都灌一遍（构造函数里一次）。来源 / 类型 / 加载器那三项是**灌的时候**
    // / 按来源算出来的，所以这里也顺手把"类型那一栏要不要藏"定下来。
    void fillDownloadFilters();
    /// 灌**某一页**上跟着"来源"变的那两项（模组加载器 / 类型），并把类型那一栏按
    /// `categoryOptions()` 有没有回空表决定画不画。用户在来源框里换了一下就调它。
    ///
    /// NOTE: 它**自己不搜索**（这一层只管把选项换一份）。"换完就按新来源搜一发"接在
    /// 构造函数里那条 `activated` 上 —— 「下拉即搜」这条规矩在那一处收口。
    /// NOTE: 换来源会把这一页的加载器/类型两项**退回「全部」**：两边的键根本不是一套
    /// （Modrinth 的 `fabric` vs CurseForge 的 `4`），留着上一个来源的选中项就是
    /// 拿 A 家的号去问 B 家 —— 而且它不报错，只会搜出个空的。
    void refreshDownloadFilterOptions(ResourceCatalog::Kind kind);
    /// 把那一份**在线版本清单**灌进五页的「版本」框（构造时一次、清单一到又一次）。
    ///
    /// NOTE: 这一格是**可输入**的，所以灌的时候要把用户手里那串字原样放回去（见实现）——
    /// 清单是异步回来的，把它当成"重新填一遍"就会清掉他刚打的版本号。
    void fillDownloadFilterVersionOptions();
    /// 这一页现在选的来源（框里没有值 / 认不出来 = Modrinth，和"没这一栏"时一致）。
    ResourceCatalog::Source downloadFilterSource(ResourceCatalog::Kind kind) const;
    /// 这一页现在选的游戏版本（**框里的字**：这一格是可输入的，用户自己打的版本号
    /// 不在列表里，读 `currentData()` 会读到一个空 —— 那正是"看着筛了、实际没筛"）。
    /// 空串 = 不筛。
    QString downloadFilterGameVersion(ResourceCatalog::Kind kind) const;
    /// 这一页现在选的模组加载器。**只有模组那一页有这一栏**（别的页连控件都没有，
    /// 见 MainWindowUI 里那段），所以别的页回空串 = 不筛。
    QString downloadFilterLoader(ResourceCatalog::Kind kind) const;
    /// 这一页现在选的类型（键来自 `categoryOptions()`）。空串 = 不筛。
    QString downloadFilterCategory(ResourceCatalog::Kind kind) const;
    /// 「清空条件」那颗键：名称框和四个筛子都回到"不筛"。**这一层只清、不搜** —— 搜是
    /// 调用方的事（那颗键上接着"清完重搜一发"，见构造函数里那段接线）。
    void resetDownloadFilters(ResourceCatalog::Kind kind);

    // ---------- 「设置」选项卡 ----------
    // / 把设置对象里的值灌进设置页的控件。构造函数里调一次；**不要**在用户改动之后
    // / 再调它 —— 那会把用户正在编辑的东西顶回盘上的旧值。
    void applySettingsToUi();
    /// 扫一遍这台机器上装了哪些 Java，结果进 m_localJavaVersions。
    ///
    /// NOTE: 这一个是**无条件**重扫。要跑几次 `java -version`（每次几十到几百毫秒），
    /// 所以"切到设置页顺手扫一下"那条路走的是下面那个带节流的 refreshJavaList()。
    void scanLocalJava();
    /// 接收后台扫描结果，并把用户手动钉住但不在常规目录里的 Java 合并进去。
    void applyDetectedJavaVersions(const QMap<int, QString> &versions);
    /// 第一次切到设置页时扫一次（一次会话里只扫这一遍）。加节流是为了不在启动路径上
    /// 加这段延迟 —— 用户专门来「Java 管理」那一页看的时候不走这条，那边要的是当场的
    /// 真实结果（见 openJavaManage）。
    void refreshJavaList();
    /// 把「Java 版本」那一行下面的说明文字刷成**当前设定**：手动/自动两档 × 路径有值/
    /// 无值，四种说法。它说的是"设定了什么"，不是"这次启动一定会用哪个" —— 自动档下真正
    /// 挑中的 Java 要到启动那一刻才知道（所以文案写的是"上次使用…"）。
    void updateJavaPathLabel();
    /// 选项卡切换：底栏左下角要不要显示实例名、以及要不要扫 Java，都在这里收口。
    void onMainTabChanged();
    /// 开「Java 管理」那一页（设置页「启动」栏那颗按钮），打开后在后台重扫一遍盘。
    void openJavaManage();
    /// 把 m_localJavaVersions 里的四个主版本灌进那一页。
    void pushJavaRowsToPage();
    /// 那一页发上来的三件事；实际扫描 / 下载由 JavaRuntimeController 在后台完成。
    void onJavaDetectRequested();
    void onJavaDownloadRequested(int major);
    void onJavaDownloadAllMissing();
    /// 「选哪个 Java」那两颗（设置页那个下拉框的语义落在这一页上）。
    /// 走的是同一对 setter + save()，所以盘上的格式一个字都没变。
    void onJavaUseAuto();
    void onJavaUseRuntime(int major);
    /// 把「内存」那一行画成当前状态：条和数字框显示盘上存的那个数，开窗/切页都**不重算**，
    /// 旁边写那句"这台机器
    /// 有多少内存、这个数怎么改"的说明。
    /// 调用点：开窗灌值时（applySettingsToUi）、切到设置页时、用户拖/填「最大内存」那一刻。
    void refreshMemoryUi();
    /// 为什么上限得由业务层喂给界面：`ui/` 不许依赖 `src/`（那条规矩），界面里那个
    /// 65536 是"单独搭出来时"的天花板，不是这台机器的天花板 —— 一台 16 GB 的机器上让它
    /// 能拖到 65536，用户拖到那儿只会在启动时拿到一句 JVM 的内存分配错误。
    ///
    /// NOTE: 它**只改上下限、不动值**（信号也挡着），值由调用方紧接着灌（见 refreshMemoryUi）。
    /// NOTE: 真要算"这台机器该给到多少"的是 MainWindowUI::memoryCapForTotalMb() —— 实例设置
    /// 那一页也要按同一个上限收口（见 InstanceSettingsDialog 里那段），所以算法只有
    /// 那一份，这里只是把它贴到设置页那两个控件上。
    void applyMemoryLimits(qint64 totalMb);
    /// 让「最小内存 ≤ 最大内存」这条一直成立 —— JVM 会拒绝相反的组合：
    /// Error occurred during initialization of VM
    /// Initial heap size set to a larger value than the maximum heap size
    /// 界面上**没有**最小内存那一格，替它把门的就是这里：
    /// 凡是改到 Xmx 的地方都得过一下。@return 夹下来了没有（调用方据此决定要不要落盘）。
    bool clampXmsToXmx(int xmxMb);
    /// 启动前把这次真正要用的内存定下来，**只改传进来的那份值**。
    ///
    /// 自动档下按**现在**的系统内存重算一次（见 refreshSuggestedMemory 那段注释：
    /// 同一台机器开着浏览器和什么都没开能差好几 GB）。
    ///
    /// NOTE: 重算的结果**只写在传进来这份值上**，两个作用域都不落盘：
    /// - 自动这一档来自全局 → 走 refreshSuggestedMemory()，它自己会写全局（那正是"全局是自动档"的意思）；
    /// - 自动这一档是这个实例自己要的 → 现算一份给这次启动用，全局一个字节
    /// 都不动（实例 A 开着自动，不该改到全局那份内存配置）。
    /// 反过来"顺手写回实例的 meta"也不行：那会给它留下一对死值，下次读出来
    /// 只会让「实例设置…」里那两行显示成"自定义 + 一个用户没填过的数"。
    ///
    /// NOTE: 这是**唯一**该在启动路径上动内存的地方，而且它绝不改设置页：实例覆盖了
    /// 2048，设置页却显示 2048，用户会以为全局被改了。
    ///
    /// @param versionId 这是哪个实例 —— 自动档要拿它数一数装了多少模组（见下）。
    /// @param io 进参是合成好的有效值，出参是定下来的内存。
    /// @param override 这个实例的覆盖（用来判断"自动这一档是谁要的"）。
    ///
    /// NOTE: 数模组这件事**只有走到"这个实例自己要自动档"那一支才做**（全局那一支走
    /// refreshSuggestedMemory()，那里没有实例可分）。数一次是读一个目录，但那两处
    /// 调用都在启动/导出的同步路径上，能省则省。
    void resolveMemoryForLaunch(const QString &versionId, EffectiveJavaSettings &io,
                                const JavaSettingsOverride &override);
    /// 这个实例的 `mods/` 目录里有多少个模组（按 `*.jar` 数）。
    ///
    /// 只数 `*.jar`：禁用的模组在盘上是 `X.jar.disabled`（见 InstanceFilesPage 的
    /// identityPath），它**不会被游戏加载**，跟着算进内存建议里只会让数字虚高。
    /// 目录不在、读不了都返回 0（"不知道"和"一个都没有"在这里是同一个落点：不加成）。
    int instanceModCount(const QString &versionId) const;
    /// 这一次启动到底执行什么：**程序**（Java 可执行文件）+ **那一串参数**。
    ///
    /// 收在一个函数里是因为有两个消费者，而它们必须给出**逐字节一样**的答案：
    /// - onStartButtonClicked()：真的启动，把 arguments 交给 startGame()（program 那一格
    /// 由 LauncherManager 自己从 settings.javaPath 取，取不到就退 PATH 上的 java ——
    /// 和下面那个兜底是同一件事）。
    /// - exportLaunchScript()：把这两个字段写成 .bat（用户要的是"双击就按这套参数启动"，
    /// 导出一份和启动器**不一样**的命令行等于导出一份假的）。
    ///
    /// @param javaSettings 这次启动要用的那一套 Java 值（全局按实例覆盖合成之后的结果）。
    /// program 就是它里面的 javaPath（空 = 还没挑过，用裸 `java` 让 cmd 走 PATH）。
    /// @param offlineMode 这一趟走不走离线（判据只有一处：offlineIdentity()）。
    /// @param quickPlay 这一次要不要"开游戏直接进某个存档 / 直接连某台服务器"
    /// （`--quickPlaySingleplayer` / `--quickPlayMultiplayer`）。默认是**没有这一发**
    /// （`Target{}`）—— 导出启动脚本那一路就是这么调的（一个"直接进这个存档"的
    /// .bat 不是用户要的东西），真启动那一路从 `m_pendingQuickPlay` 取。
    ///
    /// NOTE: assetIndexId 与 javaMajorVersion 的兜底值（"29" / 21）也在这条路上算，见
    /// versionRuntimeIds()。它**不**去挑 Java、也不下运行时 —— 那两件事分别是
    /// ensureJavaForVersion 和 startGame 的事，都不属于"拼命令行"。
    QStringList launchArguments(const QString &versionId, const QJsonObject &versionJson,
                                const EffectiveJavaSettings &javaSettings, const QString &token,
                                bool offlineMode,
                                const QuickPlay::Target &quickPlay = QuickPlay::Target()) const;
    /// 用户在那个「Java 版本对不上」的框里选的哪一条路。
    enum class JavaMismatchAnswer {
        /// 没问（绝大多数启动都走这条：实例没钉 Java、总闸关着、这个实例表过态不再问、
        /// 或者那条路径压根探不出主版本）。
        NotAsked,
        /// 「仍然用这个 Java 启动」—— 照他钉的那个启动，一个字节的配置都不改。
        KeepInstanceJava,
        /// 「改用匹配的 Java 启动」—— 把自动挑中的匹配路径写回这个实例。
        UseMatchingJava,
        /// 取消 / 关窗 / Esc —— 不启动。
        AbortLaunch,
    };

    /// 启动前问一句「实例指定的 Java 和这个版本要求对不上，你要怎么办」。
    ///
    /// 四个前置条件全满足才问：
    /// ① 这个实例**覆盖了** javaPath（全局档不匹配时是静默换一个匹配的，那是有意的
    /// 行为，不动）；
    /// ② 全局那个总闸开着（GlobalJavaSettings::warnVersionMismatch()）；
    /// ③ 这个实例没表过"以后别再问了"（wtfml.meta 的 skipJavaVersionWarning）；
    /// ④ 那条路径探得出主版本、**而且**和这个版本要求的不一样。探不出（-1：路径不存在
    /// 或认不出来）不走这个框 —— "路径无效"和"版本不符"是两件事，前者仍然是现有的
    /// 硬错误。
    /// 代价只是一次文件读（probeJavaMajorVersion 的 release 快路径），所以绝大多数启动
    /// 在这里花的是微秒级。
    ///
    /// NOTE: 这一步只**问**，不动任何状态：值、meta、全局设置都由 onStartButtonClicked()
    /// 按返回的那个答案去改。唯一一处例外是"勾了以后不再提醒"这个长期偏好 ——
    /// 用户刚点的那一下，立刻落盘才对得上他的意思（而且写失败只记日志、不拦启动：
    /// 不该因为存不下一个偏好，把他刚授权的启动掐掉）。
    JavaMismatchAnswer askAboutJavaMismatch(const QString &versionId,
                                            const JavaSettingsOverride &override,
                                            int expectedMajor);
    /// 「实例指定的 Java 版本不符时提醒」那个勾选框。
    void onJavaWarnMismatchChanged();
    void onMemoryChanged();
    /// 「最大内存」那根条动了（见 .cpp 里那段：拖动中只跟着走数字、落盘留给松手那一下）。
    void onMemorySliderMoved();
    /// 「内存分配」那颗下拉框（自动分配 / 手动分配）。
    void onMemoryModeChanged();
    void onLauncherBehaviourChanged();
    /// 「语言」那一格（界面语言）：落盘 + 提示重启 + 可选「立即重启」。
    void onLanguageChanged();
    void onDownloadSourceChanged();
    /// 「下载源」那一组里那颗「并发数」下拉框。
    void onDownloadConcurrencyChanged();
    /// 「下载速度上限」改了：落盘 + 立刻推给下载器。
    void onDownloadSpeedLimitChanged();
    /// 「代理」那一档改了：落盘 + 立刻装到进程上，
    /// 同时把手动地址那一格按档位露/收起来。
    void onDownloadProxyModeChanged();
    /// 手动代理地址那一格改完了（回车或点到别处）：落盘 + 校验 + 立刻装到进程上。
    void onDownloadProxyManualChanged();
    /// 「下载新模组时直接放进当前实例」那一格（设置页「下载」栏）。
    ///
    /// 改了这一项**不影响已经发出的那一批下载** —— 落点在点下那一瞬间就定死了（见
    /// `ResourceInstallController::installNow()`）。这是刻意的：半批落在一个地方、半批落在另一个地方，
    /// 比"这次改的下一批才生效"难查得多。
    void onDownloadAutoIntoInstanceChanged();
    /// 「缺失前置提醒」那一格改了（见 LauncherSettings 里那一节）。
    void onWarnMissingDependenciesChanged();
    /// CurseForge 的 API key 那一格编辑完了（回车 / 焦点离开）。**不是** per-character：
    /// 半串 key 存进去只会换来一次 403，而用户会以为是启动器坏了（见 .cpp 里那段）。
    void onCurseForgeApiKeyEdited();
    // ---------- 「启动选项」那三项（设置页里同一组） ----------
    // / 「显示游戏日志」开关。关掉**不关已经开着的那些窗**（用户可能正在读崩溃信息）；
    // / 打开之后从下一行输出开始就会开窗（这是刻意的，见 showGameLogLine 的说明）。
    void onShowGameLogChanged();
    /// 设置页「每日一言」那个开关：立刻落盘，开着就按当前日期取一句（关着不发任何请求）。
    void onDailyQuoteChanged();
    // ---------- 「启动提醒」那两条（设置页里同一组） ----------
    // / 十秒内重复启动同一实例时要不要先问一句。改了这一项**不影响已经开着的那些游戏**，
    // / 也不清 m_lastLaunchAttempts —— 那个表只是"上次点启动是什么时候"，关掉开关不该
    // / 把它抹掉（再打开时那十秒窗口可能正好还没过）。
    void onWarnDuplicateLaunchChanged();
    /// 同一实例已经在跑时要不要提醒可能丢进度。
    void onWarnWorldLossChanged();
    void onProcessPriorityChanged();
    /// 「窗口模式」那个下拉框（小窗 / 最大化 / 全屏）。落盘之后会调
    /// updateGameWindowSizeEnabled() —— 档位决定了下面那两个数字框还管不管用。
    void onGameWindowModeChanged();
    /// 按当前窗口模式把「窗口大小」那两个数字框（以及中间那个 ×）灰掉/点亮。
    /// 「最大化」那一档不吃那两个数（见 GameWindowSettings.hpp），所以那一档是灰的。
    ///
    /// 灌值那一段也要调它一次：那边走的是有 QSignalBlocker 的 selectByEnumData()，
    /// 走不到 onGameWindowModeChanged() 里去。
    void updateGameWindowSizeEnabled();
    /// 「窗口大小」那两个数字框共用这一个槽（和内存那两个 spin 同一套写法）。
    void onGameWindowSizeChanged();

    // ---------- 「背景图」那一节（设置页「个性化」） ----------
    // 主页左栏和页面底各一块、各一行，
    // 动的是**主题规格**（`ui/Theme.hpp`），不是 LauncherSettings —— 见 .cpp 里那一段
    // 开头写的"改内存里那份 → 落盘 → 立刻铺开"这三步。
    //
    // NOTE: 驱动这一节的四条槽（`onThemeImageChooseClicked` 那一组）和 `refreshThemeImageRows()`
    // 都在上面 `private slots:` 那一段里 —— 它们本来就只被控件上的信号驱动，做成槽是
    // 为了验证工程能按名字把 `refreshThemeImageRows()` 调起来（见那一处的 NOTE）。
    /**
     * @brief 把某一块的图改掉，然后**落盘 + 立刻铺开 + 重画这一行**。
     * @param partKey `Theme::partKey()` 的结果（今天只有 "pageBackground"）。
     * @param imagePath 改完之后要铺的那张图（**绝对路径**）；空串 = 这一块不铺图。
     * @param imageName 用户挑的那张**原来叫什么**（换图那一发给 `QFileInfo(picked).fileName()`，
     * 拖滑块/换摆法那两发把上一份的名字原样带过来）。设置页那一行「现在铺的是：…」
     * 写的是它 —— 盘上那份叫 `pageBackground.jpg`，写那个用户等于什么都没看见。
     * @param fit `int(Theme::ImageFit)`。
     * @param opacity 0~100。
     *
     * NOTE: 四个值**都由调用方给全**，不读盘、也不从控件上捡：规格是整份写回去的，所以
     * "这一次改的是哪一项"必须说清楚（拖滑块那一路只改不透明度，另外三项得原样带过来）。
     * NOTE: `fit` 收的是 int 不是 `Theme::ImageFit` —— 这个头不 include `ui/Theme.hpp`
     * （那一层的头文件不许被 src/ 按值用进来，见本文件开头那段"前置声明 vs include"）。
     */
    void setThemePartImage(const QString &partKey, const QString &imagePath,
                           const QString &imageName, int fit, int opacity);
    /**
     * @brief 把一行游戏输出写进**那一份会话**的日志窗；窗不在时按需建一个。
     * @param sessionId 哪一份会话。<= 0（不挂在任何实例上的那句话）直接丢。
     * @param text 原样写进去的那段字。
     *
     * NOTE: 判据是"设置里那个开关现在是不是开着"，不是"启动那一刻开没开"。这样用户在
     * 游戏跑着的时候把它打开，从下一行输出起就能看见 —— 比"等下次启动才显示"更符合
     * 「显示游戏日志」这几个字的字面意思。代价是那一窗里没有这个开关打开之前的输出，
     * 这是**明说的**（见设置页那句说明）。
     *
     * NOTE: 用户手动关掉过的窗不会自己再冒出来（那一笔现在记在 `GameSessionController` 那一层）。
     * "我刚把它关了、你又给我弹回来"是这类窗最招人烦的失败模式。
     */
    void showGameLogLine(int sessionId, const QString &text);

    /**
     * @brief 把**这一份会话**的日志窗拿出来（没有就现建一扇）。建不出来时返回 nullptr。
     * @param sessionId 哪一份会话。<= 0 直接返回 nullptr。
     * @param userAsked 这一发是不是"用户点名要看"（运行中实例弹层里那颗「日志」）。
     *
     * 和 showGameLogLine() 的分工：那个是"来了一行输出，顺手写进窗里"，这个是"把窗本身
     * 弄出来"。`userAsked` 因此能在两道闸上破例（「显示游戏日志」开关、用户关过不再自动
     * 开），详见实现里那段说明。
     *
     * NOTE: 建窗时**顺手 show()**（自动那一路的第一行输出必须立刻可见）。用户点名那一路
     * 另外接 show/raise/activateWindow —— 见 openGameLogForSession()。
     */
    GameLogWindow *ensureGameLogWindow(int sessionId, bool userAsked);

    /**
     * @brief 用户点名要看这一场游戏的日志（运行中实例弹层那颗「日志」）。
     * @param sessionId 哪一份会话。
     *
     * 窗已经开着就只把它抬到前面（一个字都不重开）；会话还在跑而窗还没建就现建一扇；
     * 会话已经结束了就说清"它那份日志跟着没了"（那一份只活在窗里）。
     */
    void openGameLogForSession(int sessionId);

    /**
     * @brief 把**那一份日志窗里现在的内容**存成一个文本文件（底下那颗「导出日志…」）。
     * @param sessionId 哪一份会话。
     *
     * 问存到哪儿（系统那个保存框）、写、说结果，三件事都在这儿。`ui/` 那边只发一个信号
     * （理由见 GameLogWindow 的文件头：那一层不认识数据目录）。
     *
     * NOTE: 存的是**全部行**（`allLogText()`），不是屏幕上那一份：用户只勾着 ERROR 的时候
     * 导出，拿到的仍然必须是整份日志 —— "我明明导出了、打开只有三行"是这件事最坏的
     * 一种表现。
     */
    void exportGameLog(int sessionId);
    void exportGameErrorReport(int sessionId);

    void onApplyExtraJvmArgsClicked();
    /// 把「下载源」交给真正发请求的那一层（VersionManager → DownloadManager）。
    /// 启动时读盘、用户在设置页改，两条路都走这一句，别各写一遍。
    void syncDownloadSource();
    /// 把「同时最多下几个」（设置 → 下载 → 并发数）交给真正发请求的那一层。
    /// 和上面那条同一个形状：读盘和用户改都走这一句，别各写一遍。
    void syncDownloadConcurrency();
    void syncDownloadSpeedLimit();
    /// 把「代理」（设置 → 下载 → 代理）装到进程上（`QNetworkProxy::setApplicationProxy`）。
    /// 读盘和用户改都走这一句。
    void syncDownloadProxy();
    /// 把 CurseForge 那两项设置交给数据层（VersionManager）。同上：读盘和用户改都走这一句。
    void syncCurseForgeSettings();
    /// 启动成功后按「启动后」那一项处理启动器自己。
    void applyLauncherBehaviourAfterLaunch();

    /// 把一条消息交给 Qt 的日志系统（写进 `<数据目录>/logs/wtfml.log`，命令行里也能看）。
    /// 界面里已经没有日志面板了 —— 必须让用户看到的消息走的是 `InlineMessage`，不靠这一条，
    /// 见 .cpp 里的说明。
    void appendLog(const QString &text);
    /// 刷新底部状态条那三格（头像/用户名、**任务摘要**、实例运行状态）。
    /// 显示规则见 .cpp；界面那边只负责把控件搭出来。
    void updateBottomBar();
    /// 主页大标题那句问候（规则在 util/HomeGreeting）：名字和账号胶囊同一来源，没登录不带名字。
    /// 启动、账号变化（走 updateBottomBar）、应用重新激活时重算；文字没变不重画。
    void updateHomeGreeting();

    /// 当前所有运行中进程（客户端会话 + 服务端进程），按启动时间倒序。
    QList<RunningProcesses::Entry> runningProcessEntries() const;
    /// 底栏左下那一格（运行状态）：方块颜色 + 两行字，整个重画一遍。
    void updateBottomBarRunState();

    // ---------- 任务中心 ----------
    // / 底栏中间那格**现在该说什么**：把账本里那一堆翻成 `MainWindowUI::BottomBarTaskSummary`
    // / 摆上去（谁说话算数由 `Tasks::collapsedSummary()` 定 —— 那是账本层的纯函数，
    // / 这一格没有自己的一套优先级）。
    // /
    // / NOTE: 「刚完成的那一项还露着」这一档是**有期限的**（`taskSummaryRecentWindowMs()`），
    // / 所以这里顺带把那个到期重画的小定时器重新起一次。
    void updateBottomBarTaskSummary();
    /// 任务中心那块面板要画什么（点开/刷新时**现算**一遍，和底栏那两块面板一个路数）。
    /// 它有一处**副作用**：被调一次就算"用户看过了" ⇒ 把那几个没看过的失败标记清掉
    /// （面板里那几行是唯一能把失败原因看全的地方，"列出来了"就是"看过了"）。
    QList<TaskCenter::Item> taskCenterItems();
    /// **一项** ⇒ 界面那一份（`taskCenterItems()` 和任务详情页共用的唯一翻译点）。
    /// **没有副作用**（面板那个 `taskCenterItems()` 有"看一眼 = 看过了"那一笔，这个没有）。
    TaskCenter::Item taskCenterItem(const Tasks::TaskSnapshot &task) const;
    /// 任务详情页上的那一排按钮被点了（失败能看到失败阶段、简短原因和
    /// 『查看日志/重试』—— 那一页就是这句话落地的地方）。
    void onTaskDetailAction(const QString &key, TaskDetailPage::Action action);
    /// 任务中心里**某一行被点了** ⇒ 开那一页（`key` 是任务号的十进制字符串）。
    ///
    /// NOTE: 页面对象**建一次留着**（和资源详情页 / 实例设置同一个路数）：每一次点开都
    /// `setTask()` 重摆一遍。开页走的是**非阻塞**的 `overlay->showPage()` —— 这一句返回
    /// 的时候页已经摆出来了，收页由覆盖层自己收（见 `OverlayHost::showPage()`）。
    void openTaskDetailPage(const QString &key);
    /// 「取消」那一下（页上那颗、以及面板那一行行尾那颗）—— 转手给控制器，但**分派留在
    /// 这里**：服务端安装那一趟和版本/资源下载那一批不是同一份状态，怎么分派的判据和
    /// `cancelWouldWork()` 必须逐字对齐（见 .cpp 里那段 ★）。
    void cancelTaskFromDetail(const QString &key);
    /// 任务中心那一项启动任务变了 ⇒ 主页启动卡片跟着换。只跟最近那一次启动（多开时卡片只说最新的）。
    void syncLaunchCard(Tasks::TaskId id);
    Tasks::TaskId m_launchCardTask = Tasks::invalidTaskId();
    /// F5 那一条（见 `refreshTopmostPage()`）。
    QShortcut *m_refreshShortcut = nullptr;
    /// 「重试」那一下，同上（判据本体是 `retryWouldWork()`，和取消左右对称）。
    void retryTaskFromDetail(const QString &key);

    /// 「这一类活**起手**就允许什么」那张表（唯一出处，见 .cpp 里那段说明）。
    /// 会变的那几位（`canRetry`）不在这里 —— 它由 `finishTask()` 在失败那一刻补。
    /// 「这一项**此刻**按『取消』真的会做点什么吗」—— 和 `onTaskDetailAction()` 里那条取消
    /// 分派逐字对齐（见 .cpp 里那段）。账本里那位 `canCancel` 只说了"这一类活允许取消"，
    /// 这一类活里有几段**就是取消不掉**的（加载器的安装器进程、整合包解压），得在这儿收口。
    bool cancelWouldWork(const Tasks::TaskSnapshot &task) const;
    /// 「这一项**此刻**按『重试』真的会重做**它自己**那几个失败的文件吗」—— 和
    /// `cancelWouldWork()` 左右对称的另一半（逐任务能力表）。
    ///
    /// 账本里那位 `canRetry` 说的是"**它失败的那一刻**有一批能重做的失败清单" —— 那是个
    /// 历史事实，会**留在账本里跨重启**，而真正那份失败清单（`VersionManager::m_failedBatch`）
    /// 只在内存里、只有一格。所以那一颗能不能露，得再问一次"现在这格还是不是它的"。
    bool retryWouldWork(const Tasks::TaskSnapshot &task) const;
    /// 记一件新任务（发号 + 落盘）。回它的号。
    Tasks::TaskId beginTask(Tasks::TaskKind kind, const QString &title,
                            const QString &targetId = QString(),
                            const QList<Tasks::TaskStep> &steps = QList<Tasks::TaskStep>());
    /// 收尾：成功 / 失败（失败时把号记进"没看过"）。**找不到那个号就什么都不做**。
    void finishTask(Tasks::TaskId id, bool success, const QString &errorSummary = QString());
    /// 「实例更新」父任务在准备阶段可以被取消：这一对登记 / 摘除那条取消入口。
    void setUpdateFlowCancel(Tasks::TaskId task, std::function<bool()> cancel,
                             std::function<bool()> available = {});
    void clearUpdateFlowCancel(Tasks::TaskId task);

    // ---------- 底栏那两块上拉面板（左：正在跑的实例 · 右：最近用过的账号） ----------
    //
    // 面板**本身**（怎么弹、弹在哪、一行长什么样）在 MainWindowUI 里；这里只给两样东西：
    // 点开时现算的"那几行是什么"（setBottomBarPanelContent() 的那个 provider）和
    // 行尾按钮被点之后的动作。两样都在 setupConnections() 里接进去。
    // / 面板里某一行的某个按钮被点了。
    // / @param account true = 右边那块（账号），false = 左边那块（实例）。
    // / @param key 那一行**是谁**（MainWindowUI::PanelRow::key）—— 不是行号，理由见那边
    // / 那段 NOTE（面板开着的时候列表是会变的，按行号回头去查会关错游戏/切错账号）。
    // / @param action 那一行第几颗按钮（下标由放进去的人定义，见 .cpp 里那几个 kXxxAction）。
    // /
    // / NOTE: 走到这里时面板**已经收回去了**（见 MainWindowUI 里 BottomBarPanel 那段：
    // / 这两个动作都要弹模态窗，压在 Qt::Popup 底下会点不动），所以这里可以放心 exec()。
    void onBottomBarPanelAction(bool account, const QString &key, int action);
    /// 账号弹层里点了某个（非当前）账号那一行：只切号，不触发网络刷新。
    void useAccountFromPanel(const QString &uuid);
    /// 账号弹层菜单区那一行「退出登录」（只在有当前身份时摆）。
    ///
    /// 清掉盘上的登录状态，但**不删账号**：那一项留在列表里，弹层里点它那一行就回来了
    /// （见 AccountStore::logout()）。想彻底不要这个账号走「管理账号与皮肤」里的删除。
    /// 清完把底栏、日志、外观一起刷一遍。
    void logoutCurrentAccount();
    /// 一个版本号在界面上的显示名（"已安装版本"下拉框里的那条文本）。
    /// 下拉框里没有它时退回版本号本身。
    QString instanceDisplayName(const QString &versionId) const;
    /// 启动诊断要的那一袋事实（Java/内存/日志/崩溃目录都按这一个实例现算）。
    ///
    /// 唯一出处：实例设置里的「检查与诊断」和智能体的 `diagnostics.startup` 都走它 ——
    /// 两边各拼一份的话，同一个实例会给出两套数字。
    StartupDiagnosis::Inputs startupDiagnosisInputs(const QString &versionId);
    /// 底栏中间那格"正在启动 X"里的 X：只有一份在启动时是它的显示名，
    /// 两份以上时是"N 个实例"。唯一一处拼它（见 .cpp 里的说明）。
    QString launchingInstanceName() const;
    /// 底栏左下角「启动中...」那句里的实例名。只有一份在启动时是它的名字，
    /// 两份以上时是"N 个实例"。
    ///
    /// 和上面那个 launchingInstanceName() 是**同一件事的两种写法**，差别只有一处：
    /// 这里**不走显示名那一套**（取版本名；没有就用版本号-加载器名称，
    /// 不受名字显示设置影响）。上面那个读的是下拉框里那条文本
    /// （也就是用户设的显示名，中间那格"正在启动 X ..."要的就是它），所以两个都得留着。
    QString launchingStatusName() const;
    /// 只把实例页那棵树上"（运行中）"那几处后缀刷新一遍，**不重灌整棵树**。
    ///
    /// 为什么不直接调 refreshInstalledVersions()：那条路会重扫磁盘、还会走
    /// syncTreeToCombo() 的 setCurrentItem()，而那一下会把用户折起来的组重新撑开
    /// （见 wtfml-version-tree-ui 那条踩过的坑）。游戏起落时用户没在做"刷新列表"这件事，
    /// 不该被动了他的树。
    void updateInstanceTreeRunState();
    /// 现在哪些实例**至少有一份会话**在跑（给实例树那几行加"（运行中）"用）。
    QSet<QString> runningVersionIds() const;
    /// 现在哪些实例**还没落地**（正在下 / 正在装加载器 / 正在导入整合包）。
    ///
    /// 「下载完成前该实例不在启动选项中」就靠它：refreshInstalledVersions() 拿它把
    /// 那几个从下拉框、组合框、实例树和"（N 个实例）"计数里一起滤掉，
    /// onStartButtonClicked() 那道闸口也读同一份 —— 列表里滤掉的正是闸口拦的，
    /// 不会出现"列表里能选、点下去被拦"这种自相矛盾。
    ///
    /// 这几个来源缺一不可，它们兜的是同一种缝：活儿还在进行，但此刻**没有任何一批下载
    /// 在跑**（isDownloading() 是 false），于是只看下载状态的话，这个实例看起来已经好了。
    /// - busyVersionId()：正在下这一批的那一个版本。
    /// - `m_installOccupancy`（导入整合包 / 带名字的新实例 / 补全 / 首启资源预取四档，
    ///   判据见 `InstallOccupancy`）：导入和"带名字的新实例"都有"等原版版本 json"那一段
    ///   （元数据请求，不算下载），导入还多一段解压覆盖。补全与预取**是**一批下载在跑，
    ///   busyVersionId() 那一份通常已经盖住它们 —— 列在这里是给"按钮点下去、批次还没
    ///   起来"那一小段兜底（那之间隔着一道 isBusy() 判据）；那一小段里补全要重写这个
    ///   实例的文件、预取要往共享的 assets/asset_director 里写，照样不该被启动。
    QSet<QString> installingVersionIds() const;

    /// 记 / 摘"这一份实例正在后台搬文件"。**唯一入口**，别直接动下面那个集合。
    void markFileOperationBusy(const QString &versionId);
    void releaseFileOperationBusy(const QString &versionId);

    // ------------------------------------------------------------------
    // 启动前那两条提醒（设置页「提示」栏）
    // NOTE: 设置页那一栏叫「提示」（启动 / 游戏 / 提示 / 个性化 / 下载），组标题是
    // 「启动前提醒」，同栏里还有第三条（实例的 Java 对不上时问一句）。
    // ------------------------------------------------------------------
    // 两条挡的都是**同一件事**：同一份存档被两个进程同时打开。这件事事后救不回来
    // （后保存的那一份盖掉先保存的），所以只有"点在启动上那一下"挡得住才算数 ——
    // 两条都在 onStartButtonClicked() 里、**在换令牌之前**（见那里的调用点）。

    /// 「十秒内重复启动同一实例」那个十秒（毫秒）。**唯一出处**：判据在这里、
    /// 验证工程摆边界也读这里。
    static int duplicateLaunchWindowMs() { return 10000; }

    /**
     * @brief 这一下点启动要不要继续（false = 用户在提示里选了「取消」）。
     * @param versionId 正要启动的那个实例。
     *
     * 按顺序问两条，各自受设置页里那个开关管：
     * ① 「重复启动」：这个实例**已经有一份是我开的**（同一个身份），而且我上一次点启动
     * 就在十秒之内 ⇒ 手快连点的形状。标题说"已经有一个相同的运行中游戏进程了"，
     * 前提是那一份真的在跑 —— 所以判据里带着"确实有那么一份"，不会报一句假话。
     * ② 「进度提醒」：这个实例**已经有任何一份在跑**（我开的、别人开的、上一问刚放行的）
     * ⇒ 单人存档可能被两份游戏同时打开。
     *
     * 两条**都在**这个函数里，不是"二选一"：上一问选了『仍要启动』之后接着问下一问。
     * 所以同一个实例上连点两下、两条都开着时，框是前后两个，不是一个。
     *
     * NOTE: 两条都过了之后这里**记一笔时间**（m_lastLaunchAttempts）—— ① 那个十秒窗口
     * 读的就是它。记在"过了之后"而不是"启动成功之后"是刻意的：这一笔记的是
     * **点下去的意图**，而下一步（换令牌 / 挑 Java / 起进程）失败与否和"用户现在
     * 想不想再点一下"没关系。
     */
    bool confirmLaunchWhileRunning(const QString &versionId);

    /**
     * @brief 当前账号的身份键（跨账号区分"是不是同一个人"用）。
     *
     * **离线账号的 uuid 是空的**（`AuthManager::setOfflinePlayer()` 会把 uuid 和令牌一起
     * 清掉），所以拿 uuid 当键会把所有离线账号撞成同一个 ⇒ 那种情况退回账号名。
     * 会话那一侧（`GameSession::accountUuid` / `accountName`）用的是同一个规矩，
     * 两处**必须**一致：不一致的症状是"我自己刚启动的那一份被当成别人开的"，于是
     * 「重复启动」那条永远不响，而它恰恰是最该响的那一条。
     */
    QString launchIdentityKey() const;

    // ---------- 「上一次启动」那本账 ----------
    //
    // 账本本身在 `LaunchHistoryController`（读盘、记账、改结局、写盘失败只记日志那一套，
    // 连同内存里那一份账）。判据层是 `manage/LaunchHistory`（纯函数，verify 里逐格念）。
    // 留在这一片的是**只有主窗口说得出的那几格**：
    //
    // · `appendLaunchRecord()` —— 补上"启动那一刻的显示名"和"当时是谁"这两格，转手给账本
    // （那两格一个要读界面上那个下拉框、一个要读当前账号）；
    // · `updateHomeRecent()` —— 把账本翻成主页那两排快速入口（要读下拉框里每一行的图标
    // 和收藏分组），账本一变就重画。

    /// 把"这一趟成功了"记进账本。**产品那一侧唯一的调用点**是 `onStartButtonClicked()`
    /// 里 `sessionId > 0` 那一段（进程真的起来了那一刻）—— 以成功创建游戏进程为准，
    /// 不以点击启动按钮为准。
    ///
    /// 这一层只负责补上那两格**主窗口才知道的事实**（`instanceDisplayName()` 读的是界面上
    /// 那个下拉框、`launchIdentityKey()` 读的是当前账号），记账本身在
    /// `LaunchHistoryController::recordStart()` 里。
    ///
    /// @param versionId 实例 id。
    /// @param quickPlay 这一趟要进哪儿（空的 = 普通启动）。**`Target::None` 不是"不知道"，
    /// 是"这一趟就是普通启动"** —— 普通进主菜单那一趟不许猜。
    /// @param startedAtMs 启动时刻（毫秒；取会话自己的 `startedAtMs`，不是"现在"）。
    void appendLaunchRecord(const QString &versionId, const QuickPlay::Target &quickPlay,
                            qint64 startedAtMs);

    /// 按启动历史与收藏状态重画主页两排快速入口（账本一变就重画，见 `m_launchHistory`）。
    void updateHomeRecent();

    /// 「上一次启动」那本账（`src/controllers/LaunchHistoryController.hpp`）。
    ///
    /// 不每次现读盘：主页快速入口要在"启动成功""游戏退出"两个时机重画，而那是**热路径**
    /// （启动那一发已经在同步等进程了）。读一次、写的时候顺手更新内存那一份（那一份现在
    /// 住在这一层手上，读它走 `records()`）。
    ///
    /// NOTE: 构造里建（不是初始化列表），和 `m_activityCenter` 同一个理由 —— 它要读盘，得等
    /// 前面几样都就位了。
    LaunchHistoryController *m_launchHistory = nullptr;

    /// 「实例设置」那一页**写侧**那一套（`src/controllers/InstanceSettingsController.hpp`）。
    ///
    /// 它收的是"盘上变不变"那几件事：改名的**四道闸**（正在跑的不许改 / 正在体检的不许改 /
    /// 名字合不合法 / 数据层改不动）、改完之后**谁跟着走**（`LauncherSettings` 记的"上次启动
    /// 的那一个"、分组归属键的老键新键、实例树、下拉框），以及"哪一半成了、哪一半没成"这套
    /// 分账。**一个字节的中文都没有** —— 文案走 `showInstanceSettingsNotice()`。
    ///
    /// 为什么它能拆出去而"打开那一页"不能：`openInstanceSettingsNow()` 是**十几个
    /// 闭环的接线台**（构造页、灌数据、接信号、跳页），那些拆出去只是把 connect() 换个地方
    /// 写、不是架构改善。而这一位接管的是**状态和判断**本身。
    InstanceSettingsController *m_instanceSettings = nullptr;

    /// 上一次点启动的时刻，按 **身份 → 版本 → 毫秒**（`QDateTime::currentMSecsSinceEpoch()`）。
    /// 只增不减（没人清它）：条目数 = 这台机器上"点过启动的 (身份, 实例) 组合"数，
    /// 几十项封顶，清不清都一样。
    QHash<QString, QHash<QString, qint64>> m_lastLaunchAttempts;

    /// 「加入选中的世界 / 加入服务器」排上的那一发。
    ///
    /// **一次性**：`onStartButtonClicked()` 开头把它**取走并清掉**（不是在末尾清）—— 那
    /// 一路上有七八个早退（版本 json 缺了 / 启动提醒被取消 / Java 对不上的框里点了取消…），
    /// 每一个早退都得让"这一次要进某个世界"跟着作废。否则用户取消掉那个启动提醒、过一会儿
    /// 自己点一下启动，游戏会**莫名其妙地进一个他早就放弃的世界**。
    ///
    /// 空值 = 没有这一发（绝大多数时候），命令行上因此多不出任何东西。
    QuickPlay::Target m_pendingQuickPlay;
    /// 分组归属用哪个键认这个实例：有稳定 id（wtfml.meta 的 instanceId）就用它，
    /// 没有就退回**目录名**。
    ///
    /// NOTE: 读归属和写归属必须都走这一个函数。分开写的话，"读的时候按目录名找、
    /// 写的时候按 id 存"会让归属在存下去的那一刻就对不上，而且不报任何东西 ——
    /// 表现是"归好组、重开启动器，它又出来了"。
    QString instanceKey(const QString &versionId) const;
    // 重灌所有"本地已安装实例"的看法：主页那一对（下拉框 + 列表）、实例页右边那张
    // 列表、以及实例页顶上的"当前文件夹"那句。扫的是**当前游戏文件夹**的
    // .minecraft/versions，同步返回。保住用户原来选中的那一项的逻辑在 fillVersionCombo() 里。
    void refreshInstalledVersions();
    /// 同上，但扫盘走后台（`VersionManager::requestInstalledVersions()`），**界面在结果
    /// 回来之前保持上一次的样子**。版本清单从网络回来那一下走这条：那一趟扫盘 20 份
    /// 63 ms、200 份 116 ms，正落在用户等下载页的同一段时间里。
    ///
    /// 连着发两次由 `VersionManager` 的代数保证"旧的不盖新的"；这之后如果又有人调了同步的
    /// `refreshInstalledVersions()`，晚到的那一份也按 `m_installedRefreshToken` 丢掉。
    void refreshInstalledVersionsAsync();
    /// 刷新那两条路共用的收尾：灌下拉框、实例树、服务端页、预设页的实例下拉框、
    /// 主页「最近运行」。@p warnings 里的告警只在第一次非空时写日志。
    void applyInstalledVersions(const QStringList &installed, const QStringList &warnings);
    /// 同步 / 异步刷新共用的代数：异步请求发出时记进 `m_pendingInstalledRefreshToken`，
    /// 结果回来一看代数变了就丢（这期间有人同步刷过、或者又发了一次）。
    quint64 m_installedRefreshToken = 0;
    quint64 m_pendingInstalledRefreshToken = 0;
    /// 主页启动区那块「名字 + 版本」点开的实例切换弹层（搜索 / 最近 / 全部）。只负责切当前实例，
    /// 管理动作在弹层底部那颗「管理全部实例」后面。
    void openHomeInstancePicker();
    QList<MainWindowUI::HomePickerInstance> currentFolderInstancePickerRows(const QString &currentId) const;
    /// 实例页那张树重灌一遍 —— **搜索与筛选走的就是这一条**。
    ///
    /// 它和 `refreshInstalledVersions()` 是两条路：后者要**扫盘**（每个实例读一次
    /// `wtfml.meta` 那份版本信息），这一条只用上一次扫出来的那一份
    /// （`m_instancePageParts` + `m_instancePageMarks`）。用户在搜索框里打字、点一下
    /// 筛选 chip 走的是这一条 —— 每敲一个字扫一遍盘，实例有几十个时会明显卡手。
    ///
    /// 【搜索与筛选的两条规矩】
    ///
    /// * 「搜索不改变分组归属」：归属照旧由 `homeGroupIdOf()` 答（它只认
    /// `wtfml.folders` / 版本类型），筛子只决定**哪些实例进桶**，一个字节都不写盘。
    /// * 「清空搜索恢复原展开状态」：筛着的时候每一组**都摆成展开的**（不展开的话，
    /// 命中条目躲在收起来的组里 = 用户看着像"搜不到"），但那个展开状态**不落盘** ——
    /// 清空之后各组回到 `wtfml.folders` 里记着的样子。
    void rebuildInstanceTree();

    // ---------- 服务端那一页 ----------
    //
    // / 实例页正文切到 @p mode 那一页（客户端实例 / 服务端）。
    // /
    // / ★ 它做两件事：① 把界面切过去（`MainWindowUI::setInstanceMode()`，那两颗按钮的勾和
    // / 停着的那一页的唯一出处）；② 切到服务端时**顺手重扫一遍** `Servers/`（盘上可能刚被
    // / 别的东西动过）。`m_instanceMode` 在这儿记下来。
    void setInstanceMode(MainWindowUI::InstanceMode mode);
    /// 重扫 `<数据目录>/Servers`，把服务端列表重灌一遍（`ServerRegistry::scanAll()`）。
    void refreshServerList();
    /// 打开「新建服务端」那张表单 —— 它**不是**单独开的一页：表单就住在「下载 → 服务端」
    /// 那一栏的第二层里（和「游戏」的安装页同一个形状，`downloadServerFormIndex()`）。
    /// 所以这里做的是"切到下载页 → 翻到服务端那一栏 → 停在表单那一层"。
    void openNewServerPage();
    /// 从游戏安装页点「服务端」直接打开同一版本的服务端安装页；无对应服务端时退回列表并提示。
    void openServerPageForGameVersion(const QString &gameVersion);
    void showNewServerForm(const QString &selected, bool fromDownload);
    void startServerVersionProbe(const QString &version, const QString &url,
                                 const QString &fallbackUrl, int generation);
    void fallbackToServerVersionList(const QString &title, const QString &message,
                                     bool isWarning = false);
    void cancelPendingServerVersionProbe();
    /// 表单上那颗「创建」真按下去之后那一截（`NewServerPage::accepted` 的接收端）：
    /// 建目录、写 `.wtfml-server`、切到「管理 → 服务端」、接着排安装那一趟。
    void createServerFromNewServerPage();
    /// 从表单退回版本列表那一层（「取消」/Esc/「← 返回版本列表」，以及建好之后）。
    void leaveNewServerPage();
    /// 给「新建服务端」那一页现算那句计划说明 + 「创建」能不能点（`fieldsChanged` 的接收端）。
    ///
    /// **唯一翻译点**：`ServerInstall::planFor()` 的结论 + `ServerRegistry` 的重名判据，
    /// 全在这一个函数里被翻成人话喂给页面；页面自己一个判据都不算（`ui/` 不 include `src/`）。
    void updateNewServerSummary(NewServerPage *page);

    /**
     * @brief 排一趟"安装这个服务端"的活（建目录之后那一发）。
     *
     * 按下「创建」= 建目录 + 记一份账 + **当趟就把东西下下来 /
     * 跑安装器**，进度进任务中心；装好了那一行才从「未安装」变成「可启动」。所以这个
     * 函数是"创建"那一下的后半截，不是另开的一个入口。
     *
     * 它只做三件事：① 在任务中心记一项 `Install`；② 把控制器那几路信号接到账本上；
     * ③ 让它开跑。真正的四段（取元数据 / 下载 / 跑安装器 / 核对）在
     * `ServerInstallRun::ServerInstallController` 里。
     */
    void installServer(const QString &serverId, const ServerInstall::Request &request);
    /// 跑安装器要哪个 java（**判据和装加载器那条路同一套**：版本 json 说要哪个主版本，
    /// 就用那个）。回 false = 已经弹过窗说清楚为什么了，调用方别再往下走。
    /// 这一档不跑安装器时不碰 java，直接回 true（`*out` 留空）。
    bool pickServerInstallerJava(const ServerInstall::Request &request, QString *out);

    // ---------- 服务端的启动 / 停止 / 控制台 ----------
    //
    // / 某个服务端那一条进程（**一个服务端一条** —— 同时开几个是允许的，见
    // / `ServerProcess.hpp` 文件头）。惰性创建、`this` 做父对象，信号只在这一处接一次。
    // / **这一行的那颗按钮被点了**：未装好 = 修复 / 能启动 = 启动 / 在跑 = 停止。
    // / 三档合成一个入口，是因为屏幕上就是**一颗**按钮（行尾一颗按钮 + 右键菜单）。
    void activateServerRow(const QString &serverId);
    /// 开控制台那一页（覆盖层整页）。★ 转手给 `ServerConsoleController::open()` ——
    /// 页对象、`ownerTab`、「这一页现在说的是谁」全在那边。
    void openServerConsole(const QString &serverId);
    /// 把控制台那一页现在的状态 / 三颗按钮该不该按摆一遍。**没开着那一页就什么都不做**
    /// （状态变了、来了一行日志、宽限到点，都走它）。★ 同上，转手给控制器。
    void updateServerConsole();
    /// 打开这个服务端的目录（交给系统，和"打开实例文件夹"走同一个东西）。
    void openServerFolder(const QString &serverId);
    /// 重跑一遍安装（右键菜单里那一条）：按它**记着的那一份**再拼一次请求。
    void repairServer(const QString &serverId);
    /// 删掉一个服务端（**会连着 world/ 一起没** —— 所以那一问要说清楚）。
    void removeServer(const QString &serverId);

    // ---------- 服务端设置那一页 ----------
    //
    // 整页（`OverlayHost::showPage()`）：左栏四页（常用设置 / 原始文本 / 世界备份 / 体检）、
    // 右边一个堆叠页、底下一条保存带。这一页上摆的四样东西，背后的判据分别住在
    // `ServerPropertiesForm`（那十几个旋钮）、`BackupFiles`（世界备份）、`ServerCheck` +
    // `ServerPortCheck`（体检）里 —— 这一层只做两件事：把盘上的事实翻成界面上那几行，
    // 把用户按的那一下落回盘上。
    //
    // **这一页的状态和决定它的那几条现在都在 `ServerSettingsPageController` 里**
    // （页对象、"这一页现在说的是谁"、能不能改、端口那一次的结论）。那边写着两条规矩：
    // 表单补丁叠在页面上那份原文上、服务端在跑时整页只读。
    //
    // 留在主窗口的只有两条薄壳。`openServerSettingsPage()` 这一条**在 `private slots:` 里**
    // （同上面那一小段的规矩）：它本来就只被右键菜单那一发驱动，而且 mw_check 的 T42 要靠
    // 元对象系统把这一页整页驱动起来（`openInstanceSettings()` 也是这么被 T27 调用的）。
private slots:
    void openServerSettingsPage(const QString &serverId);

private:
    /// 把这一页四页都按**盘上现在的样子**重摆一遍（开页 / 放弃改动 / 保存之后 / 备份收尾）。
    /// 薄壳：真正那一段在 `ServerSettingsPageController::refresh()`。
    void refreshServerSettingsPage();

    // ---------- 玩家名单（「玩家管理」）----------
    //
    // 四份 JSON 的判据在 `src/server/ServerPlayers`（纯函数），读写在
    // `ServerSettingsController::readPlayers()/writePlayers()`；这一层只做三件事：
    // 把行翻成页面认的字符串、问用户名字、写完之后重摆一遍。
    //
    // **服务端在跑的时候不许改**：它退出时会把自己内存里那份名单写回盘上（同
    // `server.properties` 那条理由），页面那两颗按钮已经跟着 `setEditable()` 一起锁上了。
    void refreshServerPlayers();
    void addServerPlayer();
    void removeServerPlayer(const QString &identity);
    /// 「踢出…」：把一条 `kick <名字>` 送进**正在跑**的服务端的控制台（在线动作
    /// 必须走运行控制台并回显结果），再把这一发记进控制台和启动器日志。
    void kickServerPlayer(const QString &name);

    // ---------- 服务端内容（「内容管理」）----------
    //
    // 两类：模组（服务端目录 `mods/`）和数据包（世界目录 `datapacks/`）。判据（认哪个
    // 后缀、禁用怎么改名、文件夹叫什么）全在 `src/server/ServerContent`；这一层只列目录、
    // 拷文件、改名。
    void refreshServerContent();
    /// 「添加…」：选一个 / 几个文件拷进当前那一类的文件夹（重名不覆盖、逐个报数）。
    void addServerContent();
    /// 「启用」/「禁用」：把 `.disabled` 尾巴加上或摘掉（文件内容一个字节都不动）。
    void toggleServerContent(const QString &name, bool enable);
    void openServerContentFolder();
    /// 现在这一页选中的那一类落在哪个文件夹（找不到 / `level-name` 认不出来回空串）。
    QString serverContentFolder() const;

    // ---------- 服务端资源包 ----------
    void pickServerResourcePack();

    /// 体检单上某一条的出路（`ServerCheck::actionToken()` 那个记号）。
    ///
    /// NOTE: 「跑一遍体检」本身（`runServerCheck()`）和它的原料（`serverCheckFacts()`）
    /// 跟着那一页搬进了 `ServerSettingsPageController` —— 只有这一条还留在主窗口，因为
    /// 它要动的是**服务端本身**（写 `eula` / 开端口 / 装 Java…），那是 `ServerManagementActions`
    /// 那一层的事。
    void runServerCheckAction(const QString &token);
    /// 盘上这一份 `server.properties` 里的世界目录名（`level-name`；值不合法回空串）。
    QString serverWorldName(const QString &serverId) const;
    /// 世界目录的绝对路径（`<服务端目录>/<level-name>`；`level-name` 不合法时回空串）。
    QString serverWorldDir(const QString &serverId) const;

    /// 用户点了「立即备份」：把这个世界打成一个 zip（走的就是备份那一套引擎）。
    void backupServerWorld(const QString &serverId);
    /// 用户点了「从这一份恢复…」/ 双击了那一行。
    void restoreServerWorldBackup(const QString &serverId, const QString &zipPath);
    /// 用户点了「删掉这一份…」。
    void deleteServerBackup(const QString &zipPath);
    /// 这个世界的备份在盘上有几份（按文件名前缀数，`BackupFiles` 那一套）。
    int serverWorldBackupCount(const QString &serverId) const;

    /**
     * @brief EULA 那道闸：没同意过就先问一次。
     * @return 现在能不能启动（已经同意过 / 用户当场勾了并点了同意）。
     *
     * 三条规矩（见 `ServerRun::eulaPromptText()` 那段）：**不自动接受**、只写
     * `eula=true`、原文要有可点的一行。所以这里**只有一条写盘的路** —— 用户勾了那个框
     * 并且按了「同意并启动」。
     */
    bool ensureServerEula(const QString &serverId);

    /// 起它之前那条命令要的那份计划（按它记着的那一档现算）。
    /// @param problem 非空 = 拼不出来，那句话是给用户看的（`ServerInstall::planFor()` 的
    /// 结论，这一层不另编一句）。
    ServerInstall::Plan serverLaunchPlan(const QString &serverId, QString *problem) const;

    /// 把 @p serverId 那一行的行尾按钮 + 状态字按**现在的状态**摆一遍
    /// （重扫列表、状态变了、进程退了，都走它）。不在列表里 / 已经没了 = 什么都不做。
    void updateServerRow(const QString &serverId);
    void onServerStatusChanged(const QString &serverId, ServerModel::Status status);
    void onServerLineAppended(const QString &serverId, const QString &line);
    void onServerForceKillAvailable(const QString &serverId);

    /// `<目录>/mods` 里 `.jar` 的个数（`.jar.disabled` **不算**）—— 宽限时间那两个入参
    /// 之一（另一个见 `serverWorldBytes()`）。纯函数（只看盘，不看界面）。
    static int countServerMods(const QString &dirPath);
    /// `<目录>/world` 那棵树的字节数（没有那个目录 / 读不出来 = 0）。
    static qint64 serverWorldBytes(const QString &dirPath);
    /// 此刻的搜索/筛选条件（读界面上那个搜索框和四颗 chip）。
    InstanceFilter::Query instanceFilterQuery() const;
    /// 一个实例参与筛选的那几样**事实**：可搜索的串（`InstanceFilter::textsFor()`）、
    /// 有没有加载器、有没有更新、账本上有没有它。**后两样只有这一层知道** ——
    /// `m_instanceOutdatedPaths` 和账本（`m_launchHistory->records()`）都住在这儿。
    InstanceFilter::Facts instanceFilterFacts(const VersionDisplayParts &part) const;
    // 重灌实例页左边那张「游戏文件夹」列表（每一行：上面写 customname、下面写路径、
    // 行尾一颗齿轮），并把选中项停在 FolderManager 的当前目录上（界面那边挡了信号：
    // 重灌不是用户改选，不该反过来触发一次切换）。
    void refreshFolderList();
    // 「移除文件夹」能不能点：自带那份移不掉，另外**正在动盘**的时候整列都不给动
    // （下载进行中，或者有长流程举着「先别换」的牌子 —— 见下面那对 hold/release）。
    void updateFolderButtons();
    /// 举牌：从现在起到 `endFolderSwitchHold()` 为止，游戏文件夹那一列灰着。
    ///
    /// 为什么需要它：换游戏文件夹 = 换一整份 .minecraft（`AppPaths::setMinecraftDir()` 是
    /// 全局唯一写点）。下载那条路早就用 `m_downloadInFlight` 挡住了，但**非下载类长流程**
    /// 也有一段"还没有忙态、却在等"的窗口 —— 最典型的是整合包导入：CurseForge 清单是在
    /// **嵌套事件循环**里解析的（`ImportController::runOffUiThread()`），那几秒里用户输入
    /// 照常送回来，此时忙态还没置上，切一下文件夹就会让后面 `AppPaths::versionDir()` 全部
    /// 落到另一份 .minecraft 里，而界面上没有任何异常。
    ///
    /// 计数器而不是布尔：导入流程里可能嵌套（预览 → 应用），先结束的那一层不该把别人的
    /// 牌子摘掉。用 `FolderSwitchHold` 那个 RAII 壳成对调用。
    void beginFolderSwitchHold();
    void endFolderSwitchHold();
    /// 某个游戏文件夹在列表**上面那行**显示什么：用户起的 customname → 自带那份那个
    /// 固定说法（「当前文件夹」）→ 目录名。**唯一出处**：灌列表和重命名对话框正文里
    /// 都读它（两处各算一遍的话，对话框里写着"现在显示的是 A"而列表上写着 B）。
    QString folderTitle(const QString &path) const;
    /// 行尾那颗齿轮弹出来的菜单里某一项被点了（@p action 是 MainWindowUI::FolderAction）。
    /// 界面那边只知道"第几项"，翻译成具体做哪件事是这里的事。
    void onFolderActionRequested(const QString &path, int action);
    /// 「刷新」：重新扫一遍这一份文件夹。它是当前在用的那一份时，右边那张实例树跟着重扫。
    void onFolderRefreshRequested(const QString &path);
    /// 「重命名」：改的是**显示用的名字**（FolderManager 里那份 customname），
    /// 磁盘上的目录名一个字都不动。
    void onFolderRenameRequested(const QString &path);
    /// 「打开」：拿系统的文件管理器打开那个目录。
    void onFolderOpenRequested(const QString &path);
    /// 「移除」/底下那颗「移除文件夹…」按钮共用的那一段：只从列表里拿掉，盘上不动。
    void removeFolderFromList(const QString &path);
    /// 「删除」：**连盘上的目录一起删掉**（红色那一项，不可恢复，先弹确认框）。
    void onFolderDeleteRequested(const QString &path);
    // 把 FolderManager 的当前目录交给 AppPaths（此后所有 <数据目录>/... 都对着它），
    // 并打一行日志。换文件夹的那条链路只有这一处改全局路径。
    void applyCurrentFolderToPaths();
    /// 「下载时可以借哪几份游戏文件夹里的现成文件」推到 VersionManager。
    ///
    /// 值只有一处出处：`FolderManager::reuseSourceFolders()`（用户添加的 + 官方启动器那份，
    /// 存在才算）—— "不要全盘扫描"这条闸，所以这里一个字都不许自己加工路径。
    /// 换文件夹、加/移除文件夹之后都要重推一遍。
    void syncDownloadReuseSources();
    /// 重灌版本树那四条分类（以及它们底下的版本号）。在线清单是它唯一的数据源，
    /// 清单还没拉回来时四条都空着（**不消失** —— 四条一条不少），
    /// 展开状态和选中项在重灌之后尽量还原。
    void refreshAvailableVersions();

    // ---------- 安装页（版本树的下一层）----------
    // 「要装哪个原版 / 选中哪条加载器 / 它钉了哪一版 / 名字改过没有」这一成套状态住在
    // 这一层 —— 那四样必须同进同退（见 `controllers/InstallPageController.hpp` 开头那段）。
    InstallPageController *m_installPage = nullptr;
    /**
     * @brief 点「预设」那一行时开的那张紧凑弹窗。
     *
     * **不跳页**：弹窗压在安装页上（`execPrompt`），顶上写清楚"这一份基于哪个版本 /
     * 加载器"；里面四类各挑一份，选一份就现算那份计划（只读）。
     *
     * 取消 / 点「前往预设管理」都**不写**那一份选择 —— 用户回来时还是原来那一份
     * （取消不丢安装配置）。那一行现在是 `InstallPageController` 的（这是它的薄壳）。
     */
    void openInstallPresetPicker();
    /// 这次安装要哪个 Java 去跑外部安装器（Fabric 用不上，直接回 true）。
    /// @param out 成功时写入；失败时**不碰**（调用方那份值保持不变）。
    /// @return 挑得出来返回 true；挑不出来返回 false（里面已经弹窗说清原因了）。
    ///
    /// 拆出来是因为"装一个带名字的实例"和别的安装路都要它 —— 那段判据（读版本 json 的
    /// javaVersion、按它挑运行时）抄两遍的话，早晚有一处忘了跟着改。
    // 下载期间禁用这些：安装页那一整层（名称框、加载器树、安装按钮、返回）、左栏那份
    // 文件夹列表、「导入整合包…」按钮，并把底栏中间那格的进度条切成"正在准备下载"的
    // 不确定态。进度条和底栏的显隐不在这里定
    // （取决于"在下载"和"在启动"两条），统一由 updateBottomBar() 说了算。
    //
    // 清单在下面实现里逐条写着，要核对就照着那些 setEnabled() 看。
    // **启动按钮、「已安装版本」下拉框、实例页那棵树不在这份禁用清单里**：见
    // setDownloadUiBusy() 里那段"这几项故意不灰"——"在下载"和"能启动什么"是两件事，
    // 正在装的那个按版本从列表里滤掉就够了。
    //
    // NOTE: Q_INVOKABLE 只为一件事：让 build/verify/mw_check 能在**不联网、不起进程**
    // 的前提下把界面摆进"正在下载"这个态。底栏那格"被启动借走之后要原样还回来"这条
    // 规矩只有在"在下载"时才成立（见 updateBottomBar() 里那段 NOTE）；不把这个态摆得
    // 出来，那条规矩就只能靠人眼盯。
    // 全项目就这一个 Q_INVOKABLE —— 它把"私有"这层壳朝元对象系统开了一个口子，
    // 别再开第二个；真要被别处调的接口应该在头文件里堂堂正正地公开。
    Q_INVOKABLE void setDownloadUiBusy(bool busy);
    // 进/出"同步阻塞"那一段：启动这条路上有三段是在 UI 线程上同步等的（换令牌的网络
    // 请求、挑/下 Java、startGame() 里等进程起来）。那几段里 Qt 不重绘、不响应输入，
    // Windows 过几秒还会给窗口标题挂上"(无响应)" —— 用户点了「启动游戏」看到的就是
    // "卡死"。没有可测量的进度可报，进之前先把"在等什么"写到底栏那格、套上忙指针、
    // 强制刷一次。
    // NOTE: 里面那次 processEvents() **排掉了用户输入**，所以不会把"在这期间的第二次
    // 点击"送回来、把这段路重走一遍。
    void enterSyncWait(const QString &statusText);
    void leaveSyncWait();
    // 把"启动游戏"按钮的文字刷成两行：第一行固定是"启动游戏"，第二行是「已安装版本」
    // 下拉框当前选中的**显示名**（可能带自定义名/加载器名，不是版本号；没选时是
    // "未选择版本"）。顺带把裸版本号的 tooltip 挂到同一个下拉框上。见 .cpp 里的说明。
    void updateStartButtonText();
    void updateDownloadProgressUi(int done, int total, int failed, const QString &currentFile);

    // ---------- 主页右栏那个小人 + 底栏头像 ----------
    // / 让外观那条路重走一遍（开窗 / 登录成功 / 切账号 / 换版本 / 换完外观走的都是它）。
    // /
    // / 外观那一整套决策在 `controllers/PlayerSkinController`（那边叫 `reload()`，
    // / 四个决策见那边的文件头）。这一位只剩一件事：把它需要而它看不见的那一格告诉它
    // / —— **现在选中的是哪个实例**（那两个下拉框在这一层）。
    void startPlayerSkinLoad();
    // ---------- 「账号与外观」那个窗 ----------
    // / 把这个窗建出来并接好线（幂等：已经建过就什么都不做）。
    void setupAccountSkinDialog();
    /// 先读本地账号和外观缓存。显式刷新时跳过披风列表缓存的有效期，必要时续期令牌。
    void refreshAccountSkin(bool forceCapeRefresh = false);
    void applyCachedCapes(const QString &uuid);
    void changeCape(const QString &capeId);
    /// 只按本地账号列表重建这个窗里的账号行（「需要重新登录」标记变化时用，不联网、不碰预览与披风）。
    void refreshAccountRows();
    /// 这个窗顶部那句话应该写什么（当前身份）。**唯一一处**决定它 —— 离线身份、未登录、
    /// 正版三种状态要是散在各条路上，迟早出现"底栏说离线、这里说正版"。
    QString accountSkinStatusText() const;

    /// 这一趟启动走的是不是**离线身份**。
    ///
    /// **唯一出处**：`PlayerSkinController::offlineIdentity()`（当前账号本身是不是一个离线
    /// 账号，盘上活动槽那一项）。底栏那行身份、启动时要不要令牌、外观这条路联不联网，读的
    /// 都是它 —— 三处各判一次的话，就会出现"底栏说离线、启动时却去要凭据"。
    ///
    /// 外观那一套的决策在 controller 里（那边要按它决定"能不能联网"），这一位只是一句
    /// 转手 —— 留着是为了不动下面那十几处调用点。
    ///
    /// NOTE: "离线"只有一种意思：**用一个离线账号**。判据是当前账号本身是不是一个离线
    /// 账号（盘上活动槽那一项），没有"有正版账号，但这次离线启动"那一档。
    bool offlineIdentity() const;

    /// 右栏那句话现在是哪一档（未登录 / 正在取 / 正版外观 / 取不到）。
    ///
    /// **唯一一处**决定它写什么。四句文案要是散在四条路上（开窗、拿到档案、拿到贴图、
    /// 失败），迟早出现"皮肤换好了、话还停在上一句"。
    ///
    /// 它读 `m_playerSkin->hintFacts()` 要**几格事实**再挑一句。分工是：controller
    /// 说"现在是什么状况"，这里说"这种状况该讲哪句中文"
    /// —— 文案仍然只在这一层出现（`MainWindowUI` 那五句）。
    void updateSkinHint();

    MainWindowUI *m_ui;
    // NOTE: 声明顺序就是构造顺序。m_settings 必须排在 m_folderManager **前面** ——
    // 后者在构造函数里就要读盘（「游戏文件夹」列表），晚于它构造的话拿到的
    // 指针是空的，读出来就是"一份文件夹都没添加过"。
    LauncherSettings *m_settings;
    /// Stage 2：智能体启动器工具的能力宿主（生命周期跟着主窗口）。
    std::unique_ptr<AgentLauncherHostImpl> m_agentLauncherHost;
    LauncherManager *m_launcherManager;
    AuthManager *m_authManager;
    FolderManager *m_folderManager;
    VersionManager *m_versionManager;
    /// 下载页那几类资源的取数（搜索 / 版本列表 / 图标）。**异步**的 —— 搜索框里连敲几次
    /// 回车也不会把界面卡住（见 ResourceStore.hpp 文件头那段）。
    ResourceStore *m_resourceStore;

    // ---------- 「文件管理」那几页的图标----------

    /// ---------- 实例更新检测那一趟----------

    /// **会话级**：每个实例上一趟查出来**可更新**的那些路径（键是实例 id）。
    ///
    /// 用途之一是文件管理那几页状态栏里的「可更新」靠它筛（见
    /// InstanceFilesPage::setUpdateResult）。不记的话，用户查完关掉设置页再进来，
    /// 那一格就永远是"还没查过"—— 而那趟检测刚跑完。
    ///
    /// NOTE: **键在不在 = 查过没有**（值是空表 = 查过了、一条都没有）。这两件事在界面上
    /// 是两句话（"先点一下检查更新" / "都是最新的"），所以不能只留一份名单。
    /// NOTE: 和别处那几份会话缓存一样**只往里放、不清理**：键是实例 id、值是纯路径，留着
    /// 不占多少，而"哪几个实例该清"没有出处（文件删了就自然筛不出来了 —— 那一条
    /// 已经不在这一页上了）。
    QHash<QString, QStringList> m_instanceOutdatedPaths;
    /// **会话级**：和上面那份名单**一起写、一起失效** —— 实例 id → （盘上路径 → 该换成
    /// 的那一版）。上面那份回答"哪几条要更新"（界面拿它筛「可更新」那一格、算计数），
    /// 这一份回答"换成哪一版"（悬浮条上那颗「更新」拿它拼要下的文件清单）。
    ///
    /// 批量更新用。**不在 `InstanceFilesPage` 里存**：那一层
    /// 不 include src/，`ResourceCatalog::VersionEntry` 是下载那一侧的类型；而且它也没
    /// 地方去拿 —— 唯一的来源就是这趟检测。
    ///
    /// NOTE: 和上面那份一样**只往里放、不清理**。刷新时机只有一处 = 下一次检测收尾
    /// （`finishUpdateCheck()` 里那一句 insert，和名单同一句旁边）。用户更新完之后
    /// 这一格仍然是旧的（盘上那几个文件其实已经换过了）—— 直到他再点一次
    /// 「检查更新」。这是**刻意**的：现算一份"现在还是不是最新的"要再跑一整趟
    /// 反查 + 版本列表，而那正是他刚点过的那颗按钮。
    /// 为什么这一份不跟着筛：用户点了「不再忽略」时要能**当场**把"换成哪一版"找回来
    /// （他不可能为了这个再跑一趟检测）。筛掉的那一条如果连版本 id 都丢了，"不再忽略"
    /// 就只能记成一个空 —— 那一颗按钮就永远按不动了。
    /// NOTE: 两份名单指向的仍然是同一件事（"可更新的那几条"），差别只在于**要不要提示**，
    /// 所以按筛过的那一份去这一份里取，取得到的一定是对的。
    QHash<QString, QHash<QString, ResourceCatalog::VersionEntry>> m_instanceUpdateTargets;
    /// 正在**后台搬文件**的那几个实例（模组预设 / 复制实例 / 迁移 / 升级副本 / 还原）。
    /// 它们全都会进 `installingVersionIds()`，也就是"这期间谁都不许启动、不许删、
    /// 不许再开一份搬文件"。
    ///
    /// 判据（引用计数：两趟作业压同一个实例时，先结束的那趟不许把另一趟的"忙"摘掉；
    /// 没记过就放开要当没发生）收在 `controllers/FileOperationOccupancy` 那一层，
    /// 主窗口只转发 `markFileOperationBusy()` / `releaseFileOperationBusy()`。
    FileOperationOccupancy m_fileOperationOccupancy;
    QHash<Tasks::TaskId, std::function<void()>> m_modTransferCancels;
    /// **会话级**：上一趟检测的**原始结论**（实例 id → 那一趟判出来的每一条 + 反查那一趟
    /// 自己的毛病）。
    ///
    /// 为什么不能只留上面那两份名单：名单是**按忽略 / 固定筛过**的（"这一条还要不要提示"），
    /// 而用户点了「忽略这一版」之后**不可能再跑一趟检测**（那要读几十兆 jar、发几十发网络）
    /// —— 得拿原始那一份当场重算。没有这一份的话，"忽略"只能写成"把这一条从名单里抠掉"，
    /// 而"不再忽略"就永远退不回来了（名单上已经没有它了）。
    struct UpdateCheckResult {
        QList<ModDependencies::UpdateVerdict> verdicts;
        QString lookupNote;
    };
    QHash<QString, UpdateCheckResult> m_instanceUpdateResults;
    /// 实例页正文现在停在哪种模式（客户端实例 / 服务端）。
    ///
    /// NOTE: 记的用户**最近一次**选的档（切开页面、切回来还是他选的那一档），不是"盘上有没有
    /// 服务端"。两个模式的判据分得很清：这个字段只管"露哪一页"。
    MainWindowUI::InstanceMode m_instanceMode = MainWindowUI::InstanceMode::Client;
    /// 「新建服务端」那张表单（懒建一次、之后复用）。它嵌在「下载 → 服务端」那一栏的
    /// 第二层里（`MainWindowUI::getServerFormHost()`），**不是**浮在覆盖层上的独立一页。
    NewServerPage *m_newServerPage = nullptr;
    QNetworkAccessManager *m_serverProbeNetwork = nullptr;
    QPointer<QObject> m_serverProbeRequest;
    int m_serverProbeGeneration = 0;
    QHash<QString, bool> m_serverAvailabilityCache;
    QHash<QString, QJsonObject> m_probedServerVersionJsons;
    bool m_suppressGameInstallServerJump = false;
    /// 服务端安装那四段的驱动（构造里建一次，和 `m_versionManager` 一样常驻）。
    ///
    /// 它拿的是 `VersionManager` 手上**那一份**下载器（`setDownloadManager()` —— 理由见
    /// 那个函数的说明：下载器只有一份批次状态），所以**不能**跟着哪一次安装来去。
    ServerInstallRun::ServerInstallController *m_serverInstall = nullptr;
    /// 正在装的那一趟在账本上的号（`invalidTaskId()` = 此刻没有在装的）。
    ///
    /// 「取消」那一按要按**这一项是谁**分派：服务端安装和版本/资源下载不是同一份状态
    /// （一个跑 QProcess + 自己的元数据请求，一个走 `VersionManager` 的批次），两边都
    /// 无脑叫一遍的话，取消服务端安装会顺手把别人正在跑的下载掐掉。判据就是这一个号。
    Tasks::TaskId m_serverInstallTaskId = Tasks::invalidTaskId();
    /// 账本里那一项一共有几格（百分比按"走到第几格"算，见 `installServer()` 里的 NOTE）。
    int m_serverInstallStepCount = 0;
    /// 服务端进程的所有权、复用与信号汇聚属于服务端域，主窗口只呈现状态。
    ServerWorkspaceController *m_serverWorkspace = nullptr;
    /// 起 / 停 / 强杀 / 退出之后怎么办（四格状态：备份忙态、崩溃计数、是不是我们让它停的、
    /// 几点起来的）。★ 它**不拥有**进程 —— 进程在 `m_serverWorkspace` 里。
    ServerRunController *m_serverRun = nullptr;
    /// 服务端列表的右键菜单（摆哪几项 + 点下去做什么）。
    ServerMenuController *m_serverMenu = nullptr;
    /// 下载收尾那六种来路 + 「这一趟排上了哪几份预设」那一套（见 `DownloadCompletionController`）。
    DownloadCompletionController *m_downloadCompletion = nullptr;
    ServerSettingsController *m_serverSettingsController = nullptr;
    ThemeSettingsController *m_themeSettingsController = nullptr;
    JavaSettingsController *m_javaSettingsController = nullptr;
    PresetController *m_presetController = nullptr;
    LauncherPreferencesController *m_launcherPreferences = nullptr;
    CrashReportController *m_crashReports = nullptr;
    IntegrityController *m_integrityController = nullptr;
    /// 工具箱那九件小工具（第六个选项卡，入口是导航轨底部的「工具箱」）。页面、算术、后台线程全在它那儿。
    ToolboxController *m_toolboxController = nullptr;
    BackupController *m_backupController = nullptr;
    ServerPingController *m_serverPingController = nullptr;
    FolderController *m_folderController = nullptr;
    ModpackExportController *m_modpackExportController = nullptr;
    InstanceFileController *m_instanceFileController = nullptr;
    InstanceTransferController *m_instanceTransferController = nullptr;
    /// 「继续未完成操作」：按 OperationReport 把没做完的文件操作补完。
    FileOperationRecoveryController *m_fileRecovery = nullptr;
    DownloadBatchController *m_downloadBatches = nullptr;
    QJsonObject snapshotDownloadContinuation() const;
    bool continueDownloadInstallation(Tasks::TaskId id, const QJsonObject &intent, bool targetsVerified = false);
    InstanceDialogController *m_instanceDialogController = nullptr;
    InstanceEditorController *m_instanceEditorController = nullptr;
    ImportController *m_importController = nullptr;
    GameSessionController *m_gameSessionController = nullptr;
    LaunchController *m_launchController = nullptr;
    ConnectionsController *m_connectionsController = nullptr;
    /// 主页「每日一言」：取句、当天缓存与句子池（见 controllers/DailyQuoteController.hpp）。
    DailyQuoteController *m_dailyQuote = nullptr;
    /// 这次启动之前记下的「上次打开日期」（启动时读出旧值再写入今天），以及启动那一刻有没有实例。
    /// 两样都只在启动时定一次：第一次打开的那一整趟都说「欢迎」，不因中途装了实例改口。
    QDate m_greetingLastOpened;
    bool m_greetingHadInstances = false;
    InstanceIconController *m_instanceIconController = nullptr;
    /// 控制台那一页。**页对象和"这一页现在说的是哪一个服务端"都在控制器里**
    /// （`src/controllers/ServerConsoleController`），这一层
    /// 只留 `openServerConsole()` / `updateServerConsole()` 两条转手的口子。
    ServerConsoleController *m_serverConsole = nullptr;
    /// 「服务端设置」那一页。**页对象、"这一页现在说的是哪个服务端"、能不能改、体检里
    /// 那个端口的结论，全在控制器里**（`src/controllers/ServerSettingsPageController`），
    /// 这一层只留 `openServerSettingsPage()` / `refreshServerSettingsPage()` 两条薄壳。
    ///
    /// NOTE: 原来这里还有 `m_serverSettingsRaw` / `m_serverSettingsOriginal` 两格，注释
    /// 说它们是"唯一会被写回盘上的东西"。**那句话早就不是真的了**：真正写盘的是页面那份
    /// `rawText()`（`save()` 读的就是它），这两格从搬出控制器之前起就一直只写不读。
    /// 顺着这次搬迁一起删掉。
    ServerSettingsPageController *m_serverSettings = nullptr;
    /// 正在跑的那一批「更新」。`label` 空 = 没有。
    ///
    /// 为什么要有它：`downloadResourceFiles()` 一次只收**一批**，而这一批里每个文件的
    /// 结果（成功 / 失败 + 原因）要一条一条记进任务账本（"不只显示总数"）。
    /// 文件名 → 步骤下标这层对应关系**只有发起那一处知道**（下载那一侧回来的就是文件名）。
    struct UpdateBatchRun {
        QString label;                ///< 这一批叫什么（和 `resourceDownloadFinished` 带的是同一个）
        QString instanceId;           ///< 更新的是哪个实例
        Tasks::TaskId task = Tasks::invalidTaskId();
        /// 文件名 → 还没落定的步骤下标（同名多条时按先后取）。**先登记再发起**：下载器
        /// 可能同步就回一条（盘上那个文件已经对了的那一档），那时候没登记就丢一条结果。
        QHash<QString, QList<int>> pendingSteps;
        /// 文件名 → 盘上那条的绝对路径（成功之后要按它从「可更新」名单里摘）。
        QHash<QString, QString> pathByFileName;
        /// 已经确认下好了的那些路径。
        QStringList succeeded;
        /// 每个资源一行（`名字 —— 结果`），收尾时写日志 + 弹那份清单。
        QStringList lines;
        /// 有几步是失败的（收尾时决定记成功还是失败）。
        int failedSteps = 0;
    };
    UpdateBatchRun m_updateBatch;
    /// 实例页那张树的**原料**：上一次 `refreshInstalledVersions()` 扫出来的那份（能启动
    /// 的实例 + 每一行前面那个图标）。**只有 `rebuildInstanceTree()` 读它**。
    ///
    /// 为什么要有这一格：搜索框每敲一个字、每点一下筛选 chip 都要重灌一遍树，而重灌的
    /// 原料（`installedVersions()` + 每实例一次 `displayParts()`、一次 `instanceRowMark()`）
    /// 全是**读盘**的 —— 实例有几十个时，每敲一个字扫一遍会明显卡手。所以扫盘那一段
    /// （`refreshInstalledVersions()`）把结果留在这儿，
    /// 筛选那一段（`rebuildInstanceTree()`）只重算分组、重新灌树。
    ///
    /// NOTE: 「什么时候会变」的答案和"哪些事会叫 `refreshInstalledVersions()`"逐字同一句
    /// —— 安装完、下载完、改名、换文件夹…都走那条路，所以这份缓存
    /// 不会比屏幕上那一棵树更旧。
    QList<VersionDisplayParts> m_instancePageParts;
    /// 和上面那份**一一对应**的行首图标（`instanceRowMark()` 的成品）。缓存它是因为算一张
    /// 要读版本 json，而且它是"行前面那张图"，筛选前后**同一行画的还是同一张**。
    QHash<QString, QIcon> m_instancePageMarks;
    /// 「从资源详情页返回时，那一页停在哪儿」—— 一趟性的来路（见 `NavigationReturn`）。
    ///
    /// 要保留从资源详情页退出时的滚动位置。为什么得由这一层带着走：回来时那一页是
    /// **整个重新 new 出来的**（`openInstanceSettings()` 里那份堆上的对话框），所以
    /// `InstanceFilesPage` 自己那份"重列不跳回顶端"的锚点跨不过这一道重建
    /// （见 `InstanceFilesPage::ScrollPosition`）。位置在**离开那一刻**量下来
    /// （`fileResourceRequested` 那一条接线里），开回来时在 `showPage()` 之前塞回去
    /// （`InstanceDialogController::open()` 里 `showPage()` 之前那一段）。
    ///
    /// 它**也是**「← 返回实例设置」回程用的那一份：从下载页回去时由
    /// `returnToInstanceSettingsFromDownload()` 把那条来路挪进来，于是"回哪一页、
    /// 滚到哪儿"只有一份形状（以前是 `FileScrollReturn` 另记一份）。
    ///
    /// NOTE: 一次性。`openInstanceSettings()` 那一趟不论用没对上，收尾都会清掉 ——
    /// 留着的话下一次开实例设置会拿一份过期的位置去摆（而那时用户多半已经翻到别处去了）。
    NavigationReturn m_instanceReturn;
    /// 已经问回来的版本列表，按**请求钥匙**存（ResourceCatalog::versionsRequestKey()）。
    /// 点第三层那一下要拿它换出 URL / sha1 / 大小。
    /// NOTE: 那些事实**不放在条目上**：条目在重搜时会被整个清掉，而"这一版的文件是哪几个"
    /// 本来就只属于"那一次请求"的范围。放这儿也不会留悬空指针（值是纯数据）。
    QHash<QString, QList<ResourceCatalog::VersionEntry>> m_resourceVersions;
    /// 下载页「整合包」那一页下回来的 .mrpack 落在哪 —— 下完之后要接着走导入那条路
    /// （importModpackFromFile）。键是**那一单的 label**（`downloadResourceFiles()` 收的
    /// 第三个参数，`resourceDownloadFinished` 原样带回来），空表 = 没有在等导入。
    ///
    /// NOTE: 只活一次：onResourceDownloadFinished() 里按 label `take()` 走。不 take 的话，
    /// 用户手动导入一个包之后，下一个下完的模组也会顺手再触发一次导入。
    ///
    /// NOTE: 键**必须**是那一单的 label：任何一次 onResourceDownloadFinished（普通模组、
    /// 资源包、数据包…都算）都会把记录读走并清掉，不分单的话，整合包还在下、用户又点了个
    /// 模组，模组先下完就会吃掉记录，于是 ①拿一个还没下完的半截 .mrpack 去走导入
    /// （导入那条路会当场失败），②等整合包真正下完时记录已经没了 → 弹一句「已经装进
    /// 实例里了」而**实际没装**。按 label 键控之后，"这一单是不是在等导入的那一单"
    /// 才是个能判的问题。
    ///
    /// NOTE: 光键控还不够，还有**登记时机**这一半：记录只在 `downloadResourceFiles()`
    /// 回答"开起来了"之后才写（见 downloadResourceVersion）。被拒绝的那一发会带着
    /// **同一个 label** 发一个假失败 —— 双击那一行的第二下正是这样。两半都要，
    /// 少一半就还是会被吃掉。
    /// 顺带的好处：两个整合包前后脚开始下也各自记得住（键不同，take() 各取各的）。
    struct PendingModpackImport {
        QString path;
        QString instanceName;
        Tasks::TaskId taskId = Tasks::invalidTaskId();
    };
    QHash<QString, PendingModpackImport> m_pendingModpackImports;
    QSet<QString> m_oneClickModpackIds;
    /// 树上每一个资源**完整**的那一条（按 `Hit::iconCacheId()` 存，也就是条目上
    /// `resourceIconIdRole()` 里那串）。
    ///
    /// 为什么条目上那四个角色还不够：点开一个资源的详情页要简介、slug（拼"转到 Modrinth /
    /// CurseForge"那条网址）、更新时间（头卡上那行元信息），而条目上扣着放不下这些
    /// （简介挂在 tooltip 上，slug 根本没写进条目）。
    ///
    /// NOTE: 和上面 m_resourceVersions 一样**只往里放、不清理**。键是 `<来源>-<id>`，值是
    /// 一条纯数据；重搜换一批之后旧的那些再也没人问（那些行已经不在了）。要清的话
    /// 得先知道"哪几条属于上一批"，而那件事现在没有出处 —— 靠"清空整张表"会在重搜
    /// 另一页时把别的页正在用的条目一起清掉（那四页各有一棵树，搜索是分开发起的）。
    QHash<QString, ResourceCatalog::Hit> m_resourceHits;
    /// 「资源详情」那一页的全部状态与那几条函数都在 `ResourcePageController` 里
    /// （`controllers/ResourcePageController.*`）。控制器构造时创建，页面仍然懒建。
    /// 状态本身用 `m_resourcePage->state()` 读，页面对象用 `m_resourcePage->page()`。
    ResourcePageController *m_resourcePage = nullptr;
    /// 「把这一份资源装进那个实例」那一段（落点 / 忙闸 / 任务号 / 整合包待导入表）。
    ResourceInstallController *m_resourceInstall = nullptr;
    /// 下载页每一页顶上那一行筛子的灌与读（`controllers/DownloadFilterController.*`）。
    /// 和上面那个一样**构造里就建**：主窗口一启动就要灌筛子（`fillDownloadFilters()`）。
    DownloadFilterController *m_downloadFilter = nullptr;
    /// 搜索请求、结果树与该次查询词由控制器管理；跨页面会话缓存仍在本窗口。
    ResourceSearchController *m_resourceSearch = nullptr;
    /// 「更新中心」那一页的全部状态 + 四个动作（见 `controllers/UpdateCenterController.hpp`）。
    UpdateCenterController *m_updateCenter = nullptr;
    /// 单次更新检测的请求集合、弱页面引用和反查线程。析构 UI 前先等它收尾。
    InstanceUpdateCheckController *m_instanceUpdateCheck = nullptr;
    /// 「这个项目号叫什么」的**会话级**缓存（键 = `Hit::iconCacheId()`，也就是
    /// `<来源>-<项目号>`；值 = 名字）。两个问名字的人（前置那一趟、详情页那一行）共用它。
    ///
    /// 两个用处：① 详情页那一行每换一次 chip 就要重算一次，没有缓存的话同一个号会被反复
    /// 问（而名字是**不会变**的东西，问第二遍纯粹是白发一个往返）；② "已经知道了"和
    /// "还没问过"分得开，于是 `updateResourcePageDependencies()` 能只问缺的那几个。
    /// 键里带来源是因为两家的号不是一套（`238222` 和 `nvQzSEkH` 各自在各自的命名空间里）。
    QHash<QString, QString> m_projectTitles;
    /// 这一次进下载页是**从哪个实例的设置页**点进来的（空着 = 不是从那儿来的）。
    ///
    /// 三件事读它：① 左栏顶上那颗「返回实例设置」回哪儿（连归哪颗根页、滚到哪儿都在这
    /// 一份里，见 `NavigationReturn`）；② 从这一栏点下载时默认选择哪个实例；
    /// ③ 从这一栏点开一个资源的详情页时
    /// 拿它当"当前实例"（chip 预选 + 加载器排前面）。
    ///
    /// NOTE: 它和 `m_instanceReturn` 是**两个位子**，不是一回事：这一份活到用户按
    /// 「← 返回实例设置」（或把下载页刷回首页）；那一份是"开实例设置时用一次"的。
    /// 两者不会同时在用（要回实例设置，就得先离开下载页），可生命周期不同，所以不合成一位。
    NavigationReturn m_downloadReturn;
    /// 「版本」那一格**刚按过回车**的那几页（键 = 分类键，进的是 `QSet`）。
    ///
    /// 只为一件事：那一格是可输入的，在框里按回车时 Qt 会连发两发 —— 先是
    /// `QLineEdit::returnPressed`（我们接的那一发），紧接着 `editingFinished`，而 QComboBox
    /// 自己接了后者、并在"框里那串字正好是清单里某一项"时补一次 setCurrentIndex + activated。
    /// 两条路都通到"搜一发"，所以按一下回车会搜两次。记的这一笔就是那道闸：**同一回合里
    /// 只认第一发**（那一笔由 0 毫秒的单次定时器抹掉，所以不会留到下一次操作）。
    QSet<QString> m_versionEnterSearches;
    GlobalJavaSettings *m_javaSettings;
    /// Java 扫描 / 下载队列。重活在后台线程执行，MainWindow 只负责展示结果。
    JavaRuntimeController *m_javaRuntimeController;
    // 实例分组（<数据目录>/wtfml.folders）。**不需要**在换游戏文件夹时重置它：
    // 它每个访问器都会比一下"这次问的是哪一份 .minecraft"，不一样就自己重读。
    InstanceGroupStore *m_groupStore;
    // 正开着的那一张「分组设置…」页（空 = 没开）。每次开新建一份（组清单 / 人数是
    // 开页那一刻的快照），收页之后由 openGroupDialogNow() 里那条 pageClosed 钩子收掉，
    // 所以这一格只在"开着"的这一段有效。
    GroupDialog *m_groupDialog = nullptr;
    // 玩家外观的数据层（免鉴权问官方 + 内容寻址的本地缓存）。
    PlayerSkinStore *m_skinStore;
    // 改外观那一层（**要令牌**：换皮肤、换披风）。和上面那个是两条路，见 SkinApi.hpp
    // 文件头那句分工。
    SkinApi *m_skinApi;
    PlayerSkinController *m_playerSkin = nullptr;
    // 「账号与外观」那个窗。**懒建**：没点过那个入口就是空的（见
    // setupAccountSkinDialog()）。它是 this 的子对象、也没开 WA_DeleteOnClose，所以关掉
    // 之后指针照样有效 —— 再点一次是把它 show 回来，不是重建。
    AccountSkinDialog *m_accountSkinDialog = nullptr;
    /// 换披风是一秒内连点就重复发请求的重灾区：短时间只放一次。
    QElapsedTimer m_lastCapeAction;
    QString m_capeTokenRefreshUuid;
    bool m_capeWriteInFlight = false;
    // 底栏头像那块图。**不能**在 updateBottomBar() 里现算：那个函数每次进度刷新
    // （几百毫秒一次）都会被调，现算等于一直在重渲染同一张 8×8 的皮肤。
    // NOTE: 这一格只属于"底栏要的那张图"，不是外观那条路上的状态：外观说
    // "现在画这一套"，这一格是那一套的一个派生结果（onPlayerSkinChanged 里算一次）。
    QPixmap m_avatarPixmap;
    // 下载是否在进行中。注意不能只问 m_versionManager：从点击"下载"到版本 JSON
    // 拉回来之前，VersionManager 那边还没有任何批次，这段时间也得算"忙"。
    bool m_downloadInFlight = false;
    // 「先别换游戏文件夹」的牌子有几个（见 beginFolderSwitchHold()）。和 `m_downloadInFlight`
    // 分开记：那个是"有一批下载在跑"，这个是"有一段长流程还没有忙态、但不能换目录"。
    int m_folderSwitchHold = 0;
    // 「这一批下载是谁的」—— 导入整合包 / 带名字的新实例 / 补全文件 / 首启资源预取四档。
    //
    // 为什么界面要自己记这一笔：这四档走的都是公共的 versionDownloadFinished(id, ok, msg)
    // —— 收尾那句话是说给用户看的（"整合包「X」导入好了"），而这里要判断的是"这一条信号
    // 讲的是哪一档"，好把那句「版本 X 的文件已全部就绪」换成那一档该说的话。数据层那边不
    // 另发一个信号，是为了让"一次点击 = 一个终态"这条约定继续保持唯一（见 importModpack
    // 的说明）。判据（谁算"正在改它"、任务行该叫什么、哪些实例不能启动）全在
    // `InstallOccupancy`，这里只是持有它。
    InstallOccupancy m_installOccupancy;
    // "有版本目录但 jar 名对不上"这件事是否已经写过日志。refreshInstalledVersions()
    // 一次会话会被调很多次，不记一笔就是同样的告警反复刷。
    bool m_reportedUnrecognizedVersions = false;
    // 版本树上「最新版本」那一组的"默认展开"是不是已经摆过一次了（见
    // refreshAvailableVersions() 里那一格）。**只摆第一次**：用户把它收起来之后，
    // 清单刷新（每次启动那一次、以及手动刷新拉回来那一次）不该又把它撑开。
    bool mLatestGroupExpandSeeded = false;
    // ------------------------------------------------------------------
    // 任务中心
    // ------------------------------------------------------------------
    // **唯一**记录"现在有哪些事在跑、各自跑到哪儿了"的地方（见 src/tasks/TaskManager.hpp）。
    // MainWindow 是**唯一**往它里面记的人：`DownloadManager` / `VersionManager` /
    // `LauncherManager` 照旧是执行者，这里把它们的信号翻成一次 `begin()` /
    // `setProgress()` / `succeed()`（「不把任务中心做成第二套业务逻辑」）。
    ActivityCenterController *m_activityCenter = nullptr;
    /// 「这一批下载的账」（任务号 / 谁能重试 / 父任务与步号 / 资源名 / 取消标志 / 进度节流）
    /// 全在它身上 —— 见 `controllers/DownloadTaskLedgerController.hpp` 那段"为什么是一套"。
    DownloadTaskLedgerController *m_downloadTaskLedger = nullptr;
    /// 「实例更新」父任务的取消入口（准备阶段才给）。`cancelWouldWork()` 和
    /// `cancelTaskFromDetail()` 都按它把父任务那一颗按钮接到真的那一批下载上。
    Tasks::TaskId m_updateFlowTask = Tasks::invalidTaskId();
    std::function<bool()> m_updateFlowCancel;
    /// 只用于「露不露按钮」：不能拿 m_updateFlowCancel 当查询（那一个调用会真的取消）。
    std::function<bool()> m_updateFlowCancelAvailable;
    /// 「那一批**失败下来、等着重试**的东西是谁的」—— 就是 `VersionManager::m_failedBatch`
    /// 里那一份失败清单的主人（那里面**只留得住一批**，见 `finishTask()`）。
    ///
    /// 为什么必须记这个号：`m_failedBatch` 只有一格，新一批失败会把它**盖掉**。没有这个
    /// 号的话，用户点开**上一件**失败任务的详情页，上面那颗「重试」照样露着（账本里那位
    /// `canRetry` 是它自己失败那一刻钉下的），点下去重做的却是**后一批**的文件 —— 一颗
    /// 按了会做错事的按钮。判据见 `retryWouldWork()`（「取消」那一颗的孪生兄弟）。
    /// 用户**还没看过**的那几次失败（面板里那一行挂红点、收起态那格挂红点）。
    /// 打开任务中心面板就算看过了（那是"列给你看"的唯一动作）。
    /// "刚完成的那一项还在收起态里露着"这件事到期了要重画一次（`taskSummaryRecentWindowMs()`）。
    /// **单发**：每次摆摘要时按"还差多久到期"重新起一次（见 updateBottomBarTaskSummary()）。
    QTimer *m_taskSummaryTimer = nullptr;
    /// 主窗口改大小后节流落盘（单发 400 ms；关窗时 closeEvent 再补一次）。
    QTimer *m_windowSizeSaveTimer = nullptr;
    /// 关窗合雾走到哪（`WindowMotion::interceptClose()`）：Done 时 closeEvent 直接放行，不再问第二遍。
    WindowMotion::CloseFog m_closeFog = WindowMotion::CloseFog::Idle;
    /// 任务详情页那一页。**页对象和这一页上的决定**
    /// 都在控制器里（`src/controllers/TaskDetailController`）：页是**建一次留着**、每次开页
    /// `setTask()` 重摆的那一条规矩也归它管。
    TaskDetailController *m_taskDetail = nullptr;
    AgentController *m_agentController = nullptr;

    // 现在正卡在哪一段同步等待上（空串 = 没在等）。内容就是底栏那格要写的那句话，
    // 由 enterSyncWait() 写进来、leaveSyncWait() 清掉 —— 做成字符串而不是布尔量，
    // 是因为"在等什么"只有进那一段的人知道（换令牌 / 挑 Java / 等进程起来）。
    QString m_syncWaitStatus;
    /// 同步等待**可以嵌套**（换令牌那一段自己会转嵌套事件循环，而那个循环里还能再点进
    /// 另一条会同步等的路）。这一摞记的是每一层进来**之前**那一句：内层离开时要把外层
    /// 那句还回去，而不是把"在等什么"和忙指针一起收掉（见 enterSyncWait()）。
    QStringList m_syncWaitStack;
    // 后台正在写整合包。用它挡住同一实例页上的重复点击；完成回调会清掉。

    // ---------- 一次游戏会话的那一段 ----------
    //
    // NOTE: 那两个事实结构的**定义**在 `GameSessionFacts` / `GameLogFacts` 那个头文件里
    // —— 写下它们的是 LaunchController（启动那一刻），读完它们的是那一层自己和导出那两条路。

    // ---------- 游戏异常退出 ----------

    /// 异常退出了 → 账本上补一项，并留一份 `CrashRecord`。
    void reportGameCrash(int sessionId, int exitCode, bool crashed);

    void exportCrashReport(const QString &key);
    void openCrashReportsDir(const QString &key);
};



