/**
 * @file ModDependencies.hpp
 * @brief 两件和"前置模组"有关的事的**纯函数**部分：
 * ① 装一个模组时，它声明要的那些前置里**还缺哪几个**、每个该去问哪条地址、
 * 问回来之后挑哪一版（用户 ：「选择模组后显示其依赖，点击下载后
 * 弹出窗口，让用户可以点击一键下载所有缺少的依赖」）；
 * ② 实例里已经装着的那些模组**有没有新版本**（用户 ：「实例设置那里
 * 做一个更新检测」）。
 *
 * 【为什么单独一个文件，而不是塞进 ResourceCatalog】
 *
 * ResourceCatalog 管的是"接口回的那份字节怎么读"（地址怎么拼、JSON 怎么拆）—— 它不认识
 * "实例"、也不认识"已经装了什么"。而这两件事的判据**都要**这两样：筛版本列表要知道目标实例
 * 的游戏版本和加载器，判"缺不缺"要知道那个实例盘上已经有哪些资源。把"实例"这个概念搬进
 * ResourceCatalog 会让那个文件里第一次出现"实例目录"（那是 AppPaths / VersionManager 的事），
 * 所以分开：这一层只做判断，不发一个请求、不读一个文件。
 *
 * 【这一层刻意不知道的事】
 *
 * · 不知道网络：它只**拼**地址（`Lookup::url` / `requestKey`），发不发、什么时候发、
 * 回来的是不是这一发（钥匙对得上没有）都是 ResourceStore 和 MainWindow 的事；
 * · 不知道界面：没有一句"要不要弹窗""按钮写什么"，只有 `*Text()` 那几个**唯一出处**
 * 的静态函数（界面拿它们写字，验证工程拿它们断言）；
 * · 不知道盘：`installedKeys` 是**调用方**从会话缓存里算好递进来的（主程序那边是
 * `m_instanceFileIdentities`，见 MainWindow 里那一段）。
 *
 * NOTE: 用户 说过「前置模组记得预留一键下载的接口，先不急」，`requiredDependenciesOf`
 * 那一版就停在"要下谁"（只有 project_id）。这一版接着往下走了一格：**要下谁、去哪儿问、
 * 挑哪一版、挑不出来怎么说** 全在这一层，界面那一层只负责"把 Plan 变成一批下载"。
 */

#pragma once

#include "ResourceCatalog.hpp"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ModDependencies {

/**
 * @brief "这些前置要装进哪个实例"—— 筛版本列表要的那两样，加上来源。
 *
 * 两样都得有：只按游戏版本筛会挑出一个 Forge 的前置装进 Fabric 实例（游戏进去就崩，
 * 而且那一刻没人会想到是刚才那个前置）；只按加载器筛会挑出一个 1.20.1 的装进 1.21 实例
 * （症状一样）。所以 `versionsUrl()` 两个参数都发。
 *
 * NOTE: `gameVersion` / `loader` 为空**不是错误**，是"这个实例认不出来"（纯原版、或者
 * PCL 那类合并实例读不出版本 json 里的加载器，见 MainWindow 里 `instanceLoaderName()`
 * 那段）。那时候只能是"不筛"—— 问出来的可能是任意一版，**界面得如实说这一条是猜的**，
 * 不能假装筛过。
 */
struct Target {
    /// 装进哪儿的 id（只用来写日志 / 认人，判据不用它）。
    QString instanceId;
    /// 游戏版本（`1.21.1`）。空 = 不筛。
    QString gameVersion;
    /// 加载器名（**小写**，`fabric` / `forge` / `neoforge` / `quilt`）。空 = 不筛。
    ///
    /// NOTE: 塞进来的那个词**不一定**是这一家接口认得的那几个（`OptiFine` 就不在 Modrinth /
    /// CurseForge 的白名单里）。这一层**不判**：它只把值原样递给 `versionsUrl()` /
    /// `versionsRequestKey()`，由 `ResourceCatalog::loaderRequestValue()` 按来源翻成
    /// 接口要的写法 —— **认不出来就回空串 = 不筛**（那边不报错，只是什么都筛不出来，
    /// 而"什么都没筛出来"和"这个资源没有能用的版本"在界面上是同一句话）。
    QString loader;
    /// 这个模组是从哪一家下来的 —— 前置也去**同一家**问（两家的项目号不是一套）。
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
    /// CurseForge 的 base（那一家的接口地址是可配的，见 curseForgeApiRoot）。Modrinth 不用。
    QString apiBase;

