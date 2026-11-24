#pragma once
/**
 * @file main.cpp
 * @brief 核心回归测试入口，覆盖下载、安装、启动与数据保护。
 *
 * 直接调用生产代码而不是复刻逻辑：
 * - DownloadManager::extractLibraries -> 证明 classifier 会被下载
 * - LauncherManager::prepareNatives -> 证明解压链路通、且有守卫
 * - LauncherManager::generateJvmArguments -> 证明 java.library.path / 选项过滤
 * - GlobalJavaSettings::ensureJavaForVersion -> 证明 Java 25 选得到
 * 最后再和参照用的 classpath 算法逐字节对比，证明 1.19+ 零回归。
 *
 * F/G/H 三节（pipeline / vm-batch / vm-fetch-fail 模式）验证下载批次的记账：
 * 完成信号只发一次、失败能报出来、已存在的文件被跳过、UI 不会被永久禁用。
 * 这几节用 file:// 当"远端"，不依赖 Mojang 在线。
 */

#include <QBuffer>
#include <QAbstractSocket>
#include <QByteArrayView>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSet>
#include <QSettings>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWaitCondition>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "auth/AccountStore.hpp"
#include "auth/SkinApi.hpp"
#include "auth/TokenStore.hpp"
// version.json 里那两个会变成盘上路径的名字（资源索引号 / 对象哈希）。见 ZAO 节。
#include "download/AssetNames.hpp"
#include "download/DownloadConcurrency.hpp"
#include "download/DownloadManager.hpp"
#include "download/LocalFileReuse.hpp"
#include "download/DownloadSpeedLimit.hpp"
#include "download/DownloadProxy.hpp"
#include "download/LegacyForgeInstall.hpp"
#include "download/LoaderMeta.hpp"
#include "download/ModDependencies.hpp"
#include "download/ResourceCatalog.hpp"
#include "download/ResourceStore.hpp"
// ServerInstall：服务端那八档"要下什么、要不要跑安装器、装完的入口是什么"。**纯函数**，
// 地址那一半是 逐条 curl 出来的（形状抄在那个文件的文件头注释里）。见 ZK 节。
#include "download/ServerInstall.hpp"
#include "download/VersionCategory.hpp"
#include "launcher/AsciiPathAlias.hpp"
// 起进程那条 260 字符的路径墙（`CreateProcess` 对程序和当前目录都只有 MAX_PATH）。
// 见 ZPL 那一节：纯判断，只用来给启动失败补一句人话，从不预检拦人。
#include "launcher/ProcessPathLimit.hpp"
#include "launcher/LauncherManager.hpp"
#include "launcher/LibraryRules.hpp"
#include "launcher/VersionArguments.hpp"
#include "launcher/VersionInheritance.hpp"
// 备份（世界 / 实例快照）那一层：清单、打包、还原。见 ZQ 节 —— 那一节是**真的动盘**的
// （真打 zip、真删目录、真还原），所以它要 .cpp 而不只是头文件，而且要在一个临时游戏
// 文件夹里跑（备份落点就是 `<数据目录>/cache/backups`）。
#include "manage/BackupFiles.hpp"
// 删一棵目录那两个自由函数（`removeTreeWithoutFollowingLinks` / 只读文件那一手）。见 ZAN 节
// —— 那两条**必须真动盘**才验得出来（只读位是盘上的属性、junction 更是）。
#include "manage/FolderManager.hpp"
// 「上一次启动」那本账：主页那张卡唯一的数据源。见 ZR 节 ——
// 那一节全是**纯函数**（给值、出字符串 / 一段字节），连时钟都不问（`whenText()` 的"现在"
// 是参数），所以它不需要临时游戏文件夹，只有"落在哪个文件上"那一条要换一下数据根目录。
#include "manage/LaunchHistory.hpp"
// 实例页搜索与筛选的判据层。见 ZS 节 —— 纯函数：
// 「这一条算不算匹配」全在这儿，树那一侧一条都不必真的建出来。
#include "manage/InstanceFilter.hpp"
// 实例预设。见 PA 节 —— 值那一层和盘那一层都收一个
// `root` 参数，所以整节在一个 `QTemporaryDir` 里真存真读，连界面都不用起。
#include "presets/PresetStore.hpp"
// 原版设置那一段。见 PB 节 —— 全是纯函数（字节进、字节出），
// 两份假 `options.txt` 一喂就能逐格念，不碰盘、不联网、不开界面。
#include "presets/VanillaOptions.hpp"
// 服务器列表那一段。见 PC 节 —— 判据那几条是纯值，落盘那几条
// **真的动盘**（在 `QTemporaryDir` 里造一份真 `servers.dat`，读回来逐格看），不碰共享沙箱。
#include "presets/ServerListPreset.hpp"
// 通用文件组那一段。见 PD 节 —— 和 PC 一样**真的动盘**
// （两个临时目录之间真拷真比字节），另外还**真造 junction** 来验那两道逃逸闸。
#include "presets/GenericFiles.hpp"
// 应用计划那一层。见 PE 节 —— 一段一行地算这一趟会发生
// 什么（除了通用文件组的干跑，全是纯值）。
#include "presets/PresetPlanner.hpp"
// 提取与落到实例上。见 PG 节 —— 在两个临时
// 实例目录之间真收真落，覆盖那几样**真去备份目录里看**，游戏在跑时**真去比字节**。
#include "presets/PresetApply.hpp"
#include "presets/PresetCapture.hpp"
#include "presets/PresetCompatibility.hpp"
// 资源包次序 / 启用 + 存档数据包。见 PL 节 —— 在临时实例里
// 真造 `options.txt` / `saves/<世界>/datapacks/`，验"部署"与"启用"分开报、没世界只记账。
#include "presets/DataPackPreset.hpp"
#include "presets/ResourcePackPreset.hpp"
// 模组「真正去下」之前的判据层。见 PM 节 ——
// 必要/可选失败分开、依赖按 `平台:项目号` 去重展开、optional 只列不下、embedded 无视、
// incompatible 挡住、替代项目不确认就不换、落盘大小/sha1 校验。全拿手搭的假答复逐格念。
#include "presets/ModInstallPlan.hpp"
#include "presets/InstalledModInventory.hpp"
// 工具箱那四件小工具的判据层。见 ZAA–ZAD 四节 —— 全是
// **纯函数**（哈希那一节要真读一个文件，它在一个 QTemporaryDir 里读），不联网、不开界面。
#include "tools/AnvilPlanner.hpp"
#include "tools/Beacon.hpp"
#include "tools/Brewing.hpp"
#include "tools/Banner.hpp"
#include "tools/ArmorTrim.hpp"
#include "tools/ArmorTrimAssets.hpp"
#include "tools/CoordConvert.hpp"
#include "tools/FileHash.hpp"
#include "tools/TextFormat.hpp"
#include "manage/DefaultSkin.hpp"
#include "manage/DroppedResource.hpp"  // V5z：拖进来那个文件到底是什么
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/ModpackImport.hpp"
#include "manage/ModpackExport.hpp"
#include "manage/PlayerSkinStore.hpp"
// 离线账号「换一张」那条路上的数据层。见 ZFa 节。
#include "manage/OfflineSkin.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/VersionManager.hpp"
// 体检那一层（plan / check / Report 三个纯值进出的口子）。见 ZN 节 —— 那一节全在一个临时
// 游戏文件夹里跑：它要造 jar / 库 / 资源索引 / 资源对象四档文件，不碰共享沙箱。
#include "manage/VersionIntegrity.hpp"
// 更新中心那两件：「忽略这一版 / 固定这一版」那本账
// （见 ZT 节）和更新前那份轻量回滚点（见 ZU 节）。两节都是**纯值 + 一次文件读写**，
// 各在一个 QTemporaryDir 里真存真读 —— 不碰共享沙箱、不联网。
#include "manage/UpdatePolicy.hpp"
#include "manage/UpdateRollback.hpp"
// 世界（存档）那几个改盘的动作：「导入世界」和世界改名。见 ZJ 节 —— 那一节是**真的动盘**
// 的（造两个临时游戏文件夹），所以它要 .cpp 而不只是头文件。
#include "manage/WorldFiles.hpp"
#include "util/DirectorySessionLock.hpp"
#include "manage/WorldMap.hpp"
// 运行输出共用模型：只依赖 QtCore，所以验证工程直接驱动它（ZAM 那一节）。
#include "RuntimeOutputModel.hpp"
#include "net/HttpClient.hpp"
// 服务端工作区：判据层（状态那七档 / `eula.txt` 算不算接受 /
// `server.properties` 解与写 / 我们记的那一份 —— 见 ZW 节）和扫盘那一层（`<数据目录>/Servers`
// 底下谁在住、真建真读真删 —— 见 ZX 节）。两样都离线钉死，一节都不联网、不碰 ui/。
#include "server/ServerModel.hpp"
#include "server/ServerRegistry.hpp"
// 服务端安装那条路的判据层：纯头，见下面 ZY 一节。
#include "server/ServerInstallRun.hpp"
// 服务端"跑起来 / 停下来"那条路的判据层：
// 纯头，见下面 ZZ 一节。真起进程的那一半（`ServerProcess`）归 mw_check 的 T41。
#include "server/ServerRun.hpp"
// 「服务端设置」那一页底下那两层判据：常用字段那张表 + 值域
// （`ServerPropertiesForm`）、体检（`ServerCheck`）、以及"这个端口有人占着吗"那一次真监听
// （`ServerPortCheck`）。见下面 ZV 一节。
#include "server/ServerCheck.hpp"
// 服务端那四份玩家名单：纯解析 / 纯拼 JSON，见下面 ZAE 一节。
// 读文件 / 写文件那一半在 `ServerSettingsController`（mw_check 那边接得到）。
#include "server/ServerPlayers.hpp"
// 崩溃自动重启那条策略：纯判据，见下面 ZAF 一节。
#include "server/ServerRestart.hpp"
// 服务端资源包那一份本地 zip 的判据：条目表纯判、SHA-1 借 FileHash。
#include "server/ServerResourcePack.hpp"
// 服务端内容（模组 / 数据包）的名字与分类判据：纯函数，ZAH 一节钉它。
#include "server/ServerContent.hpp"
#include "server/ServerPortCheck.hpp"
#include "server/ServerPropertiesForm.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GameWindowSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
// 结构 / 原理图的元数据与方块统计。见 SG 节 —— 三种格式
// 的树都是这里手搭的（`NbtTree::serialize()`），gzip 那一份拿 `ZipUtil` 真压。
#include "schematic/SchematicInfo.hpp"
#include "replay/ReplayInfo.hpp"
#include "diagnose/StartupDiagnosis.hpp"
#include "operations/TransferPlanner.hpp"
#include "operations/ModVersionDeclaration.hpp"
#include "operations/TransferApply.hpp"
#include "util/AppPaths.hpp"
// NbtTree：世界设置那条路的判据层。放进 ZL 那一节是为了能**拿手工拼的字节
// 去喂它** ——"越界的长度当场拒绝""改一格别的地方一个字节不变"这些角落，造一个真存档来
// 验是验不到的。
#include "util/NbtTree.hpp"
#include "util/ErrorReport.hpp"
#include "util/ImageDecode.hpp"
// ThemeImage：设置页「背景图」那两行「选择图片…」按下去之后干的事 —— 把用户挑的那张
// 拷一份进 `<数据目录>/cache/theme/<槽位>.png|jpg`。**纯路径进、路径出**，所以这一节能
// 在一个临时数据根目录里把"收得进来 / 收不进来 / 换图只留一份"这些角落逐个钉住。
// 要求：「能给主页左侧自定义图像吗？其他页面的背景能换图吗」。见 V5v。
#include "util/ThemeImage.hpp"
// 造"假 JAR"的那两个小工具（ZF 段和 mw_check 的 ZG 段共用）。见那个文件头。
#include "fakejar.hpp"
// ServerPing：服务器列表那个"连上去问一句"（状态协议）。这一节要真起一台假服务器、
// 真连一次 —— 纯函数那半只证明"我按我写的规则拆了包"，协议那头对不对得看那一趟。
#include "util/ServerPing.hpp"
#include "util/SystemMemory.hpp"
#include "util/ZipUtil.hpp"
// servers.dat 的**写侧**（改一条服务器的名字/地址）。读那一半仍旧走 NbtUtil —— 只读那条
// 设计决定没被推翻，写入端长在另一个文件里。见 ZK 节。
#include "manage/ServerListFile.hpp"
// 上面那三个写入口**谁在指挥**（改 / 加 / 删那三条链路上的四道闸）。见 ZK13 起那一节 ——
// 它收成一个假 Host 就能驱动的形状，所以这一层现在验得了。**一个 QWidget 都不碰**。
#include "controllers/ServerListController.hpp"
// 实例配置 / 世界 level.dat 的世代备份（ZQ9 那一节量"格子按哪个实例的哪个世界分"）。
// 那一层只碰 `AppPaths::worldBackupDir()` 和传进来的 worldDir，所以整节在临时数据根里跑。
#include "controllers/InstanceMaintenanceController.hpp"
// 「开游戏直接进某个存档 / 直接连某台服务器」那一对参数（--quickPlaySingleplayer /
// --quickPlayMultiplayer）的判据层。见 ZL 节。
#include "launcher/QuickPlay.hpp"
// 任务账本那一套：形状 + 几条纯规则（AL 节）、
// 账本自己的语义与落盘/读回（AM 节）。**这一层一个 QWidget 都不碰、一个网络都不开**，
// 所以能直接 new 出来喂假任务。
#include "tasks/TaskHistory.hpp"
#include "tasks/TaskManager.hpp"
#include "tasks/TaskSnapshot.hpp"
// 游戏异常退出那几句话：纯规则一张表，见下面 ZP 节。
#include "diag/GameExitDiagnosis.hpp"

inline int g_failures = 0;
inline int g_checks = 0;

/// 数"占位符没填上"那条警告（V6h 那一条用）。
/// NOTE: 处理器必须是**自由函数**（QtMessageHandler 就是个函数指针，带捕获的 lambda
/// 换不过去），所以计数也只好放在文件作用域。
inline int g_placeholderWarnings = 0;

static void countPlaceholderWarnings(QtMsgType, const QMessageLogContext &,
                                     const QString &message) {
    if (message.contains(QLatin1String("Unresolved placeholder"))) {
        ++g_placeholderWarnings;
    }
}

static QTextStream &out() {
    static QTextStream s(stdout);
    return s;
}

static void check(bool ok, const QString &what, const QString &detail = QString()) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    out() << (ok ? "  PASS  " : "  FAIL  ") << what;
    if (!detail.isEmpty()) {
        out() << "   [" << detail << "]";
    }
    out() << "\n";
    out().flush();
}

static void section(const QString &title) {
    out() << "\n=== " << title << " ===\n";
    out().flush();
}

/**
 * @brief 往一个路径写一段内容，目录不存在就建出来，**写完立刻关掉句柄**。
 *
 * NOTE: 关句柄不是讲究，是必需的。Windows 上文件被一个还开着的 QFile 攥着时，
 * 后面的 rename / 删除 / 读都会失败，而报出来的错是"拒绝访问"或者
 * "内容不是合法的 JSON" —— 看着像产品 bug，其实是测试自己没放手。
 * 用它替掉"就地 new 一个 QFile 写到作用域结束"，就不会再踩这个坑。
 */
static void writeFileAt(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(bytes);
        file.close();
    }
}

