/**
 * @file GlobalJavaSettings.cpp
 * @brief 实现 Java 路径、版本选择、内存与运行时检测设置。
 * @note 持久化键和内存自动分配规则见 .hpp。
 */
#include "GlobalJavaSettings.hpp"
#include <QCoreApplication>

#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QSaveFile>
#include "util/PowerShell.hpp"
#include <QProcess>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>

#include "../net/ControlGet.hpp"
#include "../util/AppPaths.hpp"
#include "../util/SettingsSyncRetry.hpp"
#include "../util/WriteDirectoryGuard.hpp"

namespace {
static constexpr const char kJavaPath[] = "Java/Path";
static constexpr const char kXmx[] = "Java/Xmx";
static constexpr const char kXms[] = "Java/Xms";
static constexpr const char kExtraArgs[] = "Java/ExtraArgs";
/// 设置界面「Java 版本」那一项：true = 自动（按版本要求挑），false = 手动钉死。
static constexpr const char kAutoSelect[] = "Java/AutoSelect";
/// 「内存分配」是不是自动档（按系统内存算）。老配置没有这个键，见 load() 的处理。
static constexpr const char kMemoryAuto[] = "Java/MemoryAuto";
/// 上次开启自动分配之前的那一对手动值（切回手动档时恢复它们）。
static constexpr const char kManualXmx[] = "Java/ManualXmx";
static constexpr const char kManualXms[] = "Java/ManualXms";
/// 实例指定的 Java 与版本要求不符时要不要提醒。老配置没有这个键 = 提醒（默认值）。
static constexpr const char kWarnVersionMismatch[] = "Java/WarnVersionMismatch";

/// 内置默认内存（构造函数里的初值）。自动档算不出结果时的兜底值也是它，
/// load() 判断"用户自己调过没有"时还是拿它当基准 —— 一共就这一处定义。
static constexpr int kBuiltinXmxMb = 4096;
static constexpr int kBuiltinXmsMb = 1024;

/// 自动档的取值范围与步长。
///
/// 步长和界面那两个数字框**是**一份：MainWindowUI::memoryStepMb() 也是 256（数字框里的值
/// 必须是它的整数倍，否则用户会看到一个 3071）。
/// NOTE: **上限不是**。自动档自己封在 8 GB（那是个刻意的天花板：自动算出来的数不该比
/// 用户手动能填的更激进），而界面手动档的上限是 MainWindowUI::memoryLimitMaxMb()
/// = 65536。
static constexpr int kAutoXmxMinMb = 1024;
static constexpr int kAutoXmxMaxMb = 8192;
static constexpr int kAutoXmsMinMb = 512;
static constexpr int kAutoXmsMaxMb = 8192;
static constexpr int kAutoStepMb = 256;

/// 「这个实例装了多少模组」那一档的加成（见 suggestXmxMb 的 modCount 参数）。
///
/// 为什么要它：光看机器内存算出来的值对原版够用，对两百个模组的整合包是不够的 ——
/// 同一个数字在两台机器上服务的是**两件不同的事**。只按机器内存算的话，
/// "原版和 200 模组拿到一样的建议值"。
///
/// 三个数都是**加在 total/4 那一步上的**，所以上面那两条机器侧的硬顶（available/2、
/// total/2）和 [1024, 8192] 那个体面范围**照旧说了算** —— 加成只抬"目标值"，
/// 抬不过这台机器的物理现实。这也是它不设成"直接返回一个定值"的理由。
static constexpr int kAutoXmxModsPerStep = 64;   ///< 多少模组算一档
static constexpr int kAutoXmxMbPerModStep = 512; ///< 每一档加多少 MB
static constexpr int kAutoXmxModBonusMaxMb = 4096; ///< 加成封顶（够了：那是 512 个模组）

/// 模组数量换算成 Xmx 的加成（MB）。纯函数，可单独钉。
int modCountBonusMb(int modCount) {
    if (modCount <= 0) {
        // 负数和 0 都当"没有模组"。负数不是正常输入，但拿它去算减法是更坏的答案。
        return 0;
    }
    const qint64 steps = modCount / kAutoXmxModsPerStep;
    return static_cast<int>(qMin<qint64>(steps * kAutoXmxMbPerModStep, kAutoXmxModBonusMaxMb));
}

/// 向下取整到步长的整数倍。
int roundDownToStep(qint64 valueMb) {
    return static_cast<int>(valueMb / kAutoStepMb * kAutoStepMb);
}

bool runProcess(const QString &program, const QStringList &arguments, QString *stdOut = nullptr,
                QString *stdErr = nullptr, int timeoutMs = 5000) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(timeoutMs)) {
        // **超时必须收尸**。这里以前直接 `return false`，于是那个 QProcess（栈对象）析构时
        // 子进程还活着 —— Qt 会打 `QProcess: Destroyed while process (...) is still running.`
        // 并把进程留在系统里：CI 上探到两个 JDK，卡住的 `java.exe` 越积越多（作业末尾还得
        // `Terminate orphan process`），下一轮 `java -version` 更慢，恶性循环。
        // 本函数只用来问"这一份 Java 是什么版本"，超时就说明它答不上来 —— 杀掉是正确语义。
        process.kill();
        process.waitForFinished(2000);
        return false;
    }

    // NOTE: 这里刻意用 fromLocal8Bit、**不改成 fromUtf8**。
    // 走这条路的两种调用（java -version、PowerShell）都不受 -Dstdout.encoding 约束
    // —— 那个属性是 JVM 的参数，PowerShell 不是 JVM、不认它。跟着改才是引入乱码。
    if (stdOut) {
        *stdOut = QString::fromLocal8Bit(process.readAllStandardOutput());
    }
    if (stdErr) {
        *stdErr = QString::fromLocal8Bit(process.readAllStandardError());
    }
    // NOTE: 退出码也要看。只看 exitStatus 的话，PowerShell 自己正常退出、但它那条命令
    // 失败（exitCode != 0，比如 Expand-Archive 解不开那个 zip）会被当成功 ——
    // 调用方接着去找 java.exe，找不到，报出来的是"找不到也下载不到 Java"，
    // 而真正的原因（解压失败）一个字都看不见。
    // `java -version` 成功时退出码是 0，另一个调用方不受影响。
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool verifyJavaExecutable(const QString &path, QString *versionOut = nullptr) {
    if (path.isEmpty()) {
        return false;
    }

    QString execPath = path;
    QFileInfo info(execPath);
    if (info.isDir()) {
        execPath = QDir(execPath).absoluteFilePath("bin/java.exe");
    }

    if (!QFileInfo(execPath).exists()) {
        return false;
    }

    QString stderrOutput;
    if (!runProcess(execPath, {"-version"}, nullptr, &stderrOutput, 5000)) {
        return false;
    }

    if (stderrOutput.contains("version")) {
        if (versionOut) {
            *versionOut = stderrOutput;
        }
        return true;
    }
    return false;
}

/// 从**裸版本号**（"25.0.1" / "1.8.0_422"）推主版本。
/// "java -version 的输出"和"release 文件里的 JAVA_VERSION"两条路共用这一段，
/// 免得两处各判一次"1.8 该取第二段"、慢慢就判歪一个。
int majorFromVersionString(const QString &version) {
    // NOTE: 这个函数只把**裸版本号**解析成主版本，和"哪个游戏版本要多新的 Java"无关。
    // 那张对照表（1.6-1.16.5 → 8、**1.17 → 16**、1.18-1.20.4 → 17、
    // 1.20.5-1.21.11 → 21、26.1+ → 25）不在这里 —— 实际来源是 version.json 的
    // javaVersion.majorVersion，由 MainWindow 读取（另有一处兜底值）。
    const QStringList parts = version.split(QLatin1Char('.'));
    if (parts.isEmpty()) {
        return -1;
    }

    bool ok = false;
    int majorVersion = parts[0].toInt(&ok);
    if (!ok) {
        return -1;
    }

    // For Java 8 and earlier, version is "1.8.x" so major version is the second part
    if (majorVersion == 1 && parts.size() > 1) {
        majorVersion = parts[1].toInt(&ok);
        if (!ok) {
            return -1;
        }
    }

    return majorVersion;
}

int parseJavaMajorVersion(const QString &versionOutput) {
    // Java version output examples:
    // java version "1.8.0_301" -> 8
    // java version "11.0.12" -> 11
    // java version "17.0.1" -> 17
    // java version "21.0.1" -> 21

    const QRegularExpression regex(QStringLiteral("version \"([^\"]+)\""));
    const QRegularExpressionMatch match = regex.match(versionOutput);
    if (!match.hasMatch()) {
        return -1;
    }
    return majorFromVersionString(match.captured(1));
}

/**
 * @brief 判断某个 Java 可执行文件的主版本是否与需求一致。
 * @param path Java 可执行文件或包含 bin/java.exe 的运行时目录。
 * @param major 需要的 Java 主版本（如 8 / 17 / 21 / 25），传 -1 表示不限版本。
 * @return 若路径有效且版本匹配，则返回 true.
 *
 * NOTE: 环境变量、PATH、常见安装目录里找到的 Java 都必须先过这一关才能采用。
 * 否则会静默选中错误的 Java（例如用 Java 21 去启动需要 Java 25 的 26.x，
 * 最终表现为 UnsupportedClassVersionError 而不是一条能看懂的提示）。
 */
