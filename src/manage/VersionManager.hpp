/**
 * @file VersionManager.hpp
 * @brief 声明版本发现、元数据获取与下载编排接口。
 * @note 版本列表、详情和下载由这一层统一管理。
 */
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QFuture>
#include <QObject>
#include <QStringList>

#include <functional>
#include <atomic>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QTimer;

#include "../download/DownloadManager.hpp"
#include "../download/LoaderMeta.hpp"
#include "../download/VersionCategory.hpp"
#include "InstanceMetaFile.hpp"
#include "ModpackExport.hpp"
#include "ModpackImport.hpp"
#include "VersionDisplay.hpp"
// 体检那一层（checkInstanceIntegrity 的返回值类型）。它自己不反过来包含本文件 ——
// 那一层只认 DownloadManager 的结构体和 AppPaths，谁都能拿去用。
#include "VersionIntegrity.hpp"

class VersionManager : public QObject {
    Q_OBJECT

public:
    explicit VersionManager(QObject *parent = nullptr);
    ~VersionManager();

    // Version management
    // 扫 .minecraft/versions，返回磁盘上真正装好了的版本号。判据是"认得出客户端 jar"
    // —— 先认 <id>/<id>.jar，认不到再看顶层是不是**恰好一个** jar（用户自己在资源管理器
    // 里改过目录名的实例就靠这条被认出来）。判据本体在 InstanceFiles。
    // 一个都没有时返回空列表，不再塞 "自定义版本" 占位项。
    //
    // warnings 是可选的收集器：某个目录里**有 jar、但认不出哪个是客户端 jar**（顶层不止
    // 一个、又没有和目录名同名的那个）时，往里追加一句说明。一个 jar 都没有的目录
    // （下载残骸、纯空目录、误丢进来的整合包 zip）不报 —— 那些本来就不像实例。
    // 传 nullptr 时只是不收告警，返回的列表逐项一致。
    QStringList installedVersions(QStringList *warnings = nullptr) const;
    /// `installedVersions()` 的后台版：扫盘在工作线程里做，结果从
    /// `installedVersionsScanned` 回来。
    ///
    /// 版本清单从网络回来那一下会顺带重扫一遍 `versions/`（20 份 63 ms、200 份 116 ms），
    /// 那笔钱落在"用户正在等下载页"的同一段时间里。调用方原来同步拿结果的，现在改成接
    /// 这个信号 —— 结果回来之前界面保持上一次的样子。
    ///
    /// 两条保证：
    /// * **连着触发两次，旧结果不许盖新结果**：每次请求 +1 一代，回调只认最新那一代；
    ///   上一发还在跑的话会被取消旗叫停（每个目录看一眼）。
    /// * **对象析构后不再回调**：watcher 挂在本对象上，跟着一起销毁；工作线程只持有
    ///   路径 / 取消旗，不碰 `this`。
    ///
    /// @param reportWarnings 要不要收"有 jar 但认不出客户端"的告警（见 `installedVersions()`）。
    void requestInstalledVersions(bool reportWarnings = true);
    QStringList availableVersions() const;

    /// 四个分类里的一个，连同它底下的版本。
    struct VersionChannel {
        VersionCategory::Channel channel = VersionCategory::Channel::Release;
        QString key;    ///< 见 VersionCategory::channelKey()（界面认条目的那个键）
        QString text;   ///< 见 VersionCategory::channelText()（给用户看的四个字）
        QList<MinecraftVersion> versions;  ///< 清单**原顺序**（新到旧），不是重排过的
    };

    /**
     * @brief 把一份版本清单按下载页那四个分类分好堆（**纯函数**）。
     * @param versions 清单（`DownloadManager::getAvailableVersions()` 那份，或任何同形状的）。
     *
     * 永远是 **4 个**（空的那一堆也在，界面四条一条不少），顺序就是界面上从上到下的顺序。
     * 每一堆里的顺序**原样保留**（清单本来就是新到旧）。
     *
     * 分堆的判据只有一处：VersionCategory::channelFor()（含那张愚人节版的名单，以及
     * "为什么不能用发布日期当判据"的实测记录）。这里只负责按它分堆 —— 同一套规则抄两遍，
     * 早晚会一边改一边漏。
     *
     * NOTE: 写成**静态**、收一个列表进来，是为了让验证工程能拿真清单（915 条那份夹具）直接
     * 喂进来断言"四堆加起来一条不丢"。做成成员的话，验证工程就得先能替换掉
     * VersionManager 里那个 DownloadManager 才够得着 —— 那种"只给测试开的注入口"
     * 在生产代码里是纯负担。
     */
    static QList<VersionChannel> groupIntoChannels(const QList<MinecraftVersion> &versions);

    /**
     * @brief 「可安装版本」那份清单，按下载页那四个分类分好堆（走 downloadManager 手上那份）。
     *
     * 清单还没拉回来时四堆都是空的（不是"返回空列表"）—— 界面据此照常画四条、只是点开是空的，
     * 而不是整块消失。
     */
    QList<VersionChannel> availableVersionChannels() const;

    /// 「最新版本」那一组里的一条（清单里最新的正式版 / 最新的快照）。
    struct LatestVersion {
        VersionCategory::Channel channel = VersionCategory::Channel::Release;
        MinecraftVersion version;
    };

    /// 「最新版本」那一组的**键**（界面拿它当顶层条目那个身份，进 `Qt::UserRole`）。
    ///
    /// NOTE: 它和四个分类键（`VersionCategory::channelKey()`）**必须两两不同**：树上那五个
    /// 顶层身份住在同一个角色里，而 `MainWindow::refreshAvailableVersions()` 记
    /// "哪一条是展开的"记的就是这个身份 —— 撞了之后展开状态会串到另一条上
    /// （用户展开"远古版"，回来发现是"愚人节版"开着）。
    /// 还有一条更隐蔽的：**顶层身份不能和任何版本号相同**（子条目拿版本号当身份），
    /// 所以这个键取的是一个不可能成为版本号的词。
    static QString latestGroupKey() { return QStringLiteral("latest"); }

    /// 「最新版本」那一组给用户看的名字。
    static QString latestGroupText() { return QStringLiteral("最新版本"); }

    /**
     * @brief 挑出「最新版本」那一组的两条（**纯函数**）：清单里最新的正式版与最新的快照。
     * @param versions 清单（`DownloadManager::getAvailableVersions()` 那份，或任何同形状的）。
     *
     * 顺序**固定**：正式版在前、快照在后；缺哪一类就只有一条，清单为空就是空列表
     * （界面照旧把这一组摆出来，只是里面没东西 —— 和四个空分类是同一个规矩）。
     *
     * NOTE: 判据走 `channelFor()` 而不是 `type == "snapshot"`：愚人节版在清单里也是
     * `snapshot`，按 type 判会让它当上"最新快照"（见 VersionCategory 那段说明）。
     * 同样地，**不分**正式版/快照与"用户想装哪个"——这里只回答"最新的是哪两个"。
     *
     * NOTE: 写成**静态**、收一个列表进来，理由和 groupIntoChannels() 那段一模一样
     * （验证工程能拿真清单夹具直接喂进来）。
     */
    static QList<LatestVersion> latestVersions(const QList<MinecraftVersion> &versions);

    /// 走 downloadManager 手上那份（界面用的）。
    QList<LatestVersion> availableLatestVersions() const;

    /**
     * @brief 这一格值不值得写进整合包清单的 `gameVersion`（**纯函数**，导出那两格共用）。
     * @param id 候选（`wtfml.meta` 的 realVersion / json 的 clientVersion / 链末端的 id /
     * 实例目录名）。
     * @param knownVersions 在线清单（`DownloadManager::getAvailableVersions()` 那份，或空表）。
     * @return 能不能当版本号用。
     *
     * 两种情形，看清单在不在手上：
     * ① **清单非空 ⇒ 以它为准**（`knownVersions` 里有这个 id 才算数）。这一条同时管两件
     * 相反的事：放行不以数字开头的老版本号（`rd-132211`，清单里真有、也装得出来），
     * 拦住"长得像版本号但 Mojang 没这个 id"的东西（真机上的 `20w14∞`，正式 id 是
     * `20w14infinite`）。详见 .cpp 里那段；
     * ② **清单是空表 ⇒ 看长相**：以数字开头且一个空白都没有。这一条是离线第一趟的退路，
     * 挡着 `原版ATT2地图（无模组）` 和 `1.20.2-Fabric 0.17.2` 这类最明显的假版本号。
     *
     * NOTE: 写成**静态**、收一个列表进来，理由和 groupIntoChannels() 那段一模一样
     * （验证工程能拿真清单夹具直接喂进来）。
     */
    static bool isKnownGameVersion(const QString &id,
                                   const QList<MinecraftVersion> &knownVersions);

    // Version fetching from Mojang API
    void fetchAvailableVersions();
    void fetchVersionDetails(const QString &versionId);

    // Version downloading
    void downloadVersion(const QString &versionId, const QString &destinationPath);

    /**
     * @brief 校验一个**已经装好的**实例，只补缺的和损坏的（实例设置「补全文件」那颗按钮）。
     *
     * 和 downloadVersion() 干的是同一件事的两半：同一个批次、同一批文件、同一套
     * "已有且校验通过就跳过"的规矩，收尾也走同一个 versionDownloadFinished。区别只有三处，
     * 每处都是"装一个新的"与"修一个旧的"本来的差别：
     *
     * ① **json 从盘上读**（getVersionJson()，走继承链合并），不是从 versionJsons 那张内存表
     * 取。那张表只装了"这一次运行里拉过详情"的版本，而"补全一个早就装好的实例"恰恰
     * 最常发生在重开启动器之后 —— 那时候表是空的，照 downloadVersion() 的做法会直接
     * 报"尚未获取到该版本的 version.json"。盘上那份 json 是这个实例的权威（启动读的
     * 也是它），补文件这件事不该要求先联网。
     * ② **不写 version.json**：这一趟只要"文件齐了"，盘上那份原样留着。
     * ③ **jar 的落点走 InstanceFiles::jarPath()**：加载器实例自己没有 jar，它的客户端 jar
     * 是**父版本**那一份（InstanceFiles::jarOwnerId()）。downloadVersion() 里那句
     * `destinationPath + "/" + versionId + ".jar"` 对"新装一个版本"是对的，照搬到这里
     * 却会给加载器实例多写一个没人读的 jar。
     *
     * 为什么要有它：体检和补全**问盘的判据不一样**。jar / 库两边都是"字节数 + sha1"，
     * 体检说坏了补全自己也会重下；但资源对象那一档补全只看在不在（ExistsOnly，几千个
     * 文件全量哈希会让界面卡几秒），于是"尺寸不对"的对象体检报「坏」、补全跳过它 ——
     * 用户看到"体检说要修 1 个、补全说一个都不用下"。把体检那份清单喂进来，两边就对上了。
     * （口径见 DownloadManager::setFlaggedPaths()；**只认路径、不分类别**。）
     *
     * NOTE: 收尾信号被调用方当成两用（见 `InstallOccupancy` 的「补全」那一档），所以
     * 这里的早退**必须**发终态 —— 界面点一下就在等一个结果，不发就是"点了没反应"。
     * NOTE: 想先把 json 拉一遍再补的老路走不通：那要求联网，而损坏的文件里就有可能是网络
     * 本身弄坏的。盘上那份 json 足够算出要找哪些文件。
     */
    void repairVersionFiles(const QString &versionId,
                            const QStringList &flaggedPaths = QStringList());

    /**
     * @brief 预取 GTNH 首启 MCLib / AssetDirector 要的资源（2.1.6b）。
     *
     * 请求集合不在整合包里（见 docs/features/MODPACK_IMPORT.md「请求集合怎么得到」的结论），所以这里用的是
     * **随产品发布的一份已知集合清单**（`AssetDirectorKnownSet`）：按实例 `mods/` 里的
     * 文件名认出消费者模组（今天只有 Et-Futurum-Requiem 2.6.2.25-GTNH），再按官方索引的
     * 字节数 + SHA-1 补齐对象。
     *
     * 写哪里：**只写** `<数据目录>/assets/asset_director`（AD 自己的目录）。共享
     * `assets/objects` 只读复用、一个字节都不写；游戏目录、模组文件不碰。为什么是那里、
     * 以及 AD 只判存在不重算哈希这条信任边界，见文档。
     *
     * 语义：
     * - 认不出消费者、实例还没导完、盘不可用 → 直接返回（调用方当“不适用”）；
     * - 体积超过 `kMaxKnownSetBytes`（256 MiB）→ 发一次失败终态，**一个字节都不写**；
     * - 已在盘上且校验通过的（AD 目录优先，其次共享 objects）不下；其余走
     *   `DownloadManager` 那一套：字节数 + SHA-1 校验、失败重试、看门狗、原子落盘；
     * - 取消不删文件：已经校验过的留在原处，下一次预取会跳过。
     *
     * 终态走**公共**的 `versionDownloadFinished(id, ok, msg)`（和补全、安装、导入同一条），
     * 界面那边靠自己记的实例名分档。message 为空 = 无需打扰用户（全都在盘上了）。
     *
     * @return 是否真的开了一批下载（`false` = 不适用 / 被拒，终态已经发过或一个字节都不动）。
     */
    bool prefetchAssetDirectorResources(const QString &versionId);

    /// 「这个实例要不要/能不能预取」的一次只读核算。**不写盘、不发信号**。
    ///
    /// 界面在决定"要不要为它开一批下载、建一件任务"之前得先知道答案：不适用（绝大多数实例）
    /// 与"已经齐了"两种情况下不该凭空多出一条任务行。
    struct AssetDirectorPrefetchPlan {
        bool applicable = false;   ///< 认出了已知集合的消费者模组
        bool upToDate = false;     ///< 认出来了，但一个字节都不用取
        QString setId;
        QString note;              ///< 清单里给人看的那句话（超上限时用来说"是哪套"）
        QString minecraftVersion;
        QString refusal;           ///< 非空 = 不执行（超上限 / 清单坏了）
        int objects = 0;
        int reuseOverlay = 0;
        int reuseAssets = 0;
        int toDownload = 0;
        qint64 bytesToDownload = 0;
        qint64 bytesTotal = 0;
    };
    AssetDirectorPrefetchPlan assetDirectorPrefetchPlan(const QString &versionId) const;

    // ------------------------------------------------------------------
    // 取消 / 重试
    // ------------------------------------------------------------------

    /// 现在有一批下载在跑、**而且用户可以取消它**（= 界面那颗「取消」露不露的唯一判据）。
    ///
    /// 下载批次及整合包后台解压/最终校验可取消；后台工作结束前保持实例占用。
    /// 加载器安装器进程不属于这类合作式后台工作，仍不能由这里中断。
    /// 历史上只有 `downloadManager->isBusy()`，会漏掉下载完成后的收尾阶段。
    bool canCancelDownload() const;

    /// 取消当前这一批（用户在任务详情页上点了「取消」）。
    ///
    /// 走的是和异常兜底**同一条**收尾路（`unwindDownloadBatch()`）：文件一个都不删，已经
    /// 原子提交过的那几个留在原处（下一趟重下会按 sha1 跳过），只是这一批到此为止、
    /// 该收的状态机都收掉。
    ///
    /// @return false = 当前没有可取消的批次（信号竞争、或者已经被别的东西收掉了）。
    bool cancelActiveDownload();
    /// Immutable UI intent for an interrupted file phase; never serializes a running installer.
    QJsonObject downloadContinuation() const;
    bool ownsFileBatch() const { return m_ownBatchOpen; }

    /// 上一次失败的那一批**能不能重做**（界面上那颗「重试」露不露的唯一判据）。
    bool canRetryDownload() const;

    /// 上一次失败那一批的**名字**（`FailedBatch::label`：资源下载是"模组名 版本号…"，
    /// 更新中心那一批是"「实例」里的 N 个更新"）。没有可重试的 = 空串。
    ///
    /// 界面用它在重试**之前**先记下"这一批叫什么"。`retryDownload()` 会把
    /// 失败清单整个吃掉，之后就没有地方再问得到这个名字了；而重试也要在任务中心里挂
    /// 一件任务、收尾时按名字认领那一发 `resourceDownloadFinished(label, …)`。
    QString retryBatchLabel() const;

    /// 重做上一次失败的那几个文件（**只重做失败项**，见 .cpp）。
    ///
    /// @return false = 没有可重做的，或者这会儿正有一批在跑（一次只有一批，见 .cpp）。
    bool retryDownload();

    /**
     * @brief 给一个**已经装好的**实例做体检：扫一遍它该有的文件，**只报不装**。
     *
     * 判据层在 VersionIntegrity（那个文件里写了"哪些文件算数""哪几种情况叫要修"）。
     * 这里只负责把这一份实例的**合并过**的 json 和两个解析结果递过去 —— 和
     * registerVersionFiles() 走的是同一个来源，所以体检报出来的文件集必然就是装配那一步
     * 会去下、会去写的那些。
     *
     * @param versionId 实例 id（= versions 下的目录名）。
     * @return 一趟的结论（含 count 和逐条问题）。读不出 json 时 error 非空、其余为空。
     *
     * NOTE: **同步**跑（3000 个资源对象就是 3000 次 stat，几毫秒；带哈希的只有 jar 和库）。
     * 以后那条界面路要把它放进"别卡住界面"的框里，是那边的事 —— 这一层不引定时器、
     * 不发信号，也就不需要状态机。
     */
    VersionIntegrity::Report checkInstanceIntegrity(const QString &versionId) const;

