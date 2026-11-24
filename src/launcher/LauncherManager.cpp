/**
 * @file LauncherManager.cpp
 * @brief 处理 Minecraft 游戏启动和 JVM 参数生成。
 * @note 使用 version.json 依赖和平台规定以避免 classpath 冲突。
 *
 * 这个 class 用于使用正确的 JVM 参数、 classpath 构建、特定平台 library 处理、启动 Minecraft。
 */

#include "LauncherManager.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QRegularExpression>

#include "util/StorageAvailability.hpp"

#include <algorithm>

#if defined(Q_OS_WIN)
// NOTE: 放在所有 Qt 头之后 —— windows.h 定义的东西太多（min/max 宏、`interface`……），
// 先于 Qt 包含容易撞名。NOMINMAX 是给 std::min/std::max 让路（windows.h 会定义
// min/max 宏），WIN32_LEAN_AND_MEAN 少拉一堆用不上的头（winsock、shellapi……）。
// 和 TokenStore.cpp 里那一套同一个写法。
// 用途只有一个：下面的 applyProcessPriority()（OpenProcess / SetPriorityClass）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "../auth/AccountStore.hpp"
#include "../auth/AuthManager.hpp"
#include "../manage/FolderManager.hpp"
#include "../manage/InstanceFiles.hpp"
#include "../settings/EffectiveJavaSettings.hpp"
#include "../util/AppPaths.hpp"
#include "../util/WriteDirectoryGuard.hpp"
#include "../util/ZipUtil.hpp"
#include "AsciiPathAlias.hpp"
#include "LibraryRules.hpp"
#include "ProcessPathLimit.hpp"
#include "VersionArguments.hpp"

namespace {

/**
 * @brief 调用方没给玩家名时用的兜底名字（例如测试、未登录）。
 *
 * NOTE: 值和 AuthManager 的默认玩家名一致，但那边的 playerName() 才是**唯一来源** ——
 * 真实启动路径（MainWindow）永远会把名字传进来，这里只是保证 --username 不会
 * 是空的（空名字会让游戏直接拒绝启动）。
 */
constexpr char kDefaultPlayerName[] = "Player";

/// version.json 里没有 `mainClass` 时用的默认主类。
///
/// NOTE: 别把它当成"原版主类的权威来源"。原版每个版本的 json 里都写着这个字段，
/// 这只是一个让"缺字段的老版本 / 手写的 json"跑原版主类的退路。
constexpr char kDefaultMainClass[] = "net.minecraft.client.main.Main";

/// 带**访问令牌**的那个系统属性（钥匙部分）。
///
/// NOTE: 单独拎出来是因为它有两处用途，而两处都必须认得同一条前缀：一处是拼参数
/// （`-Dminecraft.accessToken=<JWT>`），一处在打日志时把它挡掉（见 startGame()
/// 里那个 -D 循环，不能把令牌写进日志文件）。
const QString kAccessTokenProperty = QStringLiteral("-Dminecraft.accessToken=");

/**
 * @brief 把设置页那一格「全局额外参数」拆成一条条 argv。**这是唯一的拆法。**
 *
 * 注意：不能用 `split(' ', Qt::SkipEmptyParts)` —— **只认空格**。
 * 而那一格是 QPlainTextEdit（三四行高，见 MainWindowUI.cpp 里那段），用户从论坛、
 * 记事本抄参数时几乎必然**一行一条**。带换行的一整段会被当成**一条** argv 传给 java，
 * 同机 JDK 的报错是：
 * Unrecognized VM option 'UseG1GC\n-XX:+UseZGC'
 * Error: Could not create the Java Virtual Machine.
 * 退出码 1 —— 而那时 QProcess::waitForStarted(3000) **已经成功了**（java.exe 真的起来过），
 * 于是启动链照常写「已开始启动游戏」、实例树上标「（运行中）」，用户看到的是"启动了，
 * 然后什么都没发生"。所以这不是"参数不生效"的小事，是**一次静默失败**。
 *
 * 按**任意空白**拆（空格、制表符、CR、LF 都算）：写成一行还是每行一条都能用，
 * 而"用空格分隔"那条原意在两种写法下都成立。
 *
 * NOTE: 不做引号解析 —— 参数里带空格的那种写法（`-Dfoo="a b"`）本来就不是这一格支持的
 * 东西，加一套引号规则只会让"用户以为能用"变成"看着像能用但结果不可预期"。
 */
QStringList splitExtraJvmArgs(const QString &text) {
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

/**
 * @brief 取某个版本解压 natives 的目标目录。
 * @param versionId Minecraft version identifier.
 * @return 返回 versions/<id>/natives 的绝对路径。
 */
QString nativesDirectory(const QString &versionId) {
    return AppPaths::nativesDir(versionId);
}

/**
 * @brief 判断目录里是否已经有解压出来的 DLL。
 * @param dir 待检查的目录。
 * @return 若目录存在且至少含一个 .dll 则返回 true.
 */
bool hasNativeLibraries(const QString &dir) {
    return !QDir(dir).entryList(QStringList() << QStringLiteral("*.dll"), QDir::Files).isEmpty();
}

/// "这一份 natives 是**整套**解出来的"那个标记文件（写在 natives 目录里）。
///
/// 判据不能只有 `hasNativeLibraries()` —— **任意一个** .dll 就算
/// 齐了。可 natives 是按排除项分组、一组一次 `extractArchives()` 解的，第一组成功、第二组
/// 失败（盘满、被杀毒锁住、用户中途关掉启动器）时目录里已经躺着第一组的 DLL：这一趟启动
/// 会如实报错，**下一趟**却在 `hasNativeLibraries()` 那一句直接复用这份残缺目录，于是
/// 症状从"解压失败"变成"进游戏崩在 UnsatisfiedLinkError"，而且再也修不好（每次都复用）。
/// 标记只在**全部**解完、且 DLL 确实在了之后才写；失败时连目录一起删掉。
constexpr char kNativeReadyMarker[] = ".wtfml-natives-ok";

bool nativesExtractionComplete(const QString &dir) {
    return QFileInfo(QDir(dir).filePath(QLatin1String(kNativeReadyMarker))).isFile()
           && hasNativeLibraries(dir);
}

/**
 * @brief 首次为一个版本创建游戏目录时，把界面语言设成跟随系统。
 * @param gameDir 该版本的隔离游戏目录。
 *
 * 只在 options.txt 不存在时写入，所以玩家自己选过的语言永远不会被覆盖。
 * Minecraft 读取时会忽略缺失的键并用默认值补齐，因此只写一行 lang 是安全的。
 */
void ensureInitialOptions(const QString &gameDir) {
    const QString optionsPath = QDir(gameDir).absoluteFilePath(QStringLiteral("options.txt"));
    if (QFileInfo::exists(optionsPath)) {
        return;
    }
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(gameDir, &lockProblem)) {
        qWarning() << "Cannot write initial options.txt:" << lockProblem;
        return;
    }
    if (!QDir().mkpath(gameDir)) {
        return;
    }

    // QLocale::system().name() 形如 "zh_CN"，而 Minecraft 的语言代码是全小写的 "zh_cn"，
    // 正好和资源包里的 lang/<code>.json 文件名对应。
    QString lang = QLocale::system().name().toLower();
    // "C" 区域（从未设置过 locale）或者没有国家/地区后缀时，没有对应的语言文件，
    // 这时只能用 Minecraft 内置在客户端 jar 里的 en_us。
    if (lang.isEmpty() || lang == QLatin1String("c") || !lang.contains(QLatin1Char('_'))) {
        lang = QStringLiteral("en_us");
    }

    QFile file(optionsPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Failed to create" << optionsPath << file.errorString();
        return;
    }
    file.write(QStringLiteral("lang:%1\n").arg(lang).toUtf8());
    file.close();
    qDebug() << "Created initial options.txt with lang" << lang;
}

#ifdef Q_OS_WIN
/// ProcessPriority → Windows 的 priority class 常量。一一对应，没有别的换算。
DWORD priorityClassOf(ProcessPriority priority) {
    switch (priority) {
    case ProcessPriority::Low:
        return IDLE_PRIORITY_CLASS;
    case ProcessPriority::BelowNormal:
        return BELOW_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::AboveNormal:
        return ABOVE_NORMAL_PRIORITY_CLASS;
    case ProcessPriority::High:
        return HIGH_PRIORITY_CLASS;
    case ProcessPriority::Normal:
        break;
    }
    return NORMAL_PRIORITY_CLASS;
}
#endif

/**
 * @brief 给刚起来的游戏进程设 CPU 优先级（设置页「游戏」栏 →「进程优先级」）。
 *
 * @param process 已经 start() 过、而且进程真的起来了的那个 QProcess。
 * @param priority 要设的档位。
 *
 * NOTE: 调用的时机只有一处 —— waitForStarted() 成功之后、紧跟着的那一行。两个理由：
 * ① `processId()` 在进程起来之前是 0，那之前 OpenProcess 没东西可开；
 * ② 越早设越接近"从第一行代码起就是这个优先级"，晚了的话刚开始那一段初始化
 * （分配几个 GB 内存、解压 natives）已经按旧的档位跑过了。
 *
 * NOTE: 失败**只写日志、不拦启动**。设优先级是"锦上添花"，没设上不代表游戏不能玩；
 * 把它变成一条拦下启动的错误，就是拿一个次要功能去堵主流程。常见失败是
 * 进程刚好在这一瞬间退了（OpenProcess 拿到 ERROR_INVALID_PARAMETER），
 * 那不是异常。
 *
 * NOTE: 非 Windows 平台是**空操作**，不是"用 setpriority 凑一个等价物"：两边的档位
 * 根本对不上（POSIX 是 nice 值，Windows 是调度类），凑出来的东西没人验证过。
 */
void applyProcessPriority(QProcess *process, ProcessPriority priority) {
#ifdef Q_OS_WIN
    if (process == nullptr) {
        return;
    }
    const DWORD pid = static_cast<DWORD>(process->processId());
    if (pid == 0) {
        qWarning() << "Process is gone before priority could be set, skipping.";
        return;
    }
    // 只要 PROCESS_SET_INFORMATION —— 那是 SetPriorityClass 需要的最小权限。
    // 不要为了省事去要 PROCESS_ALL_ACCESS：多要的权限对我们没用，而"一个启动器去要
    // 完全控制另一个进程"正是某些安全软件盯着的形状。
    HANDLE handle = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        qWarning() << "OpenProcess failed for priority, error:" << GetLastError();
        return;
    }
    if (SetPriorityClass(handle, priorityClassOf(priority)) == 0) {
        qWarning() << "SetPriorityClass failed, error:" << GetLastError();
    } else {
        qDebug() << "Process priority set for PID" << static_cast<qulonglong>(pid);
    }
    CloseHandle(handle);
#else
    Q_UNUSED(process);
    Q_UNUSED(priority);
#endif
}

}  // namespace

