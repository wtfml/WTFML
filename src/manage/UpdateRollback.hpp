/**
 * @file UpdateRollback.hpp
 * @brief 更新前的那个**轻量回滚点**：「把要被换掉的那几个文件先抄一份
 * 放边上，出事能退回来」。
 *
 * 【它和整份备份不是一回事】
 *
 * `BackupFiles` 打的是**一整份存档 / 一整个实例**的 zip（几百 MB、几千个文件、逐文件 sha1
 * 清单、走 PowerShell 打包）—— 那是"用户点了一下备份"那个动作要的东西。而这里的场景是
 * "用户点了更新，我要在他原来的那几份 jar 上动手"：要抄的只有**这一批真会被写到的文件**
 * （几个到几十个、每个几 MB），所以不进 zip、不打包、就一个目录里平铺一份目录树 + 一张小
 * 清单。**轻量**这两个字说的就是这件事。
 *
 * 【为什么必须有一张清单，而不是"把目录抄过去就完了"】
 *
 * 因为**"这个文件原来不在"也是一件事**：更新可能给实例**添**一个新文件（用户勾了一条
 * 新装的资源），退回来的时候它得**消失**，而不是留着。只有清单记得住"当时它在不在"。
 * 清单里还记大小和 sha1：退回来之后能照着对一遍账 —— "文件在那儿"和"文件是对的"是两件事
 * （`BackupFiles::verifyTree()` 那段同一个理由）。
 *
 * 【为什么一个实例只留最新那一份】
 *
 * 这个点的用处是**"退回按下这一下之前"**，不是"留一部更新史"。留多代意味着每更新一次就多
 * 攒几十 MB，而用户真正会用的永远是最近那一趟（上一趟早就没法用来说明"现在怎么回去"了）。
 * 所以 `create()` 会先把这一个实例上一趟那份整个删掉（`removeTreeWithoutFollowingLinks`，
 * 不跟链接），再摆新的。**只有一份**这件事还让界面那句话说得很干脆（「撤销上一次更新」）。
 *
 * 【落点】
 *
 * `<数据目录>/cache/update-rollback/<实例id>/` —— 和 `cache/config-backups` /
 * `cache/world-backups` 同一个约定（挂**数据根目录**下、可以整体拷走、游戏不认这个目录）。
 * 目录里：`wtfml-rollback.json`（清单）+ `files/<实例内相对路径>`（那几份原件）。
 * 那层 `files/` 是**故意**的：相对路径那一段原样保留下来之后，"这份回滚点是哪个文件的"
 * 一眼看得出，还原也不用另记一张表来映射（`ConfigFileText::backupPathFor()` 同一个路数）。
 *
 * 这一层不 include 任何 QWidget、不发信号、不碰账本：进度那几句由调用方（MainWindow）
 * 往任务中心和日志里写。
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace UpdateRollback {

/// 清单里的一条：这一趟碰过的一个文件。
struct Entry {
    /// 相对实例目录、`/` 分隔（和 `UpdatePolicy::storeKey()` 拼出来的是同一个形状）。
    QString relativePath;
    /// **当时盘上有没有它**。false ⇒ 还原 = 把它删掉（更新新添的那一份要能收回去）。
    bool existed = false;
    qint64 size = 0;
    /// 抄下来那一份的内容 sha1（小写十六进制）。`existed = false` 时是空的。
    QString sha1;
};

/// 一份回滚点（清单）。**纯值**。
struct Snapshot {
    QString instanceId;
    QDateTime createdAt;
    /// 这一份落在哪个目录（`snapshotDir(instanceId)`）。
    QString dir;
    QList<Entry> entries;

    bool isValid() const { return !dir.isEmpty() && !instanceId.isEmpty() && !entries.isEmpty(); }
    int fileCount() const {
        int n = 0;
        for (const Entry &entry : entries) {
            if (entry.existed) {
                ++n;
            }
        }
        return n;
    }
    /// 这一份抄下来占了多少字节（界面上那句「NN MB」和"要不要清"都看它）。
    qint64 totalBytes() const {
        qint64 n = 0;
        for (const Entry &entry : entries) {
            if (entry.existed) {
                n += entry.size;
            }
        }
        return n;
    }
    /// 这一份里有没有需要"删掉"的条目（更新新添的文件）。撤销那句话说清这一点用。
    int addedCount() const {
        int n = 0;
        for (const Entry &entry : entries) {
            if (!entry.existed) {
                ++n;
            }
        }
        return n;
    }
};

/// 回滚点根目录：`<数据目录>/cache/update-rollback`。
QString rootDir();
/// 某一个实例那一份落在哪儿：`rootDir()/<实例 id>`。★ 实例 id 里可能有 `/` 这种字面量
/// （`isKnownGameVersion()` 那种放行的名字），所以这一层会把它换成合法的一段（见 .cpp）。
QString snapshotDir(const QString &instanceId);
/// 清单在那一份里叫什么（唯一出处）。
QString manifestName();
/// 清单第一行那个格式标记。
QString formatTag();

QByteArray manifestToJson(const Snapshot &snapshot);
/// @param ok 回"读不读得动"（和"是不是我们写的"分开：两句不同的错）。
Snapshot manifestFromJson(const QByteArray &bytes, bool *ok = nullptr);

/**
 * @brief 摆一份回滚点（**会先把这一个实例上一趟那份删掉**）。
 *
 * @param instanceDir 实例目录（`versions/<id>`）。
 * @param instanceId 实例 id（只用来记账 / 拼目录名）。
 * @param relativePaths 这一趟**会碰到**的那些文件，实例内相对路径。
 * @param problemOut 失败原因。
 * @return `isValid()` 为假 = 没摆成（调用方**必须当场停下来**，理由见下）。
 *
 * 抄不出来就别往下走：这一整套存在的意义就是"动手之前手上有一份旧的"。抄不动（盘满、
 * 文件被别的程序锁着）还硬往下更新，等于把"能退回来"这句话删掉之后再动手 —— 那比
 * 一开始就没这个功能更糟（用户是看着这句话才按下去的）。
 * 清单里的路径**先过一遍合法性**（不许绝对路径、不许 `..`、不许盘符）：那些路径是我们
 * 自己按目录走出来的，真出现一个不合法的，那是**上面那一层算错了**，不是用户干了什么 ——
 * 当场拒绝，比往实例目录外面写/删东西强。
 */
