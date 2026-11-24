/**
 * @file ServerRegistry.hpp
 * @brief 服务端工作区里**盘上那一份**：`<数据目录>/Servers` 底下现在
 * 有哪些服务端、每一个是哪一档、装好了没有、配置文件里写着什么。
 *
 * 【它和 ServerModel / ServerInstall 的分工】
 *
 * * `download/ServerInstall.hpp` —— 「**装**这一档要下什么、跑不跑安装器、装完是什么样」
 * （纯函数：Request → Plan，一个目录都不认识）；
 * * `server/ServerModel.hpp` —— 「一个服务端目录里的**字节**是什么意思」（`eula.txt`、
 * `server.properties`、我们记的那一份、状态那七档；纯函数，不读盘）；
 * * **本文件** —— 「那个根目录底下现在都住着谁」。**唯一一处碰盘的地方**：扫目录、
 * 读/写 `.wtfml-server`、读/写那两个配置文件、建目录、删目录。
 *
 * 分三层是因为这三件事的**测试方式**根本不一样：`ServerInstall` 靠喂 json 钉（verify 里
 * 早就有了）、`ServerModel` 靠喂字节钉、这一层靠在一个**临时目录**里真建真删钉（见 tests
 * 的 **ZX** 一节）。
 *
 * 【一个服务端 = 根底下的一个目录，目录名就是它的 id】
 *
 * 和 `versions/<id>` 同一个约定（`AppPaths::versionDir()` 那段）：名字里可以有中文、空格、
 * 全角括号（用户真机上就有 `Server1.20.2(modded)` 这种）。所以这一层**一个字节都不清洗**
 * 用户已经在用的名字 —— 只在**新建**时把用户敲进来的名字洗一遍（`sanitizeId()`）。
 *
 * 这一层**不认 `versions/`**、`versions/` 也不认它：两边的条目表各扫各的根。一个服务端
 * 目录住进 `versions/` 的话，实例页会凭空多出一个点进去什么都启动不了的"实例"。
 *
 * NOTE: 这一层里**没有 QObject、没有网络、不看时钟**（和 `UpdatePolicy` / `InstanceFilter`
 * 同一条规矩）：全是值 + 函数。所以验证工程能在一个临时目录里整段跑完。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "download/ServerInstall.hpp"
#include "server/ServerModel.hpp"

namespace ServerRegistry {

/**
 * @brief 服务端列表里的一行 —— **扫盘扫出来的事实**（不是我们记的那一份的照抄）。
 *
 * `serverId`（= 目录名）是这一行的身份；`name` 只是显示用的字符串。界面上认人（点开哪一行、
 * 启动哪一个）一律走 `serverId`，**不许拿 `name` 反查** —— 两个服务端可以叫同一个名字
 * （目录名不会重，显示名会）。
 */
struct Entry {
    QString serverId;  ///< 目录名（= 身份）
    QString dirPath;   ///< 绝对路径
    QString name;      ///< 显示名（我们记的那一份里没有就用 `serverId`）

    ServerInstall::Flavor flavor = ServerInstall::Flavor::Vanilla;
    /// 这个档是**我们记的**（true）还是**从文件反推的**（false）。反推的那一档界面上要
    /// 写着"这是猜的、点一下确认"。
    bool flavorKnown = false;
    /// 装好了没有（判据见 `ServerModel::isInstalled()`）。没装好时 `describe()` 里那句
    /// `note` 会说是**缺了哪一样**。
    bool installed = false;

    QString gameVersion;    ///< 游戏版本（我们记的；反推出来的档没有）
    QString loaderVersion;  ///< 加载器版本（同上）
    int port = -1;          ///< 端口（-1 = 读不出来 / 没这个文件）
    bool eulaAccepted = false;  ///< `eula.txt` 里写着接受了吗

    /// 这一行为什么看着不对劲（读不懂记的那一份 / 认不出是哪一档 / 没装好缺哪一样）。
    /// 空 = 一切正常。**唯一出处** —— 界面照抄，别自己另写。
    QString note;
};

/// 那个根目录：`AppPaths::serversDir()`。**唯一出处**（这一层别处不再现算）。
QString rootDir();

/// 确保根目录存在（新建第一个服务端之前要先有它）。返回 false 时 `*problemOut` 里有话说。
bool ensureRoot(QString *problemOut = nullptr);

/// 现在住在根底下的那些目录名（= serverId），**按名字排好序**。根不存在 = 空表（不是错误）。
QStringList serverIds();

/// 某个 serverId 的目录存在吗。
bool serverExists(const QString &serverId);

/// 把 @p serverId 那一个扫一遍，回一行事实。目录不在 = 回一行 `installed = false` 的空壳
/// （`note` 里会说目录不在）—— 界面照旧能把这一行画出来。
Entry describe(const QString &serverId, QString *problemOut = nullptr);

/// 根底下全部服务端，一行一个（按名字排序）。
QList<Entry> scanAll(QString *problemOut = nullptr);

// ---------------------------------------------------------------------------
// 我们记的那一份（`<服务端目录>/.wtfml-server`）
// ---------------------------------------------------------------------------

