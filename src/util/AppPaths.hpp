/**
 * @file AppPaths.hpp
 * @brief 集中定义启动器数据目录的路径，作为"我们用的是哪个 .minecraft"的唯一出处。
 * @note header-only：只有 inline 函数，不需要加 .cpp，也不用碰链接。
 *
 * 所有路径都从**一处**数据根目录推出来，调用方不许再自己拼：
 * - "切换 .minecraft 文件夹"的落点就是本文件（setMinecraftDir() 一处切换），见 FolderManager；
 * - `--gameDir` 与 ensureInitialOptions 取的是**同一个表达式** AppPaths::versionDir(versionId)，
 *   不再有第二份值，也不靠调用顺序；
 * - 资源位置只有 AppPaths::assetsDir() 这一份，索引和 objects 必然同源，
 *   游戏按 `--assetsDir` 找 indexes/<id>.json 不会扑空。
 *
 * NOTE: 这里**只存一样东西** —— 用户选中的那个数据根目录（见 setMinecraftDir()），
 * 而且它是"被显式设定、要求所有调用方都看见"的状态，不是缓存。其余的一律现算：
 * applicationDirPath() 本身是 O(1)（Qt 缓存了结果），这些函数比起随后要做的
 * 扫盘/读文件便宜到可以忽略；反过来，一旦把算出来的路径也缓存下来，这个头文件
 * 就从"路径的唯一出处"变成"路径的唯一出处 + 一份可能过期的副本"。
 */

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QLatin1Char>
#include <QString>
#include <QVariant>

