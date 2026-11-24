#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <atomic>
#include <functional>

#include "../util/SystemMemory.hpp"
#include "EffectiveJavaSettings.hpp"

/**
 * @file GlobalJavaSettings.hpp
 * @brief Declares Java runtime configuration and version management.
 * @note Provides Java detection and version-specific runtime selection.
 *
 * Responsible for locating or downloading the correct Java runtime
 * required by the selected Minecraft version.
 */
class GlobalJavaSettings : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Constructs the Java settings manager.
     * @param parent Parent QObject.
     */
    explicit GlobalJavaSettings(QObject *parent = nullptr);

    /**
     * @brief Returns the configured Java executable path.
     */
    QString javaPath() const;

    /**
     * @brief Sets the Java executable path.
     * @param path Absolute path to the java executable.
     */
    void setJavaPath(const QString &path);

    // ------------------------------------------------------------------
    // 「Java 版本」这一项（设置界面）：自动挑 / 手动钉死
    // ------------------------------------------------------------------
    // 两个档位**不是**"路径空不空"能表达的：自动档也会把挑中的路径写进 m_javaPath
    // （它同时是缓存），所以"路径非空"既可能是自动挑的、也可能是用户钉的，
    // 必须另存一个开关才分得清。

    /**
     * @brief 是否让启动器自己按版本要求挑 Java。
     * @return true = 自动（默认）；
     * false = 手动指定 —— 路径不匹配时**不换一个**，直接失败。
     */
    bool autoSelectJava() const { return m_autoSelectJava; }

    /**
     * @brief 设置自动/手动档。和其他 setter 一样**只改内存**，落盘调 save()。
     * @param autoSelect true = 自动。
     */
    void setAutoSelectJava(bool autoSelect) { m_autoSelectJava = autoSelect; }

    // ------------------------------------------------------------------
    // 「实例指定的 Java 与版本要求不符」要不要提醒（设置界面：一个勾选框）
    // ------------------------------------------------------------------
    // 提醒本身发生在启动路径上（MainWindow::onStartButtonClicked()），这里只存
    // **总闸**：关掉之后，任何实例都不再问，直接按它自己钉的那个 Java 启动。
    //
    // NOTE: 关掉的总闸**不是**"不提醒但仍然拦下" —— 那种读法会把用户唯一的出路堵死：
    // 实例钉的路径不匹配时，ensureJavaForVersion() 的手动档分支只有"报错"一条路，
    // 提醒正是他改用匹配 Java 的唯一入口。所以"不提醒"必须等于"照他说的做"。

    /// 实例指定的 Java 与版本要求不符时要不要提醒。默认 true。
    bool warnVersionMismatch() const { return m_warnVersionMismatch; }

    /// 设置并**立刻落盘**（和 setAutoMemory() 一样：单勾选框，没有「应用」按钮）。
    void setWarnVersionMismatch(bool warn);

    /**
     * @brief 把"自动挑中的 Java 路径"存回**全局**这一层：改内存 + 落盘。
     * @param path 挑中的路径。和当前值一样时**不写盘** —— 每次启动都重写一遍注册表
     * 没必要，而且那条路径本来就在盘上。
     *
     * 只有全局档该调它。实例档挑中的路径要写回那个实例的 wtfml.meta（见
     * MainWindow::onStartButtonClicked()），写到这里就是跨作用域污染。
     *
     * NOTE: 这段逻辑独立于 ensureJavaForVersion(int, QString)：那个签名只能改自己，
     * 装不下"全局档怎么存"。拆出来是为了让值版和它共用同一份定义，
     * 而不是在两处各写一遍 setJavaPath + save。
     */
    void adoptJavaPath(const QString &path);

    /**
     * @brief 把这一层的值倒成一份**有效值**，供实例覆盖层当底用。
     *
     * 这是"每实例设置"那一层唯一需要全局档提供的东西：合成
     * （resolveJavaSettings()）和自动挑 Java 都只认 EffectiveJavaSettings。
     */
    EffectiveJavaSettings effective() const;

    /**
     * @brief 隐式转换到有效值。等价于 effective()。
     *
     * 选隐式转换而不是让调用方每处都写 effective()，是为了让
     * LauncherManager::generateJvmArguments() / startGame() 换成收
     * `const EffectiveJavaSettings &` 之后，**所有现存调用点一个字都不用改**
     * （验证工程里有二十多处）。顺带 LauncherManager 从此完全不认识这个类，
     * 依赖方向从"启动器知道全局设置"变成"启动器只知道这次要用的值"。
     */
    operator EffectiveJavaSettings() const { return effective(); }

    /**
     * @brief Ensures the configured Java matches the required major version.
     * @param majorVersion Required Java major version.
     * @param component Optional runtime component name from version.json
     * (for example "java-runtime-epsilon"), used to pick the exact
     * runtime Mojang ships for that game version.
     * @return True if matching Java is available.
     *
     * 这是**全局档**的老签名：挑中的路径写回本对象并落盘（= 注册表）。
     * 加了实例覆盖之后它变成 ensureJavaForVersion(EffectiveJavaSettings&, ...)
     * 的一层**转发壳** —— 保留它是为了让 E 段那几条断言、以及所有现存调用点不用改，
     * 而且全局档的行为逐字节不变。
     *
     * @param allowDownload 透传给下面那个重载（默认 true = 产品行为不变）；验证工程传 false
     *   表示"只在本机找，找不到就失败，不下载运行时"。理由见下面那个重载的 NOTE。
     */
    bool ensureJavaForVersion(int majorVersion, const QString &component = QString(),
                              bool allowDownload = true);

    /**
     * @brief 值版：在一份**给定的**有效值上挑 Java，挑中的路径写回 io。
     *
     * @param io 进参是这次要用的值，出参是挑完之后的值（javaPath 可能被换掉）。
     * @param majorVersion 需要的主版本。
     * @param component version.json 里声明的运行时组件名（可为空）。
     * @param allowAutoSelect 允许自动挑吗。false = **手动档**：io.javaPath 不匹配时
     * 不替用户换一个，直接失败返回 false。
     * @param errorOut 失败原因（给用户看的一句话）。
     * @param allowMismatchedPath 用户已经**明确授权**"就用这个不匹配的 Java 启动"。
     * 置真时 io.javaPath 非空即返回 true，一个探测都不做。
     * @return 拿到匹配的 Java 返回 true。
     *
     * NOTE: allowMismatchedPath **只能**由「Java 版本不符」那个对话框的「仍然用这个
     * Java 启动」置真，别的调用点一个都不许传。它判在 javaMatchesMajor() **之前**
     * 是有意的：那个函数总是冷启一个 JVM（几十上百毫秒），而这个口子开的理由是
     * "用户已经在框里表过态了"，没有理由再探一次。也正因为它在前，一个**压根不存在**
     * 的路径也能从这里返回 true —— 这是这个参数唯一可能的来源是"用户点的"的结构性
     * 证据。
     *
     * NOTE: 这个函数**不碰** m_javaPath、**不落盘**、**不改任何成员** —— 挑中的路径
     * 落在哪个作用域由调用方决定：全局档写回注册表，实例档写回那个
     * 实例的 wtfml.meta。这正是"跨作用域污染"在结构上不可能发生的原因：
     * 实例 A 自动挑中的 Java 绝不会写进全局配置，也就不会覆盖用户手填的全局路径。
     *
     * NOTE: 实例覆盖了 javaPath 就等于**手动档**，调用方要把 allowAutoSelect 传 false。
     * 不这么做的话，用户给某个实例钉死的 Java 会被"按版本要求自动挑"覆盖掉，
     * 那一项覆盖也就白设了。
     *
     * NOTE: `allowDownload` 是给**测试**留的闸（默认 true = 产品行为不变）：本机没有匹配
     * 版本时，这个函数最后会去 Mojang 下几百 MB 的运行时（再兜底 Adoptium）。验证工程里
     * 那一节直接调它，于是"跑单元测试"变成了"当场下一次 Java" —— CI 上慢网络跑满 900 秒
     * 的阶段上限，整个 ci-core 判死（2026-11-03 的那次）。传 false 时它只在本机找，
     * 找不到就带着原因返回 false，不碰网络。
     *
     * @param cancel 合作式取消位（可空 = 不可取消）。调用方在**别的线程**上置真时，
     * 下载循环在下一个数据块 / 下一个步骤上停下，删掉半截压缩包与这一次解出来的运行时
     * 目录，返回 false。本地扫描（含 Program Files）不打断 —— 它是一次几百毫秒的目录遍历，
     * 打断它只会留下更多要收的东西。启动路径用它接任务中心 / 主页卡片那颗「取消」
     * （见 `LaunchController::abortStart()`）；整合包导入那条路不传（默认空），行为不变。
     */
    bool ensureJavaForVersion(EffectiveJavaSettings &io, int majorVersion,
                              const QString &component, bool allowAutoSelect,
                              QString *errorOut = nullptr, bool allowMismatchedPath = false,
                              bool allowDownload = true,
                              const std::function<void(qint64 done, qint64 total)> &progress = {},
                              const std::atomic_bool *cancel = nullptr);

    /**
     * @brief Scans local installations for Java versions.
     * @return Map from major version to executable path.
     *
     * NOTE: 这是**同步**的，而且每个候选运行时都要认一次主版本（见
     * majorVersionFromReleaseFile()）。界面在切到设置页那一下调它，
     * 所以它花多少毫秒，界面就冻多少毫秒 —— 认版本那条路快不快要紧。
     *
     * NOTE: 它是 static 的：扫的是"这台机器上有什么"，和这个对象的设置（xmx、配的
     * Java 路径……）一个字都不相干。标注成 static 正是为了把这一点写死 ——
     * 顺手也让验证工程不必为了扫一遍而 new 一个对象（那个构造函数会去读用户的
     * 真实配置）。
     */
    static QMap<int, QString> scanLocalJavaVersions();
    /// 显式扫描根，供后台任务使用；不在工作线程读取可切换的 AppPaths 根。
    static QMap<int, QString> scanLocalJavaVersions(const QString &javaDir);

    /**
     * @brief 从 JDK/JRE 根目录那个 release 文件的内容里读出主版本。
     * @param releaseText release 文件的全文。
     * @return 主版本号；文件里没有 JAVA_VERSION、或者版本认不出来时返回 -1。
     *
     * 格式：release 文件是 KEY="value" 一行一条。Java 9+ 写 JAVA_VERSION="21.0.7"，
     * Java 8 那代写 JAVA_VERSION="1.8.0_422"（主版本在第二段）。
     */
    static int majorVersionFromReleaseFile(const QString &releaseText);

    /**
     * @brief 认出一个 java.exe 的主版本：先读 release 文件，读不到才真的跑一次 java -version。
     * @param javaExe java.exe 的完整路径。
     * @param versionOutput 退回探测时用来接 java -version 的原始输出（可以传 nullptr）。
     * @return 主版本号；exe 不存在、或者两种办法都认不出来时返回 -1。
     *
     * NOTE: 不是每个运行时都有 release。原版启动器那份 jre-legacy（Java 8 的老布局）就只有
     * bin/ 和 lib/，没有这个文件 —— Java 8 那代还没有这个惯例。所以读不到时**必须**
     * 退回真的跑一次，不能拿目录名（jre-legacy / java-runtime-delta）去猜主版本：
     * 目录名是别人起的，改名或者往里放了别的东西，猜出来的版本就是错的 ——
     * 而"静默选错 Java"正是这一层最怕的错。
     *
     * NOTE: 走 release 那条快路径时**不回填 versionOutput**（那里压根没有版本串可填），
     * 所以拿到 > 0 的返回值时 versionOutput 可能还是空的。调用方（启动前那个
     * 「版本对不上」的提醒框）必须兜住"只有主版本号"这一种。
     *
     * NOTE: 公开它是因为启动路径要用它来判断"实例钉的那个 Java 到底是哪个主版本"
     * （MainWindow::askAboutJavaMismatch()）。那个判断**不能**改用
     * javaMatchesMajor()：后者只回答"是不是这个版本"，而对话框里要写清"他钉的
     * 是哪个版本"，而且它总是冷启一个 JVM。
     */
    static int probeJavaMajorVersion(const QString &javaExe, QString *versionOutput = nullptr);

    /**
     * @brief 安装加载器要用的 Java。判据只要"够新"，不像启动那样要**精确**主版本。
     *
     * @param requiredMajor 版本 json 里 javaVersion.majorVersion 声明的主版本；
     * <= 0 表示不知道（原版还没装、读不到那份 json），此时原样返回 configuredPath。
     * @param configuredPath 「设置」里配的那份（空字符串 = 没指定，走 PATH 上的 java）。
     * @param javaPathOut 这次安装该用的路径（可能 == configuredPath，也可能是空）。
     * @param reasonOut 失败原因（给用户看的一句话）。
     * @return 能定下来用哪份就返回 true；配的那份太老、本机又找不到够新的，返回 false。
     *
     * 为什么不能用 ensureJavaForVersion()：那边的判据是 javaMatchesMajor()，也就是**主版本
     * 相等**（拿 Java 21 去跑要 Java 8 的 1.16.5 会 UnsupportedClassVersionError，所以启动
     * 那条路上"精确"是对的）。而安装器只要是够新的 JVM 就能跑，"精确"会把用户手里那份好用
     * 的 Java 判成不匹配 —— 配着 Java 21 去装 1.18.2 的 Forge，那条路会替他换一个 17 出来，
     * 还会顺手 adoptJavaPath() 把全局设置改掉。
     *
     * NOTE: 结果**只用于这一次安装**：不写回 m_javaPath、不落盘、不改任何成员。点一下
     * 「下载版本」就悄悄改掉用户的 Java 路径是说不通的（"挑中的路径写回调用方那一层"
     * 是启动的语义，只该用在启动上）。
     * NOTE: 不下载新运行时。downloadJavaRuntime() / downloadJavaFromMojang() 都是**同步**的
     * （QEventLoop + 阻塞式写盘），在下载页同步跑一次就是把界面冻住好几分钟 ——
     * 安装器是异步的，不能再在这儿开一个同步的重活。挑不到就报清楚。
     *
     * NOTE: static 是**有意的**，而且和上面那条"不改任何成员"是同一件事：它要用的东西
     * 全部从参数和这台机器上取，谁也没法通过它碰到这个对象的设置。编译期钉住。
     */
    static bool installerJavaFor(int requiredMajor, const QString &configuredPath,
                                 QString *javaPathOut, QString *reasonOut);

    /**
     * @brief 从候选里挑一份"够新"的 Java：主版本 >= requiredMajor 里**最小**的那个。
     * @param candidates 主版本 -> java.exe 路径。
     * @return 挑中的路径；一个都不够新时返回空字符串。
     *
     * 取最小而不是最大：越接近版本声明的那个主版本越不容易出意外（拿 Java 25 去跑一个只要
     * 17 的安装器是"能跑但没必要"）。
     *
     * 做成公开的纯函数是为了让验证工程能直接钉这张表（理由和 majorVersionFromReleaseFile()
     * 一样）—— installerJavaFor() 自己要看环境变量、扫目录，结果取决于跑它的那台机器，
     * 所以它那两条判据只能**在真机器上**验（verify 的 loader-install 模式里那两条）。
     */
    static QString pickMinimalJavaAtLeast(int requiredMajor, const QMap<int, QString> &candidates);

    /**
     * @brief Downloads a Java runtime from Mojang for the required version.
     * @param majorVersion Required Java major version.
     * @param javaPathOut 非空时写入下载并解压出来的 java.exe 路径。
     * @return True if the download succeeded.
     *
     * NOTE: 名字里的 "FromMojang" 与实际不符：**实际来源是 Adoptium**（api.adoptium.net
     * 的 v3 assets/latest API），不是 Mojang 的服务器。函数名先不改 —— 改名要动
     * 调用点，收益只是好看。
     * NOTE: 它**不**写 m_javaPath、**不**落盘：下载归下载、存哪由调用方决定（见
     * ensureJavaForVersion 的值版）。它若自己存进全局配置，"给某个实例下载一个运行时"
     * 就会顺手改掉全局的 Java 路径。
     * @param cancel 合作式取消位（可空 = 不可取消），同 ensureJavaForVersion 的值版。
     */
    static bool downloadJavaFromMojang(
        int majorVersion, QString *javaPathOut = nullptr,
        const std::function<void(qint64 done, qint64 total)> &progress = {},
        const std::atomic_bool *cancel = nullptr);

    /**
     * @brief Current maximum heap size in megabytes.
     */
    int xmx() const;

    /**
     * @brief Sets the maximum heap size in megabytes.
     * @param mbytes Heap size in megabytes.
     */
    void setXmx(int mbytes);

    /**
     * @brief Current minimum heap size in megabytes.
     */
    int xms() const;

    /**
     * @brief Sets the minimum heap size in megabytes.
     * @param mbytes Heap size in megabytes.
     */
    void setXms(int mbytes);

    // ------------------------------------------------------------------
    // 「内存分配」自动档：按系统内存算 Xmx/Xms
    // ------------------------------------------------------------------
    // NOTE: 「自动」是**每次要用的时候现算**，不是启动时算一次就存下来 —— 同一台机器
    // 开着浏览器和什么都没开，可用内存能差好几 GB，而 JVM 只认启动那一刻的机器状态。
    // 所以 LauncherManager 生成命令行之前会再算一次（见 MainWindow 里那处调用）。

    /// 自动档是不是开着。设置页那颗「自动分配 / 手动分配」下拉框读写的就是它；
    /// 老配置没有这个键时默认手动。
    bool autoMemory() const { return m_memoryAuto; }

    /// 设置自动档并立刻落盘。**不动** m_xmx/m_xms —— 值由 refreshSuggestedMemory() 管。
    ///
    /// 从手动切到自动的那一下会把当时的 Xmx/Xms 记成「上次手动档的现场」，
    /// 供 `restoreManualMemory()` 在切回手动档时原样还回去。
    void setAutoMemory(bool autoMemory);

    /// 上次开启自动分配之前的最大 / 最小内存（MB）。
    /// 切回手动档时恢复的就是这一对；盘上单独存着，重启之后也还在。
    int manualXmx() const { return m_manualXmx; }
    int manualXms() const { return m_manualXms; }

    /// 切回手动档：把 Xmx/Xms 还原成 `setAutoMemory(true)` 那一刻记下的值并落盘。
    void restoreManualMemory();

    /**
     * @brief 自动档下按当前系统内存重算一次 Xmx/Xms。
     *
     * 手动档下**什么都不做**（一个数字都不许碰），但仍然返回这次的系统读数 ——
     * 界面要拿它写"这台机器现在有多少内存"那句说明。
     *
     * @return 这次的系统内存读数（读不到时两个字段都是 0）。
     */
    SystemMemory::Usage refreshSuggestedMemory();

    /**
     * @brief 建议的最大内存（MB）。
     * @param totalMb 系统物理内存总量；<= 0 表示读不到。
     * @param availableMb 当前可用内存；<= 0 表示读不到。
     * @param modCount 这个实例装了多个模组；<= 0（默认）表示"不知道 / 原版"。
     * **全局那一档恒传默认值** —— 全局设置没有"哪个实例"这个概念，
     * 能给这个数的只有实例那一层（见 MainWindow::resolveMemoryForLaunch()）。
     *
     * 规则（纯函数，验证工程直接钉这张表，不用去问真的系统），按这个先后顺序：
     * 1. total/4 —— "这台机器该分多少给游戏"的经验值，**加上整合包规模那一档的加成**
     * （每 64 个模组 +512 MB，最多 +4096；见 .cpp 里 kAutoXmxModsPerStep 那段）
     * 2. available/2 —— "现在还剩多少能拿，留一半给别的程序"，和上面取小的
     * 3. 夹进 [1024, 8192]
     * 4. 再套物理内存一半的硬顶（小机器上这一步会压过下限，见下）
     * 5. 向下取整到 256 的整数倍，但不小于 256（JVM 侧 setXmx 的下限）
     *
     * 为什么是这几个数：
     * - 上限 8192：**只给自动档**（手动档能填到 65536）—— 自动算出来的数不该比用户
     * 自己敢填的更激进。整合包那档加成的天花板是 +4096，所以这个上限仍然够不着。
     * - 下限 1024：再小的 Xmx 连原版 1.20 都紧张。
     * - 4 排在 3 之后是有意的：2 GB 的机器给 1024 已经是一半内存了，1 GB 的机器只该给
     * 512 —— 在一条本来就紧张的机器上按"最少 1 GB"硬凑，只会让 JVM 连堆都开不起来。
     * - 256 的整数倍：和界面那两个框的步长一致，免得框里出现 3071 这种数。
     *
     * NOTE: 加成排在**第 1 步**而不是最后：它抬的是"目标值"，第 2、4 步那两条机器侧的
     * 硬顶（半个可用内存、半个物理内存）照旧说了算。把加成加在最后，等于让一个
     * 整合包把"留一半给别的程序"这条规矩踩过去。
     */
    static int suggestXmxMb(qint64 totalMb, qint64 availableMb, int modCount = 0);

    /**
     * @brief 建议的最小内存（MB）：Xmx 的四分之一，夹在 [512, Xmx]。
     * @param xmxMb 已经定下来的最大内存。
     *
     * Xms > Xmx 会让 JVM 拒绝启动（Initial heap size ... larger than the maximum
     * heap size），所以 Xmx 比 512 还小时下限跟着退到 Xmx 上（setXmx 的下限是 256，
     * 256 那档就返回 256）—— 返回值**永远不会**大于 xmxMb。
     */
    static int suggestXmsMb(int xmxMb);

    /**
     * @brief Returns extra JVM arguments configured by the user.
     *
     * 这就是 PCL「自定义 Java 启动参数」的**全局**入口（持久化键 Java/ExtraArgs，
     * 默认 -XX:+UseG1GC）。在 LauncherManager::generateJvmArguments() 里它是先入列
     * 的那一份，"按版本"的那份（wtfml.meta 的 jvmArgs）排在它之后，所以同一条选项
     * 写两遍时单个实例胜出。
     *
     * NOTE: 界面入口在设置页「启动」栏里的「JVM 参数」那一行（ui/MainWindowUI.cpp 的 extraJvmArgsEdit
     * ＋旁边那个「应用」按钮），经 MainWindow::onApplyExtraJvmArgsClicked() 落盘。
     * 它**不跟 textChanged 存**，必须点「应用」才进配置 —— 免得半条参数进了命令行。
     */
    QString extraJvmArgs() const;

    /**
     * @brief Sets extra JVM arguments.
     * @param args Additional arguments string.
     */
    void setExtraJvmArgs(const QString &args);

    /**
     * @brief Loads persisted Java settings.
     */
    void load();

    /**
     * @brief Saves current Java settings persistently.
     *
     * NOTE: 和 `LauncherSettings::save()` 同一条规矩 —— **只写本进程改过的那几项**，
     * 没动过的一项都不碰。全局设置那棵注册表是按机器共用的：装在两个不同目录的启动器
     * 可以同时开着，整份写回会把另一份刚改的 Java 路径 / 内存打回自己启动时读到的旧值。
     * 哪几项动过由 `load()` 留下的快照比出来（见 .cpp 里 `persistedValues()`）。
     */
    void save() const;

