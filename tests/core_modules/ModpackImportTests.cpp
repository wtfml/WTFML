#include "Support.hpp"
#include "ModpackDomainChecks.hpp"
#include <QUuid>
#include "util/PowerShell.hpp"

void runMultiMCPatchImportTests(const QString &sandbox);

void runModpackImportTests(const QString &sandbox) {
    section("V. 整合包导入：清单解析 / 路径判据 / 按前缀解压");
    runMultiMCPatchImportTests(sandbox);

    // Archive layout and compatibility are production decisions, not filename guesses.
    {
        using namespace ModpackImport;
        for (const QStringList &names : {
                 QStringList{QStringLiteral("example.jar")},
                 QStringList{QStringLiteral("mods/example.jar"), QStringLiteral("mods/other.jar")},
                 QStringList{QStringLiteral("fabric.mod.json"), QStringLiteral("config/default.json")},
                 QStringList{QStringLiteral("META-INF/mods.toml"), QStringLiteral("config/default.toml")}}) {
            const auto layout = detectArchiveLayout(names);
            check(layout.modsBundle && layout.format == Format::Unknown,
                  "V0 单模组或模组合集不作为整合包", names.join(QLatin1Char(',')));
        }
        const QStringList nested{QStringLiteral("启动器.exe"),
            QStringLiteral(".minecraft/config/example.json"),
            QStringLiteral(".minecraft/mods/example.jar"),
            QStringLiteral(".minecraft/versions/1.20.1-fabric/1.20.1-fabric.json")};
        const auto nestedLayout = detectArchiveLayout(nested);
        check(nestedLayout.format == Format::GameFolder &&
                  nestedLayout.gameDirPrefix == QStringLiteral(".minecraft/") &&
                  nestedLayout.versionJsonEntry.endsWith(QStringLiteral("1.20.1-fabric.json")),
              "V0 懒人包忽略旁边启动器，仅选择明确的游戏目录");
        const QByteArray version = R"({"id":"1.20.1-fabric","inheritsFrom":"1.20.1","libraries":[{"name":"net.fabricmc:fabric-loader:0.16.14"}]})";
        const auto folder = parseGameFolderInstance(nested, version, nestedLayout.gameDirPrefix, "Example");
        check(folder.ok && folder.pack.gameVersion == QStringLiteral("1.20.1") &&
                  folder.pack.loaderVersion == QStringLiteral("0.16.14") &&
                  folder.pack.overridesDir == QStringLiteral(".minecraft") &&
                  folder.pack.extractExcludes.contains(QStringLiteral("launcher_accounts.json")),
              "V0 懒人包按版本元数据导入并排除账号文件", folder.problem);
        const QStringList ambiguous{QStringLiteral("config/a.json"),
            QStringLiteral("versions/1.20.1-forge/1.20.1-forge.json"),
            QStringLiteral("versions/1.21.1-fabric/1.21.1-fabric.json")};
        check(detectArchiveLayout(ambiguous).versionJsonEntry.isEmpty() &&
                  !parseGameFolderInstance(ambiguous, {}, {}, "Example").ok,
              "V0 多个带加载器实例不按归档顺序随便选一个");
        check(!parseGameFolderInstance(nested, R"({"id":"1.20.1-fabric","inheritsFrom":"1.20.1"})",
                                      nestedLayout.gameDirPrefix, "Example").ok,
              "V0 加载器缺明确版本时不自动装最新版代替");
        check(detectArchiveLayout({QStringLiteral("server.properties"), QStringLiteral("mods/a.jar"),
                                  QStringLiteral("config/a.json")}).serverPack,
              "V0 服务端不误认客户端懒人包");
        const QByteArray mmc = R"({"formatVersion":1,"components":[{"uid":"net.minecraftforge","version":"1.12.2-14.23.5.2860"},{"uid":"net.minecraft","version":"1.12.2"},{"uid":"org.lwjgl","version":"2.9.4"}]})";
        const auto standard = parseMultiMCInstance(mmc, "name=Standard Pack\n", "Pack/.minecraft/", {});
        check(standard.ok && standard.pack.name == QStringLiteral("Standard Pack") &&
                  standard.pack.loaderVersion == QStringLiteral("14.23.5.2860") &&
                  standard.pack.overridesDir == QStringLiteral("Pack/.minecraft"),
              "V0 普通 MultiMC Forge 包可以解析，组件顺序无关", standard.problem);
        for (const QByteArray &unsupported : {
                 QByteArray(R"({"components":[{"uid":"net.minecraft","version":"1.7.10"},{"uid":"me.eigenraven.lwjgl3ify.launchargs","cachedVersion":"2.1.16"}]})"),
                 QByteArray(R"({"components":[{"uid":"net.minecraft","version":"1.20.1"},{"uid":"net.minecraft","version":"1.21"}]})"),
                 QByteArray(R"({"components":[{"uid":"net.minecraft","version":"1.20.1"},{"uid":"net.minecraftforge"}]})"),
                 QByteArray(R"({"components":[{"uid":"net.minecraft","version":"1.20.1"},{"uid":"net.minecraftforge","version":"47.2.0"},{"uid":"net.fabricmc.fabric-loader","version":"0.16.14"}]})")}) {
            const auto result = parseMultiMCInstance(unsupported, {}, ".minecraft/", "Example");
            check(!result.ok && !result.problem.isEmpty(),
                  "V0 自定义组件、重复组件、缺版本、多加载器安全拒绝", result.problem);
        }
        const QStringList versionFolder{
            QStringLiteral(".minecraft/versions/Cobblemon/Cobblemon.json"),
            QStringLiteral(".minecraft/versions/Cobblemon/config/a.json"),
            QStringLiteral(".minecraft/versions/Cobblemon/mods/a.jar")};
        const auto versionFolderLayout = detectArchiveLayout(versionFolder);
        check(versionFolderLayout.format == Format::GameFolder &&
                  versionFolderLayout.gameDirPrefix == QStringLiteral(".minecraft/versions/Cobblemon/") &&
                  versionFolderLayout.versionJsonEntry.endsWith(QStringLiteral("Cobblemon.json")),
              "V0 只打包了一个 versions/<id>/ 目录的整合包认成游戏目录，不当资源包",
              versionFolderLayout.detail);

        const auto wrapped = detectArchiveLayout({QStringLiteral("说明.txt"), QStringLiteral("pack.mrpack")});
        check(wrapped.format == Format::Wrapper && wrapped.innerPackEntry == QStringLiteral("pack.mrpack"),
              "V0 包装压缩包内的 mrpack 可识别");

        // 下面几条钉的是「别的形状被认成了什么」，不加进去的话改一个判据顺序不会有人喊。
        // ① 世界存档：`level.dat` 就在这一层 ⇒ 是存档，不是懒人包、也不是一堆模组。
        const auto world = detectArchiveLayout({QStringLiteral("level.dat"),
                                                QStringLiteral("region/r.0.0.mca"),
                                                QStringLiteral("session.lock")});
        check(world.worldSave && !world.modsBundle && world.format == Format::Unknown,
              "V0 世界存档（根上有 level.dat）认成存档", world.detail);
        // ② 世界存档套在一层目录里也认得出（commonRootPrefix 那一趟）。
        const auto wrappedWorld = detectArchiveLayout({QStringLiteral("我的世界/level.dat"),
                                                       QStringLiteral("我的世界/region/r.0.0.mca")});
        check(wrappedWorld.worldSave && wrappedWorld.rootPrefix == QStringLiteral("我的世界/"),
              "V0 套在一层目录里的世界存档也认得出", wrappedWorld.rootPrefix);
        // ③ `mods/` + `versions/` 一起出现 ⇒ 是真 .minecraft 目录；光有 `mods/` 才是模组合集。
        const auto modsAndVersions = detectArchiveLayout({QStringLiteral("mods/a.jar"),
                                                          QStringLiteral("versions/1.20.1/1.20.1.json"),
                                                          QStringLiteral("versions/1.20.1/1.20.1.jar")});
        check(modsAndVersions.format == Format::GameFolder && !modsAndVersions.modsBundle,
              "V0 mods/ 和 versions/ 一起 ⇒ 游戏目录（不是模组合集）", modsAndVersions.detail);
        // ④ 服务端包即便带着 `options.txt` 也先认成服务端：顺序反过来就会当成客户端懒人包。
        const auto serverWithOptions = detectArchiveLayout({QStringLiteral("server.properties"),
                                                            QStringLiteral("options.txt"),
                                                            QStringLiteral("mods/a.jar")});
        check(serverWithOptions.serverPack && serverWithOptions.format == Format::Unknown,
              "V0 服务端包排在游戏目录前面（带 options.txt 也不改口）", serverWithOptions.detail);
        // ⑤ PCL 那种包装套在一层目录里：根上躺着的 `.mrpack` 照样认得出来。
        const auto wrappedInDir = detectArchiveLayout({QStringLiteral("整理包/pack.mrpack"),
                                                       QStringLiteral("整理包/说明.txt")});
        check(wrappedInDir.format == Format::Wrapper &&
                  wrappedInDir.innerPackEntry == QStringLiteral("整理包/pack.mrpack"),
              "V0 套在一层目录里的 PCL 包装也认得出", wrappedInDir.detail);
        // ⑥ 只摆着几份 .jar 的压缩包 = 一堆模组，不是整合包（和"只有一个 mods/"同一档）。
        const auto rootJarBundle = detectArchiveLayout(
            {QStringLiteral("a.jar"), QStringLiteral("b.jar"), QStringLiteral("说明.txt")});
        check(!rootJarBundle.modsBundle && rootJarBundle.format == Format::Unknown,
              "V0 根上几份 .jar 再配一个说明：不算「纯 jar 堆」（有别的文件就说不出它是什么）",
              rootJarBundle.detail);
        const auto bareRootJars =
            detectArchiveLayout({QStringLiteral("a.jar"), QStringLiteral("b.jar")});
        check(bareRootJars.modsBundle, "V0 根上只有 .jar：按模组合集处理", bareRootJars.detail);

        // ⑥b 旧版 Technic / 老式 Forge 包：加载器在 `bin/modpack.jar` 里，不写进版本 json。
        // 这一条只要求"认得出并说出来"—— 不要求推断 Forge 版本（文件名里没有版本号）。
        const auto technic = detectArchiveLayout(
            {QStringLiteral("config/a.cfg"), QStringLiteral("mods/x.jar"),
             QStringLiteral("bin/modpack.jar")});
        check(technic.format == Format::GameFolder && technic.legacyLauncherJar
                  && technic.detail.contains(QStringLiteral("加载器")),
              "★ V0 旧版 Technic 形状（bin/modpack.jar）认成懒人包并说清加载器要自己补装",
              technic.detail);
        const auto plainFolder = detectArchiveLayout(
            {QStringLiteral("config/a.cfg"), QStringLiteral("mods/x.jar")});
        check(plainFolder.format == Format::GameFolder && !plainFolder.legacyLauncherJar,
              "V0 普通懒人包没有 bin/ 启动器 jar 时不多说一句", plainFolder.detail);

        // ⑦ 几个实例打在一个压缩包里：**不猜**（随便挑一个装，剩下的那几份用户永远
        // 不知道没装），但要说得清为什么。
        const QStringList multiInstance{QStringLiteral("实例A/.minecraft/config/a.json"),
                                        QStringLiteral("实例A/.minecraft/mods/a.jar"),
                                        QStringLiteral("实例B/.minecraft/config/b.json"),
                                        QStringLiteral("实例B/.minecraft/mods/b.jar")};
        const auto multi = detectArchiveLayout(multiInstance);
        check(multi.format == Format::Unknown && !multi.gameFolderMarkers &&
                  !multi.modsBundle && multi.gameDirPrefix.isEmpty(),
              "V0 多实例懒人包不猜（不拼成一个实例、也不当成模组合集）", multi.detail);
        check(multi.detail.contains(QStringLiteral("多个")) &&
                  multi.detail.contains(QStringLiteral(".minecraft")),
              "V0 多实例懒人包说清楚为什么没导入", multi.detail);
        // 套在一层目录里的多实例也认得出（`commonRootPrefix()` 那一趟）。
        const auto wrappedMulti = detectArchiveLayout(
            {QStringLiteral("整合包/实例A/.minecraft/config/a.json"),
             QStringLiteral("整合包/实例B/.minecraft/config/b.json")});
        check(wrappedMulti.format == Format::Unknown &&
                  wrappedMulti.rootPrefix == QStringLiteral("整合包/") &&
                  wrappedMulti.detail.contains(QStringLiteral("多个 .minecraft 目录")),
              "V0 套在一层目录里的多实例也说得清", wrappedMulti.detail);
        // 只有一个 `.minecraft` 的兄弟目录（说明、启动器本体）**不算**多实例。
        const auto singleInDir = detectArchiveLayout(
            {QStringLiteral("实例A/.minecraft/config/a.json"), QStringLiteral("说明.txt")});
        check(!singleInDir.detail.contains(QStringLiteral("实例")),
              "V0 只有一个 .minecraft 时不报多实例", singleInDir.detail);

        // ⑧ 空归档 / 只有杂项：什么都不认（不建一个空实例，也不说它是模组合集）。
        for (const QStringList &nothing : {QStringList{}, QStringList{QStringLiteral("readme.txt")},
             QStringList{QStringLiteral("__MACOSX/._x"), QStringLiteral(".DS_Store")}}) {
            const auto empty = detectArchiveLayout(nothing);
            check(!empty.worldSave && !empty.serverPack && !empty.modsBundle &&
                      empty.format == Format::Unknown,
                  "V0 认不出形状时什么都不认（不猜）", nothing.join(QLatin1Char(',')));
        }

        const QString root = sandbox + QStringLiteral("/folder-import-fixture");
        const QString source = root + QStringLiteral("/source");
        writeFileAt(source + QStringLiteral("/config/example.json"), "configuration");
        writeFileAt(source + QStringLiteral("/mods/example.jar"), "mod-bytes");
        writeFileAt(source + QStringLiteral("/accounts.json"), "private-fixture");
        writeFileAt(source + QStringLiteral("/versions/old.json"), "old-version");
        const QString archive = root + QStringLiteral("/pack.zip");
        QString error;
        const bool zipped = ZipUtil::createZip(archive, {
            {QStringLiteral(".minecraft/config/example.json"), source + QStringLiteral("/config/example.json")},
            {QStringLiteral(".minecraft/mods/example.jar"), source + QStringLiteral("/mods/example.jar")},
            {QStringLiteral(".minecraft/accounts.json"), source + QStringLiteral("/accounts.json")},
            {QStringLiteral(".minecraft/versions/old.json"), source + QStringLiteral("/versions/old.json")}}, &error);
        const QString destination = root + QStringLiteral("/imported");
        int count = 0;
        const bool extracted = zipped && ZipUtil::extractSubtrees({archive}, destination,
            {QStringLiteral(".minecraft/")}, &count, &error, {}, folder.pack.extractExcludes);
        check(extracted && count == 2 && QFile::exists(destination + QStringLiteral("/mods/example.jar")) &&
                  QFile::exists(destination + QStringLiteral("/config/example.json")) &&
                  !QFile::exists(destination + QStringLiteral("/accounts.json")) &&
                  !QFile::exists(destination + QStringLiteral("/versions/old.json")),
              "V0 实际导入游戏内容，排除来源账号与旧版本文件", error);
    }

    // Opt-in local samples: read metadata only; never writes into the original archive or game directory.
    for (const QString &sample : qEnvironmentVariable("WTFML_TEST_PACK_ARCHIVES").split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        QStringList names;
        QString error;
        const auto status = ZipUtil::listEntries(sample, &names, &error);
        check(status == ZipUtil::EntryStatus::Ok, "V0 本地真实样本可读取条目", error);
        if (status != ZipUtil::EntryStatus::Ok) continue;
        const auto layout = ModpackImport::detectArchiveLayout(names);
        out() << "        [真实包] " << QFileInfo(sample).fileName() << ": " << layout.detail << "\n";
        if (layout.format == ModpackImport::Format::MultiMC) {
            QByteArray bytes;
            const auto readStatus = ZipUtil::readEntry(sample, layout.mmcPackEntry, &bytes, &error,
                QStringLiteral("读取本地样本元数据"), 4 * 1024 * 1024);
            check(readStatus == ZipUtil::EntryStatus::Ok, "V0 真实 MultiMC 元数据读取", error);
            const auto parsed = ModpackImport::parseMultiMCInstance(bytes, {}, layout.gameDirPrefix, QFileInfo(sample).completeBaseName());
            if (QFileInfo(sample).fileName().startsWith(QStringLiteral("GT_New_Horizons_"))) {
                check(!parsed.ok && parsed.problem.contains(QStringLiteral("启动组件")),
                      "V0 GTNH 特殊组件不被假装为普通 Forge 安装成功", parsed.problem);
            }
        } else if (QFileInfo(sample).fileName().contains(QStringLiteral("_Server_"))) {
            check(layout.serverPack, "V0 GTNH 服务端包明确排除客户端导入");
        } else if (layout.format == ModpackImport::Format::CurseForge ||
                   layout.format == ModpackImport::Format::Modrinth) {
            QByteArray bytes;
            const auto readStatus = ZipUtil::readEntry(sample, layout.manifestEntry, &bytes, &error,
                QStringLiteral("读取本地清单"), 16 * 1024 * 1024);
            const auto parsed = layout.format == ModpackImport::Format::CurseForge
                ? ModpackImport::parseCurseForgeManifest(bytes, true)
                : ModpackImport::parseModrinthIndex(bytes, true);
            check(readStatus == ZipUtil::EntryStatus::Ok && parsed.ok,
                  "V0 本地标准包清单可解析", parsed.problem);
        } else if (layout.format == ModpackImport::Format::Wrapper) {
            const QString inner = sandbox + QStringLiteral("/sample-") +
                QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".mrpack");
            const auto extracted = ZipUtil::extractEntryToFile(sample, layout.innerPackEntry, inner, &error);
            QByteArray bytes;
            const auto readStatus = extracted == ZipUtil::EntryStatus::Ok
                ? ZipUtil::readEntry(inner, ModpackImport::modrinthIndexName(), &bytes, &error,
                                    QStringLiteral("读取包装内清单"), 16 * 1024 * 1024)
                : ZipUtil::EntryStatus::Failed;
            const auto parsed = ModpackImport::parseModrinthIndex(bytes, true);
            check(readStatus == ZipUtil::EntryStatus::Ok && parsed.ok,
                  "V0 真包装解出到测试沙盒后可解析内层清单", parsed.problem);
            QFile::remove(inner);
        }
    }

    // --- V1. 清单文件叫什么、认哪些加载器 -------------------------------------
    check(ModpackImport::modrinthIndexName() == QStringLiteral("modrinth.index.json"),
          "V1 Modrinth 的清单文件名叫 modrinth.index.json",
          ModpackImport::modrinthIndexName());
    check(ModpackImport::curseForgeManifestName() == QStringLiteral("manifest.json"),
          "V1 CurseForge 的清单文件名叫 manifest.json",
          ModpackImport::curseForgeManifestName());
    // 顺序**钉住**：靠遍历 QJsonObject 的键去挑"包里要哪个加载器"是拿实现细节当规则用，
    // 换一次 Qt 就可能挑到另一个。四条的顺序 = 认的顺序。
    const QStringList keys = ModpackImport::loaderDependencyKeys();
    check(keys.size() == 4 && keys.at(0) == QStringLiteral("fabric-loader") &&
              keys.at(1) == QStringLiteral("forge") && keys.at(2) == QStringLiteral("neoforge") &&
              keys.at(3) == QStringLiteral("quilt-loader"),
          "V1 认的加载器键是固定的四条，顺序也固定", keys.join(QLatin1Char(',')));
    check(ModpackImport::unsupportedLoaderText(QString(), QString()).isEmpty() &&
              ModpackImport::unsupportedLoaderText(QStringLiteral("fabric-loader"),
                                                   QStringLiteral("0.16.14"))
                  .isEmpty() &&
              ModpackImport::unsupportedLoaderText(QStringLiteral("forge"),
                                                   QStringLiteral("47.4.10"))
                  .isEmpty(),
          "V1 纯原版 / Fabric / Forge 都算能用，没有一句拦人的话");
    const QString quiltText = ModpackImport::unsupportedLoaderText(
        QStringLiteral("quilt-loader"), QStringLiteral("0.20.0"));
    check(quiltText.isEmpty(),
          "V1 Quilt 不再被整合包导入拒绝（2.1.4 起客户端可安装/可升级）", quiltText);
    // NOTE: 这个函数只回答一件事：**两个清单都没有** ⇒ 认不出它是什么格式。所以钉的是
    // "把两个名字都写出来"，以及它替用户猜了一句"多半是个普通压缩包"（不说这一句，
    // 用户只能自己猜要不要放弃）。
    const QString cfText = ModpackImport::unsupportedFormatText(ModpackImport::Format::CurseForge);
    check(cfText.contains(QStringLiteral("manifest.json")) &&
              cfText.contains(ModpackImport::modrinthIndexName()) &&
              cfText.contains(QStringLiteral("压缩包")),
          "V1 认不出格式那句话：两个清单文件名都写着 + 替用户猜一句它多半是什么", cfText);
    const QString unknownText = ModpackImport::unsupportedFormatText(ModpackImport::Format::Unknown);
    check(unknownText.contains(QStringLiteral("modrinth.index.json")) &&
              unknownText.contains(QStringLiteral("manifest.json")),
          "V1 认不出格式的话把两种清单的名字都说了出来（用户能自己去核对）", unknownText);

    // --- V2. 路径判据（这条漏一个 = 整合包能往实例目录外面写文件） --------------
    {
        struct Case {
            const char *path;
            bool safe;
            const char *why;
        };
        const Case cases[] = {
            {"mods/sodium.jar", true, "普通的相对路径"},
            {"config/openloader/data/x.zip", true, "多几层也可以"},
            {"mods/../../x.jar", false, "逐段判：中间那段 `..` 拦得住"},
            {"../../x.jar", false, "开头的 `..`"},
            {"..\\..\\x.jar", false, "反斜杠一律拒绝（按 / 切它就是一整段，逐段看不出问题）"},
            {"mods\\sodium.jar", false, "同上：Windows 上反斜杠也是分隔符"},
            {"/etc/passwd", false, "绝对路径"},
            {"C:/x.jar", false, "盘符（冒号本来就在非法字符里）"},
            {"mods/a:b.jar", false, "段里带冒号"},
            {"", false, "空路径"},
            {"mods/", false, "结尾多一个斜杠会切出一个空段"},
            {"a//b", false, "中间空段"},
            {"mods/./x.jar", false, "`.` 不是一段合法的目录名"},
            {"mods/x./y.jar", false, "以点结尾的段（Windows 会悄悄吃掉那个点）"},
            {"mods/x.jar ", false, "以空格结尾的段（同上；整合包这条路不做 trim）"},
            {"mods/CON.jar", false, "段的主名是 Windows 保留设备名"},
            {"mods/con.txt", false, "保留名判据不区分大小写（section('.') 之后是 con）"},
        };
        int wrong = 0;
        for (const Case &c : cases) {
            if (ModpackImport::isSafeRelativePath(QString::fromUtf8(c.path)) != c.safe) {
                ++wrong;
                out() << "        [判错了] " << c.path << "（该是"
                      << (c.safe ? "安全" : "不安全") << "：" << c.why << "）\n";
            }
        }
        check(wrong == 0, "V2 路径安全性真值表 17 条全对", QStringLiteral("错 %1 条").arg(wrong));

        // 长度：240 是上限（Windows 的 MAX_PATH 260 减去实例目录那一段）。边界要判准，
        // 否则"刚好 240 的正常长路径"会被整包拒绝。
        const QString ok240 = QStringLiteral("mods/") + QString(235, QLatin1Char('a'));
        check(ok240.length() == 240 && ModpackImport::isSafeRelativePath(ok240),
              "V2 正好 240 个字符的路径放行", QStringLiteral("长度 %1").arg(ok240.length()));
        const QString tooLong = ok240 + QLatin1Char('a');
        check(!ModpackImport::isSafeRelativePath(tooLong), "V2 241 个字符的路径拒绝");
    }

    // --- V3. 从包名推实例名 ---------------------------------------------------
    {
        check(ModpackImport::suggestInstanceName(QStringLiteral("  一个包  ")) ==
                  QStringLiteral("一个包"),
              "V3 前后的空白去掉");
        check(ModpackImport::suggestInstanceName(
                  QStringLiteral("ATT2: The Journey Of Revelation")) ==
                  QStringLiteral("ATT2- The Journey Of Revelation"),
              "V3 建不出目录的字符换成 `-`（这里是 名字里的 `:`）",
              ModpackImport::suggestInstanceName(
                  QStringLiteral("ATT2: The Journey Of Revelation")));
        check(ModpackImport::suggestInstanceName(QStringLiteral("包.")) == QStringLiteral("包"),
              "V3 结尾的点去掉（Windows 会静默吃掉它）",
              ModpackImport::suggestInstanceName(QStringLiteral("包.")));
        check(ModpackImport::suggestInstanceName(QString()) == QStringLiteral("整合包") &&
                  ModpackImport::suggestInstanceName(QStringLiteral("   ")) ==
                      QStringLiteral("整合包") &&
                  ModpackImport::suggestInstanceName(QStringLiteral("..")) ==
                      QStringLiteral("整合包"),
              "V3 空的 / 全是空白 / `..` 都退化成一句通用的名字");
        const QString con = ModpackImport::suggestInstanceName(QStringLiteral("CON"));
        check(con != QStringLiteral("CON") && con.contains(QStringLiteral("CON")),
              "V3 保留设备名换个说法绕开（不能原样用它建目录）", con);
        const QString longName = ModpackImport::suggestInstanceName(QString(200, QLatin1Char('x')));
        check(longName.length() <= InstanceFiles::kMaxDirNameLength && !longName.isEmpty(),
              "V3 超长的名字收到实例名的长度上限内",
              QStringLiteral("长度 %1").arg(longName.length()));
        // 这一条是这一节里最要紧的：候选名字是**界面预填**到输入框里的，它自己过不了
        // 校验的话，用户一进来就撞一句报错，而他什么都没输。
        // NOTE: 这里比的是 suggestInstanceName() 的**产物**，不是包名本身 —— 包名里带
        // 冒号、带结尾的点、叫 CON 都是正常的（真那份就叫
        // "ATT2: The Journey Of Revelation.1.8.4"，名字里就有冒号）。要证的是
        // "推出来、预填进输入框的那个名字，用户直接点确定就能过"。
        const QStringList packNames = {QStringLiteral("ATT2: The Journey Of Revelation.1.8.4"),
                                       QStringLiteral("包."), QStringLiteral("CON"), QString(),
                                       longName, QStringLiteral("带 空格 的名字"),
                                       QStringLiteral(".."), QStringLiteral("名字里带/斜杠")};
        int bad = 0;
        for (const QString &packName : packNames) {
            const QString candidate = ModpackImport::suggestInstanceName(packName);
            const QString problem = InstanceFiles::validateRenameTarget(candidate, QString());
            if (!problem.isEmpty()) {
                ++bad;
                out() << "        [推出来的名字过不了校验] 包名「" << packName << "」-> 「"
                      << candidate << "」：" << problem << "\n";
            }
        }
        check(bad == 0, "V3 推出来的每一个候选名字都过得了 validateRenameTarget()",
              QStringLiteral("坏 %1 个").arg(bad));

        // --- V3b. 重名自动加后缀（suggestUniqueInstanceName）--------------------
        QSet<QString> existing{QStringLiteral("FPS Modpack"), QStringLiteral("FPS Modpack-2")};
        const auto checkAvailable = [&existing](const QString &candidate) {
            for (const QString &taken : existing) {
                if (taken.compare(candidate, Qt::CaseInsensitive) == 0) {
                    return false;
                }
            }
            return true;
        };
        check(ModpackImport::suggestUniqueInstanceName(QStringLiteral("Clean Pack"), checkAvailable) ==
                  QStringLiteral("Clean Pack"),
              "V3b 不重名时原样返回候选名");
        check(ModpackImport::suggestUniqueInstanceName(QStringLiteral("fps modpack"), checkAvailable) ==
                  QStringLiteral("fps modpack-3"),
              "V3b 大小写不敏感判重并递增 -2 / -3 后缀",
              ModpackImport::suggestUniqueInstanceName(QStringLiteral("fps modpack"), checkAvailable));
        const QString maxLenName(InstanceFiles::kMaxDirNameLength, QLatin1Char('A'));
        const QSet<QString> takenMax{maxLenName};
        const QString suffixedMax = ModpackImport::suggestUniqueInstanceName(
            maxLenName, [&](const QString &candidate) { return !takenMax.contains(candidate); });
        check(suffixedMax.endsWith(QStringLiteral("-2")) &&
                  suffixedMax.length() <= InstanceFiles::kMaxDirNameLength &&
                  InstanceFiles::validateDirName(suffixedMax).isEmpty(),
              "V3b 满 64 字符重名时截短前缀后再拼后缀，仍满足长度与目录名校验", suffixedMax);

        // --- V3c. 一键安装默认名挑选（suggestOneClickInstanceName）--------------
        {
            const auto noneUnfinished = [](const QString &) { return false; };
            const auto allAvailable = [](const QString &) { return true; };

            const auto fresh = ModpackImport::suggestOneClickInstanceName(
                QStringLiteral("FPS Modpack"), noneUnfinished, allAvailable);
            check(fresh.name == QStringLiteral("FPS Modpack") && !fresh.resume,
                  "V3c 无旧目录 → 选原名且不续装", fresh.name);

            const auto resumeBase = ModpackImport::suggestOneClickInstanceName(
                QStringLiteral("FPS Modpack"),
                [](const QString &c) { return c.compare(QStringLiteral("FPS Modpack"), Qt::CaseInsensitive) == 0; },
                [](const QString &c) { return c.compare(QStringLiteral("FPS Modpack"), Qt::CaseInsensitive) != 0; });
            check(resumeBase.name == QStringLiteral("FPS Modpack") && resumeBase.resume,
                  "V3c 原名是没装完的 → 选原名且续装", resumeBase.name);

            const auto resumeSecond = ModpackImport::suggestOneClickInstanceName(
                QStringLiteral("FPS Modpack"),
                [](const QString &c) { return c.compare(QStringLiteral("FPS Modpack-2"), Qt::CaseInsensitive) == 0; },
                [](const QString &c) {
                    return c.compare(QStringLiteral("FPS Modpack"), Qt::CaseInsensitive) != 0
                           && c.compare(QStringLiteral("FPS Modpack-2"), Qt::CaseInsensitive) != 0;
                });
            check(resumeSecond.name == QStringLiteral("FPS Modpack-2") && resumeSecond.resume,
                  "V3c 原名被正常实例占、-2 是没装完的 → 选 -2 且续装", resumeSecond.name);

            const auto allNormal = ModpackImport::suggestOneClickInstanceName(
                QStringLiteral("FPS Modpack"), noneUnfinished, checkAvailable);
            check(allNormal.name == QStringLiteral("FPS Modpack-3") && !allNormal.resume,
                  "V3c 都被正常实例占 → 选第一个空名字且不续装", allNormal.name);
        }
    }

    // --- V3b. 先试哪条地址：cdn.modrinth.com 排最前 -----------------------------
    {
        const QString cf1 = QStringLiteral(
            "https://mediafilez.forgecdn.net/files/8246/47/L_Enders_Cataclysm-3.31.jar");
        const QString cf2 = QStringLiteral(
            "https://edge.forgecdn.net/files/8246/47/L_Enders_Cataclysm-3.31.jar");
        const QString mr = QStringLiteral(
            "https://cdn.modrinth.com/data/46KJle7n/versions/C3H0azzB/L_Enders_Cataclysm-3.31.jar");

        const QStringList tri = ModpackImport::preferredUrlOrder({cf1, cf2, mr});
        check(tri.size() == 3 && tri.value(0) == mr,
              "V3b 三条地址里 modrinth 那条排到最前（另两条只换位次、不丢）",
              tri.join(QStringLiteral(" | ")));
        check(tri.value(1) == cf1 && tri.value(2) == cf2,
              "V3b 剩下那两条保持原来的相对次序（换位次不是打乱）",
              tri.join(QStringLiteral(" | ")));

        // 只有一条 modrinth 的（那个包里 42 个文件是这样）：原样返回。
        const QStringList onlyMr = ModpackImport::preferredUrlOrder({mr});
        check(onlyMr.size() == 1 && onlyMr.value(0) == mr, "V3b 单条地址原样返回");

        // 三条里一条 modrinth 都没有：**次序一个字符都不动** —— 这条最要紧，
        // 它是"认不出就别乱动"那条兜底（全是 forgecdn 的文件有 90 个）。
        const QStringList onlyCf = ModpackImport::preferredUrlOrder({cf1, cf2});
        check(onlyCf == QStringList({cf1, cf2}),
              "V3b 一条 modrinth 都没有时次序不动", onlyCf.join(QStringLiteral(" | ")));

        // host 必须按 QUrl 判，不能拿 QString::contains("modrinth") 那种办法认：
        // 一个**别家**的 host 里带这个字样（比如被抓包的镜像站）不该被当成首选。
        const QStringList trap = ModpackImport::preferredUrlOrder(
            {cf1, QStringLiteral("https://cdn.modrinth.com.evil.example/x.jar")});
        check(trap.value(0) == cf1,
              "V3b 认的是 host 本身，不是串里有没有 modrinth 那几个字",
              trap.join(QStringLiteral(" | ")));

        // 空表（清单里 downloads 是空的那些已经在解析时被剔掉了，这里只是别崩）。
        check(ModpackImport::preferredUrlOrder({}).isEmpty(), "V3b 空表返回空表");
    }

    // --- V4. 解析：一小段一小段地看它拒绝什么、接受什么 ------------------------

    {
        // 最小的一份合法清单：一个文件、一条依赖。
        const QByteArray minimal = R"json({
            "formatVersion": 1,
            "game": "minecraft",
            "name": "测试包",
            "versionId": "1.0",
            "dependencies": {"minecraft": "1.20.2", "fabric-loader": "0.16.14"},
            "files": [
                {"path": "mods/a.jar", "hashes": {"sha1": "aa"},
                 "downloads": ["https://example.invalid/a.jar"], "fileSize": 12}
            ]
        })json";
        const ModpackImport::Result okResult = ModpackImport::parseModrinthIndex(minimal);
        check(okResult.ok && okResult.pack.name == QStringLiteral("测试包") &&
                  okResult.pack.version == QStringLiteral("1.0") &&
                  okResult.pack.gameVersion == QStringLiteral("1.20.2") &&
                  okResult.pack.loaderKey == QStringLiteral("fabric-loader") &&
                  okResult.pack.loaderVersion == QStringLiteral("0.16.14") &&
                  okResult.pack.files.size() == 1 && okResult.pack.filesSkipped == 0 &&
                  okResult.pack.format == ModpackImport::Format::Modrinth,
              "V4 一份最小的合法清单：名字/版本/游戏版本/加载器/文件都对",
              okResult.ok ? QStringLiteral("%1 个文件").arg(okResult.pack.files.size())
                          : okResult.problem);
        if (okResult.ok && okResult.pack.files.size() == 1) {
            const ModpackImport::PackFile &file = okResult.pack.files.first();
            check(file.path == QStringLiteral("mods/a.jar") && file.sha1 == QStringLiteral("aa") &&
                      file.size == 12 && file.urls.size() == 1,
                  "V4 每条文件的路径 / sha1 / 字节数 / 地址都读出来了");
        }

        const auto problemOf = [](const QByteArray &json) {
            const ModpackImport::Result r = ModpackImport::parseModrinthIndex(json);
            return r.ok ? QString() : r.problem;
        };
        check(problemOf(QByteArray("这不是 json")).contains(QStringLiteral("无法解析")),
              "V4 不是 json：说「无法解析」");
        check(problemOf(QByteArray("{}")).contains(QStringLiteral("formatVersion")),
              "V4 缺 formatVersion/files：说它不是整合包清单");
        check(problemOf(R"json({"formatVersion":1,"files":[],"game":"factorio",
                                "dependencies":{"minecraft":"1.20.2"}})json")
                  .contains(QStringLiteral("Minecraft")),
              "V4 别的游戏的包：说「这不是 Minecraft 的整合包」");
        check(problemOf(R"json({"formatVersion":1,"files":[],"game":"minecraft",
                                "dependencies":{}})json")
                  .contains(QStringLiteral("dependencies.minecraft")),
              "V4 没写基于哪个原版：说清了缺的是 dependencies.minecraft");
        check(!problemOf(R"json({"formatVersion":1,"game":"minecraft",
                                 "dependencies":{"minecraft":"1.20.2"},"files":[]})json")
                   .isEmpty(),
              "V4 一条文件都没有：不装（不然就是「装完了，什么都没有」）");

        // 路径不安全 = **整包拒绝**，不是跳过那一条。它碰的是"会写到实例目录外面去"，
        // 放过其余的等于让用户在一个被做过手脚的包上继续走。
        const QByteArray traversal = R"json({
            "formatVersion": 1, "game": "minecraft",
            "dependencies": {"minecraft": "1.20.2"},
            "files": [
                {"path": "mods/a.jar", "downloads": ["https://example.invalid/a.jar"]},
                {"path": "../../外面的文件.txt", "downloads": ["https://example.invalid/b"]}
            ]
        })json";
        const ModpackImport::Result bad = ModpackImport::parseModrinthIndex(traversal);
        check(!bad.ok && bad.problem.contains(QStringLiteral("../../外面的文件.txt")) &&
                  bad.problem.contains(QStringLiteral("导入已取消")),
              "V4 有一条往外跳的路径：整包拒绝，而且把那一条原样指出来", bad.problem);

        // 跳过的两类：只给服务端用的、没有下载地址的。两个都要**记一笔**（用户看不到
        // 的那部分最容易变成"进了游戏才发现少个 mod"）。
        const QByteArray skipped = R"json({
            "formatVersion": 1, "game": "minecraft",
            "dependencies": {"minecraft": "1.20.2"},
            "files": [
                {"path": "mods/a.jar", "downloads": ["https://example.invalid/a.jar"]},
                {"path": "mods/server.jar", "downloads": ["https://example.invalid/s.jar"],
                 "env": {"client": "unsupported"}},
                {"path": "config/x.txt", "downloads": []},
                {"path": "mods/a.jar", "hashes": {"sha1": "后面那条"},
                 "downloads": ["https://example.invalid/a2.jar"]}
            ]
        })json";
        const ModpackImport::Result s = ModpackImport::parseModrinthIndex(skipped);
        check(s.ok && s.pack.files.size() == 1 && s.pack.filesSkipped == 2,
              "V4 只给服务端用的、没有下载地址的各记一笔跳过；同一个路径出现两次以最后一条为准",
              s.ok ? QStringLiteral("留下 %1、跳过 %2").arg(s.pack.files.size())
                            .arg(s.pack.filesSkipped)
                    : s.problem);
        if (s.ok && s.pack.files.size() == 1) {
            check(s.pack.files.first().sha1 == QStringLiteral("后面那条"),
                  "V4 重复路径：留下的是**最后**那一条");
        }

        // 纯原版的包：依赖表里只有 minecraft。能装，只是没有加载器要塞。
        const QByteArray vanillaPack = R"json({
            "formatVersion": 1, "game": "minecraft",
            "dependencies": {"minecraft": "1.18.2"},
            "files": [{"path": "mods/a.jar", "downloads": ["https://example.invalid/a.jar"]}]
        })json";
        const ModpackImport::Result v = ModpackImport::parseModrinthIndex(vanillaPack);
        check(v.ok && v.pack.loaderKey.isEmpty(),
              "V4 纯原版的包（依赖表里只有 minecraft）认了，加载器键是空的");

        // 名字缺了不能拒绝整个包（它只是"起实例名时的候选"）。
        const QByteArray noName = R"json({
            "formatVersion": 1, "game": "minecraft", "versionId": "9.9",
            "dependencies": {"minecraft": "1.20.2"},
            "files": [{"path": "mods/a.jar", "downloads": ["https://example.invalid/a.jar"]}]
        })json";
        const ModpackImport::Result nn = ModpackImport::parseModrinthIndex(noName);
        check(nn.ok && nn.pack.name == QStringLiteral("9.9") && nn.pack.version == QStringLiteral("9.9"),
              "V4 没有 name：退化成版本号，不因为缺名字拒绝整个包",
              nn.ok ? nn.pack.name : nn.problem);
    }

    // --- V5. ZipUtil：真造一个压缩包、真解一次 -------------------------------
    //
    // 两个坑都是实测踩出来的，都值得钉住：
    // ① PowerShell 的 Compress-Archive 写进去的条目名用的是**反斜杠**。按 `/` 比前缀
    // 的话一条都匹配不上 —— 表现是"解压成功、解出来 0 个文件"，不报任何错。
    // ② 两个前缀的**顺序**就是优先级（client-overrides 盖过 overrides），这是整合包
    // 格式的规矩，不能调换。
    {
        const QString work = sandbox + "/modpack-check";
        QDir(work).removeRecursively();
        const QString src = work + "/src";
        writeFileAt(src + "/overrides/config/a.txt", "来自 overrides");
        writeFileAt(src + "/client-overrides/config/a.txt", "来自 client-overrides");
        writeFileAt(src + "/overrides/config/b.txt", "只在 overrides 里");
        writeFileAt(src + "/overrides/mods/x.jar", "假的 mod");
        writeFileAt(src + "/modrinth.index.json", "{}");
        writeFileAt(src + "/README.txt", "不该被解出来");

        const QString archive = work + "/pack.zip";
        const QString script =
            QStringLiteral("$ProgressPreference='SilentlyContinue'; Compress-Archive -Path %1 -DestinationPath %2 -Force")
                .arg(QStringLiteral("'") + QDir::toNativeSeparators(src) + QStringLiteral("\\*'"),
                     QStringLiteral("'") + QDir::toNativeSeparators(archive) + QStringLiteral("'"));
        QProcess ps;
        ps.start(PowerShell::executable(),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                  QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                  QStringLiteral("-Command"), script});
        const bool compressed = ps.waitForFinished(180000) &&
                                ps.exitStatus() == QProcess::NormalExit && ps.exitCode() == 0;
        check(compressed, "V5 造出一个测试用的压缩包（Compress-Archive）",
              QString::fromLocal8Bit(ps.readAllStandardError()).trimmed().left(200));
        if (!compressed) {
            QDir(work).removeRecursively();
            return;
        }

        // 读单个条目：有 / 没有 / 不是 zip 三种结果必须分得清 —— 界面给的是三句不同的话。
        QByteArray bytes;
        QString error;
        check(ZipUtil::readEntry(archive, ModpackImport::modrinthIndexName(), &bytes, &error) ==
                      ZipUtil::EntryStatus::Ok &&
                  bytes == QByteArray("{}"),
              "V5 readEntry 读到了那一份清单，字节原样", QString::fromUtf8(bytes).left(40));
        check(ZipUtil::readEntry(archive, QStringLiteral("nope.json"), &bytes, &error) ==
                  ZipUtil::EntryStatus::NotFound,
              "V5 压缩包里没有那个条目：报 NotFound（= 「这不是整合包」，不是「文件坏了」）");
        // 上面那次 NotFound 之后 bytes 要被清掉，不能留着上一次的内容（留着的话调用方
        // 拿它去解析，会解出一份**上一个包**的清单）。
        check(bytes.isEmpty(), "V5 没读到东西时 out 被清空了（不然会拿上一次的字节去解析）");
        check(ZipUtil::readEntry(archive, QStringLiteral("MODRINTH.INDEX.JSON"), &bytes,
                                 &error) == ZipUtil::EntryStatus::Ok,
              "V5 条目名大小写不一样也认得（zip 的条目名区分大小写，但整合包工具不一定讲究）");
        const QString notAZip = work + "/假.zip";
        writeFileAt(notAZip, "这不是压缩包");
        check(ZipUtil::readEntry(notAZip, ModpackImport::modrinthIndexName(), &bytes, &error) ==
                  ZipUtil::EntryStatus::NotAnArchive,
              "V5 不是 zip 的文件：报 NotAnArchive（= 「这个文件打不开」）");

        // 按前缀解压：剥掉前缀、"杂项一个都不碰"、后一个前缀盖前一个。
        const QString out = work + "/out";
        int extracted = 0;
        const QStringList roots = {QStringLiteral("overrides/"), QStringLiteral("client-overrides/")};
        {
            QElapsedTimer budget;
            budget.start();
            int checks = 0;
            const bool cancelled = !ZipUtil::extractSubtrees({archive}, work + "/cancelled-overrides", roots,
                nullptr, &error, [&] { return ++checks >= 2; });
            check(cancelled && checks >= 2 && error.contains(QStringLiteral("取消")) && budget.elapsed() < 8000,
                  "V5 覆盖文件解压启动后可快速取消，不等待完整解压超时", error);
        }
        const bool extractedOk =
            ZipUtil::extractSubtrees({archive}, out, roots, &extracted, &error);
        check(extractedOk && extracted == 4,
              "V5 extractSubtrees 解出 4 个条目（两个 a.txt、一个 b.txt、一个 x.jar）",
              extractedOk ? QStringLiteral("解出 %1 个").arg(extracted) : error);

        const auto readText = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        check(!QFile::exists(out + "/overrides/config/a.txt") &&
                  !QFile::exists(out + "/client-overrides/config/a.txt") &&
                  QFile::exists(out + "/config/a.txt"),
              "V5 前缀被剥掉了：解出来的是 <目标>/config/a.txt，不是 <目标>/<前缀>/config/a.txt");
        check(readText(out + "/config/a.txt") == QStringLiteral("来自 client-overrides"),
              "V5 两个前缀撞同一个文件时，client-overrides 盖过 overrides");
        check(readText(out + "/config/b.txt") == QStringLiteral("只在 overrides 里") &&
                  readText(out + "/mods/x.jar") == QStringLiteral("假的 mod"),
              "V5 overrides 底下别的条目也解出来了");
        check(!QFile::exists(out + "/modrinth.index.json") && !QFile::exists(out + "/README.txt"),
              "V5 不属于任何前缀的条目一个都没碰（清单文件和 README 没进实例目录）");

        QDir(work).removeRecursively();
    }

    // --- V5z. 拖进来的那一个文件到底是什么 --------------------
    //
    // 要求：「打开某个实例设置的时候如果拖入.jar/.zip之类的文件，检测是整合包还是资源包
    // 等」。判据层是 `manage/DroppedResource`（纯函数：输入一条路径，输出一个 Kind）——
    // 它只做判断，问什么、往哪儿放都在 MainWindow 那边，所以这一节能在这儿钉死。
    //
    // 八个包**一趟 PowerShell 造完**（一个 ZipArchive::Create 里连着建八个）：.NET 的
    // `CreateEntry()` 按原字面量写条目名，`Compress-Archive` 会规整掉反斜杠 —— 要测
    // "反斜杠写的包"就只能走 .NET。见 V5c 里那条 NOTE，同一个理由。
    //
    // 优先级那两条是这一节的**主角**：一个包里同时有 `pack.mcmeta`+`assets/` 和
    // `shaders/`（真实世界里的资源包太常见了），判成资源包还是光影，取决于两个判据的
    // 先后 —— 排错了不会崩、不会报错，就是往 shaderpacks/ 里塞一个资源包。
    {
        const QString work = sandbox + "/dropped";
        QDir(work).removeRecursively();
        QDir().mkpath(work);

        const auto buildArchive = [&](const QString &archive,
                                      const QList<QPair<QString, QString>> &entries) {
            // 条目名/正文**只能**自己包单引号：`psQuoted()` 会顺手 toNativeSeparators，
            // 那会把条目名里的 `/` 全折成 `\` —— 造出来的就不再是我们要测的那个包了。
            const auto psq = [](const QString &text) {
                return QStringLiteral("'") + text + QStringLiteral("'");
            };
            QStringList lines;
            lines << QStringLiteral("$ErrorActionPreference='Stop'")
                  << QStringLiteral("Add-Type -AssemblyName System.IO.Compression.FileSystem")
                  << QStringLiteral("$z=[System.IO.Compression.ZipFile]::Open(%1,'Create')")
                         .arg(psQuoted(archive))
                  << QStringLiteral("try{");
            for (const QPair<QString, QString> &entry : entries) {
                if (entry.second.isEmpty()) {
                    // 空目录条目：名字以 `/` 结尾（zip 里合法，而且真有工具这么写）。
                    lines << QStringLiteral("  $z.CreateEntry(%1)").arg(psq(entry.first));
                } else {
                    lines << QStringLiteral("  $e=$z.CreateEntry(%1)").arg(psq(entry.first))
                          << QStringLiteral("  $s=$e.Open()")
                          << QStringLiteral("  $b=[System.Text.Encoding]::UTF8.GetBytes(%1)")
                                 .arg(psq(entry.second))
                          << QStringLiteral("  $s.Write($b,0,$b.Length)")
                          << QStringLiteral("  $s.Close()");
                }
            }
            lines << QStringLiteral("} finally { $z.Dispose() }");
            QProcess ps;
            ps.start(PowerShell::executable(),
                     {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                      QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                      QStringLiteral("-Command"), lines.join(QLatin1Char('\n'))});
            return ps.waitForFinished(180000) && ps.exitStatus() == QProcess::NormalExit &&
                   ps.exitCode() == 0;
        };

        const QString respack = work + "/respack.zip";
        check(buildArchive(respack,
                           {{QStringLiteral("pack.mcmeta"), QStringLiteral("{}")},
                            {QStringLiteral("assets/minecraft/lang/zh_cn.json"), QStringLiteral("{}")}}),
              "V5z 造出一个资源包（pack.mcmeta + assets/）");
        const QString datapack = work + "/datapack.zip";
        check(buildArchive(datapack,
                           {{QStringLiteral("pack.mcmeta"), QStringLiteral("{}")},
                            {QStringLiteral("data/minecraft/tags/function/a.json"), QStringLiteral("{}")}}),
              "V5z 造出一个数据包（pack.mcmeta + data/）");
        const QString shader = work + "/shader.zip";
        check(buildArchive(shader,
                           {{QStringLiteral("shaders/final.fsh"), QStringLiteral("void main(){}")},
                            {QStringLiteral("shaders/gbuffers_terrain.vsh"), QStringLiteral("void main(){}")}}),
              "V5z 造出一个光影包（shaders/）");
        // 同时有 assets/ 和 shaders/：资源包排在前面 ⇒ 必须是资源包。
        const QString both = work + "/respack-with-shader.zip";
        check(buildArchive(both,
                           {{QStringLiteral("pack.mcmeta"), QStringLiteral("{}")},
                            {QStringLiteral("assets/minecraft/lang/zh_cn.json"), QStringLiteral("{}")},
                            {QStringLiteral("shaders/final.fsh"), QStringLiteral("void main(){}")}}),
              "V5z 造出一个「资源包 + 里面还带 shaders/」的包（真实世界里的常客）");
        const QString nested = work + "/nested.zip";
        check(buildArchive(nested,
                           {{QStringLiteral("MyPack/pack.mcmeta"), QStringLiteral("{}")},
                            {QStringLiteral("MyPack/assets/minecraft/lang/zh_cn.json"),
                              QStringLiteral("{}")}}),
              "V5z 造出一个「整个目录一起压」的包（条目都套在 MyPack/ 底下）");
        const QString backslash = work + "/backslash.zip";
        check(buildArchive(backslash, {{QStringLiteral("assets\\minecraft\\x.txt"),
                                        QStringLiteral("反斜杠")}}),
              "V5z 造出一个条目名用反斜杠写的包（国内那些「一键压缩」常这么干）");
        const QString mrpack = work + "/mrpack.zip";
        check(buildArchive(mrpack,
                           {{QStringLiteral("modrinth.index.json"), QStringLiteral("{}")},
                            {QStringLiteral("overrides/mods/a.jar"), QStringLiteral("x")}}),
              "V5z 造出一个 Modrinth 整合包");
        const QString cfpack = work + "/cfpack.zip";
        check(buildArchive(cfpack,
                           {{QStringLiteral("manifest.json"), QStringLiteral("{}")},
                            {QStringLiteral("overrides/config/a.txt"), QStringLiteral("x")}}),
              "V5z 造出一个 CurseForge 整合包");
        const QString junk = work + "/junk.zip";
        check(buildArchive(junk, {{QStringLiteral("README.txt"), QStringLiteral("说明")}}),
              "V5z 造出一个「里面什么都不是」的压缩包");
        // 单模组打包成 .zip：**不是整合包**（导进去只会建一个空实例）。
        const QString modZip = work + "/single-mod.zip";
        check(buildArchive(modZip,
                           {{QStringLiteral("fabric.mod.json"), QStringLiteral("{}")},
                            {QStringLiteral("config/default.json"), QStringLiteral("{}")}}),
              "V5z 造出一个「单模组压成 .zip」的包");
        // 几个实例打在一个压缩包里：不猜，但要说清为什么。
        const QString multiZip = work + "/multi-instance.zip";
        check(buildArchive(multiZip,
                           {{QStringLiteral("实例A/.minecraft/config/a.json"), QStringLiteral("{}")},
                            {QStringLiteral("实例B/.minecraft/config/b.json"), QStringLiteral("{}")}}),
              "V5z 造出一个「几个实例打在一起」的包");

        const auto kindOf = [](const QString &path) {
            return DroppedResource::classify(path);
        };
        const auto isKind = [&](const QString &path, ResourceCatalog::Kind kind) {
            const DroppedResource::Result r = kindOf(path);
            return r.known && r.kind == kind;
        };

        check(isKind(respack, ResourceCatalog::Kind::ResourcePack),
              "V5z pack.mcmeta + assets/ ⇒ 资源包", kindOf(respack).detail);
        check(isKind(datapack, ResourceCatalog::Kind::DataPack),
              "V5z pack.mcmeta + data/ ⇒ 数据包", kindOf(datapack).detail);
        check(isKind(shader, ResourceCatalog::Kind::Shader),
              "V5z 只有 shaders/ ⇒ 光影", kindOf(shader).detail);
        // 优先级：这条要是红了，用户拖一个带 shaders/ 的资源包进来会进 shaderpacks/。
        check(isKind(both, ResourceCatalog::Kind::ResourcePack),
              "★★ V5z 同时有 assets/ 和 shaders/ ⇒ **资源包**（那一串判据的先后就是优先级，"
              "排错了就是往光影目录里塞资源包）",
              kindOf(both).detail);
        check(isKind(nested, ResourceCatalog::Kind::ResourcePack),
              "V5z 套在一层目录里的包也认得出来（GitHub 的 zipball / 右键压缩都是这个形状）",
              kindOf(nested).detail);
        check(isKind(mrpack, ResourceCatalog::Kind::Modpack),
              "V5z 根上有 modrinth.index.json ⇒ 整合包", kindOf(mrpack).detail);
        check(isKind(cfpack, ResourceCatalog::Kind::Modpack),
              "V5z 根上有 manifest.json ⇒ 整合包（CurseForge 那个格式）",
              kindOf(cfpack).detail);

        // `.jar` **一个字节都不读盘**：它是不是压缩包、里面有什么，都不影响判断
        // （模组就是模组）。这一条同时钉住"后缀大小写不敏感"。
        const QString jar = work + "/某某模组.JAR";
        writeFileAt(jar, "不是真的 jar，只是个名字");
        check(isKind(jar, ResourceCatalog::Kind::Mod),
              "V5z .jar 就是模组（后缀大小写不敏感、**不读盘**：这个文件根本不是压缩包）",
              kindOf(jar).detail);

        // 认不出来的那几档：反斜杠那个包、什么都不是的包、根本不是压缩包的文件、目录、不存在
        // 的路径。它们必须一律 `known == false` —— 界面上那一句"这几个认不出来"就是它。
        check(!kindOf(backslash).known,
              "V5z 条目名用反斜杠写的包：连 assets/ 都拼不出来 ⇒ 认不出来（如实说，不猜）",
              kindOf(backslash).detail);
        check(!kindOf(junk).known, "V5z 里面什么都没有的压缩包 ⇒ 认不出来",
              kindOf(junk).detail);
        check(!kindOf(modZip).known &&
                  kindOf(modZip).detail.contains(QStringLiteral("模组")),
              "★★ V5z 单模组压成 .zip ⇒ 认不出来，并说它是模组（不是当成整合包建个空实例）",
              kindOf(modZip).detail);
        check(!kindOf(multiZip).known &&
                  kindOf(multiZip).detail.contains(QStringLiteral("多个 .minecraft 目录")),
              "★★ V5z 几个实例打在一起 ⇒ 不导入，并说清是多个实例（不是一句「认不出来」）",
              kindOf(multiZip).detail);
        const QString notZip = work + "/其实是文本.zip";
        writeFileAt(notZip, "这不是压缩包");
        check(!kindOf(notZip).known,
              "V5z 后缀是 .zip 但不是压缩包 ⇒ 认不出来（不是「整合包」，也不是崩）",
              kindOf(notZip).detail);
        check(!kindOf(work).known, "V5z 拖进来的是个目录 ⇒ 认不出来", kindOf(work).detail);
        check(!kindOf(work + "/根本没有这个文件.zip").known,
              "V5z 盘上没有这个文件 ⇒ 认不出来", kindOf(work + "/根本没有这个文件.zip").detail);
        check(!kindOf(QString()).known, "V5z 空串 ⇒ 认不出来（不崩）");

        // `detail` 那一格是界面拿去写在括号里的"为什么"，认不出来时**必须有一句人话**
        // （空串的话那一行会变成「xxx（)」）。
        check(!kindOf(junk).detail.isEmpty() && !kindOf(notZip).detail.isEmpty(),
              "V5z 认不出来的时候带一句理由（界面上那个括号里就是它）",
              kindOf(notZip).detail);

        // 空目录条目那一档：`Compress-Archive` 会写进 `assets/` 这样以斜杠结尾的记录。
        // 只按 `assets/…` 认前缀的话，一个只有空目录的资源包会被判成"什么都没有"。
        const QString dirOnly = work + "/dir-only.zip";
        check(buildArchive(dirOnly,
                           {{QStringLiteral("pack.mcmeta"), QStringLiteral("{}")},
                            {QStringLiteral("assets/"), QString()}}),
              "V5z 造出一个「assets 只是一条空目录记录」的资源包");
        check(isKind(dirOnly, ResourceCatalog::Kind::ResourcePack),
              "V5z 目录条目本身（`assets/` 这一条）也算数 ⇒ 还是资源包",
              kindOf(dirOnly).detail);

        // `listEntries()` 就是上面那条路读条目表用的那一发：条目名**原样**拿回来
        // （不替我们归一化、不排序，谁用谁自己折），不是压缩包时和 readEntry 分得一样清。
        QStringList names;
        QString error;
        check(ZipUtil::listEntries(respack, &names, &error) == ZipUtil::EntryStatus::Ok &&
                  names.size() == 2 && names.contains(QStringLiteral("pack.mcmeta")) &&
                  names.contains(QStringLiteral("assets/minecraft/lang/zh_cn.json")),
              "V5z listEntries 把两个条目名原样拿回来了", names.join(QLatin1Char(' ')));
        check(ZipUtil::listEntries(backslash, &names, &error) == ZipUtil::EntryStatus::Ok &&
                  names.contains(QStringLiteral("assets\\minecraft\\x.txt")),
              "V5z listEntries **不替我们归一化**反斜杠（折成 `/` 是调用方的事）",
              names.join(QLatin1Char(' ')));
        check(ZipUtil::listEntries(notZip, &names, &error) == ZipUtil::EntryStatus::NotAnArchive,
              "V5z listEntries 对不是压缩包的文件报 NotAnArchive");
        // 盘上根本没有这一份：**和 readEntry 给同一个答案**（打不开 = NotAnArchive，不是
        // NotFound）。`NotFound` 在这个枚举里的意思是"**压缩包里**没有这一条"，一条都还没
        // 读到的时候谈不上"没有哪一条"；两处答得不一样的话，界面同一个故障会说出两句
        // 不同的话。上面 V5z 那一条"盘上没有这个文件 ⇒ 认不出来"是**判据层**自己先挡掉的
        // （它的 detail 是「找不到这份文件」）—— listEntries 根本轮不到。
        QByteArray oneEntry;
        check(ZipUtil::listEntries(work + "/没有这个.zip", &names, &error) ==
                      ZipUtil::EntryStatus::NotAnArchive &&
                  ZipUtil::readEntry(work + "/没有这个.zip", QStringLiteral("a.txt"), &oneEntry,
                                     &error) == ZipUtil::EntryStatus::NotAnArchive,
              "V5z listEntries 和不存在的文件对同一份读不了的东西给同一个答案"
              "（NotAnArchive，和 readEntry 一致）",
              error);
        // 空表也要能读（一个成员都没有的 zip 是合法的）。
        const QString emptyZip = work + "/empty.zip";
        check(buildArchive(emptyZip, {}), "V5z 造得出一个空压缩包");
        names = QStringList{QStringLiteral("上一次留下的")};
        check(ZipUtil::listEntries(emptyZip, &names, &error) == ZipUtil::EntryStatus::Ok &&
                  names.isEmpty(),
              "V5z 空压缩包 ⇒ 空表（而且把 out 里上一次留下的东西清掉了）");

        QDir(work).removeRecursively();
    }

    // --- V5c. extractArchives：那几条恶意条目名一个都不许解出去（zip slip） ------
    //
    // 这一节盯的是 LauncherManager::prepareNatives() 那条路（旧格式的 natives 是从
    // 官方 jar 里解出来的，用的是 ZipUtil::extractArchives）。守卫必须同时判"条目名里
    // 有没有 `..`"和"条目名本身就是绝对路径"—— .NET 的 Path.Combine 会把目标目录整个
    // 丢掉，文件落到外面去。
    {
        const QString work = sandbox + "/zip-slip";
        QDir(work).removeRecursively();
        QDir().mkpath(work);  // ZipFile::Open('Create') 不会自己建父目录
        const QString archive = work + "/evil.zip";
        // 带盘符的绝对路径那条：Path.Combine 遇到它会丢掉 $out，于是落点就是它自己。
        // 落点**故意挑在沙盒里** —— 防线漏了也只是在沙盒里多出一个文件，碰不到系统目录。
        const QString escapeTarget =
            QDir::fromNativeSeparators(QDir(work).absolutePath()) + QStringLiteral("/escaped.txt");
        // `/abs.txt` 这类"只有斜杠开头"的条目，落点是**当前盘的根目录**（.NET 拿它当
        // 相对当前盘的绝对路径）。记一下它本来在不在：这条只该在"本来没有、跑完出现了"
        // 时红，不能因为盘根目录本来就有个同名文件而误报。
        const QString driveRootEscape = QDir::rootPath() + QStringLiteral("abs.txt");
        const bool driveRootEscapeExisted = QFile::exists(driveRootEscape);

        const QString script =
            QStringLiteral("$ErrorActionPreference='Stop'\n"
                           "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                           "$zip=[System.IO.Compression.ZipFile]::Open(%1,'Create')\n"
                           "try{\n"
                           "  $items=@(\n"
                           "    @{n='ok.txt';c='正常条目'},\n"
                           "    @{n='sub\\ok2.txt';c='反斜杠条目'},\n"
                           "    @{n='../evil.txt';c='翻上去'},\n"
                           "    @{n='/abs.txt';c='斜杠开头的绝对路径'},\n"
                           "    @{n=%2;c='带盘符的绝对路径'},\n"
                           "    @{n='META-INF/MANIFEST.MF';c='签名文件'}\n"
                           "  )\n"
                           "  foreach($it in $items){\n"
                           "    $e=$zip.CreateEntry($it.n)\n"
                           "    $w=New-Object System.IO.StreamWriter($e.Open())\n"
                           "    $w.Write($it.c)\n"
                           "    $w.Dispose()\n"
                           "  }\n"
                           "} finally { $zip.Dispose() }\n")
                .arg(psQuoted(archive),
                     QStringLiteral("'") + escapeTarget + QStringLiteral("'"));
        QProcess ps;
        ps.start(PowerShell::executable(),
                 {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                  QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                  QStringLiteral("-Command"), script});
        const bool madeZip = ps.waitForFinished(180000) &&
                             ps.exitStatus() == QProcess::NormalExit && ps.exitCode() == 0;
        check(madeZip, "V5c 造出一个带恶意条目名的压缩包（ZipArchive::Create 才造得出）",
              QString::fromLocal8Bit(ps.readAllStandardError()).trimmed().left(200));
        if (!madeZip) {
            QDir(work).removeRecursively();
            return;
        }

        const QString out = work + "/out";
        QString error;
        const bool ok = ZipUtil::extractArchives({archive}, out,
                                                  {QStringLiteral("META-INF")}, &error);
        check(ok, "V5c 解压整体算成功（恶意条目是被跳过的，不是整包失败）", error);

        const auto readTextAt = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        // 正路照样得通：正常的条目要解出来，反斜杠的条目名归一之后也落在目标目录里
        // （Minecraft 那些 jar 是 `/`，但别的工具打的包不一定讲究）。
        check(readTextAt(out + "/ok.txt") == QStringLiteral("正常条目") &&
                  readTextAt(out + "/sub/ok2.txt") == QStringLiteral("反斜杠条目"),
              "V5c 正常条目照解不误，反斜杠条目名归一成 / 之后落在目标目录里");
        // 四条恶意条目：一个都不许出现在目标目录外面。
        check(!QFile::exists(work + "/evil.txt"),
              "V5c `../evil.txt` 没有翻到目标目录上面去");
        check(driveRootEscapeExisted || !QFile::exists(driveRootEscape),
              "V5c `/abs.txt` 没有落到当前盘的根目录下（Combine 会丢掉目标目录）",
              driveRootEscape);
        check(!QFile::exists(escapeTarget),
              "V5c 带盘符的绝对路径条目没有写到外面去", escapeTarget);
        check(!QFile::exists(out + "/META-INF/MANIFEST.MF"),
              "V5c excludePrefixes 那条路照样挡着（META-INF 没进目标目录）");

        QDir(work).removeRecursively();
    }

    runModpackExportFormatTests(sandbox);

    runModpackExportLookupTests(sandbox);

    runModpackExportPipelineTests(sandbox);

    runThemeImageTests(sandbox);

    // --- V6. 真清单（779 MB 那个 ATT2 的 modrinth.index.json） -----------------
    {
        const QString metaDir = QStringLiteral(REPO_ROOT_PATH) + "/tests/meta";
        QFile fixture(metaDir + "/modrinth-index-att2.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "V6 读到了真清单 fixture（meta/modrinth-index-att2.json）");
        if (!haveFixture) {
            return;
        }
        const QByteArray json = fixture.readAll();
        fixture.close();

        const ModpackImport::Result r = ModpackImport::parseModrinthIndex(json);
        check(r.ok, "V6 真的那一份清单解析通过", r.problem);
        if (!r.ok) {
            return;
        }
        const ModpackImport::Pack &pack = r.pack;
        check(pack.gameVersion == QStringLiteral("1.20.2") &&
                  pack.loaderKey == QStringLiteral("fabric-loader") &&
                  pack.loaderVersion == QStringLiteral("0.16.14"),
              "V6 依赖表读对了：1.20.2 + fabric-loader 0.16.14",
              QStringLiteral("%1 / %2 %3").arg(pack.gameVersion, pack.loaderKey,
                                               pack.loaderVersion));
        check(pack.files.size() >= 100 && pack.filesSkipped == 0,
              "V6 一百多条文件全在，一条都没被误判成该跳过",
              QStringLiteral("留下 %1、跳过 %2").arg(pack.files.size()).arg(pack.filesSkipped));

        // 这一条是这一节里最值钱的：真实的 150 条路径，逐条过判据。判据要是严错了一点
        // （比如把"名字里带点"当非法），真包会被整包拒绝 —— 而那个症状是"用户选了自己
        // 的整合包，启动器说这个包的文件清单不安全"。
        int unsafe = 0;
        QString firstUnsafe;
        int longest = 0;
        for (const ModpackImport::PackFile &file : pack.files) {
            if (!ModpackImport::isSafeRelativePath(file.path)) {
                ++unsafe;
                if (firstUnsafe.isEmpty()) {
                    firstUnsafe = file.path;
                }
            }
            if (file.path.size() > longest) {
                longest = static_cast<int>(file.path.size());
            }
        }
        check(unsafe == 0, "V6 真清单里 150 条路径一条都没被判成不安全",
              unsafe == 0 ? QStringLiteral("最长的 %1 个字符").arg(longest)
                          : QStringLiteral("%1 条，第一条是 %2").arg(unsafe).arg(firstUnsafe));

        // 界面预填的那个名字（包名推出来的）必须过得了数据层的校验，否则用户一进
        // 输入框就撞一句报错，而他什么都没输。
        const QString suggested = ModpackImport::suggestInstanceName(pack.name);
        check(InstanceFiles::validateRenameTarget(suggested, QString()).isEmpty(),
              "V6 从真包名推出来的实例名过得了校验",
              QStringLiteral("「%1」→「%2」").arg(pack.name, suggested));
    }
}