namespace AppPaths {

/// 启动器**自带**的数据根目录：<exe 所在目录>/.minecraft
///
/// 放在 exe 旁边（而不是 %APPDATA%）是刻意的：整个启动器连带游戏数据可以整体拷走。
/// 代价是必须用绝对路径，因为工作目录可能被改动。
///
/// 它是「游戏文件夹」列表里永远排第一、也删不掉的那一项（见 FolderManager）：别的
/// 目录被挪走、U 盘被拔掉的时候，启动器总得有一个能落的根目录。
inline QString defaultMinecraftDir() {
    // 测试入口可在创建任何控制器之前注入独立根；正式入口不设置此属性。
    const auto app = QCoreApplication::instance();
    const QString isolated = app ? app->property("wtfml.testDefaultMinecraftDir").toString() : QString();
    if (!isolated.isEmpty()) return isolated;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/.minecraft");
}

/// 数据根目录的可写覆盖。空串 = 用默认那份（defaultMinecraftDir()）。
///
/// NOTE: 写成函数局部 static 而不是文件级 static，免得引入静态初始化顺序的问题。
inline QString &minecraftDirOverride() {
    static QString dir;
    return dir;
}

/// 切换启动器使用的数据根目录（「游戏文件夹」列表里点一下就走这里）。
/// @param dir 目录路径；传空串 = 恢复成自带的那一份（<exe 所在目录>/.minecraft）。
///
/// NOTE: 只做 cleanPath（去掉多余的斜杠、`.`、`..`），**不**检查目录存不存在 ——
/// "它是不是个真目录"是 FolderManager 的事，这一层只管"路径长什么样"。
inline void setMinecraftDir(const QString &dir) {
    minecraftDirOverride() = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
}

/// 启动器当前的数据根目录：用户选过就是那一份，否则是自带的那一份。
///
/// 版本、资源、库、自管 Java 全部挂在它下面，所以切换它等于切换"整套 .minecraft"。
inline QString minecraftDir() {
    const QString &custom = minecraftDirOverride();
    return custom.isEmpty() ? defaultMinecraftDir() : custom;
}

/// 已安装版本的根目录：<数据目录>/versions
inline QString versionsDir() { return minecraftDir() + QStringLiteral("/versions"); }

/// 服务端工作区的根目录：<数据目录>/servers。**一个服务端一个子目录**。
///
/// 为什么**不**复用 `versions/`（这是最容易顺手做的一个选择，也是错的）：`versions/`
/// 底下每一个子目录都会被当成一个**客户端实例**（版本列表、图标、分组、`--version` 全按
/// 目录名认）。把一个服务端目录摆进去，症状不是报错，而是实例页上凭空多出一个点进去
/// 什么都启动不了的"实例"，而且用户删它的时候走的是删客户端那条路。
/// 反过来，服务端那一页也**不认** `versions/` —— 两边的条目表各扫各的根，谁也看不见谁。
///
/// NOTE: 和 `versions/`、`cache/` 平级，挂在数据根目录下，理由同上头那几位：换一份 .minecraft
/// 就是另一套服务端（"整个启动器连带数据可以整体拷走"这条在这里同样适用）。
/// NOTE: 目录名小写 `servers`，和 `versions/` 对应；Windows 大小写不敏感，旧的 `Servers`
/// 照常读得到，不需要迁移。它底下那些子目录名由用户定（新建时默认 `Server<游戏版本>`）。
inline QString serversDir() { return minecraftDir() + QStringLiteral("/servers"); }

/// 单个服务端的目录：servers/<id>。@p serverId 就是目录名（和 versions 那边同一个约定：
/// 目录名即身份，可以是中文/空格）。
inline QString serverDir(const QString &serverId) {
    return serversDir() + QLatin1Char('/') + serverId;
}

/// 启动器给一个服务端记的那一份：servers/<id>/.wtfml-server（可选文件）。
///
/// NOTE: 点开头 + 不叫 .json，理由和 `wtfml.meta` / `wtfml.folders` 同源：它住在**服务端自己
/// 的目录里**，而那个目录是游戏进程的工作目录 —— 服务端自己会往里写一堆东西
/// （`server.properties`、`eula.txt`、`world/`、`logs/`…），插件/模组也可能扫根目录。
/// 点开头 + 我们自己的格式标记（见 `ServerModel::formatTag()`）能让两边互不打扰。
inline QString serverMetaFileName() { return QStringLiteral(".wtfml-server"); }

/// servers/<id>/.wtfml-server
inline QString serverMetaPath(const QString &serverId) {
    return serverDir(serverId) + QLatin1Char('/') + serverMetaFileName();
}

inline QString crashReportsDir() {
    return minecraftDir() + QStringLiteral("/crash-reports");
}

/// ReplayMod 的录像落点：<数据目录>/replay_recordings
///
/// NOTE: 目录名是**那个模组定的**（ReplayMod 从 1.x 起就叫这个），改不了；同样是
/// "游戏/模组自己写、我们只读"，所以跟 crashReportsDir() 摆在一起。不按实例分
/// （理由同上）。那排常用目录拿 `QDir(...).exists()` 当"这一路现在有没有东西"，
/// 「录像」那一路还多一条判据（这个实例装没装 ReplayMod，见 instanceCommonFolderKeys）。
inline QString replayRecordingsDir() {
    return minecraftDir() + QStringLiteral("/replay_recordings");
}

inline QString versionDir(const QString &versionId) {
    return versionsDir() + QLatin1Char('/') + versionId;
}

/// 客户端 jar：versions/<id>/<id>.jar。是"这个版本装好了没有"的判据。
inline QString versionJarPath(const QString &versionId) {
    return versionDir(versionId) + QLatin1Char('/') + versionId + QStringLiteral(".jar");
}

/// 版本 JSON：versions/<id>/<id>.json。javaVersion / assetIndex / libraries 都在里面。
inline QString versionJsonPath(const QString &versionId) {
    return versionDir(versionId) + QLatin1Char('/') + versionId + QStringLiteral(".json");
}

/// 版本元数据那个文件名本身（`wtfml.meta`）。
///
/// 加：**给"不在当前数据目录里"的那一份用**。上面那些 versionDir() /
/// versionMetaPath() 算的全是**当前这个 .minecraft** 底下的路径（那是它们存在的
/// 理由），而"复制实例"的目标在**另一个**游戏文件夹里 —— 那一份实例的 wtfml.meta
/// 只有文件名能问这一层，其余的由调用方自己拼（见 VersionManager::copyInstance）。
///
/// NOTE: **算路径**这一层，这个字面量只有这一处（versionMetaPath() 也从这儿取）—— 复制那条路
/// 要是自己再写一遍 "wtfml.meta"，改文件名时就必然漏掉一处，而漏掉的那一处**不报错**：
/// 拷过去的那份实例在启动器眼里就是"没有元数据"，分组、图标、加载器信息一起没了。
/// 核：`ModpackExport.cpp` 那份文件排除表里的那一项也走这个函数（`kFileRules[]` 是静态
/// 初始化的，所以那一项是表外一条现算的判据）—— 别再往表里抄一个字面量。
inline QString versionMetaFileName() { return QStringLiteral("wtfml.meta"); }

/// 启动器自己的版本元数据：versions/<id>/wtfml.meta（可选文件）。
/// NOTE: 扩展名不是 .json —— <id>.json 是版本 JSON 的既定名字，撞名会让"读元数据"
/// 和"读版本 JSON"落到同一个文件上。详见 VersionManager::displayParts()。
inline QString versionMetaPath(const QString &versionId) {
    return versionDir(versionId) + QLatin1Char('/') + versionMetaFileName();
}

/// 实例图标：versions/<id>/icon.png（可选文件，用户自己挑的那张图存在这儿）。
///
/// NOTE: 挑中的那张图要**拷进实例目录**、meta 里只记一句"用本地那张"（见
/// VersionManager::iconField()），而不是记原图的绝对路径 —— 后者在改名
/// （整个目录连带搬家）之后就失效了，而那正是这个项目里天天发生的事。
/// NOTE: 固定叫 icon.png、不跟着原图的扩展名走：读的一侧只认这一个名字，用户挑了 jpg
/// 也照样按 PNG 存（QImage 读进来再写出去，格式由我们定，写不出来就报错不落盘）。
inline QString versionIconPath(const QString &versionId) {
    return versionDir(versionId) + QStringLiteral("/icon.png");
}

/// 实例分组：<数据目录>/wtfml.folders（可选文件，没分过组就不存在）。
///
/// NOTE: 它挂在**数据根目录**下、不挂在 versions 下，因为分组是"这一份 .minecraft 里
/// 的实例怎么摆"，换了一份 .minecraft 就是另一套分组。挂在 versions 里会让
/// "删掉 versions 重下一遍"顺手把用户的分组也删了。
/// NOTE: 和 wtfml.meta 一样**不叫 .json**：它是给启动器读写的配置文件，扩展名跟别的
/// json 混在一起，早晚有人拿它当"某个版本的元数据"去读。
inline QString instanceGroupFilePath() {
    return minecraftDir() + QStringLiteral("/wtfml.folders");
}

/// 旧格式（1.18.2 及以下）natives classifier jar 的解压目标：versions/<id>/natives
inline QString nativesDir(const QString &versionId) {
    return versionDir(versionId) + QStringLiteral("/natives");
}

/// 共享库根目录：<数据目录>/libraries。所有版本共用一份。
inline QString libraryDir() { return minecraftDir() + QStringLiteral("/libraries"); }

/// 资源根目录：<数据目录>/assets（objects 索引与哈希文件都在其下）。
inline QString assetsDir() { return minecraftDir() + QStringLiteral("/assets"); }

/// 玩家皮肤/披风贴图的缓存根目录：<数据目录>/skins（见 PlayerSkinStore）。
///
/// NOTE: 跟着当前游戏文件夹走 —— 换一份 .minecraft 就是各存一份缓存，和 java/、assets/
/// 一样。这是接受的：那两样本来就是"每份游戏目录各自一份"，皮肤缓存没理由特殊，
/// 而且真要共用一份反而要多一个跟游戏目录无关的落点（比如 %APPDATA%），
/// 那又和"整个启动器连带数据可以整体拷走"那条打架。
inline QString skinsDir() { return minecraftDir() + QStringLiteral("/skins"); }

/// 自管 Java 运行时的根目录：<数据目录>/java/<主版本>/…
inline QString javaDir() { return minecraftDir() + QStringLiteral("/java"); }

/// 某个主版本的 Java 运行时目录：<数据目录>/java/<majorVersion>
inline QString javaVersionDir(int majorVersion) {
    return javaDir() + QLatin1Char('/') + QString::number(majorVersion);
}

/// 下载下来的第三方安装器 jar 的落脚处：<数据目录>/cache/installers。
///
/// Forge / NeoForge 的安装器是 6 MB 上下的 jar，而"装一次"和"这个版本上换个加载器版本
/// 再装一次"往往挨得很近。留在盘上，第二次装就直接命中"文件在就算好"那条判据，不用重下。
///
/// NOTE: 挂在**数据根目录**下（不挂在 versions 里），理由和 wtfml.folders 一样：它是"给
/// 这一份 .minecraft 装东西用的工具"，换一份游戏目录就该是另一份工具。而它又不能
/// 丢进 versions/ —— 那底下每一个子目录都会被当成一个实例。
/// NOTE: 没人会去清它（装坏了也不影响：下一次装会重新下）。用户想清就自己删 cache/。
inline QString installerCacheDir() {
    return minecraftDir() + QStringLiteral("/cache/installers");
}

/// 下载页那些资源的**图标**在本地留一份的目录：<数据目录>/cache/icons。
///
/// NOTE: 挂在**数据根目录**下（和 cache/installers、cache/modpacks 一样），不挂 %APPDATA%：
/// "整个启动器连带数据可以整体拷走"这条在这里同样适用。代价是换一份 .minecraft
/// 就重新攒一份图标 —— 和 skinsDir() 那段是同一个取舍，理由也一样。
/// NOTE: 没人会去清它，清掉也不影响正确性（下一次进那一页重新攒）。过期/读坏的那些
/// 由 ResourceStore 自己换掉或删掉。
inline QString iconCacheDir() {
    return minecraftDir() + QStringLiteral("/cache/icons");
}

/// 从下载页下回来的整合包（.mrpack）的落脚处：<数据目录>/cache/modpacks。
///
/// 它只是个中转：下回来紧接着就走「导入整合包…」那条**同一条路**（解包、问实例名、下清单
/// 里那几百个文件）。留在这儿是为了"导入到一半用户反悔了"时不用重下那几十 MB —— 和安装器
/// jar 是同一种打算（见上面那段）。
///
/// NOTE: 挂在 cache/ 下面，理由和安装器一样：它不能进 versions/（那底下每一个子目录都会被
/// 当成一个实例），也不该跟着实例走。清不清由用户自己决定（重下一次而已）。
inline QString modpackCacheDir() {
    return minecraftDir() + QStringLiteral("/cache/modpacks");
}

/// 「服务器列表」那一页上一次测出来的样子（图标 / MOTD / 人数 / 延迟）：
/// <数据目录>/cache/servers，一台一个 json（见 ServerStatusCache）。
///
/// 为什么要落盘：一次测速要**真的连上去**（每台最多 4 秒超时），而这一页是"每次打开实例
/// 设置都会看见"的一页 —— 断网那一次、或者手快没等完的那一次，那一页不该是一片空白
/// （上一次明明问到过）。它和 cache/icons 是同一条路上的东西：都是"问一次很贵、
/// 答案不会一秒一变"。
///
/// NOTE: 同样挂在**数据根目录**下（和 cache/ 里那几位一样），不挂 %APPDATA% ——
/// "整个启动器连带数据可以整体拷走"这条在这里同样适用。没人会去清它，清掉也不影响
/// 正确性（下一次测速重新攒）。
inline QString serverCacheDir() {
    return minecraftDir() + QStringLiteral("/cache/servers");
}

/// 「配置文件」那一页**每存一次就留一份原来那份**的落脚处：
/// <数据目录>/cache/config-backups/<实例id>/<时间戳>/<相对 config/ 的路径>。
///
/// 为什么要有：这是一个能改坏用户配置的编辑器（改错了没有撤销），而配置是用户调了
/// 几个月的。写之前把盘上那份原样留一份，用户就能回去。
///
/// 为什么**不能**放在 `<实例>/config/.wtfml-backups/`（这是最自然、也最容易做错的那一
/// 个选择）：模组配置是**按目录树认的，不是只认 config/ 那一层** —— 盘上大量
/// `.toml` 住在 config/ 的子目录里（`config/ars_nouveau/xxx.toml`、
/// `config/Mekanism/`…），也就是说有一整套按子目录布局的配置正在被正常加载。我们
/// 塞进去的那些 `.toml` 备份会顺着同一条发现逻辑被当成**活的配置** —— 一份配置读两遍、
/// 模组读到自己上一次的样子，而且屏幕上什么都看不出来。放 cache/ 底下没有这个问题：那是
/// 数据根目录底下、和 config/ 平级、游戏不认的一个目录。
///
/// NOTE: 和 cache/ 里那几位同一条约定（挂数据根目录、可整体拷走）。**但有一处不一样**：
/// 清理只认我们自己生成的文件夹名（见 ConfigFileText::backupFoldersToRemove()），
/// 而且只删第 20 份往后的那些 —— 它是全仓唯一一处会主动删东西的缓存目录。
inline QString configBackupDir() {
    return minecraftDir() + QStringLiteral("/cache/config-backups");
}

/// 主题里那几张背景图存的地方：<数据目录>/cache/theme。
///
/// 为什么要有：用户挑的那张图**不能**直接用他自己的路径 —— 他挪走/删掉之后界面上就是一块
/// 空白，而且他多半已经忘了当初挑的是哪一张。收进来的那一份还会顺手缩到 2560 以内
/// （见 `src/util/ThemeImage.hpp`）：一张手机拍的照片是 4000×3000，原样铺上去意味着每次
/// 启动都要解出 48 MB 的位图。
///
/// NOTE: 和 cache/ 里那几位同一条约定（挂数据根目录、可以整体删掉，删了只是图没了）。
inline QString themeImageDir() {
    return minecraftDir() + QStringLiteral("/cache/theme");
}

/// 给 Java 8 用的那套 ASCII 别名（每个实例一套，见 AsciiPathAlias）：<数据目录>/cache/ascii。
///
/// 为什么要有：Java 8 打不开任何带非 ASCII 字符的路径（真机上 `20w14∞` 和中文名的 1.8 整合包
/// 都起不来），于是把要递给 Java 的那几条路径换成这底下的联接/硬链接。判据、建法、为什么不
/// 放进 versions/ 全在 `src/launcher/AsciiPathAlias.hpp` 的块注释里。
///
/// NOTE: 目录名（`ascii`）和里面每一套的名字（实例 id 的哈希）都必须是**纯 ASCII** —— 这一条
/// 是整套东西唯一的意义所在，数据目录自己不是 ASCII 的话它就没用了（那种情况
/// AsciiPathAlias::ensure() 会说清楚，而不是建一套没用的出来）。
inline QString asciiAliasRootDir() {
    return minecraftDir() + QStringLiteral("/cache/ascii");
}

/// 资源包启用顺序保存前留的那一份：<数据目录>/cache/resource-pack-backups/<实例 id>/…。
///
/// 和 `configBackupDir()` 同一个理由：改 `options.txt` 之前把原文留一份，用户能照着改
/// 回去。没有界面读它 —— 它是"用户自己去那一格子里找"的那种备份，所以那一格的布局
/// 只求一眼看得懂。格子名由实例目录名派生，改名时要跟着搬（见
/// `InstanceFiles::migrateInstanceCacheDirs()`）。
inline QString resourcePackBackupDir() {
    return minecraftDir() + QStringLiteral("/cache/resource-pack-backups");
}

/// 存档写之前留的那一份：<数据目录>/cache/world-backups/<格子名>/<时间戳>/level.dat。
///
/// NOTE: 两层目录里**格子名在外面**（和配置备份逐字同一形状：那边也是
/// `<根>/<文件名>/<时间戳>/<原名>`，见 `ConfigFileText::backupPathFor()`）。这不是
/// 随手摆的：清理"只留最近 20 代"是**按存档**算代数的 —— 格子名要是在里面，
/// 数代数的那个目录就成了"所有世界的备份混在一起"，删起来会把别人的代数算进去。
///
/// 格子名**不是**存档目录名：两个实例各有一个叫 `world` 的存档时，
/// 只用世界名会共用一个格子 —— 清理时互相挤代数，`latestWorldBackup()` 还会拿到
/// 别人世界的 `level.dat`（界面那句"拷回存档目录"就照做了）。格子的取名规矩在
/// `InstanceMaintenanceController.cpp` 的 `worldBackupSlot()`：标准布局
/// `<数据目录>/versions/<实例 id>/saves/<世界>` 取 `"<id>__<世界>"`，别的布局取
/// 世界名 + 绝对路径短哈希。
///
/// 为什么和配置备份**分开两个目录**：两边的"一份"不是一回事 —— 配置那一份是**一个文件**
/// （用户点一次「还原上一次」要还的就是那个文件），存档这一份是**整个存档目录**里我们动过
/// 的那些（`level.dat` 一份，将来要动别的就是那些）。混在一起的话，"清理只删第 20 份往后"
/// 这条规则会按两边加起来算代数，用户会遇到"我明明只改了几个配置文件，存档的备份怎么没了"。
///
/// 为什么**不能**留在存档目录里（`.wtfml-backups/` 之类）：那是游戏的世界文件夹，
/// 里面出现什么都会被游戏扫（`level.dat` 之外的文件大多无害，但"这个目录是不是一个
/// 世界"这件事是**看 `level.dat` 在不在**判的，我们往里塞东西就是在跟这个判据打交道）。
/// 更要紧的是：用户在游戏里看到的世界列表来自**存档目录的下一级**，我们凭空多出来的
/// 一层目录会在某些工具/启动器里显示成一个莫名其妙的世界。放 cache/ 底下两边都干净。
inline QString worldBackupDir() {
    return minecraftDir() + QStringLiteral("/cache/world-backups");
}

/// 用户自己点出来的备份（存档 / 实例快照）落在哪儿：
/// <数据目录>/cache/backups/{worlds,instances}。
///
/// 和上面两份**差在"谁产生的"**：那两份是**我们**动用户的文件之前顺手留的（用户没点过，
/// 甚至不知道它们存在），这一份是**用户点出来**的、他找得到、也**会自己要回去**的那种。
/// 所以它摆在同一个 `cache/` 底下（同一条"挂数据根目录、可以整体拷走"的约定），但分成
/// 另外两个目录 —— 上面那两份各有各的清理规矩（配置备份只留 20 代），而用户自己点的备份
/// **我们一个都不删**：那是他的东西。
///
/// 为什么**不能**摆在 `<实例>/backups/`（最自然的那一个选择）：实例快照默认排除
/// `assets` / `libraries`，而"排除"是**按顶层目录名**做的 —— 备份目录要是住在实例里面，
/// 第二次备份就会把第一次那份 zip 打进第二次的包里，越大越离谱；完整快照更是把上一份
/// 整个吃进去。放在 `cache/` 底下、和 `versions/` 平级，这条链天然断掉。
///
/// NOTE: 目录名分两档（`worlds` / `instances`）而不是混着摆，理由和上面配置/存档分开那一段
/// 逐字同源：一个存档包和一个实例快照包**还原到的地方根本不是一回事**，混在一个列表里
/// 早晚会有人拿实例包去还原一个存档。文件名里也带着这一档（见 BackupFiles::backupFileName）。
inline QString backupDir() {
    return minecraftDir() + QStringLiteral("/cache/backups");
}

/// 「这个文件是哪个资源」那一趟反查（`InstanceIconLookup`）的结果留在本地：
/// <数据目录>/cache/instance-icons，**一个实例一个 json**（文件名 = 实例 id 的 sha1）。
///
/// （要求：「模组图标做本地缓存功能，不要每次都拉取」）。为什么非落盘不可：
/// 那一趟要**把这一页每一份 jar 从头读一遍**（算 sha1 + murmur2 指纹，一列几十个模组就是
/// 几百 MB），再问两三家接口 —— 而"这一列文件分别对应哪个资源"这件事**不会一秒一变**：
/// 文件没换（钥匙里带着大小和修改时间）、资源也没换。`cache/icons/` 那一层存的是**图的
/// 字节**，它只有在"身份"认出来之后才轮得到 —— 少了这一份，每次启动都要把上面那一整套
/// 重来一遍，图标那几秒里全是占位拼图（用户看到的就是"每次都拉取"）。
///
/// NOTE: 挂**数据根目录**下（和 cache/ 里那几位一样，见 iconCacheDir() 那段），不挂
/// %APPDATA% —— "整个启动器连带数据可以整体拷走"这条在这里同样适用。
/// NOTE: 没人会去清它，清掉也不影响正确性（下一次进那一页重新攒）；键里带着大小和修改
/// 时间，所以**换了文件自然作废**。
inline QString instanceIconCacheDir() {
    return minecraftDir() + QStringLiteral("/cache/instance-icons");
}

}  // namespace AppPaths

