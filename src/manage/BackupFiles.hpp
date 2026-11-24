/**
 * @file BackupFiles.hpp
 * @brief 世界与实例的**备份**：打一份包、读它的清单、还原它。
 *
 * 【为什么单独一层】
 *
 * 和 `WorldFiles` / `VersionIntegrity` 同一个理由：界面层（ui/）只发信号、不碰盘，而这些
 * 动作（走一遍目录、算 sha1、起 PowerShell 打包、还原前先挪走现在的内容）全是要碰盘的，
 * 收在 src/manage 里还有一个好处 —— 验证工程能绕开"点按钮 → 弹原生选择框"那条路，
 * 直接把这些函数调起来，把"没装完的那一份会不会被当成好的""还原失败之后原来的东西还在不在"
 * 这些事钉死。
 *
 * 【一份备份长什么样】
 *
 * 一个 zip，**里面所有条目都在一层前缀底下**（世界备份是 `<世界名>/…`，实例快照是
 * `<实例目录名>/…`），加一份躺在 zip 根的清单 `wtfml-backup.json`。
 *
 * 那一层前缀不是装饰：还原走的是 `ZipUtil::extractSubtrees()`（**前缀剥掉**），所以
 * 解出来的东西正好是"这个目录里原来那些文件"，而躺在根上的清单**一个字节都不会落进
 * 用户的世界目录**（不属于那个前缀的条目那一趟根本不碰）。这也是**为什么清单不放在
 * 前缀里面**：放在里面就会被解出去。
 *
 * 【清单为什么记到每个文件】
 *
 * 要清单记「来源实例 id、时间、游戏版本、包含范围和校验值」。校验值落在**每个文件**
 * 上（名字 + 大小 + sha1），因为 zip 里那份清单**没法**记自己这个 zip 的哈希（先有清单
 * 才能装包）。逐文件那一份还有第二个用处：还原之后可以**照着它对一遍账** —— 解压到一半
 * 失败留下的半截文件，只有这样才认得出（"文件在那儿"和"文件是对的"是两件事）。
 *
 * NOTE: 逐文件清单对一个大存档（几千个 region 文件）是几百 KB 的 JSON，这是**故意**的：
 * 它换来的正是"还原之后能说一句'三千二百个文件都对上了'"。真嫌大，砍的应该是
 * `files` 而不是 `sha1` 这一层的意思 —— 但那时候得先想清楚"凭什么信这一份备份"。
 */

