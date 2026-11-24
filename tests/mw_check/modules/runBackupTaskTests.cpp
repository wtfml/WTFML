#include "Support.hpp"
void runBackupTaskTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T35.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    // 数据根目录**两头都要改**（理由见 T19）：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次。
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
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));

    // ---------- 夹具：一个"什么都有"的实例 ----------
    const QString id = QStringLiteral("1.20.1-T35");
    const QString displayName = QStringLiteral("T35快照目标");
    // 下拉框里那一句（= 名字 + "-" + 目录名）：账本标题用的是它，不是裸目录名。
    const QString expectedTitleName = displayName + QStringLiteral("-1.20.1-T35");
    const QString instancePath = AppPaths::versionDir(id);
    QDir().mkpath(instancePath);
    writeFileAt(instancePath + QLatin1Char('/') + id + QStringLiteral(".json"),
                QJsonDocument(QJsonObject{
                                  {QStringLiteral("id"), QStringLiteral("1.20.1")},
                                  {QStringLiteral("type"), QStringLiteral("release")},
                                  {QStringLiteral("mainClass"),
                                   QStringLiteral("net.minecraft.client.main.Main")},
                                  {QStringLiteral("libraries"), QJsonArray{}}})
                    .toJson(QJsonDocument::Compact));
    writeFileAt(instancePath + QLatin1Char('/') + id + QStringLiteral(".jar"),
                QByteArrayLiteral("T35-client-jar-body"));
    writeFileAt(instancePath + QStringLiteral("/wtfml.meta"),
                QJsonDocument(QJsonObject{{QStringLiteral("name"), displayName}})
                    .toJson(QJsonDocument::Compact));
    writeFileAt(instancePath + QStringLiteral("/mods/示例模组.jar"), QByteArrayLiteral("MOD-BODY"));
    writeFileAt(instancePath + QStringLiteral("/mods/data/assets/模组自己的.png"),
                QByteArrayLiteral("PNG"));
    writeFileAt(instancePath + QStringLiteral("/config/配置.toml"), QByteArrayLiteral("a=1"));
    writeFileAt(instancePath + QStringLiteral("/saves/基地周目/level.dat"),
                QByteArrayLiteral("LEVEL"));
    writeFileAt(instancePath + QStringLiteral("/assets/不该进包.bin"),
                QByteArrayLiteral("SHOULD-NOT-BE-IN"));
    writeFileAt(instancePath + QStringLiteral("/libraries/不该进包.jar"),
                QByteArrayLiteral("SHOULD-NOT-BE-IN"));
    const QString worldDir = instancePath + QStringLiteral("/saves/基地周目");
    const QString worldZipDir = BackupFiles::backupDir(BackupFiles::Kind::World);
    const QString instanceZipDir = BackupFiles::backupDir(BackupFiles::Kind::Instance);

    // 身份：离线账号（这一节一个请求都不发；顺带让构造末尾那次皮肤加载走本地）。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T35User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T35.0 建一个离线账号并设成当前身份", offlineError);

    // ---------- 账本那一套（按号认，不按位置）----------
    //
    // 账本落在**启动器自带**那一个固定位置（见 T34 那段）：同一个可执行文件里别的节也会往
    // 这本账里写东西，而且**跨运行留着**，所以只认"这一节新补出来的那几条"。
    const QString ledgerPath = AppPaths::defaultMinecraftDir() + QStringLiteral("/wtfml.tasks");
    const auto backupTasks = [&ledgerPath]() {
        QList<Tasks::TaskSnapshot> found;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Backup) {
                found.append(task);
            }
        }
        return found;
    };
    const auto backupIds = [&ledgerPath]() {
        QSet<quint64> ids;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Backup) {
                ids.insert(task.id);
            }
        }
        return ids;
    };
    // 跑之前没有、跑之后有的那一条（第 0 条是最近的 ⇒ 先遇到的更近）。
    const auto newBackup = [&ledgerPath](const QSet<quint64> &known) {
        Tasks::TaskSnapshot found;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Backup && !known.contains(task.id)) {
                found = task;
                break;
            }
        }
        return found;
    };
    const int baseline = backupTasks().size();

    // 盘上那些包的清单（"该进的进了、能重新下回来的没进去"全靠它）。
    const auto fileAt = [](const QList<BackupFiles::FileRecord> &list, const QString &path) {
        for (const BackupFiles::FileRecord &record : list) {
            if (record.path == path) {
                return record;
            }
        }
        return BackupFiles::FileRecord();
    };
    const auto readAll = [](const QString &path) {
        QByteArray bytes;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            bytes = file.readAll();
        }
        return bytes;
    };
    const auto zipsIn = [](const QString &dir, const QString &pattern) {
        QStringList names = QDir(dir).entryList(QStringList{pattern}, QDir::Files);
        names.sort();
        return names;
    };
    // 一棵目录树的指纹（相对路径 + 字节数，排序后）：拿它比"**一个字节都没动**"。
    const auto treeOf = [](const QString &rootDir) {
        QStringList out;
        QDirIterator it(rootDir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            out.append(QDir(rootDir).relativeFilePath(path) + QLatin1Char('#') +
                       QString::number(QFileInfo(path).size()));
        }
        out.sort();
        return out;
    };
    // 「这一趟真打完了」那一问才算数：`暂时无法操作` 是忙守卫弹的（下面**故意**在同一
    // 时刻再撞一发），它会先被答掉 —— 拿"答了几问"当"跑完了"，第一问一到手就早退，
    // 后面每一节都跟着红（账本上那一项会读成 "interrupted"）。
    const auto wroteBack = [](const PromptAnswerer &answerer) {
        for (const PromptAnswerer::Answered &one : answerer.answered) {
            // 开跑之前那两句不算：`创建快照` 是分岔那一问，`暂时无法操作` 是忙守卫弹的
            // （下面**故意**在同一时刻再撞一发）。剩下那一问才是"这一趟有结果了"。
            if (one.title != QStringLiteral("暂时无法操作") &&
                one.title != QStringLiteral("创建快照")) {
                return true;
            }
        }
        return false;
    };

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        // ---------- ① 「创建快照…」（概览那一颗）→ 账本一项 → 盘上一个包 ----------
        QString snapZip;
        {
            PromptAnswerer answerer({0, 0});
            answerer.start();
            const QSet<quint64> before = backupIds();
            QMetaObject::invokeMethod(&w, "snapshotInstance", Qt::DirectConnection,
                                      Q_ARG(QString, id));

            // 点下去那一刻（**不 pump 事件**）：账本上已经多了一项，而且它就在跑 ——
            // `beginTask()` 是同步的，后台那一趟还没轮到。
            const Tasks::TaskSnapshot started = newBackup(before);
            check(backupTasks().size() == baseline + 1 && started.targetId == id,
                  "★★ T35.1 点一下「创建快照…」⇒ 账本上立刻多一项，目标就是这个实例",
                  started.title);
            // 为什么这里判的是 `Interrupted` 而不是 `Running`：`Tasks::load()` 会把盘上
            // 任何**非终态**的一项读成「已中断」（启动器上次崩了那一档就是这么认出来的，
            // 见 TaskHistory::load），所以"读回来是 Interrupted"**恰恰**是"它这会儿还
            // 没跑完"的证据 —— 等它真收尾了，同一个号读回来就是 Succeeded（下面那几条）。
            check(started.state == Tasks::TaskState::Interrupted,
                  "★★ T35.1 不 pump 事件就去看 ⇒ 它还在跑（盘上那一项还没收尾）",
                  QString::number(int(started.state)));
            // 显示名是**下拉框里那一句** = 名字 + "-" + 目录名（`instanceDisplayName`
            // 拿的就是它）—— 账本上写的是用户认得的那个名字，不是裸目录名。
            check(started.title == QStringLiteral("创建「%1」的快照").arg(expectedTitleName),
                  "★ T35.1 标题里带着**用户认得的那个名字**（不是裸目录名）", started.title);
            // 快照跑着的时候这一号必须挂在「忙」名单上（`installingVersionIds()`
            // 的唯一出处）。它这十几秒一直在读整棵目录树；期间实例被删 / 改名 / 启动，
            // 包里就会静默少掉正在变的那几个文件，收尾却照样报"快照创建完成"。
            check(w.instanceBusyForTest(id),
                  "★★ T35.1 快照在跑的时候这一号在「忙」名单上（改名 / 删除 / 启动都被挡）");

            // 同一时刻再来一发：不该冒出第二项（一次只跑一趟，见 MainWindow 里那段）。
            QMetaObject::invokeMethod(&w, "snapshotInstance", Qt::DirectConnection,
                                      Q_ARG(QString, id));
            check(backupTasks().size() == baseline + 1,
                  "★★ T35.1 同一时刻再来一发 ⇒ 第二项不出现（备份和还原都是走一遍整棵目录算"
                  "每个文件的哈希，叠着跑只会互相抢盘）",
                  QString::number(backupTasks().size()));

            const bool done =
                pumpUntil([&answerer, &wroteBack] { return wroteBack(answerer); }, 60000);
            answerer.stop();
            check(done, "T35.1 那一项跑完了（打包那一趟 + 收尾都走完了）");
            check(!w.instanceBusyForTest(id),
                  "★★ T35.1 快照收尾之后忙标记摘干净了（不然这一号以后再也启动不了 / 删不掉）");
            const Tasks::TaskSnapshot finished = newBackup(before);
            check(finished.state == Tasks::TaskState::Succeeded && finished.errorSummary.isEmpty(),
                  "★ T35.1 收尾是**成功**（账本上那一项说的是摘要这件事，详情不往盘上写）",
                  finished.errorSummary);

            // 一共三问，按顺序：①分岔那一问 ②撞车那一问（busy 守卫）③收尾那一问。
            check(answerer.answered.size() == 3 &&
                      answerer.answered.at(0).title == QStringLiteral("创建快照") &&
                      answerer.answered.at(0).buttons ==
                          QStringList{QStringLiteral("创建快照"),
                                      QStringLiteral("完整快照（含可重新下载的部分）"),
                                      QStringLiteral("取消")},
                  "★★ T35.1 「创建快照」先问一句「要不要连能重新下回来的那部分一起打包」"
                  "（三颗按钮，默认那颗是排除那一档）",
                  answerer.answered.isEmpty() ? QString() : answerer.answered.at(0).title);
            check(answerer.answered.size() == 3 &&
                      answerer.answered.at(1).title == QStringLiteral("暂时无法操作") &&
                      answerer.answered.at(1).text.contains(QStringLiteral("已有备份或还原任务")),
                  "★★ T35.1 上面那一发被挡下来时**当场说了一句人话**（不是默默吞掉）",
                  answerer.answered.size() > 1 ? answerer.answered.at(1).title : QString());
            check(answerer.answered.size() == 3 &&
                      answerer.answered.at(2).title == QStringLiteral("快照创建完成"),
                  "★ T35.1 打完弹的是「快照创建完成」那一句",
                  answerer.answered.isEmpty()
                      ? QString()
                      : (answerer.answered.last().title + QStringLiteral("｜") +
                         answerer.answered.last().text));
        }

        const QStringList instanceZips = zipsIn(instanceZipDir, QStringLiteral("*.zip"));
        check(instanceZips.size() == 1,
              "★ T35.1 盘上真多出一个包（实例快照落 cache/backups/instances）",
              instanceZips.join(QLatin1Char('|')));
        if (instanceZips.size() == 1) {
            snapZip = instanceZipDir + QLatin1Char('/') + instanceZips.first();
            check(QFileInfo(snapZip).fileName().startsWith(id + QLatin1Char('-')),
                  "★ T35.1 包的文件名头一个词就是实例的**目录名**（还原时按它筛，两边同一把尺子）",
                  QFileInfo(snapZip).fileName());
            bool manifestOk = false;
            QString manifestProblem;
            const BackupFiles::Manifest manifest =
                BackupFiles::readManifest(snapZip, &manifestOk, &manifestProblem);
            check(manifestOk, "★★ T35.1 拿产品的读侧把这个包读得回来", manifestProblem);
            check(manifest.kind == BackupFiles::Kind::Instance && manifest.root == id &&
                      manifest.sourceInstanceId == id && manifest.createdAt.isValid(),
                  "★★ T35.1 清单记着来源实例 id 和时间（点名要的那几样）",
                  manifest.createdAt.toString(Qt::ISODate));
            check(!manifest.gameVersion.isEmpty(),
                  "★★ T35.1 游戏版本那一格也记下来了（实例自己那份 json 上认出来的）",
                  manifest.gameVersion);
            check(!manifest.complete &&
                      manifest.excluded == QStringList{QStringLiteral("assets"),
                                                       QStringLiteral("libraries")},
                  "★★ T35.1 默认那一档：清单如实记着「排除了 assets / libraries」",
                  manifest.excluded.join(QLatin1Char('|')));
            check(fileAt(manifest.files, QStringLiteral("assets/不该进包.bin")).path.isEmpty() &&
                      fileAt(manifest.files, QStringLiteral("libraries/不该进包.jar")).path.isEmpty(),
                  "★★ T35.1 实例根下面那两份**没进包**（「默认排除能重新下回来的」）",
                  QString::number(manifest.fileCount()));
            check(fileAt(manifest.files, QStringLiteral("mods/示例模组.jar")).size == 8 &&
                      !fileAt(manifest.files, QStringLiteral("saves/基地周目/level.dat"))
                           .path.isEmpty(),
                  "★ T35.1 模组 / 存档这些该进的都进了（路径是**相对实例目录**的）");
            check(!fileAt(manifest.files, QStringLiteral("mods/data/assets/模组自己的.png"))
                       .path.isEmpty(),
                  "★★ T35.1 排除只看**顶层**：模组自己 data 里的 assets 照收不误");
        }

        // ---------- ② 世界那一页的「立即备份」 ----------
        {
            PromptAnswerer answerer({0, 0});
            answerer.start();
            const QSet<quint64> before = backupIds();
            QMetaObject::invokeMethod(&w, "backupWorldNow", Qt::DirectConnection,
                                      Q_ARG(QString, worldDir));
            check(w.instanceBusyForTest(id),
                  "★★ T35.2 世界备份在跑的时候这一号也在「忙」名单上");
            const bool done =
                pumpUntil([&answerer, &wroteBack] { return wroteBack(answerer); }, 60000);
            answerer.stop();
            check(done, "T35.2 世界备份那一趟跑完了");
            check(!w.instanceBusyForTest(id),
                  "★★ T35.2 世界备份收尾之后忙标记也摘干净了");
            const Tasks::TaskSnapshot finished = newBackup(before);
            check(finished.state == Tasks::TaskState::Succeeded &&
                      finished.title == QStringLiteral("备份世界「基地周目」") &&
                      finished.targetId == id,
                  "★ T35.2 「立即备份」也落进同一本账（标题说的是**这个世界**）",
                  finished.title);
        }

        const QStringList worldZips = zipsIn(worldZipDir, QStringLiteral("*.zip"));
        check(worldZips.size() == 1, "★ T35.2 存档那份落在 cache/backups/worlds",
              worldZips.join(QLatin1Char('|')));
        if (worldZips.size() == 1) {
            const QString worldZip = worldZipDir + QLatin1Char('/') + worldZips.first();
            bool worldOk = false;
            QString worldProblem;
            const BackupFiles::Manifest manifest =
                BackupFiles::readManifest(worldZip, &worldOk, &worldProblem);
            check(worldOk && manifest.kind == BackupFiles::Kind::World &&
                      manifest.root == QStringLiteral("基地周目") &&
                      manifest.worldName == QStringLiteral("基地周目") &&
                      manifest.sourceInstanceId == id,
                  "★★ T35.2 存档那一份：前缀 = 存档目录名，来源实例 id 也在（还原时靠前者落回去）",
                  manifest.root);
            check(manifest.excluded.isEmpty(),
                  "★ T35.2 存档没有「能重新下回来的部分」这一说 ⇒ 排除表是空的"
                  "（那个分岔只对实例快照问）");
        }

        // ---------- ②b 实例正被别的作业占着时，世界不许改名 ----------
        //
        // 快照 / 世界备份按**改名前算好的绝对路径**逐个读整棵目录树。目录在扫描途中被
        // 改名，那一趟要么静默少读一批文件、要么写到一半找不到落点，收尾却照样报
        // "备份完成" —— 一份看着成功、其实是残的备份比失败更糟。
        //
        // NOTE: 「删除世界」那道闸读的是同一个集合（`installingVersionIds()`），断言就
        // 钉在「改名」这一条上 —— 两处必须用同一个判据，不然删得掉、改不了。
        {
            w.markFileOperationBusyForTest(id);
            dismissNextPrompt();
            bool renamed = true;
            const bool invoked =
                QMetaObject::invokeMethod(&w, "renameInstanceWorld", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, renamed), Q_ARG(QString, id),
                                          Q_ARG(QString, worldDir),
                                          Q_ARG(QString, QStringLiteral("不许改的名字")));
            check(invoked && !renamed,
                  "★★ T35.2b 这一号在「忙」名单上时世界改名被挡下（返回 false，不是悄悄改了）");
            check(g_modalTitle == QStringLiteral("重命名世界") &&
                      g_modalText.contains(QStringLiteral("正在被另一项操作占用")),
                  "★★ T35.2b 挡下来时**当场说了一句人话**（说的是它正被占用，不是改名失败）",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(QFileInfo(worldDir).isDir() &&
                      !QFileInfo(instancePath + QStringLiteral("/saves/不许改的名字")).exists(),
                  "★★ T35.2b 盘上那座存档一个字节都没动（目录还在老地方）");
            w.releaseFileOperationBusyForTest(id);
            check(!w.instanceBusyForTest(id), "T35.2b 撇干净了（后面的节不受影响）");
        }

        // ---------- ③ 「从快照恢复」那一趟：内容回来、还原前那一份也留着 ----------
        QString safetyZip;
        {
            PromptAnswerer answerer({0, 0});
            answerer.start();
            // 先把实例改乱：一个文件改内容、一个删掉、再塞一个备份里没有的。
            writeFileAt(instancePath + QStringLiteral("/mods/示例模组.jar"),
                        QByteArrayLiteral("被改过"));
            QFile::remove(instancePath + QStringLiteral("/config/配置.toml"));
            writeFileAt(instancePath + QStringLiteral("/只在这儿有.txt"),
                        QByteArrayLiteral("OLD-ONLY"));

            const QSet<quint64> before = backupIds();
            QMetaObject::invokeMethod(&w, "startBackupRestore", Qt::DirectConnection,
                                      Q_ARG(QString, snapZip),
                                      Q_ARG(QString, AppPaths::versionsDir()),
                                      Q_ARG(QString, QStringLiteral("还原「T35」的快照")),
                                      Q_ARG(QString, id),
                                      Q_ARG(QString, QStringLiteral("还原完成")));
            const bool done =
                pumpUntil([&answerer, &wroteBack] { return wroteBack(answerer); }, 60000);
            answerer.stop();
            check(done, "T35.3 还原那一趟跑完了");
            const Tasks::TaskSnapshot finished = newBackup(before);
            check(finished.state == Tasks::TaskState::Succeeded &&
                      finished.title == QStringLiteral("还原「T35」的快照"),
                  "★ T35.3 还原也记在同一本账上（和「打一份」是同一个 kind）",
                  QStringLiteral("state=%1 title=%2 id=%3")
                      .arg(static_cast<int>(finished.state))
                      .arg(finished.title)
                      .arg(finished.id));
            const QByteArray restoredMod =
                readAll(instancePath + QStringLiteral("/mods/示例模组.jar"));
            const QByteArray restoredConfig =
                readAll(instancePath + QStringLiteral("/config/配置.toml"));
            check(restoredMod == QByteArrayLiteral("MOD-BODY")
                      && restoredConfig == QByteArrayLiteral("a=1"),
                  "★★ T35.3 改过 / 删掉的那两个都回来了（落点 = versions/<目录名>，"
                  "不是倒在 versions/ 这一层）",
                  QStringLiteral("mod=[%1] config=[%2] state=%3")
                      .arg(QString::fromUtf8(restoredMod), QString::fromUtf8(restoredConfig))
                      .arg(static_cast<int>(finished.state)));
            check(!QFile::exists(instancePath + QStringLiteral("/只在这儿有.txt")),
                  "★ T35.3 备份里没有的那个文件跟着旧的整份一起走了",
                  QStringLiteral("旧的那个文件还在吗=%1 state=%2")
                      .arg(QFile::exists(instancePath + QStringLiteral("/只在这儿有.txt")) ? 1 : 0)
                      .arg(static_cast<int>(finished.state)));

            const QStringList safetyNames = zipsIn(instanceZipDir, QStringLiteral("*恢复前*.zip"));
            check(safetyNames.size() == 1,
                  "★★ T35.3 还原之前自动给「现在这一份」留了一份安全快照（文件名里带「恢复前」）",
                  safetyNames.join(QLatin1Char('|')));
            if (safetyNames.size() == 1) {
                safetyZip = instanceZipDir + QLatin1Char('/') + safetyNames.first();
                bool safetyOk = false;
                const BackupFiles::Manifest safety =
                    BackupFiles::readManifest(safetyZip, &safetyOk, nullptr);
                check(safetyOk && safety.safety,
                      "★★ T35.3 那一份清单里带着「自动做出来的」这个记号");
                check(!fileAt(safety.files, QStringLiteral("只在这儿有.txt")).path.isEmpty(),
                      "★★ T35.3 它是**改之前**那一份（被换掉的那个文件在里面）");
            }
        }

        // 那一句「这一步是能退回来的」要能兑现：把安全快照还原回去，被换掉的东西回来。
        if (!safetyZip.isEmpty()) {
            PromptAnswerer answerer({0, 0});
            answerer.start();
            const QSet<quint64> before = backupIds();
            QMetaObject::invokeMethod(&w, "startBackupRestore", Qt::DirectConnection,
                                      Q_ARG(QString, safetyZip),
                                      Q_ARG(QString, AppPaths::versionsDir()),
                                      Q_ARG(QString, QStringLiteral("还原「T35」的安全快照")),
                                      Q_ARG(QString, id),
                                      Q_ARG(QString, QStringLiteral("还原完成")));
            const bool done =
                pumpUntil([&answerer, &wroteBack] { return wroteBack(answerer); }, 60000);
            answerer.stop();
            check(done && readAll(instancePath + QStringLiteral("/只在这儿有.txt"))
                              == QByteArrayLiteral("OLD-ONLY") &&
                      readAll(instancePath + QStringLiteral("/mods/示例模组.jar"))
                          == QByteArrayLiteral("被改过"),
                  "★★ T35.3 那句「能退回来」兑得现：把安全快照还原回去，被换掉的那一份回来了");
        }

        // ---------- ④ 拿别人的包来还原：账本上是**失败**，实例目录一个字节没动 ----------
        {
            const QString foreignSource = root.path() + QStringLiteral("/别人的包.txt");
            writeFileAt(foreignSource, QByteArrayLiteral("hello"));
            const QString foreignZip = root.path() + QStringLiteral("/别人的包.zip");
            QString zipError;
            check(ZipUtil::createZip(foreignZip,
                                     {{QStringLiteral("别人的包.txt"), foreignSource}}, &zipError),
                  "T35.4 造一个别人的包（压缩包是好的，就是没有我们那份清单）", zipError);

            PromptAnswerer answerer({0, 0});
            answerer.start();
            const QSet<quint64> before = backupIds();
            const QStringList treeBefore = treeOf(instancePath);
            QMetaObject::invokeMethod(&w, "startBackupRestore", Qt::DirectConnection,
                                      Q_ARG(QString, foreignZip),
                                      Q_ARG(QString, AppPaths::versionsDir()),
                                      Q_ARG(QString, QStringLiteral("还原「T35」的快照")),
                                      Q_ARG(QString, id),
                                      Q_ARG(QString, QStringLiteral("还原完成")));
            const bool done =
                pumpUntil([&answerer, &wroteBack] { return wroteBack(answerer); }, 60000);
            answerer.stop();
            check(done, "T35.4 那一趟也收尾了");
            const Tasks::TaskSnapshot finished = newBackup(before);
            check(finished.state == Tasks::TaskState::Failed &&
                      finished.errorSummary.contains(QStringLiteral("未找到备份元数据")),
                  "★★ T35.4 别人的包 ⇒ 账本上是**失败**，那句原因说的是「不是本启动器做出来"
                  "的备份」（不是「文件坏了」—— 合成一句会让人去怀疑一个其实好好的文件）",
                  finished.errorSummary);
            check(treeOf(instancePath) == treeBefore,
                  "★★ T35.4 被拒绝的那一趟实例目录**一个字节都没动**（读清单在动盘之前）",
                  treeOf(instancePath).join(QLatin1Char('|')));
        }
    }

    // ---------- 收摊：账号、设置键、数据根目录都还回去 ----------
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T35.9 这一节加的账号清干净了");

    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T35.9 数据根目录换回来了");
}


