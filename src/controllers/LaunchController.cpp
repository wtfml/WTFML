#include "LaunchController.hpp"
#include <QCoreApplication>
#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "auth/AccountStore.hpp"
#include "auth/AuthManager.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "launcher/AsciiPathAlias.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "tasks/TaskManager.hpp"
#include "util/AppPaths.hpp"
#include "util/StorageAvailability.hpp"
#include "util/MemoryBudget.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

LaunchController::LaunchController(MainWindow *owner) : QObject(owner), m_owner(owner) {}

namespace {
class TaskAbortGuard {
public:
    explicit TaskAbortGuard(std::function<void()> onAbort) : m_onAbort(std::move(onAbort)) {}
    ~TaskAbortGuard() { if (m_armed && m_onAbort) m_onAbort(); }
    void disarm() { m_armed = false; }
private:
    std::function<void()> m_onAbort;
    bool m_armed = true;
};

/// 进来把 @p flag 置真、离开复原 —— 只给 onStartButtonClicked() 那道"不许重入"的闸用。
class FlagGuard {
public:
    explicit FlagGuard(bool &flag) : m_flag(flag) { m_flag = true; }
    ~FlagGuard() { m_flag = false; }
    FlagGuard(const FlagGuard &) = delete;
    FlagGuard &operator=(const FlagGuard &) = delete;
private:
    bool &m_flag;
};

/// 把一段会在 UI 线程上阻塞几十秒到几分钟的重活放到工作线程上跑；UI 线程在这一边
/// 跑一个**嵌套事件循环**等它。
///
/// 为什么不是「整条启动路改成回调」：那条路的顺序（换令牌 → natives → 挑 Java → 拼参数
/// → startGame）是一串前后依赖的同步步骤，拆成回调等于把整台状态机重画一遍，而收益
/// 只是"同一次点击晚一点返回"。真正要解决的是**界面在等的时候不重画、系统给标题挂
/// 『(无响应)』**—— 一个嵌套事件循环就够了：窗口照常重绘，底栏那句「正在准备…」也看得见。
///
/// 嵌套循环**会**把用户输入送回来（`enterSyncWait()` 里那次 processEvents 才排输入）。
/// 所以「启动」那条路自己的重入闸（`m_startInProgress`）必须继续挡着；这一段返回之前
/// 调用方**不许**读工作线程写的那几个变量（`loop.exec()` 只在 worker 的 finished 之后
/// 才退出，`wait()` 只是把最后那一下收干净）。
template <typename Work>
void runOffUiThread(Work work) {
    QEventLoop loop;
    QThread *worker = QThread::create([work] { work(); });
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
}

QString selectedVersionId(const QComboBox *combo) {
    return combo == nullptr ? QString() : combo->currentData().toString();
}

void versionRuntimeIds(const QJsonObject &json, QString *assetIndexId, int *javaMajorVersion) {
    QString index = json.value(QStringLiteral("assetIndex")).toObject()
                        .value(QStringLiteral("id")).toString();
    if (index.isEmpty()) index = QStringLiteral("29");
    int major = json.value(QStringLiteral("javaVersion")).toObject()
                    .value(QStringLiteral("majorVersion")).toInt();
    if (major <= 0) major = 21;
    if (assetIndexId) *assetIndexId = index;
    if (javaMajorVersion) *javaMajorVersion = major;
}

QString sessionIdentityKey(const LauncherManager::GameSession &session) {
    return session.accountUuid.isEmpty() ? session.accountName : session.accountUuid;
}

/// 身份键的**唯一算法**：正版认 uuid、离线认账号名（uuid 空着也退回名字）。
///
/// `launchIdentityKey()`（从当前账号现读）和"这一趟启动绑定的那个身份"（开头钉下来的）
/// 都走这一句 —— 两处不一致的症状见 `launchIdentityKey()` 里那段。
QString identityKeyFor(bool offline, const QString &name, const QString &uuid) {
    if (offline) return name;
    return uuid.isEmpty() ? name : uuid;
}
}

QString LaunchController::launchIdentityKey() const {
    // 规矩的整段理由在头文件里。这里只强调一处：**会话那一侧走的是同一份算法**
    // （sessionIdentityKey()，就在本文件顶上那个匿名 namespace 里）—— 两处不一致的症状
    // 是"我自己刚启动的那一份被当成别人开的"，于是「重复启动」那条永远不响。
    return identityKeyFor(m_owner->offlineIdentity(), m_owner->m_authManager->playerName(),
                          m_owner->m_authManager->playerUuid());
}

bool LaunchController::confirmLaunchWhileRunning(const QString &versionId) {
    const QString identity = launchIdentityKey();

    // 这个实例现在在跑的都有哪几份，**一次取完**：下面两条问话读的是同一份快照。
    // 卡片是嵌套事件循环里跑的（`execPrompt`），但它回来之前外面这一层不会往下走 ——
    // 所以不存在"第一问的时候还好好的、第二问的时候那一份已经退了"这种时序缝。
    bool sameInstanceRunning = false;
    bool sameIdentityRunning = false;
    for (const LauncherManager::GameSession &session : m_owner->m_launcherManager->runningGames()) {
        if (session.versionId != versionId) {
            continue;
        }
        sameInstanceRunning = true;
        if (sessionIdentityKey(session) == identity) {
            sameIdentityRunning = true;
        }
    }

    const QString displayName = m_owner->instanceDisplayName(versionId);

    // ① 「重复启动」：同一个身份、同一个实例，而且**我上一次点启动就在十秒之内**。
    //
    // NOTE: 要判"确实有那么一份在跑"（sameIdentityRunning）而不是只看时间戳。只看时间戳的话
    // 上一次启动失败（登录缺失 / 版本不完整那些早退路径都在这两问**后面**）时，
    // 用户十秒内再点一下就会被问"已经有一个相同的运行中游戏进程了" —— 而那句话是假的，
    // 界面上一个游戏进程都没有。
    if (m_owner->m_settings->warnDuplicateLaunch() && sameIdentityRunning) {
        const qint64 last = m_owner->m_lastLaunchAttempts.value(identity).value(versionId, 0);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // `now >= last` 挡的是时钟被往回拨（对时）那一档：负的间隔不该被当成"刚刚点过"，
        // 否则一次对时就能让这个框冒出来。
        const bool withinWindow =
            last > 0 && now >= last && now - last <= m_owner->duplicateLaunchWindowMs();
        if (withinWindow) {
            const int choice = InlineMessage::ask(
                m_owner->m_ui->overlay(), MessageKind::Question,
                tr("已经有一个相同的运行中游戏进程了！"),
                tr("「%1」仍在运行。再次启动会使用同一账号和实例，两个进程可能同时访问同一存档。")
                    .arg(displayName),
                QStringList{tr("取消"), tr("仍要启动")}, -1, 1, false,
                tr("可以在设置中关闭此提示（设置 → 提示 →「重复启动时确认」）。"));
            if (choice != 1) {
                return false;
            }
        }
    }

    // ② 「进度提醒」：这个实例**已经有任何一份在跑** —— 我开的、别人开的、上一问刚放行的
    // 都算。要求要的就是"上一问选了『仍要启动』之后接着问这一句"
    // （「在前面选择仍要启动的情况下/不同账户」），所以这不是"二选一"：连点两下、两个
    // 开关都开着时，框是前后两个。
    if (m_owner->m_settings->warnWorldLoss() && sameInstanceRunning) {
        const int choice = InlineMessage::ask(
            m_owner->m_ui->overlay(), MessageKind::Warning, tr("进入单人世界可能会丢失进度"),
            tr("「%1」仍在运行。\n\n同时打开同一存档会使后保存的进度覆盖先前的修改，并可能损坏存档。")
                .arg(displayName),
            QStringList{tr("取消"), tr("仍要启动")}, -1, 1, false,
            tr("可以在设置中关闭此提示（设置 → 提示 →「同一实例已运行时提醒」）。"));
        if (choice != 1) {
            return false;
        }
    }

    // 记一笔：① 那个十秒窗口下一次读的就是它。记在**两问都过了之后**，所以"用户在这一问上
    // 选了取消"不会被当成一次"想启动"—— 他在那一问上的意思显然就是不想再开一份。
    m_owner->m_lastLaunchAttempts[identity][versionId] = QDateTime::currentMSecsSinceEpoch();
    return true;
}

