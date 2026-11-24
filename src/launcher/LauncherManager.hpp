/**
 * @file LauncherManager.hpp
 * @brief 为 JVM 参数生成和游戏进程管理进行声明。
 * @note 负责 Minecraft 启动参数构建和进程生命周期管理。
 */
#pragma once
#include "util/SecretRedactor.hpp"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringConverter>
#include <QStringList>

#include <functional>
#include <memory>

#include "../settings/EffectiveJavaSettings.hpp"
#include "../settings/GameWindowSettings.hpp"
#include "ProcessPriority.hpp"
#include "QuickPlay.hpp"

/**
 * @brief 处理 Minecraft 的 JVM 参数生成和启动过程。
 *
 * 构建 classpath, 应用平台特定规定，使用配置的 Java运行时环境 启动 Minecraft 进程。
 *
 * 【同时管着好几个游戏进程】**一次 startGame() = 一份会话**，本类可以同时持有好几份
 * （`m_sessions`，每份一个 QProcess）。这是"多账户多实例"那件事的落点：用户想让同一个
 * 实例用两个账号各开一个、或者一个账号开两个不同实例，都不该被启动器拦住 —— 一个
 * java.exe 活不活得下去是游戏自己的事（同一个游戏目录开两份会不会坏档由用户判断），
 * 启动器的职责只是**不替用户做这个决定**，并且把每一份都如实报出来。
 *
 * 于是本类里**没有**"已经在跑了所以不能再起"这条早退路径（见 runningGames() 的说明）。
 * 取而代之的是三条不变式：
 * 1. 每个进程的输出/结束都带着自己的 sessionId 往外发，界面按 id 归属到具体那一份；
 * 2. 编码方式（-Dstdout.encoding）**按会话各记一份** —— 两份会话用的 Java 主版本可能
 * 不同，共用一份状态就会有一份的中文变乱码；
 * 3. startGame() 返回的是"这次会话的 id"，不是"成功了没有"。0 才是失败。
 */
class LauncherManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 一份游戏会话：一个跑着的（或者刚起来又立刻没了的）游戏进程。
     *
     * 这几个字段全是**启动那一刻**定下来的快照，之后不再变。这样做是有意的：实例可以在
     * 游戏跑着的时候被改名、账号可以被切换，而"当初是用哪个实例、哪个身份起的"是历史
     * 事实，界面要报的就是那个事实（底部那句"XXX 运行中"改了名之后跟着变会让人以为
     * 跑的是另一个进程）。
     */
    struct GameSession {
        /// 会话号，从 1 开始。0 是"没有会话"的哨兵值（startGame() 失败时返回它）。
        int sessionId = 0;
        /// 这次启动的版本号（= `versions/` 下面那个目录名，也就是实例的 id）。
        QString versionId;
        /// 启动那一刻这个实例的**显示名**（可能带自定义名/加载器名，见 MainWindowUI）。
        QString displayName;
        /// 这次启动用的身份（账号名）。离线模式/未登录时是 AuthManager 给的默认名。
        QString accountName;
        /// 这次启动用的 UUID（离线为正，可能为空 = 调用方没给）。
        QString accountUuid;
        /// 操作系统给的进程号。给界面显示用（"游戏已启动（进程ID：…）"）。
        qint64 processId = 0;
        /// 进程起来那一刻的 QDateTime::currentMSecsSinceEpoch()。
        qint64 startedAtMs = 0;
        /// 这次启动用的堆上限（MB）。低内存提醒要把"已开着的"也合计进去。
        int xmxMb = 0;
    };

    /**
     * @brief 建立一个启动管理实例。
     * @param parent Parent QObject.
     */
    explicit LauncherManager(QObject *parent = nullptr);

    /**
     * @brief 同时最多允许几份会话。
     *
     * 这不是"性能上限"，只是一个**防手滑**的阀门：界面上连点几下、或者脚本里循环调用，
     * 该有个地方拦住。8 这个数字是拍的 —— 一台能玩 Minecraft 的机器同时开八个
     * java.exe 已经远超"多开"的合理用法，真撞到它的人知道自己要什么（改成自己的构建
     * 即可）。撞上时 startGame() 会以 return 0 收尾并往日志里说一句。
     */
    static int maxConcurrentSessions() { return 8; }

    /**
     * @brief 在启动前验证游戏文件。
     * @param versionId Minecraft version identifier.
     * @param errors Optional output list for error descriptions.
     * @param versionJson Version manifest, used to check the natives requirement.
     * @return 若通过启动前检查通过，则返回 true.
     */
    bool checkPreLaunch(const QString &versionId, QStringList *errors = nullptr,
                        const QJsonObject &versionJson = QJsonObject()) const;

    /**
     * @brief 按需把旧格式的 natives classifier 解压到该版本自己的目录。
     * @param versionJson Version manifest containing library dependencies.
     * @param versionId Minecraft version identifier.
     * @param errors Optional output list for error descriptions.
     * @return 返回 natives 目录的绝对路径；该版本不需要 natives，或准备失败时返回空。
     *
     * NOTE: 解压只在该目录**整套解完过**（`natives/` 里那个完成标记 + DLL 都在）时才
     * 跳过，所以除第一次启动外几乎零开销；解压中途失败会把整个目录删掉，下一趟从头来
     * （只判"有一个 .dll"会把残缺的一套当成齐了，崩在 UnsatisfiedLinkError）。
     */
    QString prepareNatives(const QJsonObject &versionJson, const QString &versionId,
                           QStringList *errors = nullptr,
                           const std::function<void(int done, int total)> &progress = {}) const;

    /**
     * @brief 为 Minecraft 启动生成 JVM 参数。
     * @param token Authentication token.
     * @param versionId Version identifier.
     * @param assetIndexId Asset index identifier.
     * @param versionJson Version JSON manifest.
     * @param settings 这次启动**真的要用的**那一套 Java 值（全局按实例覆盖合成之后
     * 的结果，见 resolveJavaSettings()）。收 EffectiveJavaSettings 而不是
     * GlobalJavaSettings 是刻意的：启动器只该知道"这次用什么值"，
     * 不该知道它是全局档还是某个实例的覆盖 —— 那样每加一层作用域都要改这里。
     * @param offlineMode Whether to launch in offline mode.
     * @param javaMajorVersion 实际使用的 Java 主版本，用于过滤该版本不支持的 JVM 选项。
     * @param versionJvmArgs 该版本自定义的 JVM 参数（wtfml.meta 的 "jvmArgs"），
     * 来自 VersionManager::versionJvmArgs()。插在全局
     * GlobalJavaSettings::extraJvmArgs() **之后** —— JVM 参数后写的胜出，
     * 所以单个实例能盖掉全局默认那套（例如全局写死了 -XX:+UseG1GC）。
     * @param playerName 玩家名，写进 --username。离线与正版两条分支**都用这一个来源**
     * （调用方传 AuthManager::playerName()）—— 不能两条分支各写各的（正版写死
     * "Player"、离线拼一个时间戳）。空 = 调用方没给（测试、未登录），
     * 退回默认名。**不能发一个空的 --username 给游戏**，那样游戏会拒绝启动，
     * 所以空值是"退回默认"而不是"原样传下去"。
     * @param playerUuid 正版分支的 --uuid（调用方传 AuthManager::playerUuid()，带横线的
     * 标准写法）。空 = 没有档案（没登录、或者测试没给），那时退回全零 UUID ——
     * 那在正版服上等于一个"谁都不是"的身份。离线分支**不用**它（那边按名字推）。
     * @param window 这一次启动要用的窗口那一组（模式 + 分辨率）。默认是"小窗 + 原版默认
     * 的 854×480"。
     * @param screen 屏幕工作区（像素）。**只有「最大化」那一档用得上**：那一档原版没有对应
     * 参数，是照工作区算一个大小再当 --width/--height 发下去的（见
     * GameWindowSettings.hpp）。<= 0 = 调用方没给（验证工程那些纯值的构造点），
     * 那一档这时退回 window 里那两个数。
     * @param quickPlay 这一次要不要"开游戏直接进某个存档 / 直接连某台服务器"
     * （`--quickPlaySingleplayer` / `--quickPlayMultiplayer`）。默认是**没有这一发**
     * （`Target{}`），命令行上因此多不出任何东西 —— 那是绝大多数启动的处境。
     * 这一步**必须由启动器自己发**：版本 json 里那两条在
     * `VersionArguments::isVanillaGameArgument()` 的名单上，json 声明的那一条会被
     * 整条丢掉（和 `--width`/`--height` 同一档）。判据和参数名见 `QuickPlay`。
     * @return 返回 JVM 参数列表。
     *
     * NOTE: 调用方传 javaMajorVersion <= 0（未传）时按"**沿用旧行为**"处理，不是"一条都
     * 不加"：17 门槛的 --enable-native-access 在 <=0 时**照样加**（cpp 里的判断是
     * `>= 17 || <= 0`，旁边注明 0 表示调用方没传）；不加的只有 19 门槛的
     * -Dstdout/stderr.encoding 和 25 门槛的 --sun-misc-unsafe-memory-access。
     */
    QStringList generateJvmArguments(const QString &token, const QString &versionId,
                                     const QString &assetIndexId, const QJsonObject &versionJson,
                                     const EffectiveJavaSettings &settings,
                                     bool offlineMode = false, int javaMajorVersion = 0,
                                     const QStringList &versionJvmArgs = QStringList(),
                                     const QString &playerName = QString(),
                                     const QString &playerUuid = QString(),
                                     const GameWindowSettings &window = GameWindowSettings(),
                                     const ScreenWorkArea &screen = ScreenWorkArea(),
                                     const QuickPlay::Target &quickPlay = QuickPlay::Target()) const;

    /**
     * @brief 启动一个 Minecraft 游戏进程（新增一份会话）。
     * @param arguments JVM and game arguments.
     * @param settings 这次启动要用的 Java 值（只读它里面的 javaPath；const 引用
     * 是安全的，这里从来不改设置）。
     * @param versionId Version identifier.
     * @param displayName 启动那一刻这个实例的显示名。空 = 退回 versionId。
     * @param accountName 这次启动用的身份。空 = 记不着（未登录、测试），界面就不报身份。
     * @param accountUuid 这次启动用的 UUID。空 = 调用方没给（测试、离线时也可能是正随机值）。
     * @param priority 给这个游戏进程设的 CPU 优先级。默认 Normal（= 不设也一样的结果），
     * 见 ProcessPriority 那段 NOTE：这里**没有**"不设置"这一档，每次都会显式设一遍。
     * 非 Windows 平台上这个参数被忽略（没有任何等价物）。
     * @return **这次会话的 id**（> 0）；没能起来时返回 0。
     *
     * NOTE: 返回值就是会话号 —— 语义是 `sessionId > 0` 就等于"进程真的
     * 起来了"。三条早退路径（找不到 Java 可执行文件、waitForStarted 超时、
     * 会话数到顶）一律 return 0，且**都不会**留下会话。调用方拿它决定要不要把
     * 这个版本记成"上次启动的版本"，所以失败时绝不能返回非零。
     *
     * NOTE: 返回非零只代表"java 进程起来了"，不代表"游戏没崩"。后面游戏因为库不全
     * 之类的原因立刻退出时会话照样在（界面上会闪一下"运行中"再收掉），这是刻意
     * 的：那种情况恰恰最需要下次打开还选着这个版本。
     */
    int startGame(const QStringList &arguments, const EffectiveJavaSettings &settings,
                  const QString &versionId, const QString &displayName = QString(),
                  const QString &accountName = QString(), const QString &accountUuid = QString(),
                  ProcessPriority priority = ProcessPriority::Normal);

    /**
     * @brief 现在有哪些游戏进程**真的在跑**。
     * @return 按会话号升序（= 启动顺序）排好的那一串。
     *
     * 判据是"那个 QProcess 的状态不是 NotRunning"，不是"会话表里有几项" —— 两个不一样：
     * 正在启动中（还没起来）和刚失败要收掉的会话都在表里，但它们不该被报成"运行中"。
     * 会话表在 startGame() 开头就登记，就是为了让那三条早退路径上的报错**能带上实例名**。
     */
    QList<GameSession> runningGames() const;

    /// 有几个游戏进程在跑。底栏那句"实例运行中"/"N 个实例运行中"就靠它。
    int runningCount() const;

    /// 现在有没有游戏进程在跑。给"关闭启动器"那条路用（见 closeEvent）。
    bool hasRunningGame() const;

    /// 某个实例是不是正在跑（可能不止一份会话在跑它）。重命名那道闸口靠它。
    bool isVersionRunning(const QString &versionId) const;

    /**
     * @brief 按会话号取那一份会话的快照。
     * @return 找不到那个会话（已经结束了）时返回 sessionId == 0 的空结构。
     *
     * NOTE: 这个查的是**会话表**，不看进程状态 —— 刚开始启动、以及即将被收掉的那一瞬间
     * 也查得到。界面用它把一条输出归属到实例名上，那两种情况恰恰都要能查到。
     */
    GameSession session(int sessionId) const;

    /**
     * @brief 停掉某一份会话：**强杀**那个游戏进程。
     * @param sessionId 会话号。
     * @return 真发出了终止请求返回 true；那个会话已经不在（或者本来就结束了）返回 false。
     *
     * NOTE: 这是 kill()，不是"请游戏自己退出" —— Minecraft 没有能被外部优雅关闭的接口，
     * 用户从启动器上按"停止"要的就是"现在停"。界面上这句话必须说清楚（见
     * MainWindow::confirmStopGame() 那个确认框），别让人以为是"保存并退出"。
     */
    bool stopGame(int sessionId);

    /// 把所有正在跑的游戏进程**交出去**：脱离本对象的所有权，之后本对象析构也不会碰它们。
    ///
    /// 为什么必须有这一步：QProcess 的析构（Qt 6.10，qprocess.cpp）在进程还跑着的时候会
    /// `kill()` + `waitForFinished()`。游戏进程是本类的**子进程**，而 LauncherManager
    /// 又是 MainWindow 的子对象、MainWindow 在 main() 里是栈对象 —— 于是"关掉启动器"这条
    /// 最平常的操作会走到那段析构里，把游戏**强杀**（Windows 上 TerminateProcess，游戏连
    /// 存档都来不及存）。java.exe 本来活得过启动器，这一步就是让它活得下去。
    ///
    /// NOTE: 没有游戏在跑时是**空操作**（这条路径上不该出任何提示）。**交出去的是全部** ——
    /// 同时开着三份时，关掉启动器之后三份都得继续跑。
    void detachRunningGame();


    /**
     * @brief 从最终命令行判断游戏进程的输出是不是 UTF-8。
     * @param arguments 交给 QProcess 的完整参数列表。
     * @return 命令行里 -Dstdout.encoding 的值是 utf-8（不区分大小写）时返回 true。
     *
     * 这是解码侧（decodeGameOutput）的**唯一**判据，而且是从实际命令行反推的 ——
     * 不另立一个"我们加没加编码参数"的开关，两处各记一份状态迟早会失配（加参数的
     * 分支改了、解码的分支忘了改，就是一个只在中文 Windows 上出现的乱码 bug）。
     * 做成静态纯函数是为了让验证工程能直接断言这条不变量的两端。
     */
    static bool commandDeclaresUtf8Output(const QStringList &arguments);

