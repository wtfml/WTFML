#include "core_modules/Support.hpp"
#include "core_modules/LongPathTests.hpp"
#include "support/SettingsIsolation.hpp"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("WTFML");
    app.setOrganizationName("WTFML");
    // ZJ0 的目录锁用例会把**自己**再起一份来抢锁，兼容入口也得认那个参数（见 Support.hpp）。
    if (const int probe = runDirectoryLockProbeIfRequested(app.arguments()); probe >= 0) {
        return probe;
    }
    if (app.arguments().contains(QStringLiteral("--bootstrap-check"))) {
        out() << "PASS compatibility driver links and starts\n";
        return 0;
    }

    QTemporaryDir processSandbox;
    if (!processSandbox.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, processSandbox.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, processSandbox.filePath("system-settings"));
    requireIsolatedSettings(processSandbox.path(), "verify");
    app.setProperty("wtfml.testDefaultMinecraftDir", processSandbox.filePath("default-game"));
    const QString repoRoot = QStringLiteral(REPO_ROOT_PATH);
    const QString metaDir = repoRoot + "/tests/meta";
    // Per-process legacy driver fixture; never delete a shared build-local game tree.
    const QString sandbox = processSandbox.filePath(".minecraft");

    // libs 模式：只打印生产代码 extractLibraries 选中了哪些 library（不下载）。
    // 用来验证平台 rules 过滤是否真的生效。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("libs")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const QJsonObject json = loadJson(metaDir + "/" + versionId + ".json");
        if (json.isEmpty()) {
            out() << "no meta for " << versionId << "\n";
            return 2;
        }
        const QJsonArray all = json[QStringLiteral("libraries")].toArray();
        DownloadManager mgr;
        const QList<Library> picked = mgr.extractLibraries(json);

        int allArtifacts = 0;
        for (const QJsonValue &v : all) {
            if (!v.toObject()[QStringLiteral("downloads")].toObject()
                     [QStringLiteral("artifact")]
                     .toObject()[QStringLiteral("path")]
                     .toString()
                     .isEmpty()) {
                ++allArtifacts;
            }
        }

        out() << "platformName=" << LibraryRules::platformName()
              << " platformArch=" << LibraryRules::platformArch() << "\n";
        out() << "version " << versionId << ": 条目 " << all.size() << "，带 artifact 的 "
              << allArtifacts << "，extractLibraries 选中 " << picked.size() << "\n";
        int foreign = 0;
        for (const Library &lib : picked) {
            const bool isForeign = lib.path.contains(QStringLiteral("natives-linux")) ||
                                   lib.path.contains(QStringLiteral("natives-macos")) ||
                                   lib.path.contains(QStringLiteral("linux-")) ||
                                   lib.path.contains(QStringLiteral("osx-"));
            if (isForeign) {
                ++foreign;
                out() << "  [外来平台!] " << lib.path << "\n";
            }
        }
        out() << "其中非 Windows 平台条目: " << foreign << "\n";
        out().flush();
        return 0;
    }

    // scan 模式：量一次「切到设置页要冻多久」。
    // MainWindow::refreshJavaList() 是**同步**跑的，scanLocalJavaVersions() 花多少毫秒，
    // 界面就冻多少毫秒 —— 这个数就是"点开设置卡一下"的那个"一下"。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("scan")) {
        GlobalJavaSettings settings;
        QElapsedTimer scanTimer;
        scanTimer.start();
        const QMap<int, QString> versions = settings.scanLocalJavaVersions();
        const qint64 elapsed = scanTimer.elapsed();

        out() << "scanLocalJavaVersions(): " << elapsed << " ms  <- 界面就冻这么久\n";
        for (auto it = versions.cbegin(); it != versions.cend(); ++it) {
            out() << "  Java " << it.key() << "  ->  " << it.value() << "\n";
        }
        out() << "共认出来 " << versions.size() << " 个运行时\n";
        out().flush();
        return 0;
    }

    // pipeline 模式：下载批次记账的全部断言，全离线（file:// 当远端）。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("pipeline")) {

#ifndef WTFML_CORE_BASELINE
return runPipelineTests(app, sandbox);
#endif
    }

    // vm-batch 模式：VersionManager::downloadVersion 的批次编排，同样离线。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("vm-batch")) {

#ifndef WTFML_CORE_BASELINE
return runVersionManagerBatch(app, sandbox);
#endif
    }

    // loader-install 模式：**联网**真装一次带加载器的版本到沙箱里，装完连"能不能启动"
    // （启动前检查 / classpath）一起验。见这个函数上面那段说明。
    // 第三个参数是加载器（缺省 Fabric），认不出来就拒绝跑 —— 猜一个装下去是给用户装错东西。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("loader-install")) {
        VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
        if (argc >= 4) {
            const QString word = QString::fromLocal8Bit(argv[3]).toLower();
            if (word == QLatin1String("fabric")) {
                kind = VersionManager::LoaderKind::Fabric;
            } else if (word == QLatin1String("forge")) {
                kind = VersionManager::LoaderKind::Forge;
            } else if (word == QLatin1String("neoforge")) {
                kind = VersionManager::LoaderKind::NeoForge;
            } else if (word == QLatin1String("optifine")) {
                kind = VersionManager::LoaderKind::OptiFine;
            } else if (word == QLatin1String("quilt")) {
                kind = VersionManager::LoaderKind::Quilt;
            } else {
                out() << "不认识的加载器「" << word
                      << "」。用法：loader-install <游戏版本> [fabric|forge|neoforge|optifine|quilt]\n";
                out().flush();
                return 2;
            }
        }
        // 第四个词（可省）是 `mirror`：把这一趟拨到镜像源上跑。
        bool useMirror = false;
        if (argc >= 5) {
            const QString word = QString::fromLocal8Bit(argv[4]).toLower();
            if (word == QLatin1String("mirror")) {
                useMirror = true;
            } else {
                out() << "不认识的第四个参数「" << word
                      << "」。用法：loader-install <游戏版本> [fabric|forge|neoforge|optifine] "
                         "[mirror]\n";
                out().flush();
                return 2;
            }
        }

#ifndef WTFML_CORE_BASELINE
return runLoaderInstallLive(app, sandbox, QString::fromLocal8Bit(argv[2]), kind,
                                    useMirror);
#endif
    }

    // names 模式：版本显示名的纯函数真值表 + displayParts() 读盘 + LauncherSettings
    // + 已安装版本扫描的告警 + 内存建议值。
    // modpack-read 模式：拿一个**真整合包**走一遍导入的前半段，一个字节都不下。
    // verify.exe modpack-read "<整合包.zip>"
    // 检的是两件事：ZipUtil 从真压缩包（几百 MB、条目名大小写不一定对得上）里读单条目
    // 这条路，以及真清单的解析结果。这两步是**全离线**能验到的最远处 —— 再往下就要下
    // 加载器和那 150 个模组了（那条路在联网时用产品界面走）。
    //
    // 退出码：0 = 读出来、解析通过、每一条路径都过判据；否则 1（并把问题打在 stdout）。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("modpack-read")) {
        const QString zipPath = QString::fromLocal8Bit(argv[2]);
        QByteArray indexJson;
        QString error;
        const ZipUtil::EntryStatus status =
            ZipUtil::readEntry(zipPath, ModpackImport::modrinthIndexName(), &indexJson, &error);
        out() << "zip=" << zipPath << "\n"
              << "status=" << static_cast<int>(status) << "\n"
              << "bytes=" << indexJson.size() << "\n"
              << "error=" << error << "\n";
        // 没读到 Modrinth 清单时，顺手看一眼它是不是 CurseForge 格式 —— 产品界面正是这么
        // 分这两种不是整合包的（见 MainWindow::onImportModpackClicked：再读一次根目录的
        // manifest.json）。探针照抄同一套判据，好让手里这个包到底属于哪种有一个离线答案：
        // 用户问的"拿这些包导入看看"里，那个 CurseForge 包就卡在这一句上。
        if (status != ZipUtil::EntryStatus::Ok) {
            QByteArray manifest;
            const bool curseForge =
                ZipUtil::readEntry(zipPath, ModpackImport::curseForgeManifestName(), &manifest,
                                   nullptr) == ZipUtil::EntryStatus::Ok;
            out() << "format=" << (curseForge ? "curseforge" : "unknown") << "\n"
                  << "problem="
                  << ModpackImport::unsupportedFormatText(
                         curseForge ? ModpackImport::Format::CurseForge
                                    : ModpackImport::Format::Unknown)
                  << "\n";
            out().flush();
            return 3;
        }
        const ModpackImport::Result result = ModpackImport::parseModrinthIndex(indexJson);
        out() << "ok=" << (result.ok ? 1 : 0) << "\n";
        if (!result.ok) {
            out() << "problem=" << result.problem << "\n";
            out().flush();
            return 1;
        }
        const ModpackImport::Pack &pack = result.pack;
        out() << "name=" << pack.name << "\n"
              << "version=" << pack.version << "\n"
              << "game=" << pack.gameVersion << "\n"
              << "loader=" << pack.loaderKey << " " << pack.loaderVersion << "\n"
              << "files=" << pack.files.size() << " skipped=" << pack.filesSkipped << "\n"
              << "instanceName=" << ModpackImport::suggestInstanceName(pack.name) << "\n"
              << "loaderProblem="
              << ModpackImport::unsupportedLoaderText(pack.loaderKey, pack.loaderVersion)
              << "\n";
        // 每一条路径都过一遍判据。一条误判的代价是**整包被拒**（判据是有意设计成
        // "整包拒绝"的），所以真包的这 150 条路径一条都不能报不安全。
        int unsafe = 0;
        qint64 totalBytes = 0;
        QString longest;
        for (const ModpackImport::PackFile &file : pack.files) {
            if (!ModpackImport::isSafeRelativePath(file.path)) {
                ++unsafe;
                out() << "unsafe: " << file.path << "\n";
            }
            if (file.size > 0) {
                totalBytes += file.size;
            }
            if (file.path.length() > longest.length()) {
                longest = file.path;
            }
        }
        out() << "unsafePaths=" << unsafe << "\n"
              << "totalBytes=" << totalBytes << "\n"
              << "longestPath=" << longest << " (" << longest.length() << " 字符)\n";
        out().flush();
        return unsafe == 0 ? 0 : 1;
    }

    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("names")) {

#ifndef WTFML_CORE_BASELINE
runVersionNameTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstanceFileTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstalledScanTests(sandbox);
#endif
        // 排在扫描之后：这一段会往 versions/ 下建一个目录再删掉，放在扫描前面
        // 万一中途崩了，扫描那几条"沙盒里有几个版本"的断言会跟着一起红，查起来绕。

#ifndef WTFML_CORE_BASELINE
runVersionMetaWriteTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstallOccupancyTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runRenameInstanceTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstanceGroupTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runVersionJvmArgsTests(sandbox);
#endif
        // 排在这些"要数沙箱里有几个版本"的用例之后，而且它自己会收摊 ——
        // 见这一节末尾那几条清理断言。

#ifndef WTFML_CORE_BASELINE
runLoaderInheritanceTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runLoaderInstallTests();
#endif

#ifndef WTFML_CORE_BASELINE
runMemorySuggestionTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runEffectiveJavaSettingsTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runJavaReleaseParseTests();
#endif
        out() << "\n===== " << (g_failures == 0 ? "全部通过" : "有失败项") << ": "
              << (g_checks - g_failures) << "/" << g_checks << " =====\n";
        out().flush();
        return g_failures == 0 ? 0 : 1;
    }

    // vm-fetch-fail 模式：元数据死路必须有终态信号（防止 UI 永久禁用）。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("vm-fetch-fail")) {