    /**
     * @brief 体检的**清单**（只列不查）：读 json + 解库/索引 + `VersionIntegrity::plan()`。
     *
     * 界面那条路要在**后台线程**里逐条比对（`VersionIntegrity::check()` 光读盘、不碰这个
     * 对象），所以先把"该有哪些文件"在 UI 线程上算好 —— 这一步很快（一次索引 json 的读盘 +
     * 几千条内存条目），真正花时间的是拿清单去哈希那几十兆的 jar 和库。把清单拎出来，
     * 后台那一趟就**一个成员都碰不到**（`check()` 是纯函数），也就不用给这个类加锁。
     *
     * @param versionId 实例 id。
     * @param error 出不来时写一句**给人看**的话（和 checkInstanceIntegrity 里那句同一个出处）。
     * @return 清单（顺序：jar → 库 → 资源索引 → 资源对象）；error 非空时是空表。
     */
    QList<VersionIntegrity::ExpectedFile> integrityPlan(const QString &versionId,
                                                        QString *error) const;

    /// 把"接着干下一件事"排到下一轮事件循环里执行，并给这一步套上异常兜底。
    ///
    /// **什么时候必须用它**：从异步回调里往回接续的时候 —— 网络回复的槽、安装器进程的信号、
    /// DownloadManager 的 allDownloadsFinished。这些回调都是 Qt 在**它自己的分发栈里同步**
    /// 调过来的，在里面直接起下一个下载批次就等于"在上一批的状态机上重入"：新批次整个跑在
    /// 上一批的栈里。这个形状会崩：Fabric 那一批的原版文件下完
    /// → allDownloadsFinished → 同步起"加载器自己的库"那一批 → extractAssetIndex()
    /// → Qt 内部一把 QReadWriteLock 的 unlock() 抛了没接住的 std::system_error
    /// → std::terminate → abort()。
    /// 排到下一轮之后，这一步是在事件循环上从零开始的栈里跑的，外面一把 Qt 的锁都没有。
    ///
    /// **什么算"下一步"**（要排队的）：起一个下载批次、起一个子进程、动磁盘上的文件。
    /// 只**再发一个 GET** 的不排：请求的接续点（收到回复的那个槽）本来就在事件循环上，
    /// 它不碰批次状态机。
    ///
    /// 兜底那半（catch）是同一次崩溃的第二个教训：这一步真抛了异常时别再让它一路穿到
    /// std::terminate —— 那正是"启动器静默消失、一句日志都没留下"的由来。现在异常会被翻成
    /// 一次正常的失败收尾（发 versionDownloadFinished(false, ...)），界面照常弹窗。
    ///
    /// @param step 下一步。用 lambda 捕获 this 传进来；**别传引用** —— 排队的这一步是在
    /// 调用方返回之后才跑的，那时候它栈上的东西已经没了。
    void deferNextStep(std::function<void()> step);

    // ---- 带模组加载器的版本 ----

    /// 下载页那四个单选钮对应的加载器。
    enum class LoaderKind { Fabric, Forge, NeoForge, OptiFine, Quilt };

    /// 整合包导入在数据层的推进阶段，供任务中心与进度视图同步分步状态。
    enum class ModpackStage { LoaderAndVanilla, ModpackFiles, ExtractingOverrides, Verifying };

    /// 这个加载器现在接通了没有。**四个都接通了**。
    ///
    /// 为什么四个都能装之后还留着这个函数：它回答的是"勾了这个钮会发生什么"，而界面要据此
    /// 决定"点了下载之后弹哪一种话"（能做 / 还没做）—— 两边各写一份 switch 的话，接通一个
    /// 加载器时漏改一处，症状是"明明能装却弹一句『还没做』"，或者更糟，反过来。
    ///
    /// NOTE: 这里写成**逐个 case 列出来**的 switch，不是 `return true`：将来往枚举里加第五个
    /// 加载器时，这个写法会让编译器替你盯一眼（少一个 case 就有 warning），而新的那个
    /// 默认落到 false —— 界面那句「还没做」和 installLoaderVersion() 里那道守卫就正好
    /// 接住它，不会出现"勾了一个没接线的加载器，装出来一个纯原版"。
    ///
    /// NOTE: 这四条路彼此都不一样（Fabric 是我们自己拉 json + 下库；Forge/NeoForge 是
    /// `--installClient`；OptiFine 是无界面入口 + 换 APPDATA + 临时目录）。这个函数
    /// 只说"能不能装"，不说"怎么装"，所以它们在它眼里一样。
    static bool loaderInstallSupported(LoaderKind kind);

    /// 写进 wtfml.meta 的 `loaderName`（显示名里那一段）。比如 "Fabric"。
    static QString loaderDisplayName(LoaderKind kind);

    /// 一共几个加载器（界面那组单选钮里加载器选项的个数）。
    static int loaderKindCount();

    /**
     * @brief 界面那组单选钮里的下标 -> 加载器。
     * @param index QButtonGroup 的 id（= MainWindowUI 那两张表里的下标）
     * @param out 成功时写入；失败时不碰
     * @return 这个下标对应一个加载器吗（-1「一个都没勾」、以及「不装」那个下标都是 false）
     *
     * 为什么要有这么一层：界面上"勾了哪个"是个下标，而"装什么"是个枚举，中间这层翻译
     * 要是各写各的 switch，多一个加载器时漏改一处就是"勾了 Fabric 装出 Forge" —— 不报错，
     * 装完才发现。所以翻译只有这一处，verify 里按名字逐项对过。
     */
    static bool loaderKindFromIndex(int index, LoaderKind *out);

    /**
     * @brief 加载器 -> 界面那组选项里的下标（loaderKindFromIndex 的反函数）。
     * @return 下标；枚举里多了一个、而上面那张表还没跟上时返回 -1。
     *
     * 界面那边"每一条加载器一个控件"之后需要这个方向：`loaderVersionsFetched` 回来说的是
     * **枚举**，而"该填哪一条的控件"是**下标**。反着另写一张表的症状是"勾了 Fabric，
     * 版本列表填进 Forge 那一条"—— 不报错、只是装错东西。
     * （verify 里钉着这一对互为反函数。）
     */
    static int loaderIndexForKind(LoaderKind kind);

    /**
     * @brief 装一个带模组加载器的版本。
     * @param gameVersion 它基于的**原版**版本号（必须在"可安装版本"那张清单里）。
     * @param kind 装哪一种加载器。
     * @param javaPath 用哪个 Java 跑**外部安装器**（Fabric 用不上它，传空即可）。
     * 空 = 用 PATH 上的 `java`。界面上传的是"设置"里配的那个。
     *
     * 【Fabric】一次点击，两批下载：
     * 1. 拉加载器列表 → 挑一个版本（"最新那个正式版"）；
     * 2. 拉它的 profile json —— 那份**薄 json** 就是要落盘的版本 json；
     * 3. 原版没装就先把它按老路装好（客户端 jar + 库 + 资源，那才是能玩的那一半）；
     * 4. 再下加载器自己那几个库（maven 风格，见 LibraryRules::artifactFor），
     * 并把 `realVersion`/`loaderName`/`loaderVersion` 写进 <id>/wtfml.meta。
     *
     * 【Forge / NeoForge】它们装出来的东西里有**二进制补丁**（拿原版的客户端 jar 打出一份
     * srg 名 jar、再打出一份 client jar），不是靠下载能凑出来的，所以这条路是：
     * 1. 拉版本表（Forge 是 promotions_slim.json、NeoForge 是 maven-metadata.xml）→ 挑一个；
     * 2. 把它们的安装器 jar 下到 <数据目录>/cache/installers/；
     * 3. **另起一个 Java 进程**跑 `java -jar <安装器> --installClient <游戏目录>`
     * （异步 QProcess + 心跳 + 硬超时，见 runInstaller）；
     * 4. 核对它留下的那份版本 json（目录名与 inheritsFrom，见 checkInstallerResult）；
     * 5. 安装器**不下资源文件**，所以再走一遍 `downloadVersion(原版)` 把原版补齐
     * （json / 客户端 jar / 库 / 资源，已有的按现有规矩校验后跳过）；
     * 6. 写 wtfml.meta，发完成信号。
     * 它的前置条件只有一条：游戏目录里得有 `launcher_profiles.json`（安装器拿它认目录），
     * 没有就补一份最小的（见 ensureLauncherProfiles）。
     *
     * 【OptiFine】同样是"跑别人的安装器"，但那个安装器有三处不一样（实测记录见
     * LoaderMeta.hpp 里 OptiFine 那一节），所以这一段和上面那条**连顺序都不同**：
     * 1. 拉版本表（下载源是镜像就问 BMCLAPI 的 JSON 接口，否则读官方那 203 KB 的下载页）
     * → 挑一个**正式版**（两边的挑法不一样，见那两个纯函数）；
     * 2. 官方源还多一步：真文件在广告页后面，先把它那条带 token 的地址抠出来；
     * 3. 安装器 jar 下到 <数据目录>/cache/installers/（和 Forge 共用一个缓存目录）；
     * 4. **先把原版补齐** —— 安装器要拿原版的客户端 jar 当输入，这一点和 Forge
     * （装完再补）正好相反；
     * 5. 在系统临时目录里建一个**假的 .minecraft**（原版的 json / jar 各拷一份进去，
     * 再补一份最小的 launcher_profiles.json），把子进程的 APPDATA 指过去，跑
     * `java -Djava.awt.headless=true -cp <安装器> optifine.Installer`；
     * 6. 装完**去那个临时目录里认**它装出来的版本目录（名字是它自己拼的，我们算不出来），
     * 核对那份 json，再把版本目录和 `libraries/optifine/` 搬回真游戏目录 ——
     * 真游戏目录里只覆盖、不删任何东西；
     * 7. 最后照旧走一遍 `downloadVersion(原版)` 把原版补齐。
     *
     * NOTE: 两条路都在第 3 步（原版那一批）**复用** `downloadVersion()` 那条已经验过的
     * 老路，不是另写一份；所以它绑定的那几个信号（versionDetailsFetched /
     * versionDownloadFinished）在安装期间会被本类截住，UI 只会在整件事结束时收到
     * **一个**完成信号 —— 否则用户会在"原版下完"那一刻看到一个"下载完成"的框，
     * 然后加载器还在下/还在装，而按钮已经重新亮起来能点了。
     *
     * 失败一律以 `versionDownloadFinished(loaderId, false, 原因)` 收尾（还没算出
     * loaderId 时用 gameVersion 当 id），`loaderInstallMessage()` 里说清卡在哪一步。
     *
     * @param pinnedLoaderVersion 钉死加载器版本，或者是频道键（见
     * LoaderMeta::loaderChannelKey / loaderRequestFromChoice）：
     * * 一个**具体版本号** —— 跳过"拉版本表挑一个"那一步，直接用这个版本号。
     * 整合包导入走的就是这条路：它的依赖表里写着 `fabric-loader: 0.16.14`，而包里
     * 那些模组正是按这个版本配的，装"最新那个"是另一回事（多半也能跑，但那是猜）；
     * * `@latest` / `@stable` —— 按这一档挑（最新档连 alpha/beta/预览版一起算）；
     * * **空串** —— 默认行为（Fabric 挑"最新的正式版"、Forge/NeoForge 挑官方推荐版、
     * OptiFine 挑官方页那一行），也就是稳定档。
     * NOTE: 纯原版整合包不调这个函数（没有加载器要装），见 importModpack()。
     */
    void installLoaderVersion(const QString &gameVersion, LoaderKind kind,
                              const QString &javaPath = QString(),
                              const QString &pinnedLoaderVersion = QString());

    /**
     * @brief 装一个**用户起了名字**的纯原版实例（安装页那个「版本名称」框）。
     * @param instanceId 用户起的名字，就是 `versions/<名字>` 那个目录名。
     * 调用方**必须**先过 `InstanceFiles::validateDirName()`（合法的名字才调）——
     * 这里不查：和 importModpack() 同一个理由，判据只留一处。
     * @param gameVersion 基于哪个原版。
     *
     * 【它和"直接下这个版本"差在哪】`instanceId == gameVersion` 时**一模一样**：把原版那份
     * json、客户端 jar、库、资源按老路装齐，不发薄 json —— 因为"实例"就是原版本身。用户没改
     * 名字时走的就是这一支，界面上那个默认值正好等于版本号。
     *
     * 名字不一样时才多一步：装完原版，再写 `versions/<名字>/<名字>.json`：
     *
     * {"id": "<名字>", "inheritsFrom": "<原版号>", "type": "release"}
     *
     * 【为什么是薄 json 而不是抄一份完整的】和整合包实例、和 Fabric 装出来的那份**同一个
     * 形状**（见 startModpackFiles 上面那段）。抄一份完整 json 的坏处是它会变旧账：原版升了、
     * 资源索引换了，抄来的那份对不上。启动时 `getVersionJson()` 会顺着 inheritsFrom 合并，
     * 主类/jar/assetIndex/游戏参数全从父版本那一层来。
     *
     * **父版本那批文件本来就得在盘上**（客户端 jar 在 `versions/<原版号>/`、库和资源是全局
     * 共享的），所以这永远是两个目录、不是"把文件搬过去"。
     *
     * 名字撞上盘上已有的东西时**不在这儿拦**：调用方先问过用户（既有那句问话："已经装过了，
     * 要不要校验并修复？"），这里只负责"已有的文件按 sha1 校验后跳过"——也就是「重下=校验
     * 跳过」那条产品级约束。薄 json 是每次覆盖重写的（它就是一行字，没有"半旧"的问题）。
     *
     * 失败一律以 `versionDownloadFinished(instanceId, false, 原因)` 收尾。
     */
    void installNamedVanilla(const QString &instanceId, const QString &gameVersion);

    /**
     * @brief 装一个**用户起了名字**的、带模组加载器的实例。
     * @param instanceId 同 installNamedVanilla()。
     * @param gameVersion 它基于的**原版**版本号。
     * @param kind 装哪一种加载器。
     * @param pinnedLoaderVersion 钉死加载器版本（安装页上点开加载器选的那个）。空 = 按默认档挑
     * （"最新的正式版" / 官方推荐版）。
     * @param javaPath 同 installLoaderVersion()：跑外部安装器用哪个 Java。
     *
     * 全程复用 installLoaderVersion() 那条已经验过的路（它自己会把原版补齐），装完之后只多做
     * 一步：写 `versions/<名字>/<名字>.json`，`inheritsFrom` 指着**加载器装出来的那个目录**
     * （它的名字是安装器自己拼的，我们事先算不出来，所以到这一步才知道父版本是谁）。
     *
     * NOTE: 实例那份薄 json **刻意不写 wtfml.meta** —— 显示名里那三段（realVersion /
     * loaderName / loaderVersion）属于**加载器那个版本**，用户给实例起的名字就显示用户
     * 起的名字（写了的话会拼成"我的包-26.3-Forge_47.4.10-Forge"这种东西）。用户想看
     * 它是基于什么的，版本 json 里的 inheritsFrom 就是答案。
     */
    void installNamedLoader(const QString &instanceId, const QString &gameVersion, LoaderKind kind,
                            const QString &pinnedLoaderVersion = QString(),
                            const QString &javaPath = QString());

    /// 有"装一个带名字的实例"正在进行（含父版本那几段）。
    /// 和 isLoaderInstalling() / isModpackImporting() 一样，是"现在能不能点启动"那四个
    /// 判据之一（调用点在 MainWindow::onStartButtonClicked()，**不在** updateBottomBar()）。
    bool isNamedInstanceInstalling() const;

    /**
     * @brief 问某个加载器在某个游戏版本上有哪些版本可选（安装页那个展开的列表用）。
     *
     * **一个加载器一份请求，四个可以同时在飞**。同一个加载器上再来一发
     * 才把上一发 abort 掉（同一个游戏版本重新问、或者换了游戏版本）—— 换局之后旧那一发
     * 回来会填错地方，那是"选了 Fabric 却看到 Forge 的版本号"。
     *
     * NOTE: 不能只留一份、再来一发就掐上一发：安装页上四条加载器是并列的四行、
     * 一进去就四条一起问，只有一格的话后三发会把前一发掐死 —— 前三条底下永远停在
     * 「正在获取…」。每个加载器各占一格，"换一份列表"这件事由调用方换游戏版本时
     * 重新问一遍来表达。
     *
     * 四条路的数据源（都是实测过的）：
     * * Fabric `/v2/versions/loader/<mc>`（253 条 / 589 KB，接口本来就是新到旧）
     * * Forge maven 清单 5048 条 / 211 KB（**maven 清单**：promotions 那张表只回答"推荐哪个"，
     * 里面根本没有"总共有哪些" —— 1.20.1 那对键是 47.4.10 / 47.4.23，而它底下有 132 个）
     * * NeoForge maven 清单 63 KB
     * * OptiFine 跟着**下载源**走：镜像源问镜像那条 JSON，否则读官方那 200 KB 的下载页 ——
     * 必须跟着下载源，因为"列出来的版本"要能用同一个源装下去（见
     * LoaderMeta::optifineInstallerFileName 和 VersionManager::adoptOptifineEdition）。
     *
     * @param gameVersion 原版版本号。空的话直接回失败（问的是"这个版本上能装什么"）。
     * @param kind 哪一个加载器。
     */
    void requestLoaderVersions(const QString &gameVersion, LoaderKind kind);

