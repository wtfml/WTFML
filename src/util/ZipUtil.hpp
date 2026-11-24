/**
 * @file ZipUtil.hpp
 * @brief 通过 PowerShell 压缩/解压压缩包的通用工具。
 * @note 项目没有引入 zlib/QuaZip 之类的解压库，沿用 GlobalJavaSettings 里
 * 已有的"调用 PowerShell"方案。这里改用 .NET 的 ZipFile，因为
 * Expand-Archive 既不能排除 META-INF/，对 .jar 扩展名也不可靠。
 * @note **写**那一侧只有一个口（createZip）：压缩级别、条目名的分隔符、失败时删半成品
 * 这三件事都由它一处管 —— 散到调用点去做，迟早会有一处忘了删（用户下次看到的是
 * "我明明失败了，盘上却躺着一个能打开的坏包"）。
 */
#pragma once
#include <functional>

#include <QByteArray>
#include <QList>
#include <QHash>
#include <QString>
#include <QStringList>

namespace ZipUtil {
/// 批量读取包内图标；结果以归档路径索引。每张限制 1 MiB，不解压到磁盘。
QHash<QString, QByteArray> readIconsBatch(const QStringList &archives);
struct ArchiveMetadata {
    QHash<QString, QByteArray> entries;
    QHash<QString, QString> entryProblems;
    QString problem;
};
/// One bounded worker process for the whole batch; never extracts to archive paths.
QHash<QString, ArchiveMetadata> readMetadataBatch(const QStringList &archives,
    const QStringList &entryNames, QString *problem = nullptr,
    const std::function<bool()> &cancelled = {});

/// 打包时的一条：归档里叫什么（`/` 分隔）↔ 盘上哪一份文件。
struct ZipSource {
    QString entryName;   ///< 归档里的名字，**`/` 分隔**（不是 `\`，见 createZip）
    QString sourcePath;  ///< 盘上那份文件的绝对路径
};

/**
 * @brief 把若干份盘上的文件打成一个 zip。
 * @param targetZip 目标 zip 的绝对路径。父目录不存在时自动建。
 *   **目标已经存在时直接失败、不覆盖**（.NET 的 `Open(...,'Create')` 是清空重写，配上
 *   "失败删半成品"，会连盘上原来那份好的一起毁掉；调用方先挑一个没人占的名字）。
 *   NOTE: 以前这一行写的是"目标已存在则整个盖掉"，和实际想守的规矩正好相反 ——
 *   照着那句话写调用方，就会把上一份好备份当成炮灰。
 * @param entries 条目表，**顺序就是归档里的顺序**（复制出去的东西要可复现）。
 * @param errorOut 失败时的人类可读描述。
 * @return 成功返回 true。
 *
 * 失败时**把半成品删掉**（.NET 那边抛异常时那个文件已经建出来了，留在盘上比没有更坏：
 * 用户会以为包做出来了）。条目名一律用 `/` 拼：zip 规范是 `/`，而 .NET 在 Windows 上
 * 既不拦 `\` 也不改它 —— 名字里带 `\` 的条目在别的启动器（Java 的 ZipInputStream）眼里
 * 就是一个名字里带反斜杠的文件，等于整层 overrides 全废。
 *
 * NOTE: 压缩级别用的是 Optimal（.cpp 里那个常数就是旋钮）。包里绝大多数是**已经压过的**
 * jar，这一档换来的只是几个百分点 —— 真要抠速度，改成 NoCompression 是安全的，
 * 只是给出一个"和大家不一样"的包，没那个必要。
 * NOTE: 条目表**走临时文件**，不进脚本正文 —— 脚本是用命令行递给 PowerShell 的，而
 * Windows 一条命令行只有 32767 个字符（Base64 + UTF-16LE 之后脚本正文只剩一万二千
 * 来个字符），一个几百文件的实例的表就超过它了。
 * ⇒ **别为了"省一次写盘"把表改回拼进脚本**。
 */
bool createZip(const QString &targetZip, const QList<ZipSource> &entries,
                QString *errorOut = nullptr, const std::function<bool()> &cancelled = {});

/// 读一个条目的结果。**这四样必须分得清**：用户选的那个压缩包到底是不是整合包
/// （NotFound），和它是不是一个坏文件（NotAnArchive / Failed），要给的是两句完全不同的话。
enum class EntryStatus {
    Ok,           ///< 读到了，字节在 out 里
    NotFound,     ///< 压缩包能打开，但里面没有这个条目
    NotAnArchive, ///< 打不开：它不是 zip（改过扩展名的 rar/7z），或者文件坏了
    Failed,       ///< 别的失败（超时、写不出来），细节在 errorOut 里
};

/**
 * @brief 把多个压缩包解压到同一个目录。
 * @param archives 待解压的压缩包绝对路径（.zip 或 .jar）。
 * @param targetDir 目标目录，不存在则自动创建。
 * @param excludePrefixes 需要跳过的条目前缀，例如 {"META-INF/"}。
 * @param errorOut 失败时的错误描述。
 * @return 若全部解压成功则返回 true.
 */
bool extractArchives(const QStringList &archives, const QString &targetDir,
                     const QStringList &excludePrefixes = QStringList(),
                     QString *errorOut = nullptr,
                     const std::function<void(int done, int total)> &progress = {});

/**
 * @brief 读压缩包里**一个条目**的原始字节（不解压其它任何东西）。
 * @param archive 压缩包绝对路径（.zip）。
 * @param entryName 条目名，`/` 分隔的完整路径，例如 `modrinth.index.json`。
 * 先按原样找，找不到再**不区分大小写**找一遍（有些整合包工具会把文件名写得大小写
 * 不一样，而 zip 的条目名是区分大小写的）。
 * @param out 成功时写入条目的原始字节（会被 clear()）。
 * @param errorOut 失败时的人类可读描述（NotAnArchive 时是空串 —— 那种情况调用方自己有话说）。
 * @param what 这次动作叫什么，拼进 errorOut 那几句话里（`读 X 超时` / `读 X 时进程被强行结束了`）。
 * 默认是通用的一句；**调用方要把它写清楚**，因为 Failed 那一路的 errorOut 有一处是
 * **直接弹给用户看的**（整合包那条路），句子里的主语换成别人就说不通了。
 * @return 见 EntryStatus。**没这个条目**和**读失败**是两件事，调用方必须分开处理。
 *
 * NOTE: 这个重载**也带上限**（默认 16 MiB，R11）—— 它读的都是元数据 / 贴图 / class 头
 * 那类小东西。要读更大的条目就自己传 maximumBytes；几十上百 MB 的东西别读进内存，
 * 走 extractEntryToFile()。
 */
EntryStatus readEntry(const QString &archive, const QString &entryName, QByteArray *out,
                      QString *errorOut = nullptr,
                      const QString &what = QStringLiteral("读压缩包条目"));
/// 带显式上限的那个：声明大小和实际解出来的字节**两头都卡**（`maximumBytes <= 0` 才是无上限）。
EntryStatus readEntry(const QString &archive, const QString &entryName, QByteArray *out,
                      QString *errorOut, const QString &what, qint64 maximumBytes);

/**
 * @brief 把压缩包里**一个条目**解成盘上的一份文件（覆盖已有文件）。
 * @param archive 压缩包绝对路径。
 * @param entryName 条目名，`/` 分隔的完整路径。找不到时**不区分大小写**再找一遍（同 readEntry）。
 * @param targetPath 落点。父目录不存在时自动建，已存在的文件直接盖掉。
 * @param errorOut 失败时的人类可读描述（NotAnArchive 时是空串，同 readEntry）。
 * @param what 这次动作叫什么，拼进 errorOut 那几句话里。
 * @return 同 readEntry（Ok / NotFound / NotAnArchive / Failed）。
 *
 * 和 readEntry 分开是因为**大小**：这一条要解的东西是完整的整合包（几十上百 MB），
 * 读进内存再写回去没有意义；而且它没有大小上限。
 */
EntryStatus extractEntryToFile(const QString &archive, const QString &entryName,
                               const QString &targetPath, QString *errorOut = nullptr,
                               const QString &what = QStringLiteral("解压压缩包条目"));

/**
 * @brief 列出压缩包里所有条目的名字（`/` 分隔，**原样**：不排序、不去重、不改大小写）。
 *
 * @param archive 压缩包绝对路径。
 * @param out 成功时写入条目名（会被 clear()）。**目录条目也在里面**（zip 里那一条
 * `foo/` 结尾的记录），所以判据用"有没有这个前缀"比用"有没有这个文件"结实。
 * @param errorOut 失败时的人类可读描述（**NotAnArchive 时也是空串**，和 readEntry 一样）。
 * @param what 这次动作叫什么，拼进 errorOut 那几句话里（同 readEntry）。
 * @return 见 EntryStatus。**没这个条目**那一档不会出现（列一张表要么成功要么失败）。
 *
 * NOTE: 大包（几百 MB、几千个条目）列一遍只是把**名字**读出来，不解压任何东西 ——
 * 但它是**另起一个 PowerShell 进程**，所以别放进每秒都要跑的那种循环里。
 */
EntryStatus listEntries(const QString &archive, QStringList *out, QString *errorOut = nullptr,
                        const QString &what = QStringLiteral("读压缩包的条目表"));

/**
 * @brief 只解压某几个前缀底下的条目，而且**把前缀剥掉**（`overrides/config/x` → `<目标>/config/x`）。
 * @param archives 待解压的压缩包绝对路径（.zip）。
 * @param targetDir 目标目录，不存在则自动创建。
 * @param roots 前缀（`overrides/` / `client-overrides/` 这种），**顺序有意义**：
 * 排后面的**覆盖**排前面的 —— 整合包的规矩是 client-overrides 盖过 overrides。
 * 传一个**空串**表示"整包解压"（前缀是空 ⇒ 条目名原样落进目标目录）—— 那是
 * "把 .minecraft 目录直接压进来的懒人包"那一路用的；同时给了空串和其它前缀时只按空串算。
 * @param extractedOut 非空时回填真正写出去的条目数（给"解开了 N 个覆盖文件"那句话用）。
 * @param errorOut 失败时的描述。
 * @param cancelled 取消判据（解大包时每半秒问一次）。
 * @param excludePrefixes 要**跳过**的条目前缀（**剥完前缀之后**的名字，`/` 分隔）。
 * 例如懒人包导入要跳过 `libraries/`、`versions/`、`logs/` —— 那些是本地运行产物，
 * 不是用户配置，带进新实例只会弄脏目录。给 `foo/` 是"整个目录"，给 `foo.txt`
 * 是"这一份文件"。
 * @return 全部成功返回 true。
 *
 * 不属于任何前缀的条目**一个都不碰**（清单文件、README 之类都不会落进实例目录）。
 */
bool extractSubtrees(const QStringList &archives, const QString &targetDir,
                     const QStringList &roots, int *extractedOut = nullptr,
                      QString *errorOut = nullptr,
                      const std::function<bool()> &cancelled = {},
                      const QStringList &excludePrefixes = QStringList());

/**
 * @brief 把一个 gzip 文件解成普通文件（@p targetPath 会被覆盖写）。成功返回 true。
 * @param sourceGz 待解压的 .gz 绝对路径。
 * @param targetPath 解出来的落点。父目录不存在时会建，已存在的文件**直接盖掉**。
 * @param errorOut 失败时的描述（没跑起来 / 超时 / 退出码 / PowerShell 自己那句异常）。
 * @return 成功返回 true。**失败不重试** —— 解不动就是解不动。
 *
 * 为什么单独开一个口而不是走 extractArchives()：那一条收的是 zip，gzip 不是 zip
 * （一个 gz 里通常就一份裸数据，没有条目名可挑）。.NET 里对应的是 GZipStream，
 * 接口对不上，硬塞进那个循环只会让两边都变难读。
 *
 * NOTE: 别把它当「通用解压」用 —— 这里**只管 gzip 这一层**：`.tar.gz` 解出来还是
 * 一份 tar，那层 tar 得调用方自己处理；zip 走 extractArchives()。
 */
bool gunzipTo(const QString &sourceGz, const QString &targetPath, QString *errorOut = nullptr);

/**
 * @brief 把一个普通文件压成 gzip（@p targetGz 会被覆盖写）。成功返回 true。
 * @param sourcePath 盘上的源文件（必须已经存在）。
 * @param targetGz 目标 .gz 的落点。父目录不存在时会建，已存在的文件**直接盖掉**。
 * @param errorOut 失败时的描述（同上）。
 *
 * `gunzipTo()` 的反方向，专为 **`level.dat` 写回去**这一条路加的：Minecraft 存档里那份
 * NBT 是 gzip 包着的（头两字节 `1f 8b`），改完树要原样压回去 —— 直接落一份裸 NBT 的话
 * 游戏会把这个存档当成坏的（那正是用户最怕的那件事）。
 *
 * NOTE: 它和 `gunzipTo()` 一样**只管 gzip 这一层**，不是"通用压缩"。
 * NOTE: 压缩级别是 Optimal（和 createZip 同一档）。这里 **不是** 逐字节可复现的：同一个
 * 源压两次的字节可能不一样（gzip 头里有个 mtime）。所以"改过的地方对不对"不能靠
 * 比 gz 的字节，要比**解开来之后**的 NBT 字节（见验证工程 V5 那一带）。
 */
bool gzipFrom(const QString &sourcePath, const QString &targetGz,
              QString *errorOut = nullptr);

}  // namespace ZipUtil
