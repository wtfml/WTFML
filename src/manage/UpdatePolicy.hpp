/**
 * @file UpdatePolicy.hpp
 * @brief 「忽略这一版」「固定这一版」—— 更新中心里那两档"别再提示我"的
 * 判据，以及它们在盘上存的那一份（`<数据目录>/wtfml.updates`）。
 *
 * 【两档不是一回事，所以是两条记录、不是一条】
 *
 * * **忽略某一版**（`ignoreVersionId`）：用户在"可更新"那一行上按下的是「忽略这一版」——
 * 意思很具体："**这一版**我不想要，别拿它来提示我"。所以记的是**要换上的那一版的 id**，
 * 判据是"这一趟要换上的还是它吗"。作者过两天发了更新的那一版，这一条**自己就回来了**
 * （那正是用户想要的：他不讨厌新版本，他讨厌**这**一个）。
 * * **固定某一版**（`pinnedSha1`）：用户按下的是「固定这一版」——意思是"我就停在这儿了，
 * 不再提示更新"。所以记的是**盘上那一份当时的内容 sha1**，判据是"盘上还是那一份吗"。
 * 用 sha1 而不是版本号，是因为**判据手上只有 sha1**：更新检测靠反查 sha1 认人
 * （见 `InstanceIconLookup` / `ModDependencies::judgeUpdate`），"盘上这一条是版本
 * x.y.z"这个事实从来没有被算出来过 —— 编一个不存在的判据，就是这个功能塌掉的形状。
 * 副作用是好的：用户自己把文件换掉了（手动更新、或者再点一次更新），sha1 一对不上，
 * 固定**自动失效** —— 他说的是"停在那一份上"，不是"永远别看更新"。
 *
 * 【为什么记的是"实例内相对路径"，不是一个绝对路径】
 *
 * 实例目录可以被整体搬走（改数据目录、复制实例、拿 U 盘拷），绝对路径一搬就全废。相对路径
 * 还有一个好处：**"这一条是哪个文件"这件事在界面上本来就是按相对路径说的**（文件管理页
 * 那一行、更新中心那一行）。`storeKey()` 是拼它的**唯一出处**。
 *
 * 【为什么".disabled"要摘掉】
 *
 * 和文件管理页那把钥匙（`InstanceFilesPage::identityPath()`）同一条：禁用只是把 `X.jar`
 * 改名成 `X.jar.disabled`，内容一个字节都不变，用户在界面上看到的还是同一条。不摘的话，
 * "忽略过、然后禁用了一下、再启用"就会把那条记录甩掉，用户得再忽略一次。★ 摘这一下在
 * 调用方（MainWindow 手上只有 `InstanceFilesPage::identityPath()`）—— 这一层不 include ui/。
 *
 * 这个头文件里**没有 QObject、没有网络、不看时钟**：纯值 + 纯函数 + 一次文件读写。所以
 * verify 能直接喂一份 json 进来、在一个临时目录里真存真读（见 tests/main.cpp 的 ZT 节）。
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>

namespace UpdatePolicy {

/// 某一个文件的记录。**两个都空 = 这一条没有记录**（别在表里留空壳，见 `setIgnored()`）。
struct Entry {
    /// 「忽略这一版」：要跳过的**那一版**的 id（`ResourceCatalog::VersionEntry::id`）。
    QString ignoreVersionId;
    /// 「固定这一版」：用户按下那一刻盘上那一份的内容 sha1（小写十六进制）。
    QString pinnedSha1;

    bool isEmpty() const { return ignoreVersionId.isEmpty() && pinnedSha1.isEmpty(); }
};

/// 一本账：实例 id → （实例内相对路径 → 记录）。
struct Store {
    QHash<QString, QHash<QString, Entry>> byInstance;

    /// 这个实例记了几条（两档合起来）——界面上那句「N 条已忽略 / 已固定」的数就是它。
    int countFor(const QString &instanceId) const {
        return byInstance.value(instanceId).size();
    }
    /// 一条记录都没有的实例不留在表里（`save()` 之前会清一遍，见 .cpp 里那段）。
    bool isEmpty() const { return byInstance.isEmpty(); }
};

/// 这一条**现在**为什么不提示更新。
enum class Suppress {
    None,     ///< 没有记录 / 记录对不上这一趟的结论 ⇒ 照常提示
    Ignored,  ///< 记着的那一版就是要换上的那一版
    Pinned,   ///< 盘上那一份还是固定的时候那一份
};

/// 为什么这一条现在不提示 —— **唯一判据**（界面上的「已忽略 / 已固定」和"它还进不进可更新
/// 名单"都走它，不再各判一次）。
/// @param entry 这一条文件的记录（没有就传空的那个）。
/// @param currentSha1 盘上这一份现在的内容 sha1（空 = 不知道 ⇒ 固定那一档**判不了**，
/// 按"没固定"算 —— 不知道的事不能当成知道）。
/// @param targetVersionId 这一趟检测要换上的那一版（空 = 拼不出来 ⇒ 忽略那一档同样判不了）。
Suppress suppressed(const Entry &entry, const QString &currentSha1,
                    const QString &targetVersionId);

/// 「固定」这一条是不是**已经过期**了（记录在，但盘上那一份已经不是当时那一份）。
/// 过期的那几条在下一次检测时清掉（见 `dropStalePins()`）—— 留着的话，用户手动换过一遍
/// 之后那条记录会一直挂着，而他再也看不到"它其实已经失效了"。
bool pinIsStale(const Entry &entry, const QString &currentSha1);

/// 把 @p instanceId 底下**已经过期**的固定清掉（忽略那一档不清：它记的是"那一版"，只要
/// 那一版还在，用户的意思就还成立）。返回清掉了几条。
int dropStalePins(Store &store, const QString &instanceId,
                  const QHash<QString, QString> &currentSha1ByKey);

/// 记/取消「忽略这一版」。@p targetVersionId 传空 = 取消这一条忽略。
void setIgnored(Store &store, const QString &instanceId, const QString &key,
                const QString &targetVersionId);
/// 记/取消「固定这一版」。@p pinnedSha1 传空 = 取消固定。
void setPinned(Store &store, const QString &instanceId, const QString &key,
               const QString &pinnedSha1);

/// 读一条（没有就是空的那个）。
Entry entryOf(const Store &store, const QString &instanceId, const QString &key);

/// 一个文件在这本账里的键：**相对实例目录、`/` 分隔**。
/// @param absolutePath 要已经是"那把钥匙"（`.disabled` 由调用方先摘掉，见文件头那段）。
/// @return 空串 = 这个路径不在 @p instanceDir 底下（**调用方必须跳过它**，不许拿空键去记：
/// 空键会把好几条不相干的记录混成一条，而且它还删不掉）。
QString storeKey(const QString &instanceDir, const QString &absolutePath);

/// 账本落在哪儿：`<数据目录>/wtfml.updates`（和 `wtfml.folders` / `wtfml.history` 同一族：
/// 挂在数据根目录下、可以整个拷走）。
///
/// NOTE: 和那两位一样**不叫 .json** —— 它是启动器自己的配置，混在别的 json 里早晚有人拿它
/// 当某个版本的元数据去读（`AppPaths::instanceGroupFilePath()` 那段同一个理由）。
QString storePath();

/// 格式标记（写第一个字段、读的时候先看它）：认不出的文件按"这份账读不懂"处理，
/// **绝不半信半疑地往里加记录**。
QString formatTag();

QByteArray toJson(const Store &store);
/// @param ok 回"这一份读得懂吗"。
///
/// **`ok = false` 有两种情形**（`load()` 那句毛病说的是后者，两者共用同一条"读不懂"的
/// 路）：根本不是一个 json 对象；**或者**是个 json 但格式标记 / 版本号不认（别人写的、
/// 或者以后改了格式的）。两种都回空表 —— 差别只在那句话怎么写。
/// 空文件回**空表 + `ok = true`**（那是真读懂了：里面什么都没有）。所以调用方按 `ok`
/// 办事时，"读不懂"那句话一定会被说出来，而不是被当成"这个用户从来没忽略过谁"。
Store fromJson(const QByteArray &bytes, bool *ok = nullptr);

/// 读盘。文件不在 = 空表（**不是错误**：从来没忽略过谁就是这样）。
Store load(const QString &path, QString *problemOut = nullptr);
/// 写盘（**原子**：QSaveFile）。空表也会写出去（把上一次那份清干净）。
bool save(const Store &store, const QString &path, QString *problemOut = nullptr);

}  // namespace UpdatePolicy
