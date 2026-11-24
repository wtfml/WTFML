/**
 * @file ResourceStore.hpp
 * @brief 下载页那几类资源的**取数**：搜一次、问一个资源的版本列表、取一个图标。
 * 地址怎么拼、JSON 怎么读全在 ResourceCatalog 那一层（纯函数），这里只管"发出去、
 * 收回来、认出来是给谁的"。
 *
 * 【为什么是异步的，不用 HttpClient::send】
 *
 * HttpClient::send 是**同步**的（内嵌事件循环）。它的调用方都是"点一下、本来就等着一个结果"
 * 的动作（登录、换皮肤），那没问题。但搜索不是：用户会在搜索框里连敲几次回车、会点着左边的
 * 分类来回换，每一次都卡住界面十几秒的话，这个页面就等于不能用 —— 这个项目在"点启动那条路
 * 三段同步等"上已经吃过一次这个亏了（memory 里那条）。
 * 所以这里照 VersionManager 那套来：QNetworkAccessManager + finished 回调，界面一次都不停。
 *
 * 【每一发都带一把"钥匙"】
 *
 * 一次请求要认得出回来的是不是**我现在等的那一发**：用户连敲两次回车，第一发回来得比第二发
 * 晚的话，晚到的那份结果会把新的那份盖掉（屏幕上显示的是上一个搜索词的结果，而且不报错）。
 * 所以每个频道各有一把钥匙（同一个 key 上新的一发把旧的掐掉），信号也把 key 原样带回去，
 * 界面按 key 判断"这是不是我现在那一页要的"。
 * 和 VersionManager 那边"看 reply 指针还是不是手上这个"是同一个道理，只是这里跨了函数边界，
 * 得有个名字。
 *
 * 【每一发都走共享的 Control 额度】
 *
 * 搜索、版本表、项目表、精确前置、文件身份、更新批量和图标，全都用
 * `RequestBudget::ControlGet`：和 HttpClient、加载器版本查询、皮肤那些短请求共用四个
 * Control 名额（大文件仍走 Bulk）。没有名额时只排队、不阻塞界面；排队与传输计入同一个
 * 截止（接口那几发 15 秒、图标 8 秒）。取消 = 删掉作业：排队中的不会发出去，已发出的
 * abort 且不回调。
 *
 * 【两个来源（ 加的）】
 *
 * 这一层的每个入口都带一个 `ResourceCatalog::Source`：地址怎么拼、正文怎么读都在
 * ResourceCatalog 那一层按来源分叉了，这里要管的只有三件事 ——
 * ① CurseForge 的接口根地址和 `x-api-key`（**默认走一台不要 key 的社区镜像**：那件事的
 * 取舍、以及 换过一次的实测都写在 ModpackImport::curseForgeApiBase 上面）；
 * 地址和头都跟着 source 走，别的来源一个字母都不发；
 * ② 版本列表那边"200 但正文不是一个数组"的判据要按来源分（Modrinth 是数组、
 * CurseForge 是对象），分错了的症状是"CurseForge 那一页永远说版本列表读不懂"；
 * ③ 图标那几条按 `Hit::iconCacheId()`（带来源前缀）走，见下面那段。
 */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>

#include "download/ResourceCatalog.hpp"
#include "net/ControlGet.hpp"

class QNetworkAccessManager;
class QNetworkReply;

class ResourceStore : public QObject {
    Q_OBJECT

public:
    explicit ResourceStore(QObject *parent = nullptr);
    ~ResourceStore() override;

    /// CurseForge 那两发请求要用的设置（**和 VersionManager 那一对是同一件事、同一个来源**：
    /// 都是 MainWindow 从 LauncherSettings 推过来，判断和取舍都在
    /// `ModpackImport::curseForgeApiBase()` 上面，这里只负责存）。
    ///
    /// NOTE: `apiKey` 空 = 走那台不要 key 的社区镜像；非空 = 走官方并带 `x-api-key`。
    /// `baseOverride` **只有验证工程会写**（`Launcher/CurseForgeApiBase`，把接口指到
    /// 本机的假服务器上）；产品自己从不写它。
    /// NOTE: 这两条只影响**之后**发的请求 —— 改完不重发已经飞出去的那一发（用户在设置页
    /// 填完 key 回到下载页得自己再点一下搜索，这和"设置页改完什么都是下一次生效"
    /// 是一致的）。
    void setCurseForgeApiKey(const QString &apiKey);
    void setCurseForgeApiBaseOverride(const QString &base);

    /// Modrinth 那一侧的**同一个口子**（`Launcher/ModrinthApiBase`，同样**只有验证工程会写**）。
    void setModrinthApiBaseOverride(const QString &base);
    /// 手上这一份 Modrinth 根地址（空 override ⇒ 空串 = 让 ResourceCatalog 用官方那一份）。
    QString modrinthApiBase() const { return m_modrinthApiBaseOverride; }