QStringList LaunchController::launchArguments(const QString &versionId, const QJsonObject &versionJson,
                                        const EffectiveJavaSettings &javaSettings,
                                        const QString &token, bool offlineMode,
                                        const QuickPlay::Target &quickPlay) const {
    // 兜底值只在这一份 json 没写那两个字段时才用得上（唯一出处在 versionRuntimeIds）。
    QString assetIndexId;
    int javaMajorVersion = 0;
    versionRuntimeIds(versionJson, &assetIndexId, &javaMajorVersion);

    // 按版本的自定义参数来自 <id>/wtfml.meta 的 "jvmArgs"（PCL 那半边"自定义启动参数"的
    // 对应物）；全局那半边是 EffectiveJavaSettings::extraJvmArgs，在
    // generateJvmArguments() 里比它先入列，所以单个实例能盖掉全局默认值。
    // 玩家名和底栏左下角显示的是**同一个**（AuthManager::playerName()），离线与正版
    // 两条分支都用它 —— 各自拼名字的话，底栏上的名字和游戏里看到的名字可以不一样。
    // UUID 也一样：正版分支用它当 --uuid（没登录时 AuthManager 给空串，那边退回全零）。
    // 「启动选项 → 窗口模式 / 窗口大小」：全局那一份，再叠上**这个实例自己的覆盖**
    // （`wtfml.meta` 里那三个键，由「实例设置…」的启动页写）。合成的规矩在
    // resolveGameWindow()，一个键都没覆盖时结果和全局逐字相同 —— 那是绝大多数实例的
    // 处境，也是这一档必须保住的性质。
    //
    // NOTE: 这里**自己再读一次 meta**（上面合成 Java 那一份时已经读过一次）。宁可贵这一下
    // 也不把窗口这一组值沿着两处调用点传进来：那样每加一个有实例覆盖的设置项，都要
    // 再改一次 launchArguments 的签名和那两个调用点。
    const GameWindowSettings window =
        resolveGameWindow(m_owner->m_settings->gameWindowSettings(),
                          gameWindowOverrideFromJson(m_owner->m_versionManager->versionMeta(versionId)));
    // 「最大化」那一档要照**屏幕工作区**算一个大小（原版没有最大化参数），所以工作区得跟着
    // 递下去。availableGeometry() 就是扣掉任务栏那一块之后的区域，正是"最大化"要铺满的范围。
    // 拿不到屏幕时给一对 0 —— 那是 ScreenWorkArea 里"调用方没给"的哨兵值，那一档这时会
    // 退回用户填的那两个数（宁可开一个用户填过大小的窗口，也不要因为取不到屏幕就起不来）。
    ScreenWorkArea screen;
    if (const QScreen *primary = QGuiApplication::primaryScreen()) {
        const QRect available = primary->availableGeometry();
        screen.width = available.width();
        screen.height = available.height();
    }
    // 由 generateJvmArguments() 决定这三样发成哪些参数（原版那条 has_custom_resolution
    // 规则**用不上**，理由写在那边的 NOTE 里）。
    return m_owner->m_launcherManager->generateJvmArguments(
        token, versionId, assetIndexId, versionJson, javaSettings, offlineMode, javaMajorVersion,
        m_owner->m_versionManager->versionJvmArgs(versionId), m_owner->m_authManager->playerName(),
        m_owner->m_authManager->playerUuid(), window, screen, quickPlay);
}

void LaunchController::abortStart(StartCancelSource source) {
    // 只竖旗子、不做别的：调用方是 `closeEvent` 或任务中心那颗「取消」，它自己正卡在
    // "启动"那条调用栈的嵌套事件循环里。在这里直接收尾（finishTask / 删东西）会踩到
    // 那条栈上的状态。区别只有一处：**谁**要取消 —— 早退那几处按它写日志，
    // `finishAbortedStart()` 按它决定记「已取消」还是失败。
    m_cancelSource = source;
    m_abortRequested = true;
    // 正在下 Java 运行时的（换令牌与 natives 之后那一大段）也一起停：那一段在工作线程上，
    // 它只认这个合作式取消位（见 `ensureJavaForVersion()` 里的 `cancel` 参数）。
    m_abortWork.store(true, std::memory_order_relaxed);
    if (!m_startupRepairVersion.isEmpty() &&
        m_owner->m_versionManager->busyVersionId() == m_startupRepairVersion) {
        m_owner->m_versionManager->cancelActiveDownload();
    }
    // 排着的那一发"加入世界"也一起作废：这一趟都被取消了，它没有"下一次"可等 ——
    // 留着它，用户下次自己点「启动」时游戏会莫名其妙地进一个他早就放弃的世界。
    m_owner->m_pendingQuickPlay = QuickPlay::Target();
}

void LaunchController::finishAbortedStart(Tasks::TaskId launchTask, const QString &failReason) {
    if (m_cancelSource != StartCancelSource::None) {
        // 用户自己按的取消：账本收成 Cancelled（不亮"没看过的失败"红点、不弹失败框），
        // 和下载 / 备份 / 导入世界那几条取消路同一个记法。
        m_owner->m_activityCenter->tasks()->cancel(launchTask);
        return;
    }
    m_owner->finishTask(launchTask, false, failReason);
}

QString LaunchController::verifyStartupFiles(const QString &versionId, const QJsonObject &versionJson,
                                            Tasks::TaskId launchTask) {
    auto *versions = m_owner->m_versionManager;
    auto *downloads = versions->downloads();
    auto *tasks = m_owner->m_activityCenter->tasks();
    const QList<Library> libraries = downloads->extractLibraries(versionJson);
    const AssetIndex assets = downloads->extractAssetIndex(versionJson);
    const auto inspect = [&] {
        VersionIntegrity::Report report;
        runOffUiThread([&] {
            const auto files = VersionIntegrity::plan(versionJson, versionId, libraries, assets);
            for (const auto &file : files) {
                if (m_abortWork.load(std::memory_order_relaxed)) break;
                const auto verdict = VersionIntegrity::check(file);
                ++report.checked;
                if (!verdict.ok()) report.problems.append(verdict);
            }
        });
        return report;
    };
    auto report = inspect();
    if (m_abortRequested || report.ok()) return {};
    const auto summary = [](const VersionIntegrity::Report &result) {
        QStringList files;
        for (const auto &problem : result.problems) {
            if (files.size() == 8) break;
            files.append(problem.label + QStringLiteral(" · ") + problem.detail());
        }
        return result.summary() + QStringLiteral("\n\n") + files.join(QLatin1Char('\n'));
    };
    if (m_owner->m_downloadInFlight || versions->isDownloading() ||
        m_owner->m_installOccupancy.modifying(versionId)) {
        return MainWindow::tr("已有下载或安装任务正在进行，请结束后再检查这个实例。")
            + QStringLiteral("\n\n") + summary(report);
    }

    const QScopedValueRollback repairOwner(m_startupRepairVersion, versionId);
    const QScopedValueRollback downloadBusy(m_owner->m_downloadInFlight, true);
    m_owner->m_installOccupancy.setBatch(InstallOccupancy::Batch::Repair, versionId);
    const auto releaseOccupancy = qScopeGuard([&] {
        m_owner->m_installOccupancy.clearBatch(InstallOccupancy::Batch::Repair);
    });
    QEventLoop repair;
    bool finished = false;
    bool success = false;
    QString error;
    connect(versions, &VersionManager::versionDownloadFinished, &repair,
            [&](const QString &id, bool ok, const QString &message) {
        if (id != versionId) return;
        finished = true;
        success = ok;
        error = message;
        repair.quit();
    });
    tasks->setDetail(launchTask, MainWindow::tr("补全文件与资源"));
    const auto clearDetail = qScopeGuard([&] { tasks->setDetail(launchTask, {}); });
    versions->repairVersionFiles(versionId, report.problemPaths());
    if (!finished) repair.exec();
    if (m_abortRequested) return {};
    if (!success) return summary(report) + QStringLiteral("\n\n") + error;

    // 资源索引补回来后才能列出它的对象，重新核对也能发现下载器未能修复的本地文件。
    report = inspect();
    return m_abortRequested || report.ok() ? QString() : summary(report);
}

