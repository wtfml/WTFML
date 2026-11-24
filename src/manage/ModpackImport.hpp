/**
 * @file ModpackImport.hpp
 * @brief 整合包（Modrinth 的 modrinth.index.json）长什么样、哪条路径不许拼、该怎么起名。
 * @note 这个文件里**没有一个函数碰磁盘、也没有一个函数碰网络**：给一段字节、还一个结论。
 * 所以这些判据能在验证工程里逐条钉住 —— 而整合包这条路真正难的就是它们（下载、
 * 解压、落盘都是仓库里已经验过的老路，见 VersionManager::importModpack）。
 *
 * 【为什么要做整合包导入】真机上的整合包是这么个东西：
 * - 压缩包根目录一份 `modrinth.index.json`：它基于哪个 Minecraft 版本、哪个加载器
 * 的哪个版本，以及**一长串待下载文件**（每条都带 sha1 + sha512 +
 * 字节数，地址指向 cdn.modrinth.com / mediafilez.forgecdn.net）；
 * - 根目录再一份 `overrides/`：整合包作者自己塞的 config、资源包、
 * 光影，以及没托管在 Modrinth 上的 mod。**这一份不在 index 里**，只能从压缩包里解。
 * 所以"导入"= 装一个带加载器的版本（老路）+ 按清单下一批文件 + 把 overrides 解开。
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace ModpackImport {

/// 压缩包根目录下**有哪个文件**就说明它是哪种整合包。判据见 detectArchiveLayout()。
enum class Format {
    Unknown,
    Modrinth,   ///< 有 modrinth.index.json
    CurseForge, ///< 有 manifest.json
    /// MultiMC / Prism 导出的实例：`instance.cfg` + `mmc-pack.json` + `.minecraft/`。
    /// GTNH 那个包就是这个形状。它没有"待下载清单"，整个 `.minecraft/` 都在压缩包里。
    MultiMC,
    /// 普通 `.minecraft` 目录直接压出来的懒人包：根目录（或只有一层目录）里有
    /// `mods/`、`config/`、`options.txt` 这些。版本和加载器从 `versions/` 里的 json
    /// 或目录名推出来。
    GameFolder,
    /// 压缩包里又套了一个 `.mrpack`（PCL 那种"拖入启动器安装"的包装）。
    /// 这个格式只活在检测阶段：导入前会把里面那份 `.mrpack` 解出来再认一次。
    Wrapper,
};

/// 待下载的一条。`path` 已经过 isSafeRelativePath()，可以直接拼到实例目录底下。
struct PackFile {
    QString path;      ///< 相对实例目录的路径，如 `mods/sodium.jar`
    QStringList urls;  ///< 按顺序试的地址（Modrinth 一般只给一个）
    QString sha1;      ///< 清单里的 sha1，落盘后按它校验
    qint64 size = -1;  ///< 清单里的字节数；-1 = 没写（那就只校验 sha1）
    int curseForgeProjectId = 0;
    int curseForgeFileId = 0;
};

/// CurseForge 清单里的一条：**它只给两个号**（`projectID` / `fileID`），文件名和地址
/// 都没有 —— 那不是漏了，是这个格式本来就长这样（见 parseCurseForgeManifest 上面那段）。
/// 所以它得先经过一次"拿号换文件"（`VersionManager::resolveCurseForgeFiles`）才能变成
/// 上面那个 `PackFile`。
struct PackRef {
    int projectId = 0;
    int fileId = 0;
};

/// 未能自动取得的清单文件。保留原编号，避免用户只能看到一个没有行动入口的计数。
struct FileIssue {
    PackRef ref;
    QString fileName;
    QString path;
    QString sha1;
    qint64 size = -1;
    QString reason;
    QString projectWebUrl = {}; ///< 平台返回的规范项目网页；不从数字项目 ID 猜测 slug。
};

struct BundledLibrary {
    QString path; ///< Safe Maven-relative destination under libraries/.
    QByteArray bytes;
    QString sha1;
};

/// 一份解析好的整合包清单。
struct Pack {
    Format format = Format::Unknown;
    QString name;          ///< 整合包的显示名（起实例名时的候选，见 suggestInstanceName）
    QString version;       ///< 整合包自己的版本号（Modrinth 的 versionId）
    QString gameVersion;   ///< `dependencies.minecraft` —— 它基于哪个原版
    QString loaderKey;     ///< 加载器那一行的键（空 = 这个包是纯原版的，见 loaderKey*()）
    QString loaderVersion; ///< 加载器那一行的值
    QList<PackFile> files; ///< 要下载的那些（已剔掉"只给服务端用"和"没给地址"的）
    /// CurseForge 专用：还没换成地址的那些（见 PackRef）。解析完 `manifest.json` 之后
    /// **`files` 是空的、它们全在这儿** —— 换成了才挪过去。其余格式恒为空。
    QList<PackRef> pendingRefs;
    QList<FileIssue> fileIssues;
    /// 清单里**有、但我们不会下**的文件个数，三个原因：
    /// - `env.client` 写着 `unsupported`（只给服务端用的 mod，装到客户端上会崩）；
    /// - `downloads` 是空的（Modrinth 规定这种文件该出现在 overrides 里，让压缩包自己带）；
    /// - CurseForge 那边它是个**嵌套的整合包**（`fileID` 指着一个 .zip，我们不代装）。
    /// 单独计数是为了在确认框里**如实说出来**：不说的话，用户会以为整合包少装了东西。
    int filesSkipped = 0;
    /// CurseForge 专用：平台明确标记文件不可用的个数。保留旧字段名，但空下载链接
    /// 不再等同于作者禁止分发；实际失败原因仍以接口和下载结果为准。
    int filesNotDistributable = 0;
    /// CurseForge 专用：接口里**查不到**（或者文件已被作者删除 / 没过审 / 名字拼不出安全
    /// 路径）的个数。这一档是"这个包本身有问题"，用户除了换包没法处理。
    int filesMissing = 0;
    /// 清单里**点名要文件**的条数（解析时定下来，之后不再变）。-1 = 这个格式没有清单
    /// （MultiMC / GameFolder / 手工构造的包）。
    ///
    /// ★ 它和 `fileCount()` 不是一回事：CurseForge 那份空 `files: []` 的包（内容全在
    /// `overrides/` 里，比如"永利"那一份）要能导入，而"清单里本来有文件、换地址时一个都
    /// 没换回来"必须拦住。判据只能看**清单里写过几条**，不能看换完之后还剩几条。
    int declaredFileCount = -1;
    /// 导入完要如实说出来的"这个包还有哪些东西没导"（目前只有 MultiMC 那一档会写）。
    /// 为空 = 没有额外的话。确认框和收尾那句话都会带上它。
    QString supportNote;
    /// Converted declarative MultiMC runtime. Empty for ordinary manifest packs.
    QJsonObject patchedRuntime;
    QList<BundledLibrary> bundledLibraries;
    /// 解包时要跳过的**游戏目录内**前缀（MultiMC / GameFolder 用）：`versions/`、
    /// `libraries/` 这些由启动器自己装，从包里解出来只会变成旧账。Modrinth / CurseForge
    /// 恒为空（它们的覆盖目录里本来就不该有这些东西）。
    QStringList extractExcludes;
    /// 压缩包里覆盖文件那一层目录名（Modrinth 固定 `overrides` / `client-overrides`；
    /// CurseForge 由清单里的 `overrides` 字段给，**后一个对 CF 恒为空** —— 它没有这个规矩）。
    QString overridesDir = QStringLiteral("overrides");
    QString clientOverridesDir = QStringLiteral("client-overrides");
    /// 清单里点了名的文件总数（**不管下不下得成**）。确认框上那句"要下载的文件：N 个"
    /// 用它：CurseForge 的 N 条在这一步还只是号，但用户要知道的正是"这个包有多大"。
    int fileCount() const { return files.size() + pendingRefs.size(); }
};

/// 解析结果。失败时 `problem` 是**直接弹给用户看**的一句话（不夹错误码，见产品约定）。
struct Result {
    bool ok = false;
    QString problem;
    Pack pack;
};

/**
 * @brief 一个压缩包的"形状"：它是哪种整合包、真正的游戏目录在哪一层。
 *
 * 这个结构只回答"条目表长什么样"，**一个字节都不读**（条目内容由调用方按这里的
 * `*Entry` 名字去读）。所以它能在验证工程里用一份假条目表逐条钉住。
 */
