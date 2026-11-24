#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

/**
 * @file DownloadManager.hpp
 * @brief WTFML的下载与版本列表功能声明。
 * @note 定义版本和 asset 的数据结构和信号。
 *
 * 负责获取 mojang 版本列表、 library 下载、 asset 和客户端 jar 文件、验证 SHA1 完整性。
 */

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QSharedPointer>
#include <functional>
#include "NetworkManagerFactory.hpp"

#include "DownloadConcurrency.hpp"  // 「同时最多下几个」那四档（唯一出处）
#include "DownloadSpeedLimit.hpp"   // 「下载速度上限」那几档（唯一出处）
#include "LocalFileReuse.hpp"       // 「从别的游戏文件夹里借一个一模一样的文件」

class QTimer;
class QThreadPool;
class StreamedFile;
class ResumableDownload;
class DownloadCheckpoint;

struct DownloadRange {
    qint64 first = -1, last = -1, total = -1;
    QByteArray validator;
    bool active() const { return first >= 0 && last >= first && total > last; }
};

// Minecraft 数据的结构
struct MinecraftVersion {
    QString id;
    QString type;
    QString url;
    QString time;
    QString releaseTime;
};

struct Library {
    QString name;
    QString url;
    QString sha1;
    qint64 size = 0;
    QString path;
    // 下面两个字段由 extractLibraries() 从 LibraryRules::nativeClassifier() 的结果填好。
    // 产品代码里下载和启动都不看它们（natives 的解压在 LauncherManager::prepareNatives()，
    // 那边自己重新解析一遍 version.json），但**验证工程会读 isNative**（拿它核对"这个库
    // 是 natives classifier 还是普通库"），所以别按"没人用"删掉。
    //
    // 1.18.2 及以下的 natives 是 downloads.classifiers 里的独立 jar，
    // 它不进 classpath，而是启动时解压到 versions/<id>/natives/。
    bool isNative = false;
    // 该 jar 内需要跳过的条目前缀，来自 library 的 extract.exclude（通常是 META-INF/）。
    // 真正用它的地方是 prepareNatives（从 version.json 重新算一遍），这个字段本身
    // 目前**只写不读**。
    QStringList extractExclude;
};

struct AssetIndex {
    QString id;
    QString sha1;
    qint64 size;
    QString url;
    qint64 totalSize;
};

/**
 * @brief 决定一个已经存在于磁盘上的文件要不要重新下载。
 *
 * 重新点"下载"时用它来跳过已经完好的文件，避免整包重下。
 */
enum class VerifyMode {
    AlwaysDownload,
    ExistsOnly,   // 文件存在就算好（用于海量 asset，全量哈希会让界面卡住几秒）
    SizeAndSha1,  // 先比字节数，再比 SHA1（用于客户端 jar 和 library）
};

/**
 * @brief 一批下载任务的统计，供 UI 显示进度和汇总失败原因。
 *
 * 两分法的口径：**只有"真要下"的那几条进 total/done**。
 * 进度条显示缺少的文件的下载进度，而不是实例文件总数。
 *
 * · `total` / `done` / `failed` —— 这一批**真的要从网上拿**的文件。进度条画的就是它，
 * 底栏那一格念的「已下载 x / n」也是它。于是它等于"缺的那几个 / 坏的那几个"，而不是
 * "这个实例一共有多少个文件" —— 后者才是那个让用户看不懂的数（一个装好的实例点一次
 * 补全，分母 5000、分子 1，谁看都像坏了）。
 * · `skipped` —— 问过盘、本来就好好的那几个。**不进 total**：它们的字节一个都没动，算进
 * 分母只会让进度条在开跑那一瞬间就满、收尾时又读成"n 个里面 n-1 个已存在"。它不是
 * "这一批做了什么"，是"这一批省下了什么"，只在收尾那句话里报一个数。
 * · `reused` —— 目标本来不在，但**别的游戏文件夹里有一份一模一样的**。
 * 同样**不进 total**：它没走网络，进度条上也不该占一格；收尾那句话单独报一个数。
 * 判据比 `skipped` 严：`SizeAndSha1` 那一档要求来源的字节数 + SHA-1 都对得上，
 * `Exists` 那一档和盘上资源对象一个信任级别（文件名就是 SHA-1，不重算哈希）。
 * · 资源索引（`assets/indexes/<id>.json`）**两边都不算**：它每次都要重取（是"清单"不是
 * "文件"，见 downloadAssets），让它占一格的话，一个什么都没缺的实例点补全也会报
 * "下载了 1 个"。
 */
struct DownloadStats {
    int total = 0;    // 本批**真要下**的任务数（已存在而跳过的不算）
    int done = 0;     // 其中已结束的（成功、失败都算）
    int failed = 0;   // 其中失败的
    int skipped = 0;  // 因为文件已存在而跳过的（**不在 total 里**）
    int reused = 0;   // 从别的游戏文件夹硬链接 / 复制过来的（**不在 total 里**）
    QStringList failedPaths;  // 失败文件的完整路径，比 40 位哈希文件名可读
};

class DownloadManager : public QObject {
    Q_OBJECT

public:
    explicit DownloadManager(QObject *parent = nullptr, NetworkManagerFactory networkFactory = {});
    ~DownloadManager();

    // ------------------------------------------------------------------
    // 下载源：官方 / 镜像
    // ------------------------------------------------------------------
    // 界面那边是「下载源」下拉框（LauncherSettings::DownloadSource，存 QSettings），
    // 这里只留一个布尔：DownloadManager 不需要知道这个设定存在哪、怎么持久化。
    // 设置是**发请求那一刻**读的，所以改完立刻对下一次下载生效，不用重建对象。

    /// @param useMirror true = 走镜像源（BMCLAPI，第三方服务）。
    void setUseMirror(bool useMirror) { m_useMirror = useMirror; }
    bool useMirror() const { return m_useMirror; }

    /// 版本清单的地址覆盖（**只有验证工程会填**）。空 = 官方地址；非空时**不再走镜像**。
    void setVersionManifestUrlOverride(const QString &url) {
        m_versionManifestUrlOverride = url.trimmed();
    }

    /// 现在这一刻还该不该走镜像 = 用户选了镜像源，**而且**镜像没因为太慢被临时降级。
    /// 凡是"要不要换成镜像地址"的判断都问它，别直接读 m_useMirror —— 降级是运行期发生的，
    /// 直接读会绕过它。
    ///
    /// 为什么要降级而不是"镜像慢就等着"：镜像的**非 maven 路由**一律
    /// 302 到 `minio.749333.xyz`（预签名 URL，60 秒过期），那条链子可能只有几 KB/s，
    /// 而同一时刻官方那条快一个数量级。更要命的是 Qt 的 setTransferTimeout() 是"不活动
    /// 超时"，细水长流地掉字节永远重置它 —— 于是既不失败（等不到兜底）也完不成，整条安装
    /// 挂着不动。
    ///
    /// NOTE: 这个函数的调用点全是"镜像地址由本类自己拼出来"的那几处（endpoint()、
    /// fetchVersionJson()、资源索引、客户端 jar），它们的镜像主机**都是 BMCLAPI 那一个**
    /// —— 所以它问的就是那一个主机的降级状态（见 mirrorDemotedFor()）。
    /// 整合包那条路不一样：它的首选地址是清单里给的（每条文件一个），那一条走
    /// startDownload() 里按主机问的判据。
    bool mirrorPreferred() const;

    /// 镜像源（BMCLAPI）那个主机是不是正处在"太慢、先不用"的降级期里。
    /// 见 mirrorDemotedFor() —— 降级是**按主机**记的，这一条问的就是镜像源那个主机。
    bool isMirrorDemoted() const;

    /// **某一个主机**正不正在降级期里。
    ///
    /// 降级为什么必须按主机记：整合包里的"镜像"是**每条文件
    /// 各自的首选地址**（Modrinth 的清单给 2~3 条地址，`preferredUrlOrder()` 把
    /// `cdn.modrinth.com` 排前面），不是全启动器共用的那一个 BMCLAPI。一个全局的降级位
    /// 意味着一件事：**一个慢 CDN 上的几条回复，会把别的 CDN 一起关掉**。
    ///
    /// @param host 主机名（空串 = 查不到，返回 false）。
    bool mirrorDemotedFor(const QString &host) const;

    /// 把**这个主机**降级一段时间：这期间"首选地址是这个主机"的作业直接走备用地址。
    /// 由失速看门狗按**失速那一条回复自己所在的主机**调用；验证工程也直接调它
    /// （`verify.exe` 的 V6e 那几条，本地造不出"慢到触发看门狗"的服务器）。
    /// NOTE: 重复调用会把期限**往后顺延**（不是每叫一次重开一段），所以连着失速只会让
    /// 这段降级更长，不会来回切。顺延是**按主机**各自算的。
    void demoteMirrorForSlowDownload(const QString &host);

    /// 降级期多长：5 分钟。短了会在"镜像只是刚才那一条慢"上反复切换，长了会在镜像已经
    /// 好了以后还用不上它。
    static constexpr qint64 kMirrorDemoteMs = 5 * 60 * 1000;

    /// 改道官方之后**连着几条连不上**才撤销降级：3 条。
    /// 1 条太少（偶发一次超时很常见）；等太多条的话，在"官方整个不通"的网络上前几百个
    /// 作业已经死在那边了，撤得再早也来不及。
    static constexpr int kReroutedFailuresToUndoDemotion = 3;

    // ------------------------------------------------------------------
    // 镜像 302 之后的**落点**有几台是死的
    // ------------------------------------------------------------------
    // BMCLAPI 的资源地址每条都 302 到它背后临时挑的一台镜像（minio.749333.xyz /
    // bmclapiN.sakura.sld.tw / hurtmc ...）。有的**TLS 接得上、一个字节都不给**，
    // 有的落到 minio 很快就完。失败的是"这一发被分到哪台落点"，
    // 跟入口 bmclapi2 没关系。
    //
    // 只靠 kNoProgressGiveUpMs（90 秒）那条兜底的话，死落点会把一条并发位占满
    // 90 秒才重排，24 位里常年有几条在等死节点 ⇒ 整批只剩一两文件/秒。而同一时刻
    // 官方 resources.download.minecraft.net 在**同一条网**上几秒就能下一个对象。
    //
    // 所以补三条判据（都不看"是镜像还是官方"这种静态偏好，只看当下谁通）：
    // 1) 走镜像、有退路、**一个字节都没收到**的回复，12 秒就换 —— 不等 90 秒；
    // 2) 落到一台刚证明过是死的落点主机上 ⇒ 当场 abort，直接重新掷落点（约一个往返）；
    // 3) 短时间内攒够几条"落点死了"的证据 ⇒ **先探一下官方**（HEAD），探得通才把
    // 整条镜像降级 5 分钟；探不通（系统代理那条网络）就继续用镜像，不瞎切。
    // 降级之后官方要是也连不上，仍由 kReroutedFailuresToUndoDemotion 那条撤销。