/// 把一条路径包成 PowerShell 的单引号字符串（给 V5b 造包那条脚本用）。
/// NOTE: 单引号里面不转义反斜杠，所以 toNativeSeparators 是可以的；代价是路径里带 `'`
/// 会破 —— 这个沙盒路径是我们自己拼的，不带。
static QString psQuoted(const QString &path) {
    return QStringLiteral("'") + QDir::toNativeSeparators(path) + QStringLiteral("'");
}

/// 造一个"服务器答了"的 HttpClient::Reply（给 X6 那几条话术断言用）。
/// NOTE: networkError 留空 = 连上了、拿到响应了。要验"没连上"那种，见下面 httpReplyFailed。
static HttpClient::Reply httpReply(int status, const QByteArray &raw = QByteArray()) {
    HttpClient::Reply reply;
    reply.status = status;
    reply.raw = raw;
    return reply;
}

/// 造一个"压根没连上"的 Reply：status 保持初值 -1、networkError 有话说。
static HttpClient::Reply httpReplyFailed(const QString &networkError) {
    HttpClient::Reply reply;
    reply.networkError = networkError;
    return reply;
}

static QJsonObject loadJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "cannot open" << path;
        return QJsonObject();
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

// ---------------------------------------------------------------------------
// 参照用的 classpath 算法（1.19 之前的拼接规则），
// 用来和现在的输出做对比，证明 1.19+ 零回归。
// ---------------------------------------------------------------------------
// / 把一条"相对数据目录"的路径变成产品现在发出去的那种绝对路径。
// /
// / ★ 起 classpath 上的每一条都是**绝对路径**（理由见 legacy::classpath 那段），
// / 所以凡是拿相对路径去比 classpath 的断言都要过这一道。
static QString absoluteMcPath(const QString &relative) {
    return QDir::toNativeSeparators(
        QDir(AppPaths::minecraftDir()).absoluteFilePath(relative));
}

namespace legacy {

static bool allowed(const QJsonObject &libObj) {
    const QJsonArray rules = libObj[QStringLiteral("rules")].toArray();
    if (rules.isEmpty()) {
        return true;
    }
    bool ok = false;
    for (const QJsonValue &rv : rules) {
        const QJsonObject ruleObj = rv.toObject();
        const QString action = ruleObj[QStringLiteral("action")].toString();
        if (action.isEmpty()) {
            continue;
        }
        const QJsonObject osObj = ruleObj[QStringLiteral("os")].toObject();
        bool matches = osObj.isEmpty();
        if (!matches) {
            const QString ruleName = osObj[QStringLiteral("name")].toString().toLower();
            if (!ruleName.isEmpty()) {
                matches = (ruleName == QStringLiteral("windows"));
            }
        }
        if (!matches) {
            continue;
        }
        if (action == QStringLiteral("allow")) {
            ok = true;
        } else if (action == QStringLiteral("disallow") || action == QStringLiteral("deny")) {
            ok = false;
        }
    }
    return ok;
}

/// 参照用的 classpath 算法（"零回归"那条断言拿它当参照）。
static QStringList classpath(const QJsonObject &versionJson, const QString &versionId) {
    QStringList cp;
    const QString root = AppPaths::minecraftDir();
    auto add = [&](const QString &p) {
        const QString absolute = QDir::toNativeSeparators(QDir(root).absoluteFilePath(p));
        if (!p.isEmpty() && !cp.contains(absolute)) {
            cp << absolute;
        }
    };
    add(QStringLiteral("versions/%1/%1.jar").arg(versionId));
    for (const QJsonValue &v : versionJson[QStringLiteral("libraries")].toArray()) {
        const QJsonObject libObj = v.toObject();
        if (!allowed(libObj)) {
            continue;
        }
        const QJsonObject downloads = libObj[QStringLiteral("downloads")].toObject();
        const QString artifactPath =
            downloads[QStringLiteral("artifact")].toObject()[QStringLiteral("path")].toString();
        if (!artifactPath.isEmpty()) {
            add(QDir::toNativeSeparators(QStringLiteral("libraries/%1").arg(artifactPath)));
        }
        const QString classifierKey =
            libObj[QStringLiteral("natives")].toObject()[QStringLiteral("windows")].toString();
        if (!classifierKey.isEmpty()) {
            const QString classifierPath = downloads[QStringLiteral("classifiers")]
                                               .toObject()[classifierKey]
                                               .toObject()[QStringLiteral("path")]
                                               .toString();
            if (!classifierPath.isEmpty()) {
                add(QDir::toNativeSeparators(QStringLiteral("libraries/%1").arg(classifierPath)));
            }
        }
    }
    return cp;
}

}  // namespace legacy


static QString javaVersionOf(const QString &exe) {
    QProcess p;
    p.start(exe, {QStringLiteral("-version")});
    if (!p.waitForFinished(20000)) {
        return QStringLiteral("<timeout>");
    }
    const QString text = QString::fromLocal8Bit(p.readAllStandardError());
    const QRegularExpression re(QStringLiteral("version \"([^\"]+)\""));
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1) : text.trimmed();
}

/// 准备一个最小可用的沙箱 .minecraft，让 checkPreLaunch 能过。
static void prepareSandbox(const QString &sandbox, const QString &versionId) {
    QDir().mkpath(sandbox + "/versions/" + versionId);
    QDir().mkpath(sandbox + "/libraries/fake");
    QDir().mkpath(sandbox + "/assets/indexes");
    QFile jar(sandbox + "/versions/" + versionId + "/" + versionId + ".jar");
    if (jar.open(QIODevice::WriteOnly)) {
        jar.write("dummy");
    }
    QFile marker(sandbox + "/libraries/fake/marker.txt");
    if (marker.open(QIODevice::WriteOnly)) {
        marker.write("dummy");
    }
}

// ---------------------------------------------------------------------------
// 下载批次记账的验证。
//
// 全部离线：把 file:// 当"远端"用，可以精确控制哪些文件存在、哪个 URL 会失败，
// 不依赖 Mojang 在线。asset 对象的 URL 在代码里是写死到
// resources.download.minecraft.net 的，所以这里只要求"对象已经在 objects/ 里"——
// 走 ExistsOnly 的跳过分支，一个网络请求都不会发出去。
// ---------------------------------------------------------------------------

namespace synth {

struct RemoteFile {
    QString path;
    QString url;
    QString sha1;
    qint64 size = 0;
};

/// 造一个本地文件并当作"远端"资源返回（url 是 file:// 形式）。
static RemoteFile makeRemote(const QString &remoteDir, const QString &relPath,
                             const QByteArray &content) {
    RemoteFile r;
    r.path = QDir(remoteDir).absoluteFilePath(relPath);
    QDir().mkpath(QFileInfo(r.path).absolutePath());
    QFile f(r.path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(content);
        f.close();
    }
    r.url = QUrl::fromLocalFile(r.path).toString();
    r.sha1 =
        QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex());
    r.size = content.size();
    return r;
}

static QString sha1Of(const QByteArray &content) {
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex());
}

}  // namespace synth

struct BatchObservation {
    int emissions = 0;
    bool success = false;
    bool finished = false;
    bool busyWhileInFlight = false;
    bool busyAfterEndBatch = false;
    bool busyAfter = false;
    DownloadStats stats;
};

/// 跑一批下载并记录 allDownloadsFinished 发了几次、内容是什么。
template <typename StartFn>
static BatchObservation runBatch(QCoreApplication &app, StartFn start) {
    DownloadManager mgr;
    BatchObservation obs;
    QEventLoop loop;
    QObject::connect(&mgr, &DownloadManager::allDownloadsFinished, &app, [&](bool ok) {
        ++obs.emissions;
        obs.success = ok;
        obs.finished = true;
        loop.quit();
    });

    mgr.beginBatch();
    start(mgr);
    obs.busyWhileInFlight = mgr.isBusy();
    mgr.endBatch();
    obs.busyAfterEndBatch = mgr.isBusy();
    // 关键：整批都被跳过时 endBatch() 会同步发信号，此时 quit() 已经没人听了，
    // 再 exec() 就会永久阻塞。必须先看标志。
    if (!obs.finished) {
        loop.exec();
    }
    obs.busyAfter = mgr.isBusy();
    obs.stats = mgr.stats();
    return obs;
}

/// pipeline 模式的全部断言（全离线）。
int runPipelineTests(QCoreApplication &app, const QString &sandbox);

/// vm-batch 模式：走 VersionManager::downloadVersion 的完整批次编排。
/// version.json 是本地伪造的，靠 getVersionJson() 的磁盘回读填进缓存，
/// 不依赖 Mojang。
int runVersionManagerBatch(QCoreApplication &app, const QString &sandbox);

/// vm-fetch-fail 模式：元数据拉取走死路时，必须有终态信号。
int runVersionManagerFetchFail(QCoreApplication &app);

/// version.json 里的索引号 / 对象哈希能不能当文件名（纯函数，见 ZAO 节）。
int runAssetNamesTests();

// ---------------------------------------------------------------------------
// I. 版本显示名：formatVersionName（纯函数，UI 问题 #14 之后拼法固定）
// + VersionManager::displayParts()（读盘）+ LauncherSettings（读写设置）
// ---------------------------------------------------------------------------
// 这一节的意义：显示名是"用户唯一能看见的版本标识"，而拼错了不会崩、只会安静地
// 显示成另一个东西（甚至空串）。所以真值表整表逐条钉死，不留任何组合靠推理。
//
// NOTE: LauncherSettings 那一小段必须把 QSettings 重定向到沙盒 ini 再跑 —— 默认的
// NativeFormat 写的是**用户的真注册表**。上面 E 节末尾那句"测试不该改动机器上的设置"
// 是同一个顾虑，这里只是换了个办法满足它。
int runVersionNameTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// J. 已安装版本扫描：名字对不上时必须说一声
//
// 真机背景：用户的 versions/ 里有目录被手动改过名、里面的 jar 却还叫旧名字
// （851 MB 和 388 MB 的整合包各一个）。判据**不放松** —— 放宽成"目录里有任意 .jar
// 就算已安装"会把下载中途失败留下的半个版本也收进来 —— 但静默忽略会让用户以为
// 启动器坏了。这一节钉的是"必须上报"，且不该被误报的东西（空目录、散落的 zip）
// 一条都不能报。
//
// NOTE: installedVersions() 走 AppPaths::versionsDir()，也就是 exe 旁边的
// .minecraft/versions，正好就是本文件里那个 sandbox。displayParts 那几节同理。
// ---------------------------------------------------------------------------
int runVersionJvmArgsTests(const QString &sandbox);

// 实例判据：InstanceFiles::resolve() 的纯函数真值表 + 两条走真磁盘的对照。
// 真值表是**手算的**，所以它能挡住"改了判据没人发现"。
int runInstanceFileTests(const QString &sandbox);

// wtfml.meta 的写侧：合并写、坏文件拒绝写、稳定 id 幂等。
// 重命名 / 分组 / 每实例设置三件事都靠这一层。
int runVersionMetaWriteTests(const QString &sandbox);

// 实例特征（加载器 + 版本类型）一次问清楚：meta 优先、json 兜底、结果跟着
// mergedVersionJsons 一起缓存。钉的是"装完加载器界面上立刻认得出"这条。
int runInstanceTraitsTests(const QString &sandbox);

// 安装占用那四档（导入 / 安装 / 补全 / 预取）的判据：优先级、"谁算正在改它"、
// 收尾按哪一档说话。纯值进、结论出，不建界面也不碰盘。
int runInstallOccupancyTests(const QString &sandbox);

// 后台搬文件那份引用计数（两趟压同一个实例 / 没记过就放开 / 空 id）。
int runFileOperationOccupancyTests(const QString &sandbox);
int runUpgradeSelectionTests(const QString &sandbox);
int runUpgradeReportTests(const QString &sandbox);
int runUpgradeHistoryTests(const QString &sandbox);
int runUpgradeOptionsTests(const QString &sandbox);

// 重命名：名字校验的真值表 + 真改磁盘目录名（含失败路径）。
int runRenameInstanceTests(const QString &sandbox);

// 实例分组：<数据目录>/wtfml.folders。
//
// NOTE: 这一段里所有"到底写进盘里没有"的断言，一律**换一个 InstanceGroupStore 对象读回来**
// 再判。拿同一个对象自证等于没验：它把读到的内容缓存在 m_root 里，盘上那份哪怕一个
// 字节都没动，同一个对象也照样能"读出"它内存里那份看起来正确的值来。
// NOTE: 反过来，每个"先自己写一份原始文件、再读"的子段都必须**新建一个 store**：按需重读
// 的触发条件是"目录变了"，不是"文件变了"（见 InstanceGroupStore::ensureLoaded()）。
// 这是设计如此 —— 一份 .minecraft 里的文件不会被别人改，能改它的只有启动器自己。
int runInstanceGroupTests(const QString &sandbox);

// 判据只要求"认得出哪个是客户端 jar"（<id>.jar 优先，否则顶层恰好一个 jar 就是它），
// 不要求 <id>/<id>.jar（目录名和 jar 名不必一致）。
//
// NOTE: J1 那几条**有意钉住"放松后的判据"**，不是"顺手改坏"。用户自己在资源管理器里
// 改了目录名、没改里面 jar 名的实例，按"目录名和 jar 名必须一致"的判据会从列表里
// **静默消失**（真机上 388 MB 和 851 MB 的整合包各一个），本轮要消灭的正是这种静默。
int runInstalledScanTests(const QString &sandbox);

// 内存建议值：纯函数真值表 + 自动/手动档的落盘行为。
// 真值表是**手算出来的**（不是跑一遍记下输出），所以它能挡住"改了公式没人发现"。
int runMemorySuggestionTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// M. release 文件解析（GlobalJavaSettings::majorVersionFromReleaseFile）
//
// 这个纯函数决定"扫本机 Java 时能不能不启动 JVM"，而它解析错了的后果是**静默认不出**
// 已经装好的 Java（然后白白去下一个几百 MB 的运行时）。所以这里钉两件事：
// 1. 各种写法的真值表 —— 含只写 JAVA_VERSION_DATE 这种假阳性陷阱；
// 2. 真机对账 —— 每个扫出来的运行时，"release 文件里读到的版本"必须等于"真的跑一次
// java -version 问出来的版本"。这一条才是"快路没有改变结果"的凭据，
// 前一条只证明解析器本身对。
// ---------------------------------------------------------------------------
void runEffectiveJavaSettingsTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// S. 实例 Java 版本不符时的提醒开关
//
// 为什么值得单开一段：两处的默认值都必须是"提醒"（一律报错拦下，等于总是提醒）。
// 默认值写反，老配置一升级就变成"悄悄放行一个版本
// 不对的 Java"，而静默选错 Java 正是这一层最怕的错。
// ---------------------------------------------------------------------------
void runJavaWarnMismatchTests(const QString &sandbox);

void runJavaReleaseParseTests();

// ---------------------------------------------------------------------------
// T. 玩家外观（免鉴权的档案请求 + 内容寻址的贴图缓存）
//
// 这一节**全离线**。真正发请求的那一半（官方 200/204 各长什么样）这里一个字节都不碰：
// 状态码怎么读由 T7 的纯函数钉住，缓存命中的那半条路由 T6 的"同步性"钉住，剩下的
// 留给真机验证（见 PlayerSkinStore.hpp 文件头）。
//
// 为什么值得单开一段：这些判断（哪一段该取哪个字段、缓存文件叫什么名字、命中缓存要不要
// 发请求）**没有一行能靠肉眼看截图验准**，而且它们错了的表现全是"看起来没问题" ——
// 比如缓存文件名里少拼一个斜杠，症状只是"每次开窗都重下一遍贴图"，谁都不会察觉。
// ---------------------------------------------------------------------------

