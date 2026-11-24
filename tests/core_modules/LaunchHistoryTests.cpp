#include "Support.hpp"

void runLaunchHistoryTests() {
    section("ZR. 「上一次启动」那本账：格式 / 时间怎么说 / 目标没了怎么降级");

    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir temp;
    check(temp.isValid(), "ZR0 临时数据根目录建出来了");
    if (!temp.isValid()) {
        return;
    }
    AppPaths::setMinecraftDir(temp.path());
    const QString work = temp.path() + QStringLiteral("/work");

    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-13T22:14:00"), Qt::ISODate);
    const auto makeRecord = [](const QString &instanceId, const QDateTime &at) {
        LaunchHistory::Record record;
        record.instanceId = instanceId;
        record.instanceName = QStringLiteral("我的整合-") + instanceId;
        record.accountKey = QStringLiteral("账号-") + instanceId;
        record.startedAt = at;
        return record;
    };
    {
        auto failed = makeRecord(QStringLiteral("a"), now);
        failed.result = LaunchHistory::Result::StartFailed;
        const QList<LaunchHistory::Record> records{
            makeRecord(QStringLiteral("b"), now.addSecs(100)), failed,
            makeRecord(QStringLiteral("a"), now.addSecs(-100))};
        check(LaunchHistory::lastStartedAt(records, QStringLiteral("a")) == now,
              "ZR 概览最后启动按实例取最新进程记录，不依赖账本顺序或退出结果");
        check(!LaunchHistory::lastStartedAt(records, QStringLiteral("missing")).isValid()
                  && !LaunchHistory::lastStartedAt(records, QString()).isValid(),
              "ZR 没有启动记录不编造时间");
    }
    const auto firstOf = [](const QList<LaunchHistory::Record> &list) {
        return list.isEmpty() ? LaunchHistory::Record() : list.first();
    };

    // ---------- ZR1. 这本账落在哪儿、留几条 ----------
    check(LaunchHistory::historyPath() == temp.path() + QStringLiteral("/wtfml.history"),
          "★★ ZR1 账本跟着**当前游戏文件夹**走（`<当前游戏文件夹>/wtfml.history`）—— 这张卡说的是"
          "「这个文件夹里的哪一份实例上次玩过」，换了文件夹就该看见另一本账",
          LaunchHistory::historyPath());
    check(LaunchHistory::historyLimit() == 20, "ZR1 二十条封顶",
          QString::number(LaunchHistory::historyLimit()));

    // ---------- ZR2. 枚举 ⇄ 落盘用的那几个词（写和读走的是同一张表）----------
    bool keysRoundTrip = true;
    for (LaunchHistory::Result result :
         {LaunchHistory::Result::Running, LaunchHistory::Result::NormalExit,
          LaunchHistory::Result::Crash, LaunchHistory::Result::StartFailed}) {
        keysRoundTrip = keysRoundTrip
                        && LaunchHistory::resultFromKey(LaunchHistory::resultKey(result)) == result;
    }
    for (LaunchHistory::Target target : {LaunchHistory::Target::None, LaunchHistory::Target::World,
                                         LaunchHistory::Target::Server}) {
        keysRoundTrip = keysRoundTrip
                        && LaunchHistory::targetFromKey(LaunchHistory::targetKey(target)) == target;
    }
    check(keysRoundTrip, "★ ZR2 四个结局 + 三种目标：枚举 → 词 → 枚举，一格不差");
    check(LaunchHistory::resultFromKey(QStringLiteral("以后加的档")) == LaunchHistory::Result::Running
              && LaunchHistory::targetFromKey(QStringLiteral("以后加的档"))
                     == LaunchHistory::Target::None,
          "★ ZR2 认不出来的词落到**什么都没断言**的那一档（单条记录认不出来，不该让整本账作废）");

    // ---------- ZR3. 一条记录写下去读回来，逐格一样 ----------
    LaunchHistory::Record full = makeRecord(QStringLiteral("1.20.1-forge"), now);
    full.target = LaunchHistory::Target::Server;
    full.targetId = QStringLiteral("play.example.com:25565");
    full.targetDisplayName = QStringLiteral("朋友的服");
    full.durationSeconds = 4320;
    full.result = LaunchHistory::Result::Crash;

    bool ok = false;
    const QList<LaunchHistory::Record> back =
        LaunchHistory::fromJson(LaunchHistory::toJson({full}), &ok);
    const LaunchHistory::Record read = firstOf(back);
    check(ok && back.size() == 1, "ZR3 一条记录写下去读回来还是那一条",
          QString::number(back.size()));
    check(read.instanceId == full.instanceId && read.instanceName == full.instanceName
              && read.accountKey == full.accountKey && read.target == full.target
              && read.targetId == full.targetId && read.targetDisplayName == full.targetDisplayName
              && read.startedAt.toMSecsSinceEpoch() == full.startedAt.toMSecsSinceEpoch()
              && read.durationSeconds == full.durationSeconds && read.result == full.result,
          "★★ ZR3 逐格往返：哪一份 / 当时叫什么 / 谁玩的 / 进的是哪儿 / 什么时候 / 多久 / 最后怎么了");

    LaunchHistory::Record plain = makeRecord(QStringLiteral("1.21-vanilla"), now);
    // 这一档是"身份没记下来"（`accountKey` 空）—— 空着的三格**一个都不该写下去**。
    plain.accountKey.clear();
    const QJsonObject plainRoot =
        QJsonDocument::fromJson(LaunchHistory::toJson({plain})).object();
    check(plainRoot.value(QStringLiteral("format")).toString()
              == QStringLiteral("wtfml-launch-history/1"),
          "ZR3 顶上那个格式标记（认不出来就整份不认）");
    const QJsonObject plainRow = plainRoot.value(QStringLiteral("records")).toArray().at(0).toObject();
    check(!plainRow.contains(QStringLiteral("account"))
              && !plainRow.contains(QStringLiteral("targetId"))
              && !plainRow.contains(QStringLiteral("targetName")),
          "ZR3 空着的字段**根本不写下去**（不是写一个空串 —— 写空串读的人会以为那是个开关）");
    check(plainRow.contains(QStringLiteral("durationSeconds"))
              && plainRow.value(QStringLiteral("durationSeconds")).toDouble() == -1.0,
          "★ ZR3 durationSeconds **总是**写（-1 = 这一条还没结束）：「字段不在」和「写了 -1」"
          "少一层歧义，读的人一眼看得出哪一条还在跑");

    // 认不出的格式 → 整份不认（别的启动器写的、以后我们自己改了格式的旧文件，都只能被挡住）。
    QByteArray forged = LaunchHistory::toJson({plain});
    forged.replace(QByteArrayLiteral("wtfml-launch-history/1"), QByteArrayLiteral("别人的/9"));
    bool forgedOk = true;
    check(!(LaunchHistory::fromJson(forged, &forgedOk).size() > 0) && !forgedOk,
          "★★ ZR3 格式标记对不上就整份不认（不半信半疑地读成一堆空记录）");
    bool junkOk = true;
    check(LaunchHistory::fromJson(QByteArrayLiteral("这不是 json"), &junkOk).isEmpty() && !junkOk,
          "ZR3 整个文件不是 JSON ⇒ 空账 + ok=false");

    // 单条坏掉（没有实例 id / 时间认不出来）**只丢它自己**。
    const QByteArray mixed = QByteArrayLiteral(
        "{\"format\":\"wtfml-launch-history/1\",\"records\":["
        "{\"instanceId\":\"\",\"startedAt\":\"2026-10-13T10:00:00\"},"
        "{\"instanceId\":\"没有时间的那一条\",\"startedAt\":\"不是时间\"},"
        "{\"instanceId\":\"留下的那一条\",\"startedAt\":\"2026-10-13T10:00:00\"}]}");
    bool mixedOk = false;
    const QList<LaunchHistory::Record> kept = LaunchHistory::fromJson(mixed, &mixedOk);
    check(mixedOk && kept.size() == 1
              && kept.first().instanceId == QStringLiteral("留下的那一条"),
          "★★ ZR3 坏掉的那一条当它不在，别的照读（整本账不该被一条坏记录连坐）",
          QString::number(kept.size()));

    // ---------- ZR4. 盘上那一趟 + 二十条封顶 ----------
    const QString path = LaunchHistory::historyPath();
    check(LaunchHistory::load(path).isEmpty(),
          "ZR4 文件不在 = **空账**（一次都没启动过的新用户就是这样，不是错误）");
    check(LaunchHistory::save(path, {full, plain}), "ZR4 写盘成功");
    check(QFileInfo::exists(path), "★ ZR4 盘上真多了那一份");
    const QList<LaunchHistory::Record> fromDisk = LaunchHistory::load(path);
    check(fromDisk.size() == 2 && fromDisk.first().instanceId == full.instanceId
              && fromDisk.last().instanceId == plain.instanceId,
          "★ ZR4 读回来还是那两条、顺序也还是那个顺序");

    writeFileAt(path, QByteArrayLiteral("这不是 json"));
    check(LaunchHistory::load(path).isEmpty(),
          "★★ ZR4 文件被写坏了 ⇒ 空账（既不崩，也不半信半疑地留下半份）");

    writeFileAt(work + QStringLiteral("/挡住"), QByteArrayLiteral("x"));
    check(!LaunchHistory::save(work + QStringLiteral("/挡住/底下.json"), {plain}),
          "★ ZR4 目录建不出来时 save() 回 false（调用方不许静默吞掉）");

    QList<LaunchHistory::Record> many;
    for (int i = 0; i < 25; ++i) {
        many.append(makeRecord(QStringLiteral("老的那一份%1").arg(i), now.addSecs(-1000 - i)));
    }
    const LaunchHistory::Record fresh = makeRecord(QStringLiteral("刚玩的"), now);
    const QList<LaunchHistory::Record> capped = LaunchHistory::prepend(many, fresh);
    check(capped.size() == LaunchHistory::historyLimit(),
          "★★ ZR4 二十条封顶（超出来的那几条被丢掉的是**最老的**）",
          QString::number(capped.size()));
    check(capped.first().instanceId == QStringLiteral("刚玩的"),
          "ZR4 新的一条排在最前面");
    check(capped.last().instanceId == QStringLiteral("老的那一份18"),
          "★ ZR4 截掉的是**尾巴**上最老的几条，不是把新来的挤掉",
          capped.last().instanceId);
    check(LaunchHistory::prepend({}, fresh).size() == 1, "ZR4 空账上加一条就是一条");

    {
        const QString playPath = work + QStringLiteral("/wtfml.history");
        const QString stableId = QStringLiteral("023e2000-bcb6-4495-94f2-36a3f28d4020");
        const QString otherId = QStringLiteral("023e2000-bcb6-4495-94f2-36a3f28d4021");
        check(LaunchHistory::save(playPath, {plain}) &&
                  LaunchHistory::loadPlayTime(playPath, stableId).lastMs == -1,
              "ZR8 旧启动账本可读，不从历史记录倒推累计");

        LaunchHistory::PlayTimeCounter counter;
        counter.start(1, 1000);
        counter.start(2, 2000);
        check(counter.currentMs(2500) == 1500 && counter.totalMs(2500) == 1500,
              "ZR8 同实例多开时，本次和累计都只计重叠时间一次");
        check(!counter.finish(1, 3000) && counter.completed().lastMs == -1 &&
                  counter.currentMs(3500) == 2500,
              "ZR8 部分进程退出不结算，也不重置本次起点");
        check(counter.finish(2, 4000) && !counter.finish(2, 5000) &&
                  counter.completed().totalMs == 3000 && counter.completed().lastMs == 3000,
              "ZR8 最后一个进程退出才结算并集，重复退出不重复累计");
        check(LaunchHistory::savePlayTime(playPath, stableId, counter.completed()) &&
                  LaunchHistory::load(playPath).size() == 1 &&
                  LaunchHistory::save(playPath, capped),
              "ZR8 维护累计与滚动最近记录互不覆盖");

        LaunchHistory::PlayTimeCounter restarted(LaunchHistory::loadPlayTime(playPath, stableId));
        restarted.start(1, 500);
        check(restarted.currentMs(999) == 499 && restarted.totalMs(999) == 3499 &&
                  restarted.finish(1, 1400) && restarted.completed().totalMs == 3900 &&
                  restarted.completed().lastMs == 900,
              "ZR8 重启后继续累计，非重叠期间不计，毫秒不丢");
        check(LaunchHistory::savePlayTime(playPath, stableId, restarted.completed()) &&
                  LaunchHistory::savePlayTime(playPath, otherId, {600, 600}) &&
                  LaunchHistory::loadPlayTime(playPath, stableId).totalMs == 3900 &&
                  LaunchHistory::loadPlayTime(playPath, stableId).lastMs == 900 &&
                  LaunchHistory::loadPlayTime(playPath, otherId).totalMs == 600 &&
                  LaunchHistory::load(playPath).size() == LaunchHistory::historyLimit(),
              "ZR8 累计和上次按稳定实例 ID 分开存，独立于最近 20 条");
        check(LaunchHistory::playTimeText(-1).isEmpty() &&
                  LaunchHistory::playTimeText(90061000) == QStringLiteral("25:01:01"),
              "ZR8 未记录与跨天时长的显示正确");
    }

    // ---------- ZR5. 「上一次」那一条是哪一条 ----------
    // 顺序故意乱的：账上那个列表按**写入顺序**排，而"最新"要按**时间**认。
    QList<LaunchHistory::Record> shuffled;
    shuffled.append(makeRecord(QStringLiteral("更晚的那一趟"), now.addSecs(-60)));
    shuffled.append(makeRecord(QStringLiteral("更早的那一趟"), now.addSecs(-86400)));
    check(LaunchHistory::latestPlayable(shuffled).instanceId == QStringLiteral("更晚的那一趟"),
          "★ ZR5 「最新」按**时间**认，不按它在账上的位置");

    QList<LaunchHistory::Record> withFailed;
    LaunchHistory::Record failed = makeRecord(QStringLiteral("没起来的那一份"), now);
    failed.result = LaunchHistory::Result::StartFailed;
    withFailed.append(failed);  // 时间上最新，可它压根没玩成
    withFailed.append(makeRecord(QStringLiteral("真玩过的那一份"), now.addSecs(-3600)));
    check(LaunchHistory::latestPlayable(withFailed).instanceId == QStringLiteral("真玩过的那一份"),
          "★★ ZR5 「上次没起来」那一趟**不当作**「上次游玩」（拿它当上次玩了什么是撒谎）");
    check(!LaunchHistory::latestPlayable({failed}).isValid(),
          "★ ZR5 全是「没起来」的那些 ⇒ 算没有（卡片收起来）");
    check(!LaunchHistory::latestPlayable({}).isValid(), "ZR5 空账 ⇒ 没有");

    LaunchHistory::Record running = makeRecord(QStringLiteral("正开着的那一份"), now);
    running.result = LaunchHistory::Result::Running;
    check(LaunchHistory::latestPlayable({running}).instanceId
              == QStringLiteral("正开着的那一份"),
          "★ ZR5 还在跑的那一条**算数**（游戏这会儿正开着，卡片更该显示它）");

    LaunchHistory::Record noTime;
    noTime.instanceId = QStringLiteral("没有时间的那一条");
    check(!LaunchHistory::latestPlayable({noTime}).isValid(),
          "ZR5 连时间都没有的记录不算数（卡片上那两样都得有）");

    // ---------- ZR6. 「昨天 22:14」怎么说 ----------
    const QDateTime clockNow =
        QDateTime::fromString(QStringLiteral("2026-10-13T23:00:00"), Qt::ISODate);
    check(LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-10-13T22:14:00"),
                                                       Qt::ISODate),
                                  clockNow) == QStringLiteral("22:14"),
          "ZR6 今天只写时刻");
    check(LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-10-12T23:59:00"),
                                                       Qt::ISODate),
                                  QDateTime::fromString(QStringLiteral("2026-10-13T00:01:00"),
                                                        Qt::ISODate))
              == QStringLiteral("昨天 23:59"),
          "★★ ZR6 **按日期比**，不按差了多久 —— 23:59 到次日 00:01 只差两分钟，可那是"
          "「昨天」；卡片上要的正是「哪一天」");
    check(LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-10-11T09:05:00"),
                                                       Qt::ISODate),
                                  clockNow) == QStringLiteral("前天 09:05"),
          "ZR6 前天");
    check(LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-10-05T09:05:00"),
                                                       Qt::ISODate),
                                  clockNow) == QStringLiteral("2026-10-05 09:05"),
          "ZR6 再早就把日子写全（「昨天/前天」之外不给模糊话）");
    check(LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-12-31T09:05:00"),
                                                       Qt::ISODate),
                                  clockNow) == QStringLiteral("2026-12-31 09:05"),
          "ZR6 将来的时间照写（钟被往回拨过 / 文件被人动过时，写真的比编一句「刚刚」强）");
    check(LaunchHistory::whenText(QDateTime(), clockNow).isEmpty()
              && LaunchHistory::whenText(QDateTime::fromString(QStringLiteral("2026-10-13T22:14:00"),
                                                               Qt::ISODate),
                                         QDateTime())
                     .isEmpty(),
          "ZR6 时间无效 ⇒ 空串（**不编**）");

    // ---------- ZR7. 卡片上那两行字 ----------
    LaunchHistory::Record world = makeRecord(QStringLiteral("world"), now);
    world.target = LaunchHistory::Target::World;
    world.targetId = QStringLiteral("基地周目");
    world.targetDisplayName = QStringLiteral("基地周目");
    check(LaunchHistory::targetLine(world) == QStringLiteral("单人世界：基地周目"),
          "ZR7 存档那一行");
    LaunchHistory::Record worldNoName = world;
    worldNoName.targetDisplayName.clear();
    check(LaunchHistory::targetLine(worldNoName) == QStringLiteral("单人世界：基地周目"),
          "ZR7 名字没记下来时退回目标 id（不写一个空的「单人世界：」）");

    LaunchHistory::Record server = makeRecord(QStringLiteral("server"), now);
    server.target = LaunchHistory::Target::Server;
    server.targetId = QStringLiteral("play.example.com:25565");
    server.targetDisplayName = QStringLiteral("朋友的服");
    check(LaunchHistory::targetLine(server) == QStringLiteral("服务器：朋友的服"),
          "ZR7 服务器那一行写的是**名字**（地址在记录里，不在这一行）");
    check(LaunchHistory::targetLine(plain).isEmpty(),
          "★★ ZR7 普通启动那一趟这一行是**空的** —— 不写「主菜单」三个字（那是句废话，"
          "少一行才叫「没什么可说的」）");

    LaunchHistory::Record note = plain;
    note.result = LaunchHistory::Result::Crash;
    check(LaunchHistory::resultNote(note) == QStringLiteral("（上次是异常退出）"),
          "ZR7 崩过就明说");
    note.result = LaunchHistory::Result::Running;
    check(LaunchHistory::resultNote(note) == QStringLiteral("（还在玩）"),
          "★ ZR7 还在跑也明说（卡片上那颗按钮点下去会提醒「已经有一份在跑」）");
    note.result = LaunchHistory::Result::StartFailed;
    check(LaunchHistory::resultNote(note) == QStringLiteral("（上次启动失败）"),
          "ZR7 没起来那一档也有话说");
    note.result = LaunchHistory::Result::NormalExit;
    check(LaunchHistory::resultNote(note).isEmpty(),
          "ZR7 正常退出**不占一行**（绝大多数时候都正常，写出来只是噪音）");

    check(LaunchHistory::durationText(-1).isEmpty(), "ZR7 还没算出来 ⇒ 空串");
    check(LaunchHistory::durationText(0) == QStringLiteral("玩了不到 1 分钟")
              && LaunchHistory::durationText(59) == QStringLiteral("玩了不到 1 分钟"),
          "ZR7 不到一分钟那一档（「玩了」两个字和量词一起挪，不许拼出「玩了 不到 1 分钟」）");
    check(LaunchHistory::durationText(60) == QStringLiteral("玩了 1 分钟")
              && LaunchHistory::durationText(3599) == QStringLiteral("玩了 59 分钟"),
          "ZR7 分钟那一档");
    check(LaunchHistory::durationText(3600) == QStringLiteral("玩了 1 小时")
              && LaunchHistory::durationText(4320) == QStringLiteral("玩了 1 小时 12 分")
              && LaunchHistory::durationText(7200) == QStringLiteral("玩了 2 小时"),
          "ZR7 小时那一档（整点不写「0 分」）");

    AppPaths::setMinecraftDir(originalDir);
}