    /// 走镜像、有退路、还没收到第一个字节的回复最多等多久（毫秒）。
    static constexpr qint64 kNoFirstByteGiveUpMs = 12 * 1000;

    /// piston-meta / piston-data 那几条**只走官方**；而 BMCLAPI 的版本 json
    /// 路由可能「12 秒一个字节都不给」，abort 之后官方几百毫秒就回来了。元数据只有几十 KB、
    /// 又是整条安装的必经之路，等 12 秒毫无收益 —— 换路重取一次的成本比等它低一个数量级。
    /// 资源对象**不**用这条：那些文件大、换路要重下几十 MB，12 秒（kNoFirstByteGiveUpMs）
    /// 就是平衡点。
    ///
    /// NOTE: 验证工程压 `setNoFirstByteGiveUpMs()` 时元数据期限跟着它一起降（看门狗里取
    /// 两者的较小值），否则逐条用例会被这 4 秒拖住。
    static constexpr qint64 kMetadataNoFirstByteGiveUpMs = 4 * 1000;

    /// 一台落点主机被判死之后，多久之内再转到它就直接 abort（毫秒）。
    /// 两分钟：BMCLAPI 的轮换大概就是分钟级的，久了会一直错过刚恢复的那台。
    static constexpr qint64 kDeadRedirectHostBanMs = 2 * 60 * 1000;

    /// 窗口内攒到几条"落点死了"的证据就探一次官方。
    static constexpr int kDeadRedirectHitsToProbe = 4;

    /// 上面那个窗口（毫秒）。
    static constexpr qint64 kDeadRedirectWindowMs = 90 * 1000;

    /// 官方探测失败之后，多久之内不再探（毫秒）—— 探测本身也是一次真实请求。
    static constexpr qint64 kOfficialProbeCooldownMs = 2 * 60 * 1000;

    /// 三个**纯函数**（时间与字节数的判据），verify 里按边界钉过。
    /// @{
    /// 降级期过了没有。@param demotedUntilMs 0 = 从来没降级过。
    static bool mirrorDemotedAt(qint64 nowMs, qint64 demotedUntilMs);
    /// 这一跳"没在动"吗（进账不到下限就算）。
    static bool mirrorStalled(qint64 bytesDelta, qint64 minBytesPerTick);
    /// 这一跳该不该记一次"失速"。**一个字节都还没收到的不算**（bytesSeen 0）。
    ///
    /// 为什么要把"还没收到第一个字节"单独摘出去：这一跳的
    /// 进账 delta 对一条还没开始吐字节的回复来说**天然是 0**，所以 mirrorStalled() 会把它
    /// 判成"链子太慢" —— 可它那根本不是慢，是"还没轮到它"（建连、或者排在一条 h2 连接上
    /// 另外 23 条流后面）。两跳 = 10 秒，于是**任何"建连 + 首字节超过 10 秒"的回复都会记满
    /// strike**，而 strike 满了的动作是 abort 它 + 把整个主机降级 300 秒。
    /// 这种回复自己的判据本来就够用，而且都比 10 秒晚：Qt 那条 30 秒传输超时（从请求建好
    /// 起算）会先掐掉它，走"退回备用地址、不算失败"那条路（见 retryWithOfficialUrl()）；
    /// 真的一点动静都没有的，还有 kNoProgressGiveUpMs 那条兜底。
    static bool countsTowardStallStrike(qint64 bytesSeen, qint64 bytesDelta,
                                        qint64 minBytesPerTick);
    /// 又发现一次失速之后，降级期该到什么时候 —— 取晚的那个（**顺延**，不提前）。
    static qint64 demoteDeadline(qint64 nowMs, qint64 currentUntilMs);

    /// 同时最多发出去几个下载。
    ///
    /// 这个数**不是**"Qt 能开几条连接"，而是"我们允许几条在飞"—— 两者在 HTTP/1.1 时代是同一个
    /// 数（Qt 每个主机上限 6），今天不是了：官方那几台 CDN 都会 ALPN 协商出 **HTTP/2**，Qt 6
    /// 默认就用它，几十条流跑在**一条** TCP 连接上。所以"调大就是把请求塞回 Qt 那个会超时的
    /// 内部队列"这条顾虑，只在两种情形下成立：服务端把并发流数压得很低，或者这个主机
    /// **根本不是 h2**。后一种由 kMaxConcurrentPerHost 那条判据守着（它没证明自己是 h2 之前
    /// 只放 kMaxConcurrentPerHost 条），所以这个 24 是"全局上限"，不是"每个主机都能 24 条"。
    ///
    /// 基准显示 24 已经接近这条网线的天花板，48 的收益已经很小，不值得再翻一倍并发。
    /// 慢的从来不是硬盘，是"每个文件一个网络往返"这件事本身。
    ///
    /// 不限流时一次性把对象全交给 QNetworkAccessManager，请求拿不到连接，30 秒的
    /// 不活动超时照样在走 —— 于是成批报 `Operation timed out`。所以排队必须发生在我们
    /// 自己手里（见 mayIssueAnotherDownload()）；下面这个数就是"我们手里"能同时放几条。
    /// NOTE: 镜像源（BMCLAPI）在 24 路下反而会**触发失速看门狗**（它每条要过 302 到 MinIO 预
    /// 签名，比官方慢得多），降级后自动改走官方 —— 那条兜底就是为
    /// 这种情形写的，不用跟着调。
    /// 放在 public 是给验证工程用的：V6i 的边界断言要按**同一个数**钉"到顶就不发"，硬写一个
    /// 字面量的话，下次调这个数用例会静默失效。
    ///
    /// （并发数预设）：这个数是**出厂默认那一条** —— 用户可以在设置 → 下载 → 并发数里
    /// 挑 6/12/24/48，真正在用的是这个对象的 `maxConcurrentDownloads()`。四条线的数、为什么
    /// 是它们、越界的值怎么办，都在 `DownloadConcurrency.hpp`（那里是**唯一出处**，这里的值
    /// 就 = 它的 `kDefault`）。
    static constexpr int kMaxConcurrentDownloads = DownloadConcurrency::kDefault;

    /// 这一次运行真正在用的那一档（默认 = 出厂那一条）。设置里改完由 `VersionManager`
    /// 推过来（见 `setDownloadConcurrency()`），**改完立刻生效**：正在下着的那一批也在
    /// 下一拍就按新的数泵 —— 已经在飞的那几条不会被叫回来（叫回来只会白白重下一段）。
    void setMaxConcurrentDownloads(int count);
    int maxConcurrentDownloads() const { return m_maxConcurrentDownloads; }

    /// 文件正文请求要不要强制 HTTP/1.1（默认 true）。
    ///
    /// NOTE: 镜像源（BMCLAPI）对 h1/h2 不敏感，两者的速度相当。
    /// 它的资源地址 302 到落点后本来就是 h1.1，这里改不改都一样；这条默认值主要救的是
    /// 官方源和整合包 CDN。
    /// NOTE: 只关文件正文。版本清单 / 版本 json / 资源索引那几条元数据很小，h2 的多路复用
    /// 对它没有坏处，仍是 Qt 的默认协商。
    /// NOTE: 网络拥挤时 h1 与 h2 都会掉到同一量级，h2 并不更快。
    void setPreferHttp1ForFiles(bool prefer) { m_preferHttp1ForFiles = prefer; }
    bool preferHttp1ForFiles() const { return m_preferHttp1ForFiles; }

    /// 下载速度上限（字节/秒，0 = 不限）。
    ///
    /// 设置 → 下载 → 速度上限那一格推过来。**改完立刻生效**：正在下的那一批下一拍就按新的
    /// 速率放行；从"限速"切回"不限"时，攒在回复缓冲区里的那些**当场**读完（不等到下一拍）。
    ///
    /// 实现是"背压 + 令牌桶"：`readyRead` 里不再把数据读空，只把回复记进
    /// `m_throttledReplies`；一个 100 ms 的定时器按限速往各个回复里"喂"字节。回复自己的
    /// `readBufferSize()`（256 KiB）就是闸门：读得慢，Qt 就不再从 socket 往下拉。
    /// 几档、上下限、归一化在 `DownloadSpeedLimit.hpp`（**唯一出处**）。
    /// 令牌泵的节拍（毫秒）。桶的容量、预读高水位都按这个数算；改它等于改所有相关判据。
    ///
    /// 100 ms 是"看起来连续"和"定时器开销"之间的折中：Windows 上这个间隔常常迟到
    /// 10~20 ms，所以令牌桶另外留了 1/4 秒的容量（见 `DownloadSpeedLimit::burstBytes()`）。
    /// 实现在 `DownloadManagerThrottle.cpp`。
    static constexpr int kSpeedPumpMs = 100;

    void setSpeedLimitBytesPerSecond(qint64 bytesPerSecond);
    qint64 speedLimitBytesPerSecond() const { return m_speedLimitBytesPerSecond; }

    /// 和另一个下载器**共用同一个令牌桶**。
    ///
    /// 分段作业（大于 4 MiB 那些）跑在 `ResumableDownload` 自己的那一个 `DownloadManager`
    /// 上，绕在本对象之外。限速如果是**每个下载器各一份**：设 1 MB/s，三个分段作业加上
    /// 外层各拿满一份 ⇒ 实际能跑到四倍。这里把外层这一个桶交给内部那个用，整条作业
    /// 合计起来才真的是设置里那个数。
    ///
    /// 只在 `DownloadManager::startSegmentedDownload()` 里用；别的下载器（直链下载工具
    /// 那种用户手动开的另一件工具）不共用。
    void shareSpeedLimitBucketWith(DownloadManager *owner);