    /// 有加载器安装正在进行（含"先装原版"那一段）。
    /// UI 用它来兜住"两批下载之间那一瞬间按钮亮着"的缝。
    bool isLoaderInstalling() const;

    /// 有整合包正在导入（含"先装加载器"和"最后解压覆盖文件"那两段）。
    /// UI 用它和 isLoaderInstalling() 一起兜"能不能点启动" —— 判据只有
    /// MainWindow::onStartButtonClicked() 一处，别在别处再判一遍。
    /// NOTE: 这里不是 updateBottomBar() —— 底栏那格中间那一句看的是
    /// **任务账本**（`MainWindow::updateBottomBarTaskSummary()`：哪一件该被看见由
    /// `Tasks::collapsedSummary()` 定，再加上 `m_syncWaitStatus` 那一层压倒），
    /// 和这几个 "isXxx" 无关。
    bool isModpackImporting() const;

    /**
     * @brief 导入一个 Modrinth 格式的整合包。
     * @param zipPath 用户选的那个整合包文件（**只读**：里面的 overrides 由 ZipUtil 解开）。
     * @param instanceId 落地的实例名，就是 `versions/<id>` 那个目录名。
     * 调用方**必须**先用 `InstanceFiles::validateRenameTarget(id, 空)` 查过重名 ——
     * 这里不查：数据层再查一遍的话，两处判据慢慢漂开的下场是"界面放行、这里拒绝"，
     * 而用户看到的是一句没有落点的报错。
     * @param pack 已经解析好的清单（`ModpackImport::parseModrinthIndex()` 的产物）。
     * 界面先解析、拿它弹确认框，再把同一个结果交进来 —— 不解析两遍。
     * @param installerJavaPath 跑外部安装器（Forge/NeoForge）用哪个 Java，空 = PATH 上的
     * `java`。Fabric 和纯原版的整合包用不上它。
     * @param manifestFingerprint 这一趟导入的包那份清单的指纹
     * （`InstanceFiles::importManifestFingerprint()` 的产物，空 = 不知道）。
     * 写进 `wtfml.importing` 标记里 —— 下次同名再来时 `checkImportTargetName()`
     * 靠它区分"同一个包接着下"和"另一个包撞了名字"（见那边的 ★）。
     *
     * 【一次点击 = 四件事】（和装加载器那条约定的形状一样，中间每一步都不单独报"下载完成"，
     * 整件事只在最后发**一个** `versionDownloadFinished(instanceId, ...)`）：
     * 1. 装一个带加载器的版本（`installLoaderVersion()`，**钉住**整合包指定的那个版本）
     * —— 它同时会把原版补齐，因为加载器版本要基于原版才跑得起来；
     * 2. 落实例自己那份薄 json：`{"id": <实例名>, "inheritsFrom": <加载器版本>}`。
     * 薄到只有两个字段是**刻意**的，Fabric 装出来的那份也这么薄：主类、assetIndex、
     * javaVersion、游戏参数全在加载器/原版那一层，`getVersionJson()` 会走继承链合并。
     * （真机上那个包是 3 层链：整合包 → fabric-loader-0.16.14-1.20.2 → 1.20.2。）
     * 3. 按清单下那一批文件（150 条上下）到 `versions/<id>/mods/…`，每条都带 sha1，
     * 下载管理器会先校验、已有的跳过；
     * 4. 解开压缩包里的 `overrides/` 和 `client-overrides/`，剥掉那一层前缀摊到
     * `versions/<id>/` 底下（后一个盖前一个，这是整合包格式的规矩）。
     *
     * 第 3 步之前失败时，磁盘上会留下一个（多半不完整的）实例目录。**刻意不删**：删目录
     * 是不可逆的，而这个目录里可能有几百 MB 已经下好的文件。失败那句话会把目录在哪说清楚。
     *
     * 失败一律以 `versionDownloadFinished(instanceId, false, 原因)` 收尾。
     */
    void importModpack(const QString &zipPath, const QString &instanceId,
                       const ModpackImport::Pack &pack,
                       const QString &installerJavaPath = QString(),
                       const QString &manifestFingerprint = QString());

    /**
     * @brief 整合包依赖表里那个加载器的键 -> LoaderKind（**纯函数**）。
     * @return 认得返回 true；Quilt 和没见过的键返回 false。
     *
     * 键名本身只有 `ModpackImport` 那一份（`fabricLoaderKey()` 这几个），这里只是把
     * "整合包说的加载器"翻译成"本启动器能装的东西"。翻译写成一张逐条列出的表（不是
     * 遍历比较），将来多一个加载器时编译器会替你盯一眼。
     * NOTE: Quilt 走到 false 是**正常路径的一部分**：界面在更早一步就用
     * `ModpackImport::unsupportedLoaderText()` 拦下来并说清原因了。
     */
    static bool loaderKindForDependencyKey(const QString &key, LoaderKind *out);

    // 是否有一批下载正在进行（含"正在校验并跳过已存在文件"的整批）。
    bool isDownloading() const;

    /// 现在这一批下载**正在给哪个版本**写文件（空 = 没有）。
    ///
    /// 界面靠它把"启动"和"下载"两件事**按版本**分开：装 1.20.1 的这十来分钟里，一个
    /// 字节早就下完的 1.18.2 照样该能启动（两者之间没有任何关系），而 1.20.1 自己不能。
    /// 调用点只有 MainWindow::installingVersionIds() 一处 —— 判据别在别处再写一遍。
    ///
    /// NOTE: 只在**真的有一批在跑时**才认 m_currentVersionDownloading：那个成员收尾之后
    /// **不清空**（它同时是"上一批叫什么名字"，兜底终态那条路要读它，见
    /// m_currentVersionDownloading 的注释）。不门控的话，下过一次 1.20.1 之后它就会
    /// 永远回答"正在装 1.20.1"，那个实例再也点不动 —— 而症状是"下载完了就启动不了"，
    /// 看着像启动那边坏了。
    ///
    /// NOTE: 装加载器 / 装带名字的实例 / 导入整合包这三条路在"拉版本表、拉 profile"的
    /// 前奏里还没开批次，那几段要靠调用方自己记的实例名（见 installingVersionIds()）。
    QString busyVersionId() const;

    // 下载源：官方 / 镜像。转发给内部的 DownloadManager，界面只跟这一层打交道。
    // 改完立刻对**之后发出的**请求生效：库和客户端 jar 的地址在批次开头一次算完，那部分
    // 不会改道；但 5000+ 个 asset 的地址是解析完 asset index 才拼的（而索引回复要等网络），
    // 所以下载中途切源会让资源那一半整体换源。别以为这条路走不通 —— 设置页那个下拉框
    // 在下载期间并没有被禁用。
    void setUseMirror(bool useMirror);

    /// 同时最多下几个文件（设置 → 下载 → 并发数）。**和 setUseMirror 同一个
    /// 形状**：数据层看不见 LauncherSettings（那是界面那一层的），设置要由 MainWindow 推过来。
    ///
    /// 改完立刻生效，但和"改道"不一样：这条线每一拍都重读，所以**正在下的那一批也跟着变**
    /// —— 调小 = 飞着的那几条跑完就不再补；调大 = 下一条回复回来时一次补到位。已经在飞的那
    /// 几条不会被叫回来（叫回来只是白白重下一段）。归一化（越界回默认）在 DownloadManager
    /// 那一侧，见 `DownloadConcurrency.hpp`。
    void setDownloadConcurrency(int count);

    /// 下载速度上限（字节/秒，0 = 不限；设置 → 下载 → 速度上限）。
    /// 和上面那条同一个形状：数据层看不见 LauncherSettings，值由 MainWindow 推过来。
    /// 归一化（0 / 越界回 0）与那几档在 `DownloadSpeedLimit.hpp`。
    void setDownloadSpeedLimit(qint64 bytesPerSecond);

    /// 下载时允许"借现成文件"的那些游戏文件夹。
    /// 和上面几条同一个形状：值由 MainWindow 从 `FolderManager::reuseSourceFolders()` 推过来。
    /// 空 = 关掉复用（默认就是空 —— 单测里那份 DownloadManager 不该去翻开发机的盘）。
    void setReuseSourceFolders(const QStringList &folders) {
        downloadManager->setReuseSourceFolders(folders);
    }

    /**
     * @brief 这一层内部那**唯一一份**下载器（`m_downloads` 那个成员）。
     *
     * 存在的理由只有一个：**"一份批次状态"这条规矩要能被外面接上**。下载器的镜像降级、
     * 按主机的并发守卫、看门狗、失败清单全挂在同一份对象上，所以谁要下东西都得拿**这一份**，
     * 而不是自己 new 一个（同时开两批会让两边的收尾互相算错）。
     * 目前唯一的消费者是服务端安装（`ServerInstallController::setDownloadManager()`）。
     *
     * NOTE: **只借出去下文件**，别拿它去发"问一句就完事"的元数据请求 —— 那些回复会和
     * 版本/资源那几条路争 `m_pendingVersionId` 之类的等待状态（见 `loaderNetwork` 那段
     * 同一个理由）。名字避开了成员名（数据成员和成员函数不能同名），所以叫 `downloads()`。
     */
    DownloadManager *downloads() const { return downloadManager; }

    /// CurseForge 的两项设置。**和 setUseMirror 同一个形状**：数据层看不见
    /// LauncherSettings（那是界面那一层的），设置要由 MainWindow 推过来。
    ///
    /// NOTE: `apiKey` 空 = 走那台不要 key 的社区镜像；非空 = 走官方
    /// `api.curseforge.com` 并带上 `x-api-key`。判据与理由都在
    /// `ModpackImport::curseForgeApiBase()` 里，这里只负责存。
    /// `baseOverride` **只有验证工程会写**（`Launcher/CurseForgeApiBase`，把接口指到
    /// 本机的假服务器上）；产品自己从不写它。
    void setCurseForgeApiKey(const QString &apiKey);
    void setCurseForgeApiBaseOverride(const QString &base);

    /// 把 Modrinth 的接口指到别的地址上（`Launcher/ModrinthApiBase`，**只有验证工程会写**）。
    ///
    /// 和上面那条同一个形状，罩的是导出整合包那条反查路（`POST {base}/version_files`）：
    /// 没有这个口子，那一整趟就只能对着真接口跑，验证工程里没法验。产品自己从不写它。
    void setModrinthApiBaseOverride(const QString &base);

    /// 此刻该用的 Modrinth 接口根（不带结尾斜杠）：覆盖优先，否则官方那一份。
    ///
    /// 需要直连 Modrinth 的地方（导出反查、预设兼容检查）统一问这里。别在构造里再写死
    /// 一份 `https://api.modrinth.com/v2` —— 那样 `Launcher/ModrinthApiBase` 换不掉它，
    /// 验证工程里那一跑就会静默打真网络。
    QString modrinthApiRoot() const;

    /// 版本清单地址覆盖（`Launcher/VersionManifestUrl`，**只有验证工程会写**）。
    /// 空 = 官方地址；非空 = 原样请求那一条，不套镜像。
    void setVersionManifestUrlOverride(const QString &url);

    /**
     * @brief 把 CurseForge 清单里那些号（`projectID` / `fileID`）换成能下的文件。
     * @param pack 解析出来的清单，**就地改**：`pendingRefs` 清空、`files` 填好，
     * 另外把三种"不下"的计数写进 `filesNotDistributable` / `filesMissing` / `filesSkipped`。
     * @return 空串 = 成了；非空 = **不能导入**，那句话直接给用户看（含"可以让用户在设置里
     * 填自己的 CurseForge API key"这一句 —— 镜像是别人的服务，它没了、被限流了
     * 都得给用户一条自己能走的路）。
     *
     * 【为什么是同步的、为什么由界面在弹确认框之前调】两发请求、通常一两秒。放在这里
     * （而不是 importModpack 里）是因为**用户要在点确认之前就看到真实数字**：这个包有多少
     * 文件、其中几个作者不许第三方代下 —— 那些数只有换完才知道。界面上那一下有底栏的
     * 「正在…」顶着（enterSyncWait），和启动路径那三段同步等是同一个手法。
     *
     * NOTE: 不是 CurseForge 的包、或者已经换过一次的（`pendingRefs` 空）**立刻返回**，
     * 所以调用点可以对所有格式无脑调它一次。
     */
    QString resolveCurseForgeFiles(ModpackImport::Pack *pack);

    /// 这一发要用的两条设置（API key / 地址覆盖）的**快照**。
    struct CurseForgeResolveSettings {
        QString apiKey;
        QString apiBaseOverride;
    };
    CurseForgeResolveSettings curseForgeResolveSettings() const {
        return {m_curseForgeApiKey, m_curseForgeApiBaseOverride};
    }
    QString resolveCurseForgeFiles(ModpackImport::Pack *pack,
                                   const CurseForgeResolveSettings &settings);

    // ---------------------------------------------------------------------
    // 导出整合包（清单/枚举/打包都在 ModpackExport 那一层，这里只做"要联网和要读盘"的三件）
    // ---------------------------------------------------------------------

    /**
     * @brief 这个实例装的是什么加载器 —— 导出时写进清单的那一行。
     * @param instanceId 实例目录名。
     * @param keyOut `ModpackImport` 那套词（`fabric-loader` / `forge` / `neoforge`）；
     * 空串 = 纯原版（那是**正常**结果，不是失败）。
     * @param versionOut 加载器版本；纯原版时是空串。
     * @return 空串 = 认出来了（也可能是"确实是原版"）；非空 = 认不出来，那句话给用户看。
     *
     * 三条路，按顺序问：
     * 1. **顺着版本 json 的继承链看那一串 id**：`fabric-loader-0.16.14-1.20.1` /
     * `1.20.1-forge-47.4.10` / `neoforge-21.1.215`（三个 id 的拼法都有唯一出处，
     * 见 LoaderMeta）。这一条排在前面，因为它是**装在盘上的东西自己说的** ——
     * 装加载器出来的目录名就是那个 id，导入整合包出来的薄 json 也指着它。
     *
     * NOTE: 这一条要读的 id 里**混着用户自己起的名字**（PCL 那类合并实例的 id 就是
     * 目录名），而 Forge / OptiFine 那两种拼法的分隔符（`-forge-` / `-OptiFine_`）是
     * **不带头锚点**的子串 ⇒ 认了左边那半必须是**真游戏版本**（`isKnownGameVersion()`，
     * 清单里有它才算数）这一道，见 loaderIdParts() 里那段。
     * 2. **从那份 json 自己身上认**：`libraries` 里的 maven 坐标
     * （`net.fabricmc:fabric-loader` / `net.minecraftforge:forge` / `…:fmlloader` /
     * `net.neoforged.fancymodloader:loader` / `optifine:OptiFine`）＋ `arguments.game`
     * 里的 `--fml.neoForgeVersion`。它罩的是 PCL / HMCL 那类"整份 json 都合进实例
     * 目录"的实例（真机上绝大多数都是）—— 那些实例的 id 是用户起的名字、没有
     * `inheritsFrom`，第 1 条从头到尾问不出东西。判据的取舍写在 loaderEvidenceFromJson()。
     * 3. 拿不到就看 `wtfml.meta` 里的 `loaderName` / `loaderVersion`（我们自己写进去的，
     * 见 updateVersionMeta）。那一段是给界面看的显示名，所以这张反查表是照着
     * loaderDisplayName() 拼出来的，不可能和写进去的那一份漂。它罩的是"链上没有、
     * 但 meta 里记着"的那一类（比如用户手写过一个 meta）。
     *
     * NOTE: **认不出来就停下来**，不许"当成纯原版"接着导 —— 一个装了几百个模组的实例
     * 被导成纯原版包，用户装回来会发现所有模组都在、游戏起不来（加载器那一行不在），
     * 而包看上去是好的。OptiFine 是明确认得出来但两种格式都没法表达的那一个，
     * 它走的就是这条"报一句人话"的路。
     */
    QString instanceLoaderInfo(const QString &instanceId, QString *keyOut, QString *versionOut);

