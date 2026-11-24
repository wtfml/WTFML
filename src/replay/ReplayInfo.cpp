/**
 * @file ReplayInfo.cpp
 * @brief ReplayInfo.hpp 的实现：从 `.mcpr` 里读 `metaData.json`。
 */

#include "replay/ReplayInfo.hpp"

#include "tools/FileHash.hpp"
#include "util/ZipUtil.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
#include <limits>

namespace ReplayInfo {

namespace {

/// 这一次读只为一份元数据 —— 一个几百 MB 的录像不该因为读不出来就把界面卡住半天。
/// `ZipUtil::readEntry()` 自己也有超时；这里那道上限管的是"文件本身大到不可能是录像"。
constexpr qint64 kMaxArchiveBytes = 4LL * 1024 * 1024 * 1024;

QString textOf(const QJsonObject &object, const QString &key) {
    const QJsonValue value = object.value(key);
    if (!value.isString()) return QString();
    return value.toString().trimmed();
}

qint64 integerOf(const QJsonObject &object, const QString &key) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) return -1;
    const double number = value.toDouble();
    // 越界的 double 直接 static_cast<qint64> 是未定义行为（R16）：手改过的
    // metaData.json 里写一个 `1e30` 就能走到这里。这种值一律当"不知道"（-1），
    // 别让界面拿一个转出来的怪数去算时长 / 日期。
    constexpr double kQint64Limit = 9223372036854775808.0;  // 2^63，double 恰好表得出来
    if (!std::isfinite(number) || number < -kQint64Limit || number >= kQint64Limit) {
        return -1;
    }
    return static_cast<qint64>(number);
}

}  // namespace

Info read(const QString &path) {
    Info info;
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile()) {
        info.problem = QCoreApplication::translate("ReplayInfo", "未找到文件");
        return info;
    }
    info.fileBytes = file.size();
    if (info.fileBytes <= 0) {
        info.problem = QCoreApplication::translate("ReplayInfo", "文件为空");
        return info;
    }
    if (info.fileBytes > kMaxArchiveBytes) {
        info.problem = QCoreApplication::translate("ReplayInfo", "文件过大，不是有效的 Replay 录像");
        return info;
    }

    QByteArray bytes;
    QString error;
    const ZipUtil::EntryStatus status =
        ZipUtil::readEntry(path, QStringLiteral("metaData.json"), &bytes, &error,
                           QCoreApplication::translate("ReplayInfo", "读取 Replay 元数据"));
    switch (status) {
    case ZipUtil::EntryStatus::Ok:
        break;
    case ZipUtil::EntryStatus::NotFound:
        info.problem = QCoreApplication::translate("ReplayInfo", "压缩包中无 metaData.json，不是 Replay 录像");
        return info;
    case ZipUtil::EntryStatus::NotAnArchive:
        info.problem = QCoreApplication::translate("ReplayInfo", "无法打开录像压缩包。请确认文件是有效的 Replay 录像。");
        return info;
    case ZipUtil::EntryStatus::Failed:
        info.problem = error.isEmpty() ? QCoreApplication::translate("ReplayInfo", "读取压缩包失败") : error;
        return info;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        info.problem = QCoreApplication::translate("ReplayInfo", "metaData.json 无法解析（%1）")
                           .arg(parseError.errorString());
        return info;
    }
    const QJsonObject object = document.object();
    // 有这一格才算 Replay：别的 zip 里也可能恰好有一份叫 metaData.json 的清单。
    if (!object.contains(QStringLiteral("mcversion"))
        && !object.contains(QStringLiteral("fileFormat"))) {
        info.problem = QCoreApplication::translate("ReplayInfo", "metaData.json 无 Replay 版本信息");
        return info;
    }

    info.mcVersion = textOf(object, QStringLiteral("mcversion"));
    info.generator = textOf(object, QStringLiteral("generator"));
    info.serverName = textOf(object, QStringLiteral("serverName"));
    // ReplayMod 的单人录像这一格是 true；老录像没有这一格，按单人算。
    const QJsonValue single = object.value(QStringLiteral("singleplayer"));
    info.singleplayer = single.isBool() ? single.toBool() : true;
    info.durationMs = integerOf(object, QStringLiteral("duration"));
    info.dateMs = integerOf(object, QStringLiteral("date"));
    // protocol 落在 int 里也要卡一下（qint64 里合法、int 里溢出的值以前是实现定义的）。
    const qint64 protocolValue = integerOf(object, QStringLiteral("protocol"));
    info.protocol = (protocolValue >= 0 && protocolValue <= std::numeric_limits<int>::max())
                        ? static_cast<int>(protocolValue)
                        : -1;
    const QJsonValue players = object.value(QStringLiteral("players"));
    if (players.isArray()) info.playerCount = players.toArray().size();

    info.ok = true;
    return info;
}

QString durationText(qint64 ms) {
    if (ms < 0) return QString();
    if (ms < 1000) return QCoreApplication::translate("ReplayInfo", "不足 1 秒");
    const qint64 totalSeconds = ms / 1000;
    if (totalSeconds < 60) {
        return QCoreApplication::translate("ReplayInfo", "%1 秒").arg(totalSeconds);
    }
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    if (minutes < 60) {
        return seconds == 0 ? QCoreApplication::translate("ReplayInfo", "%1 分").arg(minutes)
                            : QCoreApplication::translate("ReplayInfo", "%1 分 %2 秒").arg(minutes).arg(seconds);
    }
    const qint64 hours = minutes / 60;
    const qint64 restMinutes = minutes % 60;
    return restMinutes == 0
               ? QCoreApplication::translate("ReplayInfo", "%1 小时").arg(hours)
               : QCoreApplication::translate("ReplayInfo", "%1 小时 %2 分").arg(hours).arg(restMinutes);
}

QString dateTimeText(qint64 ms) {
    // 0 也算"不知道"：ReplayMod 从来不会写一个 1970 年的日期，那多半是字段缺省。
    if (ms <= 0) return QString();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString summaryText(const Info &info) {
    if (!info.ok) return QString();
    QStringList parts;
    const QString duration = durationText(info.durationMs);
    if (!duration.isEmpty()) parts << duration;
    if (!info.mcVersion.isEmpty()) parts << info.mcVersion;
    if (info.singleplayer) {
        parts << QCoreApplication::translate("ReplayInfo", "单人");
    } else {
        parts << (info.serverName.isEmpty()
                      ? QCoreApplication::translate("ReplayInfo", "多人")
                      : QCoreApplication::translate("ReplayInfo", "多人（%1）").arg(info.serverName));
    }
    if (info.playerCount > 0) parts << QCoreApplication::translate("ReplayInfo", "%1 位玩家").arg(info.playerCount);
    if (!info.generator.isEmpty()) parts << info.generator;
    return parts.join(QStringLiteral(" / "));
}

QString detailText(const Info &info) {
    if (!info.ok) return QString();
    QStringList lines;
    const QString summary = summaryText(info);
    if (!summary.isEmpty()) lines << summary;
    const QString date = dateTimeText(info.dateMs);
    if (!date.isEmpty()) lines << QCoreApplication::translate("ReplayInfo", "录制时间：%1").arg(date);
    if (info.fileBytes > 0) {
        lines << QCoreApplication::translate("ReplayInfo", "文件大小：%1").arg(FileHash::fileSizeText(info.fileBytes));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace ReplayInfo