    /**
     * @brief 搜一次。
     * @param requestKey 这一发的钥匙（界面自己定，信号会原样带回来）。同一个 key 上还有
     * 一发没回来就先掐掉 —— 用户又敲了一次回车，他要的是后一次的结果。
     * @param kind 搜哪一类（决定 project_type 那个 facet / CurseForge 的类目号）。
     * @param source 搜哪个资源站（地址、要不要 key、正文怎么读都跟着它走）。
     * @param query 搜索词；**空串是合法的**（那就是"这类里最热门的"，进页面时就是这么问的）。
     * @param gameVersion 非空则只搜支持这个游戏版本的。
     * @param loader 非空则只搜这个加载器的（fabric/forge/...）。**界面上只有模组那一页会给它**，
     * 而 ResourceCatalog 那一侧本来也不认其它几类的加载器。
     * @param category 非空则只搜这个类型（键来自 `ResourceCatalog::categoryOptions()` ——
     * 一个 slug 或者一个类目号，**别自己拼**）。
     *
     * 结果走 searchFinished（**成功和失败都发**，"没搜到"和"没搜成"在 result 里分得开）。
     */
    void search(const QString &requestKey, ResourceCatalog::Kind kind,
                ResourceCatalog::Source source, const QString &query,
                const QString &gameVersion = QString(), const QString &loader = QString(),
                const QString &category = QString());

    /**
     * @brief 问一个资源在某个游戏版本下有哪些版本可下。
     * @param requestKey 这一发的钥匙（用 ResourceCatalog::versionsRequestKey() 拼的那把）。
     * @param source 去哪儿问（地址、要不要 key 跟着它走）。**必须和这条 Hit 的来源一样** ——
     * 两个来源的 id 空间会撞（见 Hit::iconCacheId 那段），问错边问出来的东西
     * 是另一个资源的版本列表，而且不报错。
     * @param gameVersion 空 = 不筛（"这个资源的全部版本"，界面上还没选游戏版本时不该这么问）。
     *
     * 结果走 versionsFinished。**三条路要分得开**：失败（problem 非空）/ 问到了但一条都没有
     * （versions 空、problem 空）/ 正常 —— 前两条在界面上是两句不同的话
     * （加载器树那边「无可用版本」和「列表没取到」踩过同一个坑）。
     */
    /// Resolve a Modrinth release whose dependency supplies no project identity.
    /// Uses the same request-key cancellation and bounded transport as version lists.
    void fetchModrinthVersion(const QString &requestKey, const QString &versionId);
    void fetchCurseForgeVersion(const QString &requestKey, const QString &fileId);
    // Exact installed files, not latest updates. At most 100 unique SHA-1s per call.
    void fetchModrinthFiles(const QString &requestKey, const QStringList &sha1s);
    void fetchVersions(const QString &requestKey, const QString &projectId,
                       ResourceCatalog::Source source, const QString &gameVersion,
                       const QString &loader = QString());

    /**
     * @brief 批量问"这一串文件各自该换成哪一版"。
     *
     * 这个接口（`POST /version_files/update`）就是为它准备的：**一次带一串 sha1**，
     * 回"每个 sha1 它那个项目在 `gameVersion` + `loader` 下**最新**的那一版"。295 个模组
     * = 3 发（每发 100 个哈希，见 `.cpp` 里那个 `kLatestChunk`）。
     *
     * @param requestKey 用 `ModDependencies::latestVersionsKeyFor()` 拼的那把（**唯一出处**）。
     * @param sha1s 盘上那些文件各自的 sha1（`InstanceIconLookup::Found::sha1`）。**空表 =
     * 立刻回一发空的**（"一个都不用问"也是走完了，和 `fetchProjects()` 空表那条一致）。
     * @param gameVersion / @param loader 筛参数（**空 = 不筛**）。★ 这两个**必须**和
     * `versionsUrl()` 那一路给的一样，不然同一个模组会被判成"可更新"到另一个加载器
     * 或者另一个游戏版本上去 —— 而接口不会报错，只回一个"筛过之后"的答案。
     *
     * 结果走 `latestVersionsFinished()`。**分批发、合并着回**：一发（100 个哈希）一个请求，
     * 全部回来才发一次信号（调用方不必知道里面分了几发）。problem 非空 = 整批就此作废
     * （**一条都不许当成"已是最新"**，见下面那条 NOTE）。
     *
     * NOTE: 键是**小写的 sha1**，认不出来的哈希**不在表里**（不是空对象）—— 那一条是
     * "说不清"（这个项目在这个游戏版本/加载器下没有版本，或者它压根不在 Modrinth 上），
     * **不是**"已是最新"。
     */
    void fetchLatestVersions(const QString &requestKey, const QStringList &sha1s,
                             const QString &gameVersion, const QString &loader = QString());