bool javaMatchesMajor(const QString &path, int major) {
    if (path.isEmpty()) {
        return false;
    }
    QString versionOutput;
    if (!verifyJavaExecutable(path, &versionOutput)) {
        return false;
    }
    return major < 0 || parseJavaMajorVersion(versionOutput) == major;
}

/**
 * @brief 在 JAVA_HOME / JDK_HOME 指向的运行时里查找 Java。
 * @param requiredMajor 需要的 Java 主版本，-1 表示不限。
 * @return 若找到匹配的 java.exe 则返回其路径，否则返回空.
 */
QString findJavaInEnvironment(int requiredMajor = -1) {
    const QStringList homeVars = {qEnvironmentVariable("JAVA_HOME"),
                                  qEnvironmentVariable("JDK_HOME")};
    for (const QString &home : homeVars) {
        if (home.isEmpty()) {
            continue;
        }
        const QString candidate = QDir(home).absoluteFilePath("bin/java.exe");
        if (javaMatchesMajor(candidate, requiredMajor)) {
            return candidate;
        }
    }
    return QString();
}

/**
 * @brief 在 Program Files 下递归查找常见发行版的 Java。
 * @param requiredMajor 需要的 Java 主版本，-1 表示不限。
 * @return 若找到匹配的 java.exe 则返回其路径，否则返回空.
 *
 * NOTE: 这里会遍历整个 Program Files，比较慢，因此只在更便宜的候选源
 * （已配置路径、本地运行时、环境变量、PATH）都没命中时才会走到。
 *
 * NOTE: 这一路**只留一道时间预算**（kMaxMs）。它跑在界面线程上，递归扫的是一棵我们控制
 * 不了深浅的树；只要有一处成环的重解析点、几万个条目，或者一个爬不动的目录，这个循环就
 * 会一直转下去 —— 2026-11-18 ci-core 那次就卡在这儿十几分钟（`=== E. Java 运行时选择`
 * 之后一个字都没有，直到阶段的 900 s 上限把进程杀掉）。
 *
 * NOTE: 迭代器**不带 nameFilter**（不写 `java.exe`）。带过滤时 `next()` 要扫到下一个
 * java.exe 才返回，中间几万个目录全在那一次调用里过去，预算检查根本轮不到 —— 而"目录树
 * 太大 / 爬不动"恰恰是最主要的卡法。现在每取一条目录项之前都查一次预算，名字在这里自己
 * 比（`QDirIterator` 在 `QDir::Files` 下仍会递归所有子目录，只是不把目录本身交出来）。
 *
 * NOTE: 20 s 只是"永远卡住"的上限。正常机器扫完远碰不到它（本机三个根扫完 < 1 s），所以
 * 磁盘慢、机器上装了好几个 Java 都不会因此被提前放弃；真扫满 20 s 才当"没找到"，调用方
 * 照常走"本机没有这个 Java"那条路（报错，或者去下载）。
 *
 * 限制：预算是在两条目录项之间检查的，单个 `QDirIterator::next()` 内部被卡住（网络盘、
 * 挂起的重解析点）时这里看不到 —— 那种情况得把搜索挪到工作线程，靠这个循环拦不住。
 */
QString findJavaInCommonLocations(int requiredMajor = -1) {
    constexpr qint64 kMaxMs = 20000;  ///< 整个兜底扫描的耗时上限（三个根目录共用）

    const QStringList rootDirs = {qEnvironmentVariable("ProgramFiles"),
                                  qEnvironmentVariable("ProgramFiles(x86)"),
                                  qEnvironmentVariable("ProgramW6432")};

    // 三个根目录共用一份预算：Program Files 和 Program Files (x86) 经常指到同一棵树。
    QElapsedTimer clock;
    clock.start();
    const auto giveUp = [&] {
        qWarning() << "扫描 Program Files 找 Java 用了" << clock.elapsed()
                   << "ms 还没扫完，放弃这条兜底路径";
        return QString();
    };

    for (const QString &rootDirPath : rootDirs) {
        if (rootDirPath.isEmpty()) {
            continue;
        }
        QDir rootDir(rootDirPath);
        if (!rootDir.exists()) {
            continue;
        }

        QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            // 先查预算再取值：不带 nameFilter 这一条才有意义（见函数头那条 NOTE）。
            if (clock.elapsed() > kMaxMs) {
                return giveUp();
            }
            const QString candidate = it.next();
            if (it.fileName().compare(QStringLiteral("java.exe"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (candidate.contains("Java", Qt::CaseInsensitive) ||
                candidate.contains("jdk", Qt::CaseInsensitive) ||
                candidate.contains("jre", Qt::CaseInsensitive) ||
                candidate.contains("temurin", Qt::CaseInsensitive) ||
                candidate.contains("adoptium", Qt::CaseInsensitive) ||
                candidate.contains("corretto", Qt::CaseInsensitive) ||
                candidate.contains("microsoft", Qt::CaseInsensitive) ||
                candidate.contains("zulu", Qt::CaseInsensitive)) {
                if (javaMatchesMajor(candidate, requiredMajor)) {
                    return candidate;
                }
            }
        }
    }

    return QString();
}

/**
 * @brief 查找 PATH 上的 java。
 * @param requiredMajor 需要的 Java 主版本，-1 表示不限。
 * @return 若找到匹配的 java.exe 则返回其绝对路径，否则返回空.
 *
 * NOTE: 为什么必须解析成**绝对路径**（别"简化"回去）：裸命令过不了 verifyJavaExecutable() 第一步的
 * QFileInfo::exists()；即使侥幸通过，startGame() 也会用 QFileInfo(program).exists()
 * 判定成"Java 可执行文件未找到"。
 */
QString findJavaOnPath(int requiredMajor = -1) {
    const QString resolved = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!resolved.isEmpty() && javaMatchesMajor(resolved, requiredMajor)) {
        return resolved;
    }
    return QString();
}

/// 原版启动器下载好的一个运行时。主版本**在扫描时就认出来带着走** —— 调用方
/// （scanLocalJavaVersions）要用它给 versions 建索引，重跑一遍 java -version
/// 就是让界面白冻一倍时间。
struct VanillaRuntime {
    QString exe;
    int majorVersion = -1;
};

/**
 * @brief 扫描原版启动器已经下载好的运行时目录。
 *
 * 原版启动器会把 Mojang 的运行时解压到
 * %APPDATA%/.minecraft/runtime/<component>/，目录名与 version.json 里的
 * javaVersion.component 一一对应（jre-legacy / java-runtime-alpha / beta /
 * gamma / delta / epsilon）。直接复用这些运行时可以省掉一次完整的下载，
 * 而且它们是 Mojang 针对该游戏版本挑选过的。
 *
 * @return 组件名到该运行时（路径 + 主版本）的映射。认不出主版本的组件不入表 ——
 * 它对调用方没有意义，留着只会让调用方再过滤一遍。
 */
QMap<QString, VanillaRuntime> scanVanillaRuntimeComponents() {
    QMap<QString, VanillaRuntime> components;

    QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) {
        appData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    if (appData.isEmpty()) {
        return components;
    }

    QDir runtimeRoot(QDir(appData).absoluteFilePath(".minecraft/runtime"));
    if (!runtimeRoot.exists()) {
        return components;
    }

    // 只收认得出主版本的那些：majorVersion <= 0 的运行时对调用方没有意义，
    // 留在表里还要调用方自己再过滤一遍。
    const QStringList subdirs = runtimeRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &subdir : subdirs) {
        const QString exe =
            QDir(runtimeRoot.absoluteFilePath(subdir)).absoluteFilePath("bin/java.exe");
        const int majorVersion = GlobalJavaSettings::probeJavaMajorVersion(exe);
        if (majorVersion > 0) {
            components[subdir] = VanillaRuntime{exe, majorVersion};
        }
    }
    return components;
}

/// 把一个值放进 PowerShell 的**单引号**字符串里：单引号在单引号串里靠写两个来转义。
/// NOTE: 两个路径都要过这一道。原样塞进 `'%1'` 的话，路径里只要有一个 `'`
/// （数据目录或 exe 放在这种目录下），那条命令就不是我们想发的那条了 ——
/// PowerShell 会把它切成别的参数（甚至别的命令），而这里只看"解压成功没有"。
QString psSingleQuoted(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

bool extractZipWithPowerShell(const QString &zipFile, const QString &targetDir) {
    // `-LiteralPath`：`-Path` 认通配符，路径里带个 `[` / `*` 就会被当成模式去展开
    // （数据目录在 `[备份]` 这种名字下面时就解不开，甚至解到别的文件上）。
    QString command = QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
                          .arg(psSingleQuoted(zipFile), psSingleQuoted(targetDir));
    QString stdOut, stdErr;
    bool ok = runProcess(PowerShell::executable(),
                         {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), command},
                         &stdOut, &stdErr, 60000);
    if (!ok) {
        // 解压失败必须留下**为什么**（Expand-Archive 会把它说得很清楚：空间不够、
        // 压缩包坏了、路径太长……）。不读 stderr 的话，用户看到的只有
        // 后面那句"找不到也下载不到 Java"。
        qWarning().noquote() << "解压 Java 运行时失败：" << stdErr.trimmed()
                             << QCoreApplication::translate("GlobalJavaSettings", "（zip:") << zipFile << QCoreApplication::translate("GlobalJavaSettings", "）");
    }
    Q_UNUSED(stdOut);
    return ok;
}

