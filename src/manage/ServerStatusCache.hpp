/**
 * @file ServerStatusCache.hpp
 * @brief 把「每台服务器**上一次**问到什么样」记在盘上（图标 / MOTD / 人数 / 延迟）。
 * @note 要求：「测出来的 motd 和图案保留在缓存里」。
 *
 * 【为什么值得落盘】
 *
 * 一次测速要**真的连上去**（每台最多 4 秒超时），而服务器列表是"每次打开实例设置都会
 * 看见"的一页。没有缓存的话：断网、服务器正好在重启、或者只是手慢没等完，那一页就是
 * 一片「还没测」—— 而那些东西上一次明明问到过，图标和 MOTD 也不会一秒钟变一次。
 *
 * 【缓存是"上一次的样子"，不是"现在的事实"】
 *
 * 所以它只在**列表刚建出来、还没测**的那一小会儿顶上（当作"上次看到的"），一有真的
 * 结果就整条盖掉。**不许拿它当"当前状态"**：那一格写着几人在线，用户是会当真的。
 *
 * 【一份缓存一个文件】
 *
 * `<数据目录>/cache/servers/<地址的 sha1>.json`。用一个文件装全部的话，两台机器同时开
 * 就会互相盖；文件名用 sha1 而不是地址本身，是因为地址里有 `:` `/` `[` `]`（
 * `play.example.com:25566` 这种）—— 那些字符在文件名里能不能用要按平台问，不值得。
 *
 * NOTE: 这个文件**只读写自己那几个字节**，一件事都不做：连服务器、画图标、决定"要不要
 * 现在测一遍"全都不在这一层。
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ServerStatusCache {

/// 一台服务器上一次问到的东西。
struct Entry {
    /// 缓存里**真的有这一条**（假 = 没缓存过 / 文件坏了 / 地址认不出 —— 三者调用方要做
    /// 的事是同一件：当没缓存）。
    bool valid = false;
    /// 服务器自报的版本名（`Paper 1.21.4` 这种，原样，见 ServerPing::Status::versionName）。
    ///
    /// NOTE: 老缓存没有这个键 ⇒ 读出来是空串 ⇒ 悬停提示里少这一截。**不编造**
    /// （没问到过就是没问到过；空串和"服务器真报了个空名"在界面上是同一件事：不写）。
    QString versionName;
    /// 服务器自报的在线玩家名（已去掉 `§` 码，见 ServerPing::Status::samplePlayers）。
    ///
    /// NOTE: 老缓存没有这个键 ⇒ 空表 ⇒ 悬停提示里少「在线：…」那一截。读的时候最多留
    /// 32 个（手改过的缓存不该由它决定提示框多长）。
    QStringList samplePlayers;
    /// MOTD 的原样（带 `§` 码和换行，见 ServerPing::motdRaw）。
    QString motdRaw;
    /// 图标那串原始 PNG 字节（服务器没给图标就是空的）。
    QByteArray iconPng;
    /// 上一次**连上了没有**。假的那些里 `latencyMs` 必然是 -1 —— 但反过来不成立
    /// （连上了也可以量不出延迟），所以这一位得单独存。
    bool online = false;
    /// 上一次没通的那一档**是不是超时**（来自 `ServerPing::Status::timedOut`）。
    ///
    /// 界面上那一格对**超时**只写两个字（「超时」），缓存这一份也必须分开 ——
    /// 一律写「没连上」的话，同一台服务器"刚测完写超时、过一会儿再进这一页变成没连上"，
    /// 看着像结论变了。这一位存的就是"上一次没通是不是超时"：盘上那份没通**不存
    /// `error` 那句理由**（理由是当时那一刻的），可"是不是超时"是**这一格自己要说的话**。
    /// NOTE: 老缓存没有这个键 ⇒ 读出来是 false ⇒ 写「没连上」（那是对的：我们确实不知道
    /// 上一次是怎么没通的）。只有在线那一档它是恒 false 的。
    bool timedOut = false;
    int latencyMs = -1;
    int playersOnline = -1;
    int playersMax = -1;
};

/// 读一台的缓存。**读不动一律当"没有"**（文件不在 / JSON 坏了 / 地址是空串），
/// 不报错、不弹窗 —— 缓存读不出来只说明"这一格晚一点才有东西"。
Entry load(const QString &address);

/// 写一台的缓存。**写不进去也当没这回事**（磁盘满、目录建不出来）：缓存是锦上添花，
/// 为了它打断"测速"这一件事不值当。
void save(const QString &address, const Entry &entry);

/// 这一台的缓存文件在哪（**唯一出处**，验证工程靠它删干净/直接核对）。
/// 地址认不出（空串）时回空串。
QString cachePath(const QString &address);

}  // namespace ServerStatusCache