LauncherManager::LauncherManager(QObject *parent) : QObject(parent) {
    // 没有别的初始化：QProcess 是**每份会话一个**，在 startGame() 里现建（见那里的说明）。
    // 长驻的单个 m_gameProcess 会把多开限死成一个。
}

bool LauncherManager::checkPreLaunch(const QString &versionId, QStringList *errors,
                                     const QJsonObject &versionJson) const {
    if (errors) {
        errors->clear();
    }

    const QString basePath = AppPaths::minecraftDir();
    if (basePath.isEmpty()) {
        if (errors) {
            errors->append(tr("无法获取 Minecraft 目录。"));
        }
        return false;
    }

    const QFileInfo info(basePath);
    if (!info.exists() && !QDir().mkpath(basePath)) {
        if (errors) {
            // 盘被拔掉时 `mkpath()` 只会回一个 false。原话「无法创建」听着像权限问题，
            // 用户会去改盘符/权限；能判断是卷没了就直说卷没了。
            const QString storageProblem = StorageAvailability::unavailableReason(basePath);
            errors->append(storageProblem.isEmpty()
                               ? tr("无法创建 Minecraft 目录：%1").arg(basePath)
                               : tr("游戏目录所在的磁盘现在不可用：%1\n%2")
                                     .arg(QDir::toNativeSeparators(basePath), storageProblem));
        }
        return false;
    }

    // 盘是在上面那几行之后才被拔掉的（或者网络盘半断不断）：下面每一句都会说
    // "缺少客户端 JAR"/"libraries 文件夹为空"，用户照着去找，会以为文件真被删了。
    // 问一次卷，盘不在就直接说盘不在、别的都别猜。
    // NOTE: 代价是每次启动多一次卷查询，不碰盘上的任何文件。
    const QString storageProblem = StorageAvailability::unavailableReason(basePath);
    if (!storageProblem.isEmpty()) {
        if (errors) {
            errors->append(tr("游戏目录所在的磁盘现在不可用：%1\n%2")
                               .arg(QDir::toNativeSeparators(basePath), storageProblem));
        }
        return false;
    }

    // 为特定版本检查所需的游戏文件
    //
    // NOTE: 不能硬拼 "%1/versions/%2/%2.jar"，要走 InstanceFiles，理由同
    // getVersionJson()：目录名被外部改过之后里面的 jar 还叫老名字，硬拼会
    // 报"缺少客户端 JAR"，而那个 jar 明明就在目录里躺着。认出来和还能启动是两件事，
    // 判据放松了，路径解析就得跟着放松，否则只是把失败从"列表里看不见"挪到了"点了报缺 jar"。
    const QString versionJarPath = InstanceFiles::jarPath(versionId);
    const QFileInfo jarInfo(versionJarPath);
    if (!jarInfo.exists()) {
        if (errors) {
            errors->append(tr("缺少客户端 JAR 文件：%1").arg(versionJarPath));
        }
        return false;
    }

    // 检查 libraries 文件夹
    const QString librariesPath = QString("%1/libraries").arg(basePath);
    const QDir librariesDir(librariesPath);
    if (!librariesDir.exists() ||
        librariesDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        if (errors) {
            errors->append(tr("libraries 文件夹为空或不存在"));
        }
        return false;
    }

    // 检查 assets 文件夹
    const QString assetsPath = QString("%1/assets").arg(basePath);
    const QDir assetsDir(assetsPath);
    if (!assetsDir.exists()) {
        if (errors) {
            errors->append(tr("assets 文件夹不存在"));
        }
        return false;
    }

    // 旧格式（1.18.2 及以下）依赖解压出来的 natives，缺了只会崩在
    // UnsatisfiedLinkError，不如在这里就说清楚。1.19+ 没有 natives 字段，直接跳过。
    if (!versionJson.isEmpty()) {
        bool needsNatives = false;
        const QJsonArray libraries = versionJson[QStringLiteral("libraries")].toArray();
        for (const QJsonValue &value : libraries) {
            if (LibraryRules::nativeClassifier(value.toObject()).valid()) {
                needsNatives = true;
                break;
            }
        }

        if (needsNatives && !hasNativeLibraries(nativesDirectory(versionId))) {
            if (errors) {
                errors->append(tr("该版本需要 natives 组件，但 %1 里没有解压出 DLL。")
                                   .arg(nativesDirectory(versionId)));
            }
            return false;
        }
    }

    return true;
}

QString LauncherManager::prepareNatives(const QJsonObject &versionJson, const QString &versionId,
                                        QStringList *errors,
                                        const std::function<void(int done, int total)> &progress) const {
    if (versionJson.isEmpty()) {
        return QString();
    }

    const QJsonArray libraries = versionJson[QStringLiteral("libraries")].toArray();
    QList<LibraryRules::NativeClassifier> classifiers;
    for (const QJsonValue &value : libraries) {
        const LibraryRules::NativeClassifier classifier =
            LibraryRules::nativeClassifier(value.toObject());
        if (classifier.valid()) {
            classifiers.append(classifier);
        }
    }

    // 1.19+ 走这里：classifier 写在 name 里、由 LWJGL 自己从 classpath 的 jar 提取，
    // 既不需要 -Djava.library.path 也不需要解压。
    if (classifiers.isEmpty()) {
        return QString();
    }

    const QString nativesDir = nativesDirectory(versionId);
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(AppPaths::versionDir(versionId), &lockProblem)) {
        if (errors) errors->append(lockProblem);
        return QString();
    }
    // 守卫：上一次**整套**解压过就直接复用。除第一次启动外，这里几乎是零开销。
    // 判据是标记文件（见 `nativesExtractionComplete()`），不是"有一个 DLL 了"。
    if (nativesExtractionComplete(nativesDir)) {
        return nativesDir;
    }

    const QString librariesRoot = AppPaths::libraryDir();

    // 按"这一条自己的 extract.exclude"分组解压，**不是**把所有人的排除项并成一份再去解所有
    // 人的 jar。
    //
    // 分组的代价：排除项不同的那几组各起一次 PowerShell（这些版本最多两组），
    // 而这条路**只在第一次启动时走**（上面 hasNativeLibraries 那条守卫会复用已解压的
    // 目录），所以那点开销付一次就没了。
    QHash<QString, QStringList> jarsByExcludes;
    QStringList missing;
    for (const LibraryRules::NativeClassifier &classifier : classifiers) {
        const QString jarPath = QDir(librariesRoot).absoluteFilePath(classifier.path);
        if (!QFileInfo::exists(jarPath)) {
            missing << classifier.path;
            continue;
        }
        QStringList own = classifier.excludes;
        own.sort();
        jarsByExcludes[own.join(QLatin1Char('\n'))] << QDir::toNativeSeparators(jarPath);
    }

    if (!missing.isEmpty()) {
        if (errors) {
            errors->append(tr("natives 组件尚未下载，请先重新下载该版本。缺少：%1")
                               .arg(missing.join(QStringLiteral(", "))));
        }
        return QString();
    }

    int jarCount = 0;
    for (const QStringList &group : jarsByExcludes) {
        jarCount += group.size();
    }
    qDebug() << "Extracting" << jarCount << "natives archives to" << nativesDir
             << "in" << jarsByExcludes.size() << "group(s)";
    if (progress) {
        progress(0, jarCount);
    }
    int completedOffset = 0;
    QString error;
    for (auto it = jarsByExcludes.constBegin(); it != jarsByExcludes.constEnd(); ++it) {
        // 键是排序后拼起来的排除项；空键 = 这一组一条排除项都没有（**不是**"排除项为空串"，
        // 所以要在这里分开处理 —— 空串会被 ZipUtil 归一成 "/"，那是一条匹配不到东西的前缀，
        // 看着无害，但那是靠巧合）。
        const QStringList ownExcludes =
            it.key().isEmpty() ? QStringList() : it.key().split(QLatin1Char('\n'));
        const std::function<void(int, int)> groupProgress =
            progress ? std::function<void(int, int)>([&](int done, int) {
                           progress(qMin(completedOffset + done, jarCount), jarCount);
                       })
                     : std::function<void(int, int)>{};
        if (!ZipUtil::extractArchives(it.value(), nativesDir, ownExcludes, &error, groupProgress)) {
            // 这一组没解完，前面几组已经躺进目录里了 —— 整个删掉，不留半套（见
            // `kNativeReadyMarker` 那段 NOTE：留着它，下一趟会被当成"解压过了"复用）。
            removeTreeWithoutFollowingLinks(nativesDir);
            if (errors) {
                errors->append(tr("解压 natives 失败：%1").arg(error));
            }
            return QString();
        }
        completedOffset += int(it.value().size());
    }

    if (!hasNativeLibraries(nativesDir)) {
        removeTreeWithoutFollowingLinks(nativesDir);
        if (errors) {
            errors->append(tr("natives 解压后未找到 DLL：%1").arg(nativesDir));
        }
        return QString();
    }

    // 全部解完、DLL 也确实在了 —— 这才写标记。
    // NOTE: 写不进去**不算失败**：那只会让下一趟重新解一遍（代价几秒），而"解压成功却报
    // 失败"会把一次能起来的启动拦下来。
    QFile marker(QDir(nativesDir).filePath(QLatin1String(kNativeReadyMarker)));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Cannot write natives marker, next launch will extract again:"
                   << marker.fileName() << marker.errorString();
    } else {
        marker.close();
    }

    qDebug() << "Natives ready at" << nativesDir;
    return nativesDir;
}