QString findExtractedJava(const QString &rootDir) {
    QDirIterator it(rootDir, QStringList() << "java.exe", QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString candidate = it.next();
        if (verifyJavaExecutable(candidate)) {
            return candidate;
        }
    }
    return QString();
}

/**
 * @brief 若目录下只有一个子目录且没有别的文件，则把该子目录的内容上移一层。
 * @param dir 待压平的目录。
 * @return 若发生了移动则返回 true.
 *
 * 所有 Java 运行时压缩包都自带一层以版本号命名的根目录（如 jdk-21.0.4+7-jre/），
 * 压平一层之后布局才会稳定成 <base>/<component>/bin/java.exe，
 * 与 scanLocalJavaVersions() 的目录约定一致。
 */
bool flattenSingleRootDir(const QString &dir) {
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(dir, &lockProblem)) return false;
    QDir root(dir);
    if (!root.exists()) {
        return false;
    }

    const QStringList entries = root.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    if (entries.size() != 1) {
        return false;
    }

    QDir onlyChild(root.absoluteFilePath(entries.first()));
    if (!onlyChild.exists()) {
        return false;  // 唯一项是个文件，没什么可压平的
    }

    const QStringList children =
        onlyChild.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QStringList moved;
    moved.reserve(children.size());
    for (const QString &child : children) {
        const QString target = root.absoluteFilePath(child);
        if (QFileInfo::exists(target)) {
            for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
                QDir().rename(root.absoluteFilePath(*it), onlyChild.absoluteFilePath(*it));
            }
            return false;
        }
        if (!QDir().rename(onlyChild.absoluteFilePath(child), target)) {
            // 移动失败时保留源目录；继续递归删除会让尚未移动的 Java 文件直接丢失。
            for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
                QDir().rename(root.absoluteFilePath(*it), onlyChild.absoluteFilePath(*it));
            }
            return false;
        }
        moved.append(child);
    }
    return QDir().rmdir(onlyChild.absolutePath());
}

QString userAgent() {
    const QString version = QCoreApplication::applicationVersion();
    return QStringLiteral("WTFML/") + (version.isEmpty() ? QStringLiteral("dev") : version);
}

/// 合作式取消位读一次（空 = 不可取消）。
bool cancelRequested(const std::atomic_bool *cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool downloadFileToLocal(const QUrl &url, const QString &destinationPath,
                         const std::function<void(qint64 done, qint64 total)> &progress = {},
                         const std::atomic_bool *cancel = nullptr) {
    if (cancelRequested(cancel)) {
        return false;
    }
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(QFileInfo(destinationPath).absolutePath(), &lockProblem))
        return false;
    QSaveFile file(destinationPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // NOTE: 这个超时**不能省**。这一路在事件循环里等待写盘；没有超时的话，
    // 对端连上了却不发数据时 loop.exec() 没有任何退出条件。
    // 它是"不活动超时"，一直匀速在下的不会因此被切断。
    // 取 30 秒和下载管理器那条约定一致。
    // NOTE: 这一发**不进** `RequestBudget::ControlGet`：它是几十到两百兆的运行时压缩包，
    // 属于 Bulk 那一档的量级，不是"问一句就完事"的元数据。
    request.setTransferTimeout(30000);
    if (progress) {
        progress(0, 0);
    }
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    bool writeFailed = false;
    // 用户在任务中心按了「取消」：**合作式**停在下一个数据块上。这一步不会强杀线程
    // （那会留下半截文件），而是 abort 回复、让 QSaveFile::cancelWriting() 把写到一半的
    // 临时文件删掉，调用方再收拾解压目录。
    bool cancelled = false;
    QElapsedTimer progressTimer;
    progressTimer.start();
    if (progress) {
        QObject::connect(reply, &QNetworkReply::downloadProgress,
                         [&](qint64 bytesReceived, qint64 bytesTotal) {
                             if (cancelRequested(cancel)) {
                                 cancelled = true;
                                 reply->abort();
                                 return;
                             }
                             if (bytesTotal <= 0) {
                                 if (progressTimer.elapsed() >= 150) {
                                     progressTimer.restart();
                                     progress(0, 0);
                                 }
                                 return;
                             }
                             if (bytesReceived >= bytesTotal || progressTimer.elapsed() >= 150) {
                                 progressTimer.restart();
                                 progress(bytesReceived, bytesTotal);
                             }
                         });
    }
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        if (cancelRequested(cancel)) {
            cancelled = true;
            reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty() && file.write(chunk) != chunk.size()) {
            writeFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    // 取消只查一次这一位：`abort()` 之后 finished 会带着 OperationCanceledError 回来，
    // 下面那条 `reply->error() != NoError` 分不出"用户取消"和"网络断了" —— 账本那边
    // 要的正是这个区别（取消不是失败）。所以在这里先认下来。
    cancelled = cancelled || cancelRequested(cancel);

    // finished 前通常已经发过最后一次 readyRead；仍然把尾部读尽，避免依赖信号时序。
    // 已取消的那一份不再往盘上写（尾部也属于"半截文件"）。
    if (!cancelled) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty() && file.write(tail) != tail.size()) {
            writeFailed = true;
        }
    }

    if (cancelled) {
        qInfo().noquote() << "Java runtime download cancelled:" << url.toString();
        // cancelWriting() 会把 QSaveFile 那个临时文件删掉：盘上不留半截压缩包。
        file.cancelWriting();
        reply->deleteLater();
        return false;
    }

    if (reply->error() != QNetworkReply::NoError || writeFailed) {
        // 下载失败要留下**为什么**：这条链路以前只报 "Failed to download ZIP file"，
        // 分不清是超时、代理、TLS 还是 404。
        qWarning().noquote() << "Java runtime download failed:" << reply->errorString()
                             << url.toString();
        file.cancelWriting();
        reply->deleteLater();
        return false;
    }

    if (!file.commit()) {
        reply->deleteLater();
        return false;
    }
    reply->deleteLater();
    return true;
}

/**
 * @brief 按 version.json 里声明的 component 名下载对应的 Java 运行时。
 * @param destinationDir 运行时根目录（通常是 <.minecraft>/java）。
 * @param component Mojang 的运行时组件名，如 java-runtime-epsilon。
 * @param javaPathOut 解压后找到的 java.exe 路径。
 * @return 若下载、解压**并且**在解压结果里找到一个能跑的 java.exe 才返回 true。
 * 解压成功但没找到 java.exe 的会继续试下一个 URL，全都如此就返回 false。
 */
namespace {
/// 从一份"整包 zip"的 JDK 装到 dir 里，解压后必须能跑且主版本对得上。
///
/// 存在的理由：Adoptium API 在很多国内网络下连不上（`Connection closed`），而同一台机器上
/// Microsoft OpenJDK 的下载链接（aka.ms）通常可用。安装布局保持和 Adoptium 那条一致：
/// `java/<major>/bin/java.exe`，扫描本地 Java 时不用为来源分叉。
bool installZipRuntime(const QUrl &url, const QString &dir, int expectMajor, QString *javaPathOut,
                       const std::function<void(qint64 done, qint64 total)> &progress = {},
                       const std::atomic_bool *cancel = nullptr) {
    if (!url.isValid() || dir.isEmpty() || cancelRequested(cancel)) return false;
    {
        QString lockProblem;
        if (!WriteDirectoryGuard::acquire(dir, &lockProblem)) return false;
    }
    if (!QDir().mkpath(dir)) return false;
    const QString tempZip = QDir(dir).absoluteFilePath(QStringLiteral("java_runtime.zip"));
    if (!downloadFileToLocal(url, tempZip, progress, cancel)) {
        if (progress) progress(0, 0);
        // 取消：这一趟没装成，**不留半个运行时** —— 下载器已经把压缩包删了，
        // 这里把这个运行时目录整棵收掉（里面只可能是这一次写的半成品）。
        if (cancelRequested(cancel)) QDir(dir).removeRecursively();
        return false;
    }
    if (progress) progress(0, 0);  // 解压阶段无法按条目计数，切回不确定态
    const bool extracted = extractZipWithPowerShell(tempZip, dir);
    QFile::remove(tempZip);
    if (cancelRequested(cancel)) {
        // 解压那一小段不打断（PowerShell 里的进程树不好在半路收），解完再看一眼：
        // 取消了就照样把这一份解出来的东西收干净。
        QDir(dir).removeRecursively();
        return false;
    }
    if (!extracted) return false;
    flattenSingleRootDir(dir);
    const QString javaPath = findExtractedJava(dir);
    if (javaPath.isEmpty()) return false;
    const int found = GlobalJavaSettings::probeJavaMajorVersion(javaPath, nullptr);
    if (found != expectMajor) {
        qWarning() << "Microsoft runtime major mismatch: expected" << expectMajor << "got" << found;
        return false;
    }
    if (javaPathOut != nullptr) *javaPathOut = javaPath;
    return true;
}

/// 拉一小段文本（目录列表那种几十 KB 的 HTML）。失败返回空。
QByteArray fetchSmallText(const QUrl &url) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setTransferTimeout(20000);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
    reply->deleteLater();
    return body.size() <= 4 * 1024 * 1024 ? body : QByteArray();
}