signals:
    void gameStarted(int sessionId);

    /// 某份会话的游戏输出（stdout）。
    /// @param sessionId 哪一份会话。**startGame() 失败那几条路也会带号发出来**
    /// （那时进程根本没起来），界面据此把报错挂到实例名上，所以别假定
    /// "收到这个信号的 sessionId 一定在 runningGames() 里"。
    void processOutput(int sessionId, const QString &text);
    /// 同上，stderr。
    void processError(int sessionId, const QString &text);
    /// 一份会话在**真的起来之前**就被收回（`startGame()` 的三条早退路径：会话数到顶、
    /// 找不到 Java、进程起不来）。号码和上面那几条信号对得上 —— 收信号的槽据此把"刚为它
    /// 建出来、但从此再也不会更新的"那些东西收掉（日志窗、按号记的底账）。
    ///
    /// NOTE: 那三条路是**先**带号发一句 `processError`、**再**发这一条（顺序固定）：报错
    /// 那一行因此还能挂上实例名（界面按号查显示名），收尾这一步只负责清理。
    void sessionDiscarded(int sessionId);
    /// 某份会话的游戏进程结束了。发之前那份会话已经从表里摘掉 ——
    /// 收信号的槽去查 session(id) 会查不到，要用的东西请从参数/自己那边拿。
    void gameFinished(int sessionId, int exitCode, QProcess::ExitStatus exitStatus);