    /**
     * @brief 批量问"这几个项目号叫什么"。
     *
     * 用户 要的那两件事（前置模组一键下载 / 实例更新检测）都卡在**同一个缺口**上：
     * 手上只有项目号，而项目号（Modrinth 的 `nvQzSEkH`、CurseForge 的 `238222`）给用户看
     * 等于什么都没说 —— 前置那个框里得写"需要 Sodium"、更新报告里得写"JourneyMap 有新版本"。
     * 名字只在**项目对象**里（`/projects?ids=` 那份数组 / CurseForge 的 `POST /mods`），
     * 而这两条解析路（`parseProjects` / `parseCurseForgeMods`）都是现成的。
     *
     * 两个来源**不是同一条请求**：Modrinth 是 `GET /projects?ids=[…]`，CurseForge 是
     * `POST /mods` + 一个正文明的请求体（那一份的构造在 `ModpackImport` 里，见那一处）。
     * 所以这里按来源分岔，调用方只管递来源。
     *
     * @param requestKey 用 `ResourceCatalog::projectsRequestKey()` 拼的那把（**它会把 id
     * 排一遍** —— 同一批号换个顺序也是同一把钥匙，见那一处的 NOTE）。
     * @param projectIds 要问的那些号。**空表 = 立刻回一发空的**（"一个都不用问"也是走完了，
     * 和 `ServerPing::Pinger::start()` 空表那条规矩一致）。
     * @param source 去哪儿问。**必须和这些号是哪一家的一致** —— CurseForge 那边会把号当
     * 数字发出去，递一串 Modrinth 的 base62 上去是"发一个语法就不对的请求"。
     *
     * 结果走 `projectsFinished`。problem 非空 = 没取到；problem 空 + hits 空/少几条 =
     * 问到了、但接口没给那几条（**正常**：作者删了项目、或者号本来就不对）。
     * 少几条时界面上退成"用项目号显示"，不许因为少了一条就整批作废。
     */
    void fetchProjects(const QString &requestKey, const QStringList &projectIds,
                       ResourceCatalog::Source source);

    /**
     * @brief 要一个图标。
     *
     * 内存里已经有了就**立刻**发 iconReady（不等下一轮事件循环），所以要接这个信号之后再
     * 灌那批条目，不然已经缓存过的那些图标会错过这一发。
     * 取不到（404 / 那条资源本来就没传图标）时发一个**空 QPixmap** —— 那是"没有"，不是"失败"，
     * 界面上画占位方块，也不该弹窗。
     *
     * 【两级缓存：内存 + 盘上那一份，盘上那份**不过期**】
     *
     * 盘上有一份就**立刻**把它给出去（同步，一个请求都不发）—— "第二次进这一页是秒开"
     * 就是这一条。**不看它存了多久**：用户 定的是"缓存默认不过期"。
     * 换新版的办法是下面这条**核对**，不是把旧图作废。
     *
     * 【攒够 kIconRevalidateTtlMs 才核对一次】
     *
     * 同一个图标，**这一趟运行**里第一次被要的时候，后台带条件头再问一句"变了没有"
     * （`If-None-Match` / `If-Modified-Since`，凭据就是上次存下来那两条，见
     * loadIconValidators）。三条出路：
     * * **304**（没变）→ 图一个字节都不动：不写图、不发信号、界面上一格都不动；
     * * **200 且字节和盘上那份一模一样**（主机不认条件头时就会这样）→ 同上；
     * * **200 且字节不一样**（作者换了图标）→ 写盘、存新凭据、再发一次 iconReady，
     * 界面把那一格换成新的。
     * 一次运行里每个图标只核这一遍（见 m_iconRevalidated）—— "每次打开"指的是**启动器
     * 每次启动**，不是每翻一页。
     *
     * （要求：「模组图标做本地缓存功能，不要每次都拉取」）：**"每次启动
     * 都核一遍"这件事本身也收到了一条时间闸**（`kIconRevalidateTtlMs`）—— 盘上那份**最近
     * 核过**就整趟不问它，一个请求都不发。理由不是省那三百字节，而是**这台机器上那三百字节
     * 并不便宜**：实测（见 wtfml-modrinth-icon-cdn 那笔账）`cdn.modrinth.com` 对每一张图都
     * 先回一个 307 到 `cdn-alt.modrinth.com`，而那条域名在有些网络里根本连不通 —— 一页
     * 二十来个图标就是二十来发各自等到超时。图标是**最不要紧**的东西（它长得对不对取决于
     * 作者几年换没换过一张图），为它每次启动去敲二十次门不值当。
     * 记这一位的是 `.icon.meta` 的第四行（`IconValidators::checkedAtMs`）—— 它跟着那份
     * 字节走，图一换这一位自然重来。**核对的时候才刷新它**（拿到 304 / 拿到 200 都算核过）。
     *
     * 【为什么收的是整条 Hit 而不是一个 id：要挑地址，而地址要按来源挑】
     *
     * 同一个图标有两条地址（见 ResourceCatalog 文件头）：接口给的 `Hit::iconUrl` 大多是
     * 一条**小得多**的缩略图（Modrinth 那边是 96px 的 webp，样张 4800 B，而同一张的 PNG
     * 是 35 KB；CurseForge 那边是 256px 的 png），`Hit::iconPngUrl()` 是兜底那条
     * （Modrinth 自己按 id 拼、CurseForge 是接口给的 `logo.url` 原图）。**怎么选**：
     * * 接口给的那条**这一台用得了** → 先要它；
     * * 用不了（没有地址，或者是一条本机没有解码器的 webp）→ 直接要兜底那条
     * （一次请求，别为一条解不动的地址白发一发）。
     * 先要接口给的那条时**带一个兜底**：那一条没取到/没解出来就再要一次 iconPngUrl()，
     * 而这一发**不计失败**（详见 startIconRequest）。两条地址是同一个东西时（CurseForge
     * 只给了一条、Modrinth 那条本来就是 icon.png）**不追第二发**。
     *
     * 盘上那份缓存的扩展名是 `.icon` 不是 `.png` —— 里面两种字节都可能躺着（见
     * iconCachePath），叫什么名字只说明「这是个图标」，格式由 ImageDecode 按内容判。
     *
     * 【缓存键是 `Hit::iconCacheId()`，不是裸 project_id】
     *
     * 两个来源的 id 空间会撞（Modrinth 的 base62 id 里 `238222` 也是合法的，而
     * CurseForge 那边真有这么个号）。图标缓存默认不过期，撞上的症状是"这一格永远画着
     * 另一个资源的图标"。所以下面那一排静态函数收的都是 **cacheId**（
     * `<sourceKey>-<projectId>`），`iconReady` 带回去的也是它 —— 界面按 cacheId 认那一行
     * （见 MainWindow 的 resourceIconIdRole）。
     */
    void requestIcon(const ResourceCatalog::Hit &hit);

