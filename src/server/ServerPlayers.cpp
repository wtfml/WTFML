/**
 * @file ServerPlayers.cpp
 * @brief ServerPlayers.hpp 的实现（纯解析 / 纯拼 JSON，一个字节的盘都不碰）。
 */

#include "server/ServerPlayers.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimeZone>

namespace ServerPlayers {

namespace {

/// 服务端自己写的时间格式：Mojang 那边是 `SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z")`，
/// 写出来长 ` 12:00:00 +0800`（**数字偏移、不带冒号**）。
QDateTime parseMoment(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return QDateTime();
    QDateTime moment = QDateTime::fromString(trimmed, Qt::ISODate);
    if (moment.isValid()) return moment;
    static const QRegularExpression offsetAtEnd(QStringLiteral("\\s([+-])(\\d{2})(\\d{2})$"));
    const QRegularExpressionMatch match = offsetAtEnd.match(trimmed);
    if (match.hasMatch()) {
        const int seconds = (match.captured(2).toInt() * 3600 + match.captured(3).toInt() * 60)
                            * (match.captured(1) == QStringLiteral("-") ? -1 : 1);
        const QString wall = trimmed.left(match.capturedStart()).trimmed();
        const QDate date = QDate::fromString(wall.left(10), QStringLiteral("yyyy-MM-dd"));
        const QTime time = QTime::fromString(wall.mid(11, 8), QStringLiteral("HH:mm:ss"));
        if (date.isValid() && time.isValid()) {
            return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(seconds));
        }
    }
    // 别的写法（`UTC` 这种缩写）再试一次 —— 认不出来回无效，调用方按「不猜」处理。
    return QDateTime::fromString(trimmed, QStringLiteral("yyyy-MM-dd HH:mm:ss t"));
}

QString formatMoment(const QDateTime &moment) {
    const int offset = moment.offsetFromUtc();
    const int absolute = qAbs(offset);
    return QStringLiteral("%1 %2%3%4")
        .arg(moment.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
             (offset < 0 ? QStringLiteral("-") : QStringLiteral("+")),
             QString::number(absolute / 3600).rightJustified(2, QLatin1Char('0')),
             QString::number((absolute % 3600) / 60).rightJustified(2, QLatin1Char('0')));
}

QString stringAt(const QJsonObject &object, const QString &key) {
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

}  // namespace

QString fileName(List list) {
    switch (list) {
    case List::Whitelist:
        return QStringLiteral("whitelist.json");
    case List::Ops:
        return QStringLiteral("ops.json");
    case List::BannedPlayers:
        return QStringLiteral("banned-players.json");
    case List::BannedIps:
        return QStringLiteral("banned-ips.json");
    }
    return QString();
}

QString title(List list) {
    switch (list) {
    case List::Whitelist:
        return QCoreApplication::translate("ServerPlayers", "白名单");
    case List::Ops:
        return QCoreApplication::translate("ServerPlayers", "管理员");
    case List::BannedPlayers:
        return QCoreApplication::translate("ServerPlayers", "封禁的玩家");
    case List::BannedIps:
        return QCoreApplication::translate("ServerPlayers", "封禁的 IP");
    }
    return QString();
}

Content parse(List list, const QByteArray &json) {
    Content content;
    // 文件不在（空字节）就是空名单 —— 新服务端还没跑过的时候四份文件都不存在。
    if (json.trimmed().isEmpty()) {
        return content;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        content.ok = false;
        content.problem = QCoreApplication::translate("ServerPlayers", "%1 无法解析（%2）；没有改动它。")
                              .arg(fileName(list), error.errorString());
        return content;
    }
    if (!document.isArray()) {
        content.ok = false;
        content.problem = QCoreApplication::translate("ServerPlayers", "%1 应为数组，文件未修改。").arg(fileName(list));
        return content;
    }
    const QJsonArray array = document.array();
    for (const QJsonValue &value : array) {
        if (!value.isObject()) continue;  // 宽容：坏的那一条跳过，别整份丢掉
        const QJsonObject object = value.toObject();
        Entry entry;
        switch (list) {
        case List::Whitelist:
            entry.uuid = stringAt(object, QStringLiteral("uuid"));
            entry.name = stringAt(object, QStringLiteral("name"));
            break;
        case List::Ops:
            entry.uuid = stringAt(object, QStringLiteral("uuid"));
            entry.name = stringAt(object, QStringLiteral("name"));
            entry.level = object.value(QStringLiteral("level")).toInt(4);
            entry.bypassesPlayerLimit =
                object.value(QStringLiteral("bypassesPlayerLimit")).toBool(false);
            break;
        case List::BannedPlayers:
            entry.uuid = stringAt(object, QStringLiteral("uuid"));
            entry.name = stringAt(object, QStringLiteral("name"));
            entry.created = stringAt(object, QStringLiteral("created"));
            entry.source = stringAt(object, QStringLiteral("source"));
            entry.expires = stringAt(object, QStringLiteral("expires"));
            entry.reason = stringAt(object, QStringLiteral("reason"));
            break;
        case List::BannedIps:
            entry.ip = stringAt(object, QStringLiteral("ip"));
            entry.created = stringAt(object, QStringLiteral("created"));
            entry.source = stringAt(object, QStringLiteral("source"));
            entry.expires = stringAt(object, QStringLiteral("expires"));
            entry.reason = stringAt(object, QStringLiteral("reason"));
            break;
        }
        content.entries.append(entry);
    }
    return content;
}

QByteArray serialize(List list, const QList<Entry> &entries) {
    QJsonArray array;
    for (const Entry &entry : entries) {
        QJsonObject object;
        switch (list) {
        case List::Whitelist:
            object.insert(QStringLiteral("uuid"), entry.uuid);
            object.insert(QStringLiteral("name"), entry.name);
            break;
        case List::Ops:
            object.insert(QStringLiteral("uuid"), entry.uuid);
            object.insert(QStringLiteral("name"), entry.name);
            object.insert(QStringLiteral("level"), entry.level);
            object.insert(QStringLiteral("bypassesPlayerLimit"), entry.bypassesPlayerLimit);
            break;
        case List::BannedPlayers:
            object.insert(QStringLiteral("uuid"), entry.uuid);
            object.insert(QStringLiteral("name"), entry.name);
            object.insert(QStringLiteral("created"), entry.created);
            object.insert(QStringLiteral("source"), entry.source);
            object.insert(QStringLiteral("expires"), entry.expires);
            object.insert(QStringLiteral("reason"), entry.reason);
            break;
        case List::BannedIps:
            object.insert(QStringLiteral("ip"), entry.ip);
            object.insert(QStringLiteral("created"), entry.created);
            object.insert(QStringLiteral("source"), entry.source);
            object.insert(QStringLiteral("expires"), entry.expires);
            object.insert(QStringLiteral("reason"), entry.reason);
            break;
        }
        array.append(object);
    }
    QByteArray bytes = QJsonDocument(array).toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) bytes.append('\n');
    return bytes;
}