Snapshot create(const QString &instanceDir, const QString &instanceId,
                const QStringList &relativePaths, QString *problemOut = nullptr,
                bool keepPrevious = false);

/// 这一批**真的开起来了** ⇒ 上一趟那份可以扔了（`create(..., keepPrevious=true)` 之后调）。
///
/// `create()` **不能"先把上一趟那份删掉、再摆新的"**：那样"这一批没开起来"
/// （下载器正忙、另一套安装状态机占着、接口给的文件名不能用）那个分支虽然一个字节都没
/// 动过，上一趟的回滚点却已经没了 —— 用户失去了"撤销上一次更新"这个能力。调用方
/// 传 `keepPrevious = true` 时：旧的那份先改名成 `<dir>.previous`，摆新的失败就还回去；
/// 摆好了但这一批被拒（`discard()`）也还回去；只有这一批真的开始了才在这里删掉。
/// 默认 false：只留最新那一份，不留旧账。
bool commit(const QString &instanceId);

/// 读盘上那一个实例的回滚点（`snapshotDir()` + 清单）。**不读那份清单就不是"有一份"**：
/// 清单读不动 = 这一份回滚点用不了（数据不全的东西不能拿去还原）。
/// @param ok 回"读没读到一份能用的"。
Snapshot read(const QString &instanceId, bool *ok = nullptr, QString *problemOut = nullptr);

/// 把 @p snapshot 整个删掉（撤销成功之后、或者用户自己清）。回"删干净了没有"
/// （本来就没有 = true）。
bool discard(const QString &instanceId);

/// 还原的结果。
struct RestoreResult {
    int restored = 0;   ///< 放回去几个
    int removed = 0;    ///< 删掉几个（更新新添的、当时不在的那些）
    int failed = 0;     ///< 几个没能动
    QStringList problems;  ///< 逐条原因（去过重）
    QHash<QString, QString> restoredSha1;  ///< 放回成功的：相对路径 → 内容 sha1
    bool ok() const { return failed == 0; }
};

/**
 * @brief 照着 @p snapshot 把 @p instanceDir 里的文件退回更新前。
 *
 * 逐条做：当时在的 → 从 `files/` 抄回去（**覆盖**现在那一份）；当时不在的 → 删掉。
 *
 * **一条失败不影响其余**：能退几条退几条，最后按 `failed` 说话（"有 3 个没能退回去"比
 * "失败了，一个都没动"诚实 —— 用户手上现在是半新半旧的，这件事必须看得见）。
 * 抄回去用的是 `QSaveFile`（目标旁写临时文件、commit 时原子替换），**不是**
 * `QFile::remove` + `QFile::copy`：后者删完目标再拷，中途断电就只剩半截，用户当前文件和
 * 回滚文件同时丢失。每次放回去之后还照清单里记的 sha1 对一遍 —— 文件在那儿不等于它是
 * 对的（对不上按失败记，不装作退回去了）。
 */
RestoreResult restore(const Snapshot &snapshot, const QString &instanceDir);

}  // namespace UpdateRollback