    // ---------- 图标缓存（纯函数，离线能钉住）----------

    /// 盘上那份图标的两条"凭据"（HTTP 的 ETag / Last-Modified）——**核对**那一发靠它们
    /// 问一句"变了没有"。两条都空 = 没有凭据：下一发只能老老实实要整张（主机也就只好
    /// 把整张再发一遍，那一条路照样是对的，只是费流量）。
    ///
    /// NOTE: 凭据是**跟着那一份字节**的：换了一张图，旧凭据立刻就没用了。所以写图标和
    /// 写凭据永远是同一件事的两半（见 startIconRequest 里那一发成功的那条路）。
    struct IconValidators {
        QString etag;
        QString lastModified;
        /// 它就是那道"攒够 kIconRevalidateTtlMs 才核对一次"的闸读的那一位（见
        /// requestIcon 上面那段）。0 = 不知道（老版本写的 meta 里没有这一行）—— 那就
        /// **照旧核一遍**（宁可多问一次，也别拿一个猜出来的时间当家）。
        qint64 checkedAtMs = 0;
        bool isEmpty() const { return etag.isEmpty() && lastModified.isEmpty(); }
    };

    /// 取值理由见 requestIcon 上面那段：图标是最不要紧的东西（作者几年不换一张图是常态），
    /// 而"核一遍"在有些网络里要等一串超时。7 天是个"用户根本不会注意到"的长度 —— 真换了
    /// 图，最多一周之后自己就过来了。
    static constexpr qint64 kIconRevalidateTtlMs = 7LL * 24 * 60 * 60 * 1000;

    /// 凭据那份旁挂文件：`<数据目录>/cache/icons/<cacheId>.icon.meta`。
    /// 跟着 iconCachePath() 一起走 —— 同样地，cacheId 不是文件名安全的那一类时回空串。
    ///
    /// NOTE: 为什么另起一个文件而不是塞进 `.icon` 里：那个文件是**整段图片字节**，
    /// 前面加任何东西都会让"按内容认格式"（ImageDecode，见那条 NOTE）读不出来。
    /// `.icon.meta` 是纯文本四行（etag / last-modified / 这份凭据是哪条地址给的 /
    /// 上一次核对是什么时候），读不懂就当没有。第三行是 补的、第四行是
    /// **老版本写的那几行读出来是空串**，按"没记地址 / 不知道核过
    /// 没有"处理（照样用得上 —— 前者当没凭据、后者当"该核一遍"）。
    static QString iconMetaPath(const QString &cacheId);