    /// 还能不能再往外发一条（排队那一步问它）。
    ///
    /// **纯函数**，就为了能钉住那条上限：activeCount 到顶之后**不管还排着多少条**都不发。
    /// 这不是省资源，是**正确性**问题 —— 交给 QNetworkAccessManager 的请求要是当下拿不到
    /// 连接，就进了 Qt 内部那个队列，而 30 秒的传输超时**从请求建好就开始算**，于是它们会
    /// 在拿到连接之前成批地报超时。
    /// 一个整合包导入会踩同一个坑，而且更深一层：排在后面的回复
    /// 一个字节都收不到，失速看门狗两跳（10 秒）就把它们全判成"镜像太慢"（380 条 abort +
    /// 重发），重发又是 380 条一起发 —— 最后整批挂死不动、进度条停在 1%。
    /// verify 的 V6i 按边界钉过。
    static bool mayIssueAnotherDownload(int activeCount, int queuedCount);

    /// 同上，但上限由调用方给（队列泵用的是**这个对象当下那一档**）。
    /// 两参数那个版本 = 拿出厂默认那一档问一遍，留它是为了验证工程能按那个数钉边界。
    static bool mayIssueAnotherDownload(int activeCount, int queuedCount, int cap);

    /// 这条作业**最终会打到哪个地址**上：首选地址那个主机正被降级、而这条作业知道备用
    /// 地址时，就是备用地址（`startDownload()` 里那条"别等它慢 10 秒再退"的改道）。
    /// 判断只有这一处 —— "发给谁"和"按哪个主机算并发"都问它，两边不会漂开。
    static QString effectiveDownloadUrl(const QString &url, const QString &officialUrl,
                                        bool mirrorDemoted);

    /// 一个**还没证明**自己走 HTTP/2 的主机，一次最多放几条在飞。
    ///
    /// 就是 Qt 对 HTTP/1.1 每主机（确切说是每 host:port）的连接上限。超过这个数的请求会被
    /// Qt 自己排进它的内部队列，而 30 秒的传输超时**从请求建好就开始算** —— 于是它们会在
    /// 拿到连接之前成批报 `Operation timed out`，正是 kMaxConcurrentDownloads 那段注释里
    /// 「对象超时」的形状。
    ///
    /// 所以"排队必须发生在我们自己的 m_pendingJobs 里"这条不变量要**按主机**守着：全局
    /// 24 条，但同一个主机在它证明自己走 h2 之前只放 kMaxConcurrentPerHost 条。
    ///
    /// **改道那条路也算数**。首选地址那个主机正被降级时 startDownload() 会
    /// 当场改道到备用地址，而这条判据问的必须是**备用**那个主机 —— 判断只有一处
    /// （`effectiveDownloadUrl()`），"发给谁"和"问谁"不会漂开。
    /// 这个数就是 Qt 给每个 HTTP/1.1 主机的通道数；Qt 6.5 起可以用
    /// `QHttp1Configuration::setNumberOfConnectionsPerHost()` 按请求把通道数抬上去。本类给
    /// **每一条**请求都带上它（`configureHttp1()`，连接按 URL 缓存、第一条到的请求决定
    /// 通道数），所以"我们允许几条在飞"重新等于"Qt 会开几条"。
    ///
    /// NOTE：BMCLAPI 那类有 429 限流的主机，限的是**请求数**不是连接数；24 并发本来就会
    /// 撞上它的 10 次/分钟，和这条闸无关（429 冷却另有处理）。
    static constexpr int kMaxConcurrentPerHost = 24;

    /// 同时最多几个作业走安全分段（每个作业内部固定 4 路）。上限只是别让几十个大文件
    /// 一起抢带宽/连接；满了的作业**不排队等待**，当场按普通单连接下载发出去，避免
    /// 队头的大文件把后面几千个小文件卡住。
    static constexpr int kMinSegmentedJobs = 2;
    static constexpr int kMaxSegmentedJobs = 6;
    static int maxSegmentedJobsFor(int concurrency) {
        return qBound(kMinSegmentedJobs, concurrency / 8, kMaxSegmentedJobs);
    }

    /// 还能不能再往**某一个主机**发一条。和上面那条一起判（两条都过才发）。
    /// @param activeForHost 这个主机当下有几条在飞。
    /// @param hostKnownHttp2 这个主机已经证明过走 HTTP/2（见 m_http2Hosts）。
    ///
    /// 已知 h2 的主机：能 —— 几十条流跑在**一条** TCP 连接上，放几条由全局上限说了算。
    /// 还不知道的主机：只能 kMaxConcurrentPerHost 条。这是保守估计，**代价只有一次往返**：
    /// 这个主机上第一条回复一回来，Http2WasUsedAttribute 就把答案给出来了 —— 真是 h2 就
    /// 立刻升到全局上限（官方 CDN 上实际等于安装开始时多花约 150 ms），真是 h1.1 就一直按
    /// kMaxConcurrentPerHost 守。
    static bool mayIssueAnotherDownloadToHost(int activeForHost, bool hostKnownHttp2);

    /// 一条回复"多久一点动静都没有"就判它废了（毫秒）。
    ///
    /// 为什么要有**按时间**这一条兜底：QNetworkReply **不保证**一定会发 finished。回复
    /// 可能在 HTTP/2 连接被对端掐掉之后就那么挂着，没有动静、也没有终态，
    /// pendingDownloads 永远归不了零 → 整批永远不结束（界面表现是"下载中"停在 1%，没有错误、
    /// 没有弹窗，用户只能重启）。传输超时是"不活动"超时，本该在 30 秒时收拾它们 —— 这一条
    /// 是**它没收拾干净时的兜底**，所以取它的三倍。
    /// 只认"一点动静都没有"：downloadProgress 报过一次字节就重新计时，所以一条细水长流、
    /// 但一直在掉字节的下载不会被它掐掉。
    static constexpr qint64 kNoProgressGiveUpMs = 90 * 1000;

    /// 改上面那个期限。**只给验证工程用**：那一条只能靠时间验，而 90 秒对这种逐条的用例
    /// 太贵了（真机上它是固定值，界面上没有、也不该有这个开关）。
    void setNoProgressGiveUpMs(qint64 ms) { m_noProgressGiveUpMs = ms; }
    /// 同上，改"镜像有退路、却连首字节都没等到"那条期限（默认 kNoFirstByteGiveUpMs）。
    /// 也只给验证工程用 —— 12 秒对逐条用例同样太贵。
    void setNoFirstByteGiveUpMs(qint64 ms) { m_noFirstByteGiveUpMs = ms; }
    /// @}

    /// 按**现在这一刻**的下载源，把一条"地址自带身份"的官方地址落到实际请求的地址上
    /// （镜像被降级时给回官方那条）。按版本号/索引号拼的那几处走 mirrorVersionJsonUrl()
    /// 之类的函数，不经过这里。
    QString endpoint(const QString &officialUrl) const;

    /// 此刻该请求的版本清单地址。覆盖优先（`setVersionManifestUrlOverride`，且覆盖时
    /// **不套镜像**），否则官方地址按当前下载源换算。
    ///
    /// 凡是要"先拿清单再挑版本"的调用点都问这里。别再自己拼
    /// `endpoint(officialVersionManifestUrl())` —— 那样绕过了覆盖口子，验证工程里
    /// 那一跑换不掉地址，冷路会静默打到真 Mojang 上（`ServerInstallController` 就踩过）。
    QString versionManifestUrl() const;

    // ------------------------------------------------------------------
    // 官方地址 → 镜像地址。**纯函数**，不碰成员、不发请求。
    // NOTE: 这批函数不需要 QApplication，是现有代码里最容易钉住的一批 —— verify 的 V8
    // 那一节按它们钉断言（含镜像映射和 mirrorMavenRootUrl() 的结尾斜杠），改地址先看 V8。
    // ------------------------------------------------------------------
    // 为什么要三个专门的函数、而不是一个"换 host"的：BMCLAPI 对一部分资源用了
    // 完全不同的路径形状，必须拿**版本号 / 索引号**重拼。
    // piston-meta 的 /v1/packages/<sha1>/<id>.json → /version/<id>/json（或 /indexes/<id>.json）
    // piston-data 的 /v1/objects/<sha1>/client.jar → /version/<id>/client
    // 而这三处的 sha1 在地址里、id 在调用方手上，光看 URL 分不出是版本 JSON 还是
    // 资源索引（两者形状一模一样，只能靠上下文区分是哪个 id）。

    /// 把一条**地址自带身份**的官方地址换成镜像地址。
    /// @param officialUrl 官方地址（版本清单 / 资源对象 / 库文件）。
    /// @return 镜像地址；**认不出来的地址原样返回**。
    ///
    /// 认不出来就原样返回是刻意的：宁可这一条走官方，也别拼一个不存在的镜像地址出来
    /// ——拼错的表现是"下载全失败"，比"有一部分没走镜像"难查得多。
    static QString mirrorUrl(const QString &officialUrl);

    /// 镜像源里 maven 那一支的根地址，**结尾带斜杠**。
    ///
    /// 只有一个用处：喂给 Forge / NeoForge 安装器的 `--mirror`。那两个安装器是同一套框架，
    /// 支持用命令行覆盖它自己那份镜像列表；不给的话它挑中的是 creeperhost，给了这个地址
    /// 48 个库全从 bmclapi 来、一次回退都没有。
    static QString mirrorMavenRootUrl();

    /// 这条作业要不要登记"官方备用地址"（镜像取不回来时拿它再试一次）。
    ///
    /// **纯函数**，就为了能钉住那两条边界（verify 的 V6d）。判据是"两个地址**不一样**":
    /// 一样的时候"退回官方"等于原样再发一次，只会把同一个失败重复一遍、还多花一个
    /// 30 秒传输超时。空串 = 调用方没给，也就是这条作业没有第二条路（本来就走官方、
    /// 或者镜像上压根没有对应的官方地址）。
    static bool shouldRecordOfficialFallback(const QString &requestUrl,
                                            const QString &officialUrl) {
        return !officialUrl.isEmpty() && officialUrl != requestUrl;
    }

    /// 版本 JSON：官方是 /v1/packages/<sha1>/<id>.json，镜像按版本号取。
    static QString mirrorVersionJsonUrl(const QString &versionId);