/// 清华 TUNA 的 Adoptium 镜像：列目录挑最新那个 `OpenJDK<major>U-jdk_x64_windows_hotspot_*.zip`。
/// 国内网络通常只有镜像能通（Adoptium API 和 GitHub release 都可能被掐）。
QUrl tunaJdkUrl(int major) {
    const QString base = QStringLiteral("https://mirrors.tuna.tsinghua.edu.cn/Adoptium/%1/jdk/x64/windows/").arg(major);
    const QByteArray html = fetchSmallText(QUrl(base));
    if (html.isEmpty()) return {};
    const QString pattern = QStringLiteral("OpenJDK%1U-jdk_x64_windows_hotspot_[0-9._]+\\.zip").arg(major);
    QRegularExpression re(pattern);
    auto matches = re.globalMatch(QString::fromUtf8(html));
    QString best;
    QList<int> bestParts;
    while (matches.hasNext()) {
        const QString name = matches.next().captured(0);
        QList<int> parts;
        for (const QString &piece : name.split(QRegularExpression(QStringLiteral("[^0-9]+")))) {
            if (!piece.isEmpty()) parts.append(piece.toInt());
        }
        if (parts.size() > bestParts.size()
            || (parts.size() == bestParts.size() && parts > bestParts)) {
            best = name;
            bestParts = parts;
        }
    }
    if (best.isEmpty()) return {};
    return QUrl(base + best);
}

QUrl microsoftJdkUrl(int major) {
    if (major == 25) return QUrl(QStringLiteral("https://aka.ms/download-jdk/microsoft-jdk-25-windows-x64.zip"));
    if (major == 21) return QUrl(QStringLiteral("https://aka.ms/download-jdk/microsoft-jdk-21-windows-x64.zip"));
    if (major == 17) return QUrl(QStringLiteral("https://aka.ms/download-jdk/microsoft-jdk-17-windows-x64.zip"));
    if (major == 8) return QUrl(QStringLiteral("https://aka.ms/download-jdk/microsoft-jdk-8-windows-x64.zip"));
    return {};
}

bool downloadMicrosoftRuntime(int major, QString *javaPathOut,
                             const std::function<void(qint64 done, qint64 total)> &progress = {},
                             const std::atomic_bool *cancel = nullptr) {
    if (cancelRequested(cancel)) return false;
    const QUrl url = microsoftJdkUrl(major);
    if (!url.isValid()) return false;
    if (const QUrl mirror = tunaJdkUrl(major); mirror.isValid()) {
        qInfo() << "改从清华 TUNA 镜像安装 Java" << major;
        if (installZipRuntime(mirror, AppPaths::javaVersionDir(major), major, javaPathOut,
                              progress, cancel)) {
            return true;
        }
        // 用户已经取消了：别再换源重下一遍（那正是"取消之后还在写盘"）。
        if (cancelRequested(cancel)) return false;
    }
    qInfo() << "Adoptium 不可用，改从 Microsoft OpenJDK 安装 Java" << major;
    return installZipRuntime(url, AppPaths::javaVersionDir(major), major, javaPathOut, progress,
                             cancel);
}
}  // namespace
bool downloadJavaRuntime(const QString &destinationDir, const QString &component,
                         QString *javaPathOut,
                         const std::function<void(qint64 done, qint64 total)> &progress = {},
                         const std::atomic_bool *cancel = nullptr) {
    if (cancelRequested(cancel)) return false;
    QList<QUrl> urls;
    // 每个 component 到底要哪个主版本，**以 version.json 里那份为准**（这个函数只负责
    // "按 component 名下一份**对得上**的"）：下面这几档是照着 Mojang 那张表来的 ——
    // jre-legacy → 8、alpha → 16、beta → 17、gamma → 17、delta → 21、epsilon → 25。
    // NOTE: gamma 和 delta **不是**一档：1.19~1.20.4 报的是
    // gamma 但 majorVersion 是 **17**（仓库里 1.19.json 就是 `gamma / 17`），
    // 只有 1.20.5 起的 delta 才是 21。合在一起的表现：给 1.19 下了一份 40~100 MB 的
    // Java 21，调用方拿 majorVersion 一核（17）对不上，再下一份 17 —— 白下一次。
    // 同理 1.17 的 alpha 是 **16**（不是 8）。
    if (component == "java-runtime-epsilon") {
        // Java 25 —— Minecraft 26.1 及以后要求该运行时
        urls = {QUrl("https://download.oracle.com/java/25/latest/jdk-25_windows-x64_bin.zip"),
                QUrl("https://download.bell-sw.com/java/25.0.2+12/bellsoft-jdk25.0.2+12-windows-amd64.zip")};
    } else if (component == "java-runtime-delta") {
        // Java 21 —— Minecraft 1.20.5 及以后
        urls = {
            QUrl("https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.4+7/OpenJDK21U-jre_x64_windows_hotspot_21.0.4_7.zip"),
            QUrl("https://download.bell-sw.com/java/21.0.4+7/bellsoft-jre21.0.4+7-win-amd64.zip")};
    } else if (component == "java-runtime-beta" || component == "java-runtime-gamma") {
        // Java 17 —— 1.18~1.20.4（beta 是 1.18，gamma 是 1.19~1.20.4）
        urls = {
            QUrl("https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.12+7/OpenJDK17U-jre_x64_windows_hotspot_17.0.12_7.zip"),
            QUrl(
                "https://download.bell-sw.com/java/17.0.12+7/bellsoft-jre17.0.12+7-win-amd64.zip")};
    } else if (component == "java-runtime-alpha") {
        // Java 16 —— 1.17（只有它用 16，是 Mojang 那张表里唯一一个 16 档）
        urls = {
            QUrl("https://github.com/adoptium/temurin16-binaries/releases/download/jdk-16.0.2+7/OpenJDK16U-jre_x64_windows_hotspot_16.0.2_7.zip"),
            QUrl("https://download.bell-sw.com/java/16.0.2+7/bellsoft-jre16.0.2+7-win-amd64.zip")};
    } else if (component == "jre-legacy") {
        // Java 8
        urls = {
            QUrl("https://github.com/adoptium/temurin8-binaries/releases/download/jdk8u422-b05/OpenJDK8U-jre_x64_windows_hotspot_8u422b05.zip"),
            QUrl("https://download.bell-sw.com/java/8u422+5/bellsoft-jre8u422+5-win-amd64.zip")};
    } else {
        // Default to Java 21
        urls = {
            QUrl("https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.4+7/OpenJDK21U-jre_x64_windows_hotspot_21.0.4_7.zip"),
            QUrl("https://download.bell-sw.com/java/21.0.4+7/bellsoft-jre21.0.4+7-win-amd64.zip")};
    }

    // 国内网络下 Adoptium API / GitHub release 常常连不上：先试清华 TUNA 镜像，
    // 它能通就直接用（镜像里是同一批 Temurin 资产）。
    {
        int major = 21;
        if (component == "java-runtime-epsilon") major = 25;
        else if (component == "java-runtime-delta") major = 21;
        else if (component == "java-runtime-beta" || component == "java-runtime-gamma") major = 17;
        else if (component == "java-runtime-alpha") major = 16;
        else if (component == "jre-legacy") major = 8;
        const QUrl mirror = tunaJdkUrl(major);
        if (mirror.isValid()) urls.prepend(mirror);
    }

    // 以 component 名为子文件夹，下载和解压内容都放进去
    const QString subDir = QDir(destinationDir).absoluteFilePath(component);
    // 这一路要建目录、往里解压几百 MB 的运行时。和其余写入口同一把数据根锁：另一份启动器
    // 持着这个目录时一个字节都不写 —— 半套运行时留在那儿，`findExtractedJava` 下一次还会
    // 把它当成可用的一份，比"这次没装上"更难收拾。
    {
        QString lockProblem;
        if (!WriteDirectoryGuard::acquire(subDir, &lockProblem)) {
            return false;
        }
    }
    QDir().mkpath(subDir);
    const QString tempZip = QDir(subDir).absoluteFilePath("java_runtime.zip");

    for (const QUrl &url : urls) {
        if (cancelRequested(cancel)) break;
        if (!downloadFileToLocal(url, tempZip, progress, cancel)) {
            if (progress) progress(0, 0);
            // 取消：不再换源重试，也不留半截压缩包 / 半套解压结果。
            if (cancelRequested(cancel)) break;
            continue;
        }

        // 解压到子文件夹
        if (progress) progress(0, 0);
        if (!extractZipWithPowerShell(tempZip, subDir)) {
            continue;
        }
        QFile::remove(tempZip);
        if (cancelRequested(cancel)) {
            // 解压那一小段不打断，解完再看一眼：取消了就把这一份解出来的收干净。
            break;
        }

        // 压平压缩包自带的那层版本号根目录，得到 <component>/bin/java.exe
        flattenSingleRootDir(subDir);

        // NOTE: 必须在刚解压的子目录里找。搜索父目录会命中别的 component
        // 之前解压好的 java.exe，导致版本看起来"匹配"其实不是这一份。
        const QString javaPath = findExtractedJava(subDir);
        if (!javaPath.isEmpty()) {
            if (javaPathOut) {
                *javaPathOut = javaPath;
            }
            return true;
        }
    }

    if (cancelRequested(cancel)) {
        // 这一趟没装成，别留半个运行时（下一次启动的 findExtractedJava 会把它当可用的一份）。
        QFile::remove(tempZip);
        QDir(subDir).removeRecursively();
    }
    return false;
}
}  // namespace

