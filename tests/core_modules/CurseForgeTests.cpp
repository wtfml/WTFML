#include "Support.hpp"
#include "manage/ModpackInstallReport.hpp"

void runCurseForgeTests() {
    {
        ModpackImport::FileIssue issue; issue.ref = {396246, 8769267};
        issue.projectWebUrl = QStringLiteral("https://www.curseforge.com/minecraft/modpacks/fabulously-optimized");
        check(ModpackImport::curseForgeFileWebUrl(issue) == issue.projectWebUrl + QStringLiteral("/files/8769267"),
              "CF 网页使用平台规范项目路径而不是数字 projects 路由");
        for (const auto &url : {QStringLiteral("https://evil.test/minecraft/modpacks/test"),
                 QStringLiteral("https://www.curseforge.com@evil.test/minecraft/modpacks/test"),
                 QStringLiteral("https://www.curseforge.com/minecraft/modpacks/test?redirect=evil"), QString()}) {
            issue.projectWebUrl = url;
            check(ModpackImport::curseForgeFileWebUrl(issue).isEmpty(), "CF 非规范或缺失网页不打开");
        }
        const auto urls = ModpackImport::parseCurseForgeProjectWebUrls(R"({"data":[{"id":396246,"links":{"websiteUrl":"https://www.curseforge.com/minecraft/modpacks/fabulously-optimized"}}]})");
        check(urls.contains(396246), "CF 项目响应保留规范网页");
    }
    {
        QTemporaryDir temp;
        const QString root = temp.path();
        const QByteArray bytes("manual-mod");
        ModpackImport::FileIssue issue{{123,456}, QStringLiteral("local.jar"),
            QStringLiteral("mods/local.jar"), QString::fromLatin1(QCryptographicHash::hash(bytes,
            QCryptographicHash::Sha1).toHex()), bytes.size(), QStringLiteral("平台标记不可用")};
        issue.projectWebUrl = QStringLiteral("https://www.curseforge.com/minecraft/mc-mods/example");
        check(ModpackInstallReport::remaining({issue}, root).size() == 1,
              "ZG0 手动文件尚未补齐时保留缺项");
        QDir().mkpath(root + QStringLiteral("/mods"));
        QFile local(root + QStringLiteral("/mods/local.jar"));
        check(local.open(QIODevice::WriteOnly) && local.write(QByteArray(bytes.size(),'x')) == bytes.size(),
              "ZG0 写入同名同大小的错误文件");
        local.close();
        check(ModpackInstallReport::remaining({issue}, root).size() == 1,
              "ZG0 同名且大小相同仍需 SHA-1，不误判补齐");
        check(local.open(QIODevice::WriteOnly) && local.write(bytes) == bytes.size(), "ZG0 写入正确补齐文件");
        local.close();
        check(ModpackInstallReport::remaining({issue}, root).isEmpty(),
              "ZG0 大小与 SHA-1 一致后才消除缺项");
        auto bad = issue;
        bad.sha1.clear();
        check(ModpackInstallReport::remaining({bad}, root).size() == 1,
              "ZG0 没有强校验信息时不能凭文件名确认补齐");
        bad = issue; bad.path = QStringLiteral("../local.jar");
        check(ModpackInstallReport::remaining({bad}, root).size() == 1,
              "ZG0 越界路径不读取、不算补齐");
        ModpackImport::Pack pack; pack.name = QStringLiteral("报告测试包");
        QString error;
        check(ModpackInstallReport::write(root, pack, {issue}, false, &error),
              "ZG0 缺项报告原子写入", error);
        QFile report(root + QStringLiteral("/wtfml.import-report.json"));
        check(report.open(QIODevice::ReadOnly), "ZG0 持久报告可读取");
        const auto json = QJsonDocument::fromJson(report.readAll()).object(); report.close();
        const auto reportIssues = json.value(QStringLiteral("issues")).toArray();
        const auto entry = reportIssues.isEmpty() ? QJsonObject() : reportIssues.first().toObject();
        check(!json.value(QStringLiteral("complete")).toBool() &&
                  entry.value(QStringLiteral("fileId")).toInt() == 456 &&
                  entry.value(QStringLiteral("webUrl")).toString() == QStringLiteral(
                      "https://www.curseforge.com/minecraft/mc-mods/example/files/456") &&
                  entry.value(QStringLiteral("sha1")).toString() == issue.sha1,
              "ZG0 报告保留缺项编号、固定官方网页和校验信息");
        ModpackInstallReport::Report parsed;
        check(ModpackInstallReport::read(root, &parsed, &error) && parsed.issues.size() == 1 &&
                  parsed.issues.first().sha1 == issue.sha1 && !parsed.complete &&
                  parsed.issues.first().projectWebUrl == issue.projectWebUrl,
              "ZG0 报告严格读取并保留强校验信息", error);
        auto legacy = issue; legacy.projectWebUrl.clear();
        check(ModpackInstallReport::write(root, pack, {legacy}, false, &error) &&
                  ModpackInstallReport::read(root, &parsed, &error) && parsed.issues.size() == 1 &&
                  ModpackImport::curseForgeFileWebUrl(parsed.issues.first()).isEmpty(),
              "CF 旧式无规范网址报告仍可补齐，但不猜失效网页", error);
        auto supplied = issue; supplied.path = QStringLiteral("mods/repaired.jar");
        check(ModpackInstallReport::supply(root, supplied, local.fileName(), &error) &&
                  ModpackInstallReport::remaining({supplied}, root).isEmpty(),
              "ZG0 本地文件按校验部署到清单路径而非源文件名", error);
        check(!ModpackInstallReport::supply(root, supplied, local.fileName(), &error),
              "ZG0 不静默覆盖已存在的目标文件");
        check(ModpackInstallReport::supply(root, supplied, root + QLatin1Char('/') + supplied.path, &error),
              "ZG0 选择目标文件本身可安全重新校验", error);
        supplied.path = QStringLiteral("mods/cancelled.jar");
        int calls = 0;
        check(!ModpackInstallReport::supply(root, supplied, local.fileName(), &error,
                  [&calls] { return ++calls >= 3; }) &&
                  !QFileInfo::exists(root + QLatin1Char('/') + supplied.path),
              "ZG0 发布前取消不留下半个文件");
        supplied.sha1 = QString(40, QLatin1Char('0'));
        check(!ModpackInstallReport::supply(root, supplied, local.fileName(), &error) &&
                  !QFileInfo::exists(root + QLatin1Char('/') + supplied.path),
              "ZG0 同大小错误哈希不会发布文件");
        supplied = issue; supplied.path = QStringLiteral("../escape.jar");
        check(!ModpackInstallReport::supply(root, supplied, local.fileName(), &error),
              "ZG0 本地补齐拒绝越界路径");
        auto batchIssue = issue; batchIssue.path = QStringLiteral("mods/batch-renamed.jar");
        const auto batch = ModpackInstallReport::supplyMatching(root, {batchIssue}, {local.fileName()});
        check(batch.suppliedPaths == QStringList{batchIssue.path} && batch.problems.isEmpty() &&
                  ModpackInstallReport::remaining({batchIssue}, root).isEmpty(),
              "ZG0 批量按哈希匹配，不依赖本地文件名");
        batchIssue.path = QStringLiteral("mods/batch-wrong.jar");
        batchIssue.sha1 = QString(40, QLatin1Char('0'));
        const auto mismatch = ModpackInstallReport::supplyMatching(root, {batchIssue}, {local.fileName()});
        check(mismatch.suppliedPaths.isEmpty() && mismatch.problems.size() == 1 &&
                  !QFileInfo::exists(root + QLatin1Char('/') + batchIssue.path),
              "ZG0 批量同大小错误哈希不落盘");
        const auto stopped = ModpackInstallReport::supplyMatching(root, {batchIssue}, {local.fileName()}, [] { return true; });
        check(stopped.cancelled && stopped.suppliedPaths.isEmpty(), "ZG0 批量取消保留未完成状态");
        const auto verified = ModpackInstallReport::verifyExisting(root, {issue});
        check(verified.suppliedPaths == QStringList{issue.path} && verified.problems.isEmpty(),
              "ZG0 只读重验报告全部条目，无需选择源文件");
        auto absent = issue; absent.path = QStringLiteral("mods/absent.jar");
        auto wrong = issue; wrong.sha1 = QString(40, QLatin1Char('0'));
        auto unknown = issue; unknown.sha1.clear();
        const auto mixed = ModpackInstallReport::verifyExisting(root, {issue, absent, wrong, unknown});
        check(mixed.suppliedPaths.size() == 1 && mixed.problems.size() == 3 &&
                  !QFileInfo::exists(root + QLatin1Char('/') + absent.path),
              "ZG0 重验缺失、错误哈希、未知校验逐项报告，不补写缺失文件");
        int verifyCalls = 0;
        const auto interrupted = ModpackInstallReport::verifyExisting(root, {issue},
            [&] { return ++verifyCalls >= 2; });
        check(interrupted.cancelled && interrupted.suppliedPaths.isEmpty(),
              "ZG0 文件块之间取消，不把未完成哈希计为校验成功");
        check(ModpackInstallReport::remaining({issue}, root).isEmpty(), "ZG0 取消重验不改变原文件");
        supplied = issue; supplied.sha1.clear();
        check(!ModpackInstallReport::supply(root, supplied, local.fileName(), &error),
              "ZG0 本地补齐缺少哈希不按名称猜测");
        auto invalid = json;
        invalid[QStringLiteral("schemaVersion")] = 99;
        check(report.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  report.write(QJsonDocument(invalid).toJson()) > 0, "ZG0 写入未知版本报告样本");
        report.close();
        check(!ModpackInstallReport::read(root, &parsed, &error) && parsed.issues.isEmpty(),
              "ZG0 未知版本报告拒绝且不返回旧数据");
        invalid = json;
        auto invalidEntries = reportIssues;
        auto invalidEntry = entry;
        invalidEntry[QStringLiteral("path")] = QStringLiteral("../escape.jar");
        invalidEntries[0] = invalidEntry;
        invalid[QStringLiteral("issues")] = invalidEntries;
        check(report.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  report.write(QJsonDocument(invalid).toJson()) > 0, "ZG0 写入越界报告样本");
        report.close();
        check(!ModpackInstallReport::read(root, &parsed, &error), "ZG0 报告越界路径拒绝读取");
        check(ModpackInstallReport::write(root, pack, {}, true, &error),
              "ZG0 补齐后可更新报告为完成状态", error);
        check(!ModpackInstallReport::write(root + QStringLiteral("/not-created"), pack, {}, false, &error) &&
                  !error.isEmpty(), "ZG0 报告写入失败明确返回错误");
    }
    section("ZG. CurseForge 的整合包：号 → 文件 → 落点（纯函数，全程不联网）");

    // ---------------------------------------------------------------------
    // 这个格式和 Modrinth 那一份最要紧的差别：**清单里一条地址都没有**，只有
    // `projectID` / `fileID` 两个号（真清单长这样：{"projectID":238222,"fileID":8971390,
    // "required":true}）。所以这一条路是两段：解析清单读号（ZG2）、拿号换文件（ZG3）、
    // 拿 modId 问类目决定落哪个目录（ZG4）。中间那一发网络请求在 VersionManager 里
    // （resolveCurseForgeFiles），这一节钉的是它两头的纯函数 —— 而"号写错了、正文错位、
    // 名字不安全"这些错全都发生在这两头，且都不报错（只是静默装错东西）。
    // ---------------------------------------------------------------------

    // --- ZG1. 加载器那一行怎么拆 ---------------------------------------------
    {
        // CurseForge 把名字和版本拼在一个串里：`forge-47.2.0`。拆错了的症状是
        // "装了个加载器、但版本不对"，而游戏起不来时那句话说的是"模组加载器没找到"。
        struct Case {
            const char *raw;
            const char *key;
            const char *version;
            bool known;
        };
        const Case cases[] = {
            {"forge-47.2.0", "forge", "47.2.0", true},
            {"fabric-0.15.11", "fabric-loader", "0.15.11", true},   // 注意：键要翻成
            {"neoforge-20.4.1", "neoforge", "20.4.1", true},        // Modrinth 那套名字
            {"quilt-0.20.0", "quilt-loader", "0.20.0", true},
            {"forge", "forge", "", true},                            // 老包只写名字
            {"neoForge-20.4.1-beta", "neoforge", "20.4.1-beta", true},  // 大小写 + 后缀
            {"liteloader-1.12.2", "liteloader", "1.12.2", false},    // 认不出来
        };
        int bad = 0;
        QString detail;
        for (const Case &c : cases) {
            QString key;
            QString version;
            const bool known = ModpackImport::curseForgeLoaderKey(QString::fromLatin1(c.raw), &key,
                                                                  &version);
            if (known != c.known || key != QString::fromLatin1(c.key) ||
                version != QString::fromLatin1(c.version)) {
                ++bad;
                if (detail.isEmpty()) {
                    detail = QStringLiteral("%1 → %2 / %3 / %4")
                                 .arg(QString::fromLatin1(c.raw), key, version,
                                      known ? QStringLiteral("认得") : QStringLiteral("不认得"));
                }
            }
        }
        check(bad == 0, "ZG1 七种写法都拆对了（名字 / 版本 / 认不认得）", detail);

        // 这一条是**独立的**：认不出来的加载器**也要给出键**。给空串的话，下游
        // unsupportedLoaderText() 会把 `liteloader-1.12.2` 当成"纯原版整合包"放行 ——
        // 装出来一堆 mod 躺在 mods/ 里、游戏按原版起，而且一路上一个问题都不报。
        QString unknownKey;
        QString unknownVersion;
        ModpackImport::curseForgeLoaderKey(QStringLiteral("liteloader-1.12.2"), &unknownKey,
                                          &unknownVersion);
        check(!unknownKey.isEmpty(),
              "ZG1 认不出来的加载器也给得出键（空串在下游 = 纯原版包 = 装出来进不去）",
              unknownKey);

        // 空串进来：什么都不炸，键是空的、版本是空的。
        QString emptyKey = QStringLiteral("没被清掉");
        QString emptyVersion = QStringLiteral("没被清掉");
        check(!ModpackImport::curseForgeLoaderKey(QString(), &emptyKey, &emptyVersion) &&
                  emptyKey.isEmpty() && emptyVersion.isEmpty(),
              "ZG1 空串 → 不认得 + 两个出参都是空的（不清空会让调用方读到上一条的值）");

        // 出参允许传 null（调用方只关心"认不认得"的时候）—— 崩在那一句上就太难查了。
        check(!ModpackImport::curseForgeLoaderKey(QStringLiteral("rift-1.0"), nullptr, nullptr),
              "ZG1 两个出参都给 nullptr 也不崩");
    }

    // --- ZG2. 解析 manifest.json --------------------------------------------
    {
        // 一份**照真包写的**清单：字段名一个字母都不差（projectID / fileID 这两个的
        // 大写 ID 是 CurseForge 自己的写法，写错一个大小写就等于这个包一条文件都没有）。
        const QByteArray manifest = R"json({
            "minecraft": {
                "version": "1.20.1",
                "modLoaders": [{"id": "forge-47.2.0", "primary": true}]
            },
            "manifestType": "minecraftModpack",
            "manifestVersion": 1,
            "name": "Test Pack",
            "version": "1.4.2",
            "author": "someone",
            "files": [
                {"projectID": 238222, "fileID": 8971390, "required": true},
                {"projectID": 306612, "fileID": 1000001, "required": false},
                {"projectID": 306612, "fileID": 1000001, "required": true}
            ],
            "overrides": "overrides"
        })json";

        const ModpackImport::Result r = ModpackImport::parseCurseForgeManifest(manifest);
        check(r.ok, "ZG2 一份合法清单解析通过", r.problem);
        if (r.ok) {
            const ModpackImport::Pack &pack = r.pack;
            check(pack.format == ModpackImport::Format::CurseForge &&
                      pack.gameVersion == QStringLiteral("1.20.1") &&
                      pack.name == QStringLiteral("Test Pack") &&
                      pack.version == QStringLiteral("1.4.2"),
                  "ZG2 格式 / 游戏版本 / 包名 / 包版本都对",
                  QStringLiteral("%1 | %2").arg(pack.gameVersion, pack.name));
            check(pack.loaderKey == QStringLiteral("forge") &&
                      pack.loaderVersion == QStringLiteral("47.2.0"),
                  "ZG2 加载器那一行翻译成了本启动器认的键",
                  pack.loaderKey + QStringLiteral(" ") + pack.loaderVersion);
            // **`files` 一定是空的** —— 地址要等接口那一发。这条断言是这条路的核心约定：
            // 谁要是"顺手"在这里塞一个空 PackFile 进去，数据层那道"没换过就别装"的闸
            // 就永远不响，用户拿到的会是一个只有加载器的空实例。
            check(pack.files.isEmpty() && pack.pendingRefs.size() == 2,
                  "ZG2 号都在 pendingRefs 里、files 是空的（地址要等接口那一发）",
                  QStringLiteral("files %1 / refs %2")
                      .arg(pack.files.size())
                      .arg(pack.pendingRefs.size()));
            check(pack.fileCount() == 2,
                  "ZG2 重复的那个 fileID 只算一条（同一条要下两遍 = 两份作业抢一个路径）",
                  QString::number(pack.fileCount()));
            check(pack.overridesDir == QStringLiteral("overrides") &&
                      pack.clientOverridesDir.isEmpty(),
                  "ZG2 覆盖目录按清单写的那一个；CurseForge 没有 client-overrides 这个规矩",
                  pack.overridesDir + QStringLiteral(" / ") + pack.clientOverridesDir);
            check(pack.pendingRefs.value(0).projectId == 238222 &&
                      pack.pendingRefs.value(0).fileId == 8971390,
                  "ZG2 两个号都读出来了（projectID / fileID 的大小写写错就一条都不剩）");
        }

        const auto problemOf = [](const QByteArray &json) {
            const ModpackImport::Result res = ModpackImport::parseCurseForgeManifest(json);
            return res.ok ? QString() : res.problem;
        };
        check(problemOf(QByteArray("这不是 json")).contains(QStringLiteral("无法解析")),
              "ZG2 不是 json：说「无法解析」");
        check(problemOf(QByteArray("{}")).contains(QStringLiteral("manifestVersion")),
              "ZG2 缺 manifestVersion/files：说它不是整合包清单");
        check(problemOf(R"json({"manifestVersion":1,"files":[],
                                "manifestType":"factorioModpack",
                                "minecraft":{"version":"1.20.1"}})json")
                  .contains(QStringLiteral("Minecraft")),
              "ZG2 manifestType 写着别的游戏：说「这不是 Minecraft 的整合包」");
        // **缺** manifestType 不拒（手改过的老包常常没有这一条，而它不影响能不能装）。
        const ModpackImport::Result noType = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"minecraft":{"version":"1.20.1"},
                    "files":[{"projectID":1,"fileID":2}]})json");
        check(noType.ok, "ZG2 缺 manifestType 照样解析（那两条骨架字段才是判据）", noType.problem);
        check(problemOf(R"json({"manifestVersion":1,"files":[{"projectID":1,"fileID":2}],
                                "minecraft":{"modLoaders":[{"id":"fabric-0.15.11"}]}})json")
                  .contains(QStringLiteral("minecraft.version")),
              "ZG2 没写基于哪个版本：拒绝（连装个纯原版都不知道装哪个）");
        check(problemOf(R"json({"manifestVersion":1,"files":[],"minecraft":{"version":"1.21"}})json")
                  .contains(QStringLiteral("未列出待下载文件")),
              "ZG2 一条文件都没有：拒绝（装出来会是个空实例）");

        // 加载器那一条：`primary` 在第二条时不许挑第一条（真包里有写好几条的）。
        const ModpackImport::Result multi = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"files":[{"projectID":1,"fileID":2}],
                    "minecraft":{"version":"1.20.1",
                    "modLoaders":[{"id":"fabric-0.15.11"},{"id":"forge-47.2.0","primary":true}]}})json");
        check(multi.ok && multi.pack.loaderKey == QStringLiteral("forge"),
              "ZG2 modLoaders 写了好几条时挑 primary 那一条（不是第一条）",
              multi.ok ? multi.pack.loaderKey : multi.problem);
        // 一条都没有 = 纯原版整合包，不是错误。
        const ModpackImport::Result vanilla = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"files":[{"projectID":1,"fileID":2}],
                    "minecraft":{"version":"1.20.1","modLoaders":[]}})json");
        check(vanilla.ok && vanilla.pack.loaderKey.isEmpty(),
              "ZG2 一条加载器都没有：纯原版整合包，放行", vanilla.problem);

        // 覆盖目录名是**外部输入**（它会被拼成一条路径前缀）。不安全时退回默认名，
        // 而不是把整包拒掉 —— 少解几个 config 是小事，写到实例目录外面是大事。
        const ModpackImport::Result evil = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"files":[{"projectID":1,"fileID":2}],
                    "minecraft":{"version":"1.20.1"},"overrides":"../evil"})json");
        check(evil.ok && evil.pack.overridesDir == QStringLiteral("overrides"),
              "ZG2 overrides 写着 ../evil：退回默认那一个，包照样能装",
              evil.ok ? evil.pack.overridesDir : evil.problem);
        // 真包里有自己起名的（`overrides-1.20.1` 那种），安全的要原样用。
        const ModpackImport::Result named = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"files":[{"projectID":1,"fileID":2}],
                    "minecraft":{"version":"1.20.1"},"overrides":"overrides-1.20.1"})json");
        check(named.ok && named.pack.overridesDir == QStringLiteral("overrides-1.20.1"),
              "ZG2 清单自己起的覆盖目录名（overrides-1.20.1）原样用",
              named.ok ? named.pack.overridesDir : named.problem);

        // FTB 那种包：清单字段和 CurseForge 是同一套（`manifestVersion` / `files[]` 的
        // projectID+fileID / `overrides` / `minecraft.modLoaders`），没有独立适配器也走得通。
        // ★ 这一条钉的是"别为了 FTB 另写第二个解析器"：同一套骨架，FTB 的包本来就由
        // CurseForge 托管那些号。它不写 `manifestType` —— 缺这一条本来就不拒。
        const ModpackImport::Result ftb = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"version":"1.20.0","name":"FTB Example",
                    "author":"FTB","overrides":"overrides",
                    "files":[{"projectID":238222,"fileID":5000001,"required":true},
                             {"projectID":306612,"fileID":5000002,"required":false}],
                    "minecraft":{"version":"1.20.1",
                                 "modLoaders":[{"id":"forge-47.2.3","primary":true}]}})json");
        check(ftb.ok && ftb.pack.format == ModpackImport::Format::CurseForge &&
                  ftb.pack.name == QStringLiteral("FTB Example") &&
                  ftb.pack.gameVersion == QStringLiteral("1.20.1") &&
                  ftb.pack.loaderKey == QStringLiteral("forge") &&
                  ftb.pack.loaderVersion == QStringLiteral("47.2.3") &&
                  ftb.pack.pendingRefs.size() == 2,
              "ZG2 FTB 那种清单（同一套骨架、不写 manifestType）照样解析",
              ftb.ok ? ftb.pack.name : ftb.problem);

        // 号是 0 / 负数 / 字符串的条目一律丢（丢一条只是少一个文件，`toInt()` 丢进来的
        // 0 会让接口那一发带着一个无效号去问，而那一发失败会**整包停下**）。
        const ModpackImport::Result junk = ModpackImport::parseCurseForgeManifest(
            R"json({"manifestVersion":1,"minecraft":{"version":"1.20.1"},
                    "files":[{"projectID":0,"fileID":5},{"projectID":3,"fileID":-1},
                             {"projectID":"4","fileID":"6"},{"projectID":7,"fileID":8},
                             "不是对象"]})json");
        check(junk.ok && junk.pack.pendingRefs.size() == 1 &&
                  junk.pack.pendingRefs.value(0).projectId == 7,
              "ZG2 号是 0 / 负数 / 字符串的条目一律丢掉（不是当成 0 号去问）",
              junk.ok ? QString::number(junk.pack.pendingRefs.size()) : junk.problem);
    }

    // --- ZG3. 请求体 + 接口答复 ----------------------------------------------
    {
        // 请求体的形状写错（键名、号是数字还是字符串）时接口回 400，而 400 的正文里只有
        // 一句 {"error":"…"} —— 看着和"这个包坏了"一模一样。所以键名和类型在这儿钉住。
        const QList<ModpackImport::PackRef> refs = {{238222, 8971390}, {306612, 1000001}};
        const QJsonObject filesBody =
            QJsonDocument::fromJson(ModpackImport::curseForgeFilesRequestBody(refs)).object();
        const QJsonArray fileIds = filesBody.value(QStringLiteral("fileIds")).toArray();
        check(filesBody.size() == 1 && fileIds.size() == 2 && fileIds.at(0).isDouble() &&
                  fileIds.at(0).toInt() == 8971390 && fileIds.at(1).toInt() == 1000001,
              "ZG3 问文件那一发的正文是 {\"fileIds\":[数字…]}（键名和类型都要对）",
              QString::fromUtf8(QJsonDocument(filesBody).toJson(QJsonDocument::Compact)));

        const QJsonObject modsBody = QJsonDocument::fromJson(
            ModpackImport::curseForgeModsRequestBody({238222, 306612})).object();
        const QJsonArray modIds = modsBody.value(QStringLiteral("modIds")).toArray();
        check(modsBody.size() == 1 && modIds.size() == 2 && modIds.at(0).isDouble(),
              "ZG3 问类目那一发的正文是 {\"modIds\":[数字…]}",
              QString::fromUtf8(QJsonDocument(modsBody).toJson(QJsonDocument::Compact)));

        // 顺序和问的相反；空链接仍有 CDN 候选，明确不可用与查不到分别统计。
        const QByteArray reply = R"json({"data":[
            {"id":1000001,"modId":306612,"fileName":"b.jar","fileLength":2048,
             "downloadUrl":null,"isAvailable":false,
             "hashes":[{"value":"bbbb","algo":1},{"value":"cccc","algo":2}]},
            {"id":8971390,"modId":238222,"fileName":"a.jar","fileLength":1024,
             "downloadUrl":"https://edge.forgecdn.net/files/8971/390/a.jar",
             "hashes":[{"value":"dddd","algo":2},{"value":"aaaa","algo":1}]}
        ]})json";
        const QList<ModpackImport::PackRef> asked = {{238222, 8971390},
                                                     {306612, 1000001},
                                                     {999, 555}};  // 最后一个查不到
        const ModpackImport::CurseForgeFiles parsed =
            ModpackImport::parseCurseForgeFileInfo(reply, asked);
        check(parsed.problem.isEmpty() && parsed.resolved.size() == 1 &&
                  parsed.notDistributable == 1 && parsed.missing == 1,
              "ZG3 三条问出去：换到 1 条、1 条平台标记不可用、1 条查不到（三类分得开）",
              parsed.problem.isEmpty()
                  ? QStringLiteral("换到 %1 / 不可用 %2 / 查不到 %3")
                        .arg(parsed.resolved.size())
                        .arg(parsed.notDistributable)
                        .arg(parsed.missing)
                  : parsed.problem);
        check(parsed.issues.size() == 2 && parsed.issues.first().ref.fileId == 1000001 &&
                  parsed.issues.last().ref.fileId == 555,
              "ZG3 不可用与接口未返回的条目保留原清单编号，不只统计数量");
        if (parsed.resolved.size() == 1) {
            const ModpackImport::CurseForgeFile &file = parsed.resolved.first();
            // **这一条是这一节里最值钱的**：正文的次序和问的次序相反，按位置取会把
            // a.jar 的地址安到 b.jar 上（症状是"装完有几个 mod 是错的"，比少装可怕）。
            check(file.file.path == QStringLiteral("a.jar") && file.modId == 238222 &&
                      file.file.size == 1024 &&
                      file.file.urls.value(0).contains(QStringLiteral("a.jar")),
                  "ZG3 答复的次序被打乱也认得出哪条是哪条（按 fileId 归档，不按位置）",
                  file.file.path + QStringLiteral(" / ") + file.file.urls.join(QStringLiteral(",")));
            // `hashes` 里 algo==1 那条才是 sha1；拿 hashes[0] 的话这一条拿到的是 md5
            // （下载完每一条都会被判成"校验不过"，而字节数是对的 —— 极难查）。
            check(file.file.sha1 == QStringLiteral("aaaa"),
                  "ZG3 sha1 取的是 algo==1 那一条，不是数组的第 0 条", file.file.sha1);
        }
        check(parsed.projectIds.size() == 1 && parsed.projectIds.value(0) == 238222,
              "ZG3 只有换到了的才需要去问类目（不可用 / 查不到的那两个不用问）",
              QStringLiteral("%1 个").arg(parsed.projectIds.size()));

        const auto fallback = ModpackImport::parseCurseForgeFileInfo(
            R"json({"data":[{"id":1000001,"modId":306612,"fileName":"a #%中文.jar",
            "downloadUrl":null,"fileLength":12,"hashes":[{"algo":1,"value":"aaaa"}]}]})json",
            {{306612, 1000001}});
        check(fallback.resolved.size() == 1 && fallback.notDistributable == 0 &&
                  fallback.missing == 0 && fallback.resolved.first().file.sha1 == QStringLiteral("aaaa") &&
                  fallback.resolved.first().file.urls.first() == QStringLiteral(
                      "https://edge.forgecdn.net/files/1000/1/a%20%23%25%E4%B8%AD%E6%96%87.jar"),
              "ZG3 空链接生成官方 CDN 候选并保留校验信息，特殊字符仅作为文件名编码");
        check(ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("x.jar"), {}) ==
                  QStringLiteral("https://edge.forgecdn.net/files/0/123/x.jar") &&
                  ModpackImport::curseForgeDownloadUrl(123456789, QStringLiteral("x.jar"), {}) ==
                  QStringLiteral("https://edge.forgecdn.net/files/123456/789/x.jar"),
              "ZG3 CDN 文件 ID 不依赖固定位数");
        check(ModpackImport::curseForgeDownloadUrl(0, QStringLiteral("x.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(-1, QStringLiteral("x.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(9007199254740992LL, QStringLiteral("x.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("../x.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("sub/x.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("x?.jar"), {}).isEmpty() &&
                  ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("sub\\x.jar"), {}).isEmpty(),
              "ZG3 不为非法 ID 或带目录的文件名构造 CDN 候选");
        check(ModpackImport::curseForgeDownloadUrl(123, QStringLiteral("x.jar"),
                  QStringLiteral("https://example.invalid/original.jar")) ==
                  QStringLiteral("https://example.invalid/original.jar"),
              "ZG3 接口已有地址保持不变");

        // 文件名是**外部输入**，它要拼进路径。往外跳的那两条当场算"查不到"，而
        // **整包不拒绝**（和 Modrinth 那份清单不同：那边路径是作者写在清单里的）。
        const QByteArray evilNames = R"json({"data":[
            {"id":1,"modId":10,"fileName":"../evil.jar","fileLength":1,
             "downloadUrl":"https://example.invalid/1","hashes":[]},
            {"id":2,"modId":11,"fileName":"a/../../b.jar","fileLength":1,
             "downloadUrl":"https://example.invalid/2","hashes":[]},
            {"id":3,"modId":12,"fileName":"ok.jar","fileLength":1,
             "downloadUrl":"https://example.invalid/3","hashes":[]},
            {"id":4,"modId":13,"fileName":"sub/dir.jar","fileLength":1,
             "downloadUrl":"https://example.invalid/4","hashes":[]}
        ]})json";
        const ModpackImport::CurseForgeFiles sec = ModpackImport::parseCurseForgeFileInfo(
            evilNames, {{10, 1}, {11, 2}, {12, 3}, {13, 4}});
        QStringList secPaths;
        for (const ModpackImport::CurseForgeFile &entry : sec.resolved) {
            secPaths << entry.file.path;
        }
        check(sec.problem.isEmpty() && sec.missing == 2 && sec.resolved.size() == 2 &&
                  secPaths.contains(QStringLiteral("ok.jar")) &&
                  secPaths.contains(QStringLiteral("sub/dir.jar")),
              "ZG3 往外跳的两条（../evil.jar、a/../../b.jar）算查不到，另两条照收",
              QStringLiteral("坏 %1 / 好 %2「%3」")
                  .arg(sec.missing)
                  .arg(sec.resolved.size())
                  .arg(secPaths.join(QStringLiteral(", "))));
        // 名字里带一段目录**是放行的**（判据只问"能不能安全地拼进实例目录"，不问
        // "它长不长成一个文件名"）：它会落在 `mods/sub/dir.jar`。这一条是**反过来**钉
        // 上面那条 —— 免得哪天把"名字里有斜杠"一律当成攻击，拒掉正常的内容。
        check(secPaths.contains(QStringLiteral("sub/dir.jar")),
              "ZG3 名字里带一段目录（sub/dir.jar）放行，不当成攻击", secPaths.join(QStringLiteral(", ")));

        // 字节数越界（或者干脆不是数）= 不知道字节数（-1）。越界的 double 转 qint64 是
        // 未定义行为，而那个负数会变成一个永远下不完的作业。
        const QByteArray weirdSize = R"json({"data":[
            {"id":1,"modId":10,"fileName":"a.jar","fileLength":1e30,
             "downloadUrl":"https://example.invalid/1","hashes":[]},
            {"id":2,"modId":11,"fileName":"b.jar",
             "downloadUrl":"https://example.invalid/2","hashes":[]}
        ]})json";
        const ModpackImport::CurseForgeFiles sizes =
            ModpackImport::parseCurseForgeFileInfo(weirdSize, {{10, 1}, {11, 2}});
        check(sizes.resolved.size() == 2 && sizes.resolved.value(0).file.size == -1 &&
                  sizes.resolved.value(1).file.size == -1,
              "ZG3 字节数越界 / 缺这一项 → 当「不知道」（-1），不是强转成一个负数",
              QStringLiteral("%1 / %2")
                  .arg(sizes.resolved.value(0).file.size)
                  .arg(sizes.resolved.value(1).file.size));

        // 接口出错时正文里只有一句 error（没有 data）。那句话**要原样带给用户** ——
        // 我们猜不出来它为什么拒。
        // NOTE: 这一段的正文**不能写成 R"json(…)json" 再交给 QByteArrayLiteral()** ——
        // 那是宏，一进预处理器的参数扫描就把带自定义定界符的原始字符串看错了
        // （报的是"unterminated raw string"，指着一行完全正常的写法）。转义写法。
        const QByteArray deniedBody = QByteArrayLiteral("{\"error\":\"Invalid API key\"}");
        const ModpackImport::CurseForgeFiles denied =
            ModpackImport::parseCurseForgeFileInfo(deniedBody, asked);
        check(denied.problem.contains(QStringLiteral("Invalid API key")) &&
                  denied.resolved.isEmpty(),
              "ZG3 接口回的是 {\"error\":…}：把接口那句话原样带出去", denied.problem);
        check(!ModpackImport::parseCurseForgeFileInfo(QByteArray("不是 json"), asked)
                   .problem.isEmpty(),
              "ZG3 答复读不懂：problem 非空（这一档会让整件事停下来）");
    }

    // --- ZG4. 落点：哪一类文件进哪个目录 -------------------------------------
    {
        // 判据是**类目号**，不是文件名的后缀（.zip 既可能是资源包也可能是存档，而
        // fileName 是接口给的、可以随便起）。认不出来的一律落 mods/。
        check(ModpackImport::curseForgeFilePath(6) == QStringLiteral("mods/") &&
                  ModpackImport::curseForgeFilePath(12) == QStringLiteral("resourcepacks/") &&
                  ModpackImport::curseForgeFilePath(6552) == QStringLiteral("shaderpacks/") &&
                  ModpackImport::curseForgeFilePath(17) == QStringLiteral("saves/") &&
                  ModpackImport::curseForgeFilePath(0) == QStringLiteral("mods/") &&
                  ModpackImport::curseForgeFilePath(12345) == QStringLiteral("mods/"),
              "ZG4 六种类目号各自落在对的目录，认不出来的一律 mods/");
        check(ModpackImport::curseForgeFilePath(4471).isEmpty(),
              "ZG4 嵌套的整合包（4471）回空串 = 这一条不下",
              ModpackImport::curseForgeFilePath(4471));

        const auto makeFile = [](const QString &name, int modId) {
            ModpackImport::CurseForgeFile file;
            file.file.path = name;
            file.file.urls.append(QStringLiteral("https://example.invalid/") + name);
            file.modId = modId;
            return file;
        };
        const QList<ModpackImport::CurseForgeFile> resolved = {
            makeFile(QStringLiteral("a.jar"), 100),          // 模组
            makeFile(QStringLiteral("pack.zip"), 101),       // 资源包
            makeFile(QStringLiteral("shader.zip"), 102),     // 光影
            makeFile(QStringLiteral("world.zip"), 103),      // 存档
            makeFile(QStringLiteral("nested.zip"), 104),     // 嵌套整合包
            makeFile(QStringLiteral("unknown.jar"), 105),    // 类目号问不到
        };
        QHash<int, int> classIds;
        classIds.insert(100, 6);
        classIds.insert(101, 12);
        classIds.insert(102, 6552);
        classIds.insert(103, 17);
        classIds.insert(104, 4471);
        // 105 故意不放进表里 —— 第二发失败时就是这样。

        const ModpackImport::CurseForgePlacement placed =
            ModpackImport::placeCurseForgeFiles(resolved, classIds);
        check(placed.files.size() == 5 && placed.skipped == 1 && placed.missing == 0,
              "ZG4 六个文件：五个落好、嵌套整合包记一笔跳过（不拦整件事）",
              QStringLiteral("落 %1 / 跳 %2 / 丢 %3")
                  .arg(placed.files.size())
                  .arg(placed.skipped)
                  .arg(placed.missing));
        QStringList landed;
        for (const ModpackImport::PackFile &file : placed.files) {
            landed << file.path;
        }
        check(landed.contains(QStringLiteral("mods/a.jar")) &&
                  landed.contains(QStringLiteral("resourcepacks/pack.zip")) &&
                  landed.contains(QStringLiteral("shaderpacks/shader.zip")) &&
                  landed.contains(QStringLiteral("saves/world.zip")) &&
                  landed.contains(QStringLiteral("mods/unknown.jar")),
              "ZG4 路径逐条都对（包括问不到类目的那条落到 mods/）",
              landed.join(QStringLiteral(" | ")));

        // 文件名长到加上目录之后过不了判据：这一条记 missing（不是硬塞进去）。
        // **这个数是量出来的、不是随手写的**：240 是整条路径的上限（isSafeRelativePath），
        // 名字本身正好 240（判得过），加上 `mods/` 那 5 个字符就是 245（判不过）——
        // 差 5 个字符，而它决定的是"超长文件名是少一个 mod"还是"下到一半崩在写盘上"。
        const QString huge = QString(236, QLatin1Char('x')) + QStringLiteral(".jar");
        check(ModpackImport::isSafeRelativePath(huge),
              "ZG4 那条超长名字**本身**是合法的（不然这一条测的就不是「拼完再判」）",
              QString::number(huge.length()));
        const ModpackImport::CurseForgePlacement longPaths =
            ModpackImport::placeCurseForgeFiles({makeFile(huge, 100)}, classIds);
        check(longPaths.files.isEmpty() && longPaths.missing == 1,
              "ZG4 拼上目录之后超长的路径记 missing（不是硬塞进下载队列）",
              QStringLiteral("落 %1 / 丢 %2").arg(longPaths.files.size()).arg(longPaths.missing));

        // 两个项目给出同名文件：只留一条（两条同时写一个路径是数据竞争）。
        const ModpackImport::CurseForgePlacement dupe = ModpackImport::placeCurseForgeFiles(
            {makeFile(QStringLiteral("same.jar"), 100), makeFile(QStringLiteral("same.jar"), 100)},
            classIds);
        check(dupe.files.size() == 1, "ZG4 同名文件只留下一条",
              QStringLiteral("留下 %1 条").arg(dupe.files.size()));

        // 空表进来 = 空表出去（这一发一条都没换到时就是这样，调用方按"多少个都不下"处理）。
        const ModpackImport::CurseForgePlacement empty =
            ModpackImport::placeCurseForgeFiles({}, classIds);
        check(empty.files.isEmpty() && empty.skipped == 0 && empty.missing == 0,
              "ZG4 空表进空表出");
    }

    // --- ZG5. 问哪一台接口 ---------------------------------------------------
    {
        // 官方那份要 `x-api-key`（不带 = 403， 实测），所以默认走一台不要 key 的
        // **社区镜像**。这一条链的取舍写在 curseForgeApiBase() 上面：key 编进 exe 就等于公开。
        const QString noKeyBase = ModpackImport::curseForgeApiBase(QString());
        check(!noKeyBase.isEmpty() && noKeyBase.startsWith(QStringLiteral("https://")) &&
                  !noKeyBase.contains(QStringLiteral("api.curseforge.com")),
              "ZG5 没填 key：走一台不要 key 的社区镜像（不是官方那一台）", noKeyBase);
        // 尾巴上那段 `/v1` **一个字符都不能少**：这个函数的约定是"返回到 `/v1` 为止"，
        // 调用点只往后接 `/mods/search` / `/mods/files` / `/mods/{id}/files`。少了它每一发
        // 都是 404，而**症状和"服务挂了"一模一样**（那一栏什么都搜不出来 / 导入说换不到文件）。
        // 就是这么踩的：换镜像那一笔把 `/v1` 漏在地址外面了，而这一条链在两个
        // 验证工程里**全是拿假接口验的**（假接口的 base 本身就带 `/v1`）⇒ 四个验证工程
        // 全绿、真网络每一发 404，只有 `cf-resolve-live` 那一跑看得出来。
        // 所以这条断言钉的不是"哪台机器"，是**那台机器后面必须接上 `/v1` 这一段**。
        check(noKeyBase.endsWith(QStringLiteral("/v1")),
              "ZG5 默认那台也带 `/v1` 那一段（少它每一发都是 404，而症状看着像服务挂了）",
              noKeyBase);
        check(ModpackImport::curseForgeApiBase(QStringLiteral("abc")) ==
                  QStringLiteral("https://api.curseforge.com/v1"),
              "ZG5 填了 key：改走官方",
              ModpackImport::curseForgeApiBase(QStringLiteral("abc")));
        check(ModpackImport::curseForgeApiBase(QStringLiteral("abc"),
                                               QStringLiteral("http://127.0.0.1:8123/v1")) ==
                  QStringLiteral("http://127.0.0.1:8123/v1"),
              "ZG5 覆盖地址比 key 还优先（验证工程拿它指到本机）");
        // 结尾那个 `/` 去掉：拼地址的地方一律自己带它，用户多打一个不该 404。
        check(ModpackImport::curseForgeApiBase(QString(), QStringLiteral("http://127.0.0.1:8123/v1/")) ==
                  QStringLiteral("http://127.0.0.1:8123/v1"),
              "ZG5 覆盖地址结尾多一个斜杠也认（不然拼出来是两个斜杠）",
              ModpackImport::curseForgeApiBase(
                  QString(), QStringLiteral("http://127.0.0.1:8123/v1/")));
        // 只有空白的 key 等于没填（从网页上复制很容易带一个尾随空格）。
        check(ModpackImport::curseForgeApiBase(QStringLiteral("   ")) == noKeyBase,
              "ZG5 只有空白的 key 当没填（带空格的 key 会一路变成 403）");
    }

    // --- ZG6. 「这个格式还没支持」那句话里不许再有 CurseForge -----------------
    {
        // 这一条钉的是"删干净了"：那个分支留着的话，它的症状是**明明支持却说不行**
        // （两条路都已经走不到了，所以只有靠这条断言防它回来）。
        const QString text = ModpackImport::unsupportedFormatText(ModpackImport::Format::CurseForge);
        check(!text.contains(QStringLiteral("还没")),
              "ZG6 认不出格式那句话里不再说 CurseForge「还没做那一步」", text.left(40));
        // 两句必须一模一样 = 这个函数已经不看格式了（还是两支的话，上面那条会漏掉另一支）。
        check(text == ModpackImport::unsupportedFormatText(ModpackImport::Format::Unknown),
              "ZG6 两种格式给出同一句话（那个函数已经不看参数了）");
        // 那句话本身得有用：两个清单文件名叫什么都得写着。
        check(text.contains(ModpackImport::modrinthIndexName()) &&
                  text.contains(ModpackImport::curseForgeManifestName()),
              "ZG6 那句话把两个清单文件名都写出来（用户才知道自己手上的是不是整合包）", text);
    }

    // --- ZG7. 数据层端到端：号真的发到那台机器上，换回来的东西真的落回 pack ------------
    //
    // 这一节要防的错，全都是"纯函数全绿、但整件事不成"的那种：地址少一个 `/` 变成 404、
    // 正文的键名写成 `fileIDs`、带了 key 却不发 `x-api-key`（官方 403）、
    // 失败时把 pendingRefs 清了（用户再点一次就永远换不回来）。
    {
        // 一份**照真清单写的**包：两条文件，一个模组一个资源包（好验落点分得开）。
        const QByteArray manifestJson = R"json({
            "manifestVersion": 1,
            "manifestType": "minecraftModpack",
            "name": "端到端假包",
            "version": "1.0",
            "minecraft": {"version": "1.20.1",
                          "modLoaders": [{"id": "forge-47.2.0", "primary": true}]},
            "files": [
                {"projectID": 238222, "fileID": 8971390, "required": true},
                {"projectID": 101, "fileID": 55, "required": true}
            ]
        })json";

        const auto freshPack = [&manifestJson](ModpackImport::Pack *out) {
            const ModpackImport::Result parsed =
                ModpackImport::parseCurseForgeManifest(manifestJson);
            if (!parsed.ok) {
                return false;
            }
            *out = parsed.pack;
            return true;
        };
        const auto jsonOf = [](const QByteArray &bytes) {
            return QJsonDocument::fromJson(bytes).object();
        };

        FakeCurseForgeApi api;
        api.filesBody = R"json({"data":[
            {"id":8971390,"modId":238222,"fileName":"a.jar","fileLength":1024,
             "downloadUrl":"https://edge.forgecdn.net/files/8971/390/a.jar",
             "hashes":[{"value":"aa11","algo":1}]},
            {"id":55,"modId":101,"fileName":"pack.zip","fileLength":2048,
             "downloadUrl":"https://edge.forgecdn.net/files/0/55/pack.zip",
             "hashes":[{"value":"bb22","algo":1}]}
        ]})json";
        api.modsBody = R"json({"data":[{"id":238222,"classId":6},{"id":101,"classId":12}]})json";
        {
            FakeCurseForgeApi unavailable;
            unavailable.filesBody = R"({"data":[{"id":55,"modId":101,"fileName":"pack.zip","isAvailable":false}]})";
            unavailable.modsBody = R"({"data":[{"id":101,"classId":12,"links":{"websiteUrl":"https://www.curseforge.com/minecraft/texture-packs/example"}}]})";
            check(unavailable.start(), "CF 缺项规范网页接口启动");
            if (unavailable.server.isListening()) {
                VersionManager manager;
                manager.setCurseForgeApiBaseOverride(unavailable.baseUrl());
                ModpackImport::Pack pack;
                pack.format = ModpackImport::Format::CurseForge;
                pack.pendingRefs = {{101,55}};
                const auto problem = manager.resolveCurseForgeFiles(&pack);
                check(problem.isEmpty() && pack.fileIssues.size() == 1 &&
                      ModpackImport::curseForgeFileWebUrl(pack.fileIssues.first()) ==
                          QStringLiteral("https://www.curseforge.com/minecraft/texture-packs/example/files/55"),
                      "CF 真实解析接线将平台项目网页传到不可用文件缺项", problem);
            }
        }
        check(api.start(), "ZG7 本机假接口起来了（起不来这一节就没意义，直接算失败）");
        if (api.server.isListening()) {
            // ---- ZG7a 顺利的那一条：两发都发出去、两发都答回来、落地 ----
            {
                VersionManager manager;
                manager.setCurseForgeApiBaseOverride(api.baseUrl());
                ModpackImport::Pack pack;
                check(freshPack(&pack), "ZG7 前提：那份清单解析得出来");
                check(pack.pendingRefs.size() == 2 && pack.files.isEmpty(),
                      "ZG7 前提：解析完就是「两个号、零条地址」");

                const QString problem = manager.resolveCurseForgeFiles(&pack);
                check(problem.isEmpty(), "ZG7a 换地址这一发没报错", problem);
                check(api.filesHits == 1 && api.modsHits == 1,
                      "ZG7a 两发都发出去了（号→文件 一发，类目 一发）",
                      QStringLiteral("files %1 / mods %2").arg(api.filesHits).arg(api.modsHits));
                // 两发是并着跑的。判据按**请求目标**查（不按下标）：谁先回来由网络定，
                // 拿 `paths.value(0)` 当"文件那一发"在这一台上碰巧成立，换台机器就可能反。
                check(api.paths.contains(QStringLiteral("/v1/mods/files")) &&
                          api.paths.contains(QStringLiteral("/v1/mods")),
                      "ZG7a 两条路径拼对了（少一个斜杠就是 404，那看着像「这个包坏了」）",
                      api.paths.join(QStringLiteral(", ")));
                const QByteArray filesBody = api.bodyForPath(QStringLiteral("/v1/mods/files"));
                const QByteArray modsBody = api.bodyForPath(QStringLiteral("/v1/mods"));
                const QJsonArray fileIds =
                    jsonOf(filesBody).value(QStringLiteral("fileIds")).toArray();
                const QJsonArray modIds =
                    jsonOf(modsBody).value(QStringLiteral("modIds")).toArray();
                check(fileIds.size() == 2 && fileIds.at(0).toInt() == 8971390 &&
                          fileIds.at(1).toInt() == 55,
                      "ZG7a 问的就是清单里那两个 fileID（按清单的次序）",
                      QString::fromUtf8(filesBody));
                check(modIds.size() == 2 && modIds.at(0).toInt() == 238222,
                      "ZG7a 类目那一发问的是那两个 projectID", QString::fromUtf8(modsBody));
                check(api.apiKeyForPath(QStringLiteral("/v1/mods/files")).isEmpty() &&
                          api.apiKeyForPath(QStringLiteral("/v1/mods")).isEmpty(),
                      "ZG7a 没填 key 时**不发** x-api-key 那个头（社区反代不看它）",
                      api.apiKeyForPath(QStringLiteral("/v1/mods/files")));

                check(pack.files.size() == 2 && pack.pendingRefs.isEmpty(),
                      "ZG7a 换回来的两条落到 files 里、号清空了",
                      QStringLiteral("files %1 / refs %2")
                          .arg(pack.files.size())
                          .arg(pack.pendingRefs.size()));
                QStringList landedPaths;
                for (const ModpackImport::PackFile &file : pack.files) {
                    landedPaths << file.path;
                }
                // 落点是按**类目号**分的（6 是模组、12 是资源包），不是按扩展名。
                check(landedPaths.contains(QStringLiteral("mods/a.jar")) &&
                          landedPaths.contains(QStringLiteral("resourcepacks/pack.zip")),
                      "ZG7a 一个落 mods/、一个落 resourcepacks/（按接口说的类目分）",
                      landedPaths.join(QStringLiteral(", ")));
                if (pack.files.size() == 2) {
                    const ModpackImport::PackFile &first = pack.files.value(0);
                    check(first.urls.value(0) ==
                              QStringLiteral("https://edge.forgecdn.net/files/8971/390/a.jar") &&
                              first.sha1 == QStringLiteral("aa11") && first.size == 1024,
                          "ZG7a 地址 / sha1 / 字节数三样都进了下载那一层（校验照旧一个字不用改）",
                          QStringLiteral("%1 | %2 | %3")
                              .arg(first.urls.value(0), first.sha1)
                              .arg(first.size));
                }
                check(pack.filesNotDistributable == 0 && pack.filesMissing == 0 &&
                          pack.filesSkipped == 0,
                      "ZG7a 一个「没换到」都没有（三种计数分开走，这里该是三个 0）");

                // 再调一次：**一个请求都不发**。上面界面那一半会对着同一个包再问一次，
                // 靠的就是这一条早退（判据是 pendingRefs 空不空）——
                // 没有它，每点一次确认框就多一发请求（还是对着别人那份反代打的）。
                const QString again = manager.resolveCurseForgeFiles(&pack);
                check(again.isEmpty() && api.filesHits == 1 && api.modsHits == 1,
                      "ZG7a 换过一次的包再调一次：一个请求都不发（不然确认框点几次打几发）",
                      QStringLiteral("files %1 / mods %2").arg(api.filesHits).arg(api.modsHits));
            }

            // ---- ZG7b 填了 key：那一发必须带 x-api-key（不带 = 官方 403）----
            {
                FakeCurseForgeApi keyed;
                keyed.filesBody = api.filesBody;
                keyed.modsBody = api.modsBody;
                check(keyed.start(), "ZG7b 第二个假接口起来了");
                if (keyed.server.isListening()) {
                    VersionManager manager;
                    manager.setCurseForgeApiKey(QStringLiteral("只有这一份的 key"));
                    manager.setCurseForgeApiBaseOverride(keyed.baseUrl());
                    ModpackImport::Pack pack;
                    freshPack(&pack);
                    const QString problem = manager.resolveCurseForgeFiles(&pack);
                    check(problem.isEmpty() && keyed.filesHits == 1,
                          "ZG7b 带了 key 的那一发照旧问得成", problem);
                    check(keyed.apiKeyForPath(QStringLiteral("/v1/mods/files")) ==
                              QStringLiteral("只有这一份的 key"),
                          "ZG7b 发出去的 x-api-key 就是用户填的那一份（一个字都不能变，"
                          "多一个空格就是 403）",
                          keyed.apiKeyForPath(QStringLiteral("/v1/mods/files")));
                }
            }

            // ---- ZG7c 早一发失败：整件事停下，一个字节都不装 ----
            {
                FakeCurseForgeApi failing;
                failing.filesStatus = 500;
                failing.filesBody =
                    QByteArrayLiteral("{\"error\":\"Invalid API key\"}");
                check(failing.start(), "ZG7c 会失败的假接口起来了");
                if (failing.server.isListening()) {
                    VersionManager manager;
                    manager.setCurseForgeApiBaseOverride(failing.baseUrl());
                    ModpackImport::Pack pack;
                    freshPack(&pack);
                    const QString problem = manager.resolveCurseForgeFiles(&pack);
                    check(!problem.isEmpty(), "ZG7c 早一发失败 → 有话说（不是静默地装个空实例）");
                    // 接口自己那句话要**原样带给用户**：我们猜不出来它为什么拒。
                    check(problem.contains(QStringLiteral("HTTP 500")) &&
                              problem.contains(QStringLiteral("Invalid API key")),
                          "ZG7c 那句话里带着 HTTP 码和接口的原话", problem);
                    // 反代是**别人的服务** ⇒ 必须给出一条用户自己走得了的路。
                    check(problem.contains(QStringLiteral("CurseForge API 密钥")),
                          "ZG7c 那句话里指出了「可以填自己的 key」这条出路", problem);
                    check(pack.files.isEmpty() && pack.pendingRefs.size() == 2,
                          "ZG7c 换不成时 files 还是空的、号**一个都没清**（用户再点一次还能再换）",
                          QStringLiteral("files %1 / refs %2")
                              .arg(pack.files.size())
                              .arg(pack.pendingRefs.size()));
                    check(failing.modsHits == 0,
                          "ZG7c 早一发就停了，**没有**接着问类目（省一发，也免得把话说岔）");
                }
            }

            // ---- ZG7d 429：走"限流"那句话，不把官方那个错误码端给用户 ----
            {
                FakeCurseForgeApi throttled;
                throttled.filesStatus = HttpClient::kStatusTooManyRequests;
                check(throttled.start(), "ZG7d 会回 429 的假接口起来了");
                if (throttled.server.isListening()) {
                    VersionManager manager;
                    manager.setCurseForgeApiBaseOverride(throttled.baseUrl());
                    ModpackImport::Pack pack;
                    freshPack(&pack);
                    const QString problem = manager.resolveCurseForgeFiles(&pack);
                    check(problem.contains(QStringLiteral("HTTP 429")) &&
                              problem.contains(QStringLiteral("请求频率过高")),
                          "ZG7d 429 走「限流」那句话（唯一出处是 HttpClient::throttledText）",
                          problem);
                }
            }

            // ---- ZG7e 类目那一发失败：只降级，不拦整件事 ----
            {
                FakeCurseForgeApi half;
                half.filesBody = api.filesBody;
                half.modsStatus = 500;
                half.modsBody = QByteArrayLiteral("{\"error\":\"boom\"}");
                check(half.start(), "ZG7e 只有类目那一发会失败的假接口起来了");
                if (half.server.isListening()) {
                    QStringList told;
                    VersionManager manager;
                    QObject::connect(&manager, &VersionManager::modpackImportMessage, &manager,
                                     [&told](const QString &message) { told << message; });
                    manager.setCurseForgeApiBaseOverride(half.baseUrl());
                    ModpackImport::Pack pack;
                    freshPack(&pack);
                    const QString problem = manager.resolveCurseForgeFiles(&pack);
                    // 缺的只是"落到哪个目录"，文件本身下得下来 —— 停在这里等于因为一个
                    // "放哪儿"的问题把一个能装的包拒了。
                    check(problem.isEmpty() && pack.files.size() == 2,
                          "ZG7e 类目那一发没成**不拦整件事**，两个文件照收", problem);
                    QStringList fallbackPaths;
                    for (const ModpackImport::PackFile &file : pack.files) {
                        fallbackPaths << file.path;
                    }
                    check(fallbackPaths.contains(QStringLiteral("mods/a.jar")) &&
                              fallbackPaths.contains(QStringLiteral("mods/pack.zip")),
                          "ZG7e 认不出类目的一律落 mods/（不是丢掉，也不是猜一个目录）",
                          fallbackPaths.join(QStringLiteral(", ")));
                    check(told.size() == 1 && told.value(0).contains(QStringLiteral("mods/")),
                          "ZG7e 降级这件事要在日志里留一句（不然用户只看到资源包进了 mods/）",
                          told.join(QStringLiteral(" / ")));
                }
            }
        }
    }
}