struct ArchiveLayout {
    Format format = Format::Unknown;
    /// 压缩包最外层那一层唯一目录（`Foo/`）；根目录直接摊开时是空串。
    /// 有的打包工具会多套一层（`GT New Horizons 2.8.4/`），判据不能只看根。
    QString rootPrefix;
    /// 真正的游戏目录（`.minecraft`）前缀，带结尾 `/`；空串 = 压缩包根就是游戏目录。
    /// MultiMC 是 `<rootPrefix>.minecraft/`，普通懒人包通常就是 `rootPrefix`。
    QString gameDirPrefix;
    /// 清单文件的**完整条目名**（已带上 `rootPrefix`）。只有 Modrinth / CurseForge 有。
    QString manifestEntry;
    QString mmcPackEntry;     ///< MultiMC 的 `mmc-pack.json`
    QString instanceCfgEntry; ///< MultiMC 的 `instance.cfg`
    /// GameFolder 里最可能那一份 `versions/<id>/<id>.json`（挑不出来时为空 —— 解析器
    /// 还会拿整份条目表再推一次）。
    QString versionJsonEntry;
    /// Wrapper：包里那个 `.mrpack` 的完整条目名。
    QString innerPackEntry;
    /// 压缩包里有没有 `overrides/`（判断"空清单的包"是不是还有内容可解）。
    bool hasOverrides = false;
    /// 服务端包的标记（`server.properties` / `eula.txt` / `server.jar` 这些）——
    /// **排在游戏目录前面判**：服务端包里也有 `mods/`、`config/`。
    bool serverPack = false;
    /// 世界存档（`level.dat` 就在游戏目录那一层）。
    bool worldSave = false;
    /// 有 `.minecraft` 游戏目录的迹象（`mods/` / `config/` / `options.txt` 这些）。
    /// 用于"这份 manifest.json 读不懂、但里面确实是个游戏目录"的退路。
    bool gameFolderMarkers = false;
    /// 游戏目录里有 `bin/modpack.jar` 或 `bin/minecraft.jar`：旧版 Technic / 老式 Forge 包
    /// 的形状 —— **加载器来自那份 jar，不写在版本 json 里**。
    ///
    /// 这一位只用来提示，不做推断：那份 jar 的文件名里没有版本号，进 jar 读 MANIFEST 也不
    /// 可靠。导入照样按懒人包进行，但界面/日志要说清楚"加载器要自己补装一次"。
    bool legacyLauncherJar = false;
    /// **不是整合包**：里面要么只有一个 `mods/` 目录，要么根上就摆着几份 `.jar`。
    /// 那是"别人打包好的一堆模组"，不是整合包 —— 按整合包导进去只会建一个空实例。
    bool modsBundle = false;
    /// 给日志/拖放提示用的一句人话。
    QString detail;
};

