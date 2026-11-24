#include "Support.hpp"

void runServerPlayersTests() {
    section("ZAE. 服务端那四份玩家名单：读写 / 认人 / 离线 UUID");

    // ---------- ZAE0. 四份名单的身份 ----------
    {
        check(ServerPlayers::listCount() == 4, "ZAE0 四份名单（白名单 / 管理员 / 封禁的玩家 / 封禁的 IP）",
              QString::number(ServerPlayers::listCount()));
        const QStringList files = {
            ServerPlayers::fileName(ServerPlayers::List::Whitelist),
            ServerPlayers::fileName(ServerPlayers::List::Ops),
            ServerPlayers::fileName(ServerPlayers::List::BannedPlayers),
            ServerPlayers::fileName(ServerPlayers::List::BannedIps),
        };
        check(files == QStringList({QStringLiteral("whitelist.json"), QStringLiteral("ops.json"),
                                    QStringLiteral("banned-players.json"),
                                    QStringLiteral("banned-ips.json")}),
              "★★ ZAE0 四个文件名就是服务端自己用的那四个（写错一个名字 = 界面上改了、游戏里没变）",
              files.join(QStringLiteral(",")));
        const QStringList titles = {
            ServerPlayers::title(ServerPlayers::List::Whitelist),
            ServerPlayers::title(ServerPlayers::List::Ops),
            ServerPlayers::title(ServerPlayers::List::BannedPlayers),
            ServerPlayers::title(ServerPlayers::List::BannedIps),
        };
        check(!titles.contains(QString())
                  && QSet<QString>(titles.begin(), titles.end()).size() == 4,
              "ZAE0 四格标题都有字、互不相同", titles.join(QStringLiteral(",")));
    }

    // ---------- ZAE1. 读：空文件 = 空名单；读不懂就说毛病，不猜 ----------
    {
        for (int i = 0; i < ServerPlayers::listCount(); ++i) {
            const auto list = static_cast<ServerPlayers::List>(i);
            const ServerPlayers::Content empty = ServerPlayers::parse(list, QByteArray());
            check(empty.ok && empty.entries.isEmpty(),
                  "★ ZAE1 文件不在（空字节）⇒ 空名单，`ok` 仍然是 true（新服务端四份文件都不存在）",
                  ServerPlayers::fileName(list));
            const ServerPlayers::Content blank = ServerPlayers::parse(list, QByteArrayLiteral("  \r\n"));
            check(blank.ok && blank.entries.isEmpty(),
                  "ZAE1 只有空白同样算「没有东西」", ServerPlayers::fileName(list));
        }
        const ServerPlayers::Content broken =
            ServerPlayers::parse(ServerPlayers::List::Whitelist, QByteArrayLiteral("[{\"uuid\":"));
        check(!broken.ok && !broken.problem.isEmpty() && broken.entries.isEmpty(),
              "★★ ZAE1 读不懂的 JSON ⇒ `ok = false` + 一句毛病（**不是**空名单 —— 那会让"
              "「写回去」把用户手改坏的记录抹掉）", broken.problem);
        check(broken.problem.contains(QStringLiteral("whitelist.json"))
                  && broken.problem.contains(QStringLiteral("没有改动它")),
              "★ ZAE1 那句毛病点了文件名、并且说清楚**没有改动它**", broken.problem);
        const ServerPlayers::Content notArray =
            ServerPlayers::parse(ServerPlayers::List::Ops, QByteArrayLiteral("{\"a\":1}"));
        check(!notArray.ok && notArray.problem.contains(QStringLiteral("应为数组")),
              "★ ZAE1 不是数组（手改成一个对象）⇒ 同样 `ok = false`，不猜", notArray.problem);
    }

    // ---------- ZAE2. 读：四份文件的真实字段各就各位 ----------
    {
        const ServerPlayers::Content white = ServerPlayers::parse(
            ServerPlayers::List::Whitelist,
            QByteArrayLiteral("[{\"uuid\":\"b50ad385-829d-3141-a216-7e7d7539ba7f\",\"name\":\"Notch\"},"
                              "{\"uuid\":\"\",\"name\":\"Herobrine\"}]"));
        check(white.ok && white.entries.size() == 2, "ZAE2 白名单两条都读出来",
              QString::number(white.entries.size()));
        check(white.entries.value(0).uuid
                      == QStringLiteral("b50ad385-829d-3141-a216-7e7d7539ba7f")
                  && white.entries.value(0).name == QStringLiteral("Notch"),
              "ZAE2 白名单的 `uuid` / `name` 一字不差",
              white.entries.value(0).uuid);
        check(white.entries.value(1).uuid.isEmpty()
                  && white.entries.value(1).name == QStringLiteral("Herobrine"),
              "★ ZAE2 `uuid` 空着也照样读（认人的时候退回名字 —— 见下面 ZAE4）");

        const ServerPlayers::Content ops = ServerPlayers::parse(
            ServerPlayers::List::Ops,
            QByteArrayLiteral("[{\"uuid\":\"u-1\",\"name\":\"alice\",\"level\":2,"
                              "\"bypassesPlayerLimit\":true},{\"uuid\":\"u-2\",\"name\":\"bob\"}]"));
        check(ops.ok && ops.entries.size() == 2, "ZAE2 管理员两条",
              QString::number(ops.entries.size()));
        check(ops.entries.value(0).level == 2 && ops.entries.value(0).bypassesPlayerLimit,
              "ZAE2 管理员的 `level` / `bypassesPlayerLimit` 读得出来",
              QString::number(ops.entries.value(0).level));
        check(ops.entries.value(1).level == 4 && !ops.entries.value(1).bypassesPlayerLimit,
              "★ ZAE2 缺省 ⇒ `level = 4`、不绕过人数上限（和服务端 `op` 命令的缺省一致）",
              QString::number(ops.entries.value(1).level));

        const ServerPlayers::Content players = ServerPlayers::parse(
            ServerPlayers::List::BannedPlayers,
            QByteArrayLiteral("[{\"uuid\":\"u-3\",\"name\":\"mallory\","
                              "\"created\":\"2026-10-18 12:00:00 +0800\","
                              "\"source\":\"WTFML\",\"expires\":\"forever\","
                              "\"reason\":\"Banned by WTFML\"}]"));
        check(players.ok && players.entries.size() == 1, "ZAE2 封禁玩家一条",
              QString::number(players.entries.size()));
        const ServerPlayers::Entry &banned = players.entries.first();
        check(banned.uuid == QStringLiteral("u-3") && banned.name == QStringLiteral("mallory")
                  && banned.created == QStringLiteral("2026-10-18 12:00:00 +0800")
                  && banned.source == QStringLiteral("WTFML")
                  && banned.expires == QStringLiteral("forever")
                  && banned.reason == QStringLiteral("Banned by WTFML"),
              "★ ZAE2 封禁玩家那六格（`created` / `source` / `expires` / `reason`）全读得出来",
              banned.created);

        const ServerPlayers::Content ips = ServerPlayers::parse(
            ServerPlayers::List::BannedIps,
            QByteArrayLiteral("[{\"ip\":\"203.0.113.7\",\"created\":\"2026-10-18 12:00:00 +0800\","
                              "\"source\":\"WTFML\",\"expires\":\"forever\",\"reason\":\"\"}]"));
        check(ips.ok && ips.entries.size() == 1
                  && ips.entries.first().ip == QStringLiteral("203.0.113.7"),
              "★ ZAE2 封禁 IP 认的是 `ip`（没有 `uuid` / `name` 那两格）",
              ips.entries.value(0).ip);

        const ServerPlayers::Content tolerant =
            ServerPlayers::parse(ServerPlayers::List::Whitelist,
                                 QByteArrayLiteral("[1,\"x\",null,{\"uuid\":\"u\",\"name\":\"n\"}]"));
        check(tolerant.ok && tolerant.entries.size() == 1
                  && tolerant.entries.first().name == QStringLiteral("n"),
              "★ ZAE2 数组里混着非对象的那几条跳过，剩下的照读（宽容读，不是整份丢掉）",
              QString::number(tolerant.entries.size()));
    }

    // ---------- ZAE3. 写：字段形状和游戏那一份一致，读了能原样回来 ----------
    {
        QList<ServerPlayers::Entry> ops;
        ServerPlayers::Entry alice;
        alice.uuid = QStringLiteral("u-1");
        alice.name = QStringLiteral("alice");
        alice.level = 3;
        alice.bypassesPlayerLimit = true;
        ops << alice;
        const QByteArray bytes = ServerPlayers::serialize(ServerPlayers::List::Ops, ops);
        check(bytes.endsWith('\n'), "★ ZAE3 写出来的那份以换行收尾（服务端 Gson 那一份也是）");
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        check(document.isArray() && document.array().size() == 1,
              "ZAE3 写出来的是一个数组", QString::number(document.array().size()));
        const QJsonObject object = document.array().first().toObject();
        const QStringList keys = object.keys();
        check(QSet<QString>(keys.begin(), keys.end())
                  == QSet<QString>({QStringLiteral("uuid"), QStringLiteral("name"),
                                    QStringLiteral("level"),
                                    QStringLiteral("bypassesPlayerLimit")}),
              "★★ ZAE3 管理员那一份**正好**是游戏那四个键（多一个自定义键 ⇒ 服务端下次启动"
              "整份读不懂）",
              keys.join(QStringLiteral(",")));

        bool roundTripOk = true;
        QString badList;
        for (int i = 0; i < ServerPlayers::listCount(); ++i) {
            const auto list = static_cast<ServerPlayers::List>(i);
            QList<ServerPlayers::Entry> entries;
            entries << ServerPlayers::makeEntry(list, QStringLiteral("Alex"), QStringLiteral("WTFML"),
                                                QDateTime::fromString(QStringLiteral("2026-10-18T12:00:00"),
                                                                      Qt::ISODate));
            entries << ServerPlayers::makeEntry(list, QStringLiteral("Steve"), QStringLiteral("WTFML"),
                                                QDateTime::fromString(QStringLiteral("2026-10-18T12:00:00"),
                                                                      Qt::ISODate));
            const ServerPlayers::Content back =
                ServerPlayers::parse(list, ServerPlayers::serialize(list, entries));
            if (!back.ok || back.entries.size() != 2) {
                roundTripOk = false;
                badList = ServerPlayers::fileName(list);
                break;
            }
        }
        check(roundTripOk, "★★ ZAE3 四份名单都能写出去再读回来，条数不变", badList);
        // 具体字段的往返（先造一条最全的：封禁玩家）。
        ServerPlayers::Entry full = ServerPlayers::makeEntry(
            ServerPlayers::List::BannedPlayers, QStringLiteral("Mallory"), QStringLiteral("WTFML"),
            QDateTime::fromString(QStringLiteral("2026-10-18T12:00:00"), Qt::ISODate));
        full.expires = QStringLiteral("2026-11-01 00:00:00 +0800");
        const ServerPlayers::Entry readBack =
            ServerPlayers::parse(ServerPlayers::List::BannedPlayers,
                                 ServerPlayers::serialize(ServerPlayers::List::BannedPlayers, {full}))
                .entries.value(0);
        check(readBack.uuid == full.uuid && readBack.name == full.name
                  && readBack.created == full.created && readBack.source == full.source
                  && readBack.expires == full.expires && readBack.reason == full.reason,
              "★★ ZAE3 封禁玩家那六格逐字往返（`expires` 是手写的也原样留着）",
              readBack.expires);
    }

    // ---------- ZAE4. 认人 ----------
    {
        ServerPlayers::Entry withUuid;
        withUuid.uuid = QStringLiteral("u-1");
        withUuid.name = QStringLiteral("alice");
        ServerPlayers::Entry noUuid;
        noUuid.name = QStringLiteral("bob");
        ServerPlayers::Entry ip;
        ip.ip = QStringLiteral("198.51.100.9");
        check(ServerPlayers::identityOf(ServerPlayers::List::Whitelist, withUuid)
                  == QStringLiteral("u-1"),
              "★ ZAE4 有 uuid 就认 uuid（名字可以改，uuid 不变）");
        check(ServerPlayers::identityOf(ServerPlayers::List::Whitelist, noUuid)
                  == QStringLiteral("bob"),
              "★ ZAE4 uuid 空着 ⇒ 退回名字");
        check(ServerPlayers::identityOf(ServerPlayers::List::BannedIps, ip)
                  == QStringLiteral("198.51.100.9"),
              "★ ZAE4 封禁 IP 认 `ip`（名字 / uuid 都不看）");
        const QList<ServerPlayers::Entry> entries = {withUuid, noUuid};
        check(ServerPlayers::indexOf(entries, ServerPlayers::List::Whitelist, QStringLiteral("bob")) == 1,
              "ZAE4 按名字找得到（哪怕它排在有 uuid 的那一条后面）");
        check(ServerPlayers::indexOf(entries, ServerPlayers::List::Whitelist, QStringLiteral("u-1")) == 0,
              "ZAE4 按 uuid 找得到");
        check(ServerPlayers::indexOf(entries, ServerPlayers::List::Whitelist, QString()) == -1
                  && ServerPlayers::indexOf(entries, ServerPlayers::List::Whitelist,
                                            QStringLiteral("carol")) == -1,
              "ZAE4 空的 / 不存在的身份 ⇒ -1（不拿「第一条」凑数）");
    }

    // ---------- ZAE5. 离线 UUID ----------
    {
        check(ServerPlayers::offlineUuid(QStringLiteral("Notch"))
                  == QStringLiteral("b50ad385-829d-3141-a216-7e7d7539ba7f"),
              "★★ ZAE5 `OfflinePlayer:Notch` 的 MD5 加 RFC 4122 那两个位 ⇒ "
              "b50ad385-829d-3141-a216-7e7d7539ba7f（这一格对了，离线服务器才认得出这人）",
              ServerPlayers::offlineUuid(QStringLiteral("Notch")));
        const QString many = ServerPlayers::offlineUuid(QStringLiteral("Steve"));
        check(many.size() == 36 && many.count(QLatin1Char('-')) == 4,
              "ZAE5 形状是 8-4-4-4-12（四根横杠、36 个字符）", many);
        check(many == many.toLower(), "ZAE5 一律小写（服务端文件里那份也是小写）", many);
        check(many.at(14) == QLatin1Char('3'),
              "★ ZAE5 版本位是 3（名字 + MD5 那一档）—— 第 15 个字符",
              QString(many.at(14)));
        const QChar variant = many.at(19);
        check(variant == QLatin1Char('8') || variant == QLatin1Char('9')
                  || variant == QLatin1Char('a') || variant == QLatin1Char('b'),
              "★ ZAE5 变体位落在 8~b（RFC 4122 的 10xx）—— 第 20 个字符", QString(variant));
        check(ServerPlayers::offlineUuid(QString()) == QString(),
              "★ ZAE5 空名字 ⇒ 空串（不拿 `OfflinePlayer:` 自己算一个出来）");
        check(ServerPlayers::offlineUuid(QStringLiteral("Steve"))
                  == ServerPlayers::offlineUuid(QStringLiteral("Steve")),
              "ZAE5 同一个名字两次算出同一个值（不然「加一条」和「找到那一条」对不上）");
        check(ServerPlayers::offlineUuid(QStringLiteral("Steve"))
                  != ServerPlayers::offlineUuid(QStringLiteral("steve")),
              "★ ZAE5 大小写是两个名字（游戏按原样算，我们也不折叠）");
    }

    // ---------- ZAE6. 造一条新记录 ----------
    {
        const QDateTime moment =
            QDateTime::fromString(QStringLiteral("2026-10-18T12:34:56"), Qt::ISODate);
        const ServerPlayers::Entry white = ServerPlayers::makeEntry(
            ServerPlayers::List::Whitelist, QStringLiteral("  Alex  "), QStringLiteral("WTFML"),
            moment);
        check(white.name == QStringLiteral("Alex"), "★ ZAE6 名字两头空白先剪掉", white.name);
        check(white.uuid == ServerPlayers::offlineUuid(QStringLiteral("Alex")),
              "★ ZAE6 白名单那条的 uuid 就是离线 UUID（文件名那一格填好，服务端直接认）",
              white.uuid);
        check(white.created.isEmpty() && white.expires.isEmpty() && white.reason.isEmpty(),
              "ZAE6 白名单不摆封禁那几格（空着 = 不写进 JSON）");
        const ServerPlayers::Entry op = ServerPlayers::makeEntry(
            ServerPlayers::List::Ops, QStringLiteral("Alex"), QStringLiteral("WTFML"), moment);
        check(op.level == 4 && !op.bypassesPlayerLimit,
              "★ ZAE6 新加的管理员缺省 `level = 4`、不绕过人数上限");
        const ServerPlayers::Entry ban = ServerPlayers::makeEntry(
            ServerPlayers::List::BannedPlayers, QStringLiteral("Alex"), QStringLiteral("WTFML"),
            moment);
        check(ban.created == QStringLiteral("2026-10-18 12:34:56 +0800")
                  || ban.created.startsWith(QStringLiteral("2026-10-18 12:34:56")),
              "★ ZAE6 封禁那条写上了 `created`（游戏那个 `yyyy-MM-dd HH:mm:ss t` 形状）",
              ban.created);
        check(ban.expires == QStringLiteral("forever")
                  && ban.source == QStringLiteral("WTFML")
                  && ban.reason.contains(QStringLiteral("WTFML")),
              "★ ZAE6 封禁缺省「永久」、来源写我们这个名字、理由也写清楚", ban.expires);
        const ServerPlayers::Entry ip = ServerPlayers::makeEntry(
            ServerPlayers::List::BannedIps, QStringLiteral(" 203.0.113.7 "), QStringLiteral("WTFML"),
            moment);
        check(ip.ip == QStringLiteral("203.0.113.7") && ip.name.isEmpty() && ip.uuid.isEmpty(),
              "★ ZAE6 封禁 IP 只收那个地址（不硬塞一个名字 / uuid）", ip.ip);
    }

    // ---------- ZAE7. 到期：`forever` / 看得懂 / 看不懂 ----------
    {
        const QDateTime now =
            QDateTime::fromString(QStringLiteral("2026-10-18T12:00:00"), Qt::ISODate);
        ServerPlayers::Entry entry;
        entry.expires = QStringLiteral("forever");
        check(!ServerPlayers::isExpired(entry, now), "ZAE7 `forever` ⇒ 没过期");
        entry.expires.clear();
        check(!ServerPlayers::isExpired(entry, now), "ZAE7 空着 ⇒ 没过期（不猜）");
        entry.expires = QStringLiteral("看不懂的时间");
        check(!ServerPlayers::isExpired(entry, now),
              "★★ ZAE7 认不出来的到期时间 ⇒ **不算过期**（当成已过期会让一条封禁看起来不存在）");
        entry.expires = QStringLiteral("2026-10-01 00:00:00 +0800");
        check(ServerPlayers::isExpired(entry, now), "★ ZAE7 过去的点 ⇒ 过期了",
              ServerPlayers::expiryText(entry, now));
        entry.expires = QStringLiteral("2026-12-01 00:00:00 +0800");
        check(!ServerPlayers::isExpired(entry, now), "ZAE7 将来的点 ⇒ 没过期",
              ServerPlayers::expiryText(entry, now));
        check(ServerPlayers::expiryText(entry, now) == QStringLiteral("到 2026-12-01 00:00"),
              "ZAE7 「到 …」那句话的形状（分钟就够，别把秒摆出来）",
              ServerPlayers::expiryText(entry, now));
        entry.expires = QStringLiteral("forever");
        check(ServerPlayers::expiryText(entry, now) == QStringLiteral("永久"),
              "★ ZAE7 `forever` 那一句是「永久」", ServerPlayers::expiryText(entry, now));
        entry.expires = QStringLiteral("2026-10-01 00:00:00 +0800");
        const QString expired = ServerPlayers::expiryText(entry, now);
        check(expired.startsWith(QStringLiteral("已经过期")) && expired.contains(QStringLiteral("2026-10-01")),
              "★ ZAE7 过期那一句把那一刻也写出来", expired);
        entry.expires = QStringLiteral("看不清");
        check(ServerPlayers::expiryText(entry, now).contains(QStringLiteral("看不清")),
              "ZAE7 看不懂的时间原样摆出来（不编一个日子）", ServerPlayers::expiryText(entry, now));
    }
}