/**
 * @brief 为 Minecraft 启动生成 JVM 参数。
 *
 * 构造完整的 JVM 参数集，包括内存设置、 classpath 、系统属性和游戏特定参数。
 * 使用 version.json 中的依赖项构建 classpath ，并应用针对特定平台的 library 过滤。
 *
 * **完整的参数说明看 LauncherManager.hpp 上那一段**（那是唯一权威的一份，两边都写着就会
 * 各漂各的）。这里只留"为什么这么写"的部分。
 *
 * NOTE: 这里刻意**只加硬编码做不到的事**，凡是 version.json 的 arguments.jvm 声明了的
 * 就按它的规则过滤后原样传（见下面那段）。**没有**任何一份"我们自己的调优预设"
 * 在这一层硬编码 —— 内存、GC、JIT 这些随机器/堆大小/Java 版本而变的东西，硬编码
 * 等于替用户做主了，归 versionJvmArgs 和 GlobalJavaSettings::extraJvmArgs() 两个口子。
 *
 * 真要往设置界面放这类预设，得注意版本门槛 —— JDK 21 与 25 两个运行时上：
 * -XX:+PerfDisableSharedMem 全版本可用；-XX:+UseCompactObjectHeaders 只有新 JDK
 * 接受，旧版是 Unrecognized option 直接起不来。所以预设必须一并带上版本过滤。
 */