/**
 * @brief 看一份条目表，判断它是哪种包、游戏目录在哪一层。
 * @param entries `ZipUtil::listEntries()` 的原样输出（大小写、`\`、目录条目都不讲究，
 * 这里自己归一）。
 *
 * 判定次序是**定死的**：清单（Modrinth → CurseForge）→ MultiMC → Wrapper →
 * 服务端包 → 世界存档 → 普通 `.minecraft` 目录。服务端包必须排在游戏目录前面，
 * 因为它同样有 `mods/` 和 `config/`。
 *
 * NOTE: `__MACOSX/`、`.DS_Store` 这类垃圾条目在判断"最外层只有一层目录"时要先剔掉，
 * 否则 Mac 上打的包永远判不出根前缀。
 */
ArchiveLayout detectArchiveLayout(const QStringList &entries);

/// 条目表里有没有这个**相对路径前缀**（目录以 `/` 结尾也行）。大小写不敏感、
/// `\` 当 `/`。给"空清单的包到底有没有 overrides/"这一问用。
bool archiveHasEntryPrefix(const QStringList &entries, const QString &prefix);

/**
 * @brief 解析 MultiMC / Prism 的 `mmc-pack.json`（+ 可选的 `instance.cfg`）。
 * @param mmcPackJson `mmc-pack.json` 的原始字节。
 * @param instanceCfg `instance.cfg` 的原始字节（没有就传空）。
 * @param gameDirPrefix 游戏目录在压缩包里的前缀（由 detectArchiveLayout() 给）。
 * @param fallbackName 实例名候选（`instance.cfg` 没写 `name` 时用它）。
 *
 * `components[]` 里 `net.minecraft` 给游戏版本，`net.minecraftforge` /
 * `net.fabricmc.fabric-loader` / `net.neoforged` / `org.quiltmc.quilt-loader` 给加载器。
 * 不认识的组件（如 lwjgl3ify、org.lwjgl3）拒绝导入；不能丢掉专用库或启动参数后报告成功。
 * 自定义 patches 由调用方从归档条目表检查并拒绝。
 */