void LaunchController::onStartButtonClicked() {
    // 这一条路是同步的（换令牌 / 挑 Java / startGame() 等进程起来都在 UI 线程上等），而前
    // 两段各自会转起一个**嵌套事件循环**（AuthManager 和 GlobalJavaSettings 里等网络用的
    // 那个 `QEventLoop::exec()`）。`enterSyncWait()` 里那次 `processEvents()` 排掉了用户
    // 输入，但嵌套循环**不排** —— 于是那十几秒（网速慢时是几分钟）里「启动」按钮照样点得
    // 动，而点了就是同一条路**并行走第二遍**：
    // · 第二发 beginTask（任务中心里同一件事出现两项）；
    // · 第二发换令牌 —— 刷新令牌是**轮换**的，两发抢同一张，后发把先发手里的作废；
    // · 第二发 ensureJavaForVersion（同一个运行时目录两路同时写）；
    // · 最后同一个实例起**两个**进程 —— 正是「启动提醒」拼命想拦住的那件事（同一份存档
    // 被两个 JVM 同时打开，后保存的盖掉先保存的），从侧门进来了。
    //
    // 拦在这一层而不是灰按钮：等待中用户看到的是忙指针，这一下点击**没有第二个意思**，
    // 静默丢掉就是他要的。判据必须摆在取 quickPlay **之前** —— 被丢掉的那次点击不该把
    // "加入选中的世界"那一发顺手消费掉。
    if (m_startInProgress) {
        return;
    }
    const FlagGuard startGuard(m_startInProgress);
    // 上一趟（关窗 / 任务中心 / 卡片）留下的取消不能带到这一趟里来（见 `abortStart()`）。
    m_abortRequested = false;
    m_cancelSource = StartCancelSource::None;
    m_abortWork.store(false, std::memory_order_relaxed);
    // 检查点：每一处**刚从一个嵌套事件循环里出来**的地方都用它判一次。用户在那段等待里
    // （换令牌最长 15 秒、natives 最长 180 秒、下 Java 可能几分钟）点任务中心的「取消」，
    // 或者点关闭并确认之后，路就停在这里，不再把游戏拉起来、也不再往下写盘。
    const auto shouldStop = [this] { return m_abortRequested; };
    // 早退那几处共用的一句日志：按取消来源分（关窗 / 任务中心）。写在同一处是为了
    // "取消来源"只有一个读者 —— 四处各拼一句的话，改一处就会漏三处。
    const auto logCancelledStart = [this] {
        m_owner->appendLog(m_cancelSource == StartCancelSource::LauncherClosing
                               ? m_owner->tr("启动已取消：启动器正在关闭。")
                               : m_owner->tr("启动已取消。"));
    };

    // 「加入选中的世界 / 加入服务器」排上的那一发：**开头就取走并清掉**。
    //
    // 为什么是开头不是末尾：这一路上有七八个早退（版本 json 缺了 / natives 没备成 /
    // 启动提醒被取消 / Java 对不上的框里点了取消…），每一个早退都得让这一发跟着作废 ——
    // 在末尾取的话，"取消掉启动提醒、过一会儿自己点一下启动"会让游戏莫名其妙地进一个
    // 用户早就放弃的世界。而"取走"这件事必须发生在任何会让出事件循环的框之前，否则
    // 那些框里再进来一次启动就会看见同一个还没被消费掉的值。
    const QuickPlay::Target quickPlay = m_owner->m_pendingQuickPlay;
    m_owner->m_pendingQuickPlay = QuickPlay::Target();

    // 先认"启动哪一个"：下面那道闸口是按版本判的（见那段说明），所以版本号要在它前面拿到。
    // 只认"已安装版本"下拉框：启动只可能发生在本地已经装好的版本上。
    const QString versionId = selectedVersionId(m_owner->m_ui->getInstalledVersionCombo());
    if (versionId.isEmpty()) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("未选择版本"),
                             tr("请先在「已安装版本」下拉框中选择一个版本。"));
        return;
    }

    // 游戏文件夹那块盘不在（移动硬盘拔了、网络盘断了）：再往下走，`checkPreLaunch()`
    // 会在 `mkpath()` 上失败，报出来的是一句「无法创建 Minecraft 目录」——
    // 用户会以为是权限或者名字的问题，照着去改设置。先问一次卷，直说盘没了。
    // 这是"能不能启动"的门，不影响别的实例：换个游戏文件夹里的实例照常能起。
    const QString storageProblem =
        StorageAvailability::unavailableReason(AppPaths::minecraftDir());
    if (!storageProblem.isEmpty()) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("游戏目录不可用"),
                             tr("游戏目录所在的磁盘当前不可用：\n%1\n\n如果是移动硬盘或网络盘，接好之后再点启动；也可以在「设置」中更换游戏目录。")
                                 .arg(storageProblem));
        return;
    }

    // 闸口：**正在下载/安装的那一个**不能启动 —— 文件还没齐，点了也是崩。
    //
    // NOTE: 判据**按版本**判。"有任何一批下载在跑就拦"会把
    // 启动和下载彻底串起来：装 1.20.1 的那十来分钟里，用户想开一个早就装好的
    // 1.18.2 是点不动的，而两者之间一个字节的关系都没有。按版本判之后只有"你选中的那个
    // 正好在装"才拦 —— 也就只有那一种情况真的会启动一个文件不全的实例。
    // 那几个 isXxx() 判据（装加载器 / 导入整合包 / 装带名字的实例的前奏）都收进了
    // 它们被收进了 m_owner->installingVersionIds()：那几条路在"拉版本表、拉 profile"的前奏里
    // 还没开批次，靠的就是界面自己记的那个实例名。
    //
    // NOTE: 正常情况下用户根本选不到"正在装的那一个" —— 它已经被
    // refreshInstalledVersions() 从列表里滤掉了（"下载完成前该实例不在启动选项中"）。
    // 这一道是兜底：滤掉那份列表和这道闸口读的是**同一个**
    // m_owner->installingVersionIds()，两边不可能各说各的。
    if (m_owner->installingVersionIds().contains(versionId)) {
        // 这一句盖的是**整份** `installingVersionIds()`：除了"文件没下完"，里面还有
        // 复制 / 快照 / 还原那几类（它们也在读写这棵目录树）。说成"没下载完"会让
        // 正在做快照的用户以为程序记错了，所以把几类都点出来。
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("这个实例正忙"),
                             tr("「%1」正在安装、导入、复制、备份或还原。请等待操作完成后再启动此实例。")
                                 .arg(m_owner->instanceDisplayName(versionId)));
        return;
    }

    // **上一次没导完的整合包**不能启动。它已经从「已安装版本」里滤掉了
    // （见 VersionManager::installedVersions），这一道是兜底：本会话里较早放进下拉框、
    // 或者别处传进来的 id 仍然可能指到它。启动它会崩在缺库上；而且启动过程中那一步
    // `updateVersionMeta()` 会写出 wtfml.meta —— `isUnfinishedImport()` 从此认不出它，
    // "接着上次的导入"（`ImportNameVerdict::OkResume`）那条路就永久断了。
    if (InstanceFiles::isUnfinishedImport(versionId)) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("实例安装未完成"),
                             tr("「%1」的整合包尚未导入完成，部分游戏文件缺失。\n\n请用相同的实例名称重新导入该整合包，以继续下载。")
                                 .arg(m_owner->instanceDisplayName(versionId)));
        return;
    }

    // 启动提醒（设置页「提示」栏）：同一份存档被两个进程同时打开这件事**事后救不
    // 回来**（后保存的那一份盖掉先保存的），所以只有"点在启动上那一下"挡得住才算数。
    //
    // NOTE: 摆在**换令牌之前**是刻意的。那一段是同步的（可能要发请求、单次超时 15 秒），
    // 让用户先等十几秒再被告知"你已经开过了"是最差的顺序 —— 而且这两问的答案不受
    // 令牌影响（取消了就什么都不用做）。
    if (!confirmLaunchWhileRunning(versionId)) {
        return;
    }

    // 记在"用户确认要启动"这一刻、而不是"进程真的起来了"那一刻：后面那几段全是同步的、
    // 界面在这期间**完全不响应**（换令牌 15 秒超时 / 本机没 Java 时要下一个完整的运行时 /
    // startGame 里等进程起来），而那正是最需要一句反馈的时候。记早了也不行 —— 上面那两个
    // 早退（没选版本、"你选中的那个正在下载"）连"用户想启动谁"都还没定下来。
    //
    // NOTE: 那两段同步等待里底栏那一格显示的是**等待那句话**（见 updateBottomBarTaskSummary()
    // 里同步等待压在上面那一段），这件任务的标题和步骤在**任务中心面板**里看得到。
    const Tasks::TaskId launchTask =
        m_owner->beginTask(Tasks::TaskKind::Launch, tr("启动 %1").arg(m_owner->instanceDisplayName(versionId)),
                  versionId,
                  {Tasks::TaskStep{tr("校验登录状态"), Tasks::TaskStepState::Running, QString()},
                   Tasks::TaskStep{tr("检查启动文件"), Tasks::TaskStepState::Pending, QString()},
                   Tasks::TaskStep{tr("准备 Java 运行环境"), Tasks::TaskStepState::Pending, QString()},
                   Tasks::TaskStep{tr("等待游戏启动"), Tasks::TaskStepState::Pending, QString()},
                   Tasks::TaskStep{tr("完成"), Tasks::TaskStepState::Pending, QString()}});
    // 这一趟的收尾责任挂在栈上：从这儿往下到函数末尾有七八个早退（版本信息缺失 / 继承链
    // 不全 / natives 起不来 / ASCII 别名建不出 / 启动前检查没过 / Java 不匹配那个框里点了
    // 取消…），每一个都会让这件任务永远停在"正在启动"上。见 TaskAbortGuard 那段。
    // failReason：早退时写进任务的那一句（主页启动卡片「启动失败」下面那一行读它）。
    QString failReason;
    TaskAbortGuard launchGuard([this, launchTask, &failReason]() { finishAbortedStart(launchTask, failReason); });

    // 这一趟走不走离线：**只看当前账号是不是离线账号**（唯一出处，见 m_owner->offlineIdentity()）。
    const bool offlineMode = m_owner->offlineIdentity();

    QString token;
    bool offlineFallbackLaunch = false;
    if (!offlineMode) {
        for (;;) {
            if (shouldStop()) {
                logCancelledStart();
                return;
            }
            // 拿不到令牌的原因有好几种（从没登录过 / 网络不通 / 刷新令牌失效 / XSTS 拦截），
            // 分别处理：网络问题允许重试或离线游玩，刷新令牌失效或未登录必须重新登录。
            QString tokenError;
            // 这一段**是同步的**：令牌需要换的时候它要在 UI 线程上发请求（单次超时 15 秒）。
            token = m_owner->m_authManager->loadSavedToken(&tokenError);
            if (shouldStop()) {
                logCancelledStart();
                return;
            }
            if (!token.isEmpty()) {
                break;
            }

            const AuthManager::RefreshFailureKind failureKind =
                m_owner->m_authManager->lastRefreshFailureKind();
            if (m_owner->m_authManager->loggedIn() &&
                failureKind == AuthManager::RefreshFailureKind::Network) {
                const QString reason =
                    tokenError.isEmpty() ? tr("网络连接失败") : tokenError;
                const int choice = InlineMessage::ask(
                    m_owner->m_ui->overlay(), MessageKind::Warning, tr("无法刷新登录凭据"),
                    tr("无法刷新登录凭据：%1。可以重试，或以离线方式进入游戏（无法加入正版服务器）。").arg(reason),
                    {tr("重试"), tr("离线游玩"), tr("取消")}, 0, -1, false);
                if (shouldStop()) {
                    logCancelledStart();
                    return;
                }
                if (choice == 0) {
                    continue;
                }
                if (choice == 1) {
                    token = m_owner->m_authManager->savedAccessToken();
                    if (token.isEmpty()) {
                        token = QStringLiteral("0");
                    }
                    offlineFallbackLaunch = true;
                    m_owner->appendLog(
                        m_owner->tr("无法刷新登录凭据（%1），已选择以离线方式启动。").arg(reason));
                    break;
                }
                // 「取消」（2）或 Esc / 关窗（-1）：按 #61 取消记法将任务记为「已取消」，不带失败原因。
                abortStart(StartCancelSource::TaskCenter);
                logCancelledStart();
                return;
            }

            if (m_owner->m_authManager->loggedIn() &&
                failureKind == AuthManager::RefreshFailureKind::ReloginRequired) {
                InlineMessage::warning(
                    m_owner->m_ui->overlay(), tr("需要重新登录"),
                    tokenError.isEmpty()
                        ? tr("登录凭据已失效，请重新登录微软账号，或在「账号与外观…」中添加离线账号。")
                        : tr("%1\n\n请重新登录微软账号，或在「账号与外观…」中添加离线账号。")
                              .arg(tokenError));
                failReason = tr("登录凭据已失效，需要重新登录。");
                return;
            }

            // NOTE: 这里的"离线"指的是**那一个动作**：在
            // 「账号与外观…」那个窗里填个名字、点「添加离线账号」。
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("登录缺失"),
                                 tokenError.isEmpty()
                                     ? tr("请先登录微软账号，或在「账号与外观…」中添加离线账号。")
                                     : tr("%1\n\n请重新登录微软账号，或在「账号与外观…」中添加离线账号。")
                                           .arg(tokenError));
            failReason = tr("登录状态失效，请重新登录");
            return;
        }
    }

    // 换令牌那一段转过嵌套事件循环 —— 用户可能在里面点了关闭。
    if (shouldStop()) {
        logCancelledStart();
        return;
    }

    const QString launchPlayerName = m_owner->m_authManager->playerName();
    const QString launchPlayerUuid = m_owner->m_authManager->playerUuid();
    // 账本（「上一次启动」/ 任务详情里"当时是谁"）读的也是这一份，不是"记账那一刻的当前账号"：
    // 从这行往下到 `appendLaunchRecord()` 之间**还有**会转嵌套事件循环的同步段（准备 Java
    // 运行时、启动前的磁盘检查），账号面板在那中间点得动。记账时现读会把这一趟记到 B 头上
    // （而游戏其实是用 A 起来的），「重复启动」那条判据也会跟着对错人。
    const QString launchAccountKey = identityKeyFor(offlineMode, launchPlayerName, launchPlayerUuid);

    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 0, Tasks::TaskStepState::Done,
        offlineMode ? tr("离线账号，无需联网登录")
                    : (offlineFallbackLaunch ? tr("离线游玩（使用本地档案）") : QString()));
    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 1, Tasks::TaskStepState::Running);
    // version.json 要提前取：natives 的准备和启动前检查都依赖它
    QJsonObject versionJson = m_owner->m_versionManager->getVersionJson(versionId);

    // 拿不到 version.json 就不能猜：静默用 javaMajorVersion = 21 和
    // assetIndex = "29" 继续启动的话，26.1（需要 Java 25）会崩在
    // UnsupportedClassVersionError，且看不出原因。
    if (versionJson.isEmpty()) {
        m_owner->appendLog(m_owner->tr("找不到版本 %1 的 version.json").arg(versionId));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("版本信息缺失"),
            tr("找不到版本 %1 的 version.json，无法确定所需的 Java 版本。\n请先选中该版本并点击\"下载\"重新获取版本信息。")
                .arg(versionId));
        failReason = tr("找不到版本信息");
        return;
    }

    // 继承链不完整（带模组加载器的版本最常见：它基于的原版没装）时**现在就说清楚**。
    //
    // 必须单列这一步，不能靠上面那句"版本信息缺失"：加载器版本自己那份 json 是**在**的，
    // 缺的是原版那份。用户照着"请重新下载该版本"做，下回来的还是同一个加载器版本，
    // 问题原样复现。
    const QString inheritanceProblem = m_owner->m_versionManager->inheritanceProblem(versionId);
    if (!inheritanceProblem.isEmpty()) {
        m_owner->appendLog(m_owner->tr("版本继承链不完整：%1").arg(versionId));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("版本不完整"), inheritanceProblem);
        failReason = tr("版本不完整");
        return;
    }

    m_owner->beginFolderSwitchHold();
    const auto folderHold = qScopeGuard([this] { m_owner->endFolderSwitchHold(); });
    const QString integrityProblem = verifyStartupFiles(versionId, versionJson, launchTask);
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    if (!integrityProblem.isEmpty()) {
        failReason = integrityProblem;
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("无法启动"), integrityProblem);
        return;
    }

    // 旧格式（1.18.2 及以下）的 natives 必须先落地，否则会被 checkPreLaunch 拦下。
    // 该方法自带守卫，已经解压过的版本直接返回，不会重复解压。
    //
    // 它**跑在工作线程上**（`runOffUiThread()`）。这一段底下是
    // 180 秒的 PowerShell；同步跑在 UI 线程上时，`enterSyncWait()` 只能保证"进去之前
    // 把话说出来"，之后窗口是真的不重绘 —— 不放在工作线程上时，第一次启动那几秒到几十秒里，用户看到
    // 的就是标题挂上「(无响应)」。放在工作线程上窗口照常重绘，底栏那句也一直看得见。
    // `prepareNatives()` 是 const、只碰参数和盘上的文件，不读启动器的可变状态。
    QStringList nativeErrors;
    QString nativesDir;
    LauncherManager *launcher = m_owner->m_launcherManager;
    {
        auto stageActive = std::make_shared<std::atomic_bool>(true);
        const QPointer<MainWindow> weakOwner(m_owner);
        const auto onNativeProgress = [weakOwner, stageActive, launchTask](int done, int total) {
            if (!stageActive->load()) {
                return;
            }
            QMetaObject::invokeMethod(
                weakOwner.data(),
                [weakOwner, stageActive, launchTask, done, total]() {
                    if (!weakOwner || !stageActive->load()) {
                        return;
                    }
                    weakOwner->m_activityCenter->tasks()->setProgress(launchTask, done, total);
                },
                Qt::QueuedConnection);
        };
        runOffUiThread([&] {
            nativesDir =
                launcher->prepareNatives(versionJson, versionId, &nativeErrors, onNativeProgress);
        });
        stageActive->store(false);
    }
    // natives 那一段（旧版解压最长 180 秒）也转过嵌套事件循环：取消要在这里就停下，
    // 别再把 ASCII 别名、启动前检查那几笔写盘做完才收（那些都是"起步之后才做"的活）。
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    if (nativesDir.isEmpty() && !nativeErrors.isEmpty()) {
        m_owner->appendLog(m_owner->tr("准备 natives 失败："));
        for (const QString &error : nativeErrors) {
            m_owner->appendLog(error);
        }
        // NOTE: 这条**必须弹窗**：只写日志的话，日志面板在主页是收起来的，用户看到的就是"点了启动、什么都没发生"。
        // 同文件 appendLog 上面那段 NOTE 的规矩：拦下用户操作的消息一律要弹窗。
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("无法启动"),
                             tr("准备 natives 失败，游戏起不来：\n\n%1")
                                 .arg(nativeErrors.join(QStringLiteral("\n"))));
        failReason = tr("准备 natives 失败");
        return;
    }
    if (!nativesDir.isEmpty()) {
        m_owner->appendLog(m_owner->tr("natives 已就绪：%1").arg(nativesDir));
    }

    // Java 8 打不开任何带非 ASCII 字符的路径（真机上 `20w14∞` 和中文名的 1.8 整合包都起不来，
    // 报的是 `找不到或无法加载主类`）。要换的话就在**这个位置**把别名那套建出来：
    // natives 刚落地（上面那一步），而 classpath / --gameDir 还没拼（下面 launchArguments()
    // 里才拼）—— 早一步 natives 不存在、晚一步命令行已经发出去。
    // 判据在 AsciiPathAlias 里（Java ≤ 8 且路径里真有非 ASCII 字符），绝大多数实例走到这里
    // 就是一次"三个字符串里有没有 > 0x7F"的检查，不碰盘。
    {
        int aliasMajor = 0;
        versionRuntimeIds(versionJson, nullptr, &aliasMajor);
        if (AsciiPathAlias::needed(aliasMajor,
                                   {AppPaths::versionDir(versionId),
                                    InstanceFiles::jarPath(versionId)})) {
            const AsciiPathAlias::Paths alias = AsciiPathAlias::pathsFor(versionId);
            const QString aliasProblem = AsciiPathAlias::ensure(alias, versionId, nativesDir);
            if (!aliasProblem.isEmpty()) {
                m_owner->appendLog(m_owner->tr("准备 ASCII 别名失败：%1").arg(aliasProblem));
                // 同上面那两条：拦下用户操作的话一律要弹窗（否则用户看到的只是"点了没反应"，
                // 或者一句被 Java 8 说岔了的 `找不到主类`）。
                InlineMessage::warning(m_owner->m_ui->overlay(), tr("无法启动"), aliasProblem);
                failReason = tr("准备路径别名失败");
                return;
            }
            m_owner->appendLog(m_owner->tr("这个实例的路径里有非 ASCII 字符（Java 8 打不开），已改用 ASCII 别名：%1")
                          .arg(alias.root));
        }
    }

    QStringList errors;
    if (!m_owner->m_launcherManager->checkPreLaunch(versionId, &errors, versionJson)) {
        m_owner->appendLog(m_owner->tr("启动前检查失败："));
        for (const QString &error : errors) {
            m_owner->appendLog(error);
        }
        // 同上：缺 Java、游戏目录写不进去这一类都在这里，必须让用户看见。
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("无法启动"),
                             tr("启动前检查没通过，游戏起不来：\n\n%1")
                                 .arg(errors.join(QStringLiteral("\n"))));
        failReason = tr("启动前检查未通过：%1").arg(errors.value(0));
        return;
    }

    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 1, Tasks::TaskStepState::Done);
    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 2, Tasks::TaskStepState::Running);
    // 兜底值只在 version.json 没写这个字段时才用得上（两处兜底的唯一出处是
    // versionRuntimeIds()）—— 这里要**提前**拿到 javaMajorVersion，下面挑 Java 就得用它。
    // assetIndex 那一半这里不取：它只进命令行，而那串参数是 launchArguments() 拼的。
    int javaMajorVersion = 0;
    versionRuntimeIds(versionJson, nullptr, &javaMajorVersion);
    // 主类自己说了算的那一条：json 里那个 javaVersion 是**包作者写的**，而"换过主类"的
    // 整合包经常写错 —— 真机上 `GTNH 2.8.4` 写的是 8，它的主类
    // `me.eigenraven.lwjgl3ify.relauncherstub.RelauncherStubMain` 却是 Java 17 编的，
    // 拿 Java 8 起来就是一句 UnsupportedClassVersionError（class file version 61）。
    // 主类 class 文件头上那个版本是唯一说得准这件事的东西，所以以它为准，**只抬高、
    // 不降低**（读不到就是 0 = 没意见，json 那个值照旧）。
    //
    // NOTE: 只对"json 声明 ≤ 8"的版本做这一步：那一步要读一次 zip（一次 PowerShell），
    // 而"声明新 Java 却要更新的"那种错配至今没在真机上见过 —— 68 个实例一个都不走
    // 这条路，行为逐字节不变。
    // NOTE: 抬高之后下面那一串（挑 Java / 下载运行时 / 按版本要求过滤 JVM 参数 / 不匹配
    // 时问用户）拿到的都是**这个**值 —— 那正是我们要的：给 GTNH 挑的就该是 17。
    // NOTE: 上面 ASCII 别名那一步用的是 json 里那个值（那时还没挑 Java）。诊断那一侧
    // （"声明是 8"）本来就更保守，多建一套用不上的别名没有任何害处。
    if (javaMajorVersion > 0 && javaMajorVersion <= 8) {
        const int fromMainClass = InstanceFiles::mainClassJavaMajor(
            versionId, versionJson.value(QStringLiteral("mainClass")).toString());
        if (fromMainClass > javaMajorVersion) {
            m_owner->appendLog(m_owner->tr("这个实例的主类要 Java %1（version.json 里写的是 %2），按主类算。")
                          .arg(fromMainClass)
                          .arg(javaMajorVersion));
            javaMajorVersion = fromMainClass;
        }
    }
    // version.json 里的 javaVersion.component 指明了 Mojang 为该版本准备的运行时，
    // 例如 26.1 是 java-runtime-epsilon（Java 25）。
    const QString javaComponent = versionJson["javaVersion"].toObject()["component"].toString();

    // ---------- 每实例设置：先把"这次到底用哪一套值"合成出来 ----------
    //
    // 这个实例的覆盖来自它自己的 wtfml.meta（「实例设置…」写的那些键）。一个实例
    // 一个键都不用覆盖时 override.isEmpty()，下面这条路走出来的值和加覆盖层之前
    // 逐字节一致 —— 那是绝大多数实例的处境，也是这条改动必须保住的性质。
    const JavaSettingsOverride override =
        javaOverrideFromJson(m_owner->m_versionManager->versionMeta(versionId));
    EffectiveJavaSettings javaSettings =
        resolveJavaSettings(m_owner->m_javaSettings->effective(), override);

    // 实例覆盖了 Java 路径 = 这个实例是**手动档**：全局那个「自动」开关不许盖掉它。
    // 不这么判的话，用户给某个实例钉死的 Java 会在下一次启动时被"按版本要求自动挑"
    // 悄悄换掉，而那一项覆盖也就白设了。
    const bool instanceOwnsJavaPath = override.javaPath.has_value();
    bool allowAutoSelect = m_owner->m_javaSettings->autoSelectJava() && !instanceOwnsJavaPath;

    // 「问」和「做」分开：这一段只负责拿到用户的答案，值、meta、全局设置一概不动 ——
    // 那些由下面按答案各走各的。问在合成值之后、挑 Java 之前。
    const MainWindow::JavaMismatchAnswer answer = m_owner->askAboutJavaMismatch(versionId, override, javaMajorVersion);
    // 那一问也是内嵌提示（`execPrompt` 转嵌套事件循环）：任务中心在那段等待里按的取消
    // 到这里就该停 —— 别再进低内存那一问、也别去下 Java 运行时。
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    if (answer == MainWindow::JavaMismatchAnswer::AbortLaunch) {
        return;
    }
    // 低内存提醒：这次要的堆已经超过当前可用物理内存的 80% 时先问一句。
    // 排在 Java 那一问之后、下载/启动之前：太早用户还没确定 Java，太晚就只剩卡死。
    {
        const qint64 availableMb = MemoryBudget::availablePhysicalMb();
        int alreadyRunningMb = 0;
        for (const LauncherManager::GameSession &session :
             m_owner->m_launcherManager->runningGames()) {
            alreadyRunningMb += qMax(0, session.xmxMb);
        }
        if (MemoryBudget::lowMemoryFor(javaSettings.xmx, alreadyRunningMb, availableMb)) {
            if (!InlineMessage::question(
                    m_owner->m_ui->overlay(), tr("可用内存不足"),
                    tr("此实例需要 %1 MB 堆内存，运行中的实例共分配 %3 MB。系统可用物理内存约 %2 MB。\n\n继续启动可能导致卡顿。可关闭其他程序或调低内存分配。确认启动？")
                        .arg(javaSettings.xmx)
                        .arg(availableMb)
                        .arg(alreadyRunningMb),
                    tr("仍要启动"), tr("取消"), false)) {
                return;
            }
        }
    }
    // 那一问本身也是内嵌提示（`execPrompt` 转嵌套事件循环）。
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    // 「仍然用这个 Java 启动」只有一个后果：把"允许不匹配"那个口子打开。判在
    // javaMatchesMajor() 之前（见 ensureJavaForVersion 的值版），所以哪怕那条路径
    // 根本起不来，用户也已经明说了"就用它"。
    const bool allowMismatchedPath = (answer == MainWindow::JavaMismatchAnswer::KeepInstanceJava);
    // 「改用匹配的 Java 启动」也是只有一个后果：把实例钉的那条路径**丢掉**，
    // 让这次走"按版本要求自动挑"。丢掉的只是这次要用的那份值 —— 实例的 meta 要等
    // 挑出来之后才写回去（那才是用户要的"在实例设置中恢复为匹配的 Java"）。
    if (answer == MainWindow::JavaMismatchAnswer::UseMatchingJava) {
        javaSettings.javaPath.clear();
        allowAutoSelect = true;
    }

    QString javaError;
    bool javaReady = false;
    GlobalJavaSettings *javaStore = m_owner->m_javaSettings;
    {
        auto stageActive = std::make_shared<std::atomic_bool>(true);
        const QPointer<MainWindow> weakOwner(m_owner);
        const auto onJavaProgress = [weakOwner, stageActive, launchTask](qint64 done, qint64 total) {
            if (!stageActive->load()) {
                return;
            }
            QMetaObject::invokeMethod(
                weakOwner.data(),
                [weakOwner, stageActive, launchTask, done, total]() {
                    if (!weakOwner || !stageActive->load()) {
                        return;
                    }
                    weakOwner->m_activityCenter->tasks()->setProgress(launchTask, done, total);
                },
                Qt::QueuedConnection);
        };
        runOffUiThread([&] {
            javaReady = javaStore->ensureJavaForVersion(javaSettings, javaMajorVersion, javaComponent,
                                                        allowAutoSelect, &javaError,
                                                        allowMismatchedPath, true, onJavaProgress,
                                                        &m_abortWork);
        });
        stageActive->store(false);
    }
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    if (!javaReady) {
        m_owner->appendLog(m_owner->tr("无法获取所需的 Java 运行时环境：版本 %1").arg(javaMajorVersion));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("Java 运行时缺失"),
            instanceOwnsJavaPath
                ? tr("「%1」指定的 Java 用不了（路径不存在，或者主版本不是 %2）。\n可以在「实例设置…」里改这个实例的 Java 路径，或者把它切回「跟随全局」。")
                      .arg(m_owner->instanceDisplayName(versionId))
                      .arg(javaMajorVersion)
                : tr("找不到或无法下载 Java %1，请手动指定 Java 路径。").arg(javaMajorVersion));
        failReason = tr("找不到可用的 Java %1").arg(javaMajorVersion);
        return;
    }

    if (m_owner->offlineIdentity() != offlineMode
        || m_owner->m_authManager->playerName() != launchPlayerName
        || m_owner->m_authManager->playerUuid() != launchPlayerUuid) {
        m_owner->appendLog(m_owner->tr("启动已取消：准备 Java 的途中账号被切换了（%1）。")
                               .arg(launchPlayerName));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("账号被切换了"),
            tr("准备 Java 时，当前账号由「%1」切换为「%2」。\n为避免账号信息与凭据不匹配，已取消启动。请重新启动游戏。")
                .arg(launchPlayerName, m_owner->m_authManager->playerName()));
        failReason = tr("准备 Java 时账号被切换");
        return;
    }

    // Java 已就绪，接下来拼参数、创建进程并等待游戏的首条输出；完成由会话回调确认。
    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 2, Tasks::TaskStepState::Done);
    m_owner->m_activityCenter->tasks()->setStepState(launchTask, 3, Tasks::TaskStepState::Running);

    // 「改用匹配的 Java」：把刚挑中的那条路径写回**这个实例**。
    //
    // 「改用匹配的 Java」写的是**这个实例**的 wtfml.meta，
    // 不是全局 —— 下面那段 adoptJavaPath 的注释说的就是为什么不能写全局。
    //
    // NOTE: 这一步失败**要弹窗而且不启动**。这次启动其实能成（javaSettings 里已经有
    // 路径了），但下次启动开头又会读到原来的覆盖、又问一遍同一个问题 ——
    // 看起来就像"他的选择没被记住"。宁可现在说清楚，也别让用户对着一个反复
    // 出现的框去猜。
    if (answer == MainWindow::JavaMismatchAnswer::UseMatchingJava) {
        QString writeError;
        if (!m_owner->m_versionManager->updateVersionMeta(
                versionId, {{javaPathMetaField(), QJsonValue(javaSettings.javaPath)}},
                &writeError)) {
            m_owner->appendLog(m_owner->tr("把匹配的 Java 写进实例设置失败：%1").arg(writeError));
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("实例设置写入失败"),
                tr("已找到兼容的 Java（%1），但无法保存在实例「%2」的设置中：\n%3\n\n请在「实例设置」中手动指定路径，再启动游戏。")
                    .arg(javaSettings.javaPath, m_owner->instanceDisplayName(versionId), writeError));
            failReason = tr("实例设置写入失败");
            return;
        }
        m_owner->appendLog(m_owner->tr("已把「%1」指定的 Java 换成 %2")
                      .arg(m_owner->instanceDisplayName(versionId), javaSettings.javaPath));
    }

    // 挑中的 Java 路径落在**哪一层** —— 这就是"跨作用域污染"那道坎。
    //
    // 实例覆盖了 javaPath 的实例**不需要写回任何地方**（所以没有"实例档写回 meta"
    // 那一步）：那种情况上面刚判成手动档，而手动档下 ensureJavaForVersion() 只有两种
    // 结局 —— 原样认可那个路径，或者直接失败返回。**它没有能力换一个路径出来**，
    // 也就是说只要实例钉了路径，这次启动就不可能改到全局配置，污染在结构上不存在。
    //
    // 剩下的就只有全局档：自动挑中的路径写回全局配置
    // （adoptJavaPath() 里那句"值没变就不写盘"也守着同一条）。
    if (!instanceOwnsJavaPath) {
        m_owner->m_javaSettings->adoptJavaPath(javaSettings.javaPath);
    }

    // 自动档内存要**在这里**再算一次（不是开窗时算的那次）：从打开启动器到点下"启动"
    // 可能过了很久，中间可能开了浏览器、IDE，可用内存差好几 GB，而 JVM 只认这一刻的机器
    // 状态。手动档下这一句什么都不动。
    //
    // NOTE: 这里**不刷设置页**。把设置页那两个框硬刷成"这次实际
    // 用的值"就不对了：实例覆盖了 2048 而设置页显示
    // 2048，用户会以为全局被改了。设置页显示的是**全局那一层**，它只由用户自己
    // 在设置页的改动（和切到设置页时的那次重算）决定。
    m_owner->resolveMemoryForLaunch(versionId, javaSettings, override);

    // 这一串就是"这一次启动到底执行什么"的参数那半边。它和「导出启动脚本」导出的那份
    // 是**同一个函数**给的（见 launchArguments 的说明）—— 那边还要程序名，程序名在这里
    // 由 LauncherManager 自己从 javaSettings.javaPath 取（取不到退 PATH 上的 java）。
    const QStringList jvmArgs =
        launchArguments(versionId, versionJson, javaSettings, token, offlineMode, quickPlay);
    // 记住"上次启动的版本"的时机是**进程真的起来了**，不是"表达式算完了"。
    // startGame() 有三条早退路径（找不到 Java / waitForStarted 超时 / 会话数到顶），
    // 那些路径上不能先报"已开始启动游戏"再显示错误。以"起来了"为准：
    // 因为库缺失之类的原因崩掉，恰恰是最需要下次打开还选着这个版本的时候。
    //
    // startGame() 返回之前那一段（准备 natives、探 Java、等进程起来）是**同步阻塞**的，
    // 界面根本没机会重画 —— 所以那一段报不出百分比，只能报"在等什么"。
    // NOTE: 这里那句和下面 m_owner->updateBottomBar() 里那句是**同一句话**（都出自
    // MainWindowUI::launchingStatusText()），所以进程起来的那一刻文字不会跳。
    // 最后一道：到这里为止没有再让出过事件循环，但下面那句 `startGame()` 一旦返回就
    // 已经有进程了 —— 取消只在这一句之前还有意义。
    if (shouldStop()) {
        logCancelledStart();
        return;
    }
    // 身份和实例名都在**启动这一刻**钉进会话里（LauncherManager 的 GameSession 快照）：
    // 之后切账号、改实例名都不会影响"这一份当初是谁跑的"。多开这件事最要紧的就是这条 ——
    // 同一个实例挂两个账号各开一个时，两份会话的 accountName 必须是各自的，不能都读
    // "当前登录的那个"。
    // startGame() 那三条早退路径（会话数到顶 / Java 找不到 / 进程起不来）
    // 只发一句 `processError`，而那个信号平时是**游戏 stderr 的出口**（不能弹框），
    // 接它的 onProcessError() 因此只往日志面板里写一行 —— 可日志面板在主页是收起来的，
    // 用户点了启动，看到的只是"没反应"（任务中心那一行失败、却不带原因）。
    //
    // 这三条都发生在 startGame() **返回之前**、同步发出，所以只在这一小段里接一份：
    // 接完立刻断开。游戏跑起来之后的 stderr 也走同一个信号，那种不是"启动失败"。
    QString startFailure;
    const QMetaObject::Connection startErrorConnection =
        QObject::connect(m_owner->m_launcherManager, &LauncherManager::processError, m_owner,
                         [&startFailure](int, const QString &text) {
                             if (startFailure.isEmpty()) startFailure = text;
                         },
                         Qt::DirectConnection);
    const int sessionId = m_owner->m_launcherManager->startGame(
        jvmArgs, javaSettings, versionId, m_owner->instanceDisplayName(versionId),
        launchPlayerName, launchPlayerUuid,
        m_owner->m_settings->processPriority());
    QObject::disconnect(startErrorConnection);
    if (sessionId <= 0 && !startFailure.isEmpty()) {
        // 拦下用户操作的消息一律要弹窗（产品硬约束），并且要把原因写进那件任务 ——
        // 否则任务中心里只有一行"失败"，点进去也说不清是为什么。
        // 这一支之后不会再往下走（sessionId <= 0），栈上那个兜底撤销要收掉，
        // 不然它会拿一个空原因把刚才这笔覆盖掉（终态只落一次，覆盖不了，但那句
        // `finishTask` 就是多余的）。
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("无法启动"), startFailure);
        m_owner->finishTask(launchTask, false, startFailure);
        launchGuard.disarm();
    } else if (sessionId <= 0) {
        failReason = tr("游戏进程没有启动");
    }
    if (sessionId > 0) {
        m_owner->m_settings->setLastLaunchedVersion(versionId);
        m_owner->appendLog(m_owner->tr("已开始启动游戏，正在执行 JVM 参数..."));
        // 启动进度从这里开始走到"这一份自己吐出第一行输出"为止。startGame() 返回之前
        // 那段由任务中心的步骤和主页启动卡片报"在等什么"—— 这里能覆盖的是随后的
        // JVM/游戏初始化。
        // 顺序上安全：startGame() 内部那条 "游戏已启动" 的 processOutput 是在它
        // return 之前同步发出的，那时这一份还不在集合里，所以不会被当成"游戏已经出话了"。
        //
        // 下面那四笔（启动进度 / 对上的任务 / 名字 / 启动事实）现在**一次
        // 就位**，走 `GameSessionController::beginSession()` —— 它们同寿，而且判断和记账
        // 都在那一层。
        // 这一趟的收尾**已经交给别处**了（上面那句 + 后面那些进程信号），栈上那个兜底
        // 撤销 —— 不撤的话函数一退出它就把这件任务按失败收掉了。
        launchGuard.disarm();
        // 这一趟的"启动事实"在**进程真的起来**这一刻留一份。退出那一刻
        // `javaSettings`、会话表、名字全都已经不是这一趟的了，而异常退出那一项要报的正是
        // 这几个数（实例名、身份、Java 路径、内存参数、加载器）。
        //
        // NOTE: Java 的**主版本**故意不在这里取：那是"这一趟实际用的那个 Java 是什么版本"，
        // 崩溃那一刻现读 `<javaPath>/release` 就够（见 buildCrashRecord），平时一次盘
        // 都不碰。这里记的是"要什么"（`javaMajorVersion`，已经按主类抬高过）。
        //
        // NOTE: 这一份会话快照放在这一层 —— 记账那一句也要读
        // 它的 `startedAtMs`："上一次启动"记的就是**进程真的起来**那一刻，不是"现在"。
        // NOTE: 名字记的是**"这一份当初是谁"**，和 startGame() 收到的那个名字是同一个值
        // （都是 `m_owner->instanceDisplayName(versionId)`）—— 之后用户改名、切账号都
        // 不影响它，理由同 startGame() 那段（多开最要紧的就是这条）。
        const LauncherManager::GameSession session = m_owner->m_launcherManager->session(sessionId);
        {
            GameSessionController::GameSessionFacts facts;
            facts.instanceId = versionId;
            facts.instanceName = m_owner->instanceDisplayName(versionId);
            facts.accountName = launchPlayerName;
            facts.requiredJavaMajor = javaMajorVersion;
            facts.javaPath = javaSettings.javaPath;
            facts.xmxMb = javaSettings.xmx;
            facts.xmsMb = javaSettings.xms;
            facts.loaderName = InstancePagesController::loaderName(m_owner->m_versionManager->displayParts(versionId),
                                                  m_owner->m_versionManager, &facts.loaderVersion);
            facts.processId = session.processId;
            facts.startedAtMs = session.startedAtMs;
            // 这一份会话对上的就是刚才那件任务（多开时一人一件，认的是会话号不是版本号 ——
            // 同一个实例开两份也不会串）。收尾在**游戏吐出第一行输出**那一刻
            // （onProcessOutput / onProcessError），或者"一行都没出就死了"那一刻
            // （onGameFinished）—— 都不在这条栈上，所以那四笔的状态由那一层自己管。
            m_owner->m_gameSessionController->beginSession(sessionId, facts.instanceName, launchTask,
                                                           facts);
        }
        // 记进「上一次启动」那本账。**位置就是"以成功创建游戏进程为准"这句规矩的
        // 落点** —— 这一整个 `if (sessionId > 0)` 块只有"进程真起来了"才走得到（startGame()
        // 那三条早退路径在上面就返回了）。`quickPlay` 是这一趟开头取走的那个目标：空 = 普通
        // 启动，有值 = 这一趟游戏自己会进那个世界 / 那台服务器。
        appendLaunchRecord(versionId, quickPlay, session.startedAtMs, launchAccountKey);
        // 实例树上"（运行中）"要当场出来：这一份已经从"开始启动"走到"进程真的起来了"。
        m_owner->updateInstanceTreeRunState();
        // 这一档只对"真的起来了"生效 —— startGame() 那三条早退路径下面根本走不到这里。
        m_owner->applyLauncherBehaviourAfterLaunch();
    }
    m_owner->updateBottomBar();
}

void LaunchController::appendLaunchRecord(const QString &versionId, const QuickPlay::Target &quickPlay,
                                    qint64 startedAtMs, const QString &accountKey) {
    // 转手之前只补两格"只有这一层说得出来"的事实：
    // · **显示名**：`m_owner->instanceDisplayName()` 读的是界面上那个下拉框，而且这一句要被**记下来**
    // —— 实例以后改名、被删都不影响这一条（"记录"该有的样子就是当时那句话）；
    // · **身份键**：由调用方给（`onStartButtonClicked()` 在开头跟令牌一起钉下来的那一份）；
    // 没给（测试脚手架那种"直接记账"的调用）才从当前账号现读。
    // 记账本身（认不认这一趟、快捷启动的目标怎么翻、结局怎么改写、写不进去怎么说话）全在
    // 那一层里，见它文件头那段"为什么这一件事该有一个自己的 controller"。
    const QString identity = accountKey.isEmpty() ? launchIdentityKey() : accountKey;
    m_owner->m_launchHistory->recordStart(versionId, m_owner->instanceDisplayName(versionId), identity,
                                 quickPlay, startedAtMs);
}