private:
    /**
     * @brief 当前可持久化的那几项，**按写进 QSettings 的形态**摆成一份表。
     *
     * 键名 → 值映射的唯一出处：`load()` 用它记快照，`save()` 拿它逐项比、只写变了的。
     * 两边共用一份表，就不会出现"save() 里漏一个键、那一项永远存不下来"。
     */
    QVariantMap persistedValues() const;

    /// `load()` 读完之后（= 上次落盘）那一份值。`save()` 逐项与它比，只写不同项。
    /// mutable：`save()` 是 const，但写完要把快照推进到新值。
    mutable QVariantMap m_persisted;

    QString m_javaPath;
    int m_xmx;
    int m_xms;
    QString m_extraJvmArgs;
    /// 「Java 版本」选的是不是自动档。默认 true —— 老配置文件（没有这个键）读回来也是自动。
    bool m_autoSelectJava = true;
    /// 「实例指定的 Java 版本不符时提醒」这个总闸。默认 true —— 该提醒就提醒；
    /// 老配置读回来同样如此。
    bool m_warnVersionMismatch = true;
    /// 「内存分配」选的是不是自动档（按系统内存算）。默认手动 —— 老配置没有这个键。
    bool m_memoryAuto = false;
    /// 上次开启自动分配之前那一对手动值（切回手动档时原样还回去）。
    /// 初值和 `.cpp` 里的内置默认一致（4 GB / 1 GB）；load() 会按盘上那份覆盖。
    int m_manualXmx = 4096;
    int m_manualXms = 1024;
};