QStringList LauncherManager::generateJvmArguments(const QString &token, const QString &versionId,
                                                  const QString &assetIndexId,
                                                  const QJsonObject &versionJson,
                                                  const EffectiveJavaSettings &settings,
                                                  bool offlineMode, int javaMajorVersion,
                                                  const QStringList &versionJvmArgs,
                                                  const QString &playerName,
                                                  const QString &playerUuid,
                                                  const GameWindowSettings &window,
                                                  const ScreenWorkArea &screen,
                                                  const QuickPlay::Target &quickPlay) const {
    if (!offlineMode) SecretRedactor::remember(token);
    // 分辨率那两个数**这一层不再自己判**：哪些数要发、发不发，全在 gameWindowArguments()
    // 那个纯函数里 —— 它是纯的，验证工程能直接把三档 + 屏幕尺寸喂进去看发出来什么。
    //
    // NOTE: 下面填 `${resolution_width}` 那两个占位符**还是**照 window 里那两个数来
    // （不是照"最大化算出来的那一个"）：占位符这条路今天根本走不到（见它自己那段），
    // 真走到了，"用户设的那两格"也是更该填进去的那一份。
    const int windowWidth = window.width;
    const int windowHeight = window.height;

    QStringList args;

    qDebug() << "=== Generating JVM Arguments ===";
    qDebug() << "Memory settings: -Xms" << settings.xms << "m -Xmx" << settings.xmx << "m";
    qDebug() << "Offline mode:" << offlineMode;
    qDebug() << "Java major version:" << javaMajorVersion;

    // 玩家名只有一个来源（调用方传 AuthManager::playerName()），两条分支都取它。
    // 空值退回默认名：--username 空着游戏会直接拒绝启动，所以宁可兜一个名字。
    const QString username = playerName.isEmpty() ? QString::fromLatin1(kDefaultPlayerName)
                                                   : playerName;

    args << QStringLiteral("-Xms%1m").arg(settings.xms);
    args << QStringLiteral("-Xmx%1m").arg(settings.xmx);

    if (!settings.extraJvmArgs.isEmpty()) {
        qDebug() << "Extra JVM args:" << settings.extraJvmArgs;
        args << splitExtraJvmArgs(settings.extraJvmArgs);
    }

    // 按版本的自定义参数，**排在全局之后**：JVM 对同一条可重复选项取后写的那个，
    // 所以单个实例能盖掉全局默认那套（全局默认是 -XX:+UseG1GC）。
    if (!versionJvmArgs.isEmpty()) {
        qDebug() << "Per-version JVM args:" << versionJvmArgs;
        args << versionJvmArgs;
    }

    args << QStringLiteral("-Dlog4j2.formatMsgNoLookups=true");
    args << QStringLiteral("-XX:-OmitStackTraceInFastThrow");
    // --enable-native-access 是 JDK 17+ 才有的选项，用于启用对 Java 25+ 的原生兼容性。
    // Java 8（1.16.5 及以下需要）碰到它会直接 Unrecognized option 秒退，连主类都到不了，
    // 所以必须按实际使用的 Java 版本过滤。javaMajorVersion 为 0 表示调用方没传，此时照发。
    if (javaMajorVersion >= 17 || javaMajorVersion <= 0) {
        args << QStringLiteral("--enable-native-access=ALL-UNNAMED");
    }
    // --sun-misc-unsafe-memory-access：Java 8/17/21 **拒绝**（Unrecognized option，
    // JVM 直接起不来）、Java 25 接受。这条写错门槛的后果不是"参数不生效"而是"游戏压根
    // 启动不了"，所以门槛取**下界 25**：JEP 498 是在 JDK 24 引入这个选项的，
    // 24 落在没有验证过的区间（21 拒绝、25 接受之间正好隔着它），而两边的
    // 代价完全不对称 —— 少加一条只是少压一句警告，加错一条是游戏起不来。
    // 26.x 的 version.json 自己声明了它，下面的去重会让声明成为唯一那份；这条主要是
    // 兜住"用户给老版本手动指定了新 Java"。
    if (javaMajorVersion >= 25) {
        args << QStringLiteral("--sun-misc-unsafe-memory-access=allow");
    }
    // -Dstdout.encoding / -Dstderr.encoding 是 JDK 19 才引入的属性（JDK-8284879）。
    // Java 8-18 会**忽略**它们，System.out 仍按本地代码页（中国区 Windows = GBK）写字节，
    // 那时按 UTF-8 解码反而是把好好的中文解成乱码。所以这条门槛和
    // decodeGameOutput() 的解码选择必须同时成立 —— 实现上不让两边各记一份状态，
    // 由 startGame() 从最终命令行反推（见那里的注释）。
    if (javaMajorVersion >= 19) {
        args << QStringLiteral("-Dstdout.encoding=utf-8");
        args << QStringLiteral("-Dstderr.encoding=utf-8");
    }
    args << QStringLiteral("-Djava.net.preferIPv4Stack=true");

    // NOTE: 去重按**路径**比，大小写不敏感 —— Windows 的路径本来就是大小写不敏感的，
    // 而 BootstrapLauncher（Forge/NeoForge）拿 `java.class.path` 里的字符串去和
    // JVM 模块路径里的**盘上路径**做 `Paths.get(a).equals(Paths.get(b))` 比较，
    // `WindowsPath.equals` 也是忽略大小写的。这里跟着它保持一致。
    //
    // 判据改从一个跟着走的 `QSet`（键 = `toCaseFolded()`，和上面那句
    // `Qt::CaseInsensitive` 同一套折叠规则）里查，不再每来一个库就把不断变长的
    // `QStringList` 线性扫一遍 —— 一次启动约 200 个库，那就是约 200 次 O(n) 比较。
    // `classpathSeen` 与 `classpath` **必须同时维护**，唯一写入口就是下面这个 lambda。
    QStringList classpath;
    QSet<QString> classpathSeen;
    const auto addClasspathPath = [&](const QString &path) {
        if (path.isEmpty()) {
            return;
        }
        const QString key = path.toCaseFolded();
        if (classpathSeen.contains(key)) {
            return;
        }
        classpathSeen.insert(key);
        classpath << path;
    };

    bool needsNativesDir = false;
    // 先添加主版本 JAR 文件。NOTE: jar 的**名字和目录**都走 InstanceFiles 解析，而不是
    // 直接写 versionId —— 两个坑：
    // 1. 目录名被外部改过之后里面的 jar 还叫老名字，按 id 硬拼出来的 classpath 会指向一个
    // 不存在的文件，表现是 JVM 一句 "Could not find or load main class" 就没了；
    // 2. **带模组加载器的版本目录里根本没有 jar**（Fabric 装出来的只有一份 json），
    // 客户端 jar 在原版那个目录里 —— jarRelativePath() 会顺着 inheritsFrom 链找到它。
    //
    // 必须发**绝对路径**：相对路径在这里
    // 不是"风格问题"，是**实打实的启动失败**：
    // Forge/NeoForge 的 version json 用 `${library_directory}`（绝对）拼了 `-p` 模块路径，
    // 而 classpath 上是相对串 —— BootstrapLauncher 逐个比"这个 jar 是不是已经在 JVM 的
    // 模块路径上了"，相对的 `Paths.get("libraries/cpw/...")` 和绝对路径
    // cpw\\...")` 不相等，于是它认为同一个 jar 出现了两次并抛：
    // IllegalStateException: Module named cpw.mods.bootstraplauncher was already on the
    // JVMs module path loaded from D:\...\bootstraplauncher-2.0.2.jar but class-path
    // contains it at location libraries\cpw\mods\bootstraplauncher\2.0.2\bootstraplauncher-2.0.2.jar
    // 原版启动器发的也是绝对路径，这里与它一致。
    //
    // 绝对路径的基准是 AppPaths::minecraftDir()，和 startGame() 里那个 setWorkingDirectory
    // 无关了 —— cwd 换成版本目录（PCL 那种）不再需要同时改这里。
    const QString minecraftRoot = AppPaths::minecraftDir();
    const auto absoluteEntry = [&minecraftRoot](const QString &relative) {
        return QDir::toNativeSeparators(QDir(minecraftRoot).absoluteFilePath(relative));
    };

    // ---------------- ASCII 别名（只对 Java 8 生效，见 AsciiPathAlias 那个文件） ----------
    // Java 8 打不开任何带非 ASCII 字符的路径（真机上 `20w14∞` 和中文名的 1.8 整合包都起不来，
    // 报的是 `找不到或无法加载主类`）。要换的那几条路径**共用这一个判据**：客户端 jar（进
    // classpath）、natives（-Djava.library.path 和 ${natives_directory}）、--gameDir。
    //
    // 建它是 MainWindow 在启动前那一步做的（AsciiPathAlias::ensure()，失败会弹窗并中止），
    // 这里**只读盘**：这一层仍然是"给我一份 version.json，我拼一条命令行"，副作用只有
    // 下面那个 natives 的空目录。
    // NOTE: 只判 gameDir 和 jar：natives 那一份是 ensure() 按"真实的 natives 目录存不存在"
    // 建的（不存在就不建），跟着 needsNativesDir 走 —— 那边真缺了会在下面
    // hasNativeLibraries() 那一句里现原形（qWarning），不会静默发一条错路径出去。
    const AsciiPathAlias::Paths asciiAlias = AsciiPathAlias::pathsFor(versionId);
    const QString realGameDir = AppPaths::versionDir(versionId);
    const QString realJarPath = InstanceFiles::jarPath(versionId);
    const bool useAsciiAlias =
        AsciiPathAlias::needed(javaMajorVersion, {realGameDir, realJarPath}) &&
        AsciiPathAlias::isReady(asciiAlias);
    if (useAsciiAlias) {
        qDebug() << "Java 8 + 非 ASCII 路径：改用 ASCII 别名" << asciiAlias.root;
    }
    const QString gameDirForJava =
        useAsciiAlias ? QDir::toNativeSeparators(asciiAlias.gameDir)
                      : QDir::toNativeSeparators(AppPaths::versionDir(versionId));
    // 实例目录顶层某个 jar 在 classpath 上该用的路径（要不要走别名都在这里一处判）。
    // `ownerId` 是**必填**的。这个 lambda 不能自己调
    // `jarOwnerId(versionId)`：下面"兄弟 jar"那一圈读的是 `versionDir(versionId)` 里的
    // 文件 —— 两处对同一个 `versionId` 问出了**不同**的目录：PCL 那种合并实例里客户端
    // jar 在 `versions/<原版>`、兄弟 jar 在 `versions/<实例>`，于是兄弟 jar 的读取用实例
    // 目录、拼出来的路径却指回原版目录。轻则那个 jar 不在 classpath 上（主类找不到，症状
    // 和这个功能要修的 bug 一模一样），重则**同名不同物**：原版目录里恰好有个同名 jar 时
    // 塞进 classpath 的是另一个文件。读取方和拼路径方必须是同一个目录。
    const auto classpathJarFor = [&](const QString &ownerId, const QString &fileName) {
        return useAsciiAlias ? QDir::toNativeSeparators(asciiAlias.jarFor(fileName))
                             : absoluteEntry(
                                   QStringLiteral("versions/%1/%2").arg(ownerId, fileName));
    };

    addClasspathPath(classpathJarFor(InstanceFiles::jarOwnerId(versionId),
                                     InstanceFiles::jarName(versionId)));

    // 根据 version.json library 依赖构建 classpath
    if (!versionJson.isEmpty()) {
        auto libs = versionJson[QStringLiteral("libraries")].toArray();
        for (const auto &value : libs) {
            auto libObj = value.toObject();
            // 跳过不适用这个平台的库
            if (!LibraryRules::libraryAllowed(libObj)) {
                continue;
            }

            // 解析（含"只有 name+url"的 maven 风格库，加载器版本全是这种）在
            // LibraryRules::artifactFor() —— 下载时挑库用的是同一个函数。
            // NOTE: 加载器的库**必须**进 classpath，否则 Knot/Mixin 这些类找不到，
            // 症状是 "Could not find or load main class net.fabricmc.loader..."。
            //
            // 判据用 hasPath() 而不是 valid()：valid() 要求有下载地址（那是"下得动"的意思，
            // 下载列表用的是它），而 OptiFine 那两个库**没有地址**、jar 就在盘上 —— 拿
            // valid() 当判据会把它们从 classpath 上漏掉，症状是启动时 launchwrapper 找不到。
            const LibraryRules::Artifact artifact = LibraryRules::artifactFor(libObj);
            if (artifact.hasPath()) {
                addClasspathPath(
                    absoluteEntry(QStringLiteral("libraries/%1").arg(artifact.path)));
            }

            // NOTE: 旧格式（1.18.2 及以下）的 natives classifier jar 不进 classpath ——
            // 它们由 prepareNatives 解压到 versions/<id>/natives/，再通过
            // -Djava.library.path 提供给 LWJGL。把不存在的 classifier 路径塞进
            // classpath 会让这些版本崩在 UnsatisfiedLinkError。
            // 1.19+ 的库没有 natives 字段，走上面的 artifact 分支留在 classpath 上，
            // 由 LWJGL 自己从 jar 里提取。
            //
            // 判据**必须**和 prepareNatives()（决定"到底解压了没有"）以及
            // checkPreLaunch()（决定"缺了要不要提前拦"）用的是**同一条**：
            // LibraryRules::nativeClassifier(...).valid()。不能写成
            // `!libObj["natives"].toObject().isEmpty()`：那只问"有没有
            // natives 字段"，不问**这一台机器**用不用得上：1.12.2 的
            // lwjgl-platform:2.9.2-nightly 在 Windows 上被 rules 禁掉、只有 Linux 的
            // classifier 时，它照样让这里要一个 natives 目录，而 prepareNatives() 那份
            // 一个 DLL 都不会解出来；两边对不上的表现是"verify 说这个版本没问题、启动时
            // 只留一句 qWarning、进游戏崩在 UnsatisfiedLinkError"。
            if (LibraryRules::nativeClassifier(libObj).valid()) {
                needsNativesDir = true;
            }
        }
    } else {
        qDebug() << "Warning: versionJson is empty, classpath will include only the version JAR.";
    }

    // NOTE: 必须是绝对路径。JVM 在启动早期就读取并缓存 java.library.path，
    // 相对路径的解析基准是那一刻的工作目录，不可依赖；原版启动器传的也是绝对路径。
    // NOTE: 走了 ASCII 别名时这里换成别名里那一份（它是个联接，指的还是同一个目录）——
    // -Djava.library.path 是**递给 Java 看**的路径，非 ASCII 一样会被它认岔。
    if (needsNativesDir) {
        const QString nativesDir = useAsciiAlias ? asciiAlias.nativesDir : nativesDirectory(versionId);
        if (hasNativeLibraries(nativesDir)) {
            const QString nativePath = QDir::toNativeSeparators(nativesDir);
            args << QStringLiteral("-Djava.library.path=%1").arg(nativePath);
            // LWJGL 2（1.12.2 及以下）只认 java.library.path，这条是给它的冗余保险
            args << QStringLiteral("-Dorg.lwjgl.librarypath=%1").arg(nativePath);
        } else {
            qWarning() << "Natives directory is missing or empty, the game will most likely fail "
                          "with UnsatisfiedLinkError:"
                       << nativesDir;
        }
    }

    // 主类**从 version.json 读**，不再硬编码。硬编码的后果是带模组加载器的版本
    // "装上了但没生效"：Fabric 的 KnotClient / Forge 的 bootstrap 都在这个字段里，
    // 不看它就是把原版主类跑起来，加载器一行都没执行。
    //
    // NOTE: 取不到字段时退回原版主类。这不是兜底"猜一个"：
    // 没有这个字段的版本（老版本、手写的 json）本来就是跑原版主类。
    //
    // NOTE: 这里（拼 classpath 的时候）就把它读出来，是因为下面那段"主类是不是装在
    // 实例目录里另一个 jar 里"要用它判；发到命令行上是后面的事（位置见那里的 NOTE）。
    const QString mainClass = versionJson.value(QStringLiteral("mainClass")).toString();
    const QString mainClassEntry = mainClass.isEmpty()
                                       ? QString()
                                       : QString(mainClass).replace(QLatin1Char('.'), QLatin1Char('/')) +
                                             QStringLiteral(".class");

    // 主类既不在客户端 jar、也不在 libraries 那 107 条里时，最后在**实例目录**里找一找：
    // 把"真的装着主类"的那个 jar 加进 classpath。
    //
    // NOTE: 真机上就这么一例（`GTNH 2.8.4`）—— 它用 lwjgl3ify 重生启动，主类
    // `me.eigenraven.lwjgl3ify.relauncherstub.RelauncherStubMain` 装在版本目录里那个
    // 和客户端 jar 并排的 `lwjgl3ify-forgePatches.jar` 里，而**那个 jar 不在
    // libraries 里**（json 那 51 个库一个都不提它，arguments.jvm 也是空的）。
    // PCL 的做法是把版本目录下**所有** jar 都塞进 classpath，那也能起来，但代价是
    // 随便一个躺在版本目录里的 jar 都会进 classpath（可能带进同名类）。
    // 这里只加"真的装着主类"的那一个：
    // - `GTNH 2.8.4` 的那个 jar 里有这个类 ⇒ 加上（这正是它起不来的原因）；
    // - `Hyperlands` 目录里也有第二个 jar（`hyperlands.jar`），那一个是模组、
    // 里面没有主类 ⇒ 一个都不加，classpath 与不处理时逐字节相同。
    // `魔改版ATT2-…` 那个重复的客户端 jar 同理（里面有主类，但它本来就在 classpath
    // 上 —— 上面那句 contains 会把它挡掉）。
    //
    // 判据是"jar 的原始字节里有没有这条 `xxx/yyy/Main.class` 这个字符串"。zip 的条目名是
    // **明文**存在文件里的（本地头 + 中央目录各一份），所以不需要解压、也不需要 zip 库
    // （工程里没有引入 —— 读 zip 一律借 PowerShell，而这里每次启动都要查，不能再起进程）。
    // 万一某个 jar 的**内容**里恰好有这串字节，后果也只是多加一个无害的 jar。
    if (!mainClassEntry.isEmpty()) {
        const QString instanceDir = AppPaths::versionDir(versionId);
        const QStringList siblingJars = InstanceFiles::scanDir(versionId).jarNames;
        // 循环不变量：主类那串字节只算一次（不能每轮 `toUtf8()` 重来一遍）。
        const QByteArray mainClassBytes = mainClassEntry.toUtf8();
        for (const QString &jar : siblingJars) {
            // 读的还是**真实**那一份（别名里的是硬链接，字节一模一样，但真实那份肯定在）；
            // 加进 classpath 的是要递给 Java 的那一个路径（Java 8 时是别名，见上面）。
            const QString full = QDir::toNativeSeparators(instanceDir + QLatin1Char('/') + jar);
            // 这些 jar 就在**实例自己**的目录里（`instanceDir` 上面那句），owner 传 versionId。
            const QString jarPath = classpathJarFor(versionId, jar);
            if (classpathSeen.contains(jarPath.toCaseFolded())) {
                continue;  // 已经在 classpath 上了（客户端 jar 自己，或者别的路径已经把
                           // 同一个文件加过一遍）—— 别再进一遍
            }
            QFile file(full);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray bytes = file.readAll();
            file.close();
            if (bytes.contains(mainClassBytes)) {
                qDebug() << "Main class" << mainClass << "is in a sibling jar of the instance:"
                         << jar;
                addClasspathPath(jarPath);
            }
        }
    }

    const QString classpathStr = classpath.join(";");
    qDebug() << "Classpath:" << classpathStr;

    // ------------------------------------------------------------------
    // version.json 声明的 JVM 参数（arguments.jvm）
    // ------------------------------------------------------------------
    // NOTE: 不能忽略 arguments.jvm、只发我们自己硬编码的一小串参数。
    // 26.3 正是靠 arguments.jvm 声明了 '-XX:StackShadowPages=32'（这个 JDK 的默认值
    // 是 8）：影子区不够时，编译代码在资源重载这种深栈路径上会越过影子区踩到保护页，
    // 表现为 jvm.dll 里的访问冲突 —— 没有 Java 异常、没有 hs_err、没有崩溃报告，
    // 进程直接消失。凡是 version.json 声明了的，就该按它的规则过滤后原样传给 JVM，
    // 而不是由我们维护一份会过期的参数清单。
    // 和上面那条 -Djava.library.path 同一个数（差别只在这里要 native 分隔符）：它在
    // version.json 里是 `${natives_directory}`，1.13～1.16 那几代的行星/Forge 就靠它。
    const QString nativesDir = QDir::toNativeSeparators(
        useAsciiAlias ? asciiAlias.nativesDir : nativesDirectory(versionId));
    const QString librariesDir = AppPaths::libraryDir();

    QHash<QString, QString> placeholders;
    placeholders[QStringLiteral("natives_directory")] = nativesDir;
    placeholders[QStringLiteral("library_directory")] = QDir::toNativeSeparators(librariesDir);
    placeholders[QStringLiteral("launcher_name")] = QStringLiteral("WTFML");
    placeholders[QStringLiteral("launcher_version")] =
        QCoreApplication::applicationVersion().isEmpty()
            ? QStringLiteral("1.0")
            : QCoreApplication::applicationVersion();
    placeholders[QStringLiteral("classpath")] = classpathStr;
    placeholders[QStringLiteral("classpath_separator")] = QStringLiteral(";");
    placeholders[QStringLiteral("version_name")] = versionId;

    QStringList declared =
        VersionArguments::expand(versionJson.value(QStringLiteral("arguments"))
                                     .toObject()
                                     .value(QStringLiteral("jvm"))
                                     .toArray(),
                                 placeholders);

    // BootstrapLauncher 构建转换后的 Minecraft 模块时须忽略原版客户端。
    // 继承或改名实例的实际 JAR 不一定叫 <versionId>.jar；只补真实文件名，不改实例身份。
    if (mainClass == QLatin1String("cpw.mods.bootstraplauncher.BootstrapLauncher")) {
        const QString clientName = InstanceFiles::jarName(versionId);
        const QString prefix = QStringLiteral("-DignoreList=");
        for (QString &arg : declared) {
            if (!arg.startsWith(prefix) || clientName.isEmpty()) continue;
            QStringList names = arg.mid(prefix.size()).split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (!names.contains(clientName)) names.append(clientName);
            arg = prefix + names.join(QLatin1Char(','));
        }
    }

    // 同一条属性只留一份：version.json 声明了就以它为准。值与我们完全相同时也归到
    // 这里，免得同一个 -D 在命令行上出现两次。
    //
    // NOTE: 实现上删的是**整个 args 列表**，所以用户自定义的参数（全局 extraJvmArgs /
    // 按版本 versionJvmArgs，它们排在列表前面）**也在删除范围内** —— 其中
    // -Djava.library.path 那条是按前缀删的，连值都不看，用户写了个不一样的目标也会被删。
    // 净效果与"不删"相同，所以不算"替用户改参数"：declared 是下面才追加到列表末尾的，
    // 而这类可重复选项 JVM 取后写的那个，声明的那份本来就会赢。别以为用户的参数没进这个循环。
    const auto dropOwnIfDeclared = [&args, &declared](const QString &prefix,
                                                      const QString &ownArg) {
        if (VersionArguments::declaresPrefix(declared, prefix)) {
            args.removeAll(ownArg);
        }
    };
    dropOwnIfDeclared(QStringLiteral("--enable-native-access="),
                      QStringLiteral("--enable-native-access=ALL-UNNAMED"));
    dropOwnIfDeclared(QStringLiteral("--sun-misc-unsafe-memory-access="),
                      QStringLiteral("--sun-misc-unsafe-memory-access=allow"));
    // stdout/stderr 这两个按**一对**处理：只要 version.json 声明了其中任意一个，我们那
    // 两条就都撤掉。理由是解码侧（decodeGameOutput）只有一个开关 —— 真出现"stdout 用
    // gbk、stderr 用 utf-8"这种混搭，那一个开关无论取哪边都是错的。已知的 version.json
    // 没有一条声明过它们，这纯粹是让那条不变量在纸面上也成立。
    if (VersionArguments::declaresPrefix(declared, QStringLiteral("-Dstdout.encoding=")) ||
        VersionArguments::declaresPrefix(declared, QStringLiteral("-Dstderr.encoding="))) {
        args.removeAll(QStringLiteral("-Dstdout.encoding=utf-8"));
        args.removeAll(QStringLiteral("-Dstderr.encoding=utf-8"));
    }
    if (VersionArguments::declaresPrefix(declared, QStringLiteral("-Djava.library.path="))) {
        for (int i = args.size() - 1; i >= 0; --i) {
            if (args.at(i).startsWith(QStringLiteral("-Djava.library.path="))) {
                args.removeAt(i);
            }
        }
    }
    qDebug() << "Declared JVM arguments from version.json:" << declared;
    args << declared;

    // 声明的参数引用了 natives 目录就把它建出来：26.3 把它的 /lwjgl /jna /netty /java
    // 子目录当作 LWJGL/JNA/netty 的提取目录，原版启动器在启动前总会创建这个目录。
    // 建个空目录本身无害（prepareNatives 认的是"解压完成"标记 + DLL，空目录两样都没有，
    // 需要解压的版本会照常重解一遍）。
    if (!QDir(nativesDir).exists()) {
        for (const QString &arg : declared) {
            if (arg.contains(nativesDir, Qt::CaseInsensitive)) {
                QDir().mkpath(nativesDir);
                break;
            }
        }
    }

    args << QStringLiteral("-cp") << classpathStr;
    // 主类（上面拼 classpath 时已经读过一次，见那里的 NOTE）。
    //
    // NOTE: 位置必须在 `-cp` **之后**、游戏参数之前 —— 它是 JVM 的第一个位置参数
    // （非选项参数），JVM 拿第一个当主类、其余当程序参数。放进 `-cp` 之前会在
    // 某些版本上被 JVM 当成"类还没就绪"而报 Could not find or load main class。
    args << (mainClass.isEmpty() ? QString::fromLatin1(kDefaultMainClass) : mainClass);

    // 游戏参数占位符（${auth_uuid} 那一套）要用的三样：下面那两条分支各自定下来，等到
    // 函数末尾展开 arguments.game 时用同一份。
    QString argUuid;
    QString argToken;
    QString argUserType;

    // --gameDir / --assetsDir 传**绝对路径**，而不是"靠 cwd 解析出来的相对路径"。
    //
    // NOTE: 不能写成 "versions/<id>" 和 "assets" 两个相对路径让 cwd 解析：同一个目录
    // 在两处各算一遍、其中一处依赖调用顺序，切换 cwd 时就会漂。绝对路径的解析结果
    // 逐字节相同，但不再依赖 cwd，也为"切换 .minecraft 文件夹"准备了条件。
    //
    // cwd 跟随隔离实例；共享 assets 与 classpath 使用绝对路径，不依赖 cwd。
    if (offlineMode) {
        // 离线模式：使用虚假登录证书
        // NOTE: 玩家名就是调用方给的那个（底栏上显示的名字）：名字就是账户的
        // 身份，每次启动换一个的话，单机存档的主人公名字会跟着变，离线服务器的
        // 离线 UUID（由名字推出来）也会每次都被当成新玩家。唯一真的需要"不撞名"
        // 的场合是同一台机器上开两个离线实例，那种情况用户自己会改名字。
        //
        // NOTE: uuid 必须从名字推（md5("OfflinePlayer:"+名字) 摆成 RFC 4122 v3），不能每次
        // 启动新生成一个：换一个就会被当成新玩家。发出去的 `--uuid`、以及命令行
        // 里 `${auth_uuid}` 那个占位符用的都是它，于是任何采用客户端 uuid 的消费方
        // （一部分单机存档、部分服务器和模组）会把同一个名字当成新玩家：
        // 背包、权限、领地全丢。它和 AccountStore 里离线账号那份身份**同一个值**，
        // 启动器界面上显示的、和游戏里用的是同一个身份。
        // 命令行上的形状不变：仍然是带横线的小写 uuid。
        // 形状是稳的：名字在上面归一过了（空值退回默认名），offlineUuidFor() 给的是
        // 32 位十六进制，dashedUuid() 补成 36 字符的标准写法。
        const QString fakeUuid = AuthManager::dashedUuid(AccountStore::offlineUuidFor(username));
        argUuid = fakeUuid;
        argToken = QStringLiteral("0");
        argUserType = QStringLiteral("legacy");

        qDebug() << "Offline mode - Player:" << username << "UUID:" << fakeUuid;

        args << QStringLiteral("--username") << username << QStringLiteral("--version")
             << versionId << QStringLiteral("--gameDir") << gameDirForJava
             << QStringLiteral("--assetsDir") << AppPaths::assetsDir()
             << QStringLiteral("--assetIndex") << assetIndexId << QStringLiteral("--uuid")
             << fakeUuid << QStringLiteral("--accessToken")
             << QStringLiteral("0")  // 离线模式使用的虚假令牌
             << QStringLiteral("--userType") << QStringLiteral("legacy");
    } else {
        // 正版登录：使用真实登录证书
        // NOTE: 令牌和 UUID 都是真的（登录流程走完整条链，档案里的 name/uuid 都拿
        // 到了）。没有档案时（没登录、或者测试没传）才退回全零 UUID —— 那等于一个
        // "谁都不是"的身份，在正版服上不可用。
        const QString uuid = playerUuid.isEmpty()
                                 ? QStringLiteral("00000000-0000-0000-0000-000000000000")
                                 : playerUuid;
        argUuid = uuid;
        argToken = token;
        argUserType = QStringLiteral("msa");
        qDebug() << "Token length:" << token.length();
        args << kAccessTokenProperty + token;
        args << QStringLiteral("-Dminecraft.version=%1").arg(versionId);

        args << QStringLiteral("--username") << username << QStringLiteral("--version")
             << versionId << QStringLiteral("--gameDir") << gameDirForJava
             << QStringLiteral("--assetsDir") << AppPaths::assetsDir()
             << QStringLiteral("--assetIndex") << assetIndexId
             << QStringLiteral("--uuid") << uuid << QStringLiteral("--accessToken") << token
             << QStringLiteral("--userType") << QStringLiteral("msa");
    }

    // --userProperties：**1.8 那一代的主类把它标成 required**，不发就是启动即退：
    // Exception in thread "main" joptsimple.MissingRequiredOptionException:
    // Missing required option(s) ['userProperties']
    // `1.8` 这个实例每次都是这一句。
    //
    // 原版 json 自己声明了 `--userProperties ${user_properties}`，而
    // `user_properties` 这个占位符启动器不提供；上面那段"跳过原版那套参数名"的硬编码块
    // 又**只在 VersionArguments::isVanillaGameArgument() 名单里存着名字**——名单里有它、
    // 硬编码这边若不发，两边都不发，就凭空少了一条 required 的参数。名单和这里必须
    // 一一对应，加一条名单就加一条这里。
    //
    // 值就是原版启动器发的那个空对象：没有 profile 属性时它发的正是 `{}`（1.8 的
    // Main 拿它做 `new JsonObject()` 解析，空对象合法）。两个分支发同一条，所以放在
    // if/else 外面，只有一处。
    args << QStringLiteral("--userProperties") << QStringLiteral("{}");

    // ------------------------------------------------------------------
    // version.json 自己声明的游戏参数（arguments.game / 老格式 minecraftArguments）
    // ------------------------------------------------------------------
    // NOTE: 这一段不能省：NeoForge 26.2 的版本 json
    // 在 arguments.game 里写着 `--fml.neoForgeVersion 26.2.0.88 --fml.mcVersion 26.2
    // --fml.neoFormVersion 2`，而只展开 arguments.jvm 的话 FML 报
    // "When launching in production, --fml.mcVersion must be present as a
    // command-line argument"、`VersionSupportMatrix.<init>` 拿着空版本号 NPE，游戏
    // 在加载窗口那一步就没了。Forge 走同一条路（--launchTarget forgeclient）。
    // 纯原版不受影响：那边 arguments.game 就是我们上面硬编码的这一串，一条都不会
    // 重复（判据在 VersionArguments::extraGameArguments 的注释里）。
    //
    // 这里传进去的是**继承链合并过**的那份 json（调用点给的就是这个），merge 把
    // arguments.game 按"父在前、子在后"拼在一起、并且丢掉了 inheritsFrom —— 所以
    // extraGameArguments 里不能拿 inheritsFrom 当判据（见那边的 NOTE）。
    //
    // NOTE: 位置在**最后**（主类和 --gameDir 那一段之后）。游戏参数对 JVM 来说只是"给主类
    // 的字符串"，顺序上只有"谁盖住谁"的意义；这些加载器参数我们一条都没发过，
    // 没有覆盖关系，放最后最不打扰上面那两段已经验过的形状。
    QHash<QString, QString> gamePlaceholders;
    gamePlaceholders[QStringLiteral("auth_player_name")] = username;
    gamePlaceholders[QStringLiteral("version_name")] = versionId;
    gamePlaceholders[QStringLiteral("game_directory")] =
        QDir::toNativeSeparators(AppPaths::versionDir(versionId));
    gamePlaceholders[QStringLiteral("assets_root")] =
        QDir::toNativeSeparators(AppPaths::assetsDir());
    gamePlaceholders[QStringLiteral("assets_index_name")] = assetIndexId;
    gamePlaceholders[QStringLiteral("auth_uuid")] = argUuid;
    gamePlaceholders[QStringLiteral("auth_access_token")] = argToken;
    gamePlaceholders[QStringLiteral("auth_session")] = argToken;
    gamePlaceholders[QStringLiteral("user_type")] = argUserType;
    gamePlaceholders[QStringLiteral("version_type")] =
        versionJson.value(QStringLiteral("type")).toString(QStringLiteral("release"));
    // 「游戏窗口大小」那两个占位符。原版 json 里那条
    // `{"rules":[{"allow","features":{"has_custom_resolution":true}}],
    // "value":["--width","${resolution_width}","--height","${resolution_height}"]}`
    // 用的是这两个名字。注意：**不是**"让原版那条规则生效"的手段（理由见下面 --width
    // 那一段），而在当前代码里它俩用不上 —— 凡是带这两个占位符的条目，它前面
    // 那个开关一定是 `--width`/`--height`，而那两位在 isVanillaGameArgument() 表里，整条
    // （连同值）都会被 extraGameArguments 丢掉；占位符替换现在也挪到了过滤之后（见那边
    // 第 3 条规矩），丢掉的不再替换，所以连告警都不会有。
    // 留着它是**兜底**：将来若真有一条发 `${resolution_width}` 的路活下来，填了值总比把
    // `${...}` 原样交给游戏强（那种"参数凭空变形"正是 26.3 崩溃的成因）。
    if (windowWidth > 0 && windowHeight > 0) {
        gamePlaceholders[QStringLiteral("resolution_width")] = QString::number(windowWidth);
        gamePlaceholders[QStringLiteral("resolution_height")] = QString::number(windowHeight);
    }

    const QStringList extraGameArgs =
        VersionArguments::extraGameArguments(versionJson, args, gamePlaceholders);
    if (!extraGameArgs.isEmpty()) {
        qDebug() << "Extra game arguments from version.json:" << extraGameArgs;
        args << extraGameArgs;
    }

    // ------------------------------------------------------------------
    // 游戏窗口（设置页「游戏」栏 →「窗口模式」+「窗口大小」）
    // ------------------------------------------------------------------
    // NOTE: 原版 json 里明明有一条现成的 `--width ${resolution_width} --height
    // ${resolution_height}` 规则，但这里**不能靠它**，两处都是死的：
    // ① 那条规则挂在 `has_custom_resolution` 这个 feature 上，而启动器一律传
    // 空 feature 集合（见 VersionArguments::expand 的说明），规则根本不命中；
    // ② 就算把 feature 打开也没用 —— `--width`/`--height` 在
    // isVanillaGameArgument() 那张"原版参数"表里，extraGameArguments 会把它们
    // 连同值一起跳掉（那张表的语义是"这一条由启动器自己发"）。
    // 所以**打开 feature 是白做**，真正该做的事就是这一句：由启动器自己发，和上面
    // --username 那一段同源。顺带一提，1.12.2 那种老版本（minecraftArguments 里
    // 压根没有 width/height）走的也是这一句 —— 游戏自己的 main 从 1.6 起就认
    // `--width/--height`（1.12.2 的 Main 里那对 defaultsTo(854, 480) 就是默认窗口
    // 大小的出处），所以两条路发的是同一种参数。
    //
    // NOTE: 那个 `!args.contains("--width")` 是**兜底**，按现在这两条路它永远成立 ——
    // 别拿"老版本 json 会自带一对 width/height"当它的理由：老格式 json 里那一条
    // `--width ${resolution_width}` 已经被上面 ② 整条丢掉了（连同它的值），根本走不到
    // 这里。留着它的意义只是"这一对参数在列表里已经有人发过了，就别再发一遍"这个写法
    // 本身：真出现第二条会发它的路时，重复的选项会让 joptsimple 直接报错退出。
    //
    // NOTE: 上面那个兜底现在罩的是**整组**（连 `--fullscreen` 一起）。按分析它永远不成立，
    // 所以"罩整组"和"只罩 --width"在今天的代码里是同一件事；真要有一天 json 自己
    // 发了一对 --width，"全屏那一趟少一个 --fullscreen"这个后果是**可以接受的** ——
    // 整组参数本来就写着"这一条由启动器自己发"。
    if (!args.contains(QStringLiteral("--width"))) {
        const QStringList windowArgs = gameWindowArguments(window, screen);
        if (!windowArgs.isEmpty()) {
            args << windowArgs;
            qDebug() << "Game window args:" << windowArgs;
        }
    }

    // ------------------------------------------------------------------
    // 「开游戏直接进这个存档 / 直接连这台服务器」
    // ------------------------------------------------------------------
    // NOTE: 和上面那一段同源 —— 版本 json 自己声明了 `--quickPlaySingleplayer` /
    // `--quickPlayMultiplayer`（1.20 起），而那两位在
    // `isVanillaGameArgument()` 的名单上，所以上面 `extraGameArguments()` 把 json
    // 那一条连同它的 `${quickPlaySingleplayer}` 整条丢掉了。**该由谁发**这件事只有
    // 这一个答案：和 `--username` / `--width` 一样，启动器自己发。
    //
    // 判据（这个版本认不认这条参数）**不在这里** —— 它在界面那一层就拦住了
    // （`QuickPlay::supports()`，按钮在支持的版本上才露）。发一条不认它的版本不是
    // "忽略"，是游戏当场抛 UnrecognizedOptionException 退出，所以那道闸必须有；
    // 而"命令行的拼装"和"这一档该不该出现"是两件事，各归各的地方（同 GameWindowSettings
    // 那条规矩：LauncherManager 只问纯函数要参数、拼上去）。
    if (!quickPlay.isEmpty()) {
        const QStringList quickPlayArgs = QuickPlay::arguments(quickPlay);
        qDebug() << "Quick play args:" << quickPlayArgs;
        args << quickPlayArgs;
    }

    qDebug() << "Total arguments count:" << args.length();
    return args;
}