    /// 客户端 jar：官方是 /v1/objects/<sha1>/client.jar，镜像按版本号取。
    static QString mirrorClientJarUrl(const QString &versionId);

    /// 资源索引 JSON：官方是 /v1/packages/<sha1>/<id>.json，镜像按索引号取。
    ///
    /// NOTE: 这一条是整套镜像里唯一**当下是坏的**路由：镜像上全是 404（同一时刻
    /// /version/<id>/json、/version/<id>/client、/assets/<hash> 都是 200）。地址本身没拼错
    /// （它自己的文档里就写着这个形状），所以
    /// 这里保留映射、由 downloadAssets() 那一侧兜底退回官方 —— 别把这条删了当"没用"。
    static QString mirrorAssetIndexUrl(const QString &assetIndexId);

    /// 版本清单的官方地址。
    static QString officialVersionManifestUrl();

    // 版本列表操作
    void fetchVersionManifest();
    QList<MinecraftVersion> getAvailableVersions() const;

    // 版本 JSON 文件操作
    void fetchVersionJson(const QString &versionId);
    QJsonObject getVersionJson() const;

    // 下载操作
    // / NOTE: 每一条下载都是**先排进队列再发**（见 mayIssueAnotherDownload）：作业少的时候
    // / 队列当场就把它发出去，和"直接发"没有任何区别；作业一多（几千个资源对象、
    // / 几百个整合包文件）就只有 kMaxConcurrentDownloads 条在飞，其余留在我们自己的
    // / 队列里等 —— 在 Qt 的队列里等会被它的传输超时算进去，而那正是"成批失败"的来源。
    // @param expectedSize 文件应有的字节数，传 -1 表示未知（旧版本 JSON 和 natives
    // classifier 都没有 size 字段），此时退化成只比 SHA1。
    // @param mode 见 VerifyMode，决定已存在的文件要不要重下。
    // @param officialUrl 这条地址**换成镜像之前**的那条官方地址（可选）。给了它，
    // 镜像取不回来时会拿它再试一次（见 m_officialFallbacks）。
    // 不给 = 没有"退一步"这回事（本来就走官方、或者镜像上就没有第二条路）。
    void downloadFile(const QString &url, const QString &destinationPath,
                      const QString &expectedSha1 = QString(), qint64 expectedSize = -1,
                      VerifyMode mode = VerifyMode::SizeAndSha1,
                      const QString &officialUrl = QString(), const DownloadRange &range = DownloadRange());
    void setAllowOverwrite(bool allow) { m_allowOverwrite = allow; }

    /// 允许从哪些游戏文件夹里"借"现成文件。
    ///
    /// 下载新版本的时候（甚至在全新文件夹里下载新东西的时候）复用其它 mc
    /// 文件夹/版本的文件以减少等待时间；文件夹的使用仅限用户添加了的和官方
    /// 启动器默认文件夹（如果存在），不要全盘扫描。
    ///
    /// 这里传进来的就是**全部**可用的根，`DownloadManager` 自己不扫盘、不猜路径 —— 传空 =
    /// 关掉复用（默认就是空）。`VersionManager` 在切换游戏文件夹 / 文件夹列表变化时推过来
    /// （见 `FolderManager::reuseSourceFolders()`），当前文件夹自己会被跳过。
    void setReuseSourceFolders(const QStringList &folders) { m_reuseSourceFolders = folders; }
    QStringList reuseSourceFolders() const { return m_reuseSourceFolders; }
    /// 分段下载的续传缓存根目录。空 = `DownloadCache::root()`（正式缓存）；测试用它指向
    /// 临时目录，避免把用例的残留留在用户缓存里。
    void setRangeCacheRoot(const QString &root) { m_rangeCacheRoot = root; }
    /// 是否允许把满足判据的作业自动升级成安全分段。
    /// `ResumableDownload` 内部的私有 DownloadManager **必须**关掉它，否则
    /// `startSingle()`（带 sha1 + size、不带 Range）会再次被升级成分段，形成
    /// "探测 → 回退单发 → 探测"的无限递归。
    void setSegmentedDownloadEnabled(bool enabled) { m_segmentEnabled = enabled; }
    /// 分段协调器的探测请求复用同一代理、证书策略与网络连接池。
    QNetworkAccessManager *networkAccess() const { return networkManager; }
    void downloadLibraries(const QList<Library> &libraries, const QString &basePath);
    /// 资源索引和资源对象都落在 AppPaths::assetsDir() 下，也就是 --assetsDir 指的那个
    /// 目录。调用方**不能**指定别处：索引和 objects 必须同源，否则游戏按 --assetsDir
    /// 去找 indexes/<id>.json 就会扑空。
    /// @param assetIndex version.json 的 assetIndex 节点，必须带 id / url / sha1。
    void downloadAssets(const AssetIndex &assetIndex);
    /// @param versionId 版本号。镜像源下客户端 jar 是**按版本号**取的
    /// （官方是按 sha1 取），所以这个参数是必需的，不能只给 URL。
    void downloadClientJar(const QString &versionId, const QString &url,
                           const QString &destinationPath, const QString &expectedSha1,
                           qint64 expectedSize = -1);

    // 批次记账。beginBatch() / endBatch() 之间的所有下载被看作一批，
    // 整批结束时 allDownloadsFinished 只会发射一次；两者之间不允许提前 return，
    // 否则 m_batchOpen 永远为真，完成信号永不发射，UI 会一直停在"下载中"。
    void beginBatch();
    /// 把"体检点名的那几条"交给**当前这一批**：它们不问盘上那份，直接重下。
    ///
    /// 为什么需要它：体检（VersionIntegrity）和补全
    /// （VersionManager::repairVersionFiles）对**资源对象**用的判据不一样 —— 体检比字节数，
    /// 补全只看在不在（ExistsOnly：那 3600 个对象全量哈希要几秒，见 parseAndDownloadAssets
    /// 里那段）。于是一个"大小不对"的对象会被体检报成「坏」，而补全**跳过它**，用户看到的
    /// 是"体检说要修 1 个、补全说一个都不用下"—— 两份界面互相打脸。
    /// 点名之后两边就对上了：体检说谁是坏的，这一批就把它当"要下"。
    ///
    /// NOTE: **按路径认人**，不分类别（jar / 库那两类的判据本来就是 sha1，体检点它们的名时
    /// 补全自己也照样重下，多这一条不影响它们；收益全在资源对象那一档上）。
    /// NOTE: 只对**当前这一批**有效 —— beginBatch() 清掉、放弃 / 取消也清掉。它是一句
    /// "这一批怎么做"的交代，不是一份常驻状态。
    void setFlaggedPaths(const QStringList &paths);
    void endBatch();
    /// 放弃当前批次：**一个完成信号都不发**，只把"批次开着"这个状态清掉。
    ///
    /// 只有一个调用者：VersionManager 的异常兜底（见那边 deferNextStep() 的 NOTE）。
    /// 正常的收尾一律走 endBatch()。异常正好打在 beginBatch() 和 endBatch() 之间时，
    /// m_batchOpen 会一直是 true，之后每一次下载都会被 isBusy() 顶回来（界面表现是
    /// "已有下载任务正在进行"，只能重启）—— 这个方法就是给那条路兜底的。
    ///
    /// 为什么不能拿 endBatch() 代替：endBatch() 会跑 checkAllDownloadsFinished()，那一句
    /// 有可能**同步**发出 allDownloadsFinished，于是又回到"在槽里接着干下一件事"这件正在
    /// 收尾的事上，而且那一次并不是真的收尾、报出去的话也是错的。所以这里把 m_batchEmitted
    /// 直接置真：之后所有"这一批结束了"的判断都会被挡掉。已经在飞的那几个回复照样跑完
    /// （文件照落盘），只是不再有任何完成信号。
    ///
    /// **但"不再有完成信号"只挡住了"这一批的最后一条"那一次**：
    /// 已经在飞的那几条回来时仍然是照常 --pendingDownloads / m_doneJobs++ /
    /// emit downloadFinished 的。放弃之后如果**又开了一批**（用户点取消、再点一次下载
    /// 就是这么个次序），上一批那几条残留就会把新批的计数加花：m_doneJobs 可能超过
    /// m_totalJobs（进度条上的数字比总数还大），也可能刚好把
    /// checkAllDownloadsFinished() 的几条判据凑齐 → **提前宣布整批下完**，界面报"下载
    /// 完成"而文件还在路上。所以每一条回复都记着"我是第几批发出去的"，不属于当前这一批
    /// 的那几条只收连接、不记账、不发信号（见 m_batchGeneration）。
    void abandonBatch();

    /// **用户点了取消**：停止发新作业、把在飞的那几条真的掐掉、保留已经原子提交的文件。
    ///
    /// 和 `abandonBatch()` 的两点差别，都是"取消"这个词要求的：
    /// · `abandonBatch()` 只管"之后不再报"，**已经在飞的那几百 MB 照样跑完**（它是给内部
    /// 异常兜底用的，那时候多的是要收拾的事，不差这一件）。取消不行 —— 用户按的就是
    /// "别下了"，而且那几条会让 `isBusy()` 一直是 true ⇒ 底栏那格一直"在忙"、之后再点
    /// 一次下载会被"已有下载任务正在进行"顶回来。
    /// · `abandonBatch()` 不动代次，所以已经在飞的那几条回来时**仍然算当前这一批**的账。
    /// 取消要加代次：那几条是 `abort()` 出来的，回来的错误是 `Operation canceled` ——
    /// 算进失败清单的话，用户看到的是"取消了，然后有 24 个文件下载失败"。
    ///
    /// **一个字都不删**：已经落盘的（`QSaveFile` 原子提交过的那几个）留在原处。取消不是
    /// 回滚，下一趟重下时 `fileAlreadyValid()` 会按 sha1/字节数把它们跳过。
    ///
    /// NOTE: 它会**同步**触发 `finished`（`abort()` 在某些后端是同步回调的），所以先加代次
    /// 再 abort —— 那些回调走的就是"这一条不属于当前批次"那条路。
    void cancelBatch();

