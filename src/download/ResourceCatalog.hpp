/**
 * @file ResourceCatalog.hpp
 * @brief 下载页「整合包 / 模组 / 数据包 / 资源包 / 光影」那几类资源的**纯函数**部分：
 * 「哪一类 + 搜什么」→ 请求地址 → 解析出来的结果。不联网、不碰磁盘。
 * @note 真正发请求、落图标缓存的是 ResourceStore。这一层拆出来是为了两件事：
 * ① 解析能拿真响应当夹具离线测（build/verify 里喂 json，一个字节的网络都不走）；
 * ② 地址是怎么拼出来的能单独钉住 —— 拼错了和解析错了是完全两回事，混在一起
 * 报出来的错会指向错的地方。
 *
 * 【数据源是 Modrinth，不带 key】
 *
 * 实测：`GET https://api.modrinth.com/v2/search` 不要任何鉴权，
 * 响应头带 `X-Ratelimit-Limit: 300`（每分钟 300 次）—— 一次搜索一条请求的量级够用。
 * CurseForge 那份的清单里没有下载地址、要带 key 去问接口（见 ModpackImport 里那段），
 * 所以**这一轮只做 Modrinth**，将来接 CurseForge 时这一层照着加一个来源即可。
 *
 * 【图标那件事：两条地址，一条小一条大】
 *
 * 所以这一层只做两件纯事：**把接口给的地址存下来**（Hit::iconUrl，顺手验一下它是不是
 * 我们认的那个域名）和**认出它是不是一条 webp**（isWebpUrl）。"这一台要走哪条、
 * 解不动怎么办"是 ResourceStore 的事。
 *
 * 相册/截图那部分只有 webp（连 raw_url 都是），这一轮仍然不碰 —— 用户的原话是
 * 「我只要那个程序图标，不要详情里的图片」。
 */
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace ResourceCatalog {

/// 下载页那几类能"先搜出来再下"的资源。
///
/// **顺序不重要，键才重要**：kindKey() 出来的字符串要和下载页左栏那份分类表
/// （MainWindowUI::downloadCategoryKeys()）里对应的那一条**逐字相同** —— 界面上"点的是
/// 哪一类"和"去问哪一类"靠这个字符串对起来。验证工程里有一条断言把这个等号钉住。
enum class Kind {
    Mod,          ///< 模组
    Modpack,      ///< 整合包（下回来是 .mrpack，接着走既有的导入流程）
    DataPack,     ///< 数据包（Modrinth 的 datapack）
    Shader,       ///< 光影
    ResourcePack, ///< 资源包
};

/// 这几类都在这里列一遍（界面按它建页面、验证按它循环）。
///
/// NOTE: 起顺序**照着下载页左栏那份表排**（整合包 / 模组 / 数据包 / 资源包 /
/// 光影，去掉「游戏」那条）—— 原来是一份"差不多"的顺序，跟界面对不上，读代码时要在
/// 两个文件之间来回翻。谁也没靠这个顺序干活（界面按表建、验证按表循环），所以改它
/// 不用动别处；但别再回到各排各的那种写法。
QList<Kind> allKinds();

/// API 里那一类的名字，也正是下载页分类的键：mod / modpack / datapack / shader / resourcepack。
QString kindKey(Kind kind);

/// 从下载页那份分类键认回 Kind；不是这几类（比如 "game"）时返回 false 并且不动 *out。
bool kindFromCategoryKey(const QString &key, Kind *out);

/// 这一类下回的文件装进实例的哪个子目录。
///
/// 整合包**没有**子目录（它走导入那条路，清单说往哪放就往哪放），所以回空串。
///
/// NOTE: 数据包是 `<实例>/datapacks/` —— 和三个兄弟类一样"下进实例自己的目录"。原版只认
/// 存档里那份（`<存档>/datapacks/`），所以这里的产物**不会自动生效**，得用户自己搬进
/// 某个存档（或者用 Modrinth 那类"全局数据包"的模组）。做"挑一个存档再放进去"那条路
/// 要先有存档列表（`<实例>/saves/`）和一次选择，这一轮不做 —— 但界面上那句提示不能
/// 撒谎，见 downloadSearchHints() 里数据包那一条。
QString folderForKind(Kind kind);

/// 人看的名字（「模组」「整合包」…）。界面上那几个字从这里取，免得两处各写一份。
QString kindLabel(Kind kind);

/// Modrinth 接口的根地址（`https://api.modrinth.com/v2`，**不带结尾斜杠**）。
///
/// NOTE: 唯一出处仍然是本文件 .cpp 里那个匿名命名空间的 `apiRoot()` —— 这里只是把它露
/// 出来给"导出整合包"那条反查路用（`POST /version_files`，见
/// VersionManager::lookupModrinthVersionFiles）。**别在别处再写一遍这个字符串**：
/// 哪天要换镜像（mcimirror 那个也认 `/modrinth/v2/...`），改漏一处的症状是
/// "搜索走了镜像、反查直连官方"，而两边都不会报错，只在慢网下才看得出来。
QString modrinthApiRoot();

// ---------- 来源（ 加的第二个数据源）----------
//
// 在此之前这一层**只有一个数据源**（Modrinth），所以"从哪儿搜的"这件事压根没有名字。
// 用户要「按来源搜」（要求见 downloadFilterRow 那段）之后它就得是个东西了：一个页面上
// 同时只搜一个来源，但**同一个资源在两边是两个不同的 id**，而且两边能筛的东西、
// 回来的形状、连"哪个字段是加载器"都不一样。
//
// | | Modrinth | CurseForge |
// |---|---|---|
// | 搜索 | `GET /v2/search?query=&facets=…` | `GET /v1/mods/search?gameId=432&classId=…` |
// | 筛游戏版本 | facets `versions:1.21.1` | 查询参数 `gameVersion=1.21.1` |
// | 筛加载器 | facets `categories:fabric`（**不是** `loaders:`） | `modLoaderType=1`（**数字**） |
// | 筛类型 | facets `categories:optimization` | `categoryId=6814`（**数字**） |
// | 排序 | `index=relevance` / `downloads` | `sortField=6`（**两边都只用它**，见 searchUrl） |
// | 要不要 key | 不要 | 官方那份要（默认走一台不要 key 的社区镜像，见 curseForgeApiBase） |
// | 答复根 | `{"hits":[…]}` | `{"data":[…],"pagination":{…}}` |
//
// 所以下面凡是"两边不一样"的地方都是一个**按来源分叉的函数**，不是两套平行的函数名：
// 界面上只有"来源"这一个下拉框在变，跟着它变的东西不该在调用点各写一遍。

/// 去哪儿搜。
enum class Source {
    Modrinth,   ///< 官方 v2 接口，不要 key（一直以来的那一个）
    CurseForge, ///< 官方要 key、默认走社区镜像（见 ModpackImport::curseForgeApiBase）
};

/// 两个来源都在这里列一遍。**顺序就是界面上那个下拉框的顺序**（Modrinth 在前面：
/// 它是不要 key 的那个，也是"原来就能用"的那个）。
QList<Source> allSources();

/// 稳定的键（`modrinth` / `curseforge`）。进条目的 userData、拼请求钥匙都用它。
QString sourceKey(Source source);

/// 下拉框里那一行字（`Modrinth` / `CurseForge`）—— 这两个是专有名词，不翻译。
QString sourceLabel(Source source);

/// 从键认回来；认不出来返回 false 并且不动 *out（和 kindFromCategoryKey 一个规矩）。
bool sourceFromKey(const QString &key, Source *out);

/// 这一类在 CurseForge 那边是哪个类目（**搜索必须带它** —— 它的 `/mods/search`
/// 没有"项目类型"这个概念，只有类目号）。
int curseForgeClassId(Kind kind);

// ---------- 那一行筛选项（版本 / 加载器 / 来源 / 类型）----------

/// 一个下拉框里的一项。
///
/// `key` 空串 = 「全部」（不筛）。**不是每个来源都认得同一个键**：Modrinth 那个是 facet
/// 里那个词（`optimization` / `fabric`），CurseForge 那个是**数字**（类目号 6814、
/// modLoaderType 1）—— 所以选项表是"按来源"取的，别自己拼一个键塞进去。
struct FilterOption {
    QString key;
    QString label; ///< 给用户看的（「优化」「Forge」）

    /// 这一项是不是「全部」（键是空串）。界面上那一项永远排第一。
    bool isAll() const { return key.isEmpty(); }
};

/// 「类型」那一栏的选项。**写死的一份常用分类**，不是从接口拉的（用户 选的：
/// 选项稳定、离线可测、不额外发请求）。
///
/// 【返回的表长什么样】第一项**永远是「全部」**（键空串）—— 界面上那一格直接照抄这张表，
/// 不用自己再补一项、也不用自己保证它排第一。回**空表**的情况只有一种：这一来源 × 这一类
/// 根本没有分类可选（见下），那时界面上该把整个「类型」框**藏起来**
/// （摆一个只有「全部」的框等于骗人）。
///
/// NOTE: 表里那些键**原样来自接口**（Modrinth 是 slug / CurseForge 是类目号），一个字母
/// 都不能自己编：`optimization` 写成 `optimisation` 不会报错，只是搜出来是空的。
/// 中文标签是我们起的（接口给的是英文名）。两张表**不一样长**，别按行号对齐。
QList<FilterOption> categoryOptions(Source source, Kind kind);

/// 「模组加载器」那一栏的选项（**只有模组那一页有这一栏** —— 资源包/数据包/光影/整合包
/// 谈不上加载器，要求「模组加载器只在模组那栏存在」）。
///
/// 形状和 categoryOptions 一样：第一项是「全部」，**回空表 = 这一栏别画**
/// （这两个来源的模组都有加载器可选，所以现在永远不会空 —— 留着这条是为了"哪一来源 × 哪一
/// 类没有这一栏"将来还能有一个出处，而不是让界面自己判断）。
///
/// 四个加载器两边的键不一样：Modrinth 要 `fabric`，CurseForge 要 `4`（数字）。
/// **两边顺序一样**（fabric / forge / neoforge / quilt），所以那个下拉框在两个来源之间
/// 来回切时行不会跳。
QList<FilterOption> loaderOptions(Source source);

/// 一个加载器名 → **这一家接口要的那个写法**（`versionsUrl` 拼参数时用的那一个）。
///
/// 这是"加载器名"这件事上**唯一**一处按来源翻译的地方，因为两侧的词形不是一套：
/// Modrinth 的 facet 要小写那个词（`fabric`），CurseForge 的 `modLoaderType` 要**数字**
/// （`4`）。递一个对不上的词上去**不会报错**，那边只是筛不出东西 —— 于是"装进 Fabric
/// 实例的前置"会挑出一个任意加载器的版本，症状是游戏进去就崩，而那一刻没人会想到是它。
///
/// 收的写法**两套都认**（`4` / `fabric` / `Fabric` —— 界面那两张表的键和标签都在这儿），
/// 认不出来（`OptiFine` 这种不在白名单里的、空串）**回空串 = 不筛**（和 `instanceLoaderName()`
/// 给不出名字时一个意思，见 ModDependencies::Target 那段）。
QString loaderRequestValue(const QString &loader, Source source);

// ---------- 搜索 ----------

/// 一条搜索结果。这里每个字段都是搜索接口**本来就给了**的 —— 没有为了它多发一次请求。
struct Hit {
    QString projectId; ///< 唯一 id（图标地址、版本查询都靠它）—— **只在同一个来源里唯一**
    QString slug;      ///< 网址里的那一段（`/mod/sodium` 的 sodium），出错时给用户看更认得出
    QString title;
    QString description; ///< 一行简介（界面上按一行省略号画）
    QString author;
    qint64 downloads = 0;
    QStringList categories;   ///< 原始的那一串（加载器 + 分类标签混在一起）
    QStringList gameVersions; ///< 它支持哪些 MC 版本 —— "选游戏版本"那一步就用它

    /// 这一条是从哪儿搜出来的。默认是 Modrinth（老调用点不用改），但**读的人必须看它**：
    /// 图标在哪条域名上、版本列表问哪个接口、下载地址给不给 sha1，三件事都跟着它变。
    Source source = Source::Modrinth;

    /// 加载器**原样**：接口自己说的那一串（CurseForge 的搜索响应里加载器根本不在
    /// `categories` 里 —— 它在每条 file 的 `modLoader` 数字上，解析时已经换成名字了）。
    ///
    /// NOTE: 为什么不能全靠 categories 现推（老路子 `loaders()`）：CurseForge 的
    /// categories 装的是「Performance」「Technology」那种**分类标签**，一个加载器
    /// 名字都没有。空着的时候 `loaders()` 照旧按 categories 认（Modrinth 那条路）。
    QStringList explicitLoaders;

    /// NOTE: **存的是原样那一串**，只在 isUsableIconUrl() 过不去时留空 —— 它是什么格式、
    /// 这一台解不解得动，都不该在这一层判（这一层不碰网络、也不碰解码器）。
    /// 解不动那件事的处置在 ResourceStore 里：走不通就退回 iconPngUrl()。
    QString iconUrl;

    /// 接口给的**第二条**图标地址（只有 CurseForge 有：`logo.url`，比上面那条 256px
    /// 缩略图大）。
    ///
    /// NOTE: 为什么不塞进 `iconUrl` 就完事：那两条是**主次**关系（先试小的、解不动/取不到
    /// 再退大的），而 Modrinth 那条路的"第二条"是自己按 id 拼出来的
    /// （`iconUrlForProject`）。两条路都得有"退哪去"这个答案，所以 iconPngUrl()
    /// 统一回答它 —— 这一项就是 CurseForge 那个答案的出处。
    QString iconFallbackUrl;

    /// 这个项目**最后一次更新**是什么时候（ISO 8601 串）。
    ///
    /// 两个来源都有这一栏，只是名字不同（Modrinth 是 `date_modified`、CurseForge 是
    /// `dateModified`），解析的时候都读到这一栏来 —— 界面上那句「↑ 5 天前」不需要知道
    /// 这条 hit 是从哪一家搜出来的。
    ///
    /// NOTE: 空串 = 接口没给（字段没了 / 响应被人改过）。界面上那一截就**不显示**，
    /// 不要拿它当"很久以前"编一句话出来。
    QString updatedIso;

    /// 加载器（`categories` 里属于加载器的那几个）。界面上当标签显示、以及挑版本时筛。
    ///
    /// NOTE: `explicitLoaders` 非空就**只认它**（CurseForge 那条路）—— 那边的 categories
    /// 是分类标签，按现有规则去里面找 `fabric` 会一个都找不到，症状是"点击那一下
    /// 不报错、只是加载器那道闸再也不拦了"。
    QStringList loaders() const;

    /// 兜底那个图标地址：Modrinth 是 `https://cdn.modrinth.com/data/{id}/icon.png`
    /// （**200 image/png**），CurseForge 是接口给的 `logo.url`（`iconFallbackUrl`）；
    /// 两边都没有时回空串（画占位方块）。
    /// 项目没传过图标时它是 404 —— 那不算错。
    QString iconPngUrl() const;

    /// 图标在**盘上那两级缓存**里叫什么（`<来源>-<id>`）。
    ///
    /// 为什么要带来源：两个来源的 id 各在自己那一套里唯一，`238222` 这种**纯数字 id 在
    /// Modrinth 那条 base62 里也是合法的**。不带来源的话，一旦两边撞上，症状是"这一格的
    /// 图标是另一个模组的"，而且缓存不过期 ⇒ 一直错下去。
    QString iconCacheId() const;
};

struct SearchResult {
    bool ok = false;
    QString problem; ///< 给用户看的中文话术（空 = 没问题）
    int totalHits = 0;
    int offset = 0;
    int limit = 0;
    QList<Hit> hits;
};

/// 搜索请求的地址。`limit` 由调用方定（界面上是一页 20 条，`offset` 用于"加载更多"）。
/// 四个筛子**空串一律 = 不筛**（那一段就不出现在地址里，而不是发一个空值上去 ——
/// 两个接口对空值的反应都不一样，`gameVersion=` 实测是按"没有这个参数"处理的，
/// 但那是巧合，不该靠）。
/// @param source Modrinth 那条走 facets，CurseForge 那条走查询参数（见文件上面那张表）。
/// @param category 类型（`categoryOptions` 给的键：一个 slug，或者一个类目号）。
/// @param apiBase **只给 CurseForge 用**：接口根地址（`…/v1`）。空 = 走默认那条
/// （社区镜像）—— 那件事只有一处出处（ModpackImport::curseForgeApiBase），
/// 这里不自己拼一个域名。验证工程拿它把请求指到本机的假服务器上。
QString searchUrl(Kind kind, const QString &query, int offset, int limit,
                  const QString &gameVersion = QString(), const QString &loader = QString(),
                  Source source = Source::Modrinth, const QString &category = QString(),
                  const QString &apiBase = QString());

/// 解析 Modrinth 的搜索响应。**读不懂就如实报**（`problem` 里写清缺了什么），不半懂不懂地
/// 返回空列表 —— 空列表在界面上就是"什么都没搜到"，那是另一句话。
SearchResult parseSearch(const QByteArray &json);

/// 解析 CurseForge 的搜索响应（`{"data":[…],"pagination":{…}}`）。
SearchResult parseCurseForgeSearch(const QByteArray &json);

/// 把一串游戏版本按**从旧到新**排（新的在后）。**只给 CurseForge 那条路用。**
///
/// 为什么要有它：Modrinth 那份搜索响应里 `versions` 本来就是旧的在前（有断言钉着，
/// 界面直接倒着画就是"新的在前"）；而 CurseForge 的 `latestFilesIndexes` 顺序是随机的，
/// 不排的话第二层会是"1.21.1 / 1.20.1 / 1.21 / 1.19.2"这种。**两边最后必须落到同一个
/// 方向**，否则"新的在前"这件事就变成两个来源各说各的。
///
/// 比法是**按 `.` 切成数字段比大小**（1.9 < 1.10 —— 字符串比会得到反的），段数不同时
/// 短的那个在前（1.21 < 1.21.1），数字段之外的那一段按字符串比（`1.21-pre1` 这类）。
QStringList gameVersionsOldestFirst(const QStringList &versions);

/// 这一串是不是**快照/预览版**那一类（`26.2-rc1` / `1.21.4-pre3` / `24w14a`）。
///
/// 四套形状，都是照着**版本号自己**判的（接口不给这一栏）：
/// * 新式快照 `26.2-snapshot-1`（这一套也要认：不勾「显示快照版」时，它们
/// 不能摆在那一排 chip 上，树第二层也不能多出十来行）；
/// * 尾巴上带 `-preN` / `-rcN` 的（1.21.4-pre3、26.2-rc1）—— 新老两套命名都长这样；
/// * 空格式的预览版 `1.14 Pre-Release 3` / `1.14.2 Pre-Release 4`（老 Mojang 的写法）；
/// * 老式快照号 `24w14a` / `13w41b`（两位年 + 'w' + 两位周 + 一个小写字母）。
///
/// **认不出来的都算"不是快照"**（`1.14.3 - Combat Test`、`20w14∞`、`26.2hyperland` 这些
/// 愚人节/实验版一律不算）。不猜的理由和 `releaseChannelRank` 那条一样：把一条正式版说成
/// 快照，用户会在那一排 chip 里**找不到**它（它被折进大版本了），而这件事在界面上没有
/// 任何提示。宁可多显示一个。
///
/// NOTE: **折出来的那一段也必须是版本号**（纯数字加点）。愚人节版 `1.RV-Pre1` 折出来是
/// `1.RV`，一个谁也不是的大版本 —— 摆在那一排里用户点进去什么都搜不到。所以它
/// 和 `20w14infinite` 一样，整条不算快照（见 .cpp 里 `isPlainVersionStem()`）。
bool isSnapshotGameVersion(const QString &version);

/// 快照版归到**对应的大版本**：`26.2-rc1` → `26.2`、`1.21.4-pre3` → `1.21.4`、
/// `26.2-snapshot-1` → `26.2`、`1.14 Pre-Release 3` → `1.14`。
///
/// 不是快照的**原样还回去**（`1.21.4` → `1.21.4`），认不出来的也原样还回去。老式快照号
/// 没有大版本可归（`24w14a` 前面那个版本号谁都猜不出来），也原样还回去 —— 调用方据此
/// 判断"这一条折不了"（见 gameVersionChoices 里那段：折不了的快照在不显示快照时就不摆）。
QString majorGameVersionOf(const QString &version);

/// 「选游戏版本」那一步要摆的一串（**新的在前**）。
///
/// NOTE: 它吃的是**声明表**（`Hit::gameVersions`）而不是整条 Hit —— 详情页那排 chip 勾一下
/// 「显示快照版」要重算一次，而那时候手上只有开页时存下来的这一串（不必再留一整条
/// Hit）。这份声明表是**旧的在前**（Sodium 那份实测从 1.16.3 一路排到 26.3），而这个
/// 资源"有哪些版本可下"那个接口给的**正好相反**（新的在前，见 parseVersions）。
/// 同一个"版本"两个字，两个接口的顺序不一样 —— 所以这里单独有个函数、单独钉一条
/// 断言，而不是在各个调用点上就地反向一次：就地反的话，将来谁多反一次、谁少反一次，
/// 症状都只是"顺序有点怪"，没人会去查。
///
/// `showSnapshots`（用户要的「显示快照版」勾选框，）：
/// * **false（默认那一档）**：快照版**归到对应的大版本里**（要求「把快照版本的模组
/// 放到对应的大版本里（如 26.2rc1 放在 26.2 里）」）—— 声明了 `26.2-rc1` 的资源，
/// 这一排上出现的是 `26.2` 那一颗；同一个大版本下声明过的几个快照（26.2-rc1、
/// 26.2-rc2）合成一颗。**折不了的那些（24w14a 这种没有大版本的）不摆** —— 勾选框叫
/// 「显示快照版」，不勾的时候就不该有快照版露在外面；它们照样在「全部」里看得到。
/// * **true**：每一条声明过的版本各摆一颗，原样（`26.2` 和 `26.2-rc1` 各是各的）。
QStringList gameVersionChoices(const QStringList &declaredGameVersions, bool showSnapshots);

/// 那个勾选框被拨动之后，这一排 chip 上该按哪一颗（回空串 = 「全部」）。
///
/// 为什么要有它：折与不折是**两份不一样的表**，用户现在按着的那一颗（`26.2`）在另一档里
/// 可能压根不存在（那个资源只声明过 `26.2-rc1`，没有正式版）—— 直接"表里没有就退回全部"
/// 会把用户从他正在看的那条线上甩走，而他刚刚只是勾了一下"想看看快照"。
///
/// 三步，从头到尾都是"同一件事的两种说法"：
/// ① 现在这一颗在另一档里**还在** → 就是它（正式版两边都有，绝大多数情况走这一支）；
/// ② 退回去（勾 → 不勾）：把它折一下再找（`26.2-rc1` → `26.2`）；
/// ③ 勾上（不勾 → 勾）：在这条线上挑**最新的那一条快照**（`26.2` 只有 rc 没有正式版时，
/// 勾上就该落在最新的那颗 rc 上，而不是"全部"）。
/// 都不成立才回空串。**只认 key，不认文字**（chip 的身份就是 key）。
QString gameVersionAfterToggle(const QStringList &declaredGameVersions, bool showSnapshots,
                               const QString &current);

/// 游戏版本落在哪个**大版本组**里（详情页那一排 chip 的第一层）：取版本号前两段，
/// `1.21.4` → `1.21`、`1.21` → `1.21`、`1.7.10` → `1.7`、`26.1.1` → `26.1`。
///
/// 预发布与新式快照先折到所属版本再取组：`1.21.4-pre3` → `1.21`、`26.2-rc1` → `26.2`、
/// `26.2-snapshot-1` → `26.2`。没有所属版本可归的（老式周快照 `24w14a`、`b1.7.3`、
/// 愚人节版）回**空串**，调用方把它们放进最后一组「其他」。
///
/// 详情页顶排 chip 的 key 就是这个大版本（`1.21`）；折不出的那一组用 `otherSeriesKey()`。
QString gameVersionSeriesOf(const QString &version);

/// 「其他」组的 key（不会和 `1.21` 这种纯数字大版本撞）。
QString otherSeriesKey();

/// 顶排那一排 chip 的 key 表（**新的在前**）：@p declaredGameVersions 经 `gameVersionChoices()`
/// 之后按大版本去重，折不出大版本的并成最后一个 `otherSeriesKey()`。
QStringList gameVersionSeriesChoices(const QStringList &declaredGameVersions, bool showSnapshots);

/// 兜底那个图标地址（`https://cdn.modrinth.com/data/{id}/icon.png`）。
///
/// 实测更正：**它不是"每个项目都有"的那一条**，只是**老项目的别名**。
/// 拿用户真实 `mods/` 里 20 个模组一个一个要过：**11 个回 404**（FerriteCore / Fabric
/// Language Kotlin / Fabric Permissions API / Carpet / ArmorQuickSwap / Fox Friend …），
/// 9 个回 200（Sodium / Fabric API / Jade / cloth-config / YACL / Iris …）。分界是
/// "这个项目的图标当年就是按 `icon.png` 这个名字存的" —— **在那之后传的图标一律不在这条
/// 路上**（它们存在 `data/<id>/<哈希>.png` 或 `…/<哈希>_96.webp` 下面，只有接口的
/// `icon_url` 说得出来，见 projectsRequestUrl 那段）。
/// 所以它只配当**最后一档**（接口那条没给、或者这台机器解不了 webp 时），不能当成一条
/// "自己拼得出来"的正常地址 —— 老注释那句「这个地址实测回 200 image/png」是在 Sodium
/// 那一张上量的，它恰好有那个别名。
/// 项目没上传图标时回 404，那不算错 —— 取不到就画占位方块。
QString iconUrlForProject(const QString &projectId);

/// 批量问"这几个项目号各自的图标地址"：`GET {apiBase}/projects?ids=["a","b"]`。
///
/// 为什么非问不可：真正那条图标地址在**带哈希的文件名**底下，除了接口没人说得出来
/// （见上面 `iconUrlForProject()` 那段）。`InstanceIconLookup` 认出来的那些文件手上只有
/// 项目号，少了这一发就只能退到那条"老项目才有"的别名上去 —— 症状是"文件管理里的模组
/// 绝大多数只有占位拼图"，而且**屏幕上一点异常都没有**（那一发 404 被当成"这个项目没传过
/// 图标"，那本来是完全正常的一件事）。
///
/// @param apiBase 接口根地址（`https://api.modrinth.com/v2`）。**空 = 官方那一份**
/// （唯一出处仍是 `apiRoot()`）—— 和 `InstanceIconLookup` / `VersionManager`
/// 里那个 override 是同一条规矩。
QString projectsRequestUrl(const QStringList &projectIds, const QString &apiBase = QString());

/// 解析上面那一发的回复（一个**数组**）→ 项目号 → 图标地址。
///
/// NOTE: 那条地址是**外部输入**，照样要过 `isUsableIconUrl()`（见那一处）。读不懂 /
/// 根本不是数组 / 那一条没带 `icon_url` / 带的是别的域名 —— 一律当"这个项目没有
/// 图标地址"，**不报错**：一个项目没传过图标是完全正常的一件事，而这里报出来的
/// "错"没有第二个人能处置。
///
/// NOTE: 起它是 `parseProjects()` 上面薄薄一层（同一份响应，那边读的是**整个
/// 项目**）。留着的理由：图标那两处调用点（`InstanceIconLookup`）只要这一格，
/// 而"没有图标地址"和"没有这个项目"在那边是两件事（前者照样认身份）。
QHash<QString, QString> parseProjectIconUrls(const QByteArray &json);

/// `GET /projects?ids=[…]` 的回复（一个**数组**）→ 每个项目一条完整的 `Hit`。
///
/// 补。要求：「除了模组以外的几个都是点击模组名称自动进入下载界面
/// （前提是匹配上了）」—— 文件管理那几页认出来的资源要能**点名字进它自己的那一页**
/// （见 MainWindow::openInstanceFileResource），而资源详情页的头卡要的是名字 / slug /
/// 简介 / 支持的游戏版本 / 下载量 —— 这几样**只有项目对象里有**（`/version_files` 那一发
/// 是"版本"的回复，`/fingerprints` 那一发只有号）。好消息是这一发**本来就在发**
/// （图标地址非问它不可），所以这是"同一份字节多读几栏"，不多一个请求。
///
/// 每一条填：`projectId`（`id`）、`slug`、`title`、`description`、`downloads`、
/// `updatedIso`、`categories`、`gameVersions`、`loaders`（进 `explicitLoaders`）、
/// `iconUrl`（过 `isUsableIconUrl()`，见上）。
///
/// NOTE: 这一份是**项目**的回复，栏名和搜索那份（`parseSearch`）不一样：项目号在 `id`
/// 里（搜索是 `project_id`）、游戏版本在 `game_versions` 里（搜索是 `versions`）、
/// 更新时间在 `updated` 里（搜索是 `date_modified`）。照搜索那份的栏名读会**一条都
/// 读不出来**，而屏幕上的样子是"认出来了、但没有详情"（名字空着），不报错。
/// NOTE: 读不懂 / 根本不是数组 / 那一条没带 `id`：跳过。回空表 = 这一趟没有详情，
/// 那不是错误（调用方手上还有"这是哪个项目"，见 `Found::hit` 那段）。
/// NOTE: **不排序**（游戏版本照接口给的顺序）—— 和 `parseSearch` 同一条规矩，摆到界面上
/// 之前那一排 chip 自己会过 `gameVersionChoices()`。
QList<Hit> parseProjects(const QByteArray &json);

/// `POST /mods` 的回复（`{"data":[…]}`）→ 每个项目一条完整的 `Hit`。
///
/// 那一家"按 id 问项目"（`ModpackImport::curseForgeModsRequestBody()`）和"搜"回的是
/// **同一个对象形状**（`data[]` 里那些字段一个不差），所以这里直接转手给
/// `parseCurseForgeSearch()`：抄第二份解析的代价是那边的字段名一改（比如
/// 给 `authors` 补的那个空数组兜底），这一条路上就悄悄少一截。
///
/// NOTE: 分页那一栏（`pagination`）在这一份响应里没有 ⇒ `SearchResult` 里那几个数是 0，
/// 调用方要的是 `hits`。出错的形状（没有 `data`）回**空表** —— 和 `parseProjects()`
/// 同一档，调用方那时手上只剩"这是哪个项目"。
QList<Hit> parseCurseForgeMods(const QByteArray &json);

/// 这条地址是不是一条 webp（**按路径后缀判**，不联网）。整条 URL 都空时是 false。
///
/// 为什么要有它：webp 那条只在"这台机器解得动"时才值得要（见 ImageDecode::webpAvailable），
/// 而"解得动"和"这一条是不是 webp"是两件事 —— 接口有时直接给 `icon.png`，那时两条地址
/// 是同一个东西，再去"退回"一次就是白发一个请求。
bool isWebpUrl(const QString &url);

/// 接口给的图标地址能不能拿来发请求。
///
/// NOTE: 这是**外部输入**（从接口正文里读的），所以只放行 `https` + **这一家自己的域名**，
/// 别的域名、别的协议、以及空串一律当"没给"（那一步退回自己拼的 iconPngUrl()）。
/// 理由和 ResourceStore 里 isSafeFileStem() 那段一样：这个字符串的下一个用途是
/// "拿去发一个请求"，是整条链上最不该由别人说了算的地方。顺带也挡住
/// `cdn.modrinth.com.别的什么.com` 这种看着像的域名（比的是 host 全等）。
/// @param source 两个来源的图在不同域名上：Modrinth 是 `cdn.modrinth.com`，
/// CurseForge 是 `media.forgecdn.net`（实测搜索响应里的 `logo.url` /
/// `logo.thumbnailUrl` 都在这个域下）。**不是"多放行一个域"**：CurseForge
/// 那条路上收到一条 cdn.modrinth.com 的地址是不该发生的事，就当没给。
bool isUsableIconUrl(const QString &url, Source source = Source::Modrinth);

/// 下载量那串字（`2.3 亿` / `1234 万` / `5678`）。
/// 这是个纯函数是有缘故的：它是**唯一**一处决定"多少万以上换个单位"的地方，
/// 界面上三处都用它，钉成断言才不会三处各说各的。
QString formatDownloads(qint64 downloads);

// ---------- 版本 ----------

/// 清单里要下的那个文件。
struct FileEntry {
    QString url;
    QString filename;
    qint64 size = -1;
    QString sha1;
    bool primary = false; ///< 主文件（一个版本里通常只有一个；界面默认下它）

    /// 这一条**下不了**时给用户的那句话（空 = 下得了）。
    ///
    /// 平台明确标记不可用，或者无法获得安全下载地址时填写。
    /// 空 downloadUrl 本身不证明作者禁止第三方分发，不能据此归因。
    QString unavailableNote;
};

/// 前置模组（这一轮只读出来，不出按钮，见 requiredDependenciesOf）。
struct Dependency {
    QString projectId;
    QString versionId;
    QString type; ///< required / optional / incompatible / embedded

    /// "必须有它才能跑"那一类。
    bool required() const { return type == QStringLiteral("required"); }
};

struct VersionEntry {
    QString id;
    QString versionNumber;
    QString name;
    QString versionType; ///< release / beta / alpha
    QString datePublished;
    QStringList gameVersions;
    QStringList loaders;
    QList<FileEntry> files;
    QList<Dependency> dependencies;

    /// 这一版自己被下过多少次。
    ///
    /// 两个来源的名字不一样（Modrinth 那份版本对象里是 `downloads`、CurseForge 那条文件是
    /// `downloadCount`），解析时都读到这一栏 —— 界面上那句「下载 113 万次」不必按来源分叉。
    ///
    /// NOTE: **0 = 接口没给这一栏**，不是"没人下过"。界面上 0 就把那一截省掉：
    /// 真写成「下载 0 次」看着像这一版有问题。
    qint64 downloads = 0;


    /// 界面上默认要下的那个文件（`primary` 那条；一个都没有时回第一条；空清单回 nullptr）。
    const FileEntry *primaryFile() const;
    bool dependencyMetadataKnown = false; // Missing data is not an empty dependency list.
};

// ---------- 资源详情页要用的那几件（都是纯函数，离线钉得住）----------

/// 把一个 ISO 时间串说成「14 天前」这种话。
///
/// 分档：不到一分钟「刚刚」、不到一小时「N 分钟前」、不到一天「N 小时前」、不到一个月
/// 「N 天前」、不到一年「N 个月前」、再往上「N 年前」。
///
/// **空串 / 读不懂 / 是将来时间都回空串**：调用点把空串理解成"这一截不显示"。将来时间
/// 也回空串是刻意的 —— 那说明这台机器的钟或者接口的时间戳有一个是错的，说「-3 天前」
/// 比不说更让人困惑。
QString relativeTimeText(const QString &isoTime);

/// 同上，"现在"由调用方给（验证工程拿它钉那几档的边界 —— 真跑的话每一条都会随
/// 当下时间漂移，只有把 now 钉死才测得准）。
QString relativeTimeText(const QString &isoTime, const QDateTime &now);

/// 发布渠道那两个字：`release` 回**空串**（正式版什么都不写，界面上就少一截）、
/// `beta` →「测试版」、`alpha` →「早期测试版」。认不出来的也回空串（**不猜**）。
///
/// 为什么正式版回空串而不是「正式版」：一屏里绝大多数是正式版，人人都有那三个字的时候
/// 它就不是信息了，而屏幕左边那个字母方块已经把渠道说了一遍。
QString releaseChannelNote(const QString &versionType);

/// 发布渠道落在**哪一档**：`release` → 0、`beta` → 1、`alpha` → 2。
///
/// 详情页那个版本行左边的字母方块按它取色（`ResourceDetailMarks::channelColor`：0 绿 /
/// 1 橙 / 2 红）。那三个数字是**次序**不是枚举值，两处必须说的是同一件事。
///
/// 认不出来（空串 / 没见过的词 / CurseForge 那边 `releaseType` 是 0 没写）回 **-1** ——
/// 不是 0。回 0 等于把一条不知道是什么渠道的版本画成绿色的"正式版"，那是把"不知道"
/// 说成了"很稳"；-1 在界面上是一块**灰的、不写字母**的方块（见那边的 `channelColor`）。
int releaseChannelRank(const QString &versionType);

/// 加载器名（接口给的小写）→ 界面上那个显示名（`Fabric` / `Forge` / `NeoForge` / `Quilt`）。
/// **认不出来就原样还回去** —— 显示一个 `optifine` 也比显示空串强（空串在界面上是
/// 「这个版本没有加载器」，那是另一件事）。
QString loaderDisplayName(const QString &loader);

/// 版本列表里那一行的小字：
/// 「Fabric, sodium-fabric-0.9.2+mc26.2, 下载 113 万次, 更新于 14 天前, 测试版」。
///
/// 哪几截在、哪几截不在全看数据：没有文件名就不写、下载量是 0（= 接口没给）不写、
/// 时间读不懂不写、正式版不写渠道。**每一截都在这里决定**，界面上那一行只负责画。
QString versionRowSubText(const VersionEntry &version);

/// 按游戏版本筛一份清单（详情页中间那一排 chip，每一颗就是一次这个）。
///
/// 看的是 `VersionEntry::gameVersions`（这一版**声明过**的每一个游戏版本），**不是**
/// "它落在哪个组里"（`groupVersions` 那个归属）。一条同时支持 1.20.1 / 1.20.2 的版本
/// 落在 1.20.2 那一组，但选了 1.20.1 那一格时它照样该在。两件事在详情页里是分开的两步
/// （先筛、再分组），别合并 —— 合并了之后"按 1.20.1 筛"会漏掉一半它本来支持的版本，
/// 而界面上看起来只是"这个模组少列了几版"。
///
/// `gameVersion` 空串 = 不筛（「全部」那一颗，原样还回去）。
///
/// `showSnapshots` 必须和 `gameVersionChoices` 收到的是**同一个值**（就是那个勾选框）：
/// 不勾时这一排上的 `26.2` 那一颗代表的是"26.2 这条线"（含它的 rc / pre 快照），所以
/// 声明了 `26.2-rc1` 的版本也要算在 `26.2` 里；勾上时 `26.2` 就只是 `26.2`，
/// `26.2-rc1` 得去点它自己那一颗。两处传得不一样的话症状是"点了 26.2 少了几个版本"，
/// 而那一排 chip 本身看着是对的。
QList<VersionEntry> versionsForGameVersion(const QList<VersionEntry> &versions,
                                           const QString &gameVersion, bool showSnapshots);

/// 按大版本筛（详情页顶排 chip 现在筛的是它）：保留声明过**该大版本任一细版本**的那些，
/// 同样看 `gameVersions` 而不是它落在哪个组里。@p series 空串 = 不筛；`otherSeriesKey()` =
/// 声明里有折不出大版本的版本（旧测试版等）。不勾「显示快照版」时折不出大版本的周快照
/// 不参与匹配（和 `gameVersionChoices()` 一致，否则「其他」会圈进只声明了 `24w14a` 的版本）。
QList<VersionEntry> versionsForSeries(const QList<VersionEntry> &versions, const QString &series,
                                      bool showSnapshots);

/// 详情页头卡上那行元信息：「Fabric / NeoForge · 下载 2.31 亿次 · 更新于 5 天前 · Modrinth」。
///
/// 四截各缺各的：一个加载器都没有（数据包那类）不写第一截、下载量 0（接口没给）不写、
/// 时间空/读不懂不写；**来源那一截总在**（这一行是"这是哪一家来的"，图标域名和版本列表
/// 都跟着它走，用户要拿它对照网页）。
///
/// 用的字和版本列表那一行**是同一套**（`formatDownloads` / `relativeTimeText` /
/// `loaderDisplayName` / `sourceLabel`）—— 同一页上同一个东西两种说法最扎眼。
QString hitMetaLine(const Hit &hit);

/// 版本列表怎么分组：**一组 = 一个游戏版本 + 一个加载器**（就是详情页里那个
/// 「Fabric 26.2」的组头）。
///
/// 一条版本只进**一个**组：进它自己声明的**最新**那个游戏版本、和它的**主加载器**
/// （`loaders` 里按 fabric / forge / neoforge / quilt 的次序取第一个；一个都没有 = 通用）。
/// 之所以不"每个声明过的游戏版本各进一次"：那样同一条会在一屏里出现好几遍，
/// 用户数不清自己到底有几个版本。
///
/// NOTE: 所以**筛某个游戏版本要看 `VersionEntry::gameVersions`**（它声明过的每一个），
/// 不是看它落在哪个组里 —— 一条同时支持 1.20.1 和 1.20.2 的版本，落在 1.20.2 那一组，
/// 但选了 1.20.1 那一格时它照样该在。这两件事在详情页里是分开的两步
/// （先按 chip 筛、再分组），别合并。
///
/// 组的次序：游戏版本**新的在前**，同一个游戏版本里按加载器的固定次序
/// （fabric / forge / neoforge / quilt），通用那一组排最后。组**里面**的顺序原样保留
/// （接口给的是新的在前，见 parseVersions）。
///
/// `preferredLoader`（ 加，**小写**，空 = 没人在乎）：「把**这个**加载器的
/// 那些组排在前面」。详情页从某个实例点进来时用实例自己的加载器 —— 要求「对应的
/// 模组加载器的版本放在前面」。**游戏版本那一条优先于它**（组序的第一关键字还是游戏
/// 版本的新旧，这一位只在同一个游戏版本里改次序）：不然选了 1.21.1 那一 chip 之后，
/// 一个支持 1.21.1 的老 Fabric 组会排到 26.2 的 NeoForge 组前面。
///
/// 词形必须是**小写**那一套（`VersionGroup::loader` 的写法）。`instanceModLoader()` 回
/// 的是显示名「Fabric」/「NeoForge」—— 直接递进来会**一个字都比不上**，症状是这一位
/// 永久不生效而屏幕上一点异常都没有（见 MainWindow 里那个换算）。
///
/// NOTE: 认不出来的加载器（`loaderOrderIndex()` 那把尺子之外的）照旧排到最后 ——
/// 这一位只把**点名的那一个**提上来，不是"把它挪到最后"。点名的那个又不认识时
/// 整份次序和没传一样。
///
/// `showSnapshots`（和那一排 chip 收到的是同一个值）：不勾时组头写的那个游戏版本也
/// **归到大版本** —— 一条最新只声明到 `26.2-rc1` 的版本，组头写「Fabric 26.2」，而不是
/// 「Fabric 26.2-rc1」。要求是「把快照版本的模组放到对应的大版本里」，而分组就是
/// "这个模组落在哪个大版本下"这件事的出处；只有这一排 chip 折了、组头不折的话，
/// 用户点 `26.2` 进去看见的是「Fabric 26.2-rc1」那一组，两处各说各的。
struct VersionGroup {
    QString gameVersion; ///< 组头里那个游戏版本；一条都没有时是空串
    QString loader;      ///< 组头里那个加载器（小写，空 = 通用）
    QList<VersionEntry> versions;

    /// 组头那行字：「Fabric 26.2」/「通用 1.21.1」/（两个都没有时）「其它版本」。
    QString title() const;
};

QList<VersionGroup> groupVersions(const QList<VersionEntry> &versions, bool showSnapshots,
                                  const QString &preferredLoader = QString());

/// 这个项目在**它自己那一家**的网页地址（详情页那颗「转到 Modrinth / CurseForge」用）。
///
/// 两家的网址形状不一样：Modrinth 是 `modrinth.com/<类>/<slug>`、CurseForge 是
/// `curseforge.com/minecraft/<那家的类名>/<slug>`，两张类名表都在这里（**类名是网址的
/// 一部分，拼错了回 404 而不报错**，所以只此一处）。拼不出来（slug 空）回空串，
/// 那颗按钮就不画。
///
/// NOTE: 为什么收 kind 而不是整条 Hit：类名这一段跟着"这一条是从哪一栏搜出来的"走，
/// 而栏位不在 Hit 里（它在树条目的 resourceKindRole 上）。
QString projectPageUrl(Source source, Kind kind, const QString &slug);

/// MC 百科按名字搜的那条地址（详情页那颗「转到 MC 百科」用）。
///
/// 那一家**没有按项目 id 直接跳的路**（它是另一套号，和对面的 id 对不上），所以只能拿
/// 名字去搜 —— 这是"跳到那儿附近"而不是"跳到那一页"，名字里带中文/空格都照原样转义。
QString mcmodSearchUrl(const QString &name);

/// 某个资源在某个 MC 版本（+可选加载器）下的版本列表地址。
/// `gameVersion` / `loader` 为空 = 不筛（界面在没选游戏版本时不该问这条，见调用点注释）。
///
/// `apiBase` **两个来源都认**（空 = 走官方那一份）：它是验证工程那个口子
/// （`Launcher/ModrinthApiBase` / `Launcher/CurseForgeApiBase`）。 之前这里只认
/// CurseForge，Modrinth 那条写死了官方地址 —— 而那个口子**不生效时一声不响**，于是"全程
/// 离线"的验证工程其实一直打到了真接口上（见 ResourceCatalog.cpp 里那段 ★）。
QString versionsUrl(const QString &projectId, const QString &gameVersion,
                    const QString &loader = QString(),
                    Source source = Source::Modrinth, const QString &apiBase = QString());

/// 「这一串文件各自该换成哪一版」—— `POST {base}/version_files/update`。
///
/// 这个接口就是为它准备的：**一次带一串 sha1**，回"每个 sha1 它那个项目在
/// `gameVersion` + `loader` 下**最新**的那一版"。295 个模组 = 3 发（每发 100 个哈希，
/// 见 `kLookupChunk` 那个数）。
QString versionFilesUpdateUrl(const QString &apiBase = QString());

/// 上面那一发的请求体。`sha1s` 全空（或全是空白）时返回空 `QByteArray` —— 那一发
/// **不该发出去**（空表的回复必然是空表）。
///
/// `loader` 递**显示名**（「Fabric」）或者键（`fabric`）都行 —— 翻那一步在**里面**
/// （`loaderRequestValue()`，和 `versionsUrl()` 那一句逐字同一个出处），翻不出来就**不筛**
/// （那一格整个不出现）。★ 别指望调用方先翻好：`dependencyTargetFor()` 给的是显示名那一套
/// （见那个函数），而这一发只认小写那个词 —— 递错的症状是"一个哈希都匹配不上"，不是报错。
QByteArray versionFilesUpdateRequestBody(const QStringList &sha1s, const QString &gameVersion,
                                         const QString &loader);

/// 解析 `POST /version_files/update` 的答复。**键是小写的 sha1**（调用方拿本地那个文件的
/// sha1 去查，本地算出来的一律是小写）。
///
/// NOTE: 读不懂（不是对象）时返回空表 —— 和 `parseVersions()` 那边"读不懂就是空表"同一条
/// 规矩；"接口回了一句读不懂的话"这件事由调用方（`ResourceStore`）那句 problem 说。
QHash<QString, VersionEntry> parseVersionFilesUpdate(const QByteArray &json);

// ---- 请求的"钥匙" ----
//
// 一次异步请求要认得出回来的是不是**我现在等的那一发**：用户连敲两次回车，第一发回来得比
// 第二发晚的话，晚到的那份会把新的那份盖掉（屏幕上显示的是上一个搜索词的结果，而且不报错）。
// 所以每一发都带一把钥匙（见 ResourceStore），这两把是拼钥匙的**唯一出处**。

/// 一次搜索的钥匙（按分类 + 来源分，一个页面上同时只有一发）。
///
/// 带上来源是**必须**的：同一个页面上用户可能刚点完 Modrinth 又切成 CurseForge，
/// 钥匙不带来源的话，前一趟回来的结果会被当成"我现在等的那一发"填进树里 ——
/// 界面上就是"切到 CurseForge，显示的还是 Modrinth 那批"。
QString searchRequestKey(Kind kind, Source source = Source::Modrinth);

/// 一次"问某个资源的版本列表"的钥匙。带上游戏版本、加载器和来源，因为它们都会改变
/// 问出来的东西（两个来源的 id 空间还可能撞上，见 Hit::iconCacheId 那段）。
QString versionsRequestKey(const QString &projectId, const QString &gameVersion,
                           const QString &loader = QString(),
                           Source source = Source::Modrinth);

/// 一次"批量问这几个项目叫什么"的钥匙（ 加，前置模组和更新检测都要名字）。
///
/// id **先排序再拼**：同一批号不管调用方是按什么顺序攒的，都是同一把钥匙。不排的话，
/// 同一批号换个顺序就成了"另一发"，前一趟回来的结果被当成过期的丢掉 —— 症状是
/// "这一批名字永远填不上"，而屏幕上看不出任何异常。
QString projectsRequestKey(const QStringList &projectIds, Source source = Source::Modrinth);

/// 拿两份真响应逐条核过 `date_published`（各 12 条和 22 条，都严格递减），
/// 所以界面直接从上往下画就是了，**不要再反向遍历一次**。这个坑值得写下来：一开始
/// 按"接口大概是从旧到新"的直觉写了个反向，结果列表头条是最老的那个版本，而且不报错。
///
/// 这一层刻意不自己排 —— 写一个"1.9 和 1.10 谁新"的比较器，错了同样不会有任何东西报警，
/// 而接口本来就是按发布时间排好的。
///
/// 【两个来源的形状：按根判，调用方不用管】
/// 响应根是**数组** = Modrinth（`/project/{id}/version`）；是**对象里带 `data`** =
/// CurseForge（`/mods/{id}/files`）。两边回来的是同一样东西（"这个资源有哪些版本可下"），
/// 所以分岔放在这个函数里面 —— 让调用方先判来源再挑一个解析器，等于把"哪个接口回哪种形状"
/// 这件事抄到调用点上，抄错了的症状是"CurseForge 那一页永远是空的"。
///
/// CurseForge 那一边有三处要翻译（都是实测的形状差异，见 parseCurseForgeVersions）：
/// * 那边的 `data[]` **一条就是一个文件**（不是"一个版本里带几个文件"），所以一条直接
/// 变成一个 VersionEntry，`files` 里只有它自己；
/// * `gameVersions` 里混着 `Client` / `Server` / `Forge` 这种**不是游戏版本**的东西
/// （Modrinth 那边是干净的），要挑出来：游戏版本那几项长得像版本号（以数字开头），
/// 加载器那几项顺带翻成小写（`Forge` → `forge`，加载器那道闸比的是小写）；
/// * `releaseType` 是个**数字**（1 release / 2 beta / 3 alpha），翻成和 Modrinth 一样的词。
QList<VersionEntry> parseVersions(const QByteArray &json);

/// 解析 CurseForge 的版本（文件）列表（`{"data":[…]}`）。**根不是数组时回空表** ——
/// 那一发没成的话形状根本不对（错误正文是个对象），`parseVersions` 就是把这一条当"这份不是
/// 版本清单"处理的。单独有个函数是为了能拿真响应当夹具离线钉住那三处翻译（见 parseVersions）。
QList<VersionEntry> parseCurseForgeVersions(const QByteArray &json);

// ---------- 前置模组（这一轮只留接口）----------

/// 某一条版本"启动前必须一起下"的那些前置，解析成待下载清单。
///
/// NOTE: 用户要求前置模组**先预留接口、不出按钮**（要求「前置模组记得预留一键下载的接口，
/// 先不急」）。所以这里只把"要下谁"算出来，**没有**任何调用方真去下它。
/// 下一步接的时候大致是：拿到这张清单 → 逐个问 `versionsUrl(projectId, ...)` →
/// 取每个的 primaryFile() 一起塞进下载批次。缺的要如实列给用户（有的前置作者
/// 删过版本，问不到东西），别默默少下。
QList<Dependency> requiredDependenciesOf(const QList<VersionEntry> &versions,
                                         const QString &versionId);

// ---------- 一条 Hit 的盘上往返 ----------

/// 把一条 `Hit` 写成 JSON 对象 / 从 JSON 对象读回来。
///
/// NOTE: 这一对必须**逐字段配平** —— 加了 `Hit` 的一个字段却忘了加在这两边，症状是
/// "第二次启动之后那一栏是空的"（而不是报错）。`verify` 里有一条往返断言盯着它。
/// NOTE: 读的一侧**不认识的一律当空**（字段没了 / 被别的东西改过）：这是缓存，不是判据，
/// 缺一栏的代价是界面上少一句话，不该让整份文件作废。
QJsonObject hitToJson(const Hit &hit);
Hit hitFromJson(const QJsonObject &object);

} // namespace ResourceCatalog