    /**
     * @brief 这个实例**基于哪个原版** —— 导出清单里 `gameVersion` / `minecraft` 那一格。
     * @return 版本号（如 `1.20.1`）。空串 = 什么都没读到（调用点该停下来问一句，别拿一个
     * 空串去拼清单）。
     *
     * 几条路，和 instanceLoaderInfo() 同一个顺序：
     * 1. `wtfml.meta` 里那一格 `realVersion`（装的时候写下的，也是显示名用的那一个）；
     * 2. 顺 `inheritsFrom` 走到链的末端，看链上每一份 json 的 `clientVersion` —— 合并过的
     * 实例（PCL / HMCL 那类）只有这一格说得出口，它们的 id 是用户起的名字；
     * 3. 链末端那个 **id** —— 薄 json 的最后一步就是原版那一份；
     * 4. 实例自己的**目录名**（最不可信的那一个，见下面那条 NOTE）。
     *
     * NOTE: 第 2 条不能用 getVersionJson()：那个是**合并过的**（见它自己的说明），
     * `inheritsFrom` 已经被吃掉，走一步就到底了。所以这里用的是 rawVersionJson()。
     * NOTE: **每一格都要过 isKnownGameVersion()**（判据的出处是那一段说明）。一句话版本：
     * 在线清单在手时以清单为准，清单没取到时看长相。**写进清单的必须是一个真的能装
     * 出来的版本号** —— 假版本号（`原版ATT2地图（无模组）` / `20w14∞`）的后果是"包看上去
     * 是好的、收包的人那边找不到这个版本"，那正是这个功能最怕的失败。
     * 几条路都问不出来时返回空串，调用点该停下来报一句。
     */
    QString instanceGameVersion(const QString &instanceId);

    /**
     * @brief 那个游戏版本**有没有数据包这个东西** —— 纯函数那一半，不碰盘。
     * @param versionId 版本号（也可以是实例的目录名，判据自己会过滤）。
     * @param releaseTime 盘上那份 json 里的 `releaseTime`（`T15:11:46+00:00` 这种）；
     * 空串 = 没读到，走下面第 2 条。
     * @return true = 这个版本有数据包。
     *
     * 数据包（`<存档>/datapacks/` 那个东西）是快照 **17w43a** 引进的。
     * 判据按顺序问两条：
     * 1. **盘上那份 json 自己写着什么时候发布的**就按它比 ``。这一条**离线也准**，
     * 而且快照（`17w43a` / `18w01a`）、远古版（`a1.2.6` / `b1.7.3`）全都分得对 ——
     * 它们的 `releaseTime` 就是真日期。
     * 2. 没读到（薄 json / 手写的 json）就看版本号本身，以数字开头的按 `主.次 >= (1,13)` 比。
     *
     * NOTE: 第 2 条是近似 —— `13w41a` 那类**老快照**的版本号看着像 1.3 以后的，会被判成
     * "有数据包"（实际没有；它比 17w43a 早了四年）。写在这儿不修：那一档的实例在真机
     * 上一个都没有，而为了它去把每个快照的日期都抄一张表进来，不值得。要判错也是
     * "多给一页"那一侧 —— 用户勾掉它就行。
     *
     * NOTE: 两条都问不出来时**给**（返回 true）。宁可多列一页让用户自己勾掉，也不要静默
     * 藏掉一页 —— 用户会觉得"这个启动器把我的数据包页弄没了"。
     */
    static bool gameVersionSupportsDatapacks(const QString &versionId,
                                             const QString &releaseTime);

    /**
     * @brief 这个**实例**用不用得上「数据包」那一页（= 它基于的那个游戏版本有没有数据包）。
     *
     * 就是把它那份**合并过的** json 里的 `releaseTime` 和实例自己报的版本号取出来，
     * 交给 gameVersionSupportsDatapacks()。合并那一层是关键：`releaseTime` 会从父那层
     * 继承上来（见 VersionInheritance::merge 里那一串"子非空用子、否则用父"），所以一个
     * 薄 json 的加载器实例照样问得出来。
     *
     * NOTE: 这里是**读盘**的（json + meta），和 instanceGameVersion() 一样是现取不缓存 ——
     * 它只在打开实例设置时问一次。
     */
    bool instanceSupportsDatapacks(const QString &instanceId);

    /**
     * @brief 这个实例**实际**基于哪个模组加载器 —— 名字和版本。
     * @param nameOut 加载器名（`loaderDisplayName()` 那套词，外加 `Quilt`）；空串 = 没认出来。
     * @param versionOut 加载器版本；认出来但那一格拿不到时是空串。
     * @return nameOut 非空（= 认出来了）。
     *
     * 【为什么不能只看 `wtfml.meta` 里的 `loaderName`】
     * 那一格**只有我们自己的安装器会写**（见 updateVersionMeta）。而 PCL / HMCL 那一类
     * 启动器把整份 json 合进实例目录（真机上绝大多数实例都是这样），它们的 `wtfml.meta`
     * 里只有 `instanceId` —— 于是用户盘上 **70 个实例里 67 个读不出加载器**，看得见的
     * 后果有四处在同一个根上：
     * · 实例设置里「模组」那一页灰着勾不上（用户 报的：`海波篮子` 是
     * Fabric 0.19.5、mods/ 里 48 个 jar 连 litematica 都有，却被判成"没有模组加载器"）；
     * · 实例行前面那个标志画成草方块（见 defaultInstanceMark()）；
     * · 自动归类把 67 个实例全扔进「原版」组，而「可安装mod」永远是空的
     * （见 defaultGroupIdFor()）—— 和 那个 `type` 的 bug 是同一个形状；
     * · 装模组时"加载器对不上"那一道闸恒不放行（见 MainWindow::instanceModsUsable()）。
     *
     * 判据两条，按"离这个实例多近"排，任一条命中就返回：
     * 1. `wtfml.meta` 的 `loaderName`（我们自己装的实例，也含用户手写的 meta）；
     * 2. 版本 json 的 `libraries` / `arguments.game` 里认得出来（`loaderEvidenceFromJson()`——
     * 走 `getVersionJson()`，也就是**合并过的**那一份，所以 `inheritsFrom` 指到
     * `fabric-loader-0.19.5-26.4-snapshot-1` 的薄实例照样罩得住）。
     *
     * NOTE: **`mods/` 里有 jar 不算**。这一格回答的是"它基于哪个加载器"，而往 mods/ 里
     * 丢一个 jar 不构成任何一条加载器证据（一个装了 jar 的纯原版实例仍然起不来那些
     * mod）。"这一页对用户有没有用"是另一个问题，那个补在调用的那一侧 ——
     * 见 MainWindow::instanceModsUsable()。
     *
     * NOTE: 这里**读盘**（meta + json），但两层缓存兜着：meta 那半走
     * `readVersionMeta()`（按"在不在 / 修改时刻 / 字节数"失效），json 那半算过一次就跟着
     * 合并缓存那一格记着（见 instanceTraits()）。所以一次会话里每个实例的 json 只认一遍，
     * 之后每次刷新只多一次 meta 的 stat。
     *
     * NOTE: 第 2 条**先看 OptiFine**（和导出那条路的顺序一致，见 loaderEvidenceFromJson）。
     * 所以一个"既装了 Fabric、又装了 OptiFine"的实例在这里报的是 `OptiFine` ——
     * 对它装 Fabric 模组时那句提示会说得不准。真机上那两个 OptiFine 实例只有 OptiFine，
     * 所以顺序照旧；真要修就得动 loaderEvidenceFromJson 那个顺序，而它管着导出，
     * 别为了这一格动它。
     */
    bool instanceModLoader(const QString &instanceId, QString *nameOut,
                           QString *versionOut = nullptr) const;

    /**
     * @brief 一个实例**显示层**要的两条事实，一次问齐：加载器，和版本分类那个 `type`。
     *
     * 合并成一次的原因很实际：这两个答案都来自同一次 `wtfml.meta` 读。分开问两遍
     * （`instanceModLoader()` + 版本分类）就是每个实例每次刷新多一遍 stat ——
     * 实例树在 200 个实例上要问两轮（行首图标一轮、自动归类一轮），那一层实测能把
     * 一次刷新里三成的阻塞摊在这几条 stat 上。
     *
     * 两条的取法各自和原来一模一样（别在这里"顺便统一"）：
     * - `loaderName`：`wtfml.meta` 里那格**原样**（trimmed，可能是用户手写的 `fabric`），
     *   没有才回落到版本 json 里认出来的**显示名**；都没有 = 空串。
     * - `type`：`wtfml.meta` 里的 `type` 优先，其次是版本 json 自己写的那个；都没有 = 空串
     *   （调用方当**正式版**处理，见 MainWindowViewData.cpp 里 versionChannelOf()）。
     *
     * NOTE: json 那半算过一遍就跟着合并缓存那一格一起记着（见 MergedJsonEntry）——
     * 丢掉 json 缓存的每一处都会把这两个答案一起丢掉，所以不必再维护第二份失效逻辑。
     * NOTE: meta 那半**每次问都现读一次 stat**（和 readVersionMeta() 的失效判据一致）：
     * 用户在外面手改 wtfml.meta 之后，下一次刷新就该看见新的加载器名。
     */
    struct InstanceTraits {
        QString loaderName;     ///< 加载器名（meta 里那格原样；或 json 认出来的显示名）
        QString loaderVersion;  ///< 只在 loaderName 非空时有意义
        QString type;           ///< "release" / "snapshot" / "old_beta" …；空 = 不知道
    };
    InstanceTraits instanceTraits(const QString &instanceId) const;

    /**
     * @brief 反查：本地 sha1 → Modrinth 的版本。
     * @param sha1s 本地算出来的 sha1（大小写不敏感）。
     * @param out 命中的写在这里，**键是发过去的那个 sha1**（见 parseModrinthVersionFiles）。
     * @return 空串 = 这一发成了（**一个都没认出来也是成了**）；非空 = 没成，那句话给用户看。
     */
    QString lookupModrinthVersionFiles(const QStringList &sha1s,
                                       QHash<QString, ModpackExport::ModrinthHit> *out);

    /// 上一条的 CurseForge 版（指纹 → 文件）。同样：一个都没认出来也算成了。
    QString lookupCurseForgeFingerprints(const QList<quint32> &fingerprints,
                                         QHash<quint32, ModpackExport::CurseForgeHit> *out);

    /// 跑完一次导出的前半段：枚举 → 算哈希 → 反查。**不写盘、不建目录**（清单和 zip 在
    /// writePackArchive 里）。两条反查路就是上面那两个方法，验证工程能拿假的换掉它们 ——
    /// 所以这个方法自己也是可测的（见 build/verify 里 V5i/V5j 那种跑法）。
    ModpackExport::ExportPlan buildExportPlan(const ModpackExport::ExportRequest &request,
                                             const ModpackExport::ExportProgress &progress = {},
                                             const ModpackExport::ExportCancel &cancel = {});

    /**
     * @brief 后半段：清单写进暂存目录 → 打成一个 zip → 改成 @p targetZip。
     * @param plan 前半段的产物。
     * @param targetZip 用户挑的目标路径（`.mrpack` / `.zip` 随他）。
     * @return 空串 = 成了；非空 = 那句给用户看的话。**失败时盘上不会留下半成品**。
     *
     * 「先写临时名、成功了再改名」是这里做的：用户中途关掉启动器、或者磁盘满了，盘上留下的
     * 是一个 `xxx.mrpack.part`，而不是一个能双击、打不开的"整合包"。清单本身也先写进那个
     * 暂存目录，再从那儿打进去（打包那一步只认盘上的文件，见 ZipUtil::createZip）。
     */
    QString writePackArchive(const ModpackExport::ExportPlan &plan, const QString &targetZip,
                             const std::function<bool()> &cancelled = {});

    /// Forge / NeoForge 安装器的命令行（**纯函数**，就是为了能钉住 `--mirror` 那一条）。
    /// @param installerPath 安装器 jar 在盘上的路径。
    /// @param targetDir `--installClient` 后面那个目录。
    /// @param mavenRoot `--mirror` 后面那个根（镜像模式 = BMCLAPI，官方模式 = 加载器自己的
    /// 官方 maven 根）。空串 = 这一条不给。
    /// @param supportsMirror 探到安装器认不认 `--mirror`（见 parseInstallerHelp）。老安装器
    /// 不认它，喂过去就是当场报错退出，所以探到不认就**不给**。
    static QStringList forgeInstallerArguments(const QString &installerPath,
                                               const QString &targetDir,
                                               const QString &mavenRoot,
                                               bool supportsMirror = true);

    /// 一个安装器的命令行开关认到什么程度（跑一次 `--help` 探出来的）。
    ///
    /// NOTE: 为什么要探：Forge 1.8.9 那个年代的安装器（SimpleInstaller）**没有**
    /// `--installClient` 这条路 —— 拿真机日志里的 1.8.9-11.15.1.1722 实测，
    /// 它的 `--help` 只有 `--extract` / `-h` / `--installServer` / `--offline`（545 B），
    /// 把 `--installClient` 喂过去，它当场 joptsimple.UnrecognizedOptionException 退出码 1，
    /// 用户看到的是一段 Java 堆栈。所以跑安装器之前先
    /// 问一句它认什么。
    ///
    /// 一个一个探下来的边界（实测，每个都下了 4~5 MB 的安装器跑
    /// `java -jar <安装器> --help`）：**1.7.10 / 1.8.9（11.15.1.1722 和 11.15.1.2318
    /// 两个都试了）/ 1.9.4 / 1.10.2 / 1.11.2 都不认**；1.12.2（14.23.5.2859）和
    /// 26.2（65.1.0）都认。所以这不是"某个版本坏了"，是整整一代安装器都只有图形界面。
    ///
    /// 默认值全是"宁可信其有"：探不出来（进程起不来、超时、输出是空的）就按默认值往下走，
    /// 让真正那一次安装去撞、去报它自己的错 —— 探针的意义是"少一次莫名其妙的报错"，
    /// 不能变成"多一道把本来能装的版本拦下来的关"。
    struct InstallerOptions {
        bool probed = false;               ///< `--help` 真的跑完并拿到了输出
        bool supportsInstallClient = true; ///< 认 `--installClient <目录>`
        bool supportsMirror = true;        ///< 认 `--mirror <根地址>`
    };

    /// 从 `java -jar <安装器> --help` 的输出里认出它认哪些开关（**纯函数**）。
    static InstallerOptions parseInstallerHelp(const QString &helpText);

    // Version JSON access
    //
    // **返回的是走完继承链、合并好的那一份**（见 VersionInheritance）：带模组加载器的版本
    // 自己那份 json 里没有主 jar、assetIndex、javaVersion、游戏参数，也没有主类 —— 那些
    // 全在它基于的原版那份里。要是把原始那份直接交给启动路径，症状是"assetIndex 退回 29、
    // Java 退回 21、主类还是原版的 Main"，也就是**加载器像没装一样**。
    //
    // 合并结果按 id 缓存（版本 JSON 一旦落盘就不再变，合并结果同理）。原始那份另有一个
    // 缓存，下载路径用的仍是原始的 —— 写进磁盘的必须是原样的 json，不能是我们合并出来的
    // 那一份（加载器的安装器、别的启动器都按原样读它）。见 rawVersionJson()。
    QJsonObject getVersionJson(const QString &versionId) const;

    // 把上面说的那**两个按 id 缓存的表**一起丢掉（`versionJsons` / `mergedVersionJsons`）。
    //
    // 这两张表的键**只有版本 id**（= 目录名），而目录名在"换了一整份
    // .minecraft"之后不再是唯一的 —— 两份目录里各有一个 `1.20.1` 是常态。所以
    // **换当前游戏文件夹那一路必须喊这一句**（`MainWindow` 里
    // `FolderManager::currentFolderChanged` 那个 lambda），和 m_javaListScanned 那几件
    // "跟着上一份走的事"一起重置。不喊的症状：切完文件夹再启动同名的那个实例，
    // 全程用的是上一份目录里那份 json（classpath / natives / javaVersion / assetIndex
    // 全按它算），而游戏自己去读新目录的 json。详见 .cpp 里那段。
    void clearVersionJsonCache();

    // 这个版本的继承链有没有问题：没问题返回空串，有问题返回给用户看的一句话（父版本
    // 没装 / 链成环 / 链太长）。
    //
    // 为什么单独一个函数：`getVersionJson()` 遇到父版本缺失时只能给出"合不完整的那一份"，
    // 而它的调用方（启动路径）分不出"这份 json 本来就长这样"和"父版本没装" —— 后者
    // 必须弹窗说清楚（要用户去装原版），套用「请重新下载该版本」那句话会把人引到死路上。
    //
    // NOTE: 只读，不写盘。它在启动路径上被调用。
    QString inheritanceProblem(const QString &versionId) const;

    // 读某个版本参与"显示名"的那些部分（自定义名、模组加载器名/版本）。
    // 数据源是 <id>/wtfml.meta 这个可选文件，读不到就全是空串、显示名退化成纯版本号。
    // `id` 永远用参数填，不从文件里取。
    //
    // NOTE: 文件名是 wtfml.meta 而不是 wtfml.json，这是必须的——<id>.json 是版本 JSON
    // 的既定名字，万一真去下载一个 id 就叫 "wtfml" 的版本，versions/wtfml/wtfml.json
    // 会同时是版本 JSON 和元数据文件。后果不是崩溃，是更糟的静默错：getVersionJson()
    // 读到一份没有 "id" 字段的对象**仍然返回它**，调用方只判 isEmpty() 就会一路绿灯
    // 走到兜底的 assetIndex/javaMajorVersion，也就是"静默选错 Java"那个老 bug。
    // 换个扩展名，这个撞名在结构上就不可能发生。
    VersionDisplayParts displayParts(const QString &versionId) const;

