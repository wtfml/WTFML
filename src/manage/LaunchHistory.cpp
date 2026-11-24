/**
 * @file LaunchHistory.cpp
 * @brief 「上一次启动」那本账的实现（形状与取舍见头文件）。
 */

#include "LaunchHistory.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include "util/WriteDirectoryGuard.hpp"

#include "util/AppPaths.hpp"

#include <limits>

namespace LaunchHistory {

QDateTime lastStartedAt(const QList<Record> &records, const QString &instanceId) {
    QDateTime latest;
    if (instanceId.isEmpty()) return latest;
    for (const auto &record : records) {
        if (record.instanceId == instanceId && record.startedAt.isValid()
            && (!latest.isValid() || record.startedAt > latest)) {
            latest = record.startedAt;
        }
    }
    return latest;
}
namespace {

/// 盘上那个格式标记。**写死一个字符串**而不是"看有没有哪个字段"：以后改格式时，旧文件在
/// 这里被挡住，而不是半信半疑地读成一堆空记录（和 `BackupFiles::formatTag()` 同一套）。
QString formatTag() {
    return QStringLiteral("wtfml-launch-history/1");
}

/// 那一层的名字（`{"format": …, "records": [...]}`）。
const char *kRecordsKey = "records";
const char *kPlayTimesKey = "playTimes";

QJsonObject readHistoryObject(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject();
}

bool writeHistoryObject(const QString &path, const QJsonObject &root) {
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

}  // namespace

void PlayTimeCounter::start(int sessionId, qint64 tickMs) {
    if (m_sessions.isEmpty()) m_startedAtMs = tickMs;
    m_sessions.insert(sessionId);
}

bool PlayTimeCounter::finish(int sessionId, qint64 tickMs) {
    const qint64 total = totalMs(tickMs);
    const qint64 current = currentMs(tickMs);
    if (!m_sessions.remove(sessionId) || !m_sessions.isEmpty()) return false;
    m_completed = {total, current};
    return true;
}

qint64 PlayTimeCounter::currentMs(qint64 tickMs) const {
    return m_sessions.isEmpty() ? 0 : qMax<qint64>(0, tickMs - m_startedAtMs);
}

qint64 PlayTimeCounter::totalMs(qint64 tickMs) const {
    const qint64 room = std::numeric_limits<qint64>::max() - m_completed.totalMs;
    return m_completed.totalMs + qMin(room, currentMs(tickMs));
}

static QJsonObject playTimeObject(const QString &path) {
    const QJsonObject root = readHistoryObject(path);
    if (root.value(QStringLiteral("format")).toString() != formatTag()) return {};
    return root.value(QLatin1String(kPlayTimesKey)).toObject();
}

static PlayTime parsePlayTime(const QJsonObject &entry) {
    const qint64 total = qMax<qint64>(0, entry.value(QStringLiteral("totalMs")).toInteger());
    const qint64 last = entry.value(QStringLiteral("lastMs")).toInteger(-1);
    return {total, last >= 0 && last <= total ? last : -1};
}

PlayTime loadPlayTime(const QString &path, const QString &stableInstanceId) {
    if (stableInstanceId.isEmpty()) return {};
    return parsePlayTime(playTimeObject(path).value(stableInstanceId).toObject());
}

QHash<QString, PlayTime> loadPlayTimes(const QString &path) {
    QHash<QString, PlayTime> result;
    const QJsonObject entries = playTimeObject(path);
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        result.insert(it.key(), parsePlayTime(it.value().toObject()));
    }
    return result;
}

bool savePlayTime(const QString &path, const QString &stableInstanceId, const PlayTime &playTime) {
    if (stableInstanceId.isEmpty() || !WriteDirectoryGuard::acquire(path)) return false;
    QJsonObject root = readHistoryObject(path);
    if (root.value(QStringLiteral("format")).toString() != formatTag()) {
        if (QFileInfo::exists(path)) return false;
        root.insert(QStringLiteral("format"), formatTag());
        root.insert(QLatin1String(kRecordsKey), QJsonArray());
    }
    QJsonObject playTimes = root.value(QLatin1String(kPlayTimesKey)).toObject();
    playTimes.insert(stableInstanceId, QJsonObject{
        {QStringLiteral("totalMs"), playTime.totalMs}, {QStringLiteral("lastMs"), playTime.lastMs}});
    root.insert(QLatin1String(kPlayTimesKey), playTimes);
    return writeHistoryObject(path, root);
}

QString playTimeText(qint64 milliseconds) {
    if (milliseconds < 0) return {};
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600)
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString historyPath() {
    return AppPaths::minecraftDir() + QStringLiteral("/wtfml.history");
}

int historyLimit() {
    return 20;
}

QString resultKey(Result result) {
    switch (result) {
    case Result::Running:
        return QStringLiteral("running");
    case Result::NormalExit:
        return QStringLiteral("normalExit");
    case Result::Crash:
        return QStringLiteral("crash");
    case Result::StartFailed:
        return QStringLiteral("startFailed");
    }
    return QStringLiteral("running");
}

QString targetKey(Target target) {
    switch (target) {
    case Target::None:
        return QStringLiteral("none");
    case Target::World:
        return QStringLiteral("world");
    case Target::Server:
        return QStringLiteral("server");
    }
    return QStringLiteral("none");
}

Result resultFromKey(const QString &key) {
    if (key == QLatin1String("normalExit")) {
        return Result::NormalExit;
    }
    if (key == QLatin1String("crash")) {
        return Result::Crash;
    }
    if (key == QLatin1String("startFailed")) {
        return Result::StartFailed;
    }
    // 认不出来的词（以后加了一档、或者文件被人手改过）落到 `Running`：单条记录认不出来
    // 不该让整本账作废，而 `Running` 是这里唯一一档"什么都没有断言"的值。
    return Result::Running;
}

Target targetFromKey(const QString &key) {
    if (key == QLatin1String("world")) {
        return Target::World;
    }
    if (key == QLatin1String("server")) {
        return Target::Server;
    }
    return Target::None;
}

static QJsonArray recordsArray(const QList<Record> &records) {
    QJsonArray array;
    for (const Record &record : records) {
        QJsonObject item;
        item.insert(QStringLiteral("instanceId"), record.instanceId);
        item.insert(QStringLiteral("instanceName"), record.instanceName);
        // 空的**不写**（省得读的人以为它是个开关，和 BackupFiles 那边同一条规矩）。
        if (!record.accountKey.isEmpty()) {
            item.insert(QStringLiteral("account"), record.accountKey);
        }
        item.insert(QStringLiteral("target"), targetKey(record.target));
        if (!record.targetId.isEmpty()) {
            item.insert(QStringLiteral("targetId"), record.targetId);
        }
        if (!record.targetDisplayName.isEmpty()) {
            item.insert(QStringLiteral("targetName"), record.targetDisplayName);
        }
        item.insert(QStringLiteral("startedAt"),
                    record.startedAt.toString(Qt::ISODateWithMs));
        // `durationSeconds < 0` = 还没算出来（还在跑）。**照样写**这一格（写 -1）：读的人
        // 一眼看得出"这一条没结束"，比"字段不在"少一层歧义。
        item.insert(QStringLiteral("durationSeconds"), record.durationSeconds);
        item.insert(QStringLiteral("result"), resultKey(record.result));
        array.append(item);
    }
    return array;
}

QByteArray toJson(const QList<Record> &records) {
    QJsonObject root;
    root.insert(QStringLiteral("format"), formatTag());
    root.insert(QLatin1String(kRecordsKey), recordsArray(records));
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QList<Record> fromJson(const QByteArray &bytes, bool *ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return QList<Record>();
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != formatTag()) {
        return QList<Record>();
    }

    QList<Record> records;
    const QJsonArray array = root.value(QLatin1String(kRecordsKey)).toArray();
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject item = value.toObject();
        Record record;
        record.instanceId = item.value(QStringLiteral("instanceId")).toString();
        record.instanceName = item.value(QStringLiteral("instanceName")).toString();
        record.accountKey = item.value(QStringLiteral("account")).toString();
        record.target = targetFromKey(item.value(QStringLiteral("target")).toString());
        record.targetId = item.value(QStringLiteral("targetId")).toString();
        record.targetDisplayName = item.value(QStringLiteral("targetName")).toString();
        record.startedAt = QDateTime::fromString(
            item.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
        record.durationSeconds = qint64(item.value(QStringLiteral("durationSeconds")).toDouble(-1));
        record.result = resultFromKey(item.value(QStringLiteral("result")).toString());
        // 认不出是谁（没有实例 id）或者没有时间的那一条**当它不在**：卡片上那两样都得有，
        // 留着只会让下面每一处都得判一次"这条是不是空的"。
        if (record.isValid()) {
            records.append(record);
        }
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return records;
}

QList<Record> load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QList<Record>();
    }
    const QByteArray bytes = file.readAll();
    file.close();
    return fromJson(bytes, nullptr);
}