    /// 读那两条凭据。读不到（没有 / 读不动 / 内容不成形）一律回**空的** ——
    /// "没有凭据"和"凭据是空的"是同一件事：下一发整张重来。
    /// @param url 这一发**要去要的那条地址**（requestIcon 挑出来的那条，不是 project 的
    /// 唯一地址 —— 同一个 project 有两条）。meta 里第三行记着这份凭据是哪条地址的，
    /// **对不上就当没有凭据**：理由见实现处那条 NOTE（拿 A 的 ETag 去问 B，轻则白下
    /// 一整张，重则问出一个 304 来、这一格从此画着另一条地址的图）。
    static IconValidators loadIconValidators(const QString &cacheId, const QString &url);

    /// 把这一发拿到的那两条凭据写到盘上，并**盖上"刚核过"那个时间戳**（第四行，见
    /// IconValidators::checkedAtMs）—— 这个函数的所有调用点都是"刚刚真的和主机说了一句
    /// 话"，所以时间戳就在这儿盖，调用方不用管。
    static bool saveIconValidators(const QString &cacheId, const IconValidators &validators,
                                   const QString &url);

    /// 某个图标的缓存路径：`<数据目录>/cache/icons/<cacheId>.icon`。
    /// cacheId 一律走 `Hit::iconCacheId()`（带来源前缀，见上面那段）。
    ///
    /// cacheId 不是文件名安全的那一类（含 `/`、`\`、`:`、`..` 之类）时回**空串** ——
    /// 宁可不在盘上缓存，也不要拼出一个能写到别的目录去的路径。这个 id 是从接口正文里来的，
    /// 按"外部输入"对待（来源前缀是我们自己加的，但 project_id 那一半不是）。
    ///
    /// NOTE: 后缀是 `.icon` 不是 `.png`。这一格两种字节都会落进来（先要 webp 那条成功的
    /// 话存的就是 webp），叫 `.png` 就是一句假话 —— 而这个假话会害到下一个「看见
    /// .png 就当 PNG 去读」的人。**认格式一律按内容**（ImageDecode::decode）。
    static QString iconCachePath(const QString &cacheId);

    /// 读盘上那份图标缓存。
    /// @param bytesOut 命中时写入文件内容（没命中不动它）。
    /// @return 文件在不在。**在就一定能用**：这里没有"够不够新"这一说了（见 requestIcon）。
    static bool loadIconFromCache(const QString &cacheId, QByteArray *bytesOut);

    /// 把一份刚下回来的（能解码成图的）图标字节写到盘上。
    /// best-effort：写不进去就算了，**不是错误**（这次照样把图发给界面）。
    static bool saveIconToCache(const QString &cacheId, const QByteArray &bytes);

    /// 把盘上那份图标缓存删掉（读到坏字节时用，见 requestIcon）。**凭据一起删** ——
    /// 光删图不删凭据的话，下一次会拿着一条"对应另一份字节"的 ETag 去问，主机答 304，
    /// 于是这一格永远也画不出东西来。
    static void dropIconFromCache(const QString &cacheId);

    /// 除了图标之外**每一条请求**共用的那个构造：`User-Agent: WTFML` + 额外头
    /// + 15 秒（`HttpClient::kDefaultTimeoutMs`，唯一出处）。
    ///
    /// NOTE: 这 15 秒现在是**排队 + 传输**的整项预算（这一发走 `RequestBudget::ControlGet`，
    /// 它按剩余时间设 Qt 自己的 transferTimeout；这里设的那一格会被它覆盖成同一个量级）。
    /// 这里仍然写着它，是因为这张真值表（哪一发是几秒）在 ZB 那一节被直接断言着。
    ///
    /// @param headers 额外要带的（现在只有 CurseForge 的 `x-api-key`，见 `apiKeyHeaders()`
    /// 那个匿名命名空间里的唯一出处）。空表 = 一个头都不加。
    /// @param contentType 有正文的那一发（CurseForge 的 `POST /mods`）**必须**给的
    /// `Content-Type`；没有正文的 GET 传空。
    ///
    /// NOTE: 同一个正文的四条路里另外三条（`InstanceIconLookup` / `VersionManager` /
    /// `ModpackImport`）都走 `HttpClient::send(..., "application/json", ...)`，
    /// 只有走 QNAM 的这一条漏了。放在这里（而不是 .cpp 的匿名命名空间里）是给
    /// build/verify 钉的 —— 它是**纯的**：造一个 QNetworkRequest 再把头发读回来，
    /// 一个网络请求都不发（验证工程里那张"哪一发带哪个头"的表就是这么钉的）。
    static QNetworkRequest resourceRequest(
        const QString &url, const QList<QPair<QByteArray, QByteArray>> &headers = {},
        const QByteArray &contentType = QByteArray());