    // 读某个版本自定义的 JVM 参数（wtfml.meta 里可选的 "jvmArgs" 字符串数组）。
    // 读不到就是空列表，调用方不必区分"文件没有"和"字段没有"。
    //
    // 用途：PCL 允许用户自定义 Java 启动参数，这里预留"按版本"的那半边 ——
    // "全局"那半边是 GlobalJavaSettings::extraJvmArgs()，早就生效了。
    // NOTE: 参数本身可以做任何事（包括把 JVM 起不来），不做白名单校验 —— 这是用户
    // 自己写的文件、自己指定的版本，和直接改命令行是一回事。
    QStringList versionJvmArgs(const QString &versionId) const;

    // meta 里"追加 JVM 参数"那个字段名。
    //
    // 公开出来只为一件事：写侧（每实例设置对话框点确定时）在 MainWindow 里，它得
    // 往 updateVersionMeta() 传一个 key。读侧（versionJvmArgs）和写侧各写一遍
    // "jvmArgs" 这个字面量，迟早会有一边写成 "jvmargs" —— 而那个错的形态是
    // **静默不生效**：字符串不匹配不报错，参数就是不进命令行。
    static QString jvmArgsField();

    // 这个实例的「Java 版本与要求不符时不再提醒」开关（wtfml.meta 里可选的
    // "skipJavaVersionWarning"）。字段缺失、读到坏文件、类型不对，全都是 false = **要提醒**。
    //
    // NOTE: 默认为什么是"提醒"而不是"免打扰"：这个键只有用户点过「以后不再提醒这个实例」
    // 才会被写下，没写过就是"没表过态"；而"没表态"的语义是一律报错拦下。
    // 反过来默认（缺省即免打扰）会让老实例一升级就静默放行一个版本不对的 Java，
    // 而静默选错 Java 正是这一层最怕的错。
    // NOTE: 类型不对（写成字符串、写成数字）也按"没表态"处理，但要 qWarning 一声 ——
    // 这是用户手写的文件，写错了必须让他看得出来，否则表现成"明明写了却不生效"。
    bool skipJavaVersionWarning(const QString &versionId) const;

    // meta 里那个开关的字段名。公开的理由和 jvmArgsField() 一样：写侧在 MainWindow 里，
    // 它得往 updateVersionMeta() 传一个 key，两边各写一遍字面量的错法是**静默不生效**。
    static QString skipJavaVersionWarningField();

    // 这个实例自己挑的图标（wtfml.meta 里可选的 "icon"）。空串 = **没挑过**，用默认那张
    // （按版本分类 / 加载器算，见 MainWindow::instanceRowMark()）。
    //
    // 取值是带前缀的串，三种形态：
    // - 空串 → 默认
    // - "builtin:<候选键>" → 内置候选里那一张（键见 VersionIcons::iconCandidateKeys()）
    // - "local" → 用实例目录下那份 icon.png（AppPaths::versionIconPath()）
    //
    // NOTE: 为什么"本地图片"存的是一个**固定名字**而不是用户挑的那张图的路径：实例改名会
    // 把整个目录搬走，绝对路径当场失效 —— 而改名在这个项目里是常规操作（设置对话框
    // 第一行就是它）。挑中的图在点确定那一刻就被拷进实例目录了，所以"用本地那张"
    // 这句话永远有对应的东西。
    //
    // NOTE: 类型不对（写成数字/对象）按"没挑过"处理，但要 qWarning 一声 —— 理由同
    // skipJavaVersionWarning()：用户手写的文件写错了必须让他看得出来。
    QString versionIcon(const QString &versionId) const;

    // meta 里那个图标字段名。公开的理由和 jvmArgsField() 一样：写侧在 MainWindow 里。
    static QString iconField();

    // meta 里"这个实例的左栏显示哪几页"那个字段名。公开的理由和
    // jvmArgsField() 一样：写侧在 MainWindow 里。
    //
    // 形状：`"settingsPages": { "mod": false, "shader": false }` —— **只装用户显式勾掉的
    // 那几条**，一条一个 `false`。这不是一份对称的"选择"表，是一个**藏起来的页**的集合：
    // 勾选框只在那一页可用时能勾，而不可用就没得选 ⇒ 存下来的**只可能是 false**。
    // 所以读侧就是一句话 `显示 = 那一页可用 && 表里没写 false`（见
    // InstanceSettingsDialog::shownEntryKeys()），而"再勾上"就是把键去掉。
    //
    // NOTE: 空对象写出去是 QJsonValue::Null ⇒ 删键（和 icon 那一格同一个约定，见
    // updateVersionMeta 的 @param fields）。所以"七条全显示"的实例盘上**没有这个键**。
    // NOTE: 可用性（有没有模组加载器 / 有没有 litematica / 游戏版本够不够新）**不写盘**，
    // 每次现算 ⇒ "勾掉 → 拆掉 iris → 装回 iris"之后那一格会自己回来（默认显示）。
    static QString settingsPagesField();

    // 读这个实例的整份 wtfml.meta（文件没有、读不到、解析不了都是空对象）。
    //
    // 这是"每实例设置"的读侧入口：MainWindow 拿它喂给 javaOverrideFromJson()。
    // 单独开一个公开读口而不是让调用方去凑 displayParts + versionJvmArgs 两个
    // 半截的读侧 —— 覆盖那五项里有四项（javaPath / xmx / xms / autoMemory）现在
    // **没有**任何专门的读函数，逐个加会变成五个功能重复的小函数。
    //
    // NOTE: 只读，不写盘、不生成 instanceId。它在启动路径上被调用。
    QJsonObject versionMeta(const QString &versionId) const;

    // ---- wtfml.meta 的写侧 ----
    //
    // 重命名 / 分组 / 每实例设置三件事都要写它。写侧放在同一个类里，是因为"什么算坏文件、
    // 坏了要不要吭声、类型不对怎么办"这套判断已经在 readVersionMeta 里成文了，
    // 另立一处必然要抄第二遍、然后慢慢抄歪。

    // 把一批字段**合并**写进 <id>/wtfml.meta，其余字段原样保留。
    //
    // @param fields 要写的字段；值为 QJsonValue::Null 表示**删掉**该字段。
    // NULL 这个约定是给"改回跟随全局"用的 —— 那种情况要的是"这个键不存在"，
    // 而不是"这个键等于全局的值"（后者会让这个实例在用户改全局之后不跟着走）。
    // @param errorOut 失败原因（给用户看的一句话）。数据层不弹窗、也不只写 qWarning
    // （用户看不见），一律交给调用方去弹。
    // @return 写成功返回 true。
    //
    // NOTE: 合并而不是整体替换。这个文件是用户可以手写的，里面可能已经有 jvmArgs /
    // nameFormat / realVersion —— 整体覆盖写会把它们抹掉，而那正是读侧花了力气
    // 支持的东西。同理，文件**存在但解析不了**时**拒绝写**：往一份读不懂的内容上
    // 合并等于把用户手写的部分丢掉，比"这次没写成"糟得多。
    // NOTE: 这些规矩的实现在 manage/InstanceMetaFile（按路径工作，暂存目录 / 别的游戏
    // 文件夹也能用）；这里只负责"当前数据目录下的哪一份"和目录锁。
    // NOTE: 非 const —— 它真的往磁盘上写东西。"看起来是只读"的假 const 会诱使人在
    // 扫描路径上调用它，而扫描一次会话要跑无数遍、绝不能带副作用。
    bool updateVersionMeta(const QString &versionId, const QJsonObject &fields,
                           QString *errorOut = nullptr);

    // 这个实例的稳定 id（wtfml.meta 里的 "instanceId"）；没有或读不到就是空串。
    // **只读，不生成、不写盘** —— 它在扫描路径上被调用。
    QString instanceId(const QString &versionId) const;

    // 确保这个实例有稳定 id：没有就生成一个写进 meta，返回它；写盘失败返回空串。
    //
    // 按需生成，例如修改实例、记录游玩时长。扫描只读元数据。
    QString ensureInstanceId(const QString &versionId);

    // 把这个实例改名：**真改磁盘上的目录名**，并把新名字写进 meta 的显示名。
    //
    // @param oldId 现在的目录名。
    // @param newName 新名字（会 trim）。校验规则在 InstanceFiles::validateRenameTarget()。
    // @param errorOut 失败原因（给用户看的一句话，可能有多行）。
    // @return 全部成功返回 true。
    //
    // 执行顺序是刻意的，每一步都能反悔：
    // 1. 全部前置检查（名字非法 / 重名 / 目录不在）—— 磁盘一个字节都不动；
    // 2. 写 meta（显示名 + 稳定 id）—— 排最前是因为它可能失败，而它失败时我们
    // **还什么都没破坏**；
    // 3. `<老名>.jar` → `<新名>.jar`，4. `<老名>.json` → `<新名>.json`，
    // 5. 目录 `versions/<老名>` → `versions/<新名>`。
    // 3~5 失败时按 5→4→3 逆序回滚，回滚自身也失败时 errorOut 里会写清**当前磁盘是
    // 什么状态**，绝不静默留下一半改动。
    //
    // 先内后外的另一个好处：崩在第 3、4 步之后留下的状态（jar 名和新目录名还没对上）
    // 恰好被 InstanceFiles 判据的第 ② 条认得出，用户在列表里看得见、还能再改一次。
    //
    // NOTE: "这个实例正在跑"**不在这里判** —— VersionManager 看不见游戏进程，那是
    // LauncherManager 的状态。运行中的实例在 Windows 上 jar 被 JVM 锁着，等到
    // rename 失败再报错，用户看到的是一句"拒绝访问"，而该说的是"先关掉游戏"。
    // 所以那道闸口在 MainWindow 的槽里，且必须在调这一句**之前**判完。
    // NOTE: 非 const —— 它真的改磁盘。
    bool renameInstance(const QString &oldId, const QString &newName, QString *errorOut = nullptr);

    /// 把一个实例**整个删掉**：`versions/<id>` 那一棵（jar、json、wtfml.meta、以及里面
    /// 的一切）连同目录一起从盘上抹掉。要求：「删除前需要弹窗提示」（那句
    /// 提示在 MainWindow::confirmDeleteInstance()，这一层**不弹窗**，这是本类的规矩）。
    ///
    /// 只做这一件事，别的都不管：
    /// - **不碰分组归属**（wtfml.folders 里可能还留着一条指向它的记录，见调用点那段）；
    /// - **不碰设置**（Launcher/LastLaunchedVersion 指向它的话，下次启动时那一格找不到人，
    /// 表现是"启动器没选中任何实例"—— 那是启动路径本来就有的容错，不用在这儿清）；
    /// - **不判"它在不在跑"**：和 renameInstance() 同一条（VersionManager 看不见游戏
    /// 进程，那道闸口在 MainWindow）。
    ///
    /// NOTE: 删的是**目录树**，所以走的是 FolderManager 那边那支
    /// `removeTreeWithoutFollowingLinks()`（**不追链接**）—— `QDir::removeRecursively()`
    /// 会跟着 junction/符号链接进到那一头，把游戏文件夹**外面**的东西一起删掉
    /// （实测过，理由写在那个函数的注释里）。用户在实例目录里挂一个 junction 是
    /// 很少见、但后果最严重的一种。
    /// NOTE: 非 const —— 它真的删盘。
    /// @param versionId 目录名（会 trim 并校验：空 / `..` / 带路径分隔符的名一律拒掉 ——
    /// 这个入参直接拼进路径，校验不是形式）。
    /// @param errorOut 失败原因（给用户看的一句话，可能带"删到一半"那种状态说明）。
    /// @return 那个目录已经不在盘上了返回 true。
    bool deleteInstance(const QString &versionId, QString *errorOut = nullptr);

    /**
     * @brief 把一份实例**原样复制**到**另一个游戏文件夹**里去（要求：
     * 「实例设置的概览里面加一条复制实例，点击以后让用户选择复制到的文件夹，然后
     * 给定复制后的实例名称」）。
     *
     * 「另一个游戏文件夹」是这里的定义，不是实现细节：`<目标文件夹>/versions/<新名字>/`
     * 才是落点。理由是"副本要能用" —— 拷到随便一个目录里，启动器的实例列表根本看不见它，
     * 用户还得自己把那个目录加成游戏文件夹；而拷进一个游戏文件夹的 `versions/` 里，
     * 它下一趟刷新就是一列正常的实例（分组归属除外，那要用户在界面里再选一次）。
     *
     * 复制做到"这一份能启动"为止，一共三件事（顺序和 renameInstance() 那套同源）：
     * 1. 整棵目录树（含 wtfml.meta、icon.png、mods/、saves/ ……）**逐文件**拷过去；
     * 2. 里面那个 `<老名>.jar` / `<老名>.json` 改成 `<新名>.jar` / `<新名>.json`
     * —— 版本 json 和主 jar 的名字必须和目录名一致（InstanceFiles 的判据第 ② 条），
     * 光拷不改的话那一份在列表里认不出来；
     * 3. 写目标那一份的 wtfml.meta：**换一个全新的稳定 id**（`instanceId` 是"从生到死
     * 跟着这个实例"的，两份实例共用一个 id 会让分组、图标这些认错人），显示名改成
     * 新目录名。
     *
     * NOTE: 和 renameInstance() **不是**一回事，别互相替换：那个是"这一份换个名字"，
     * 源和目标都在同一个 versions/ 下、动完源就不在了；这个是"多出一份"，源一个
     * 字节都不动。
     * NOTE: "这个实例正在跑"**不在这里判**（VersionManager 看不见游戏进程，那道闸口在
     * MainWindow，见 renameInstance() 的同一条）。跑着的时候拷出来的是**那一刻的
     * 一份快照** —— 本来也没什么可抱怨的，但用户以为自己在做备份时得知道这一点，
     * 所以那句话由调用点在动手之前说。
     * NOTE: **目标不能落在源实例目录里面**。挑目标的是个普通目录选择框，
     * 用户完全可以点进 `versions/<这一份自己>` 再按确定 —— 那时"先 mkpath 目标、再列
     * 源目录"这两步会把自己刚造出来的那一层也列进来，**自己抄自己、每层再深一层**，
     * 一路写到盘满。判据是 InstanceFiles::pathIsSameOrInside()（比规范化之后的真路径，
     * 解掉 junction / 8.3 短名那些别名）；界面那一层有一条同款的、话术更长的。
     * NOTE: 失败时**只删自己刚写出来的暂存**（不是删源，也不是删目标位置已有的东西）。
     * 副本先落在 `versions/.wtfml-staging/<uuid>`，成功后改名发布；慢盘上拷到一半时
     * 用户或另一份启动器在目标名字下新建的内容不会被这一趟清掉。暂存删不掉也照实报，
     * 不能把“清不掉”说成成功。
     *
     * @param sourceId 源实例的目录名（当前数据目录 `versions/` 下的那一个）。
     * @param destGameFolder 目标**游戏文件夹**（那份 .minecraft 的根）。空串直接拒掉 ——
     * 拼出 `<空>/versions/...` 会落到当前盘根上。
     * @param newName 副本的目录名（= 显示名），走 InstanceFiles::validateDirName() 那套
     * 规矩（长度、分隔符、尾随点、Windows 保留设备名）。
     * @param errorOut 失败原因（给用户看的一句话）。
     * @return 目标那一份已经能用了才 true。
     * NOTE: 非 const —— 它真的写盘。
     */
    bool copyInstance(const QString &sourceId, const QString &destGameFolder, const QString &newName,
                      QString *errorOut = nullptr);

    /**
     * @brief 同 `copyInstance()`，但**源目录由调用方在主线程算好传进来**。
     *
     * 为什么要有这一份：`copyInstance()` 内部用 `AppPaths::versionDir(sourceId)` 现算源目录，
     * 而它现在会被**后台线程**调用（「复制实例」/「升级副本」都挪进了 `QtConcurrent::run`）。
     * `AppPaths::minecraftDir()` 读的是进程级的可变 `QString`，用户完全可以在这趟跑着的
     * 时候切换游戏文件夹 —— 两个线程一读一写就是数据竞争，而且真的会拷错目录
     * （拷的是**新**游戏文件夹里同名的那一份，或者直接报"找不到实例目录"）。
     *
     * 后台调用方一律走这一份：把 `AppPaths::versionDir(id)` 在**主线程**取好，捕进 worker。
     *
     * @param sourceDir 源实例目录的绝对路径（调用方在主线程算好）。
     * @param sourceId 源实例的目录名（用来把副本里的 `<老名>.jar/.json` 改成新名）。
     * @param destGameFolder / @param newName / @param errorOut 同 `copyInstance()`。
     */
    bool copyInstanceFrom(const QString &sourceDir, const QString &sourceId,
                          const QString &destGameFolder, const QString &newName,
                          QString *errorOut = nullptr);