#ifndef WTFML_CORE_BASELINE
return runVersionManagerFetchFail(app);
#endif
    }

    // fetch 模式：用生产下载代码把一个版本拉全（客户端 jar + 库 + natives classifier）。
    // 不再自带批次（beginBatch/endBatch），用来验证"不带批次"的用法也发得出完成信号。
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("fetch")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const QString mcRoot = QString::fromLocal8Bit(argv[3]);
        const QJsonObject json = loadJson(metaDir + "/" + versionId + ".json");
        if (json.isEmpty()) {
            out() << "no meta for " << versionId << "\n";
            return 2;
        }

        QDir().mkpath(mcRoot + "/versions/" + versionId);
        QFile jf(mcRoot + "/versions/" + versionId + "/" + versionId + ".json");
        if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            jf.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
            jf.close();
        }

        DownloadManager mgr;
        int done = 0;
        int failed = 0;
        bool finished = false;
        bool signalSuccess = false;
        QObject::connect(&mgr, &DownloadManager::downloadFinished, &app,
                         [&](const QString &name, bool ok, const QString &err) {
                             ++done;
                             if (!ok) {
                                 ++failed;
                                 out() << "  FAIL " << name << "  " << err << "\n";
                                 out().flush();
                             }
                         });
        QEventLoop loop;
        QObject::connect(&mgr, &DownloadManager::allDownloadsFinished, &app,
                         [&](bool ok) {
                             finished = true;
                             signalSuccess = ok;
                             loop.quit();
                         });

        const QJsonObject client = json[QStringLiteral("downloads")].toObject()
                                       [QStringLiteral("client")].toObject();
        mgr.downloadClientJar(versionId, client[QStringLiteral("url")].toString(),
                              mcRoot + "/versions/" + versionId + "/" + versionId + ".jar",
                              client[QStringLiteral("sha1")].toString(),
                              client[QStringLiteral("size")].toVariant().toLongLong());

        const QList<Library> libs = mgr.extractLibraries(json);
        int natives = 0;
        for (const Library &l : libs) {
            if (l.isNative) {
                ++natives;
            }
        }
        out() << "开始下载 " << libs.size() << " 个库（其中 natives classifier " << natives
              << " 个）\n";
        out().flush();
        mgr.downloadLibraries(libs, mcRoot + "/libraries");

        // 信号可能在 exec() 之前就发出来（全部跳过时），必须先看标志
        if (!finished) {
            loop.exec();
        }
        out() << "完成 " << done << " 项，失败 " << failed << " 项"
              << "（allDownloadsFinished success=" << signalSuccess << "）\n";
        out().flush();
        return failed == 0 ? 0 : 1;
    }

    // launch 模式：完整复刻 MainWindow::onStartButtonClicked 的顺序，一路走到
    // LauncherManager::startGame，也就是真正拉起游戏进程。
    // 用法：verify launch <versionId> <major> [timeoutMs]
    // 必须放在目标 .minecraft 的上一级目录里跑（startGame 用 applicationDirPath）。
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("launch")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const int major = QString::fromLocal8Bit(argv[3]).toInt();
        const int timeoutMs = argc >= 5 ? QString::fromLocal8Bit(argv[4]).toInt() : 90000;
        const QJsonObject json = loadJson(metaDir + "/" + versionId + ".json");
        if (json.isEmpty()) {
            return 2;
        }

        GlobalJavaSettings settings;
        settings.load();
        const QString originalPath = settings.javaPath();
        LauncherManager mgr;
        QObject::connect(&mgr, &LauncherManager::processOutput, &app,
                         [](int, const QString &t) { out() << "  [game] " << t << "\n"; out().flush(); });
        QObject::connect(&mgr, &LauncherManager::processError, &app,
                         [](int, const QString &t) { out() << "  [game!] " << t << "\n"; out().flush(); });

        QEventLoop loop;
        QObject::connect(&mgr, &LauncherManager::gameFinished, &loop,
                         [&](int, int code, QProcess::ExitStatus st) {
                             out() << "游戏退出: code=" << code
                                   << " status=" << (st == QProcess::NormalExit ? "Normal" : "Crash")
                                   << "\n";
                             loop.quit();
                         });

        QStringList errors;
        const QString nativesDir = mgr.prepareNatives(json, versionId, &errors);
        out() << "prepareNatives -> "
              << (nativesDir.isEmpty() ? QStringLiteral("<无 natives 需求>") : nativesDir) << "\n";
        for (const QString &e : errors) {
            out() << "  natives error: " << e << "\n";
        }
        if (nativesDir.isEmpty() && !errors.isEmpty()) {
            return 2;
        }

        if (!mgr.checkPreLaunch(versionId, &errors, json)) {
            out() << "checkPreLaunch 失败: " << errors.join(QStringLiteral("; ")) << "\n";
            return 2;
        }

        const QString component =
            json[QStringLiteral("javaVersion")].toObject()[QStringLiteral("component")].toString();
        if (!settings.ensureJavaForVersion(major, component)) {
            out() << "找不到 Java " << major << "\n";
            return 2;
        }
        out() << "java = " << settings.javaPath() << "\n";
        out().flush();

        const QString assetIndex = json[QStringLiteral("assetIndex")].toObject()["id"].toString();
        const QStringList args =
            mgr.generateJvmArguments(QString(), versionId, assetIndex, json, settings, true, major);

        QTimer::singleShot(timeoutMs, &loop, [&]() {
            out() << "超时 " << timeoutMs << "ms，结束等待\n";
            loop.quit();
        });
        out() << "startGame -> session " << mgr.startGame(args, settings, versionId,
                                                          versionId, QStringLiteral("verify-probe"))
              << "\n";
        out().flush();
        loop.exec();

        settings.setJavaPath(originalPath);
        settings.save();
        return 0;
    }

    // assets 模式：跑生产代码的 downloadAssets，把缺失的资源补齐。
    // 注意 parseAndDownloadAssets 用的是 applicationDirPath()，所以这个 exe
    // 必须放在目标 .minecraft 的上一级目录里跑。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("assets")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const QJsonObject json = loadJson(metaDir + "/" + versionId + ".json");
        if (json.isEmpty()) {
            return 2;
        }
        DownloadManager mgr;
        int done = 0;
        int failed = 0;
        bool finished = false;
        QObject::connect(&mgr, &DownloadManager::downloadFinished, &app,
                         [&](const QString &name, bool ok, const QString &err) {
                             ++done;
                             if (!ok) {
                                 ++failed;
                                 out() << "  FAIL " << name << "  " << err << "\n";
                                 out().flush();
                             }
                         });
        QEventLoop loop;
        QObject::connect(&mgr, &DownloadManager::allDownloadsFinished, &app,
                         [&](bool) {
                             finished = true;
                             loop.quit();
                         });

        const AssetIndex idx = mgr.extractAssetIndex(json);
        out() << "开始补齐 assets index " << idx.id << "\n";
        out().flush();
        mgr.downloadAssets(idx);
        // 对象全都在盘上时，整批会在 exec() 之前就结束
        if (!finished) {
            loop.exec();
        }
        out() << "完成 " << done << " 项，失败 " << failed << " 项\n";
        out().flush();
        return failed == 0 ? 0 : 1;
    }

    // dump 模式：把 generateJvmArguments 的结果逐行写入文件，供 shell 用真实
    // 参数启动游戏（验证 Java 25 那一条真正跑得起来）。
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("dump")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const int major = QString::fromLocal8Bit(argv[3]).toInt();
        const QJsonObject json = loadJson(metaDir + "/" + versionId + ".json");
        if (json.isEmpty()) {
            out() << "no meta for " << versionId << "\n";
            return 2;
        }
        GlobalJavaSettings s;
        s.load();
        const QString originalPath = s.javaPath();
        // 和 MainWindow 一样先解析出该版本需要的 Java，再生成参数
        const QString component =
            json[QStringLiteral("javaVersion")].toObject()[QStringLiteral("component")].toString();
        if (!s.ensureJavaForVersion(major, component)) {
            out() << "no java for major " << major << "\n";
            return 2;
        }
        LauncherManager mgr;
        // 复刻 MainWindow 的顺序：先备好 natives，再生成参数
        QStringList nativeErrors;
        const QString nativesDir = mgr.prepareNatives(json, versionId, &nativeErrors);
        out() << "natives=" << (nativesDir.isEmpty() ? QStringLiteral("<无需/none>") : nativesDir)
              << "\n";
        for (const QString &e : nativeErrors) {
            out() << "natives error: " << e << "\n";
        }
        const QString assetIndex = json[QStringLiteral("assetIndex")].toObject()["id"].toString();
        const QStringList args = mgr.generateJvmArguments(QString(), versionId, assetIndex, json, s,
                                                          true, major);
        QFile f(QString::fromLocal8Bit(argv[4]));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return 2;
        }
        for (const QString &a : args) {
            f.write(a.toUtf8());
            f.write("\n");
        }
        // 把选中的 java 写到 stdout 供 shell 取用
        out() << "wrote " << args.size() << " args to " << f.fileName() << "\n";
        out() << "java=" << s.javaPath() << "\n";
        out().flush();
        // 测试不该改动机器上的设置
        s.setJavaPath(originalPath);
        s.save();
        return 0;
    }

    // dumpmerged 模式：和 dump 一样把参数写到文件，但版本 json 用的是**继承合并过**的那份
    // （从沙箱 build/.minecraft 里真装好的版本目录读）。
    //
    // 为什么不能直接用 meta/ 里那份：那份是加载器版本（或原版）自己写的原样 json，加载器
    // 那份里**没有原版的库**（log4j、joml 之类全在父那份），照它拼出来的 classpath 连
    // LogManager 都找不到 —— 那样起不来，也就验不出"游戏真的能启动"。
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("dumpmerged")) {
        const QString versionId = QString::fromLocal8Bit(argv[2]);
        const int major = QString::fromLocal8Bit(argv[3]).toInt();
        const QString sandbox = repoRoot + "/build/.minecraft";
        AppPaths::setMinecraftDir(sandbox);
        VersionManager vm;
        const QJsonObject json = vm.getVersionJson(versionId);
        if (json.isEmpty()) {
            out() << "no version " << versionId << " under " << sandbox << "\n";
            return 2;
        }
        GlobalJavaSettings s;
        s.load();
        const QString originalPath = s.javaPath();
        const QString component =
            json[QStringLiteral("javaVersion")].toObject()[QStringLiteral("component")].toString();
        if (!s.ensureJavaForVersion(major, component)) {
            out() << "no java for major " << major << "\n";
            return 2;
        }
        LauncherManager mgr;
        QStringList nativeErrors;
        const QString nativesDir = mgr.prepareNatives(json, versionId, &nativeErrors);
        out() << "natives=" << (nativesDir.isEmpty() ? QStringLiteral("<none>") : nativesDir)
              << "\n";
        const QString assetIndex = json[QStringLiteral("assetIndex")].toObject()["id"].toString();
        const QStringList args = mgr.generateJvmArguments(QString(), versionId, assetIndex, json, s,
                                                          true, major);
        QFile f(QString::fromLocal8Bit(argv[4]));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return 2;
        }
        for (const QString &a : args) {
            f.write(a.toUtf8());
            f.write("\n");
        }
        out() << "wrote " << args.size() << " args to " << f.fileName() << "\n";
        out() << "java=" << s.javaPath() << "\n";
        out().flush();
        s.setJavaPath(originalPath);
        s.save();
        return 0;
    }

    const QStringList versions = {"26.3", "26.1", "1.19", "1.18.2", "1.12.2"};
    // A verification run must not inherit files left by an earlier run. The
    // old build-local harness happened to start with a hand-maintained
    // .minecraft tree; CTest should be reproducible from a clean checkout.
    QDir(sandbox).removeRecursively();
    QDir().mkpath(sandbox);
    AppPaths::setMinecraftDir(sandbox);

    QMap<QString, QJsonObject> jsons;
    for (const QString &v : versions) {
        jsons[v] = loadJson(metaDir + "/" + v + ".json");
        if (jsons[v].isEmpty()) {
            out() << "FATAL: 缺少元数据 " << metaDir << "/" << v << ".json\n";
            return 2;
        }
    }

    DownloadManager dm;
    LauncherManager launcher;

    section("A. natives classifier 选择（DownloadManager::extractLibraries）");
    QMap<QString, QList<Library>> nativesOf;
    for (const QString &v : versions) {
        const QList<Library> libs = dm.extractLibraries(jsons[v]);
        QList<Library> natives;
        QList<Library> artifacts;
        bool ghost = false;
        for (const Library &lib : libs) {
            if (lib.isNative) {
                natives << lib;
            } else {
                artifacts << lib;
            }
            if (lib.url.isEmpty() || lib.path.isEmpty()) {
                ghost = true;
            }
        }
        nativesOf[v] = natives;

        int libCount = jsons[v][QStringLiteral("libraries")].toArray().size();
        out() << "\n  " << v << ": libraries=" << libCount << " artifact=" << artifacts.size()
              << " natives=" << natives.size() << "\n";
        for (const Library &n : natives) {
            out() << "        " << n.path << "\n";
        }
        check(!ghost, QString("%1: 没有空 url/path 的幽灵下载条目").arg(v));
    }
    out() << "\n";
    check(nativesOf["1.18.2"].size() == 8, "1.18.2 选出 8 个 natives classifier",
          QString::number(nativesOf["1.18.2"].size()));
    // 1.12.2 共 5 个库带 natives 字段，其中：
    // lwjgl-platform:2.9.2 的 rules 只 allow osx -> 排除
    // java-objc-bridge 只有 osx 的 natives key -> 无 windows classifier
    // 剩下 3 个（lwjgl-platform:2.9.4 / jinput-platform / text2speech）
    check(nativesOf["1.12.2"].size() == 3, "1.12.2 选出 3 个 natives classifier",
          QString::number(nativesOf["1.12.2"].size()));
    check(nativesOf["1.19"].isEmpty(), "1.19 不需要 classifier（新格式）");
    check(nativesOf["26.1"].isEmpty(), "26.1 不需要 classifier（新格式）");

    bool has292 = false;
    for (const Library &n : nativesOf["1.12.2"]) {
        if (n.path.contains(QStringLiteral("2.9.2"))) {
            has292 = true;
        }
    }
    check(!has292, "1.12.2 的 rules 过滤生效（没有 2.9.2 的 DLL）");

    section("B. 1.19+ classpath 零回归（对比基准算法）");
    for (const QString &v : {"1.19", "26.1"}) {
        GlobalJavaSettings s;
        const QStringList args =
            launcher.generateJvmArguments(QString(), v, QStringLiteral("1"), jsons[v], s, true, 21);
        const int cpIndex = args.indexOf(QStringLiteral("-cp"));
        const QStringList actual =
            cpIndex >= 0 ? args.at(cpIndex + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts)
                         : QStringList();
        const QStringList expected = legacy::classpath(jsons[v], v);

        check(actual == expected, QString("%1: classpath 与基准逐项一致").arg(v),
              QString("new=%1 old=%2").arg(actual.size()).arg(expected.size()));
        if (actual != expected) {
            const QSet<QString> a(actual.begin(), actual.end());
            const QSet<QString> e(expected.begin(), expected.end());
            for (const QString &only : a - e) {
                out() << "        仅新代码: " << only << "\n";
            }
            for (const QString &only : e - a) {
                out() << "        仅旧代码: " << only << "\n";
            }
        }

        // 按 version.json 原样传递：1.19 和 26.1 都声明了这条属性，值就是该版本的
        // natives 目录。断言"有且只有一条，且是存在的绝对路径" —— 这比"不许出现"更贴近
        // 原版行为，也仍然能抓住"把不存在的路径塞进去"那类回归（QDir::exists 会失败）。
        int libPathCount = 0;
        QString libPath;
        for (const QString &a : args) {
            if (a.startsWith(QStringLiteral("-Djava.library.path="))) {
                ++libPathCount;
                libPath = a.mid(QStringLiteral("-Djava.library.path=").size());
            }
        }
        check(libPathCount == 1, QString("%1: -Djava.library.path 恰好一条").arg(v),
              QString::number(libPathCount));
        check(QFileInfo(libPath).isAbsolute() && QDir(libPath).exists(),
              QString("%1: -Djava.library.path 是存在的绝对路径").arg(v), libPath);
    }

    section("C. JVM 选项按 Java 版本过滤");
    for (const auto &pair : QList<QPair<int, bool>>{{8, false}, {17, true}, {21, true}, {25, true}}) {
        GlobalJavaSettings s;
        const QStringList args = launcher.generateJvmArguments(QString(), QStringLiteral("1.19"),
                                                              QStringLiteral("1"), jsons["1.19"], s,
                                                              true, pair.first);
        const bool present = args.contains(QStringLiteral("--enable-native-access=ALL-UNNAMED"));
        check(present == pair.second,
              QString("Java %1 -> --enable-native-access %2")
                  .arg(pair.first)
                  .arg(pair.second ? "存在" : "不存在"));
    }

    section("C2. arguments.jvm 展开（26.3 的 StackShadowPages）");
    // -----------------------------------------------------------------
    // generateJvmArguments 必须把 version.json 的 arguments.jvm 按规则过滤后原样
    // 传进去。26.3 崩在 jvm.dll 访问冲突，根因就是这条链没走通：它声明的
    // -XX:StackShadowPages=32 从来没被传递过，而 JDK 25 的默认值只有 8。影子区不够
    // 时，编译代码在资源重载那种深栈路径上会越过影子区踩到保护页 —— 没有 Java 异常、
    // 没有 hs_err、没有崩溃报告，进程直接消失。
    {
        const auto countExact = [](const QStringList &list, const QString &value) {
            int n = 0;
            for (const QString &item : list) {
                if (item == value) {
                    ++n;
                }
            }
            return n;
        };
        const auto countPrefix = [](const QStringList &list, const QString &prefix) {
            int n = 0;
            for (const QString &item : list) {
                if (item.startsWith(prefix)) {
                    ++n;
                }
            }
            return n;
        };
        const auto hasPrefix = [&countPrefix](const QStringList &list, const QString &prefix) {
            return countPrefix(list, prefix) > 0;
        };

        struct Expect {
            QString version;
            int javaMajor;
            QStringList mustContainExact;
            QStringList mustContainPrefix;  // 形如 "-Djna.tmpdir="，值后面还有内容
            QStringList mustNotContain;
        };
        const QList<Expect> expectations = {
            {QStringLiteral("26.3"),
             25,
             {QStringLiteral("-XX:StackShadowPages=32"), QStringLiteral("--add-exports"),
              QStringLiteral("java.base/jdk.internal.misc=ALL-UNNAMED"),
              QStringLiteral("--enable-native-access=ALL-UNNAMED")},
             {QStringLiteral("-XX:HeapDumpPath="), QStringLiteral("-Djava.library.path="),
              QStringLiteral("-Djna.tmpdir="),
              QStringLiteral("-Dorg.lwjgl.system.SharedLibraryExtractPath="),
              QStringLiteral("-Dio.netty.native.workdir=")},
             {QStringLiteral("-XstartOnFirstThread")}},
            {QStringLiteral("26.1"),
             25,
             {QStringLiteral("--sun-misc-unsafe-memory-access=allow"),
              QStringLiteral("--enable-native-access=ALL-UNNAMED")},
             {QStringLiteral("-Djna.tmpdir=")},
             {QStringLiteral("-XX:StackShadowPages=32"), QStringLiteral("-Xss1M")}},
            // Win10 规则挂在 os.version 的 "^10\." 上。用 QSysInfo::productVersion()
            // 在 Win11 上会得到 "11"，这两条就会静默消失 —— 所以顺带把它断言住。
            {QStringLiteral("1.18.2"),
             17,
             {QStringLiteral("-Dos.name=Windows 10"), QStringLiteral("-Dos.version=10.0")},
             {QStringLiteral("-XX:HeapDumpPath="), QStringLiteral("-Djava.library.path=")},
             {QStringLiteral("-XX:StackShadowPages=32"), QStringLiteral("-Xss1M")}},
            // 1.12.2 根本没有 arguments 字段（走旧版 minecraftArguments），展开结果为空，
            // 所以这里只断言"没有不该有的东西"；它兜底的 java.library.path 依赖 natives
            // 已经解压，放在 section D 里验（那里的沙箱状态是确定的）。
            {QStringLiteral("1.12.2"),
             8,
             {},
             {},
             {QStringLiteral("--enable-native-access=ALL-UNNAMED"),
              QStringLiteral("-XX:StackShadowPages=32")}},
        };

        for (const Expect &e : expectations) {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QString(), e.version, QStringLiteral("1"), jsons[e.version], s, true, e.javaMajor);

            out() << "\n  " << e.version << ": 共 " << args.size() << " 个参数\n";
            for (const QString &need : e.mustContainExact) {
                check(args.contains(need), QString("%1: 含 %2").arg(e.version, need));
            }
            for (const QString &prefix : e.mustContainPrefix) {
                check(hasPrefix(args, prefix), QString("%1: 含 %2*").arg(e.version, prefix));
            }
            for (const QString &banned : e.mustNotContain) {
                check(!hasPrefix(args, banned), QString("%1: 不含 %2").arg(e.version, banned));
            }

            // --add-exports 和它的值是 JSON 里的两个独立元素，必须原顺序、相邻传递，
            // 不能被"配对"或重排。
            if (e.version == QLatin1String("26.3")) {
                const int flag = args.indexOf(QStringLiteral("--add-exports"));
                check(flag >= 0 && flag + 1 < args.size() &&
                          args.at(flag + 1) ==
                              QStringLiteral("java.base/jdk.internal.misc=ALL-UNNAMED"),
                      "26.3: --add-exports 的值紧跟在它后面");
            }

            // 任何残留的 ${...} 都说明有占位符没替换掉
            for (const QString &a : args) {
                if (a.contains(QStringLiteral("${"))) {
                    check(false, QString("%1: 没有未替换的占位符").arg(e.version), a);
                }
            }

            // -cp 只能有一条（version.json 里的 -cp ${classpath} 必须被丢掉，否则
            // 命令行上会出现两个 -cp，而且前一个的值是未展开的占位符）
            check(countExact(args, QStringLiteral("-cp")) == 1,
                  QString("%1: -cp 恰好一条").arg(e.version),
                  QString::number(countExact(args, QStringLiteral("-cp"))));
            const int cp = args.indexOf(QStringLiteral("-cp"));
            check(cp >= 0 && cp + 2 < args.size() &&
                      args.at(cp + 2) == QStringLiteral("net.minecraft.client.main.Main"),
                  QString("%1: 主类紧跟 classpath").arg(e.version));

            // 同一条属性不许出现两次（我们硬编码的和 version.json 声明的会重叠）
            check(countExact(args, QStringLiteral("--enable-native-access=ALL-UNNAMED")) <= 1,
                  QString("%1: --enable-native-access 不重复").arg(e.version),
                  QString::number(
                      countExact(args, QStringLiteral("--enable-native-access=ALL-UNNAMED"))));
            check(countPrefix(args, QStringLiteral("-Djava.library.path=")) <= 1,
                  QString("%1: -Djava.library.path 不重复").arg(e.version),
                  QString::number(countPrefix(args, QStringLiteral("-Djava.library.path="))));
        }
    }

    section("C3. 新增选项的门槛（OmitStackTrace / sun-misc / stdout.encoding）");
    {
        const auto countPrefix = [](const QStringList &list, const QString &prefix) {
            int n = 0;
            for (const QString &item : list) {
                if (item.startsWith(prefix)) {
                    ++n;
                }
            }
            return n;
        };

        struct Gate {
            int javaMajor;
            bool expectSunMisc;   // --sun-misc-unsafe-memory-access=allow（实测 Java 25+）
            bool expectEncoding;  // -Dstdout.encoding=utf-8（JDK 19 才引入这两个属性）
            QString note;
        };
        const QList<Gate> gates = {
            {8, false, false, QStringLiteral("Java 8：两条都不能加，加了就起不来")},
            {17, false, false, QStringLiteral("Java 17：会拒绝 sun-misc")},
            {19, false, true, QStringLiteral("Java 19：encoding 的下界（17 忽略、21 生效）")},
            {21, false, true, QStringLiteral("Java 21：拒绝 sun-misc，编码已生效")},
            // 24 这一档是**刻意**放"没有"边的：JEP 498 在 JDK 24 引入这个选项，但本机
            // 没有 24 的运行时，21 拒绝 / 25 接受之间无法验证。代价不对称 —— 少加一条
            // 只是少压一句警告，加错一条是 JVM 起不来 —— 所以门槛取实测下界 25。
            {24, false, true, QStringLiteral("Java 24：无法验证，取保守边（见注释）")},
            {25, true, true, QStringLiteral("Java 25：接受 sun-misc，门槛的下界")},
            {0, false, false, QStringLiteral("调用方没传版本：一律按最保守的来")},
        };

        for (const Gate &g : gates) {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QString(), QStringLiteral("1.19"), QStringLiteral("1"), jsons["1.19"], s, true,
                g.javaMajor);

            const QString label = g.javaMajor <= 0 ? QStringLiteral("未传") : QString::number(g.javaMajor);
            check(args.contains(QStringLiteral("--sun-misc-unsafe-memory-access=allow")) ==
                      g.expectSunMisc,
                  QString("Java %1: --sun-misc-unsafe-memory-access %2（%3）")
                      .arg(label, g.expectSunMisc ? QStringLiteral("有") : QStringLiteral("没有"), g.note));
            check(countPrefix(args, QStringLiteral("-Dstdout.encoding=")) == (g.expectEncoding ? 1 : 0),
                  QString("Java %1: -Dstdout.encoding %2")
                      .arg(label, g.expectEncoding ? QStringLiteral("恰好一条") : QStringLiteral("没有")));
            // 两个属性必须同进同退：只加一个会让 stdout/stderr 用不同编码解码
            check(countPrefix(args, QStringLiteral("-Dstderr.encoding=")) ==
                      countPrefix(args, QStringLiteral("-Dstdout.encoding=")),
                  QString("Java %1: stdout/stderr 编码成对出现").arg(label));
            if (g.expectEncoding) {
                check(args.contains(QStringLiteral("-Dstdout.encoding=utf-8")) &&
                          args.contains(QStringLiteral("-Dstderr.encoding=utf-8")),
                      QString("Java %1: 两个编码都是 utf-8").arg(label));
            }
            // 实测 Java 8/17/25 全接受，所以这条必须**无条件**在
            check(args.contains(QStringLiteral("-XX:-OmitStackTraceInFastThrow")),
                  QString("Java %1: 无条件带 -XX:-OmitStackTraceInFastThrow").arg(label));

            // 这一批里唯一有回归风险的一处：加参数（编码）和改解码必须同进同退。
            // 只加参数不改解码 -> 中文日志成为乱码；只改解码不加参数 -> 同样乱码。
            // 直接用生产代码那个判据把两端串起来断言，将来谁改歪了这里先红。
            check(LauncherManager::commandDeclaresUtf8Output(args) == g.expectEncoding,
                  QString("Java %1: 生成参数与解码判据一致（%2）")
                      .arg(label, g.expectEncoding ? QStringLiteral("两边都是 UTF-8")
                                                   : QStringLiteral("两边都是本地代码页")));
        }

        // 解码判据本身的真值表（它是纯函数，跟参数生成本身无关的那几种输入也要对）
        struct Utf8Case {
            QStringList args;
            bool expect;
            QString note;
        };
        const QList<Utf8Case> utf8Cases = {
            {{}, false, QStringLiteral("命令行里没有这个属性 -> 本地代码页")},
            {{QStringLiteral("-Dstdout.encoding=utf-8")}, true, QStringLiteral("值就是 utf-8")},
            {{QStringLiteral("-Dstdout.encoding=UTF-8")}, true, QStringLiteral("值不区分大小写")},
            {{QStringLiteral("-Dstdout.encoding=gbk")}, false,
             QStringLiteral("显式写了别的编码 -> 不能当 UTF-8 解")},
            {{QStringLiteral("-Dstderr.encoding=utf-8")}, false,
             QStringLiteral("只有 stderr 那条不算（判据只看 stdout）")},
            {{QStringLiteral("-Dstdout.encoding=utf-8"), QStringLiteral("-Dstdout.encoding=gbk")},
             false, QStringLiteral("出现多次时取最后一份（JVM 就是这个语义）")},
            {{QStringLiteral("-Dstdout.encoding=gbk"), QStringLiteral("-Dstdout.encoding=utf-8")},
             true, QStringLiteral("同上，反过来")},
        };
        for (const Utf8Case &c : utf8Cases) {
            check(LauncherManager::commandDeclaresUtf8Output(c.args) == c.expect,
                  QString("解码判据: %1").arg(c.note));
        }

        // 按版本的自定义参数：排在全局之后（JVM 后写的胜出，所以单实例能盖掉全局默认）
        GlobalJavaSettings s;
        s.setExtraJvmArgs(QStringLiteral("-XX:+UseG1GC"));
        const QStringList withVersionArgs = launcher.generateJvmArguments(
            QString(), QStringLiteral("1.19"), QStringLiteral("1"), jsons["1.19"], s, true, 8,
            QStringList{QStringLiteral("-XX:+PerfDisableSharedMem"),
                        QStringLiteral("-XX:MaxGCPauseMillis=50")});
        const int globalIndex = withVersionArgs.indexOf(QStringLiteral("-XX:+UseG1GC"));
        const int versionIndex = withVersionArgs.indexOf(QStringLiteral("-XX:MaxGCPauseMillis=50"));
        check(globalIndex >= 0, "全局 extraJvmArgs 在参数里");
        check(versionIndex > globalIndex, "按版本的参数排在全局 extraJvmArgs 之后",
              QString("global=%1 version=%2").arg(globalIndex).arg(versionIndex));
        check(withVersionArgs.contains(QStringLiteral("-XX:+PerfDisableSharedMem")),
              "按版本的参数逐条传入，没有被打散");

        // 不带 versionJvmArgs 时参数列表里不该出现这些（默认值是空列表）
        const QStringList withoutVersionArgs = launcher.generateJvmArguments(
            QString(), QStringLiteral("1.19"), QStringLiteral("1"), jsons["1.19"], s, true, 8);
        check(!withoutVersionArgs.contains(QStringLiteral("-XX:MaxGCPauseMillis=50")),
              "不传 versionJvmArgs 时不会凭空出现按版本的参数");

        // 用户自定义参数**不参与去重**：那是用户自己写的，启动器替他删掉才是越权。
        // 26.1 的 version.json 自己声明了 --sun-misc-unsafe-memory-access=allow，
        // 用户再写一条 deny，两条都该在（JVM 取后写的，所以 version.json 那份胜出）。
        const QStringList userOverlap = launcher.generateJvmArguments(
            QString(), QStringLiteral("26.1"), QStringLiteral("1"), jsons["26.1"], s, true, 25,
            QStringList{QStringLiteral("--sun-misc-unsafe-memory-access=deny")});
        check(countPrefix(userOverlap, QStringLiteral("--sun-misc-unsafe-memory-access=")) == 2,
              "用户自定义的同名前缀参数不被去重",
              QString::number(
                  countPrefix(userOverlap, QStringLiteral("--sun-misc-unsafe-memory-access="))));

        // version.json 声明了就以它为准：我们自己硬编码的那条必须被去掉。
        // 用一个最小 version.json 保证"declared 里有它"这件事是确定的。
        QJsonArray jvmEntries;
        jvmEntries.append(QStringLiteral("--sun-misc-unsafe-memory-access=deny"));
        jvmEntries.append(QStringLiteral("-Dstdout.encoding=gbk"));
        QJsonObject argumentsObj;
        argumentsObj.insert(QStringLiteral("jvm"), jvmEntries);
        QJsonObject denyJson;
        denyJson.insert(QStringLiteral("arguments"), argumentsObj);

        const QStringList declaredWins = launcher.generateJvmArguments(
            QString(), QStringLiteral("1.19"), QStringLiteral("1"), denyJson, s, true, 25);
        check(!declaredWins.contains(QStringLiteral("--sun-misc-unsafe-memory-access=allow")),
              "version.json 声明了 sun-misc 时，我们硬编码的那条被去掉");
        check(declaredWins.contains(QStringLiteral("--sun-misc-unsafe-memory-access=deny")),
              "version.json 的 sun-misc 值原样保留");
        check(!declaredWins.contains(QStringLiteral("-Dstdout.encoding=utf-8")),
              "version.json 声明了 stdout.encoding 时，我们硬编码的那条被去掉");
        check(declaredWins.contains(QStringLiteral("-Dstdout.encoding=gbk")),
              "version.json 的 stdout.encoding 值原样保留");
        check(!declaredWins.contains(QStringLiteral("-Dstderr.encoding=utf-8")),
              "version.json 声明 stdout.encoding 时，我们那两条成对撤掉",
              "（解码侧只有一个开关，混搭编码无论取哪边都是错的）");
    }

    section("C4. --gameDir / --assetsDir 是绝对路径（两个分支都要）");
    // -----------------------------------------------------------------
    // --gameDir / --assetsDir 是绝对路径，不依赖 cwd —— 所以这里既断言"是绝对的"，
    // 也断言"指的正是那个目录"，两件事都要钉住。
    {
        const QString expectedGameDir = sandbox + "/versions/1.19";
        const QString expectedAssetsDir = sandbox + "/assets";

        const auto argValue = [](const QStringList &list, const QString &flag) -> QString {
            const int i = list.indexOf(flag);
            return (i >= 0 && i + 1 < list.size()) ? list.at(i + 1) : QString();
        };

        for (const bool offline : {true, false}) {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, offline, 17);

            const QString branch = offline ? QStringLiteral("离线") : QStringLiteral("在线");
            const QString gameDir = argValue(args, QStringLiteral("--gameDir"));
            const QString assetsDir = argValue(args, QStringLiteral("--assetsDir"));

            check(QDir::isAbsolutePath(gameDir),
                  QString("%1: --gameDir 是绝对路径").arg(branch), gameDir);
            check(QDir::isAbsolutePath(assetsDir),
                  QString("%1: --assetsDir 是绝对路径").arg(branch), assetsDir);
            check(QFileInfo(gameDir).absoluteFilePath() ==
                      QFileInfo(expectedGameDir).absoluteFilePath(),
                  QString("%1: --gameDir 就是 versions/<id>").arg(branch), gameDir);
            check(QFileInfo(assetsDir).absoluteFilePath() ==
                      QFileInfo(expectedAssetsDir).absoluteFilePath(),
                  QString("%1: --assetsDir 就是 .minecraft/assets").arg(branch), assetsDir);
            // 两个分支给的值必须完全一样：否则"离线能跑在线不能跑"这种 bug 只会在
            // 用户切换登录方式时冒出来
            if (!offline) {
                GlobalJavaSettings s2;
                const QStringList offlineArgs = launcher.generateJvmArguments(
                    QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                    jsons["1.19"], s2, true, 17);
                check(argValue(offlineArgs, QStringLiteral("--gameDir")) == gameDir &&
                          argValue(offlineArgs, QStringLiteral("--assetsDir")) == assetsDir,
                      "离线/在线两个分支的 --gameDir/--assetsDir 逐字节相同");
            }
        }
    }

    section("C5. 多会话：一个 LauncherManager 同时管好几份游戏进程");
    // -----------------------------------------------------------------
    // 多账户多实例那件事的地基。四件容易写错的：① 早退路径不能留下"幽灵会话"
    // （底栏会永远挂着「实例运行中」）；② 两份会话的输出必须各带各的号，不然日志分不出
    // 谁是谁、"启动中"进度也会互相顶掉；③ 停一份不能连带停另一份；④ 撑到上限之后
    // 再来一份要被拦下、且不改变已有那几份。
    //
    // 长命进程用系统自带的 `ping -n N 127.0.0.1`（十几秒，够做并发断言，也能被杀）。
    // **不用 `java -version`**：它几十毫秒就结束，两份根本重叠不上，"同时在跑"这件事
    // 就无从断言了。
    {
        LauncherManager mgr;
        check(mgr.runningCount() == 0, "刚建出来时一份会话都没有");
        check(!mgr.hasRunningGame(), "hasRunningGame() 同样是假");
        check(mgr.runningGames().isEmpty(), "会话列表也是空的");
        check(mgr.session(0).sessionId == 0, "会话号 0 是哨兵，查它是空的");
        check(mgr.session(99).sessionId == 0, "没见过的会话号查出来也是空的");
        check(!mgr.isVersionRunning(QStringLiteral("1.20.1")), "没跑就是没跑");
        check(!mgr.isVersionRunning(QString()),
              "空版本号永远是假（不能变成「问谁都说在跑」）");

        // (a) 找不到 Java 的早退路径：返回 0，而且**不留会话**。
        GlobalJavaSettings missing;
        missing.setJavaPath(sandbox + "/no-such-java.exe");
        // 早退路径除了那句带号的 processError，还必须再发一条
        // `sessionDiscarded`。界面（GameSessionController）收到 processError 时会按号
        // 给这一份建一扇游戏日志窗、记一笔底账 —— 而这一号此后**再也不会有任何信号**
        // （它连进程都没起），所以不补这一条的话，那些东西会随"启动失败的次数"一直留在
        // 内存里、窗也永远不自己关。两发的号码必须对得上。
        QList<int> errorSessionIds;
        QList<int> discardedSessionIds;
        QObject::connect(&mgr, &LauncherManager::processError, &app,
                         [&errorSessionIds](int sessionId, const QString &) {
                             errorSessionIds.append(sessionId);
                         });
        QObject::connect(&mgr, &LauncherManager::sessionDiscarded, &app,
                         [&discardedSessionIds](int sessionId) {
                             discardedSessionIds.append(sessionId);
                         });
        check(mgr.startGame({QStringLiteral("-version")}, missing, QStringLiteral("1.20.1")) == 0,
              "Java 路径不存在时 startGame 返回 0（不是会话号）");
        check(errorSessionIds.size() == 1 && errorSessionIds.first() > 0,
              "报错那一句带着会话号发出来（界面据此把话挂到实例名上）",
              QString::number(errorSessionIds.size()));
        check(discardedSessionIds == errorSessionIds,
              "早退之后**再发一条 sessionDiscarded**，号码和报错那一句是同一个"
              "（界面靠它把刚替这一号建出来的日志窗和底账收掉）",
              QStringLiteral("discarded=%1 error=%2")
                  .arg(discardedSessionIds.size())
                  .arg(errorSessionIds.size()));
        check(mgr.runningCount() == 0 && !mgr.hasRunningGame(),
              "启动失败**不留下会话** —— 底栏不该挂着一句「实例运行中」",
              QString::number(mgr.runningCount()));
        check(mgr.session(errorSessionIds.isEmpty() ? 0 : errorSessionIds.first()).sessionId == 0,
              "失败的那一份连号都不占（会话表里没有它）");

        const QString ping = QStandardPaths::findExecutable(QStringLiteral("ping"));
        if (ping.isEmpty()) {
            // 静默通过比不测更糟 —— 这台机器上跑不了就把话说明白。
            check(true, "（PATH 里没有 ping，跳过多会话那几条真进程断言）");
        } else {
            GlobalJavaSettings shell;
            shell.setJavaPath(ping);

            // 收到的输出按会话号归档。**一条都没收到也算过**：这里要钉的是"哪一份说的"
            // （sessionId 对不对），不是"进程吐了多少话" —— 后者是 ping 的行为，不是我们的。
            QHash<int, int> outputCount;
            QObject::connect(&mgr, &LauncherManager::processOutput, &app,
                             [&outputCount](int sessionId, const QString &) {
                                 ++outputCount[sessionId];
                             });

            // ---- 两份**同一个实例**：这就是"一个实例同一账号多次启动 / 不同账号各一个"
            // 那条需求的最小形状（身份不同、实例相同）。
            const QStringList args{QStringLiteral("-n"), QStringLiteral("20"),
                                   QStringLiteral("127.0.0.1")};
            const int first = mgr.startGame(args, shell, QStringLiteral("1.20.1"),
                                            QStringLiteral("实例A"), QStringLiteral("账号甲"));
            const int second = mgr.startGame(args, shell, QStringLiteral("1.20.1"),
                                             QStringLiteral("实例A"), QStringLiteral("账号乙"));
            check(first > 0 && second > 0, "两份都起来了",
                  QStringLiteral("first=%1 second=%2").arg(first).arg(second));
            check(first != second, "两次启动拿到的是**两个**会话号");
            check(mgr.runningCount() == 2, "同一个实例开两份 = 两份会话同时在跑",
                  QString::number(mgr.runningCount()));
            check(mgr.isVersionRunning(QStringLiteral("1.20.1")),
                  "这个实例现在算「运行中」");
            check(!mgr.isVersionRunning(QStringLiteral("1.20.2")),
                  "别的版本没有被连带说成在跑");

            const QList<LauncherManager::GameSession> games = mgr.runningGames();
            check(games.size() == 2, "runningGames() 有两份");
            check(games.size() == 2 && games.at(0).sessionId == first &&
                      games.at(1).sessionId == second,
                  "runningGames() 按会话号（= 启动顺序）排，不是哈希表的随机顺序");
            check(games.size() == 2 && games.at(0).accountName == QStringLiteral("账号甲") &&
                      games.at(1).accountName == QStringLiteral("账号乙"),
                  "两份会话各记各的身份 —— 多账户多实例的要害（都读「当前登录的那个」就全错了）",
                  games.size() == 2
                      ? games.at(0).accountName + QStringLiteral("/") + games.at(1).accountName
                      : QString());
            check(games.size() == 2 && games.at(0).displayName == QStringLiteral("实例A") &&
                      games.at(1).displayName == QStringLiteral("实例A"),
                  "实例名是启动那一刻的快照");
            check(mgr.session(second).sessionId == second && mgr.session(second).processId > 0,
                  "按会话号查得到，而且记下了进程号",
                  QString::number(mgr.session(second).processId));

            // 输出各带各的号。
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 8000 &&
                   (outputCount.value(first) == 0 || outputCount.value(second) == 0)) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            check(outputCount.value(first) > 0 && outputCount.value(second) > 0,
                  "两份会话的输出各带各的 sessionId（日志里才分得出谁是谁）",
                  QStringLiteral("first=%1 second=%2")
                      .arg(outputCount.value(first))
                      .arg(outputCount.value(second)));
            check(outputCount.size() == 2,
                  "没有第三个会话号冒出来", QString::number(outputCount.size()));

            // ---- 停一份，另一份不受影响。
            check(!mgr.stopGame(9999), "停一个不存在的会话号返回 false（不崩）");
            check(mgr.stopGame(second), "stopGame 对在跑的那一份返回 true");
            timer.restart();
            while (mgr.runningCount() > 1 && timer.elapsed() < 15000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            check(mgr.runningCount() == 1, "停掉一份之后只剩一份在跑",
                  QString::number(mgr.runningCount()));
            check(mgr.runningGames().size() == 1 &&
                      mgr.runningGames().first().sessionId == first,
                  "剩下的是**另一份**（停一份没连带停另一份）");
            check(mgr.isVersionRunning(QStringLiteral("1.20.1")),
                  "同一个实例还剩一份在跑，所以它还是「运行中」");
            check(!mgr.stopGame(second), "已经停掉的那份再停一次返回 false");

            // ---- 上限那条路：撑到上限，再来一份要被拦下。
            const int cap = LauncherManager::maxConcurrentSessions();
            check(cap >= 2, "上限至少得是 2，不然「多开」这个功能本身就是空的",
                  QString::number(cap));
            QList<int> filled{first};
            for (int i = mgr.runningCount(); i < cap; ++i) {
                const int id = mgr.startGame(args, shell, QStringLiteral("1.20.1"),
                                             QStringLiteral("实例B"), QStringLiteral("账号丙"));
                if (id <= 0) {
                    break;
                }
                filled.append(id);
            }
            check(mgr.runningCount() == cap, "会话数撑到了上限", QString::number(mgr.runningCount()));
            check(mgr.startGame(args, shell, QStringLiteral("1.20.1")) == 0,
                  "到了上限再启动一份，返回 0（被拦下）");
            check(mgr.runningCount() == cap,
                  "被拦下那一次**没有**挤掉已有的一份", QString::number(mgr.runningCount()));

            for (int id : filled) {
                mgr.stopGame(id);
            }
            timer.restart();
            while (mgr.runningCount() > 0 && timer.elapsed() < 30000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            check(mgr.runningCount() == 0 && !mgr.hasRunningGame(),
                  "全停掉之后一份都不剩（会话表收干净了）",
                  QString::number(mgr.runningCount()));
            check(mgr.runningGames().isEmpty(), "会话列表也跟着空了");
            check(discardedSessionIds.size() == 1,
                  "正常结束的那些会话一条 sessionDiscarded 都不发（那是早退路径的专利）",
                  QString::number(discardedSessionIds.size()));
        }
    }

    section("C6. --username 只有调用方给的那一个来源（离线/正版都一样）");
    // -----------------------------------------------------------------
    // 底栏左下角显示的名字和游戏里那个名字必须是同一个 —— 两条分支都取调用方传进来的
    // playerName（真实路径传 AuthManager::playerName()），不许各自拼名字。
    {
        const auto argValue = [](const QStringList &list, const QString &flag) -> QString {
            const int i = list.indexOf(flag);
            return (i >= 0 && i + 1 < list.size()) ? list.at(i + 1) : QString();
        };
        const auto usernameOf = [&argValue](const QStringList &list) {
            return argValue(list, QStringLiteral("--username"));
        };

        for (const bool offline : {true, false}) {
            const QString branch = offline ? QStringLiteral("离线") : QStringLiteral("正版");
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, offline, 17, QStringList(), QStringLiteral("Player"));
            check(usernameOf(args) == QStringLiteral("Player"),
                  QString("%1分支：--username 是调用方给的名字").arg(branch), usernameOf(args));
        }

        // 用户自己改过名字（以后从 profile 拉回来的真实名字）也要原样传下去
        {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, true, 17, QStringList(), QStringLiteral("Steve"));
            check(usernameOf(args) == QStringLiteral("Steve"),
                  "换一个名字也照传（离线）", usernameOf(args));
        }

        // 调用方没给名字（测试、老调用点）：退回默认名，**绝不能是空的** ——
        // --username 空着游戏会直接拒绝启动。
        {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, true, 17);
            check(usernameOf(args) == QStringLiteral("Player"),
                  "没传名字时退回默认名 Player（不是空串）", usernameOf(args));
            check(args.contains(QStringLiteral("--username")),
                  "而且 --username 这个开关本身还在");
        }

        // 名字里带空格也不该被拆成两个参数（老实现拼时间戳时不会有这种情况，
        // 换成用户自定义名字之后会）
        {
            GlobalJavaSettings s;
            const QStringList args = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, true, 17, QStringList(), QStringLiteral("My Name"));
            check(usernameOf(args) == QStringLiteral("My Name"),
                  "带空格的名字原样作为**一个**参数传下去", usernameOf(args));
        }

        // ---- --uuid：正版分支用调用方给的账户 UUID，离线分支不用它 ----
        // 老实现的正版分支发的是全零 UUID（登录流程那时不拉档案，没有 uuid 来源），
        // 在正版服上等于一个"谁都不是"的身份。现在档案里有了，就得传下去。
        {
            const auto uuidOf = [&argValue](const QStringList &list) {
                return argValue(list, QStringLiteral("--uuid"));
            };
            const QString steveUuid = QStringLiteral("069a79f4-44e9-4726-a5be-fca90e38aaf5");

            GlobalJavaSettings s;
            const QStringList online = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, false, 17, QStringList(), QStringLiteral("Steve"), steveUuid);
            check(uuidOf(online) == steveUuid, "正版分支：--uuid 是调用方给的账户 UUID",
                  uuidOf(online));

            // 没给（没登录、或者老调用点）：退回全零，**不是空串** —— 空的 --uuid
            // 游戏会直接拒绝启动。
            const QStringList noUuid = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, false, 17, QStringList(), QStringLiteral("Player"));
            check(uuidOf(noUuid) == QStringLiteral("00000000-0000-0000-0000-000000000000"),
                  "正版分支没给 UUID 时退回全零（不是空串）", uuidOf(noUuid));

            // 离线分支的 uuid 是**每次随机生成**的（单机存档认这个），不该被账户 UUID 顶掉
            const QStringList offlineArgs = launcher.generateJvmArguments(
                QStringLiteral("fake-token"), QStringLiteral("1.19"), QStringLiteral("1"),
                jsons["1.19"], s, true, 17, QStringList(), QStringLiteral("Steve"), steveUuid);
            check(uuidOf(offlineArgs) != steveUuid && !uuidOf(offlineArgs).isEmpty(),
                  "离线分支的 --uuid 还是它自己随机生成的那个", uuidOf(offlineArgs));
        }
    }

    section("D. natives 解压（LauncherManager::prepareNatives）");
    for (const QString &v : {"1.18.2", "1.12.2"}) {
        prepareSandbox(sandbox, v);
        QDir().mkpath(sandbox + "/versions/" + v);
        QFile::copy(metaDir + "/" + v + ".json",
                    sandbox + "/versions/" + v + "/" + v + ".json");

        // The former build/verify tree contained downloaded native JARs that
        // were never source-controlled. Generate tiny but genuine JARs here
        // so the extraction test is hermetic. The legacy case checks the
        // exact DLL names, while the modern case only needs a real DLL entry.
        bool nativeFixturesReady = true;
        for (qsizetype i = 0; i < nativesOf[v].size(); ++i) {
            QMap<QString, QByteArray> entries;
            if (v == QLatin1String("1.12.2") && i == 0) {
                entries.insert(QStringLiteral("lwjgl.dll"), QByteArrayLiteral("dll"));
                entries.insert(QStringLiteral("lwjgl64.dll"), QByteArrayLiteral("dll64"));
                entries.insert(QStringLiteral("OpenAL32.dll"), QByteArrayLiteral("openal32"));
                entries.insert(QStringLiteral("OpenAL64.dll"), QByteArrayLiteral("openal64"));
                entries.insert(QStringLiteral("jinput-dx8.dll"), QByteArrayLiteral("jinput"));
            } else {
                entries.insert(QStringLiteral("native-%1.dll").arg(i),
                               QByteArrayLiteral("native"));
            }
            nativeFixturesReady =
                makeFakeJar(sandbox + "/libraries/" + nativesOf[v].at(i).path, entries) &&
                nativeFixturesReady;
        }
        check(nativeFixturesReady, QString("%1: 自行生成 natives JAR 夹具").arg(v));

        const QString nativesDir = sandbox + "/versions/" + v + "/natives";
        const QFileInfo before(nativesDir);

        QStringList errors;
        const QString got = launcher.prepareNatives(jsons[v], v, &errors);
        check(!got.isEmpty(), QString("%1: prepareNatives 返回目录").arg(v),
              errors.join(QStringLiteral("; ")));
        check(QFileInfo(got).absoluteFilePath() == QFileInfo(nativesDir).absoluteFilePath(),
              QString("%1: 目录落在 versions/%1/natives").arg(v), got);

        const QStringList dlls =
            QDir(nativesDir).entryList(QStringList() << QStringLiteral("*.dll"), QDir::Files);
        out() << "        " << dlls.size() << " 个 DLL: " << dlls.join(QStringLiteral(", "))
              << "\n";
        check(!dlls.isEmpty(), QString("%1: 解压出 DLL").arg(v));

        if (v == QLatin1String("1.12.2")) {
            check(dlls.contains(QStringLiteral("lwjgl.dll")) &&
                      dlls.contains(QStringLiteral("lwjgl64.dll")) &&
                      dlls.contains(QStringLiteral("OpenAL32.dll")) &&
                      dlls.contains(QStringLiteral("OpenAL64.dll")) &&
                      dlls.contains(QStringLiteral("jinput-dx8.dll")),
                  "1.12.2: 关键 DLL 齐全");
        }

        // 守卫：第二次调用不应重新解压（目录 mtime 不变）
        const QDateTime mtimeBefore = QFileInfo(nativesDir).lastModified();
        QStringList errors2;
        const QString again = launcher.prepareNatives(jsons[v], v, &errors2);
        check(again == got && QFileInfo(nativesDir).lastModified() == mtimeBefore,
              QString("%1: 第二次调用走守卫、不重复解压").arg(v));

        // 解压后 checkPreLaunch 应该过
        QStringList preErrors;
        const bool pre = launcher.checkPreLaunch(v, &preErrors, jsons[v]);
        check(pre, QString("%1: checkPreLaunch 通过").arg(v),
              preErrors.join(QStringLiteral("; ")));

        // 生成的参数里必须带上 java.library.path
        GlobalJavaSettings s;
        const QStringList args = launcher.generateJvmArguments(
            QString(), v, QStringLiteral("1"), jsons[v], s, true, v == QLatin1String("1.12.2") ? 8 : 17);
        QString libPath;
        for (const QString &a : args) {
            if (a.startsWith(QStringLiteral("-Djava.library.path="))) {
                libPath = a.mid(QStringLiteral("-Djava.library.path=").size());
            }
        }
        check(QFileInfo(libPath).isAbsolute() && QDir(libPath).exists(),
              QString("%1: -Djava.library.path 是存在的绝对路径").arg(v), libPath);
    }

    // 反证：删掉 natives 目录后，1.18.2 的 checkPreLaunch 必须拦下来
    {
        const QString nativesDir = sandbox + "/versions/1.18.2/natives";
        QDir(nativesDir).removeRecursively();
        QStringList preErrors;
        const bool pre = launcher.checkPreLaunch(QStringLiteral("1.18.2"), &preErrors, jsons["1.18.2"]);
        check(!pre, "1.18.2: natives 缺失时 checkPreLaunch 会拦下",
              preErrors.join(QStringLiteral("; ")));
    }

    section("E. Java 运行时选择（GlobalJavaSettings::ensureJavaForVersion）");
    GlobalJavaSettings settings;
    settings.load();
    out() << "  配置中的 java: " << settings.javaPath() << "\n";
    const QString originalPath = settings.javaPath();

    // **默认不下载**。这一段以前直接调 ensureJavaForVersion()，本机没有匹配版本时它会去
    // Mojang 下几百 MB 的运行时（再兜底 Adoptium）—— CI 上慢网络偶尔跑满 900 秒的阶段上限，
    // 整个 ci-core 判死（2026-11-03 那次就是）。现在默认只在本机找：找得到就核对版本，
    // 找不到就明确"本机没有、未下载"，不把"没联网"变成"构建失败"。
    // 真要验下载那条路：设 WTFML_TEST_JAVA_DOWNLOAD=1 再跑这个 exe。
    const bool allowDownload = qEnvironmentVariableIsSet("WTFML_TEST_JAVA_DOWNLOAD");
    out() << "  允许下载运行时: " << (allowDownload ? "是" : "否（设 WTFML_TEST_JAVA_DOWNLOAD=1 打开）") << "\n";

    const QList<QPair<int, QString>> requirements = {
        {25, QStringLiteral("java-runtime-epsilon")},
        {21, QStringLiteral("java-runtime-delta")},
        {17, QStringLiteral("java-runtime-beta")},
        {8, QStringLiteral("jre-legacy")},
    };
    for (const auto &req : requirements) {
        // 每次都从"错误"的配置出发，验证会主动去找匹配的版本
        settings.setJavaPath(originalPath);
        const bool ok = settings.ensureJavaForVersion(req.first, req.second, allowDownload);
        const QString got = settings.javaPath();
        // Java 8 的 -version 输出是 "1.8.0_51"，其余是 "25.0.1" 这种
        const QString expect = req.first == 8 ? QStringLiteral("1.8") : QString::number(req.first);
        if (!ok) {
            // 只有"不允许下载"时，本机缺这一档才是合法结果；此时不能假装成功。
            check(!allowDownload, QString("需要 Java %1 -> 找到").arg(req.first), got);
            out() << "  Java " << req.first << " 本机没有，按设置未下载\n";
            continue;
        }
        const QString ver = javaVersionOf(got);
        check(ok, QString("需要 Java %1 -> 找到").arg(req.first), got);
        check(ver.startsWith(expect),
              QString("需要 Java %1 -> 实际版本 %2").arg(req.first).arg(ver));
    }

    // 恢复用户原来的配置，测试不该改动机器上的设置
    settings.setJavaPath(originalPath);
    settings.save();
    out() << "  已恢复配置中的 java: " << settings.javaPath() << "\n";