private:
    /// 一次游戏会话的内部记录（对外的快照是 GameSession）。
    struct Entry {
        GameSession info;
        QProcess *process = nullptr;
        /// 这份会话的输出是不是 UTF-8（= 它的命令行里有 -Dstdout.encoding=utf-8）。
        /// **按会话各记一份**：两份会话用的 Java 主版本可能不同（同一台机器上装了好几个
        /// Java 是常态），共用一份状态就会有一份的中文变乱码。
        bool outputIsUtf8 = false;
        /// 为什么要它：`readyRead` 是**按管道里的字节块**来的，一次读到的末尾完全可能
        /// 正落在**一个汉字的两个字节中间**（Java ≤18 走本地代码页 = 中国区 GBK，那更是
        /// 两字节一个汉字）。每块各自 `QString::fromLocal8Bit(块)` 会让前半截和后半截
        /// 各解出一个替换字符，日志里零星多出乱码。QStringDecoder 是**有状态**的，
        /// 末尾那半个字节留着等下一块拼上。
        std::shared_ptr<QStringDecoder> stdoutDecoder;
        std::shared_ptr<QStringDecoder> stderrDecoder;
        SecretRedactor::Stream stdoutSecrets;
        SecretRedactor::Stream stderrSecrets;
    };

    /// 按某份会话**实际使用的**输出编码解码它的 stdout/stderr。
    /// 判断依据由 startGame() 从命令行上反推出来，见那里的说明。
    static QString decodeGameOutput(bool utf8, std::shared_ptr<QStringDecoder> &slot,
                                    const QByteArray &bytes);

    /// 把一个已经结束/失败的会话从表里摘掉并释放它的 QProcess（进程指针从会话自己那一格
    /// 上取，这里不需要外面再传一个）。
    /// @param sessionId 要收掉的那个会话号。
    /// @param abandoned 这一份**从没真的起来过**（`startGame()` 的三条早退路径）⇒ true：
    /// 摘完再发一条 sessionDiscarded，让界面把刚为它建出来的日志窗/底账收干净。
    /// 从 handleProcessFinished() 来的那一路**必须留在 false** —— 那一份的收尾归
    /// gameFinished（日志窗关不关、崩溃那几行留不留全在那边判），在这里抢先发
    /// sessionDiscarded 会把崩溃现场一起清掉。
    void discardSession(int sessionId, bool abandoned = false);

    void handleReadyReadStandardOutput(int sessionId);
    void handleReadyReadStandardError(int sessionId);
    void handleProcessFinished(int sessionId, int exitCode, QProcess::ExitStatus exitStatus);

    /// 会话号 -> 会话。**包含正在启动和即将收掉的**，见 runningGames() 的说明。
    QHash<int, Entry> m_sessions;
    /// 下一个会话号。从 1 开始，0 留给"没有会话"。
    int m_nextSessionId = 1;
};