    // ---------- 下载页那几类资源：把一个文件下到实例里（模组 / 数据包 / 资源包 / 光影）----------
    //
    // 一次点击 = 一个（或几个）文件，下完就结束，没有"接着装点什么"的后续 —— 所以它
    // 不在上面那几套安装状态机里，也不需要调用方分两步。
    //
    // NOTE: 这一层**不认识 Modrinth**（不知道 project_id、不知道 dependencies）。这个结构里
    // 全是"下什么"的物理事实：地址、落盘的文件名、哈希、大小。上游（MainWindow）负责
    // 把 ResourceCatalog 读出来的那些翻译成它 —— 这样将来接 CurseForge 时，这一层
    // 一个字节都不用改。
    struct ResourceFile {
        QString url;
        /// 落盘的文件名。**不能带路径分隔符**（带 ".." 的会被拒掉，见实现）。
        QString filename;
        /// 可以空（空了就只比大小）。Modrinth 每条文件都给了 sha1，正常不会是空的。
        QString sha1;
        qint64 size = -1;
        /// 为什么需要"按条带目录"：更新中心那一批是**用户在同一页上勾出来的**，勾中的那些
        /// 可以跨类（`mods/` 里两个 + `resourcepacks/` 里一个），而"换成哪一版"这件事是
        /// **就地换**的 —— 那一条原来在哪个目录，新的那一份就回哪个目录。一个批次只有一个
        /// 目录的话，要么跨类的那几条下不了，要么就得按目录拆成好几批（而
        /// `downloadResourceFiles()` 一次只收一批，第二发会被 `isBusy()` 直接拒掉）。
        QString dir;
    };

    /**
     * @brief 把一批文件下到某个目录里。**异步**，结果走 resourceDownloadFinished。
     *
     * @param destinationDir 落盘的目录（绝对的）。实例那条路是
     * `<版本目录>/mods`、`.../resourcepacks`、`.../shaderpacks`；整合包那条路是
     * 缓存目录（下回来的是 .mrpack，接着走导入）。**由调用方拼** —— 这一层不认识
     * `AppPaths` 里"实例目录怎么算"这套东西（它只管往一个目录里写文件）。
     * @param busyInstanceId 这一批在写哪个实例（`versions/<id>` 那个目录名）。**空 = 谁都不锁**
     * （往缓存里下 .mrpack 就是这种：盘上没有任何实例在被写）。
     * 界面的"能不能启动"按它判（busyVersionId()）——写 mods/ 的时候用户在启动游戏，
     * JVM 可能正好在读那半截文件。
     * @param label 收尾那句话里"下的是什么"（「Sodium 0.6.13」这种），原样带回信号。
     * @param files 要下的文件。
     *
     * 一批在跑、或者那三套安装状态机在跑的时候**拒绝开始新的**（和 downloadVersion() /
     * importModpack() 同一道闸口）：DownloadManager 只有一份批次状态，两批叠在一起会让先那一批
     * 的完成信号提前发出去（它的相位还没走完）。拒绝时也会发终态信号，界面不会卡在"下载中"。
     *
     * @return **这一批到底开起来没有**。
     *
     * NOTE: 起返回值是有用的，别当它是 void。理由：被拒绝时**照样会发一次
     * `resourceDownloadFinished(label, false, …)`**，而那个 label 和"这一单真正跑完"
     * 那条路上的 label 是**同一个字符串** —— 调用方没法从信号本身分辨"这是我这一单
     * 失败了"还是"我这一单根本没开始"。它自己的 `m_pendingModpackImports` 就是靠这个
     * 分辨的：登记待导入记录必须在"真的开起来了"之后（双击那一发的第二下会带着同一个
     * label 撞上这道闸，然后拿着同一个 label 发一次假失败，把记录吃掉）。
     */
    bool downloadResourceFiles(const QString &destinationDir, const QString &busyInstanceId,
                               const QString &label, const QList<ResourceFile> &files);

signals:
    void versionsFetched(const QStringList &versions);
    /// `requestInstalledVersions()` 的回复。只有**最新一次**请求会发：旧的那一发的结果
    /// 直接丢掉（见那个函数的注释）。
    void installedVersionsScanned(const QStringList &versions, const QStringList &warnings);
    void versionDetailsFetched(const QString &versionId, const QJsonObject &versionJson);
    // 进度只走这个信号，UI 把它显示成进度条，不再往日志里写。
    void versionDownloadStats(int done, int total, int failed, const QString &currentFile);
    // 语义是"整批结束"，一次下载只会发一次。
    // errorMessage 在成功时是给用户看的汇总，失败时是失败文件清单。
    void versionDownloadFinished(const QString &versionId, bool success,
                                 const QString &errorMessage = QString());
    // 安装加载器期间给用户看的**过程**话术（"正在获取 Fabric 的版本列表…"、
    // "原版 1.19 还没装，先把它装好…"）。UI 把它写进日志，不弹窗。
    // 单独一个信号而不是复用 stats：stats 那四个数是给进度条用的，往里塞话术会让
    // "进度条显示的是文件数" 这件事变得不可信。
    void loaderInstallMessage(const QString &text);

    /**
     * @brief requestLoaderVersions() 的回复。**每次请求都会回一次**（成功、失败都回）。
     *
     * @param versions 用户能挑的版本号，按各自加载器的天然次序（两份 maven 清单按版本号新到旧、
     * Fabric 按接口给的次序）。每一条都带着 `stable`（是不是正式版）—— 界面靠它标出
     * 「测试版」、也靠它决定「下载稳定版（release）」那一行能不能点。空 = 一个都没有。
     * @param errorMessage 失败时说明原因（界面按它弹窗）；成功时是空串。
     *
     * NOTE: 列表里每一条的 `stable` **从数据层一路带到界面上**（四个加载器的判据见
     * LoaderVersionEntry::stable 的注释）：界面靠它标「测试版」、也靠它决定
     * 「下载稳定版（release）」那一行能不能点。
     */
    void loaderVersionsFetched(const QString &gameVersion, LoaderKind kind, bool success,
                               const LoaderMeta::LoaderVersionList &versions,
                               const QString &errorMessage);

    /// 整合包导入中间那几句（装加载器、下清单里那批文件、解压覆盖文件）。
    ///
    /// NOTE: 和 loaderInstallMessage 分开，理由是**收尾那句话该说谁**：导入整件事的终态
    /// 说的是"整合包"，而中间这几句也要一起进底栏那格状态文字（用户等的是整合包，
    /// 不是加载器）。两条路各自说话，界面按各自的信号名接。
    void modpackImportMessage(const QString &text);

    /// 整合包导入进入下一个关键阶段时发射，界面层据此推进任务中心的步骤列表。
    void modpackImportStageChanged(ModpackStage stage, const QString &note);

    /// 装"一个带名字的实例"中间那几句（正在装原版 / 正在装加载器 / 写实例文件）。
    ///
    /// NOTE: 和上面两条**分开**的理由同上：收尾那句话该说谁。这一条路最后那句说的是
    /// 「实例 <用户起的名字> 装好了」，而中间那几句说的也是这个名字 —— 不能借
    /// loaderInstallMessage 来说，那几句在底栏会读成"用户在装加载器"。
    void instanceInstallMessage(const QString &text);

    /// 下载页那几类资源里挑中的一个版本下完了（见 downloadResourceFiles）。**整批只发一次**。
    ///
    /// @param label 就是传进去的那个"下的是什么"（「Sodium 0.6.13」），界面照它拼标题。
    /// @param errorMessage 成功时是那批文件的汇总（"1 个文件已全部就绪。"），失败时是失败清单。
    ///
    /// NOTE: 单独一个信号、**不复用 versionDownloadFinished**：那个信号的第一个参数是
    /// "哪个版本"，界面按它去清"这个实例正在安装"的标记（见界面那一层收尾，`DownloadCompletionController::onFinished`）——
    /// 而一个模组下完不该去动任何实例的安装状态。语义不同的东西共用一个信号，
    /// 错法是静默的：界面上看是"下完一个模组，某个正在装的实例忽然被解禁了"。
    void resourceDownloadFinished(const QString &label, bool success,
                                  const QString &errorMessage = QString());

    /**
     * @brief 「往实例里下一个资源文件」那一批里**单个文件**的结果（更新任务
     * 要显示每个资源的结果，不只显示总数）。
     *
     * @param label 就是 `downloadResourceFiles()` 收的那一个（和 `resourceDownloadFinished`
     * 带的**是同一个字符串** —— 调用方靠它认"这是我那一批"）。
     * @param fileName 落盘那个文件的**名字**（`DownloadManager::downloadFinished()` 的口径，
     * 就是 `ResourceFile::filename`）。**不是**绝对路径：那一位在这个信号里从来没有过。
     * @param success 这一条成没成。
     * @param errorMessage 失败原因（成功时空串）。
     *
     * 只在这一批是**资源那一批**的时候发（`m_resourceDownload.running`）。加载器安装 /
     * 整合包导入 / 带名字的实例那三条路上，界面拿这些逐条结果没有用处（那几件事的收尾是
     * "装好了没有"，不是"哪几个文件好了"），而每一条都转出去等于把 5000 个 asset 逐个
     * 报一遍 —— 几千行一模一样的话（见构造函数里那段注释）。
     *
     * NOTE: 和 `versionDownloadStats` 那一发**不是一回事**：那一发是"进度"（会丢、有节流、
     * 给进度条用），这一发是"结果"（一条不漏、给账本里那一步用）。
     */
    void resourceDownloadFileFinished(const QString &label, const QString &fileName, bool success,
                                      const QString &errorMessage = QString());

private slots:
    void onVersionManifestFetched(const QList<MinecraftVersion> &versions);
    void onVersionJsonFetched(const QJsonObject &versionJson);
    void onAllDownloadsFinished(bool success);

private:
    // 读 <id>/wtfml.meta 并解析成对象，实现只有一份（InstanceMetaFile::read）。
    // 文件不存在、打不开、格式错都返回空对象（后两种会告警）。
    // metaPathOut 非空时回填路径，供调用方在告警里指出是哪个文件。
    // unreadableOut 非空时回填"文件在、但改不动"：读不懂，**或者**格式版本比这个
    // 启动器新。返回空对象有两种含义（没有这个文件 / 有这个文件但读不懂），写侧必须
    // 分得清：前者可以新建，后者得拒绝写，否则就是把用户手写的内容覆盖掉。
    QJsonObject readVersionMeta(const QString &versionId, QString *metaPathOut = nullptr,
                                bool *unreadableOut = nullptr) const;

    // 磁盘/网络上的那一份，**原样**，不走继承链。getVersionJson() 和继承链的走法都从
    // 这里取数；下载路径（写 `<id>.json` 落盘）也只用它。
    QJsonObject rawVersionJson(const QString &versionId) const;

    // ---- 加载器安装的内部状态机 ----

    // 一次加载器安装走到哪一步了。Idle 之外的任何值都意味着"这件事还没结束" ——
    // 两个地方靠它做判断：onVersionJsonFetched（拦下"原版"取 json 的回复，别让
    // UI 再发一次下载）和 onAllDownloadsFinished（那一批结束时是收尾还是接着下一段）。
    struct LoaderInstall {
        enum class Phase {
            Idle,
            FetchingList,           ///< Fabric：正在拉加载器版本列表
            FetchingProfile,        ///< Fabric：正在拉它的 profile json
            FetchingInstallerList,  ///< Forge/NeoForge/OptiFine：正在拉版本表
            FetchingInstallerToken, ///< OptiFine（官方源）：正在从广告页里抠下载地址
            WaitingInstallerJar,    ///< 三个都要跑安装器的：安装器 jar 正在下
            ProbingInstallerOptions, ///< Forge/NeoForge：先跑一次 `--help`，看它认哪些开关
            RunningInstaller,       ///< 三个都要跑安装器的：那个 Java 进程正在跑
            WaitingParentJson,      ///< 正在取原版的 version.json（走 fetchVersionDetails）
            WaitingParentBefore,    ///< OptiFine：原版那一批正在下（它的安装器要拿原版当输入）
            WritingLegacyProfile,   ///< 老 Forge：正照它的安装说明写版本 json + 摆 universal jar
            WaitingParentFiles,     ///< 原版那一批正在下（客户端 jar / 库 / 资源）
            WaitingLoaderFiles,     ///< Fabric：加载器自己那几个库正在下
        };

        Phase phase = Phase::Idle;
        LoaderKind kind = LoaderKind::Fabric;
        QString gameVersion;
        QString loaderName;     ///< wtfml.meta 的 loaderName
        QString loaderVersion;  ///< wtfml.meta 的 loaderVersion
        QString loaderId;       ///< 版本目录名，也是最终 finished 信号里的 id
        /// 整合包指定的加载器版本（空 = 默认行为：拉版本表挑一个）。
        /// NOTE: 它一非空就连"拉版本表"那一步都不走 —— 挑出来的和钉死的必须泾渭分明，
        /// 否则镜像那份旧表挑不出 1.16 之后的版本时，会把整合包要的版本悄悄换掉。
        QString pinnedVersion;
        /// 没钉版本时按哪一档挑（安装页那两行「下载最新版（alpha/beta）」/「下载稳定版
        /// （release）」）。钉了版本时它不起作用 —— 那一格由 pinnedVersion 说了算。
        ///
        /// NOTE: 它是**界面那一格**（`@latest` / `@stable` / 空串）在数据层这边解码出来的
        /// （LoaderMeta::loaderRequestFromChoice），空串解出来就是 Stable —— 也就是
        /// "让安装器按默认规矩挑"。
        LoaderMeta::LoaderChannel channel = LoaderMeta::LoaderChannel::Stable;
        /// 跑外部安装器用哪个 Java（空 = PATH 上的 `java`）。Fabric 用不上。
        QString javaPath;
        /// 安装器 jar 要下的地址，以及它落在盘上的哪个文件。两条路都在拉完版本表时填好。
        QString installerUrl;
        QString installerPath;
        /// 它在 cache/installers/ 里叫什么名字。
        ///
        /// 为什么不从 installerUrl 的最后一段取（Forge/NeoForge 就是那么干的）：OptiFine
        /// 走镜像源时那个地址的最后两段是 `type`/`patch`（`/optifine/1.18.2/HD_U/H9`），
        /// 不是文件名 —— 照着取会落成一个叫 `H9` 的文件。
        QString installerFileName;

        /// 版本表那条请求的状态（只有 Forge / NeoForge 用得上）。
        QString listOfficialUrl;
        bool listRequestWasMirror = false;
        bool listRetriedOfficial = false;
        /// 镜像那一发已经用过了没有（开局就在镜像上、或者已经退过去一次）。
        /// 官方那条**连不上**时 NeoForge 会往镜像退一次；用过就不再退，免得两边来回弹。
        bool listMirrorTried = false;

        /// 安装器 jar 的常规坐标 404 之后，换 `<游戏>-<加载器>-<游戏>` 那种坐标试过一次没有。
        /// 只试一次（见 VersionManager::retryInstallerDownloadAlternate）。
        bool retriedAltCoordinate = false;

        /// 为什么是这个布尔、不是 `phase != Idle` 之类的推断：这一条决定"下一批原版文件
        /// 下完之后要不要再下一遍**加载器自己的库**"。Forge/NeoForge 正常那条路是安装器
        /// 已经把库摆好了（收尾），这一条是**我们**摆（还得下十几个库）—— 两者在同一批
        /// 原版文件下完的那个接续点上分岔，只看 kind 分不开（都是 Forge）。
        bool legacySelfInstall = false;

        bool active() const { return phase != Phase::Idle; }
        // 结算用的 id：还没算出 loaderId 时退化成原版号 —— 失败信号总得带一个 id。
        QString reportId() const { return loaderId.isEmpty() ? gameVersion : loaderId; }

        // 收尾（成功、失败都走这里）：回到 Idle，也就是"这件事结束了"。
        // NOTE: 会一起清掉 loaderName/loaderVersion/loaderId —— 要用的字段先在调用前存下来，
        // 忘了存的表现是消息里那个名字变成空的（不崩，但话就说不清了）。
        void reset() { *this = LoaderInstall(); }
    };

    void fetchFabricLoaderList();
    void onFabricLoaderListReply(QNetworkReply *reply, const QString &jobError);
    void fetchFabricProfile();
    void onFabricProfileReply(QNetworkReply *reply, const QString &jobError);
    // profile 拿到手之后：先看原版在不在，在就直接下一段，不在就先装原版。
    void startParentOrLoaderFiles();
    // 把加载器 json 落盘并下它自己那几个库（走 downloadVersion 那条老路）。
    void startLoaderFiles();

    // 老 Forge 安装器（不认 `--installClient`）那条路：先取原版的 json（说明书里那些"没有
    // url、原版又已经带着"的库要靠它认出来），再照说明书写我们自己的版本 json、摆好
    // universal jar，最后才轮到"补原版 + 下加载器自己的库"（前者走 WaitingParentJson，
    // 后者复用 startLoaderFiles()）。
    void startLegacySelfInstall();
    void writeLegacyProfile(const QJsonObject &parentJson);