/// 造一段"看起来像皮肤 PNG"的字节：8 字节签名 + IHDR + 宽高。
/// NOTE: **不是**一张真能解码的图 —— looksLikeSkinPng() 只看文件头这 24 个字节，
/// 这一节要验的正是"它只看了那么点"（真解码那一步在界面侧，见 .hpp）。
static QByteArray fakeSkinPng(quint32 width, quint32 height) {
    const auto bigEndian = [](quint32 value) {
        QByteArray bytes;
        bytes.append(static_cast<char>((value >> 24) & 0xff));
        bytes.append(static_cast<char>((value >> 16) & 0xff));
        bytes.append(static_cast<char>((value >> 8) & 0xff));
        bytes.append(static_cast<char>(value & 0xff));
        return bytes;
    };
    QByteArray bytes("\x89PNG\r\n\x1a\n", 8);
    bytes.append(QByteArray(4, '\0'));  // 块长度：这里不看它
    bytes.append(QByteArrayLiteral("IHDR"));
    bytes.append(bigEndian(width));
    bytes.append(bigEndian(height));
    return bytes;
}

void runPlayerSkinTests(const QString &sandbox);



// ---------------------------------------------------------------------------
// ZF 节：离线账号**自己挑的**那张皮肤（「换一张」这条路的落点）
// ---------------------------------------------------------------------------
//
// 这一节只验数据层：存在哪、模型那一行怎么归一化、撤掉之后是不是真的一张都不剩。
// "挑完之后屏幕上换不换"由 controller / 主窗口那两条路走，不在这一节。
void runOfflineSkinTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// ZF. 离线名字 → 游戏会给的那张官方默认皮肤（src/manage/DefaultSkin）
//
// 这一节有两半：**纯函数**那一半（Java 的 UUID.hashCode()、18 元表的下标、两套规矩各自的
// 路径、缓存文件名怎么拼）和**真读一个 JAR** 那一半。后半是真跑的：工程自己造几个假版本
// 目录 + 假 JAR，因为这一节最要紧的一条断言恰恰是"判据是**那个 JAR 里有没有这一条**，
// 不是版本号" —— 拿真版本号去判就证不出这件事（而且分界线那两个小版本本机一个都没有）。
//
// 假 JAR 那套工具（makeFakeJar / makePngBytes）在 fakejar.hpp 里，和 mw_check 的 ZG 段共用。
// ---------------------------------------------------------------------------
void runDefaultSkinTests(const QString &sandbox);

/// 把 JSON 值压成一行字符串，用来做"逐字段相等"的比较。
/// QJsonObject 没有值语义的 ==，比较它们的 toJson 是同一件事，而且失败时能直接打印出来。
static QString flatJson(const QJsonValue &value) {
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return value.toString();
}

// ---------------------------------------------------------------------------
// U. 带模组加载器的版本：inheritsFrom 的合并、主类、maven 风格的库
//
// 两个 fixture 都是**真货**：1.19.json 是 Mojang 的元数据，fabric-loader-0.19.5-1.19.json
// 是 meta.fabricmc.net 给的 profile json（原样存下来，一个字没改）。用真数据是因为
// "薄 json"的那些特征（没有 jar、没有 assetIndex、库只有 name+url）正是这节要证的东西，
// 自造的假 json 只会证回我自己以为的格式。
// ---------------------------------------------------------------------------
void runLoaderInheritanceTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// V. 加载器安装：地址怎么拼、列表里怎么挑、profile 能不能装、界面下标怎么翻
//
// 这一节整节**离线**。真正发请求的那两条路在这一节里跑不起来：Fabric 那条要真下几 MB
// 的库、Forge/NeoForge 那条要真起一个 Java 进程装一百秒，沙箱里都没法替。所以把"决定"
// 和"发请求"拆开：所有决定（LoaderMeta 里的纯函数、VersionManager 里的下标翻译）都在
// 这一节钉住；"真发请求那一次"是 `loader-install` 那个**联网**模式（见下面那一段），
// 不进默认模式。
void runLoaderInstallTests();

// ---------------------------------------------------------------------------
// loader-install 模式：**联网**真装一次带加载器的版本，装进沙箱。
//
// 这是 V 节验不了的那一半：真发请求、真落盘、真写 wtfml.meta，装完还顺手验"装出来的
// 东西能不能启动"（启动前检查 + classpath）。四个加载器走的是**三条完全不同的路**
// （Fabric 是我们自己拉 json + 下库；Forge/NeoForge 是另起一个 Java 进程跑它们的安装器
// `--installClient <目录>`；OptiFine 是**同一个安装器的无界面入口** `optifine.Installer`，
// 但它把命令行给的目录整个丢掉、只认 %APPDATA% 下面那个 .minecraft，所以得给子进程换
// APPDATA、把它指到我们自己的一个临时目录上，装完再从那个临时目录里把产物认出来搬回去），
// 所以这个模式也得分着跑：
//
// verify.exe loader-install 1.21.8 （默认 Fabric）
// verify.exe loader-install 1.20.1 forge
// verify.exe loader-install 1.21.1 neoforge
// verify.exe loader-install 1.18.2 optifine
//
// 后面还能再跟一个词 `mirror`：整个安装走**镜像源**（这一条是 加的，用来真验
// "Forge/NeoForge 的下载拨到 bmclapi"这件事 —— 安装器 jar、版本表、还有安装器**自己**下
// 那十几个库（靠给它 `--mirror`）都该走镜像）。不给这个词就是官方源，和产品默认一致。
//
// **不进默认模式**：默认模式必须能离线跑（这一条要联网）。Forge/NeoForge/OptiFine 那三条
// 还额外要求**机器上有 java**（PATH 上，或者用 JAVA_HOME 那种环境变量指一个）—— 它们是
// 真的起一个 Java 子进程，这一点没法在验证里替掉。
//
// 前提：沙箱（bin/.minecraft）里已经有一个装好的原版版本 —— versions/<mc>/ 下
// **同时**有 <mc>.json 和 <mc>.jar。这样这条路只会下加载器自己那点东西（几 MB 的库 /
// 6 MB 的安装器），最后那个"把原版补齐"的步骤也只会走校验 + 补资源，不会去拉原版那
// 几百 MB 的客户端 jar。
//
// NOTE: 装完之后沙箱里会多出来东西（versions/ 下的加载器目录、cache/installers/ 下的
// 安装器 jar、还有 launcher_profiles.json），这是**故意留着的**：这个模式的价值
// 之一就是让人能去盘上看真装出来的是什么样子。清沙箱的话这些东西一起没。
// ---------------------------------------------------------------------------
int runLoaderInstallLive(QCoreApplication &app, const QString &sandbox,
                                const QString &gameVersion, VersionManager::LoaderKind kind,
                                bool useMirror);

// ============================================================================
// W / X 节：账号与外观 —— 多账号存储（AccountStore）和皮肤披风那几条接口的纯函数（SkinApi）
// ============================================================================

/// 造一份"活动槽里那份凭据"该有的样子。字段名照抄 AuthManager 落盘时用的那一套
/// （Minecraft 那份是 access_token / expires_at / name / uuid；微软那份必须有 refresh_token）。
/// expires_at 给一个固定的远未来值 —— 这一节不构造 AuthManager，没人去解析它。
static QJsonObject stubMinecraftPayload(const QString &uuid, const QString &name) {
    QJsonObject payload;
    payload.insert(QStringLiteral("access_token"), QStringLiteral("at-") + name);
    payload.insert(QStringLiteral("expires_at"), QStringLiteral("2030-01-01T00:00:00Z"));
    payload.insert(QStringLiteral("name"), name);
    payload.insert(QStringLiteral("uuid"), uuid);
    return payload;
}

static QJsonObject stubMicrosoftPayload(const QString &refreshToken) {
    QJsonObject payload;
    payload.insert(QStringLiteral("access_token"), QStringLiteral("ms-at-") + refreshToken);
    payload.insert(QStringLiteral("refresh_token"), refreshToken);
    return payload;
}

/// 把"活动槽"摆成某个账号的样子。写盘顺手断言一次 —— 后面那一串断言全都建立在
/// "盘上确实有这两份凭据"之上，前提不成立的话后面的红是误导。
static void putActiveCredentials(const QString &label, const QString &uuid, const QString &name,
                                 const QString &refreshToken) {
    check(TokenStore::writeJson(TokenStore::microsoftKey(), stubMicrosoftPayload(refreshToken)),
          label + QStringLiteral(" 前提：微软凭据写得进沙盒"));
    check(TokenStore::writeJson(TokenStore::minecraftKey(), stubMinecraftPayload(uuid, name)),
          label + QStringLiteral(" 前提：Minecraft 凭据写得进沙盒"));
}

/// 列表里第 index 项（越界返回空项）。**故意不让它崩** —— 一条失败的断言把整节带崩，
/// 后面几十条结果就全看不到了。
static SavedAccount accountAt(const QList<SavedAccount> &list, int index) {
    return index >= 0 && index < list.size() ? list.at(index) : SavedAccount();
}

void runAccountSkinTests(const QString &sandbox);
void runSettingsSyncRetryTests();

// ---------------------------------------------------------------------------
// V. 整合包导入（Modrinth 格式）：清单解析、路径判据、按前缀解压
//
// 三层各验各的，因为它们的错法完全不同：
// * 纯函数（ModpackImport）：一段 json 进去、一个结果出来，不碰磁盘不联网；
// * ZipUtil 那两个新函数：真造一个压缩包、真解一次 —— 下面 V5 钉的两个坑都是实测
// 踩出来的（条目名里的反斜杠、两个前缀的优先级）；
// * 真清单：meta/modrinth-index-att2.json 是从一个真整合包（779 MB，ATT2）里原样
// 读出来的，用它证"真实的一百多条路径一条都没被误判"—— 自造的假 json 只会证回
// 我自己以为的格式。
// ---------------------------------------------------------------------------
// =========================================================================
// Y 节：下载页那四个分类 + 四个加载器的「能挑哪些版本」。
//
// 这一节全是**真数据**（meta/ 下那几份夹具都是原样存下来的真响应），因为要证的恰恰是
// 「这份真清单/真清单文件里的分布对不对」—— 自己编几条只能证明「我按我写的规则分了堆」。
// =========================================================================
void runVersionChannelTests();

void runModpackImportTests(const QString &sandbox);

/// 一个只答 CurseForge 那两条路的假接口（127.0.0.1、随机端口）。
///
/// ZG7 用它把**中间那一发**钉住 —— 上面几节钉的都是两头（清单进 / 答复出），而"地址拼成
/// 什么样、正文写成什么样、带了哪个头、答复怎么落回 pack"只有在这里量得出来。它不碰外网，
/// 在哪台机器上跑都是同一个结果。
///
/// NOTE: 正文必须**按 Content-Length 收全了再答**，答完带着 `Connection: close` 关掉。
/// 理由：HttpClient 那一侧用的是同一个 QNAM（VersionManager::loaderNetworkManager），
/// Qt 会把连接留着复用，而"一个连接上只答一发"那种图省事的写法（本文件里 V6f 那两
/// 个假镜像就是这个形状 —— 它们**故意**不答）在第二个请求上会一声不响地不答，那一发
/// 就得等满 15 秒超时，整节的结论全变。
struct FakeCurseForgeApi {
    QTcpServer server;
    int filesStatus = 200;
    QByteArray filesBody;
    int modsStatus = 200;
    QByteArray modsBody;
    int filesHits = 0;
    int modsHits = 0;
    QStringList paths;    ///< 每一发问的那条路径（按到的次序）
    QList<QByteArray> bodies;
    QStringList apiKeys;  ///< 每一发带的 x-api-key（**没带 = 空串**，这一档要能分出来）

    /// 按**请求目标**取那一发的东西。两发是并着发出去的（`resolveCurseForgeFiles` 里
    /// 文件那一发和类目那一发各自 QNetworkReply），谁先回来不该被断言 —— 所以判据一律
    /// 按路径查，不按到达次序取下标。
    int indexOfPath(const QString &path) const { return paths.indexOf(path); }
    QByteArray bodyForPath(const QString &path) const {
        const int index = indexOfPath(path);
        return index < 0 ? QByteArray() : bodies.value(index);
    }
    QString apiKeyForPath(const QString &path) const {
        const int index = indexOfPath(path);
        return index < 0 ? QString() : apiKeys.value(index);
    }

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { onReadyRead(socket); });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const {
        // 结尾**不带**斜杠：和产品自己拼地址的写法一致（curseForgeApiBase 结尾会去掉它）。
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    }

    void onReadyRead(QTcpSocket *socket) {
        QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            socket->setProperty("buffer", buffer);  // 头还没到齐
            return;
        }
        const QByteArray head = buffer.left(headerEnd);
        const QByteArray rest = buffer.mid(headerEnd + 4);
        int length = 0;
        QString apiKey;
        QString path;
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            const int colon = trimmed.indexOf(':');
            if (colon > 0) {
                const QByteArray name = trimmed.left(colon).trimmed().toLower();
                const QString value = QString::fromUtf8(trimmed.mid(colon + 1)).trimmed();
                if (name == QByteArrayLiteral("content-length")) {
                    length = value.toInt();
                } else if (name == QByteArrayLiteral("x-api-key")) {
                    apiKey = value;
                }
            } else if (trimmed.startsWith(QByteArrayLiteral("POST "))) {
                path = QString::fromUtf8(trimmed.split(' ').value(1));
            }
        }
        if (rest.size() < length) {
            socket->setProperty("buffer", buffer);  // 正文还没到齐
            return;
        }
        socket->setProperty("buffer", QByteArray());

        const bool isFiles = path.endsWith(QStringLiteral("/mods/files"));
        if (isFiles) {
            ++filesHits;
        } else {
            ++modsHits;
        }
        paths << path;
        bodies << rest.left(length);
        apiKeys << apiKey;

        const int status = isFiles ? filesStatus : modsStatus;
        const QByteArray payload = isFiles ? filesBody : modsBody;
        socket->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' +
                      (status == 200 ? QByteArrayLiteral("OK") : QByteArrayLiteral("Error")) +
                      QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
                      QByteArray::number(payload.size()) +
                      QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + payload);
        socket->flush();
        socket->disconnectFromHost();
    }
};

void runCurseForgeTests();
void runModpackCompletionTests(const QString &sandbox);
void runAssetDirectorKnownSetTests(const QString &sandbox);

// =========================================================================
// Z 节：下载页「模组 / 整合包 / 资源包 / 光影」那一层（ResourceCatalog）。
//
// 全是纯函数：一段 JSON 进去、一个结果出来，不联网不碰盘。喂的两份夹具是**真响应原样存下来的**
// （meta/modrinth-search-mod.json = 一次真搜索的三条；meta/modrinth-versions-iris.json =
// Iris 在 1.21.1 + fabric 下的 12 个版本）—— 这一节要证的恰恰是"真响应里那几个字段我们读对了"，
// 自己编一段 JSON 只能证明"我按我以为的格式读了一遍"。
//
// 拼地址那几条**不走"URL 里有没有这段话"那种子串断言**，而是把参数解码回来再当 JSON 读一遍：
// 子串断言在"少了一层方括号"这种错法上照样过（`[["project_type:mod"]]` 去掉一层之后，
// 里面那截字还是原样在的），而那个错在真接口上不报错、只是筛选静默失效。
// =========================================================================