    /// 图标那一发用的请求：8 秒（kIconTimeoutMs，比 kDefaultTimeoutMs 短），
    /// **可能带**"变了没有"那两个头。这 8 秒同样是排队 + 传输的整项预算（见上面那条 NOTE）。
    ///
    /// 【空的那条一律不发】
    ///
    /// 一个空的条件头（`If-None-Match:` 后面什么都没有）不是"我没有凭据"，它是一句语法上
    /// 不成形的话，主机怎么答都不好说。所以两条凭据各自判："有这条才发这个头"，两条都空
    /// = 一个条件头都不发 = 老老实实要整张（也就是盘上没有那份、或者主机上次没给凭据）。
    ///
    /// NOTE: 它放在这里（而不是 .cpp 里那个匿名命名空间）是给 build/verify 看的 —— 上面
    /// 那张真值表在 ZB 那一节逐条钉着。这个函数是**纯的**：造一个 QNetworkRequest
    /// 再把头发读回来，一个网络请求都不发。
    static QNetworkRequest iconRequest(const QString &url, const IconValidators &validators);

    // ---------- "别一张张干等"那道闸 ----------

    /// 连着这么多张图标都倒在**主机级**的失败上就不再一张张问了（见 m_iconHostFailures）。
    static constexpr int kIconGiveUpAfter = 3;

    /// 这一发失败算不算"主机级"（= 这一路整条不通，而不是"这张图有问题"）？
    ///
    /// 404 是"这个资源没有图标"、429 是"你问得太勤了" —— 两条都说明**主机是活的**，
    /// 不算。别的（0 = 没连上/超时、5xx）= 整条路不通。
    static bool isHostLevelIconFailure(int httpStatus);

    /// 掐掉所有还在飞的请求（窗口关了、或者要重来一遍时用）。
    void abortAll();

    /// 一页取多少条。**没有"加载更多"**：这一轮就是搜一页出来给用户挑，翻页还没做
    /// （要做得连 offset 和"结果到底有没有下一页"一起做，不是只把数字改大）。
    ///
    /// NOTE: 放在 signals 段**外面**（这里、而不是下面）—— 那一整段是给 moc 看的，
    /// 里面放一个数据成员 moc 会直接报 `Not a signal or slot declaration`。
    static constexpr int kSearchPageSize = 20;

signals:
    /// 搜索回来了（成功和失败都发）。
    void searchFinished(const QString &requestKey, const ResourceCatalog::SearchResult &result);

    /// 版本列表回来了。problem 非空 = 没取到；problem 空 + versions 空 = 问到了、就是没有。
    void modrinthVersionFinished(const QString &requestKey, const QString &projectId,
                                const ResourceCatalog::VersionEntry &version, const QString &problem);
    void curseForgeVersionFinished(const QString &requestKey, const QString &projectId,
                                  const ResourceCatalog::VersionEntry &version, const QString &problem);
    void modrinthFilesFinished(const QString &requestKey, const QHash<QString, QString> &projects,
                              const QHash<QString, ResourceCatalog::VersionEntry> &versions,
                              const QString &problem);
    void versionsFinished(const QString &requestKey,
                          const QList<ResourceCatalog::VersionEntry> &versions,
                          const QString &problem);

    /// 「这一串文件各自该换成哪一版」那一批回来了（见 fetchLatestVersions）。
    ///
    /// NOTE: 表里的键是**小写的 sha1**，认不出来的**不在表里** —— 那是"说不清"，不是
    /// "已是最新"。problem 非空 = 整批没成（**这一批一条都不许判成"已是最新"**：
    /// 429 那种"问得太勤"要是被当成"没有新版本"，用户就会以为自己的模组是最新的 ——
    /// 那比"说不清"坏得多）。
    void latestVersionsFinished(const QString &requestKey,
                                const QHash<QString, ResourceCatalog::VersionEntry> &latest,
                                const QString &problem);

    /// 批量问项目的那一发回来了（见 fetchProjects）。
    ///
    /// NOTE: 和 `versionsFinished` 那条"三条路要分得开"的规矩**不一样**：这里少几条
    /// （或者一条都没有）**不是错误** —— 作者删了项目、号本来就不对，都是常有的事，
    /// 而用户那边看到的只是"这一条的名字没取到"，用项目号顶上就是了。所以只有
    /// "整发没成"才算 problem。
    void projectsFinished(const QString &requestKey, const QList<ResourceCatalog::Hit> &hits,
                          const QString &problem);

    /// 图标到了（空 QPixmap = 这条资源没有图标，画占位）。
    ///
    /// @param iconId `Hit::iconCacheId()`（带来源前缀）—— 界面按它认那一行，见 requestIcon
    /// 那段"缓存键"。
    void iconReady(const QString &iconId, const QPixmap &icon);

private:
    /// 失败话术（**不是** 429：那条有自己唯一的一句话，见 HttpClient::throttledText）。
    /// 正文里带着接口自己那句 `description` 时把它带上 —— 实测接口报错就是这个形状，
    /// 它比我们编的"搜索失败"有用得多。
    ///
    /// @param source CurseForge 那一趟**再带一句"可以去设置里填自己的 key"**：默认那条路
    /// 是社区镜像（别人的服务），它没了、被限流了、或者官方那边改了规矩，用户手上有
    /// 一条自己能走的路 —— 这句话不给的话，界面上就只剩"搜索失败"四个字
    /// （和 VersionManager::resolveCurseForgeFiles 那条路的说法保持一致）。
    /// @param hasKey 手上有没有用户自己填的那份 CurseForge key —— 它决定那句话指向哪条出路
    /// （有 key 就是"这份可能不对/失效了"，没有就是"可以填一份").**收的是这个事实、
    /// 不是 key 本身**：这个函数是静态的（不碰成员），而且话术里一个字都不该出现 key。
    static QString failureText(QNetworkReply *reply, const QByteArray &body, const QString &what,
                               ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth,
                               bool hasKey = false);