    // ---- 整合包导入的内部状态机 ----
    //
    // 和 LoaderInstall 同一个形状、同一个理由：一次点击 = 好几件事（装加载器 → 落实例
    // json → 下清单那批文件 → 解压覆盖文件），每一步的接续点都在别人的信号槽里，需要一个
    // "走到哪了"的记号。少了它，中间那几批会各自被当成一次独立的下载 —— 界面会弹好几次
    // "下载完成"、按钮中途解禁，用户以为装完了。
    struct ModpackInstall {
        enum class Phase {
            Idle,
            InstallingLoader,    ///< 交给 installLoaderVersion() 那条路（只有它会装加载器）
            WaitingParentJson,   ///< 纯原版整合包：正在取原版的 version.json
            WaitingParentFiles,  ///< 纯原版整合包：原版那一批正在下
            WritingVersionJson,  ///< 落实例自己那份薄 json（瞬时，但排在事件循环上）
            DownloadingFiles,    ///< 清单里那批文件正在下
            ExtractingOverrides, ///< 后台解压 overrides；完成前保持实例占用
            VerifyingCompletion, ///< 后台校验与安装凭据生成；保持占用，可取消
        };

        Phase phase = Phase::Idle;
        QString zipPath;    ///< 用户选的那个整合包文件
        QString instanceId; ///< 落地的实例名（= versions/<id> 那个目录名）
        QString loaderId;   ///< 实例的父版本：加载器那个目录（纯原版包就是原版号）
        ModpackImport::Pack pack;
        /// 这个包那份清单的指纹（`InstanceFiles::importManifestFingerprint()`）。
        /// 落进 `wtfml.importing` 标记：同名的下一次导入靠它认"是不是同一个包"。
        QString manifestFingerprint;
        int extractedEntries = 0; ///< 解开了几个覆盖文件（收尾那句话要说出来）

        bool active() const { return phase != Phase::Idle; }
        // 收尾（成功、失败都走这里）：回到 Idle，也就是"这件事结束了"。
        // NOTE: 和 LoaderInstall::reset() 一样会连 pack 一起清掉 —— 要用的字段先在调用前
        // 存下来，忘了存的表现是那句话里整合包的名字变成空的。
        void reset() { *this = ModpackInstall(); }
    };

    /// 实例那份薄 json 落盘，然后起清单那批下载（也用于纯原版包：父版本是原版号）。
    void startModpackFiles();
    /// 解压 overrides/ 与 client-overrides/（剥掉前缀摊到实例目录里）。
    void extractModpackOverrides();
    /// 成功收尾：写 wtfml.meta + 发**唯一**那个终态信号。
    void finishModpackImport();
    void commitModpackImport(const QList<ModpackImport::FileIssue> &issues);
    /// 失败收尾。会把还在跑的加载器安装一起收掉（导入失败 = 整件事结束）。
    void failModpackImport(const QString &reason);

    // ---- 装"一个用户起了名字的实例"的内部状态机 ----
    //
    // 形状和 ModpackInstall 一样、理由也一样（一次点击 = 好几件事，每一步的接续点都在别人的
    // 信号槽里），但它**只有两步**：装父版本 → 写实例自己那份薄 json。整合包那条路后面的
    // "下清单那批文件""解压覆盖"这里都没有，所以单独一个小状态，别去挤 ModpackInstall
    // （挤进去的话"哪些相位算导入"会多出几种只在原版安装时才出现的组合）。
    //
    // NOTE: 父版本是**加载器**时这里只有一个相位 WaitingLoader —— 剩下的整段（拉版本表、
    // 跑安装器、补原版）全在 LoaderInstall 那个状态机里，这里只是**挂在它后面**：
    // finishLoaderInstall() 收尾前会看一眼这个状态还在不在。两套状态同时 active 是
    // 这一条路的**正常**样子，别把它当成"状态串了"。
    struct NamedInstance {
        enum class Phase {
            Idle,
            WaitingParentJson,  ///< 正在取父版本（原版）的 version.json
            WaitingParentFiles, ///< 父版本（原版）那一批正在下
            WaitingLoader,      ///< 父版本是加载器：整段交给 installLoaderVersion()
        };

        Phase phase = Phase::Idle;
        QString instanceId;  ///< 用户起的名字 = `versions/<id>` 那个目录名
        QString parentId;    ///< 实例薄 json 的 `inheritsFrom`

        bool active() const { return phase != Phase::Idle; }
        void reset() { *this = NamedInstance(); }
    };

    /// 写实例自己那份薄 json。`instanceId == parentId` 时**什么都不做** —— 那一支就是
    /// "用户没改名字、装的就是原版本身"，不该在旁边多出一个同名的目录（那是同一个目录）。
    /// @return 写成功、或者本来就不需要写 → true；写失败 → false，**此时失败终态已经发过了**
    /// （调用方只能退出，不能再发一个信号）。
    bool writeNamedInstanceJson();
    /// 成功收尾：写薄 json + 发**唯一**那个终态信号（成功）。id 报的是用户的实例名。
    void finishNamedInstance(const QString &parentId, const QString &parentMessage);
    /// 失败收尾：清状态 + 发终态信号。加载器那条路上失败由 failLoaderInstall() 接手。
    void failNamedInstance(const QString &reason);

    /// 把一份版本 json 原样写到 `<destinationPath>/<versionId>.json`。
    /// 下载路径和整合包导入共用这一份（QSaveFile：断电只会留下旧内容，不会留下半截 json）。
    static bool writeVersionJson(const QString &versionId, const QString &destinationPath,
                                 const QJsonObject &json, QString *errorOut);

    /// 按一份**已经合并好的** json 把"这个版本要哪些文件"登记进当前批次：客户端 jar、库、资源。
    /// **不 beginBatch / 不 endBatch / 不写 json** —— 那些分别是调用方自己的事。
    ///
    /// 单独开一个口是因为 downloadVersion()（装一个新的）和 repairVersionFiles()（修一个旧的）
    /// 对"要哪些文件"必须给出**逐字节一样**的答案：两条各写一份的下场是将来加一类文件只加到
    /// 其中一条上，而那种 bug 的形状是"点了补全还是起不来"——查起来最费劲的那种。
    ///
    /// @param versionJson 已经走完继承链合并的 json。
    /// @param jarOwnerId 客户端 jar 在镜像源里算哪个版本号取（= 磁盘上那份 jar 的主人）。
    /// @param jarPath 客户端 jar 落到的完整路径。
    void registerVersionFiles(const QJsonObject &versionJson, const QString &jarOwnerId,
                              const QString &jarPath);

    /// 加载器版本号定了之后那一段（从版本表挑出来的、整合包钉死的，都走这里）：
    /// 算 loaderId / 安装器地址 / 安装器文件名，再把"下安装器"排到下一轮。
    /// NOTE: 消息**不在这儿发** —— "挑中 xxx"和"整合包指定的是 xxx"是两句不同的话，
    /// 由调用方在调它之前自己说。
    void adoptInstallerVersion(const QString &version);

    // ---- Forge / NeoForge / OptiFine：跑它们自己的安装器 ----
    //
    // NOTE: 这一段里的**前两个**只有 Forge/NeoForge 走（拉版本表 + 起子进程），后面那几个
    // 是三个共用的：下安装器 jar、读它的输出、心跳/超时、收尾核对。OptiFine 的版本表
    // 和"装到哪去"完全是另一回事，单独在下面一段里。

    // 拉版本表（Forge 的 promotions_slim.json / NeoForge 的 maven-metadata.xml）。
    // 两份接口一个 JSON 一个 XML，但都是"拉一段文本、挑一个版本号"，所以共用一条路，
    // 按 kind 在回复处理里分派。
    void fetchInstallerList();
    /// 发一发"取版本表"的请求。url 可能是镜像那条、也可能是官方那条（见
    /// onInstallerListReply 里"镜像挑不出来就退回官方"那一段）。
    void requestInstallerList(const QString &url);
    /// 镜像那份版本表挑不出来时的退路：换官方那条再发一发。
    /// @return 真的退了（调用方应当直接 return，等下一发回复）返回 true。
    bool retryInstallerListFromOfficial();
    void onInstallerListReply(QNetworkReply *reply, const QString &jobError);
    // 把安装器 jar 下到 cache/installers/（一批下载，完成信号回到 onAllDownloadsFinished）。
    void startInstallerDownload();
    // 前置检查 + 起那个 Java 子进程。**异步**：它不阻塞界面，结果由 onInstallerFinished 收。
    // Forge/NeoForge 会**先**探一次 `--help`（见 probeInstallerOptions），探完才跑真正的安装。
    void runInstaller();
    // 起一次 `java -jar <安装器> --help`，只为问出这一版安装器认哪些命令行开关。
    void probeInstallerOptions();
    // 探针那一次跑完：落定 InstallerOptions，然后接着跑真正的安装（或者报"装不了"）。
    void onProbeFinished(int exitCode, bool crashed);
    // 真正跑安装器（探针之后、或者 OptiFine 那种不用探的）：起进程 + 报进度 + 开定时器。
    void startInstallerProcess();
    /// 安装器 jar 没下下来时的第二次机会：换 Forge 的另一种 maven 坐标再下一遍。
    /// @return 真的换了（调用方应当直接 return，等那一批的完成信号）返回 true。
    bool retryInstallerDownloadAlternate();
    // 读子进程的输出（只留尾巴，用来在失败时报"卡在哪"）。
    void onInstallerOutput();
    // 每秒一跳：报心跳，顺带执行硬超时。
    void onInstallerTick();
    void onInstallerError(int processError);
    void onInstallerFinished(int exitCode, bool crashed);
    // 收尾那一路（超时 / 起不来）：先把 phase 挪开再杀进程，然后发失败信号。
    void abortInstaller(const QString &reason);
    // 安装器装完之后把**原版**补齐：它不下 assets，而且 Forge 连原版的 json 都不写
    // （只在 versions/<原版>/ 留下一份客户端 jar）。
    void startParentCompletion();

    // ---- OptiFine：无界面入口 + 换 APPDATA ----

    // 拉版本表：镜像源是一条 JSON 接口，官方只有那 203 KB 的下载页可读。两边回复的形状
    // （JSON 数组 / HTML）和挑法都不一样，所以按下载源分派，不共用回复处理。
    // @param fromMirror 这条回复是从哪条源要来的 —— 由**发请求那一处**决定好传进来，
    // 回复处理里不再重新读下载源（理由见定义处：两次读之间用户能换设置，
    // 换了就会拿另一种格式去解同一条回复）。
    void fetchOptifineVersionList();
    void onOptifineVersionListReply(QNetworkReply *reply, bool fromMirror,
                                    const QString &jobError);
    // 版本号定了之后的收尾：填 installerFileName/loaderVersion，再按下载源决定"直接下（镜像）"
    // 还是"先去广告页拿 token（官方）"。**两条路共用**（版本表挑出来的 / 用户钉死的），
    // 差别只在"版本号从哪来"。定义处有详细说明。
    // @param fromMirror 同上：走镜像还是走官方那条路，由调用方定好传进来。
    void adoptOptifineEdition(const QString &edition,
                              const LoaderMeta::OptiFineMirrorPath &mirrorPath,
                              bool fromMirror);
    // 官方那条路多一步：真文件在广告页后面，得先把它那条带 token 的地址抠出来。
    void fetchOptifineDownloadToken();
    void onOptifineDownloadTokenReply(QNetworkReply *reply, const QString &jobError);
    // OptiFine 的安装器**要拿原版的客户端 jar 当输入**，所以原版必须先在盘上才能起它
    // （这一点和 Forge 装完再补正好相反）。判据和 startParentOrLoaderFiles() 一样：
    // 版本 json + 客户端 jar 两份文件都在。
    void ensureParentForOptifine();
    // 在系统临时目录里建一个假的 .minecraft：原版的 json / jar 各拷一份进去，再补一份最小
    // 的 launcher_profiles.json（安装器认它）。子进程的 APPDATA 会指到它上面。
    bool prepareOptifineWorkDir(QString *errorOut);
    // 把那个假 .minecraft 里的产物搬回真游戏目录：认版本目录（名字是安装器自己拼的，我们
    // 算不出来）、核对那份 json、覆盖着拷走版本目录和 libraries/optifine/，然后收尾。
    void adoptOptifineProducts();
    QString optifineWorkRoot() const;
    QString optifineWorkMinecraftDir() const;
    // 删掉那个临时目录（递归删 —— 它整个是我们自己刚建的，见成员那里的 NOTE）。
    // 成功和失败两条终态都调用：finishLoaderInstall() / failLoaderInstall()。
    void cleanupOptifineWorkDir();

    // launcher_profiles.json 是那三个安装器的硬前置（缺了它安装器直接拒绝干活 —— Forge 是
    // 第一件事就查，OptiFine 是装完最后一步才查）。
    // 没有就补一份最小的；已经有就一个字都不动 —— 里面可能是用户真正的启动器档案。
    // @param minecraftDir 补到哪个 .minecraft 里。**是个参数、不是 AppPaths::minecraftDir()**：
    // OptiFine 那一份是补给我们自己的临时工作目录的（它的安装器只认 APPDATA）。
    // @param createdOut 非空时回填"这次是不是我们新建的"（只有新建才值得跟用户说一声）。
    // NOTE: 非 const —— 它 mkpath + QSaveFile，真往磁盘上写东西；这一份头文件里
    // 为别的写侧函数专门立过规矩（见 updateVersionMeta / renameInstance 那两条
    // "非 const —— 它真的改磁盘"）。假 const 会诱使人在只读路径（扫描、刷新列表）上
    // 调用它，那些路径一次会话要跑很多遍、绝不能带副作用。
    bool ensureLauncherProfiles(const QString &minecraftDir, bool *createdOut, QString *errorOut);

    // 把 realVersion / loaderName / loaderVersion 合并写进 <id>/wtfml.meta。
    void writeLoaderMeta();
    // 成功/失败的**唯一**终态：写 meta（成功时）、清状态、发 versionDownloadFinished。
    // @param message 那批下载的汇总（"N 个文件已全部就绪"那种），拼进终态话术里。
    void finishLoaderInstall(bool success, const QString &message);
    // 中途失败：清状态、把缓存里那份"还没落盘的 json"扔掉、发一个失败信号收尾。
    // @param reason 给用户看的一句话（谁卡住了、下一步能做什么）。
    void failLoaderInstall(const QString &reason);

    // deferNextStep() 的加载器版本：排到下一轮，而且**只在相位还是 expected 的时候才跑**。
    // 排队的这一步是在调用方返回之后才执行的，中间那一轮里这次安装可能已经被别的东西收了尾
    // （用户又点了一次、安装器超时那一路报了错）—— 相位对不上就是"这份接续没人要了"，
    // 和每个回复处理函数开头那几句"状态已经换过"是同一个判据。
    void deferLoaderStep(LoaderInstall::Phase expected, std::function<void()> step);

    /// deferNextStep() 的"带名字的实例"版本：同上，只在相位还是 expected 时才跑。
    void deferNamedStep(NamedInstance::Phase expected, std::function<void()> step);

    // deferNextStep() 的兜底出口：把排队的这一步抛出来的异常翻成一次失败收尾
    // （abandonBatch() + 终态信号）。见 deferNextStep() 的 NOTE。
    void recoverFromInternalError(const QString &what);

    /// 把"当前这一批**不再继续下去了**"这件事收尾：按"谁还活着"依次收掉整合包导入 → 带名字的
    /// 实例 → 加载器安装 → 资源下载 → 纯版本下载，各自把话说出去。
    ///
    /// **两个调用者，只有 reason 那句话不一样**：`recoverFromInternalError()`（内部异常，
    /// 先 `abandonBatch()`）和 `cancelActiveDownload()`（用户点的取消，先 `cancelBatch()`）。
    /// 两处各写一份的话，早晚有一处漏掉新加的那个状态机 —— 漏掉的表现是"那个状态永远留在
    /// 活着的相位上，之后每一次都报'已有任务正在进行'，只能重启启动器"。
    ///
    /// 顺序是从**最外层**往里：导入整合包的第一步就是装加载器（两个状态机同时活着），
    /// 装一个带名字的实例时父版本是加载器（同理）—— 收里层那个的话，外层会卡在半路上。
    void unwindDownloadBatch(const QString &reason);

    // ------------------------------------------------------------------
    // 批次的开与收：**这一批是不是我开的**
    // ------------------------------------------------------------------
    //
    // 下载器是**共用**的（服务端安装那四段也在同一个 `DownloadManager` 上开批次），而
    // `allDownloadsFinished` 只有 `onAllDownloadsFinished()` 一个接收方 —— 所以它必须能分辨
    // "这一批是我开的"还是别人的。四位包装把 `DownloadManager` 那四个批次动作各收成一处
    // （全仓调用它们**只有**这四个函数，别处再写一遍就漏记了）。来龙去脉见 .cpp 里那一段。

    /// `downloadManager->beginBatch()` 的**唯一出处**（先把"这一批是我开的"记上）。
    void beginOwnBatch();
    /// `downloadManager->endBatch()` 的**唯一出处**。**不**销号：`endBatch()` 多数时候一个
    /// 信号都不发，真正那一发要等最后一个回复回来（见 `onAllDownloadsFinished()`）。
    void endOwnBatch();
    /// `downloadManager->abandonBatch()` 的**唯一出处**（内部异常那条路）。销号。
    void abandonOwnBatch();
    /// `downloadManager->cancelBatch()` 的**唯一出处**（用户点取消那条路）。销号。
    void cancelOwnBatch();