int GlobalJavaSettings::probeJavaMajorVersion(const QString &javaExe, QString *versionOutput) {
    if (!QFileInfo::exists(javaExe)) {
        return -1;
    }
    // 没有 release 的运行时只能起一次 java.exe 去问（冷启动几百毫秒）。按
    // “路径 + java.exe 的修改时间”缓存一次，避免每次打开设置/做升级评估都重付。
    struct CachedProbe {
        qint64 stamp = 0;
        int major = -1;
        QString version;
    };
    static QMutex cacheMutex;
    static QHash<QString, CachedProbe> cache;
    const QFileInfo exeInfo(javaExe);
    const QString cacheKey = exeInfo.absoluteFilePath();
    const qint64 stamp = exeInfo.lastModified().toMSecsSinceEpoch();
    {
        const QMutexLocker locker(&cacheMutex);
        const auto it = cache.constFind(cacheKey);
        if (it != cache.constEnd() && it->stamp == stamp) {
            if (versionOutput != nullptr) *versionOutput = it->version;
            return it->major;
        }
    }

    int major = -1;
    QString version;
    // release 文件在 java.exe 的上一层：<运行时根>/bin/java.exe → <运行时根>/release
    QDir runtimeRoot = exeInfo.absoluteDir();
    runtimeRoot.cdUp();
    QFile releaseFile(runtimeRoot.absoluteFilePath(QStringLiteral("release")));
    if (releaseFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        version = QString::fromUtf8(releaseFile.readAll());
        major = GlobalJavaSettings::majorVersionFromReleaseFile(version);
    }
    if (major <= 0) {
        // 没有 release 文件（或者里面没有能认的 JAVA_VERSION）：只能真的问它一次。
        QString probed;
        if (!verifyJavaExecutable(javaExe, &probed)) {
            const QMutexLocker locker(&cacheMutex);
            cache.insert(cacheKey, CachedProbe{stamp, -1, QString()});
            return -1;
        }
        version = probed;
        major = parseJavaMajorVersion(probed);
    }
    {
        const QMutexLocker locker(&cacheMutex);
        cache.insert(cacheKey, CachedProbe{stamp, major, version});
    }
    if (versionOutput != nullptr) *versionOutput = version;
    return major;
}

GlobalJavaSettings::GlobalJavaSettings(QObject *parent)
    : QObject(parent),
      m_xmx(kBuiltinXmxMb)  // 4GB
      ,
      m_xms(kBuiltinXmsMb)  // 1GB
      ,
      m_extraJvmArgs(QStringLiteral("-XX:+UseG1GC")) {
    // load() 里读回的 Xmx/Xms 可能是盘上存的值（手动档），也可能之后被自动档重算掉
    // （第一次读设置页或第一次启动时）。构造函数这里只保证"没读过盘之前也是合法值"。
    load();
}

QString GlobalJavaSettings::javaPath() const { return m_javaPath; }

EffectiveJavaSettings GlobalJavaSettings::effective() const {
    EffectiveJavaSettings settings;
    settings.javaPath = m_javaPath;
    settings.xmx = m_xmx;
    settings.xms = m_xms;
    settings.autoMemory = m_memoryAuto;
    settings.extraJvmArgs = m_extraJvmArgs;
    return settings;
}

void GlobalJavaSettings::setJavaPath(const QString &path) { m_javaPath = path; }

int GlobalJavaSettings::xmx() const { return m_xmx; }

void GlobalJavaSettings::setXmx(int mbytes) { m_xmx = qMax(256, mbytes); }

int GlobalJavaSettings::xms() const { return m_xms; }

void GlobalJavaSettings::setXms(int mbytes) { m_xms = qMax(128, mbytes); }

QString GlobalJavaSettings::extraJvmArgs() const { return m_extraJvmArgs; }

void GlobalJavaSettings::setExtraJvmArgs(const QString &args) { m_extraJvmArgs = args; }

void GlobalJavaSettings::load() {
    QSettings settings;
    m_javaPath = settings.value(kJavaPath).toString();
    // NOTE: 这两个数**读回来也要过一遍钳位**。盘上那份不能当成可信的（这条规矩在
    // LauncherSettings 那边写得很清楚：宽高、标题读侧写侧都钳），直接 toInt() 赋给成员
    // 就少了这一道。
    // 翻车的例子：注册表 HKCU\Software\WTFML\WTFML\Java\Xmx 被手改成 0（或者一个非数字
    // 串 —— QVariant→int 失败同样是 0）→ 界面那格被 setRange 夹出下限、那行说明文字里
    // 是 **0 MB**、真正发给 JVM 的却是 resolveJavaSettings 钳出来的 256m：**三处数字
    // 互不相同**。更糟的是启动链发现"最小值 > 最大值"之后会 save()，把这个坏值**自己
    // 写回注册表** —— 从此它就活下来了，用户改回来都难对得上账。
    // NOTE: 钳位的唯一出处是 setXmx()/setXms()（256 / 128 那两道下限写在它们里面），
    // 所以这里用 setter 而不是自己再写一遍 qMax。
    // NOTE: 认不出数字（ok == false）时**要说一声**：静默把用户的值换成下限，和"识别失败
    // 装作没事"是同一个毛病。这一行进日志文件（安装 handler 之后就有落点）。
    bool okXmx = false;
    bool okXms = false;
    const int rawXmx = settings.value(kXmx, m_xmx).toInt(&okXmx);
    const int rawXms = settings.value(kXms, m_xms).toInt(&okXms);
    if (!okXmx || !okXms) {
        qWarning() << "内存设置读出来不是数字，已按下限处理：Xmx" << rawXmx << "Xms" << rawXms;
    }
    setXmx(rawXmx);
    setXms(rawXms);
    m_extraJvmArgs = settings.value(kExtraArgs, m_extraJvmArgs).toString();
    // 默认值取成员当前值（构造时是 true）：注册表里没这一项就落到"自动"。
    m_autoSelectJava = settings.value(kAutoSelect, m_autoSelectJava).toBool();

    // 「内存分配」自动档：没有这个键 = 手动档，读回 false。
    m_memoryAuto = settings.value(kMemoryAuto, m_memoryAuto).toBool();

    // 自动档之前的现场：没有这两个键（老配置 / 从没开过自动）就拿刚刚读回来的 Xmx/Xms
    // 当现场 —— 那是用户当前真正在用的值，切回手动时至少不会凭空改成内置默认值。
    bool okManualXmx = false;
    bool okManualXms = false;
    const int rawManualXmx = settings.value(kManualXmx, m_xmx).toInt(&okManualXmx);
    const int rawManualXms = settings.value(kManualXms, m_xms).toInt(&okManualXms);
    m_manualXmx = okManualXmx ? qMax(256, rawManualXmx) : m_xmx;
    m_manualXms = okManualXms ? qMax(128, rawManualXms) : m_xms;
    if (m_manualXms > m_manualXmx) {
        m_manualXms = m_manualXmx;
    }

    // 提醒总闸：没有这个键（老配置）就落到成员默认值 true ——
    // 这类不匹配一律提醒。
    m_warnVersionMismatch = settings.value(kWarnVersionMismatch, m_warnVersionMismatch).toBool();

    // 从这一刻起，"盘上该是什么"就是这一份。save() 只写与它不同的项 —— 全局设置
    // 是几份启动器共用的注册表，整份写回会把别人刚改的一项打回去（见 .hpp 里那条 NOTE）。
    // 放在最后：前面归一化（钳位、手改坏值的兜底）都已经落进成员了。
    m_persisted = persistedValues();
}