QString identityOf(List list, const Entry &entry) {
    if (list == List::BannedIps) return entry.ip;
    return entry.uuid.isEmpty() ? entry.name : entry.uuid;
}

int indexOf(const QList<Entry> &entries, List list, const QString &identity) {
    if (identity.isEmpty()) return -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (identityOf(list, entries.at(i)) == identity) return i;
    }
    return -1;
}

QString offlineUuid(const QString &name) {
    if (name.isEmpty()) return QString();
    QByteArray hash = QCryptographicHash::hash(
        QByteArrayLiteral("OfflinePlayer:") + name.toUtf8(), QCryptographicHash::Md5);
    // RFC 4122：版本 3（名字 + MD5），变体 10xx。
    hash[6] = static_cast<char>((hash.at(6) & 0x0f) | 0x30);
    hash[8] = static_cast<char>((hash.at(8) & 0x3f) | 0x80);
    const QString hex = QString::fromLatin1(hash.toHex());
    // 最后一组是 **12** 个十六进制字符（8-4-4-4-12），不是 4：少截了这一组出来的是
    // 25 个字符的「UUID」——服务端按 36 个字符的规规矩矩去比对，一辈子也对不上。
    return QStringLiteral("%1-%2-%3-%4-%5")
        .arg(hex.mid(0, 8), hex.mid(8, 4), hex.mid(12, 4), hex.mid(16, 4), hex.mid(20));
}

Entry makeEntry(List list, const QString &nameOrIp, const QString &source, const QDateTime &now) {
    Entry entry;
    if (list == List::BannedIps) {
        entry.ip = nameOrIp.trimmed();
    } else {
        entry.name = nameOrIp.trimmed();
        entry.uuid = offlineUuid(entry.name);
        if (list == List::Ops) {
            entry.level = 4;
            entry.bypassesPlayerLimit = false;
        }
    }
    if (list == List::BannedPlayers || list == List::BannedIps) {
        entry.created = formatMoment(now);
        entry.source = source;
        entry.expires = QStringLiteral("forever");
        entry.reason = source.isEmpty() ? QStringLiteral("Banned by an operator.")
                                        : QStringLiteral("Banned by %1").arg(source);
    }
    return entry;
}

bool isExpired(const Entry &entry, const QDateTime &now) {
    const QString expires = entry.expires.trimmed();
    if (expires.isEmpty() || expires.compare(QStringLiteral("forever"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    const QDateTime moment = parseMoment(expires);
    // 认不出来的到期时间 ⇒ 当作没过期（见头文件那条"不猜"）。
    if (!moment.isValid()) return false;
    return moment < now;
}

QString expiryText(const Entry &entry, const QDateTime &now) {
    const QString expires = entry.expires.trimmed();
    if (expires.isEmpty() || expires.compare(QStringLiteral("forever"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate("ServerPlayers", "永久");
    }
    const QDateTime moment = parseMoment(expires);
    if (!moment.isValid()) {
        return QCoreApplication::translate("ServerPlayers", "到 %1").arg(expires);
    }
    if (moment < now) {
        return QCoreApplication::translate("ServerPlayers", "已经过期（%1）")
            .arg(moment.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    return QCoreApplication::translate("ServerPlayers", "到 %1").arg(moment.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
}

}  // namespace ServerPlayers