Result parseMultiMCInstance(const QByteArray &mmcPackJson, const QByteArray &instanceCfg,
                            const QString &gameDirPrefix, const QString &fallbackName);

/// Patches keyed by component UID; bundled files keyed by archive-relative entry.
/// No scripts/commands or unimplemented patch operators are silently accepted.
Result parsePatchedMultiMCInstance(const QByteArray &manifest, const QByteArray &config,
    const QHash<QString, QByteArray> &patches, const QHash<QString, QByteArray> &bundledFiles,
    const QString &gameDirPrefix, const QString &fallbackName);
/// Publish archive-local libraries without replacing a different existing file.
bool publishBundledLibraries(const Pack &pack, const QString &minecraftRoot, QString *problem);

/// 把 MultiMC 包的 mmc-pack / patches / instance.cfg 留在实例目录（导出时原样带回）。
bool preserveMultiMCSource(const QString &zipPath, const QString &instanceDir);

/**
 * @brief 解析一个普通 `.minecraft` 懒人包。
 * @param entries 整份条目表（用来找 `versions/` 下的版本 json）。
 * @param versionJson 挑出来的那一份 `versions/<id>/<id>.json`；没有或读不动就传空。
 * @param rootPrefix 游戏目录前缀（由 detectArchiveLayout() 给）。
 * @param fallbackName 实例名候选（压缩包文件名）。
 *
 * 版本和加载器从三个地方推，按可靠程度排：版本 json 的 `inheritsFrom` / `id` /
 * `clientVersion` → `versions/` 下的目录名 → `libraries/` 里的加载器坐标。
 * **推不出来就拒绝**（并说清缺什么），不猜一个版本硬装。
 */
Result parseGameFolderInstance(const QStringList &entries, const QByteArray &versionJson,
                               const QString &rootPrefix, const QString &fallbackName);

/// 压缩包根目录下那份清单的名字。放这儿而不是让各个调用点各写一遍字面量：
/// "这是不是整合包"、"读哪一条"、"去重"三处用的必须是同一个串。
QString modrinthIndexName();
QString curseForgeManifestName();

/**
 * @brief 解析一份 `modrinth.index.json`。
 * @param json 那一条条目的**原始字节**。
 * @return 成功时 ok=true；否则 problem 里是一句给用户看的原因（**不是**"解析失败"）。
 *
 * 每一条拒绝的理由都是"用户能理解、也多半能自己处理"的：这份 json 读不懂（不是整合包）、
 * 它不是 Minecraft 的包、没写基于哪个版本、文件清单一团糟、里面有往外跳的路径。
 *
 * NOTE: **路径不安全的整包直接拒绝**（而不是跳过那一条）。整合包是从网上下来的压缩包，
 * 里面出现 `mods/../../x.jar` 这种路径只有两种可能：文件坏了，或者有人想往
 * 实例目录外面写东西。这两种情况都不该"照常装、只少一个文件"。
 *
 * @param allowEmptyFiles `files` 是空数组时放行。默认 false；调用方核过压缩包里确实有
 * `overrides/` 那一层之后再传 true（和 parseCurseForgeManifest 同一个取舍）。
 */
Result parseModrinthIndex(const QByteArray &json, bool allowEmptyFiles = false);