QVariantMap GlobalJavaSettings::persistedValues() const {
    // 键名 → 值，和以前 save() 里那张逐条 setValue 的表一一对应。
    QVariantMap values;
    values.insert(QString::fromLatin1(kJavaPath), m_javaPath);
    values.insert(QString::fromLatin1(kXmx), QVariant(m_xmx));
    values.insert(QString::fromLatin1(kXms), QVariant(m_xms));
    values.insert(QString::fromLatin1(kExtraArgs), m_extraJvmArgs);
    values.insert(QString::fromLatin1(kAutoSelect), QVariant(m_autoSelectJava));
    values.insert(QString::fromLatin1(kMemoryAuto), QVariant(m_memoryAuto));
    values.insert(QString::fromLatin1(kManualXmx), QVariant(m_manualXmx));
    values.insert(QString::fromLatin1(kManualXms), QVariant(m_manualXms));
    values.insert(QString::fromLatin1(kWarnVersionMismatch), QVariant(m_warnVersionMismatch));
    return values;
}

void GlobalJavaSettings::save() const {
    const QVariantMap now = persistedValues();
    QVariantMap changed;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (m_persisted.value(it.key()) != it.value()) {
            changed.insert(it.key(), it.value());
        }
    }
    if (changed.isEmpty()) {
        return;
    }

    // 走 SettingsSyncRetry：理由和 LauncherSettings::save() 那条一样（Windows 上一次
    // AccessError 就把这一次改动丢了），重试只落在这一批改过的键上。
    QString fileName;
    int attempts = 0;
    bool wroteAnything = false;
    const bool ok = SettingsSyncRetry::write(
        [&](QSettings &settings) {
            // 自动档算出来的 Xmx/Xms 是"会自己变的"：另一扇窗可能在我们算完之后把档位
            // 切回手动，并把用户要的那一对写回盘。这时再把自动值写下去，就是拿自动值
            // 盖掉手动现场（症状：切回手动后内存显示成自动算的那个数 —— 2026-11-04
            // mw_check 那条间歇失败）。
            //
            // 判据：**本对象上一次保存时是自动档**（`m_persisted`）而**现在盘上是手动档**
            // —— 说明档位是别人改的，这两格不归我们写。刚由手动切自动的那一发 `wasAuto`
            // 还是假，不受影响（那一发要写的正是 ManualXmx / ManualXms 与 MemoryAuto）。
            // 每次尝试都重新读盘：重试发生在几十毫秒之后，那时盘上的档位可能已经变了。
            const bool staleAutoWriter =
                m_persisted.value(QString::fromLatin1(kMemoryAuto)).toBool()
                && !settings.value(QString::fromLatin1(kMemoryAuto), false).toBool();
            wroteAnything = false;
            for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
                if (staleAutoWriter
                    && (it.key() == QLatin1String(kXmx) || it.key() == QLatin1String(kXms))) {
                    continue;
                }
                settings.setValue(it.key(), it.value());
                wroteAnything = true;
            }
        },
        &fileName, &attempts);
    if (!ok) {
        // 和 LauncherSettings::save() 同一条：失败时基线不推进，下一次保存会重试。
        qWarning() << "GlobalJavaSettings: 写入 Java 设置失败（已试" << attempts
                   << "次），未推进已保存基线：" << fileName;
        return;
    }
    if (!wroteAnything) {
        return;
    }
    m_persisted = now;
}

void GlobalJavaSettings::setWarnVersionMismatch(bool warn) {
    m_warnVersionMismatch = warn;
    // 立刻落盘的理由和 setAutoMemory() 一样：界面上就这么一个勾选框，没有「应用」按钮。
    save();
}

void GlobalJavaSettings::setAutoMemory(bool autoMemory) {
    // 只有"手动 → 自动"这一下要记现场。已经在自动档上再调一次 true 不能把现场覆盖成
    // 自动算出来的那个数 —— 用户切回手动时想拿回的是**开自动之前**的值。
    if (autoMemory && !m_memoryAuto) {
        m_manualXmx = m_xmx;
        m_manualXms = m_xms;
    }
    m_memoryAuto = autoMemory;
    // 和 setLauncherBehaviour() 一样：改完立刻落盘。注意**不在这里重算** Xmx/Xms ——
    // 从手动切到自动时由 refreshSuggestedMemory() 算；切回手动走 restoreManualMemory()。
    save();
}

void GlobalJavaSettings::restoreManualMemory() {
    m_memoryAuto = false;
    setXmx(m_manualXmx);
    setXms(m_manualXms);
    if (m_xms > m_xmx) {
        m_xms = m_xmx;
    }
    save();
}

int GlobalJavaSettings::suggestXmxMb(qint64 totalMb, qint64 availableMb, int modCount) {
    if (totalMb <= 0) {
        // 读不到系统内存（非 Windows 平台、或者系统调用失败）：退回内置默认值。
        // 这里**不**拿 availableMb 硬凑 —— 连总量都不知道，任何推算都是编的。
        // 加成也不加：不知道这台机器有多少内存，"给整合包多点"就没有可加的基数。
        return kBuiltinXmxMb;
    }

    // 整合包规模那一档加在**基准**上（第 1 步），不是加在最后的结果上 —— 见上面
    // kAutoXmxModsPerStep 那段：加成抬的是目标值，机器侧那两条硬顶照旧说了算。
    qint64 budget = totalMb / 4 + modCountBonusMb(modCount);
    if (availableMb > 0) {
        budget = qMin(budget, availableMb / 2);
    }
    // 先在"体面范围"里夹一次。三个参数都写成 qint64：常量是 int，混着传会让 qBound 的
    // 重载解析变成二义的。
    budget = qBound(qint64(kAutoXmxMinMb), budget, qint64(kAutoXmxMaxMb));
    // 再套物理内存一半的硬顶。这一条**排在下限之后**是有意的：小机器上它会压过下限
    // （2 GB 的机器给 1024 已经是一半了，1 GB 的机器只该给 512）—— 在一条本来就紧张的
    // 机器上按"最少 1 GB"硬凑，只会让 JVM 连堆都开不起来，比给个小值更糟。
    budget = qMin(budget, totalMb / 2);
    // 但绝不小于 JVM 侧的下限（setXmx 会钳到 256）：返回一个会被 setter 悄悄改掉的值，
    // 就是"界面显示的和实际用的不一致"。
    return qMax(roundDownToStep(budget), 256);
}

int GlobalJavaSettings::suggestXmsMb(int xmxMb) {
    // Xms 给 Xmx 的四分之一：太小会一直扩容（常见的卡顿来源），太大等于一启动就占满。
    //
    // Xms **必须** <= Xmx，否则 JVM 直接起不来：
    // Error occurred during initialization of VM
    // Initial heap size set to a larger value than the maximum heap size
    // 所以下限和上限要一起排好序再夹：Xmx 比 512 还小的时候（setXmx 的下限是 256），
    // 下限得跟着退到 Xmx 上，直接写 qBound(512, x, xmx) 会变成 min > max，取到的值没有意义。
    const int xmx = qMax(xmxMb, 256);
    const int upper = qMin(xmx, kAutoXmsMaxMb);
    const int quarter = roundDownToStep(xmx / 4);
    return qBound(qMin(kAutoXmsMinMb, upper), quarter, upper);
}

SystemMemory::Usage GlobalJavaSettings::refreshSuggestedMemory() {
    const SystemMemory::Usage usage = SystemMemory::read();
    if (!m_memoryAuto) {
        // 手动档：读还是要读（界面拿它写说明），但一个数字都不许动。
        return usage;
    }
    setXmx(suggestXmxMb(usage.totalMb, usage.availableMb));
    setXms(suggestXmsMb(m_xmx));
    return usage;
}

int GlobalJavaSettings::majorVersionFromReleaseFile(const QString &releaseText) {
    // release 文件是 KEY="value" 一行一条，版本那条叫 JAVA_VERSION：
    // Java 9+ : JAVA_VERSION="21.0.7"
    // Java 8 : JAVA_VERSION="1.8.0_422"
    // 刻意**只认带引号的 JAVA_VERSION=** 这一种写法：同一个文件里还有
    // JAVA_VERSION_DATE、MODULES 之类的行，宽泛地找 "version" 会撞上它们。
    // 找不到就返回 -1，调用方退回真的跑一次 java -version，不猜。
    static const QRegularExpression regex(QStringLiteral("JAVA_VERSION=\"([^\"]+)\""));
    const QRegularExpressionMatch match = regex.match(releaseText);
    if (!match.hasMatch()) {
        return -1;
    }
    return majorFromVersionString(match.captured(1));
}

QMap<int, QString> GlobalJavaSettings::scanLocalJavaVersions() {
    return scanLocalJavaVersions(AppPaths::javaDir());
}