    /**
     * @brief "这个项目号"在**已经装了什么**那张表里叫什么。
     *
     * 就是 `Hit::iconCacheId()` 那个拼法（`<来源>-<项目号>`）。**必须和那边一模一样** ——
     * 对不上等于"装过的前置每次都当成没装、再下一遍"，而且不报错（用户看到的是"同一个模组
     * 怎么装了两份"）。那边的唯一出处是 `Hit::iconCacheId()`，这里跟着它走而不是另抄一份
     * 字符串格式：拼法一改，两边一起改。
     */
    QString installedKey(const QString &projectId) const {
        ResourceCatalog::Hit hit;
        hit.projectId = projectId;
        hit.source = source;
        return hit.iconCacheId();
    }
};

/// 这一层自己去问版本列表时要的那把钥匙（`@p consumer` 是"谁在问"）。
///
/// @param consumer 谁在问（`dependency` / `update`）。**新加一个消费方就得换个名字** ——
/// 重名等于回到上面那个坑。
QString versionsKeyFor(const QString &consumer, const QString &projectId,
                       const QString &gameVersion, const QString &loader,
                       ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth);

/// 和上面那条**同一个形状**，管的是一批**项目号**那一发（`ResourceStore::fetchProjects`：
/// "这几个号叫什么"）。`consumer` 那张名单和上面那条各算各的（一个消费方在两条路上是
/// 同一个名字，但两条路的钥匙本来就是两份不同的字符串）。
///
/// 为什么这一条也得挂消费方：`fetchProjects()` 和 `fetchVersions()` 一样
/// 是**按钥匙去重**的（同一把钥匙再来一发会把前一发 abort 掉，那一发的结果于是永远不回）。
/// 今天有两个人在问名字：「装一个模组时那几个前置叫什么」（`dependency`）和「详情页上那
/// 一行前置写什么」（`detail`）。两边问的**恰好是同一批号**时撞车的话，先发那一趟的
/// `pending` 永远减不到 0 —— 用户看到的是"前置那个框不出来"或者"那一行字一直空着"，
/// 而屏幕上一点异常都没有。
QString projectsKeyFor(const QString &consumer, const QStringList &projectIds,
                       ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth);

/// 这一版声明的必装前置（**不看装没装**、也不问名字）——"要下谁"这一层。
///
/// 它是依赖解析和安装的真值来源：只过滤 optional / incompatible / embedded
/// 和没有 projectId 的坏数据。不要在这里为了精简 UI 隐藏真实依赖，否则
/// `missingRequiredDependencies()` 也会看不见它，用户选「一起下载」时反而不会装。
///
/// 同一个项目在清单里出现两次时**只留第一条**（按项目号去重）：两发同钥匙的版本请求
/// 在 `ResourceStore` 那边第二发会把第一发掐掉、而且不回调，按条数等回信的一方
/// `pending` 永远归不了零。展示清单也读这一份，所以同一个名字不会列两遍。
QList<ResourceCatalog::Dependency> requiredDependenciesOf(
    const ResourceCatalog::VersionEntry &version);

/// 详情页「这一版要装的前置」那一行要展示的精简清单。
/// 现在只隐藏 Fabric API；这是纯展示规则，不影响缺失判定和一键安装。
QList<ResourceCatalog::Dependency> displayedRequiredDependencies(
    const ResourceCatalog::VersionEntry &version);

/// 一条"去问这个前置有哪些版本"的活。
///
/// 地址和钥匙**都在这儿拼好**（用的是 `versionsUrl()` / `versionsRequestKey()` 那两个唯一
/// 出处）—— 接的人只管发，不许自己再拼一遍：两个来源的参数名不一样（Modrinth 是 `loaders`、
/// CurseForge 是 `modLoaderType`，那一家还要数字），自己拼一定会漏掉一处。
struct Lookup {
    ResourceCatalog::Dependency dependency;
    QString url;
    QString requestKey;
};