int LauncherManager::startGame(const QStringList &arguments, const EffectiveJavaSettings &settings,
                               const QString &versionId, const QString &displayName,
                               const QString &accountName, const QString &accountUuid,
                               ProcessPriority priority) {
    // 返回值 = 这次会话的 id（> 0 就等于"进程真的起来了"）。三条早退路径都必须 return 0：
    // 调用方拿非零当"这次算不算启动成功"的判据（据此记住上次启动的版本），任何一条漏掉
    // 都会把失败当成功记下来。这个函数里**每一条 return 都要想一遍返回值**。
    //
    // NOTE: 不能有"已经在跑了就拒绝"这条早退：多开是这个启动器明确要支持的事
    // （见类头的说明），拦在这儿等于把"同一个实例开两份""一个账号开两个实例"全堵死。
    if (m_nextSessionId <= 0) {
        // 跑满 21 亿次之后才会到这里，而那一下 0 是"失败"的哨兵值 —— 绕开它。
        m_nextSessionId = 1;
    }
    const int sessionId = m_nextSessionId++;

    SecretRedactor::rememberArguments(arguments);
    // 防手滑的阀门，不是性能上限（见头里的说明）。这一支**不登记会话**：它连一行日志
    // 都不该挂在某个实例名下（被拦下的是"又开一个"这件事本身，不是某个实例）。
    if (runningCount() >= maxConcurrentSessions()) {
        qWarning() << "Too many game sessions already running:" << runningCount();
        emit processError(0, tr("同时运行的游戏已经有 %1 个了（上限 %2），先关掉几个再启动。")
                                 .arg(runningCount())
                                 .arg(maxConcurrentSessions()));
        return 0;
    }

    // 会话**先登记再干活**：后面几条早退路径上的报错要能带上实例名（界面按 sessionId
    // 查显示名），所以这一笔必须在第一个 return 之前落下。失败时由 discardSession() 收回。
    Entry entry;
    entry.info = GameSession{sessionId, versionId,
                             displayName.isEmpty() ? versionId : displayName,
                             accountName, accountUuid, 0, 0, settings.xmx};
    m_sessions.insert(sessionId, entry);

    // 游戏进程的输出怎么解码，从**最终命令行**上反推（见下面那个静态函数的说明），
    // 而不是另立一个"我们加没加编码参数"的开关。正常情况下这条判据是自洽的：
    // generateJvmArguments() 只在 javaMajorVersion >= 19 时加 -Dstdout.encoding=utf-8，
    // Java 8-18 会忽略那两个属性、仍按本地代码页写字节，于是正确地回落到 fromLocal8Bit。
    //
    // NOTE: 判据是命令行本身，它看不见"这条是谁加的"。用户在全局/按版本的自定义参数里
    // 手写 -Dstdout.encoding=utf-8 时，Java 8-18 照样忽略它，而这里会判成 UTF-8，
    // 于是中文变乱码。这是**刻意接受**的：宁可跟着实际命令行走，也不要两处各记一份
    // 状态（那种失配更难查）。
    //
    // NOTE: 这一笔记在**会话自己**身上（entry.outputIsUtf8），不再是本类的一个成员 ——
    // 同时开着两份时它们可能用着不同的 Java 主版本，共用一份会导致其中一份乱码。
    const bool outputIsUtf8 = commandDeclaresUtf8Output(arguments);
    m_sessions[sessionId].outputIsUtf8 = outputIsUtf8;
    qDebug() << "Game output is UTF-8:" << outputIsUtf8 << "session" << sessionId;

    // javaPath 为空 = "用系统 PATH 上的那个 java"，这时候交给 QProcess 的应该是裸名字
    // （它自己会按 PATH 找）—— 但下面那道存在性预检是 `QFileInfo(program).exists()`，
    // 它把相对名字**按当前工作目录解**（CWD 是数据目录，那儿不会躺着 java.exe），于是这一
    // 支**永远**报「Java可执行文件未找到：java」：兜底路径一次都没成功过。
    //
    // NOTE: 判据要跟着"谁负责去找这个文件"走。裸名字的找法只有一个出处：
    // QStandardPaths::findExecutable()，和 GlobalJavaSettings 挑 java 时用的是同
    // 一个函数（那边 :264）。这里把解析结果直接写回 program，下面那道预检和
    // QProcess 拿到的就是同一个绝对路径。
    QString program = settings.javaPath;
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(QStringLiteral("java"));
        if (program.isEmpty()) {
            qWarning() << "Java executable not found on PATH and no path is configured.";
            emit processError(sessionId,
                              tr("没有找到 Java：设置里没有指定 Java 路径，系统 PATH 上也没"
                                 "有 java。请在「设置 → 启动 → Java 管理」里指定一份。"));
            discardSession(sessionId, /*abandoned=*/true);
            return 0;
        }
        qDebug() << "Java resolved from PATH:" << program;
    }

    // 记录启动信息
    qDebug() << "=== Game Launch Started ===";
    qDebug() << "Java executable:" << program;
    qDebug() << "JVM arguments count:" << arguments.length();

    // 检测 Java 可执行文件存在（仅限绝对路径）
    const QFileInfo javaInfo(program);
    if (!javaInfo.exists()) {
        qWarning() << "Java executable not found at:" << program;
        // 长路径这一档 `QFileInfo::exists()` 本身就看不穿（没开 LongPathsEnabled 时它
        // 走的就是 CreateFile 那一套），所以"没找到"可能只是路径太长。补一句，别让用户
        // 对着一条明明存在的 java.exe 反复核对。
        emit processError(sessionId, tr("Java可执行文件未找到：%1").arg(program) +
                                        ProcessPathLimit::tooLongHint({program}));
        discardSession(sessionId, /*abandoned=*/true);
        return 0;
    }
    qDebug() << "Java path verified:" << javaInfo.absoluteFilePath();

    // 记录部分重要参数
    for (int i = 0; i < arguments.length(); ++i) {
        const QString &arg = arguments.at(i);
        if (arg.startsWith("-D")) {
            // **带令牌的那一条只打钥匙和长度**。不能一律
            // `arg.left(50)`：`-Dminecraft.accessToken=<JWT>` 的前 50 个字符会跟着这
            // 一行写进 <数据目录>/logs/wtfml.log（这个进程把它整份都重定向到那个文件，
            // 见 main.cpp 的 installFileLog）。50 个字符不足以直接拿来用，但那是**凭据的
            // 一部分**，没有任何理由落盘。「Token length:」那行只打长度，不会泄露令牌。
            if (arg.startsWith(kAccessTokenProperty)) {
                qDebug() << "  " << kAccessTokenProperty + QStringLiteral("<省略，长度 %1>")
                                                  .arg(arg.length() - kAccessTokenProperty.length());
            } else {
                const QString safe = SecretRedactor::redact(arg);
                qDebug() << "  " << safe.left(50) << (safe.length() > 50 ? "..." : "");
            }
        } else if (arg == "-cp" || arg == "-classpath") {
            qDebug() << "  " << arg;
            if (static_cast<int>(i + 1) < arguments.length()) {
                ++i;
                qDebug() << "    " << SecretRedactor::redact(arguments.at(i));
            }
        }
    }

    // 每份会话一个 QProcess。父对象是 this，所以本类析构时它们会被一起收掉
    // （那条链正是 detachRunningGame() 要绕开的，见那边的说明）。
    auto *process = new QProcess(this);
    process->setProgram(program);
    process->setArguments(arguments);

    // 相对配置和模组资源必须在 --gameDir 下解析，不能让 FancyMenu/MCEF 读共享根中的文件。
    QString isolatedPath = AppPaths::versionDir(versionId);
    QDir().mkpath(isolatedPath);
    const int gameDirIndex = arguments.indexOf(QStringLiteral("--gameDir"));
    const QString workingDirectory = gameDirIndex >= 0 && gameDirIndex + 1 < arguments.size()
        ? arguments.at(gameDirIndex + 1) : isolatedPath;
    process->setWorkingDirectory(workingDirectory);
    qDebug() << "Working directory:" << workingDirectory;
    // 第一次启动这个版本时，让游戏界面语言跟随系统
    ensureInitialOptions(isolatedPath);
    qDebug() << "Isolated game directory:" << isolatedPath;

    // 三条信号接在**这一份会话自己**的进程上，用 lambda 把 sessionId 带上 ——
    // 不走 sender() 反查：那种写法在"进程刚结束、会话已经从表里摘掉"的那一瞬间
    // 查不到东西（而那一刻恰恰要读剩下的输出）。
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, sessionId]() { handleReadyReadStandardOutput(sessionId); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, sessionId]() { handleReadyReadStandardError(sessionId); });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, sessionId](int exitCode, QProcess::ExitStatus exitStatus) {
                handleProcessFinished(sessionId, exitCode, exitStatus);
            });
    m_sessions[sessionId].process = process;

    qDebug() << "Starting java process... (session" << sessionId << ")";
    process->start();
    if (!process->waitForStarted(3000)) {
        qWarning() << "Failed to start game process";
        qWarning() << "Error:" << process->errorString();
        // 这一句现在会**弹给用户看**（`LaunchController` 在 `startGame()`
        // 那一小段里接了一份 `processError`，见那边的说明），所以不能把 %1 原样递出去。
        // Windows 系统自己那几句消息是**格式串**（`FormatMessage` 给的是
        // 「%1 不是有效的 Win32 应用程序。」），Qt 的 errorString() 把它留在
        // 「Process failed to start: %1 …」里 —— 用户看到的会是一个像程序坏了才有的 %1。
        // 把 %1 换成那个可执行文件（系统那句话里的"主体"就是它），再去掉 Qt 那句英文
        // 前缀，剩下的就是系统自己一句完整的话。
        QString startReason = process->errorString();
        startReason.replace(QLatin1String("%1"), QDir::toNativeSeparators(program));
        const QString englishPrefix = QStringLiteral("Process failed to start: ");
        if (startReason.startsWith(englishPrefix)) {
            startReason.remove(0, englishPrefix.size());
        }
        emit processError(sessionId,
                          tr("无法启动游戏进程：%1").arg(startReason) +
                              ProcessPathLimit::tooLongHint({program, workingDirectory}));
        discardSession(sessionId, /*abandoned=*/true);
        return 0;
    }

    qDebug() << "Java process started, PID:" << process->processId();
    applyProcessPriority(process, priority);
    // 记"谁在跑"的**唯一**时机：进程真的起来了（三条早退路径都不记）—— 底栏那条状态和
    // "启动中"进度的收尾都读它。上一步 discardSession() 过的会话在这里已经不在表里了，
    // 所以下面这两笔只可能落在**活着**的会话上。
    m_sessions[sessionId].info.processId = static_cast<qint64>(process->processId());
    m_sessions[sessionId].info.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    emit gameStarted(sessionId);
    emit processOutput(sessionId, tr("游戏已启动（进程ID：%1）").arg(process->processId()));
    return sessionId;
}

