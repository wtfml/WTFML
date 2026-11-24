#include "Support.hpp"
#include <QProcess>

void runResourceAndDataPackTests() {
    QTemporaryDir scratch;
    check(scratch.isValid(), "PL 建得出一个临时目录来摆夹具");
    if (!scratch.isValid()) {
        return;
    }

    const auto writeBytes = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return file.write(bytes) == bytes.size();
    };
    const auto readBytes = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        return file.readAll();
    };

    section("PL1. 资源包：提取时把「启用 / 次序」记进那一段");
    const QString sourceInstance = scratch.filePath(QStringLiteral("rpSource"));
    QDir().mkpath(sourceInstance + QStringLiteral("/resourcepacks"));
    const QByteArray sourceOptions = QByteArrayLiteral(
        "resourcePacks:[\"vanilla\",\"file/A.zip\",\"file/B.zip\"]\nfov:0.5\n");
    check(writeBytes(sourceInstance + QStringLiteral("/options.txt"), sourceOptions),
          "PL1 夹具：源实例的 options.txt 写下去了");
    for (const char *name : {"A.zip", "B.zip", "C.zip"}) {
        check(writeBytes(sourceInstance + QStringLiteral("/resourcepacks/") +
                             QLatin1String(name),
                         QByteArrayLiteral("zip")),
              "PL1 夹具：资源包文件写下去了");
    }

    const Presets::ResourcePackOrderState order =
        Presets::readResourcePackOrderState(sourceInstance);
    check(order.problem.isEmpty() && order.hasList && order.enabledOrder.size() == 3,
          "★★ PL1 `resourcePacks:` 那一行读得出来（3 项，低优先级在前）",
          order.problem);

    Presets::PresetSection packsSection;
    packsSection.id = QStringLiteral("rp");
    packsSection.kind = Presets::SectionKind::GenericFiles;
    packsSection.mode = Presets::ApplyMode::CopyIfMissing;
    for (const char *name : {"A.zip", "B.zip", "C.zip"}) {
        QJsonObject item;
        item.insert(QStringLiteral("path"),
                    QStringLiteral("resourcepacks/") + QLatin1String(name));
        item.insert(QStringLiteral("copyOnly"), true);
        packsSection.items.append(item);
    }
    check(Presets::annotateResourcePackSection(packsSection, order),
          "★★ PL1 启用信息真加进去了");
    check(Presets::resourcePackSectionHasOrdering(packsSection),
          "★★ PL1 这一段现在带启用信息");
    const QJsonObject itemA = packsSection.items.at(0).toObject();
    const QJsonObject itemC = packsSection.items.at(2).toObject();
    check(itemA.value(QStringLiteral("enabled")).toBool() &&
              itemA.value(QStringLiteral("priority")).toInt(-1) == 1,
          "★★ PL1 A.zip 记成「开着，优先级 1」");
    check(!itemC.value(QStringLiteral("enabled")).toBool() &&
              !itemC.contains(QStringLiteral("priority")),
          "★★ PL1 C.zip 记成「关着」，而且不编一个 priority 出来");
    check(packsSection.items.at(1).toObject().value(QStringLiteral("priority")).toInt(-1) == 2,
          "★★ PL1 B.zip 的优先级是 2（不是数组下标 1）");

    // 没读到那一行（新实例）⇒ 一格都不加，绝不拿"全是关着"去冒充。
    const QString noOptionsInstance = scratch.filePath(QStringLiteral("rpNoOptions"));
    QDir().mkpath(noOptionsInstance);
    const Presets::ResourcePackOrderState none =
        Presets::readResourcePackOrderState(noOptionsInstance);
    Presets::PresetSection untouched = packsSection;
    untouched.items = QJsonArray();
    for (const char *name : {"A.zip", "C.zip"}) {
        QJsonObject item;
        item.insert(QStringLiteral("path"),
                    QStringLiteral("resourcepacks/") + QLatin1String(name));
        untouched.items.append(item);
    }
    check(!Presets::annotateResourcePackSection(untouched, none),
          "★★ PL1 没读到 options.txt ⇒ 一格启用信息都不加");
    check(!untouched.items.at(0).toObject().contains(QStringLiteral("enabled")),
          "★★ PL1 不许把「没读到」写成「全是关着」");

    section("PL2. 资源包：按预设重排并启用，目标上没点名的留着");
    const QString targetInstance = scratch.filePath(QStringLiteral("rpTarget"));
    QDir().mkpath(targetInstance + QStringLiteral("/resourcepacks"));
    const QByteArray targetOptions = QByteArrayLiteral(
        "resourcePacks:[\"vanilla\",\"file/old.zip\"]\nfov:0.25\n");
    check(writeBytes(targetInstance + QStringLiteral("/options.txt"), targetOptions),
          "PL2 夹具：目标实例的 options.txt 写下去了");
    for (const char *name : {"old.zip", "A.zip", "B.zip", "C.zip"}) {
        check(writeBytes(targetInstance + QStringLiteral("/resourcepacks/") +
                             QLatin1String(name),
                         QByteArrayLiteral("zip")),
              "PL2 夹具：目标上的资源包文件写下去了");
    }
    const QString rpBackupRoot = scratch.filePath(QStringLiteral("rpBackups"));

    const Presets::ResourcePackOrderReport applied =
        Presets::applyResourcePackOrdering(targetInstance, packsSection, rpBackupRoot);
    check(applied.ok && applied.hasOrdering && applied.wrote,
          "★★ PL2 资源包的启用顺序写进去了", applied.problem);
    check(applied.enabled == 2, "★★ PL2 报告里「启用了 2 个」",
          QString::number(applied.enabled));
    const Presets::ResourcePackOrderState afterApply =
        Presets::readResourcePackOrderState(targetInstance);
    check(afterApply.enabledOrder ==
              QStringList{QStringLiteral("vanilla"), QStringLiteral("file/old.zip"),
                          QStringLiteral("file/A.zip"), QStringLiteral("file/B.zip")},
          "★★★ PL2 内置包与没点名的 old.zip 原样留着，预设点名的按优先级排在末尾",
          afterApply.enabledOrder.join(QStringLiteral(", ")));
    check(!afterApply.enabledOrder.contains(QStringLiteral("file/C.zip")),
          "★★ PL2 预设里明写「关着」的 C.zip 没有被打开");
    check(readBytes(targetInstance + QStringLiteral("/options.txt"))
              .contains(QByteArrayLiteral("fov:0.25")),
          "★★ PL2 同一个文件里别的键一个字节都没动");

    const Presets::ResourcePackOrderReport again =
        Presets::applyResourcePackOrdering(targetInstance, packsSection, rpBackupRoot);
    check(again.ok && again.hasOrdering && !again.wrote,
          "★★ PL2 顺序已经对了 ⇒ 不重写 options.txt");

    section("PL3. 资源包：没有启用信息 / 目标太老 ⇒ 只部署，不冒充启用");
    Presets::PresetSection plainSection = packsSection;
    for (int i = 0; i < plainSection.items.size(); ++i) {
        QJsonObject item = plainSection.items.at(i).toObject();
        item.remove(QStringLiteral("enabled"));
        item.remove(QStringLiteral("priority"));
        plainSection.items.replace(i, item);
    }
    check(!Presets::resourcePackSectionHasOrdering(plainSection),
          "PL3 去掉那两格之后这一段就没有启用信息了");
    const QByteArray beforePlain = readBytes(targetInstance + QStringLiteral("/options.txt"));
    const Presets::ResourcePackOrderReport plain =
        Presets::applyResourcePackOrdering(targetInstance, plainSection, rpBackupRoot);
    check(plain.ok && !plain.hasOrdering && !plain.wrote,
          "★★ PL3 没有启用信息 ⇒ 只部署、不写 options.txt");
    check(readBytes(targetInstance + QStringLiteral("/options.txt")) == beforePlain,
          "★★★ PL3 那份 options.txt 一个字节都没动");

    const QString oldInstance = scratch.filePath(QStringLiteral("rpOld"));
    QDir().mkpath(oldInstance + QStringLiteral("/resourcepacks"));
    const QByteArray oldOptions = QByteArrayLiteral("fov:0.5\n");
    writeBytes(oldInstance + QStringLiteral("/options.txt"), oldOptions);
    writeBytes(oldInstance + QStringLiteral("/resourcepacks/A.zip"), QByteArrayLiteral("zip"));
    const Presets::ResourcePackOrderReport oldTarget =
        Presets::applyResourcePackOrdering(oldInstance, packsSection, rpBackupRoot);
    check(oldTarget.ok && !oldTarget.wrote && !oldTarget.details.isEmpty(),
          "★★ PL3 目标 options.txt 里没有 resourcePacks 那一行 ⇒ 只部署、不添加那一行",
          oldTarget.problem);
    check(readBytes(oldInstance + QStringLiteral("/options.txt")) == oldOptions,
          "★★★ PL3 老版本那份 options.txt 一个字节都没动");

    section("PL4. 数据包：从存档里收出来");
    const QString dpSource = scratch.filePath(QStringLiteral("dpSource"));
    QDir().mkpath(dpSource + QStringLiteral("/saves/W1/datapacks"));
    QDir().mkpath(dpSource + QStringLiteral("/saves/W2"));
    QDir().mkpath(dpSource + QStringLiteral("/saves/W3/datapacks/dirpack"));
    const QByteArray packOne = QByteArrayLiteral("pack-one");
    check(writeBytes(dpSource + QStringLiteral("/saves/W1/datapacks/pack1.zip"), packOne),
          "PL4 夹具：W1 里那个 zip 写下去了");
    check(writeBytes(dpSource + QStringLiteral("/saves/W3/datapacks/dirpack/pack.mcmeta"),
                     QByteArrayLiteral("{}")),
          "PL4 夹具：W3 里那个目录包写下去了");

    const QString dpFiles = scratch.filePath(QStringLiteral("dpFiles"));
    QDir().mkpath(dpFiles);
    const Presets::DataPackCaptureReport dpCapture = Presets::captureDataPacks(dpSource, dpFiles);
    check(dpCapture.ok, "★★ PL4 数据包收得回来", dpCapture.problem);
    check(dpCapture.packs == 2 && dpCapture.worlds == 2 && dpCapture.sections.size() == 1,
          "★★★ PL4 两个世界收到两个包，做成一段",
          QStringLiteral("packs=%1 worlds=%2 sections=%3")
              .arg(dpCapture.packs)
              .arg(dpCapture.worlds)
              .arg(dpCapture.sections.size()));
    const Presets::PresetSection dpSection =
        dpCapture.sections.isEmpty() ? Presets::PresetSection() : dpCapture.sections.first();
    check(Presets::dataPackHandlesSection(dpSection),
          "★★★ PL4 那一段的 id 与种类能重新认出来（旧版读到会当成「没有适配器」）");
    check(dpSection.kind == Presets::SectionKind::Custom &&
              dpSection.mode == Presets::ApplyMode::CopyIfMissing,
          "★★ PL4 段种类是 Custom、落法是「缺了才拷」");
    check(QFileInfo::exists(dpFiles + QStringLiteral("/saves/W1/datapacks/pack1.zip")) &&
              QFileInfo::exists(dpFiles + QStringLiteral("/saves/W3/datapacks/dirpack/pack.mcmeta")),
          "★★ PL4 包真的拷进了预设的 files/（目录包整棵拷）");
    // 预设里再摆一份「目标上没有那个世界」的包：PL5 要验的正是那一条。
    QDir().mkpath(dpFiles + QStringLiteral("/saves/W2/datapacks"));
    check(writeBytes(dpFiles + QStringLiteral("/saves/W2/datapacks/pack1.zip"), packOne),
          "PL4 夹具：预设里 W2 那一份也摆好了（用来验「待应用」）");

    section("PL4b. 未登记的中断目录不能仅凭前缀自动清理或归位");
    {
        const auto put = [&writeBytes](const QString &path, const QByteArray &bytes) {
            return QDir().mkpath(QFileInfo(path).absolutePath()) && writeBytes(path, bytes);
        };
        const QString source = scratch.filePath(QStringLiteral("dpCrashSource"));
        const QString root = scratch.filePath(QStringLiteral("dpCrashRoot"));
        const QString relative = QStringLiteral("/saves/W9/datapacks/");
        check(put(source + relative + QStringLiteral("dirpack/pack.mcmeta"), "original"),
              "PL4b 源数据包创建成功");
        const auto first = Presets::captureDataPacks(source, root);
        const QString live = root + relative + QStringLiteral("dirpack");
        check(first.ok, "PL4b 首次采集成功", first.problem);
        const QString token = Presets::dataPackReplaceToken(QStringLiteral("dirpack"));
        const QString old = root + relative + QStringLiteral(".wtfml-old-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000000");
        const QString unknown = root + relative + QStringLiteral(".wtfml-copy-") + token
            + QStringLiteral("-user-not-a-uuid");
        check(QDir().rename(live, old) && put(unknown + "/keep.txt", "USER"),
              "PL4b 摆出缺失目标、未登记旧目录和相似前缀用户目录");
        const auto missing = Presets::captureDataPacks(source, root);
        check(!missing.problems.isEmpty() && !QFileInfo::exists(live)
                  && readBytes(old + "/pack.mcmeta") == "original"
                  && readBytes(unknown + "/keep.txt") == "USER",
              "PL4b 缺失目标时不猜测归属，原件和相似前缀目录均完整保留");
        check(put(live + "/pack.mcmeta", "CURRENT"), "PL4b 创建另一份当前内容");
        const auto present = Presets::captureDataPacks(source, root);
        check(!present.problems.isEmpty()
                  && readBytes(live + "/pack.mcmeta") == "CURRENT"
                  && readBytes(old + "/pack.mcmeta") == "original"
                  && readBytes(unknown + "/keep.txt") == "USER",
              "PL4b 目标存在也不自动删除旧目录或同前缀用户数据");
    }

    section("PL4c. 有登记的中断换包按登记回滚（原件放回原位、暂存收掉）");
    {
        const auto put = [&writeBytes](const QString &path, const QByteArray &bytes) {
            return QDir().mkpath(QFileInfo(path).absolutePath()) && writeBytes(path, bytes);
        };
        const QString source = scratch.filePath(QStringLiteral("dpTxnSource"));
        const QString root = scratch.filePath(QStringLiteral("dpTxnRoot"));
        const QString relative = QStringLiteral("/saves/W9/datapacks/");
        check(put(source + relative + QStringLiteral("dirpack/pack.mcmeta"), "original"),
              "PL4c 源数据包创建成功");
        const auto first = Presets::captureDataPacks(source, root);
        check(first.ok, "PL4c 首次采集成功", first.problem);

        const QString token = Presets::dataPackReplaceToken(QStringLiteral("dirpack"));
        const QString oldName = QStringLiteral(".wtfml-old-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000001");
        const QString stagingName = QStringLiteral(".wtfml-copy-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000002");
        const QString live = root + relative + QStringLiteral("dirpack");
        const QString journalPath =
            root + relative + QStringLiteral(".wtfml-txn-") + token + QStringLiteral(".json");
        check(QDir().rename(live, root + relative + oldName)
                  && put(root + relative + stagingName + QStringLiteral("/partial.txt"),
                         "PARTIAL"),
              "PL4c 摆出「目标已挪走、新内容还没发布」的现场");
        const QByteArray journal = QByteArrayLiteral(
            "{\"format\":\"wtfml-datapack-txn/1\",\"target\":\"dirpack\",\"staging\":\"")
            + stagingName.toUtf8() + QByteArrayLiteral("\",\"displaced\":\"")
            + oldName.toUtf8() + QByteArrayLiteral("\",\"state\":\"displaced\"}");
        check(put(journalPath, journal), "PL4c 登记落盘");

        const auto recovered = Presets::captureDataPacks(source, root);
        check(recovered.ok && readBytes(live + QStringLiteral("/pack.mcmeta")) == "original",
              "★★★ PL4c 登记在 ⇒ 原件按登记放回原位，采集照常",
              recovered.problem + recovered.problems.join(QStringLiteral("；")));
        check(!QFileInfo::exists(root + relative + oldName)
                  && !QFileInfo::exists(root + relative + stagingName)
                  && !QFileInfo::exists(journalPath),
              "★★★ PL4c 暂存、旧内容、登记都收干净了");
    }

    section("PL4d. 登记读不懂 ⇒ 一个字节都不动，采集报问题");
    {
        const auto put = [&writeBytes](const QString &path, const QByteArray &bytes) {
            return QDir().mkpath(QFileInfo(path).absolutePath()) && writeBytes(path, bytes);
        };
        const QString source = scratch.filePath(QStringLiteral("dpBadTxnSource"));
        const QString root = scratch.filePath(QStringLiteral("dpBadTxnRoot"));
        const QString relative = QStringLiteral("/saves/W9/datapacks/");
        check(put(source + relative + QStringLiteral("dirpack/pack.mcmeta"), "original"),
              "PL4d 源数据包创建成功");
        const auto first = Presets::captureDataPacks(source, root);
        check(first.ok, "PL4d 首次采集成功", first.problem);

        const QString token = Presets::dataPackReplaceToken(QStringLiteral("dirpack"));
        const QString oldName = QStringLiteral(".wtfml-old-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000003");
        const QString stagingName = QStringLiteral(".wtfml-copy-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000004");
        const QString live = root + relative + QStringLiteral("dirpack");
        const QString journalPath =
            root + relative + QStringLiteral(".wtfml-txn-") + token + QStringLiteral(".json");
        check(QDir().rename(live, root + relative + oldName)
                  && put(root + relative + stagingName + QStringLiteral("/keep.txt"), "PARTIAL"),
              "PL4d 摆出现场");
        check(put(journalPath,
                  QByteArrayLiteral("{\"format\":\"some-other-app/1\",\"target\":\"dirpack\"}")),
              "PL4d 摆一份读不懂的登记");

        const auto refused = Presets::captureDataPacks(source, root);
        check(!refused.problems.isEmpty() && !QFileInfo::exists(live)
                  && readBytes(root + relative + oldName + QStringLiteral("/pack.mcmeta"))
                         == "original"
                  && readBytes(root + relative + stagingName + QStringLiteral("/keep.txt"))
                         == "PARTIAL"
                  && readBytes(journalPath).contains("some-other-app"),
              "★★★ PL4d 读不懂的登记保留全部现场，不猜归属",
              refused.problems.join(QStringLiteral("；")));
    }

    section("PL4e. 登记说「已发布」⇒ 收掉旧内容、保留新内容");
    {
        const auto put = [&writeBytes](const QString &path, const QByteArray &bytes) {
            return QDir().mkpath(QFileInfo(path).absolutePath()) && writeBytes(path, bytes);
        };
        const QString source = scratch.filePath(QStringLiteral("dpPubTxnSource"));
        const QString root = scratch.filePath(QStringLiteral("dpPubTxnRoot"));
        const QString relative = QStringLiteral("/saves/W9/datapacks/");
        // 源那一侧摆的是**另一个包**：采集会走同目录的 copyOverwrite（顺带扫登记），
        // 但不会覆盖 `dirpack` 本身 —— 这样才能看到"新发布的那一份没有被回滚掉"。
        check(put(source + relative + QStringLiteral("otherpack/pack.mcmeta"), "OTHER"),
              "PL4e 源数据包创建成功");
        const auto first = Presets::captureDataPacks(source, root);
        check(first.ok, "PL4e 首次采集成功", first.problem);

        const QString token = Presets::dataPackReplaceToken(QStringLiteral("dirpack"));
        const QString oldName = QStringLiteral(".wtfml-old-") + token
            + QStringLiteral("-00000000-0000-0000-0000-000000000005");
        const QString live = root + relative + QStringLiteral("dirpack");
        const QString journalPath =
            root + relative + QStringLiteral(".wtfml-txn-") + token + QStringLiteral(".json");
        check(put(live + QStringLiteral("/pack.mcmeta"), "NEW")
                  && put(root + relative + oldName + QStringLiteral("/pack.mcmeta"), "original"),
              "PL4e 摆出「新的已发布、旧的还没收」的现场");
        check(put(journalPath,
                  QByteArrayLiteral("{\"format\":\"wtfml-datapack-txn/1\","
                                    "\"target\":\"dirpack\",\"staging\":\"\",\"displaced\":\"")
                      + oldName.toUtf8()
                      + QByteArrayLiteral("\",\"state\":\"published\"}")),
              "PL4e 登记落盘");

        const auto completed = Presets::captureDataPacks(source, root);
        check(completed.ok && readBytes(live + QStringLiteral("/pack.mcmeta")) == "NEW",
              "★★★ PL4e 已发布的那一份不动，采集照常", completed.problem);
        check(!QFileInfo::exists(root + relative + oldName) && !QFileInfo::exists(journalPath),
              "★★★ PL4e 旧内容与登记都收掉了");
    }

    section("PL5. 数据包：有同名世界就部署，没有就记「待应用」且不建世界");
    const QString dpTarget = scratch.filePath(QStringLiteral("dpTarget"));
    QDir().mkpath(dpTarget + QStringLiteral("/saves/W1"));

    QList<Presets::DataPackItem> wanted;
    Presets::DataPackItem one;
    one.world = QStringLiteral("W1");
    one.name = QStringLiteral("pack1.zip");
    wanted.append(one);
    Presets::DataPackItem missing;
    missing.world = QStringLiteral("W2");
    missing.name = QStringLiteral("pack1.zip");
    wanted.append(missing);
    QList<Presets::PresetSection> made;
    QString makeWhy;
    check(Presets::sectionsFromDataPacks(wanted, &made, &makeWhy),
          "PL5 由两项做出一段", makeWhy);
    check(made.size() == 1, "PL5 两项归在同一段");

    const Presets::DataPackApplyReport dpApplied =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::CopyIfMissing, false);
    check(dpApplied.ok, "★★ PL5 数据包落得下去", dpApplied.problem);
    check(dpApplied.deployed == 1 && dpApplied.deferred == 1 && dpApplied.skipped == 0,
          "★★★ PL5 「部署 1 个、待应用 1 个」分开报（部署不许冒充成两个都装上了）",
          QStringLiteral("%1 / %2 / %3")
              .arg(dpApplied.deployed)
              .arg(dpApplied.deferred)
              .arg(dpApplied.skipped));
    check(readBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip")) == packOne,
          "★★ PL5 已有的世界 W1 里那份真进去了");
    check(!QFileInfo::exists(dpTarget + QStringLiteral("/saves/W2")),
          "★★★ PL5 没有的世界没有被新建出来");
    check(readBytes(dpTarget + QStringLiteral("/datapacks-pending/W2/pack1.zip")) == packOne,
          "★★★ PL5 那一个进了 <实例>/datapacks-pending/<世界>/");
    check(Presets::pendingDataPacks(dpTarget, QStringLiteral("W2")) ==
              QStringList{QStringLiteral("pack1.zip")},
          "★★ PL5 待应用清单里记着它");
    check(readBytes(dpTarget + QStringLiteral("/wtfml.datapacks"))
              .contains(QByteArrayLiteral("wtfml-datapacks/1")),
          "★★ PL5 清单带着格式标记（读不懂的版本不会被当成空的盖掉）");
    check(!QFileInfo::exists(dpTarget + QStringLiteral("/saves/W1/level.dat")),
          "★★★ PL5 这一层不碰 level.dat ⇒ 不会冒充「游戏里已经启用」");
    check(dpApplied.details.join(QStringLiteral("；")).contains(QStringLiteral("/datapack list")),
          "★★ PL5 报告里明说「开没开由游戏里 /datapack list 决定」",
          dpApplied.details.join(QStringLiteral("；")));

    const Presets::DataPackApplyReport dpAgain =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::CopyIfMissing, false);
    check(dpAgain.ok && dpAgain.deployed == 0 && dpAgain.deferred == 0 && dpAgain.skipped == 2,
          "★★ PL5 再来一趟「缺了才拷」⇒ 两个都跳过、一个字节不写",
          QStringLiteral("%1/%2/%3")
              .arg(dpAgain.deployed)
              .arg(dpAgain.deferred)
              .arg(dpAgain.skipped));

    section("PL6. 数据包：合并 / 追加做不了主，什么都别写；跳过就是跳过");
    const QByteArray pendingBefore = readBytes(dpTarget + QStringLiteral("/wtfml.datapacks"));
    const QByteArray deployedBefore =
        readBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip"));
    const Presets::DataPackApplyReport merged =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::Merge, false);
    check(merged.ok && merged.needsChoice == 2 && merged.deployed == 0 && merged.deferred == 0,
          "★★★ PL6 「合并」对数据包报「待选择 2 个」，一个都不写",
          QStringLiteral("%1/%2/%3").arg(merged.needsChoice).arg(merged.deployed).arg(merged.deferred));
    check(!merged.details.isEmpty(), "★★ PL6 而且说清楚该怎么办");
    check(readBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip")) == deployedBefore &&
              readBytes(dpTarget + QStringLiteral("/wtfml.datapacks")) == pendingBefore,
          "★★★ PL6 盘上那份包和待应用清单逐字节相同");

    const Presets::DataPackApplyReport skipped =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::Skip, false);
    check(skipped.ok && skipped.skipped == 2 && skipped.deployed == 0,
          "★★ PL6 跳过那档只报跳过");

    section("PL7. 数据包：「覆盖」两趟 —— 不点头一个字节都不动");
    const QByteArray oldTargetPack = QByteArrayLiteral("old-on-target");
    check(writeBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip"), oldTargetPack),
          "PL7 夹具：把目标上那份改成另一份");
    const Presets::DataPackApplyReport notConfirmed =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::Replace, false);
    check(!notConfirmed.ok && !notConfirmed.problem.isEmpty(),
          "★★★ PL7 覆盖没确认 ⇒ 整段不动", notConfirmed.problem);
    check(readBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip")) == oldTargetPack,
          "★★★ PL7 目标上那份一个字节都没动");

    const Presets::DataPackApplyReport replaced =
        Presets::applyDataPacks(dpFiles, dpTarget, made.first(), Presets::ApplyMode::Replace, true);
    check(replaced.ok && replaced.deployed == 1 && replaced.deferred == 1,
          "★★ PL7 确认之后才真覆盖", replaced.problem);
    check(readBytes(dpTarget + QStringLiteral("/saves/W1/datapacks/pack1.zip")) == packOne,
          "★★ PL7 目标上那份换成了预设里的");

    section("PL8. 数据包：路径形状不对 ⇒ 整段不认（不猜）");
    Presets::PresetSection bad = dpSection;
    QJsonObject badItem;
    badItem.insert(QStringLiteral("path"), QStringLiteral("saves/W1/../W2/datapacks/x.zip"));
    bad.items = QJsonArray{badItem};
    QList<Presets::DataPackItem> ignored;
    QString badWhy;
    check(!Presets::dataPackItemsFromSection(bad, &ignored, &badWhy) && !badWhy.isEmpty(),
          "★★★ PL8 带 `..` 的路径整段不认", badWhy);
    badItem.insert(QStringLiteral("path"), QStringLiteral("mods/x.jar"));
    bad.items = QJsonArray{badItem};
    check(!Presets::dataPackItemsFromSection(bad, &ignored, &badWhy),
          "★★ PL8 不是 saves/<世界>/datapacks/<包> 的路径也不认");
    badItem.insert(QStringLiteral("path"), QStringLiteral("saves/W1/datapacks/x.zip"));
    badItem.insert(QStringLiteral("world"), QStringLiteral("W2"));
    bad.items = QJsonArray{badItem};
    check(!Presets::dataPackItemsFromSection(bad, &ignored, &badWhy),
          "★★ PL8 世界名和路径对不上也不认");

    section("PL9. 计划与应用引擎都认数据包：版本太老整行做不了");
    // ---------------------------------------------------------------------
    // 计划这边另开一个干净目标：`dpTarget` 上 W1 那份已经在 PL5 落过了（"缺了才拷"会
    // 把部署数算成跳过），这一节要看的是「部署 1 / 待应用 1」那两格。
    const QString dpPlanTarget = scratch.filePath(QStringLiteral("dpPlanTarget"));
    QDir().mkpath(dpPlanTarget + QStringLiteral("/saves/W1"));
    Presets::PlanRequest planRequest;
    planRequest.presetFilesRoot = dpFiles;
    planRequest.sections = made;
    planRequest.target.instanceRoot = dpPlanTarget;
    planRequest.target.supportsDataPacks = false;
    const Presets::ApplyPlan blockedPlan = Presets::planPresetApplication(planRequest);
    check(blockedPlan.ok && blockedPlan.rows.size() == 1 && blockedPlan.rows.first().blocked,
          "★★★ PL9 17w43a 之前 ⇒ 数据包那一行是「做不了」",
          blockedPlan.rows.isEmpty() ? QString() : blockedPlan.rows.first().problem);
    check(blockedPlan.rows.first().problem.contains(QStringLiteral("17w43a")),
          "★★ PL9 理由说清楚是哪条版本线", blockedPlan.rows.first().problem);

    planRequest.target.supportsDataPacks = true;
    const Presets::ApplyPlan livePlan = Presets::planPresetApplication(planRequest);
    check(livePlan.ok && livePlan.rows.size() == 1 && !livePlan.rows.first().blocked,
          "★★ PL9 支持的版本上这一段算得出来");
    check(livePlan.rows.first().count == 1 && livePlan.rows.first().deferred == 1,
          "★★★ PL9 计划表上「1 个部署、1 个待应用」是两格",
          QStringLiteral("count=%1 deferred=%2")
              .arg(livePlan.rows.first().count)
              .arg(livePlan.rows.first().deferred));
    check(Presets::applyPlanRowText(livePlan.rows.first()).contains(QStringLiteral("待应用")),
          "★★★ PL9 计划表那一行真写着「待应用」",
          Presets::applyPlanRowText(livePlan.rows.first()));
    check(livePlan.writesAnything() && livePlan.deferredCount() == 1,
          "★★ PL9 「有待应用」也算这一趟会写东西（它要写盘上的 pending 目录）");

    // 应用引擎那一侧：版本闸真的一个字节都不写。
    const QString presetsGateRoot = scratch.filePath(QStringLiteral("presetsGate"));
    const QString gateInstance = scratch.filePath(QStringLiteral("gateInstance"));
    QDir().mkpath(gateInstance);
    Presets::PresetManifest gateManifest;
    gateManifest.id = Presets::newPresetId();
    gateManifest.name = QStringLiteral("PL 数据包");
    gateManifest.sections = made;
    const QString gateFiles = Presets::filesDir(presetsGateRoot, gateManifest.id);
    for (const Presets::DataPackItem &item : wanted) {
        // 用 world/name 现拼（这两项是自己填的，`relativePath` 那一格本来就空）。
        const QString relative = QStringLiteral("saves/%1/datapacks/%2")
                                     .arg(item.world, item.name);
        QDir().mkpath(QFileInfo(gateFiles + QStringLiteral("/") + relative).absolutePath());
        writeBytes(gateFiles + QStringLiteral("/") + relative, packOne);
    }
    Presets::PresetApplyRequest gateRequest;
    gateRequest.instanceId = QStringLiteral("gate");
    gateRequest.instanceRoot = gateInstance;
    gateRequest.supportsDataPacks = false;
    const Presets::PresetApplyResult gateResult =
        Presets::applyPresetToInstance(presetsGateRoot, gateManifest, gateRequest);
    check(!gateResult.rows.isEmpty() && gateResult.rows.first().blocked,
          "★★★ PL9 应用引擎也认那条版本闸", gateResult.problem);
    check(!gateResult.wroteAnything() && gateResult.deferred == 0,
          "★★★ PL9 被版本闸挡住 ⇒ 不报部署、也不报待应用");
    check(!QFileInfo::exists(gateInstance + QStringLiteral("/wtfml.datapacks")) &&
              !QFileInfo::exists(gateInstance + QStringLiteral("/saves")) &&
              !QFileInfo::exists(gateInstance + QStringLiteral("/datapacks-pending")),
          "★★★ PL9 目标实例上什么都没多出来");

    gateRequest.supportsDataPacks = true;
    QDir().mkpath(gateInstance + QStringLiteral("/saves/W1"));
    const Presets::PresetApplyResult liveResult =
        Presets::applyPresetToInstance(presetsGateRoot, gateManifest, gateRequest);
    check(liveResult.deferred == 1 && liveResult.rows.first().deferred == 1,
          "★★ PL9 支持的版本上应用引擎把「待应用 1 个」报出来",
          QStringLiteral("deferred=%1").arg(liveResult.deferred));
    check(QFileInfo::exists(gateInstance + QStringLiteral("/saves/W1/datapacks/pack1.zip")) &&
              QFileInfo::exists(gateInstance + QStringLiteral("/datapacks-pending/W2/pack1.zip")),
          "★★ PL9 该部署的部署了、该待应用的摆好了");

    section("PL10. 数据包：`saves/<世界>/datapacks` 是 junction ⇒ 一个字节都不往那儿写");
    {
        // ★ 这一条要的是 `pathHasLinkComponent()` 判的**中间那层**：世界目录本身是普通目录
        // （`worldUsable()` 放行），危险的是它底下那个 `datapacks/` 整根指着别处 ——
        // `removeTreeNoLinks()` / 拷贝会一路穿过去，删改的就不是"这个实例里的包"了。
        // 用户这么摆不是搞坏了：存档放网盘、数据包放另一块盘都很常见。
        const QString linkInstance = scratch.filePath(QStringLiteral("dpLinkInstance"));
        const QString outside = scratch.filePath(QStringLiteral("dpOutside"));
        QDir().mkpath(linkInstance + QStringLiteral("/saves/W1"));
        QDir().mkpath(outside);
        const QString linkPath = linkInstance + QStringLiteral("/saves/W1/datapacks");
        const bool madeLink =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(linkPath),
                               QDir::toNativeSeparators(outside)}) == 0
            && QFileInfo(linkPath).isJunction();
        check(madeLink, "★ PL10 前提：真造出一根 junction（`saves/W1/datapacks` → 外面）");
        if (madeLink) {
            QList<Presets::DataPackItem> linkedWanted;
            Presets::DataPackItem linkedItem;
            linkedItem.world = QStringLiteral("W1");
            linkedItem.name = QStringLiteral("pack1.zip");
            linkedWanted.append(linkedItem);
            QList<Presets::PresetSection> linkedSections;
            QString linkedWhy;
            check(Presets::sectionsFromDataPacks(linkedWanted, &linkedSections, &linkedWhy) &&
                      linkedSections.size() == 1,
                  "PL10 由一项做出一段", linkedWhy);

            const Presets::DataPackApplyReport through =
                Presets::applyDataPacks(dpFiles, linkInstance, linkedSections.first(),
                                        Presets::ApplyMode::CopyIfMissing, false);
            check(!through.ok && through.deployed == 0 &&
                      through.problem.contains(QStringLiteral("链接")),
                  "★★ PL10 落包那一趟 refuses（说清楚是链接，不冒充成功）", through.problem);
            check(!QFileInfo::exists(outside + QStringLiteral("/pack1.zip")),
                  "★★★ PL10 链接那一头**没有**多出包来（穿透的话它会出现在外面那个目录里）");
            // 也不许"绕开"成待应用：世界明明在盘上，落 pending 等于给用户记一份假的「待应用」。
            check(!QFileInfo::exists(linkInstance +
                                     QStringLiteral("/datapacks-pending/W1/pack1.zip")),
                  "★★ PL10 也没有改落到 `datapacks-pending/W1/`（世界是在的，那不是「待应用」）");
            // 链接本体只能 rmdir（删除链接，不跟着删那一头）。
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(linkPath)});
        }
    }
}