/**
 * @brief 这个版本声明的必装前置里，目标实例**还缺哪几个**。
 *
 * 判据三样，缺一不可：
 * · `dependency.required()` —— optional / incompatible / embedded 都不下（`embedded` 是
 * 已经打进那个 jar 里的，再下一份反而是两份）；
 * · `projectId` 非空 —— 空的那一条是接口给的烂数据，拼出来的地址会问出一堆东西来；
 * · 目标实例里**没有**它 —— `installedKeys` 里有 `target.installedKey(projectId)` 就跳过。
 *
 * @param version 用户点"下载"的那一版（它自己的 `dependencies`）。
 * @param installedKeys 目标实例里已经认出来的资源（键是 `<来源>-<项目号>`，见
 * `Target::installedKey`）。
 * @param target 装进哪儿。
 * @return 还缺的那些，**顺序原样**（接口给的顺序，不排 —— 排一个"哪个更该先下"的比较器，
 * 错了不会有任何东西报警，而下载顺序本来不影响结果）。一条不缺就回空表。
 *
 * 同一项目出现两次的去重在 `requiredDependenciesOf()` 里（**唯一一处**），这里不会
 * 为同一个项目发两把同钥匙的活。
 *
 * NOTE: 回空表 ≠ "这个模组没有前置" —— 也可能是"前置都装着了"。**别拿它当那句话用**：
 * 要那个数得看 `requiredDependenciesOf()`（那边不看已装）。
 */
QList<Lookup> missingRequiredDependencies(const ResourceCatalog::VersionEntry &version,
                                          const QSet<QString> &installedKeys,
                                          const Target &target);

/// 挑出来的一个前置：哪一版、下哪个文件。
struct Resolved {
    ResourceCatalog::Dependency dependency;
    ResourceCatalog::VersionEntry version;
    /// 要下的那个文件（`version.primaryFile()` 的内容 —— 拷一份而不是存指针：
    /// `Plan` 会被存进 MainWindow 的成员里跨好几轮事件循环，`version` 是它自己那一份，
    /// 存指针就是存一个指向"某个临时表里的元素"的指针）。
    ResourceCatalog::FileEntry file;
    /// 给界面看的名字（`buildPlan` 从 titlesByProjectId 里填；那里没有就是项目号）。
    /// 下载标签和"下好了"那句话读它 —— 一行 `AANobbMI 0.6.13` 用户认不出是什么。
    QString title;
};

/**
 * @brief 从一个前置项目问回来的版本列表里挑一版。
 *
 * 两条路，**不要合并**：
 * · 接口**钉了** `versionId`（作者说了"就要这一版"）→ 必须正好是那一条。找不到 =
 * 解不开，**不许退而求其次挑最新的一条** —— 作者钉版本往往是因为新旧不兼容，
 * 悄悄换一版比"这个前置没装上"更难查（症状是那几个模组一起不生效）。
 * · 没钉（`versionId` 空 = "哪个版本都行"）→ 取**列表第一条**。这份列表接口给的就是
 * **新的在前**（`parseVersions` 那段有实测），所以这里不排序、也不按版本号比大小 ——
 * 那种比较器（"1.9 比 1.10 旧"）写错了不会有任何东西报警。
 *
 * 挑出来的那一条**还得有文件可下**：`primaryFile()` 为空、或者它的 `url` 是空串
 * （CurseForge 那边 = 作者关掉了第三方分发），都算解不开 —— 那时候界面上要说的正是
 * 文件里那句 `unavailableNote`，不是"找不到版本"。
 *
 * @param versions 问回来的版本列表（顺序 = 接口给的顺序，新的在前）。
 * @param[out] out 解开了才写。
 * @param[out] why 解不开时写一句中文（**直接能给用户看**）。可以为 nullptr。
 * @return 解开了没有。
 */
bool resolveDependency(const ResourceCatalog::Dependency &dependency,
                       const QList<ResourceCatalog::VersionEntry> &versions, Resolved *out,
                       QString *why);

/// 一批前置算下来的结果。
struct Plan {
    /// 要下的（每条一版一文件）。
    QList<Resolved> downloads;
    /// 解不开的那些，**一条一句中文**（每句都带上是哪个前置 —— 只说"有一个前置没找到"
    /// 等于没说，用户手上没有任何线索）。
    QStringList unresolved;
};