    // Opt-in at the owning executor's batch boundary; nested range downloaders stay unregistered.
    bool enableCheckpoint(const QString &path, const QJsonObject &context);
    bool canPauseBatch() const;
    bool pauseBatch();
    bool resumeBatch(QString *problem = nullptr);
    bool restoreBatch(const QString &path, QString *problem = nullptr);
    bool batchPaused() const { return m_batchPaused; }
    QString checkpointPath() const;
    QJsonObject checkpointContext() const;
    /// 换掉恢复记录里那几格**来路信息**（`context`），并当场落盘。
    ///
    /// 只给"先登记、后补齐"那一条路用：批次一开始就把接续意图登记进记录（便宜的那几格），
    /// 真正要读盘的配置目录快照在后台算完再补进去（见 `DownloadBatchController`）。
    /// 记的不是这一批文件本身，所以**不碰 `dirty`**：那是文件表的账。
    /// @return 落盘成功与否。调用方据此决定"没补上"要不要让这一批按普通下载收尾。
    bool updateCheckpointContext(const QJsonObject &context);
    bool deferCheckpointContinuation(bool refreshSources = false);
    void forgetCheckpoint();
    void setCommitGuard(std::function<QString()> guard) { m_commitGuard = std::move(guard); }

    /// 失败作业要重做时需要的**全部**东西（见 `failedJobs()`）。够 `downloadFile()` 再喂一遍。
    struct FailedJob {
        /// 这一条该从哪儿下（失败那一次用的那个地址）。
        QString url;
        /// 落到哪个**绝对**路径。
        QString destinationPath;
        /// 空 = 这一条本来就没有 sha1 可核。
        QString expectedSha1;
        /// -1 = 调用方也不知道该多大。
        qint64 expectedSize = -1;
        /// 走镜像时留着的那条退路（空 = 没有）。重试时照样带着。
        QString officialUrl;
    };

    /// **上一次收尾的那一批**里失败的那几条。`beginBatch()` 会清空它。
    ///
    /// 只留"够重试用的那一份"，不留整个 `PendingJob`：重试时校验档一律按 SizeAndSha1 重新
    /// 发（失败的那几个在盘上根本不存在，档位只影响 `fileAlreadyValid()` 那一次问话）。
    QList<FailedJob> failedJobs() const;

    bool isBusy() const;
    DownloadStats stats() const;

    // 工具方法：哈希/校验，以及把 version JSON 解析成上面那些结构体（版本清单和单个
    // 版本 JSON 各一个入口）。留在 public 是给验证工程用的 —— 它会直接调
    // extractLibraries()/extractAssetIndex() 断言"选中了哪些库、natives classifier
    // 挑得对不对"。verifyFileSha1() 只有本类自己调。
    //
    // calculateFileSha1() 是 **static**：它一个成员都不碰。体检那一层
    // （VersionIntegrity::check）要按装配侧**同一个**算法算哈希，而它手上没有、也不该有
    // 一个 DownloadManager 实例（那一层不下载）。写第二份读文件的哈希循环是要避免的事：
    // 两份算法只要有一天对不上（比如这边换成 buffer 分块），"校验不过"就会变成
    // 一句没人查得动的假话。
    bool verifyFileSha1(const QString &filePath, const QString &expectedSha1);
    /// @param cancel 后台那条线（`pumpPrecheckQueue()`）传进来的取消令牌：给了就按 1 MiB
    ///               分块读、每块看一次；不给（界面线程 / 体检那一层）照旧一次性交给
    ///               `QCryptographicHash` —— 哈希算法只有这一份，两条路算出同一个值。
    static QString calculateFileSha1(const QString &filePath,
                                     const std::atomic_bool *cancel = nullptr);
    /// `fileAlreadyValid()` 的**后半段**：会真去读文件体（比 SHA-1 / 看 zip 头）的那一半。
    ///
    /// 为什么拆出来：前半段（点名 / 不存在 / 0 字节 / 大小对不上 / ExistsOnly）全是
    /// stat 级别的判断，界面线程上做不心疼；后半段要**整份读**，几千个已经躺在实例里的
    /// 文件就是几十秒的卡顿 —— `scheduleFile()` 靠这条分界决定"这一份要不要丢进后台"。
    /// 两条路必须给出同一个结论，所以它是 public、验证工程直接拿它问一遍（见
    /// `tests/mw_check/DownloadBatchChecks.cpp` 的 `precheck-existing` 一节）。
    static bool fileBodyValid(const QString &path, const QString &expectedSha1,
                              qint64 expectedSize, VerifyMode mode,
                              const std::atomic_bool *cancel = nullptr);
    void parseVersionManifest(const QJsonDocument &json);
    void parseVersionJson(const QJsonDocument &json);
    QList<Library> extractLibraries(const QJsonObject &versionJson);
    AssetIndex extractAssetIndex(const QJsonObject &versionJson);

signals:
    void versionManifestFetched(const QList<MinecraftVersion> &versions);
    void versionJsonFetched(const QJsonObject &versionJson);
    void downloadProgress(const QString &fileName, qint64 bytesReceived, qint64 bytesTotal);
    /// Newly received file-body bytes, including range retries, excluding restored cache.
    void transferBytes(qint64 bytes);
    void downloadFinished(const QString &fileName, bool success,
                          const QString &errorMessage = QString());
    // 进度用标量发，避免每来一个网络分片就拷贝一份 DownloadStats。
    void batchProgress(int done, int total, int failed, const QString &currentFile);
    void allDownloadsFinished(bool success);
    void batchStarted();
    void checkpointChanged();

private slots:
    /// 看门狗每跳一次（5 秒）：看"在看之下"的那些回复动没动，没动就降级 + abort。
    void onStallWatchdogTick();
    /// 限速泵每跳一次（100 ms，只有设了上限时才在跳）：按令牌往回复里喂字节。
    void onSpeedPumpTick();
    void onVersionManifestReplyFinished();
    void onVersionJsonReplyFinished();
    void onDownloadReplyFinished();
    void onDownloadReplyProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onAssetIndexDownloaded();

private:
    friend class DownloadBatchController;
    // Only the recovery controller supplies worker-validated targets while retaining the receipt lock.
    bool resumeValidatedBatch(const QSharedPointer<DownloadCheckpoint> &receipt,
        const QHash<QString, QJsonObject> &targets);
    QHash<QString, QJsonObject> m_resumeTargets;
    QSharedPointer<DownloadCheckpoint> m_checkpoint;
    bool m_batchPaused = false;
    bool m_checkpointFailed = false;
    bool m_checkpointPumpQueued = false;
    bool m_suspendingBatch = false;
    std::function<QString()> m_commitGuard;
    std::function<QString()> checkpointGuard(const QString &path) const;
    QNetworkAccessManager *networkManager;
    NetworkManagerFactory m_networkFactory;
    QList<MinecraftVersion> availableVersions;
    QJsonObject currentVersionJson;
    // 两张表都以 QNetworkReply* 为键，回复一结束就从表里摘掉（onDownloadReplyFinished）。
    QMap<QNetworkReply *, QString> activeDownloads;  // reply -> 目标路径
    QHash<QNetworkReply *, QSharedPointer<StreamedFile>> m_streamedFiles;
    QMap<QNetworkReply *, QString> expectedSha1s;    // reply -> 期望的 SHA1
    // reply -> 期望的字节数（没登记过 = 不知道）。
    //
    // NOTE: 和 expectedSha1s 成对：**两处都要摘**（成功 / 失败 / 退回官方 / 看门狗判死）。
    // 只摘一张的症状是"一条刚从那条主机发出去的下载立刻被判成字节数不对" —— 键是
    // 回复指针，Qt 释放之后下一批可能落在同一个地址上。
    QMap<QNetworkReply *, qint64> expectedSizes;
    /// **官方备用地址**（reply -> 官方 URL）。只在"这条作业走的是镜像"时登记。
    ///
    /// 资源索引也走这条退路（镜像那条 `/indexes/<id>.json` 路由当下全 404，
    /// 而索引拿不到 = 那几千个资源对象一个都不知道要下谁）。镜像不是 404 时也会
    /// 302 跳到 openbmclapi 的兄弟节点（本地节点没缓存这个对象时的正常行为），而那个
    /// 节点可能慢到 13 KB 要 25 秒、有的 30 秒拿不全；官方那条同一时刻 1 秒就给了。
    /// 于是 30 秒的传输超时一到，整批就报"原版没下全"。
    /// 所以**所有**走镜像的下载都能退一步（库 / 资源对象 / 客户端 jar / 安装器 jar）。
    QMap<QNetworkReply *, QString> m_officialFallbacks;
    int pendingDownloads;

    /// 一条**元数据作业**（版本清单 / 版本 json）的全部信息。
    ///
    /// 元数据作业**不落盘、也不计进 pendingDownloads**（见 fetchVersionManifest()）—— 但它们
    /// 要进 m_stallMeters，否则一条卡住的元数据请求没人判它失败，整条流程就挂在半路。
    /// 这张表就是给 failWedgedDownload() 用的判据：看门狗
    /// 收一条回复时，得知道"该不该动那套计数"。没有这个判据，看门狗掐掉一条元数据回复
    /// 时会照样 `--pendingDownloads`（那条计数它从来没加过）→ 真在飞的文件作业就少算一个 →
    /// checkAllDownloadsFinished() 提前宣布整批下完，而文件还在路上。
    struct MetadataJob {
        /// 日志里人看得懂的名字（「version_manifest.json」/「version_json」）。
        /// 空 = 这条回复不是元数据作业（表里查不到时 .value() 给的就是这个）。
        QString label;
        /// 这一份该是哪个版本（版本 json 才有；清单那条为空 = 不核这一项）。
        QString expectedId;
        /// 这一份该是什么字节（取自官方 URL 里的 sha1；镜像那条自拼的地址没有 → 空 = 不核）。
        QString expectedSha1;
    };
    QMap<QNetworkReply *, MetadataJob> m_metadataReplies;