QMap<int, QString> GlobalJavaSettings::scanLocalJavaVersions(const QString &javaDir) {
    // NOTE: 定义处按 C++ 的规矩不写 static（.hpp 里那一声才作数）。它是 static 的：
    // 扫的是这台机器上有什么，不读也不写这个对象的任何状态。
    QMap<int, QString> versions;

    QDir dir(javaDir);
    if (dir.exists()) {
        const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &subdir : subdirs) {
            const QString javaExe = dir.absoluteFilePath(subdir + "/bin/java.exe");
            const int majorVersion = GlobalJavaSettings::probeJavaMajorVersion(javaExe);
            if (majorVersion > 0) {
                versions[majorVersion] = javaExe;
            }
        }
    }

    // 原版启动器已经下载好的运行时（jre-legacy / java-runtime-*）同样可以复用，
    // 省掉一次完整下载。这里用 first-wins，优先保留启动器自己下载的那一份。
    const QMap<QString, VanillaRuntime> vanillaComponents = scanVanillaRuntimeComponents();
    for (auto it = vanillaComponents.cbegin(); it != vanillaComponents.cend(); ++it) {
        const VanillaRuntime &runtime = it.value();
        if (!versions.contains(runtime.majorVersion)) {
            qDebug() << "Reusing vanilla launcher runtime" << it.key() << "for Java"
                     << runtime.majorVersion;
            versions[runtime.majorVersion] = runtime.exe;
        }
    }

    return versions;
}

QString GlobalJavaSettings::pickMinimalJavaAtLeast(int requiredMajor,
                                                   const QMap<int, QString> &candidates) {
    if (requiredMajor <= 0) {
        return QString();
    }
    // QMap 是按 key 升序走的，所以**第一个**够新的就是最小的那个，直接返回。
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        if (it.key() >= requiredMajor && !it.value().isEmpty()) {
            return it.value();
        }
    }
    return QString();
}

bool GlobalJavaSettings::installerJavaFor(int requiredMajor, const QString &configuredPath,
                                          QString *javaPathOut, QString *reasonOut) {
    if (javaPathOut != nullptr) {
        *javaPathOut = configuredPath;
    }
    const int configuredMajor = probeJavaMajorVersion(configuredPath);
    if (requiredMajor <= 0 || configuredPath.isEmpty() || configuredMajor >= requiredMajor) {
        return true;
    }

    QMap<int, QString> candidates = scanLocalJavaVersions();
    // NOTE: 刻意**不**翻 Program Files（findJavaInCommonLocations()）：它是递归扫整棵树、
    // 而且每个 java.exe 都要跑一次 java -version，几百毫秒起步。真装在 Program Files
    // 里的 Java 一般也在 PATH 上，上面那条就够。
    const auto addCandidate = [&candidates](const QString &path) {
        const int major = probeJavaMajorVersion(path);
        if (major > 0 && !candidates.contains(major)) {
            candidates.insert(major, path);
        }
    };
    addCandidate(QStandardPaths::findExecutable(QStringLiteral("java")));
    for (const char *var : {"JAVA_HOME", "JDK_HOME"}) {
        const QString home = qEnvironmentVariable(var);
        if (!home.isEmpty()) {
            addCandidate(QDir(home).absoluteFilePath(QStringLiteral("bin/java.exe")));
        }
    }

    const QString picked = pickMinimalJavaAtLeast(requiredMajor, candidates);
    if (picked.isEmpty()) {
        int highest = 0;
        for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
            highest = qMax(highest, it.key());
        }
        qWarning() << "No Java" << requiredMajor << "for the installer; configured:"
                   << configuredPath << "highest local:" << highest;
        if (reasonOut != nullptr) {
            *reasonOut =
                QCoreApplication::translate("GlobalJavaSettings", "此版本需要 Java %1，当前使用 Java %2，检测到的最高版本为 Java %3。\n\n请安装 Java %1，或在设置中指定 Java 路径。")
                    .arg(requiredMajor)
                    .arg(configuredMajor > 0 ? QString::number(configuredMajor)
                                             : QCoreApplication::translate("GlobalJavaSettings", "版本未知"))
                    .arg(highest > 0 ? QString::number(highest) : QCoreApplication::translate("GlobalJavaSettings", "没有"));
        }
        return false;
    }
    if (javaPathOut != nullptr) {
        *javaPathOut = picked;
    }
    return true;
}

bool GlobalJavaSettings::downloadJavaFromMojang(
    int majorVersion, QString *javaPathOut,
    const std::function<void(qint64 done, qint64 total)> &progress,
    const std::atomic_bool *cancel) {
    if (cancelRequested(cancel)) return false;
    // 先试清华 TUNA 镜像：国内网络里 Adoptium API 也许能通，但 GitHub release 的下载
    // 十有八九被掐（`Connection closed`）；镜像里是同一批 Temurin 资产。
    if (const QUrl mirror = tunaJdkUrl(majorVersion); mirror.isValid()) {
        qInfo() << "改从清华 TUNA 镜像安装 Java" << majorVersion;
        if (installZipRuntime(mirror, AppPaths::javaVersionDir(majorVersion), majorVersion,
                              javaPathOut, progress, cancel)) {
            return true;
        }
        if (cancelRequested(cancel)) return false;
    }
    qDebug() << "Starting Java download from Adoptium for version:" << majorVersion;

    // Use Adoptium API to download Java
    const QString apiUrl =
        QString("https://api.adoptium.net/v3/assets/latest/%1/hotspot").arg(majorVersion);
    qDebug() << "Fetching from Adoptium API:" << apiUrl;

    QNetworkAccessManager manager;
    QNetworkRequest request(apiUrl);
    // 同 downloadFileToLocal()：这一路也是 UI 线程上的同步请求，没有截止的话
    // 对端连上不发数据就永远回不来（这个请求只是取一份 JSON，30 秒足够）。
    // 排队与截止交给 `RequestBudget::ControlGet`（与其它元数据请求共用 Control 名额）；
    // 30 秒现在是「排队 + 传输」的合计 —— 这一路本来就阻塞等待，收场没变。
    QByteArray body;
    QString problem;
    QEventLoop loop;
    {
        // 只为了给作业一个主人：这一路没有现成的 QObject 可以挂。
        QObject owner;
        RequestBudget::ControlGet::start(
            &owner, &manager, request, 30000,
            [&](QNetworkReply *reply, const QString &jobError) {
                if (reply == nullptr) {
                    problem = jobError;
                } else {
                    body = reply->readAll();
                    problem = reply->error() == QNetworkReply::NoError ? QString()
                                                                       : reply->errorString();
                    reply->deleteLater();
                }
                loop.quit();
            });
        loop.exec();
    }

    if (!problem.isEmpty()) {
        qDebug() << "Network error fetching from Adoptium API:" << problem;
        if (cancelRequested(cancel)) return false;
        return downloadMicrosoftRuntime(majorVersion, javaPathOut, progress, cancel);
    }

    qDebug() << "Adoptium API response received successfully";
    QJsonDocument doc = QJsonDocument::fromJson(body);

    QJsonArray assets = doc.array();
    if (assets.isEmpty()) {
        qDebug() << "No assets found in Adoptium API response";
        if (cancelRequested(cancel)) return false;
        return downloadMicrosoftRuntime(majorVersion, javaPathOut, progress, cancel);
    }

    // 在 Windows x64 的包里挑一个：优先 JRE，没有 JRE 才退到 JDK（见下面的 downloadUrl 选取）
    QString downloadUrl;
    QString jreUrl;
    QString jdkUrl;
    for (const QJsonValue &asset : assets) {
        QJsonObject assetObj = asset.toObject();
        QJsonObject binary = assetObj["binary"].toObject();

        QString os = binary["os"].toString();
        QString arch = binary["architecture"].toString();
        QString imageType = binary["image_type"].toString();

        if (os == "windows" && arch == "x64") {
            QJsonObject package = binary["package"].toObject();
            const QString link = package["link"].toString();
            if (link.isEmpty()) {
                continue;
            }
            // Adoptium 对较新的 Java（25+）未必发布 JRE 构建，所以优先取 JRE，
            // 没有 JRE 时退回 JDK —— 两者都能跑游戏，JDK 只是体积大一些。
            if (imageType == "jre" && jreUrl.isEmpty()) {
                jreUrl = link;
            } else if (imageType == "jdk" && jdkUrl.isEmpty()) {
                jdkUrl = link;
            }
        }
    }

    downloadUrl = jreUrl.isEmpty() ? jdkUrl : jreUrl;
    if (downloadUrl.isEmpty()) {
        qDebug() << "No suitable Windows x64 JRE/JDK found in Adoptium API response";
        if (cancelRequested(cancel)) return false;
        return downloadMicrosoftRuntime(majorVersion, javaPathOut, progress, cancel);
    }
    qDebug() << "Chosen download URL:" << downloadUrl;

    // Download the zip
    const QString downloadDir = AppPaths::javaVersionDir(majorVersion);
    qDebug() << "Download directory:" << downloadDir;
    // 同 downloadJavaRuntime()：建目录 + 解压是一整趟写，先取数据根锁。
    {
        QString lockProblem;
        if (!WriteDirectoryGuard::acquire(downloadDir, &lockProblem)) {
            qDebug() << "Java directory is locked:" << lockProblem;
            return false;
        }
    }
    QDir().mkpath(downloadDir);
    QString tempZip = QDir(downloadDir).absoluteFilePath("java_runtime.zip");
    qDebug() << "Temp ZIP path:" << tempZip;

    qDebug() << "Downloading ZIP from:" << downloadUrl;
    if (!downloadFileToLocal(QUrl(downloadUrl), tempZip, progress, cancel)) {
        qDebug() << "Failed to download ZIP file";
        if (progress) progress(0, 0);
        if (cancelRequested(cancel)) {
            // 取消：不再换源重下、也不留半个运行时。
            QDir(downloadDir).removeRecursively();
            return false;
        }
        return downloadMicrosoftRuntime(majorVersion, javaPathOut, progress, cancel);
    }
    qDebug() << "ZIP downloaded successfully";

    if (progress) progress(0, 0);
    qDebug() << "Extracting ZIP...";
    const bool extracted = extractZipWithPowerShell(tempZip, downloadDir);
    if (!extracted) {
        qDebug() << "Failed to extract ZIP";
        QFile::remove(tempZip);
        return false;
    }
    qDebug() << "ZIP extracted successfully";
    QFile::remove(tempZip);
    if (cancelRequested(cancel)) {
        // 解压那一小段不打断，解完再看一眼：取消了就把这一份解出来的收干净。
        QDir(downloadDir).removeRecursively();
        return false;
    }

    // 压平压缩包自带的版本号根目录，使布局稳定为 java/<majorVersion>/bin/java.exe，
    // 这样 scanLocalJavaVersions() 下次能直接认得出来。
    flattenSingleRootDir(downloadDir);

    qDebug() << "Finding extracted Java...";
    QString javaPath = findExtractedJava(downloadDir);
    if (!javaPath.isEmpty()) {
        qDebug() << "Found Java at:" << javaPath;
        // 只回填路径，不碰 m_javaPath、不落盘 —— 见 .hpp 里的 NOTE：存到哪个作用域
        // 是调用方的事。
        if (javaPathOut != nullptr) {
            *javaPathOut = javaPath;
        }
        return true;
    }

    qDebug() << "No Java executable found after extraction";
    return false;
}
/**
 * @brief Ensures the correct Java version is available for the specified Minecraft version.
 *
 * 候选顺序：已配置路径 → 本地已安装运行时（含原版启动器下载的 java-runtime-*）
 * → JAVA_HOME/JDK_HOME → PATH → 常见安装目录 → 按 component 下载 → 按主版本下载。
 * 前五个来源找到的 Java **必须主版本匹配**才采用，否则会静默选中错误的 Java
 * （例如拿 Java 21 去启动需要 Java 25 的 26.x，只会得到 UnsupportedClassVersionError）。
 *
 * @param majorVersion Required Java major version (e.g., 8, 11, 17, 21, 25)
 * @param component Optional runtime component name from version.json,
 * e.g. "java-runtime-epsilon"
 * @return true if appropriate Java is available and configured, false otherwise
 */
