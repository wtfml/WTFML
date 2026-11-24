/**
 * @file InstanceFiles.hpp
 * @brief 判定 versions/ 下的一个目录算不算实例，并解析它真正该用的 jar / json 文件名。
 * @note header-only 的那两个（AppPaths / LibraryRules）分别只管"路径长什么样"和
 * "版本 JSON 里的平台规则"，本文件是第一个既要纯规则又要扫盘的，所以带 .cpp。
 *
 * 【这个文件存在的理由】
 *
 * 改造前实例的身份是**目录名**：`versions/<id>/<id>.jar` 存在才算装好了。这个判据有一个
 * 真机上真实存在的后果 —— 用户在资源管理器里把目录改了名，却没改里面 jar 的名字
 * （`原版ATT2地图（无模组）/` 里躺着 `ATT2原版地图（无模组）.jar`，388 MB 和 851 MB 的
 * 整合包各一个），这些实例在列表里**静默消失**，观感就是"启动器少了两三个版本"。
 *
 * 现在把判据放松成"顶层有 jar 且认得出哪个是客户端 jar"，同时**路径不再按名字硬拼**
 * —— 认出来和还能启动是两件事，只做前者会造出"列表里看得见、点启动报缺 jar"。
 * jar / json 的解析散在四处（VersionManager::getVersionJson、LauncherManager 的
 * checkPreLaunch 和 classpath），所以收在这里一份，免得四处各自漂移。
 *
 * 【第二轮放松：带模组加载器的版本一个 jar 都没有】
 *
 * 上面那条判据的第二次撞墙是**继承型实例**：Fabric 装出来的
 * `versions/fabric-loader-0.15.11-1.20.1/` 里只有一份 json（外加用户丢进去的 mods/），
 * 客户端 jar 在原版那个目录里。照"顶层有 jar"判，这些实例**一个都进不了列表** ——
 * 表现和上一次一模一样：装了、磁盘上看得见、启动器里没有。
 *
 * 所以 recognized() 多了一条路（声明了 `inheritsFrom` 也算数），jar 的路径也跟着变成
 * "顺着链找主人"（jarOwnerId()）。这两条是配套的，缺一个就会回到"看得见、点不了"。
 *
 * NOTE: 判据本身**不看 wtfml.meta**，这是刻意的 —— 元数据文件只有启动器写过才有，
 * 而"被外部改过名"的那些目录恰恰一份都没有，拿它当判据等于救不了它们。
 * wtfml.meta 负责的是别的事：真名（显示名）与稳定 id（抗改名的分组归属）。
 */
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace InstanceFiles {

/// 实例目录名的长度上限（`validateDirName()` 用的就是它）。
///
/// NOTE: 放在头文件里是为了让**别的地方也要收长度**时用的是同一个数 —— 整合包那边从
/// 包名推实例名（ModpackImport::suggestInstanceName）也要收到 64，各写一个 64
/// 的话，改一处漏一处的症状是"候选名字自己就过不了校验"。
constexpr int kMaxDirNameLength = 64;

/// 一个**文件名**（不含目录）的长度上限，`validateFileName()` 用它。
///
/// 和 kMaxDirNameLength 分开：64 是"实例目录名"的规矩，而 mod 文件名可以到 60+ 个
/// 字符，再长一点也完全正常（`sodium-0.5.11+mc1.20.1-forge.jar` 这种拼接很常见）。
/// 这条挡的只是"把一整句话当文件名用"那种，同时给整条路径（MAX_PATH 260）留出实例目录
/// 那一段的余量。
constexpr int kMaxFileNameLength = 200;

/// 扫一个实例目录得到的原始材料。纯数据 —— 判据本体 resolve() 只吃它、不碰磁盘，
/// 所以真值表能在验证工程里直接钉住，不用先去造目录。
struct DirListing {
    /// 目录名，同时就是实例 id（进 --version、拼所有路径都用它）。
    QString dirName;
    /// 顶层 *.jar 的文件名（**只有文件名，不含路径**）。
    QStringList jarNames;
    /// 顶层 *.json 的文件名（同上）。
    QStringList jsonNames;
    /// 顶层那份版本 JSON 声明的父版本（`inheritsFrom`）；没有就是空串。
    ///
    /// NOTE: 这个字段**不由 scanDir() 填**（那会让热路径上每个实例多读一次 json），
    /// 而是 resolveInstance() 在"一个 jar 都认不出来"时才去读出来填上 ——
    /// 见那里的说明。手工构造 DirListing 的断言可以随便填。
    QString inheritsFrom;
};

/// 一个实例该用哪个 jar、哪个 json。
struct Resolved {
    /// 客户端 jar 的**文件名**；空 = 这个目录里没有自己的客户端 jar
    /// （可能是"继承型"实例，见 inheritsFrom；也可能就是认不出来）。
    QString jarName;
    /// 版本 JSON 的文件名；空 = 没有版本 JSON。**不影响 recognized()**，理由见 resolve()。
    QString jsonName;
    /// 顶层所有 jar 的名字。认不出来时调用方拿它去写告警，认出来时留着方便排查。
    QStringList jarCandidates;
    /// 版本 JSON 声明的父版本（`inheritsFrom`）。空 = 这份版本是自立的。
    QString inheritsFrom;

    /// 这个目录算不算实例。
    ///
    /// 两条路：① 自己有客户端 jar（老判据，绝大多数实例）；② 自己**声明了父版本** ——
    /// 带模组加载器的版本就长这样：Fabric 装出来的目录里只有一份 json，连 jar 都没有，
    /// 主 jar 在原版那个目录里（见 jarOwnerId()）。少了 ② 的话，用户用官方安装器装好的
    /// Fabric / Forge 版本在列表里**一个都看不见**。
    bool recognized() const { return !jarName.isEmpty() || !inheritsFrom.isEmpty(); }
};

/**
 * @brief 判据本体：一个目录该被当成哪个版本。
 * @param listing 扫出来的原始材料。
 * @return 解析结果；jarName 为空表示这个目录不算实例。
 *
 * 三条规则，按顺序：
 * ① `<目录名>.jar` 在清单里 → 用它。精确匹配优先，绝大多数实例走这条。
 * ② 否则顶层**恰好一个** .jar → 用它。这是"用户自己在资源管理器里改了目录名"
 * 那种实例；不认这一条，它就会整个被忽略掉。
 * ③ 其余（一个都没有 / 有两个以上且没有精确匹配）→ 不算实例。
 * json 单独按 ①/② 解析（再加一条 ②'：顶层 json 多于一个时挑"名字对得上某个 jar"的
 * 那一个 —— 杂物的 json 对不上，见 InstanceFiles.cpp 的 pickJsonByJarStem()），
 * 但**解析不到不算认不出来**。②' 还挑不出来时，resolveInstance() 会读几个 json 的
 * **内容**再挑一次（那一层才碰磁盘）；resolve() 本身只吃文件名，仍然是纯的。
 */
Resolved resolve(const DirListing &listing);

/**
 * @brief 把一个实例目录扫成 DirListing。
 * @param versionId 实例 id（= 目录名）。
 * @return 目录不存在时三个清单都是空的。
 */
DirListing scanDir(const QString &versionId);

/**
 * @brief scanDir() + resolve()。
 * @param versionId 实例 id（= 目录名）。
 */
Resolved resolveInstance(const QString &versionId);

/**
 * @brief 这个实例的**客户端 jar 归哪个版本目录所有**。
 * @param versionId 实例 id（= 目录名）。
 * @return 自己目录里就有客户端 jar 时返回它自己；自己没有就顺着 `inheritsFrom` 链往上找，
 * 返回链上第一个"自己有 jar"的那一层的 id；整条链都没有就返回链上最后一个
 * （这样报错信息会指着那个真正该有 jar 的目录，而不是加载器目录）。
 *
 * 为什么不能假设"jar 就在自己目录里"：Fabric 装出来的版本目录里**只有一份 json**，
 * 一个 jar 都没有 —— 客户端 jar 是原版那份（`versions/1.20.1/1.20.1.jar`）。拼 classpath
 * 和启动前检查都得按"主人"的路径来，否则要么 classpath 指着一个不存在的文件
 * （JVM 一句 "Could not find or load main class" 就没了），要么直接报"缺少客户端 JAR"。
 *
 * NOTE: 环（手写的 json 首尾相接）和超长链都要挡住 —— 挡法是深度上限 + 访问过的 id 集合，
 * 见实现。这里**不报错**，只是退化成一个"指回自己/最后一个"的答案：真正的报错
 * 由调用方（启动前检查说"缺 jar"、或 VersionManager 说"父版本没装"）去说。
 */
QString jarOwnerId(const QString &versionId);

/**
 * @brief 客户端 jar 的**文件名**（不含路径）。拼 classpath 要的是这个。
 * @param versionId 实例 id（= 目录名）。
 *
 * NOTE: 名字取自 jarOwnerId() 那个目录 —— 对继承型实例（没有自己的 jar）返回的是
 * **父版本那个 jar 的名字**，不是 `<id>.jar`。
 * NOTE: 解析不出来时**退回** `<id>.jar`（老名字），而不是返回空串。想要名字的调用方
 * （checkPreLaunch、拼 classpath）要的是"一个能拿去看在不在的东西"；在正常目录上这个
 * 退回值就是解析结果本身，所以它们的行为逐字节不变，真解析不出来时报的也还是原来那句错。
 */
QString jarName(const QString &versionId);

/**
 * @brief 版本 JSON 的**文件名**（不含路径）。
 * @param versionId 实例 id（= 目录名）。
 * @return 解析不出来时退回 `<id>.json`，理由同 jarName()。
 */
QString jsonName(const QString &versionId);

/**
 * @brief 客户端 jar 在磁盘上的实际路径
 * （<数据目录>/versions/<jarOwnerId()>/<jarName()>）。
 * @param versionId 实例 id（= 目录名）。
 */
QString jarPath(const QString &versionId);

/**
 * @brief 客户端 jar 相对 <数据目录> 的路径（`versions/<主人>/<文件名>`），拼 classpath 用。
 * @param versionId 实例 id（= 目录名）。
 *
 * 单独开一个口而不是让调用方自己拼 `versions/%1/%2`：那两段分别来自 jarOwnerId() 和
 * jarName()，调用方拼错了（比如拿实例 id 当主人）只在继承型实例上出错 —— 而那正是
 * 真机上最难碰到的形态。分隔符是正斜杠（Java 在 Windows 上也认），和改动前逐字节一致。
 */
QString jarRelativePath(const QString &versionId);

/**
 * @brief 版本 JSON 在磁盘上的实际路径。
 * @param versionId 实例 id（= 目录名）。
 */
QString jsonPath(const QString &versionId);

/**
 * @brief 这个实例的主类**至少**要哪个大版本的 Java —— 直接从 class 文件头上读出来。
 * @param versionId 实例 id（= 目录名）。
 * @param mainClass version.json 里那个主类的点分名字（空串 ⇒ 返回 0）。
 * @return 需要的大版本（8 / 17 / 21 …）；读不到、认不出、没有这个类时返回 **0**。
 *
 * 【为什么不能只信 version.json 里那个 javaVersion.majorVersion】
 * 它是**包作者写上去的**，而"换过主类"的整合包经常写错。真机上 `GTNH 2.8.4` 就是：
 * json 写 `javaVersion.majorVersion = 8`，主类
 * `me.eigenraven.lwjgl3ify.relauncherstub.RelauncherStubMain` 却是 **Java 17 编的**
 * （class 文件版本 61），于是 Java 8 一起来就是一句
 * java.lang.UnsupportedClassVersionError: … class file version 61.0,
 * this version of the Java Runtime only recognizes class file versions up to 52.0
 * 主类的 class 文件版本是**唯一**说得准这件事的东西（同一个道理，工程里另有一处：
 * 离线账号的默认皮肤是"看 JAR 里有没有那条定规矩"而不是看版本号）。
 *
 * 【取值】class 文件的第 6、7 字节（大端）是 major version：**大版本 = 它 - 44**
 * （52 → 8、61 → 17、65 → 21）。这个换算从 Java 1.1（45）起就没变过。
 *
 * NOTE: 读的是 zip 条目（借 ZipUtil，一次 PowerShell），而且**只在该版本声明的 Java ≤ 8
 * 时才被调用**（见 MainWindow 那一处）—— 热路径上不多这一趟。
 * NOTE: 找不到就是 0，**不猜**。调用方按"0 = 没意见"处理（保持 json 里那个值）。
 */
int mainClassJavaMajor(const QString &versionId, const QString &mainClass);

/**
 * @brief 一个名字能不能当 versions 下的目录名。**纯函数**，不碰磁盘。
 * @param rawName 用户输入的原始文本（内部先 trim 再判）。
 * @return 合法返回空串；否则返回给用户看的一句原因。
 *
 * 挡掉的东西分两类，理由不同：
 * - **真会造成事故的**：`/` `\`（同时是路径分隔符，放过去就是路径穿越）、
 * 空名字、`.` / `..`。这一类漏掉的后果不是"名字难看"，是文件跑到 versions 外面去。
 * - **Windows 会阴人的**：结尾的点（系统**静默**删掉）、保留设备名（`CON`、
 * `COM1`……根本建不出来）。这一类漏掉的后果是"用户输 A、盘上出现 B"，而且报错
 * 发生在 rename 那一刻，信息通常只有一句"拒绝访问"。
 *
 * 尾随**空格**不在上面那一类里：输入先 trim，所以它根本到不了 Windows 那一步。
 * 用户多敲一个空格不是错误，替他去掉正是他想要的；而尾随的点 trim 管不着，真交给
 * Windows 就会被悄悄吃掉 —— 自己拦下来、把决定权交回用户。
 *
 * 长度上限 64 是**名字**的，不是完整路径的。真机上最长的实例名
 * （`原版ATT2地图（无模组）`）是一层十几个字符，`versions/<id>/<id>.jar` 里 id 要
 * 出现两次，再加上数据目录本身 —— 64 留的余量足够，同时挡掉"把一整句话当名字用"。
 *
 * NOTE: **中文名、全角括号、名字里带空格都是合法的**，真机上就长这样，别顺手加
 * 一条"只许 ASCII"的规则。
 */
QString validateDirName(const QString &rawName);

/**
 * @brief 一段路径段（目录名或文件名）合不合法 —— 和"实例名"**同一套规矩**，只是不判长度。
 * @param segment 一段（`mods` / `sodium.jar` 这种），**不含**分隔符。
 * @return 合法返回空串；否则返回给用户看的一句原因。
 *
 * 整合包清单里的相对路径是逐段拿它判的（见 ModpackImport::isSafeRelativePath）：同一个
 * 判据只写一份，不然"哪些名字建不出来"这件事会在两个地方慢慢漂开。
 *
 * NOTE: 长度不在这一层判。名字的长度上限（kMaxDirNameLength）是**实例目录名**的规矩，
 * 而路径段的长度上限是另一回事（真实的 mod 文件名可以到 60+ 个字符，按 64 判会
 * 把一个正常的整合包挡在外面）。
 */
QString validatePathSegment(const QString &segment);

/**
 * @brief 一个**平铺文件名**（`mods/` 里那一层）能不能用 —— 和路径段同一套规矩，
 *        再加一条长度上限（`kMaxFileNameLength`）。纯函数，不碰磁盘。
 * @return 合法返回空串；否则返回给用户看的一句原因。
 *
 * 这一条专门给"要往盘上落的名字"用，而那几个名字的来源是**外部**（Modrinth /
 * CurseForge 的 `filename` 字段、升级审计里记下来的那一份）。它们不是用户自己在对话框
 * 里敲的，所以不能指望"用户不会输入反斜杠"这种事 —— 一个作者把文件名写成
 * `..\..\startup.bat`（或者在文件名里塞一个 `:` 借 NTFS 的备用数据流）是完全做得到的。
 * 判据本体就是 `validatePathSegment()` + 长度：同一个规矩只写一份。
 */
QString validateFileName(const QString &name);

/**
 * @brief @p candidate 是不是 @p parent 自己、或者它**底下**的某一层。
 * @return 是（含"就是它自己"）返回 true。
 *
 * 加，起因是一次真会把盘写爆的事故：「复制实例」的目标目录可以和源实例目录
 * **撞上**。那一步挑目标用的是个普通的目录选择框，用户能一路点进
 * `versions/我的整合包` 再按确定 —— 于是 `versions/我的整合包/versions/我的整合包 副本`
 * 落在源目录**里面**，而那两步复制（先 `mkpath` 目标、再列源目录）会把自己刚造出来的
 * 那个 `versions/` 也列进去 ⇒ 每层再往深一层，每层重抄整个实例，直到路径撑爆为止。
 * 判断这一件事只需要一个判据，所以它在这儿（两个调用方共用一份：界面那一层管话术、
 * 数据层那一层管兜底）。
 *
 * **比的是规范化之后的真路径**：两边都必须已经在盘上。挑文件夹那一步带的是
 * `QDir::DontResolveSymlinks`，拿回来的路径不做规范化，只比字面量挡不住符号链接 / 8.3
 * 短名 / 大小写这些**别名** —— `canonicalFilePath()` 解的正是这些。
 *
 * 但 canonicalFilePath() **不解 junction**（Windows 上它走 `slowCanonicalized()`，
 * 逐段判的是 `isSymLink()`，而 junction 在 Qt 里是 `JunctionType`、`isSymLink()` 是
 * false），所以这一条自己再接一轮 `QFileInfo::junctionTarget()` 逐段把 junction 换成
 * 真目录（.cpp 里 `resolveJunctions()`，带 32 层防环）。**这条不是补丁，是必需的一半**：
 * 只靠 canonicalFilePath 的话，一个指着实例目录的 junction 能整条绕过去。
 *
 * canonical 解不掉的时候（路径不存在、权限不够）退回 `cleanPath(absolutePath())` ——
 * 别名那一档就漏了，但**判据不消失**（宁可漏一种写法，也不要"这一位读不出来就整条放行"）。
 *
 * NOTE: 尾随分隔符先削掉再比前缀 —— 不然 `D:/a` 和 `D:/ab` 会被认成包含关系
 * （`"D:/ab".startsWith("D:/a")` 是 true 的那种经典错法）。
 */
bool pathIsSameOrInside(const QString &parent, const QString &candidate);
/// 解析符号链接、junction 与路径别名；不存在的路径保留绝对形式。
QString canonicalPath(const QString &path);

/**
 * @brief 这个链接（符号链接 / junction）**最终指着哪儿**；不是链接、或解析不出来时回空串。
 *
 * 为什么不直接用 `QFileInfo::junctionTarget()`：它解析的是重解析点里存的那串 substitute
 * name，而那串字面量可以是相对的、也可能带 `\??\Global\` 这类前缀（本机实测：`cmd /c
 * mklink /J` 造出来的就是 `\??\Global\D:\...`，Qt 于是把相对的那一段拼到 junction 自己的
 * 父目录上）。Windows 上唯一可靠的问法是开一个句柄再让文件系统回答 —— 这里就是那一层，
 * 给"别名还指着原来那份吗"这类判据用（`AsciiPathAlias` 与 `resolveJunctions()`）。
 *
 * 非 Windows 上退回 `symLinkTarget()`。
 */
QString resolveLinkTarget(const QString &path);

/**
 * @brief @p relativePath 这一条（相对 @p root 的）路径上，**有没有任何一层是链接**
 * （符号链接 / junction）。
 *
 * 判的是**逐段**，不是整条：`config/共享/热键.json` 里真正危险的往往是中间那层
 * `config/共享` 是一根指着别处的 junction —— 只看最后的文件判不出来（它自己不是链接）。
 *
 * 与 `pathIsSameOrInside()` 分工不同：那一条判"**最终落到哪儿**"，这一条判"**经过哪儿**"。
 * 两者都不是对方的替代 —— 目标目录本身是一根链接时，"在不在里面"两边解出来是一致的
 * （所以那条闸放行），能挡住它的只有这一条。
 *
 * NOTE: 不存在的段一律当成"不是链接"（路径还不存在是最正常的事）；`isJunction()` /
 * `isSymLink()` 对不存在的路径都回 false。
 */
bool pathHasLinkComponent(const QString &root, const QString &relativePath);

/**
 * @brief 改名时的完整校验：validateDirName() + "重名了没有 / 是不是没改"。
 * @param rawName 用户输入的新名字。
 * @param currentId 现在这个实例的目录名（不参与重名比较的那个）。
 * @return 合法返回空串；否则返回给用户看的一句原因。
 *
 * 重名比较**不区分大小写**：Windows 的文件系统本来就不区分，`versions/Foo` 和
 * `versions/foo` 是同一个目录，按区分大小写放过去，用户在资源管理器里就会看到
 * 两个"同名"的实例。
 * NOTE: 但只差大小写的那种改名（`Foo` → `foo`）是**合法**的，它改的是磁盘上那个
 * 名字的显示大小写。所以恰好等于 currentId 的那一项不参与比较，也不能因为
 * "目标已存在"就直接拒 —— 那个"已存在"的目标就是它自己。
 */
QString validateRenameTarget(const QString &rawName, const QString &currentId);

/// 一次整合包导入能不能用这个名字。
enum class ImportNameVerdict {
    Ok,        ///< 名字合法、盘上也没有它 —— 正常导入
    OkResume,  ///< 撞上的是"上一次没下完的导入" —— 可以接着往里下（见 isUnfinishedImport）
    Taken,     ///< 名字本身不合法、或者被**别的**实例占了 —— 只能换名字
};

/**
 * @brief 整合包导入的名字判据：validateRenameTarget() + 一条"接着下"的口子。
 * @param rawName 用户输入的名字。
 * @param problemOut 非空时，Taken 那条路会把给用户看的那句话写进来（另两种清空）。
 * @param manifestFingerprint 这一趟要导入的包那份清单的指纹（`importManifestFingerprint()`
 * 的产物；空 = 调用方不知道，不查这一条）。
 *
 * 同名**不等于**同一个包。盘上那次没导完的导入记着上一次那份清单的指纹，
 * 这一趟的指纹和它对不上时回 `Taken`（措辞说清是"另一个整合包"）：否则把两个不同的
 * 包往同一个目录里接着导，前一包的半份文件会和新包混在一起 —— 清单里 sha1 对不上的
 * 那几条还会被当成"已经有文件"跳过，装出来是个谁都没见过的组合。
 * 老标记里没有指纹（只记了开始时间），认不出来就按 `OkResume` 放行 —— 不能因为
 * 认不出旧标记，反而把用户已经下到一半的那一份锁死。
 *
 * 和「重命名」判据的差别**只有这一条**：撞上的是"上次没下完的导入"时不算被占 ——
 * 整合包是几百个文件分着下的，网一断就留下半份，
 * 而重名判据只认目录名。不让接着下的话，用户唯一的办法是"换个名字重导"，也就是把已经
 * 下好的那几百 MB 再下一遍 —— 这跟本启动器「重下=校验跳过」那条规矩是相反的。
 *
 * NOTE: OkResume **不是**"静默覆盖"：调用方必须问一句再往下走（见 MainWindow 里那个
 * 确认框）—— 接着下会重新解开包里的 config/资源包，压在用户改过的文件上。
 */
ImportNameVerdict checkImportTargetName(const QString &rawName,
                                        QString *problemOut = nullptr,
                                        const QString &manifestFingerprint = QString());

/**
 * @brief 这个目录像不像"一次没下完的整合包导入"。
 * @param id 目录名（= 实例 id）。
 *
 * 优先判据：实例目录存在且有 `importMarkerPath()` 时，一律视为未完成。
 * 即使 meta 或客户端 jar 已写入，也必须等最后提交时移除标记才放行。
 * 以下是没有标记时的旧版半成品兼容判据（都只看盘，不联网），缺一不可：
 * ① **没有** `wtfml.meta`：启动器把一份实例做成功过就会留下它（导入是在最后一步写的、
 * 装加载器是装完写的、改名时也会补）。没有它 = 启动器从没把这一份做完。
 * ② **没有**客户端 jar：整合包装出来的实例目录里只有一份薄 json + 下下来的那些文件，
 * 主 jar 在原版/加载器那一层。顶层出现 jar 说明这一份不是导入留下的（用户自己放的，
 * 或者手工做的），那种目录不该被导入盖上去。
 * ③ 自己的版本 json 声明了父版本（`inheritsFrom`）—— 那正是导入写下的那份 json 的形状。
 * ④ **而且**能认出它是"导入留下的"：json 恰好只有导入第一步写的那三格
 * （`id` / `inheritsFrom` / `type`）。
 *
 * 补的 ④：③ 单独一条**太宽** —— 别的启动器装出来的加载器版本（Fabric /
 * Forge / NeoForge 的 profile）、用户从别处整个拷来的加载器实例，同样是"没有 meta、
 * 没有顶层 jar、json 里有 inheritsFrom"。只按 ①–③ 判的话，那些**完全正常**的实例会
 * 从启动列表里消失、点启动还会被拦一句"还没装完"。真正能区分的只有两点：我们在导入
 * 开始时亲手落的标记，以及导入写的那份 json 薄到只有三格（第三方 profile 都带
 * `mainClass` / `libraries`，长得不这么薄）。
 *
 * 三条都满足时它**仍然可能是**一台机器上手工拼出来的实例（那种目录也没有 meta、也没有
 * jar），所以调用方要问一句。判据宁可偏保守：认错了的代价是"少一条能接着下的路"，
 * 而放宽的代价是"把用户的实例盖掉"。
 */
bool isUnfinishedImport(const QString &id);

/// 导入开始时在实例目录里落下的标记（`<实例目录>/wtfml.importing`）。
///
/// 它和 `wtfml.meta` 是相反的两件事：meta 是"做完了"，这个是"正在做、还没做完"。
/// 单独一个文件、不进版本 json，是因为 json 的字段第三方工具也会读写，多一个私有字段
/// 既容易被清掉，也会让"那份 json 是不是我们写的"更难看出来。成功收尾时删掉。
/// 起内容是 JSON：`{"startedAt": "...", "manifestSha256": "..."}`。
/// 老版本写的是一行 ISO 时间 —— 读侧把它当"没有指纹"，不影响旧标记留下的半成品。
QString importMarkerPath(const QString &id);

/// 整合包清单的指纹：对**清单文件的原始字节**算 sha256（小写十六进制，64 个字符）。
///
/// 为什么认原始字节而不是解析出来的字段：重新打一次包（zip 重新压过、修改时间变了）时
/// 里面那份清单的字节还是一模一样，而"是不是同一个包"问的正是这件事；反过来说，两个
/// 不同的包几乎不可能连清单字节都一样（名字、版本号、文件表里总有一样不同）。
/// 传空字节回空串 —— 那表示"不知道"，标记里不记指纹、比较时也不拿它拦人。
QString importManifestFingerprint(const QByteArray &manifestJson);

/// 读 `<实例目录>/wtfml.importing` 里记着的清单指纹。
/// 没有标记 / 老版本留下的纯时间戳标记 / 读不动 ⇒ 空串（= 认不出，不拿它拦人）。
QString importMarkerFingerprint(const QString &id);

/// 实例改名之后，把"按目录名认人"的缓存格子搬到新名字底下
/// （`<每个根>/<oldId>` → `<每个根>/<newId>`）。
///
/// 为什么要有：配置备份（`cache/config-backups/<id>/…`）、资源包备份
/// （`cache/resource-pack-backups/<id>`）这些格子的名字都由实例目录名派生。不改名之前
/// 一切正常，改名之后**新名字查不到那些备份、清理也够不着它们** —— 用户看到的是
/// "改了个名，我的配置回退记录全没了"，而磁盘上其实还在，只是没人再按那个名字去找。
///
/// 只搬**目录**：格子里面是时间戳那一层，内容里不抄实例名，所以整目录 rename 就够。
/// 目标格子已经有东西时**不合并** —— 那多半是以前叫过这个名字的另一个实例留下的，
/// 两边的"第 N 代"没法排先后，合并会把一边挤掉。留在原地并报一句，让用户自己决定。
///
/// @param roots 缓存根目录（拼上实例名的那一层）。
/// @param movedOut 真搬成功的格子数（可空）。
/// @return 没搬成的那几处，一句一处（空 = 全搬好了或本来就没有可搬的）。
QStringList migrateInstanceCacheDirs(const QString &oldId, const QString &newId,
                                     const QStringList &roots, int *movedOut = nullptr);

}  // namespace InstanceFiles