    /// 批次代次：**每 beginBatch() 加一**。发出去的每一条回复都把自己发出去那一刻的代次
    /// 记在 m_replyGenerations 里，收尾那几步（计数 / 失败清单 / 进度 / 完成判定）先问一遍
    /// "这一条属于当前这一批吗"。
    ///
    /// NOTE: 为什么需要它：abandonBatch() 只是把 m_batchEmitted 置真，已经在飞的那几条回复
    /// 回来时**照样**会 --pendingDownloads、m_doneJobs++、发 downloadFinished。"放弃之后
    /// 又开一批"是常事（取消、再点一次下载），上一批的残留就会算进新批 —— 计数错乱，甚至
    /// 提前宣布整批下完。代次是唯一能分清"这条回复是哪一批的"的东西：指针不行（Qt 会复用
    /// 地址）、路径不行（同一批里每个文件也各不相同）。
    quint64 m_batchGeneration = 0;
    /// 回复 -> 它发出去时的代次。**每一条**发出去的回复都要登记（文件作业在 startDownload，
    /// 资源索引在 downloadAssets），并在收尾时摘掉；漏登记的那条会按"当前这一批"处理
    /// （默认值就是当前代次）—— 只影响那一批的计数，不会崩。
    QHash<QNetworkReply *, quint64> m_replyGenerations;

    /// 这一条回复属于**当前**这一批吗。批次之外的一次性作业（版本清单、加载器版本列表、
    /// 安装器下载……）查不到记录，按当前这一批处理 —— 它们本来就各算各的，从来不在批次里。
    bool replyIsFromCurrentBatch(QNetworkReply *reply) const;

    /// 等着发的作业。**每一条下载都从这里过**（资源对象、库、客户端 jar、整合包清单里
    /// 那几百个文件都一样）：一次资源补齐是 3000 多个对象，全塞给 QNetworkAccessManager
    /// 的话 Qt 每个主机只开 6 条连接，其余的在它内部干等，而 30 秒的传输超时**从请求
    /// 建好就开始算**，于是它们排到队尾之前就成批地报 `Operation timed out` —— 网络其实
    /// 是通的。发出去的条件就是那两个纯函数（mayIssueAnotherDownload / ...ToHost），
    /// 一问"全局还有位置吗"、一问"这个主机还有位置吗"。
    /// NOTE: 排在这里的作业已经计进 m_totalJobs（进度条的分母），但**不计进**
    /// pendingDownloads（那是"已经发出去的"），所以 checkAllDownloadsFinished()
    /// 与 isBusy() 都必须把这条队列也算上，否则会在还排着几百个的时候就宣布
    /// 整批下完 / 报告"没在忙"。
    struct PendingJob {
        QString url;
        QString destinationPath;
        QString expectedSha1;
        // 期望的字节数，-1 = 调用方也不知道。
        //
        // NOTE: 和 expectedSha1 一样**必须排队时就带上** —— 它是"这条回复回来时拿什么核
        // 这份正文"的一部分（见 onDownloadReplyFinished）。丢掉它的话，写盘那一步只剩
        // sha1 一条判据；而拿不到 sha1 的那些库（maven 风格的那几个，见 LibraryRules.hpp
        // 里那条说明）就变成"任何 200 正文都算成功"。
        qint64 expectedSize = -1;
        // 排队时也得把它带上：作业在队列里等的这段时间，是没法回头问调用方要的。
        QString officialUrl;
        // 这个作业要去哪个主机。排队时算一次（QUrl 解析一次），因为发送那一步要拿它按主机
        // 计数，而那一步发生在**每次**有回复收尾时、要扫一遍队列 —— 几百个作业的时候
        // 一圈就是几百次解析。
        // NOTE: 它是**排队那一刻**那条地址的主机（镜像源下就是镜像主机），而真正发出去时
        // 可能已经被"镜像降级"改道到官方（见 startDownload）。所以极端情况下官方那个
        // 主机的额度会被少算一条（这条按镜像的额度放过去了）—— 这个方向是**故意**的：
        // 宁可偶尔多发一条，也不要因为一个对不上的计数把队列卡死。
        QString host;
        int retryAttempts = 0;
        qint64 readyAtMs = 0;
        DownloadRange range;
        /// 这条作业要不要走安全分段（`scheduleFile()` 按判据置位）。分段作业不占
        /// QNetworkReply，详见 m_segmentJobs 的说明。
        bool segmented = false;
        /// "已经退过官方地址" / "已经退过普通下载"的一次性标记：重新入队后要能记住，
        /// 否则会在两条路之间来回打转。
        bool officialRetried = false;
        bool plainRetried = false;
    };
    QQueue<PendingJob> m_pendingJobs;
    bool m_budgetWakePending = false;
    void wakeForRequestBudget();
    void probeFasterSource(QNetworkReply *source, qint64 currentRate);
    bool m_speedProbeInFlight = false;
    QPointer<QNetworkReply> m_speedProbe;
    qint64 m_nextSpeedProbeMs = 0;

    // ------------------------------------------------------------------
    // 「从别的游戏文件夹里借文件」（见 setReuseSourceFolders()）
    // ------------------------------------------------------------------
    // / 一条**还在盘上找来源**的作业。它既没占网络连接、也**还没计进 m_totalJobs** ——
    // / 借成功就计进 m_reusedJobs（不进分母），借不到再退回 PendingJob 那条路，那时候才
    // / 计进 m_totalJobs。所以队列里每一条对进度条分母的影响都是"还没定"。
    // /
    // / NOTE: 和 PendingJob 一样把 url / officialUrl 带上：借不到的那一瞬间要能原样退回网络，
    // / 而那时候调用方（VersionManager）早就走远了。
    struct ReuseJob {
        QString url;
        QString destinationPath;
        /// 目标在**当前**游戏文件夹里的相对路径（`LocalFileReuse::relativePathWithin()` 的结果）。
        /// 候选来源是按它拼出来的，见 `candidatePaths()`。
        QString relativePath;
        QString expectedSha1;
        qint64 expectedSize = -1;
        VerifyMode mode = VerifyMode::SizeAndSha1;
        QString officialUrl;
    };
    QQueue<ReuseJob> m_reuseQueue;
    /// 允许借文件的根（见 setReuseSourceFolders()）。空 = 这个功能关着。
    QStringList m_reuseSourceFolders;
    /// 借文件专用的线程池。**不能**用 QThreadPool::globalInstance()：那儿还跑着别的活，
    /// 而且几千个作业同时往里丢会把全局池的排队人数撑爆（哈希是大文件、一次几百毫秒）。
    QThreadPool *m_reusePool = nullptr;
    /// 正在后台找来源 / 落盘的作业数。isBusy() 与 checkAllDownloadsFinished() 都要算上它，
    /// 否则借文件那一段会被当成"这一批已经下完了"。
    int m_reuseActive = 0;
    /// 这一批借成功了多少个（见 DownloadStats::reused）。
    int m_reusedJobs = 0;
    /// 取消令牌：beginBatch / 取消 / 放弃 / 析构都换一个新的，旧的那个置真。
    /// 后台作业只读它（复制循环每个 1 MiB 块看一次），换新 = 让旧作业尽快收手。
    QSharedPointer<std::atomic_bool> m_reuseCancel;
    /// 借文件那条线的代次。**和 m_batchGeneration 分开**：纯值层（LocalFileReuse）的用例
    /// 不走 beginBatch，而借文件的结果要能单独作废（见 clearReuseJobs()）。
    quint64 m_reuseGeneration = 0;

    // ------------------------------------------------------------------
    // 「要整份读文件的那几条判据」后台化（见 scheduleFile()）
    // ------------------------------------------------------------------
    // / 一条还在后台问盘上的作业。它对应 `scheduleFile()` 里"盘上那份如果真去比 SHA-1、
    // / 或者要给它算一份原件指纹，就得整份读一遍"的那一档。
    // /
    // / NOTE: **不进 m_totalJobs**：结论出来之前它既不算跳过、也不算要下。和借文件那条线
    // / 同一个道理 —— 分母是"真要下的那几个"，还没定下来的不该先占一格。
    struct PrecheckJob {
        QString url;
        QString destinationPath;
        QString expectedSha1;
        QString officialUrl;
        qint64 expectedSize = -1;
        VerifyMode mode = VerifyMode::SizeAndSha1;
        DownloadRange range;
        /// 要不要顺手把原件指纹（`DownloadCheckpoint::fingerprint`，SHA-256）也算出来。
        /// 只有这一批有 checkpoint 才算：没 checkpoint 的批次根本不登记原件，白读一遍没意义。
        bool wantFingerprint = false;
    };
    QQueue<PrecheckJob> m_precheckQueue;
    /// 后台问盘专用线程池。**1 条**：这个活是整份读文件，几条并排在机械盘上只会互相抢磁头；
    /// 借文件那个池是 4 条，但它是"硬链接一瞬间 / 复制一次"，不是同一个性质。
    QThreadPool *m_precheckPool = nullptr;
    /// 正在后台问的作业数。isBusy() 与 checkAllDownloadsFinished() 都要算上它，
    /// 否则"只剩已有文件要核对"的那一段会被当成"这一批已经下完了"。
    int m_precheckActive = 0;
    /// 取消令牌：beginBatch / 取消 / 放弃 / 析构都换一个新的，旧的那个置真。
    QSharedPointer<std::atomic_bool> m_precheckCancel;
    /// 问盘那条线的代次。**和 m_batchGeneration 分开**：裸 downloadFile 不走 beginBatch，
    /// 而结果要能单独作废（见 clearPrecheckJobs()）。
    quint64 m_precheckGeneration = 0;

    /// 一个正在跑的安全分段作业。它没有 QNetworkReply（内部由 ResumableDownload 用自己的
    /// DownloadManager 发最多 4 条 Range 请求），所以**不计进 pendingDownloads**；全局并发
    /// 额度按"一个作业槽"算，batch 收尾 / isBusy() 必须把这张表也算上。
    struct SegmentJob {
        QString url;  // 实际尝试的地址（可能已被镜像降级改道到官方）
        QString destinationPath, expectedSha1, officialUrl, host;
        qint64 expectedSize = -1;
        bool officialRetried = false;
        bool plainRetried = false;
        quint64 generation = 0;
    };
    QHash<ResumableDownload *, SegmentJob> m_segmentJobs;

    /// 已经证明过走 HTTP/2 的主机。**只增不减**：一个主机是不是 h2 不会变。
    ///
    /// 由 QNetworkAccessManager::finished 一处统一填（见 noteReplyProtocol()），不在这四个
    /// finished 槽里各写一遍 —— 那四个槽里任何一个漏了，症状都只是"这个主机的并发上不去"，
    /// 不报警，很难发现。
    QSet<QString> m_http2Hosts;
    QSet<QString> m_http1OnlyHosts;