// ---------------------------------------------------------------------------
// CurseForge：`manifest.json`
// ---------------------------------------------------------------------------
//
// 【这个格式难在哪儿】它的 `files[]` 里**一条地址都没有**，只有两个号：
//
// {"projectID": 238222, "fileID": 8971390, "required": true}
//
// GET /v1/mods/{modId}/files/{fileId} 一条一条问（200）
// POST /v1/mods/files {"fileIds":[...]} **批量**，一个包一问就够（200）
// POST /v1/mods {"modIds":[...]} 批量问"这是模组还是资源包"（200）
//
// 官方那份 `api.curseforge.com` **要 `x-api-key`**（不带 = 403）。所以默认走的是
// 一台不要 key 的社区镜像（同一个接口形状，连 `{"data":[…]}` 那一层都一样）—— 默认用
// 哪一台、为什么，写在 curseForgeApiBase() 上面。用户自己填了 key 就改走官方。

/**
 * @brief 解析一份 CurseForge 的 `manifest.json`。
 * @param json 那个条目的原始字节。
 * @param allowEmptyFiles `files` 是空数组时放行（见下面那条 NOTE）。默认 false。
 * @return 成功时 ok=true，`pack.files` **是空的** —— 那些号在 `pack.pendingRefs` 里等着换。
 *
 * 和 Modrinth 那一条一样，"读不懂"永远是一句用户看得懂的话（少了哪个字段、值是什么）。
 * 加载器那一行的写法两边**完全不同**：CurseForge 是 `{"id": "forge-47.2.0"}`，一个字符串
 * 把名字和版本拼在一起，见 curseForgeLoaderKey()。
 *
 * NOTE: `files: []` 可能是合法的"纯 overrides 包"（整个 .minecraft 被重新打包，清单只剩
 * 骨架）。但那要**压缩包里真有** `overrides/` 那一层才算 —— 纯逻辑层看不到压缩包，所以
 * 默认拒；调用方核过压缩包之后传 `allowEmptyFiles = true`。清单点名了却一条号都读不出的
 * （`declaredFileCount > 0` 而 `pendingRefs` 空）**永远拒**，和这个开关无关。
 */
Result parseCurseForgeManifest(const QByteArray &json, bool allowEmptyFiles = false);

/**
 * @brief CurseForge 那个 `modLoaders[].id` 拆成 Modrinth 那套键。
 * @param rawId `forge-47.2.0` / `fabric-0.15.11` / `neoforge-20.4.1` / `quilt-0.20.0` / 光秃秃的 `forge`。
 * @param keyOut `Pack::loaderKey` 该填什么（fabric → `fabric-loader`、quilt → `quilt-loader`，
 * 翻译过来之后**下游一个字都不用改**：unsupportedLoaderText / loaderKindForDependencyKey
 * 认的就是这四个键）。
 * @param versionOut `Pack::loaderVersion`；`rawId` 里没带版本（老包的 `"forge"`）时是空串，
 * 空串在安装那一步是"挑最新的"。
 * @return 认不认识这个加载器。**认不出来也给 keyOut**（原样那个前缀，如 `liteloader`）——
 * 空串在 unsupportedLoaderText() 眼里是"纯原版包，放行"，装出来一个进不去的整合包。
 *
 * NOTE: 拆在**第一个** `-` 上，不是最后一个：`neoForge-20.4.1-beta` 这种带后缀的也得
 * 把版本整段留住。
 */
bool curseForgeLoaderKey(const QString &rawId, QString *keyOut, QString *versionOut);

/// 批量问文件那一发的请求体（`{"fileIds":[…]}`）。纯函数是为了能离线钉住 ——
/// 这一发的形状写错（键名、号是数字还是字符串）时接口回 400，而 400 的正文里
/// 只有一句 `{"error":"…"}`，看着和"这个包坏了"一模一样。
QByteArray curseForgeFilesRequestBody(const QList<PackRef> &refs);

/// 批量问"这是哪一类"那一发的请求体（`{"modIds":[…]}`）。
QByteArray curseForgeModsRequestBody(const QList<int> &projectIds);