QList<LauncherManager::GameSession> LauncherManager::runningGames() const {
    // 先按会话号收集：QHash 的遍历顺序不定，而"第几个在跑"这事得稳定 —— 底栏和日志里
    // 一次列好几个实例时，两次刷新换个顺序看着就像抖动。按号升序 = 按启动顺序。
    QList<int> ids;
    ids.reserve(m_sessions.size());
    for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        const Entry &entry = it.value();
        if (entry.process != nullptr && entry.process->state() != QProcess::NotRunning) {
            ids.append(it.key());
        }
    }
    std::sort(ids.begin(), ids.end());

    QList<GameSession> games;
    games.reserve(ids.size());
    for (int id : ids) {
        games.append(m_sessions.constFind(id)->info);
    }
    return games;
}

int LauncherManager::runningCount() const { return runningGames().size(); }

bool LauncherManager::hasRunningGame() const { return !runningGames().isEmpty(); }

bool LauncherManager::isVersionRunning(const QString &versionId) const {
    if (versionId.isEmpty()) {
        return false;
    }
    const QList<GameSession> games = runningGames();
    for (const GameSession &game : games) {
        if (game.versionId == versionId) {
            return true;
        }
    }
    return false;
}

LauncherManager::GameSession LauncherManager::session(int sessionId) const {
    const auto it = m_sessions.constFind(sessionId);
    return it == m_sessions.constEnd() ? GameSession() : it->info;
}