bool save(const QString &path, const QList<Record> &records) {
    if (!WriteDirectoryGuard::acquire(path)) return false;
    QJsonObject root = readHistoryObject(path);
    if (root.value(QStringLiteral("format")).toString() != formatTag()) root = QJsonObject();
    root.insert(QStringLiteral("format"), formatTag());
    root.insert(QLatin1String(kRecordsKey), recordsArray(records));
    return writeHistoryObject(path, root);
}

QList<Record> prepend(const QList<Record> &records, const Record &record) {
    QList<Record> out;
    out.append(record);
    const int limit = historyLimit();
    for (const Record &existing : records) {
        if (out.size() >= limit) {
            break;
        }
        out.append(existing);
    }
    return out;
}

Record latestPlayable(const QList<Record> &records) {
    Record best;
    for (const Record &record : records) {
        if (!record.isValid() || record.result == Result::StartFailed) {
            continue;
        }
        if (!best.isValid() || record.startedAt > best.startedAt) {
            best = record;
        }
    }
    return best;
}

QString whenText(const QDateTime &at, const QDateTime &now) {
    if (!at.isValid() || !now.isValid()) {
        return QString();
    }
    const QString clock = at.toString(QStringLiteral("HH:mm"));
    // 按**日期**比，不按"差了多少秒"：23:59 和 00:01 之间只差两分钟，可那两分钟正好跨了
    // 一天 —— 写成"2 分钟前"没错，但卡片上那句要的是"哪一天"，`昨天 00:01` 才是用户
    // 心里那一句。
    const int days = at.date().daysTo(now.date());
    if (days == 0) {
        return clock;
    }
    if (days == 1) {
        return QCoreApplication::translate("LaunchHistory", "昨天 %1").arg(clock);
    }
    if (days == 2) {
        return QCoreApplication::translate("LaunchHistory", "前天 %1").arg(clock);
    }
    // 将来的时间（钟被改过 / 文件被人动过）落到这儿会写出一个未来的日子 —— 那也是**真的**
    // 那一格里的内容，比编一句"刚刚"强。
    return at.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString whenText(const QDateTime &at) {
    return whenText(at, QDateTime::currentDateTime());
}

QString targetLine(const Record &record) {
    switch (record.target) {
    case Target::World:
        return QCoreApplication::translate("LaunchHistory", "单人世界：%1").arg(
            record.targetDisplayName.isEmpty() ? record.targetId : record.targetDisplayName);
    case Target::Server:
        return QCoreApplication::translate("LaunchHistory", "服务器：%1").arg(
            record.targetDisplayName.isEmpty() ? record.targetId : record.targetDisplayName);
    case Target::None:
        break;
    }
    return QString();
}

QString resultNote(const Record &record) {
    switch (record.result) {
    case Result::Crash:
        return QCoreApplication::translate("LaunchHistory", "（上次是异常退出）");
    case Result::Running:
        return QCoreApplication::translate("LaunchHistory", "（还在玩）");
    case Result::StartFailed:
        return QCoreApplication::translate("LaunchHistory", "（上次启动失败）");
    case Result::NormalExit:
        break;
    }
    return QString();
}

QString durationText(qint64 seconds) {
    if (seconds < 0) {
        return QString();
    }
    // 出的是**整句**（「玩了 1 小时 12 分」），不是光一个数量：卡片上那一行是"目标 ·
    // 时长（结局）"拼出来的，量词不同（分钟 / 小时）时"玩了"两个字必须一起挪 ——
    // 让调用方去拼会拼出「玩了 不到 1 分钟」这种话。
    if (seconds < 60) {
        return QCoreApplication::translate("LaunchHistory", "玩了不到 1 分钟");
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QCoreApplication::translate("LaunchHistory", "玩了 %1 分钟").arg(minutes);
    }
    const qint64 hours = minutes / 60;
    const qint64 rest = minutes % 60;
    if (rest == 0) {
        return QCoreApplication::translate("LaunchHistory", "玩了 %1 小时").arg(hours);
    }
    return QCoreApplication::translate("LaunchHistory", "玩了 %1 小时 %2 分").arg(hours).arg(rest);
}

}  // namespace LaunchHistory