    /// CurseForge 那一趟的接口根地址（`…/v1`）。**唯一出处是
    /// `ModpackImport::curseForgeApiBase()`** —— 这里只把手上那两条设置递进去，
    /// 不自己拼域名、也不判 key 空不空（那个判断在那边一处）。
    QString curseForgeApiBase() const;

    /// 发一发图标请求（`requestIcon` 挑完地址之后的那一步，**自己会往 m_iconPending 里记**）。
    ///
    /// @param iconId 这一格是谁（`Hit::iconCacheId()`）。
    /// @param url 这一发要的地址。
    /// @param fallbackUrl 这一发没成时再要一次的那个地址；空 = 没有下一发。
    /// @param onDiskBytes 盘上那一份的字节（空 = 盘上没有）。两个用处：① 带条件头去问
    /// "变了没有"；② 真拿回一整张时**比一下**，一模一样就不写盘、不发信号。
    /// @param validators 盘上那份的两条凭据（空 = 没有，那一发就老老实实要整张）。
    ///
    /// 【两条地址的那条路，规则都在这】
    ///
    /// 只有"接口给的那条这一台用得了"那一条路会带 fallbackUrl（见 requestIcon）。
    /// 什么时候真退回去：**这一发答话了、但那不是一张能解的图** —— 404（地址不在）、403、
    /// 或者 200 配一段解不出来的正文（本机没有 webp 编解码器时理论上不会走到这，但
    /// "理论上"不该当判据）。这一退**不计失败**：它不是"这一路不通"，是"这条地址这条格式
    /// 不行，换一条"。
    ///
    /// NOTE: **状态 0 和 5xx 不退**（`isHostLevelIconFailure`）。那两条说明整条路都不通，
    /// 换到一个同主机、同一条链子上的地址只会再耗一个 8 秒超时，还白推一格那道闸
    /// （症状是"明明只是网断了，三次之后这一页再也不问了"）。
    /// NOTE: 退回去那一发**不带 onDiskBytes / validators**（`QByteArray()` / 空的）——
    /// 换了地址就是换了资源，拿 A 的凭据去问 B 是没有意义的，老老实实要整张。
    /// NOTE: `validators` **没有默认值**（另外那两样有）：`IconValidators` 里有一个带初值
    /// 的成员（`checkedAtMs`），而在**同一个类里面**拿它当默认值是过不了编译的
    /// （"default member initializer required before the end of its enclosing class"）
    /// —— 那两处调用点（requestIcon 里）本来就都自己写清楚了要哪一份凭据，不亏。
    void startIconRequest(const QString &iconId, const QString &url, const QString &fallbackUrl,
                          const QByteArray &onDiskBytes, const IconValidators &validators);

    /// 这一发没取到新图标，该怎么回这一发：手上还有一张（内存里的）就把**它**重发一遍，
    /// 确实没有才发空 QPixmap。
    ///
    /// NOTE: 这是 iconReady 那条"每一发请求都会回一次"的契约里唯一的弯，为什么必须弯：
    /// 过期刷新那一条路上，盘上那张旧的**已经画在界面上了** —— 这时候再发一个空
    /// QPixmap 会把那一格擦成透明方块（症状是"作者换了图标、这次没问到，图标反而
    /// 没了"）。重发同一张 = 契约不破，那一格也不动。
    void answerIcon(const QString &iconId);

    /// 记一次"主机级"的图标失败，够数了就**放弃这一批**（见 m_iconGaveUp）。
    void noteIconFailure(int httpStatus);

    QNetworkAccessManager *m_net = nullptr;
    /// 每个频道手上那一发（key -> 作业）。
    ///
    /// 存的是 `RequestBudget::ControlGet` 作业，不是裸 QNetworkReply：这几发都要和别的短
    /// 请求共用四个 Control 名额（见文件头那段）。**换一发 = 删掉旧的作业** —— 还排在队里
    /// 的那一个字节都不会发；已经发出去的那一发在析构里 abort，且**不回调**。
    /// 于是"回来的时候这一发还是不是手上这个"这个问题在旧数据上就不存在了：被换掉的那一发
    /// 根本不会回调。回调里照旧先把钥匙摘掉（`m_replies.remove(requestKey)`）—— 信号发出去
    /// 之后接的人可能立刻在同一个钥匙上开新的一发，晚一步摘就等于把新的那格删了。
    QHash<QString, RequestBudget::ControlGet *> m_replies;