bool LauncherManager::stopGame(int sessionId) {
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd() || it->process == nullptr ||
        it->process->state() == QProcess::NotRunning) {
        return false;  // 已经结束了（或者刚被交出去）：没有可停的东西
    }
    qInfo() << "用户要求停止实例:" << it->info.displayName
            << "PID:" << it->info.processId;
    // kill() 就是 TerminateProcess，游戏来不及存档 —— 界面那边必须先把这件事说清楚
    // （见 MainWindow::confirmStopGame() 那个确认框）。这里不 waitForFinished()：
    // 结束那条路会走 handleProcessFinished，界面据此收状态，同步等在这里只会卡住界面。
    it->process->kill();
    return true;
}

void LauncherManager::detachRunningGame() {
    const QList<GameSession> games = runningGames();
    if (games.isEmpty()) {
        return;  // 没在跑（或者已经交出去过）：什么都不做，也不留日志
    }
    for (const GameSession &game : games) {
        const auto it = m_sessions.find(game.sessionId);
        if (it == m_sessions.end() || it->process == nullptr) {
            continue;
        }
        qInfo() << "游戏还在跑：把进程交出去，关掉启动器不会把它杀掉。实例:"
                << game.displayName << "PID:" << game.processId;
        // 先断信号：交出去之后 handleProcessFinished 之类的槽不该再被调到（那时界面已经在拆了）。
        it->process->disconnect();
        // 脱离 this 的所有权 —— 这才是关键那一步：析构链（MainWindow 是 main() 里的栈对象）
        // 走到这里时，这些 QProcess 已经不是它的子对象，那套"进程还在跑就
        // kill()+waitForFinished()"的析构逻辑**跑不到**，java.exe 自己继续活在系统里。
        it->process->setParent(nullptr);
        // NOTE: 这些对象是**故意不释放**的（它们已经没人管了，泄漏的就是几个 QProcess 壳子，
        // 进程退出时由系统回收）。释放它们就等于执行析构、等于把游戏强杀 —— 那正是这
        // 一步要避免的事。指针也不置空：留着活着的对象比留一个会空指针解引用的洞安全
        // （这条路只在退出前走一次）。清表只是本类不再认它们。
    }
    m_sessions.clear();
}