/// 换回来的一个文件。**它和 `PackFile` 差一步**：`path` 里此刻只有一个文件名、还没有目录 ——
/// 目录得拿 `modId` 去问 classId（文件对象自己不带类目号），见 placeCurseForgeFiles()。
struct CurseForgeFile {
    PackFile file;
    int modId = 0;
    int fileId = 0;
};

/// 一次批量问号的结果。**四条出路必须分得开**（这和"无可用版本 / 列表没取到"是同一课）：
/// - `problem` 非空 ⇒ 这一发根本没成（网络、限流、正文读不懂）—— 整件事停下来报错；
/// - 换到的进 `resolved`；
/// - 换不到的**分两类计数**：平台明确标记不可用（`Pack::filesNotDistributable`）、
/// 接口里查不到（`Pack::filesMissing`）。
struct CurseForgeFiles {
    QList<CurseForgeFile> resolved;
    QList<FileIssue> issues;
    QList<int> projectIds;   ///< 这批文件属于哪几个项目（去重后，拿它去问 classId）
    int notDistributable = 0;
    int missing = 0;
    QString problem;         ///< 非空 = 这一发没成
};

/**
 * @brief 接口正文 → 文件。@param asked 是**这一问问的那些号**（正文里缺哪几条要能算出来）。
 *
 * 正文里那条 `hashes` 数组里 `algo` 是 1 的那条就是 sha1（2 是 md5）—— 有它，
 * 下载那一段的 `VerifyMode::SizeAndSha1` 一个字都不用改。
 *
 * NOTE: `fileName` 是**外部输入**，它要拼进路径里（`mods/<fileName>`），所以这里按
 * `isSafeRelativePath()` 逐条判过：带 `/`、`\`、`..` 的当场算"查不到"（记进 missing），
 * 绝不放行到写盘那一步去。名字不安全的文件**整个包不拒绝**（和 Modrinth 那份清单里
 * 写死路径的情形不同：那边路径是清单作者写的，这边是一个我们控制不了的文件名）。
 */
CurseForgeFiles parseCurseForgeFileInfo(const QByteArray &json, const QList<PackRef> &asked);

/// API 链接为空时生成官方 CDN 候选；只接受正的文件 ID 和安全的单个文件名。
/// 候选地址不证明文件可用，也不表示取得再分发许可；下载仍须检查响应与完整性。
QString curseForgeDownloadUrl(qint64 fileId, const QString &fileName, const QString &declaredUrl);
QString curseForgeFileWebUrl(const FileIssue &issue);
QHash<int, QString> parseCurseForgeProjectWebUrls(const QByteArray &json);

/**
 * @brief 这个项目属于哪一类 → 文件该落到实例目录的哪个子目录。
 * @param classId CurseForge 的类目号（6 模组 / 12 资源包 / 6552 光影 / 6945 数据包 /
 * 17 存档 / 4471 整合包）。
 * @return 带结尾斜杠的目录名，如 `mods/`；**空串 = 这一条不下**（嵌套的整合包）。
 *
 * 6 那一档和"认不出来"合在一起：认不出来的一律落 `mods/`（主流启动器都这么做）——
 * 落错的代价是"这个文件没用"，而拒绝的代价是"整合包装不全"，后者重得多。
 *
 * NOTE: 判据是**类目号，不是文件名的后缀**：`.zip` 既可能是资源包也可能是存档，
 * 而 `fileName` 是接口给的、可以随便起。
 */
QString curseForgeFilePath(int classId);

/// 把换回来的文件摆进目录。**这是"号 → 能下的文件"的最后一步**。
struct CurseForgePlacement {
    QList<PackFile> files;   ///< 这一步之后 `path` 才是完整的、能直接下的相对路径
    QList<FileIssue> issues;
    int skipped = 0;         ///< 类目是嵌套整合包（见 curseForgeFilePath 的空串那一档）
    int missing = 0;         ///< 拼出来的路径过不了 isSafeRelativePath()（多半是太长）
};

/**
 * @brief 给每个文件补上目录。
 * @param resolved 上一步换回来的（`file.path` 里此刻只有一个文件名）。
 * @param classIdByProject `modId → classId`，来自 POST /v1/mods 那一发。
 * **里面没有的号一律当"认不出来"**（落 `mods/`）—— 拿不到类目号不该让文件消失。
 *
 * NOTE: 同一个路径出现两次时**以最后一条为准**（和 Modrinth 那一份的 `pathDedupKey()`
 * 是同一课：文件名是外部输入，撞车是可能的，而两条同时写一个文件是数据竞争）。
 */