/// 读 @p serverId 我们记的那一份。文件不在 = 空 Info（**不是错误**：用户从外面拖进来的
/// 服务端就是这样）。`problemOut` 只在"文件在、但读不懂"时有话（那种情况下回空 Info + 那句
/// 毛病 —— 和 `UpdatePolicy::load()` 逐字同源）。
ServerModel::Info readMeta(const QString &serverId, QString *problemOut = nullptr);

/// 写（**原子**：QSaveFile）。目录不在时会先建。
///
/// ★ **合并写**：不是照 @p info 重建整份 —— 盘上那份读得懂就当底，只把已知字段盖上去，
/// 未知字段（更新的启动器写的、用户手写的）原样保留。盘上那份读不懂 / 超过 4 MiB 时
/// 没有底可用，这时才是一次整份写；要不要走到这一步由调用方决定（见 `setRestartOnCrash()`）。
bool writeMeta(const QString &serverId, const ServerModel::Info &info,
               QString *problemOut = nullptr);

/// 只改「异常退出后自动重启」那一格（右键菜单里那一项）。
///
/// ★ **读不懂就不写**：`readMeta()` 在"文件在、但读不懂"时回的是**空** `Info`，照那个
/// 写回去等于用一份最小合法记录顶掉盘上那份（游戏版本 / 内存 / 端口全没了）。所以这一格
/// 走这条专用路：先按 `readMeta()` 的 `problem` 判一下，有问题就原样留着、把那句话回给
/// 调用方。文件本来就不在（用户从外面拖进来的服务端）不算问题，照写。
///
/// NOTE: `writeMeta()` 自己也是"读盘上那份当底、再盖已知字段"的合并写（未知字段保得住），
/// 但**模型自己拥有的键先删后盖**（见 `ServerModel::ownedMetaKeys()`）—— 不先删的话，
/// 默认值（序列化时省略）就永远清不掉，例如关掉自动重启回读仍是开着的。
/// 所以这条路保护的是"读不懂 ⇒ 连一次写都不要发生"，而不是"只有它会保护未知字段"。
bool setRestartOnCrash(const QString &serverId, bool on, QString *problemOut = nullptr);

// ---------------------------------------------------------------------------
// 建 / 删
// ---------------------------------------------------------------------------

/// 把一个用户敲进来的名字洗成能当目录名的 id：去掉 `< > : " / \ | ? *` 与首尾空白和点、
/// 把控制字符换成 `_`；洗空了回空串（调用方得让用户重敲）。
QString sanitizeId(const QString &desired);

/// 一个还没被占用的 id：@p desired 被占了就在后面加 `-2`、`-3`…（找的是**目录**在不在，
/// 不是我们记的那一份在不在 —— 用户手建的空目录也算占着）。
QString uniqueServerId(const QString &desired);

/// 新建时默认的显示名：`Server<游戏版本>`（照用户真机上那份的叫法，`E:\MC\Server1.20.1`）。
/// 版本为空就回 `Server`。
QString defaultServerName(ServerInstall::Flavor flavor, const QString &gameVersion);

/// 建一个新服务端的目录、把我们记的那一份写进去。`serverId` 要是**洗过、且没被占**的
/// （调用方先走 `sanitizeId()` / `uniqueServerId()`）。
bool createServer(const QString &serverId, const ServerModel::Info &info,
                  QString *problemOut = nullptr);

/// 整个删掉一个服务端目录（连带里面那些文件）。
///
/// 这是**唯一一处会删用户东西的地方**：调用方必须已经在界面上问过用户、并且说清楚了
/// "它会连带 world/ 一起删"。这一层只做几道**安全闸**（不许是空 id、不许有路径分隔符、
/// 解出来的路径必须**严格在根目录底下**），保证它删不到根、也删不到根外面去。
bool removeServer(const QString &serverId, QString *problemOut = nullptr);

// ---------------------------------------------------------------------------
// 那两个配置文件（薄封装：判据全在 ServerModel）
// ---------------------------------------------------------------------------

/// `<服务端目录>/eula.txt`
QString eulaPath(const QString &serverId);
/// `<服务端目录>/server.properties`
QString propertiesPath(const QString &serverId);

/// 读 `eula.txt` 里那句"接受了没有"。文件不在 = false（服务端还没跑过）。
bool readEula(const QString &serverId);
/// 写一份**接受**的 `eula.txt`（内容见 `ServerModel::eulaFileText()`）。这是"用户主动勾了"
/// 之后才轮得到的那一下 —— 这一层**不问**，问是界面的事。
bool acceptEula(const QString &serverId, QString *problemOut = nullptr);

/// 读 `server.properties`（文件不在 = 空表 + 没有毛病）。
ServerModel::Properties readProperties(const QString &serverId, QString *problemOut = nullptr);
/// 写 `server.properties`（原子）。没有那个文件时会新建。
bool writeProperties(const QString &serverId, const ServerModel::Properties &properties,
                     QString *problemOut = nullptr);

}  // namespace ServerRegistry
