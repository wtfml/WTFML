#include "Support.hpp"

void runReplayInfoTests() {
    section("RP. Replay 录像（.mcpr）的元数据");

    QTemporaryDir dir;
    check(dir.isValid(), "RP0 临时目录");
    if (!dir.isValid()) {
        return;
    }
    const QString root = dir.path();

    // 造一份**真**的 `.mcpr`：zip 里一份 metaData.json + 一份假的 recording.tmcpr
    // （后者只占个名字 —— 实现那边明确不读它）。
    const auto makeReplay = [&](const QString &name, const QByteArray &meta,
                                const QString &metaEntry = QStringLiteral("metaData.json")) {
        const QString replayPath = root + QStringLiteral("/") + name;
        const auto dirFor = root + QStringLiteral("/sources-") + name;
        QDir().mkpath(dirFor);
        QString why;
        QList<ZipUtil::ZipSource> entries;
        if (!metaEntry.isEmpty()) {
            const QString metaPath = dirFor + QStringLiteral("/metaData.json");
            writeFileAt(metaPath, meta);
            entries.append({metaEntry, metaPath});
        }
        const QString recordingPath = dirFor + QStringLiteral("/recording.tmcpr");
        writeFileAt(recordingPath, QByteArray(8, '\0'));
        entries.append({QStringLiteral("recording.tmcpr"), recordingPath});
        if (!ZipUtil::createZip(replayPath, entries, &why)) {
            check(false, QStringLiteral("RP0 造一份真 `.mcpr`：%1").arg(name), why);
            return QString();
        }
        return replayPath;
    };

    // ---------- RP1. 一份完整的录像 ----------
    {
        const QByteArray meta = R"({
            "singleplayer": false,
            "serverName": "mc.example.com",
            "duration": 200500,
            "date": 1690000000000,
            "mcversion": "1.20.1",
            "fileFormat": "MCPR",
            "fileFormatVersion": 14,
            "protocol": 763,
            "generator": "ReplayMod v2.6.11",
            "players": ["a", "b", "c"]
        })";
        const QString path = makeReplay(QStringLiteral("full.mcpr"), meta);
        if (!path.isEmpty()) {
            const ReplayInfo::Info info = ReplayInfo::read(path);
            check(info.ok && info.problem.isEmpty(), "RP1 一份完整的录像读得出来",
                  info.ok ? QString() : info.problem);
            check(info.mcVersion == QStringLiteral("1.20.1")
                      && info.generator == QStringLiteral("ReplayMod v2.6.11")
                      && info.protocol == 763,
                  "RP1 版本 / 录制器 / 协议号",
                  QStringLiteral("%1 / %2 / %3")
                      .arg(info.mcVersion, info.generator).arg(info.protocol));
            check(!info.singleplayer && info.serverName == QStringLiteral("mc.example.com"),
                  "RP1 多人录像记得住服务器名");
            check(info.durationMs == 200500 && info.dateMs == 1690000000000,
                  "RP1 时长 / 录制时间按原数（毫秒）");
            check(info.playerCount == 3, "RP1 玩家数 = players 数组长度",
                  QString::number(info.playerCount));
            check(info.fileBytes > 0, "RP1 文件大小");
            const QString summary = ReplayInfo::summaryText(info);
            check(summary.contains(QStringLiteral("3 分 20 秒"))
                      && summary.contains(QStringLiteral("1.20.1"))
                      && summary.contains(QStringLiteral("多人（mc.example.com）"))
                      && summary.contains(QStringLiteral("3 位玩家"))
                      && summary.contains(QStringLiteral("ReplayMod v2.6.11")),
                  "★ RP1 那一行字：时长 / 版本 / 单人还是多人 / 玩家数 / 录制器",
                  summary);
            const QString detail = ReplayInfo::detailText(info);
            check(detail.contains(QStringLiteral("录制时间："))
                      && detail.contains(QStringLiteral("文件大小：")),
                  "RP1 悬停提示补上录制时间与文件大小", detail);
        }
    }

    // ---------- RP2. 老录像：只有一格版本 ----------
    {
        const QString path =
            makeReplay(QStringLiteral("old.mcpr"), QByteArray(R"({"mcversion":"1.8.9"})"));
        if (!path.isEmpty()) {
            const ReplayInfo::Info info = ReplayInfo::read(path);
            check(info.ok && info.durationMs == -1 && info.dateMs == -1
                      && info.playerCount == -1 && info.protocol == -1,
                  "★ RP2 缺的字段留 -1（**不拿 0 顶**：0 毫秒和没记录是两件事）",
                  QStringLiteral("时长=%1 日期=%2 玩家=%3")
                      .arg(info.durationMs).arg(info.dateMs).arg(info.playerCount));
            check(info.singleplayer, "RP2 没有 `singleplayer` 那一格 ⇒ 按单人算");
            check(ReplayInfo::summaryText(info) == QStringLiteral("1.8.9 / 单人"),
                  "★ RP2 不知道的字段一个都不往那一行里塞",
                  ReplayInfo::summaryText(info));
        }
    }

    // ---------- RP3. 读不出来的几种 ----------
    {
        const QString notZip = root + QStringLiteral("/plain.mcpr");
        writeFileAt(notZip, QByteArrayLiteral("this is not a zip at all"));
        const ReplayInfo::Info broken = ReplayInfo::read(notZip);
        check(!broken.ok && (broken.problem.contains(QStringLiteral("打不开"))
                             || broken.problem.contains(QStringLiteral("Replay"))),
              "★ RP3 不是 zip：说「打不开、不是 Replay 录像」，不硬当录像念", broken.problem);

        const QString noMeta =
            makeReplay(QStringLiteral("nometa.mcpr"), QByteArray(), QString());
        if (!noMeta.isEmpty()) {
            const ReplayInfo::Info missing = ReplayInfo::read(noMeta);
            check(!missing.ok && missing.problem.contains(QStringLiteral("metaData.json")),
                  "★ RP3 zip 里没有 metaData.json ⇒ 明说缺的是哪一份", missing.problem);
        }

        const QString badJson =
            makeReplay(QStringLiteral("badjson.mcpr"), QByteArrayLiteral("{oops"));
        if (!badJson.isEmpty()) {
            const ReplayInfo::Info bad = ReplayInfo::read(badJson);
            check(!bad.ok && bad.problem.contains(QStringLiteral("无法解析")),
                  "RP3 元数据不是 JSON：说「读不懂」", bad.problem);
        }

        const QString otherZip = makeReplay(QStringLiteral("other.mcpr"),
                                            QByteArrayLiteral(R"({"name":"not a replay"})"));
        if (!otherZip.isEmpty()) {
            const ReplayInfo::Info other = ReplayInfo::read(otherZip);
            check(!other.ok && other.problem.contains(QStringLiteral("版本信息")),
                  "★ RP3 有 metaData.json 但没有 Replay 的版本信息 ⇒ 不当成录像",
                  other.problem);
        }

        const ReplayInfo::Info absent = ReplayInfo::read(root + QStringLiteral("/nope.mcpr"));
        check(!absent.ok && absent.problem.contains(QStringLiteral("未找到")),
              "RP3 文件不存在", absent.problem);
    }

    // ---------- RP5. 越界的数字当"不知道"（R16） ----------
    //
    // metaData.json 是能手改的。1e30 这种值 double→qint64 是未定义行为（转出来的数
    // 谁也说不准），以前它会直接变成 durationMs / dateMs；protocol 还会再被截进 int。
    {
        const QString path = makeReplay(QStringLiteral("huge.mcpr"), QByteArray(R"({
            "mcversion": "1.20.1",
            "duration": 1e30,
            "date": -1e30,
            "protocol": 1e30
        })"));
        if (!path.isEmpty()) {
            const ReplayInfo::Info info = ReplayInfo::read(path);
            check(info.ok, "RP5 前提：元数据本身读得出来");
            check(info.durationMs == -1 && info.dateMs == -1 && info.protocol == -1,
                  "★★ R16 越界的数字一律当「不知道」（-1），不拿 static_cast 出来的怪数去显示",
                  QStringLiteral("时长=%1 日期=%2 协议=%3")
                      .arg(info.durationMs).arg(info.dateMs).arg(info.protocol));
            // 那一行里不许出现负数转出来的怪时长 / 怪日期。
            const QString summary = ReplayInfo::summaryText(info);
            check(!summary.contains(QStringLiteral("小时")) &&
                      !summary.contains(QStringLiteral("秒")),
                  "RP5 越界的时长不进那一行摘要", summary);
        }
    }

    // ---------- RP4. 单纯的格式化那几格 ----------
    {
        check(ReplayInfo::durationText(-1).isEmpty(),
              "★ RP4 负数是「不知道」⇒ 空串（不是「不足 1 秒」）");
        check(ReplayInfo::durationText(0) == QStringLiteral("不足 1 秒")
                  && ReplayInfo::durationText(59999) == QStringLiteral("59 秒"),
              "RP4 不足一秒 / 不到一分钟");
        check(ReplayInfo::durationText(60000) == QStringLiteral("1 分")
                  && ReplayInfo::durationText(200500) == QStringLiteral("3 分 20 秒"),
              "RP4 分 + 秒（整分不拖一个「0 秒」）");
        check(ReplayInfo::durationText(3600000) == QStringLiteral("1 小时")
                  && ReplayInfo::durationText(3900000) == QStringLiteral("1 小时 5 分"),
              "RP4 超过一小时按「时 分」说");
        check(ReplayInfo::dateTimeText(-1).isEmpty() && ReplayInfo::dateTimeText(0).isEmpty()
                  && ReplayInfo::dateTimeText(1690000000000).size() == 16,
              "★ RP4 日期 0 / 负数是「没记录」⇒ 空串；有记录时是 `yyyy-MM-dd HH:mm`");
    }
}