bool LauncherManager::commandDeclaresUtf8Output(const QStringList &arguments) {
    bool isUtf8 = false;
    for (const QString &arg : arguments) {
        if (arg.startsWith(QStringLiteral("-Dstdout.encoding="))) {
            // 同一条属性出现多次时 JVM 取最后一份，这里跟着取最后一份。
            const QString encoding = arg.mid(arg.indexOf(QLatin1Char('=')) + 1);
            isUtf8 = (encoding.compare(QStringLiteral("utf-8"), Qt::CaseInsensitive) == 0);
        }
    }
    return isUtf8;
}

QString LauncherManager::decodeGameOutput(bool utf8, std::shared_ptr<QStringDecoder> &slot,
                                          const QByteArray &bytes) {
    // 判据来自 startGame() 从命令行反推出来、记在**那份会话自己**身上的 outputIsUtf8。
    //
    // **一块一块地解**，不是一个字节串换一次。无状态的
    // `fromUtf8 / fromLocal8Bit` 不行：调用方拿到的是 `readyRead` 给的一段段字节 ——
    // 读边界正好落在一个汉字的两个字节中间时（Java ≤18 走本地代码页 = 中国区 GBK，
    // 两字节一个汉字，管道一满就切），前半截和后半截各自解成一个替换字符，日志窗里
    // 零星多出乱码字。QStringDecoder 把末尾那半个字符留在自己的状态里，下一段接着拼。
    //
    // NOTE: 只给游戏进程用这一份。GlobalJavaSettings::runProcess 和 ZipUtil 里的
    // fromLocal8Bit 不能跟着改 —— 它们解的是 PowerShell / java -version 的输出，
    // 前者根本不是 JVM、不认 -Dstdout.encoding（那是**整段读完再解**，不存在切在
    // 半个字中间的问题），改了才是引入乱码。
    if (!slot) {
        // 第一次用到这一条流：按这一份的命令行判出来的编码建一个。
        slot = std::make_shared<QStringDecoder>(
            utf8 ? QStringDecoder::Utf8 : QStringDecoder::System);
    }
    if (slot->isValid()) {
        return (*slot)(bytes);
    }
    // 编码名建不出来（理论上到不了这一步：Utf8 / System 都是内置的）。退回无状态那条路，
    // 至少不会一个字都不显示。
    return utf8 ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

void LauncherManager::discardSession(int sessionId, bool abandoned) {
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) {
        return;
    }
    QProcess *process = it->process;
    m_sessions.erase(it);
    if (process != nullptr) {
        // 先断信号再删：deleteLater() 是"回到事件循环再释放"，这中间那个进程对象还活着，
        // 万一它又吐一次 readyRead（结束前最后那几行）就会打到已经摘掉的会话上。
        process->disconnect();
        process->deleteLater();
    }
    if (!abandoned) {
        return;
    }
    // `startGame()` 的三条早退路径（找不到 Java / 路径不对 / 进程起不来）
    // 会先带号发一句 `processError`，再把这一号会话收回。那一句本身是对的（用户要看到
    // 原因），但它走的是**和游戏 stderr 同一条槽** —— 界面可能已经替它建了一扇游戏
    // 日志窗、记了一笔按号归档的底账 —— 而从此以后再没有任何一条信号会提到这个号：
    // 那扇窗不会自己关（自动关窗只发生在"正常以 0 退出"那条路上），底账也不会被删。
    // 发这一条，让界面把那一份收干净。
    // NOTE: 摆在 erase **之后**：收信号的槽里不许再查到一份已经不存在的会话（和
    // gameFinished 同一条规矩），要用的东西自己在收到报错那一刻就取走。
    emit sessionDiscarded(sessionId);
}

void LauncherManager::handleReadyReadStandardOutput(int sessionId) {
    // NOTE: find() 不是 constFind() —— 解码器是会话自己的一份状态，解一段就动一次。
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || it->process == nullptr) {
        return;
    }
    // 这一份**不许** `.trimmed()`。往下传的文本是**按行切**的：接的那一位
    // （GameLogWindow::appendText）把"还没等到 `\n` 的那半行"留在 m_pending 里等下一块来拼，
    // 而 `.trimmed()` 削掉的**正是每一块结尾那个 `\n`** —— 于是"已经写完的最后一行"被当成
    // 半行挂住，和下一块的头一行粘成 `CD` 这种**伪行**（级别颜色跟着认错，导出的日志也一样
    // 是错的），每个分块边界中一次。空块本来就由下面那句 isEmpty() 拦住。
    const QString output = it->stdoutSecrets.push(decodeGameOutput(it->outputIsUtf8, it->stdoutDecoder,
                                            it->process->readAllStandardOutput()));
    if (!output.isEmpty()) {
        qDebug().noquote() << "[Game Output]" << it->info.displayName << output;
        emit processOutput(sessionId, output);
    }
}

void LauncherManager::handleReadyReadStandardError(int sessionId) {
    const auto it = m_sessions.find(sessionId);  // 同上：要动解码器状态
    if (it == m_sessions.end() || it->process == nullptr) {
        return;
    }
    // 和上面 stdout 那一支逐字同一个理由：**不 trim**（那个 `\n` 是行与行之间的分界）。
    const QString error = it->stderrSecrets.push(decodeGameOutput(it->outputIsUtf8, it->stderrDecoder,
                                           it->process->readAllStandardError()));
    if (!error.isEmpty()) {
        qWarning().noquote() << "[Game Error]" << it->info.displayName << error;
        emit processError(sessionId, error);
    }
}

void LauncherManager::handleProcessFinished(int sessionId, int exitCode,
                                            QProcess::ExitStatus exitStatus) {
    const auto it = m_sessions.find(sessionId);  // 最后那几段也要走会话自己的解码器
    if (it == m_sessions.end() || it->process == nullptr) {
        return;  // 已经被交出去过（detach）或者收掉了：这一份没有任何可报的
    }
    QProcess *process = it->process;
    const bool outputIsUtf8 = it->outputIsUtf8;
    const QString displayName = it->info.displayName;

    qDebug() << "=== Game Process Finished ===";
    qDebug() << "Session:" << sessionId << "Instance:" << displayName;
    qDebug() << "Exit code:" << exitCode;
    qDebug() << "Exit status:" << (exitStatus == QProcess::NormalExit ? "Normal" : "Crash");

    // 检查剩余的输出
    const QByteArray remainingOutput = process->readAllStandardOutput();
    const QByteArray remainingError = process->readAllStandardError();

    {
        // NOTE: 走**同一个解码器**（不是新起一个）：这是同一条流的最后一段，前面那一块
        // 可能正停在半个汉字上 —— 新起一个的话那半个字永远补不回来。
        // 同样**不 trim**（见 handleReadyReadStandardOutput 里那段 ★）：这一份是整条流的
        // 最后一段，它结尾那个 `\n` 就是最后一行"写完了"的凭据。
        const QString output =
            it->stdoutSecrets.push(decodeGameOutput(outputIsUtf8, it->stdoutDecoder, remainingOutput), true);
        if (!output.isEmpty()) {
            qDebug() << "Remaining output:" << output;
            emit processOutput(sessionId, output);
        }
    }

    {
        const QString error =
            it->stderrSecrets.push(decodeGameOutput(outputIsUtf8, it->stderrDecoder, remainingError), true);
        if (!error.isEmpty()) {
            qDebug() << "Remaining error:" << error;
            emit processError(sessionId, error);
        }
    }

    // 摘在 emit 之前：接 gameFinished 的槽会去读 runningGames() 刷新底栏，反过来的话
    // 它会读到"刚退出的那一份还在跑"。
    // NOTE: **不**带 abandoned —— 这一份是真跑完了，收尾归 gameFinished 那一路（界面靠它
    // 决定日志窗关不关、崩溃那几行留不留），绝不能在这里再发 sessionDiscarded 抢先把
    // 那一份底账清掉。
    discardSession(sessionId);
    emit gameFinished(sessionId, exitCode, exitStatus);
}