#pragma once
#include <functional>

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace BackupFiles {

/// 这一份备份收的是哪一档东西。**决定它落在哪个子目录里、也决定还原到哪儿**。
enum class Kind {
    World,     ///< 一个存档（`<实例>/saves/<世界名>`）。
    Instance,  ///< 一个实例（默认排除可以重新下回来的那一堆）。
};

/// 备份里的一个文件。
struct FileRecord {
    QString path;   ///< **相对那一层前缀**，`/` 分隔（`region/r.0.0.mca`）
    qint64 size = 0;
    QString sha1;   ///< 小写十六进制
};

/// 一份备份的清单（`wtfml-backup.json`）。**纯值**：不碰盘、不联网。
struct Manifest {
    Kind kind = Kind::World;
    /// zip 里那一层前缀，也是还原时落回去的那个目录名（世界名 / 实例目录名）。
    QString root;
    /// 文件名里那个词（世界名 / 实例名）。**只用来拼文件名**，不参与还原。
    QString label;
    /// 来源实例 id（清单里要求的）。世界备份也记 —— 世界是从哪个实例里备出来的。
    QString sourceInstanceId;
    QString sourceInstanceName;
    /// 游戏版本那一句（「1.20.1（Fabric 0.15.11）」这种，拼好之后存进来）。
    QString gameVersion;
    /// 世界备份才有；实例快照留空。
    QString worldName;
    /// **完整快照**（没排除任何东西）。实例快照默认不是。
    bool complete = false;
    /// 这一份是**还原之前自动做的那一份**（安全快照），不是用户点出来的那一份。
    /// 界面上两者不加区分，这个字段是给"以后要在一处列表里分堆"留的口子，
    /// 也是给判据用的（验证工程要认得出"哪一份是自动做出来的"）。
    bool safety = false;
    QDateTime createdAt;
    /// 包含范围（每条一句人话，直接摆给用户看）。
    QStringList scope;
    /// **排除掉**的顶层目录名（完整快照时是空的）。
    QStringList excluded;
    QList<FileRecord> files;
    qint64 totalBytes = 0;

    int fileCount() const { return int(files.size()); }
    /// 认得出来的一份清单（有格式标记、有前缀、有时间）。读盘读坏 / 拿一个别人的 zip
    /// 来还原时靠它说话。
    bool isValid() const;
};

/// 清单在 zip 根叫什么。**唯一出处**（写和读都走它）。
QString manifestEntryName();

/// 清单第一行那个格式标记。**写死一个字符串**而不是"看有没有哪个字段"，这样以后改格式
/// 能一眼认出来（认不出的包按"不是我们做的"处理，绝不半信半疑地往里解）。
QString formatTag();

QByteArray manifestToJson(const Manifest &manifest);
/// 读一份清单。@p ok 回"读不读得动"（**和"是不是我们做的"分开**：解析失败和
/// 格式对不上是两句不同的错）。
Manifest manifestFromJson(const QByteArray &bytes, bool *ok = nullptr);

/// 备份根：`<当前游戏文件夹>/cache/backups`（= `AppPaths::backupDir()`，那边写了为什么
/// 是 `cache/` 底下而不是实例里面）。
QString backupRoot();
/// 备份落在哪个子目录：世界 `cache/backups/worlds`、实例 `cache/backups/instances`。
QString backupDir(Kind kind);
/// 某个 label 的备份文件名**都**以它开头（`<换了非法字符的 label>-`）。
///
/// 为什么要有这么一个"半个名字"：界面上要摆一份"这个世界 / 这个实例的备份"清单，而
/// **读一份清单要起一次 PowerShell**（`ZipUtil::readEntry` 那把锤子）—— 几十份备份就是
/// 几十次进程起停，用户点一下「恢复…」要等好几秒。文件名前缀是个便宜的筛子：先按它缩小
/// 范围，等用户真挑了一份再读那一份的清单（那时才需要验它的 `root` 对不对）。
QString backupFilePrefix(const QString &label);

/// 备份文件名：`<label>--101530.zip`（**一定是合法文件名** —— label 里的非法
/// 字符先过一遍替换，见 .cpp）。`safety` 为真时中间多一个「恢复前」。
QString backupFileName(const QString &label, const QDateTime &at, bool safety = false);

/// 把挑备份那一段要摆出来的那几份**按时间从新到旧**排好（`backupFileName()` 的逆运算：
/// 认名字里那串定宽时间戳，**不是**按整个文件名字典序）。
///
/// 为什么不能直接按文件名字典序：自动留下的「恢复前-」安全快照在名字里多插了一段汉字，
/// 汉字码位在数字之后 ⇒ 每一份安全快照都会排到最新的常规备份前面，"越上面越新"就反了。
/// 同一秒里自动那份排前面（它就是用户刚点的那份改动之前的原样，一眼要看得见）。
/// 名字里认不出时间戳的那几份排在最后（按名字倒序，稳定可复现）。
QStringList sortBackupsNewestFirst(const QStringList &paths, const QString &prefix);

/// 一份备份在那一列里怎么写大小（「12.4 MB」）。
///
/// **自己算**，不用 `QLocale::formattedDataSize()`：那个的写法跟着系统语言走
/// （"1.5 MiB" / "1,5 MB"），而这几句要在判据里逐字断言 —— 跟着区域设置变的字符串没法钉。
///
/// NOTE: 这个格式原来在**三处**各有一份（世界页、`ServerManagementActions`、以及搬
/// 「服务端设置」时新分出来的那一份）—— 三份一模一样却谁也不认识谁，改一处不会有任何
/// 报错。现在只有这一份。
QString sizeText(qint64 bytes);

/// @p fileName 是不是"还原之前自动做的那一份"（安全快照）。
///
/// 从**文件名**认，不去读清单：读一份清单要起一次 PowerShell，而这一列里可能摆着几十份
/// （见 `backupFilePrefix()` 那段）。那个记号本身是 `backupFileName()` 定的 ——
/// 「恢复前-」夹在前缀和时间戳中间。
bool nameIsSafety(const QString &fileName, const QString &prefix);

/// 走一遍目录，把要记进清单的文件列出来（**按路径排序**，可复现）。
/// @param rootDir 要走的目录（必须存在）。
/// @param excludeTop 要跳过的**顶层目录名**（实例快照排 assets / libraries 用的就是它）。
/// @param problemOut 失败原因（目录不在 / 读不动）。**跳过的东西不算失败**。
/// @return 空表 + problemOut 非空 = 没走成；空表 + problemOut 空 = 这个目录本来就是空的。
QList<FileRecord> collectFiles(const QString &rootDir, const QStringList &excludeTop = QStringList(),
                               QString *problemOut = nullptr,
                               const std::function<bool()> &cancelled = {});

/// 打一份备份。
/// 先在备份目标同卷创建私有暂存副本并校验，再复查源清单；变化时拒绝发布、提示重试。
/// 压缩仅使用暂存副本，成功后才改名发布；需要暂存和归档两份空间，取消/失败自动清理。
/// @param manifest 描述（`files` / `totalBytes` 由这一趟自己填，传进来的一律被覆盖）。
/// @param sourceDir 盘上要打进去的那个目录。
/// @param targetZip 落点（父目录会自动建）。
/// @param problemOut 失败原因。**失败时盘上不会留下半成品**（`ZipUtil::createZip` 自己删）。
/// @return 打出来的那一份清单（`ok=false` 时 = 上面那份描述原样返回，别拿它当成功的凭据，
/// 认 `problemOut`）。
struct WriteResult {
    bool ok = false;
    Manifest manifest;
    QString problem;
    QString zipPath;
};
WriteResult writeBackup(const Manifest &manifest, const QString &sourceDir,
                        const QString &targetZip, const std::function<bool()> &cancelled = {});

/// 读一份备份的清单（从 zip 里那个根条目）。**不是我们做的包**和**读不动**在这里是两件事
/// （两句话不一样），调用方只要看 `ok`。
Manifest readManifest(const QString &zipPath, bool *ok, QString *problemOut);

/// 还原一份备份。
struct RestoreResult {
    /// 走到哪一步了。**成功也是它**（`Ok`）—— 下面那几步按这个顺序一字排开，调用方
    /// （任务账本上那几步）拿它标"第几步红了"，不用去猜 `problem` 那句话里说到了谁。
    ///
    /// NOTE: 加这一格是因为 `problem` 是**给人看的**（措辞随时会改），而"哪一步"是给
    /// 账本看的 —— 从一句中文里抠关键字来判，改一个字就静默错位。
    enum class Stage {
        Ok,             ///< 走完了（`ok` 那一格才算数）
        ReadManifest,   ///< ① 读清单
        Space,          ///< ② 动盘之前：这块盘放不放得下（**这一档盘上一个字节都没动**）
        SafetySnapshot, ///< ③ 给"现在这一份"做安全快照
        RemoveOld,      ///< ③ 把"现在这一份"整个挪走
        Extract,        ///< ④ 解压
        Verify,         ///< ⑤ 逐文件对账（走完了但没对上）
    };

    bool ok = false;
    Stage stage = Stage::Ok;
    bool recoveryFailed = false; ///< 自动归位失败，必须报告错误，不能以用户取消掩盖。
    QString problem;      ///< 失败原因（人话，直接摆给用户）
    QString safetyZip;    ///< 这一趟自动做出来的安全快照（没做 = 空）
    QString retainedPath; ///< 发布成功但旧目录清理失败时的保留路径（应向用户提示）。
    int checked = 0;      ///< 还原之后逐文件对账对了几条
    int mismatched = 0;   ///< 对不上的条数；>0 时不发布暂存内容，原目录保持不变。
    QString targetPath;   ///< 真正落回去的那个目录
};

/**
 * @brief 把 @p zipPath 里 `root/` 那一段还原到 @p targetParentDir 底下。
 *
 * 读清单和空间预检 → 原目录完整安全快照 → 同卷暂存解压及逐文件校验 →
 * 复查原目录未被外部修改 → 将旧目录改名保留 → 发布已校验的新目录。
 * 解压、校验、取消或发布失败不先删除原数据；发布失败尝试将旧目录归位。
 * 归位失败时旧目录留在独立路径，错误信息给出位置，绝不由暂存清理删除。
 * 发布后才清理旧目录；清理失败通过 retainedPath 提示。目标含链接或同名文件拒绝操作。
 * 两次改名不是文件系统级原子交换；受管数据目录通过事务日志做保守的启动恢复。
 * 外部目录不自动恢复；新旧目录并存时不覆盖任何一份，保留日志供人工检查。
 *
 * NOTE: 这里**不碰界面也不碰账本**：进度那几句由调用方（MainWindow）往任务中心写。
 *
 * @param cancelled 协作式停止，覆盖快照、解压、校验与提交前检查。
 * 仅最终两次改名及必要回滚不响应取消，避免留下半次提交。
 */
/// 同步进度通知；提交区间不接受取消，观察者不得抛出异常或修改用户目录。
enum class RestorePhase { Prepared, OriginalRetained, Published, Committed };
using RestoreObserver = std::function<void(RestorePhase)>;
RestoreResult restoreBackup(const QString &zipPath, const QString &targetParentDir,
                            const std::function<bool()> &cancelled = {},
                            const RestoreObserver &observer = {});

/// 启动/切换数据目录时在扫描实例之前调用；只恢复本数据目录内已验证的中断还原。
/// 不覆盖新出现的目标，不按前缀删除目录，活动事务跳过。
QStringList recoverInterruptedRestores(const QString &gameDir);

/// 还原这一份备份大约要占多少字节 = 清单里各文件大小之和 + 一点余量（余量见 .cpp）。
/// 纯函数（不碰盘）：`restoreBackup()` 拿它对着空闲空间判，验证工程拿它钉判据。
qint64 restoreSpaceNeeded(const Manifest &manifest);

/// @p freeBytes 够不够放 @p neededBytes。
/// @p freeBytes < 0 = **问不出来**（卷没准备好 / 认不出文件系统）⇒ 回 `true`（**不拦**）：
/// 宁可让它走下去、在解压那一步如实失败，也不要凭一个假数字把用户挡在门外。
bool hasRoomFor(qint64 freeBytes, qint64 neededBytes);

/// 对账：@p dir 底下那些文件跟 @p files 里记的名字/大小/sha1 对不对得上。
/// @return 对不上的条数（0 = 全对上）。**多出来的文件不算**（备份是"至少恢复这些"），
/// 缺一个或内容不一样都算。
int verifyTree(const QString &dir, const QList<FileRecord> &files,
               const std::function<bool()> &cancelled = {});

/// 那一句话（「包含范围」摆给用户看的那几句）拼好之后的形状：`saves/基地周目（1234 个文件）`。
/// 世界备份和实例快照各拼各的，这里只提供"范围里那一条"怎么写的唯一出处。
QString scopeLine(const QString &what, int fileCount);

}  // namespace BackupFiles