/// 取 URL 里某个查询参数，**按百分号解码回原样**（拼地址那几条断言要读回原值）。
///
/// NOTE: 这里是自己切 `&` / `=` 再 QUrl::fromPercentEncoding，**不是** QUrlQuery。
/// 实测QUrlQuery 把 `%3A` 解回了 `:`，却把 `%5B` / `%5D` 原样留着 ——
/// 它只解"它认为必须解"的那些，于是读回来的是半解码的东西，看着像产品把地址拼错了
/// （三条断言就是这么假红的）。要测的是"拼进去的原值能不能原样读回来"，那就自己解，
/// 别用一个自带一套规矩的解析器去验另一个拼串的人。
static QString urlParam(const QString &url, const QString &key) {
    const int question = url.indexOf(QLatin1Char('?'));
    if (question < 0) {
        return QString();
    }
    const QStringList pairs = url.mid(question + 1).split(QLatin1Char('&'));
    for (const QString &pair : pairs) {
        const int equals = pair.indexOf(QLatin1Char('='));
        if (equals > 0 && pair.left(equals) == key) {
            return QUrl::fromPercentEncoding(pair.mid(equals + 1).toUtf8());
        }
    }
    return QString();
}

/// 把一段字符串当 JSON 读（上面那几条断言用它验 facets / game_versions 的形状）。
static QJsonDocument jsonOf(const QString &text) {
    return QJsonDocument::fromJson(text.toUtf8());
}

void runResourceCatalogTests();

// =========================================================================
// Z8 节：**资源详情页**那一页要的那几件（都是纯函数，离线钉得住）。
//
// 那一页（ui/ResourceDetailPage）自己只负责画：分组怎么分、chip 筛什么、时间那句怎么说、
// 渠道那一档是哪一档，全在 ResourceCatalog 里 —— 那也正是这一节要钉的。
//
// 为什么要一节专门的：这几句话**错了不会报错**，只会让人看到一份"少了几个版本"的列表
// （筛错了）、或者一条看不见的渠道标签（档位认错了）。两类都能在这一节里量出来。
// =========================================================================

void runResourceDetailTests();

// =========================================================================
// DQ 节：主页「每日一言」的判据层（`src/util/DailyQuote`）—— 语种与限长、按日期
// 定选题、三种接口的解析、当天缓存、句子池、36 条兜底、署名。全脱网：喂的都是写死的字节。
// =========================================================================

void runDailyQuoteTests();

// HG 节：主页大标题的问候规则（`src/util/HomeGreeting`）—— 时段边界、首次与升级老用户、久别、有名无名。
void runHomeGreetingTests();

// =========================================================================
// ZI 节：资源商店那几发请求也走共享的 Control 额度。
//
// 纯接线（搜索 / 带正文的 POST / 图标 / 换发与删作业），本机假接口，一个字节都不出网。
// 排队与截止本身的语义在 PH6 / T8 / RequestBudgetChecks 里钉，这里不重复。
// =========================================================================

void runResourceStoreBudgetTests();

// =========================================================================
// R1 节：更新检查那个批次表（`ResourceStore::LatestRun`）的收场（R1）。
//
// 每一发跑完都会 `deleteLater()` 把自己收掉 —— 一发已回、别的还在飞的时候，
// 同一把钥匙重建批次或者直接关窗，**不许**对已经析构的作业再删一次。
// 本机假接口，一个字节都不出网。
// =========================================================================

void runResourceStoreLatestRunTests();

// =========================================================================
// ZB 节：下载页图标的**盘上那一份缓存**、它的**凭据**、以及那一发请求本身
// （都是 ResourceStore 的静态部分）。
//
// 纯函数 + 文件层，不联网：这一节一次都不构造 ResourceStore（一构造就建
// QNetworkAccessManager），所以图标那条真网络路仍然只有 mw_check 的 resource-live 覆盖。
//
// 为什么这些非得钉住：图标这一路 实测从国内**根本走不通** ——
// `cdn.modrinth.com` 对每一张图只回一个 307，真正的字节在 `cdn-alt.modrinth.com` 上，
// 而那个域名连不上。于是"盘上那份缓存"就变成用户还能看到图的唯一来源，它拼错路径，
// 症状只是"图标又变慢了"，没有任何东西会报错。
//
// 所以这一节钉的是三件事：那份字节读得回来、那条**凭据**存得住读得回、以及**那一发
// 请求到底带不带条件头**（真值表在 ZB3）。第二、三件事最要紧 —— 凭据读没了 =
// "核对"退化成每次整张重下（用户那句"如果严重占用网络就不做了"正好踩在这）；
// 条件头发错了（比如两个空头也照发）= 主机怎么答都不好说，而这**不会报任何错**。
// =========================================================================