CurseForgePlacement placeCurseForgeFiles(const QList<CurseForgeFile> &resolved,
                                         const QHash<int, int> &classIdByProject);

/**
 * @brief `POST /v1/mods` 的答复 → `modId → classId`。
 * @param problem 非空则这一发没成（正文读不懂 / 没有 data）。
 *
 * NOTE: 这一发没成**不该让整个导入停下** —— 缺的只是"落到哪个目录"（退成 `mods/`，
 * 见 placeCurseForgeFiles），文件本身还是下得下来的。所以 problem 是给调用方
 * "说一句"用的，不是"报错"用的。
 */
QHash<int, int> parseCurseForgeClassIds(const QByteArray &json, QString *problem = nullptr);

/**
 * @brief 问哪一台接口。
 * @param apiKey 用户在设置里填的 key；空 = 走那台不要 key 的社区镜像。
 * @param override 非空则**优先**用它（`Launcher/CurseForgeApiBase`，验证工程拿它把接口
 * 指到本机的假服务器上）。产品自己**从不写这个键**。
 *
 * 【为什么默认是镜像】官方那份要 `x-api-key`（不带 = 403）。而 key 一旦
 * 编进 exe 就等于公开（谁都能从二进制里抠出来），泄露的 key 会被吊销 —— 所以默认走一台
 * 不要 key 的镜像。**代价要说清楚**：那是**别人的服务**，可能限流、可能哪天就没了。
 * 所以：① 用户填了自己的 key 就改走官方；
 * ② 接口那一发失败时那句话里要提到"可以在设置里填自己的 CurseForge API key"
 * （见 VersionManager）。
 */
QString curseForgeApiBase(const QString &apiKey, const QString &override = QString());

// ---- `dependencies` 里那几个键 ----
//
// 键名只有这一份：判断"这是个加载器包还是纯原版包"、映射到 VersionManager::LoaderKind、
// 以及给用户报"这个包要 Quilt"时用的都是它们。各写各的字面量的话，写错一个字符的症状是
// "明明是 Fabric 包，却按纯原版装了"——那种错不报警，装完才发现。

/// 整合包依赖表里加载器那几个键。**顺序就是认的顺序**（一份包里只会有其中一个）。
QStringList loaderDependencyKeys();
QString fabricLoaderKey();
QString forgeLoaderKey();
QString neoforgeLoaderKey();
QString quiltLoaderKey();

/**
 * @brief 这个加载器本启动器装不装得了。装得了（含"纯原版整合包"）返回空串。
 * @param loaderKey `Pack::loaderKey`。
 * @param loaderVersion `Pack::loaderVersion`（只说给用户听，判断不看它）。
 *
 * NOTE: 认不出来的键走的是和 Quilt **同一条**路（"这个加载器本启动器还不认识"），
 * 不返回空串 —— 返回空串等于"那就当纯原版装吧"，装出来一个进不去的整合包。
 */
QString unsupportedLoaderText(const QString &loaderKey, const QString &loaderVersion);

/// 认不出格式时给用户看的那一句：说清两种包各自长什么样、他手上这个多半是什么。
///
/// NOTE: 起 CurseForge 那一支**删了**（那个格式已经能装，见上面那一段）。
/// 留着一个"还没支持"的分支，将来只会以"明明支持却说不行"的形式出现。
QString unsupportedFormatText(Format format);

/**
 * @brief 一条相对路径能不能安全地拼到实例目录底下（**纯判据**）。
 *
 * 逐段按 `InstanceFiles::validatePathSegment()` 判（那一套和"实例名"共用，只有一处）：
 * 非法字符、控制字符、`.`/`..`、结尾的点或空格、Windows 保留设备名。另外这里还管两件
 * 路径独有的事：
 * - **反斜杠一律拒绝**：Modrinth 的清单用 `/`，出现 `\` 只可能是有人想借 Windows 的
 * 分隔符绕过逐段判据（`mods\..\..\x.jar` 按 `/` 切是一段，逐段看起来完全正常）；
 * - **不许以 `/` 开头**（绝对路径），也不许空段（`a//b`，逐段判据会把它切出一个空段）。
 * 长度上限按整条路径算（240，Windows 的 MAX_PATH 减去实例目录本身那段），不按单段算 ——
 * 真实的 mod 文件名可以很长（`mods/[1.20.2]xxx-fabric-quilt-forge-1.20.x.jar` 就有 60+），
 * 单段限长会把正常的整合包挡在外面。
 */