    /// 安装器输出的最后一行非空内容（心跳和失败报告都用它）。
    QString installerLastOutputLine() const;

    DownloadManager *downloadManager;
    // 加载器元数据那两次 GET 用**自己**的 network manager，不复用 DownloadManager 的：
    // 后者的 fetchVersionJson 是"一次只等一个回复"的（m_pendingVersionId 那种），
    // 混用会让两条路互相把对方的等待状态顶掉。
    QNetworkAccessManager *loaderNetwork = nullptr;

    /// `loaderNetwork` 的**唯一出处**（没有就建，建一次就一直用）。
    ///
    /// NOTE: 每个 `fetch*` 自己写「nullptr 就 new」的话，会漏掉直接从"发请求"开始的两条路：
    /// `fetchFabricProfile()` 和 `fetchOptifineDownloadToken()` —— 走这两条的时候它还是
    /// nullptr，解引用当场崩。最要紧的一条入口是整合包导入：
    /// `importModpack()` → `installLoaderVersion(..., 包里钉死的版本)` →
    /// Fabric 那条「钉死了就别去问版本表」的分支直接调 `fetchFabricProfile()`。
    /// 所以在"全新启动 → 从没点过安装页 → 导入一个 Fabric 整合包"这条路上，
    /// 启动器会当场消失（GUI 子系统，连一行日志都没有）。
    /// 收成这一处之后，谁用谁建，「漏了一个懒建点」不再是可能的错。
    QNetworkAccessManager *loaderNetworkManager();
    LoaderInstall m_loaderInstall;
    ModpackInstall m_modpackInstall;
    QFuture<void> m_modpackOverridesWork;
    std::shared_ptr<std::atomic_bool> m_modpackOverridesCancelled;
    QString m_modpackOverridesFailure;

    /// CurseForge 那两发请求要用的设置（见 setCurseForgeApiKey 那一段）。
    QString m_curseForgeApiKey;
    QString m_curseForgeApiBaseOverride;
    /// Modrinth 那条反查路的地址覆盖（见 setModrinthApiBaseOverride）。空 = 官方地址。
    QString m_modrinthApiBaseOverride;

    /// 「装一个带名字的实例」走到哪了（见上面那段）。父版本是加载器时它会和
    /// m_loaderInstall **同时** active —— 那不是状态串了，是这一条路的正常样子。
    NamedInstance m_namedInstance;

    /// 「往实例里下一个资源文件」那一批（见 downloadResourceFiles）。
    ///
    /// NOTE: 它**没有相位**，只有一个"在不在"的标记 —— 这条路就是"下一批文件、下完报一次"，
    /// 没有"接着还要干什么"。所以这里刻意不挤进上面那几套状态机（挤进去的话，
    /// onAllDownloadsFinished 里每一段都要多问一句"这个相位会不会是资源下载留下的"）。
    /// NOTE: 它存在的唯一理由：onAllDownloadsFinished 最后那一句是**无条件**
    /// `emit versionDownloadFinished(...)`，不在这里拦一下的话，下一批模组会被报成
    /// "版本 xxx 的文件已全部就绪"（而那个版本根本没有在下载）。
    struct ResourceDownload {
        bool running = false;
        QString label;
        void reset() { *this = ResourceDownload(); }
    };
    ResourceDownload m_resourceDownload;

    /// 上一次**失败**的那一批里，失败的那几个文件（「重试只重做失败项」）。
    ///
    /// `valid` = 这一批**值得**重试。只有"纯下文件"的批次才算数：加载器安装、整合包导入、
    /// 带名字的实例那三条路上，重下几个文件补不回来（加载器的安装器得再跑一次、导入还得解压
    /// 覆盖），露一颗点了也修不好的「重试」比不露更糟。
    struct FailedBatch {
        bool valid = false;
        /// 是下载页那几类资源那一批 —— 收尾那一段靠它决定发 `resourceDownloadFinished`
        /// 还是 `versionDownloadFinished`（重试之后终态还能报对，全靠这两个快照）。
        bool resource = false;
        QString id;
        QString label;
        QList<DownloadManager::FailedJob> jobs;
        void reset() { *this = FailedBatch(); }
    };
    FailedBatch m_failedBatch;

    /// 正在跑的那一趟「GTNH 首启资源预取」。同一时刻只可能有一趟（开批次前就问过
    /// `downloadManager->isBusy()`），所以一格就够。
    ///
    /// 收尾它在 `onAllDownloadsFinished()` 里**排在最前面**：这一批不是版本文件、也不是
    /// 加载器安装，落到下面那几条分支里会报出「版本 X 的文件已全部就绪」这种错话。
    struct AssetDirectorPrefetch {
        bool active = false;
        QString versionId;
        QString setId;            ///< 日志/收尾话术用（「哪一份已知集合」）
        int objects = 0;          ///< 清单里的对象总数
        int reuseOverlay = 0;     ///< `<ad>` 里已有且校验通过的
        int reuseAssets = 0;      ///< 共享 `assets/objects` 里已有且校验通过的
        int toDownload = 0;       ///< 这一趟交给下载器的对象数
        qint64 bytesToDownload = 0;
        void reset() { *this = AssetDirectorPrefetch(); }
    };
    AssetDirectorPrefetch m_assetDirectorPrefetch;

    // ---- "这个加载器有哪些版本可选"那几发 GET 的状态 ----
    //
    // 为什么不复用 m_loaderInstall：那一位装的是**一次安装**的状态机（相位、安装器地址、
    // 钉死的版本……），而"列版本"是安装**之前**的一次只读询问 —— 用户在安装页上把四个加载器
    // 挨个点开看看，一次安装都还没发生。混进去的话，点开一个加载器就会把 m_loaderInstall
    // 弄成"正在装"的样子，isLoaderInstalling() 当场变 true，底栏和按钮全跟着错。
    //
    // **每个加载器各占一格，四个可以同时在飞**。只留一格的话，
    // 新的一发会把上一发 abort 掉 —— 四个加载器在安装页上是四条并列的行，一进那一页
    // 四条一起问（要求："获取列表不能点开了再获取，得全获取完"），只有一格的话后三发
    // 会把前一发掐死，前三条底下就永远停在「正在获取…」。
    struct LoaderVersionRequest {
        QString gameVersion;
        QPointer<QObject> queued;
        quint64 generation = 0;
        // 这一发是走**镜像**那条路问出去的吗。**随请求钉住**，不在回复回来时再读一遍
        // downloadManager->useMirror()。
        //
        // NOTE: OptiFine 两条路回来的正文**格式不一样**（镜像是 JSON 数组，官方是广告页
        // HTML），而"用不用镜像"是用户可以随时点的开关。问出去到回来之间那几百毫秒
        // 里开关被点一下，正文就会按**另一条路**的规矩解析 —— 症状是「列表读不懂」
        // 或者列出一份空的，而用户什么都没做错。另外三个加载器不走镜像，这里恒 false。
        bool fromMirror = false;
        // 这一发问的**官方**那条地址。官方连不上时拿它换镜像（只对 NeoForge 成立，
        // 见 VersionManager::loaderVersionsMirrorFallback()）。
        QString officialUrl;
        // 已经往镜像退过一次了没有 —— 只退一次，镜像再失败就照实报。
        bool mirrorFallbackUsed = false;
        // 正在飞的那一份。**同一个加载器**上换一局（同一个游戏版本重新问、或者换了
        // 游戏版本）时把它 abort 掉（abort 之后 finished() 照样会来一发，回复处理函数
        // 靠"这一个指针还是我手上这个吗"把它丢掉）。
        QNetworkReply *reply = nullptr;
    };
    // 键是**加载器下标**（VersionManager::loaderIndexForKind 那套），不是枚举 —— 枚举当
    // QHash 的键要靠 qHash 的特化，下标是 int，省一层。
    QHash<int, LoaderVersionRequest> m_loaderVersionRequests;

    // 官方那条地址问不通时该换哪个镜像地址；没得换返回空串。
    //
    // 只有 NeoForge 一支成立：它的 maven-metadata.xml 在 BMCLAPI 上是**同一份**
    // （实测全量），而官方 host 在国内**直接连不上**。Forge 那个同名文件
    // 镜像上是旧的，换过去会得出"没有 Forge"的假结论。理由全文在 .cpp 里。
    QString loaderVersionsMirrorFallback(LoaderKind kind, const QString &officialUrl) const;
    // 发那一发 GET，回复交给 onLoaderVersionsReply()。@p fromMirror 跟着这一发钉住
    // （见 LoaderVersionRequest 里那一格）。
    void fetchLoaderVersions(LoaderKind kind, const QString &url, bool fromMirror);
    void onLoaderVersionsReply(LoaderKind kind, QNetworkReply *reply);
    // 按加载器把这一份回复解析成"能挑的版本"。失败时 returned.error 非空（是给用户看的话）。
    struct LoaderVersionParse {
        LoaderMeta::LoaderVersionList versions;
        QString error;
    };
    // @param fromMirror 这一份正文是镜像那条路回来的吗。**收参数**，不在这里读
    // downloadManager->useMirror() —— 见 LoaderVersionRequest 里那段 NOTE。
    LoaderVersionParse parseLoaderVersions(LoaderKind kind, const QString &gameVersion,
                                           const QByteArray &body, bool fromMirror) const;

    // 跑那三个外部安装器（Forge / NeoForge / OptiFine）的子进程。懒建 —— Fabric 那条路
    // 一个进程都不起，所以装 Fabric 之前它一直是 nullptr。
    //
    // NOTE: 它**不能**同步跑（waitForFinished）。实测 Forge 47.4.10 在空目录上装一次要
    // 100 秒上下，NeoForge 更久；阻塞在槽里就是界面整体冻住那么久，连"未响应"
    // 都没有（Qt 的事件循环根本没在转）。所以走异步信号，另外配一个定时器报心跳 +
    // 兜硬超时。
    QProcess *installerProcess = nullptr;
    /// 这一次安装用的 Java 和安装目录：runInstaller() 里定下来，探完 `--help` 之后起真正那个
    /// 进程时要用**同一份**。存成员而不是重算，是因为 OptiFine 那个 targetDir 是临时工作
    /// 目录（重算要再建一次、还可能不一致）。
    QString m_installerProgram;
    QString m_installerTargetDir;
    /// 探 `--help` 那一次的原始输出。和 m_installerOutput **分开收**：后者是"失败时贴给用户
    /// 看的那几行"，混进一段 --help 只会把真正有用的东西挤出那 64 KB。
    QByteArray m_probeOutput;
    /// 这个安装器认哪些命令行开关（探完 `--help` 才有结论；没探到时按"宁可信其有"）。
    InstallerOptions m_installerOptions;
    // OptiFine 安装的临时工作目录（APPDATA 指过去的那一份）。懒建 —— 只有真去装
    // OptiFine 时才存在，装完（成功或失败）立刻删掉。
    //
    // NOTE: 它**必须**是系统临时目录里的一份真目录。删它是递归删（QTemporaryDir 的
    // remove() 就是这么干的），而在这里那是安全的：那底下只有我们自己刚建的东西，
    // 没有目录联接、没有用户的文件。真游戏目录那边就绝不允许这么删 ——
    // "不去动真游戏目录"正是换来的东西（见 LoaderMeta.hpp 里 OptiFine 那一节）。
    QTemporaryDir *optifineWorkDir = nullptr;
    QTimer *installerTimer = nullptr;
    /// 安装器输出的尾巴（stdout + stderr 混在一起）。只留最后 64 KB：失败时要报"卡在哪
    /// 一行"，但没必要把几万行 "Patching xxx 1/1" 都攥在内存里。
    QByteArray m_installerOutput;
    qint64 m_installerStartedAt = 0;  ///< QDateTime::currentMSecsSinceEpoch()，算超时用
    int m_installerTicks = 0;         ///< 定时器跳了几次，用来决定该不该报心跳
    // versionId -> versionJson。mutable 是因为 getVersionJson() 是 const 的，
    // 但它在回读磁盘后要顺手填充这个缓存。
    mutable QMap<QString, QJsonObject> versionJsons;
    // versionId -> **合并后**的 versionJson（走完 inheritsFrom 链）。单独一张表：
    // 往 versionJsons 里塞合并结果会把"原始那份"盖掉，而下载路径要靠原始那份落盘。
    //
    // 每一格里除了 json 本体，还懒算一份**只取决于这份 json** 的事实（加载器证据和
    // `type`，见 instanceTraits()）。放在同一格里是有意的：丢掉 json 缓存的那六处
    // （复制 / 删除 / 整合包替换 / 加载器安装 / 换文件夹 / 单版本 remove）现在**不必**
    // 再记得多清一张表 —— 漏清一处的症状是"删掉重装同一个版本号之后，实例行前面还是
    // 旧加载器的标志、归类还留在旧组里"，而屏幕上没有任何一句话说得通。
    //
    // NOTE: `const` 成员不是笔误：instanceTraits() 是 const 方法，事实是在第一次问的时候
    // 补进缓存那一格里的（和这张表本身迟到填充是一个道理）。
    struct MergedJsonEntry {
        QJsonObject json;
        mutable bool factsScanned = false;  ///< 下面那几个字段算过没有
        mutable bool loaderFound = false;   ///< loaderEvidenceFromJson() 认出来没有
        mutable QString loaderKey;          ///< 认出来时的证据键（见 loaderDisplayNameForEvidenceKey）
        mutable QString loaderVersion;
        mutable QString type;               ///< json 自己写的 "type"（可能是空串）
    };
    mutable QMap<QString, MergedJsonEntry> mergedVersionJsons;
    /// 取合并 json 那一格（缓存里没有就先取一次；取不到 / 继承链没走完 ⇒ nullptr）。
    /// 只在 instanceTraits() 用得上；`getVersionJson()` 不给指针是因为调用方要的是内容。
    const MergedJsonEntry *mergedJsonEntry(const QString &versionId) const;
    /// 一份 `<id>/wtfml.meta` 的读结果缓存（见 `readVersionMeta()`）。
    ///
    /// 为什么要缓存：实例树**每重灌一次**就对同一个实例问三四个问题 —— 显示名一次、
    /// 稳定 id 一次、"按加载器归到哪个默认组"两次。真读是"打开 + 读 + 解析"，200 个
    /// 实例那一趟实测一百多毫秒（`build/tmp-wtfml-probe` 的分段探针，同一版二进制），
    /// 而其中九成是同一个文件的第 2~4 次打开。
    ///
    /// NOTE: 键是**完整路径**而不是版本 id —— 换一整份 .minecraft 之后两份目录里各有
    /// 一个 `1.20.1`（同 `clearVersionJsonCache()` 那段理由）。
    /// NOTE: 判据里带着"在不在 + 修改时刻 + 字节数"。盘上那份被**别人**改过（用户
    /// 手写、别的启动器）一定看得出来；唯一看不出来的是"同一个毫秒里、同样字节数"
    /// 的改写 —— 我们自己写的那条路不会落进这个缝（`updateVersionMeta()` 写完当场把
    /// 这一格丢掉）。旧注释里那句"缓存了就会变成改了名不生效"说的就是这个缝，别把
    /// 那两个判据摘掉。
    struct MetaCacheEntry {
        bool exists = false;
        qint64 modifiedMs = 0;
        qint64 size = -1;
        QJsonObject meta;
        InstanceMetaFile::Status status = InstanceMetaFile::Status::Missing;
    };
    mutable QHash<QString, MetaCacheEntry> m_metaCache;
    /// 缓存的上界（张数，不是字节）。数据目录来回切时旧路径会留在表里，
    /// 过了这个数就整表丢掉重来（比按 LRU 记账简单，而这里没有"热"可言）。
    static constexpr int kMaxMetaCacheEntries = 4096;
    QString m_currentVersionDownloading;
    // 现在这一批下载**是不是我自己开的**（见上面四个包装那一段）。分辨"别人的批次"全看它：
    // `onAllDownloadsFinished()` 开头那道判据、还有 `busyVersionId()` 那道门控，问的都是它。
    // 它在 `beginOwnBatch()` 置真、在"这一批不会再有完成信号了"的四个地方销号
    // （`onAllDownloadsFinished()` / `abandonOwnBatch()` / `cancelOwnBatch()`）。
    bool m_ownBatchOpen = false;
    // 正在取 version.json 的版本号。拿到手的 JSON 里没有 "id" 字段时（网络失败、
    // manifest 里没有这个版本），就靠它告诉 UI"这次失败了"，否则 UI 会一直等下去。
    QString m_pendingVersionId;
    // 版本列表还没拉过时要先拉列表，拿到之后再回来取 version.json。
    // 这里记下待取的版本号，续接动作放在 onVersionManifestFetched 里。
    QString m_fetchJsonAfterManifest;
    /// 后台扫盘的代数：每次 `requestInstalledVersions()` +1，回调只认最新那一代。
    quint64 m_installedScanGeneration = 0;
    /// 在途那一发的取消旗（析构时也置真）。用 shared_ptr 是因为工作线程可能比某一次
    /// 请求活得久 —— 它只持有这个旗和路径，不碰本对象。
    std::shared_ptr<std::atomic_bool> m_installedScanCancelled;
};
