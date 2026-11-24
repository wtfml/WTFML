#include "Support.hpp"

int runVersionManagerBatch(QCoreApplication &app, const QString &sandbox) {
    const QString versionId = QStringLiteral("synth-1");
    const QString work = sandbox + "/synth-vm";
    QDir(work).removeRecursively();
    const QString remoteDir = work + "/remote";
    const QString base = sandbox + "/versions/" + versionId;

    const synth::RemoteFile jar = synth::makeRemote(remoteDir, QStringLiteral("client.jar"),
                                                   QByteArray("vm-client-jar-body"));
    // A valid, already-cached object makes this fixture independent of the pipeline test.
    // The old manual probe used an empty index, rejected by production validation.
    // A registered test must supply its own valid index instead of relying on cache state.
    const QByteArray assetBody("vm-cached-asset");
    const QString assetHash = synth::sha1Of(assetBody);
    writeFileAt(sandbox + "/assets/objects/" + assetHash.left(2) + "/" + assetHash, assetBody);
    const QJsonObject objects{{QStringLiteral("minecraft/vm-fixture"),
                              QJsonObject{{QStringLiteral("hash"), assetHash},
                                          {QStringLiteral("size"), assetBody.size()}}}};
    const synth::RemoteFile index = synth::makeRemote(
        remoteDir, QStringLiteral("indexes/synthidx.json"),
        QJsonDocument(QJsonObject{{QStringLiteral("objects"), objects}}).toJson());

    QJsonArray libArr;
    for (int i = 0; i < 2; ++i) {
        const synth::RemoteFile lib = synth::makeRemote(
            remoteDir, QStringLiteral("libs/vmlib%1.jar").arg(i),
            QStringLiteral("vm-lib-body-%1").arg(i).toUtf8());
        libArr.append(QJsonObject{
            {QStringLiteral("name"), QStringLiteral("synth:vmlib%1:1.0").arg(i)},
            {QStringLiteral("downloads"),
             QJsonObject{{QStringLiteral("artifact"),
                          QJsonObject{{QStringLiteral("url"), lib.url},
                                      {QStringLiteral("sha1"), lib.sha1},
                                      {QStringLiteral("size"), lib.size},
                                      {QStringLiteral("path"),
                                       QStringLiteral("synth/vmlib%1/1.0/vmlib%1-1.0.jar")
                                           .arg(i)}}}}}});
    }
    const QJsonObject versionJson{
        {QStringLiteral("id"), versionId},
        {QStringLiteral("libraries"), libArr},
        {QStringLiteral("downloads"),
         QJsonObject{{QStringLiteral("client"),
                      QJsonObject{{QStringLiteral("url"), jar.url},
                                  {QStringLiteral("sha1"), jar.sha1},
                                  {QStringLiteral("size"), jar.size}}}}},
        {QStringLiteral("assetIndex"),
         QJsonObject{{QStringLiteral("id"), QStringLiteral("synthidx")},
                     {QStringLiteral("url"), index.url},
                     {QStringLiteral("sha1"), index.sha1},
                     {QStringLiteral("size"), index.size}}}};

    // 落盘，让 getVersionJson() 能从磁盘回读进缓存（downloadVersion 的前置条件）
    QDir().mkpath(base);
    QFile jsonFile(base + "/" + versionId + ".json");
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out() << "无法写入 " << jsonFile.fileName() << "\n";
        return 2;
    }
    jsonFile.write(QJsonDocument(versionJson).toJson(QJsonDocument::Indented));
    jsonFile.close();

    VersionManager vm;
    int emissions = 0;
    bool lastSuccess = false;
    QString lastMessage;
    bool finished = false;
    int statsSignals = 0;
    int lastDone = 0;
    int lastTotal = 0;
    QEventLoop loop;
    QObject::connect(&vm, &VersionManager::versionDownloadFinished, &app,
                     [&](const QString &id, bool ok, const QString &msg) {
                         ++emissions;
                         lastSuccess = ok;
                         lastMessage = msg;
                         finished = true;
                         out() << "        versionDownloadFinished: id=" << id
                               << " success=" << ok << "\n";
                         loop.quit();
                     });
    QObject::connect(&vm, &VersionManager::versionDownloadStats, &app,
                     [&](int done, int total, int, const QString &) {
                         ++statsSignals;
                         lastDone = done;
                         lastTotal = total;
                     });

    section("G. VersionManager 批次编排");
    const QJsonObject loaded = vm.getVersionJson(versionId);
    check(!loaded.isEmpty(), "G0 版本 JSON 从磁盘回读成功（缓存已填充）");
    check(!vm.isDownloading(), "G0 下载前 isDownloading() 为假");

    const QString blockedDestination = work + "/blocked-destination";
    writeFileAt(blockedDestination, QByteArray("keep"));
    vm.downloadVersion(versionId, blockedDestination);
    check(emissions == 1 && !lastSuccess && !lastMessage.isEmpty(),
          "G0 JSON 保存失败立即给出一个失败终态");
    check(!vm.isDownloading() && !vm.downloads()->isBusy() && statsSignals == 0,
          "G0 JSON 保存失败不开下载批次、不发进度");
    check(!QFileInfo::exists(blockedDestination + "/" + versionId + ".jar"),
          "G0 JSON 保存失败不下载客户端文件");
    emissions = 0;
    finished = false;

    vm.downloadVersion(versionId, base);
    check(vm.isDownloading(), "G1 已开始下载：isDownloading() 为真（UI 据此禁用启动）");
    if (!finished) {
        loop.exec();
    }
    out() << "        第一批: emissions=" << emissions << " statsSignals=" << statsSignals
          << " done/total=" << lastDone << "/" << lastTotal << "\n";
    check(emissions == 1, "G1 冷批次: versionDownloadFinished 恰好 1 次（不是每个文件一次）",
          QString::number(emissions));
    check(lastSuccess, "G1 冷批次: success == true");
    check(statsSignals > 0, "G1 冷批次: 进度通过 versionDownloadStats 发出（不再进日志）",
          QString::number(statsSignals));
    check(lastDone == lastTotal && lastTotal == 3,
          "G1 冷批次: 分母里的 3 个作业全部完成（jar 1 + 库 2；索引不进分母）",
          QString("%1/%2").arg(lastDone).arg(lastTotal));
    check(!vm.isDownloading(), "G1 冷批次: 结束后 isDownloading() 为假");

    // 第二批：文件都在了，只剩索引要重取
    emissions = 0;
    finished = false;
    statsSignals = 0;
    vm.downloadVersion(versionId, base);
    if (!finished) {
        loop.exec();
    }
    out() << "        第二批: emissions=" << emissions << " statsSignals=" << statsSignals << "\n";
    check(emissions == 1, "G2 重复下载: 仍然恰好 1 次", QString::number(emissions));
    check(lastSuccess, "G2 重复下载: success == true");
    check(lastMessage.contains(QStringLiteral("跳过")),
          "G2 重复下载: 汇总里说明了跳过了几个文件", lastMessage);
    check(!vm.isDownloading(), "G2 重复下载: 结束后 isDownloading() 为假");

    // 前置条件不满足（缓存里没有这个版本）也必须给终态，不能让 UI 干等
    emissions = 0;
    finished = false;
    vm.downloadVersion(QStringLiteral("no-such-cached-version"), work + "/nope");
    if (!finished) {
        out() << "        前置条件不满足时没有发出终态信号\n";
    }
    check(emissions == 1 && !lastSuccess,
          "G3 没有 version.json 时: 立刻回一个失败终态（UI 不会卡在下载中）",
          QString::number(emissions));

    // --- G4：从信号里往回接续（不可以在上一批的栈里同步起）-------------------
    //
    // 批次不可以在上一批的栈里同步起：原版那一批下完 -> allDownloadsFinished -> **在上一批
    // 的栈里**同步起了"加载器自己的库"那个批次 -> extractAssetIndex() -> Qt 内部一把
    // QReadWriteLock 的 unlock() 会抛没接住的 std::system_error -> std::terminate -> abort()
    // （启动器静默消失、一句日志都没有，取证工具在 build/tools/crashdump/）。
    //
    // 所以"接着干下一件事"排到下一轮事件循环（VersionManager::deferNextStep），并给排队的
    // 那一步套一层兜底：真抛了异常就翻成失败终态，别再穿到 terminate。
    // 下面这几条钉的就是这两件事 —— 它们不依赖网络，也不会真崩（崩了就是验证进程没了）。
    section("G4. 接续排到下一轮 + 兜底不 terminate");

    int deferredRan = 0;
    vm.deferNextStep([&deferredRan] { ++deferredRan; });
    check(deferredRan == 0, "G4.1 排队的那一步不在调用栈里同步跑（重入就是这么来的）",
          QString::number(deferredRan));
    QCoreApplication::processEvents();
    check(deferredRan == 1, "G4.2 下一轮事件循环里跑到了", QString::number(deferredRan));

    emissions = 0;
    lastSuccess = true;
    lastMessage.clear();
    vm.deferNextStep([] { throw std::runtime_error("验证工程故意抛的异常"); });
    QCoreApplication::processEvents();
    check(emissions == 1 && !lastSuccess,
          "G4.3 排队那一步抛异常 -> 翻成一个失败终态（不是 terminate/abort）",
          QString("emissions=%1 success=%2").arg(emissions).arg(lastSuccess));
    check(lastMessage.contains(QStringLiteral("内部错误")),
          "G4.4 那句话里说明了是内部错误（用户看得懂、也照旧弹窗）", lastMessage);
    check(!vm.isDownloading(),
          "G4.5 兜底之后批次状态是干净的（不会永远卡在「下载中」、按钮不会永久变灰）");

    // --- G5：补全文件（VersionManager::repairVersionFiles）------------------------
    //
    // 实例设置「启动」栏那颗「补全文件」走的就是它。用户 的原话是「校验已装的，
    // 缺哪个补哪个」—— 前半句是重点：**不是"缺了才下"**，内容不对的也要换掉。
    //
    // 它和 downloadVersion() 有三处刻意不同（见 .hpp 里那段块注释），三条都在这里钉：
    // ① json 从盘上读 —— 这一节从头到尾没有网络、也没有先 fetchVersionDetails 过；
    // ② **不**写 version.json（那一步只有"下载一个版本"才该做）；
    // ③ 客户端 jar 的落点按 InstanceFiles::jarPath()：加载器实例补的是**父版本**那一份。
    //
    // 用上面那批已经下好的文件当前提（G1/G2 摆的就是 jar 1 + 库 2 + 索引 1 这 4 个作业）。
    section("G5. 补全文件：校验已装的，只补缺的和坏的");

    const QString jarPath = base + "/" + versionId + ".jar";
    const QString lib0Path = sandbox + "/libraries/synth/vmlib0/1.0/vmlib0-1.0.jar";
    const QString lib1Path = sandbox + "/libraries/synth/vmlib1/1.0/vmlib1-1.0.jar";
    const QString jsonPath = base + "/" + versionId + ".json";

    auto readBytes = [](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        return f.readAll();
    };

    check(QFile::exists(jarPath) && QFile::exists(lib0Path) && QFile::exists(lib1Path),
          "G5.0 前提：4 个文件都在盘上（前两批下的）");

    // 坏的那两类都摆上：jar 换成**字节数一样、内容不对**的一份（证明"校验"看的是 sha1 而不是
    // 光比大小），另一个库直接删掉（证明缺的会补）。
    const QByteArray realJar = readBytes(jarPath);
    QByteArray bogusJar(realJar.size(), 'x');
    if (bogusJar == realJar) {
        bogusJar[0] = 'y';  // 极端情况：真内容本来就全是 x
    }
    writeFileAt(jarPath, bogusJar);
    QFile::remove(lib0Path);
    const QByteArray lib1Before = readBytes(lib1Path);
    const QByteArray jsonBefore = readBytes(jsonPath);

    emissions = 0;
    finished = false;
    lastSuccess = false;
    lastMessage.clear();
    lastDone = 0;
    lastTotal = 0;
    vm.repairVersionFiles(versionId);
    check(vm.isDownloading(), "G5.1 补全期间 isDownloading() 为真（这个实例据此不能启动）");
    if (!finished) {
        loop.exec();
    }
    out() << "        补全第一批: emissions=" << emissions << " done/total=" << lastDone << "/"
          << lastTotal << "\n";
    check(emissions == 1 && lastSuccess, "G5.1 补全: 恰好一个成功终态",
          QString("emissions=%1 success=%2").arg(emissions).arg(lastSuccess));
    // 补全的分母只剩**缺的和坏的**（这里 = 内容不对的 jar + 被删掉的那个库），本来就好的
    // 那个库和每次都重取的索引都不在里头 —— 不是"这个实例一共有几个文件"。
    check(lastDone == 2 && lastTotal == 2,
          "★ G5.1 补全: 分母 == 缺的 + 坏的 == 2（好的那个库、索引都不进分母）",
          QString("%1/%2").arg(lastDone).arg(lastTotal));
    check(readBytes(jarPath) == realJar,
          "★ G5.2 内容不对的 jar 被换回来了（字节数一样，靠 sha1 认出来的）");
    check(QFile::exists(lib0Path) && readBytes(lib0Path) == QByteArray("vm-lib-body-0"),
          "G5.3 删掉的那个库补回来了");
    check(readBytes(lib1Path) == lib1Before,
          "G5.4 本来就是好的那个库一个字节都没动（校验通过就跳过，不是全量重写）");
    check(lastMessage.contains(QStringLiteral("跳过")),
          "G5.5 汇总里说明了有几个是已存在的", lastMessage);
    // 逐字钉住那句话的形状：两个数是**并列**的（另有），不是包含关系 —— "其中 %2 个本来
    // 就存在"叠在分母上会被读成"只下了 1 个"；分母本身也只剩缺的那几个。
    check(lastMessage.contains(
              QStringLiteral("2 个文件已全部就绪（另有 2 个已存在，已跳过）。")),
          "★ G5.5 汇总: 下了 2 个，另有 2 个（库和预置资源对象）并列计为跳过", lastMessage);
    check(readBytes(jsonPath) == jsonBefore,
          "★ G5.6 补全**不动** version.json（和 downloadVersion 的区别 ②）");
    check(!vm.isDownloading(), "G5.6 结束后 isDownloading() 为假");

    // 再补一次：全都在了 ⇒ 还是成功终态（不是"没活干就当失败"），而且字节一个不变
    emissions = 0;
    finished = false;
    lastSuccess = false;
    lastMessage.clear();
    lastDone = 0;
    lastTotal = 0;
    vm.repairVersionFiles(versionId);
    if (!finished) {
        loop.exec();
    }
    check(emissions == 1 && lastSuccess, "G5.7 重复补全: 仍然是一个成功终态", lastMessage);
    check(lastMessage.contains(QStringLiteral("跳过")),
          "G5.7 重复补全: 汇总里明说这次跳过了", lastMessage);
    check(lastMessage.contains(QStringLiteral("无需下载")),
          "★ G5.7 重复补全: 一个都没缺时不许说「0 个文件已全部就绪」"
          "（用户读到的那个说不通的 n-1 就是它）",
          lastMessage);
    check(readBytes(jarPath) == realJar && readBytes(lib0Path) == QByteArray("vm-lib-body-0") &&
              readBytes(lib1Path) == lib1Before,
          "G5.7 重复补全: 三个文件一个字节都没动");
    check(!vm.isDownloading(), "G5.7 结束后 isDownloading() 为假");

    // --- G5.12：体检点名的那几条（ 加的第二个入参）------------------------
    //
    // 「体检说这个文件是坏的、补全却跳过它」这个洞的补法：把体检那份清单（每条都带绝对路径）
    // 一起交给补全，那一批就**不问盘上那份**、直接重下。链子是
    // MainWindow::repairInstanceFiles → repairVersionFiles(id, flaggedPaths) →
    // DownloadManager::setFlaggedPaths() → fileAlreadyValid() 头一句。
    //
    // 为什么非有它不可：jar / 库两边问盘的判据都是"字节数 + sha1"，体检说坏了补全自己也看得出来；
    // 但**资源对象**那一档补全只看在不在（ExistsOnly，几千个文件全量哈希要几秒），于是
    // "尺寸不对"的对象体检报「坏」、补全说"一个都不用下" —— 用户看到两份界面互相打脸。
    // 这一节的资源对象已预置本地，不出网；这里拿**库和 jar**
    // 钉这条链：它们盘上全是好的（G5.7 刚验过），只有"被点名"这一个理由会让它们重下一遍。
    {
        emissions = 0;
        finished = false;
        lastSuccess = false;
        lastMessage.clear();
        lastDone = 0;
        lastTotal = 0;
        const QByteArray lib0Before = readBytes(lib0Path);
        vm.repairVersionFiles(versionId, QStringList{jarPath});
        if (!finished) {
            loop.exec();
        }
        check(emissions == 1 && lastSuccess, "G5.12 点名补全: 一个成功终态", lastMessage);
        // 和不点名那两趟（G5.7 的「无需下载」）对照着看：同样三个文件都在盘上、
        // 全都完好，只有 jar 被点了名 ⇒ 分母是 1 而不是 0。
        check(lastDone == 1 && lastTotal == 1,
              "★ G5.12 点名的那一条进了分母（同样是「全都好好的」，不点名时这个数是 0）",
              QString("%1/%2").arg(lastDone).arg(lastTotal));
        check(readBytes(jarPath) == realJar,
              "G5.12 点名的 jar 真被重新下了一遍（内容还是对的 —— 它是从 file:// 重取的）");
        check(readBytes(lib0Path) == lib0Before && readBytes(lib1Path) == lib1Before,
              "G5.12 没点名的那两个一个字节都没动");
        check(lastMessage.contains(
                  QStringLiteral("1 个文件已全部就绪（另有 3 个已存在，已跳过）。")),
              "G5.12 汇总: 点名的算下了 1 个，两份库与预置资源对象计为跳过 3 个", lastMessage);
    }

    // 点名的路径对不上号（别的实例的文件、或者一条早就不存在的老路径）⇒ 一条都不多下。
    // 这一条钉的是"**只认路径**"：不能写成"清单非空就整批重下"。
    {
        emissions = 0;
        finished = false;
        lastSuccess = false;
        lastMessage.clear();
        lastDone = 0;
        lastTotal = 0;
        vm.repairVersionFiles(versionId,
                              QStringList{work + QStringLiteral("/nobody/else/whatever.jar")});
        if (!finished) {
            loop.exec();
        }
        check(lastTotal == 0 && lastMessage.contains(QStringLiteral("无需下载")),
              "★ G5.13 点名表里对不上号的路径: 一条都不多下（点名表是**按路径**认人的）",
              QString("%1/%2 %3").arg(lastDone).arg(lastTotal).arg(lastMessage));
    }

    // 再补一次、**不点名** ⇒ 又回到"无需下载"：那张点名表是**这一批**的交代，
    // beginBatch() 会清掉它（不然 G5.9 点过名的那条会一直跟着后面每一批重下）。
    {
        emissions = 0;
        finished = false;
        lastSuccess = false;
        lastMessage.clear();
        lastDone = 0;
        lastTotal = 0;
        vm.repairVersionFiles(versionId);
        if (!finished) {
            loop.exec();
        }
        check(lastTotal == 0 && lastMessage.contains(QStringLiteral("无需下载")),
              "★ G5.14 点名只管这一批: 下一批不点名就又一条都不下（beginBatch 清掉了那张表）",
              QString("%1/%2 %3").arg(lastDone).arg(lastTotal).arg(lastMessage));
        check(readBytes(jarPath) == realJar, "G5.14 jar 还是好的（没有又被重下一遍）");
    }

    // 读不出 json：**立刻**回一个失败终态，而且一个批次都不开（UI 不能卡在「检查中」）
    emissions = 0;
    finished = false;
    lastSuccess = true;
    lastMessage.clear();
    vm.repairVersionFiles(QStringLiteral("no-such-version"));
    check(emissions == 1 && !lastSuccess,
          "G5.8 没有 version.json 时: 立刻回一个失败终态", QString::number(emissions));
    check(lastMessage.contains(QStringLiteral("version.json")),
          "G5.8 那句话点的是「读不出 version.json」（不是一句笼统的失败）", lastMessage);
    check(!vm.isDownloading(), "G5.8 而且一个批次都没开（不然按钮会一直灰着）");

    // ③ 加载器实例：目录里只有一份 json（主 jar 在原版那边），补全要补到**父版本**的目录里去
    const QString vanillaId = QStringLiteral("synth-g5vanilla");
    const QString loaderId = QStringLiteral("synth-g5loader");
    const QString vanillaDir = sandbox + "/versions/" + vanillaId;
    const QString loaderDir = sandbox + "/versions/" + loaderId;
    const synth::RemoteFile vanillaJar = synth::makeRemote(
        remoteDir, QStringLiteral("client-vanilla.jar"), QByteArray("g5-vanilla-jar-body"));
    QDir().mkpath(vanillaDir);
    QDir().mkpath(loaderDir);
    const QJsonObject vanillaJson{
        {QStringLiteral("id"), vanillaId},
        {QStringLiteral("libraries"), QJsonArray()},
        {QStringLiteral("downloads"),
         QJsonObject{{QStringLiteral("client"),
                      QJsonObject{{QStringLiteral("url"), vanillaJar.url},
                                  {QStringLiteral("sha1"), vanillaJar.sha1},
                                  {QStringLiteral("size"), vanillaJar.size}}}}}};
    writeFileAt(vanillaDir + "/" + vanillaId + ".json",
                QJsonDocument(vanillaJson).toJson(QJsonDocument::Indented));
    const QJsonObject loaderJson{
        {QStringLiteral("id"), loaderId},
        {QStringLiteral("inheritsFrom"), vanillaId},
        {QStringLiteral("mainClass"),
         QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient")}};
    writeFileAt(loaderDir + "/" + loaderId + ".json",
                QJsonDocument(loaderJson).toJson(QJsonDocument::Indented));
    // 父版本那份 jar 先摆一个内容不对的：这一条验的正是"补到哪个目录里去了"
    writeFileAt(vanillaDir + "/" + vanillaId + ".jar", QByteArray("not-the-vanilla-jar"));

    emissions = 0;
    finished = false;
    lastSuccess = false;
    lastMessage.clear();
    vm.repairVersionFiles(loaderId);
    if (!finished) {
        loop.exec();
    }
    check(emissions == 1 && lastSuccess, "G5.9 加载器实例补全: 成功", lastMessage);
    check(readBytes(vanillaDir + "/" + vanillaId + ".jar") ==
              QByteArray("g5-vanilla-jar-body"),
          "★ G5.9 客户端 jar 补进了**父版本**的目录（加载器实例自己没有 jar）",
          readBytes(vanillaDir + "/" + vanillaId + ".jar"));
    check(!QFile::exists(loaderDir + "/" + loaderId + ".jar"),
          "G5.9 加载器实例自己的目录里**没有**多出一个 jar（区别 ③ 的另一半）");

    return g_failures == 0 ? 0 : 1;
}

