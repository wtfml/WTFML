#include "Support.hpp"
int MwModpackImportChecks(const QDir &versionsDir, MainWindow &window, GlobalJavaSettings * &windowJava, const std::function<void(const QString &)> &pinGlobalJava, const std::function<void()> &unpinGlobalJava) {
    // ------------------------------------------------------------------
    // 这一节驱动的是 MainWindow::applyModpackImport() —— 导入的**执行部分**，
    // 也就是"把清单解析出来、该拦的拦掉、算出安装器要的 Java，然后开始导入"。
    // 它被拆出来单独能调，正是为了这里：上面那一半（QFileDialog → QInputDialog →
    // 确认框）全是模态的，offscreen 上驱动不了，而真会错的判断都在这一半。
    //
    // NOTE: 这一节的每一条都要求"一个字节都没下"（日志里没有「开始导入整合包」、
    // 沙盒里没多出目录）。挡不住的典型样子是"弹了窗、用户点掉了，然后它照样
    // 开始下" —— 那种错从弹窗上完全看不出来。
    {
        QPushButton *importBtn =
            byObjectName<QPushButton>(&window, MainWindowUI::importModpackButtonName());
        check(importBtn != nullptr && importBtn->isEnabled(),
              "AA0 窗口上找得到「导入整合包…」，而且是亮的");

        // 驱动一次。返回值 = "真的开始导入了"；被拦下来时它是 false，而弹窗已经弹过了。
        // zipPath 给一个不存在的路径：被拦下的那几条路**一个字节都不读它**（读它的是
        // ZipUtil，在 onImportModpackClicked() 那一半里）。
        const QString kZip = QStringLiteral("C:/没有这个目录/整合包.zip");
        // `format` 默认 Modrinth（这一节里绝大多数用例都是那一种）；`invokedOut` 是**接线
        // 自检**用的：第四个参数是自定义枚举，Q_ARG 那个类型名和注册名对不上的话
        // invokeMethod **静默返回 false** —— 而那看起来和"这条路被拦住了"一模一样，
        // 下面每一条断言都会假绿。所以调用方拿得到那个标志，并且专门有一条断言钉它。
        const auto tryImport = [&window, &kZip](const QByteArray &json, const QString &name,
                                                ModpackImport::Format format =
                                                    ModpackImport::Format::Modrinth,
                                                bool *invokedOut = nullptr) {
            bool ok = false;
            const bool invoked = QMetaObject::invokeMethod(
                &window, "applyModpackImport", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
                Q_ARG(QString, kZip), Q_ARG(QString, name), Q_ARG(QByteArray, json),
                Q_ARG(ModpackImport::Format, format));
            if (invokedOut != nullptr) {
                *invokedOut = invoked;
            }
            return invoked && ok;
        };
        // ---- AA 接线自检：这两条必须先绿，不然下面全是假绿 ----
        //
        // NOTE: 这两发也会弹框（`这不是 json` 进哪个解析器都是"读不懂"），所以每一发之前
        // 都要 `dismissNextPrompt()` 排一个 0 毫秒的定时器 —— 不排的话没人去点它，
        // 而那个提示开在嵌套事件循环里，整节会挂在那儿（不是红，是**不动了**）。
        {
            bool modrinthWired = false;
            dismissNextPrompt();
            tryImport(QByteArrayLiteral("这不是 json"), QStringLiteral("新实例"),
                      ModpackImport::Format::Modrinth, &modrinthWired);
            check(modrinthWired,
                  "AA0a invokeMethod 认得这四个参数（不认得就是静默 false，下面全假绿）");
            // 两种格式都要过一遍：Q_DECLARE_METATYPE 的位置写错（放进命名空间里）时，
            // 注册出来的名字是裸的 `Format`，而 Q_ARG 那侧写的是 `ModpackImport::Format`。
            bool curseForgeWired = false;
            dismissNextPrompt();
            tryImport(QByteArrayLiteral("这不是 json"), QStringLiteral("新实例"),
                      ModpackImport::Format::CurseForge, &curseForgeWired);
            check(curseForgeWired, "AA0a CurseForge 那个格式名也认得（同一个注册）");
        }

        // 沙盒里本来有几个版本目录。这一节跑完必须还是这么多 —— 被拦下的导入不该在盘上
        // 留半个实例目录。
        const int dirsBefore =
            QDir(versionsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        const int importsBefore =
            countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包"));

        // ---- AA1 一串读不懂的字节 ----
        // 用户随手选了个不是整合包的压缩包，而里面的 modrinth.index.json 是坏的。
        dismissNextPrompt();
        check(!tryImport(QByteArrayLiteral("这不是 json，一个字节都不是"), QStringLiteral("新实例")),
              "AA1 清单读不懂 → 拒绝");
        check(g_modalTitle == QStringLiteral("导入整合包"),
              "AA1 弹的是「导入整合包」", g_modalTitle + QStringLiteral(" / ") + g_modalText);
        check(g_modalWasWarning, "AA1 是警告框（这是一条错误）");
        check(g_modalText.contains(QStringLiteral("无法解析")),
              "AA1 弹窗说清了是这份清单无法解析", g_modalText);

        // ---- AA2 是合法 json，但不是整合包清单 ----
        // 真正会遇到的形态：这个文件叫 modrinth.index.json，但里面是别的东西
        // （比如别人打包时把自己的配置塞了进来）。
        dismissNextPrompt();
        check(!tryImport(QByteArrayLiteral(R"({"hello":"world"})"), QStringLiteral("新实例")),
              "AA2 合法 json 但不是清单 → 拒绝");
        check(g_modalText.contains(QStringLiteral("无法作为整合包清单读取")),
              "AA2 弹窗指出它缺了什么", g_modalText);

        // ---- AA3 是清单，但 files 一条都没有 ----
        dismissNextPrompt();
        check(!tryImport(QByteArrayLiteral(R"({"formatVersion":1,"game":"minecraft",)"
                                           R"("dependencies":{"minecraft":"1.20.1"},"files":[]})"),
                         QStringLiteral("新实例")),
              "AA3 空清单 → 拒绝");
        check(g_modalText.contains(QStringLiteral("整合包清单未列出待下载文件")),
              "AA3 弹窗说的是「一条文件都没有」", g_modalText);

        // 下面三条要走到"加载器 / 名字"那几步，得有一份**读得出来**的清单。
        // 文件那条只放一条（下载地址是个不存在的域名 —— 这几条路都不会去下它）。
        const QByteArray kFabricJson = QByteArrayLiteral(R"({
            "formatVersion": 1,
            "game": "minecraft",
            "versionId": "1.0.0",
            "name": "假整合包",
            "dependencies": {"minecraft": "1.20.1", "fabric-loader": "0.16.14"},
            "files": [{"path": "mods/example.jar",
                       "hashes": {"sha1": "0000000000000000000000000000000000000000"},
                       "downloads": ["https://example.invalid/example.jar"],
                       "fileSize": 1234}]
        })");

        // ---- AA4 Quilt：2.1.4 起客户端可安装/可升级，不再在加载器那一关被拒 ----
        // 这里只钉数据层的判据，不真去下 Quilt：安装链路由 core 的 V9 和实例更新专项覆盖。
        check(ModpackImport::unsupportedLoaderText(ModpackImport::quiltLoaderKey(),
                                                   QStringLiteral("0.20.0")).isEmpty(),
              "AA4 要 Quilt 的包不再被加载器闸门拒绝");

        // ---- AA5 名字建不出目录来（Windows 保留设备名）----
        // 走到这一步说明清单、加载器都过了 —— 这是"只剩名字不对"那条路。
        dismissNextPrompt();
        check(!tryImport(kFabricJson, QStringLiteral("CON")), "AA5 名字叫 CON → 拒绝");
        check(g_modalTitle == QStringLiteral("换个名字") && g_modalWasWarning,
              "AA5 弹的是「换个名字」的警告框",
              g_modalTitle + QStringLiteral(" / ") + g_modalText);
        check(g_modalText.contains(QStringLiteral("保留设备名")),
              "AA5 弹窗说清了为什么这个名字不行", g_modalText);

        // ---- AA6 重名：沙盒里已经有一个叫 1.18.2 的实例 ----
        // 判据和「重命名」那条路**共用一份**（InstanceFiles::validateRenameTarget）——
        // 这里验的是它真的被调了，而不是"数据层自己又查了一遍"（数据层有意不查，
        // 见 VersionManager::importModpack 的说明）。
        dismissNextPrompt();
        check(!tryImport(kFabricJson, QStringLiteral("1.18.2")), "AA6 撞上已存在的实例名 → 拒绝");
        check(g_modalTitle == QStringLiteral("换个名字") && g_modalWasWarning,
              "AA6 也是「换个名字」的警告框",
              g_modalTitle + QStringLiteral(" / ") + g_modalText);
        check(g_modalText.contains(QStringLiteral("已经有一个叫")) &&
                  g_modalText.contains(QStringLiteral("1.18.2")),
              "AA6 弹窗指出了撞的是哪一个", g_modalText);
        // 大小写不敏感：磁盘上是 26.3，用户输入 26.3 上面那条已经覆盖；这里补一条
        // "换个大小写也躲不过去"的 —— 同一份判据的规矩。
        dismissNextPrompt();
        check(!tryImport(kFabricJson, QStringLiteral("26.3")), "AA6b 重名判据是同一份（26.3 也拒）");
        check(g_modalText.contains(QStringLiteral("已经有一个叫")),
              "AA6b 同样说的是「已经有一个叫」", g_modalText);

        // ---- AA7 要 Forge/NeoForge 的包，而安装器要的 Java 本机给不出来 ----
        // 这一条补的是**另一件**事：Forge/NeoForge 的安装器自己就是个 Java 程序，
        // 所以"这个版本要的 Java"必须在**开始之前**算出来。算不出来的话，用户会等半天
        // 等来一句"下载失败"，永远猜不到该去装个 Java。
        //
        // 做法和 Y3 那条同一套：摆一个 javaVersion=99 的版本 json（99 是没有机器能满足的
        // 要求，于是这条在**任何**机器上离线都能跑绿），再把设置里的 Java 指到一个不存在
        // 的路径上（空路径 = 走 PATH 那份，那条路会原样往下装）。
        const QString kJava99Id = QStringLiteral("1.21.4");
        const QString kJava99Json =
            versionsDir.filePath(kJava99Id + QLatin1Char('/') + kJava99Id +
                                 QStringLiteral(".json"));
        writeFileAt(kJava99Json,
                    QByteArrayLiteral("{\"id\":\"1.21.4\",\"javaVersion\":{\"majorVersion\":99}}"));
        const QByteArray kNeoForgeJson = QByteArrayLiteral(R"({
            "formatVersion": 1,
            "game": "minecraft",
            "versionId": "1.0.0",
            "name": "假整合包",
            "dependencies": {"minecraft": "1.21.4", "neoforge": "21.1.72"},
            "files": [{"path": "mods/example.jar",
                       "hashes": {"sha1": "0000000000000000000000000000000000000000"},
                       "downloads": ["https://example.invalid/example.jar"],
                       "fileSize": 1234}]
        })");
        const QString kBogusJava = QStringLiteral("C:/没有这个目录/java.exe");
        check(windowJava != nullptr && QFile::exists(kJava99Json),
              "AA7 前提：拿得到窗口里那份 Java 设置，假版本目录也摆好了", kJava99Json);
        if (windowJava != nullptr && QFile::exists(kJava99Json)) {
            pinGlobalJava(kBogusJava);
            QApplication::processEvents();
            GlobalJavaSettings probe;
            check(probe.javaPath() == kBogusJava && !probe.autoSelectJava(),
                  "AA7 前提：设置里的 Java 已经指到那份不存在的路径上了（手动档）",
                  probe.javaPath());
            // 前提不成立就**不调** applyModpackImport：空路径那条路会原样往下走去联网。
            if (probe.javaPath() == kBogusJava && !probe.autoSelectJava()) {
                dismissNextPrompt();
                check(!tryImport(kNeoForgeJson, QStringLiteral("导入的新实例")),
                      "AA7 要 Java 99 的 NeoForge 包 → 在开始之前就拒绝");
                check(g_modalTitle == QStringLiteral("Java 版本不够") && g_modalWasWarning,
                      "AA7 弹的是「Java 版本不够」",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
                check(g_modalText.contains(QStringLiteral("Java 99")) &&
                          g_modalText.contains(QStringLiteral("设置")),
                      "AA7 说清了要哪个 Java、以及该去哪儿指路径", g_modalText);
            }
            // 收拾：Java 翻回「自动」，假版本目录删掉（Y3 也是这么收的）。
            unpinGlobalJava();
            QApplication::processEvents();
            check(windowJava->autoSelectJava(), "AA7 收尾：Java 回到「自动」",
                  windowJava->javaPath());
        }
        QDir(versionsDir.filePath(kJava99Id)).removeRecursively();
        check(!QDir(versionsDir.filePath(kJava99Id)).exists(), "AA7 那个假版本目录删干净了");

        // ---- AA8 「上次没下完的导入」能接着下 ----
        // 真机上这一条是必需的：整合包是几百个文件分着下的，网一断就留下半份，而重名
        // 判据只认目录名 —— 不能接着下的话，用户唯一的办法是"换个名字重导"，也就是把
        // 已经下好的那几百 MB 再下一遍（和「重下=校验 跳过」正好相反）。
        //
        // 判据本体（InstanceFiles::checkImportTargetName）是纯函数，真值表直接钉。
        // 最后两条用 applyModpackImport() 走一遍 —— 验的是"执行那一半也认同一个判据"。
        {
            // 半成品的样子 = 导入写下的那一份薄 json（只有 id / inheritsFrom / type），
            // 没有 wtfml.meta（启动器没做完），顶层也没有客户端 jar。
            const auto makeHalf = [&versionsDir](const QString &id, const QByteArray &json) {
                versionsDir.mkpath(id);
                writeFileAt(versionsDir.filePath(id + QLatin1Char('/') + id +
                                                 QStringLiteral(".json")),
                            json);
            };
            const auto halfJson = [](const QString &id) {
                return QByteArrayLiteral("{\"id\":\"") + id.toUtf8() +
                       QByteArrayLiteral("\",\"inheritsFrom\":\"1.20.1-forge-47.3.33\","
                                         "\"type\":\"release\"}");
            };
            const QString kHalf = QStringLiteral("半成品");
            makeHalf(kHalf, halfJson(kHalf));
            check(InstanceFiles::isUnfinishedImport(kHalf),
                  "AA8 半成品认出来了（没有 meta、没有 jar、json 声明了父版本）");
            QString problem;
            check(InstanceFiles::checkImportTargetName(kHalf, &problem) ==
                      InstanceFiles::ImportNameVerdict::OkResume,
                  "AA8 这个名字可以接着用", problem);
            check(problem.isEmpty(), "AA8 接着下那条路不给用户留一句问题描述");
            check(InstanceFiles::checkImportTargetName(QStringLiteral("全新的实例"), &problem) ==
                      InstanceFiles::ImportNameVerdict::Ok,
                  "AA8 盘上没有的名字照旧是普通导入", problem);

            // ① 有 wtfml.meta = 启动器把这一份做**完**过（导入最后一步写的、装加载器装完
            // 写的、改名也补）→ 那是别人的实例，不许被接着下。
            const QString kDone = QStringLiteral("半成品-做完了");
            makeHalf(kDone, halfJson(kDone));
            writeFileAt(versionsDir.filePath(kDone + QStringLiteral("/wtfml.meta")),
                        QByteArrayLiteral("{\"instanceId\":\"x\"}"));
            check(!InstanceFiles::isUnfinishedImport(kDone),
                  "AA8 有 wtfml.meta 的不算半成品（启动器做完过）");
            check(InstanceFiles::checkImportTargetName(kDone, &problem) ==
                      InstanceFiles::ImportNameVerdict::Taken,
                  "AA8 做完了的那一份仍然拒（不许覆盖用户的实例）", problem);
            check(problem.contains(QStringLiteral("已经有一个叫")),
                  "AA8 拒的理由还是那句话", problem);

            // ② 顶层有客户端 jar = 这不是整合包导入留下的目录（导入的实例目录里只有薄 json
            // + 下下来的那些文件，主 jar 在原版/加载器那一层）。
            const QString kJar = QStringLiteral("半成品-有jar");
            makeHalf(kJar, halfJson(kJar));
            writeFileAt(versionsDir.filePath(kJar + QLatin1Char('/') + kJar +
                                             QStringLiteral(".jar")),
                        QByteArrayLiteral("stub"));
            check(!InstanceFiles::isUnfinishedImport(kJar), "AA8 顶层有 jar 的不算半成品");
            check(InstanceFiles::checkImportTargetName(kJar, &problem) ==
                      InstanceFiles::ImportNameVerdict::Taken,
                  "AA8 有 jar 的那一份也拒", problem);

            // ③ json 没声明父版本 = 不是导入写下的那份形状（可能是一份手工放进去的
            // 老式实例：自己没有 jar、json 也不继承）。
            const QString kNoParent = QStringLiteral("半成品-没父版本");
            makeHalf(kNoParent, QByteArrayLiteral("{\"id\":\"x\",\"type\":\"release\"}"));
            check(!InstanceFiles::isUnfinishedImport(kNoParent),
                  "AA8 json 不声明父版本的不算半成品");

            // ④ 名字本身就不合法的那条路**不查盘**：`..` 这种名字拼出来的路径不可信，
            // 拿它去盘上问"是不是半成品"没有意义。
            check(InstanceFiles::checkImportTargetName(QStringLiteral(".."), &problem) ==
                      InstanceFiles::ImportNameVerdict::Taken,
                  "AA8 名字不合法照旧拒（而且不因为盘上有个同名目录就放行）", problem);

            // ⑤ 执行那一半也认这条口子。用 AA7 那套"Java 99"的套路：Forge/NeoForge 的包
            // 在**开始之前**要算安装器用的 Java，算不出来就停在那儿 —— 于是这一条能
            // 在离线机器上验"名字这一关过了没有"，一个字节都不会下。
            // 判据：弹的是「Java 版本不够」（名字过了）而不是「换个名字」（名字没过）。
            const QString kJava99Id2 = QStringLiteral("1.21.4");
            writeFileAt(versionsDir.filePath(kJava99Id2 + QLatin1Char('/') + kJava99Id2 +
                                             QStringLiteral(".json")),
                        QByteArrayLiteral("{\"id\":\"1.21.4\",\"javaVersion\":"
                                          "{\"majorVersion\":99}}"));
            if (windowJava != nullptr) {
                pinGlobalJava(QStringLiteral("C:/没有这个目录/java.exe"));
                QApplication::processEvents();
                GlobalJavaSettings probe;
                if (probe.javaPath() == QStringLiteral("C:/没有这个目录/java.exe") &&
                    !probe.autoSelectJava()) {
                    dismissNextPrompt();
                    check(!tryImport(kNeoForgeJson, kHalf),
                          "AA8 半成品的名字 + 要 Java 99 的包 → 停在下一步（没开始导入）");
                    check(g_modalTitle == QStringLiteral("Java 版本不够"),
                          "AA8 执行那一半认这条口子（拦下它的是**下一步**，不是名字）",
                          g_modalTitle + QStringLiteral(" / ") + g_modalText);
                    // 对照：同一个包撞上**别的**实例名（26.3 是个装好的实例）时，
                    // 名字这一关就该把它拦下来 —— 免得上面那条变成"随便什么都放行"。
                    dismissNextPrompt();
                    check(!tryImport(kNeoForgeJson, QStringLiteral("26.3")),
                          "AA8 对照：同一个包撞上别的实例名 → 拒");
                    check(g_modalTitle == QStringLiteral("换个名字"),
                          "AA8 对照：拒它的是名字那一关",
                          g_modalTitle + QStringLiteral(" / ") + g_modalText);
                }
            }
            QDir(versionsDir.filePath(kJava99Id2)).removeRecursively();
            if (windowJava != nullptr) {
                unpinGlobalJava();
                QApplication::processEvents();
            }

            // 收拾：这一节造的那几个目录一个都不留（AA 收尾要数目录个数）。
            for (const QString &id : {kHalf, kDone, kJar, kNoParent}) {
                check(QDir(versionsDir.filePath(id)).removeRecursively(),
                      QStringLiteral("AA8 收尾：清理干净 ") + id);
            }
        }

        // ---- AA9 CurseForge 那一份清单（`manifest.json`）走的是同一个执行函数 ----
        //
        // CurseForge 那一份清单也能装了（有下载地址就装，不是直接拒）。
        // 这一组钉三件事：
        // ① 界面那一半真的按格式分流了（同一个字节串交给 CurseForge 的解析器，不是
        // Modrinth 那个 —— 两个解析器给的原因话说的是**不同的清单文件名**，一眼分得开）；
        // ② CurseForge 那个加载器写法（一个串里带版本）翻译到 Modrinth 那套键之后，
        // 下游那几道闸照旧认（包括"认不出来的加载器"那条 —— 键不能是空串）；
        // ③ **一个字节都不花**：这些用例全停在"换地址"那一步之前，所以离线也能跑
        // （换地址那一发要真网络，验它在 verify 的 ZG7：本机假接口端到端）。
        {
            const auto cfManifest = [](const char *loader) {
                return QByteArrayLiteral("{\"manifestVersion\":1,\"manifestType\":"
                                         "\"minecraftModpack\",\"name\":\"CF 假包\","
                                         "\"version\":\"1.0\",\"minecraft\":{\"version\":"
                                         "\"1.20.1\",\"modLoaders\":[{\"id\":\"") +
                       QByteArray(loader) +
                       QByteArrayLiteral("\",\"primary\":true}]},"
                                         "\"files\":[{\"projectID\":238222,\"fileID\":8971390,"
                                         "\"required\":true}]}");
            };

            // ① 一句读不懂的字节：拦它的是 **CurseForge** 那个解析器 —— 它说的是
            // `manifest.json`（Modrinth 那个会说 `modrinth.index.json`）。这一条就是
            // "格式真的分流了"最直接的证据。
            dismissNextPrompt();
            check(!tryImport(QByteArrayLiteral(R"({"hello":"world"})"), QStringLiteral("新实例"),
                             ModpackImport::Format::CurseForge),
                  "AA9 合法 json 但不是 CF 清单 → 拒");
            check(g_modalTitle == QStringLiteral("导入整合包"),
                  "AA9 弹的还是「导入整合包」", g_modalTitle);
            check(g_modalText.contains(ModpackImport::curseForgeManifestName()) &&
                      !g_modalText.contains(ModpackImport::modrinthIndexName()),
                  "AA9 但这句话是 **CurseForge** 那个解析器说的（它只说 manifest.json）",
                  g_modalText);

            // ② 清单在、一条文件都没有：CF 的包不可能长这样，拒。
            dismissNextPrompt();
            check(!tryImport(QByteArrayLiteral("{\"manifestVersion\":1,\"minecraft\":"
                                               "{\"version\":\"1.20.1\"},\"files\":[]}"),
                             QStringLiteral("新实例"), ModpackImport::Format::CurseForge),
                  "AA9 文件清单是空的 → 拒");
            check(g_modalText.contains(QStringLiteral("整合包清单未列出待下载文件")),
                  "AA9 说清了是清单空的", g_modalText);

            // ③ 加载器那一关认得 CF 那个写法：`quilt-0.20.0` 翻译成 `quilt-loader`。
            // 2.1.4 起 Quilt 已接通，所以这里的结论是"键/版本拆对了、不再拒绝"。
            QString quiltKey;
            QString quiltVersion;
            check(ModpackImport::curseForgeLoaderKey(QStringLiteral("quilt-0.20.0"), &quiltKey,
                                                     &quiltVersion)
                      && quiltKey == ModpackImport::quiltLoaderKey()
                      && quiltVersion == QStringLiteral("0.20.0")
                      && ModpackImport::unsupportedLoaderText(quiltKey, quiltVersion).isEmpty(),
                  "AA9 quilt 的 CF 写法拆成 quilt-loader / 0.20.0，且不再拒绝",
                  quiltKey + QStringLiteral(" / ") + quiltVersion);

            // ④ **认不出来的加载器**：`liteloader` 在 CF 那边是合法的写法。这里最要紧的是
            // 它**不许**被当成"纯原版包"放行 —— 键给了空的 key 就会一路装出一个进得去但
            // 全是原版的实例（模组躺在 mods/ 里没人管），而那一个错不报警。
            dismissNextPrompt();
            check(!tryImport(cfManifest("liteloader-1.12.2"), QStringLiteral("新实例"),
                             ModpackImport::Format::CurseForge),
                  "AA9 liteloader 的 CF 包 → 拒（不许当成纯原版包放行）");
            check(g_modalTitle == QStringLiteral("无法安装这个整合包") &&
                      g_modalText.contains(QStringLiteral("liteloader")),
                  "AA9 拦它的话里带着那个加载器的名字", g_modalTitle + QStringLiteral(" / ") +
                                                          g_modalText);

        }

        // ---- AA10 CF 的"换地址"那一发：界面那一半真的发出去了、失败时拦得住 ----
        //
        // 上面 AA9 那几条全停在换地址**之前**（那一步要真网络）。这一条把接口指到本机的假
        // 服务器上（`Launcher/CurseForgeApiBase`：产品从不写这个键，验证工程拿它当口子），
        // 于是**同一段产品代码**在这台机器上就能整条跑完，一个外网包都不发。钉三件事：
        // ① 界面那一半确实调了"换地址"那一发（假服务器上数得到那一次连接）；
        // ② 它失败时**拦得住**：不开始导入、沙盒里不留东西；
        // ③ 弹的是「拿不到文件清单」这个标题 —— 这个标题只有 CF 那一支会弹。
        //
        // NOTE: 地址是 MainWindow **构造那一刻**从 QSettings 读进去的（LauncherSettings 的
        // 构造 + 构造里那句 syncCurseForgeSettings），所以必须"先写 ini、再造窗口"。
        // 这也是为什么这里要另起一个窗口，而不是用上面那个 —— 它早就构造完了。
        {
            OneShotCfErrorApi api;
            check(api.start(), "AA10 本机假接口起来了（起不来这一条就没意义，直接算失败）");
            if (api.server.isListening()) {
                const QString kBaseKey = QStringLiteral("Launcher/CurseForgeApiBase");
                {
                    QSettings sandbox;
                    sandbox.setValue(kBaseKey, api.baseUrl());
                    sandbox.sync();
                }
                {
                    MainWindow cfWindow;
                    cfWindow.show();
                    QApplication::processEvents();

                    // 纯原版的 CF 包（一条 modLoaders 都没有）—— 加载器那一关放行，
                    // 于是这一发一定走到"换地址"，而它注定失败（假接口回 500）。
                    const QByteArray vanillaCf = QByteArrayLiteral(
                        "{\"manifestVersion\":1,\"manifestType\":\"minecraftModpack\","
                        "\"name\":\"CF 纯原版假包\",\"version\":\"1.0\","
                        "\"minecraft\":{\"version\":\"1.20.1\",\"modLoaders\":[]},"
                        "\"files\":[{\"projectID\":238222,\"fileID\":8971390,"
                        "\"required\":true}]}");
                    bool started = true;
                    dismissNextPrompt();
                    const bool invoked = QMetaObject::invokeMethod(
                        &cfWindow, "applyModpackImport", Qt::DirectConnection,
                        Q_RETURN_ARG(bool, started), Q_ARG(QString, kZip),
                        Q_ARG(QString, QStringLiteral("CF 假包-新实例")),
                        Q_ARG(QByteArray, vanillaCf),
                        Q_ARG(ModpackImport::Format, ModpackImport::Format::CurseForge));
                    check(invoked && !started,
                          "AA10 换地址失败 → 导入没开始（不装一个只有加载器的空实例）");
                    check(api.hits >= 1,
                          "AA10 那一发真的发到了我们指的那台机器上（发到别处就说明口子没生效）",
                          QString::number(api.hits));
                    check(g_modalTitle == QStringLiteral("拿不到文件清单"),
                          "AA10 弹的是「拿不到文件清单」（这个标题只有 CF 那一支会弹）",
                          g_modalTitle + QStringLiteral(" / ") + g_modalText);
                    check(g_modalText.contains(QStringLiteral("接口说你没填 key")),
                          "AA10 接口那句原话原样带给了用户（我们不猜它为什么拒）", g_modalText);
                    check(g_modalWasWarning,
                          "AA10 这是会挡住用户操作的那一类 ⇒ 是警告框（不是一句报信）");
                }
                {
                    // 还回去：产品自己从不写这个键，验证工程也不能给它留在沙盒的 ini 里
                    // （下一个跑的人会捡到一个指向本机端口的地址）。
                    QSettings sandbox;
                    sandbox.remove(kBaseKey);
                    sandbox.sync();
                    check(!QSettings().contains(kBaseKey), "AA10 那个口子键还回去了（没留在沙盒里）");
                }
            }
        }

        // ---- AA11 已经有一批下载在跑：这时候**拖一个整合包进来** ----
        //
        // 那颗按钮这时候是灰的，可是窗口的拖放**不经过按钮**（用户从资源管理器里把文件
        // 拖进来，走的是 dropEvent）。这里必须有一道闸：没有的话「导入」那一档占用
        // 会被第二趟直接覆盖，第一趟收尾时按那个 id 收，两趟互相把对方的忙态/目录认错。
        {
            const int startedBefore = countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包"));
            window.seedDownloadInFlightForTest(true);
            const int dirsBeforeDrop =
                QDir(versionsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
            dismissNextPrompt();
            const bool invoked =
                QMetaObject::invokeMethod(&window, "importModpackFromFile", Qt::DirectConnection,
                                          Q_ARG(QString, kZip), Q_ARG(QString, QString()));
            check(invoked, "AA11 importModpackFromFile 接得上（两个参数都认得）");
            check(g_modalTitle == QStringLiteral("现在正忙"),
                  "★★ AA11 下载还没完时拖进来的整合包被挡下，并且当场说了一句人话"
                  "（不是默默吞掉、也不是排进队列）",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(g_modalText.contains(QStringLiteral("已有下载或导入任务正在进行")),
                  "AA11 那句话说的就是「已经有一件在跑」", g_modalText);
            check(!g_modalWasWarning,
                  "AA11 这是报信那一类（不是错误）");
            check(countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包")) == startedBefore,
                  "★★ AA11 它一个导入流程都没起（按钮灰着不等于拖放也被挡住）");
            check(QDir(versionsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() ==
                      dirsBeforeDrop,
                  "AA11 沙盒里也没多出半个目录");
            window.seedDownloadInFlightForTest(false);
        }

        // ---- 收尾：这一节一条导入都没真开始过 ----
        check(countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包")) == importsBefore,
              "AA 收尾：日志里一次「开始导入整合包」都没有（全是被拦下的）",
              QString::number(importsBefore) + QStringLiteral(" → ") +
                  QString::number(countOccurrences(messagesSnapshot(),
                                                   QStringLiteral("开始导入整合包"))));
        check(importBtn != nullptr && importBtn->isEnabled(),
              "AA 收尾：导入按钮还是亮的（一次都没进忙态）");
        // AA12 GTNH 首启资源预取（2.1.6b）的接线：这几条包都不是已知集合的消费者，
        // 所以预取一条都不该起 —— 既不该凭空多出一条任务，也不该把界面顶进忙态、
        // 更不该在共享 assets 里建出 asset_director。
        {
            const QString overlay = AppPaths::assetsDir() + QStringLiteral("/asset_director");
            const bool overlayBefore = QFileInfo::exists(overlay);
            const int imports = countOccurrences(messagesSnapshot(), QStringLiteral("正在预取"));
            // 它是私有槽（和 repairInstanceFiles 同一条规矩）：验证工程按名字 invoke，
            // 这样接线断了会当场露出来（invokeMethod 返回 false），而不是编译期换一种写法。
            const bool invokedMissing = QMetaObject::invokeMethod(
                &window, "maybePrefetchAssetDirector", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("no-such-instance")));
            check(invokedMissing, "AA12 maybePrefetchAssetDirector 接得上（一个 QString 参数）");
            const QStringList existing =
                QDir(versionsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            if (!existing.isEmpty()) {
                QMetaObject::invokeMethod(&window, "maybePrefetchAssetDirector", Qt::DirectConnection,
                                          Q_ARG(QString, existing.first()));
            }
            check(importBtn != nullptr && importBtn->isEnabled(),
                  "AA12 预取不适用于这个实例时不把界面顶进忙态");
            check(!QFileInfo::exists(overlay) || overlayBefore,
                  "AA12 不适用时连 assets/asset_director 都不会被建出来", overlay);
            check(countOccurrences(messagesSnapshot(), QStringLiteral("正在预取")) == imports,
                  "AA12 不适用时一条“正在预取”的日志都不会写");
        }
        // ---- AA13 下载页一键导入入口与忙态防重入 -------------------------------
        {
            const int startedBefore = countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包"));
            window.seedDownloadInFlightForTest(true);
            dismissNextPrompt();
            const bool invoked = QMetaObject::invokeMethod(
                &window, "importModpackOneClick", Qt::DirectConnection, Q_ARG(QString, kZip),
                Q_ARG(QString, QStringLiteral("oneclick-busy-test")));
            check(invoked, "AA13 importModpackOneClick 接得上");
            check(g_modalTitle == QStringLiteral("现在正忙"),
                  "AA13 已有任务在跑时拦截一键导入续接", g_modalTitle);
            check(countOccurrences(messagesSnapshot(), QStringLiteral("开始导入整合包")) == startedBefore,
                  "AA13 被忙态拦截时一个字节都没开始导");
            window.seedDownloadInFlightForTest(false);
        }

        // ---- AA14 一键导入续装不再二次确认；指纹冲突改弹命名框 ---------------
        if (windowJava != nullptr) {
            QTemporaryDir tempPackDir;
            const QByteArray manifestBytes = QByteArrayLiteral(R"({
                "formatVersion": 1,
                "game": "minecraft",
                "versionId": "1.0.0",
                "name": "一键续装假包",
                "dependencies": {"minecraft": "1.21.4", "neoforge": "21.1.72"},
                "files": [{"path": "mods/example.jar",
                           "hashes": {"sha1": "0000000000000000000000000000000000000000"},
                           "downloads": ["https://example.invalid/example.jar"],
                           "fileSize": 1234}]
            })");
            const QString manifestPath = tempPackDir.filePath(QStringLiteral("modrinth.index.json"));
            const QString mrpackPath = tempPackDir.filePath(QStringLiteral("oneclick-resume.mrpack"));
            writeFileAt(manifestPath, manifestBytes);
            QString zipErr;
            const bool zipped = ZipUtil::createZip(
                mrpackPath, {{QStringLiteral("modrinth.index.json"), manifestPath}}, &zipErr);
            check(zipped, "AA14 前提：测试用 .mrpack 打出来了", zipErr);

            const QString kJava99Id3 = QStringLiteral("1.21.4");
            writeFileAt(versionsDir.filePath(kJava99Id3 + QLatin1Char('/') + kJava99Id3 +
                                             QStringLiteral(".json")),
                        QByteArrayLiteral("{\"id\":\"1.21.4\",\"javaVersion\":"
                                          "{\"majorVersion\":99}}"));
            pinGlobalJava(QStringLiteral("C:/没有这个目录/java.exe"));
            QApplication::processEvents();

            const QString fingerprint = InstanceFiles::importManifestFingerprint(manifestBytes);
            const QString kResumeId = QStringLiteral("oneclick-resume-half");
            versionsDir.mkpath(kResumeId);
            writeFileAt(InstanceFiles::importMarkerPath(kResumeId),
                        QStringLiteral("{\"startedAt\":\"2026-01-01T00:00:00Z\",\"manifestSha256\":\"%1\"}")
                            .arg(fingerprint)
                            .toUtf8());

            const QString kMismatchId = QStringLiteral("oneclick-mismatch-half");
            versionsDir.mkpath(kMismatchId);
            writeFileAt(InstanceFiles::importMarkerPath(kMismatchId),
                        QByteArrayLiteral("{\"startedAt\":\"2026-01-01T00:00:00Z\","
                                          "\"manifestSha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}"));

            GlobalJavaSettings probe;
            if (zipped && probe.javaPath() == QStringLiteral("C:/没有这个目录/java.exe")
                && !probe.autoSelectJava()) {
                const int resumeLogsBefore = countOccurrences(
                    messagesSnapshot(), QStringLiteral("继续安装上次未完成的「%1」").arg(kResumeId));
                g_modalTitle.clear();
                dismissNextPrompt();
                const bool invokedResume = QMetaObject::invokeMethod(
                    &window, "importModpackOneClick", Qt::DirectConnection,
                    Q_ARG(QString, mrpackPath), Q_ARG(QString, kResumeId));
                check(invokedResume, "AA14 OkResume 调用接得上");
                check(g_modalTitle == QStringLiteral("需要 Java 环境"),
                      "AA14 OkResume 不再弹第二次「接着上次的导入」确认框（直接走到下一步）",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
                check(countOccurrences(messagesSnapshot(),
                                       QStringLiteral("继续安装上次未完成的「%1」").arg(kResumeId))
                          == resumeLogsBefore + 1,
                      "AA14 OkResume 日志里留下一句继续安装说明");

                g_modalTitle.clear();
                g_modalWasWarning = false;
                dismissNextPrompt();
                const bool invokedMismatch = QMetaObject::invokeMethod(
                    &window, "importModpackOneClick", Qt::DirectConnection,
                    Q_ARG(QString, mrpackPath), Q_ARG(QString, kMismatchId));
                check(invokedMismatch, "AA14 指纹冲突调用接得上");
                check(g_modalTitle == QStringLiteral("换个名字") && !g_modalWasWarning,
                      "AA14 指纹冲突时不直接报错失败，而是弹命名框让用户换名",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
            }

            unpinGlobalJava();
            QApplication::processEvents();
            QDir(versionsDir.filePath(kJava99Id3)).removeRecursively();
            QDir(versionsDir.filePath(kResumeId)).removeRecursively();
            QDir(versionsDir.filePath(kMismatchId)).removeRecursively();
        }

        check(QDir(versionsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == dirsBefore,
              "AA 收尾：沙盒里没多出目录（被拦下的导入不在盘上留东西）",
              QString::number(dirsBefore) + QStringLiteral(" → ") +
                  QString::number(QDir(versionsDir).entryList(QDir::Dirs |
                                                              QDir::NoDotAndDotDot).size()));
    }
    return 0;
}