/**
 * @brief 把"问回来的答案"算成"下什么"。
 *
 * @param lookups `missingRequiredDependencies()` 发出去的那一批（**顺序照原样**，
 * Plan 里 downloads 的顺序就跟着它）。
 * @param answers 钥匙（`Lookup::requestKey`）→ 那一发的版本列表。缺席（钥匙不在里面）
 * 算**解不开**，理由写"没问到"—— 那一发失败和"它一个版本都没有"是两件事，
 * 但用户的下一步是同一件（去项目页自己看），所以分成两句话、一个字都不许含糊。
 * @param titlesByProjectId 项目号 → 名字（界面上那几句话里用的是名字）。空表也行
 * （那就是名字取不到，退成项目号 —— 列出来总比"有 2 个前置没找到"强）。
 */
Plan buildPlan(const QList<Lookup> &lookups,
               const QHash<QString, QList<ResourceCatalog::VersionEntry>> &answers,
               const QHash<QString, QString> &titlesByProjectId = {});

// ---------------------------------------------------------------------------
// 更新检测
// ---------------------------------------------------------------------------

/// 盘上一条"认得出来是哪个资源"的文件（更新检测的输入）。
struct InstalledItem {
    /// 本地那个文件的绝对路径（原样回来，只用来认人 / 写日志）。
    QString path;
    /// 它属于哪个资源（`<来源>-<项目号>`，见 `Target::installedKey`）。
    QString iconKey;
    /// 资源名（没有就是空串 —— 那时候界面上写的是文件名）。
    QString title;
    /// 本地这个文件的 **sha1**。空串 = 不知道（认不出来），这一条只能判"不知道"。
    ///
    /// 判据是它，不是"文件名像不像"：模组文件重命名是家常便饭（`xxx-1.2.jar` 和
    /// `xxx.jar` 是同一份），而**同一个文件的 sha1 换不了**。接口两条路都带它
    /// （Modrinth `files[].hashes.sha1`、CurseForge `hashes` 里 `algo == 1` 那条）。
    QString sha1;
    /// 问版本列表时**要不要带上实例的加载器**。
    ///
    /// 它就是 `kindUsesLoader()`（那支函数是这条规矩的唯一出处）在**这一条文件**上的答案，
    /// 由调用方在"反查回来、知道这个路径在哪一类目录下"那一刻填上。
    ///
    /// 为什么不是"一律带上"：那个筛一旦带上，**只对模组是对的**。同一个模组的 Fabric /
    /// Forge 两版是两条版本线（不带筛的话问回来的"最新"可能是另一个加载器的 jar，装进去
    /// 就崩），所以模组必须带；而资源包 / 光影包 / 数据包在两家接口上写的是**别的词**
    /// —— Modrinth 的资源包写 `minecraft`、光影写 `iris` / `optifine`（实测 Complementary
    /// 那条就是 `['iris','optifine']`）、数据包写 `datapack` —— 带上「Fabric」上去
    /// **不会报错、只是一个都不匹配**，于是那一类的每一条都判成"说不清"。真机实测
    /// （用户那个整合包）：14 个资源包 + 光影包一条都判不出来，报告里全是
    /// "没问到版本列表（网络或接口没成）"—— 而同一个 sha1 把 `loaders` 去掉再问，
    /// 接口**当场就答**（同一条 `curl` 实测，见 docs/FEATURES.md）。
    bool loaderApplies = true;
    ResourceCatalog::VersionEntry currentVersion;
    QString projectId;
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
};

/// 这一类资源问版本列表时，**要不要带实例的加载器**（上面那一位的语义出处）。
///
/// 只有模组（和整合包那一类 —— 它自己就带加载器）要。资源包 / 光影包 / 数据包**一个都
/// 不带**：加载器那几格里它们写的词和实例的加载器根本不是一套（见上面那段的实测）。
bool kindUsesLoader(ResourceCatalog::Kind kind);

/// 一条更新检测的结论。
enum class UpdateState {
    /// 说不清（文件 sha1 不知道 / 那一发没问回来 / 这一版在列表里找不到）。
    /// **不是"已是最新"** —— 界面上这两句话不能混。
    Unknown,
    /// 盘上这一版就是列表里最新的那一条。
    UpToDate,
    /// 列表里有比它新的（`newerCount` 条）。
    Outdated,
};

