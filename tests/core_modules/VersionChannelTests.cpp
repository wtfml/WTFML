#include "Support.hpp"

void runVersionChannelTests() {
    const QString metaDir = QStringLiteral(REPO_ROOT_PATH) + "/tests/meta";

    section("Y. 版本四分类（真清单 915 条）");

    QList<MinecraftVersion> manifest;
    QStringList manifestIds;
    {
        QFile fixture(metaDir + "/manifest.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Y1 读到了真清单夹具（meta/manifest.json）");
        if (!haveFixture) {
            return;
        }
        const QByteArray json = fixture.readAll();
        fixture.close();

        // 走产品自己那条解析路（DownloadManager::parseVersionManifest），不是在这儿再写一份
        // 解析 —— 否则这一节验的是「测试怎么读清单」，和产品读出来的是不是同一份就没保证了。
        DownloadManager downloadManager;
        downloadManager.parseVersionManifest(QJsonDocument::fromJson(json));
        manifest = downloadManager.getAvailableVersions();
        check(manifest.size() == 915, "Y1 产品那条解析路读出 915 条",
              QString::number(manifest.size()));
        if (manifest.size() != 915) {
            return;
        }

        // type 必须带出来 —— 四分类的前三类全靠它，丢了的话前三个分类会一起空掉，
        // 而"空掉"在界面上和"这个版本没有版本"长得一模一样，所以得在这儿钉住。
        int typed = 0;
        for (const MinecraftVersion &version : manifest) {
            if (!version.type.isEmpty()) {
                ++typed;
            }
            manifestIds.append(version.id);
        }
        check(typed == manifest.size(), "Y1 每一条的 type 都读出来了（前三类的判据来源）",
              QStringLiteral("%1/%2").arg(typed).arg(manifest.size()));
    }

    const QList<VersionManager::VersionChannel> channels =
        VersionManager::groupIntoChannels(manifest);

    // --- Y2. 分堆的账 -----------------------------------------------------
    check(channels.size() == VersionCategory::channelCount(), "Y2 分出来就是四堆",
          QString::number(channels.size()));
    if (channels.size() != VersionCategory::channelCount()) {
        return;
    }
    check(channels.at(0).channel == VersionCategory::Channel::Release &&
              channels.at(1).channel == VersionCategory::Channel::Snapshot &&
              channels.at(2).channel == VersionCategory::Channel::Old &&
              channels.at(3).channel == VersionCategory::Channel::AprilFools,
          "Y2 四堆的顺序 = 界面上从上到下的顺序（正式/快照/远古/愚人节）");

    const QStringList expectedTexts{QStringLiteral("正式版"), QStringLiteral("快照版"),
                                    QStringLiteral("远古版"), QStringLiteral("愚人节版")};
    const QStringList expectedKeys{QStringLiteral("release"), QStringLiteral("snapshot"),
                                   QStringLiteral("old"), QStringLiteral("aprilfools")};
    bool textsOk = true;
    bool keysOk = true;
    for (int i = 0; i < 4; ++i) {
        if (channels.at(i).text != expectedTexts.at(i)) {
            textsOk = false;
        }
        if (channels.at(i).key != expectedKeys.at(i)) {
            keysOk = false;
        }
    }
    check(textsOk, "Y2 四段文字对得上", channels.at(3).text);
    check(keysOk, "Y2 四个键对得上（界面认键、不认文字）", channels.at(3).key);

    const int expectedCounts[4] = {103, 742, 61, 9};
    const QString expectedNotes[4] = {
        QStringLiteral("Y2 正式版 103 条（清单里的 release 一共就这些）"),
        QStringLiteral("Y2 快照版 742 条（愚人节那 9 条已经从 751 条里摘走了）"),
        QStringLiteral("Y2 远古版 61 条（old_beta 26 + old_alpha 35，两样都要收）"),
        QStringLiteral("Y2 愚人节版 9 条")};
    for (int i = 0; i < 4; ++i) {
        check(channels.at(i).versions.size() == expectedCounts[i], expectedNotes[i],
              QString::number(channels.at(i).versions.size()));
    }

    int total = 0;
    QStringList allIds;
    for (const VersionManager::VersionChannel &channel : channels) {
        total += channel.versions.size();
        for (const MinecraftVersion &version : channel.versions) {
            allIds.append(version.id);
        }
    }
    check(total == manifest.size(), "Y2 四堆加起来一条不丢",
          QStringLiteral("%1 vs %2").arg(total).arg(manifest.size()));
    check(allIds.size() == QSet<QString>(allIds.begin(), allIds.end()).size(),
          "Y2 也没有哪一条进了两堆（上面那件事的另一面）");
    // 顺序原样（清单本来就是新到旧）。重排过的话界面上第一条就不是最新的那个了。
    check(channels.at(0).versions.value(0).id == QStringLiteral("26.3"),
          "Y2 正式版第一条是 26.3（顺序原样，没被重排）",
          channels.at(0).versions.value(0).id);
    check(channels.at(1).versions.value(0).id == QStringLiteral("26.3-rc-3"),
          "Y2 快照版第一条是 26.3-rc-3", channels.at(1).versions.value(0).id);
    check(channels.at(2).versions.value(0).id == QStringLiteral("b1.8.1"),
          "Y2 远古版第一条是 b1.8.1（清单里 old_beta 排在 old_alpha 前面）",
          channels.at(2).versions.value(0).id);

    // 每一条的 type 和它落的那一堆必须对得上 —— 这是"前三类照抄 type"这件事本身
    int mismatched = 0;
    for (const VersionManager::VersionChannel &channel : channels) {
        for (const MinecraftVersion &version : channel.versions) {
            if (VersionCategory::channelFor(version.id, version.type) != channel.channel) {
                ++mismatched;
            }
        }
    }
    check(mismatched == 0, "Y2 每一堆里的每一条都是按 channelFor 的结论放进去的",
          QString::number(mismatched));

    // --- Y2b. 「最新版本」那一组的两条 --------------------------------------
    const QList<VersionManager::LatestVersion> latest = VersionManager::latestVersions(manifest);
    check(latest.size() == 2, "Y2b 清单上挑得出两条（一个正式版、一个快照）",
          QString::number(latest.size()));
    check(latest.size() == 2 &&
              latest.at(0).channel == VersionCategory::Channel::Release &&
              latest.at(1).channel == VersionCategory::Channel::Snapshot,
          "Y2b 顺序固定：正式版在前、快照在后（不是按清单里的先后）",
          QStringLiteral("%1 条").arg(latest.size()));
    if (latest.size() != 2) {
        return;
    }
    check(latest.at(0).version.id == QStringLiteral("26.3") &&
              latest.at(1).version.id == QStringLiteral("26.3-rc-3"),
          "Y2b 这份夹具上挑出来的是 26.3 与 26.3-rc-3（清单前两条，一条一版）",
          latest.at(0).version.id + QStringLiteral(" / ") + latest.at(1).version.id);
    // 和"它自己那一堆的第一条"必须是**同一份数据**：两处各挑各的，早晚会出现
    // "最新版本里写着 A、正式版那一栏的第一条却是 B"。
    check(latest.at(0).version.releaseTime == channels.at(0).versions.value(0).releaseTime &&
              latest.at(0).version.type == channels.at(0).versions.value(0).type &&
              latest.at(1).version.releaseTime == channels.at(1).versions.value(0).releaseTime &&
              latest.at(1).version.type == channels.at(1).versions.value(0).type,
          "Y2b 那两条就是「正式版 / 快照版」两堆各自的头一条（同一份数据、不是各挑各的）");

    // 清单是空的（离线 / 第一发就失败）⇒ 一条都没有。界面照旧把那一组摆出来，里面是空的。
    check(VersionManager::latestVersions({}).isEmpty(), "Y2b 空清单挑出来就是空的（不是两条空的）");

    // 缺哪一类就只有一条 —— **不许**拿另一类去补位。这份清单里只有快照没有正式版。
    QList<MinecraftVersion> onlySnapshot;
    onlySnapshot.append(MinecraftVersion{QStringLiteral("26.3-rc-3"), QStringLiteral("snapshot"),
                                         QString(), QString(),
                                         QStringLiteral("2026-09-14T12:58:38+00:00")});
    const QList<VersionManager::LatestVersion> oneOnly =
        VersionManager::latestVersions(onlySnapshot);
    check(oneOnly.size() == 1 && oneOnly.at(0).channel == VersionCategory::Channel::Snapshot &&
              oneOnly.at(0).version.id == QStringLiteral("26.3-rc-3"),
          "Y2b 只有快照没有正式版时，就只给那一条（不拿别的类补位）",
          QString::number(oneOnly.size()));

    // 愚人节版在清单里也是 type == snapshot：按 type 判会让它当上"最新快照"。
    QList<MinecraftVersion> withAprilFools;
    withAprilFools.append(MinecraftVersion{QStringLiteral("26w14a"), QStringLiteral("snapshot"),
                                           QString(), QString(),
                                           QStringLiteral("2026-04-01T12:00:00+00:00")});
    withAprilFools.append(MinecraftVersion{QStringLiteral("26.3"), QStringLiteral("release"),
                                           QString(), QString(),
                                           QStringLiteral("2026-09-15T11:23:02+00:00")});
    withAprilFools.append(MinecraftVersion{QStringLiteral("26.2-snapshot-1"),
                                           QStringLiteral("snapshot"), QString(), QString(),
                                           QStringLiteral("2026-08-25T12:53:43+00:00")});
    const QList<VersionManager::LatestVersion> noApril =
        VersionManager::latestVersions(withAprilFools);
    check(noApril.size() == 2 &&
              noApril.at(1).version.id == QStringLiteral("26.2-snapshot-1"),
          "Y2b 最新的快照**跳过**愚人节版（26w14a 也是 type=snapshot，但它进的是愚人节那一栏）",
          QStringLiteral("%1 条，快照那条是 %2")
              .arg(noApril.size())
              .arg(noApril.size() > 1 ? noApril.at(1).version.id : QStringLiteral("<无>")));

    // 那一组的身份：**非空、且和四个分类键两两不同**（同一个角色里住着树上五个顶层身份，
    // 撞了之后"哪一条是展开的"那笔账会串到另一条上）。
    const QString latestKey = VersionManager::latestGroupKey();
    check(!latestKey.isEmpty() && !VersionCategory::channelKeys().contains(latestKey),
          "Y2b 「最新版本」那一组的身份非空、且和四个分类键都不撞",
          latestKey);
    check(!VersionManager::latestGroupText().isEmpty(),
          "Y2b 那一组给用户看的名字非空", VersionManager::latestGroupText());

    // --- Y3. 愚人节版那张名单 ---------------------------------------------
    const QStringList aprilIds = VersionCategory::aprilFoolsVersionIds();
    check(aprilIds.size() == 9, "Y3 名单 9 条", QString::number(aprilIds.size()));

    // 名单里的每一条都得在**这份**清单里 —— 打错一个字符的那条会立刻掉出来。
    QStringList missingInManifest;
    for (const QString &id : aprilIds) {
        if (!manifestIds.contains(id, Qt::CaseInsensitive)) {
            missingInManifest.append(id);
        }
    }
    check(missingInManifest.isEmpty(),
          "Y3 名单里每一条都能在这份真清单里找到（打错一个字符就会掉出来）",
          missingInManifest.join(QStringLiteral("、")));

    // 反过来：那 9 条**全**在愚人节版那一堆里（没有谁被留在快照版里）
    QStringList aprilInBucket;
    for (const MinecraftVersion &version : channels.at(3).versions) {
        aprilInBucket.append(version.id);
    }
    check(aprilInBucket.contains(QStringLiteral("3D Shareware v1.34"), Qt::CaseInsensitive),
          "Y3 有大写和空格的 3D Shareware v1.34 也认出来了（比的时候不能区分大小写）",
          aprilInBucket.join(QStringLiteral("、")));
    check(aprilInBucket.contains(QStringLiteral("1.RV-Pre1")),
          "Y3 1.RV-Pre1（3/31 发布，日期规则会漏掉它）也在里面");
    int aprilLeftInSnapshot = 0;
    for (const MinecraftVersion &version : channels.at(1).versions) {
        if (VersionCategory::isAprilFoolsVersion(version.id)) {
            ++aprilLeftInSnapshot;
        }
    }
    check(aprilLeftInSnapshot == 0, "Y3 愚人节那几条没有谁还被留在快照版里",
          QString::number(aprilLeftInSnapshot));

    // 名单里每一条的发布日期都得落在 3/31~4/1 这个窗口 —— 这条是**名单打错的兜底判据**：
    // 打错的那条会立刻掉出日期窗口。上一条（"在不在清单里"）单独是抓不全的。
    QStringList oddDates;
    for (const MinecraftVersion &version : channels.at(3).versions) {
        const QString date = version.releaseTime.mid(5, 5);
        if (date != QLatin1String("03-31") && date != QLatin1String("04-01")) {
            oddDates.append(version.id + QStringLiteral(" @") + version.releaseTime.left(10));
        }
    }
    check(oddDates.isEmpty(), "Y3 愚人节版那 9 条的发布日期都在 3/31~4/1 这个窗口里",
          oddDates.join(QStringLiteral("、")));

    // 日期那条规则为什么被否掉：落在 3/31~4/1 的**不止**愚人节版
    QStringList onDateButNotApril;
    for (const MinecraftVersion &version : manifest) {
        const QString date = version.releaseTime.mid(5, 5);
        if ((date == QLatin1String("03-31") || date == QLatin1String("04-01")) &&
            !VersionCategory::isAprilFoolsVersion(version.id)) {
            onDateButNotApril.append(version.id);
        }
    }
    check(onDateButNotApril.contains(QStringLiteral("26.1.1")),
          "Y3 4/1 发布的**正式版** 26.1.1 不是愚人节版（日期当判据就会把它塞进那一栏）",
          onDateButNotApril.join(QStringLiteral("、")));
    check(onDateButNotApril.size() == 5,
          "Y3 落在那个日期窗口里的正经版本一共 5 条（这就是日期规则被否掉的原因）",
          QStringLiteral("%1: %2")
              .arg(onDateButNotApril.size())
              .arg(onDateButNotApril.join(QStringLiteral("、"))));

    // --- Y4. 几张表本身 ---------------------------------------------------
    check(VersionCategory::channelKeys() == expectedKeys, "Y4 四个键");
    check(VersionCategory::channelTexts() == expectedTexts, "Y4 四段文字");
    // 键和文字**一一对应**：两张表按同一个顺序，且 channelAt/channelIndex 互为反函数
    bool consistent = true;
    for (int i = 0; i < VersionCategory::channelCount(); ++i) {
        const VersionCategory::Channel channel = VersionCategory::channelAt(i);
        if (VersionCategory::channelIndex(channel) != i || channels.at(i).channel != channel) {
            consistent = false;
        }
    }
    check(consistent, "Y4 channelAt 和 channelIndex 互为反函数、且和界面那条顺序是同一个顺序");
    // 越界不许崩，也不许给一个 -1 那种（界面下标和堆数将来对不上时）
    check(VersionCategory::channelAt(-1) == VersionCategory::Channel::Release &&
              VersionCategory::channelAt(99) == VersionCategory::Channel::Release,
          "Y4 channelAt 越界时回一个确定的分类（不是 -1 / 不崩）");
    check(VersionCategory::isAprilFoolsVersion(QStringLiteral("3d shareware v1.34")) &&
              VersionCategory::isAprilFoolsVersion(QStringLiteral("3D SHAREWARE V1.34")),
          "Y4 认愚人节版时大小写不敏感");
    check(!VersionCategory::isAprilFoolsVersion(QStringLiteral("26.1.1")), "Y4 26.1.1 不是愚人节版");
    check(VersionCategory::channelFor(QString(), QStringLiteral("release")) ==
              VersionCategory::Channel::Release,
          "Y4 id 空着时按 type 走（不给空 id 瞎认）");
    // 没见过的 type：落到快照版（宁可多列一条，也不让它从四个分类里凭空消失）
    check(VersionCategory::channelFor(QStringLiteral("99w99a"), QStringLiteral("whatever")) ==
              VersionCategory::Channel::Snapshot,
          "Y4 没见过的 type 落到快照版（一条都不许从四分类里漏掉）");

    section("Y5. 四个加载器的「能挑哪些版本」（真 maven 清单 / 真页面）");
    // 这一段回答的是「安装页上点开一个加载器，列表里该有些什么」。四份数据源都是真的：
    // meta/forge-maven-metadata.xml 211487 B / 5048 条
    // meta/neoforge-maven-metadata.xml 63597 B
    // meta/optifine-downloads.html 200723 B（官方下载页）
    // meta/optifine-1.18.2-list.json 11 条（镜像那份 JSON）

    // --- Y6. Forge：新开的那个 maven 源 -----------------------------------
    {
        QFile fixture(metaDir + "/forge-maven-metadata.xml");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Y6 读到了真 Forge maven 清单（meta/forge-maven-metadata.xml）");
        if (haveFixture) {
            const QByteArray xml = fixture.readAll();
            fixture.close();
            check(xml.size() > 200000, "Y6 那一份是 200 KB 以上（5048 条那个全集）",
                  QString::number(xml.size()));

            const QStringList v1201 = LoaderMeta::loaderVersionStrings(
                LoaderMeta::listForgeVersions(xml, QStringLiteral("1.20.1")));
            check(v1201.size() == 132, "Y6 Forge 1.20.1 下有 132 个版本",
                  QString::number(v1201.size()));
            check(v1201.value(0) == QStringLiteral("47.4.23"),
                  "Y6 最新的那个排第一（1.20.1 是 47.4.23）", v1201.value(0));
            check(v1201.contains(QStringLiteral("47.4.10")),
                  "Y6 官方在用的那个 47.4.10 也在列表里",
                  QStringLiteral("列表 %1 条").arg(v1201.size()));

            // 新到旧（清单自己是按发布时间排的，不能直接拿来当"版本大小"的顺序）
            bool descending = true;
            for (int i = 1; i < v1201.size(); ++i) {
                if (LoaderMeta::compareDottedVersions(
                        LoaderMeta::parseDottedVersion(v1201.at(i - 1)),
                        LoaderMeta::parseDottedVersion(v1201.at(i))) < 0) {
                    descending = false;
                }
            }
            check(descending, "Y6 整份列表严格从新到旧");

            // 老版本那种 <游戏>-<加载器>-<游戏> 坐标要归一
            const QStringList v189 = LoaderMeta::loaderVersionStrings(
                LoaderMeta::listForgeVersions(xml, QStringLiteral("1.8.9")));
            check(v189.contains(QStringLiteral("11.15.1.2318")) &&
                      v189.contains(QStringLiteral("11.15.1.1722")),
                  "Y6 1.8.9 那两种坐标都归一成版本号了",
                  v189.mid(0, 3).join(QStringLiteral(", ")));
            bool tailsGone = true;
            for (const QString &version : v189) {
                if (version.contains(QLatin1Char('-'))) {
                    tailsGone = false;
                }
            }
            check(tailsGone, "Y6 归一之后没有剩下带尾巴的");

            // 1.10 那条：坐标尾巴是 1.10.0，按**字符串**前缀剥不掉，得逐段比
            const QStringList v110 = LoaderMeta::loaderVersionStrings(
                LoaderMeta::listForgeVersions(xml, QStringLiteral("1.10")));
            check(v110.contains(QStringLiteral("12.18.0.2000")),
                  "Y6 1.10 的尾巴 1.10.0 也剥掉了（逐段比前缀，不是比字符串）",
                  v110.mid(0, 3).join(QStringLiteral(", ")));

            // 带字母后缀的那些（10.13.3.1401-1710ls）不收 —— 它们"谁更新"没有定义
            const QStringList v1710 = LoaderMeta::loaderVersionStrings(
                LoaderMeta::listForgeVersions(xml, QStringLiteral("1.7.10")));
            bool onlyDotted = true;
            for (const QString &version : v1710) {
                if (LoaderMeta::parseDottedVersion(version).isEmpty()) {
                    onlyDotted = false;
                }
            }
            check(onlyDotted && !v1710.isEmpty(),
                  "Y6 1.7.10 那 13 个带字母后缀的不收进来（但正经的那些还在）",
                  QStringLiteral("%1 条").arg(v1710.size()));

            // 不串味：清单里没有的游戏版本回空，而不是把前缀够得着的那些串进来
            check(LoaderMeta::listForgeVersions(xml, QStringLiteral("1.7")).isEmpty(),
                  "Y6 清单里没有 1.7（只有 1.7.10）时回空，不会把 1.7.10 的串进来");
            check(LoaderMeta::listForgeVersions(xml, QStringLiteral("1.2")).isEmpty(),
                  "Y6 1.2 也不会把 1.20.x 的串进来");
            check(LoaderMeta::listForgeVersions(xml, QString()).isEmpty(),
                  "Y6 空游戏版本号回空（不拿它去前缀匹配整份清单）");

            // 归一那一手单独钉一遍（两头的形状 + 差一个字都不行）
            check(LoaderMeta::forgeLoaderVersionFromCoordinate(QStringLiteral("1.20.1-47.4.10"),
                                                              QStringLiteral("1.20.1")) ==
                      QStringLiteral("47.4.10"),
                  "Y6 常规坐标 1.20.1-47.4.10 → 47.4.10");
            check(LoaderMeta::forgeLoaderVersionFromCoordinate(
                      QStringLiteral("1.8.9-11.15.1.2318-1.8.9"), QStringLiteral("1.8.9")) ==
                      QStringLiteral("11.15.1.2318"),
                  "Y6 老坐标 1.8.9-11.15.1.2318-1.8.9 → 11.15.1.2318");
            check(LoaderMeta::forgeLoaderVersionFromCoordinate(QStringLiteral("1.20.1-47.4.10"),
                                                              QStringLiteral("1.20"))
                      .isEmpty(),
                  "Y6 游戏版本 1.20 不会把 1.20.1 的坐标当成自己的（差一个字都不行）");
        }
    }

    // --- Y6b. 频道键：那两块牌子与"真版本号"互不相认 ----------------------
    //
    // 界面上那两档（下载最新版 / 下载稳定版）是拿 `@latest` / `@stable` 两个键表达的；
    // 用户点某一条具体版本号时走的是同一根管子的另一支。这两种取值必须互相排斥，
    // 否则"钉了 1.20.1-47.4.10 结果按最新档挑"这种事就会静悄悄地发生。
    {
        check(LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Stable) ==
                  QStringLiteral("@stable") &&
                  LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Newest) ==
                      QStringLiteral("@latest"),
              "Y6b 两个键就是 @stable / @latest");
        LoaderMeta::LoaderChannel channel = LoaderMeta::LoaderChannel::Stable;
        check(LoaderMeta::loaderChannelFromKey(QStringLiteral("@latest"), &channel) &&
                  channel == LoaderMeta::LoaderChannel::Newest,
              "Y6b @latest 解成最新档");
        channel = LoaderMeta::LoaderChannel::Newest;
        check(LoaderMeta::loaderChannelFromKey(QStringLiteral("@stable"), &channel) &&
                  channel == LoaderMeta::LoaderChannel::Stable,
              "Y6b @stable 解成稳定档（上一次解出来的结果不会留在里头）");
        check(!LoaderMeta::loaderChannelFromKey(QStringLiteral("1.20.1-47.4.10"), &channel) &&
                  !LoaderMeta::loaderChannelFromKey(QString(), &channel) &&
                  !LoaderMeta::loaderChannelFromKey(QStringLiteral("@Latest"), &channel),
              "Y6b 真版本号 / 空串 / 大小写不一样的键 -> 都不是频道键");
        // 这条是上面所有断言的地基：`@` 不在版本号那个白名单里（`[A-Za-z0-9._+-]`），
        // 所以真版本号永远长不成 `@latest` 那个样子，两个键也永远过不了真版本号那关。
        check(!LoaderMeta::isSafeVersionId(QStringLiteral("@latest")) &&
                  !LoaderMeta::isSafeVersionId(QStringLiteral("@stable")) &&
                  LoaderMeta::isSafeVersionId(QStringLiteral("1.20.1-47.4.10")),
              "★ Y6b `@` 不在版本号白名单里 —— 两个键和真版本号不可能撞车");

        const LoaderMeta::LoaderRequest byChannel =
            LoaderMeta::loaderRequestFromChoice(QStringLiteral("@latest"));
        check(!byChannel.pinned() && byChannel.channel == LoaderMeta::LoaderChannel::Newest,
              "Y6b @latest -> 没钉版本、按最新档挑");
        const LoaderMeta::LoaderRequest byDefault = LoaderMeta::loaderRequestFromChoice(QString());
        check(!byDefault.pinned() && byDefault.channel == LoaderMeta::LoaderChannel::Stable,
              "Y6b 空串 -> 按稳定档挑（「按老规矩挑」那条路）");
        const LoaderMeta::LoaderRequest pinned =
            LoaderMeta::loaderRequestFromChoice(QStringLiteral("1.20.1-47.4.10"));
        check(pinned.pinned() && pinned.pinnedVersion == QStringLiteral("1.20.1-47.4.10") &&
                  pinned.channel == LoaderMeta::LoaderChannel::Stable,
              "Y6b 真版本号 -> 钉死它（频道那个字段就不起作用了）");
    }

    // --- Y7. NeoForge -----------------------------------------------------
    //
    // 上游对 26.3 **只发过 -beta**：beta 一律列出来、在第二行标「测试版」；「稳定档」
    // 挑不出来时那条自己灰掉写「没有正式版」。所以这一段钉的是 beta 出现在列表里，
    // 不是"列表里没有 -beta"。
    {
        QFile fixture(metaDir + "/neoforge-maven-metadata.xml");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Y7 读到了真 NeoForge maven 清单");
        if (haveFixture) {
            const QByteArray xml = fixture.readAll();
            fixture.close();

            const QString games[4] = {QStringLiteral("1.21.1"), QStringLiteral("26.2"),
                                      QStringLiteral("1.20.2"), QStringLiteral("1.21.10")};
            bool allNonEmpty = true;
            bool flagsOk = true;              // 每条的 stable 就是「版本号里没有 - 或 +」
            bool stableIsFirstStable = true;  // 稳定档挑的 == 列表里第一条正式版
            bool newestIsFirst = true;        // 最新档挑的 == 列表第一条
            for (const QString &game : games) {
                const LoaderMeta::LoaderVersionList list =
                    LoaderMeta::listNeoForgeVersions(xml, game);
                if (list.isEmpty()) {
                    allNonEmpty = false;
                    continue;
                }
                QString firstStable;
                for (const LoaderMeta::LoaderVersionEntry &entry : list) {
                    const bool expected = !entry.version.contains(QLatin1Char('-')) &&
                                          !entry.version.contains(QLatin1Char('+'));
                    if (entry.stable != expected) {
                        flagsOk = false;
                    }
                    if (firstStable.isEmpty() && entry.stable) {
                        firstStable = entry.version;
                    }
                }
                // 两档挑法必须和列表走同一份解析 —— 不能列表一套、挑版本又一套
                if (LoaderMeta::pickNeoForgeVersion(xml, game).version != firstStable) {
                    stableIsFirstStable = false;
                }
                if (LoaderMeta::pickNeoForgeVersion(xml, game, LoaderMeta::LoaderChannel::Newest)
                        .version != list.first().version) {
                    newestIsFirst = false;
                }
            }
            check(allNonEmpty, "Y7 那四个游戏版本都列得出东西");
            check(flagsOk, "Y7 每条的 stable 就是「版本号里没有 - 或 +」（界面靠它标「测试版」）");
            check(stableIsFirstStable,
                  "Y7 稳定档挑出来的 == 列表里第一条正式版（和列表同一处解析，不是两套）");
            check(newestIsFirst, "Y7 最新档挑出来的 == 列表第一条（beta 也在里头挑）");

            // 用户报的那一版的原样数据：26.3 只发过 -beta，9 条。
            const LoaderMeta::LoaderVersionList nf263 =
                LoaderMeta::listNeoForgeVersions(xml, QStringLiteral("26.3"));
            check(nf263.size() == 9, "★ Y7 26.3 那 9 条 -beta 全列出来了（一条都不许漏）",
                  QStringLiteral("%1 条").arg(nf263.size()));
            int stable263 = 0;
            for (const LoaderMeta::LoaderVersionEntry &entry : nf263) {
                stable263 += entry.stable ? 1 : 0;
            }
            // 注意 `nf263.size() == 9` 这一半不能省：不带它的话，"列表被清空"那个老 bug
            // 也能让这条断言变绿（空列表里当然没有正式版）。
            check(nf263.size() == 9 && stable263 == 0 &&
                      LoaderMeta::pickNeoForgeVersion(xml, QStringLiteral("26.3")).version.isEmpty(),
                  "★ Y7 26.3 列出来的 9 条一条正式版都没有 -> 稳定档挑不出来（那条灰掉写「没有正式版」）",
                  QStringLiteral("%1 条 / 正式 %2 条").arg(nf263.size()).arg(stable263));
            check(LoaderMeta::pickNeoForgeVersion(xml, QStringLiteral("26.3"),
                                                  LoaderMeta::LoaderChannel::Newest)
                      .version == QStringLiteral("26.3.0.8-beta"),
                  "★ Y7 26.3 最新档挑得出 26.3.0.8-beta —— 点「下载最新版」装的就是它");

            // beta 不是都挂在尾巴上：1.21.10 里第 3 条就是 beta，前两条是正式版。
            // 所以挑正式版必须逐条看 stable，不能靠"从头往下取第一条"或者比名字长短。
            const LoaderMeta::LoaderVersionList v2110 =
                LoaderMeta::listNeoForgeVersions(xml, QStringLiteral("1.21.10"));
            check(v2110.size() == 63 && v2110.value(0).stable && !v2110.value(2).stable &&
                      v2110.value(2).version == QStringLiteral("21.10.62-beta"),
                  "★ Y7 1.21.10 第 3 条是 21.10.62-beta（beta 混在正式版中间）",
                  QStringLiteral("%1 条，第 3 条 %2")
                      .arg(v2110.size())
                      .arg(v2110.value(2).version));

            // 21.1.x 和 21.10.x 不能串（逐段比前缀那条）
            const QStringList v211 = LoaderMeta::loaderVersionStrings(
                LoaderMeta::listNeoForgeVersions(xml, QStringLiteral("1.21.1")));
            bool noCross = true;
            for (const QString &version : v211) {
                if (version.startsWith(QLatin1String("21.10"))) {
                    noCross = false;
                }
            }
            check(noCross, "Y7 1.21.1 的列表里没有 21.10.x（逐段比前缀，不是比字符串）",
                  QStringLiteral("%1 条").arg(v211.size()));
        }
    }

    // --- Y8. OptiFine：两条路各列各的，但正式版那一段必须一致 -----------------
    //
    // 两条路**不逐条一致**：预览版的覆盖范围本来就不同 —— 官方页按小节走（1.18.2 只有
    // 3 条正式版），镜像是全量（3 条正式版 + 8 条预览版）。所以这里钉的是"正式版子序列
    // 一致"，不是逐条一致。
    {
        QFile listFixture(metaDir + "/optifine-1.18.2-list.json");
        QFile pageFixture(metaDir + "/optifine-downloads.html");
        const bool haveList = listFixture.open(QIODevice::ReadOnly);
        const bool havePage = pageFixture.open(QIODevice::ReadOnly);
        check(haveList && havePage, "Y8 读到了 OptiFine 那两份真夹具（镜像 JSON + 官方页）");
        if (haveList && havePage) {
            const QJsonArray mirror = QJsonDocument::fromJson(listFixture.readAll()).array();
            const QString html = QString::fromUtf8(pageFixture.readAll());
            listFixture.close();
            pageFixture.close();

            const LoaderMeta::LoaderVersionList pageList =
                LoaderMeta::listOptifineVersionsFromDownloadsPage(html, QStringLiteral("1.18.2"));
            const LoaderMeta::LoaderVersionList mirrorVersionList =
                LoaderMeta::listOptifineVersionsFromMirror(mirror, QStringLiteral("1.18.2"));
            const QStringList fromPage = LoaderMeta::loaderVersionStrings(pageList);
            const QStringList fromMirror = LoaderMeta::loaderVersionStrings(mirrorVersionList);
            const QStringList expected{QStringLiteral("HD_U_H9"), QStringLiteral("HD_U_H7"),
                                       QStringLiteral("HD_U_H6")};
            check(fromPage == expected, "Y8 官方页 1.18.2 列出来是 H9/H7/H6（那一小节就这 3 条）",
                  fromPage.join(QStringLiteral(", ")));

            // 镜像是全量：3 条正式版 + 8 条预览版，而且**混着排**（H9 后面紧跟 H9_pre3），
            // 所以两条路逐条一致在这份真数据上不成立 —— 这里比的是正式版子序列。
            int mirrorStable = 0;
            QStringList mirrorStableList;
            for (const LoaderMeta::LoaderVersionEntry &entry : mirrorVersionList) {
                if (entry.stable) {
                    ++mirrorStable;
                    mirrorStableList << entry.version;
                }
            }
            check(mirror.size() == 11 && fromMirror.size() == 11 && mirrorStable == 3,
                  "Y8 镜像 1.18.2 是 11 条（3 条正式版 + 8 条预览版）",
                  QStringLiteral("原始 %1 / 列表 %2 / 正式版 %3")
                      .arg(mirror.size())
                      .arg(fromMirror.size())
                      .arg(mirrorStable));
            check(fromMirror.size() != fromPage.size(),
                  "★ Y8 两条路**不再逐条一致**（镜像多了老版本的预览版）",
                  QStringLiteral("页 %1 条 / 镜像 %2 条")
                      .arg(fromPage.size())
                      .arg(fromMirror.size()));
            // 一致的是**正式版子序列**：换个下载源不该连正式版都翻个面。
            check(mirrorStableList == expected,
                  "★ Y8 两条路的正式版子序列一致（页 H9/H7/H6 == 镜像里挑出 stable 的那 3 条）",
                  mirrorStableList.join(QStringLiteral(", ")));

            // 挑法（默认档 = 稳定档）：两条路都落在 H9 上，而且 H9 就是列表里第一条正式版。
            check(LoaderMeta::pickOptifineFromDownloadsPage(html, QStringLiteral("1.18.2")).fileName ==
                      QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "Y8 官方页稳定档挑出来是 H9 那个 jar");
            check(LoaderMeta::pickOptifineFromMirror(mirror, QStringLiteral("1.18.2")).fileName ==
                      QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "Y8 镜像稳定档挑出来是 H9 那个 jar");

            // 列表里第一条**正式版**必须就是稳定档挑出来的那个 —— 列出来的和装下去
            // 的是同一个，用户照着列表点不会装到别的东西上。
            QString firstStable;
            for (const LoaderMeta::LoaderVersionEntry &entry : mirrorVersionList) {
                if (entry.stable) {
                    firstStable = entry.version;
                    break;
                }
            }
            check(!firstStable.isEmpty() &&
                      firstStable == LoaderMeta::optifineEditionFromFile(
                                         QStringLiteral("1.18.2"),
                                         LoaderMeta::pickOptifineFromMirror(
                                             mirror, QStringLiteral("1.18.2")).fileName),
                  "★ Y8 镜像里第一条正式版 == 稳定档实际挑的那条", firstStable);

            // 版本号 ↔ 文件名（「钉死一个 OptiFine 版本」那条路就是靠它落地的）
            check(LoaderMeta::optifineInstallerFileName(QStringLiteral("1.18.2"),
                                                       QStringLiteral("HD_U_H9")) ==
                      QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "Y8 版本号 → 文件名（正式版）");
            check(LoaderMeta::optifineInstallerFileName(QStringLiteral("1.18.2"),
                                                       QStringLiteral("HD_U_H9_pre2")) ==
                      QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre2.jar"),
                  "Y8 版本号 → 文件名（预览版要补 preview_ 前缀）");

            // 对着那 11 条真数据**逐条**核两个往返：版本号↔文件名、版本号↔镜像路径两段
            int checkedRows = 0;
            int badRows = 0;
            for (const QJsonValue &value : mirror) {
                const QJsonObject entry = value.toObject();
                const QString fileName = entry.value(QStringLiteral("filename")).toString();
                const QString edition =
                    LoaderMeta::optifineEditionFromFile(QStringLiteral("1.18.2"), fileName);
                ++checkedRows;
                if (LoaderMeta::optifineInstallerFileName(QStringLiteral("1.18.2"), edition) !=
                    fileName) {
                    ++badRows;
                }
                const LoaderMeta::OptiFineMirrorPath path =
                    LoaderMeta::optifineMirrorPathFromEdition(edition);
                if (path.type != entry.value(QStringLiteral("type")).toString() ||
                    path.patch != entry.value(QStringLiteral("patch")).toString()) {
                    ++badRows;
                }
            }
            check(checkedRows == 11 && badRows == 0,
                  "Y8 那 11 条真数据上：版本号↔文件名、版本号↔镜像路径段 两个往返都对",
                  QStringLiteral("核了 %1 条、错 %2 条").arg(checkedRows).arg(badRows));
            // 切点是**最后一个**下划线 —— 这正是上面那 11 条往返全对的理由：
            // 正式版 HD_U_H9 -> HD_U/H9（和镜像自己的 type/patch 一致）
            // 预览版 HD_U_H9_pre2 -> HD_U_H9/pre2（也一致，不是 HD_U/H9）
            // 要是在第一个下划线处切，预览版那 6 条会全错，而正式版那 3 条照样对。
            const LoaderMeta::OptiFineMirrorPath releasePath =
                LoaderMeta::optifineMirrorPathFromEdition(QStringLiteral("HD_U_H9"));
            check(releasePath.type == QStringLiteral("HD_U") &&
                      releasePath.patch == QStringLiteral("H9"),
                  "Y8 切点在最后一个下划线（正式版 HD_U_H9 → HD_U / H9）",
                  QStringLiteral("%1 / %2").arg(releasePath.type, releasePath.patch));
            const LoaderMeta::OptiFineMirrorPath previewPath =
                LoaderMeta::optifineMirrorPathFromEdition(QStringLiteral("HD_U_H9_pre2"));
            check(previewPath.type == QStringLiteral("HD_U_H9") &&
                      previewPath.patch == QStringLiteral("pre2"),
                  "Y8 预览版切出来也是镜像那两个字段（HD_U_H9_pre2 → HD_U_H9 / pre2）",
                  QStringLiteral("%1 / %2").arg(previewPath.type, previewPath.patch));
            // 一个下划线都没有的版本号切不出来，如实回 ok()=false —— 调用方靠它报"认不出
            // 下载地址"，而不是拼一个一定 404 的路径出去（见 adoptOptifineEdition 里那个守卫）。
            check(!LoaderMeta::optifineMirrorPathFromEdition(QStringLiteral("H9")).ok(),
                  "Y8 一个下划线都没有的版本号（H9）回 ok()=false");
            check(!LoaderMeta::optifineMirrorPathFromEdition(QString()).ok(),
                  "Y8 空版本号回 ok()=false");
            check(!LoaderMeta::optifineMirrorPathFromEdition(QStringLiteral("HD_")).ok(),
                  "Y8 尾巴是空的（HD_）也回 ok()=false，不给出半个路径");
        }
    }

    // --- Y9. Fabric：列表和挑法走同一份解析 --------------------------------
    {
        // 夹具里那份是 profile（薄 json），不是「版本列表」那个接口的回复，
        // 所以这里按接口的形状造一份最小的：{loader:{version,stable}}。
        const QJsonArray list =
            QJsonDocument::fromJson(R"([
                {"loader":{"version":"0.19.5","stable":true}},
                {"loader":{"version":"0.19.4","stable":false}},
                {"loader":{"version":"0.19.3","stable":false}},
                {"loader":{"version":"","stable":false}}
            ])")
                .array();
        const LoaderMeta::LoaderVersionList entries = LoaderMeta::listFabricLoaderVersions(list);
        check(LoaderMeta::loaderVersionStrings(entries) ==
                  (QStringList{QStringLiteral("0.19.5"), QStringLiteral("0.19.4"),
                               QStringLiteral("0.19.3")}),
              "Y9 Fabric：空版本号那条被跳过、顺序原样（接口本来就是新到旧）",
              LoaderMeta::loaderVersionStrings(entries).join(QStringLiteral(", ")));
        check(entries.value(0).stable && !entries.value(1).stable,
              "Y9 Fabric：stable 只标在接口自己标了的那条上");
        check(LoaderMeta::pickFabricLoader(list).version == QStringLiteral("0.19.5"),
              "Y9 Fabric：挑法和列表走同一份解析，挑出来的还是 0.19.5");
        check(LoaderMeta::pickFabricLoader(QJsonArray()).ok() == false,
              "Y9 Fabric：空列表回 ok()=false（不是回一个空版本的 ok()=true）");
    }

    section("Y10. 版本号下面那行小字：发布日期（精确到日）");
    // ---------------------------------------------------------------------
    // 要求：「在版本号下面小字写发布时间（精确到日）」。那一行现在是
    // releaseDateText() 从 releaseTime 里切出来的 —— 走的必须是真清单那 915 条
    // （每一条都是 `T11:23:02+00:00` 这种 25 个字符的 ISO8601），
    // 不是自己编两个好看的字符串。
    {
        int cut = 0;
        int tenChars = 0;
        QSet<QString> dates;
        for (const MinecraftVersion &version : manifest) {
            const QString text = releaseDateText(version.releaseTime);
            if (!text.isEmpty()) {
                ++cut;
                dates.insert(text);
            }
            // **精确到日**：切出来就是那 10 个字符，不是 `T11:23:02+00:00`
            // 这种一长串，也不是 `2026/09/15` 那种换个写法。
            if (text.size() == 10 && text == version.releaseTime.left(10)) {
                ++tenChars;
            }
        }
        check(cut == manifest.size() && tenChars == manifest.size(),
              "Y10 915 条真清单的 releaseTime 全都切成了 yyyy-MM-dd（一个空的都没有）",
              QStringLiteral("%1/%2 条有日期，%3 条正好是前 10 个字符")
                  .arg(cut)
                  .arg(manifest.size())
                  .arg(tenChars));
        check(dates.size() > 100,
              "Y10 那 915 条切出来的日期有 100 多种（真值，不是写死的）",
              QString::number(dates.size()));
    }
    check(releaseDateText(QStringLiteral("2026-09-15T23:30:00+00:00")) ==
              QStringLiteral("2026-09-15"),
          "Y10 UTC 当天最后一刻那条还是当天（不许被本机时区推到第二天）",
          releaseDateText(QStringLiteral("2026-09-15T23:30:00+00:00")));
    check(releaseDateText(QStringLiteral("2026-09-15T00:30:00+00:00")) ==
              QStringLiteral("2026-09-15"),
          "Y10 UTC 当天刚开始那条也还是当天（不许被推到前一天）",
          releaseDateText(QStringLiteral("2026-09-15T00:30:00+00:00")));
    check(releaseDateText(QStringLiteral("2026-09-15T11:23:02Z")) == QStringLiteral("2026-09-15"),
          "Y10 `Z` 那种写法也认（清单换写法不至于整列空掉）",
          releaseDateText(QStringLiteral("2026-09-15T11:23:02Z")));
    check(releaseDateText(QStringLiteral("  2026-09-15T11:23:02+00:00  ")) ==
              QStringLiteral("2026-09-15"),
          "Y10 两头有空白也照样切（值从别的地方来的时候带空白是常事）");
    // 兜底那条：认不出来的时候，前 10 个字符长得像日期就认它。
    check(releaseDateText(QStringLiteral("2026-09-15 11:23:02")) == QStringLiteral("2026-09-15"),
          "Y10 不带 T 的写法（`2026-09-15 11:23:02`）也切出那 10 个字符",
          releaseDateText(QStringLiteral("2026-09-15 11:23:02")));
    check(releaseDateText(QStringLiteral("2026-09-15")) == QStringLiteral("2026-09-15"),
          "Y10 只给到日的那半截原样用");
    // 认不出来就回空串 —— 调用方（代理）见空串就**不画那一行**，所以这里不能瞎猜一个日期。
    check(releaseDateText(QString()).isEmpty() && releaseDateText(QStringLiteral("   ")).isEmpty(),
          "Y10 空值 / 只有空白回空串（那一行就不画，而不是画个 0000-00-00）");
    check(releaseDateText(QStringLiteral("not-a-date")).isEmpty() &&
              releaseDateText(QStringLiteral("2026-13-45")).isEmpty() &&
              releaseDateText(QStringLiteral("2026-09-1")).isEmpty(),
          "Y10 认不出来的、月份日期越界的、只给到 9 号那种都回空串（不瞎猜）",
          releaseDateText(QStringLiteral("2026-13-45")));
}