    /// 「这一串文件各自该换成哪一版」那一批（见 fetchLatestVersions）。**和上面那张表
    /// 不是一个形状**：那一批是"分几发出去、合并着回**一发**信号"，所以要存的是"这一批
    /// 跑到哪儿了"，而不是"这一发是谁"。
    struct LatestRun {
        quint64 generation = 0;
        /// 还差几发没回来（到 0 就发信号）。
        int pending = 0;
        /// 已经拼起来的（**小写 sha1** → 它那个项目最新的一版）。
        QHash<QString, ResourceCatalog::VersionEntry> merged;
        /// 整批的毛病（任何一发报错都算整批的）。
        QString problem;
        /// 飞出去的那几发（重来 / 析构时要掐掉）。
        ///
        /// **存 QPointer，不是裸指针**：作业跑到头会 `deleteLater()` 把自己收掉，而这条
        /// 列表里留下的那一格没人摘（回调只认 generation 和 pending）。一发已经回、别的
        /// 还在飞的时候关窗（`abortAll()`）或同一把钥匙上再开一批，就会对已经析构的对象
        /// 再 `delete` 一次 —— 堆损坏。null 的那几格表示"这一发已经自己收场了"，跳过即可。
        QList<QPointer<RequestBudget::ControlGet>> replies;
    };
    QHash<QString, LatestRun> m_latestRuns;
    /// 发批次的代号（只增不减，见 LatestRun::generation）。
    quint64 m_latestGeneration = 0;
    /// 图标：`Hit::iconCacheId()` -> 图。前面还有一层盘上的（见上面的静态函数），这里是
    /// 这一趟运行的副本，命中它连盘都不用读。
    QHash<QString, QPixmap> m_icons;
    /// 正在问的图标（同一格别问两遍）。
    QSet<QString> m_iconPending;
    /// 明确没有图标的（HTTP 404）。注意这和"这次没取到"**不是一回事**：网络失败不进这里，
    /// 下次还会再试一次。
    QSet<QString> m_iconMissing;
    /// **这一趟运行**里已经核对过的图标（见 requestIcon 那段"每次打开核对一次"）。
    /// 一道一次性的闸：同一个图标这一趟只核一遍，翻页、重搜、来回换分类都不再问第二遍 ——
    /// 用户要的是"每次打开"核一遍，不是每翻一页核一遍。
    ///
    /// NOTE: 往里记的只有**盘上有那份**的图标 —— 有那份才谈得上"核对"（要拿它的凭据去问、
    /// 拿它的字节去比）。盘上没有的那些是"第一次拿这张图"，它们成不成由 m_iconMissing
    /// 和上面那道闸管着；**记进这里反而会把重试的路堵死**：开机时没网、二十张全没拿到，
    /// 用户把代理打开再点一下刷新重搜，本该重新去要（重搜会清掉 m_iconGaveUp），
    /// 而"已经核过了"是清不掉的 —— 那一页就会永远停在占位方块上。
    QSet<QString> m_iconRevalidated;

    /// 连着几次图标请求倒在"主机级"的失败上（成功一次就清零）。够 kIconGiveUpAfter 次就
    /// 把 m_iconGaveUp 立起来。
    ///
    /// 这件事是 实测出来的：Modrinth 的 CDN 把每一张图 307 到
    /// `cdn-alt.modrinth.com`，而那个域名从国内**连不上** —— 于是二十来个图标各自把超时
    /// 耗完（六条连接轮着来），界面上的格子一个都不出。第 3 张还失败的时候就该认账：
    /// 用户要的是那一页能看，不是让启动器一张张去撞墙。盘上那些已经存下来的**照给**
    /// （它们一个请求都不用发，见 requestIcon）。
    int m_iconHostFailures = 0;
    /// 已经放下了。**不是永久状态**：下一次搜索会清掉它（用户又动了 = 网络可能刚变好，
    /// 比如他刚把代理打开）—— 这道闸只管"这一次、这一屏别干等"，不该管一整个会话。
    bool m_iconGaveUp = false;

    /// CurseForge 那两发要用的设置（见 setCurseForgeApiKey 那一段）。都是 MainWindow 从
    /// LauncherSettings 推过来的 —— 这一层不读设置（和 VersionManager 一个规矩）。
    QString m_curseForgeApiKey;
    /// **只有验证工程会写它**（`Launcher/CurseForgeApiBase`，把接口指到本机的假服务器上）。
    QString m_curseForgeApiBaseOverride;
    /// 同上，Modrinth 那一侧（`Launcher/ModrinthApiBase`）。见 setModrinthApiBaseOverride。
    QString m_modrinthApiBaseOverride;
};
