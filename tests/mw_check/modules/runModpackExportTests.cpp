#include "Support.hpp"
void runModpackExportTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T21.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    FakeLookupApi api;
    check(api.start(), "T21.0 假接口服务器起来了");

    // 数据根目录**两头都改**（理由见 T19/T20：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次）。两个接口地址也从设置键推下去 —— 这正是被验的那段接线。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const QVariant sourceBefore = sandbox.value(QStringLiteral("Download/Source"));
    const QVariant modrinthBefore = sandbox.value(QStringLiteral("Launcher/ModrinthApiBase"));
    const QVariant curseForgeBefore = sandbox.value(QStringLiteral("Launcher/CurseForgeApiBase"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.setValue(QStringLiteral("Download/Source"),
                     LauncherSettings::downloadSourceKey(DownloadSource::Official));
    sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), api.modrinthBase());
    sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), api.curseForgeBase());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));

    // ---------- 夹具 ----------
    //
    // 一条**真的继承链**：实例 → fabric-loader-0.16.14-1.20.1 → 1.20.1。两条链走的都是
    // rawVersionJson()（没合并过的那份），所以每一层都得有一份自己的 json。
    // 末端用**真版本号**（理由见本节开头 ②）。
    const QString idA = QStringLiteral("T21测试包");
    const QString idB = QStringLiteral("T21认不出");
    const QString idO = QStringLiteral("T21OptiFine");
    const QString idU = QStringLiteral("T21没有版本号");
    const QString idLoader = QStringLiteral("fabric-loader-0.16.14-1.20.1");
    const QString idVanilla = QStringLiteral("1.20.1");
    makeInstalledVersion(versionsDir, idA);
    makeInstalledVersion(versionsDir, idB);
    makeInstalledVersion(versionsDir, idO);
    makeInstalledVersion(versionsDir, idU);
    makeInstalledVersion(versionsDir, idLoader);
    makeInstalledVersion(versionsDir, idVanilla);

    const auto thinJson = [](const QString &id, const QString &parent) {
        return QStringLiteral("{\"id\":\"%1\",\"inheritsFrom\":\"%2\",\"type\":\"release\","
                              "\"mainClass\":\"net.minecraft.client.main.Main\","
                              "\"libraries\":[],\"arguments\":{\"game\":[],\"jvm\":[]}}")
            .arg(id, parent)
            .toUtf8();
    };
    const auto vanillaJson = [](const QString &id) {
        return QStringLiteral("{\"id\":\"%1\",\"type\":\"release\","
                              "\"mainClass\":\"net.minecraft.client.main.Main\","
                              "\"libraries\":[],\"arguments\":{\"game\":[],\"jvm\":[]},"
                              "\"assets\":\"%1\"}")
            .arg(id)
            .toUtf8();
    };
    writeFileAt(versionsDir.filePath(idA + QStringLiteral("/") + idA + QStringLiteral(".json")),
                thinJson(idA, idLoader));
    writeFileAt(versionsDir.filePath(idB + QStringLiteral("/") + idB + QStringLiteral(".json")),
                thinJson(idB, idVanilla));
    writeFileAt(versionsDir.filePath(idLoader + QStringLiteral("/") + idLoader +
                                     QStringLiteral(".json")),
                thinJson(idLoader, idVanilla));
    writeFileAt(versionsDir.filePath(idVanilla + QStringLiteral("/") + idVanilla +
                                     QStringLiteral(".json")),
                vanillaJson(idVanilla));
    // idO：链上什么都没有，是 **wtfml.meta** 里记着 OptiFine（T20 用的也是这一档）。
    // realVersion 一起给上，好让"读不出原版"那道闸**不会**先把它拦下来（先拦了就看不出
    // 是哪一个问句了）。
    makeVersionMeta(versionsDir, idO,
                    QByteArrayLiteral("{\"loaderName\":\"OptiFine\",\"loaderVersion\":\"H9\","
                                      "\"realVersion\":\"1.20.1\"}"));
    // idU：链上没有任何加载器痕迹，目录名也不像版本号 ⇒ gameVersion 读不出来。

    // 实例 idA 里那些东西：**能认出来的一个、认不出来的两个、配置一个、外加两个必须
    // 留在门外**（存档与日志）。
    const QString dirA = AppPaths::versionDir(idA);
    const QByteArray bytesKnown = QByteArrayLiteral("T21 known mod bytes");
    const QByteArray bytesUnknown = QByteArrayLiteral("T21 unknown mod bytes 你好");
    const QByteArray bytesPack = QByteArrayLiteral("T21 resource pack bytes");
    writeFileAt(dirA + QStringLiteral("/mods/known.jar"), bytesKnown);
    writeFileAt(dirA + QStringLiteral("/mods/unknown.jar"), bytesUnknown);
    writeFileAt(dirA + QStringLiteral("/resourcepacks/材质.zip"), bytesPack);
    writeFileAt(dirA + QStringLiteral("/config/t21.toml"), QByteArrayLiteral("t21 = true"));
    writeFileAt(dirA + QStringLiteral("/saves/world/level.dat"), QByteArrayLiteral("level"));
    writeFileAt(dirA + QStringLiteral("/logs/latest.log"), QByteArrayLiteral("log"));
    // 自定义名：清单里那一格（`spec.name`）要的是**界面上那个显示名**，不是目录名。
    makeVersionMeta(versionsDir, idA, QByteArrayLiteral("{\"name\":\"T21 我的包\"}"));

    const QString dirB = AppPaths::versionDir(idB);
    writeFileAt(dirB + QStringLiteral("/mods/nobody.jar"), QByteArrayLiteral("nothing known"));

    // idP：**PCL 那种"整份 json 都合进实例目录"的实例** —— 真机上绝大多数实例都是这个形状，
    // 用户撞的那句「导出失败：读不出 海波篮子 基于哪个原版」就是它。它的目录名
    // 不像版本号、那份 json 里也没有 inheritsFrom，所以"基于哪个原版"和"装的什么加载器"
    // **只有那份 json 自己说得出来**（clientVersion + libraries）。这条夹具的作用就是让
    // 这一点一路验到清单上。
    // clientVersion 故意挑一个**和上面那条链末端不一样的真版本号**（1.18.2 vs 1.20.1）：
    // 这样"清单里那一格是从这份 json 自己读出来的"才有分辨力 —— 编一个 `1.98.7` 出来的话
    // 它在两种模式下都过不了闸（见本节开头 ②），而要是哪天代码错拿了别处那一格（比如
    // 顺到了 idVanilla），两个不同的真版本号当场就对不上。
    const QString idP = QStringLiteral("T21海波篮子");
    makeInstalledVersion(versionsDir, idP);
    writeFileAt(versionsDir.filePath(idP + QStringLiteral("/") + idP + QStringLiteral(".json")),
                QByteArrayLiteral("{\"id\":\"T21海波篮子\",\"clientVersion\":\"1.18.2\","
                                  "\"type\":\"release\","
                                  "\"mainClass\":\"net.fabricmc.loader.impl.launch.knot.KnotClient\","
                                  "\"libraries\":[{\"name\":\"net.fabricmc:fabric-loader:0.16.14\"},"
                                  "{\"name\":\"org.ow2.asm:asm:9.7\"}],"
                                  "\"arguments\":{\"game\":[],\"jvm\":[]},\"assets\":\"1.18.2\"}"));
    makeVersionMeta(versionsDir, idP, QByteArrayLiteral("{\"name\":\"T21 海波篮子\"}"));
    const QString dirP = AppPaths::versionDir(idP);
    writeFileAt(dirP + QStringLiteral("/mods/known.jar"), bytesKnown);
    writeFileAt(dirP + QStringLiteral("/config/t21p.toml"), QByteArrayLiteral("t21p = true"));

    const auto sha1Of = [](const QByteArray &bytes) {
        return QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
    };
    const auto sha512Of = [](const QByteArray &bytes) {
        return QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha512).toHex());
    };
    const QString sha1Known = sha1Of(bytesKnown);
    const quint32 fingerprintKnown = ModpackExport::curseForgeFingerprintOfBytes(bytesKnown);

    // Modrinth 那一发的答复：**只认 known.jar 一个**，另外两个不在 map 里（接口认不出来
    // 就是"不在 map 里"，不是报错）。
    {
        QJsonObject hashes;
        hashes.insert(QStringLiteral("sha1"), sha1Known);
        hashes.insert(QStringLiteral("sha512"), sha512Of(bytesKnown));
        QJsonObject file;
        file.insert(QStringLiteral("hashes"), hashes);
        file.insert(QStringLiteral("url"),
                    QStringLiteral("https://cdn.example.invalid/data/T21/known.jar"));
        file.insert(QStringLiteral("filename"), QStringLiteral("known.jar"));
        file.insert(QStringLiteral("size"), static_cast<double>(bytesKnown.size()));
        QJsonObject version;
        version.insert(QStringLiteral("project_id"), QStringLiteral("T21Proj"));
        version.insert(QStringLiteral("version_number"), QStringLiteral("1.0.0"));
        // `client_only` ⇒ 清单里该多出一段 `env`（那一对的唯一出处是 applyEnvironment）。
        version.insert(QStringLiteral("environment"), QStringLiteral("client_only"));
        version.insert(QStringLiteral("files"), QJsonArray{file});
        QJsonObject reply;
        reply.insert(sha1Known, version);
        api.modrinthReply = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    }
    // CurseForge 那一发的答复：键是**我们自己算出来的那个指纹**（接口原样回给我们）。
    {
        QJsonObject hash;
        hash.insert(QStringLiteral("algo"), 1);
        hash.insert(QStringLiteral("value"), sha1Known);
        QJsonObject file;
        file.insert(QStringLiteral("modId"), 4242);
        file.insert(QStringLiteral("id"), 777);
        file.insert(QStringLiteral("isAvailable"), true);
        file.insert(QStringLiteral("displayName"), QStringLiteral("T21 认得出的模组"));
        file.insert(QStringLiteral("fileName"), QStringLiteral("known.jar"));
        file.insert(QStringLiteral("downloadUrl"),
                    QStringLiteral("https://edge.example.invalid/files/777/known.jar"));
        file.insert(QStringLiteral("fileLength"), static_cast<double>(bytesKnown.size()));
        file.insert(QStringLiteral("fileFingerprint"), static_cast<double>(fingerprintKnown));
        file.insert(QStringLiteral("hashes"), QJsonArray{hash});
        QJsonObject match;
        match.insert(QStringLiteral("id"), 4242);
        match.insert(QStringLiteral("file"), file);
        QJsonObject data;
        data.insert(QStringLiteral("exactMatches"), QJsonArray{match});
        data.insert(QStringLiteral("unmatchedFingerprints"), QJsonArray());
        QJsonObject reply;
        reply.insert(QStringLiteral("data"), data);
        api.curseForgeReply = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    }

    check(QDir(dirA).exists() && QFile::exists(dirA + QStringLiteral("/saves/world/level.dat")),
          "T21.0 夹具摆好了（一条真继承链 + 一个能认出来的模组 + 存档与日志各一份）");

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        // 产品里那个显示名（清单里 `name` 那一格该写的东西）。**从这里取，不在测试里
        // 自己拼**：拼一份就等于把"用的是显示名"这条不变量的期望值写死在测试里了。
        const QComboBox *combo = w.ui()->getInstalledVersionCombo();
        const int atA = combo->findData(idA);
        const QString shownA = atA >= 0 ? combo->itemText(atA) : idA;

        // 跑一次导出，并照着脚本把中途弹出的提示答掉。
        const auto runExport = [&w](const QString &id, const QString &path,
                                    const QList<int> &script) {
            PromptAnswerer answerer(script);
            answerer.start();
            QMetaObject::invokeMethod(&w, "exportModpack", Qt::DirectConnection,
                                      Q_ARG(QString, id), Q_ARG(QString, path));
            // Export planning and archive writing run in worker threads. Keep
            // pumping the GUI loop until every scripted prompt (including the
            // terminal success/error message) has actually been handled.
            QElapsedTimer wait;
            wait.start();
            while (answerer.answered.size() < script.size() && wait.elapsed() < 30000) {
                QApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(2);
            }
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            answerer.stop();
            return answerer.answered;
        };
        const auto promptText = [](const QList<PromptAnswerer::Answered> &answered, int index) {
            return index < answered.size() ? answered.at(index).text : QString();
        };

        // ---------- ① .mrpack：Modrinth 那一整趟 ----------
        //
        // 这一段是这一节的主线：两发地址、哈希、清单、overrides、zip 的条目名，全部经过
        // **产品自己的代码**（MainWindow → VersionManager → HttpClient → 假服务器）。
        const QString packA = root.filePath(QStringLiteral("T21 导出.mrpack"));
        {
            const int requestsBefore = api.requests.size();
            const QList<PromptAnswerer::Answered> answered = runExport(idA, packA, {0});

            check(api.requests.size() == requestsBefore + 1 &&
                      api.lastRequest() ==
                          QStringLiteral("POST /v2/version_files HTTP/1.1"),
                  "★ T21.1 那一发真的发到了**盘上那个设置键指的地方**"
                  "（`Launcher/ModrinthApiBase` → LauncherSettings → VersionManager → 请求行）"
                  "—— 少接一段就会打到 api.modrinth.com，而那一发只会静静超时 15 秒",
                  api.lastRequest());

            // 请求体：三个候选文件全在（说明"要不要问"那张表在真实例上也走对了）。
            const QJsonObject asked =
                QJsonDocument::fromJson(api.bodies.value(api.bodies.size() - 1)).object();
            const QJsonArray askedHashes = asked.value(QStringLiteral("hashes")).toArray();
            bool askedOk = askedHashes.size() == 3;
            bool containsKnown = false;
            for (const QJsonValue &value : askedHashes) {
                containsKnown = containsKnown || value.toString() == sha1Known;
            }
            check(askedOk && containsKnown &&
                      asked.value(QStringLiteral("algorithm")).toString() ==
                          QStringLiteral("sha1"),
                  "★ T21.1 发过去的是**真算出来的 sha1**：三个候选（两个模组 + 一个资源包）"
                  " 一条不落，配置与存档那两个文件没问（那张判据表在真实例上的样子）",
                  QStringLiteral("%1 条").arg(askedHashes.size()));

            check(answered.size() == 1 && answered.at(0).title == QStringLiteral("导出完成"),
                  "T21.1 成功那一趟只弹一句「导出完成」（不多问、也不报错）",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            const QString report = promptText(answered, 0);
            check(report.contains(QDir::toNativeSeparators(packA)),
                  "T21.1 报告里写着包落在哪儿");
            check(report.contains(QStringLiteral("已识别 3 个待检查文件中的 1 个")) &&
                      report.contains(QStringLiteral("其余 3 个文件已包含在整合包中")),
                  "★ T21.1 报告里的那几个数是**照实报的**：问了 3 个、认出来 1 个、"
                  "其余 3 个（两个认不出的 + 一个配置）原样进包",
                  report.left(120));
            check(report.contains(QStringLiteral("未导出 logs、saves（共 5 个文件）")),
                  "★ T21.1 被排掉的目录照实说出来（存档和截图是用户自己的东西，"
                  "打进包里会盖掉收包那个人的），而且数的是**连目录里面一起**多少文件",
                  report);
            check(report.contains(QStringLiteral("Modrinth 规范")),
                  "T21.1 报告里说了这是哪一种规范的包");

            check(QFile::exists(packA) && !QFile::exists(packA + QStringLiteral(".part")),
                  "T21.1 盘上是成品那个名字，不留 .part");
        }

        // ---------- ② 那个包里的东西（条目名这一层）+ 清单读得回来 ----------
        {
            QByteArray manifest;
            QString readError;
            const ZipUtil::EntryStatus status = ZipUtil::readEntry(
                packA, ModpackExport::indexFileName(ModpackExport::Format::Modrinth), &manifest,
                &readError, QStringLiteral("读清单"));
            check(status == ZipUtil::EntryStatus::Ok && !manifest.isEmpty(),
                  "★ T21.2 清单在 zip 根、名字就是规范钉死的 modrinth.index.json", readError);

            const ModpackImport::Result round = ModpackImport::parseModrinthIndex(manifest);
            check(round.ok, "★ T21.2 这个包**我们自己的解析器**读得回来（= 别人能装回来）",
                  round.problem);
            if (round.ok) {
                check(round.pack.name == shownA &&
                          round.pack.name.contains(QStringLiteral("T21 我的包")),
                      "★ T21.2 清单里的名字是**界面上那个显示名**（wtfml.meta 里的自定义名），"
                      " 不是目录名 —— 目录名写进去，收包的人看到一个「T21测试包」",
                      QStringLiteral("清单=%1 界面=%2").arg(round.pack.name, shownA));
                check(round.pack.gameVersion == idVanilla,
                      "★ T21.2 gameVersion 是**顺继承链走到末端**拿到的（实例 json → "
                      "fabric-loader-… → 1.20.1）。这一格错了的后果是包看着是好的、"
                      "别人装不回来",
                      round.pack.gameVersion);
                check(round.pack.loaderKey == ModpackImport::fabricLoaderKey() &&
                          round.pack.loaderVersion == QStringLiteral("0.16.14"),
                      "★ T21.2 加载器那一行也是从**链上的目录名**反读出来的",
                      QStringLiteral("%1 %2").arg(round.pack.loaderKey, round.pack.loaderVersion));
                check(round.pack.files.size() == 1 &&
                          round.pack.files.first().path == QStringLiteral("mods/known.jar") &&
                          round.pack.files.first().sha1 == sha1Known,
                      "T21.2 认出来的那一条在清单里，路径是它在实例里的相对路径、"
                      " sha1 就是本地那个文件的",
                      QString::number(round.pack.files.size()));
            }
            // `env` 那一对：接口说 client_only 才写（那一对的唯一出处是 applyEnvironment）。
            const QJsonArray files =
                QJsonDocument::fromJson(manifest).object().value(QStringLiteral("files")).toArray();
            const QJsonObject env =
                files.isEmpty()
                    ? QJsonObject()
                    : files.first().toObject().value(QStringLiteral("env")).toObject();
            check(files.size() == 1 &&
                      env.value(QStringLiteral("client")).toString() ==
                          QStringLiteral("required") &&
                      env.value(QStringLiteral("server")).toString() ==
                          QStringLiteral("unsupported"),
                  "T21.2 接口说 client_only 时清单里多出那段 `env`（不写 = 两边都装）",
                  QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));

            QByteArray got;
            QString entryError;
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/mods/unknown.jar"), &got,
                                     &entryError,
                                     QStringLiteral("读覆盖文件")) == ZipUtil::EntryStatus::Ok &&
                      got == bytesUnknown,
                  "★ T21.2 认不出来的模组原样躺在 overrides/ 里、字节一字不差"
                  "（这是「一个都不丢」的落点）",
                  entryError);
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/resourcepacks/材质.zip"), &got,
                                     &entryError, QStringLiteral("读覆盖文件")) ==
                      ZipUtil::EntryStatus::Ok &&
                      got == bytesPack,
                  "T21.2 中文名的资源包也一样（路径那一层不许被编码搞坏）");
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/config/t21.toml"), &got,
                                     &entryError,
                                     QStringLiteral("读覆盖文件")) == ZipUtil::EntryStatus::Ok &&
                      got == QByteArrayLiteral("t21 = true"),
                  "T21.2 没问过接口的配置也照样进包");
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/saves/world/level.dat"), &got,
                                     &entryError, QStringLiteral("读存档")) ==
                      ZipUtil::EntryStatus::NotFound,
                  "★ T21.2 **存档一个字节都不在包里**（默认不进包：那是这个人的东西，"
                  "不是这个整合包的）");
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/logs/latest.log"), &got,
                                     &entryError,
                                     QStringLiteral("读日志")) == ZipUtil::EntryStatus::NotFound,
                  "★ T21.2 日志也不在");
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/wtfml.meta"), &got,
                                     &entryError, QStringLiteral("读实例设置")) ==
                      ZipUtil::EntryStatus::NotFound,
                  "★ T21.2 启动器自己的实例设置（wtfml.meta）不进包 ——"
                  " 导出去等于把上一个人的实例设置抄给下一个人");
            check(ZipUtil::readEntry(packA, QStringLiteral("overrides/T21测试包.jar"), &got,
                                     &entryError, QStringLiteral("读版本文件")) ==
                      ZipUtil::EntryStatus::NotFound,
                  "★ T21.2 这个实例自己的版本文件（<实例名>.jar）不进包 ——"
                  " 装什么由清单里那两行管，塞一份进去会和导入方自己下下来的打架");
        }

        // ---------- ③ .zip：CurseForge 那一半（格式只看扩展名） ----------
        {
            const QString packZip = root.filePath(QStringLiteral("T21 导出.zip"));
            const int requestsBefore = api.requests.size();
            const QList<PromptAnswerer::Answered> answered = runExport(idA, packZip, {0});
            check(api.requests.size() == requestsBefore + 1 &&
                      api.lastRequest() == QStringLiteral("POST /v1/fingerprints HTTP/1.1"),
                  "★ T21.3 同一个实例、只是把文件名换成了 .zip ⇒ 走的是**另一条路**"
                  "（`/v1/fingerprints`）。格式只看扩展名，没有第三个开关",
                  api.lastRequest());

            // 正文里那些指纹是**字符串**不是数字（写成数字那边回 400，而 400 的正文只有
            // 一句 error，看着和"这个包本身有问题"一模一样）。
            const QByteArray cfBody = api.bodies.value(api.bodies.size() - 1);
            const QJsonArray sent =
                QJsonDocument::fromJson(cfBody).object().value(QStringLiteral("fingerprints")).toArray();
            bool sentOk = sent.size() == 3;
            for (const QJsonValue &value : sent) {
                // **数字**，不是字符串。发字符串的话实测回 400（`invalid type: string "…"，
                // expected i64`），CurseForge 那边**一条都认不出来**、还不吭声（唯一会说话
                // 的是 400 的正文）。见 ModpackExport.cpp 里 curseForgeFingerprintsRequestBody()。
                sentOk = sentOk && value.isDouble();
            }
            check(sentOk && cfBody.contains(QByteArray::number(fingerprintKnown)),
                  "★ T21.3 指纹发的是**真正的数字**（int64，不是字符串 —— 字符串那边回 400），"
                  " 而且是我们按 murmur2 算出来的那个值",
                  QString::fromUtf8(cfBody.left(160)));

            check(answered.size() == 1 && answered.at(0).title == QStringLiteral("导出完成") &&
                      promptText(answered, 0).contains(QStringLiteral("CurseForge 规范")),
                  "T21.3 成功那一趟只弹一句，而且说的是「CurseForge 规范」",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);

            QByteArray manifest;
            QString readError;
            const ZipUtil::EntryStatus status = ZipUtil::readEntry(
                packZip, ModpackExport::indexFileName(ModpackExport::Format::CurseForge),
                &manifest, &readError, QStringLiteral("读清单"));
            check(status == ZipUtil::EntryStatus::Ok,
                  "★ T21.3 这一份包里躺的是 manifest.json（而不是 modrinth.index.json）"
                  "—— 名字和正文是一对，凑错了别人的启动器当场说不认识", readError);
            const ModpackImport::Result round = ModpackImport::parseCurseForgeManifest(manifest);
            check(round.ok, "T21.3 CF 那份清单我们自己也解析得回来", round.problem);
            if (round.ok) {
                check(round.pack.pendingRefs.size() == 1 &&
                          round.pack.pendingRefs.first().projectId == 4242 &&
                          round.pack.pendingRefs.first().fileId == 777,
                      "T21.3 认出来的那一条写成了两个号（projectID + fileID）——"
                      " CF 的清单里没有地址、没有文件名，只有这两个号",
                      QString::number(round.pack.pendingRefs.size()));
                check(round.pack.gameVersion == idVanilla &&
                          round.pack.loaderKey == ModpackImport::fabricLoaderKey() &&
                          round.pack.loaderVersion == QStringLiteral("0.16.14"),
                      "T21.3 CF 那份的 minecraft.version 与 modLoaders 也是同一条链上来的",
                      QStringLiteral("%1 %2").arg(round.pack.gameVersion, round.pack.loaderKey));
            }
            QByteArray got;
            QString entryError;
            check(ZipUtil::readEntry(packZip, QStringLiteral("overrides/mods/unknown.jar"), &got,
                                     &entryError,
                                     QStringLiteral("读覆盖文件")) == ZipUtil::EntryStatus::Ok &&
                      got == bytesUnknown,
                  "T21.3 认不出来的那一个同样进 overrides（两种格式的覆盖层都叫 overrides）");
        }

        // ---------- ④ 反查没成：先问一句，回答决定了盘上有没有东西 ----------
        //
        // 这是 PCL 那个「联网获取部分文件信息失败，是否继续导出？」的形状。两条岔路都要走：
        // 取消 ⇒ **盘上什么都不留**；继续 ⇒ 一个挂在 overrides 上、断网也做得出来的包。
        api.modrinthStatus = 500;
        {
            const QString cancelled = root.filePath(QStringLiteral("T21 取消的.mrpack"));
            const QList<PromptAnswerer::Answered> answered = runExport(idA, cancelled, {1});
            check(answered.size() == 1 && answered.at(0).title == QStringLiteral("继续导出吗"),
                  "★ T21.4 接口挂了（HTTP 500）时**先问一句**，不是默默导出一个全是 overrides 的包"
                  "（也不是当成错误直接停下 —— 断网照样能出包）",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            check(promptText(answered, 0).contains(QStringLiteral("HTTP 500")),
                  "★ T21.4 那句话里带着接口回了什么（「没连上」和「接口挂了」是两件事，"
                  "用户据此决定要不要开代理再试一次）",
                  promptText(answered, 0));
            check(answered.at(0).buttons.size() == 2 &&
                      answered.at(0).buttons.at(0) == QStringLiteral("继续导出") &&
                      answered.at(0).buttons.at(1) == QStringLiteral("取消"),
                  "T21.4 两颗按钮的字与顺序（继续导出在前）", answered.at(0).buttons.join('/'));
            check(!QFile::exists(cancelled) && !QFile::exists(cancelled + QStringLiteral(".part")),
                  "★ T21.4 答「取消」⇒ 盘上**连 .part 都不留**（留半个包比不导出更坏："
                  "用户会以为做出来了）");

            const QString continued = root.filePath(QStringLiteral("T21 继续的.mrpack"));
            const QList<PromptAnswerer::Answered> answered2 = runExport(idA, continued, {0, 0});
            check(answered2.size() == 2 &&
                      answered2.at(1).title == QStringLiteral("导出完成"),
                  "★ T21.4 答「继续导出」⇒ 接着做完，末尾照旧报一句完成",
                  QStringLiteral("%1 条提示").arg(answered2.size()));
            QByteArray manifest;
            QString readError;
            check(ZipUtil::readEntry(continued,
                                     ModpackExport::indexFileName(ModpackExport::Format::Modrinth),
                                     &manifest, &readError, QStringLiteral("读清单")) ==
                      ZipUtil::EntryStatus::Ok,
                  "T21.4 这个包也真打出来了", readError);
            QByteArray got;
            QString entryError;
            check(ZipUtil::readEntry(continued, QStringLiteral("overrides/mods/known.jar"), &got,
                                     &entryError, QStringLiteral("读覆盖文件")) ==
                      ZipUtil::EntryStatus::Ok &&
                      got == bytesKnown,
                  "★ T21.4 反查没成时**认不出来的那些照样进 overrides**（一个文件都不丢）："
                  "这一份包不需要任何接口就能装回去");
        }
        api.modrinthStatus = 200;

        // ---------- ⑤ 一个资源都没认出来：换一句话问，但不许默默往下走 ----------
        //
        // 和④不是一回事：接口**成了**，只是这个实例里一个能认出来的都没有（老包、自己改过
        // 的模组）。这种包我们自己都装不回来（解析器拒收空 files），所以必须让用户知情。
        {
            const QString packB = root.filePath(QStringLiteral("T21 认不出.mrpack"));
            const QList<PromptAnswerer::Answered> answered = runExport(idB, packB, {0, 0});
            check(answered.size() == 2 && answered.at(0).title == QStringLiteral("继续导出吗"),
                  "★ T21.5 一个都没认出来时也问一句（不是错误弹窗：包是能用的，"
                  "只是启动器不能替收包的人单独更新或者去重）",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            check(promptText(answered, 0).contains(QStringLiteral("未识别可由平台下载的资源")) &&
                      !promptText(answered, 0).contains(QStringLiteral("HTTP")),
                  "★ T21.5 说的是**「一个资源都没认出来」**，不是「接口没连上」 ——"
                  " 这两句话对应的是完全不同的两件事（一个是网络、一个是这个包自己）",
                  promptText(answered, 0));
            check(promptText(answered, 1).contains(QStringLiteral("已识别 1 个待检查文件中的 0 个")),
                  "T21.5 报告里如实写着认出来 0 个", promptText(answered, 1).left(80));
            QByteArray got;
            QString entryError;
            check(QFile::exists(packB) &&
                      ZipUtil::readEntry(packB, QStringLiteral("overrides/mods/nobody.jar"), &got,
                                         &entryError,
                                         QStringLiteral("读覆盖文件")) ==
                          ZipUtil::EntryStatus::Ok &&
                      got == QByteArrayLiteral("nothing known"),
                  "★ T21.5 答「继续导出」⇒ 一个模组都不丢（这个实例是纯原版的包："
                  "链上没有任何加载器痕迹 ⇒ loaderKey 空 ⇒ 清单里不写依赖那一行）");
        }

        // ---------- ⑥ 两道门口的闸：拦住了就一个字都不许写 ----------
        {
            const QString packO = root.filePath(QStringLiteral("T21 OptiFine.mrpack"));
            const int requestsBeforeO = api.requests.size();
            const QList<PromptAnswerer::Answered> answered = runExport(idO, packO, {0});
            check(answered.size() == 1 &&
                      answered.at(0).title == QStringLiteral("此实例不符合整合包导出要求"),
                  "★ T21.6 OptiFine 实例**认得出、写不下去**（两种规范的依赖表里都没有它那一格）"
                  "⇒ 如实报一句，而不是照原版导出一个「进去全是没光影的模组」的包",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            check(promptText(answered, 0).contains(QStringLiteral("OptiFine")),
                  "T21.6 那句话里点名 OptiFine（用户据此才知道该怎么办）", promptText(answered, 0));
            check(!QFile::exists(packO) && !QFile::exists(packO + QStringLiteral(".part")),
                  "★ T21.6 拦下来时盘上什么都不留");
            check(api.requests.size() == requestsBeforeO,
                  "★ T21.6 而且**一发接口都没发**（这道闸在算哈希之前 —— "
                  "一个几百兆的实例不该为了一个注定要拒绝的结果读一遍盘）",
                  QStringLiteral("这一趟发了 %1 发").arg(api.requests.size() - requestsBeforeO));

            const QString packU = root.filePath(QStringLiteral("T21 没版本号.mrpack"));
            const QList<PromptAnswerer::Answered> answered2 = runExport(idU, packU, {0});
            check(answered2.size() == 1 &&
                      answered2.at(0).title == QStringLiteral("无法识别原版游戏版本"),
                  "★ T21.6 连「基于哪个原版」都读不出来时也拦下来（清单里那一格是必需的，"
                  "编一个数字出来等于给收包的人一个假版本号）",
                  answered2.isEmpty() ? QStringLiteral("没弹") : answered2.at(0).title);
            check(!QFile::exists(packU) && !QFile::exists(packU + QStringLiteral(".part")),
                  "T21.6 这一道也一样：盘上什么都不留");
        }

        // ---------- ⑦ 实例目录都不在的时候 ----------
        {
            const QString packMissing = root.filePath(QStringLiteral("T21 不存在的.mrpack"));
            const QList<PromptAnswerer::Answered> answered =
                runExport(QStringLiteral("T21根本不存在"), packMissing, {0});
            check(answered.size() == 1 && answered.at(0).title == QStringLiteral("导出失败"),
                  "T21.7 目录不在时说「导出失败」（这一档是「盘上出了事」，"
                  "和上面那两句「这个实例本身导不了」不是一回事）",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            check(!QFile::exists(packMissing),
                  "T21.7 同样不写盘");
        }

        // ---------- ⑧ PCL 那种合并过的实例：两格都得从 json 自己身上读出来 ----------
        //
        // 用户那句「导出失败：读不出 海波篮子 基于哪个原版」在整条路上的回访。这一档在
        // 数据层认出来还不够 —— 数据层认出来了、清单上写的是别的（或者干脆是"纯原版"），
        // 包照样是坏的，所以这里一路验到**清单那两格**上。
        {
            const QString packP = root.filePath(QStringLiteral("T21 海波篮子.mrpack"));
            const int requestsBeforeP = api.requests.size();
            const QList<PromptAnswerer::Answered> answered = runExport(idP, packP, {0});
            check(answered.size() == 1 && answered.at(0).title == QStringLiteral("导出完成"),
                  "★ T21.8 PCL 那种合并过的实例**导得出去**（以前这一档直接停在"
                  "「读不出…基于哪个原版」上，一个字都不写）",
                  answered.isEmpty() ? QStringLiteral("没弹") : answered.at(0).title);
            check(api.requests.size() == requestsBeforeP + 1,
                  "★ T21.8 而且真的走到了反查那一发（本地那两格都读出来了才会往下走）",
                  QStringLiteral("发了 %1 发").arg(api.requests.size() - requestsBeforeP));

            QByteArray manifestP;
            QString readErrorP;
            const ZipUtil::EntryStatus statusP = ZipUtil::readEntry(
                packP, ModpackExport::indexFileName(ModpackExport::Format::Modrinth), &manifestP,
                &readErrorP, QStringLiteral("读清单"));
            check(statusP == ZipUtil::EntryStatus::Ok && !manifestP.isEmpty(),
                  "★ T21.8 清单在 zip 根、读得回来", readErrorP);
            const ModpackImport::Result roundP = ModpackImport::parseModrinthIndex(manifestP);
            check(roundP.ok, "★ T21.8 这个包我们自己的解析器也读得回来（= 别人装得回来）",
                  roundP.problem);
            if (roundP.ok) {
                check(roundP.pack.gameVersion == QStringLiteral("1.18.2"),
                      "★★ T21.8 gameVersion 是从那份 json 自己的 clientVersion 读出来的"
                      "（1.18.2 这一格没有别的来源：目录名不像版本号、链上一步都走不动；"
                      "而且它**故意不等于**上面链末端那个 1.20.1 —— 拿错了哪一格当场对不上）",
                      roundP.pack.gameVersion);
                check(roundP.pack.loaderKey == ModpackImport::fabricLoaderKey() &&
                          roundP.pack.loaderVersion == QStringLiteral("0.16.14"),
                      "★★ T21.8 加载器那一行是从 libraries 认出来的（fabric 0.16.14）——"
                      "认不出来的话这里会是一条**纯原版**包：装着 133 个模组的实例导出去，"
                      "收包的人模组都在、游戏起不来，而包看上去是好的",
                      QStringLiteral("%1 %2").arg(roundP.pack.loaderKey, roundP.pack.loaderVersion));
                check(roundP.pack.name.contains(QStringLiteral("T21 海波篮子")),
                      "T21.8 名字照旧取显示名（那条路没被这一改带走）", roundP.pack.name);
            }
        }
    }

    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
    sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), modrinthBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), curseForgeBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T21.9 数据根目录换回来了");
}