struct UpdateVerdict {
    InstalledItem item;
    UpdateState state = UpdateState::Unknown;
    /// 新的那几条里**最新的那一条**（Outdated / UpToDate 时是列表第一条；Unknown 时是空）。
    QString latestVersionId;
    QString latestVersionNumber;
    QString latestFileName;
    /// 比它新的有几条（UpToDate = 0，Unknown = -1）。
    int newerCount = -1;
    /// 说不清时那一句中文（空了就是说得清）。
    QString reason;
    ResourceCatalog::VersionEntry currentVersion;
};

/// An empty scope replaces the whole result; a partial check preserves unqueried files.
QList<UpdateVerdict> mergeScopedUpdates(const QList<UpdateVerdict> &previous,
                                      const QList<UpdateVerdict> &checked,
                                      const QSet<QString> &scope);
QString updateDifferenceText(const ResourceCatalog::VersionEntry &current,
                             const ResourceCatalog::VersionEntry &target,
                             const QHash<QString, QString> &plannedVersions = {},
                             const QHash<QString, QString> &projectNames = {});

/**
 * @brief 拿"这个资源问回来的版本列表"判盘上这一条是不是最新的。
 *
 * 判据只有一条：**这个文件的 sha1 落在列表的第几条上**。
 * · 落在第 0 条（最新的那一条）→ 已是最新；
 * · 落在第 k 条（k > 0）→ 有 k 条比它新；
 * · 一条都没落上 → 说不清。
 *
 * 【为什么会"一条都没落上"（这一档不是错误）】
 *
 * · 列表是**按目标实例筛过**的（游戏版本 + 加载器，见 `versionsUrl`），盘上那一版可能是
 * 另一个游戏版本的 —— 那不是"过期"，是"这一版不该装在这个实例里"；
 * · 作者把老版本删了（接口就不再列它）；
 * · 用户自己改过那个 jar（改过的 jar sha1 当然对不上，而它**确实**不是任何一版）。
 * 这三种的下一步都是"去项目页自己看"，所以合成一句。**不许**退化成"按版本号比大小" ——
 * 那是上面那段说的那个不会报警的比较器。
 *
 * @param versions 空的（那一发失败 / 这个项目没有版本）→ 也说不出所以然，回 Unknown。
 */
UpdateVerdict judgeUpdate(const InstalledItem &item,
                          const QList<ResourceCatalog::VersionEntry> &versions,
                          const QString &problem = QString());

/**
 * @brief 拿**批量那一发**给的"最新那一版"判盘上这一条是不是最新的。
 *
 * 和上面那个 `judgeUpdate()` 的判据**是同一条**（"盘上这一份是不是最新的那一版"），区别只在
 * 手里有什么：那一条拿的是整个版本列表（所以能说"比它新的有几条"），这一条拿的是接口直接
 * 给的**最新那一版**（`POST /version_files/update`，一次带一串 sha1 —— 见
 * `ResourceStore::fetchLatestVersions()` 那段：一页几百个模组各问一发会把 Modrinth 问到
 * 429，那一发就是为这件事加的）。
 *
 * · `latest` 有东西、而且它某个文件的 sha1 就是 `item.sha1` → **已是最新**；
 * · `latest` 有东西、对不上 → **可更新**（换成它的那一版）；
 * · `latest` 空的 → **说不清**（不是"已是最新"）：接口对"这个哈希"什么都没给 —— 那个项目
 * 在这个游戏版本 / 加载器下没有版本，或者它压根不在 Modrinth 上。
 *
 * @param problem 那一批自己的毛病（429 / 没连上 / 正文读不懂）。非空时**整批**都是
 * "说不清"，而且 `reason` 就是它 —— 这就是「无法正常获取更新」
 * 的正面修法：429 不能被吞成"没问到版本列表（网络或接口没成）"，否则用户看不见真因。
 */
UpdateVerdict judgeUpdateAgainstLatest(const InstalledItem &item,
                                       const ResourceCatalog::VersionEntry &latest,
                                       const QString &problem = QString());

/// 批量那一发的钥匙（**唯一出处** —— 拼钥匙和"那一槽认不认这一发"读的是同一个字）。
///
/// 和 `versionsKeyFor()` 同一个规矩（分隔符 `|`、消费方挂在尾部），只是**没有"哪个项目"
/// 那一格**：这一发问的是一整串 sha1，认得它的东西是"游戏版本 + 加载器 + 消费方"。
QString latestVersionsKeyFor(const QString &consumer, const QString &gameVersion,
                             const QString &loader = QString());

