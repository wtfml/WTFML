#include "Support.hpp"
#include "TextMarks.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/LaunchController.hpp"
#include "util/SecretRedactor.hpp"
void runInstanceStartActionsTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T19.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }
    {
        AppPaths::setMinecraftDir(root.path());
        const QString secret = QStringLiteral("__PROCESS_CREDENTIAL_TEST_987654321__");
        QString output, errors;
        bool finished = false;
        LauncherManager manager;
        QObject::connect(&manager, &LauncherManager::processOutput, &manager,
                         [&](int, const QString &text) { output += text; });
        QObject::connect(&manager, &LauncherManager::processError, &manager,
                         [&](int, const QString &text) { errors += text; });
        QObject::connect(&manager, &LauncherManager::gameFinished, &manager,
                         [&](int, int, QProcess::ExitStatus) { finished = true; });
        EffectiveJavaSettings java;
        java.javaPath = QCoreApplication::applicationFilePath();
        const auto before = messagesSnapshot().size();
        const int session = manager.startGame(
            {QStringLiteral("token-echo"), QStringLiteral("--accessToken"), secret}, java,
            QStringLiteral("credential-test"));
        check(session > 0 && pumpUntil([&] { return finished; }, 5000),
              "凭据测试子进程正常结束");
        const QString mask = SecretRedactor::replacement();
        // Windows CRT text streams expand LF to CRLF; the launcher preserves it.
        output.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        errors.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        check(output.endsWith(mask + QLatin1Char('\n') + mask) &&
                  !output.contains(secret) && errors == mask + QLatin1Char('\n'),
              "stdout/stderr 分块和退出残片均在信号发出前遮盖");
        const QString logs = messagesSnapshot().mid(before).join(QLatin1Char('\n'));
        check(!logs.contains(secret) && !logs.contains(secret.left(secret.size() / 2)),
              "进程输出调试日志不含完整凭据或分块片段");
        AppPaths::setMinecraftDir(originalDir);
    }

    // 数据根目录**两头都要改**（理由见 AH0：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次），顺手把「下载源」钉成**官方**：镜像那一档会把客户端
    // jar 的地址换成按版本号拼出来的 BMCLAPI 地址（见 DownloadManager::downloadClientJar），
    // 那一下就真出网了，而这一节的远端全是 file://。之前的节可能把它留成 Mirror
    // （设置页那几段验的就是"改完立刻落盘"），所以这里必须显式写。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const QVariant sourceBefore = sandbox.value(QStringLiteral("Download/Source"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.setValue(QStringLiteral("Download/Source"),
                     LauncherSettings::downloadSourceKey(DownloadSource::Official));
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    // 构造 MainWindow 会扫这几样（扫不到只会写日志，但那些话会混进这一节要看的行里）。
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));

    // ---------- 夹具：远端两个文件 + 两个"看起来已安装"的实例 ----------
    // 远端是这一节自己造的两个本地文件（url 是 file:// 形式，sha1/size 现算）。
    const QDir remoteDir(root.filePath(QStringLiteral("remote")));
    remoteDir.mkpath(QStringLiteral("."));
    const auto makeRemote = [&remoteDir](const QString &relPath,
                                         const QByteArray &content) -> QJsonObject {
        const QString path = remoteDir.filePath(relPath);
        QDir().mkpath(QFileInfo(path).absolutePath());
        writeFileAt(path, content);
        return QJsonObject{
            {QStringLiteral("url"), QUrl::fromLocalFile(path).toString()},
            {QStringLiteral("sha1"),
             QString::fromLatin1(
                 QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex())},
            {QStringLiteral("size"), static_cast<qint64>(content.size())}};
    };
    const QByteArray realJarBytes = QByteArrayLiteral("T19-client-jar-body-0123456789");
    const QByteArray realLibBytes = QByteArrayLiteral("T19-lib-body");
    const QJsonObject jarRemote = makeRemote(QStringLiteral("maven/t19/client.jar"), realJarBytes);
    const QJsonObject libRemote =
        makeRemote(QStringLiteral("maven/t19/t19lib-1.0.jar"), realLibBytes);

    const auto versionJsonFor = [&](const QString &id) {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("type"), QStringLiteral("release")},
            {QStringLiteral("mainClass"), QStringLiteral("net.minecraft.client.main.Main")},
            {QStringLiteral("libraries"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("synth:t19lib:1.0")},
                 {QStringLiteral("downloads"),
                  QJsonObject{{QStringLiteral("artifact"),
                               QJsonObject{
                                   {QStringLiteral("url"), libRemote.value(QStringLiteral("url"))},
                                   {QStringLiteral("sha1"), libRemote.value(QStringLiteral("sha1"))},
                                   {QStringLiteral("size"), libRemote.value(QStringLiteral("size"))},
                                   {QStringLiteral("path"),
                                    QStringLiteral("synth/t19lib/1.0/t19lib-1.0.jar")}}}}}}}},
            {QStringLiteral("downloads"),
             QJsonObject{{QStringLiteral("client"), jarRemote}}}};
    };

    // 版本 id 取**版本号形状**（不是"测试包A"那种）：显示名就是拿它拼的，取个怪名字会
    // 让"显示名里含自定义名"这条断言分不清是拼对还是碰巧。
    const QString idA = QStringLiteral("1.99.1");
    const QString idB = QStringLiteral("1.99.2");
    const QString emptyId = QStringLiteral("1.99.9-empty");
    const QString nameA = QStringLiteral("T19补全测试包");
    const QString nameB = QStringLiteral("T19被挡住的那个");

    // 坏的 jar：**字节数和远端一模一样、内容全是 'x'** —— 判据只能是 sha1。
    // 这一份就是 ① 的载体（大小对得上、内容不对 ⇒ 必须重下）。
    const auto bogusBytes = [&jarRemote]() {
        return QByteArray(static_cast<int>(jarRemote.value(QStringLiteral("size")).toInt()), 'x');
    };
    const auto installFake = [&](const QString &id, const QString &customName) {
        const QDir dir(versionsDir.filePath(id));
        dir.mkpath(QStringLiteral("."));
        writeFileAt(dir.filePath(id + QStringLiteral(".json")),
                    QJsonDocument(versionJsonFor(id)).toJson(QJsonDocument::Compact));
        writeFileAt(dir.filePath(id + QStringLiteral(".jar")), bogusBytes());
        // 自定义名（wtfml.meta 的 "name"）：⑤ 那条断言靠它分辨"收尾那句说的是谁"。
        writeFileAt(dir.filePath(QStringLiteral("wtfml.meta")),
                    QJsonDocument(QJsonObject{{QStringLiteral("name"), customName}})
                        .toJson(QJsonDocument::Compact));
    };
    installFake(idA, nameA);
    installFake(idB, nameB);
    // 第三个：只有 jar、**没有 json** —— ③ 那一档（"版本信息缺失"，而且一个批次都不许开）。
    makeInstalledVersion(versionsDir, emptyId);
    check(QFile::exists(versionsDir.filePath(idA + QStringLiteral("/") + idA + QStringLiteral(".json")))
              && QFile::exists(versionsDir.filePath(emptyId + QStringLiteral("/") + emptyId
                                                    + QStringLiteral(".jar"))),
          "T19.0 两个假实例和一个「没有 json」的目录都摆好了");

    const QString jarPathA = versionsDir.filePath(idA + QStringLiteral("/") + idA
                                                  + QStringLiteral(".jar"));
    const QString jsonPathA = versionsDir.filePath(idA + QStringLiteral("/") + idA
                                                   + QStringLiteral(".json"));
    const QString jarPathB = versionsDir.filePath(idB + QStringLiteral("/") + idB
                                                  + QStringLiteral(".jar"));
    const QString libPath = root.path() + QStringLiteral("/libraries/synth/t19lib/1.0/"
                                                         "t19lib-1.0.jar");
    const QByteArray bogusJarA = readBytesAt(jarPathA);
    const QByteArray bogusJarB = readBytesAt(jarPathB);
    const QByteArray jsonABefore = readBytesAt(jsonPathA);
    check(bogusJarA != realJarBytes && bogusJarA.size() == realJarBytes.size(),
          "T19.0 摆好的坏 jar 和远端**一样大、不一样的内容**（判据只能是 sha1）",
          QStringLiteral("%1 vs %2").arg(bogusJarA.size()).arg(realJarBytes.size()));
    check(!QFile::exists(libPath), "T19.0 那个库先不在盘上（缺的那一类）");

    // 身份：一个**新加的离线账号**（离线 ⇒ 这一节一个请求都不发，而且导出那条路不用令牌）。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T19User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T19.0 建一个离线账号并设成当前身份（离线 ⇒ 导出那条路不用令牌）", offlineError);
    // 开窗就把 A 选上：产品恢复"上次启动的版本"的唯一来源就是盘上这一格。
    const QString lastVersionBefore = LauncherSettings().lastLaunchedVersion();
    rememberLastVersion(idA);

    // 只看得见"这一节开始之后"写的那几行日志（messagesSnapshot() 是**整个会话**的全局）。
    const int logMark = messagesSnapshot().size();
    const auto newLogContains = [&logMark](const QString &needle) {
        for (int i = logMark; i < messagesSnapshot().size(); ++i) {
            if (messagesSnapshot().at(i).contains(needle)) {
                return true;
            }
        }
        return false;
    };

    bool ready = false;
    {
        MainWindow w;
        w.show();
        QApplication::processEvents();
        VersionManager *vm = w.findChild<VersionManager *>();
        QComboBox *combo = comboByObjectName(&w, MainWindowUI::installedVersionComboName());
        ready = vm != nullptr && combo != nullptr && selectedId(combo) == idA;
        check(ready,
              "T19.0 主窗口起来了：版本管理器 / 已安装版本下拉框都在，选中的就是那一个实例",
              QStringLiteral("vm=%1 combo=%2(%3 项, 选中 %4)")
                  .arg(vm != nullptr)
                  .arg(combo != nullptr)
                  .arg(combo != nullptr ? combo->count() : -1)
                  .arg(combo != nullptr ? selectedId(combo) : QString()));
        if (ready) {
            const int indexA = combo->findData(idA);
            const QString displayA = indexA >= 0 ? combo->itemText(indexA) : QString();
            check(!displayA.isEmpty() && displayA != idA,
                  "T19.1 那个实例在下拉框里显示的是**用户认得的名字**、不是裸目录名 ——"
                  " 下面几条「收尾说的是谁」全靠它分辨",
                  displayA);
            const int indexB = combo->findData(idB);
            const QString displayB = indexB >= 0 ? combo->itemText(indexB) : QString();
            check(!displayB.isEmpty() && displayB != idB,
                  "T19.1 另一个夹具（③那一趟用的）也是自定义名", displayB);

            // ---------- ⓪ 体检：只报不装 ----------
            // 待办里那条「独立的完整性校验入口」的判据层。它和 ① 那个「补全文件」是同一件
            // 事的两半 —— 那一个缺什么补什么（联网、写盘），这一个一个字节都不写、一条
            // 请求都不发。
            //
            // 这里走的是**产品那条链**（VersionManager::checkInstanceIntegrity →
            // VersionIntegrity::plan/check），不是直接调那一层：清单从哪来（合并过的 json +
            // extractLibraries/extractAssetIndex）正是最容易两边写岔的地方。
            {
                // 这一份夹具的形状：jar 在、**字节数一样内容不对**（⇒ 坏），库不在（⇒ 缺），
                // json 里没有 assetIndex（⇒ 资源那两档一条都不列）。
                const VersionIntegrity::Report report = vm->checkInstanceIntegrity(idA);
                check(report.error.isEmpty() && report.checked == 2,
                      "T19.15 体检扫出两个该在的文件（客户端 jar + 那一个库）",
                      report.summary());
                check(report.problems.size() == 2 && report.missing() == 1
                          && report.corrupt() == 1,
                      "★ T19.15「缺」和「坏」分得开（缺 = 不在 / 0 字节，坏 = 大小或 sha1"
                      " 对不上）",
                      report.summary());
                check(report.summary() == QStringLiteral("2 个文件里 2 个要修：缺 1 个、坏 1 个"),
                      "T19.15 summary() 就是那一句话（界面直接显示它，所以一个字都别改）",
                      report.summary());
                bool saidMissing = false;
                bool saidWrongSha1 = false;
                for (const VersionIntegrity::Verdict &verdict : report.problems) {
                    if (verdict.status == VersionIntegrity::Status::Missing) {
                        saidMissing = verdict.detail() == QStringLiteral("缺少文件")
                                      && !QFile::exists(verdict.path);
                    }
                    if (verdict.status == VersionIntegrity::Status::WrongSha1) {
                        // 路径要和产品自己的 jarPath() 算出同一份（QFileInfo 比，免得被
                        // 正反斜杠咬一口）。
                        saidWrongSha1 = QFileInfo(verdict.path) == QFileInfo(jarPathA);
                    }
                }
                check(saidMissing, "T19.15 缺的那一条说的是「缺少文件」");
                check(saidWrongSha1,
                      "★ T19.15 那个坏 jar 判的是 **sha1 对不上**、不是字节数 —— 夹具里那两个"
                      "大小一模一样");

                // 这一条是「只报不装」的正身：体检走完之后盘上**一个字节都没变**。
                const VersionIntegrity::Report reportB = vm->checkInstanceIntegrity(idB);
                check(reportB.problems.size() == 2,
                      "T19.15 另一个夹具也一样是两个问题（下面拿它比「盘上没变」）",
                      reportB.summary());
                check(readBytesAt(jarPathB) == bogusJarB && !QFile::exists(libPath),
                      "★★ T19.15 体检**不修**：坏 jar 和缺的库原样躺着（修是「补全文件」的事）");
                check(!vm->isDownloading(),
                      "T19.15 而且一个下载批次都没开（这一层根本不碰 DownloadManager 的批次）");

                // 读不出 json 的那一档：只有 error 一句，别的都是空的 —— 它和"跑了、
                // 发现一堆问题"是两回事，界面那两句话也不一样。
                const VersionIntegrity::Report noJson = vm->checkInstanceIntegrity(emptyId);
                check(!noJson.error.isEmpty() && noJson.checked == 0
                          && noJson.problems.isEmpty() && noJson.summary() == noJson.error,
                      "★ T19.15 读不出 version.json ⇒ 只有 error 那一句（summary() 原样回它）",
                      noJson.error);
            }

            // ---------- ① 补全：只补缺的和坏的 ----------
            // 这一趟里会弹的**只有**②那一发（"暂时无法操作"）：补全自己那一趟的收尾句
            // 落在它后面，而 InlineMessage 一次只摆一条（第二条会被丢掉，命令行里留一句
            // 「已经有一条提示开着」）—— 收尾句的**形状**留给下面 ④ 那一趟干净的场景验。
            PromptAnswerer answerer({0});
            answerer.start();
            QMetaObject::invokeMethod(&w, "repairInstanceFiles", Qt::DirectConnection,
                                      Q_ARG(QString, idA));
            check(vm->isDownloading(),
                  "T19.2 点下去那一刻批次就开起来了（这一批据此把它从「能启动的实例」里滤掉）");
            check(combo->findData(idA) < 0,
                  "★ T19.2 补全期间它**不在**「已安装版本」下拉框里（文件正在被写，这时候"
                  "启动会读到半截 jar）");
            check(readBytesAt(jarPathA) == bogusJarA,
                  "T19.2 同步返回那一刻还没写盘（真的下载要过事件循环）");

            // ---------- ② 一批在飞的时候点另一个实例：当场顶回来 ----------
            QMetaObject::invokeMethod(&w, "repairInstanceFiles", Qt::DirectConnection,
                                      Q_ARG(QString, idB));
            check(answerer.answered.size() == 1,
                  "T19.3 一批还在飞的时候点另一个实例：**当场**弹一句（不是排队等）",
                  QString::number(answerer.answered.size()));
            if (!answerer.answered.isEmpty()) {
                const PromptAnswerer::Answered &refused = answerer.answered.at(0);
                check(refused.title == QStringLiteral("暂时无法操作"),
                      "T19.3 那句话的标题是「暂时无法操作」", refused.title);
                check(refused.text.contains(
                          QStringLiteral("已有下载或安装任务正在进行，请结束后再检查这个实例。")),
                      "T19.3 正文说清是「另有一批在跑」（不是一句笼统的忙）", refused.text);
            }
            check(readBytesAt(jarPathB) == bogusJarB,
                  "T19.3 被顶回来的那一次**一个字节都没动**（它那个坏 jar 原样躺着）");
            check(!newLogContains(QStringLiteral("开始校验「%1」").arg(displayB)),
                  "★ T19.3 而且它那一趟压根**没开始**（连「开始校验」那一行日志都没有 ——"
                  " 光看文件没变分不出「被拦住了」和「跑完了但没活干」）");

            // ---------- ①（续）那一批真的走完了 ----------
            const bool completed = pumpUntil([vm] { return !vm->isDownloading(); }, 20000);
            answerer.stop();
            check(completed, "T19.4 那一批走完了（忙态、被滤掉的实例都跟着恢复）");
            check(readBytesAt(jarPathA) == realJarBytes,
                  "★ T19.5 坏掉的 jar 被换回来了（字节数一样、内容不对 —— 判据是 sha1）");
            check(readBytesAt(libPath) == realLibBytes,
                  "T19.5 缺的那个库也补回来了（「缺哪个补哪个」的那一半）");
            check(readBytesAt(jsonPathA) == jsonABefore,
                  "T19.5 **不动** version.json（补全不是重装）");
            check(combo->findData(idA) >= 0 && selectedId(combo) == idA,
                  "★ T19.5 补全完它**回到**下拉框里、而且被选中（补全期间它是被滤掉的，"
                  "刷新只保得住「原来选中的」那一项，保不住「被滤掉的那一个」）");
            check(newLogContains(QStringLiteral("正在校验「%1」的文件，跳过已存在的文件").arg(displayA)),
                  "T19.5 日志里「开始」那一行说的是显示名（点下去就该有一行，用户要看得见它动了）");
            check(newLogContains(TextMarks::done() + QStringLiteral(" 实例「%1」的文件校验完成：").arg(displayA)),
                  "T19.5 日志里「结束」那一行也是");

            // ---------- ⓪（续）补全之后再体检一遍：一句坏话都没有 ----------
            // 和上面那一趟成对 —— 同一个函数、同一份实例：补全之前两个问题，补全之后零个。
            // 这也是「只报不装」的另一面（它不会因为"我刚报过"就顺手改点什么）。
            {
                const VersionIntegrity::Report after = vm->checkInstanceIntegrity(idA);
                check(after.error.isEmpty() && after.checked == 2 && after.problems.isEmpty(),
                      "★ T19.15 补全之后同一份体检是「2 个文件都完好」（修之前是「2 个要修」）",
                      after.summary());
                check(after.summary() == QStringLiteral("2 个文件都完好"),
                      "T19.15 好的那一句话也钉住", after.summary());
            }

            // ---------- ③ 换一个实例再来一趟干净的：收尾那句话的形状 ----------
            // ①那一趟的收尾句被②那一发顶掉了（提示一次只摆一条 —— 这是既有设计，不是
            // 这批改动引进来的），所以那句话的**形状**挪到这里验：这一趟从点下去到收尾
            // 中间一条别的提示都没有。
            {
                PromptAnswerer answerer4({0});
                answerer4.start();
                QMetaObject::invokeMethod(&w, "repairInstanceFiles", Qt::DirectConnection,
                                          Q_ARG(QString, idB));
                const bool finished = pumpUntil([vm] { return !vm->isDownloading(); }, 20000);
                const bool shown =
                    pumpUntil([&answerer4] { return !answerer4.answered.isEmpty(); }, 10000);
                answerer4.stop();
                check(finished, "T19.6 这一趟也走完了（同一段代码、换一个实例）");
                check(shown && !answerer4.answered.isEmpty() &&
                          answerer4.answered.at(0).title == QStringLiteral("检查完成"),
                      "★ T19.6 收尾那句话的标题是「检查完成」（不是「下载完成」—— 用户点的是检查）",
                      answerer4.answered.isEmpty() ? QStringLiteral("没弹")
                                                   : answerer4.answered.at(0).title);
                check(!answerer4.answered.isEmpty() &&
                          answerer4.answered.at(0).text.contains(
                              QStringLiteral("实例「%1」的文件都齐了。").arg(displayB)),
                      "★ T19.6 那句话说的是**下拉框里那个显示名** —— 收尾必须是「先清 "
                      "「补全」那一档占用、再 setDownloadUiBusy(false)」：反过来这一趟刷新时"
                      "它还被滤着，名字就只能退回裸目录名",
                      answerer4.answered.isEmpty() ? QString() : answerer4.answered.at(0).text);
                check(!answerer4.answered.isEmpty() &&
                          answerer4.answered.at(0).text.contains(QStringLiteral("个文件已全部就绪")),
                      "T19.6 汇总那句（几个文件、跳过几个）是数据层给的",
                      answerer4.answered.isEmpty() ? QString() : answerer4.answered.at(0).text);
                check(readBytesAt(jarPathB) == realJarBytes,
                      "T19.6 它那个坏 jar 也换回来了");
            }

            {
                auto *launcher = w.findChild<LaunchController *>();
                auto *activity = w.findChild<ActivityCenterController *>();
                check(launcher && activity, "T19 启动控制器与任务账本就绪");
                if (launcher && activity) {
                    auto *tasks = activity->tasks();
                    const int selected = combo->findData(idA);
                    combo->setCurrentIndex(selected);
                    QFile::remove(libPath);
                    writeFileAt(jarPathA, bogusJarA);
                    bool reachedJava = false;
                    const auto watch = QObject::connect(tasks, &Tasks::TaskManager::taskChanged, &w,
                        [&](Tasks::TaskId id) {
                            const auto row = tasks->snapshot(id);
                            if (!reachedJava && row.kind == Tasks::TaskKind::Launch && row.steps.size() > 2
                                && row.steps.at(2).state == Tasks::TaskStepState::Running) {
                                reachedJava = true;
                                launcher->abortStart(LaunchController::StartCancelSource::TaskCenter);
                            }
                        });
                    PromptAnswerer answerer({0, 0});
                    answerer.start();
                    launcher->onStartButtonClicked();
                    answerer.stop();
                    QObject::disconnect(watch);
                    check(reachedJava && readBytesAt(libPath) == realLibBytes
                              && readBytesAt(jarPathA) == realJarBytes,
                          "T19 启动时补回缺失的库和同大小的坏 JAR，再进入 Java 阶段");

                    const QString remoteLibrary = remoteDir.filePath(QStringLiteral("maven/t19/t19lib-1.0.jar"));
                    const bool removed = QFile::remove(libPath) && QFile::remove(remoteLibrary);
                    check(removed, "T19 缺失的依赖库无法从测试源补回");
                    PromptAnswerer failure({0});
                    failure.start();
                    launcher->onStartButtonClicked();
                    failure.stop();
                    check(!failure.answered.isEmpty() &&
                              failure.answered.first().title == QStringLiteral("无法启动") &&
                              failure.answered.first().text.contains(QStringLiteral("synth:t19lib:1.0")) &&
                              !QFile::exists(libPath),
                          "T19 补文件失败时停止启动并显示缺失的依赖库");
                    writeFileAt(remoteLibrary, realLibBytes);
                    writeFileAt(libPath, realLibBytes);
                }
            }

            // ---------- ④ 没有 version.json：一句人话，而且一个批次都不开 ----------
            {
                const int before3 = combo->findData(emptyId);
                PromptAnswerer answerer3({0});
                answerer3.start();
                QMetaObject::invokeMethod(&w, "repairInstanceFiles", Qt::DirectConnection,
                                          Q_ARG(QString, emptyId));
                const bool shown =
                    pumpUntil([&answerer3] { return !answerer3.answered.isEmpty(); }, 10000);
                answerer3.stop();
                check(shown && !answerer3.answered.isEmpty() &&
                          answerer3.answered.at(0).title == QStringLiteral("版本信息缺失"),
                      "T19.7 读不出 version.json ⇒ 标题「版本信息缺失」",
                      answerer3.answered.isEmpty() ? QStringLiteral("没弹")
                                                   : answerer3.answered.at(0).title);
                check(!answerer3.answered.isEmpty() &&
                          answerer3.answered.at(0).text.contains(
                              QStringLiteral("未找到 version.json")),
                      "★ T19.7 正文点的是「读不出 version.json」，不是一句笼统的失败",
                      answerer3.answered.isEmpty() ? QString() : answerer3.answered.at(0).text);
                check(!vm->isDownloading(),
                      "★ T19.7 而且**一个批次都没开**（开了的话按钮会一直灰着、"
                      "底栏那根进度条也停不下来）");
                check(combo->findData(emptyId) == before3,
                      "T19.7 它也没被当成「正在装的那一个」从列表里滤掉");
                check(newLogContains(QStringLiteral("无法校验 %1 的文件：找不到 version.json")
                                         .arg(emptyId)),
                      "T19.7 日志里留了一行（命令行那侧看得到）");
            }

            // ---------- ⑤ 空 id：什么都不做 ----------
            {
                PromptAnswerer quiet({0});
                quiet.start();
                QMetaObject::invokeMethod(&w, "repairInstanceFiles", Qt::DirectConnection,
                                          Q_ARG(QString, QString()));
                const bool fired = pumpUntil([&quiet] { return !quiet.answered.isEmpty(); }, 400);
                quiet.stop();
                check(!fired && !vm->isDownloading(),
                      "T19.8 空 id：什么都不做（产品里走不到 —— 对话框那边没选中实例就不给点；"
                      "验证工程打一条空 id 进来时也不该去碰数据层）");
            }

            // ---------- ⑥ 导出 .bat（离线身份那条路） ----------
            const QString scriptPath = root.filePath(QStringLiteral("out/start.bat"));
            QDir().mkpath(QFileInfo(scriptPath).absolutePath());
            {
                PromptAnswerer answerer5({0});
                answerer5.start();
                QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                          Q_ARG(QString, idA), Q_ARG(QString, scriptPath));
                const bool shown =
                    pumpUntil([&answerer5] { return !answerer5.answered.isEmpty(); }, 10000);
                answerer5.stop();
                check(shown && !answerer5.answered.isEmpty() &&
                          answerer5.answered.at(0).title == QStringLiteral("导出完成"),
                      "T19.9 导出完摆一句结果（不是默默写一个文件就走了）",
                      answerer5.answered.isEmpty() ? QStringLiteral("没弹")
                                                   : answerer5.answered.at(0).title);
                if (!answerer5.answered.isEmpty()) {
                    const PromptAnswerer::Answered &one = answerer5.answered.at(0);
                    check(one.text.contains(QDir::toNativeSeparators(scriptPath)),
                          "T19.9 正文把落点写全了（用户要照着双击它）", one.text);
                    check(one.text.contains(QStringLiteral("这一份用的是离线身份")),
                          "★ T19.9 离线那一趟的备注：说清游戏里就是这个名字", one.text);
                    check(one.text.contains(QStringLiteral("导出的是本次启动使用的参数")),
                          "T19.9 最后那句「改了要再导一遍」的依据也在", one.text);
                }
            }
            const QByteArray scriptBytes = readBytesAt(scriptPath);
            const QString scriptText = QString::fromLocal8Bit(scriptBytes);
            check(!scriptBytes.isEmpty(), "T19.10 .bat 写出来了");
            check(scriptText.startsWith(QStringLiteral("@echo off\r\n")),
                  "T19.10 第一句是 @echo off（否则每一行都要回显出来）",
                  scriptText.left(20));
            // 中文注释只在**本地代码页写得出来**的时候才会出现（英文版 Windows 上产品
            // 退回英文注释，见 exportLaunchScript 里 localOrAscii 那段）。判据跟着同一张
            // 规矩走 —— 否则这台机器绿、那台机器红，量的就不是产品了。
            const bool localCanEncodeChinese =
                QString::fromLocal8Bit(QStringLiteral("由 WTFML 导出").toLocal8Bit()) ==
                QStringLiteral("由 WTFML 导出");
            check(scriptText.contains(localCanEncodeChinese
                                          ? QStringLiteral("rem 由 WTFML 导出")
                                          : QStringLiteral("rem Exported by WTFML")),
                  "★ T19.10 顶上那行 rem 按**本地 8 位**读得回来（不是 UTF-8 写出去的 ——"
                  " cmd 是按当前代码页读文件，中文会变成乱码）");
            check(!scriptText.contains(QStringLiteral("带着你的登录令牌")),
                  "★ T19.10 离线这一趟**没有**那句令牌警告（那一句是给在线身份看的）");
            check(scriptText.contains(QStringLiteral("cd /d ")) &&
                      scriptText.contains(QDir::toNativeSeparators(root.path())),
                  "T19.10 有 cd /d <数据目录> —— classpath 那几段是**相对它**写的"
                  "（脚本不在那儿跑就找不着 jar）",
                  scriptText);
            check(scriptText.contains(QStringLiteral("--accessToken 0")) &&
                      scriptText.contains(QStringLiteral("--username %1").arg(accountName)),
                  "★ T19.10 离线身份那两条：--accessToken 0 / --username 就是那个账号名",
                  scriptText);
            check(scriptText.contains(QStringLiteral("-cp")) &&
                      scriptText.contains(QStringLiteral("net.minecraft.client.main.Main")),
                  "T19.10 classpath 与主类都在（和启动器真正启动那一次同源）");
            check(scriptText.count(QStringLiteral("\r\n")) == 4,
                  "T19.10 整份脚本就 4 行：@echo off / rem / cd / 命令行一条（都在一行上）",
                  QString::number(scriptText.count(QStringLiteral("\r\n"))));

            // 再导一遍：同一个落点、逐字节一样（QSaveFile 覆盖写，脚本本身没有随机成分）。
            {
                PromptAnswerer answerer5b({0});
                answerer5b.start();
                QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                          Q_ARG(QString, idA), Q_ARG(QString, scriptPath));
                pumpUntil([&answerer5b] { return !answerer5b.answered.isEmpty(); }, 10000);
                answerer5b.stop();
                check(readBytesAt(scriptPath) == scriptBytes,
                      "★ T19.10 再导一遍逐字节一样（脚本里没有任何随机成分，"
                      "覆盖写也不该留下半截）");
            }

            // ---------- ⑦ 老 meta 里的 autoMemory 不许劫持导出的 -Xmx ----------
            // 「自动（按系统内存）」那一档从界面上撤掉了（要求「内存分配
            // 和最大内存这玩意完全就是重复的，只保留那个拖动的」）⇒ 读侧一律手动档。
            // 这一节钉的正是那件事：meta 里同时写着 autoMemory:true 和一对具体的
            // xmx/xms 时，导出的命令行必须用**那对数**，而不是按系统内存（还带模组加成）
            // 现算一个 —— 老实例的 meta 里那个键到处都是，认它的结果就是"用户拖出来的数
            // 每次导出都被换掉"。
            {
                const QString idC = QStringLiteral("1.99.3-modpack");
                installFake(idC, QStringLiteral("T19模组包"));
                const int pinnedXmx = 3072;
                const int pinnedXms = 1024;
                writeFileAt(versionsDir.filePath(idC + QStringLiteral("/wtfml.meta")),
                            QByteArrayLiteral("{\"name\":\"T19模组包\",\"autoMemory\":true,"
                                              "\"xmx\":3072,\"xms\":1024}"));
                // 摆 200 个模组（外加一个禁用的反例）：要是导出还在走自动档那一支，它就会
                // 数出 200 来加成，得到的数几乎必然不是 3072。
                const int mods = 200;
                const QDir modsDir(versionsDir.filePath(idC + QStringLiteral("/mods")));
                modsDir.mkpath(QStringLiteral("."));
                for (int i = 0; i < mods; ++i) {
                    writeFileAt(modsDir.filePath(QStringLiteral("m%1.jar").arg(i)),
                                QByteArrayLiteral("x"));
                }
                writeFileAt(modsDir.filePath(QStringLiteral("off.jar.disabled")),
                            QByteArrayLiteral("x"));

                const QString scriptPathC = root.filePath(QStringLiteral("out/start-modpack.bat"));
                {
                    PromptAnswerer answerer7({0});
                    answerer7.start();
                    QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                              Q_ARG(QString, idC), Q_ARG(QString, scriptPathC));
                    pumpUntil([&answerer7] { return !answerer7.answered.isEmpty(); }, 10000);
                    answerer7.stop();
                }
                const QString scriptTextC = QString::fromLocal8Bit(readBytesAt(scriptPathC));
                check(scriptTextC.contains(QStringLiteral("-Xmx%1m").arg(pinnedXmx)) &&
                          scriptTextC.contains(QStringLiteral("-Xms%1m").arg(pinnedXms)),
                      "★ T19.14 meta 里的 autoMemory:true 被忽略，导出的就是那一对 xmx/xms",
                      scriptTextC);
                const SystemMemory::Usage usage = SystemMemory::read();
                const int autoWouldBe =
                    GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb, mods);
                if (autoWouldBe != pinnedXmx) {
                    check(!scriptTextC.contains(QStringLiteral("-Xmx%1m").arg(autoWouldBe)),
                          "★ T19.14 也不是自动档按系统内存 + 模组数现算的那一版",
                          scriptTextC);
                }
            }

            // ---------- ⑦ 读不出 json：不导 ----------
            {
                const QString missingPath = root.filePath(QStringLiteral("out/missing.bat"));
                PromptAnswerer answerer6({0});
                answerer6.start();
                QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                          Q_ARG(QString, QStringLiteral("1.99.9-nosuch")),
                                          Q_ARG(QString, missingPath));
                const bool shown =
                    pumpUntil([&answerer6] { return !answerer6.answered.isEmpty(); }, 10000);
                answerer6.stop();
                check(shown && !answerer6.answered.isEmpty() &&
                          answerer6.answered.at(0).title == QStringLiteral("导出失败"),
                      "T19.11 找不到 version.json ⇒ 「导出失败」",
                      answerer6.answered.isEmpty() ? QStringLiteral("没弹")
                                                   : answerer6.answered.at(0).title);
                check(!answerer6.answered.isEmpty() &&
                          answerer6.answered.at(0).text.contains(QStringLiteral("找不到版本")),
                      "T19.11 正文说清是版本信息拿不到",
                      answerer6.answered.isEmpty() ? QString() : answerer6.answered.at(0).text);
                check(!QFile::exists(missingPath),
                      "★ T19.11 **一份都没写出去**（半截的 .bat 双击起来行为是随机的，"
                      "而用户会以为「导出来的就是用不了的」）");
            }

            // ---------- ⑧ 在线身份但拿不到令牌：也不导 ----------
            // logout() 只清活动槽那三个键（账号还留在列表里），于是 offlineIdentity() 变假、
            // 而盘上确实没有 refresh_token ⇒ loadSavedToken() 在**本地**就失败（不出网，
            // 正是这一节要的）。
            check(AccountStore::logout(),
                  "T19.12 退掉当前账号（下面那一档要的是「在线身份拿不到令牌」）");
            TokenStore::clear(TokenStore::microsoftKey());
            TokenStore::clear(TokenStore::minecraftKey());
            const QString onlinePath = root.filePath(QStringLiteral("out/online.bat"));
            {
                PromptAnswerer answerer7({0});
                answerer7.start();
                QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                          Q_ARG(QString, idA), Q_ARG(QString, onlinePath));
                const bool shown =
                    pumpUntil([&answerer7] { return !answerer7.answered.isEmpty(); }, 10000);
                answerer7.stop();
                check(shown && !answerer7.answered.isEmpty() &&
                          answerer7.answered.at(0).title == QStringLiteral("导出失败"),
                      "T19.12 在线身份但拿不到令牌 ⇒ 「导出失败」",
                      answerer7.answered.isEmpty() ? QStringLiteral("没弹")
                                                   : answerer7.answered.at(0).title);
                check(!answerer7.answered.isEmpty() &&
                          answerer7.answered.at(0).text.contains(
                              QStringLiteral("要先能拿到登录令牌")),
                      "★ T19.12 而且点明了是**令牌**的问题（导一份没有令牌的脚本 = 导一份"
                      "起不来的脚本，所以宁可一份都不导）",
                      answerer7.answered.isEmpty() ? QString() : answerer7.answered.at(0).text);
                check(!QFile::exists(onlinePath), "★ T19.12 一份都没写出去");
            }
            {
                const QString secret = QStringLiteral("__ONLINE_SCRIPT_TEST_123456789__");
                const QJsonObject token{{QStringLiteral("access_token"), secret},
                                        {QStringLiteral("expires_at"), QStringLiteral("2999-01-01T00:00:00Z")},
                                        {QStringLiteral("name"), QStringLiteral("ScriptTest")},
                                        {QStringLiteral("uuid"), QStringLiteral("12345678901234567890123456789012")}};
                check(TokenStore::writeJson(TokenStore::minecraftKey(), token),
                      "写入隔离的在线脚本测试身份");
                PromptAnswerer answerer({0});
                answerer.start();
                QMetaObject::invokeMethod(&w, "exportLaunchScript", Qt::DirectConnection,
                                          Q_ARG(QString, idA), Q_ARG(QString, onlinePath));
                pumpUntil([&] { return !answerer.answered.isEmpty(); }, 10000);
                answerer.stop();
                QFile exported(onlinePath);
                const bool opened = exported.open(QIODevice::ReadOnly);
                const QString body = opened ? QString::fromUtf8(exported.readAll()) : QString();
                check(opened && !body.contains(secret) &&
                          body.contains(QStringLiteral("%WTFML_MC_ACCESS_TOKEN%")) &&
                          body.contains(QStringLiteral("if not defined WTFML_MC_ACCESS_TOKEN")) &&
                          !body.contains(QStringLiteral("__WTFML_RUNTIME_MC_TOKEN_7E41__")),
                      "真实在线脚本导出不落盘令牌并提供缺失凭据门禁");
            }
        }
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T19.13 这一节加的账号清干净了（后面不该知道它存在过）");

    LauncherSettings().setLastLaunchedVersion(lastVersionBefore);
    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T19.13 数据根目录换回来了");
}

