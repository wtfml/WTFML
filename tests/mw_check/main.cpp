#include "modules/Support.hpp"
#include "support/SettingsIsolation.hpp"
#include "LoadingState.hpp"
#include "ActivityPopover.hpp"
#include "modules/LiveProbes.hpp"
#include "controllers/JavaRuntimeController.hpp"
#include "manage/VersionManager.hpp"
#include "presets/InstalledModInventory.hpp"
#include <QFile>
#include <atomic>
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "auth/AccountStore.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include <QComboBox>
#include <QElapsedTimer>
#include <QThread>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <QSet>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QScopeGuard>

int main(int argc, char *argv[]) {
    // 验证工程跑在哪台机器上都该给出同样的结果：低内存提醒按"充足内存"这一档算，
    // 免得宿主机刚好紧张时多弹一个确认框，把别的断言带偏。
    if (qEnvironmentVariableIsEmpty("WTFML_AVAILABLE_MB")) {
        qputenv("WTFML_AVAILABLE_MB", "65536");
    }
    if (argc >= 4 && std::strcmp(argv[1], "token-echo") == 0) {
        const std::string secret(argv[3]);
        const auto half = secret.size() / 2;
        std::fwrite(secret.data(), 1, half, stdout);
        std::fwrite(secret.data(), 1, half, stderr);
        std::fflush(stdout); std::fflush(stderr);
        Sleep(60);
        std::fwrite(secret.data() + half, 1, secret.size() - half, stdout);
        std::fwrite(secret.data() + half, 1, secret.size() - half, stderr);
        std::fputs("\n", stdout); std::fputs("\n", stderr);
        std::fwrite(secret.data(), 1, half, stdout);
        std::fflush(stdout); std::fflush(stderr);
        return 0;
    }
    // ---------- session-holder：给"真的有一份会话在跑"当替身 ----------
    // mw_check.exe session-holder
    //
    // AH 那一段要验"左面板一行一个会话"，那就非得真有几份会话在跑不可 —— 而真的 JVM
    // 在这儿是个又贵又脆的依赖（机器上未必装了、起来了还得等它吐日志）。于是启动路径的
    // javaPath 指到**这个 exe 自己**、argv 里塞这个词：这一支认出来就睡够四分钟，
    // 别的什么都不做。对 LauncherManager 来说它和 java 没有区别（一个真的、活着的进程）。
    //
    // NOTE: 必须排在**最前面**，连 QApplication 都不建：这个进程是被启动器起的"游戏"，
    // 它不该去碰沙盒设置（那会在跑测试的这个进程脚下把沙盒重来一遍）、也不该
    // 需要图形平台。
    // NOTE: 睡四分钟是给"子进程被忘了"留的兜底（AH11 会把每一份都停掉）。活着的 holder
    // 会锁住 mw_check.exe 这个文件，下一次构建就写不进去 —— 所以宁可它自己也会醒。
    // ---------- 被当成 java 起的时候：老老实实当一回 java ----------
    // <这个 exe> -Xmx2048m -Djava.library.path=… -cp … net.minecraft.client.main.Main …
    //
    // AG 那一段要让 **MainWindow 自己那条启动路径**真的起来一个"游戏进程"（验的是
    // 「启动中」那一态：黄方块 + 「<名字>启动中...」）。真 JVM 太贵，所以那一段把
    // 这个 exe **复制**一份当假 java（旁边摆一个 release 文件，见那里的说明），
    // 于是这一支认的就是"启动器发出来的那串参数"的形状：
    // · argv[1] 以 `-` 开头（-Xmx / -Xms / -D… / -cp，java 命令行一定是这样；
    // 而这个工程自己的每一种模式名都不是——它们见下面那几段）；
    // · 里面有 `-version` → 那是探版本来问的，回一句就退（真 java 也这么干）；
    // · 否则**静静地睡着**：这正是"进程起来了、游戏还没出话"那个窗口，
    // 也就是产品里黄方块亮着的那一段（睡四分钟，和 session-holder 同一个兜底）。
    //
    // NOTE: 一定要成对收尾 —— 这里的进程是**这个 exe 的副本**，活着的时候锁住的是沙盒里
    // 那个副本文件（见 AG 段末尾"停掉之后才删假 java"）。
    // 这一支的形状判据是"argv[1] 以 `-` 开头" —— 而下面 `--fake-server` 那一支也长这样，
    // 所以**必须把它摘出去**（不摘的话"服务端"会被当成"等着不起来的那份 java"睡上四分钟：
    // 屏幕上的症状是"起得来、但没有一行输出、也永远不退"，T41 全红得像产品坏了）。
    const bool launchedAsServerJava = argc >= 4 &&
        std::strcmp(argv[argc - 3], "-jar") == 0 &&
        std::strcmp(argv[argc - 2], "server.jar") == 0 &&
        std::strcmp(argv[argc - 1], "nogui") == 0;
    if (argc >= 2 && argv[1][0] == '-' && std::strcmp(argv[1], "--fake-server") != 0 &&
        !launchedAsServerJava) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-version") == 0) {
                std::fprintf(stderr, "openjdk version \"17.0.12\" 2024-07-16\n");
                std::fflush(stderr);
                return 0;
            }
        }
        Sleep(4 * 60 * 1000);
        return 0;
    }

    if (argc >= 2 && std::strcmp(argv[1], "session-holder") == 0) {
        Sleep(4 * 60 * 1000);
        return 0;
    }

    // ---------- 被当成服务端起的时候：老老实实当一回 Minecraft 服务端 ----------
    // <这个 exe> --fake-server <mode>
    //
    // T41 要验的是 `ServerProcess` 那一整条路（起 → 读到 `Done (` → 写一句 → 停 / 强杀），
    // 而真 JVM + 真服务端是个又贵又脆的依赖（几个 GB 的下载，还得联网）。所以那一节把
    // program 指到**这个 exe 自己**、argv 里塞这一支：它和 java 没有区别（一个真的、活着的
    // 进程，会往 stdout 吐行、会从 stdin 读行）。
    //
    // 三种 mode：
    // · `normal` —— 吐几句开场白 + 那一行 `Done (…)!`（产品就靠这一行判"起来了"），
    // 然后**逐行读 stdin**：`stop` ⇒ 自己退 0，别的原样回声（T41 拿它验
    // `sendCommand()` 真的送进去了）；
    // · `never` —— 吐完开场白就**再也不理 stdin**（读到 `stop` 也不退）：留给"宽限时间
    // 到点 → 强制结束"那一段。它一定会被杀掉，所以给个四分钟兜底；
    // · `crash` —— 吐完开场白立刻退 1：量"非 0 退出 = 异常退出"和那个预警框。
    //
    // NOTE: 必须排在**最前面**，连 QApplication 都不建（同上面那两支的理由：这个进程是被
    // 我们的 QProcess 起的，它不该去碰沙盒设置，也不该需要图形平台）。
    // NOTE: 输出必须**不缓冲** —— 缓冲住的话父进程的 `readyRead` 要等到进程退出才来，
    // 而"起来了"那一行恰恰是要在它还活着的时候读到的。
    if (launchedAsServerJava || (argc >= 3 && std::strcmp(argv[1], "--fake-server") == 0)) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        const std::string mode = launchedAsServerJava ? "startup-wait" : argv[2];
        std::fprintf(stdout, "[12:00:00] [Server thread/INFO]: Starting minecraft server\n");
        std::fprintf(stdout, "[12:00:01] [Server thread/INFO]: Preparing level \"world\"\n");
        if (mode == "startup-zero") {
            std::fprintf(stdout, "[Server thread/WARN]: **** FAILED TO BIND TO PORT!\n");
            return 0;
        }
        if (mode == "wrongjava-hang")
            std::fprintf(stdout, "Caused by: java.lang.UnsupportedClassVersionError: incompatible runtime\n");
        if (mode != "startup-wait" && mode != "wrongjava-hang")
            std::fprintf(stdout, "Done (1.234s)! For help, type \"help\"\n");
        std::fflush(stdout);
        if (mode == "crash") {
            return 1;
        }
        std::string line;
        while (std::getline(std::cin, line)) {
            if (mode == "never") {
                continue;  // 听见了，装作在忙
            }
            if (line == "stop") {
                std::fprintf(stdout,
                             "[12:00:02] [Server thread/INFO]: Stopping the server\n");
                std::fflush(stdout);
                return 0;
            }
            if (mode == "startup-wait" && line == "ready") {
                std::fprintf(stdout, "Done (1.234s)! For help, type \"help\"\n");
                continue;
            }
            if (mode == "startup-wait" && line == "exit-before-ready") {
                return 0;
            }
            // ASCII 标记：见 T41.3 那段 —— 窄字符串字面量按执行字符集写、对面按系统
            // 代码页读，在英文版 Windows 上对不上。
            std::fprintf(stdout, "[12:00:02] [Server thread/INFO]: [echo] %s\n", line.c_str());
            std::fflush(stdout);
        }
        // stdin 关了（父进程那一头收摊了）：当作被要求收工 —— 不许转在那儿把 exe 锁住。
        return 0;
    }

    QApplication app(argc, argv);
    Q_INIT_RESOURCE(icons);

    // 动效全关。这个工程量的是界面本身长什么样，不是它在动的时候长什么样；
    // 半路飞进来一段动画只会让判据时灵时不灵。规矩见 ui/Animations.hpp。
    Anim::setEnabled(false);
    runPromptProbeChecks();
    // ---------- auth-live：**真注册表 + 真网络** 的一次性探针 ----------
    // 只为回答"盘上那份令牌现在到底还能不能换出会话"。它读的必须是产品自己那份 QSettings
    // （HKCU\Software\WTFML\WTFML），所以**必须在沙盒重定向之前**分派出去 —— 晚了读到的
    // 就是 check-settings 里的空表，探针会伪装成"从来没登过"。跑完即退，不进默认那一跑
    // （默认那一跑要离线、要不碰用户配置）。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("auth-live")) {
        QApplication::setOrganizationName(QStringLiteral("WTFML"));  // 和 src/main.cpp 一致
        QApplication::setApplicationName(QStringLiteral("WTFML"));
        qInstallMessageHandler(nullptr);  // 别让 collectMessage 把 qWarning 收进断言桶里
        AuthManager auth;
        QString reason;
        const QString token = auth.loadSavedToken(&reason);
        std::printf("player=%s\n", qPrintable(auth.playerName()));
        std::printf("uuid=%s\n", qPrintable(auth.playerUuid()));
        std::printf("tokenChars=%s\n",
                    token.isEmpty() ? "<empty>" : qPrintable(QString::number(token.size())));
        std::printf("reason=%s\n", reason.isEmpty() ? "<none>" : qPrintable(reason));
        std::fflush(stdout);
        return token.isEmpty() ? 1 : 0;
    }

    // 和 src/main.cpp 钉同一个样式：这个工程量的是真主窗口的几何/可见性，那些值里有
    // 样式度量算出来的部分（sizeHint 等）。不钉住就成了"拿原生样式的结果去背书 Fusion
    // 的产品"。（放在装日志处理器之前，免得样式相关的消息混进断言用的日志里。）
    QApplication::setStyle(Theme::createStyle());
    // 从这一行起收集所有日志（MainWindow 构造里就开始写了，晚了就漏）。
    if (app.arguments().contains(QStringLiteral("range-resume-child"))) return runRangeChild(app.arguments());
    if (app.arguments().contains(QStringLiteral("segmented-download-check"))) return runRangeChecks();
    if (app.arguments().contains(QStringLiteral("segment-install-check"))) return runSegmentInstallChecks();
    if (app.arguments().contains(QStringLiteral("tool-features-check"))) return runToolFeatureChecks();
    if (app.arguments().contains(QStringLiteral("preset-install-check"))) return runPresetInstallChecks();
    if (app.arguments().contains(QStringLiteral("mod-action-panel-check"))) return runModActionPanelChecks();
    if (app.arguments().contains(QStringLiteral("java-manage-check"))) return runJavaManageChecks();
    if (app.arguments().contains(QStringLiteral("instance-upgrade-check"))) return runInstanceUpgradeChecks();
    if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QLatin1String("loader-install-live")) {
        const QString root = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        const QString mc = QString::fromLocal8Bit(argv[3]);
        const QString loader = QString::fromLocal8Bit(argv[4]).toLower();
        VersionManager::LoaderKind kind;
        if (loader == QLatin1String("fabric")) kind = VersionManager::LoaderKind::Fabric;
        else if (loader == QLatin1String("forge")) kind = VersionManager::LoaderKind::Forge;
        else if (loader == QLatin1String("neoforge")) kind = VersionManager::LoaderKind::NeoForge;
        else if (loader == QLatin1String("quilt")) kind = VersionManager::LoaderKind::Quilt;
        else return 2;
        if (!QDir().mkpath(root)) return 2;
        AppPaths::setMinecraftDir(root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + "/system-settings");
        app.setOrganizationName(QStringLiteral("WTFML-install-smoke"));
        app.setApplicationName(QStringLiteral("install-smoke"));
        VersionManager manager;
        manager.downloads()->setUseMirror(false);
        QEventLoop loop;
        bool finished = false, success = false, timedOut = false;
        QObject::connect(&manager, &VersionManager::loaderInstallMessage, [](const QString &text) {
            fprintf(stdout, "STAGE %s\n", qPrintable(text)); fflush(stdout);
        });
        QObject::connect(&manager, &VersionManager::versionDownloadFinished, &loop,
            [&](const QString &id, bool ok, const QString &reason) {
                finished = true; success = ok;
                fprintf(stdout, "RESULT %s success=%d %s\n", qPrintable(id), ok, qPrintable(reason)); fflush(stdout);
                loop.quit();
            });
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            fprintf(stdout, "TIMEOUT; incomplete install, not a pass\n"); fflush(stdout);
            manager.cancelActiveDownload(); loop.quit();
        });
        const int limitSeconds = argc >= 8 ? qBound(10, QString::fromLocal8Bit(argv[7]).toInt(), 900) : 180;
        deadline.start(limitSeconds * 1000);
        manager.installNamedLoader(QStringLiteral("smoke-%1-%2").arg(loader, mc), mc, kind,
            argc >= 7 ? QString::fromLocal8Bit(argv[6]) : QString(), QString::fromLocal8Bit(argv[5]));
        if (!finished) loop.exec();
        return timedOut ? 124 : finished ? (success ? 0 : 1) : 124;
    }
    if (app.arguments().contains(QStringLiteral("high-speed-download-check"))) { return MwHighSpeedDownloadCheck(app); }
    if (app.arguments().contains(QStringLiteral("download-retry-check"))) { return MwDownloadRetryCheck(); }
    if (app.arguments().contains(QStringLiteral("activity-selection-check"))) { return MwActivitySelectionCheck(); }
    if (app.arguments().contains(QStringLiteral("workspace-check"))) { return MwWorkspaceCheck(app); }
    g_previousHandler = qInstallMessageHandler(collectMessage);
    const auto restoreLogHandler = qScopeGuard([] { qInstallMessageHandler(g_previousHandler); });
    {
        QList<QThread *> writers;
        for (int i = 0; i < 8; ++i) {
            auto *writer = QThread::create([] {
                for (int n = 0; n < 500; ++n)
                    collectMessage(QtDebugMsg, QMessageLogContext(), QStringLiteral("log-race-fixture"));
            });
            writers.append(writer);
            writer->start();
        }
        // Read snapshots while writers append, not only after joining them.
        for (auto *writer : writers) {
            while (!writer->wait(1)) { const auto snapshot = messagesSnapshot(); Q_UNUSED(snapshot); }
            delete writer;
        }
        check(countOccurrences(messagesSnapshot(), QStringLiteral("log-race-fixture")) == 4000,
              "并发日志：八个工作线程写入与主线程读取不丢记录");
    }

    // ---------- 沙盒：QSettings 必须在**第一个 QSettings 对象出现之前**重定向 ----------
    // MainWindow 构造里就有 LauncherSettings / GlobalJavaSettings 在读盘，晚一步就晚了。
    // org 和 app 两个都必须设：INI 后端也会因 org 为空而 setStatus(AccessError)
    // （qsettings.cpp:1069-1073），那样是个"写得进去但状态报错"的假沙盒。
    QTemporaryDir processSandbox(QCoreApplication::applicationDirPath() + QStringLiteral("/mw-check-XXXXXX"));
    if (!processSandbox.isValid()) return 2;
    app.setProperty("wtfml.testDefaultMinecraftDir", processSandbox.filePath(QStringLiteral(".minecraft")));
    app.setProperty("wtfml.testAgentDataDir", processSandbox.filePath(QStringLiteral("agent")));
    AppPaths::setMinecraftDir(QString());
    const QDir root(processSandbox.path());
    const QString settingsDir = root.filePath(QStringLiteral("check-settings"));
    QDir(settingsDir).removeRecursively();
    QDir().mkpath(settingsDir);
    QApplication::setOrganizationName(QStringLiteral("WTFML-check"));
    QApplication::setApplicationName(QStringLiteral("mw_check"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root.filePath("system-settings"));
    requireIsolatedSettings(processSandbox.path(), "mw_check");
    check(AppPaths::defaultMinecraftDir() == root.filePath(QStringLiteral(".minecraft")) &&
              AppPaths::minecraftDir() == AppPaths::defaultMinecraftDir(),
          "主窗口测试默认数据根属于本进程临时目录");
    // 每个 run 都从"从没启动过任何版本"开始，否则上一轮留下的记录会让冷启动断言失效
    QFile::remove(settingsDir + QStringLiteral("/WTFML-check/mw_check.ini"));

    // ---------- 启动器级那几份账本：也不许跨趟（纳入稳定回归）----------
    // 它们落在 `AppPaths::defaultMinecraftDir()`，此测试入口已注入每进程独立根。
    // 它不跟当前游戏文件夹走：`TaskHistory::filePath()` 读的就是它，因为任务历史是**启动器级**
    // 账本 —— 用户换游戏文件夹它也不该跟着搬（见 `src/tasks/TaskHistory.hpp` 文件头那段）。
    // 于是问题来了：上一趟收尾时写下的终态任务会被这一趟**恢复出来**，`collapsedSummary()`
    // 还在它那 8 秒窗内时就会占着底栏中间那一格 ⇒ 那几条"没有下载也没有启动时那一格写着
    // 占位话"偶发红（真凶是**上一趟**的服务端世界备份「备份世界「world」」，不是这一次跑
    // 出来的东西）。`wtfml.folders` 同理：它是分组账本，留着的话 `seedDefaultGroups()`
    // 就不会种默认那五组，实例页那一批判据跟着变脸。
    //
    // ⇒ 和上面那份 QSettings 沙盒同一个形状：跑之前清掉，这一趟才和"干净装一次"同构。
    // （清了之后这一趟自己还会把它们写回来，所以下一趟仍然从干净开始 —— 幂等。）
    {
        const QString dataDir = AppPaths::defaultMinecraftDir();
        for (const QString &name : {QStringLiteral("wtfml.tasks"), QStringLiteral("wtfml.history"),
                                    QStringLiteral("wtfml.folders")}) {
            QFile::remove(dataDir + QLatin1Char('/') + name);
        }
    }
    if (const int live = runLiveProbeMode(argc, argv); live >= 0) {
        return live;
    }

    if (app.arguments().contains(QStringLiteral("update-center-check"))) { runUpdateCenterActionTests(); return g_fail == 0 ? 0 : 1; }
    if (app.arguments().contains(QStringLiteral("instance-start-actions-check"))) {
        runInstanceStartActionsTests();
        return g_fail == 0 ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("dropped-file-check"))) {
        runDroppedFileTests();
        return g_fail == 0 ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("resource-detail-check"))) {
        runResourceDetailPageTests();
        runInstanceFileResourceTests();
        return g_fail == 0 ? 0 : 1;
    }
    if (app.arguments().contains(QStringLiteral("folder-switching-check"))) {
        const QDir versions(AppPaths::versionsDir());
        makeInstalledVersion(versions, QStringLiteral("1.18.2"));
        makeInstalledVersion(versions, QStringLiteral("26.3"));
        makeInstalledVersion(versions, QStringLiteral("1.20.1"));
        makeVersionMeta(versions, QStringLiteral("1.20.1"),
                        R"({"name":"我的整合包","loaderName":"Forge","loaderVersion":"47.2.0"})");
        MainWindow window;
        const int result = MwFolderSwitchingChecks(root, window);
        return result != 0 ? result : g_fail == 0 ? 0 : 1;
    }

    // ---------- modpack-import-live：**真网络**跑一次整合包导入 ----------
    // mw_check.exe modpack-import-live "<整合包.zip>" "<实例名>" "<游戏根目录>"
    //
    // 存在的理由：四个工程验的都是离线那部分，唯一没验过的是"合法导入走完最后一步"
    // （要联机下加载器、再下清单里那几百个文件，几个 GB）。这一条用**产品自己的代码**
    // 真跑一遍：同一个 MainWindow、同一个 applyModpackImport()、同一个 VersionManager。
    //
    // 设置仍然走沙盒（绝不碰用户注册表），但**游戏根目录**指向 argv 给的真目录：下好的
    // 东西留在那儿，启动器里能直接看到这个实例。**跑完不清理** —— 一次真导入的产物
    // 就是结果本身，删掉等于白下（那几个 GB）。
    //
    // Java 在这里是**钉死**的（写进沙盒设置，AutoSelect 关掉）：整合包要 Forge，
    // Forge 的安装器自己是个 Java 程序，让"自动挑"在这条几小时的路上多一个变量不划算。
    // 自动档那条路有别的断言管着（Y3/AA7）。
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("modpack-import-live")) {
        const QString zipPath = QString::fromLocal8Bit(argv[2]);
        const QString instanceName = QString::fromLocal8Bit(argv[3]);
        const QString gameRoot =
            QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[4])).absoluteFilePath());
        const QString javaPath = argc >= 6 ? QDir::cleanPath(QString::fromLocal8Bit(argv[5]))
                                           : QString();

        std::printf("整合包：%s\n实例名：%s\n游戏根目录：%s\n",
                    qPrintable(zipPath), qPrintable(instanceName), qPrintable(gameRoot));
        if (!QFileInfo::exists(zipPath)) {
            std::printf("!! 压缩包不在：%s\n", qPrintable(zipPath));
            return 3;
        }
        // 游戏根目录写进沙盒设置：FolderManager 构造时读的就是这两个键，界面（MainWindow）
        // 拿到 currentFolderChanged 之后会自己调 AppPaths::setMinecraftDir()。
        {
            QSettings sandbox;
            sandbox.setValue(QStringLiteral("Launcher/Folders"),
                             QStringList({gameRoot}));
            sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), gameRoot);
            if (!javaPath.isEmpty()) {
                sandbox.setValue(QStringLiteral("Java/Path"), javaPath);
                sandbox.setValue(QStringLiteral("Java/AutoSelect"), false);
            }
            sandbox.sync();
            std::printf("沙盒设置：%s\n", qPrintable(sandbox.fileName()));
        }

        // 两种整合包各读各的清单（ 起 CurseForge 那一份也能导）。这一发是**真导入**
        // 探针，所以格式必须自己认出来 —— 让调用的人还要记着加一个参数，迟早会记错。
        QByteArray indexJson;
        QString readError;
        ModpackImport::Format format = ModpackImport::Format::Unknown;
        for (const auto candidate : {ModpackImport::Format::Modrinth,
                                     ModpackImport::Format::CurseForge}) {
            const QString entryName = candidate == ModpackImport::Format::Modrinth
                                          ? ModpackImport::modrinthIndexName()
                                          : ModpackImport::curseForgeManifestName();
            const ZipUtil::EntryStatus found =
                ZipUtil::readEntry(zipPath, entryName, &indexJson, &readError);
            std::printf("读清单（%s）：status=%d bytes=%d %s\n", qPrintable(entryName),
                        static_cast<int>(found), int(indexJson.size()), qPrintable(readError));
            if (found == ZipUtil::EntryStatus::Ok) {
                format = candidate;
                break;
            }
            indexJson.clear();
        }
        if (format == ModpackImport::Format::Unknown) {
            std::printf("!! 两份清单一个都没有，导入无从谈起\n");
            return 3;
        }

        // 自定义枚举要注册了才能过 Q_ARG（见下面 AA 那一段的说明）。这里**必须自己注一次**：
        // 这一条探针走的是 mode 分支，在那个注册点**之前**就返回了。
        qRegisterMetaType<ModpackImport::Format>("ModpackImport::Format");

        MainWindow liveWindow;
        liveWindow.show();
        QApplication::processEvents();
        std::printf("AppPaths 认的游戏根目录：%s\n",
                    qPrintable(AppPaths::minecraftDir()));
        const QString versionsRoot = AppPaths::versionsDir();
        const int dirsBefore = QDir(versionsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        std::printf("versions 下现有 %d 个目录\n", dirsBefore);

        bool started = false;
        const bool invoked = QMetaObject::invokeMethod(
            &liveWindow, "applyModpackImport", Qt::DirectConnection, Q_RETURN_ARG(bool, started),
            Q_ARG(QString, zipPath), Q_ARG(QString, instanceName), Q_ARG(QByteArray, indexJson),
            Q_ARG(ModpackImport::Format, format));
        std::printf("applyModpackImport：invoked=%d started=%d\n", invoked ? 1 : 0,
                    started ? 1 : 0);
        if (!invoked || !started) {
            std::printf("!! 导入没开始（上面那条被拦下了）\n");
            return 1;
        }

        // 轮询：把日志流出来、把模态窗点掉（失败那条路会弹一个，没人点就永远卡住）、
        // 每 30 秒报一次心跳（下载进度**不进日志**，只能从进度条上读）。
        QElapsedTimer clock;
        clock.start();
        int printed = 0;
        bool finished = false;
        bool succeeded = false;
        const qint64 kDeadlineMs = qint64(6) * 3600 * 1000;  // 六个钟头：几个 GB 走慢网
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (InlineMessageBox *box = currentPrompt()) {
                InlineMessageBox *forLog = box;
                recordPrompt(forLog);
                std::printf("[%7.1fs][弹窗] %s\n        %s\n", clock.elapsed() / 1000.0,
                            qPrintable(g_modalTitle), qPrintable(g_modalText));
                std::fflush(stdout);
                box->close();
            }
            for (; printed < messagesSnapshot().size(); ++printed) {
                const QString line = messagesSnapshot().at(printed);
                const bool done = line.contains(QStringLiteral("导入好了"));
                const bool failed = line.contains(QStringLiteral("没有导入"));
                std::printf("[%7.1fs] %s\n", clock.elapsed() / 1000.0, qPrintable(line));
                std::fflush(stdout);
                if (done || failed) {
                    finished = true;
                    succeeded = done;
                }
            }
            if (!finished && clock.elapsed() % 30000 < 1000) {
                // 进度条只有一个标量（value/maximum），「失败」和「跳过」在它上面看不出来 ——
                // 一条"完成数在涨、盘上却一个文件都没有"的路，光看它是分辨不出
                // 「跳过（文件本来就在）」和「下完了」的。所以四个数一起报。
                int done = -1;
                int total = -1;
                if (auto *bar = liveWindow.findChild<QProgressBar *>(
                        MainWindowUI::progressBarName())) {
                    done = bar->value();
                    total = bar->maximum();
                }
                int dmDone = -1;
                int dmTotal = -1;
                int dmFailed = -1;
                int dmSkipped = -1;
                if (auto *dm = liveWindow.findChild<DownloadManager *>()) {
                    const DownloadStats st = dm->stats();
                    dmDone = st.done;
                    dmTotal = st.total;
                    dmFailed = st.failed;
                    dmSkipped = st.skipped;
                }
                const QDir instDir(versionsRoot + QLatin1Char('/') + instanceName);
                qint64 bytes = 0;
                const QFileInfoList entries =
                    instDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo &info : entries) {
                    if (info.isDir()) {
                        const QDir sub(info.absoluteFilePath());
                        for (const QFileInfo &file :
                             sub.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
                            bytes += file.size();
                        }
                    } else {
                        bytes += info.size();
                    }
                }
                std::printf("[%7.1fs][心跳] 进度条=%d/%d 下载=完成 %d/%d 失败 %d 跳过 %d "
                            "实例目录=%lld MB\n",
                            clock.elapsed() / 1000.0, done, total, dmDone, dmTotal, dmFailed,
                            dmSkipped, static_cast<long long>(bytes / 1024 / 1024));
                std::fflush(stdout);
            }
            if (finished) {
                std::printf("[%7.1fs] 终态：%s\n", clock.elapsed() / 1000.0,
                            succeeded ? "导入好了" : "没有导入");
                QApplication::quit();
                return;
            }
            if (clock.elapsed() > kDeadlineMs) {
                std::printf("[%7.1fs] 超时（六个钟头），探针退出，进程里那个导入没停\n",
                            clock.elapsed() / 1000.0);
                QApplication::quit();
            }
        });
        poll.start(1000);
        QApplication::exec();
        poll.stop();

        std::printf("\n===== 实例目录 %s =====\n", qPrintable(versionsRoot + QLatin1Char('/') +
                                                               instanceName));
        const QDir instDir(versionsRoot + QLatin1Char('/') + instanceName);
        for (const QFileInfo &info : instDir.entryInfoList(QDir::Files | QDir::Dirs |
                                                           QDir::NoDotAndDotDot)) {
            std::printf("  %s%s\n", qPrintable(info.fileName()),
                        info.isDir() ? "/" : qPrintable(QStringLiteral("  %1 B").arg(info.size())));
        }
        std::printf("退出码：%d\n", succeeded ? 0 : 1);
        return succeeded ? 0 : 1;
    }

    // ---------- multimc-import-live：**真网络**导入一个 MultiMC/Prism 实例包 ----------
    // mw_check.exe multimc-import-live "<包.zip>" "<实例名>" "<游戏根目录>"
    //
    // 为什么另开一个：modpack-import-live 只认 Modrinth / CurseForge 那两份清单，
    // 而 GTNH 2.8.4 那种包是 MultiMC 导出的（mmc-pack.json + patches/ + .minecraft/）。
    // 它的导入入口在 MainWindow::importModpackFromFile()，路上有两问（实例名、开始导入），
    // 这里用定时器把这两问**按产品给的默认值答掉**（名字填 argv 那个），其余提示
    // （失败原因）只记录、点掉。走的是和界面完全同一条栈：applyModpackImport 那一段
    // 之后的一切（含导入成功后的首启资源预取）都会真的跑。
    //
    // 2.1.6c 用它：真 GTNH 包、空数据根、产品自己下 Java、产品自己预取首启资源。
    if (argc >= 5 && QString::fromLocal8Bit(argv[1]) == QLatin1String("multimc-import-live")) {
        const QString zipPath = QString::fromLocal8Bit(argv[2]);
        const QString instanceName = QString::fromLocal8Bit(argv[3]);
        const QString gameRoot =
            QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[4])).absoluteFilePath());
        std::printf("整合包：%s\n实例名：%s\n游戏根目录：%s\n", qPrintable(zipPath),
                    qPrintable(instanceName), qPrintable(gameRoot));
        std::fflush(stdout);
        if (!QFileInfo::exists(zipPath)) {
            std::printf("!! 压缩包不在：%s\n", qPrintable(zipPath));
            return 3;
        }
        {
            QSettings sandbox;
            sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({gameRoot}));
            sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), gameRoot);
            sandbox.sync();
            std::printf("沙盒设置：%s\n", qPrintable(sandbox.fileName()));
        }
        MainWindow liveWindow;
        liveWindow.show();
        QApplication::processEvents();
        std::printf("AppPaths 认的游戏根目录：%s\n", qPrintable(AppPaths::minecraftDir()));

        QElapsedTimer clock;
        clock.start();
        int printed = 0;
        bool finished = false;
        bool succeeded = false;
        bool invoked = false;
        bool imported_ok = false;
        const qint64 kDeadlineMs = qint64(8) * 3600 * 1000;  // 真导入：下载 + 解压
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&] {
            // 两问按产品默认值答掉：名字那一问把输入框填成 argv 给的名字，其余点第一颗
            // （「是」/「确定」）。警告框只有一颗，点掉就是"我知道了"。
            if (InlineMessageBox *box = currentPrompt()) {
                recordPrompt(box);
                std::printf("[%7.1fs][弹窗] %s\n        %s\n", clock.elapsed() / 1000.0,
                            qPrintable(g_modalTitle), qPrintable(g_modalText));
                std::fflush(stdout);
                if (auto *edit = box->findChild<QLineEdit *>()) {
                    edit->setText(instanceName);
                }
                // 首启资源那两问（"没预取完，再试一次吗？"）点**最后一颗**（知道了）：
                // 探针要的是记录现场，不是在这一跑里无限重试。
                int index = 0;
                if (g_modalText.contains(QStringLiteral("首次启动资源"))) {
                    index = qMax(0, box->findChildren<QPushButton *>().size() - 1);
                }
                if (auto *button = box->findChild<QPushButton *>(InlineMessageBox::buttonName(index))) {
                    button->click();
                } else {
                    box->close();
                }
            }
            for (; printed < messagesSnapshot().size(); ++printed) {
                const QString line = messagesSnapshot().at(printed);
                const bool imported = line.contains(QStringLiteral("导入好了"));
                const bool failed = line.contains(QStringLiteral("没有导入"));
                // 首启资源预取在"导入完成"那一问关掉之后才开始，所以不能一见「导入好了」
                // 就退 —— 那正是它会被漏掉的地方。预取有它自己那几句终态话。
                const bool prefetchDone = line.contains(QStringLiteral("首次启动资源已预取"))
                                          || line.contains(QStringLiteral("首次启动资源下载不完整"))
                                          || line.contains(QStringLiteral("首次启动资源已齐全"))
                                          || line.contains(QStringLiteral("的首次启动资源预取。"))
                                          || line.contains(QStringLiteral("未预取"));
                std::printf("[%7.1fs] %s\n", clock.elapsed() / 1000.0, qPrintable(line));
                std::fflush(stdout);
                if (imported) {
                    imported_ok = true;
                }
                if (failed) {
                    finished = true;
                    succeeded = false;
                }
                if (imported_ok && prefetchDone) {
                    finished = true;
                    succeeded = true;
                }
            }
            if (!finished && clock.elapsed() % 30000 < 1000) {
                int done = -1;
                int total = -1;
                if (auto *bar = liveWindow.findChild<QProgressBar *>()) {
                    done = bar->value();
                    total = bar->maximum();
                }
                std::printf("[%7.1fs] 进度 %d/%d\n", clock.elapsed() / 1000.0, done, total);
                std::fflush(stdout);
            }
            if (finished || clock.elapsed() > kDeadlineMs) {
                poll.stop();
                QCoreApplication::quit();
            }
        });
        poll.start(200);
        // **必须排到事件循环里再调**：importModpackFromFile() 在"问名字""点确认"那两问上
        // 是**同步**的（进嵌套事件循环），DirectConnection 直接调会把这条栈钉死在第一问上，
        // 定时器根本没机会起来 —— 实测就是那样卡住的（进程活着、一行日志都没有）。
        QTimer::singleShot(0, &liveWindow, [&] {
            invoked = QMetaObject::invokeMethod(
                &liveWindow, "importModpackFromFile", Qt::DirectConnection,
                Q_ARG(QString, zipPath), Q_ARG(QString, instanceName));
            std::printf("importModpackFromFile：invoked=%d\n", invoked ? 1 : 0);
            std::fflush(stdout);
            if (!invoked) {
                poll.stop();
                QCoreApplication::quit();
            }
        });
        QCoreApplication::exec();
        std::printf("RESULT ok=%d elapsed=%llds\n", succeeded ? 1 : 0,
                    static_cast<long long>(clock.elapsed() / 1000));
        return succeeded ? 0 : 1;
    }

    // ---------- java-self-managed：用产品自己的代码下一份自管 Java ----------
    // mw_check.exe java-self-managed "<游戏根目录>" <主版本>
    //
    // 2.1.6c 要的是"启动器自己管的那份运行时"（<数据目录>/java/<主版本>/bin/java.exe），
    // 而不是本机装的 JDK。产品里这条下载只出现在 Java 管理页那四颗按钮和启动前那段同步
    // 等待里（`GlobalJavaSettings::downloadJavaFromMojang`），验证工程够不着 —— 所以留这
    // 一个一次性探针：**落点、解压、`java.exe -version` 自检全走产品代码**，这里只负责
    // 把数据根指过去、把拿到的路径打出来。不进 CTest（要联网、几百 MB）。
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("java-self-managed")) {
        const QString gameDir =
            QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath());
        const int major = QString::fromLocal8Bit(argv[3]).toInt();
        if (gameDir.isEmpty() || major <= 0) {
            std::printf("!! 用法：mw_check.exe java-self-managed <游戏根目录> <主版本>\n");
            return 3;
        }
        QDir().mkpath(gameDir);
        AppPaths::setMinecraftDir(gameDir);
        std::printf("游戏根目录：%s\nJava 主版本：%d\n自管落点：%s\n", qPrintable(gameDir), major,
                    qPrintable(AppPaths::javaVersionDir(major)));
        std::fflush(stdout);
        QString javaPath;
        const bool ok = GlobalJavaSettings::downloadJavaFromMojang(major, &javaPath);
        std::printf("RESULT ok=%d java=%s\n", ok ? 1 : 0, qPrintable(javaPath));
        return ok ? 0 : 1;
    }

    // ---------- instance-scan：**只读**扫一遍真实例，看导出那两格读出来是什么 ----------
    // mw_check.exe instance-scan <数据目录>
    //
    // 存在的理由： 用户在真机上撞的那句「导出失败：读不出 海波篮子 基于哪个原版」，
    // 真因是"PCL 那种把整份 json 合进实例目录"的实例 —— 链上没有加载器目录名、json 里也没有
    // inheritsFrom，于是两格都读不出来。修完之后要能**照着他的盘**重跑一遍，看这一档现在是
    // 什么样子（而不是只信我们自己在沙箱里摆的夹具）。
    //
    // **一个字节都不写**：实例目录、wtfml.meta、版本 json 全走读路径。用法就是
    // `mw_check.exe instance-scan E:/MC/.minecraft`（那台机器上的盘符照写）。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("instance-scan")) {
        const QString dataDir = QString::fromLocal8Bit(argv[2]);
        AppPaths::setMinecraftDir(dataDir);
        const QDir versionsRoot(dataDir + QStringLiteral("/versions"));
        const QStringList ids =
            versionsRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        std::printf("数据目录：%s（%d 个实例）\n", qPrintable(dataDir), int(ids.size()));

        VersionManager manager;
        // **先取一次在线清单**（产品里启动时也会取，见 MainWindow::onVersionsFetched 那句
        // 「已更新在线版本列表」）。它算作判据的一半：`rd-132211` / `c0.30_01c` 这些老版本号
        // 不在清单里就认不出来（不再以数字开头的判据挡着的缘故），所以探针不取清单的话
        // 看到的结果比产品里更差、也就少了意义。取不到（离线）就照旧。
        QStringList onlineIds;
        {
            QEventLoop loop;
            QObject::connect(&manager, &VersionManager::versionsFetched, &loop,
                             [&](const QStringList &list) {
                                 onlineIds = list;
                                 loop.quit();
                             });
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            manager.fetchAvailableVersions();
            loop.exec();
            std::printf("在线清单：%d 条%s\n", int(onlineIds.size()),
                        onlineIds.isEmpty() ? "（没取到：下面按「长得像版本号」那条判据算）" : "");
        }
        int gameOk = 0;
        int gameEmpty = 0;
        int loaderOk = 0;
        int loaderRefused = 0;
        int loaderVanilla = 0;
        for (const QString &id : ids) {
            const QString game = manager.instanceGameVersion(id);
            QString key;
            QString version;
            const QString problem = manager.instanceLoaderInfo(id, &key, &version);
            game.isEmpty() ? ++gameEmpty : ++gameOk;
            QString loader;
            if (!problem.isEmpty()) {
                ++loaderRefused;
                loader = QStringLiteral("拒绝：") + problem;
            } else if (key.isEmpty()) {
                ++loaderVanilla;
                loader = QStringLiteral("(原版)");
            } else {
                ++loaderOk;
                loader = key + QLatin1Char('/') + version;
            }
            // 另一条候选判据的对照：**只认清单里有的**（清单在手时以它为准）。这一列是拿来
            // 比较"放行"与"严格"两种规矩在真实盘上差在哪几条的 —— 差出来的那几条就是
            // "目录名长得像版本号、但 Mojang 根本没有这个 id"（`20w14∞` / `26.2hyperland`
            // 这种），写进清单收包的人那边装不回来。
            const QString strict =
                onlineIds.isEmpty() ? QStringLiteral("-")
                                    : (onlineIds.contains(game) ? QStringLiteral("在") : QStringLiteral("不在"));
            std::printf("  %-34s game=%-14s 清单=%-4s loader=%s\n", qPrintable(id),
                        qPrintable(game.isEmpty() ? QStringLiteral("(读不出)") : game),
                        qPrintable(strict), qPrintable(loader));
        }
        std::printf("小结：原版读出来 %d / 读不出 %d；加载器认出 %d / 拒绝 %d / 按原版 %d\n",
                    gameOk, gameEmpty, loaderOk, loaderRefused, loaderVanilla);
        return gameEmpty == 0 && loaderRefused == 0 ? 0 : 1;
    }

    // ---------- update-target：**只读**看「检查更新」那两个筛参数在真机上读出来是什么 -----
    // mw_check.exe update-target <数据目录> [实例目录名]
    //
    // 存在的理由： 用户报「hyperlands-new 里明明有不少模组要更新，界面上一个都
    // 没有」。那一趟要的两个**筛参数**（游戏版本 / 加载器）在产品里是
    // `MainWindow::dependencyTargetFor()` 算出来的，而它**原来**读的是
    // `VersionManager::displayParts().realVersion` —— 那一格**只有 `wtfml.meta` 里写着
    // `realVersion` 才有值**，而**没有任何代码会写它**（见 VersionDisplayParts 那段）。
    // 于是绝大多数实例（PCL / HMCL 那类把整份 json 合进实例目录的，用户盘上正是大多数）
    // 递上去的是一个**空串**，而"空 = 不筛"：问回来的版本列表是"这个项目最新的那一版"，
    // 和这个实例的游戏版本**没有任何关系**（实测：Hyperlands-new 空着报 40 条可更新，
    // 递上真版本 `1.21.11` 之后是 12 条）。
    //
    // **一个字节都不写**：实例目录、wtfml.meta、版本 json 全走读路径。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("update-target")) {
        const QString dataDir = QString::fromLocal8Bit(argv[2]);
        AppPaths::setMinecraftDir(dataDir);
        const QDir versionsRoot(dataDir + QStringLiteral("/versions"));
        QStringList ids = versionsRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (argc >= 4) {
            const QString only = QString::fromLocal8Bit(argv[3]);
            ids = ids.filter(only); // 只给了一段子串就只打这几个（找那一个实例时用）
        }
        VersionManager manager;
        // 在线清单：`isKnownGameVersion()` 那一半判据要用它（和产品启动时同一发）。
        {
            QEventLoop loop;
            QObject::connect(&manager, &VersionManager::versionsFetched, &loop,
                             [&](const QStringList &) { loop.quit(); });
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            manager.fetchAvailableVersions();
            loop.exec();
        }
        int emptyTarget = 0;
        int emptyOld = 0;
        std::printf("数据目录：%s\n", qPrintable(QDir::toNativeSeparators(dataDir)));
        std::printf("  %-34s %-14s %-14s %s\n", "实例", "今天递的", "旧判据(realVersion)",
                    "加载器");
        for (const QString &id : ids) {
            // ① 产品今天递的那个（`dependencyTargetFor()` 那一句，逐字照抄）。
            const QString target = manager.instanceGameVersion(id);
            // ② 旧判据那一路（留着当对照）。
            const VersionDisplayParts parts = manager.displayParts(id);
            // 加载器：`instanceLoaderName()` 那两段（meta 里那格 → 回扣版本 json），
            // 再 `loaderGroupKey()`（小写）—— 递的就是这个。
            QString loader = parts.loaderName;
            if (loader.isEmpty()) {
                QString name;
                QString version;
                if (manager.instanceModLoader(id, &name, &version)) {
                    loader = name;
                }
            }
            if (target.trimmed().isEmpty()) {
                ++emptyTarget;
            }
            if (parts.realVersion.trimmed().isEmpty()) {
                ++emptyOld;
            }
            std::printf("  %-34s %-14s %-14s %s\n", qPrintable(id),
                        qPrintable(target.isEmpty() ? QStringLiteral("(空)") : target),
                        qPrintable(parts.realVersion.isEmpty() ? QStringLiteral("(空)")
                                                               : parts.realVersion),
                        qPrintable(loader.isEmpty() ? QStringLiteral("(认不出)")
                                                    : loader.toLower()));
        }
        std::printf("小结：%d 个实例里，产品今天**递不出游戏版本**的有 %d 个"
                    "（换判据之前是 %d 个）\n",
                    int(ids.size()), emptyTarget, emptyOld);
        return emptyTarget == 0 ? 0 : 1;
    }

    // ---------- cf-resolve-live：**真网络**跑一次"号 → 文件"（只问两发，不下任何文件）-----
    // mw_check.exe cf-resolve-live [项目号:文件号] …
    // 不给就默认问 JEI（238222）那一对真号。
    //
    // 存在的理由：CurseForge 那条路的**其余部分**都验过了（清单解析 / 请求体 / 答复解析 /
    // 落点 / 界面接线，见 verify 的 ZG 节和 mw_check 的 AA9/AA10），那些用的都是我们自己
    // 造的答复。唯一没验过的是**真接口**：当下那台社区镜像认不认我们这个正文形状
    // （键名、号是数字）、回不含 `hashes.algo==1` 和 `downloadUrl`、以及一条真地址长什么样。
    // **这个探针问的就是 curseForgeApiBase() 当下指的那一台**（不填 key 时 = 镜像），
    // 所以"镜像又换了一台"这件事一跑它就知道。
    // 这一条只问两发（几 KB），**不下载任何文件**，所以几分钟也等不出来一个 GB。
    //
    // 用产品自己的代码：同一个 VersionManager::resolveCurseForgeFiles、同一份拼地址与
    // 解析（不另抄一遍）。打印的是"换回来几条、落点、三种计数的数"。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("cf-resolve-live")) {
        QList<ModpackImport::PackRef> refs;
        for (int i = 2; i < argc; ++i) {
            const QString spec = QString::fromLocal8Bit(argv[i]);
            const QStringList parts = spec.split(QLatin1Char(':'));
            if (parts.size() != 2) {
                std::printf("!! 认不出这一对：%s（要写成 项目号:文件号）\n", qPrintable(spec));
                return 3;
            }
            refs.append({parts.at(0).toInt(), parts.at(1).toInt()});
        }
        if (refs.isEmpty()) {
            refs.append({238222, 8971390});
        }

        // 清单里那一条（这一发只要号，剩下的字段随便填得像样）。
        QByteArray filesJson;
        for (const ModpackImport::PackRef &ref : refs) {
            if (!filesJson.isEmpty()) {
                filesJson += ',';
            }
            filesJson += QByteArrayLiteral("{\"projectID\":") + QByteArray::number(ref.projectId) +
                         QByteArrayLiteral(",\"fileID\":") + QByteArray::number(ref.fileId) +
                         QByteArrayLiteral("}");
        }
        const QByteArray manifest = QByteArrayLiteral("{\"manifestVersion\":1,\"manifestType\":"
                                                     "\"minecraftModpack\",\"name\":\"真号探针\","
                                                     "\"version\":\"1.0\",\"minecraft\":"
                                                     "{\"version\":\"1.20.1\"},\"files\":[") +
                                    filesJson + QByteArrayLiteral("]}");
        const ModpackImport::Result parsed = ModpackImport::parseCurseForgeManifest(manifest);
        if (!parsed.ok) {
            std::printf("!! 自造的清单解析不过：%s\n", qPrintable(parsed.problem));
            return 3;
        }
        ModpackImport::Pack pack = parsed.pack;
        std::printf("要问的：%d 个号，接口：%s\n", pack.pendingRefs.size(),
                    qPrintable(ModpackImport::curseForgeApiBase(QString())));

        VersionManager manager;
        QObject::connect(&manager, &VersionManager::modpackImportMessage, &manager,
                         [](const QString &message) {
                             std::printf("[日志] %s\n", qPrintable(message));
                         });
        QElapsedTimer clock;
        clock.start();
        const QString problem = manager.resolveCurseForgeFiles(&pack);
        const qint64 tookMs = clock.elapsed();

        if (!problem.isEmpty()) {
            std::printf("!! 没换成（%lld ms）：\n%s\n", static_cast<long long>(tookMs),
                        qPrintable(problem));
            return 1;
        }
        std::printf("换成了（%lld ms）：%d 条\n", static_cast<long long>(tookMs), pack.files.size());
        for (const ModpackImport::PackFile &file : pack.files) {
            std::printf("  %s\n    地址 %s\n    sha1 %s  字节 %lld\n", qPrintable(file.path),
                        qPrintable(file.urls.value(0)), qPrintable(file.sha1),
                        static_cast<long long>(file.size));
        }
        std::printf("没换到的：作者不许代下 %d / 查不到 %d / 跳过 %d\n", pack.filesNotDistributable,
                    pack.filesMissing, pack.filesSkipped);
        const bool complete =
            pack.files.size() + pack.filesNotDistributable + pack.filesMissing == refs.size();
        std::printf(complete ? "账对得上（换到的 + 两种没换到的 = 问出去的）\n"
                             : "!! 账对不上：有三条号不见了\n");
        return complete && !pack.files.isEmpty() ? 0 : 1;
    }

    // ---------- resource-live：**真网络**跑一次"从资源表里装一个资源" ----------
    // mw_check.exe resource-live [类] [搜索词] [来源]
    // 类：mod（默认）/ datapack / resourcepack / shader —— 每类的落点目录各不相同
    // 来源：modrinth（默认）/ curseforge —— **这一格是 加的**，理由见下面
    // 拨来源那一段：不给它这一节永远只走 Modrinth，CurseForge 那条路真网络上一次
    // 都没跑过（两个验证工程里全是假接口）。
    //
    // 存在的理由：#45~#49 那几节验的都是"界面搭对了 / 取数接上了"，而**点第三层那一下
    // 最后在盘上落下什么**从没跑过一次 —— ResourceStore::fetchVersions → 挑 primaryFile
    // → VersionManager::downloadResourceFiles → DownloadManager 这条线在两个验证工程里
    // 都只被读过，没有一处被执行过。这一节用产品自己的代码真跑一遍：同一个 MainWindow、
    // 同一棵资源树、同一串 itemClicked 信号，落盘到沙盒里那个假实例的 mods/ /
    // datapacks/ / resourcepacks/ / shaderpacks/ 下（哪个是 folderForKind 说了算，
    // 测试不另抄一份）。
    //
    // 整合包**故意不在这一节里**：那一路下完紧接着就走导入（要问实例名、挑 Java，然后下
    // 清单里几百个文件、几个 GB），跑一次几个钟头，而且会在盘上留下一个真实例。它有自己的
    // 一次性探针（modpack-import-live）。
    //
    // 沙盒：设置仍然走沙盒（绝不碰用户注册表），但游戏根目录**另造一份**
    // `<exe 目录>/resource-live/.minecraft` —— 不能借用默认那一份：这一跑会在 versions/
    // 里多出一个实例、几个 MB 的产物，而默认那一跑的"下拉框里有几个版本"是有断言的
    // （多一个实例就红）。这一份是**专门给这一节的**，跑完留着当证据（产物就是结果本身）。
    // 每次跑只清**这一类的子目录**（不清整份）：每类各跑一次，几个目录都留在盘上；
    // 而"文件在不在"那条断言只认这一类的目录，上一跑的同名残留正好被这一清清掉。
    //
    // 入口：**从实例设置里那颗「下载」进来**（和用户一样），不是直接把左栏拨到那一行。
    // 理由见下面那一段（不是从实例进来的话，第三步会弹一个原生「下载到哪个文件夹」框，
    // 这个探针点不掉它）。
    //
    // 退出码：0 = 全绿；1 = 有断言红；2 = 网络这一关没过（搜索没结果/限流/超时，**不是**
    // 产品的错，别去改代码）；3 = 环境没搭起来（类名不认、控件找不齐、实例没选中）。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("resource-live")) {
        // 不写类就是模组（最常用的那一类，也是"点开模组后是选择游戏版本"那句原话里那个）。
        ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
        QStringList extraKinds;
        if (argc >= 3) {
            const QString asked = QString::fromLocal8Bit(argv[2]);
            if (asked == QLatin1String("modpack")) {
                std::printf("!! 整合包那一路下完接着就走导入（问实例名、挑 Java、下几个 GB），"
                            "不适合放进这一节。它有自己的一次性探针：\n"
                            "   mw_check.exe modpack-import-live <包.zip> <实例名> <游戏根目录>\n");
                return 3;
            }
            if (!ResourceCatalog::kindFromCategoryKey(asked, &kind)) {
                extraKinds << asked;
            }
        }
        if (!extraKinds.isEmpty()) {
            std::printf("!! 类只认 mod / datapack / resourcepack / shader"
                        "（整合包见上面那句）：收到「%s」\n",
                        qPrintable(extraKinds.first()));
            return 3;
        }
        // 不给搜索词就用这一类里最常见的那一个 —— 探针要的是"有一条能点到底的东西"，
        // 不是某个特定的包。
        QString defaultQuery = QStringLiteral("sodium");
        if (kind == ResourceCatalog::Kind::ResourcePack) {
            defaultQuery = QStringLiteral("faithful");
        } else if (kind == ResourceCatalog::Kind::Shader) {
            defaultQuery = QStringLiteral("complementary");
        } else if (kind == ResourceCatalog::Kind::DataPack) {
            defaultQuery = QStringLiteral("terralith");
        }
        const QString query = argc >= 4 ? QString::fromLocal8Bit(argv[3]) : defaultQuery;
        // 可选第 4 个参数：**来源**（modrinth / curseforge）。不给 = 界面上那个默认（Modrinth）。
        //
        // 为什么加它：CurseForge 那一条路（搜索 → 版本清单 → 下载 → 落盘）在两个验证工程里
        // **全是拿假接口验的**（mw_check 的 ZH/ZI 那几节、verify 的 Z7e），真网络上一次都没
        // 跑通过 —— 而它和 Modrinth 那一套的接线处处不同（`searchFilter=` 那一段、`{"data":[…]}`、
        // 文件地址来自 `downloadUrl` + `hashes[algo==1]`、类目号靠 `classId`，见
        // ResourceCatalog 文件头那张表）。不跑这一趟，"CurseForge 打没打通"就只能靠读代码猜。
        // 给了这个参数，那条路才是**真的**走了一遍（原来那条路的默认值走的是界面上那一档，
        // 也就是 Modrinth —— 不改来源就永远验不到 CF）。
        ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
        if (argc >= 5) {
            const QString asked = QString::fromLocal8Bit(argv[4]);
            if (!ResourceCatalog::sourceFromKey(asked, &source)) {
                std::printf("!! 来源只认 modrinth / curseforge：收到「%s」\n", qPrintable(asked));
                return 3;
            }
        }
        const QString instanceId = QStringLiteral("resource-live-试装");
        const QString liveRoot = root.filePath(QStringLiteral("resource-live/.minecraft"));
        const QString key = ResourceCatalog::kindKey(kind);
        const QString folder = ResourceCatalog::folderForKind(kind);
        // 只清**这一类**的落点目录：每类各跑一次时，另几类的产物要留着（那正是想看的东西），
        // 而这一类的旧文件必须清掉 —— "文件在不在盘上"那条断言只认这一类的目录，留着
        // 上一跑的同名文件会让它当场变成假绿。
        const QString productDir = liveRoot + QStringLiteral("/versions/") + instanceId +
                                   QLatin1Char('/') + folder;
        QDir(productDir).removeRecursively();
        QDir().mkpath(liveRoot);
        {
            // 和 modpack-import-live 同一套：FolderManager 构造时读的就是这两个键，
            // 窗口拿到 currentFolderChanged 之后自己会调 AppPaths::setMinecraftDir()。
            QSettings sandbox;
            sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({liveRoot}));
            sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), liveRoot);
            sandbox.sync();
        }
        makeInstalledVersion(QDir(liveRoot + QStringLiteral("/versions")), instanceId);
        // 模组以外的几类：给这个假实例**写一个模组加载器**进去。
        //
        // 为什么（ 加数据包时补的）：点第三层那一下前面有一道「加载器对不上」的闸，
        // 而它只有在"实例读得出加载器"时才会判 —— 裸实例永远放行，所以这一节**从来没走过
        // 那道闸**。写上一个之后，这几类才是真的在判：
        // · 数据包那一版清单里 `loaders` 是 `["datapack"]`、资源包是 `["minecraft"]` ——
        // 它们跟"这个实例装的是哪个模组加载器"半点关系没有。那道闸原来照 `size()==1`
        // 一律拦，于是给一个装了加载器的实例装数据包/资源包会**每次都弹一次**
        // 「这个实例是 Fabric，而这一版是给 datapack 用的」——那句提示说不通，而且弹多了
        // 用户就再也不看这类提醒了（真正该拦的那次也跟着被忽略）。这一节现在把它钉住：
        // 修好之后这几类会一路走到「下载完成」，没修好就会停在一个「加载器对不上」的框上
        // （下面那条"弹的是「下载完成」"当场变红）。
        // · 模组那一类**不写**：它的落点（题中之意）正是那道闸该管的事，而默认那几个搜索词
        // 是给"能一路下到底"挑的 —— 写个加载器进去反而可能因为撞上一个别的加载器的模组
        // 而在这条探针上弹出框来（那是对的行为，但不是这一节要看的）。
        if (kind != ResourceCatalog::Kind::Mod) {
            makeVersionMeta(QDir(liveRoot + QStringLiteral("/versions")), instanceId,
                            R"({"loaderName":"Fabric","loaderVersion":"0.15.11"})");
        }
        // 让它成为「已安装版本」下拉框里选中的那一个。**不能指望下拉框自己选中第 0 项**：
        // 只要 placeholderText 非空，往空模型里插第一批数据时它一项都不选（见
        // fillVersionCombo 里那段 NOTE），选中项只能由"上次启动的那个"恢复回来。真用户
        // 也是这样（他以前启动过这个实例，或者自己在下拉框里挑了一个）—— 这里走的是产品
        // 自己的 LauncherSettings，不是绕过它直接设下拉框。
        rememberLastVersion(instanceId);

        std::printf("类：%s（落到 %s/）\n来源：%s\n搜索词：%s\n实例：%s\n游戏根目录：%s\n",
                    qPrintable(ResourceCatalog::kindLabel(kind)), qPrintable(folder),
                    qPrintable(ResourceCatalog::sourceLabel(source)), qPrintable(query),
                    qPrintable(instanceId), qPrintable(liveRoot));

        MainWindow liveWindow;
        liveWindow.show();
        QApplication::processEvents();

        int printed = 0;
        const int column = MainWindowUI::versionTreeKeyColumn();
        const auto yesNo = [](bool flag) {
            return flag ? QStringLiteral("有") : QStringLiteral("没有");
        };
        // 把进程日志流出来（写到 printed 那一行为止）。
        const auto flushLogs = [&printed] {
            for (; printed < messagesSnapshot().size(); ++printed) {
                std::printf("        [日志] %s\n", qPrintable(messagesSnapshot().at(printed)));
            }
            std::fflush(stdout);
        };
        // 冒出来的模态框：记下它说了什么，然后点掉。
        //
        // **没有框的时候什么都不做**（千万不要顺手调一次 recordPrompt(nullptr)）：
        // `recordPrompt()` 第一件事就是把 g_modal* 那几个全局清空（见它的实现）——
        // 而下面那个循环拿"g_modalTitle 非空"当**产品已经给结论了**的判据、最后那条断言
        // 读的也是它。写成一进来就 record 的话：文件落地之后还要再转若干圈，框在这几圈里
        // 被点掉、下一圈进来就把刚记下的「下载完成」抹成空串 ⇒ 断言红在"框标题=「」"上，
        // 而 `[弹窗]` 那一行明明已经打出来了（ 在 CurseForge 那一跑上正好踩到：
        // 同一段代码走 Modrinth 时框落在那 20 圈的最后几圈，所以一直没露出来）。
        const auto sweepModal = [] {
            InlineMessageBox *box = currentPrompt();
            if (box == nullptr) {
                return;
            }
            if (box->objectName() == MainWindowUI::resourceDownloadInstancePickerName()) {
                auto *download = box->buttonAt(0);
                if (download->isEnabled()) download->click();
                else box->reject();
                return;
            }
            recordPrompt(box);
            std::printf("        [弹窗] 「%s」\n", qPrintable(g_modalTitle));
            std::fflush(stdout);
            box->close();
        };
        const auto drainOnce = [&flushLogs, &sweepModal] {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            flushLogs();
            sweepModal();
        };
        auto *categoryList =
            byObjectName<QListWidget>(&liveWindow, MainWindowUI::downloadCategoryListName());
        auto *stack = byObjectName<QStackedWidget>(&liveWindow, MainWindowUI::downloadStackName());
        auto *combo = comboByObjectName(&liveWindow, MainWindowUI::installedVersionComboName());
        auto *edit = byObjectName<QLineEdit>(&liveWindow, MainWindowUI::downloadSearchEditName(key));
        auto *resTree =
            byObjectName<QTreeWidget>(&liveWindow, MainWindowUI::downloadResourceTreeName(key));
        auto *manager = liveWindow.findChild<DownloadManager *>();
        std::printf("AppPaths 认的游戏根目录：%s\n", qPrintable(AppPaths::minecraftDir()));

        check(AppPaths::minecraftDir().compare(liveRoot, Qt::CaseInsensitive) == 0,
              "窗口认的游戏根目录就是这一节自己那份沙盒（没落到用户真目录上）",
              QString("%1 ≠ %2").arg(AppPaths::minecraftDir(), liveRoot));
        check(categoryList != nullptr && stack != nullptr && combo != nullptr && edit != nullptr &&
                  resTree != nullptr,
              QString("「%1」那一页的搜索框、它的资源树、以及「已安装版本」下拉框都找得到")
                  .arg(ResourceCatalog::kindLabel(kind)),
              QString("分类列表 %1 / 堆叠页 %2 / 下拉框 %3 / 搜索框 %4 / 资源树 %5")
                  .arg(yesNo(categoryList != nullptr), yesNo(stack != nullptr),
                       yesNo(combo != nullptr), yesNo(edit != nullptr),
                       yesNo(resTree != nullptr)));
        check(selectedId(combo) == instanceId,
              "沙盒里那个实例已经在下拉框里选中（第三层那一下要往它里面装）",
              QString("选中=「%1」 想要=「%2」 下拉框里 %3 条（versions/ 下 %4 个目录）")
                  .arg(selectedId(combo), instanceId)
                  .arg(combo->count())
                  .arg(QDir(AppPaths::versionsDir())
                           .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                           .size()));

        if (categoryList == nullptr || stack == nullptr || combo == nullptr || edit == nullptr ||
            resTree == nullptr || selectedId(combo) != instanceId) {
            std::printf("!! 环境没搭起来（上面那几条），这一节不往下跑\n");
            return 3;
        }

        // 切到这一类那一页 —— 走的是界面上那条路（点分类 → 右栏翻页 → 那一刻它会自己
        // 去问一次"最热门的"）。紧接着（**不跑事件循环**）就把搜索词敲进去回车：第二发
        // 会按同一个请求钥匙把第一发掐掉，于是树上最后留下的确定是我们搜的那个词的结果，
        // 而不是"等一下看哪一发先回来"。
        const int modIndex = MainWindowUI::downloadCategoryKeys().indexOf(key);
        check(modIndex >= 0,
              QString("「%1」在分类表里的下标认得出来（和 ResourceCatalog 是同一个键）")
                  .arg(ResourceCatalog::kindLabel(kind)),
              QString::number(modIndex));
        // ---- 把「来源」拨到这一跑要验的那一家（不给就保持界面上那个默认）----
        //
        // 顺序要紧：**必须在点分类/敲回车之前拨**。这一页第一次进来时已经按默认来源问过
        // 一发，而上面那一手"点分类 → 立刻敲回车"靠的是"第二发按同一个请求钥匙把第一发
        // 掐掉"（见 ResourceStore 的 searchRequestKey）—— 钥匙里就带着来源，拨晚了那一发
        // 还是按老来源发的，最后留在树上的东西和下面那些断言对不上。
        // 拨来源**本身不发请求**（ZI2 钉着这一条）：用户还没说「就用它搜」。
        {
            MainWindowUI *ui = liveWindow.ui();
            QComboBox *sourceCombo =
                ui == nullptr ? nullptr : ui->getDownloadFilterSourceCombo(key);
            check(sourceCombo != nullptr,
                  QString("「%1」那一页的「来源」下拉框找得到（这一跑才拨得动它）")
                      .arg(ResourceCatalog::kindLabel(kind)));
            if (sourceCombo != nullptr) {
                const int want = sourceCombo->findData(ResourceCatalog::sourceKey(source));
                check(want >= 0,
                      QString("「来源」里有一项认得出 %1 这个键").arg(ResourceCatalog::sourceKey(source)),
                      QStringLiteral("框里有 %1 项").arg(sourceCombo->count()));
                if (want >= 0) {
                    sourceCombo->setCurrentIndex(want);
                }
                ResourceCatalog::Source picked = ResourceCatalog::Source::Modrinth;
                check(ResourceCatalog::sourceFromKey(sourceCombo->currentData().toString(), &picked) &&
                          picked == source,
                      "拨完之后框里真的是这一家（下面那几发都按它发）",
                      sourceCombo->currentData().toString());
            }
        }
        //
        // 从实例设置的下载按钮进入，目标选择器会预选这一节的试装实例。
        bool openedSettings = false;
        bool clickedDownload = false;
        const auto openDownloadFromInstance = [&] {
            OverlayHost *host = currentPageOverlay();
            auto *page =
                host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            if (page == nullptr) {
                return;
            }
            openedSettings = true;
            InstanceFilesPage *files = page->filesPage(key);
            if (files == nullptr || files->downloadButton() == nullptr) {
                return;
            }
            clickedDownload = true;
            files->downloadButton()->click();
        };
        QMetaObject::invokeMethod(&liveWindow, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, instanceId));
        openDownloadFromInstance();
        // 收起来那一份对话框是 `deleteLater()` 收的：让它先落地，免得后面再开时
        // `findChild<InstanceSettingsDialog *>()` 摸到这一份已经收起来的。
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(openedSettings && clickedDownload,
              "从实例设置里点开「下载新模组」那颗键（这一路会把「装进哪个实例」记下来）",
              QString("实例设置=%1 下载键=%2").arg(yesNo(openedSettings), yesNo(clickedDownload)));
        if (!clickedDownload) {
            std::printf("!! 没点着「下载」那颗键（上面那条红了），这一节到此为止\n");
            return 3;
        }
        // 翻页与选栏是排到下一轮事件循环的（见 continueOpenDownloadCategory 那段），
        // 等它真的落在那一行上再往下敲搜索词。
        const bool pageReady =
            pumpUntil([categoryList, modIndex] { return categoryList->currentRow() == modIndex; },
                      5000, &printed);
        check(pageReady, "按实例那一页那颗「下载」进来，左栏选中了这一栏",
              QString("选中行=%1 想要=%2").arg(categoryList->currentRow()).arg(modIndex));
        edit->setText(query);
        QMetaObject::invokeMethod(edit, "returnPressed", Qt::DirectConnection);

        // 回车那一下就该在树上看见反应（走的是产品里那条 connect，不是我们自己调的槽）：
        // 树清空、盖上整块加载态「正在搜索「<搜索词>」…」。
        QWidget *loadingLayer = LoadingState::overlay(resTree);
        const QString loadingSays = loadingLayer != nullptr ? loadingLayer->accessibleName() : QString();
        check(resTree->topLevelItemCount() == 0 && loadingSays.contains(query),
              "回车那一下，树上立刻盖上「正在搜索「<搜索词>」…」加载态",
              QString("行数=%1 加载态=「%2」").arg(resTree->topLevelItemCount()).arg(loadingSays));

        // 等真结果。判据是"第一行不是灰的" —— 灰的那些（正在搜索/没搜到/搜索失败）都不是
        // 能点开的东西，它们下面那些断言也无从谈起。
        const auto firstHit = [resTree, column]() -> QTreeWidgetItem * {
            if (resTree->topLevelItemCount() == 0) {
                return nullptr;
            }
            QTreeWidgetItem *item = resTree->topLevelItem(0);
            return item->isDisabled() ? nullptr : item;
        };
        const bool gotHits = pumpUntil([&firstHit] { return firstHit() != nullptr; }, 60000,
                                       &printed);
        check(gotHits, QString("真网络上搜「%1」搜出东西来了").arg(query),
              QString("第一行=「%1」")
                  .arg(resTree->topLevelItemCount() > 0 ? resTree->topLevelItem(0)->text(column)
                                                        : QStringLiteral("<树上什么都没有>")));
        if (!gotHits) {
            std::printf("!! 网络这一关没过（上面那行/上面那些日志），外网不通或接口限流。"
                        "这一节到此为止，别去改产品的代码。\n");
            return 2;
        }

        QTreeWidgetItem *hit = firstHit();
        const QString projectId = hit->data(column, Qt::UserRole).toString();
        check(!projectId.isEmpty(), "命中的那一行的身份是 project_id（不是显示文字）", projectId);
        // 这一行必须**记着自己是从哪一家来的**：点开它第二层、第三层问的是那一家的接口
        // （`/mods/{id}/files` 和 `/project/{id}/version` 根本不是同一套地址）。默不作声地
        // 记成另一家，症状是"点开 CurseForge 的一条，列出来的是 Modrinth 那一套版本"。
        check(hit->data(column, MainWindowUI::resourceSourceRole()).toString() ==
                  ResourceCatalog::sourceKey(source),
              "命中的那一行记着的来源就是这一跑拨的那一家",
              hit->data(column, MainWindowUI::resourceSourceRole()).toString());
        check(hit->childCount() > 0 && !hit->child(0)->isDisabled(),
              "它底下已经摆好了「选游戏版本」那一步的那一串",
              QString("子条目数=%1").arg(hit->childCount()));

        // 点开它（第一层那一下是"只展开/收起"）。
        QMetaObject::invokeMethod(resTree, "itemClicked", Qt::DirectConnection,
                                  Q_ARG(QTreeWidgetItem *, hit), Q_ARG(int, 0));
        check(hit->isExpanded(),
              QString("点一下「%1」那一行，它展开了（「点开」是看看有什么，不是「选它」）")
                  .arg(ResourceCatalog::kindLabel(kind)));

        // 挑一个"真有得下"的游戏版本。接口给的这一串是**新的在前**（gameVersionChoices），
        // 所以从第一条开始试。试不出来**不是错**：作者没给这个游戏版本传过东西时，产品摆的
        // 正是一行灰的「这个游戏版本上没有可下载的版本。」—— 那句话是对的界面，不是 bug。
        QTreeWidgetItem *versionRow = nullptr;
        QStringList tried;
        for (int i = 0; versionRow == nullptr && i < hit->childCount() && i < 6; ++i) {
            QTreeWidgetItem *gameItem = hit->child(i);
            if (gameItem->isDisabled()) {
                continue;
            }
            const QString gameVersion = gameItem->data(column, Qt::UserRole).toString();
            // 第二层**第一次**点开：这一下才去问"这个游戏版本上哪些版本可下"。
            QMetaObject::invokeMethod(resTree, "itemClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem *, gameItem), Q_ARG(int, 0));
            const bool answered =
                pumpUntil([gameItem] {
                              return gameItem->childCount() > 0 && !gameItem->child(0)->isDisabled();
                          },
                          30000, &printed);
            tried << QStringLiteral("%1=%2").arg(gameVersion,
                                                 answered ? QStringLiteral("有") : QStringLiteral("没有"));
            if (answered) {
                versionRow = gameItem->child(0);
            }
        }
        check(versionRow != nullptr, "挑到了一个真有可下版本的游戏版本（点开它会列出版本号）",
              tried.join(QLatin1Char(' ')));
        if (versionRow == nullptr) {
            std::printf("!! 前几条游戏版本都没得下（上面那串），这一节到此为止\n");
            return 2;
        }
        std::printf("要点的那一版：%s（游戏版本 %s）\n", qPrintable(versionRow->text(column)),
                    qPrintable(versionRow->parent()->data(column, Qt::UserRole).toString()));

        // 落点：<实例>/<folderForKind 说的那个子目录>/<那一行写的文件名>。**不要**自己另编
        // 一套命名 —— 行的正文就是 primaryFile()->filename（见 fillResourceVersionRows），
        // 落盘用的也是它（见 downloadResourceFiles），这一条断言正是把那两个"同一个名字"
        // 钉在一起。
        const QString destDir = AppPaths::versionDir(instanceId) + QLatin1Char('/') + folder;
        const QString destFile = destDir + QLatin1Char('/') + versionRow->text(column);
        check(destDir.compare(productDir, Qt::CaseInsensitive) == 0 &&
                  destDir.startsWith(liveRoot, Qt::CaseInsensitive),
              QString("落点是这一节沙盒里那个实例的 %1/（没伸到别的地方去）").arg(folder),
              QDir::toNativeSeparators(destDir));

        // 点第三层那一下之前先等版本清单那条请求收尾：DownloadManager 只有**一份**批次状态，
        // 它正忙的时候 downloadResourceFiles() 会当场拒绝并弹一个「下载失败」。那不是这一节
        // 要验的东西（窗口一开就在拉版本表），等它闲下来再点。
        if (manager != nullptr) {
            const bool idle = pumpUntil([manager] { return !manager->isBusy(); }, 60000, &printed);
            std::printf("点之前 DownloadManager 闲下来了：%s\n",
                        idle ? "是" : "否（仍会点，红了就是这条）");
        }

        // 下完那一下产品会弹一个框（成功是「下载完成」、失败是「下载失败」）。模态窗一出来
        // 它的 exec() 就在等人点，而**点它这件事只能发生在那个嵌套事件循环里** —— 谁在这之前
        // 调了 processEvents()，谁就被裹在那个嵌套循环里出不来了，"转完一圈再看一眼框"
        // 这种写法在这里是**死锁**（实测卡了十分钟，最后靠杀进程收场）。唯一的办法是排一个
        // **定时器**：定时器在任何事件循环里都会响，包括那个嵌套的。
        //
        // NOTE: 也不能用现成的 dismissNextPrompt() —— 它排的是"0 毫秒 + 300 毫秒"两条
        // **一次性**定时器，那个设计是给"点完框立刻弹出"的动作的（登录、启动）。
        // 这一节要等的是**真下载**（一秒到几分钟）：那两条早在提示出现之前就响完了
        // （响的那一刻那一层还是空的，直接返回），提示照样没人点。
        QTimer modalSweeper;
        QObject::connect(&modalSweeper, &QTimer::timeout, [&flushLogs, &sweepModal] {
            flushLogs();  // 定时器在嵌套循环里也响，日志正好跟着一起流
            sweepModal();
        });
        // 先把"上一次记下来的框"清掉：下面那个等待循环拿 g_modalTitle 非空当"产品给结论了"
        // 的判据，留着旧值会当场误判成"下完了"。
        g_modalTitle.clear();
        g_modalText.clear();
        g_modalWasWarning = false;
        modalSweeper.start(100);
        QMetaObject::invokeMethod(resTree, "itemClicked", Qt::DirectConnection,
                                  Q_ARG(QTreeWidgetItem *, versionRow), Q_ARG(int, 0));

        const auto landed = [&destFile] {
            const QFileInfo info(destFile);
            return info.exists() && info.size() > 0;
        };
        QElapsedTimer downloadClock;
        downloadClock.start();
        bool ok = false;
        for (;;) {
            drainOnce();
            if (landed()) {
                ok = true;
                break;
            }
            if (!g_modalTitle.isEmpty()) {
                break;  // 产品已经给结论了（那块框要么说好了、要么说失败了）
            }
            if (downloadClock.elapsed() > 300000) {
                break;
            }
        }
        // 文件落地不等于收尾那几行走完了：写盘在前、弹框在后（见 onResourceDownloadFinished）。
        // 再跑一小会儿，让「下载完成」那个框出来又被点掉，g_modalTitle 才有值可断言。
        // 判据是"记下来了就停"而不是转满固定圈数：圈数写死的话，框刚好落在最后几圈之后
        // 就是一条**偶发**红（真下载要多久不由我们决定），而多转的那几圈只是白等。
        for (int i = 0; i < 60 && g_modalTitle.isEmpty(); ++i) {
            drainOnce();
        }
        modalSweeper.stop();

        const QFileInfo landedInfo(destFile);
        check(ok, QString("点第三层那一下，文件真的落到了 <实例>/%1/ 下（在盘上、非空）")
                      .arg(folder),
              QString("%1 → %2")
                  .arg(QDir::toNativeSeparators(destFile),
                       landedInfo.exists() ? QStringLiteral("%1 B").arg(landedInfo.size())
                                           : QStringLiteral("盘上没有这个文件")));
        check(g_modalTitle == QStringLiteral("下载完成"),
              "下完弹的是「下载完成」那个框（不是「下载失败」）",
              QString("框标题=「%1」正文=「%2」")
                  .arg(g_modalTitle, QString(g_modalText).replace(QLatin1Char('\n'), QLatin1Char(' '))));

        std::printf("\n===== %s =====\n", qPrintable(QDir::toNativeSeparators(destDir)));
        const QDir dir(destDir);
        for (const QFileInfo &info :
             dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
            std::printf("  %s%s\n", qPrintable(info.fileName()),
                        info.isDir() ? "/"
                                     : qPrintable(QStringLiteral("  %1 B").arg(info.size())));
        }
        std::printf("产物留着（就是这一节的结果本身）：%s\n",
                    qPrintable(QDir::toNativeSeparators(liveRoot)));
        std::printf("\n===== %s: %d/%d =====\n", g_fail == 0 ? "全部通过" : "有失败", g_pass,
                    g_pass + g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    // ---------- update-live：**真网络 + 用户自己的盘**原样跑一遍「检查更新」那条路 --
    // mw_check.exe update-live <实例目录> [游戏版本] [加载器] [最多几条] [模式] [缓存]
    //
    // 存在的理由： 用户报「无法正常获取更新」（他自己指定的是**模组更新**）。
    // 这条路在产品里是 `MainWindow::checkInstanceUpdates()`，三步：
    // ① 反查（`InstanceIconLookup::lookup`：sha1 + 指纹 → 项目号 + `sha1`）；
    // ② **每个项目各问一发版本列表**（`ResourceStore::fetchVersions`）；
    // ③ `ModDependencies::judgeUpdate()` 逐条判"这一版是不是最新的"。
    // 一页几百个模组就是**几百发请求** —— 这一节照着他的盘把这三步原样跑一遍，把"每条判成
    // 什么、总共花多久、撞没撞限流（429）"都打出来。它是**判据**：修之前跑、修之后再跑，
    // 两次的数字要说得上话。
    //
    // **一个字节都不写**：`lookup` 只读文件算哈希（不给 `Options::cacheDir` 就是"不用缓存"，
    // 见那边），`fetchVersions` 只发网络请求。所以可以直接对着
    // `E:/MC/.minecraft/versions/<实例>/mods` 跑，不必先摆沙盒。
    //
    // 参数里**游戏版本和加载器要人给**（产品是从实例自己读的：`dependencyTargetFor()`）——
    // 这一节刻意不去复刻那条读法，因为要量的是"这一趟请求发出去会怎么样"。
    //
    // 模式一栏：`batch` = 走**批量那一发**（`POST /v2/version_files/update`，一个项目都不
    // 单独问）；缺省 = 走产品今天那条"一项目一发"的路。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("update-live")) {
        QString dir = QString::fromLocal8Bit(argv[2]);
        if (QFileInfo::exists(dir + QStringLiteral("/mods"))) {
            dir += QStringLiteral("/mods");
        }
        const QString gameVersion = argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString();
        const QString loader = argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QString();
        const int limit = argc >= 6 ? QString::fromLocal8Bit(argv[5]).toInt() : 0;
        const bool batchMode = argc >= 7 && QString::fromLocal8Bit(argv[6]) == QLatin1String("batch");
        // 第 7 个参数 = `cache` —— 反查那一趟**挂上产品那份盘上缓存**
        // （`AppPaths::instanceIconCacheDir()` + 实例目录名当键，和
        // `checkInstanceUpdates()` 里那两句逐字同一个口径）。不给就是"不用缓存"。
        // 为什么要这一档：产品那一趟**几乎从不真的去读盘算哈希** —— 用户先点开过模组页
        // （那一趟把身份 + sha1 写进了缓存），检查更新进来时**整批都是缓存命中**。
        // 两者走的是同一份 `Found`，但"命中"那条路上任何一位没带出来，症状都只在
        // 真机上看得见。
        const bool useCache = argc >= 8 && QString::fromLocal8Bit(argv[7]) == QLatin1String("cache");

        QDir mods(dir);
        QStringList paths;
        for (const QFileInfo &info :
             mods.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            if (InstanceIconLookup::isLookupCandidate(info.absoluteFilePath())) {
                paths.append(info.absoluteFilePath());
            }
        }
        if (limit > 0 && paths.size() > limit) {
            paths = paths.mid(0, limit);
        }
        std::printf("目录：%s（%d 个候选）\n", qPrintable(QDir::toNativeSeparators(dir)),
                    int(paths.size()));
        std::printf("目标：游戏版本「%s」加载器「%s」；模式 = %s\n", qPrintable(gameVersion),
                    qPrintable(loader), batchMode ? "批量（version_files/update）" : "一项目一发");
        if (paths.isEmpty()) {
            std::printf("这个目录里没有 `.jar` / `.zip`，量不了。\n");
            return 2;
        }

        // ---------- ① 反查（和产品那一趟同一个函数）----------
        InstanceIconLookup::Options lookupOptions;
        if (useCache) {
            // 数据目录从实例目录**倒着**认回来（`<数据目录>/versions/<实例>`）——
            // 缓存目录是挂在数据根底下的（见 AppPaths::instanceIconCacheDir()）。
            const QString instanceDir = dir.endsWith(QStringLiteral("/mods"))
                                            ? dir.left(dir.size() - 5)
                                            : dir;
            const QString dataDir = instanceDir.section(QStringLiteral("/versions/"), 0, 0);
            AppPaths::setMinecraftDir(dataDir);
            lookupOptions.cacheDir = AppPaths::instanceIconCacheDir();
            lookupOptions.cacheKey = QFileInfo(instanceDir).fileName();
            std::printf("缓存：%s（键「%s」）\n",
                        qPrintable(QDir::toNativeSeparators(lookupOptions.cacheDir)),
                        qPrintable(lookupOptions.cacheKey));
        }
        QElapsedTimer timer;
        timer.start();
        const InstanceIconLookup::Result result = InstanceIconLookup::lookup(paths, lookupOptions);
        const qint64 lookupMs = timer.elapsed();
        if (!result.problem.isEmpty()) {
            std::printf("★ problem：%s\n", qPrintable(result.problem));
        }
        std::printf("反查：认出 %d 条、读不动 %d 个，用了 %lld ms\n", int(result.found.size()),
                    result.unreadable, static_cast<long long>(lookupMs));

        // ---------- ② 攒钥匙（照 `onUpdateCheckLookupFinished` 那一段逐字）----------
        //
        // **这一批文件带不带加载器**。产品的规矩是"只有模组带"
        // （`ModDependencies::kindUsesLoader()`，那段注释里有实测）：资源包 / 光影包 /
        // 数据包带上实例的加载器 **一条都问不到**。这一节按**目录名**认类别 —— 名字走
        // `ResourceCatalog::folderForKind()` 那张表（不在这儿另抄一份 `mods` / `resourcepacks`）。
        bool loaderApplies = true;
        {
            const QString leaf = QFileInfo(dir).fileName();
            for (const ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
                const QString folder = ResourceCatalog::folderForKind(kind);
                if (!folder.isEmpty() && leaf.compare(folder, Qt::CaseInsensitive) == 0) {
                    loaderApplies = ModDependencies::kindUsesLoader(kind);
                    break;
                }
            }
        }
        const QString itemLoader = loaderApplies ? loader : QString();
        std::printf("加载器：「%s」%s\n", qPrintable(itemLoader),
                    loaderApplies ? "（这一类要带）" : "（这一类不带 —— 带了就一条都问不到）");

        QHash<QString, QList<ModDependencies::InstalledItem>> itemsByKey;
        for (const InstanceIconLookup::Found &found : result.found) {
            if (found.hit.projectId.isEmpty()) {
                continue;
            }
            ModDependencies::InstalledItem item;
            item.path = found.path;
            item.iconKey = found.hit.iconCacheId();
            item.title = found.hit.title;
            item.sha1 = found.sha1;
            item.loaderApplies = loaderApplies;
            itemsByKey[ModDependencies::versionsKeyFor(QStringLiteral("update"),
                                                       found.hit.projectId, gameVersion, itemLoader,
                                                       found.hit.source)]
                .append(item);
        }
        std::printf("要问的项目：%d 个\n", int(itemsByKey.size()));

        // 一个文件在**两家**都认出来时 `result.found` 里就是两条（一条 Modrinth、一条
        // CurseForge，路径逐字同一个）—— 这里把它数出来：产品那条路按这一条一条往下递，
        // 于是"两条"会变成"两发请求 + 报告里两行"（这一节量"一趟要发多少发"时必须知道它）。
        {
            QHash<QString, int> perPath;
            for (auto it = itemsByKey.constBegin(); it != itemsByKey.constEnd(); ++it) {
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    ++perPath[item.path];
                }
            }
            int twice = 0;
            int items = 0;
            for (auto it = perPath.constBegin(); it != perPath.constEnd(); ++it) {
                items += it.value();
                if (it.value() > 1) {
                    ++twice;
                }
            }
            int modrinthRecords = 0;
            int curseforgeRecords = 0;
            QSet<QString> modrinthPaths;
            QSet<QString> curseforgePaths;
            for (auto it = itemsByKey.constBegin(); it != itemsByKey.constEnd(); ++it) {
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    const int dash = item.iconKey.indexOf(QLatin1Char('-'));
                    const QString source = dash > 0 ? item.iconKey.left(dash) : item.iconKey;
                    if (source == QLatin1String("curseforge")) {
                        ++curseforgeRecords;
                        curseforgePaths.insert(item.path);
                    } else {
                        ++modrinthRecords;
                        modrinthPaths.insert(item.path);
                    }
                }
            }
            std::printf("按文件去重：%d 个文件（%d 条记录），两家都认出来的有 %d 个\n",
                        int(perPath.size()), items, twice);
            std::printf("  记录：Modrinth %d 条（%d 个文件）、CurseForge %d 条（%d 个文件）；"
                        "只有 CurseForge 认出来的文件 %d 个\n",
                        modrinthRecords, int(modrinthPaths.size()), curseforgeRecords,
                        int(curseforgePaths.size()),
                        int(curseforgePaths.size() - int((curseforgePaths & modrinthPaths).size())));
        }

        QHash<QString, QList<ResourceCatalog::VersionEntry>> answers;
        QHash<QString, QString> problems;
        timer.restart();

        if (batchMode) {
            // ---------- ②' 新产品那条路（ 改的）----------
            //
            // 和 `MainWindow::onUpdateCheckLookupFinished()` 逐条对齐：
            // ① 同一个文件两家都认出来时**只查一家**（走 Modrinth），CurseForge 那一条
            // **押着**等 Modrinth 先开口；
            // ② Modrinth 那一拨走**批量那一发**（一次一串 sha1）；
            // ③ 只有 CurseForge 认出来的那些照旧一项目一发；
            // ④ 批量那一发**没给出答案**的那些，退回问 CurseForge。
            QSet<QString> modrinthPaths;
            for (const InstanceIconLookup::Found &found : result.found) {
                if (!found.hit.projectId.isEmpty() &&
                    found.hit.source == ResourceCatalog::Source::Modrinth) {
                    modrinthPaths.insert(found.path);
                }
            }
            QHash<QString, QList<ModDependencies::InstalledItem>> batchItems;
            QHash<QString, QList<ModDependencies::InstalledItem>> heldItems;
            QHash<QString, QList<ModDependencies::InstalledItem>> singleItems;
            for (auto it = itemsByKey.constBegin(); it != itemsByKey.constEnd(); ++it) {
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    const int dash = item.iconKey.indexOf(QLatin1Char('-'));
                    const QString source = dash > 0 ? item.iconKey.left(dash) : item.iconKey;
                    const bool fromModrinth = source != QLatin1String("curseforge");
                    if (fromModrinth && !item.sha1.isEmpty()) {
                        batchItems[item.sha1.toLower()].append(item);
                        continue;
                    }
                    if (!fromModrinth && modrinthPaths.contains(item.path)) {
                        heldItems[it.key()].append(item);
                        continue;
                    }
                    singleItems[it.key()].append(item);
                }
            }
            std::printf("分流：批量 %d 个 sha1、押着等 Modrinth 的 %d 个、直接问单发的 %d 个\n",
                        int(batchItems.size()), int(heldItems.size()), int(singleItems.size()));

            ResourceStore store;
            bool done = false;
            QHash<QString, ResourceCatalog::VersionEntry> latest;
            QString problem;
            QObject::connect(&store, &ResourceStore::latestVersionsFinished, &store,
                             [&](const QString &, const QHash<QString, ResourceCatalog::VersionEntry>
                                                        &answer,
                                 const QString &why) {
                                 latest = answer;
                                 problem = why;
                                 done = true;
                             });
            store.fetchLatestVersions(QStringLiteral("probe"), batchItems.keys(), gameVersion,
                                      itemLoader);
            if (!pumpUntil([&]() { return done; }, 180000)) {
                std::printf("★ 批量那一发**没回来**（180 秒）。\n");
                return 1;
            }
            if (!problem.isEmpty()) {
                std::printf("★ 批量那一发报错：%s\n", qPrintable(problem));
            }
            std::printf("批量那一发回来了 %d 条（问了 %d 个 sha1，上限 %d 个一发 ⇒ %d 发）\n",
                        int(latest.size()), int(batchItems.size()), 100,
                        int((batchItems.size() + 99) / 100));

            // 退回问 CurseForge 的那些（批量没给出它 sha1 的答案）。
            QHash<QString, QList<ModDependencies::InstalledItem>> refire;
            int released = 0;
            for (auto it = heldItems.constBegin(); it != heldItems.constEnd(); ++it) {
                bool answered = false;
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    if (!item.sha1.isEmpty() && latest.contains(item.sha1)) {
                        answered = true;
                        break;
                    }
                }
                if (answered) {
                    ++released;
                    continue;
                }
                refire[it.key()] = it.value();
            }
            std::printf("押着的那些：Modrinth 答了的 %d 个（到此为止）、退回问 CurseForge 的 %d 个\n",
                        released, int(refire.size()));

            // 逐条判（批量那些）。
            int outdated = 0;
            int current = 0;
            int unknown = 0;
            QStringList lines;
            const auto judgeOne = [&](const ModDependencies::UpdateVerdict &verdict) {
                switch (verdict.state) {
                case ModDependencies::UpdateState::Outdated:
                    ++outdated;
                    break;
                case ModDependencies::UpdateState::UpToDate:
                    ++current;
                    break;
                case ModDependencies::UpdateState::Unknown:
                    ++unknown;
                    break;
                }
                if (verdict.state != ModDependencies::UpdateState::UpToDate) {
                    lines.append(QStringLiteral("  %1 %2 %3")
                                     .arg(QFileInfo(verdict.item.path).fileName(),
                                          verdict.state == ModDependencies::UpdateState::Outdated
                                              ? QStringLiteral("可更新")
                                              : QStringLiteral("说不清"),
                                          verdict.state == ModDependencies::UpdateState::Outdated
                                              ? verdict.latestVersionNumber
                                              : verdict.reason));
                }
            };
            QSet<QString> heldPaths;
            for (auto it = heldItems.constBegin(); it != heldItems.constEnd(); ++it) {
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    heldPaths.insert(item.path);
                }
            }
            for (auto it = batchItems.constBegin(); it != batchItems.constEnd(); ++it) {
                const ResourceCatalog::VersionEntry found = latest.value(it.key());
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    // 押着的那些：这一发没答就**不在这儿判**（下面的 CurseForge 那一发说了算）。
                    if (found.id.isEmpty() && heldPaths.contains(item.path)) {
                        continue;
                    }
                    judgeOne(ModDependencies::judgeUpdateAgainstLatest(item, found, problem));
                }
            }

            // 剩下的两拨（只有 CurseForge 认出来的 + 退回的）一项目一发。
            QHash<QString, QList<ModDependencies::InstalledItem>> allSingles = singleItems;
            for (auto it = refire.constBegin(); it != refire.constEnd(); ++it) {
                allSingles[it.key()].append(it.value());
            }
            QHash<QString, QList<ResourceCatalog::VersionEntry>> answers;
            QHash<QString, QString> problems;
            QSet<QString> waiting;
            QObject::connect(&store, &ResourceStore::versionsFinished, &store,
                             [&](const QString &key, const QList<ResourceCatalog::VersionEntry> &v,
                                 const QString &p) {
                                 if (!waiting.contains(key)) {
                                     return;
                                 }
                                 waiting.remove(key);
                                 answers.insert(key, v);
                                 problems.insert(key, p);
                             });
            for (auto it = allSingles.constBegin(); it != allSingles.constEnd(); ++it) {
                const QString iconKey = it.value().first().iconKey;
                ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
                const int dash = iconKey.indexOf(QLatin1Char('-'));
                if (dash > 0) {
                    ResourceCatalog::sourceFromKey(iconKey.left(dash), &source);
                }
                const QString projectId = dash > 0 ? iconKey.mid(dash + 1) : iconKey;
                waiting.insert(it.key());
                store.fetchVersions(it.key(), projectId, source, gameVersion, itemLoader);
            }
            if (!pumpUntil([&]() { return waiting.isEmpty(); }, 300000)) {
                std::printf("★ 还有 %d 发单发没回来（300 秒到）。\n", int(waiting.size()));
            }
            QHash<QString, int> problemTally;
            for (auto it = allSingles.constBegin(); it != allSingles.constEnd(); ++it) {
                const QString why = problems.value(it.key());
                if (!why.isEmpty()) {
                    problemTally[why] += 1;
                }
                const QList<ResourceCatalog::VersionEntry> answer =
                    why.isEmpty() ? answers.value(it.key()) : QList<ResourceCatalog::VersionEntry>();
                for (const ModDependencies::InstalledItem &item : it.value()) {
                    judgeOne(ModDependencies::judgeUpdate(item, answer, why));
                }
            }
            for (const QString &line : lines) {
                std::printf("%s\n", qPrintable(line));
            }
            std::printf("用了 %lld ms。可更新 %d、已是最新 %d、说不清 %d（一共 %d 条）\n",
                        static_cast<long long>(timer.elapsed()), outdated, current, unknown,
                        outdated + current + unknown);
            for (auto it = problemTally.constBegin(); it != problemTally.constEnd(); ++it) {
                std::printf("  ★ %d 发报错：%s\n", it.value(), qPrintable(it.key()));
            }
            return 0;
        }

        // ---------- ② 一项目一发（产品今天这条路）----------
        ResourceStore store;
        QSet<QString> waiting;
        QObject::connect(&store, &ResourceStore::versionsFinished, &store,
                         [&](const QString &key, const QList<ResourceCatalog::VersionEntry> &v,
                             const QString &p) {
                             if (!waiting.contains(key)) {
                                 return;
                             }
                             waiting.remove(key);
                             answers.insert(key, v);
                             problems.insert(key, p);
                         });
        for (auto it = itemsByKey.constBegin(); it != itemsByKey.constEnd(); ++it) {
            const QString iconKey = it.value().first().iconKey;
            ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
            const int dash = iconKey.indexOf(QLatin1Char('-'));
            if (dash > 0) {
                ResourceCatalog::sourceFromKey(iconKey.left(dash), &source);
            }
            const QString projectId = dash > 0 ? iconKey.mid(dash + 1) : iconKey;
            waiting.insert(it.key());
            store.fetchVersions(it.key(), projectId, source, gameVersion, loader);
        }
        if (!pumpUntil([&]() { return waiting.isEmpty(); }, 300000)) {
            std::printf("★ 还有 %d 发没回来（300 秒到）。\n", int(waiting.size()));
        }
        const qint64 versionsMs = timer.elapsed();

        // ---------- ③ 判 ----------
        QHash<QString, int> problemTally;
        int outdated = 0;
        int current = 0;
        int unknown = 0;
        for (auto it = itemsByKey.constBegin(); it != itemsByKey.constEnd(); ++it) {
            const QString problem = problems.value(it.key());
            if (!problem.isEmpty()) {
                problemTally[problem] += 1;
            }
            const QList<ResourceCatalog::VersionEntry> answer =
                problem.isEmpty() ? answers.value(it.key()) : QList<ResourceCatalog::VersionEntry>();
            for (const ModDependencies::InstalledItem &item : it.value()) {
                const ModDependencies::UpdateVerdict verdict =
                    ModDependencies::judgeUpdate(item, answer);
                switch (verdict.state) {
                case ModDependencies::UpdateState::Outdated:
                    ++outdated;
                    break;
                case ModDependencies::UpdateState::UpToDate:
                    ++current;
                    break;
                case ModDependencies::UpdateState::Unknown:
                    ++unknown;
                    break;
                }
                if (verdict.state != ModDependencies::UpdateState::UpToDate) {
                    std::printf("  %-46s %-9s %s\n", qPrintable(QFileInfo(item.path).fileName()),
                                verdict.state == ModDependencies::UpdateState::Outdated ? "可更新"
                                                                                        : "说不清",
                                qPrintable(verdict.state == ModDependencies::UpdateState::Outdated
                                               ? verdict.latestVersionNumber
                                               : verdict.reason));
                }
            }
        }
        std::printf("版本列表那 %d 发用了 %lld ms。可更新 %d、已是最新 %d、说不清 %d\n",
                    int(itemsByKey.size()), static_cast<long long>(versionsMs), outdated, current,
                    unknown);
        for (auto it = problemTally.constBegin(); it != problemTally.constEnd(); ++it) {
            std::printf("  ★ %d 发报错：%s\n", it.value(), qPrintable(it.key()));
        }
        return 0;
    }

    // ---------- icon-lookup-live：**真网络 + 用户自己的盘**量一次"这一页到底有几张图" --
    // mw_check.exe icon-lookup-live <目录>
    //
    // 存在的理由：`InstanceIconLookup` **必须收接口给的图标地址**，不能指望
    // `Hit::iconPngUrl()` 自己拼的 `cdn.modrinth.com/data/<id>/icon.png` ——
    // 那条**只是老项目的别名**。用户盘上 20 个模组有 11 个回 404，所以这条命令要能
    // **照着他的盘跑一遍**看现在是什么样，而不是只信假服务器上那几条断言。
    //
    // **一个字节都不写**：`InstanceIconLookup::lookup` 只读文件算哈希 + 发网络请求
    // （图标缓存是 `ResourceStore` 那一层的事，这条路根本不碰它，`Options` 也不带任何落盘
    // 的东西）—— 所以可以直接对着 `E:/MC/.minecraft/mods` 跑，不必先摆沙盒。
    //
    // 打三样：
    // ① 每个文件认出来的来源 / 项目号 / 图标地址；
    // ② **老那条别名还灵不灵**：拿 `ResourceCatalog::iconUrlForProject()` 拼出来的地址
    // 真发一发，看回 200 还是 404 —— 这一列就是那个 bug 本身（修之前，这张表上唯一
    // 能拿到图的就是"这一列回 200"的那几个）；
    // ③ 小结：认出几个 / 拿到地址几个 / 老办法能用几个。
    // 只报数、不断言（网络通和不通都是要看的结果），所以 0 = 量完了、2 = 目录里一个候选都没有。
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("icon-lookup-live")) {
        QString dir = QString::fromLocal8Bit(argv[2]);
        // 给的是游戏根目录（底下有 mods/）就自动拐进 mods/ —— 两种都收，省得每次都打全。
        if (QFileInfo::exists(dir + QStringLiteral("/mods"))) {
            dir += QStringLiteral("/mods");
        }
        const QDir mods(dir);
        QStringList paths;
        for (const QFileInfo &info :
             mods.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            if (InstanceIconLookup::isLookupCandidate(info.absoluteFilePath())) {
                paths.append(info.absoluteFilePath());
            }
        }
        std::printf("目录：%s（%d 个候选）\n", qPrintable(QDir::toNativeSeparators(dir)),
                    int(paths.size()));
        if (paths.isEmpty()) {
            std::printf("这个目录里没有 `.jar` / `.zip`，量不了。\n");
            return 2;
        }

        QElapsedTimer timer;
        timer.start();
        const InstanceIconLookup::Result result = InstanceIconLookup::lookup(paths);
        const qint64 lookupMs = timer.elapsed();
        if (!result.problem.isEmpty()) {
            std::printf("★ problem（这一趟没完整跑完）：%s\n", qPrintable(result.problem));
        }
        if (!result.iconProblem.isEmpty()) {
            std::printf("★ iconProblem（身份认出来了，图标地址那一发没成）：%s\n",
                        qPrintable(result.iconProblem));
        }
        std::printf("算成哈希 %d 个、读不动 %d 个；认出 %d 条，用了 %lld ms\n", result.hashed,
                    result.unreadable, int(result.found.size()), static_cast<long long>(lookupMs));

        // 老那条别名到底灵不灵（只对 Modrinth 那些有意义 —— CurseForge 拼不出地址来）。
        QNetworkAccessManager network;
        int modrinthFound = 0;
        int withIcon = 0;
        int legacyOk = 0;
        int legacy404 = 0;
        for (const InstanceIconLookup::Found &found : result.found) {
            QString legacy;
            if (found.hit.source == ResourceCatalog::Source::Modrinth) {
                ++modrinthFound;
                const HttpClient::Reply reply = HttpClient::send(
                    &network, QUrl(ResourceCatalog::iconUrlForProject(found.hit.projectId)),
                    HttpClient::Verb::Get, QByteArray(), QByteArray(), QByteArray(),
                    HttpClient::kDefaultTimeoutMs,
                    QList<QPair<QByteArray, QByteArray>>{{QByteArrayLiteral("User-Agent"),
                                                          QByteArrayLiteral("WTFML")}});
                legacy = reply.status == 200 ? QStringLiteral("老地址 200")
                                             : QStringLiteral("老地址 %1").arg(reply.status);
                reply.status == 200 ? ++legacyOk : ++legacy404;
            }
            if (!found.hit.iconUrl.isEmpty()) {
                ++withIcon;
            }
            std::printf("  %-38s %-11s %-22s %s\n",
                        qPrintable(QFileInfo(found.path).fileName()),
                        found.hit.source == ResourceCatalog::Source::Modrinth ? "Modrinth"
                                                                             : "CurseForge",
                        qPrintable(found.hit.projectId),
                        qPrintable(found.hit.iconUrl.isEmpty() ? QStringLiteral("(没有图标地址)")
                                                               : found.hit.iconUrl));
            if (!legacy.isEmpty()) {
                std::printf("  %-38s %s\n", "", qPrintable(legacy));
            }
        }
        std::printf("小结：认出 %d 条（Modrinth %d / CurseForge %d）；**拿到图标地址 %d 条**；"
                    "老那条别名能用 %d 条、404 %d 条\n",
                    int(result.found.size()), modrinthFound, int(result.found.size()) - modrinthFound,
                    withIcon, legacyOk, legacy404);
        return 0;
    }

    // ---------- icon-live：**真网络**量一次"图标这一路现在到底是什么状况" ----------
    // mw_check.exe icon-live [数量] 数量默认 20（下载页一页就是 20 条）
    //
    // 存在的理由： 用户报"图标加载好慢"。查下去，**慢的原因在网络那边**：
    // `cdn.modrinth.com` 对每一张图只回一个 307，真正的字节在 `cdn-alt.modrinth.com` 上，
    // 而那个域名从国内连不上（连 TCP 都建不起来，一直等到超时）。启动器这一侧能做的是
    // "别一张张干等"（见 ResourceStore 里 m_iconGaveUp 那段），而这件事只能在真网络上量：
    // 这一节就量三件事 ——
    // ① 二十来张图标，从发出去到全部回来一共多久（那道闸起作用时是**一个超时的量级**，
    // 不是"二十张各自超时再排队"的量级）；
    // ② 有几张是空手回来的（CDN 不通时应该差不多是全部）；
    // ③ 盘上缓存攒下来了几份（下一次跑这些就是一个请求都不发）。
    // 又加了第四件：**这一跑要的是哪条地址、回来的又是什么字节** —— 能解 webp 的
    // 机器现在先要那条小 7 倍的 `_96.webp`，盘上落下来的是 png 还是 webp 一眼看得出来
    // （见 ResourceStore::requestIcon）。两台机器（有/没有那个编解码器）跑出来的数会不一样，
    // 所以这里也是只报数。
    // （同一天第三批）再加两件，为的是用户那句"缓存不过期，每次打开用网络刷新、
    // 比一比有没有变（**如果严重占用网络就不做了**）"：
    // ⑤ **同一趟运行里再要一次**（重搜 / 翻页那条路）：应该一条同步就回、一个请求都不发；
    // ⑥ **核对那一发到底多少字节**：拿盘上第一份有凭据的图标，用**产品自己那个
    // `ResourceStore::iconRequest()`** 真发一发（同一个函数、同一套头），把状态码和
    // 正文大小打出来 —— "敢做的全部理由就是它便宜"这句话，得有个真数撑着。
    // 理想结果是 304 加几十~几百字节，而盘上那份整图是几十 KB。
    // **不断言网络的好坏**（通和不通都是要看的结果），所以这一节只报数、不返回失败码：
    // 0 = 量完了；2 = 搜索那一步就没回来（那连"量"都量不了）。
    //
    // 沙盒：游戏根目录指到 `<exe 目录>/icon-live/.minecraft`，图标缓存也就落在那一份里
    // （见 AppPaths::iconCacheDir）—— 不碰用户的真目录，也不碰默认那一跑那份沙盒（它没有
    // "cache/icons 里该有几份"这种断言，但多一份东西总是少一个变量）。
    // NOTE: 这份沙盒**不删**：连跑两次才看得出缓存那一档（第二次应该秒回）。
    if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("icon-live")) {
        const int wanted = argc >= 3 ? QString::fromLocal8Bit(argv[2]).toInt() : 20;
        const QString liveRoot = root.filePath(QStringLiteral("icon-live/.minecraft"));
        QDir().mkpath(liveRoot);
        AppPaths::setMinecraftDir(liveRoot);
        {
            QSettings sandbox;
            sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({liveRoot}));
            sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), liveRoot);
        }
        std::printf("沙盒：%s\n", qPrintable(QDir::toNativeSeparators(liveRoot)));
        std::printf("图标缓存：%s\n", qPrintable(QDir::toNativeSeparators(AppPaths::iconCacheDir())));

        ResourceStore store;
        const QString key = ResourceCatalog::searchRequestKey(ResourceCatalog::Kind::Mod);
        ResourceCatalog::SearchResult found;
        bool searched = false;
        QObject::connect(&store, &ResourceStore::searchFinished, &store,
                         [&found, &searched](const QString &, const ResourceCatalog::SearchResult &r) {
                             found = r;
                             searched = true;
                         });
        QElapsedTimer searchClock;
        searchClock.start();
        // 空搜索词 = "这一页里最热门的那些"，和进页面时那一发一模一样。
        // 来源写死 Modrinth：这条探针从一开始量的就是那一家（它的 CDN、它的 `_96.webp`、
        // 它认不认条件头）—— 换成 CurseForge 是另一条链子，要量就单独开一条。
        store.search(key, ResourceCatalog::Kind::Mod, ResourceCatalog::Source::Modrinth,
                     QString());
        if (!pumpUntil([&searched]() { return searched; }, 60000)) {
            std::printf("!! 搜索这一步就没回来（网络这一关没过，量不了）\n");
            return 2;
        }
        std::printf("搜索：%lld ms，%d 条\n", searchClock.elapsed(), int(found.hits.size()));

        QList<ResourceCatalog::Hit> picked;
        QStringList ids;
        for (const ResourceCatalog::Hit &hit : found.hits) {
            if (ids.size() >= wanted) {
                break;
            }
            picked.append(hit);
            ids.append(hit.projectId);
        }
        // "这台机器能不能解 webp"是这一跑最要紧的一个前提（决定了它去要哪一条地址），
        // 所以先把它打出来，再看下面的数才对得上。
        std::printf("webp：这台机器 %s（%s）\n",
                    ImageDecode::webpAvailable() ? "解得了" : "解不了",
                    ImageDecode::webpAvailable() ? "列表图标走 _96.webp 那条小 7 倍的"
                                                 : "列表图标走 icon.png 那条");
        int withIconUrl = 0;
        for (const ResourceCatalog::Hit &hit : picked) {
            if (!hit.iconUrl.isEmpty()) {
                ++withIconUrl;
            }
        }
        std::printf("接口给了图标地址的：%d/%d 条（其余走自己拼的 icon.png）\n", withIconUrl,
                    int(picked.size()));

        // 每一发**第一张**到达的时刻（过期刷新会再发一次，那种不算）。
        QElapsedTimer iconClock;
        QHash<QString, qint64> arrivedAt;
        int emptyCount = 0;
        QObject::connect(&store, &ResourceStore::iconReady, &store,
                         [&arrivedAt, &emptyCount, &iconClock](const QString &id,
                                                               const QPixmap &icon) {
                             if (arrivedAt.contains(id)) {
                                 return;
                             }
                             arrivedAt.insert(id, iconClock.elapsed());
                             if (icon.isNull()) {
                                 ++emptyCount;
                             }
                         });
        iconClock.start();
        for (const ResourceCatalog::Hit &hit : picked) {
            store.requestIcon(hit);
        }
        const bool allBack = pumpUntil(
            [&arrivedAt, &ids]() { return arrivedAt.size() >= ids.size(); }, 180000);

        qint64 last = 0;
        QList<qint64> moments = arrivedAt.values();
        std::sort(moments.begin(), moments.end());
        if (!moments.isEmpty()) {
            last = moments.last();
        }
        std::printf("图标：要了 %d 张，回来 %d 张，空手 %d 张，用了 %lld ms%s\n", int(ids.size()),
                    int(arrivedAt.size()), emptyCount, last, allBack ? "" : "（没等齐，超时了）");
        // 到达时刻的头几个：那道闸起作用时是"前几张各等一个 8 秒，后面的几乎同时到"。
        QStringList line;
        for (int i = 0; i < moments.size() && i < 8; ++i) {
            line << QString::number(moments.at(i));
        }
        std::printf("前 %d 张的到达时刻（ms）：%s\n", int(line.size()),
                    qPrintable(line.join(QStringLiteral(", "))));
        // 盘上那几份：文件名后缀一律是 `.icon`（里面可能是 png 也可能是 webp，见
        // ResourceStore::iconCachePath），所以"到底是哪种"只能**按内容认** —— 这里数一下
        // 就是想知道"webp 那条路真的走通了吗"，光看请求成功是看不出来的。
        const QDir cacheDir(AppPaths::iconCacheDir());
        const QStringList cached = cacheDir.entryList({QStringLiteral("*.icon")}, QDir::Files);
        int webpFiles = 0;
        int pngFiles = 0;
        qint64 totalBytes = 0;
        for (const QString &name : cached) {
            QFile file(cacheDir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray head = file.read(16);
            totalBytes += file.size();
            file.close();
            if (head.startsWith(QByteArrayLiteral("RIFF")) &&
                head.mid(8, 4) == QByteArrayLiteral("WEBP")) {
                ++webpFiles;
            } else if (head.startsWith(QByteArrayLiteral("\x89PNG"))) {
                ++pngFiles;
            }
        }
        std::printf("盘上缓存了 %d 份（webp %d / png %d，共 %lld 字节）\n", int(cached.size()),
                    webpFiles, pngFiles, totalBytes);
        std::printf("再跑一次这条探针：命中盘上那份的话就是秒回、一个请求都不发。\n");

        // ---------- ⑤ 同一趟运行里再要一次（重搜 / 翻页那条路）----------
        // 契约是"盘上/内存有那份的话，requestIcon() 立刻把图发出来、一个请求都不发"。
        // 这里量的就是**当场回来了几条** —— 真正的网络那几条不可能同步回（Qt 的异步请求
        // 至少要下一轮事件循环），所以"同步回的条数"就是"直接从缓存给出去的条数"。
        {
            int emitted = 0;
            int emptyAgain = 0;
            const QMetaObject::Connection conn = QObject::connect(
                &store, &ResourceStore::iconReady, &store,
                [&emitted, &emptyAgain](const QString &, const QPixmap &icon) {
                    ++emitted;
                    if (icon.isNull()) {
                        ++emptyAgain;
                    }
                });
            QElapsedTimer again;
            again.start();
            for (const ResourceCatalog::Hit &hit : picked) {
                store.requestIcon(hit);
            }
            const qint64 elapsed = again.elapsed();
            QObject::disconnect(conn);
            std::printf("同一趟里再要一次：%lld ms 里同步回了 %d/%d 条（空手 %d 条）"
                        "—— 同步回的那些一个请求都没发\n",
                        elapsed, emitted, int(picked.size()), emptyAgain);
        }

        // ---------- ⑥ 核对那一发多少钱（"每次打开用网络刷新一下"的全部代价）----------
        //
        // 量法：挑盘上**头一份有凭据的**图标，地址按当初那条规矩重新挑一遍（ETag 是跟着
        // 地址的，换一条就问不出 304 来），然后用产品自己那个 iconRequest() 真发一发。
        // 这一发和"下次启动那句'变了没有'"是同一句话 —— 同一个函数、同一套头。
        // 不变的条件：上一次跑过、盘上落了图**而且**主机给了凭据（两个都在 meta 那份里）。
        {
            // 挑地址这一步得**先做**：凭据是跟着一条地址的，只有按"这一趟真会去问的
            // 那条地址"去读，才问得出这份凭据是不是它的（读别的地址上那份，一定回空 ——
            // 那会把这条探针判成"这台机器没有凭据"，而这跟被测代码无关）。
            const ResourceCatalog::Hit *probe = nullptr;
            QString probeUrl;
            bool probeUsedGiven = false;
            for (const ResourceCatalog::Hit &hit : picked) {
                // 挑地址这一步要和产品那边**一字不差**（见 ResourceStore::requestIcon）：
                // 接口给的那条这一台用得了就先要它，否则直接要兜底那条。
                const bool givenUsable =
                    !hit.iconUrl.isEmpty() &&
                    (!ResourceCatalog::isWebpUrl(hit.iconUrl) || ImageDecode::webpAvailable());
                const QString urlThis = givenUsable ? hit.iconUrl : hit.iconPngUrl();
                if (!ResourceStore::loadIconValidators(hit.iconCacheId(), urlThis).isEmpty()) {
                    probe = &hit;
                    probeUrl = urlThis;
                    probeUsedGiven = givenUsable;
                    break;
                }
            }
            if (probe == nullptr) {
                std::printf("核对那一发：盘上一份凭据都没有（多半是第一次跑，或者这台机器"
                            "根本没能把图下下来）—— 这一项量不了。再跑一次这条探针就有了。\n");
            } else {
                const QString url = probeUrl;
                const ResourceStore::IconValidators validators =
                    ResourceStore::loadIconValidators(probe->iconCacheId(), url);
                QNetworkAccessManager net;
                QNetworkReply *reply = net.get(ResourceStore::iconRequest(url, validators));
                bool done = false;
                QObject::connect(reply, &QNetworkReply::finished, reply,
                                 [&done]() { done = true; });
                QElapsedTimer clock;
                clock.start();
                const bool answered = pumpUntil([&done]() { return done; }, 20000);
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const qint64 bodyBytes = reply->readAll().size();
                const qint64 ms = clock.elapsed();
                reply->deleteLater();
                const qint64 wholeFile =
                    QFileInfo(ResourceStore::iconCachePath(probe->iconCacheId())).size();
                std::printf("核对那一发（%s%s）：状态 %d，正文 %lld 字节，%lld ms%s\n",
                            qPrintable(probe->iconCacheId()),
                            probeUsedGiven ? " / 接口给的那条" : " / 兜底那条",
                            status, bodyBytes, ms, answered ? "" : "（没回来，超时了）");
                std::printf("     盘上那份整图是 %lld 字节 —— 没变的时候花的正是上面那个零头\n",
                            wholeFile);
                std::printf("     带了什么：If-None-Match=%s，If-Modified-Since=%s\n",
                            validators.etag.isEmpty() ? "（没有）" : qPrintable(validators.etag),
                            validators.lastModified.isEmpty()
                                ? "（没有）"
                                : qPrintable(validators.lastModified));
                if (status == HttpClient::kStatusNotModified) {
                    std::printf("     → 主机答 304（没变）：这一发就是「每次打开核一遍」的全部代价\n");
                } else if (status == 200) {
                    std::printf("     → 主机答 200：它**不认**这两个头，把整张又发了一遍 ——"
                                "内容没变的话产品那边比字节就收手了（不写盘、不发信号），"
                                "但流量是这个数\n");
                }
            }
        }
        return 0;
    }

    const QDir versionsDir(root.filePath(QStringLiteral(".minecraft/versions")));
    // 沙盒里那一棵 versions/ 必须从"干净装一次"开始。`makeInstalledVersion()` 只补不删
    // （缺 jar 就写一个），于是上一趟改过的东西会原样活到这一趟 —— S2b 就栽在这一格上：
    // 上一趟把 wtfml.meta 里的 skipJavaVersionWarning 点成 true 之后，下一趟"那一格本来
    // 勾着（缺省的那一侧）"就恒假，而它点下去反而把值改回"提醒"，后面"当场写进 meta"
    // 也跟着假。这个沙盒是 exe 旁边的一次性目录（不是用户的数据），所以整棵删掉重来；
    // 用链接安全的那一手（万一上一趟被强杀、里头留着 junction，不许顺着它删到外面去）。
    removeTreeWithoutFollowingLinks(versionsDir.absolutePath());
    versionsDir.mkpath(QStringLiteral("."));

    // J 节用：「认不出哪个是客户端 jar」的那个目录 —— 顶层两个 jar、又没有和目录名同名的
    // 那一个。这类目录**继续不算已安装版本**，但必须上报一次（J 节验的就是这个"一次"），
    // 所以它必须在 MainWindow 构造之前摆好，让冷启动那次扫描去发现它。
    //
    // NOTE: 「目录名和 jar 名对不上」的目录**认得出**（那正是判据要救的情形），
    // 不能拿它当"认不出"的例子。真正还认不出的只有"顶层多个 jar"这一类。
    const QString kTwoJarsDir = QStringLiteral("两个jar的目录");
    const QString kTwoJarsA = QStringLiteral("aaa.jar");
    const QString kTwoJarsB = QStringLiteral("bbb.jar");
    makeInstalledVersion(versionsDir, QStringLiteral("1.18.2"));
    makeInstalledVersion(versionsDir, QStringLiteral("26.3"));
    // 带元数据的那个：显示名不再是裸版本号。加载器版本档默认关闭，所以下面
    // 断言里看不到 47.2.0 —— 那是刻意的默认值，不是丢了。
    makeInstalledVersion(versionsDir, QStringLiteral("1.20.1"));
    makeVersionMeta(versionsDir, QStringLiteral("1.20.1"),
                    R"({"name":"我的整合包","loaderName":"Forge","loaderVersion":"47.2.0"})");
    versionsDir.mkpath(kTwoJarsDir);
    for (const QString &jarName : {kTwoJarsA, kTwoJarsB}) {
        QFile jar(versionsDir.filePath(kTwoJarsDir + QLatin1Char('/') + jarName));
        if (jar.open(QIODevice::WriteOnly)) {
            jar.write("stub");
            jar.close();
        }
    }
    std::printf("沙盒：%s\n", qPrintable(versionsDir.absolutePath()));
    std::printf("设置：%s\n", qPrintable(settingsDir));

    const QString kDecorated = QStringLiteral("我的整合包-1.20.1-Forge");

    // X 节要拿 Q_ARG(PlayerSkinProfile, …) 去打 onSkinProfileReady（"档案到了"那条路），
    // 而 QMetaObject::invokeMethod 是按**类型名**找这个参数的 —— 不注册就找不到。
    qRegisterMetaType<PlayerSkinProfile>("PlayerSkinProfile");
    // 同理：Y2c/Y2d 那几节要自己把"列表到了"那一发回复打进 onLoaderVersionsFetched，
    // 参数是带正式版标记的 LoaderVersionList。
    // 产品那边在 VersionManager 的构造函数里注册（发信号的也是它），这里自己再注一次 ——
    // 这一节不经过 VersionManager 也能跑，不能指望它。
    qRegisterMetaType<LoaderMeta::LoaderVersionList>("LoaderMeta::LoaderVersionList");
    // 同理：AA 节要拿 Q_ARG(ModpackImport::Format, …) 去打 applyModpackImport() 的第四个
    // 参数（ 加的：这个函数原来只认 Modrinth 一种清单，现在要按格式分流）。
    // **注册名必须和 Q_ARG 里写的一字不差**：`ModpackImport::Format` 带命名空间，而
    // Q_DECLARE_METATYPE 放在命名空间里的话注册出来的名字是裸的 `Format` ——
    // 对不上就等于没注册，而 invokeMethod 只会**静默返回 false**（看着像"那条路没被拦住"）。
    qRegisterMetaType<ModpackImport::Format>("ModpackImport::Format");

    // ---------- 默认那一跑：外网换成 127.0.0.1 上的一台假服务器 ----------
    // 第一个 MainWindow 一构造就会发版本清单（还有账号那几节的皮肤档案、下载页四页的
    // 首次搜索）。真实网络在场时，这一跑的时长和树里有什么都不归它管：断网机器上每次
    // 都要等满 15 秒，连得上的机器上又会把当天官方那份清单塞进树里。
    //
    // 地址走**产品自己的设置键**（`LauncherSettings` 读、产品永不写），所以这里验的
    // 也正是"这些键真的被用上了"。必须在 `MainWindow window;` **之前**落盘。
    FakeNetwork fakeNet;
    // 夹具起不来就**停**：原来只记一条失败然后照常构造 MainWindow，那一跑实际打的是
    // 真 Mojang —— 断网机器上等满超时，连得上时把当天官方清单塞进树里，
    // 而"离线套件"的名义一点没变。宁可红在夹具上。
    if (!fakeNet.start()) {
        check(false, "默认那一跑的假外网起来了（起不来就不往下跑：会打到真地址上）");
        return 2;
    }
    installFakeNetworkSettings(fakeNet);

    MainWindow window;
    window.show();
    // 7a：窗宽不到 1024 时左导航轨自动折成图标轨（任务卡、实例状态那两格的字都收起）。
    // offscreen 屏只有 800x600，按默认尺寸起会被夹窄 ⇒ 这里按设计稿的标准尺寸放宽，量的是展开的轨。
    window.resize(1280, 800);
    window.ui()->titleBar()->setCollapsed(false);  // 155 起每次启动导航先收起：宽窗也要显式展开
    QApplication::processEvents();

    // 冷启动那一发清单必须落在夹具上：没落到的话（键没读、或者读晚了）它打的是真地址，
    // 这一跑就又回到"看网络脸色"的老样子 —— 而界面上完全看不出来区别。
    pumpUntil([&fakeNet]() { return fakeNet.count(QStringLiteral("/version_manifest_v2.json")) >= 1; },
              5000);
    check(fakeNet.count(QStringLiteral("/version_manifest_v2.json")) == 1,
          "冷启动那一发版本清单打到了夹具上（正好一发）",
          QStringLiteral("次数=%1 最先几条=%2")
              .arg(fakeNet.count(QStringLiteral("/version_manifest_v2.json")))
              .arg(fakeNet.paths.mid(0, 4).join(QStringLiteral(", "))));


    // ---------- 找控件（按 objectName） ----------
    QComboBox *installed = comboByObjectName(&window, MainWindowUI::installedVersionComboName());
    QPushButton *startBtn = startButton(&window);
    // 下载页那一套：版本树 → 安装页，不再有「可安装版本」下拉框。
    QTreeWidget *versionTree =
        window.findChild<QTreeWidget *>(MainWindowUI::versionTreeName());
    QStackedWidget *gameStack =
        window.findChild<QStackedWidget *>(MainWindowUI::downloadGameStackName());
    QLineEdit *installNameEdit =
        window.findChild<QLineEdit *>(MainWindowUI::installNameEditName());
    // 名字不能用时那句原因所在的那行**红字**（名字框底下那句灰色说明已经删了）。
    QLabel *installNameError =
        window.findChild<QLabel *>(MainWindowUI::installNameErrorName());
    QLabel *installTargetLabel =
        window.findChild<QLabel *>(MainWindowUI::installTargetLabelName());
    QPushButton *installBtn = window.findChild<QPushButton *>(MainWindowUI::installButtonName());
    QPushButton *installBackBtn =
        window.findChild<QPushButton *>(MainWindowUI::installBackButtonName());

    // 日志现在是进程级的一条流（不再是每个窗口自己一块面板），所以"报了没报、
    // 报了几次"只能在窗口刚建好这一刻记个基线。后面 G/H 两节还会再开三个 MainWindow，
    // 它们各自也会对同一个沙盒目录报一次 —— 那是另外几个窗口的事，不该混进这一条。
    const int coldStartWarnings = countOccurrences(messagesSnapshot(), kTwoJarsDir);

    check(installed != nullptr, "找得到「已安装版本」下拉框（按 objectName）");
    check(startBtn != nullptr, "找得到「启动游戏」按钮");
    check(versionTree != nullptr, "找得到版本树（按 objectName）");
    check(gameStack != nullptr, "找得到游戏页那两层堆叠（按 objectName）");
    check(installNameEdit != nullptr, "找得到安装页的版本名称输入框（按 objectName）");
    check(installBtn != nullptr, "找得到安装页那个「安装」按钮（按 objectName）");
    check(installBackBtn != nullptr, "找得到「← 返回版本列表」（按 objectName）");

    if (installed == nullptr || versionTree == nullptr || startBtn == nullptr) {
        std::printf("\n===== 有失败: %d/%d =====\n", g_pass, g_pass + g_fail);
        return 1;
    }

    header("A. 冷启动（三个版本都在沙盒里，从没成功启动过任何版本）");
    check(installed->placeholderText() == MainWindowUI::chooseVersionPlaceholder(),
          "有已安装版本时占位文案是『请选择一个版本』，不是『暂无已安装版本』",
          installed->placeholderText());

    check(installed->findData(QStringLiteral("1.18.2")) >= 0 &&
              installed->findData(QStringLiteral("26.3")) >= 0 &&
              installed->findData(QStringLiteral("1.20.1")) >= 0,
          "三个版本都在下拉框里（按 id 找得到）");

    // 没有元数据文件时显示文本必须**等于** id —— 这是"有自定义名前这个功能是隐形的"
    // 的真断言。用户已经装好的那些版本不该因为这次改动换个名字显示。
    {
        const int plain = installed->findData(QStringLiteral("26.3"));
        check(installed->itemText(plain) == QStringLiteral("26.3"),
              "没有 wtfml.meta 的版本，显示文本就是版本号", installed->itemText(plain));
        const int decorated = installed->findData(QStringLiteral("1.20.1"));
        check(installed->itemText(decorated) == kDecorated,
              "有 wtfml.meta 的版本，显示文本是 自定义名-版本号-加载器名",
              installed->itemText(decorated));
    }

    // 有实例必有当前实例：从没启动过、也没手选过 ⇒ 第一个（resolveCurrentInstance()）。
    check(installed->currentIndex() == 0, "有实例必有当前实例：冷启动挑的是第一个",
          QString::number(installed->currentIndex()));
    check(buttonFirstLine(startBtn) == QStringLiteral("启动游戏"),
          "按钮第一行是「启动游戏」", buttonFirstLine(startBtn));
    check(buttonVersionLine(startBtn) == installed->itemText(0),
          "按钮第二行就是自动挑的那一个", buttonVersionLine(startBtn));
    check(startBtn->text().count(QLatin1Char('\n')) == 1, "按钮文字正好两行", startBtn->text());
    check(installed->toolTip().contains(installed->itemData(0).toString()),
          "下拉框 tooltip 跟着自动挑的那一个", installed->toolTip());

    header("B. id 和显示文本分家（这次重构最容易倒退的一条）");
    {
        const int decorated = installed->findData(QStringLiteral("1.20.1"));
        check(installed->itemText(decorated) == kDecorated &&
                  installed->itemData(decorated).toString() == QStringLiteral("1.20.1"),
              "带装饰的版本：itemText 是显示名、itemData 是裸 id",
              installed->itemText(decorated) + " / " +
                  installed->itemData(decorated).toString());
        // 这一对是整次重构的最小复现：按文本找不到，按 id 找得到。
        check(installed->findText(QStringLiteral("1.20.1")) < 0,
              "findText(裸 id) 找不到 —— 显示文本已经不是 id 了");
        check(installed->findData(QStringLiteral("1.20.1")) >= 0, "findData(裸 id) 找得到");
    }

    header("C. 用户手选 → 按钮第二行和 tooltip 跟着变");
    {
        installed->setCurrentIndex(installed->findData(QStringLiteral("26.3")));
        QApplication::processEvents();
        check(buttonVersionLine(startBtn) == QStringLiteral("26.3"),
              "选中 26.3 后第二行是 26.3", buttonVersionLine(startBtn));
        check(installed->toolTip().contains(QStringLiteral("26.3")),
              "tooltip 显示裸版本号", installed->toolTip());

        installed->setCurrentIndex(installed->findData(QStringLiteral("1.20.1")));
        QApplication::processEvents();
        check(buttonVersionLine(startBtn) == kDecorated,
              "选中带自定义名的版本后第二行是带装饰的显示名", buttonVersionLine(startBtn));
        check(selectedId(installed) == QStringLiteral("1.20.1"),
              "但 selectedVersionId 仍然是裸 id", selectedId(installed));
        check(installed->toolTip().contains(QStringLiteral("1.20.1")),
              "tooltip 是裸 id（关掉版本名档后这是唯一能看到它的地方）",
              installed->toolTip());

        installed->setCurrentIndex(installed->findData(QStringLiteral("1.18.2")));
        QApplication::processEvents();
        check(buttonVersionLine(startBtn) == QStringLiteral("1.18.2"),
              "改选 1.18.2 后第二行跟着变", buttonVersionLine(startBtn));
    }

    // 版本清单回来那条刷新现在走后台（`MainWindow::refreshInstalledVersionsAsync()`）：
    // 等版本管理器真把结果发回来再断言 —— 不等的话，"什么都没变"和"还没回来"长得一样。
    const auto waitInstalledScan = [&window] {
        auto *manager = window.findChild<VersionManager *>();
        if (manager == nullptr) return false;
        bool done = false;
        QEventLoop loop;
        const auto connection = QObject::connect(
            manager, &VersionManager::installedVersionsScanned, &loop,
            [&done, &loop] {
                done = true;
                loop.quit();
            });
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(connection);
        return done;
    };

    header("D. 刷新路径（信号被 QSignalBlocker 挡掉，靠手动收尾）");
    {
        // onVersionsFetched() 是 private slot，但走元对象能调到，且它不弹任何对话框
        // （里面只写日志）。它内部会调 refreshInstalledVersionsAsync()，正是要验的那条路。
        const bool invoked = QMetaObject::invokeMethod(
            &window, "onVersionsFetched", Qt::DirectConnection,
            Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(invoked, "调得动 onVersionsFetched（元对象里有这个槽）");
        check(waitInstalledScan(), "后台扫盘在 3 秒内把结果发回来了");
        QApplication::processEvents();

        check(selectedId(installed) == QStringLiteral("1.18.2"),
              "刷新后已安装下拉框保住了原选中项 1.18.2", selectedId(installed));
        check(buttonVersionLine(startBtn) == QStringLiteral("1.18.2"),
              "刷新后按钮第二行没有变陈旧（手动收尾生效）", buttonVersionLine(startBtn));
        check(installed->toolTip().contains(QStringLiteral("1.18.2")),
              "刷新后 tooltip 也没变陈旧", installed->toolTip());
    }

    header("E. 选中的版本消失了 → 按钮不能留个启动不了的版本号");
    {
        QFile jar(versionsDir.filePath(QStringLiteral("1.18.2/1.18.2.jar")));
        check(jar.remove(), "把 1.18.2.jar 抽走（模拟版本被删/损坏）");

        const bool invoked = QMetaObject::invokeMethod(
            &window, "onVersionsFetched", Qt::DirectConnection,
            Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(invoked, "再刷一次");
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();

        check(installed->findData(QStringLiteral("1.18.2")) < 0, "1.18.2 从下拉框里消失了");
        check(installed->count() == 2, "剩下两个版本", QString::number(installed->count()));
        // 当前实例被删 ⇒ 顺延到同一位置上的那一个（1.18.2 原来在第 0 位，现在第 0 位是 1.20.1）。
        check(selectedId(installed) == QStringLiteral("1.20.1"),
              "当前实例没了 ⇒ 顺延到同一位置上的 1.20.1（有实例必有当前实例）", selectedId(installed));
        check(buttonVersionLine(startBtn) == kDecorated,
              "按钮第二行换成顺延到的那一个，没留着已经启动不了的 1.18.2",
              buttonVersionLine(startBtn));
        check(installed->placeholderText() == MainWindowUI::chooseVersionPlaceholder(),
              "还有可选版本，所以占位文案仍是『请选择一个版本』",
              installed->placeholderText());

        makeInstalledVersion(versionsDir, QStringLiteral("1.18.2"));
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(installed->count() == 3 && selectedId(installed) == QStringLiteral("1.18.2"),
              "1.18.2 回来了、这期间没手选别的 ⇒ 当前实例换回它（顺延只是替身）",
              QString("count=%1 idx=%2").arg(installed->count()).arg(installed->currentIndex()));

        // 再挤掉一次，但这回用户在它不在的时候手选了 26.3 ⇒ 它回来也不抢回去。
        check(QFile::remove(versionsDir.filePath(QStringLiteral("1.18.2/1.18.2.jar"))), "再抽走一次 1.18.2.jar");
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        installed->setCurrentIndex(installed->findData(QStringLiteral("26.3")));
        QApplication::processEvents();
        makeInstalledVersion(versionsDir, QStringLiteral("1.18.2"));
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(selectedId(installed) == QStringLiteral("26.3"),
              "被挤掉期间用户手选了 26.3 ⇒ 1.18.2 回来也不抢回去", selectedId(installed));
        // 收尾：后面几段按 1.18.2 是当前实例写的，手选回去（和用户点一下同一条路）。
        installed->setCurrentIndex(installed->findData(QStringLiteral("1.18.2")));
        QApplication::processEvents();
    }

    header("F. 真的一个版本都没装 → 占位文案退回「暂无已安装版本」");
    {
        // 单独造一个只有空目录的沙盒窗口是做不到的（沙盒是全局的），所以这里
        // 直接验另一半：把目录整个挪走，看占位文案会不会跟着变。
        const QString stash = root.filePath(QStringLiteral("stash-versions"));
        QDir(stash).removeRecursively();
        QDir().rename(versionsDir.absolutePath(), stash);
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList()));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(installed->count() == 0, "一个条目都没有", QString::number(installed->count()));
        check(installed->placeholderText() == MainWindowUI::noInstalledVersionPlaceholder(),
              "空清单时占位文案是『暂无已安装版本』", installed->placeholderText());
        check(buttonVersionLine(startBtn) == QStringLiteral("未选择版本"),
              "按钮第二行仍是「未选择版本」", buttonVersionLine(startBtn));

        QDir().rename(stash, versionsDir.absolutePath());
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(installed->count() == 3, "版本目录回来后三个又都在",
              QString::number(installed->count()));
    }

    header("G. 启动恢复：打开时自动选上上次启动过的版本");
    {
        rememberLastVersion(QStringLiteral("26.3"));
        {
            MainWindow w2;
            w2.show();
            QApplication::processEvents();
            QComboBox *combo2 =
                comboByObjectName(&w2, MainWindowUI::installedVersionComboName());
            QPushButton *btn2 = startButton(&w2);
            check(combo2 != nullptr && btn2 != nullptr, "第二个窗口的控件找得到");
            if (combo2 != nullptr && btn2 != nullptr) {
                check(selectedId(combo2) == QStringLiteral("26.3"),
                      "开窗就选中了记着的 26.3（走的是 QSettings，不是成员变量）",
                      selectedId(combo2));
                check(buttonVersionLine(btn2) == QStringLiteral("26.3"),
                      "按钮第二行与之一致", buttonVersionLine(btn2));
                check(combo2->placeholderText() == MainWindowUI::chooseVersionPlaceholder(),
                      "占位文案不因为『记住了』而变；它只描述『有没有可选版本』",
                      combo2->placeholderText());
            }
        }
        // 跨实例：再开一个，记着的还是 26.3（上一个是同一个进程里 new 出来的，
        // 但如果值只活在成员里，第二个窗口就选不上了）
        {
            MainWindow w3;
            w3.show();
            QApplication::processEvents();
            QComboBox *combo3 = comboByObjectName(&w3, MainWindowUI::installedVersionComboName());
            check(combo3 != nullptr && selectedId(combo3) == QStringLiteral("26.3"),
                  "第三个窗口读到的仍是 26.3（值确实落了盘）",
                  combo3 == nullptr ? QStringLiteral("<没有控件>") : selectedId(combo3));
        }
    }

    header("H. 记着的版本已经不在了 → 按规则挑一个（上次启动 → 上次选中 → 第一个）");
    {
        rememberLastVersion(QStringLiteral("26.3"));
        QFile jar(versionsDir.filePath(QStringLiteral("26.3/26.3.jar")));
        check(jar.remove(), "抽走 26.3.jar");
        {
            MainWindow w4;
            w4.show();
            QApplication::processEvents();
            QComboBox *combo4 =
                comboByObjectName(&w4, MainWindowUI::installedVersionComboName());
            QPushButton *btn4 = startButton(&w4);
            check(combo4 != nullptr && btn4 != nullptr, "四个窗口的控件找得到");
            if (combo4 != nullptr && btn4 != nullptr) {
                check(combo4->currentIndex() == 0, "记着的 26.3 不在了 ⇒ 挑第一个（有实例必有当前实例）",
                      QString::number(combo4->currentIndex()));
                check(combo4->placeholderText() == MainWindowUI::chooseVersionPlaceholder(),
                      "占位文案是『请选择一个版本』（还有别的版本可选），不是『暂无已安装版本』",
                      combo4->placeholderText());
                check(buttonVersionLine(btn4) == combo4->itemText(0),
                      "按钮第二行是挑中的那一个", buttonVersionLine(btn4));
                check(combo4->toolTip().contains(combo4->itemData(0).toString()),
                      "tooltip 跟着挑中的那一个", combo4->toolTip());
            }
        }
        makeInstalledVersion(versionsDir, QStringLiteral("26.3"));
    }

    header("J. 实例判据：认得出的进来，认不出的说一次、不刷屏");
    {
        // 沙盒里的 `两个jar的目录/aaa.jar + bbb.jar` 在 MainWindow 构造之前就摆好了，
        // 所以冷启动那次扫描已经该报过一条。
        //
        // NOTE: 收的是 Qt 的日志消息（MainWindow::appendLog 现在写 qDebug），不再是
        // 界面上的文本框 —— 那个面板 分选项卡时去掉了。这条通道正是
        // "从命令行启动才看得到"的那条，所以它必须继续有内容。
        check(!messagesSnapshot().isEmpty(), "整个会话写出来过日志");
        check(window.findChild<QTextEdit *>() == nullptr,
              "窗口里确实没有日志面板了（日志只在命令行里）");
        check(coldStartWarnings == 1, "冷启动时对认不出的目录恰好写了一条日志",
              QString::number(coldStartWarnings));
        check(countOccurrences(messagesSnapshot(), kTwoJarsA) >= 1 &&
                  countOccurrences(messagesSnapshot(), kTwoJarsB) >= 1,
              "那条日志把两个候选 jar 的名字都点了出来");

        // 再走两遍刷新（onVersionsFetched 和 onVersionDownloadFinished 都会调
        // refreshInstalledVersions()，一次会话里它至少被调三次）。这两遍必须
        // **一个字都不再写** —— 否则每次刷新都在日志里留一条同样的抱怨。
        // 比的是"这条消息的总数有没有变"，不是某个窗口自己的面板。
        const int beforeRefresh = countOccurrences(messagesSnapshot(), kTwoJarsDir);
        for (int i = 0; i < 2; ++i) {
            QMetaObject::invokeMethod(
                &window, "onVersionsFetched", Qt::DirectConnection,
                Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
            check(waitInstalledScan(), "后台扫盘回来了");
            QApplication::processEvents();
        }
        check(countOccurrences(messagesSnapshot(), kTwoJarsDir) == beforeRefresh,
              "又刷新两次之后一条都没有增加",
              QString::number(countOccurrences(messagesSnapshot(), kTwoJarsDir)));

        // 上报归上报，"不算已安装版本"这条判据没动：它不该出现在下拉框里。
        // （判据放松的是"jar 名和目录名必须一致"，不是"认不出也收进来"。）
        const auto listedIds = [&]() {
            QStringList ids;
            for (int i = 0; i < installed->count(); ++i) {
                ids << installed->itemData(i).toString();
            }
            return ids;
        };
        check(!listedIds().contains(kTwoJarsDir), "认不出的目录仍然不在已安装列表里");
        check(installed->count() == 3, "已安装列表还是那 3 个",
              QString::number(installed->count()));

        // ---- 另一半：用户自己在资源管理器里改过目录名的实例必须认得出来 ----
        // 这是判据要救的那个真机场景（388 MB / 851 MB 的整合包各一个，会从列表里
        // 静默消失）。现造一个出来再刷新：目录名是「改名了的整合包」，里面躺着的是
        // 「旧名字.jar」和「旧名字.json」。
        const QString renamedDir = QStringLiteral("改名了的整合包");
        const QString renamedJar = QStringLiteral("旧名字.jar");
        versionsDir.mkpath(renamedDir);
        {
            QFile file(versionsDir.filePath(renamedDir + QLatin1Char('/') + renamedJar));
            if (file.open(QIODevice::WriteOnly)) {
                file.write("stub");
                file.close();
            }
        }
        // 那份 json 必须写得**能解析**（原来和 jar 一样是一串 "stub"）。
        // 实例行前面那个图标（用户要的"每个版本一个对的图标"）要读它的 `type` —— 走的
        // 正是 `VersionManager::getVersionJson()`，而它遇到解析不了的文件会 qWarning
        // （"Failed to parse ..."）。真机上被外部改过名的实例手里当然是一份真 json；
        // 这里图省事写一串假的，量出来的就是"认得出目录却每次刷新抱怨一句"，而这一节
        // 验的恰恰是「认得出来的目录**一条告警都不写**」。
        {
            QFile file(versionsDir.filePath(renamedDir + QLatin1Char('/') + QStringLiteral("旧名字.json")));
            if (file.open(QIODevice::WriteOnly)) {
                file.write("{\"id\":\"旧名字\"}");
                file.close();
            }
        }
        const int beforeRenameScan = countOccurrences(messagesSnapshot(), renamedDir);
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();

        check(listedIds().contains(renamedDir),
              "外部改过名的实例被认出来了、进了已安装列表", listedIds().join(QStringLiteral(",")));
        check(installed->count() == 4, "列表里多出来它一个",
              QString::number(installed->count()));
        // 红了的时候把"到底是哪一句话"带出来 —— 不写出来的话只能看到数字不等，还得回去
        // 复现一遍才能知道是谁在抱怨（这一条只能看消息，消息本身就是被验的东西）。
        const auto messagesNaming = [](const QString &needle) {
            QStringList hits;
            for (const QString &message : messagesSnapshot()) {
                if (message.contains(needle)) {
                    hits << message;
                }
            }
            return hits.join(QStringLiteral(" ／ "));
        };
        check(countOccurrences(messagesSnapshot(), renamedDir) == beforeRenameScan,
              "认得出来的目录一条告警都不写",
              messagesNaming(renamedDir));

        // 认出来还不够 —— 路径也得跟着解析，否则是"列表里看得见、点启动报缺 jar"。
        {
            VersionManager probe;
            check(probe.installedVersions().contains(renamedDir),
                  "数据层也认它（不只是界面把它画上去了）");
        }

        // 收拾干净，免得影响后面几节对"沙盒里有几个实例"的计数
        QDir(versionsDir.filePath(renamedDir)).removeRecursively();
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(installed->count() == 3, "收拾之后回到那 3 个",
              QString::number(installed->count()));
    }

    header("K. 底部状态条：三格都接上了真的东西");
    {
        QFrame *bar = window.findChild<QFrame *>(MainWindowUI::bottomBarName());
        QLabel *avatar = window.findChild<QLabel *>(MainWindowUI::avatarLabelName());
        QLabel *account = window.findChild<QLabel *>(MainWindowUI::accountLabelName());
        QLabel *status = window.findChild<QLabel *>(MainWindowUI::statusLabelName());
        QLabel *runState = window.findChild<QLabel *>(MainWindowUI::runStateLabelName());
        QLabel *runDetail =
            window.findChild<QLabel *>(MainWindowUI::runStateDetailLabelName());
        QProgressBar *progress = window.findChild<QProgressBar *>(MainWindowUI::progressBarName());
        QTabWidget *tabs = window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());

        check(bar != nullptr && avatar != nullptr && account != nullptr && status != nullptr &&
                  runState != nullptr && runDetail != nullptr && progress != nullptr,
              "底栏那几块都能按 objectName 找回来");
        check(tabs != nullptr && tabs->count() == 6, "真窗口里是六个选项卡（含工具箱）");

        if (account != nullptr && avatar != nullptr) {
            // 左下角显示的名字来自 MainWindow 自己那个 AuthManager。测试这边新起一个
            // 同样的 AuthManager（同一份沙盒 QSettings、同样没有登录过），两边的
            // playerName() 必须一致 —— 不一致就说明 MainWindow 根本没读它。
            AuthManager probe;
            check(account->text() == probe.playerName(),
                  "左下角显示的是 AuthManager 里的名字", account->text());
            // 头像现在是**真贴图抠出来的那一块**（没登录时是默认那套经典外观的头），
            // 不再是首字母方块 —— 所以判据从 text 换成了 pixmap。
            check(!avatar->pixmap().isNull() &&
                      avatar->pixmap().size() ==
                          QSize(MainWindowUI::avatarSize(), MainWindowUI::avatarSize()),
                  "没登录时头像画的是默认外观那个头（32×32 的贴图，不是首字母方块）",
                  QStringLiteral("%1x%2")
                      .arg(avatar->pixmap().width())
                      .arg(avatar->pixmap().height()));
            check(avatar->text().isEmpty(), "头像上不再写首字母了", avatar->text());
            // 没登录过时 AuthManager 给的是默认名，而且它同时是启动游戏时 --username 的
            // 那个值（"离线登录也用这个名字"）—— 所以这里把默认名也钉住，
            // 免得哪天有人顺手把它翻译成中文：那会变成发给游戏的用户名。
            check(probe.playerName() == QStringLiteral("Player"),
                  "默认玩家名是 Player（不是空的、也不该是翻译过的文案）", probe.playerName());
        }

        if (progress != nullptr && status != nullptr) {
            // "中间的进度条如果正在下载"——现在没在下载也没在启动 ⇒ 那根条藏着。
            // 但那一**格**自己（和那句「暂无后台任务」）是亮的： 任务中心那一次
            // 改的（原来整格藏起来）—— 它现在是"点一下展开任务中心"那颗按钮。
            check(!progress->isVisible() && status->isVisible() &&
                      status->text() == MainWindowUI::taskSummaryNoTaskText(),
                  "没有下载也没有启动时：那根条藏着、那一格写着占位话", status->text());
            // 刷新（版本清单到达）不算"在下载"：它既不该把进度条放出来，也不该在那格写出别的话。
            QMetaObject::invokeMethod(
                &window, "onVersionsFetched", Qt::DirectConnection,
                Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
            check(waitInstalledScan(), "后台扫盘回来了");
            QApplication::processEvents();
            check(!progress->isVisible() &&
                      status->text() == MainWindowUI::taskSummaryNoTaskText(),
                  "刷新版本清单之后那一格还是占位话（刷新≠下载）", status->text());
        }

        QLabel *runMark = window.findChild<QLabel *>(MainWindowUI::runStateMarkName());
        if (runState != nullptr && runDetail != nullptr && runMark != nullptr) {
            // 这个进程里没起过游戏：轨底那一格常驻（进程起停时轨上不增减元素），写「未运行」。
            QPushButton *runButton =
                window.findChild<QPushButton *>(MainWindowUI::runStateButtonName());
            check(runButton != nullptr && !runButton->isHidden() &&
                      runState->text() == MainWindowUI::runStateNotRunningText(),
                  "没有进程在跑时轨底运行状态那一格照旧在，写「未运行」", runState->text());
            check(runState->text() != MainWindowUI::runStateRunningCountText(1) &&
                      runState->text() != MainWindowUI::runStateRunningCountText(2),
                  "（不会说成「%1 个进程运行中」）",
                  runState->text());
            // 那颗方块这时候是**灰的**（★ 用户改的：「黄和绿不变，红色改有实例
            // 报错，没有实例用灰色」），而且不是藏着的（四档都有一颗亮的方块）。
            // 判据是"量到的那张图的像素" —— 不是再调一次 runStateMarkColor()：那个函数
            // 是取色的规则，这里要验的是"MainWindow 真的按那个规则摆了图"。
            check(!runMark->pixmap().isNull(),
                  "那颗状态点备好了图（格子收着，图照样按状态换）", QStringLiteral("%1x%2")
                                                       .arg(runMark->pixmap().width())
                                                       .arg(runMark->pixmap().height()));
            const QPalette markPalette = runStateMarkPalette(&window);
            const QColor wantGrey =
                MainWindowUI::runStateMarkColor(MainWindowUI::RunStateMark::Idle, markPalette);
            const QImage got = runMark->pixmap().toImage();
            // 空图不许调 pixelColor()（图是空的时候那一下是没定义的），先出图再量色。
            const QColor gotColor = got.isNull() ? QColor()
                                                 : got.pixelColor(got.width() / 2, got.height() / 2);
            check(!got.isNull() && gotColor == wantGrey &&
                      gotColor != MainWindowUI::runStateMarkColor(
                                      MainWindowUI::RunStateMark::Running, markPalette) &&
                      gotColor != MainWindowUI::runStateMarkColor(
                                      MainWindowUI::RunStateMark::Error, markPalette),
                  "没实例在跑 → 方块画的是**灰**那一档（既不是绿的，也不是「有实例异常退出」那个红）",
                  QStringLiteral("量到 %1，灰该是 %2、绿是 %3、红是 %4")
                      .arg(gotColor.name())
                      .arg(wantGrey.name())
                      .arg(MainWindowUI::runStateMarkColor(MainWindowUI::RunStateMark::Running,
                                                           markPalette)
                               .name())
                      .arg(MainWindowUI::runStateMarkColor(MainWindowUI::RunStateMark::Error,
                                                           markPalette)
                               .name()));
            // 再来一条**不看色值**的：量到的那个像素"说不上是什么颜色"。上面那条证的是
            // "和按规则算出来的一模一样"，这条证的是"那确实是一档灰"，两条一起才挡得住
            // "规则和用法一起走偏"（比如两边都用深色档的取色，等式照样成立）。
            // NOTE: 这条**不能**写成 `runStateHueBand(...) == 0`（那是它还是红的时候的写法）：
            // 灰没有色相，hueBand 对它回 -1，跟"认不出"是同一个值。
            check(runStateIsGrey(gotColor),
                  "而且那个像素确实是一档**灰**（饱和度低到说不上是哪个颜色）", gotColor.name());
            // 第二行这时候是空的（没有实例名可说）：藏起来 + 文字清空，两件事都要
            check(!runDetail->isVisible() && runDetail->text().isEmpty(),
                  "没有实例在跑时第二行是空的（藏起来，不留上一句的残影）", runDetail->text());
        }
    }

    header("X. 主页右栏那个小人 + 底栏头像：接线");
    if (const int result = MwPlayerSkinChecks(window); result != 0) return result;
    header("X2. 主页实例带：收起状态落盘、数量上限");
    if (const int result = MwHomeQuickCollapseChecks(window); result != 0) return result;

    // ---------- 设置页的控件（按 objectName，L/M 两节共用） ----------
    // 设置页「Java 版本」那一格从下拉框换成了「Java 管理…」这颗按钮
    // （用户点名的改法）。**选哪个 Java** 那件事搬进了那一页，所以这一层能读的东西
    // 只剩"那颗按钮在不在"和"说明文字说的是哪一档"。那一页自己由 T26 那一节验。
    QPushButton *javaManageButton =
        byObjectName<QPushButton>(&window, MainWindowUI::javaManageButtonName());
    QLabel *javaLabel = window.findChild<QLabel *>(MainWindowUI::javaPathLabelName());
    QCheckBox *javaWarn =
        byObjectName<QCheckBox>(&window, MainWindowUI::javaWarnMismatchCheckName());
    QComboBox *memoryMode =
        byObjectName<QComboBox>(&window, MainWindowUI::memoryModeComboName());
    QLabel *memoryHint = window.findChild<QLabel *>(MainWindowUI::memoryHintName());
    QSpinBox *maxSpin = byObjectName<QSpinBox>(&window, MainWindowUI::memoryMaxSpinName());
    auto *maxSlider = static_cast<MemorySlider *>(
        byObjectName<QSlider>(&window, MainWindowUI::memoryMaxSliderName()));
    QComboBox *behaviourCombo =
        byObjectName<QComboBox>(&window, MainWindowUI::launcherBehaviourComboName());
    QComboBox *sourceCombo =
        byObjectName<QComboBox>(&window, MainWindowUI::downloadSourceComboName());
    QPlainTextEdit *jvmEdit =
        byObjectName<QPlainTextEdit>(&window, MainWindowUI::extraJvmArgsEditName());
    // 「全局额外参数」失焦即存：信号源就是失焦那个过滤器。
    SettingsParts::CommitOnFocusOut *applyJvm = window.ui()->getExtraJvmArgsCommit();
    // 「启动选项」那一组（AE 节用）
    QCheckBox *showLogCheck =
        byObjectName<QCheckBox>(&window, MainWindowUI::showGameLogCheckName());
    QComboBox *priorityCombo =
        byObjectName<QComboBox>(&window, MainWindowUI::processPriorityComboName());
    QSpinBox *windowWidthSpin =
        byObjectName<QSpinBox>(&window, MainWindowUI::gameWindowWidthSpinName());
    QSpinBox *windowHeightSpin =
        byObjectName<QSpinBox>(&window, MainWindowUI::gameWindowHeightSpinName());
    QComboBox *windowModeCombo =
        byObjectName<QComboBox>(&window, MainWindowUI::gameWindowModeComboName());
    // 两个数字框中间那个「×」。它也得跟着灰 —— 灰了两个框却留一个亮着的 × 是那种"看着
    // 像坏了"的界面。
    QLabel *windowSizeTimes = window.findChild<QLabel *>(MainWindowUI::gameWindowSizeTimesLabelName());
    // 「外观」那一组（AI 节用）：那一格在 UI 问题 #3 里撤掉了（商标固定），所以 AI 节量的是
    // "它不在"。启动页「窗口大小 · 恢复默认」那颗按钮也在这一组里找。
    QPushButton *gameWindowSizeReset =
        byObjectName<QPushButton>(&window, MainWindowUI::gameWindowSizeResetButtonName());
    const bool settingsWired = javaManageButton != nullptr && javaLabel != nullptr &&
                               javaWarn != nullptr &&
                               memoryMode != nullptr && memoryHint != nullptr &&
                               maxSpin != nullptr && maxSlider != nullptr &&
                               behaviourCombo != nullptr && sourceCombo != nullptr &&
                               jvmEdit != nullptr && applyJvm != nullptr &&
                               showLogCheck != nullptr && priorityCombo != nullptr &&
                               windowWidthSpin != nullptr && windowHeightSpin != nullptr &&
                               windowModeCombo != nullptr && windowSizeTimes != nullptr &&
                               gameWindowSizeReset != nullptr;

    GlobalJavaSettings *windowJava = window.findChild<GlobalJavaSettings *>();
    const std::function<void(const QString &)> pinGlobalJava = [windowJava](const QString &path) {
        if (windowJava == nullptr) {
            return;
        }
        windowJava->setAutoSelectJava(false);
        windowJava->setJavaPath(path);
        // 落盘：新造的读者（下面几节里那几个 `GlobalJavaSettings probe;`）要读得到。
        windowJava->save();
    };
    const std::function<void()> unpinGlobalJava = [windowJava] {
        if (windowJava == nullptr) {
            return;
        }
        // 「自动」那一档**刻意不清路径**（那条路径同时是"上次挑中的"的缓存），只翻开关。
        windowJava->setAutoSelectJava(true);
        windowJava->save();
    };

    header("L. 设置页：开窗时就把盘上的值灌进控件了");
    // 沙盒的 ini 在开窗前刚被删过、这个窗口也没改过任何设置，所以这里读到的应当全是
    // 默认值。断言因此拿**另一个读盘者**当对照（同一个 ini 里 new 一个
    // GlobalJavaSettings / LauncherSettings），而不是写死 4096 这类字面量：写死只能
    // 证明"我抄对了数字"，拿对照实例才能证明"界面显示的就是盘上存的那个值"。
    check(settingsWired, "设置页那二十个控件都能按 objectName 找回来");
    if (const int result = MwJavaPreferencesChecks(settingsWired, window, javaManageButton, javaLabel, javaWarn, memoryMode, memoryHint, maxSpin, behaviourCombo, sourceCombo, jvmEdit, maxSlider); result != 0) return result;

    header("M. 在设置页改东西：立刻落盘、立刻生效");
    if (const int result = MwMemoryPreferencesChecks(settingsWired, kDecorated, window, installed, memoryMode, memoryHint, maxSpin, behaviourCombo, sourceCombo, jvmEdit, applyJvm, maxSlider); result != 0) return result;

    header("N. 冷启动读盘：手动指定的 Java 和镜像源都读得回来");
    if (const int result = MwJavaSelectionChecks(root); result != 0) return result;

    header("P. 游戏文件夹：列表、切换与落盘");
    if (const int result = MwFolderSwitchingChecks(root, window); result != 0) return result;

    header("Q. 登录：错误话术、拿不到令牌的原因、界面的三个状态");
    if (const int result = MwLoginCompletionChecks(); result != 0) return result;

    header("R. 令牌存盘往返：开窗时从盘上把上次登录读回来");
    // ------------------------------------------------------------------
    // NOTE: 这一段补的是 暴露出来的空白 —— 四个验证工程里 MinecraftTokenData /
    // MicrosoftTokenData 一次都没出现过，也就是说"盘上有令牌 → 重开启动器 → 底栏显示
    // 上次那个名字"这条路上**一条断言都没有**。那天真机上底栏一直显示占位的 Player，
    // 查了一整轮才发现是外部写入者多写了一个字节（Qt 靠 @ByteArray 结尾那个 ')' 认出
    // 包装，多一个 NUL 就整段读不出来，还不报错）。
    // 这里钉的是**读这一侧**，而且刻意按"外部写入者"的视角写：键名和 obfuscate() 那套
    // 算法都照抄字面量 —— 那正是任何往这个存储里写值的东西必须对上的合同。
    const QString kTokenKey = QStringLiteral("MinecraftTokenData");
    {
        const QByteArray json =
            R"({"access_token":"mw-check-token","expires_at":"2999-01-01T00:00:00Z",)"
            R"("name":"Steve","uuid":"069a79f444e94726a5befca90e38aaf5"})";
        const QByteArray key = QCryptographicHash::hash(
            QByteArray("WTFMLSecretKey2026"), QCryptographicHash::Sha256);
        const QByteArray plain = json.toBase64();
        QByteArray stored;
        stored.reserve(plain.size());
        for (int i = 0; i < plain.size(); ++i) {
            stored.append(plain.at(i) ^ key.at(i % key.size()));
        }

        QSettings raw;
        raw.setValue(kTokenKey, stored);
        raw.sync();

        // 新起一个 AuthManager 就等于"刚开起来的启动器"：构造函数里那次读盘说了算。
        AuthManager probe;
        check(probe.playerName() == QStringLiteral("Steve"),
              "开窗时从盘上读回了上次登录的用户名", probe.playerName());
        check(probe.loggedIn(), "盘上有档案时 loggedIn() 是 true");
        check(probe.playerUuid() == QStringLiteral("069a79f4-44e9-4726-a5be-fca90e38aaf5"),
              "读回来的 UUID 是补好横线的标准写法", probe.playerUuid());

        // 令牌还没过期 → 拿启动令牌不该碰网络（这条同时钉住了 expires_at 的解析）
        QString reason;
        const QString token = probe.loadSavedToken(&reason);
        check(token == QStringLiteral("mw-check-token"),
              "有效期内 loadSavedToken() 直接用盘上那张，不重新换", token);
        check(reason.isEmpty(), "成功时不该带着失败原因", reason);

        // 界面也得跟着变：底栏真名 + 第二行「正版登录」。
        MainWindow w8;
        w8.show();
        QApplication::processEvents();
        QLabel *account = byObjectName<QLabel>(&w8, MainWindowUI::accountLabelName());
        QLabel *avatar = byObjectName<QLabel>(&w8, MainWindowUI::avatarLabelName());
        QLabel *accountType = byObjectName<QLabel>(&w8, MainWindowUI::accountTypeLabelName());
        check(account != nullptr && avatar != nullptr && accountType != nullptr,
              "第八个窗口里用户名 / 头像 / 底栏第二行都找得到");
        if (account != nullptr && avatar != nullptr && accountType != nullptr) {
            check(account->text() == QStringLiteral("Steve"),
                  "底栏左下显示的是盘上那个真名，不是占位的 Player", account->text());
            // 头像现在是贴图（不是首字母了）。这一屏盘上有令牌、沙盒里却没有外观缓存，
            // 所以拿到的是**默认外观那个头** —— 真皮肤要等 PlayerSkinStore 从官方要回来，
            // 那件事由 X 节直接驱动 applyPlayerSkin() 来验，不依赖网络。
            check(!avatar->pixmap().isNull() && avatar->text().isEmpty(),
                  "登录状态下头像也是一块贴图（不再是首字母）", avatar->text());
            // 这条原来钉的是那个按钮写着「重新登录」（按钮已经并进「账号与外观」那个窗了）。
            // 换成钉**它想说的那件事**：盘上有可用凭据 → 这次启动用的是正版身份。
            check(accountType->text() == MainWindowUI::accountTypeOnlineText(),
                  "盘上有可用凭据 → 底栏第二行说「正版登录」，不是「未登录」",
                  accountType->text());
        }

        // 负例：把令牌拿走 → 又回到"没登录过"的模样。没有这一条，上面几条可能只是恒真。
        raw.remove(kTokenKey);
        raw.sync();
        AuthManager cleared;
        check(cleared.playerName() == QStringLiteral("Player") && !cleared.loggedIn(),
              "盘上没令牌时又回到占位的 Player（上面几条不是恒真）", cleared.playerName());
    }

    header("R2. 令牌存盘往返（新格式）：DPAPI 密文读得回来，坏值不会读成垃圾");
    // ------------------------------------------------------------------
    // R 节钉的是**旧**格式（XOR 混淆）还读得出来 —— 那是向后兼容，老用户盘上躺着的那份不能废。
    // 这一节钉新格式： 起产品写的是 "WTFML-PROTECTED-1\n" + DPAPI(base64(JSON))，
    // 密文绑定当前 Windows 用户，令牌不再跟着那个 ini 一起被抄走。
    // 写法和 R 一致：以"外部写入者"的视角自己造字节，验产品的读。
    {
        // 这里照样**自己调** CryptProtectData，而不是用产品里那个 protectToken()（它是私有的）——
        // 独立实现写、产品读，才算真的验了格式；拿产品的写去喂产品的读是同义反复。
        auto dpapi = [](const QByteArray &in, bool encrypt) -> QByteArray {
            DATA_BLOB src{};
            src.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(in.constData()));
            src.cbData = DWORD(in.size());
            DATA_BLOB out{};
            const BOOL ok =
                encrypt ? CryptProtectData(&src, L"mw_check", nullptr, nullptr, nullptr,
                                           CRYPTPROTECT_UI_FORBIDDEN, &out)
                        : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr,
                                             CRYPTPROTECT_UI_FORBIDDEN, &out);
            if (!ok) {
                return QByteArray();
            }
            const QByteArray result(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
            LocalFree(out.pbData);
            return result;
        };

        const QByteArray magic = QByteArrayLiteral("WTFML-PROTECTED-1\n");
        const QByteArray json =
            R"({"access_token":"mw-check-dpapi","expires_at":"2999-01-01T00:00:00Z",)"
            R"("name":"Alex","uuid":"069a79f444e94726a5befca90e38aaf6"})";
        const QByteArray protectedValue = magic + dpapi(json.toBase64(), true);

        QSettings raw;
        raw.setValue(kTokenKey, protectedValue);
        raw.sync();

        // 盘上那份必须是密文：明文 token 一个字都不该露出来。这条是"真加密 vs 混淆"的分界线 ——
        // 谁要是把写入退回旧的 XOR，这一段会红。
        const QByteArray stored = raw.value(kTokenKey).toByteArray();
        check(stored.startsWith(magic), "盘上那份带新格式的前缀", stored.left(20));
        check(!stored.contains("mw-check-dpapi"), "密文里不含明文 token（XOR 混淆就藏不住）");

        AuthManager probe;
        check(probe.playerName() == QStringLiteral("Alex"),
              "新格式（DPAPI）开窗时读回了用户名", probe.playerName());
        check(probe.loggedIn() && probe.playerUuid() == QStringLiteral("069a79f4-44e9-4726-a5be-fca90e38aaf6"),
              "新格式读回来的 UUID 也对", probe.playerUuid());
        QString reason;
        check(probe.loadSavedToken(&reason) == QStringLiteral("mw-check-dpapi") && reason.isEmpty(),
              "新格式在有效期内直接给出盘上那张令牌，不换", reason);

        // 负例：密文动一个字节 → DPAPI 解不开 → 必须老老实实退回"没登录"，
        // 而不是把垃圾解成半截 JSON。没有这一条，"能读出来"可能只是因为读了个空。
        QByteArray broken = protectedValue;
        broken[broken.size() / 2] = char(broken.at(broken.size() / 2) ^ 0xff);
        raw.setValue(kTokenKey, broken);
        raw.sync();
        AuthManager corrupted;
        check(corrupted.playerName() == QStringLiteral("Player") && !corrupted.loggedIn(),
              "密文被改坏时退回占位的 Player，不把垃圾当登录", corrupted.playerName());

        raw.remove(kTokenKey);
        raw.sync();
    }

    header("S. 实例重命名：真改目录名 + 那些「跟着走」的状态");
    if (const int result = MwRenameStateChecks(versionsDir); result != 0) return result;

    header("S2. 实例设置页：没有确定/取消，值一改就落盘，出口只有那颗返回箭头");
    if (const int result = MwInstanceRenameChecks(versionsDir, window); result != 0) return result;

    header("T. 实例分组：树怎么画、组头、跟着游戏文件夹走");
    if (const int result = MwInstanceGroupsChecks(root, versionsDir); result != 0) return result;

    header("T37. 实例页的搜索与筛选：搜什么 / 四颗 chip / 清空恢复原样");
    if (const int result = MwInstanceFiltersChecks(); result != 0) return result;

    header("T39. 服务端那一半：模式切换的接线 + 建服务端真的落了盘");
    if (const int result = MwServerCreationChecks(root); result != 0) return result;

    header("T40. 服务端安装那四段真的跑起来");
    if (const int result = MwIntegrityActionsChecks(root); result != 0) return result;

    header("T18. 实例设置「概览」栏那四行：图标+名称 / 两颗按钮 / 置顶勾选 / 导出占位");
    if (const int result = MwInstanceOverviewChecks(root, versionsDir); result != 0) return result;

    header("U. 每实例设置：实例覆盖写在实例里，不渗到全局");
    if (const int result = MwOverridePersistenceChecks(root, settingsDir, versionsDir, maxSpin, maxSlider); result != 0) return result;

    header("AG. 左下角那颗状态方块：走产品那条启动路，黄的那一态真的亮起来");
    if (const int result = MwRunStateLifecycleChecks(root, versionsDir); result != 0) return result;

    header("Y. 下载页「模组加载器」那组钮的接线");
    if (const int result = MwInstalledVersionsChecks(versionsDir, window, gameStack, installNameEdit, installNameError, installBtn, installBackBtn, windowJava, pinGlobalJava, unpinGlobalJava); result != 0) return result;

    header("Y2. 下载页左栏那份分类：点哪个，右栏就翻到哪一页");
    if (const int result = MwDownloadNavigationChecks(window); result != 0) return result;

    header("Y6. 版本树那四条分类：接线与守卫");
    if (const int result = MwVersionActivationChecks(window, versionTree, gameStack, installNameEdit, installTargetLabel, installBackBtn); result != 0) return result;

    header("Y7. 点得快的时候，开关行每一下都要算（版本树 / 加载器树）");
    if (const int result = MwRapidRowActivationChecks(window, versionTree, gameStack, installNameEdit, installTargetLabel, installBackBtn); result != 0) return result;

    header("Z. 底栏名字下面那一行（三种取值）+ 「账号与外观」那个入口");
    if (const int result = MwAccountAppearanceChecks(); result != 0) return result;

    header("AA. 整合包导入：界面那几道守卫（全程离线）");
    if (const int result = MwModpackImportChecks(versionsDir, window, windowJava, pinGlobalJava, unpinGlobalJava); result != 0) return result;

    header("AB. 下载进度进的是任务账本：底栏那格 + 任务中心面板");
    if (const int result = MwTaskSummaryChecks(window); result != 0) return result;

    header("AB6. 压力：几十项任务同屏（大量下载 + 多实例启动 + 批量改文件）");
    if (const int result = MwTaskStressChecks(window); result != 0) return result;

    header("AB7. 任务中心收在别的选项卡上时，底栏那一按要把它收回当前页");
    // ------------------------------------------------------------------
    // 用户报的形状：在主页把任务中心点开 → 切到下载页（这一页按设计**收在**别的选项卡上，
    // 不关也不销毁）→ 再点底栏中间那格，**什么都没发生**。
    //
    // 根子在 `toggleBottomBarPanel()`：它把"同一位还开着"判成 `overlayHost->isOpen() &&
    // (panel->isVisible() || parked())`，可收回又只调 `dismiss()` —— 而 `dismiss()` 按设计
    // **不收收着的页**（OverlayHost::closeTopLayer 里 `!evenWhenParked && !onOwnersTab()`
    // 直接返回 false）。于是 `sameAnchor` 为真、`dismiss()` 空转、这一按被"同一颗 = 收回"
    // 那条早退整个吞掉。`openTaskCenterPanel()` 里 修的是同一个坑，只是入口不同。
    //
    // 修法：收着的页 + 点的就是它那一颗 ⇒ `evict()` 之后在当前这一页重新摊开。
    {
        QTabWidget *ab7Tabs = window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
        QPushButton *ab7Cell =
            window.findChild<QPushButton *>(MainWindowUI::taskSummaryButtonName());
        OverlayHost *ab7Overlay = window.ui() == nullptr ? nullptr : window.ui()->overlay();
        check(ab7Tabs != nullptr && ab7Cell != nullptr && ab7Overlay != nullptr,
              "AB7 找得到选项卡 / 底栏中间那格 / 覆盖层");
        if (ab7Tabs != nullptr && ab7Cell != nullptr && ab7Overlay != nullptr) {
            // 上一节（AB6）收页只 `close()` 了面板、覆盖层那一位还占着 —— 先按产品的
            // 出口收干净，不然下面第一按会先走"残影 + evict"那一路。
            if (ab7Overlay->pageOpen()) {
                ab7Overlay->evict();
            }
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

            // ---- ① 在主页摊开（这一页的 ownerTab 就是主页）----
            ab7Tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
            QApplication::processEvents();
            ab7Cell->click();
            ActivityPopover::openAllIn(&window);  // 摘要先弹浮动面板，从「全部任务」进整页
            QApplication::processEvents();
            QWidget *ab7Panel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            check(ab7Panel != nullptr && ab7Panel->isVisible(),
                  "AB7 先在主页把任务中心摊开（基线）");

            // ---- ② 切到下载页：按设计收起来，页还在、覆盖层还占着这一位 ----
            ab7Tabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
            QApplication::processEvents();
            check(ab7Overlay->parked(),
                  "AB7 切到下载页之后那一页**收着**（覆盖层没被释放）",
                  QStringLiteral("parked=%1 open=%2")
                      .arg(ab7Overlay->parked())
                      .arg(ab7Overlay->isOpen()));

            // ---- ③ 在当前这一页点底栏那格：必须重开，且不许把人踢回主页 ----
            const int ab7TabBefore = ab7Tabs->currentIndex();
            ab7Cell->click();
            // singleShot(0) 排的那一发要两轮事件循环才轮到。
            QApplication::processEvents();
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QApplication::processEvents();
            check(ab7Tabs->currentIndex() == ab7TabBefore,
                  "AB7 这一按不许把人从当前选项卡踢走",
                  QStringLiteral("%1 → %2").arg(ab7TabBefore).arg(ab7Tabs->currentIndex()));
            QWidget *ab7Panel2 = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            check(ab7Panel2 != nullptr && ab7Panel2->isVisible(),
                  "AB7 点在同一颗上 ⇒ 任务中心在**当前这一页**重新摊开（不再点了没反应）");

            // 收干净 + 回主页，别给后面几节留一页开着的覆盖层。
            if (ab7Overlay->pageOpen()) {
                ab7Overlay->evict();
            }
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            ab7Tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
            QApplication::processEvents();
        }
    }

    header("AC. 底栏左右两格钉死宽度：中间那根条不许跟着文字长短伸缩");
    // ------------------------------------------------------------------
    // 用户报的原话：「下载时启动左边实例运行中和条会有干涉，让条变长变短」。
    //
    // 底栏那三个格子的拉伸因子（0 / 4 / 0，中间夹两个 stretch 1）**只分富余宽度**，
    // 管不了「谁的最小宽度更大」。多开之后左边那两行字从固定长度「实例运行中」变成变长的
    // 「N 个实例运行中」+「A（账号甲）、B（账号乙）」，右边用户名也随账号变长短 ——
    // 于是中间那根条跟着一伸一缩。
    // 修法是把左边那格**钉死**（MainWindowUI::bottomBarRunStateColumnWidth()）、右边那格
    // 给**上限**（bottomBarAccountColumnWidth()，它是右对齐贴窗边的，钉死会在最右边留白），
    // 超出的文字由这几个格子自己截成省略号。
    // 这里量两件事：宽度确实被钉死/封顶了，以及**文字变长时进度条真的没有跟着动**。
    {
        QLabel *acRun = byObjectName<QLabel>(&window, MainWindowUI::runStateLabelName());
        QLabel *acRunDetail =
            byObjectName<QLabel>(&window, MainWindowUI::runStateDetailLabelName());
        QLabel *acAccount = byObjectName<QLabel>(&window, MainWindowUI::accountLabelName());
        QPushButton *acRunButton =
            byObjectName<QPushButton>(&window, MainWindowUI::runStateButtonName());
        QPushButton *acAccountButton =
            byObjectName<QPushButton>(&window, MainWindowUI::accountButtonName());
        QProgressBar *acBar = window.findChild<QProgressBar *>(MainWindowUI::progressBarName());
        check(acRun != nullptr && acRunDetail != nullptr && acAccount != nullptr &&
                  acRunButton != nullptr && acAccountButton != nullptr && acBar != nullptr,
              "AC0 找得到底栏那几格（两头那两颗按钮也在）");

        if (acRun != nullptr && acRunDetail != nullptr && acAccount != nullptr &&
            acRunButton != nullptr && acAccountButton != nullptr && acBar != nullptr) {
            // 结构这一层：两头**两颗按钮**的宽度是钉死的（最小值 = 最大值）——
            // 钉在按钮上、不钉在里面那几行字上：那两行有藏起来的时候，而布局里藏起来的
            // 控件宽度是 0，钉在字上等于没钉（AD 那节量的就是这个）。
            // 按钮里面那两行字各自封了顶（bottomBarRunStateTextWidth()），
            // 所以长名字最多把字截成省略号，撑不开按钮。
            // 运行状态那颗按钮在图标轨里要缩到轨宽（issue #85），所以它的宽度改成**封顶**：
            // 展开轨里就是 184，文字再长也撑不开；图标轨里跟着轨走。
            check(acRunButton->minimumWidth() == 0 &&
                      acRunButton->maximumWidth() ==
                          MainWindowUI::bottomBarRunStateColumnWidth(),
                  "AC1 左边那颗按钮的宽度封顶（展开轨 184，图标轨跟着轨缩）",
                  QStringLiteral("min=%1 max=%2")
                      .arg(acRunButton->minimumWidth())
                      .arg(acRunButton->maximumWidth()));
            check(acAccountButton->minimumWidth() ==
                          MainWindowUI::bottomBarAccountButtonWidth() &&
                      acAccountButton->maximumWidth() ==
                          MainWindowUI::bottomBarAccountButtonWidth(),
                  "AC1 右边那颗按钮的宽度也钉死（所以用户名多长都不影响中间那根条）",
                  QStringLiteral("min=%1 max=%2")
                      .arg(acAccountButton->minimumWidth())
                      .arg(acAccountButton->maximumWidth()));
            // 两行真正的判据是"**留给字的**宽度一样"，不是"两行封的是同一个数"——
            // 第一行那格是 136（方块在布局里另占一格，不吃这个上限），
            // 第二行那格的上限要 136 + 方块段（它的缩进是 contentsMargins，从上限里扣）。
            // 照原样封同一个数，第二行能写的只有 120 px，右边比第一行短 16 px。
            check(acRun->maximumWidth() == MainWindowUI::bottomBarRunStateTextWidth() &&
                      acRunDetail->maximumWidth() ==
                          MainWindowUI::bottomBarRunStateTextWidth() +
                              MainWindowUI::bottomBarRunStateMarkWidth() &&
                      acRunDetail->maximumWidth() - acRunDetail->contentsMargins().left() ==
                          acRun->maximumWidth(),
                  "AC1 两行**留给字的**宽度封的是同一个数（第二行那格多一个缩进段）",
                  QStringLiteral("max=%1/%2 · 留给字 %3/%4")
                      .arg(acRun->maximumWidth())
                      .arg(acRunDetail->maximumWidth())
                      .arg(acRun->maximumWidth())
                      .arg(acRunDetail->maximumWidth() -
                           acRunDetail->contentsMargins().left()));
            check(acAccount->maximumWidth() == MainWindowUI::bottomBarAccountColumnWidth(),
                  "AC1 右边用户名那格有宽度上限",
                  QStringLiteral("max=%1").arg(acAccount->maximumWidth()));

            // 行为这一层：把左边两行换成「多开」时才有的长文字，量中间那根条有没有动。
            //
            // NOTE: 没有实例在跑时 updateBottomBar() 会把这两行藏起来（文字是「实例未运行」
            // 那一支不藏，但本节跑的时候这个进程里没起过游戏，状态是"藏着的"），
            // 而**藏起来的东西不参与布局** —— 那样量出来的「条没动」是假的
            // （文字再长也影响不到）。所以这里手动把它们显示出来。量完不用收回去：
            // 本节是全套最后一段。
            acRun->setVisible(true);
            acRunDetail->setVisible(true);
            QApplication::processEvents();
            const int acBarWidthBefore = acBar->width();
            check(acBarWidthBefore > 0, "AC1 前提：窗口真的布局过了（进度条有宽度）",
                  QString::number(acBarWidthBefore));

            acRun->setText(MainWindowUI::runStateRunningCountText(3));
            acRunDetail->setText(
                QStringLiteral("很长很长的实例名字甲乙丙丁戊己庚辛壬癸、"
                               "另一个也很长的名字子丑寅卯辰巳午未申酉戌亥"));
            QApplication::processEvents();
            check(acBar->width() == acBarWidthBefore,
                  "AC2 左边换成「3 个实例运行中」+ 两个长名字之后，进度条宽度一点没变",
                  QStringLiteral("%1 → %2").arg(acBarWidthBefore).arg(acBar->width()));
            check(acRunButton->width() <= MainWindowUI::bottomBarRunStateColumnWidth(),
                  "AC2 左边那颗按钮自己也没被撑宽（不超过封顶的 184）",
                  QStringLiteral("%1（上限 %2）")
                      .arg(acRunButton->width())
                      .arg(MainWindowUI::bottomBarRunStateColumnWidth()));

            // AC3 文字换了、**宽度没换**（这是把宽度钉死的必然结果）—— 画出来的东西
            // 必须跟着换。这一条盯的是 ElidedLabel 里那个"按宽度缓存省略结果"的坑：
            // 宽度是钉死的、文字改了宽度也不改，resizeEvent 不触发，缓存就永不失效 ——
            // 第一句话会一直挂在底栏上（三个实例跑着，写的还是「实例运行中」）。
            // 所以量的是**像素**，不是宽高：宽高在这儿本来就不会动。
            acRun->setText(MainWindowUI::runStateRunningCountText(1));
            QApplication::processEvents();
            const QImage acShotBefore = acRun->grab().toImage();
            acRun->setText(MainWindowUI::runStateRunningCountText(3));
            QApplication::processEvents();
            const QImage acShotAfter = acRun->grab().toImage();
            check(!acShotBefore.isNull() && acShotBefore.size() == acShotAfter.size(),
                  "AC3 前提：两次都抓到了图，而且大小一样（宽度确实没变）");
            check(acShotBefore != acShotAfter,
                  "AC3 文字换了、宽度没换，画出来的也得换（不是挂着上一句）");
        }
    }
    header("AD. 左边那格「有字 / 没字」，中间那根条一步都不许动");
    // 这一节量三件事：① 手工把两行藏起来再显示出来，条不动；② 走**真路径**（切一下
    // 选项卡触发一次 updateBottomBar() 的「没有实例在跑」那一支），条还是不动；③ 那种
    // 时候左下角确实什么都不显示 —— 「没实例在跑就不写废话」这条产品行为不能被这次改动
    // 弄丢。
    //
    // NOTE: ② 的扳机 从主页那个「离线模式」勾选框换成了切选项卡（那个勾选框
    // 去掉了）。挑它的理由和 AB 那节一样：公开、而且**一行状态都不改** —— 这里量的
    // 就是"重画之后那两行该是什么样"，扳机自己不许先写一遍。
    {
        QProgressBar *adBar = window.findChild<QProgressBar *>(MainWindowUI::progressBarName());
        QLabel *adStatus = window.findChild<QLabel *>(MainWindowUI::statusLabelName());
        QLabel *adRun = byObjectName<QLabel>(&window, MainWindowUI::runStateLabelName());
        QLabel *adRunDetail =
            byObjectName<QLabel>(&window, MainWindowUI::runStateDetailLabelName());
        QTabWidget *adTabs = window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
        check(adBar != nullptr && adStatus != nullptr && adRun != nullptr &&
                  adRunDetail != nullptr && adTabs != nullptr,
              "AD0 找得到底栏那几格和那个选项卡容器");

        if (adBar != nullptr && adStatus != nullptr && adRun != nullptr &&
            adRunDetail != nullptr && adTabs != nullptr) {
            // 中间那格得真的在：藏着的控件宽度是 0，那样量出来的「没变」是 0 == 0 的假绿。
            adBar->setVisible(true);
            adStatus->setVisible(true);
            adRun->setVisible(true);
            adRunDetail->setVisible(true);
            adRun->setText(MainWindowUI::runStateRunningCountText(1));
            adRunDetail->setText(QStringLiteral("1.20.1"));
            QApplication::processEvents();
            const int adShown = adBar->width();
            check(adShown > 0, "AD1 前提：进度条真的在、有宽度", QString::number(adShown));

            // ---- ① 从「有」到「无」再回来：条一步都不许动 ----
            adRun->setVisible(false);
            adRunDetail->setVisible(false);
            QApplication::processEvents();
            check(adBar->width() == adShown,
                  "AD1 把左边那两行藏起来：进度条宽度一点没变（这一格的宽度不是那两行给的）",
                  QStringLiteral("%1 → %2").arg(adShown).arg(adBar->width()));
            adRun->setVisible(true);
            adRunDetail->setVisible(true);
            QApplication::processEvents();

            // ---- ② 走真路径：没有实例在跑那一支 ----
            repaintBottomBar(&window);
            check(adBar->width() > 0 && adBar->width() == adShown,
                  "AD2 真跑一次「没有实例在跑」（切选项卡当扳机）：条还是不动",
                  QStringLiteral("%1 → %2").arg(adShown).arg(adBar->width()));

            // ---- ③ 那时候左边写的是「实例未运行」那个**终态**，不是上一句的残影 ----
            // 这一支正是这条：摆出来的现场（「1.20.1」+ 两行字）被真跑一遍的
            // updateBottomBar() 冲掉，换成"没有实例在跑"该有的那句话。
            check(adRun->text() == MainWindowUI::runStateNotRunningText(),
                  "AD3 真跑一次之后左边写的是「实例未运行」（不是挂着上一句）",
                  QStringLiteral("可见=%1 文字=%2")
                      .arg(adRun->isVisible() ? QStringLiteral("是") : QStringLiteral("否"))
                      .arg(adRun->text()));
        }
    }
    header("AE. 设置页「启动选项」三项：接线 + 改一下立刻落盘 + 日志窗那条路");
    if (const int result = MwGameLogLifecycleChecks(settingsWired, window, showLogCheck, priorityCombo, windowWidthSpin, windowHeightSpin, windowModeCombo, windowSizeTimes); result != 0) return result;

    header("O. 恢复现场");
    {
        QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                  Q_ARG(QStringList, QStringList({QStringLiteral("1.20.1")})));
        check(waitInstalledScan(), "后台扫盘回来了");
        QApplication::processEvents();
        check(installed->count() == 3, "沙盒回到 3 个版本",
              QString::number(installed->count()));

        // 收尾：整个沙盒（含 QSettings 的 ini）删掉，别给下次运行留脏数据
        QDir(versionsDir).removeRecursively();
        QDir(settingsDir).removeRecursively();
        check(!QDir(versionsDir.absolutePath()).exists() &&
                  !QDir(settingsDir).exists(),
              "沙盒已清理");
    }

    header("ZG. 离线账号那张脸：算得出来就用官方默认皮肤，算不出来退回内置 Steve");
    runOfflineDefaultSkinTests();

    header("ZB8. 图标「核对那一发」的三种出路（本地假服务器，不碰外网）");
    runIconRevalidateTests();

    header("ZH. 下载页第二个取数源：按来源发请求（本地假服务器，不碰外网）");
    runResourceSourceTests();

    header("AH. 底栏那两块上拉面板：正在跑的实例（一行一个会话、带时长）/ 最近登录的三个账户");
    runBottomBarPanelTests();

    header("AI. 左上角那行字固定是商标（UI 问题 #3）");
    // ------------------------------------------------------------------
    // 要求：左上角固定写我们的商标，不再允许自定义（自定义会带出字体 / 字号问题）。
    // 这一节量三件事：那五个字母就是 WTFML；设置页里**没有**那一格输入框了；
    // 盘上留着老用户那份 `Launcher/TitleText` 也不影响开窗（这一项不再被读）。
    {
        TitleBar *strip = window.ui() != nullptr ? window.ui()->titleBar() : nullptr;
        check(strip != nullptr, "AI 前提：拿得到顶上那条");
        check(TitleBar::defaultBrandText() == QStringLiteral("WTFML"),
              "AI1 商标就是 WTFML 这五个字母", TitleBar::defaultBrandText());
        check(window.findChild<QLineEdit *>(QStringLiteral("titleTextEdit")) == nullptr,
              "AI2 设置页里没有「主页标题」那一格输入框了");
        // 老用户盘上那份自定义值：写一个进去，再开一个窗口，那一格照样不在、窗口照开。
        {
            QSettings raw;
            raw.setValue(QStringLiteral("Launcher/TitleText"), QStringLiteral("我的启动器"));
            raw.sync();
        }
        MainWindow withStaleTitle;
        QApplication::processEvents();
        TitleBar *staleStrip =
            withStaleTitle.ui() != nullptr ? withStaleTitle.ui()->titleBar() : nullptr;
        check(staleStrip != nullptr &&
                  withStaleTitle.findChild<QLineEdit *>(QStringLiteral("titleTextEdit")) == nullptr,
              "★ AI3 盘上留着旧的自定义标题时，设置页里照样没有那一格、条照样在");
        check(!staleStrip->grab().isNull(), "AI3 条照常画得出来（不因为盘上那份旧值而空着）");
        // 收尾：把那份旧值抹掉，别留给后面的节。
        QSettings().remove(QStringLiteral("Launcher/TitleText"));
        QSettings().sync();
    }

    header("AJ. 启动前那两条提醒：十秒内重复启动 / 同一实例已经在跑（都要拦得住、放得行）");
    runLaunchGuardTests();

    // NOTE: 排在**最后**：这一节要造一个 MainWindow，而它会顺手改当前身份（见里面对
    // AccountStore::addOffline 那一句）—— 排在别的节前面就等于替它们换了身份。
    header("ZI. 下载页那一行筛子的接线：控件 → 请求地址（本地假 CF 接口，不碰外网）");
    runDownloadFilterTests();

    // 同样排在**最后**：这一节也要造 MainWindow，也会换当前身份（离线账号），而且它会
    // 把「下载源」钉成官方（这一节的远端全是 file://）—— 排在别的节前面会污染它们。
    header("T19. 实例设置「启动」栏那两颗按钮：补全文件 / 导出启动脚本（离线，file:// 当远端）");
    runInstanceStartActionsTests();

    // 同样排在**最后**：这一节也要造 MainWindow、也要换当前身份（离线账号）。它要的是
    // 「体检正在跑」这个状态（闸口那几条全靠它），所以**不能**混进别的节里跑。
    header("T33. 完整性检查：点下去 → 后台一趟 → 闸口 → 报告页（离线）");
    runIntegrityScanTests();

    // 同样排在**最后**：这一节也要造 MainWindow、也要换当前身份（离线账号）。
    // 同样排在**最后**：这一节也要造 MainWindow、也要换当前身份（离线账号）。它**不换**
    // 数据根目录（账本那一份故意挂在启动器自带的固定位置，见 T34 那段说明）—— 所以它
    // 前后要是有别的节在读那本账，看到的会多出这一节补的那几项。
    header("T34. 游戏异常退出：从 onGameFinished 那一路补出来的一项（离线）");
    runGameCrashTests();

    header("T35. 世界与实例备份：创建快照 / 立即备份 / 还原（离线）");
    runBackupTaskTests();

    // 同样排在**最后**：这一节也要造 MainWindow、也要换当前身份（离线账号），还要
    // **换数据根目录**（那本账落在 `<当前游戏文件夹>/wtfml.history` 底下 —— 不换的话
    // 这一节就会往用户真正的那本账上写）。它**不碰网络**：三个夹具的 Java 都钉在
    // 一个不存在的路径上，点到启动那一步就停在提示上（见那一节的文件头）。
    header("T36. 「上一次启动」那张卡：账怎么写 / 卡片上写什么 / 点下去走哪条路（离线）");

    header("T20. 实例设置「文件管理」那八页：目录从哪儿来 / 启用·禁用 / 删除（离线）");
    runInstanceFilesPagesTests();

    header("T21. 导出整合包：从「导出…」那颗按钮到盘上那个包（离线，假接口）");
    runModpackExportTests();

    header("T22. 「文件管理」那几页的图标：按文件哈希反查「这是哪个资源」（离线，假接口）");
    runInstanceIconLookupTests();

    // 同样挨着 T21/T22：它也要把数据根目录换到临时目录（`cache/servers` 挂在它下面）。
    header("T23. 服务器那点东西落盘：测速结果来回一趟不走样（离线）");
    runServerStatusCacheTests();

    header("ZJ. 资源详情页那条路：下载页点某一行 → 整页详情 → chip → 点某一版（离线，假 CF 接口）");
    runResourceDetailPageTests();

    header("T24. 实例设置里点一条行尾的「详情」 ⇒ 开那个资源自己的那一页（离线，假接口）");
    runInstanceFileResourceTests();

    header("T25. 从实例设置里点「下载新模组」⇒ 落到下载页那一栏 + 左上角那颗返回键（离线）");
    runDownloadFromInstanceTests();

    // 同样排在**最后**：这一节也要造 MainWindow、也要换数据根目录，还会改
    // `Java/Path` / `Java/AutoSelect` 那两个键（节尾逐字还原）。**一颗「下载」都不按。**
    header("T26. 设置页那颗「Java 管理…」开出来的那一页：四行 + 两颗动作键 + 那颗「选用」（离线）");
    runJavaManageTests();

    // 同样排在**最后**：也要造 MainWindow、也要换数据根目录（节尾逐字还原）。这一节
    // **一个原生选择框都不点** —— 「复制实例」和「导入世界」那两颗的第一步都是
    // QFileDialog，测试里点下去就是卡死，所以量的都是它们**动手的那一半**。
    header("T27. 概览页的「复制实例」+ 存档页那支笔 / 顶上两颗（离线）");
    runCopyAndSavePageTests();

    header("T28. 文件管理那几页顶上那条搜索 + 状态栏里的「可更新」（离线，假接口）");
    runInstanceUpdateFilterTests();

    // 排在**最后**：同样要造 MainWindow、要换数据根目录（节尾逐字还原）。这一节是 T28 的
    // 另一半 —— 那一趟里"同一个文件被两家各认了一遍"这件事（见那一节的开头那段）。
    header("T32. 更新检测：一个文件只查一家 + 退回问 CurseForge（离线，假接口）");
    runUpdateCheckSourceFallbackTests();

    // T32 的补充：同一个项目底下两个文件、批量只答得出其中一个（见那一节开头那段）。
    header("T32b. 更新检测：同一项目两条文件、批量只答一条 ⇒ 另一条照样退回问（离线，假接口）");
    runUpdateCheckSameProjectFallbackTests();

    // 同样排在**最后**：这一节造真 MainWindow、换数据根目录（节尾逐字还原），还从那一页
    // 上真按那几颗 chip（忽略 / 固定 / 更新 / 撤销），并且**真读盘**核那本账和那份回滚点。
    header("T38. 更新中心上那几颗动作：忽略 / 固定 / 逐条更新 / 撤销回滚点（离线，假接口）");
    runUpdateCenterActionTests();

    // 同样排在**最后**：这一节也要造 MainWindow、也要换数据根目录（节尾逐字还原），
    // 还会往**主题规格**和 `Theme/…` 那几个设置键里写东西（节尾一样还原）。它一个原生框
    // 都不点：「选择图片…」底下是 QFileDialog，点下去就是卡死 —— 所以"挑完图之后"那一态
    // 是照产品那两步在沙盒里搭出来的（见那一节里那段）。
    header("T29. 设置页「背景图」那两行：控件 → 主题规格 → 盘（离线）");
    runThemeImageRowTests();
    runFileDragIntentTests();

    // 紧跟 T29 的三处理由完全一样：要造 MainWindow、要换数据根目录（节尾逐字还原）、要往
    // 主题规格和 `Theme/…` 那几个键里写东西（节尾一样还原）。它也是
    // **一个原生框都不点**：三块色板底下是覆盖层、导出/导入底下是 QFileDialog。
    header("T39. 设置页「主题」那一节：配色 / 主题色 / 语义色 / 文字不透明度（离线）");
    runThemeControlsTests();

    // 同样排在**最后**：也要造 MainWindow、也要换数据根目录（节尾逐字还原）。这一节
    // **一个原生框都不点**（整合包那一路第一步是 QFileDialog，点下去就是卡死），量的全是
    // "拖进去之后"那一半：认类型 → 问不问 → 落到哪个目录 → 问"装到哪个实例"。
    header("T30. 拖进来的文件：认类型 / 问不问 / 落点 / 装到哪个实例（离线）");
    runDroppedFileTests();

    // 同样排在**最后**：也要造 MainWindow、也要换数据根目录（节尾逐字还原）。这一节两趟
    // 都走**同一条用户路**（实例设置 → 模组 → 详情 → 点某一版），第一趟量"缺前置那个弹窗
    // 说了什么 + 一起下载真的把前置排进这一批"，第二趟量"设置里关掉之后什么都不装"；
    // 末尾那一段按路径选中 → 点禁用/启用/删除，量的是**盘上的字节**。
    header("T31. 前置依赖弹窗 + 从实例点进详情页再下载 + 批量动作真改盘（离线，假接口）");
    runDependencyAndBatchTests();

    // **哪儿都能排**：这一节不造 MainWindow、不碰数据根目录、不看网络 —— 它只跟一条
    // 真进程打交道（那条"服务端"是本工程自己的 exe，见 `--fake-server` 那一支）。
    header("T41. 服务端「跑起来 / 停下来」：起 → 读到 `Done (` → 写一句 → 停 / 宽限到点强杀（离线）");
    runServerProcessTests();

    // **也要造 MainWindow、也要换数据根目录**（同一节自己的沙盒，节尾逐字还原）。这一节走
    // 一整条用户路：服务端列表右键 → 「服务端设置」→ 表单改一格 → 保存 → 盘上那份文件，
    // 切「原始文本」→ 改一行 → 保存 → 整份写回去，再按「立即备份」看盘上真多一个 zip。
    header("T42. 「服务端设置」那一整页：表单 / 原始文本 / 备份 / 体检（离线）");
    runServerSettingsPageTests();

    // **也要造 MainWindow、也要换数据根目录**（同一节自己的沙盒，节尾逐字还原）。这一节
    // 量的是**关窗那一刻**：后台那几趟活、那几个子进程、还在飞的网络请求各自怎么收场。
    header("T43. 关窗收尾：worker 线程 / 子进程 / 还在飞的下载（离线）");
    runShutdownTests();

    // 这一节不造 MainWindow、也不碰数据根目录：只立一张 QTreeWidget，量的是
    // "藏起来选完再显示时滚动条会怎么动"和 restoreTreeScroll() 那一下（见 Support.hpp）。
    header("T44. 管理页右侧那张实例树：打开时不许停在最底下");
    runInstanceTreeScrollTests();

    // 夹具这一跑有没有"没演到的路由"：非零说明产品打了夹具没覆盖的地址，那一发拿到 404
    // （和真服务器上"没有这个路由"同形）。它不是"这条断言意外地没红"的替代品，但能更早
    // 说明这一跑其实没测到东西 —— 或者又开始悄悄依赖真实网络了。排查看 `unrouted`。
    check(fakeNet.notFound == 0, "假外网没有收到没路由的请求（每一发都打在被演到的路径上）",
          QStringLiteral("404 了 %1 发：%2")
              .arg(fakeNet.notFound)
              .arg(fakeNet.unrouted.mid(0, 8).join(QStringLiteral(", "))));

    std::printf("\n===== %s: %d/%d =====\n", g_fail == 0 ? "全部通过" : "有失败", g_pass,
                g_pass + g_fail);
    // 显式冲掉：这一行原来靠退出时的 stdio 收尾落盘，而**异常终止不冲缓冲**。2026-10-22
    // 那次 SEGFAULT 的日志正好止在 T43.9 —— 单看日志分不清"汇总没来得及打"还是"汇总打了、
    // 崩在退出路径上"。冲了这一下，以后两档一眼可辨（同 `check()` 里那一句的理由）。
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}

// UrlSpy 的元对象（见文件上面那个类）。AUTOMOC 就是靠这一句认出"main.cpp 里有 Q_OBJECT
// 要处理"，生成的那份叫 main.moc —— 放在**整个文件最后**：它引用的类得先声明过。
// UrlSpy meta-object is generated from modules/Support.hpp.