    // 批次记账
    int m_totalJobs = 0;
    int m_doneJobs = 0;
    int m_failedJobs = 0;
    int m_skippedJobs = 0;
    QStringList m_failedPaths;
    /// 这一批里"体检点过名"的那几条（见 setFlaggedPaths()）。按路径认人，beginBatch() 清空。
    QStringList m_flaggedPaths;
    /// 和 m_failedPaths **一一对应**的那一份详细信息（多了 url / sha1 / 字节数 / 退路），
    /// 只给"重试失败的那几条"用 —— 见 failedJobs()。beginBatch() 清空。
    ///
    /// NOTE: 为什么不复用 m_failedPaths：那一份是给**日志和界面**看的（"哪些文件没下来"），
    /// 序号也是按它算的；两张表合成一张的话，"重试"就得从一条路径反推出是拿哪个
    /// 地址、按什么 sha1 下的 —— 推不出来（同一个目标路径可能来自镜像或官方两条地址）。
    QList<FailedJob> m_failedJobInfos;
    QString m_currentFile;
    bool m_batchOpen = false;
    bool m_batchEmitted = false;
    // 析构时 abort() 可能同步回调进 finished 处理函数，用它挡住
    bool m_shuttingDown = false;
    // 下载源。false = 官方（默认）。
    bool m_useMirror = false;
    QString m_versionManifestUrlOverride;
    bool m_allowOverwrite = true;
    bool m_segmentEnabled = true;
    /// 正在 `ResumableDownload::start()` 的调用栈里：它可能同步发 finished，
    /// 那一次结账要推迟到栈退出之后（见 finishSegmentedJob）。
    bool m_startingSegment = false;
    /// 空 = `DownloadCache::root()`（见 setRangeCacheRoot()）。
    QString m_rangeCacheRoot;
    /// 同时最多几条在飞（出厂 = `kMaxConcurrentDownloads`；设置里能改，见那四档）。
    int m_maxConcurrentDownloads = kMaxConcurrentDownloads;
    /// 文件正文强制 HTTP/1.1（见 setPreferHttp1ForFiles()）。
    bool m_preferHttp1ForFiles = true;

    // ------------------------------------------------------------------
    // 下载限速
    // ------------------------------------------------------------------
    // / 上限（字节/秒），0 = 不限（出厂）。归一化在 DownloadSpeedLimit.hpp。
    qint64 m_speedLimitBytesPerSecond = DownloadSpeedLimit::kUnlimited;
    /// 100 ms 一拍的令牌泵。**只有设了上限才在跳**：不设的时候连一次空转都没有。
    QTimer *m_speedPump = nullptr;
    /// "缓冲区里有数据、等着被限速放行"的回复。这一张表**只**在限速时非空。
    QSet<QNetworkReply *> m_throttledReplies;
    /// 令牌桶。**可以和外层那个下载器共用**（见 shareSpeedLimitBucketWith()）：
    /// 分段作业的内部下载器拿的是同一份，不然每个作业各算一份上限、"1 MB/s"实际能跑出好几倍。
    struct SpeedBucket {
        /// 上限（字节/秒），0 = 不限。共用时改在这里，所有用它的下载器下一拍都按新值走。
        qint64 limit = DownloadSpeedLimit::kUnlimited;
        /// 令牌余额（字节，带小数：低档位一拍只放几百字节，取整会把速率拖到远低于设定值）。
        double tokens = 0;
        /// 上一次续令牌的时刻（`clock` 读数）。共用时两个下载器先后续，用同一个时刻。
        qint64 lastRefillMs = 0;
        QElapsedTimer clock;
        /// 有几个下载器**借用**了这一份（见 shareSpeedLimitBucketWith()）。0 = 只有建它的那个。
        ///
        /// `QSharedPointer` 没有公开的引用计数可问（`useCount()` 在 Qt 6 里
        /// 不存在），而"这一拍没人等就把余额抹掉"这条只在**独占**时成立：共用时外层这一拍
        /// 没人等，不代表内层那个分段作业也没人等，抹了就是每 100 ms 把它的节奏打回零。
        int sharers = 0;
    };
    QSharedPointer<SpeedBucket> m_speedBucket;
    /// 这一份桶是**借来的**（`shareSpeedLimitBucketWith()` 加过 `sharers`）。析构时要还回去
    /// —— 分段作业那个内层下载器是每个作业新建的，不还的话外层的 `sharers` 只增不减，
    /// "没人等就把余额抹掉"那条永远不成立（一次几秒的作业会让之后每一次都自带一小口余量）。
    bool m_speedBucketBorrowed = false;

    // ------------------------------------------------------------------
    // 失速看门狗（见 .cpp 顶部那段说明）
    // ------------------------------------------------------------------
    // / 一条在飞回复的记账。**每一条**发出去的回复都进这张表（不是只有走镜像那些）：
    // / - bytesSeen / lastTickBytes / strikes 只有"走了镜像、而且登记过官方备用地址"的
    // / 回复才看 —— 那种才有退路，abort 掉它是"换一条路再试"；别的回复 abort 掉就是
    // / 纯粹把这次下载弄失败；
    // / - lastSignOfLifeMs 对**所有**回复都看：发出去之后一直没动静的那条，谁都不该一直
    // / 攥着它（见 kNoProgressGiveUpMs，那一条救的正是"没有退路"的回复）。
    struct StallMeter {
        /// 到手的累计字节（高水位，只增不减）。取的是 `bytesAvailable()` —— 为什么不取
        /// downloadProgress 报的那个数，见 onStallWatchdogTick 的 (0)。
        qint64 bytesSeen = 0;
        qint64 lastTickBytes = 0;  ///< 上一跳时看到的那个值
        int strikes = 0;           ///< 连着几跳"没动"
        /// 最近一次**有任何动静**的时刻（发出去那一刻就先记一次），m_clock 的毫秒读数。
        qint64 lastSignOfLifeMs = 0;
    };
    QHash<QNetworkReply *, StallMeter> m_stallMeters;
    QTimer *m_stallWatchdog = nullptr;
    QElapsedTimer m_clock;
    /// 主机名 → 降级期的截止时刻（m_clock 的毫秒读数）。表里没有 = 没降级过。
    /// **按主机各记各的**，理由见 mirrorDemotedFor()：整合包那条路上"镜像"是每条文件
    /// 各自的首选地址，一个全局的降级位会让一个慢 CDN 把别的 CDN 一起关掉。
    QHash<QString, qint64> m_mirrorDemotedUntilByHost;
    /// 镜像主机 → "改道官方之后连着失败了几条"。
    QHash<QString, int> m_reroutedOfficialFailures;

    /// 落点主机 → "死到什么时候"（m_clock 的毫秒读数）。302 转手之后一个字节都不来、
    /// 或者从这台落点收到的字节长期不够的，记进来；到期前再转到它就直接 abort，
    /// 代价只有一个往返（不算 12 秒、更不算 90 秒）。见 kDeadRedirectHostBanMs。
    QHash<QString, qint64> m_deadRedirectHosts;
    /// 最近窗口内"落点死了"的时刻（m_clock 读数，先进先出）。攒够
    /// kDeadRedirectHitsToProbe 条就探一次官方，见 probeOfficialRoute()。
    QList<qint64> m_deadRedirectHits;
    /// 探测官方用哪条地址：最近一次死落点那条作业的官方地址（一定是真实存在的文件）。
    QString m_officialProbeUrl;
    /// 同一时刻只发一条探测。
    bool m_officialProbeInFlight = false;
    /// 探测失败之后到什么时候为止不再探（m_clock 读数）。见 kOfficialProbeCooldownMs。
    qint64 m_officialProbeFailedUntilMs = 0;
    /// 判"卡死"的期限，默认 kNoProgressGiveUpMs（只有验证工程会改它）。
    qint64 m_noProgressGiveUpMs = kNoProgressGiveUpMs;
    /// 判"镜像有退路、却连首字节都没等到"的期限，默认 kNoFirstByteGiveUpMs
    /// （只有验证工程会改它）。
    qint64 m_noFirstByteGiveUpMs = kNoFirstByteGiveUpMs;

    // kMaxConcurrentDownloads 的声明在上面 public 那一节（紧挨着 mayIssueAnotherDownload），
    // 因为验证工程要按那个数钉边界；理由写在那处。


    /// 一条**被降级改道到官方**的回复成/败了：记一笔，必要时撤掉那只镜像主机的降级。
    ///
    /// 只认"连不上"这一类错误（超时 / 连不上 / 代理断了）；HTTP 404 这种不算 —— 那是
    /// 文件本身的事，跟"官方这条链路通不通"无关。
    /// 成功一条就把计数清零：官方只要还在出数据，慢镜像那只主机就该继续被绕开。
    /// @param forceUnreachable "90 秒一个字节都没有"那种卡死：错误码是 NoError，
    /// 光看 error() 判不出来，由调用方直接告诉这里"它确实连不上"。
    void noteReroutedReplyOutcome(QNetworkReply *reply, bool success,
                                  bool forceUnreachable = false);

    /// 看门狗 abort 掉一条**302 转过手**的镜像回复之后，先回镜像入口再试一次
    /// （入口会重新挑一台落点），而不是直接退官方。镜像这一侧只重试一次。
    /// @return 真的重排了 = true（调用方按"作业还在"收尾：不记失败、不增完成数）。
    bool retryStalledFromMirror(QNetworkReply *reply, const QString &destinationPath);

    /// 一条镜像回复在 302 之后**一个字节都没收到**：把落点那台主机记进"死落点"表
    /// （到期前再转到它就直接 abort，见 kDeadRedirectHostBanMs），并在窗口内攒够
    /// kDeadRedirectHitsToProbe 条之后探一次官方。
    /// @param landed 302 最终落到的地址（reply->url()）。
    /// @param entry 这一发请求的入口地址（reply->request().url()）。
    /// @param officialFallback 这条作业的官方地址（探测就用它；空 = 这次不提供）。
    /// NOTE: 判"换了一台落点没有"用的是**主机 + 端口**：同一个主机名换端口也是换了一台
    /// （本地假服务器 / 反代就是这种形状），只比主机名会把它们当成同一跳。
    void noteDeadRedirectTarget(const QUrl &landed, const QUrl &entry,
                                const QString &officialFallback);