// ---------------------------------------------------------------------------
// 界面上的字（**唯一出处** —— 界面和验证工程都读这里，别在别处再写一遍）
// ---------------------------------------------------------------------------

/// 弹窗标题：`还有前置没装`。
QString dependencyPromptTitle();

/// 弹窗正文的**句头**：「此模组缺失必要的前置：」/「此光影包缺失必要的前置：」。
/// 起光影包也查前置（用户确认），所以句头得认这一版是哪一类。
QString dependencyPromptHead(ResourceCatalog::Kind kind);

/**
 * @brief 弹窗正文：句头 + 名单（一行一条）+「要一起下载吗？」。空表回空串
 * （**空表时不该弹这个框**，见调用点）。
 *
 * 名单怎么摆是**这一层说了算**（用户 点名的截断规则，那一整段写在 .cpp 里
 * 的 `dependencyNameLines`）：**总数 ≤ 4 全列**、**> 4 列前 3 条 + 「还有 N 个未列出…」**。
 * 所以永远看不到「还有 1 个未列出」那一行。
 *
 * @param kind 这一版是模组还是光影包（只决定句头那半句）。
 * @param names 还没装的那些的**名字**（一条一行，前面带个短横）。名字取不到时调用方
 * 递进来的是项目号（见 MainWindow 里那段）—— 总比一条都不列强。
 */
QString dependencyPromptText(ResourceCatalog::Kind kind, const QStringList &names);

/**
 * @brief 同一件事，但**还没认过这个实例装了什么**时那一句。
 *
 * 这两个形态**不能合并**：`m_instanceFileIdentities` 是"文件管理页那一趟反查"填的，
 * 没进过那一页就是空的 —— 那时候我们**不知道**用户装没装过它。说成"这几个还没装"
 * 是在替用户认下一件我们不知道的事（他可能早就装了，于是被哄着下了第二份，而 mods/ 里
 * 同一个模组两份，游戏加载哪一份是按文件名顺序定的）。
 *
 * 所以这一版在句头后面补一句"不知道你有没有"，名单照上面的规矩摆，不提"缺"。
 */
QString dependencyPromptUnknownText(ResourceCatalog::Kind kind, const QStringList &names);

/// 弹窗里那行**灰字**（「可以在设置中关闭此提示（设置 → 提示 →「检查前置模组」）。」）。
/// 用户 ：「弹窗里灰字写可以在设置里面关掉弹窗」。界面拿它喂
/// `InlineMessage::ask(...)` 的 note，验证工程拿它断言 —— 别在别处再写一遍。
QString dependencyPromptNoteText();

/// 「一起下载」（用户 要的那个"一键下载所有缺少的依赖"）。
QString dependencyAllButtonText();

/// 「只下这个」（一起下载的反面 —— 用户得能说"不"）。
QString dependencyOnlyThisButtonText();

/// 下完了但有几个没解开时那一句（空表回空串 = 不显示）。
QString unresolvedNoteText(const QStringList &lines);

/// 日志里那一行：「正在查询 %1 个前置模组的可用版本…」。
QString dependencyLookupLogText(int count);

/// 详情页上那一行（用户 ：「如果有前置模组，在下载页的游戏版本选择和模组版本
/// 选择中间加一条写明」）：`%1` = 名字用「、」连起来的那一串。
///
/// 说的是"**这一版**要装什么"，所以句子里**不出现"还缺"** —— 那两个字是点下载那一刻
/// （`dependencyPromptText`）才知道的事，这里的用户还没说要下呢。
QString detailPrerequisiteText(const QStringList &names);

/// 同一件事，但**名字一条都没取到**（那一发没成、或者接口那边没有这几条）。
/// 只说得出口"有几条"，那就只说这些 —— **不许退成项目号**：用户 报的那件事
/// （「这个前置模组缺失的提示为数字而非模组名称」）正是"把项目号当名字用"的下场，
/// 而那一串数字对他什么也没说。
QString detailPrerequisiteCountText(int count);

QString updateUpToDateText();

QString updateOutdatedText();

QString updateUnknownText();

QString updateSummaryText(int checked, int outdated);

}  // namespace ModDependencies