#ifndef WTFML_CORE_BASELINE
runPipelineTests(app, sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runVersionNameTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstanceFileTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstalledScanTests(sandbox);
#endif

    // -----------------------------------------------------------------
    // 排在扫描之后，理由见 names 模式里同名调用点上的注释。

#ifndef WTFML_CORE_BASELINE
runVersionMetaWriteTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstallOccupancyTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runRenameInstanceTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runInstanceGroupTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runVersionJvmArgsTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runMemorySuggestionTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runEffectiveJavaSettingsTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runJavaWarnMismatchTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runJavaReleaseParseTests();
#endif

#ifndef WTFML_CORE_BASELINE
runPlayerSkinTests(sandbox);
#endif

    // 紧挨着它：这一节管的是"离线账号那张脸"的另一半 —— 能算出游戏会给哪一张时，
    // 就用那一张（随机那张是退路）。两节验的东西不同，别合并。

#ifndef WTFML_CORE_BASELINE
runDefaultSkinTests(sandbox);
#endif
    // 再往上一层：离线账号**用户自己挑的那张**（最优先，压过上面两者）。

#ifndef WTFML_CORE_BASELINE
runOfflineSkinTests(sandbox);
#endif

    // -----------------------------------------------------------------
    // 排在最后：这一节自己把 QSettings 搬进沙盒又搬回来（跑完是原生形态），
    // 所以别把它插在中间 —— 上面那些节读盘走的是真实注册表。

#ifndef WTFML_CORE_BASELINE
runAccountSkinTests(sandbox);
#endif

#ifndef WTFML_CORE_BASELINE
runLoaderInheritanceTests(sandbox);
#endif

    // -----------------------------------------------------------------
    // 纯函数那半：地址怎么拼、列表怎么挑、profile 能不能装、界面下标怎么翻。
    // 整节离线（联网那条路没法在沙箱里跑），所以它跟 U 节一样是"自己收摊"的。

#ifndef WTFML_CORE_BASELINE
runLoaderInstallTests();
#endif

    // -----------------------------------------------------------------------
    // 整合包导入（纯函数 + 真解一次压缩包 + 真清单），见上面 V 节。
    // Y 节：四个分类 / 四个加载器的可选版本列表（纯函数，喂真清单和真夹具）。
    // 排在整合包那节之前 —— 它只读 meta/ 下的夹具、不碰沙箱，放哪儿都不影响别人。

#ifndef WTFML_CORE_BASELINE
runVersionChannelTests();
#endif

#ifndef WTFML_CORE_BASELINE
runModpackImportTests(sandbox);
#endif

    // ZG 节：CurseForge 那一份整合包（清单里只有号 → 换文件 → 落点），纯函数、不联网。
    // 紧跟在上面那一节后面 —— 两节讲的是同一件事的两种格式，摆在一起才好对着看。

#ifndef WTFML_CORE_BASELINE
runCurseForgeTests();
runModpackCompletionTests(sandbox);
#endif

    // -----------------------------------------------------------------------
    // 下载页那几类资源的数据层（拼地址 + 读真响应），纯函数、离线，见上面 Z 节。

#ifndef WTFML_CORE_BASELINE
runResourceCatalogTests();
#endif

    // -----------------------------------------------------------------------
    // 资源详情页要的那几件（时间那句 / 渠道那一档 / 分组 / chip 筛 / 头卡那行字），
    // 纯函数、离线，见上面 Z8 节。紧挨着上面那一节：两边读的是同一份夹具。

#ifndef WTFML_CORE_BASELINE
runResourceDetailTests();
#endif

    // -----------------------------------------------------------------------
    // 图标缓存（盘上那一份）+ "别一张张干等"那道闸，见上面 ZB 节。纯函数 + 文件层，
    // 自己收摊（换过的数据根目录在节末换回来）。

#ifndef WTFML_CORE_BASELINE
runIconCacheTests();
#endif

    // -----------------------------------------------------------------------
    // 一串字节 → 一张图（Qt 认得的那一半 + webp 走系统 WIC 那一半），见上面 ZC 节。
    // 要真解码，所以会碰一下 COM/WIC —— 不联网、不落盘。

#ifndef WTFML_CORE_BASELINE
runImageDecodeTests();
#endif

    // -----------------------------------------------------------------
    // 设置页「启动选项」那三项（显示游戏日志 / 进程优先级 / 窗口大小）+ 窗口大小进命令行
    // 这条链，见上面 ZD 节。自己开一个 QSettings 沙盒、跑完收摊。

#ifndef WTFML_CORE_BASELINE
runLaunchOptionsTests(sandbox);
#endif

    // -----------------------------------------------------------------
    // 服务器列表那个"连上去问一句"（Server List Ping），见上面 ZH 节。这一节自己起一台
    // 假的服务器、只连 127.0.0.1 —— 不联网，但**要真连**（纯函数那半证不了协议）。

#ifndef WTFML_CORE_BASELINE
runServerPingTests();
#endif

    // -----------------------------------------------------------------
    // `servers.dat` 的写侧（改一条服务器的名字 / 地址），见上面 ZK 节。手搓 NBT 夹具、
    // 真落盘、真读回来 —— 全在系统临时目录里，不动沙箱。

#ifndef WTFML_CORE_BASELINE
runServerListEditTests();
#endif
    // 上面那一节钉数据层，这一节钉**谁在指挥**（那四道闸 + 三行日志）。

#ifndef WTFML_CORE_BASELINE
runServerListControllerTests();
#endif

    // -----------------------------------------------------------------
    // quick play（--quickPlaySingleplayer / --quickPlayMultiplayer）那一层判据，见上面
    // ZL 节。纯值进、bool 出；真 json 那两条读的是 meta/ 里的夹具。

#ifndef WTFML_CORE_BASELINE
runQuickPlayTests(metaDir);
#endif

#ifndef WTFML_CORE_BASELINE
runErrorReportTests();
runLaunchScriptExportTests();
#endif

    // -----------------------------------------------------------------
    // 「复制实例」和「导入世界 / 世界改名」那两条真动盘的路，见上面 ZJ 节。自己开两个
    // 临时游戏文件夹、跑完换回去 —— **不碰共享沙盒**（这一节要造一整份实例和一个世界，
    // 留在那儿会给后面、以及下一次重跑留下没预料到的包袱）。

#ifndef WTFML_CORE_BASELINE
runCopyInstanceAndWorldTests();
#endif

    // -----------------------------------------------------------------
    // Java 8 的 ASCII 别名（路径里带非 ASCII 字符就起不来），见上面 ZM 节。自己开一个临时
    // 游戏文件夹、跑完换回去 —— 这一节要真建联接和硬链接，留在共享沙箱里会碍后面的事。

#ifndef WTFML_CORE_BASELINE
runAsciiAliasTests();
#endif

    // -----------------------------------------------------------------
    // 「从别的游戏文件夹里借文件」（见上面 LF 节）。纯值那半不碰盘；
    // DownloadManager 那半自己开临时游戏文件夹、跑完换回去 —— 那几条证明的是
    // "借到了就真的一个网络请求都不发"（地址给的是一条必然失败的 file://）。

#ifndef WTFML_CORE_BASELINE
runLocalFileReuseTests();
#endif

    // -----------------------------------------------------------------
    // natives 解压的完成标记（半套目录不许复用），见上面 ZNat 节。同样自开临时游戏文件夹。

#ifndef WTFML_CORE_BASELINE
runNativesMarkerTests();
#endif

    // -----------------------------------------------------------------
    // 体检（只报不装）那套判据，见上面 ZN 节。**一个文件都不下、一个字节都不写**，
    // 所以它只在一个临时游戏文件夹里摆文件、比结论。清单和实例对不对得上（"体检说的就是
    // 这个实例装配时会去下的那些"）在 mw_check 的 T19.15。

#ifndef WTFML_CORE_BASELINE
runVersionIntegrityTests();
#endif

    // -----------------------------------------------------------------
    // 任务账本那一套，见上面 AL / AM 两节。**一个
    // QWidget 都不碰、一个网络都不开**：纯规则喂值进去、账本自己在一个临时目录里落盘读回
    // —— 不碰共享沙盒。

#ifndef WTFML_CORE_BASELINE
runTaskLedgerTests();
#endif

    // -----------------------------------------------------------------
    // 游戏异常退出那几句话，见上面 ZP 节。纯函数：值进、
    // 字符串出，不看时钟、不读盘、不起进程 —— 那一张提示表在这里逐格念一遍。

#ifndef WTFML_CORE_BASELINE
runGameExitDiagnosisTests();
#endif

    // -----------------------------------------------------------------
    // 世界与实例备份，见上面 ZQ 节。**真打 zip、真删目录、
    // 真还原**：自己开一个临时游戏文件夹（备份落点就是 `<数据目录>/cache/backups`）、跑完
    // 换回去 —— 不碰共享沙盒；这一节留下的痕迹在盘上是好几个压缩包，留在沙箱里碍事。

#ifndef WTFML_CORE_BASELINE
runBackupTests();
#endif

    // -----------------------------------------------------------------
    // 「上一次启动」那本账，见上面 ZR 节。**全是纯函数**：
    // 值进、字符串/一段字节出，不问时钟（那一列的"现在"是参数）—— 只有"账本落在哪个文件上"
    // 那一条要换一下数据根目录（换完就还原），所以它和 ZQ 那一节一样自己开临时目录。

#ifndef WTFML_CORE_BASELINE
runLaunchHistoryTests();
#endif

    // -----------------------------------------------------------------
    // 实例预设，见上面 PA 节。值那一层是纯函数，
    // 盘那一层在一个临时目录里真存真读（每一件都收一个 `root` 参数，所以连全局数据根目录
    // 都不用改）—— 不联网、不碰共享沙箱、不开界面。

#ifndef WTFML_CORE_BASELINE
runPresetStoreTests();
#endif

    // -----------------------------------------------------------------
    // 原版设置那一段，见上面 PB 节。**全是纯函数**：`options.txt`
    // 的字节进、字节出 —— 门槛是"读进来再写出去逐字节相同"，所以这一节一条临时目录都不用开。

#ifndef WTFML_CORE_BASELINE
runVanillaOptionsTests();
#endif

    // -----------------------------------------------------------------
    // 服务器列表那一段，见上面 PC 节。判据那几条是纯值；
    // "落到那个实例上"那几条**真动盘** —— 在一个临时目录里造真 `servers.dat`，跑完读回来
    // 逐字节比（"没碰过的条目一个字节都不变"只有比字节才验得出来）。

#ifndef WTFML_CORE_BASELINE
runServerListPresetTests();
#endif
    // -----------------------------------------------------------------
    // 通用文件组那一段，见上面 PD 节。这一节的重量在
    // "我们**没**弄坏用户东西"和"我们**没**跑出这一组去"：覆盖不删多出来的文件、缺了才拷
    // 是逐文件判的、源侧和目标侧那两根 junction 都真造了一根来验。

#ifndef WTFML_CORE_BASELINE
runGenericFilesTests();
#endif
    // -----------------------------------------------------------------
    // 应用计划那一层，见上面 PE 节。这一节验的是下面那张
    // 表：段一行、数从适配器那边真算出来、几道闸（没点头的覆盖 / 游戏在跑 / 不认识的种类）
    // 真的会拦。★ 这一层只读：除了通用文件组的干跑、MaLiLib 那一档读一遍目标上的
    // `config/<模组>.json`，一个字节都不写。

#ifndef WTFML_CORE_BASELINE
runPresetPlannerTests();
#endif
    // -----------------------------------------------------------------
    // MaLiLib 那一族的热键，见上面 PF 节。
    // 这一节的重量在三处：**按结构认模组**（TweakerMore / Masa Gadget / OMMC 这些名字代码里
    // 一个都没有）、**合并不许退化成覆盖**（目标上没被点名的那几格一个都不动）、**拒绝都在
    // 改动之前**（类型对不上就不转）。

#ifndef WTFML_CORE_BASELINE
runMalilibKeysTests();
#endif
    // -----------------------------------------------------------------
    // 提取与落到实例上，见上面 PG 节。和 PE 那一节的
    // 分界是"有没有真写盘"：这一节拿两份真实例形状的临时目录走一遍收与落，最值钱的是
    // 覆盖那两趟（第一趟一个字节都不动）、游戏在跑整趟不写、以及备份里那份是不是原样。

#ifndef WTFML_CORE_BASELINE
runPresetCaptureApplyTests();
#endif
    // -----------------------------------------------------------------
    // 资源包次序 / 启用 + 世界级数据包，见上面 PL 节。
    // 验收原话是「没世界就待应用；部署不冒充启用」：这一节在两个临时实例里真造
    // `options.txt` 与 `saves/<世界>/datapacks/`，比的都是逐字节。

#ifndef WTFML_CORE_BASELINE
runResourceAndDataPackTests();
#endif
    // -----------------------------------------------------------------
    // 模组「真正去下」之前的判据层，
    // 见上面 PM 节。验收原话是「不错装不兼容文件；替代项目需确认；落盘校验」：
    // 这一节全拿手搭的假答复逐格念，一条网络都不碰；落盘校验读的是临时目录里的真文件。

#ifndef WTFML_CORE_BASELINE
runModInstallPlanTests();
#endif
    // -----------------------------------------------------------------
    // 模组预设的兼容检查，见上面 PH 节。
    // **一条网络都不碰**：四个假提供方分别代表立刻答 / 慢答 / 手动放行 / 永远不答，
    // 验的是去重、本地优先、并发上限、旧 generation 作废、失败不算缺失、缓存只收准话。

#ifndef WTFML_CORE_BASELINE
runPresetCompatibilityTests();
#endif
    // -----------------------------------------------------------------
    // 大量预设 / 大量引用 / 连换几十代 / 先关检查器，见上面 PN 节。验收原话是「无过期回调、失控并发或主线程假死」。

#ifndef WTFML_CORE_BASELINE
runPresetStressTests();
#endif
    // -----------------------------------------------------------------
    // 实例页的搜索与筛选，见上面 ZS 节。**全是纯函数**：
    // 值进、bool 出，不读盘、不问时钟 —— 连一条实例都不必真的存在。

#ifndef WTFML_CORE_BASELINE
runInstanceFilterTests();
#endif

    // -----------------------------------------------------------------
    // 更新中心的「忽略 / 固定」那本账，见上面 ZT 节。
    // **纯函数 + 一个临时目录里的真存真读**：不联网、不碰共享沙箱、不开任何界面。

#ifndef WTFML_CORE_BASELINE
runUpdatePolicyTests();
#endif

    // -----------------------------------------------------------------
    // 更新前那份轻量回滚点，见上面 ZU 节。**真动盘**：
    // 自己开一个临时游戏文件夹造实例目录（这一层整个存在的意义就是"动手之前手上有一份
    // 旧的"，纯值上验不出来），跑完换回来。

#ifndef WTFML_CORE_BASELINE
runUpdateRollbackTests();
#endif

    // -----------------------------------------------------------------
    // 服务端工作区：判据层（状态 / EULA /
    // server.properties / 我们记的那一份），见上面 ZW 节。**纯函数**：值进、结构出。

#ifndef WTFML_CORE_BASELINE
runServerModelTests();
#endif

    // -----------------------------------------------------------------
    // 扫盘那一层，见上面 ZX 节。**真动盘**：自己开一个临时
    // 数据根目录，真建服务端目录、真读写那两个配置文件、真删 —— 跑完换回来。

#ifndef WTFML_CORE_BASELINE
runServerRegistryTests();
#endif

    // -----------------------------------------------------------------
    // 服务端安装那四段的判据层，见上面 ZY 节。
    // **纯函数**：喂一份计划 + 一份字节进去，回一个结论出来（不联网、不建目录、不起 java
    // —— 真干那三件事的是驱动层，归 mw_check）。

#ifndef WTFML_CORE_BASELINE
runServerInstallRunTests();
#endif

    // -----------------------------------------------------------------
    // 服务端"跑起来 / 停下来"那条路的判据层，见上面 ZZ 节。**纯函数**：那条命令怎么拼、起来没有、退了算
    // 哪一档状态、宽限多久才允许强杀（真起进程那一半 `ServerProcess` 归 mw_check 的 T41）。

#ifndef WTFML_CORE_BASELINE
runServerRunTests();
#endif

    // -----------------------------------------------------------------
    // 「服务端设置」那一页底下那两层判据，见上面
    // ZV 节。**几乎全是纯函数**：表本身 / 收不收 / 合回文件 / 体检报哪几条与次序。唯一的例外
    // 是 ZV6 那一条 —— "这个端口有人占着吗"要**真听一次**（本机、不联网）。

#ifndef WTFML_CORE_BASELINE
runServerSettingsTests();
#endif

    // -----------------------------------------------------------------
    // 服务端那四份玩家名单，见上面 ZAE 节。
    // **纯函数**：读要宽容 / 写按游戏那份字段形状 / 认人 / 离线 UUID / 到期。

#ifndef WTFML_CORE_BASELINE
runServerPlayersTests();
#endif

    // -----------------------------------------------------------------
    // 服务端异常退出后自动重启那条策略，
    // 见上面 ZAF 节。**纯函数**：哪一档才拉 / 连着几次收手 / 退避 / 策略往返（不读盘、不看时钟）。

#ifndef WTFML_CORE_BASELINE
runServerRestartTests();
#endif

    // -----------------------------------------------------------------
    // 服务端资源包那一份本地 zip，见上面 ZAG 节。
    // 条目表判据是纯的；那一发 `inspect()` 会真打一个 zip 再读它（走 ZipUtil + FileHash）。

#ifndef WTFML_CORE_BASELINE
runServerResourcePackTests();
#endif
    // 服务端内容：模组 / 数据包的名字与分类（ZAH）。

#ifndef WTFML_CORE_BASELINE
runServerContentTests();
#endif
    // 删一棵目录（FolderManager）：只读文件 + 不追链接。见上面 ZAN 节 —— **真动盘**
    // （只读位是盘上的属性，junction 更是）。

#ifndef WTFML_CORE_BASELINE
runFolderRemovalTests();
#endif
    // 工具箱那两件：信标计算 / 酿造助手（ZAI）。

#ifndef WTFML_CORE_BASELINE
runToolboxExtraTests();
#endif

    // -----------------------------------------------------------------
    // 正版登录那两格随构建配置走的常数，见上面 ZA 节。
    // **纯函数**：一串地址进去、端口出来（不联网、不开监听；真绑端口那一半在 AuthManager
    // 里，归 mw_check）。

#ifndef WTFML_CORE_BASELINE
runOAuthEndpointTests();
#endif

#ifndef WTFML_CORE_BASELINE
runSettingsSyncRetryTests();
#endif

    // -----------------------------------------------------------------
    // 「同时最多下几个」那几档（并发数预设），见上面 ZO 节。
    // **纯函数 + 一个字面对象**：四档那个表、归一化、以及"队列泵问的是这个对象当下那一档"
    // （真发请求那一半归 mw_check）。

#ifndef WTFML_CORE_BASELINE
runDownloadConcurrencyTests();
#endif
    // 「下载速度上限」那几档（下载限速），见上面 ZP 节。

#ifndef WTFML_CORE_BASELINE
runDownloadSpeedLimitTests();
#endif

#ifndef WTFML_CORE_BASELINE
runDownloadProxyTests();
#endif

    // -----------------------------------------------------------------
    // 工具箱那四件小工具，见上面 ZAA–ZAD 四节。
    // **纯函数**：彩色文本那三份形态的往返 / 坐标换算与 F3+C 那条命令 / 哈希与那串"已知值"
    // （要真读一个文件，在临时目录里）/ 铁砧那套账（最后那一条拿穷举当独立对照）。
    // 界面那一侧（ToolboxPage + ToolboxController）的断言不在这一节里。

#ifndef WTFML_CORE_BASELINE
runTextFormatTests();
#endif

#ifndef WTFML_CORE_BASELINE
runCoordConvertTests();
#endif

#ifndef WTFML_CORE_BASELINE
runFileHashTests();
#endif

#ifndef WTFML_CORE_BASELINE
runAnvilPlannerTests();
#endif

    // -----------------------------------------------------------------
    // 结构 / 原理图的元数据与方块统计，见上面 SG 节。
    // 三种格式的**真形状**都由这一节自己搭（`NbtTree::serialize()`），gzip 那一份真压一遍。

#ifndef WTFML_CORE_BASELINE
runSchematicInfoTests();
#endif

#ifndef WTFML_CORE_BASELINE
runReplayInfoTests();
#endif

#ifndef WTFML_CORE_BASELINE
runLogFileReaderTests();
#endif

#ifndef WTFML_CORE_BASELINE
runStartupDiagnosisTests();
#endif

    // -----------------------------------------------------------------
    // 跨实例迁移 / 升级副本，见上面 TR 节。
    // 计划层只读盘、落盘层只增加；这一节自己搭源与目标两棵树，junction 那一发真造。

#ifndef WTFML_CORE_BASELINE
runTransferPlannerTests();
runLongPathTests();
#endif

    out() << "\n===== " << (g_failures == 0 ? "全部通过" : "有失败项") << ": "
          << (g_checks - g_failures) << "/" << g_checks << " =====\n";
    out().flush();
    return g_failures == 0 ? 0 : 1;
}