/// 一张现造的、解得出像素的 PNG（这一节要拿它当真图标字节用）。
static QByteArray solidPngBytes(const QColor &color, int side = 32) {
    QImage image(side, side, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    buffer.close();
    return bytes;
}

/// 一个只答一种话的假图标服务器（127.0.0.1、随机端口）。
///
/// 记着"来问过几次"和"那一发带了哪些条件头"—— ZB8 要验的东西全在这两件事上。用它的
/// 理由和 V6f 那个假镜像一样：**不碰外网**，在哪台机器上跑都是同一个结果；而"核对那一发"
/// 是"核对那一发"最要紧的那条路，光靠真网络那条探针（mw_check icon-live，只报数不断言）
/// 是钉不住的。
struct FakeIconServer {
    QTcpServer server;
    QByteArray body;      ///< 200 时发回去的正文
    QByteArray headers;   ///< 200 时额外带的头（ETag / Last-Modified 那两条）
    int status = 200;
    int requests = 0;
    QString lastIfNoneMatch;
    QString lastIfModifiedSince;

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    if (socket->property("answered").toBool()) {
                        return; // 一个连接上只答一发（这一发不带正文，读一次就够）
                    }
                    socket->setProperty("answered", true);
                    const QByteArray request = socket->readAll();
                    ++requests;
                    const int headerEnd = request.indexOf("\r\n\r\n");
                    const QByteArray head = headerEnd > 0 ? request.left(headerEnd) : request;
                    for (const QByteArray &line : head.split('\n')) {
                        const QByteArray trimmed = line.trimmed();
                        const int colon = trimmed.indexOf(':');
                        if (colon <= 0) {
                            continue;
                        }
                        const QByteArray name = trimmed.left(colon).trimmed().toLower();
                        const QString value =
                            QString::fromUtf8(trimmed.mid(colon + 1)).trimmed();
                        if (name == "if-none-match") {
                            lastIfNoneMatch = value;
                        } else if (name == "if-modified-since") {
                            lastIfModifiedSince = value;
                        }
                    }
                    QByteArray response = QByteArrayLiteral("HTTP/1.1 ") +
                                          QByteArray::number(status) +
                                          (status == 304 ? QByteArrayLiteral(" Not Modified\r\n")
                                                         : QByteArrayLiteral(" OK\r\n"));
                    if (status == 304) {
                        // 没变：一个字节的正文都没有。**这一条正是"不严重占网络"的全部根据。**
                        response += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
                    } else {
                        response += headers;
                        response += QByteArrayLiteral("Content-Length: ") +
                                    QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n");
                        response += body;
                    }
                    socket->write(response);
                    socket->flush();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

void runIconCacheTests();

// =========================================================================
// ZC. 一串字节 → 一张图（src/util/ImageDecode）
//
// 这一节验的是"图标怎么解"这件事的**两半**：Qt 自己认得的那一半（png / jpg / gif…）和
// Qt 不认得的 webp（走系统 WIC）。第二半有个特殊之处：**它在这台机器上有没有，是个运行时
// 事实**（那个编解码器是微软单独发的扩展包，Win11 默认带着、Win10 上要装商店那个
// 「WebP 图像扩展」），所以断言必须按 webpAvailable() 分两支 —— 只写一支的话，换一台机器
// 跑出来就是红的，而那不是回归。
// =========================================================================

void runImageDecodeTests();

// =========================================================================
// ZD. 设置页「启动选项」那三项（设置 → 命令行这一条链）
//
// NOTE: 有一件事这一节**证不到**，如实写在这儿：applyProcessPriority() 调的是 Win32 的
// SetPriorityClass()，它躲在 LauncherManager.cpp 的匿名命名空间里、还要一个真进程
// 句柄。能验的那一半（档位 → 字符串 → 档位、认不出来的值兜到 Normal）在下面；
// 另一半（那句 Win32 调用到底有没有生效）只能真机上开任务管理器看一眼。
//
// NOTE: 窗口大小那几条为什么这么绕（合成一份 json、而不是只喂真夹具）：真夹具里那条
// `--width` 规则挂在 `has_custom_resolution` 上，而启动器一律传空 feature 集合，
// 所以那条规则**根本没命中过** —— 只喂真夹具的话，"我们发的那一对"和"json 那条
// 被丢掉了"这两件事里任何一件坏掉，结果都是"恰好一条 --width"，红不了。所以要再
// 造一份**把那个值写死成别的数字**的 json：谁发的这一对，从这里就分得出来了。
// =========================================================================

void runLaunchOptionsTests(const QString &sandbox);

// ---------------------------------------------------------------------------
// ZPL. 起进程那条 260 字符的路径墙（`ProcessPathLimit`）
//
// 这一层**只解释、不拦**：判断本身是纯的（长度 ≥ 260），用途只有一个 —— 在
// `QProcess` 报 `FailedToStart` 之后给用户补一句「路径太长」。所以这里钉的是
// 边界（259 不提示 / 260 提示）、挑最长的那条、以及"提示里一定是完整路径"。
// 「这个判断有没有接到那四个启动点上」归 mw_check / 真机，不在这一节。
// ---------------------------------------------------------------------------
void runProcessPathLimitTests();

// =========================================================================
// ZH. 服务器列表：真的连上去问一句（Server List Ping / 状态协议）
//
// 用户 问的「服务器列表能实际 ping 服务器并显示服务器图标和 motd 吗」。这一
// 节分两半，因为两半的错法完全不同：
//
// * **拆包那一半**（地址怎么拆、VarInt 怎么编怎么读、MOTD 怎么展平、图标那串 data URL
// 怎么解）全是纯函数，喂什么进来看什么出来；
// * **协议那一半必须真连一次**。只验纯函数的话，"我们发出去的握手长什么样"和"服务器真
// 答回来一坨我们读不读得下来"这两件事一条都没碰到 —— 而真机上错的恰恰是这两件：握手
// 里写错一格（比如下一状态写成 2）服务器会等一个永远不来的登录包，一个字节都不答；
// 收包不按长度收全，带图标的响应会缺尾巴、解出来是一张破图。所以下面起一台**真**
// QTcpServer 说这个协议，并且把它**收到的字节**逐字段拆开核对。
//
// NOTE: 假服务器**跑在自己的线程里**，这不是讲究：产品的 ServerPing::ping() 是同步的
// （waitForConnected / waitForReadyRead 一路等到底），中间**不转 Qt 的事件循环**。
// 同一个线程里摆一台 QTcpServer，那一侧的新连接通知永远排不上队，看到的会是"连上了
// 但没人回答" —— 那正好是超时那一档的样子，而那是测试自己的错，不是产品的。
//
// NOTE: 这一节只碰 127.0.0.1，**一条都不出网**（所以断网的机器上也该全绿）。
// =========================================================================

/// 一台说 Server List Ping 的假服务器（127.0.0.1、随机端口），跑在自己的线程里。
///
/// 它只做两件事：**把收到的字节留着**（用来证我们发出去的那个握手是对的），以及按 mode
/// 回一句。它不解析收到的包 —— 拆包那件事产品自己会做，这里要是也写一份解析，两边一起
/// 错成同一个样子就谁也看不出来了。
class FakeStatusServer : public QThread {
public:
    /// 服务器怎么答这一发。
    enum Mode {
        Answer,      ///< 正常答：包里是 json 那段
        Silent,      ///< 收下、**一个字都不回**（"服务器没有回答"那一档）
        Garbage,     ///< 回一个包，正文不是 JSON
        HugeLength,  ///< 只回一个长度前缀，报一个天文数字（测那道上限闸）
        NotStatus,   ///< 包 id 不是 0x00
        ShortBody,   ///< 包自己那个长度是诚实的，坏的是里面"正文多长"那一格
    };

    explicit FakeStatusServer(Mode mode) : m_mode(mode) {}

    /// 响应里那段 JSON（Answer / Garbage / NotStatus / ShortBody 这几档用）。
    QByteArray json = QByteArrayLiteral("{}");
    /// 服务器**收到的**字节（原样，按到的次序）。
    QByteArray seen;
    /// 收到几个连接（"连不上"那几档根本不进这里）。
    int connections = 0;
    /// 起来之后监听的端口。
    quint16 port = 0;
    /// listen 成没成。
    bool listening = false;

    /// 起了、端口也拿到了才返回 —— 不然 port 还是 0，调用方会连到"随便一个端口"上去。
    void startAndWait() {
        start();
        m_ready.acquire();
    }

    void run() override {
        QTcpServer server;
        listening = server.listen(QHostAddress::LocalHost, 0);
        if (!listening) {
            m_ready.release();
            return;
        }
        port = server.serverPort();
        m_ready.release();

        // 等一个连接：**用这条线程自己的事件循环**（这条线程上没有别的事要转，所以这样最
        // 稳），外面套一个硬上限 —— 这一条要是等不着，整节就没法往下走。
        {
            QEventLoop loop;
            QObject::connect(&server, &QTcpServer::newConnection, &loop, &QEventLoop::quit);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            if (!server.hasPendingConnections()) {
                loop.exec();
            }
        }
        QTcpSocket *socket = server.nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        ++connections;

        // 收"握手 + 状态请求"这两个包。这里**不解析**，只等它们到齐 —— 客户端是两发一起
        // 写出去的，所以一次 readAll 就把两个包一起拿到了。
        //
        // NOTE: 退出条件必须写成"读到了、而且对面接着没别的了"，**不能**写成"读到够 N 个
        // 字节"：握手一共才 22 个字节，写成 `seen.size() < 48` 的话每一次都会在这儿
        // 干等满那个上限，于是下面量"耗时"的每一条断言（latency、当场放弃那两条）
        // 量到的全是这段死等的时间 —— 实测就是 1.6 秒，看起来像产品慢。
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 2000) {
            if (!socket->waitForReadyRead(120)) {
                if (!seen.isEmpty()) {
                    break;
                }
                continue;  // 还没拿到任何东西，接着等（最多 2 秒）
            }
            seen += socket->readAll();
        }

        if (m_mode == Silent) {
            // 收下、不回：让客户端自己等到超时（那一档验的就是这个）。
            //
            // **监听不能收**：「叫停之后马上再测一趟」那一节会在上一趟还卡在超时里时
            // 再连进来。要是这里等第一条断开就 `return`，监听跟着没了，第二趟那条
            // 已经握上手、还在 backlog 里没被 accept 的连接会被 RST 掉 —— 量到的
            // 是"对面把连接断了"，不是"连上了没答话"，`timedOut` 也就不是 true。
            // 所以这里继续守着端口，来一条收一条、一条都不答，守够 4 秒再收摊
            // （测试那边最多等到两趟各 600 ms，离 4 秒很远）。
            QList<QTcpSocket *> held;
            held.append(socket);
            QEventLoop holdLoop;
            QObject::connect(&server, &QTcpServer::newConnection, &holdLoop, &QEventLoop::quit);
            QTimer tick;
            tick.setInterval(50);
            QObject::connect(&tick, &QTimer::timeout, &holdLoop, &QEventLoop::quit);
            tick.start();
            QElapsedTimer holdClock;
            holdClock.start();
            while (holdClock.elapsed() < 4000) {
                while (server.hasPendingConnections()) {
                    if (QTcpSocket *next = server.nextPendingConnection()) {
                        ++connections;
                        held.append(next);
                    }
                }
                holdLoop.exec();
            }
            qDeleteAll(held);
            return;
        }

        socket->write(replyBytes());
        socket->waitForBytesWritten(1000);
        socket->flush();
        // 客户端读完就关；给它一点时间把包收全，然后收摊。
        socket->waitForDisconnected(1500);
        delete socket;
    }

private:
    /// 一个完整的响应包（VarInt 长度前缀 + 正文）。
    QByteArray replyBytes() const {
        QByteArray body;
        switch (m_mode) {
        case Garbage:
            body = ServerPing::varIntBytes(0x00) + ServerPing::varIntBytes(json.size()) + json;
            break;
        case NotStatus:
            body = ServerPing::varIntBytes(0x01) + ServerPing::varIntBytes(json.size()) + json;
            break;
        case ShortBody:
            // 包自己那个长度是**诚实的**（客户端会老老实实收全），坏的是里面"正文多长"那
            // 一格 —— 这两种长度是两回事，只有分别喂得出来才分得清。
            body = ServerPing::varIntBytes(0x00) + ServerPing::varIntBytes(9999) + json;
            break;
        case HugeLength:
            // 只发前缀，一个字节正文都不发：客户端应当**看到这个数就放弃**，而不是等一堆
            // 永远不来的字节（那正好是"超时"这条路上最难看的一种死法）。
            return ServerPing::varIntBytes(kTooBigForTest);
        case Answer:
        case Silent:
        default:
            body = ServerPing::varIntBytes(0x00) + ServerPing::varIntBytes(json.size()) + json;
            break;
        }
        return ServerPing::varIntBytes(body.size()) + body;
    }

    /// 比产品那道 1 MiB 的上限（ServerPing.cpp 里那个 kMaxPacketBytes）大就行 —— 这里写
    /// 4 MiB，离上限足够远，将来上限调大一点这条也不会跟着红。
    static constexpr int kTooBigForTest = 4 * 1024 * 1024;

    Mode m_mode;
    QSemaphore m_ready;
};

/// 收摊：等假服务器那条线程跑完。
///
/// NOTE: **必须等**。一个还在跑的 QThread 被析构掉（"QThread: Destroyed while thread is
/// still running"）会直接把进程带崩，而这儿崩掉的话后面那些节一条都跑不到 —— 看
/// 起来会像是"别的东西被弄坏了"。run() 里每一段都有上限，所以正常情况下
/// 这里一定等得到。
static void waitFakeServer(FakeStatusServer &server) {
    if (!server.wait(12000)) {
        check(false, "ZH 假服务器那条线程没停下来（最多等 12 秒）");
        server.terminate();
        server.wait(3000);
    }
}

/// 几台假服务器互相等的那个闸（ZH10 用）：每一台连上来时报一次到，然后**等到大家都到齐**。
///
/// 这是"多线程一起问"这件事唯一能被**确定地**验出来的构造：量耗时是环境说了算的
/// （本机连一个没人听的回环端口要等满整个超时，ZH7 实测过），而"你等我、我等你"这件事
/// 串行跑**根本不可能**成立 —— 第 1 台永远等不来第 2 台。
class GateBarrier {
public:
    explicit GateBarrier(int expected) : m_expected(expected) {}

    /// 报一个到，然后等大家都到齐（最多 @p ms 毫秒）。
    /// @return 到齐了没有。**等不到时调用方必须做点看得见的坏事**（这里是"不回话"），
    /// 否则"没到齐"会被下一句照常回的那一答盖过去，整节就变成了一个假绿。
    bool arriveAndWait(int ms) {
        QMutexLocker locker(&m_mutex);
        ++m_arrived;
        if (m_arrived >= m_expected) {
            m_condition.wakeAll();
            return true;
        }
        return m_condition.wait(&m_mutex, ms);
    }

    int arrived() {
        QMutexLocker locker(&m_mutex);
        return m_arrived;
    }

private:
    QMutex m_mutex;
    QWaitCondition m_condition;
    const int m_expected;
    int m_arrived = 0;
};

/// 一台"要等到大家一起连上来才回话"的假服务器（ZH10 用）。
///
/// 它和 FakeStatusServer 是两路：那一台是"按 mode 答一发给一台客户端"，这一台的重点全在
/// **它什么时候才答**。答的那句话里带着自己的号（`{"description":"gate-<n>"}`），所以
/// 每一条结果都能认出是不是自己那一台 —— 并发之后结果回来的次序不再等于下标次序，
/// 这一点得有东西钉住。
class GatedStatusServer : public QThread {
public:
    GatedStatusServer(GateBarrier *gate, int id) : m_gate(gate), m_id(id) {}

    /// 起来之后监听的端口。
    quint16 port = 0;
    bool listening = false;

    /// 起了、端口也拿到了才返回（同 FakeStatusServer::startAndWait）。
    void startAndWait() {
        start();
        m_ready.acquire();
    }

    void run() override {
        QTcpServer server;
        listening = server.listen(QHostAddress::LocalHost, 0);
        if (!listening) {
            m_ready.release();
            return;
        }
        port = server.serverPort();
        m_ready.release();

        QEventLoop loop;
        QObject::connect(&server, &QTcpServer::newConnection, &loop, &QEventLoop::quit);
        // 上限掐得比产品那道 ping 超时（默认 4 秒）短一截：这一格里等不到就该走"没到齐"，
        // 让它和"对面连都不来连"这两件事在时间上分得开。
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        if (!server.hasPendingConnections()) {
            loop.exec();
        }
        QTcpSocket *socket = server.nextPendingConnection();
        if (socket == nullptr) {
            return;
        }

        // 先把"握手 + 状态请求"收下再进闸门 —— 对面是两发一起写出去的，收完这两个包
        // 这一台才算"真的连上来了"。
        QElapsedTimer clock;
        clock.start();
        QByteArray seen;
        while (clock.elapsed() < 2000) {
            if (!socket->waitForReadyRead(120)) {
                if (!seen.isEmpty()) {
                    break;
                }
                continue;
            }
            seen += socket->readAll();
        }

        // 这一节的全部要害就在这一句：**等别的几台也连上来**。
        const bool allHere = m_gate->arriveAndWait(2500);
        if (!allHere) {
            // 等不到 = 对面不可能是同时在问。**一个字都不回**：让那一条走到超时，
            // 于是"串行"这件事变成一条红的断言，而不是"慢一点但是过了"。
            socket->waitForDisconnected(500);
            delete socket;
            return;
        }

        const QByteArray json =
            QStringLiteral("{\"description\":\"gate-%1\"}").arg(m_id).toUtf8();
        const QByteArray body =
            ServerPing::varIntBytes(0x00) + ServerPing::varIntBytes(json.size()) + json;
        socket->write(ServerPing::varIntBytes(body.size()) + body);
        socket->waitForBytesWritten(1000);
        socket->flush();
        socket->waitForDisconnected(1500);
        delete socket;
    }

private:
    QSemaphore m_ready;
    GateBarrier *m_gate;
    int m_id;
};

void runServerPingTests();

// ---------------------------------------------------------------------------
// ZK 节用的那几支：**手搓 NBT 字节**。
//
// 故意不借 `NbtTree::serialize` 造夹具 —— 借产品自己的序列化器造夹具，验的就变成"它自己
// 和自己一致"。手搓的这一份同时就是"游戏写出来的那份形状"的样本，字段顺序照真机上那几份
// `servers.dat`（`hidden` → `ip` → `name` → `icon`）。
// ---------------------------------------------------------------------------
namespace {

/// NBT 的一格：类型 + 名字（u16 长度 + UTF-8）+ 载荷。
QByteArray nbtTag(quint8 type, const QByteArray &name, const QByteArray &payload) {
    QByteArray out;
    out.append(static_cast<char>(type));
    out.append(static_cast<char>((name.size() >> 8) & 0xFF));
    out.append(static_cast<char>(name.size() & 0xFF));
    out.append(name);
    out.append(payload);
    return out;
}

/// TAG_String 的载荷：u16 长度 + UTF-8 字节。
QByteArray nbtString(const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    QByteArray out;
    out.append(static_cast<char>((utf8.size() >> 8) & 0xFF));
    out.append(static_cast<char>(utf8.size() & 0xFF));
    out.append(utf8);
    return out;
}

/// TAG_Int 的载荷（大端）。
QByteArray nbtInt(qint32 value) {
    QByteArray out;
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
    return out;
}

/// 一个复合标签的载荷：里面那几格 + 结尾的 TAG_End。
QByteArray nbtCompoundBody(const QList<QByteArray> &tags) {
    QByteArray out;
    for (const QByteArray &tag : tags) {
        out.append(tag);
    }
    out.append('\0');
    return out;
}

/// 一格 `TAG_List<TAG_Compound>` 的载荷 —— 表里每一条**不带名字也不带类型**（NBT 的规矩，
/// 类型和条数写在表头）。
QByteArray nbtServerList(const QList<QByteArray> &items) {
    QByteArray out;
    out.append('\x0a');  // 元素类型 = TAG_Compound
    out.append(nbtInt(static_cast<qint32>(items.size())));
    for (const QByteArray &item : items) {
        out.append(item);
    }
    return out;
}

/// 一条服务器。`hasName` / `hasIp` 为假 = 盘上根本没有那一格（1.12.2 那种老 fixture 里就
/// 没有 `name`；没有 `ip` 的那一条则**根本不会出现在界面上**，见 ZK7）。
struct NbtServerItem {
    QString name;
    QString ip;
    bool hasName = true;
    bool hasIp = true;
    bool hidden = false;
    bool hasHidden = false;
    QString icon;
};

QByteArray nbtServerItem(const NbtServerItem &item) {
    QList<QByteArray> tags;
    if (item.hasHidden) {
        tags.append(nbtTag(0x01, QByteArrayLiteral("hidden"),
                           QByteArray(1, static_cast<char>(item.hidden ? 1 : 0))));
    }
    if (item.hasIp) {
        tags.append(nbtTag(0x08, QByteArrayLiteral("ip"), nbtString(item.ip)));
    }
    if (item.hasName) {
        tags.append(nbtTag(0x08, QByteArrayLiteral("name"), nbtString(item.name)));
    }
    if (!item.icon.isEmpty()) {
        tags.append(nbtTag(0x08, QByteArrayLiteral("icon"), nbtString(item.icon)));
    }
    return nbtCompoundBody(tags);
}

/// 一整份 `servers.dat`：**无名**根复合标签 → 一格 `servers`。
QByteArray nbtServersDat(const QList<NbtServerItem> &items) {
    QList<QByteArray> encoded;
    for (const NbtServerItem &item : items) {
        encoded.append(nbtServerItem(item));
    }
    return nbtTag(0x0A, QByteArray(),
                  nbtCompoundBody({nbtTag(0x09, QByteArrayLiteral("servers"),
                                          nbtServerList(encoded))}));
}

bool writeBytesAt(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray readBytesAt(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

/// 按名字取一格字符串（不在 / 不是字符串 → 空串）。ZK7 / ZK8 要直接看盘上那棵树。
QString nbtFieldText(const NbtTree::Node &compound, const QString &name) {
    const NbtTree::Node *node = NbtTree::field(compound, name);
    return (node != nullptr && node->type == NbtTree::Type::String) ? node->text : QString();
}

/// 一个复合标签里那几格的名字，按**盘上的顺序**。
QStringList nbtFieldNames(const NbtTree::Node &compound) {
    QStringList names;
    for (const NbtTree::Field &field : compound.fields) {
        names.append(field.first);
    }
    return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// ZK 节：`servers.dat` 的写侧（`ServerListFile` 的**改 / 加 / 删**三个入口）。
//
// 要求：「在服务器列表那里做一个编辑地址和名称的功能」。读那一半仍旧走
// `NbtUtil`（只读那条设计决定一个字没松），这一节钉的是**写**。
//
// ZK12 往后是同一页的另两件事（要求：「编辑要能改名字改地址以及删除，服务器那个页面里
// 放一个添加服务器的按钮，允许添加」）：加（`addEntry`）和删（`removeEntry`）。三个入口
// **共用同一个开头**（`loadServers()` + `findEntryIndex()`），所以这里钉的很多条其实是在
// 钉那个开头 —— 那正是"认不出就不写"只有一处实现的好处。
//
// 最值钱的一条是 ★「**认的是值不是下标**」：`NbtUtil::readServerList()` 会跳过没有 `ip` 的
// 元素，于是"界面上第 N 行"和"盘上第 N 个元素"不是一回事 —— 照下标去改就是**改错行**，而且
// 改完在界面上看不出来（名字和地址都还是"合法"的，只是长在了别人身上）。所以夹具里故意把
// 一条没有 `ip` 的塞在最前面，改完回盘上逐格看是谁被改了。
//
// 另一半是「**拒写就得是一个字节都不动**」：每一条拒写的路都当场比对整份字节。
// ---------------------------------------------------------------------------
void runServerListEditTests();

// ---------------------------------------------------------------------------
// R2. NBT 读入的两个上限（R2）。
//
// 正文字节数（`kMaxSize`）管不住"一个字节变成一个 Node"：List 的元素最少只占一个字节，
// 项数却是对方说了算。这里钉"元素总数过头上限就拒绝"和"项数 × 元素最少字节 > 剩余字节
// 就拒绝"，顺带钉一条正常规模照样读得动。
// ---------------------------------------------------------------------------
void runNbtLimitsTests();

// ---------------------------------------------------------------------------
// ZK17 节起：上面那三个写入口**谁在指挥**（`ServerListController`）。
//
// ZK1–ZK16 钉的是数据层本身（`ServerListFile`）。可是"什么时候该写、什么时候一个字都不该
// 动"是另一层的事，落在 `ServerListController`。那四道闸（两样都没变就别写 / 地址
// 必填 / 认值不认下标 / 改完重读重测）**一条都得直接钉住** —— 只钉对话框信号
// （mw_check 那几条）验不到"有没有落盘"。
//
// 假 Host 记四本账：问过哪几句（连预填一起）、弹过什么、记过哪几行日志、喊过几次"重读重测"。
// ---------------------------------------------------------------------------
void runServerListControllerTests();

// ---------------------------------------------------------------------------
// ZL 节：quick play 那两个参数（`QuickPlay`）。
//
// 要求：「世界和服务器都做一个临时占位（加入选中的世界（仅限有这个参数的
// 版本）/服务器）先试试看能不能生效」。
//
// 这一节钉三件事：
// 1. **判据的形状**（`supports()`）—— 认的是版本 json 的 `arguments.game` 里有没有写着
// 这个名字，**不是版本号**。真 json（`meta/26.2.json`）是照游戏自己的选项表生成的，
// 所以"和真写的那一份对得上"这件事只有拿真 json 才验得出来；
// 2. ★ **要问合并过的那一份** —— 加载器版本的 profile 里一条 quick play 都没有（全在父
// 版本那份里），拿没合并的那份问，Forge / Fabric 的实例会一个都答"不支持"；
// 3. 这一对参数**必须由启动器自己发**（json 自己声明的那一条会被整条丢掉）。
// ---------------------------------------------------------------------------
void runQuickPlayTests(const QString &metaDir);

// 这一份文本**是要发给别人的**（拖进群里、贴进 issue），所以它长什么样得能被钉死 ——
// 而它偏偏又最容易"悄悄变样"（改一句措辞、多一个字段，谁也不会当场看出来）。所以这一节
// 把整份报告逐行钉住：头部那几行的顺序与形状、退出码那一行**只在真的收到过的时候**写、
// 空的那一段写一句"一行都没有"、以及两种文件名的清洗。
//
// NOTE: 时间是从外面传进去的（`ErrorReport` 不读时钟），所以 `生成时间：` 那一行在这里
// 是个定值 —— 这一条是整个文件里唯一能钉住"时间那一格长什么样"的办法。
void runErrorReportTests();
int runLaunchScriptExportTests();

// =========================================================================
// ZJ. 「复制实例」和「导入世界 / 世界改名」
//
// 这一节盯的是**真的动盘**那两件事：
// · `VersionManager::copyInstance()` —— 实例设置的概览里那一颗「复制实例」。
// · `WorldFiles::copyWorldInto()` / `renameWorld()` —— 存档那一页顶上那颗「导入世界」
// 和行尾那支笔里的「重命名…」。
//
// 为什么在这一层测而不是在 mw_check 里：那两颗按钮的前一站是**原生选择框**
// （QFileDialog），验证工程驱动不了它。所以真正动手的那一半被切成了"收几个字符串"的纯
// 函数 —— 和 deleteInstance() / exportLaunchScript() 那两处同一个切法。
//
// NOTE: 这一节自己开两个临时游戏文件夹（源一个、目标一个），跑完把数据根目录换回去。
// 不往共享沙盒里写东西：这一节要造的是一整份实例和一个世界，留在共享沙盒里会
// 给后面（以及下一次重跑）的节留下没预料到的包袱。
// =========================================================================

/// 造一个最小可用的实例目录（jar + json + meta + 一点嵌套内容）。
/// @param versionRoot `<某游戏文件夹>/versions`
/// @param id 目录名（= 实例 id）
/// @param metaBytes 写进 `wtfml.meta` 的字节；空串 = 不写这个文件
static QString makeInstanceDir(const QString &versionRoot, const QString &id,
                               const QByteArray &metaBytes = QByteArray()) {
    const QString dir = versionRoot + QLatin1Char('/') + id;
    writeFileAt(dir + QLatin1Char('/') + id + QStringLiteral(".jar"), "fake-jar-bytes");
    writeFileAt(dir + QLatin1Char('/') + id + QStringLiteral(".json"),
                R"({"id":"1.20.1","mainClass":"net.minecraft.client.main.Main"})");
    writeFileAt(dir + QStringLiteral("/mods/sodium.jar"), "mod-bytes");
    writeFileAt(dir + QStringLiteral("/saves/我的世界/level.dat"), "level-bytes");
    if (!metaBytes.isEmpty()) {
        writeFileAt(dir + QStringLiteral("/wtfml.meta"), metaBytes);
    }
    return dir;
}

/// 一个目录里某项还在不在（顺手把 `QFileInfo::exists` 的"文件或目录"语义写清楚）。
static bool existsAt(const QString &path) {
    return QFileInfo::exists(path);
}

/// 读盘上一份 `wtfml.meta`（路径自己拼，不走 AppPaths —— 目标在另一个游戏文件夹里）。
static QJsonObject readMetaAt(const QString &dir) {
    QFile file(dir + QStringLiteral("/wtfml.meta"));
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

void runTaskLedgerTests();

void runCopyInstanceAndWorldTests();

/// `--directory-lock-probe <目录> [--hold]`：另起一个**真进程**去抢数据目录锁。
///
/// 放在共用夹具里的原因：ZJ0 那条用例拿 `QCoreApplication::applicationFilePath()` 把**自己**
/// 再起一份，所以每个装了这套断言的入口都得认这个参数。原来只有 `verify_instances` 的 main 认，
/// `verify`（兼容入口）跑整套时子进程会掉进"直接跑全部断言"，父进程读不到 `LOCKED`，
/// 两条锁断言假红 —— 红的是夹具，不是锁。
///
/// 返回值：不是探针模式返回 -1（调用方接着跑自己的断言）；是探针模式返回进程退出码
/// （抢不到锁 73，其它 0），调用方直接 `return` 它。
int runDirectoryLockProbeIfRequested(const QStringList &arguments);

// ============================================================================
// ZM. Java 8 的 ASCII 别名（"路径里有非 ASCII 字符就起不来"那个坑）
//
// 真机那趟里 `20w14∞` 和中文名的 1.8 整合包都只留下一句
// `找不到或无法加载主类`：Java 8 打不开带非 ASCII 字符的路径（这台机器开着 Windows 的
// "Beta: 使用 Unicode UTF-8"，Java 8 认定的编码却是 GBK）。产品侧的绕法是另建一套纯 ASCII
// 的路径（联接 + 硬链接），判据/建法都在 src/launcher/AsciiPathAlias.hpp 里。
//
// 这一节要**真建**：纯函数那半只证明"我按我写的规则挑了路径"，而"联接真的通向实例目录"
// 这件事只有写一个文件过去、再从另一边看见它才算数。
// ============================================================================
void runAsciiAliasTests();

// ============================================================================
// ZNat. natives 解压的"整套解完了"标记
//
// 旧判据是"目录里有没有 .dll"。可 natives 是**分组**解压的（每组一次
// `ZipUtil::extractArchives`），第一组成功、第二组失败（盘满 / 被杀软锁住 / 用户中途关掉
// 启动器）时目录里已经躺着第一组的 DLL：这一趟如实报错，**下一趟**却被当成"解过了"直接
// 复用这份残缺目录 —— 症状从"解压失败"变成"进游戏崩在 UnsatisfiedLinkError"，而且每次
// 启动都复用同一份坏东西，永远修不好。
//
// 标记只在全部解完、DLL 确实在了之后才写；解压失败时整个目录删掉。
// 这一节全在临时游戏文件夹里跑（`AppPaths::setMinecraftDir()` 一处切换）。
// ============================================================================
void runNativesMarkerTests();

// ============================================================================
// ZN. 体检（VersionIntegrity）：只报不装的那套判据
//
// 待办里那条「独立的完整性校验入口」的判据层。**装一遍**那条路早就在了
// （VersionManager::repairVersionFiles，缺什么补什么），缺的是不装新的、只体检：
// 跑一趟，然后告诉用户"你这 3120 个文件里有 2 个要修"。
//
// 这一节全在一个**临时游戏文件夹**里跑（`AppPaths::setMinecraftDir()` 一处切换，跑完
// 换回去）：要造 jar / 库 / 资源索引 / 资源对象四档文件，留在共享沙箱里会给后面的节
// 留下没预料到的包袱。
//
// 这里亲手钉的是**判据**那一半（plan 出哪些条、check 怎么判、Report 怎么数）。清单和
// 实例对得上对不上（"体检说的文件就是这个实例装配时会去下的那些"）在 mw_check 的
// T19.15 —— 那边立着真 MainWindow、真 VersionManager。
// ============================================================================
void runVersionIntegrityTests();

// ============================================================================
// ZP. 游戏异常退出那几句话
//
// 这一节盯的是**规则本身**：`src/diag/GameExitDiagnosis` 是个纯函数（值进、字符串出，
// 不看时钟、不读盘、不开进程），所以那张表能在这里逐格念一遍。
//
// 为什么非要单独验它：那条规矩是「仅做规则明确的提示…不做不透明的『AI 猜错』」。
// "规则明确"这件事唯一能证明的方式，就是**每一条提示都能指着某个字段说"因为它这样"**
// —— 而这条链路在真机上要真起一次游戏、真崩一次才走得到，在这里喂一组值就走完了。
//
// 判据里故意混了**几组"什么都不该说"的**（只拿到一个 Java 主版本、日志没收到、一条错误
// 都没有）：那种情况下多说一句就是猜。
// ============================================================================
void runGameExitDiagnosisTests();

/**
 * @brief 世界与实例备份：清单 / 打包 / 还原。
 *
 * 这一节**真打 zip**（走产品那条 PowerShell / .NET 的路）、**真删目录**、**真还原**，
 * 所以它在一个 `QTemporaryDir` 里换一次 `AppPaths::setMinecraftDir()`（备份落点就是
 * `<数据目录>/cache/backups`），跑完换回来 —— 不碰共享沙盒。
 *
 * 盯的是四件事，每一件都是"错了会静默丢东西"的那种：
 * ① 清单里的名字 / 大小 / sha1 是**数出来的**，不是别人说多少就是多少；
 * ② 排除只按**顶层目录名**（别处的同名目录照收）；
 * ③ 还原之前**先**给"现在这一份"做安全快照，**做不成就不许动**；
 * ④ 解出来了还要**逐文件对账**（"在"和"对"是两件事）。
 */
void runBackupTests();

// ---------------------------------------------------------------------------
// ZR. 「上一次启动」那本账
//
// 这一节里最要紧的两条，都不是"能不能跑"，而是**这句规矩有没有被写歪**：
// · 「上一次」以成功创建游戏进程为准 ⇒ `latestPlayable()` 跳过"没起来"那一档；
// · 「昨天」按**日期**算，不按差了多少秒（23:59 → 次日 00:01 只差两分钟，可那是昨天）。
// ---------------------------------------------------------------------------
void runLaunchHistoryTests();

// ---------------------------------------------------------------------------
// PA. 实例预设：值那一层 + 盘那一层
//
// 这一层的要求原话是「PresetStore CRUD、排序与唯一默认项；schema 校验、损坏项隔离和原子写入；
// 新建、复制、重命名、删除和从实例提取；管理页；ID 使用 UUID，显示名不作为磁盘路径」，两条
// 验收是「损坏单个预设不阻止其他预设加载」和「非法名称无法逃逸目录」。
//
// 这一节收的是**除"从实例提取"和"管理页"之外**的全部：那两样一个要适配器层（P2/P3）、一个
// 要界面（P1 的后半），都不是这一节念得出来的。
//
// 这一节里最要紧的三条，都不是"能不能跑"，而是**这三句规矩有没有被写歪**：
// · id 那道闸的判据是"必须是一个规范的 UUID"，不是"里面没有斜杠" —— 后一种黑名单挡不住
// `..`、挡不住 `C:`，也挡不住 `NUL`（Windows 上那个"写下去就不见了"的设备名）；
// · 一份读不回来**只进 broken、不进列表**，别的几份照旧列得出来；
// · 下标读不回来时那一趟**不写盘**（写回去等于把用户排好的顺序抹平，而屏幕上什么都不说）。
//
// 整节纯函数 + 一个 QTemporaryDir 里的真存真读：不联网、不碰共享沙箱、不碰 ui/。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 模组预设的兼容检查，见下面 PH 节。
//
// 这一节**一条网络都不碰**：`CompatibilityProvider` 就是为这件事留的口子，四个替身
// 分别代表"立刻答"、"慢答"、"等我们手动放行"、"永远不答"。要验的也正是那几句规矩：
// · 本地能判的（钉死的版本/加载器、坏引用）不问网络；
// · 同一个项目只问一遍，必要项取"或"；
// · 在途请求数封顶；
// · 旧 generation 的迟到回调一个字都不许改（用户换了另一份预设）；
// · 网络失败 / 超时 / 429 只算「暂无法确认」，**绝不计入缺失**；
// · 缓存只收 `Matched` / `Missing`，过期的要重问。
// ---------------------------------------------------------------------------
namespace {

struct ImmediateProvider : Presets::CompatibilityProvider {
    QList<QString> asked;
    QHash<QString, Presets::CompatibilityState> answers;
    void query(const Presets::ModReference &reference, const Presets::CompatibilityContext &,
               Reply reply) override {
        asked.append(reference.key());
        reply(answers.value(reference.key(), Presets::CompatibilityState::Matched),
              QStringLiteral("替身给的结论"));
    }
};

struct SlowProvider : Presets::CompatibilityProvider {
    int delayMs = 25;
    int inFlight = 0;
    int maxSeen = 0;
    int askedCount = 0;
    void query(const Presets::ModReference &, const Presets::CompatibilityContext &,
               Reply reply) override {
        ++inFlight;
        ++askedCount;
        maxSeen = qMax(maxSeen, inFlight);
        QTimer::singleShot(delayMs, [this, reply]() {
            --inFlight;
            reply(Presets::CompatibilityState::Matched, QString());
        });
    }
};

/// 把回调收着，等测试自己决定什么时候、按什么顺序放行（验 generation 用）。
struct ControlledProvider : Presets::CompatibilityProvider {
    QList<Reply> pending;
    void query(const Presets::ModReference &, const Presets::CompatibilityContext &,
               Reply reply) override {
        pending.append(reply);
    }
};

/// 永远不答（验超时那一档）。
struct SilentProvider : Presets::CompatibilityProvider {
    void query(const Presets::ModReference &, const Presets::CompatibilityContext &,
               Reply) override {}
};

/// 等到这一代收尾；返回"真的收尾了"（false = 超时了，验证工程会红）。
bool waitForChecker(Presets::CompatibilityChecker &checker, int timeoutMs = 5000) {
    if (!checker.running()) {
        return true;
    }
    QEventLoop loop;
    QObject::connect(&checker, &Presets::CompatibilityChecker::finished, &loop,
                     &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return !checker.running();
}

Presets::ModReference makeRef(const QString &platform, const QString &projectId,
                              bool required = true, const QString &name = QString(),
                              const QString &gameVersion = QString(),
                              const QString &loader = QString()) {
    Presets::ModReference reference;
    reference.platform = platform;
    reference.projectId = projectId;
    reference.required = required;
    reference.name = name;
    reference.gameVersion = gameVersion;
    reference.loader = loader;
    return reference;
}

}  // namespace

void runPresetCompatibilityTests();

// 「大量预设、限流、切版本/关闭弹窗/取消与回归」这几条的要求原话是
// 「无过期回调、失控并发或主线程假死」—— 这一节里最值钱的三条：
// · 一次性几百条引用，检查器的在途数**绝不越过上限**，而且真的收得了尾；
// · 连着换几十代（用户来回切版本），旧一代的迟到回调**一个字都不许改**新结果；
// · 检查器比提供方先没（关闭那一页）时，晚到的 `Reply` 不许碰已析构对象。
// 另外两格是数量级的粗检（几百份预设 / 几百条引用），只钉"能跑完、算得对"，不钉毫秒。
// ---------------------------------------------------------------------------
void runPresetStressTests();

void runPresetStoreTests();

// ---------------------------------------------------------------------------
// PB. 原版设置那一段：`options.txt` 的保序模型
//
// 这一层的要求原话：「建立保序的 `options.txt` 模型；保留未知行和目标版本新增字段；基础设置与
// `key_` 按键分组选择；默认 Merge，Replace 必须明确确认；检测标准按键的精确重复；显示器、
// 全屏分辨率等硬件相关项默认不选」，外加那两句「不要简单用 `QMap` 重写整个文件，也不要把
// 缺失字段当空值写回」。
//
// 这一节最要紧的三格，全是"我们**没**弄坏用户东西"的那种红：
// · **原地往返**：读进来再写出去逐字节相同（别人的行、行尾、非 UTF-8 的字节都不许动）；
// · **目标版本新增的键一个都不许丢** —— 拿一张 map 重写整个文件，坏就坏在这儿；
// · **认不出来的行任何一档都不删**，连"覆盖"都不删。
//
// 整节纯函数：两份假 `options.txt` 一喂，不该动盘、不该联网、不碰 ui/。
// ---------------------------------------------------------------------------
void runVanillaOptionsTests();

// ---------------------------------------------------------------------------
// PC. 服务器列表那一段：`servers.dat` 的计划与落盘
//
// 这一层的要求原话：「`servers.dat` 支持 Append / Replace / Skip；按规范化地址去重并保留顺序」。
//
// 这一节和 PB 那一节最大的不同：这一层**真的动盘**（PB 是字节进字节出）。所以夹具是
// 真的 `servers.dat`（用 ZK 那一节那套 NBT 组字节的帮手造出来的），跑完再读回来逐格看。
// 纯判据那几条（归一化、计划的数）不碰盘。
//
// 这一节里最值钱的几条：
// · **去重按规范化地址**（`MC.Example.com` 和 `mc.example.com:25565` 是同一台），但
// 报出来的"没写的那几条"一条都不许少 —— 悄悄丢掉是这一档最坏的错法；
// · **已有的顺序一格都不许重排**（新条目一律接在表尾）；
// · **「覆盖」是就地改名不是清空重建** —— 盘上那些 `icon` / `hidden` 一个都不许丢；
// · **没确认的「覆盖」一个字节都不写**；
// · **认不出来的元素任何一档都不动**（连「覆盖」都不动）；
// · **一条都不用动时连写都不写**（否则用户实例里会多出一个变了 mtime 的文件）。
// ---------------------------------------------------------------------------
void runServerListPresetTests();

// ---------------------------------------------------------------------------
// PD. 通用文件组那一段：`config/` / `defaultconfigs/` / 资源包 / 光影
//
// 这一层的要求原话：「GenericFileAdapter 支持 `config/`、`defaultconfigs/`、资源包和光影包；
// 每项支持 Replace / CopyIfMissing / Skip；路径穿越、绝对路径和符号链接逃逸检查；未知配置
// 明确标记为"只能复制/覆盖"」。
//
// 这一节和 PC 那一节一样**真的动盘**（两个临时目录之间真拷真读），而它最值钱的几条是
// "我们**没**弄坏用户东西"和"我们**没**跑出这一组去"：
// · **`Replace` 不删目标上多出来的文件** —— 那是不可恢复的，而"覆盖"这两个字在这儿的
// 意思只有一个：同名的那份换成我的；
// · **`CopyIfMissing` 逐文件判**（一项是一棵子树时，用户已经调过的那几份一个都不动）；
// · **`Merge` / `Append` 下什么都不写**（对一坨字节谈不上合并）—— 条数报出来给用户选，
// 而不是悄悄按复制做；
// · **三道路径闸**：绝对路径 / `..` / 空段 / 非法字符 / 保留设备名，以及**源侧和目标侧的
// junction**（那两条只有真造一根链接才验得出来）。
// ---------------------------------------------------------------------------
void runGenericFilesTests();

// ---------------------------------------------------------------------------
// PE. 把一份预设落到一个实例上这件事的**计划**
//
// 这一层只回答一件事：**"你按下应用之后会发生什么"**。下面那张表就是它的全部输出：
//
// 游戏设置 合并 18 项
// 原版按键 合并 23 项，冲突 1 项
// 服务器列表 追加 3 个
// 配置文件 复制 8 个，覆盖 1 个
//
// 所以这一节的重量在两件事上：
// · **表上那几个数是从适配器那边真算出来的**（走 `sectionsFrom*()` 造真段 —— 手搓一个
// `QJsonArray` 去喂它，验的就只是"加法对不对"，那件事没有价值）；
// · **几道闸真的会拦**：还没确认的「覆盖」、游戏在跑、不认识的种类、段和它的身份对不上。
//
// 除了通用文件组那一段的干跑（要真数一遍目标上缺哪几份），这一节全是纯值 —— 这也正是
// 这一层该有的样子：拿任意一份预设对任意一个实例**算**，算完不满意就丢掉，盘上什么都没变。
// ---------------------------------------------------------------------------
void runPresetPlannerTests();


// ---------------------------------------------------------------------------
// PF. MaLiLib 那一族的热键
//
// 这一层的要求原话：「首批识别 malilib.json、litematica.json、tweakeroo.json、minihud.json、
// itemscroller.json，并通过结构识别 TweakerMore、Masa Gadget、OMMC 等扩展；默认仅合并热键；
// 可选合并全部已知设置、完整覆盖、缺失时复制；热键连同触发、上下文、额外键、顺序、独占和取消
// 行为保存；身份使用 modId/groupId/optionName，不使用翻译名和行号；目标字段缺失时跳过并报告，
// 类型变化时拒绝转换；保存来源 Minecraft、加载器、模组和 MaLiLib 版本」。
//
// 这一节的重量在三件事上：
//
// · **认这一族靠的是结构、不是文件名**：实测那 236 份 `config/*.json` 里，模组名的表永远列
// 不全（TweakerMore / Masa Gadget / OMMC 这些扩展的名字一个都没写进代码），可它们的结构
// 一眼就是这一族的 —— 所以判据落在**内容**上（`malilibInspect()`）；
// · **合并不许退化成覆盖**（最要紧的那一格）：目标上那些**没被这一份点名**的热键一个都不许
// 动。这一条要是错了，用户应用一次预设就把自己几百个键位清空了；
// · **拒绝都在改动之前**：类型对不上就「不转」，不是「先写一半再说」。
//
// 夹具是**真格式的缩小版**（`{"keys": "M,A"}` 这种逗号分隔的 GLFW 词、热键那一大格和值那一
// 大格分开、顶层那个 `config_version` 标量），不是我们想象出来的形状。
// ---------------------------------------------------------------------------
void runMalilibKeysTests();


// ---------------------------------------------------------------------------
// PG. 提取与落到实例上：收一份 → 落回另一个实例 → 备份 / 两趟确认 / 游戏在跑
// ---------------------------------------------------------------------------
//
// 这一节和 PE 那一节的分界：PE 只算"会怎么样"（一个字节都不写）；这一节**真的在两个临时
// 实例目录之间收和落**。所以夹具是两份形状正确的实例目录（`options.txt` + `config/`），
// 而最值钱的四条都是"盘上到底变了没有"：
// · 合并落下去以后，目标上预设没点名的键**还在**；
// · 覆盖那一趟**先**回 `needsConfirm`、盘上一个字节都没动，第二次才真写；
// · 游戏在跑 ⇒ 整趟不写（拿字节比，不是看那一句说明）；
// · 改写过的那一份**在备份目录里有一份原样的**。
void runPresetCaptureApplyTests();

void runResourceAndDataPackTests();

// 模组平台身份、依赖/冲突、必要可选失败与下载校验 —— 这几条的要求原话是
// 「不错装不兼容文件；替代项目需确认；落盘校验」—— 这一节的三条红线：
// · 本地先筛：版本自己的 `gameVersions` / `loaders` 对不上就不许下（"接口筛过"不算数）；
// · 替代项目**绝不自己挑**：没有用户确认就只报「等你确认」，确认了也要它自己过兼容关；
// · 下完的文件按大小 + sha1 对一遍；接口没给 sha1 时只能说"没校验"，不许说"校验通过"。
// 这一层不发网络、不读实例盘：版本答复、实例里装了什么、确认过哪一条都是手搭的假值。
// ---------------------------------------------------------------------------
void runModInstallPlanTests();

// 「首装一开始就把目标实例的配置目录整棵拍个快照」那一笔的代价（只量，形状才断言）。
void runPresetSnapshotScaleTests(const QString &sandbox);

// 这一层的要求原话：「搜索实例名、版本号、加载器；筛选原版/模组实例、可更新、最近运行；搜索不
// 改变分组归属；清空搜索恢复原展开状态」。四句里只有**前两句**是这一节的事（纯判据：
// 一条实例算不算匹配）；后两句说的是"重灌树时别动别的状态"，那是接线，在 mw_check 的 T37。
//
// 这一节最要紧的不是"能不能筛出来"，而是**会不会筛错**：
// · 空搜索词 + 四颗都不勾 ⇒ 一条都不许滤掉（`isActive()` 为假是那条路的唯一判据）；
// · 一个空串混进可比对的串里 ⇒ `contains("")` 恒真 ⇒ 搜什么都搜得到（最难发现的一种坏）；
// · 词与词是**与**、词与串是**或**（用户打两个词是"都要有"，不是"任一"）。
// ---------------------------------------------------------------------------
void runInstanceFilterTests();

// ---------------------------------------------------------------------------
// ZT. 更新中心的「忽略这一版 / 固定这一版」：一本账 + 两条判据
//
// 这一节盯的是"**什么时候它自己就回来了**"这件事 —— 那两档"别再提示我"最怕的不是记不住，
// 是**记死了**（用户按一下就把这个资源永远钉在原地，作者再发新版本也不提示了）：
// · 忽略记的是**要换上的那一版的 id** ⇒ 下一趟要换的是另一版时，它自己回来；
// · 固定记的是**盘上那一份的 sha1** ⇒ 文件被换过（手动 / 又更新了一次）就自动失效；
// · 两个"不知道"（拿不到当前 sha1 / 拼不出目标版本）**一律按"没记过"算** —— 不知道的
// 事不能当成知道（不知道当成知道，症状是"更新不提示了，而且用户不知道为什么"）。
//
// 整节纯函数 + 一个 QTemporaryDir 里的真存真读：不联网、不碰共享沙箱、不碰 ui/。
// ---------------------------------------------------------------------------
void runUpdatePolicyTests();

// ---------------------------------------------------------------------------
// ZU. 更新前那份轻量回滚点：抄一份 / 退回去 / 撤干净
//
// 这一节要**真动盘**（在一个临时游戏文件夹里造实例目录），因为这一层整个存在的意义就是
// "动手之前手上有一份旧的"：抄得对不对、退回来的**是内容是字节**、更新新添的那个文件撤回
// 时**要消失** —— 这三件事在纯值上一条都验不出来。
//
// 最要紧的几条（错了会静默丢数据）：
// · 清单里"当时有没有这个文件"要记准（记错了 = 撤销时把一个**本来就在**的文件删掉）；
// · 只留**最新那一份**（留多代 = 每更新一次攒几十 MB，而用户永远只用最近那一次）；
// · 一条失败**不影响其余**（"3 个没退回去"比"失败了，一个都没动"诚实）。
// ---------------------------------------------------------------------------
void runUpdateRollbackTests();

// ---------------------------------------------------------------------------
// ZW. 服务端判据层：状态 / EULA / server.properties / 我们记的那一份
//
// 这一节全是**纯函数**（值进、结构出）：一个服务端目录都不必真的存在、一个进程都不必起。
// 盯的是几处"错了不会报错、只会静默丢东西"的地方：
// · `eula.txt` 到底算不算"接受了"（文件头那段声明里就有 `eula` 字样，判成"含 true 就算"会误判）；
// · 我们写下去的那份 `eula.txt` 里那句声明**必须原样留着**（它是"用户接受了"的文本凭证）；
// · `server.properties` 解了再写出去要**原地往返**（注释、键序、CRLF 一个字节都不许动 ——
// 用户在表单里改了端口，别的东西不该跟着变）；
// · 从文件反推"这是哪一档"、以及"装好了没有"的判据（Spigot 那一档产物名拼不出来只能扫）。
// ---------------------------------------------------------------------------
void runServerModelTests();

// ---------------------------------------------------------------------------
// ZX. 服务端工作区：`<数据目录>/Servers` 底下谁在住
//
// 这一节**真动盘**：在一个 QTemporaryDir（当成临时数据根目录）里真建目录、真写文件、真删。
// 盯的是这一层唯一一件本事 —— "把盘上那个目录读成一行事实"：
// · 自己记的那一份（`.wtfml-server`）优先，没有才从文件反推（反推的要标成"猜的"）；
// · "装好了没有"是照盘上的文件认的（缺哪一样要说得出来）；
// · `removeServer()` 的三道闸（空 id / 路径分隔符 / 不许删到根外面）。
// ---------------------------------------------------------------------------
void runServerRegistryTests();

// ---------------------------------------------------------------------------
// ZY. 服务端安装那四段的**判据层**
//
// `ServerInstallRun.hpp` 那个头整个是纯函数：喂一份 `Plan` + 一份字节进去，回一个结论出来。
// 所以这一节**不联网、不建目录、不起 java** —— 那三件事是驱动层（`ServerInstallController`）
// 干的，归 mw_check。这一节盯的就是"取哪一份元数据 / 取回来怎么读 / 分成几步"这几件纯事。
void runServerInstallRunTests();

// ---------------------------------------------------------------------------
// ZZ. 服务端「跑起来 / 停下来」那一层的**判据层**
//
// `ServerRun.hpp` 和 `ServerInstallRun.hpp` 同一个形状：纯头、全 inline、不联网、不起进程、
// 不看时钟 —— 喂几个数进来、回一个结论出去。所以这一节能把它整片钉死。真的 `QProcess` /
// `QTimer`（`ServerProcess`）在 mw_check 的 T41，那儿拿本工程自己的 exe 当"服务端"跑整趟。
//
// 这一层里最要紧的一条是 **内存那几项插在第几位**（`Plan::launchJvmArgsIndex`）：
// `-Xmx` 这类参数必须插在 `@user_jvm_args.txt` / `win_args.txt` **之前** —— 那张
// win_args.txt **自带主类**，`-Xmx` 落在它后面会被 java 当成给服务端的**程序参数**，
// 一个字都不生效、也不报错（界面上那根内存滑杆成了摆设）。ZZ0 里那几条钉的是这个顺序。
// ---------------------------------------------------------------------------
void runServerRunTests();

// ---------------------------------------------------------------------------
// ZV. 「服务端设置」那两层判据
//
// 这一页上有四样东西，其中三样背后各有一层**纯判据**：
// · `ServerPropertiesForm`（哪几项、值域多少、收不收、合回文件）—— 见 ZV0 ~ ZV4；
// · `ServerCheck`（体检报哪几条、按什么次序、每一条的出路是什么记号）—— 见 ZV5 ~ ZV6；
// · `ServerPortCheck`（"这个端口有人占着吗"）—— 见 ZV7（**这一条要真占一次**，不联网）。
//
// 这一节里最要紧的几条，都是"错了不会报错、只会静默做错事"的地方：
// · 表里写的 `fallback` **自己必须过得了 `accepts()`** —— 缺省值都收不住的表，
// 用户点一次保存就会把 `server.properties` 写坏（而且他不会知道）；
// · `applyEdits()` **只碰点名的键**（注释、键序、CRLF、我们不认识的键一个字节都不动）；
// · 体检的**次序**（拦路的在最上面）和**在跑时不报端口**那两条。
// ---------------------------------------------------------------------------
void runServerSettingsTests();

// ---------------------------------------------------------------------------
// ZAE. 服务端那四份玩家名单
//
// 白名单 / 管理员 / 封禁的玩家 / 封禁的 IP —— 四份文件都由服务端自己写，格式是 Gson 的漂亮
// JSON。这一节钉的是判据层 `ServerPlayers`（全纯函数，不碰盘）：
//
// · **读要宽容**：文件不在 = 空名单；读不懂 = `ok = false` + 一句毛病。这一条最要紧 ——
// 拿空名单覆盖一份用户手改坏的 JSON，等于把他里面的记录全抹了；
// · **写按游戏那一份的字段形状**：多一个自定义键，服务端下次启动就整份读不懂、丢掉；
// · 离线 UUID 是确定的（`OfflinePlayer:<名字>` 的 MD5 + RFC 4122 那两个位），
// 「按名字加一条」不登录 Mojang 也算得对。
// ---------------------------------------------------------------------------
void runServerPlayersTests();

// ---------------------------------------------------------------------------
// ZAF. 服务端异常退出后要不要自动拉起
//
// 这一节钉的是 `ServerRestart` 那几条纯判据。最要紧的三条：
//
// · **用户自己停的 / 正常退出**一律不拉 —— 点了「停止」五秒后又起来是最吓人的；
// · **连着崩几次就收手**，而且次数是「连着」的（跑够久再崩算新的一轮）；
// · 两次之间**退避**（5、10、20、40、60 封顶）：给用户留出看控制台的时间。
//
// 真接线在 `ServerRunController::onExited()`（排那一发 `singleShot`），策略那两格在
// `.wtfml-server` 里 —— 下面顺带钉一遍往返：记不住的设置等于没有。
// ---------------------------------------------------------------------------
void runServerRestartTests();

// ---------------------------------------------------------------------------
// ZAG. 服务端资源包：这份本地 zip 能不能用 / SHA-1 对不对
//
// 要求那一句是：「服务端资源包选择本地 zip、计算 SHA-1，并明确 URL 托管仍需用户提供，
// 启动器不假装本地路径能被其他玩家访问」。这一节钉**判据那一半**：
//
// · 游戏只认**最外层**那一份 `pack.mcmeta` —— "整个文件夹一起压"是最常见的错；
// · 只有 `data/` 没有 `assets/` 的那份是**数据包**，放错地方了，说法要跟着换；
// · SHA-1 得是这份 zip 的**真摘要**（小写 40 位）—— 填错一格，客户端每个人进服都
// 会卡在一个下不下来的资源包上。
//
// 真 zip 由这一节自己用 `ZipUtil::createZip()` 打（和导出整合包同一条写侧），不碰安装。
// ---------------------------------------------------------------------------
void runServerResourcePackTests();

// ---------------------------------------------------------------------------
// ZAH. 服务端内容：模组 / 数据包的名字与分类
//
// 内容那一页只认两种文件（`mods/*.jar`、`<世界>/datapacks/*.zip`），禁用 = 加 `.disabled`
// 尾巴。这一层不碰盘，所以这里钉的全是"名字上的规矩"：
//
// · **认名先摘尾巴**：`X.jar.disabled` 是禁用的模组，`X.jar.bak` 不是模组（尾巴摘不掉就
// 落选）—— 少了这一步，禁用过的文件会在那一页上"消失"；
// · **改名只动尾巴**：内容、大小、mtime 一个字节都不变（用户随时能改回来）；
// · **认不出来的回空串**，不猜（调用方拿空串说明"这一行动不了"）。
// ---------------------------------------------------------------------------
void runServerContentTests();

// ---------------------------------------------------------------------------
// ZAI. 工具箱那两件：信标计算 / 酿造助手
//
// 两件都是纯算术 + 一张表，所以这里直接钉数字和路线：
//
// · **信标**：一层 `(2n+1)^2` 块、累计 9 / 34 / 83 / 164；范围 `10n+10`；效果按级解锁。
// 这些数错一格，界面上给出的"还要多少块"就会骗人（用户真的会照它去挖矿）。
// · **酿造**：链式前驱（隐身 ← 夜视 ← 粗制的药水）、水瓶直出（虚弱 / 缓降）、
// 红石与荧石二选一、以及"滞留必须先从喷溅来"那一步；材料从水瓶起算，
// 结果写产物瓶数、等级与时长（瞬间效果只写说明）。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 删一棵目录（`FolderManager` 里那两个自由函数）。见 ZAN 节。
//
// 这一节钉的是两件**只有在真盘上才看得见**的事：
// ① Windows 上 Qt 6.10 的 `QFile::remove()` 就是一句 `DeleteFile()`，**不会**替你摘
// `FILE_ATTRIBUTE_READONLY` —— 只读文件于是"删不掉"。用户从只读介质拷来的整合包、被
// 网盘/别的启动器标成只读的 jar，删实例、删文件时会卡在这一步，而界面那句提示写的是
// "多半是它正被游戏占着"：关掉游戏再删一百次也没用。
// ② 删这一棵**绝不许顺着链接走过去**：`QDir::removeRecursively()` 跟进去把链接那头的东西
// 真删掉（见 FolderManager.cpp 文件头那段实测记录）。
// ---------------------------------------------------------------------------
void runFolderRemovalTests();

void runToolboxExtraTests();

// ---------------------------------------------------------------------------
// ZA. 微软登录的回环端口、PKCE 和 state。
// ---------------------------------------------------------------------------
void runOAuthEndpointTests();

// ---------------------------------------------------------------------------
// ZO. 「同时最多下几个」那几档（并发数预设）
//
// 24 是"出厂默认那一档"，用户能在设置 → 下载 → 并发数里挑 6/12/24/48。判据层是
// `DownloadConcurrency`（唯一出处：四档、
// 上下限、越界的值怎么办）；这里钉的就是它 + 那条线在 DownloadManager 上真的被用上了。
//
// 这一节最要紧的两条：
// · **队列泵问的是这个对象当下那一档**（不是那个常量）。漏了这一条的形状是"设置里改成 6
// 了、界面上也显示 6、下载却还是 24 条一起飞" —— 不报错、也看不出是哪儿没接上。
// · 越界的值回**默认**而不是夹到边上：夹到边上会让"被限流 / 下不动"看起来像用户自己选的。
// ---------------------------------------------------------------------------
void runDownloadConcurrencyTests();

// ---------------------------------------------------------------------------
// ZP. 「下载速度上限」那几档（下载限速）
//
// 判据层是 `DownloadSpeedLimit`（唯一出处：几档、上下限、归一化、令牌桶容量）；这里钉的
// 就是它 + 那条线在 DownloadManager 上真的被用上了。
//
// 和 ZO 那条最大的不同：**0 是合法值**（= 不限，也是出厂值）。所以"越界回默认"在这里
// 等于"越界回不限" —— 写成夹到下限（32 KiB/s）就把"关掉限速"变成了"最慢那一档"，
// 用户改回「不限」时下载反而更慢，那种错误没有任何报错。
// ---------------------------------------------------------------------------
void runDownloadSpeedLimitTests();

// ===========================================================================
// ZQ. 代理：三档 + 手动地址解析
//
// 这一节只碰纯判据（`DownloadProxy`）和 `LauncherSettings` 的存取；"装到进程上"那一下
// （`QNetworkProxy::setApplicationProxy`）在 mw_check 的冷启动那一节里核。
// ===========================================================================
void runDownloadProxyTests();

// ===========================================================================
// 工具箱那四层（用户点名的那一批小工具）
//
// 这一批的共同形状：**纯函数**（除了哈希要读一个真文件 —— 它在临时目录里读）。
// 界面那一侧（`ui/ToolboxPage.cpp` + `src/controllers/ToolboxController.cpp`）一条断言都不在
// 这一节里，这一节只管"算得对不对"。
// ===========================================================================

/// ZAA 节。`§` 码 / JSON 组件 / `/tellraw` 三份形态之间的往返。
void runTextFormatTests();

/// ZAB 节。8:1 换算、区块 / 区域文件坐标、以及 F3+C 复制出来的那条命令。
void runCoordConvertTests();

/// ZAC 节。三档摘要、已知值的归一化、结论，以及真读一个文件（带进度与取消）。
void runFileHashTests();

/// ZAD 节那两个穷举用的单位（`plan()` 那套 DP 的独立对照）。
struct ToolboxBruteUnit {
    QMap<QString, int> enchantments;
    int penalty = 0;
    bool isBook = true;
};

/**
 * @brief 把所有合并次序展开数一遍，回"合完这件事的附魔 → 最小总代价"。
 *
 * 这是 ZAD 那条"计划确确实实最省"的**独立算法**（写法故意和 `AnvilPlanner` 里那套子集 DP
 * 不一样）：两边算出来的数一样，才说明 DP 的剪枝没把最优解剪掉。
 * NOTE: 中间件是什么跟着左边（`targetItem.isBook`）—— 铁砧的结果就是把左边那件改一改。
 * 只收 `wantBook` 那一档的收尾（全是书 ⇒ 合出来是书；有装备 ⇒ 合出来必须是装备）。
 */
static QString toolboxEnchantKey(const QMap<QString, int> &map) {
    QStringList parts;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        parts.append(it.key() + QLatin1Char('=') + QString::number(it.value()));
    }
    return parts.join(QLatin1Char(','));
}

/// 记忆化用的键：**多重集**的规范写法（顺序无关、每件的附魔/前科/是不是书都要在键里）。
static QString toolboxBruteStateKey(const QList<ToolboxBruteUnit> &units) {
    QStringList parts;
    parts.reserve(units.size());
    for (const ToolboxBruteUnit &unit : units) {
        parts.append(QStringLiteral("%1|%2|%3")
                         .arg(toolboxEnchantKey(unit.enchantments))
                         .arg(unit.penalty)
                         .arg(unit.isBook ? 1 : 0));
    }
    parts.sort();
    return parts.join(QLatin1Char(';'));
}

/**
 * @brief 从 @p units 这套材料出发，**把所有合并次序展开**后能到达的「结果附魔 → 最小总代价」。
 *
 * 和 `AnvilPlanner` 那套子集 DP 是**两个独立实现**（那边有支配剪枝和每层 12 条的额度，
 * 这边是老老实实枚举），两边对上才说明那边没剪掉最优解。
 *
 * NOTE: 这里必须记忆化。不记忆化时 6 件的一组就要几十秒（2026-11-04 实测：6 件单组
 * 超过 40 秒），7、8 件根本跑不动 —— 而"6 件以上到底准不准"正是要验的东西。
 * 键用**多重集**（`toolboxBruteStateKey()`）：同一套材料不论从哪条路径合到这儿，
 * 后面能到达的结果与最小代价都一样，所以缓存的是"从这里出发"的最小代价，而不是累计值。
 */
static QMap<QString, int> toolboxBruteForceMemo(const QList<ToolboxBruteUnit> &units, bool wantBook,
                                                QHash<QString, QMap<QString, int>> &memo) {
    if (units.isEmpty()) {
        return {};
    }
    const QString key = toolboxBruteStateKey(units);
    const auto cached = memo.constFind(key);
    if (cached != memo.constEnd()) {
        return cached.value();
    }
    QMap<QString, int> best;
    if (units.size() == 1) {
        if (units.first().isBook == wantBook) {
            best.insert(toolboxEnchantKey(units.first().enchantments), 0);
        }
        memo.insert(key, best);
        return best;
    }
    for (int left = 0; left < units.size(); ++left) {
        for (int right = left + 1; right < units.size(); ++right) {
            for (int swapped = 0; swapped < 2; ++swapped) {
                const ToolboxBruteUnit &targetItem = units.at(swapped == 0 ? left : right);
                const ToolboxBruteUnit &sacrificeItem = units.at(swapped == 0 ? right : left);
                Anvil::Unit target;
                target.enchantments = targetItem.enchantments;
                target.penalty = targetItem.penalty;
                target.isBook = targetItem.isBook;
                Anvil::Unit sacrifice;
                sacrifice.enchantments = sacrificeItem.enchantments;
                sacrifice.penalty = sacrificeItem.penalty;
                sacrifice.isBook = sacrificeItem.isBook;
                const Anvil::MergeCost merge = Anvil::mergeCost(target, sacrifice);
                QList<ToolboxBruteUnit> rest;
                rest.append(ToolboxBruteUnit{merge.result, merge.resultPenalty, targetItem.isBook});
                for (int index = 0; index < units.size(); ++index) {
                    if (index != left && index != right) {
                        rest.append(units.at(index));
                    }
                }
                const QMap<QString, int> deeper = toolboxBruteForceMemo(rest, wantBook, memo);
                for (auto it = deeper.constBegin(); it != deeper.constEnd(); ++it) {
                    const int total = merge.total + it.value();
                    auto slot = best.find(it.key());
                    if (slot == best.end() || total < slot.value()) {
                        best.insert(it.key(), total);
                    }
                }
            }
        }
    }
    memo.insert(key, best);
    return best;
}
static int toolboxBruteForceMinCostFor(const QList<ToolboxBruteUnit> &units,
                                       const QMap<QString, int> &target) {
    bool wantBook = true;
    for (const ToolboxBruteUnit &item : units) {
        if (!item.isBook) {
            wantBook = false;
            break;
        }
    }
    const QString wanted = toolboxEnchantKey(target);
    int best = -1;
    // 记忆化跨子集共用：同一个多重集在"材料可不用"的那些子集里会反复出现。
    QHash<QString, QMap<QString, int>> memo;
    for (int mask = 1; mask < (1 << units.size()); ++mask) {
        QList<ToolboxBruteUnit> subset;
        for (int index = 0; index < units.size(); ++index) {
            if ((mask & (1 << index)) != 0) {
                subset.append(units.at(index));
            }
        }
        const QMap<QString, int> results = toolboxBruteForceMemo(subset, wantBook, memo);
        const auto it = results.constFind(wanted);
        if (it != results.constEnd() && (best < 0 || it.value() < best)) {
            best = it.value();
        }
    }
    return best;
}

/// ZAD 节。铁砧那套账：前科、附魔价值、互斥、39 级上限，以及"计划确实最省"。
void runAnvilPlannerTests();

// ---------------------------------------------------------------------------
// 结构 / 原理图的元数据与方块统计
// ---------------------------------------------------------------------------
//
// 三种格式各造一份**真形状**的 NBT（用 `NbtTree::serialize()` 手搭，不依赖任何外部文件）：
// · 原版结构 `.nbt` —— `size` + `palette` + `blocks`；
// · Sponge `.schematic` —— `Width/Height/Length` + `Palette` + `BlockData`（varint，
// 其中一格故意写成两字节，验跨字节那个读法）；
// · Litematica `.litematic` —— 两个 region，一处 bits=2、一处 bits=5（后者的第 13 个值
// 从第 60 位开始，**故意跨 long**；测试自己那套打包是独立写的一份，不是抄实现）。
// 另外验：后缀骗人时按**字段**认、空气不算方块数、gzip 那一层（拿 `ZipUtil` 真压一份）、
// 数据版本那张表、以及三种"读不动"（认不出 / 空的 / 太大）。
void runSchematicInfoTests();

void runReplayInfoTests();

/// LF：日志文件（含 .log.gz）的读行、数行、末尾截取、gzip 头识别。
void runLogFileReaderTests();

void runStartupDiagnosisTests();

// 「这块盘还在不在」那一句判据：正常卷不许误报，盘没了必须说得出话。
void runStorageAvailabilityTests();

void runTransferPlannerTests();
void runOperationReportTests();

// ---------------------------------------------------------------------------
// 「从别的游戏文件夹里借文件」
// ---------------------------------------------------------------------------
// 要求：「下载新版本的时候（甚至在全新文件夹里下载新东西的时候）复用其它 mc
// 文件夹/版本的文件以减少等待时间」；随后补的那道闸：「文件夹的使用仅限用户添加了的和
// 官方启动器默认文件夹（如果存在），不要全盘扫描」。
//
// 这一节分两半：
// · LF1–LF5 是纯值层（`LocalFileReuse`）：路径怎么对、候选怎么排、来源算不算数、
// 硬链接 / 复制怎么落 —— 一个 QTemporaryDir 里造两棵树就能逐格念，不联网。
// · LF6 之后是 `DownloadManager` 那一层：证明"借到了就真的不发网络请求"，以及
// 点名 / AlwaysDownload 那两条路**不许**借。
void runLocalFileReuseTests();

// ---------------------------------------------------------------------------
// 恢复记录在**大批量**下的两笔账（`DownloadCheckpoint::save()` / `load()` 各一趟）。
// ---------------------------------------------------------------------------
// `save()` 是同步跑在 GUI 线程上的（`pumpDownloadQueue()` 那一段就是它），而记录上限是
// 50,000 条 / 32 MiB —— 5 万条一份到底多大、存/读各要多久，只有量出来才知道值不值得
// 挪到后台。这一节**只量、不拿耗时当断言**（那是这台机器的速度），钉形状的那几条才是断言：
// 5 万条往返逐格还在、第 50001 条拒收、投毒那份拒读且一个字节都不动。
void runDownloadReceiptScaleTests(const QString &sandbox);