bool GlobalJavaSettings::ensureJavaForVersion(int majorVersion, const QString &component,
                                              bool allowDownload) {
    // 这个重载 = **全局档**：挑中的路径写回本对象并落盘。除了"值从成员换成一份副本"，
    // 这一步的行为与无实例覆盖时逐字节一致。
    EffectiveJavaSettings io = effective();
    QString error;
    if (!ensureJavaForVersion(io, majorVersion, component, m_autoSelectJava, &error,
                              /*allowMismatchedPath=*/false, allowDownload)) {
        return false;
    }
    adoptJavaPath(io.javaPath);
    return true;
}

void GlobalJavaSettings::adoptJavaPath(const QString &path) {
    if (path == m_javaPath) {
        return;
    }
    m_javaPath = path;
    save();
}

bool GlobalJavaSettings::ensureJavaForVersion(
    EffectiveJavaSettings &io, int majorVersion, const QString &component, bool allowAutoSelect,
    QString *errorOut, bool allowMismatchedPath, bool allowDownload,
    const std::function<void(qint64 done, qint64 total)> &progress,
    const std::atomic_bool *cancel) {
    const auto fail = [errorOut](const QString &reason) {
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    };
    // 进来之前就已经取消了（换令牌 / natives 那两段里按的取消）：一个字节都别下。
    if (cancelRequested(cancel)) {
        return fail(QCoreApplication::translate("GlobalJavaSettings", "已取消准备 Java 运行时。"));
    }

    qDebug() << "Ensuring Java for version:" << majorVersion << "component:" << component;

    // First, check if we already have the correct version configured
    if (!io.javaPath.isEmpty()) {
        // 用户在「Java 版本不符」那个框里点了「仍然用这个 Java 启动」：他要的就是这条路径。
        // 判在 javaMatchesMajor() **之前**是有意的 —— 那个函数总是冷启一个 JVM，而这里
        // 已经拿到过用户的明确授权，没必要再花那几十毫秒去确认一件他已经决定的事。
        if (allowMismatchedPath) {
            qDebug() << "Using the mismatched Java the user explicitly chose:" << io.javaPath;
            return true;
        }
        if (javaMatchesMajor(io.javaPath, majorVersion)) {
            qDebug() << "Using configured Java version" << majorVersion << "at:" << io.javaPath;
            return true;
        }
        qDebug() << "Configured Java does not match required version" << majorVersion << ":"
                 << io.javaPath;
    }

    // 手动档到此为止：配置的路径不匹配（或者压根没配）时**不替用户换一个**。
    // 自动挑在手动档下是帮倒忙 —— 用户钉 Java 8 就是为了跑 1.16.5，静默换成 Java 21
    // 只会得到 UnsupportedClassVersionError，而且看不出是启动器换的。报错好过猜。
    if (!allowAutoSelect) {
        const QString reason =
            QCoreApplication::translate("GlobalJavaSettings", "当前设置为手动指定 Java，")
            + (io.javaPath.isEmpty() ? QCoreApplication::translate("GlobalJavaSettings", "未指定路径")
                                     : io.javaPath + QCoreApplication::translate("GlobalJavaSettings", " 的主版本不是 ")
                                           + QString::number(majorVersion))
            + QCoreApplication::translate("GlobalJavaSettings", "，未找到兼容版本时取消启动。");
        qWarning() << reason;
        return fail(reason);
    }

    // Scan local Java versions
    QMap<int, QString> localVersions = scanLocalJavaVersions();
    if (localVersions.contains(majorVersion)) {
        const QString path = localVersions[majorVersion];
        qDebug() << "Found matching local Java version" << majorVersion << "at:" << path;
        io.javaPath = path;
        return true;
    }

    // Try to find in environment
    QString candidate = findJavaInEnvironment(majorVersion);
    if (!candidate.isEmpty()) {
        qDebug() << "Found matching Java in environment:" << candidate;
        io.javaPath = candidate;
        return true;
    }

    candidate = findJavaOnPath(majorVersion);
    if (!candidate.isEmpty()) {
        qDebug() << "Found matching Java on PATH:" << candidate;
        io.javaPath = candidate;
        return true;
    }

    candidate = findJavaInCommonLocations(majorVersion);
    if (!candidate.isEmpty()) {
        qDebug() << "Found matching Java in common locations:" << candidate;
        io.javaPath = candidate;
        return true;
    }

    // 下载之前先看闸：测试（以及将来的"离线模式"）只允许在本机找，找不到就如实失败。
    // 这一道放在两个下载分支**共同的前面**，所以不存在"跳过 Mojang 又落到 Adoptium"的缺口。
    if (!allowDownload) {
        qDebug() << "Download disabled for Java" << majorVersion << "- local scan only";
        return fail(QCoreApplication::translate("GlobalJavaSettings", "这台机器上没有 Java %1，且当前不允许下载运行时。")
                        .arg(majorVersion));
    }

    // 按 version.json 声明的 component 下载对应运行时
    if (!component.isEmpty()) {
        qDebug() << "Downloading runtime component" << component << "for Java" << majorVersion;
        const QString javaBase = AppPaths::javaDir();
        QString downloaded;
        if (downloadJavaRuntime(javaBase, component, &downloaded, progress, cancel) &&
            javaMatchesMajor(downloaded, majorVersion)) {
            qDebug() << "Downloaded and verified Java" << majorVersion << "at:" << downloaded;
            io.javaPath = downloaded;
            return true;
        }
        if (cancelRequested(cancel)) {
            return fail(QCoreApplication::translate("GlobalJavaSettings", "已取消准备 Java 运行时。"));
        }
        qDebug() << "Component" << component << "did not provide a matching Java, falling back";
    }

    // Download from Adoptium as a last resort
    qDebug() << "No matching Java found, downloading version" << majorVersion << "from Adoptium...";
    QString downloaded;
    // NOTE: 这一条**不**再认一次主版本（downloadJavaFromMojang 内部
    // 已经确认解压出来的 java.exe 能跑，而 Adoptium 是照 majorVersion 要的包）。
    // 多判一次会让"本来能启动"变成"报错"。
    if (downloadJavaFromMojang(majorVersion, &downloaded, progress, cancel)) {
        qDebug() << "Downloaded Java version" << majorVersion;
        io.javaPath = downloaded;
        return true;
    }
    if (cancelRequested(cancel)) {
        return fail(QCoreApplication::translate("GlobalJavaSettings", "已取消准备 Java 运行时。"));
    }

    qDebug() << "Failed to obtain Java version" << majorVersion;
    return fail(QCoreApplication::translate("GlobalJavaSettings", "找不到也下载不到 Java %1。").arg(majorVersion));
}