bool isSafeRelativePath(const QString &path);

/**
 * @brief 从整合包的名字推一个**合法**的实例名候选（给"给这个实例起个名"那个输入框当地板）。
 *
 * 两步，只做必要的事：把名字里 Windows 建不出目录的字符换成 `-`，再把长度收到 64
 * （和 `InstanceFiles::validateDirName()` 那条上限**是同一个数**，超了它就不认）。
 * 结果一定过得了 validateDirName()，所以用户直接点确定就能用。
 *
 * NOTE: **不做**"去空格、去中文、只留 ASCII"那种清理 —— 真机上的实例名就长这样
 * （`原版ATT2地图（无模组）`），那是用户的实例，不是文件名。
 */
QString suggestInstanceName(const QString &packName);

/**
 * @brief 一键安装整合包时推算出的默认实例名及是否属于续装未完成导入。
 */
struct OneClickInstanceName {
    QString name;
    bool resume = false;
};

/**
 * @brief 在 suggestInstanceName() 的基础上，遇到重名时自动追加 `-2`、`-3` 后缀，保证返回的实例名可直接落盘。
 * @param packName 整合包名。
 * @param isAvailable 可选重名判据（返回 true 表示名字可用）；为空时默认用当前游戏目录下
 *        `InstanceFiles::validateRenameTarget(candidate, QString()).isEmpty()`。
 */
QString suggestUniqueInstanceName(const QString &packName,
                                  const std::function<bool(const QString &)> &isAvailable = {});

/**
 * @brief 为下载页整合包一键安装推算默认实例名：优先续装同名链上未装完的目录，否则挑首个可用空名。
 * @param packName 整合包名。
 * @param isUnfinished 可选未完成导入判据（返回 true 表示该目录是上次没装完的半成品）；
 *        为空时默认用 `InstanceFiles::isUnfinishedImport(candidate)`。
 * @param isAvailable 可选空名判据（返回 true 表示名字可用）；为空时默认用当前游戏目录下
 *        `InstanceFiles::validateRenameTarget(candidate, QString()).isEmpty()`。
 */
OneClickInstanceName suggestOneClickInstanceName(
    const QString &packName,
    const std::function<bool(const QString &)> &isUnfinished = {},
    const std::function<bool(const QString &)> &isAvailable = {});

/**
 * @brief 清单里给的那几条地址，按"先试哪条"重排一遍：`cdn.modrinth.com` 排最前。
 * @param urls 清单里那一条文件的 `downloads`（1~3 条）。
 * @return 同样那几条，只换次序；认不出 modrinth 的整份原样返回。
 *
 * NOTE: 并发与按主机限流不归这里管（见 `DownloadManager` 的 `kMaxConcurrentDownloads`
 * 与 `kMaxConcurrentPerHost`）。别在这条路上假设某个并发数。
 */
QStringList preferredUrlOrder(const QStringList &urls);

}  // namespace ModpackImport

// 这个枚举要**过一个 Q_INVOKABLE 的边界**：MainWindow::applyModpackImport() 的第四个参数是
// 它，而验证工程是拿 QMetaObject::invokeMethod 驱动那一半的（那一半是私有的）。
// 声明放在**命名空间外面**：放里面声明出来的名字是裸的 `Format`，而 Q_ARG 那侧写的名字是
// `ModpackImport::Format` —— 对不上就等于没注册（Qt 只在运行时报一句 no such type，
// 而 invokeMethod 只是**静默返回 false**，看着像"那条路没被拦住"）。
// 同款先例见 LoaderMeta.hpp 末尾那两行。
Q_DECLARE_METATYPE(ModpackImport::Format)