    /// 用 HEAD 探一下官方那条到底通不通；通了才把 mirrorHost 降级。
    /// 只在"镜像的落点短时间内连着死"之后被调到；同一时刻只有一发在飞。
    void probeOfficialRoute(const QString &mirrorHost);

    /// 从镜像取失败时，用官方地址**重发一次**（作业还是同一个，不算失败）。
    /// @param reply 刚失败的那条回复。
    /// @param destinationPath 落盘路径（原样搬给重发出去的那一条）。
    /// @param finishedSlot 重发出去那条回复该接哪个 finished 槽 —— 索引有自己的
    /// （onAssetIndexDownloaded，成功了还要解析索引），别的都走通用的那个。
    /// @return 真的重发了 = true（调用方必须立刻 return，别走结账那一段）；
    /// 没有备用地址 = false（调用方按普通失败处理）。
    bool retryWithOfficialUrl(QNetworkReply *reply, const QString &destinationPath,
                              void (DownloadManager::*finishedSlot)());

    void startDownload(const QUrl &url, const QString &destinationPath,
                       const QString &expectedSha1 = QString(), qint64 expectedSize = -1,
                       const QString &officialUrl = QString(), int retryAttempts = 0,
                       const DownloadRange &range = DownloadRange());
    bool requeueTransientFailure(QNetworkReply *reply, const QString &destinationPath);
    /// downloadFile() 的本体：登记进批次、跳过已经下好的、剩下的排进 m_pendingJobs，
    /// 再由 pumpDownloadQueue() 按连接预算发出去。
    void scheduleFile(const QString &url, const QString &destinationPath,
                      const QString &expectedSha1, qint64 expectedSize, VerifyMode mode,
                      const QString &officialUrl = QString(), const DownloadRange &range = DownloadRange());
    /// 把队列里排着的作业发出去，直到连接用完（判据 = mayIssueAnotherDownload() 加
    /// mayIssueAnotherDownloadToHost()）。
    void pumpDownloadQueue();

    /// 限速开着时每条回复最多预读多少字节（不限速时**不用**这个数，仍是 256 KiB）。
    ///
    /// 为什么不能继续用 256 KiB：QNAM 会把正文一口气读进自己的缓冲，而 `finished` 那一刻
    /// 缓冲区里剩下的字节是被 `stream->drain(reply)` **一次写完**的 —— 也就是说这一段天然
    /// 绕过令牌桶。文件比缓冲小的时候整份都在这一段里：实测 2000 × 32 KiB、限 4 MiB/s，
    /// 3.0 s 就下完了（≈5 倍于上限）。限速时按"一拍里这一路能分到多少"给缓冲，
    /// 让 TCP 在源头上就被按住（配合收尾那一口记账 + 欠账没还清时不发新作业，
    /// 见 onSpeedPumpTick / pumpDownloadQueue / onDownloadReplyFinished）。
    qint64 throttledReadBufferBytes() const;

    /// 把一条作业真正交给网络：计进 m_totalJobs、算出主机与"要不要分段"，排进
    /// m_pendingJobs，再泵一次。
    ///
    /// `scheduleFile()` 与"借文件没借到、退回网络"（onReuseFinished）共用这一处 ——
    /// 两条路的记账必须一模一样，否则用户会看到"总数比实际下载数少一个"这种账。
    void queueNetworkJob(const QString &url, const QString &destinationPath,
                         const QString &expectedSha1, qint64 expectedSize,
                         const QString &officialUrl, const DownloadRange &range);

    /// 往后台池里丢新作业，直到 m_reuseActive 到 4 条。
    void pumpReuseQueue();
    /// 一条借文件作业的结账（主线程）：成功记 reused，失败退回网络（见 .cpp）。
    void onReuseFinished(const ReuseJob &job, LocalFileReuse::Method method, const QString &error);
    /// 把排着队的借文件作业全部丢掉，并让已经在跑的那些尽快收手（换一个取消令牌）。
    ///
    /// 什么时候调：beginBatch（新一批不继承上一批的）、abandonBatch / cancelBatch、析构。
    /// **不**改 m_reuseActive：那几条还在池子里跑，它们回来时会自己减掉。
    void clearReuseJobs();

    /// 这一条调度要不要**整份读文件**才能问出来（比 SHA-1，或算原件指纹）。
    ///
    /// 便宜的那几档（不存在 / 0 字节 / 点名 / 只看存在 / 大小对不上且没要求指纹）在这里
    /// 回 false —— `scheduleFile()` 就照老样子在界面线程上一句话问完。回 true 的那些
    /// 交给 `pumpPrecheckQueue()`。
    bool precheckReadsFileBody(const QString &path, const QString &expectedSha1, qint64 expectedSize,
                               VerifyMode mode, const DownloadRange &range) const;
    /// 往后台池里丢一条问盘作业（一次只有一条在跑，见 m_precheckPool 的说明）。
    void pumpPrecheckQueue();
    /// 一条问盘作业的结账（主线程）：通过 = 跳过，不通过 = 走 `continueAfterPrecheck()`。
    void onPrecheckFinished(const PrecheckJob &job, bool valid, const QJsonObject &original);
    /// 核对过之后的那一段尾巴：登记原件指纹 → 借文件 → 排进网络队列。
    ///
    /// `scheduleFile()` 的便宜分支与后台核对完成后的回调**共用这一处** —— 两条路的记账
    /// 必须一模一样，否则会出现"后台核对过的那几个没登记进恢复记录"这种账。
    /// @param original 后台已经算好的原件指纹；nullptr = 现算（便宜那一支：文件根本不在）。
    void continueAfterPrecheck(const PrecheckJob &job, const QJsonObject *original);
    /// 把排着队的问盘作业全部丢掉，并让已经在跑的那一条尽快收手（换一个取消令牌）。
    ///
    /// 什么时候调：beginBatch、abandonBatch / cancelBatch、析构。**不**改 m_precheckActive：
    /// 那一条还在池子里跑，它回来时会自己减掉（代次对不上，结论直接丢）。
    void clearPrecheckJobs();
    /// 把一个**已经在途过**的作业放回队列**最前面**（重试 / 换源 / 补投都走这里）。
    ///
    /// 为什么不 `enqueue()` 到队尾：一条重试排在几千个还没发过的资源对象后面，等于
    /// "失败一次就多等一整轮" —— 一条 429 重试在队尾会等很久。
    /// 排头不会堵住别人：`pumpDownloadQueue()` 会跳过 `readyAtMs` 还没到的作业，
    /// 退避期间的排头作业只是被跳过，后面的作业照发。
    void requeueDownloadJob(const PendingJob &job);
    /// 这条作业是否适合安全分段：大小已知且在 [4 MiB, 32 GiB]、有 SHA-1、不是分段请求本身、
    /// 并且是 http(s)。纯函数，便于测试。
    static bool segmentedEligible(const QString &url, const QString &expectedSha1,
                                  qint64 expectedSize, const DownloadRange &range);
    /// 把一个满足判据的作业交给 ResumableDownload（探测 + 最多 4 路 Range + 合并校验）。
    void startSegmentedDownload(const PendingJob &job);
    /// ResumableDownload 结束时的结账：成功 / 退官方 / 退普通下载 / 记失败，见 .cpp。
    void finishSegmentedJob(ResumableDownload *task, bool success, const QString &error);
    /// 所有分段作业按 4 条连接占用的全局额度（保守：探测/合并阶段实际更少）。
    int segmentConnections() const;
    /// 这个主机当下有几条在飞。**从 activeDownloads 现算**，不另开一个计数器：
    /// 那张表有一堆摘除点（成功 / 失败 / 退回官方 / 看门狗判死），多一个计数器就多一处
    /// 能对不上的地方，而对不上的症状是队列**永远发不出去**（整批挂死）。
    /// 主机名从回复自己的 request() 取 —— 从队列那头是推不出来的，镜像那条 302 之后
    /// 落在哪个主机上只有回复知道。
    int activeCountForHost(const QString &host) const;
    /// 一条回复收尾时，顺手记下它用的是不是 HTTP/2（挂在 QNetworkAccessManager::finished 上，
    /// 每一条回复都走这里，一条连接的事只问一次）。
    void noteReplyProtocol(QNetworkReply *reply);
    /// 把一条"发出去之后一直没有动静、而且大概永远不会结束"的回复**判失败**，自己做完整套
    /// 记账。为什么不能只 abort() 了等它自己走 finished：这条路的整个前提就是"它可能永远
    /// 不结束"，指着 abort 收尾等于把整批挂死（见 kNoProgressGiveUpMs）。
    void failWedgedDownload(QNetworkReply *reply);
    /// 日志里给一条回复起个名字：文件作业是落地路径，元数据作业是「版本清单」/「版本 json」。
    /// 元数据作业不在 activeDownloads 里，只打那里的路径会打成空串。
    QString jobLabel(QNetworkReply *reply) const;
    void handleDownloadError(QNetworkReply *reply, const QString &errorMessage);
    void checkAllDownloadsFinished();
    void emitBatchProgress(const QString &currentFile);
    bool fileAlreadyValid(const QString &path, const QString &expectedSha1, qint64 expectedSize,
                          VerifyMode mode);
    /// 读索引、把里面每个对象排进下载队列。
    /// 起**回 bool**：索引读不动 / 不是 JSON 是"这一趟安装没有资源可下"的
    /// 真故障，只 `qWarning` 就 return 不行 —— 索引这条**故意不占** total/done 的格子，
    /// 于是整批照样报成功（版本装好了，材质音效一个都没有）。调用方拿 false 走失败那条路
    /// （`handleDownloadError` + 镜像退官方），用户至少能看见"索引解析失败"。
    /// 另外两种"看着像、其实不是索引"的也回 false：`objects` 为空、以及对象全被
    /// 40 位 hash 那道过滤剔掉（一个都没排上）。
    bool parseAndDownloadAssets(const QString &indexPath, QString *problemOut);
};

Q_DECLARE_METATYPE(MinecraftVersion)
Q_DECLARE_METATYPE(Library)
Q_DECLARE_METATYPE(AssetIndex)

#endif  // DOWNLOADMANAGER_H
