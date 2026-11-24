#include "Support.hpp"
#include "manage/ManualRecovery.hpp"
#include "manage/RecoveryReview.hpp"
#include "manage/RestoreJournal.hpp"
#include "manage/OwnedStaging.hpp"
#include "util/DirectorySessionLock.hpp"

#include <QLockFile>

void runBackupTests() {
    section("ZQ. 世界与实例备份：清单 / 打包 / 还原");

    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir temp;
    check(temp.isValid(), "ZQ0 临时数据根目录建出来了");
    if (!temp.isValid()) {
        return;
    }
    AppPaths::setMinecraftDir(temp.path());

    const auto sha1Of = [](const QByteArray &bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
    };
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
    const auto recordOf = [&sha1Of](const QString &path, const QByteArray &bytes) {
        BackupFiles::FileRecord record;
        record.path = path;
        record.size = bytes.size();
        record.sha1 = sha1Of(bytes);
        return record;
    };

    const QString work = temp.path() + QStringLiteral("/work");
    const QDateTime at = QDateTime::fromString(QStringLiteral("2026-10-13T10:15:30"), Qt::ISODate);

    // ---------- ZQ1. 清单本身：往返、格式标记、前缀的合法性 ----------
    //
    // 清单是这一整套的凭据，所以先把它单独钉一遍：进 JSON 之前和出 JSON 之后**逐格一样**
    // （少一格就是"还原出来落错地方"），认不出的格式 / 可疑的前缀**整份不认**。
    BackupFiles::Manifest sample;
    sample.kind = BackupFiles::Kind::World;
    sample.root = QStringLiteral("基地周目");
    sample.label = QStringLiteral("基地周目");
    sample.sourceInstanceId = QStringLiteral("1.20.1-forge-47.2.0");
    sample.sourceInstanceName = QStringLiteral("我的整合");
    sample.gameVersion = QStringLiteral("1.20.1（Forge 47.2.0）");
    sample.worldName = QStringLiteral("基地周目");
    sample.createdAt = at;
    sample.scope = QStringList{BackupFiles::scopeLine(QStringLiteral("saves/基地周目"), 2)};
    sample.files = QList<BackupFiles::FileRecord>{
        recordOf(QStringLiteral("level.dat"), QByteArrayLiteral("LEVELDATA")),
        recordOf(QStringLiteral("region/r.0.0.mca"), QByteArrayLiteral("REGION-0-0"))};
    // 故意写一个错的总数：清单是**记录**，不是凭据 —— 手改过这个数，界面上的"共 N MB"
    // 不该跟着错（下面 ZQ1 第三条就是钉这个）。
    sample.totalBytes = 999999;

    bool ok = false;
    const BackupFiles::Manifest back =
        BackupFiles::manifestFromJson(BackupFiles::manifestToJson(sample), &ok);
    check(ok && back.kind == BackupFiles::Kind::World && back.root == sample.root &&
              back.label == sample.label && back.sourceInstanceId == sample.sourceInstanceId &&
              back.sourceInstanceName == sample.sourceInstanceName &&
              back.gameVersion == sample.gameVersion && back.worldName == sample.worldName &&
              back.createdAt == sample.createdAt && back.scope == sample.scope,
          "★ ZQ1 清单往返：四样（来源实例 id / 时间 / 游戏版本 / 包含范围）一字不差",
          back.gameVersion);
    check(back.files.size() == 2 &&
              fileAt(back.files, QStringLiteral("region/r.0.0.mca")).sha1
                  == sha1Of(QByteArrayLiteral("REGION-0-0")) &&
              fileAt(back.files, QStringLiteral("region/r.0.0.mca")).size == 10,
          "ZQ1 逐文件记录（名字 / 大小 / sha1）往返");
    check(back.totalBytes == 19 && back.totalBytes != sample.totalBytes,
          "★ ZQ1 totalBytes 以数出来的为准（手改过那个数不作数）",
          QString::number(back.totalBytes));

    // 格式标记对不上 → 整份不认。**这是最要紧的一条**：别的启动器做的包（或者以后我们自己
    // 改了格式的旧包）只能被挡住，不能被半信半疑地往里解。
    QByteArray forged = BackupFiles::manifestToJson(sample);
    forged.replace(BackupFiles::formatTag().toUtf8(), QByteArrayLiteral("other-launcher/1"));
    bool forgedOk = true;
    check(!(BackupFiles::manifestFromJson(forged, &forgedOk).isValid()) && !forgedOk,
          "★★ ZQ1 格式标记对不上就整份不认（别人的包绝不半信半疑地往里解）");

    const QByteArray oddKind = QByteArrayLiteral(
        "{\"format\":\"wtfml-backup/1\",\"kind\":\"tarball\",\"root\":\"x\","
        "\"createdAt\":\"2026-10-13T10:15:30\"}");
    bool oddOk = true;
    check(!BackupFiles::manifestFromJson(oddKind, &oddOk).isValid() && !oddOk,
          "★★ ZQ1 认不出的档（既不是 world 也不是 instance）→ 整份不认，不猜");

    check(!BackupFiles::Manifest().isValid(), "ZQ1 空清单不算数（前缀和时间都得有）");
    BackupFiles::Manifest escape = sample;
    escape.root = QStringLiteral("../别人的目录");
    check(!escape.isValid(), "★★ ZQ1 前缀里带 `..` 的清单不认（还原的落点是拼出来的）");
    escape.root = QStringLiteral("a/b");
    check(!escape.isValid(), "★★ ZQ1 前缀里带 `/` 的清单不认");
    escape.root = QStringLiteral("C:/Windows");
    check(!escape.isValid(), "★★ ZQ1 前缀是绝对路径的清单不认");
    escape.root = QStringLiteral("..");
    check(!escape.isValid(), "ZQ1 前缀就是 `..` 的清单不认");
    for (const QString &sneaky : {QStringLiteral(".. "), QStringLiteral(". "),
                                  QStringLiteral("..."), QStringLiteral("a. "),
                                  QStringLiteral("a "), QStringLiteral("CON"),
                                  QStringLiteral("nul.txt")}) {
        escape.root = sneaky;
        check(!escape.isValid(),
              QStringLiteral("★★ ZQ1 前缀「%1」不认（Windows 会把末尾的空格/点折掉 ⇒ 它落回的"
                             "是**上一层/本层目录**，不是一个新的名字；保留设备名同理）")
                  .arg(sneaky));
    }
    const QByteArray trailingSpaceRoot = QByteArrayLiteral(
        "{\"format\":\"wtfml-backup/1\",\"kind\":\"world\",\"root\":\".. \","
        "\"createdAt\":\"2026-10-13T10:15:30\"}");
    bool trailingOk = true;
    check(!BackupFiles::manifestFromJson(trailingSpaceRoot, &trailingOk).isValid() && !trailingOk,
          "★★ ZQ1 从 JSON 读进来那条路也挡住了末尾空格（不是只有手搓的结构体才查）");
    const QByteArray slashRoot = QByteArrayLiteral(
        "{\"format\":\"wtfml-backup/1\",\"kind\":\"world\",\"root\":\"../../x\","
        "\"createdAt\":\"2026-10-13T10:15:30\"}");
    bool slashOk = true;
    check(!BackupFiles::manifestFromJson(slashRoot, &slashOk).isValid() && !slashOk,
          "★ ZQ1 从 JSON 读进来那条路也挡住了（不是只有手搓的结构体才查）");

    // 实例快照那两个字段只对它有意义：一个恒为 false 的 `complete` 落在世界备份上，
    // 读的人只会以为那是个开关。
    BackupFiles::Manifest snap = sample;
    snap.kind = BackupFiles::Kind::Instance;
    snap.root = QStringLiteral("1.20.1-forge-47.2.0");
    snap.worldName.clear();
    snap.complete = false;
    snap.excluded = QStringList{QStringLiteral("assets"), QStringLiteral("libraries")};
    const QByteArray snapJson = BackupFiles::manifestToJson(snap);
    check(snapJson.contains(QByteArrayLiteral("\"excluded\"")) &&
              !BackupFiles::manifestToJson(sample).contains(QByteArrayLiteral("\"excluded\"")),
          "ZQ1 `excluded` / `complete` 只有实例快照才写（世界备份无从排除起）");
    bool snapOk = false;
    const BackupFiles::Manifest snapBack = BackupFiles::manifestFromJson(snapJson, &snapOk);
    check(snapOk && snapBack.kind == BackupFiles::Kind::Instance && !snapBack.complete &&
              snapBack.excluded == snap.excluded && snapBack.worldName.isEmpty(),
          "ZQ1 实例快照那份（完整标记 + 排除表）往返");

    // ---------- ZQ2. 走目录：哪些进、排序、排除只看顶层 ----------
    const QString worldDir = work + QStringLiteral("/基地周目");
    writeFileAt(worldDir + QStringLiteral("/level.dat"), QByteArrayLiteral("LEVELDATA"));
    writeFileAt(worldDir + QStringLiteral("/region/r.0.0.mca"), QByteArrayLiteral("REGION-0-0"));
    writeFileAt(worldDir + QStringLiteral("/region/r.0.-1.mca"), QByteArrayLiteral("REGION-0-N1"));
    writeFileAt(worldDir + QStringLiteral("/sub/assets/keep.txt"), QByteArrayLiteral("keep"));
    writeFileAt(worldDir + QStringLiteral("/assets/skip.txt"), QByteArrayLiteral("skip"));
    writeFileAt(worldDir + QStringLiteral("/.session.lock"), QByteArrayLiteral("lock"));

    QString collectProblem;
    const QList<BackupFiles::FileRecord> allFiles =
        BackupFiles::collectFiles(worldDir, QStringList(), &collectProblem);
    check(collectProblem.isEmpty() && allFiles.size() == 6,
          "ZQ2 走一遍世界目录：6 个文件（嵌套的、名字里带点的都在）",
          QString::number(allFiles.size()) + " / " + collectProblem);
    check(fileAt(allFiles, QStringLiteral("region/r.0.0.mca")).sha1
              == sha1Of(QByteArrayLiteral("REGION-0-0")) &&
              fileAt(allFiles, QStringLiteral(".session.lock")).size == 4,
          "ZQ2 每条记录的校验值是按盘上内容真算的");

    QStringList collected;
    for (const BackupFiles::FileRecord &record : allFiles) {
        collected.append(record.path);
    }
    QStringList sorted = collected;
    std::sort(sorted.begin(), sorted.end());
    check(collected == sorted,
          "★★ ZQ2 清单按路径排序（同一份内容打两次包，清单要逐字一样）", collected.join('|'));
    const QList<BackupFiles::FileRecord> again =
        BackupFiles::collectFiles(worldDir, QStringList(), nullptr);
    QStringList collectedAgain;
    for (const BackupFiles::FileRecord &record : again) {
        collectedAgain.append(record.path);
    }
    check(collectedAgain == collected, "ZQ2 走两遍结果一样");

    const QList<BackupFiles::FileRecord> noAssets =
        BackupFiles::collectFiles(worldDir, QStringList{QStringLiteral("assets")}, nullptr);
    check(noAssets.size() == 5 && fileAt(noAssets, QStringLiteral("assets/skip.txt")).path.isEmpty(),
          "★ ZQ2 顶层 `assets/` 被排除掉了（默认不收能重新下回来的那一堆）",
          QString::number(noAssets.size()));
    check(fileAt(noAssets, QStringLiteral("sub/assets/keep.txt")).path
              == QStringLiteral("sub/assets/keep.txt"),
          "★★ ZQ2 排除**只看顶层**：某个模组自己 data 里的 assets 照收不误"
          "（按名字全局排除会悄悄漏东西，几年之后才有人发现）");

    const QString emptyDir = work + QStringLiteral("/空目录");
    QDir().mkpath(emptyDir);
    QString emptyProblem;
    check(BackupFiles::collectFiles(emptyDir, QStringList(), &emptyProblem).isEmpty() &&
              emptyProblem.isEmpty(),
          "ZQ2 空目录回一张空表，**不是**一个错误（空表和「没走成」分得开）");
    QString missingProblem;
    check(BackupFiles::collectFiles(work + QStringLiteral("/没有这个目录"), QStringList(),
                                    &missingProblem)
                    .isEmpty() &&
              !missingProblem.isEmpty(),
          "ZQ2 目录不在时如实报错", missingProblem);

    // ---------- ZQ3. 打包：真打一个 zip，再用产品的读侧读回来 ----------
    BackupFiles::Manifest spec;
    spec.kind = BackupFiles::Kind::World;
    spec.root = QStringLiteral("基地周目");
    spec.label = QStringLiteral("基地周目");
    spec.sourceInstanceId = QStringLiteral("1.20.1-forge-47.2.0");
    spec.sourceInstanceName = QStringLiteral("我的整合");
    spec.gameVersion = QStringLiteral("1.20.1（Forge 47.2.0）");
    spec.worldName = QStringLiteral("基地周目");
    spec.scope = QStringList{BackupFiles::scopeLine(QStringLiteral("saves/基地周目"), 6)};
    // createdAt 故意不填：用户在界面上点的没有"时间"这回事，是打包那一趟补上的。

    const QString worldZip = BackupFiles::backupDir(BackupFiles::Kind::World) + QLatin1Char('/')
                             + BackupFiles::backupFileName(spec.label, at);
    {
        QString problem;
        check(BackupFiles::collectFiles(worldDir, {}, &problem, [] { return true; }).isEmpty()
              && problem.contains(QStringLiteral("取消")), "ZQ3 备份扫描响应取消且不返回半张清单", problem);
        const QString cancelledZip = work + QStringLiteral("/cancelled-backup.zip");
        const auto cancelled = BackupFiles::writeBackup(spec, worldDir, cancelledZip, [] { return true; });
        check(!cancelled.ok && !QFileInfo::exists(cancelledZip), "ZQ3 取消备份不创建归档");
        int checks = 0;
        const auto interrupted = BackupFiles::writeBackup(spec, worldDir, cancelledZip,
            [&] { return ++checks >= 3; });
        check(!interrupted.ok && !QFileInfo::exists(cancelledZip), "ZQ3 扫描途中取消不会发布不完整备份");
        const QString existingZip = work + QStringLiteral("/existing-before-cancel.zip");
        writeFileAt(existingZip, "preserved");
        const bool stopped = !ZipUtil::createZip(existingZip, {}, &problem, [] { return true; });
        QFile old(existingZip);
        check(stopped && old.open(QIODevice::ReadOnly) && old.readAll() == QByteArray("preserved"),
              "ZQ3 取消打包不删除原有文件");
        // 目标已经存在、而这一趟**本来能打成功**：必须拒绝，而不是把旧文件盖掉。
        // NOTE: 这一条放在 ZQ3 里面是因为它盯的正是"上一份好的备份"—— PowerShell 那一侧
        // `ZipFile::Open($target,'Create')` 会把已有目标当场截成 0 字节，失败那一路又会把它
        // 删掉；等出了事再看盘上就只剩"打包失败"和一份没了。
        const QString occupiedZip = work + QStringLiteral("/occupied-target.zip");
        const QString occupiedSource = work + QStringLiteral("/occupied-source.txt");
        writeFileAt(occupiedZip, "keep-me");
        writeFileAt(occupiedSource, "new-content");
        QString occupiedProblem;
        const bool overwritten = ZipUtil::createZip(
            occupiedZip, {{QStringLiteral("x.txt"), occupiedSource}}, &occupiedProblem);
        QFile keptZip(occupiedZip);
        check(!overwritten && occupiedProblem.contains(QStringLiteral("已经存在")) &&
                  keptZip.open(QIODevice::ReadOnly) &&
                  keptZip.readAll() == QByteArray("keep-me"),
              "ZQ3 目标已存在时拒绝打包，原有文件一个字节都不动", occupiedProblem);
    }
    {
        const QString changedSource = work + QStringLiteral("/changing-source");
        const QString changedOutput = work + QStringLiteral("/changing-output");
        QDir().mkpath(changedOutput);
        const QString input = changedSource + QStringLiteral("/level.dat");
        writeFileAt(input, QByteArrayLiteral("before"));
        bool changed = false;
        const auto result = BackupFiles::writeBackup(spec, changedSource,
            changedOutput + QStringLiteral("/backup.zip"), [&] {
                if (!changed && !QDir(changedOutput).entryList(
                        {QStringLiteral(".wtfml-backup-*")}, QDir::Dirs | QDir::Hidden
                            | QDir::NoDotAndDotDot).isEmpty()) {
                    changed = true;
                    writeFileAt(input, QByteArrayLiteral("edited")); // same length, different hash
                }
                return false;
            });
        check(changed && !result.ok && result.problem.contains(QStringLiteral("变化")),
              "ZQ3 扫描后同长度文件变化拒绝发布备份", result.problem);
        check(QDir(changedOutput).entryList(QDir::Files | QDir::Dirs | QDir::Hidden
                  | QDir::NoDotAndDotDot).isEmpty(), "ZQ3 失败后清除暂存目录及未发布归档");
        const auto retry = BackupFiles::writeBackup(spec, changedSource,
            changedOutput + QStringLiteral("/backup.zip"));
        check(retry.ok && retry.manifest.files.size() == 1
                  && retry.manifest.files.first().sha1 == sha1Of(QByteArrayLiteral("edited")),
              "ZQ3 文件稳定后可重新备份且记录新哈希", retry.problem);
        bool added = false;
        const auto addition = BackupFiles::writeBackup(spec, changedSource,
            changedOutput + QStringLiteral("/addition.zip"), [&] {
                if (!added && !QDir(changedOutput).entryList(
                        {QStringLiteral(".wtfml-backup-*")}, QDir::Dirs | QDir::Hidden
                            | QDir::NoDotAndDotDot).isEmpty()) {
                    added = true;
                    writeFileAt(changedSource + QStringLiteral("/new.dat"), "new");
                }
                return false;
            });
        check(added && !addition.ok && addition.problem.contains(QStringLiteral("变化"))
                  && !QFileInfo::exists(changedOutput + QStringLiteral("/addition.zip")),
              "ZQ3 复制期间新增文件不发布缺项备份", addition.problem);
        bool stoppedAtStaging = false;
        const auto stop = BackupFiles::writeBackup(spec, changedSource,
            changedOutput + QStringLiteral("/cancel-stage.zip"), [&] {
                stoppedAtStaging = !QDir(changedOutput).entryList(
                    {QStringLiteral(".wtfml-backup-*")}, QDir::Dirs | QDir::Hidden
                        | QDir::NoDotAndDotDot).isEmpty();
                return stoppedAtStaging;
            });
        check(stoppedAtStaging && !stop.ok
                  && !QFileInfo::exists(changedOutput + QStringLiteral("/cancel-stage.zip"))
                  && QDir(changedOutput).entryList({QStringLiteral(".wtfml-backup-*")},
                       QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
              "ZQ3 暂存途中取消清理副本且不发布归档", stop.problem);
    }
    const BackupFiles::WriteResult written = BackupFiles::writeBackup(spec, worldDir, worldZip);
    check(written.ok && QFile::exists(worldZip), "ZQ3 世界备份真打出来了", written.problem);
    check(written.manifest.fileCount() == 6 && written.manifest.createdAt.isValid() &&
              written.manifest.totalBytes == 9 + 10 + 11 + 4 + 4 + 4,
          "★ ZQ3 清单里的条数和总数是这一趟自己数出来的，时间也是这一趟补上的",
          QString::number(written.manifest.fileCount()));
    if (written.ok) {
        const QByteArray zipBytes = readAll(worldZip);
        check(zipBytes.contains(QByteArrayLiteral("wtfml-backup.json")) &&
                  zipBytes.contains(QByteArrayLiteral("/region/r.0.0.mca")),
              "★ ZQ3 zip 里：清单躺在**根上**，世界那堆文件在**一层前缀底下**"
              "（清单放前缀里就会被解回用户的世界目录）");

        bool readOk = false;
        QString readProblem;
        const BackupFiles::Manifest readBack =
            BackupFiles::readManifest(worldZip, &readOk, &readProblem);
        check(readOk && readBack.kind == BackupFiles::Kind::World &&
                  readBack.root == QStringLiteral("基地周目") &&
                  readBack.sourceInstanceId == spec.sourceInstanceId &&
                  readBack.gameVersion == spec.gameVersion && readBack.fileCount() == 6,
              "ZQ3 产品的读侧把这份包读回来了（含来源实例 id 和游戏版本）", readProblem);
    }

    // 别人的包：压缩包是好的，就是没有我们那份清单。
    const QString foreignZip = work + QStringLiteral("/别人的包.zip");
    writeFileAt(work + QStringLiteral("/别人的包.txt"), QByteArrayLiteral("hello"));
    QString foreignZipError;
    check(ZipUtil::createZip(
              foreignZip,
              {{QStringLiteral("别人的包.txt"), work + QStringLiteral("/别人的包.txt")}},
              &foreignZipError),
          "ZQ3 造一个别人的包（内容好好的，就是没有我们那份清单）", foreignZipError);
    bool foreignOk = true;
    QString foreignProblem;
    BackupFiles::readManifest(foreignZip, &foreignOk, &foreignProblem);
    check(!foreignOk && foreignProblem.contains(QStringLiteral("未找到备份元数据")),
          "★★ ZQ3 「不是本启动器做的包」和「读不动」是**两句不同的话**（这里说的是前者，"
          "不然用户会去怀疑一个其实好好的文件）",
          foreignProblem);
    bool junkOk = true;
    QString junkProblem;
    BackupFiles::readManifest(work + QStringLiteral("/别人的包.txt"), &junkOk, &junkProblem);
    check(!junkOk && junkProblem.contains(QStringLiteral("无法作为压缩包打开")),
          "ZQ3 拿一个根本不是压缩包的文件来 → 说的是「打不开」", junkProblem);

    // 打不成的那几路：一个半成品都不许留在盘上。
    const QString neverZip = work + QStringLiteral("/不该出现.zip");
    const BackupFiles::WriteResult missing =
        BackupFiles::writeBackup(spec, work + QStringLiteral("/没有这个目录"), neverZip);
    check(!missing.ok && !missing.problem.isEmpty() && !QFile::exists(neverZip),
          "★ ZQ3 来源目录不在 → 如实报错，盘上不留半成品", missing.problem);
    BackupFiles::Manifest noRoot = spec;
    noRoot.root.clear();
    check(!BackupFiles::writeBackup(noRoot, worldDir, neverZip).ok,
          "ZQ3 没说清前缀的清单打不了包（还原时落点拼不出来）");
    const BackupFiles::WriteResult emptyWrite =
        BackupFiles::writeBackup(spec, emptyDir, work + QStringLiteral("/空.zip"));
    check(!emptyWrite.ok && !QFile::exists(work + QStringLiteral("/空.zip")),
          "ZQ3 空目录没什么可备份的（不是打一个空包出来）", emptyWrite.problem);

    // ---------- ZQ4. 还原：安全快照、拒绝、对账 ----------
    const QString restoreParent = work + QStringLiteral("/还原到这儿");
    QDir().mkpath(restoreParent);
    const QString restoredWorld = restoreParent + QStringLiteral("/基地周目");

    // ① 还原到一块空地：那儿本来什么都没有，所以**不该**做安全快照。
    const BackupFiles::RestoreResult fresh = BackupFiles::restoreBackup(worldZip, restoreParent);
    check(fresh.ok && fresh.checked == 6 && fresh.mismatched == 0 && fresh.safetyZip.isEmpty(),
          "ZQ4 还原到一块空地：6 个文件全对上，没做安全快照（那儿本来什么都没有）",
          fresh.problem);
    check(fresh.targetPath == restoredWorld,
          "★ ZQ4 落点 = 目标父目录 + 清单里那个前缀（不是文件名，也不是别处）",
          fresh.targetPath);
    check(readAll(restoredWorld + QStringLiteral("/level.dat")) == QByteArrayLiteral("LEVELDATA") &&
              readAll(restoredWorld + QStringLiteral("/region/r.0.0.mca"))
                  == QByteArrayLiteral("REGION-0-0"),
          "★ ZQ4 解出来的内容一字不差（前缀被剥掉，落回来正是那个目录）");
    check(!QFile::exists(restoredWorld + QStringLiteral("/wtfml-backup.json")) &&
              !QFile::exists(restoreParent + QStringLiteral("/wtfml-backup.json")) &&
              !QFile::exists(restoreParent + QStringLiteral("/level.dat")),
          "★★ ZQ4 躺在 zip 根上的清单没有落进用户的世界目录，"
          "存档那堆文件也没倒在上一层（前缀剥掉之后由我们补回来）");
    check(QFile::exists(restoredWorld + QStringLiteral("/sub/assets/keep.txt")) &&
              QFile::exists(restoredWorld + QStringLiteral("/.session.lock")),
          "ZQ4 嵌套的、名字里带点的都回来了");

    // ② 还原到"已经有一份"的地方：必须**先**做安全快照，再整个挪走。
    // 往里塞一个备份里没有的文件 + 手改 level.dat —— 这两样如果没有被安全快照收走，
    // 就在"还原"这个动作里**没了**。
    writeFileAt(restoredWorld + QStringLiteral("/只在这儿有.txt"), QByteArrayLiteral("OLD-ONLY"));
    writeFileAt(restoredWorld + QStringLiteral("/level.dat"), QByteArrayLiteral("CHANGED-BY-HAND"));
    const BackupFiles::RestoreResult over = BackupFiles::restoreBackup(worldZip, restoreParent);
    check(over.ok && !over.safetyZip.isEmpty() && QFile::exists(over.safetyZip),
          "★ ZQ4 还原之前先给「现在这一份」做了一份安全快照", over.problem);
    if (!over.safetyZip.isEmpty()) {
        bool safetyOk = false;
        QString safetyProblem;
        const BackupFiles::Manifest safety =
            BackupFiles::readManifest(over.safetyZip, &safetyOk, &safetyProblem);
        check(safetyOk && safety.safety,
              "★★ ZQ4 那一份是「自动做出来的」（清单里带着 safety 这个记号，界面才认得出它）",
              safetyProblem);
        check(safety.root == QStringLiteral("基地周目") &&
                  safety.sourceInstanceId == spec.sourceInstanceId,
              "ZQ4 安全快照记的还是原来那个前缀 / 来源实例（不然恢复回来会落到别的地方）");
        check(fileAt(safety.files, QStringLiteral("只在这儿有.txt")).sha1
                  == sha1Of(QByteArrayLiteral("OLD-ONLY")),
              "★ ZQ4 安全快照里真有东西（那个「备份里没有、只在盘上」的文件也收进去了）");
        check(fileAt(safety.files, QStringLiteral("level.dat")).sha1
                  == sha1Of(QByteArrayLiteral("CHANGED-BY-HAND")),
              "★★ ZQ4 安全快照是**改之前**那一份 —— 顺序反了的话这里会是解出来的内容");
    }
    check(!QFile::exists(restoredWorld + QStringLiteral("/只在这儿有.txt")),
          "ZQ4 旧的整个挪走了（备份里没有的那个文件跟着一起走）");
    check(readAll(restoredWorld + QStringLiteral("/level.dat")) == QByteArrayLiteral("LEVELDATA"),
          "ZQ4 手改过的那一份被备份里的内容盖回去了");

    {
        const QString atomicParent = work + QStringLiteral("/atomic-restore");
        const QString atomicWorld = atomicParent + QStringLiteral("/基地周目");
        const QString oldData = atomicWorld + QStringLiteral("/level.dat");
        writeFileAt(oldData, "KEEP-ORIGINAL");
        const auto hasStaging = [&] {
            return !QDir(atomicParent).entryList({QStringLiteral(".wtfml-restore-*")},
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
        };
        bool stoppedDuringExtract = false;
        const auto cancelledExtract = BackupFiles::restoreBackup(worldZip, atomicParent, [&] {
            stoppedDuringExtract = hasStaging();
            return stoppedDuringExtract;
        });
        check(stoppedDuringExtract && !cancelledExtract.ok
                  && cancelledExtract.stage == BackupFiles::RestoreResult::Stage::Extract
                  && readAll(oldData) == QByteArrayLiteral("KEEP-ORIGINAL") && !hasStaging(),
              "ZQ4 暂存解压取消保留原目录并清理半成品", cancelledExtract.problem);
        bool changed = false;
        const auto changedDuringExtract = BackupFiles::restoreBackup(worldZip, atomicParent, [&] {
            if (!changed && hasStaging()) {
                changed = true;
                writeFileAt(oldData, "NEW-EXTERNAL-DATA");
            }
            return false;
        });
        check(changed && !changedDuringExtract.ok
                  && changedDuringExtract.stage == BackupFiles::RestoreResult::Stage::RemoveOld
                  && readAll(oldData) == QByteArrayLiteral("NEW-EXTERNAL-DATA") && !hasStaging(),
              "ZQ4 解压期间外部修改原存档拒绝替换且保留新内容", changedDuringExtract.problem);
        const auto success = BackupFiles::restoreBackup(worldZip, atomicParent);
        check(success.ok && readAll(oldData) == QByteArrayLiteral("LEVELDATA") && !hasStaging(),
              "ZQ4 稳定后可重试还原且清除旧目录和暂存", success.problem);
        const QString fileParent = work + QStringLiteral("/same-name-file");
        const QString fileTarget = fileParent + QStringLiteral("/基地周目");
        writeFileAt(fileTarget, "DO-NOT-DELETE");
        const auto fileBlocked = BackupFiles::restoreBackup(worldZip, fileParent);
        check(!fileBlocked.ok && readAll(fileTarget) == QByteArrayLiteral("DO-NOT-DELETE"),
              "ZQ4 同名普通文件不再被删除", fileBlocked.problem);
    }

    // ③ 拿别人的包来还原：**在动盘之前**就停下，一个字节都不动。
    writeFileAt(restoredWorld + QStringLiteral("/level.dat"), QByteArrayLiteral("STILL-HERE"));
    const BackupFiles::RestoreResult refused = BackupFiles::restoreBackup(foreignZip, restoreParent);
    check(!refused.ok && refused.stage == BackupFiles::RestoreResult::Stage::ReadManifest &&
              !refused.problem.isEmpty(),
          "★★ ZQ4 别人的包 → 第一步（读清单）就停下，连「第几步」都指得出来"
          "（账本上那几步靠它，不去抠中文里的关键字）",
          refused.problem);
    check(readAll(restoredWorld + QStringLiteral("/level.dat")) == QByteArrayLiteral("STILL-HERE"),
          "★★ ZQ4 拒绝的时候现在这一份**一个字节都没动**（读清单在动盘之前）");

    // ③' ★ ：落点已经是个**空目录**时照常还原，不许卡死在这一档。
    // 原来只要落点是个已存在的目录就无条件先做安全快照，而空目录的 `writeBackup()`
    // 必然失败（「这个目录是空的，没什么可备份的」）⇒ 之后每一次还原都停在
    // 安全快照那一档，界面只说"目录没有被改动"，用户没有任何出口（上一次解压被杀在
    // 第一份文件之前留下的正是这么一个空目录）。
    {
        const QString emptyParent = work + QStringLiteral("/空落点");
        QDir().mkpath(emptyParent + QStringLiteral("/基地周目"));
        const BackupFiles::RestoreResult ontoEmpty =
            BackupFiles::restoreBackup(worldZip, emptyParent);
        check(ontoEmpty.ok && ontoEmpty.safetyZip.isEmpty() &&
                  readAll(emptyParent + QStringLiteral("/基地周目/level.dat")) ==
                      QByteArrayLiteral("LEVELDATA"),
              "★★ ZQ4 落点是个空目录 ⇒ 照常还原（没有「现在这一份」可留，不算失败）",
              ontoEmpty.problem);
    }

    // ③''' ★ ：落点树里有链接（junction / 符号链接）⇒ **拒绝还原**，动盘之前停。
    // 包里和安全快照里都记不住"这里是个链接"，而 `removeTreeWithoutFollowingLinks()`
    // 会把链接本身摘掉、再按包里那份普通目录解出来 —— 链接指向的内容不会丢，但链接
    // 这件事退不回来。
    {
        const QString linkedParent = work + QStringLiteral("/有链接的落点");
        const QString linkedWorld = linkedParent + QStringLiteral("/基地周目");
        QDir().mkpath(linkedWorld);
        writeFileAt(linkedWorld + QStringLiteral("/level.dat"),
                    QByteArrayLiteral("KEEP-LINKED"));
        const QString outside = work + QStringLiteral("/链接那一头");
        QDir().mkpath(outside);
        writeFileAt(outside + QStringLiteral("/外面的.txt"), QByteArrayLiteral("OUTSIDE"));
        const QString linkPath = linkedWorld + QStringLiteral("/共享目录");
        const bool madeLink =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"),
                               QStringLiteral("/J"), QDir::toNativeSeparators(linkPath),
                               QDir::toNativeSeparators(outside)}) == 0
            && QFileInfo(linkPath).isJunction();
        check(madeLink, "★ ZQ4' 先在落点里造一根真 junction（mklink /J）");
        if (madeLink) {
            const BackupFiles::RestoreResult refusedLink =
                BackupFiles::restoreBackup(worldZip, linkedParent);
            check(!refusedLink.ok &&
                      refusedLink.stage == BackupFiles::RestoreResult::Stage::SafetySnapshot &&
                      refusedLink.safetyZip.isEmpty() &&
                      refusedLink.problem.contains(QStringLiteral("链接")),
                  "★★ ZQ4' 落点里有链接 ⇒ 拒绝还原、停在安全快照之前"
                  "（链接换成普通目录就退不回来了）",
                  refusedLink.problem);
            check(readAll(linkedWorld + QStringLiteral("/level.dat"))
                          == QByteArrayLiteral("KEEP-LINKED") &&
                      QFileInfo(linkPath).isJunction() &&
                      QFileInfo(outside + QStringLiteral("/外面的.txt")).isFile(),
                  "★★ ZQ4' 拒绝之后旧数据 / 链接本身 / 链接那一头全都原样");
            check(QDir().rmdir(linkPath), "★ ZQ4' 清理 junction（只摘链接本体）");
        }
    }

    // ③'' ★ ：还原接上停止条件（关窗 / 整项 600 秒），但**只在挪走旧那一份
    // 之前**生效。三种停法各钉一条：一开始就停、读清单之后停、安全快照做完停。
    {
        const QString cancelParent = work + QStringLiteral("/取消还原");
        QDir().mkpath(cancelParent + QStringLiteral("/基地周目"));
        writeFileAt(cancelParent + QStringLiteral("/基地周目/level.dat"),
                    QByteArrayLiteral("KEEP-ME"));
        const BackupFiles::RestoreResult never =
            BackupFiles::restoreBackup(worldZip, cancelParent, [] { return true; });
        check(!never.ok && never.stage == BackupFiles::RestoreResult::Stage::ReadManifest &&
                  never.problem.contains(QStringLiteral("取消")),
              "★★ ZQ4 一开始就要停 ⇒ 读清单那一步停下，话里说得出「取消或超时」",
              never.problem);
        check(readAll(cancelParent + QStringLiteral("/基地周目/level.dat"))
                  == QByteArrayLiteral("KEEP-ME"),
              "★★ ZQ4 一开始就停时盘上原样（连安全快照都没做）");

        // 第二次才停：入口那一次放行，走到「空间检查」之前那一次停下 —— 一个字节都没动。
        int asked = 0;
        const BackupFiles::RestoreResult early =
            BackupFiles::restoreBackup(worldZip, cancelParent, [&asked] { return ++asked >= 2; });
        check(!early.ok && early.stage == BackupFiles::RestoreResult::Stage::Space &&
                  early.safetyZip.isEmpty(),
              "★★ ZQ4 动盘之前停下：不做安全快照、不解压", early.problem);
        check(readAll(cancelParent + QStringLiteral("/基地周目/level.dat"))
                  == QByteArrayLiteral("KEEP-ME"),
              "★★ ZQ4 动盘之前停下时旧那一份完好（取消不等于删旧数据）");

        // 安全快照做完、旧目录还没动：这一档也必须收得住。判据是"备份目录里多了一份" ——
        // 打包途中的检查点都还在写，只有发布之后才多出来。
        const QString backupRoot = BackupFiles::backupDir(BackupFiles::Kind::World);
        const auto zipCount = [&backupRoot] {
            return QDir(backupRoot).entryList(QStringList{QStringLiteral("*.zip")}).size();
        };
        const int before = zipCount();
        const BackupFiles::RestoreResult late = BackupFiles::restoreBackup(
            worldZip, cancelParent, [&] { return zipCount() > before; });
        check(!late.ok && late.stage == BackupFiles::RestoreResult::Stage::SafetySnapshot &&
                  late.safetyZip.isEmpty(),
              "★★ ZQ4 安全快照做完、旧目录还没动时停下", late.problem);
        check(zipCount() == before,
              "★★ ZQ4 停下时把这一趟刚做的安全快照删掉（不在列表里留下没要过的东西）");
        check(readAll(cancelParent + QStringLiteral("/基地周目/level.dat"))
                  == QByteArrayLiteral("KEEP-ME"),
              "★★ ZQ4 这一档停下同样不碰旧那一份");
    }

    // ④ 解出来了但对不上账：手搓一个"清单说 A、包里是 B"的包。
    {
        const QString tamper = work + QStringLiteral("/对不上");
        const QByteArray realBytes = QByteArrayLiteral("真内容");
        writeFileAt(tamper + QStringLiteral("/云存档/level.dat"), realBytes);
        BackupFiles::Manifest lying;
        lying.kind = BackupFiles::Kind::World;
        lying.root = QStringLiteral("云存档");
        lying.label = QStringLiteral("云存档");
        lying.createdAt = at;
        BackupFiles::FileRecord rec;
        rec.path = QStringLiteral("level.dat");
        rec.size = realBytes.size();                     // 长短先对上
        rec.sha1 = sha1Of(QByteArrayLiteral("另一个内容"));  // 内容对不上
        lying.files = QList<BackupFiles::FileRecord>{rec};
        const QString lyingJson = work + QStringLiteral("/说谎的清单.json");
        writeFileAt(lyingJson, BackupFiles::manifestToJson(lying));
        const QString lyingZip = work + QStringLiteral("/对不上.zip");
        QString lyingZipError;
        check(ZipUtil::createZip(
                  lyingZip,
                  {{BackupFiles::manifestEntryName(), lyingJson},
                   {QStringLiteral("云存档/level.dat"),
                    tamper + QStringLiteral("/云存档/level.dat")}},
                  &lyingZipError),
              "ZQ4 手搓一个「清单和内容对不上」的包", lyingZipError);
        const BackupFiles::RestoreResult mismatch = BackupFiles::restoreBackup(lyingZip, work);
        check(!mismatch.ok && mismatch.stage == BackupFiles::RestoreResult::Stage::Verify &&
                  mismatch.mismatched == 1 && mismatch.checked == 1,
              "★★ ZQ4 解得开但对不上账 → 报出对不上的条数"
              "（「文件在那儿」和「文件是对的」是两件事）",
              mismatch.problem);
        check(!QFile::exists(work + QStringLiteral("/云存档")),
              "ZQ4 对不上账不发布半成品到正式目录");
        const QString oldFile = work + QStringLiteral("/云存档/level.dat");
        writeFileAt(oldFile, QByteArrayLiteral("PRESERVE-ORIGINAL"));
        const auto mismatchOver = BackupFiles::restoreBackup(lyingZip, work);
        check(!mismatchOver.ok && mismatchOver.stage == BackupFiles::RestoreResult::Stage::Verify
                  && readAll(oldFile) == QByteArrayLiteral("PRESERVE-ORIGINAL"),
              "ZQ4 损坏备份还原失败后原存档逐字节保持不变", mismatchOver.problem);
        check(QDir(work).entryList({QStringLiteral(".wtfml-restore-*")},
                  QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
              "ZQ4 校验失败清理私有还原暂存");
    }

    // ⑤ **安全快照做不成就不许动** —— 这一整套里唯一不可接受的结果是"先把旧的删了、
    // 再回来告诉你备份没做出来"。办法：把实例快照那个备份目录的位置摆成一个**文件**，
    // 于是 mkpath 建不出来、zip 也落不了地。
    {
        const QString instanceDir = work + QStringLiteral("/我的实例");
        writeFileAt(instanceDir + QStringLiteral("/options.txt"), QByteArrayLiteral("fov:70"));
        writeFileAt(instanceDir + QStringLiteral("/assets/big.bin"), QByteArrayLiteral("60 MB 假装"));
        writeFileAt(instanceDir + QStringLiteral("/mods/某模组/data/assets/pack.png"),
                    QByteArrayLiteral("模组自己的"));
        BackupFiles::Manifest snapSpec;
        snapSpec.kind = BackupFiles::Kind::Instance;
        snapSpec.root = QStringLiteral("我的实例");
        snapSpec.label = QStringLiteral("我的实例");
        snapSpec.excluded = QStringList{QStringLiteral("assets"), QStringLiteral("libraries")};
        const QString snapZip = work + QStringLiteral("/实例快照.zip");
        const BackupFiles::WriteResult snapWrite =
            BackupFiles::writeBackup(snapSpec, instanceDir, snapZip);
        check(snapWrite.ok, "ZQ5 实例快照也打得出来", snapWrite.problem);
        check(fileAt(snapWrite.manifest.files, QStringLiteral("assets/big.bin")).path.isEmpty() &&
                  fileAt(snapWrite.manifest.files, QStringLiteral("mods/某模组/data/assets/pack.png"))
                          .path
                      == QStringLiteral("mods/某模组/data/assets/pack.png"),
              "★★ ZQ5 快照默认把能重新下回来的那两份（assets / libraries）排除掉了，"
              "但模组自己带的 assets 一个不落");

        const QString instanceBackups = BackupFiles::backupDir(BackupFiles::Kind::Instance);
        QDir().mkpath(QFileInfo(instanceBackups).absolutePath());
        writeFileAt(instanceBackups, QByteArrayLiteral("占住这个名字"));  // ← 让安全快照落不了地

        writeFileAt(instanceDir + QStringLiteral("/options.txt"), QByteArrayLiteral("fov:90"));
        const BackupFiles::RestoreResult blocked = BackupFiles::restoreBackup(snapZip, work);
        check(!blocked.ok && blocked.stage == BackupFiles::RestoreResult::Stage::SafetySnapshot &&
                  blocked.problem.contains(QStringLiteral("原有内容已保留")),
              "★★ ZQ5 安全快照做不出来时当场停下，并且**明确说这个目录没被动过**",
              blocked.problem);
        check(readAll(instanceDir + QStringLiteral("/options.txt")) == QByteArrayLiteral("fov:90") &&
                  QFile::exists(instanceDir + QStringLiteral("/assets/big.bin")),
              "★★ ZQ5 那一趟现在这一份**一个字节都没动**（先删后说不可接受）");

        QFile::remove(instanceBackups);
        QDir().mkpath(instanceBackups);  // 还回去，别影响后面
    }

    // ---------- ZQ6. 落点、文件名、范围那一句 ----------
    check(BackupFiles::backupRoot() == temp.path() + QStringLiteral("/cache/backups") &&
              BackupFiles::backupDir(BackupFiles::Kind::World)
                  == BackupFiles::backupRoot() + QStringLiteral("/worlds") &&
              BackupFiles::backupDir(BackupFiles::Kind::Instance)
                  == BackupFiles::backupRoot() + QStringLiteral("/instances"),
          "ZQ6 备份落在 <数据目录>/cache/backups/{worlds,instances}",
          BackupFiles::backupRoot());
    const QString oddName = BackupFiles::backupFileName(QStringLiteral("基地:周目/一"), at);
    check(oddName == QStringLiteral("基地-周目-一-2026-10-13-101530.zip"),
          "★ ZQ6 文件名里的非法字符换成 `-`，时间戳定宽（按名字排序 = 按时间排序）", oddName);
    check(BackupFiles::backupFileName(QStringLiteral("基地周目"), at, true)
              == QStringLiteral("基地周目-恢复前-2026-10-13-101530.zip"),
          "ZQ6 安全快照那一份文件名里带「恢复前」");
    check(BackupFiles::backupFileName(QStringLiteral("基地：周目"), at)
              .startsWith(BackupFiles::backupFilePrefix(QStringLiteral("基地：周目"))),
          "★★ ZQ6 前缀筛子和文件名是**同一把尺子**"
          "（不是的话用户自己的备份在列表里就看不见了）");
    check(BackupFiles::backupFileName(QStringLiteral(" . "), at)
              == QStringLiteral("backup-2026-10-13-101530.zip"),
          "ZQ6 光剩下点和空格的 label 退回 `backup`（Windows 会把结尾的点悄悄吃掉，"
          "两次备份就撞成同一个名字了）");
    check(BackupFiles::backupFilePrefix(QString(200, QLatin1Char('a'))).size() == 81,
          "ZQ6 label 太长时截到 80", QString::number(BackupFiles::backupFilePrefix(
                                            QString(200, QLatin1Char('a'))).size()));
    check(BackupFiles::scopeLine(QStringLiteral("saves/基地周目"), 6)
              == QStringLiteral("saves/基地周目（6 个文件）"),
          "ZQ6 范围那一句的唯一出处");

    // ---------- ZQ6'. 挑备份那一列的顺序（「越上面越新」） ----------
    {
        const QString label = QStringLiteral("新世界");
        const QString prefix = BackupFiles::backupFilePrefix(label);
        const QString dir = BackupFiles::backupDir(BackupFiles::Kind::World) + QLatin1Char('/');
        const QString newerFile = dir + BackupFiles::backupFileName(
                                            label, QDateTime(QDate(2026, 10, 19), QTime(21, 30)));
        const QString safetyFile = dir + BackupFiles::backupFileName(
                                             label, QDateTime(QDate(2026, 10, 19), QTime(21, 30)),
                                             /*safety=*/true);
        const QString midFile = dir + BackupFiles::backupFileName(
                                          label, QDateTime(QDate(2026, 10, 10), QTime(9, 0)));
        const QString oldFile = dir + BackupFiles::backupFileName(
                                          label, QDateTime(QDate(2026, 10, 1), QTime(8, 0)));
        const QStringList sorted = BackupFiles::sortBackupsNewestFirst(
            QStringList{oldFile, safetyFile, midFile, newerFile}, prefix);
        // 原来按整名字典序倒排 —— 安全快照名字里那段汉字码位在数字之后，
        // 于是**每一份安全快照都排到了最新那份的上面**，"越上面越新"当场不成立。
        check(sorted.size() == 4 && sorted.value(0) == safetyFile && sorted.value(1) == newerFile,
              "★★ ZQ6' 「恢复前-」安全快照不会因为汉字排在数字后面就被摆到最新那份前面"
              "（同一秒里它在前、常规那份紧跟）",
              sorted.join(QLatin1Char('|')));
        check(sorted.value(2) == midFile && sorted.value(3) == oldFile,
              "★ ZQ6' 剩下的按时间从新到旧", sorted.join(QLatin1Char('|')));
        const QStringList junk = BackupFiles::sortBackupsNewestFirst(
            QStringList{dir + QStringLiteral("新世界-随手放的一份.zip"), newerFile}, prefix);
        check(junk.size() == 2 && junk.value(0) == newerFile,
              "★ ZQ6' 名字里认不出时间戳的那一份排最后（宁可往下摆，也不能顶掉最新那份）",
              junk.join(QLatin1Char('|')));
    }

    // ---------- ZQ7. 还原之后的逐文件对账（verifyTree）单独钉一遍 ----------
    const QString checkDir = work + QStringLiteral("/对账");
    writeFileAt(checkDir + QStringLiteral("/a.txt"), QByteArrayLiteral("AAAA"));
    writeFileAt(checkDir + QStringLiteral("/b.txt"), QByteArrayLiteral("BBBBBB"));
    writeFileAt(checkDir + QStringLiteral("/多出来的.txt"), QByteArrayLiteral("多说一个不算错"));

    const BackupFiles::FileRecord goodA = recordOf(QStringLiteral("a.txt"), QByteArrayLiteral("AAAA"));
    const BackupFiles::FileRecord goodB =
        recordOf(QStringLiteral("b.txt"), QByteArrayLiteral("BBBBBB"));
    check(BackupFiles::verifyTree(checkDir, {goodA, goodB}) == 0,
          "ZQ7 全对上时 0 条对不上");
    check(BackupFiles::verifyTree(checkDir, {goodA, goodB}, [] { return true; }) > 0,
          "ZQ7 校验可取消且不能误报通过");
    check(BackupFiles::verifyTree(
              checkDir, {goodA, recordOf(QStringLiteral("没有这个.txt"), QByteArrayLiteral("x"))})
              == 1,
          "ZQ7 缺一个算一条");
    BackupFiles::FileRecord shortOne = goodA;
    shortOne.size += 1;
    check(BackupFiles::verifyTree(checkDir, {shortOne}) == 1,
          "ZQ7 长短不对就算不对（不用再算哈希）");
    BackupFiles::FileRecord sameSizeWrongBody = goodB;
    sameSizeWrongBody.sha1 = sha1Of(QByteArrayLiteral("CCCCCC"));
    check(BackupFiles::verifyTree(checkDir, {sameSizeWrongBody}) == 1,
          "★ ZQ7 长短一样但内容不一样 → 也算不对（只有哈希认得出这种）");
    BackupFiles::FileRecord noHash = goodA;
    noHash.sha1.clear();
    check(BackupFiles::verifyTree(checkDir, {noHash}) == 0,
          "ZQ7 清单里本来就没记校验值（手改过的清单）→ 查不了，不算它错");
    check(BackupFiles::verifyTree(
              checkDir, {recordOf(QStringLiteral("多出来的.txt"), QByteArrayLiteral("多说一个不算错"))})
              == 0,
          "ZQ7 备份里没有、盘上多出来的文件不算对不上（备份是「至少恢复这些」）");

    // ---------- ZQ8. 坏场景：包坏了 / 盘不够----------
    //
    // 三个坏场景里，「**中断**」那一支已经在 ZQ7 钉过（长短不对 = 解到一半被打断留下的
    // 半截文件，`verifyTree` 的 size 那一支认得出），ZQ4 ④ 也钉了端到端那一趟。这里补上
    // 另外两个：① **包本身坏了**（读到一半断掉 / 根本不是包）；② **盘放不下**。
    {
        // ① 包坏了：把一份好好的包**切掉后半截**（zip 的中央目录在尾巴上，切了就读不动）。
        const QByteArray wholeZip = readAll(worldZip);
        check(!wholeZip.isEmpty(), "ZQ8 手上先得有一份好的包（ZQ3 打出来的那份）");
        const QString brokenZip = work + QStringLiteral("/截断的包.zip");
        writeFileAt(brokenZip, wholeZip.left(200));
        bool brokenOk = true;
        QString brokenProblem;
        BackupFiles::readManifest(brokenZip, &brokenOk, &brokenProblem);
        check(!brokenOk && !brokenProblem.isEmpty(),
              "★★ ZQ8 半截的包读不动，而且**说得出一句话**（不是静默回一份空清单）",
              brokenProblem);
        writeFileAt(restoredWorld + QStringLiteral("/level.dat"), QByteArrayLiteral("STILL-HERE-2"));
        const BackupFiles::RestoreResult fromBroken =
            BackupFiles::restoreBackup(brokenZip, restoreParent);
        check(!fromBroken.ok && fromBroken.stage == BackupFiles::RestoreResult::Stage::ReadManifest &&
                  readAll(restoredWorld + QStringLiteral("/level.dat"))
                      == QByteArrayLiteral("STILL-HERE-2"),
              "★★ ZQ8 拿坏包来还原：第一步就停下，现在这一份一个字节没动",
              fromBroken.problem);

        // ② 盘放不下：先钉纯函数那一条（判据的出处）。
        BackupFiles::Manifest small;
        small.kind = BackupFiles::Kind::World;
        small.root = QStringLiteral("小世界");
        small.createdAt = at;
        small.files = {recordOf(QStringLiteral("a"), QByteArrayLiteral("0123456789")),
                       recordOf(QStringLiteral("b"), QByteArrayLiteral("abcdefghij"))};
        const qint64 need = BackupFiles::restoreSpaceNeeded(small);
        check(need > 20,
              "★ ZQ8 「要多少空间」= 清单里各文件大小之和**再加一点余量**（不是刚好那 20 个字节）",
              QString::number(need));
        check(BackupFiles::hasRoomFor(need, need) && BackupFiles::hasRoomFor(need + 1, need)
                  && !BackupFiles::hasRoomFor(need - 1, need),
              "ZQ8 够 / 多一个 / 差一个：三种都判对");
        check(BackupFiles::hasRoomFor(-1, need),
              "★★ ZQ8 空闲空间**问不出来**（-1）时**不拦**"
              "（宁可走到解压那一步如实失败，也不凭一个假数字挡人）");

        // ② 端到端：手搓一个清单里写着"1 PB"的包，落到一块**空地**上。
        const QString hugeParent = work + QStringLiteral("/塞不下");
        QDir().mkpath(hugeParent);
        const QByteArray tiny = QByteArrayLiteral("小得可怜");
        const QString srcUnder = work + QStringLiteral("/源/大实例/region/r.0.0.mca");
        writeFileAt(srcUnder, tiny);
        BackupFiles::Manifest hugeManifest;
        hugeManifest.kind = BackupFiles::Kind::Instance;
        hugeManifest.root = QStringLiteral("大实例");
        hugeManifest.label = QStringLiteral("大实例");
        hugeManifest.createdAt = at;
        BackupFiles::FileRecord hugeRec = recordOf(QStringLiteral("region/r.0.0.mca"), tiny);
        hugeRec.size = 1LL << 50;  // 1 PB（清单在吹牛，反正轮不到解压那一步）
        hugeManifest.files = {hugeRec};
        const QString hugeJson = work + QStringLiteral("/大清单.json");
        writeFileAt(hugeJson, BackupFiles::manifestToJson(hugeManifest));
        const QString hugeZip = work + QStringLiteral("/塞不下.zip");
        QString hugeZipError;
        check(ZipUtil::createZip(hugeZip,
                                 {{BackupFiles::manifestEntryName(), hugeJson},
                                  {QStringLiteral("大实例/region/r.0.0.mca"), srcUnder}},
                                 &hugeZipError),
              "ZQ8 手搓一个「清单说要 1 PB」的包", hugeZipError);
        const BackupFiles::RestoreResult noRoom = BackupFiles::restoreBackup(hugeZip, hugeParent);
        check(!noRoom.ok && noRoom.stage == BackupFiles::RestoreResult::Stage::Space &&
                  noRoom.problem.contains(QStringLiteral("目标磁盘空间不足")),
              "★★ ZQ8 空地那块盘放不下 → 在**动盘之前**就停下，而且话说得明白", noRoom.problem);
        check(!QFileInfo::exists(hugeParent + QStringLiteral("/大实例")),
              "★★ ZQ8 停下时那地上**什么都没建**"
              "（看空间这一眼排在安全快照和解压之前，盘上一个字节都没动）");

        // ③ 原地安全发布也需要同时容纳新旧目录；空间不足必须在快照/替换前拒绝。
        writeFileAt(hugeParent + QStringLiteral("/大实例/regions-先来的.txt"),
                    QByteArrayLiteral("OLD"));
        const BackupFiles::RestoreResult inPlace = BackupFiles::restoreBackup(hugeZip, hugeParent);
        check(!inPlace.ok && inPlace.stage == BackupFiles::RestoreResult::Stage::Space
                  && inPlace.safetyZip.isEmpty()
                  && readAll(hugeParent + QStringLiteral("/大实例/regions-先来的.txt")) == QByteArrayLiteral("OLD")
                  && QDir(hugeParent).entryList({QStringLiteral(".wtfml-restore-*")},
                       QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
              "★★ ZQ8 原地还原空间不足时提前拒绝、不做快照且原文件不变",
              inPlace.problem);
    }

    section("ZQ9. 世界 level.dat 的世代备份：**按「哪个实例的哪个世界」分格子**");
    // -----------------------------------------------------------------
    // 格子原来只按**存档目录名**分（`cache/world-backups/world/<时间戳>`），
    // 两个实例各有一个叫 `world`（中文环境里更常见的是都叫「新的世界」）的存档就共用
    // 一格。两件坏事：A 备份一次会把 B 的代数挤掉一份（清理按每格只留最近 N 代算），
    // 而 `latestWorldBackup()` 会拿到**另一个世界**的 level.dat —— 界面上那句提示正是
    // "要还原的话把那一份拷回存档目录"，用户照着做，拿到的就是别人的种子和地形设置。
    // 这一节拿两个同名的世界（分别在两个实例目录下）把两个方向都钉死。
    {
        const auto writeWorld = [](const QString &dir, const QByteArray &bytes) {
            QDir().mkpath(dir);
            writeFileAt(dir + QStringLiteral("/level.dat"), bytes);
        };
        const auto readAll = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const QString instanceA = temp.path() + QStringLiteral("/versions/实例甲");
        const QString instanceB = temp.path() + QStringLiteral("/versions/实例乙");
        const QString worldA = instanceA + QStringLiteral("/saves/world");
        const QString worldB = instanceB + QStringLiteral("/saves/world");
        // 两份内容必须一眼分得出（下面比的是字节，不是"有没有"）。
        writeWorld(worldA, QByteArrayLiteral("LEVEL-A"));
        writeWorld(worldB, QByteArrayLiteral("LEVEL-B"));

        QString why;
        check(InstanceMaintenanceController::backupWorldLevelData(worldA, 20, &why),
              "ZQ9 甲实例的 world 备份成功", why);
        check(InstanceMaintenanceController::backupWorldLevelData(worldB, 20, &why),
              "ZQ9 乙实例的 world 也备份成功（同名不冲突）", why);

        const QString latestA = InstanceMaintenanceController::latestWorldBackup(worldA);
        const QString latestB = InstanceMaintenanceController::latestWorldBackup(worldB);
        check(!latestA.isEmpty() && !latestB.isEmpty(), "ZQ9 两边的「最近一份」都找得到",
              latestA + QStringLiteral(" / ") + latestB);
        check(latestA != latestB,
              "★★ ZQ9 两个实例的同名世界**各自一格**（不是同一个 path 被两边当成自己的）",
              latestA + QStringLiteral(" / ") + latestB);
        check(readAll(latestA) == QByteArrayLiteral("LEVEL-A"),
              "★★ ZQ9 甲实例查到的「最近一份」是**甲自己的** level.dat（不是乙的）",
              QString::fromLatin1(readAll(latestA)));
        check(readAll(latestB) == QByteArrayLiteral("LEVEL-B"),
              "★★ ZQ9 乙实例查到的「最近一份」是**乙自己的** level.dat",
              QString::fromLatin1(readAll(latestB)));
        check(QDir::cleanPath(latestA).contains(QStringLiteral("实例甲")),
              "ZQ9 备份路径里认得出是哪个实例（不是一串看不懂的哈希）", latestA);

        // 清理是**按格**算代数的：乙再备份一次（keep=1）只能删乙自己那一代，
        // 甲那一份必须原样在 —— 这正是"共用一格"时会被误删的那种东西。
        check(InstanceMaintenanceController::backupWorldLevelData(worldB, 1, &why),
              "ZQ9 乙实例再备份一次（keep=1）", why);
        const QString latestAAfter = InstanceMaintenanceController::latestWorldBackup(worldA);
        check(!latestAAfter.isEmpty() && readAll(latestAAfter) == QByteArrayLiteral("LEVEL-A"),
              "★★ ZQ9 乙清自己的老代数时**没有**把甲那一份一起删掉", latestAAfter);
        const QString slotB = QFileInfo(QFileInfo(latestB).absolutePath()).absolutePath();
        const int gensB = QDir(slotB).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        check(gensB == 1, "ZQ9 乙那一格按 keep=1 收成了一代", QString::number(gensB));

        const QString instanceSmp = temp.path() + QStringLiteral("/versions/SMP");
        const QString instanceSmp2024 = temp.path() + QStringLiteral("/versions/SMP__2024");
        const QString worldSmpOld = instanceSmp + QStringLiteral("/saves/2024__old");
        const QString worldSmp2024 = instanceSmp2024 + QStringLiteral("/saves/old");
        writeWorld(worldSmpOld, QByteArrayLiteral("LEVEL-SMP"));
        writeWorld(worldSmp2024, QByteArrayLiteral("LEVEL-SMP2024"));
        check(InstanceMaintenanceController::backupWorldLevelData(worldSmpOld, 20, &why),
              "ZQ9 名里带 __ 的那一对（甲）备份成功", why);
        check(InstanceMaintenanceController::backupWorldLevelData(worldSmp2024, 20, &why),
              "ZQ9 名里带 __ 的那一对（乙）备份成功", why);
        const QString latestSmpOld = InstanceMaintenanceController::latestWorldBackup(worldSmpOld);
        const QString latestSmp2024 =
            InstanceMaintenanceController::latestWorldBackup(worldSmp2024);
        check(!latestSmpOld.isEmpty() && !latestSmp2024.isEmpty()
                  && latestSmpOld != latestSmp2024,
              "★★ ZQ9 「SMP / 2024__old」和「SMP__2024 / old」各占一格（`__` 拼出来的"
              "同形格子名不许再撞）",
              latestSmpOld + QStringLiteral(" / ") + latestSmp2024);
        check(readAll(latestSmpOld) == QByteArrayLiteral("LEVEL-SMP")
                  && readAll(latestSmp2024) == QByteArrayLiteral("LEVEL-SMP2024"),
              "★★ ZQ9 两格里的 level.dat 各是各的",
              QString::fromLatin1(readAll(latestSmpOld)) + QStringLiteral(" / ")
                  + QString::fromLatin1(readAll(latestSmp2024)));
    }

    section("ZQ10. 还原事务中断恢复：归位、冲突、锁与路径边界");
    {
        const QString root = work + QStringLiteral("/restore-journal-tests");
        QDir().mkpath(RestoreJournal::directory(root));
        const auto fixture = [&](const QString &name, const QString &state) {
            const QString target = QDir(root).filePath(name);
            const QString retained = QDir(root).filePath(QStringLiteral(".wtfml-restore-previous-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces));
            auto json = RestoreJournal::record(target, retained, true);
            json.insert(QStringLiteral("state"), state);
            const QString path = QDir(RestoreJournal::directory(root)).filePath(
                QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
            writeFileAt(retained + QStringLiteral("/level.dat"), "ORIGINAL");
            check(RestoreJournal::write(path, json), "ZQ10 中断事务日志写入成功");
            return QStringList{target, retained, path};
        };
        const auto interrupted = fixture(QStringLiteral("world"), QStringLiteral("prepared"));
        const auto recovered = BackupFiles::recoverInterruptedRestores(root);
        check(!recovered.isEmpty() && readAll(interrupted[0] + QStringLiteral("/level.dat")) == "ORIGINAL"
                  && !QFileInfo::exists(interrupted[1]) && !QFileInfo::exists(interrupted[2]),
              "ZQ10 两次改名之间中断可归位原目录并移除本事务日志");
        check(BackupFiles::recoverInterruptedRestores(root).isEmpty(), "ZQ10 恢复操作幂等");
        const auto active = fixture(QStringLiteral("active"), QStringLiteral("prepared"));
        QLockFile busy(active[2] + QStringLiteral(".lock"));
        busy.setStaleLockTime(0);
        check(busy.tryLock(0), "ZQ10 活动事务持有独立锁");
        BackupFiles::recoverInterruptedRestores(root);
        check(!QFileInfo::exists(active[0]) && QFileInfo::exists(active[1]),
              "ZQ10 活动事务不可被启动恢复抢占");
        busy.unlock();
        BackupFiles::recoverInterruptedRestores(root);
        check(readAll(active[0] + QStringLiteral("/level.dat")) == "ORIGINAL",
              "ZQ10 释放事务锁后可恢复");
        const auto conflict = fixture(QStringLiteral("conflict"), QStringLiteral("prepared"));
        writeFileAt(conflict[0] + QStringLiteral("/level.dat"), "NEW-TARGET");
        BackupFiles::recoverInterruptedRestores(root);
        check(readAll(conflict[0] + QStringLiteral("/level.dat")) == "NEW-TARGET"
                  && readAll(conflict[1] + QStringLiteral("/level.dat")) == "ORIGINAL"
                  && QFileInfo::exists(conflict[2]), "ZQ10 存在新目标时保留双方并留下日志待处理");
        const auto reviews = RecoveryReview::inspect(root);
        const auto review = RecoveryReview::read(root, conflict[2]);
        QString reviewProblem;
        check(!reviews.isEmpty() && review.problem.isEmpty()
                  && review.target == conflict[0] && review.retained == conflict[1],
              "ZQ10 人工检查可读取待处理事务的双方目录");
        auto changedReview = review;
        changedReview.target += QStringLiteral("-changed");
        check(!RecoveryReview::acknowledge(root, changedReview, &reviewProblem)
                  && QFileInfo::exists(conflict[2]), "ZQ10 过期检查结果不能归档日志");
        QLockFile reviewLock(conflict[2] + QStringLiteral(".lock"));
        reviewLock.setStaleLockTime(0);
        check(reviewLock.tryLock(0) && !RecoveryReview::acknowledge(root, review, &reviewProblem),
              "ZQ10 活动事务不能标记已检查");
        reviewLock.unlock();
        check(RecoveryReview::acknowledge(root, review, &reviewProblem)
                  && !QFileInfo::exists(conflict[2])
                  && QFileInfo::exists(root + QStringLiteral("/cache/restore-reviewed/")
                                      + QFileInfo(conflict[2]).fileName())
                  && readAll(conflict[0] + QStringLiteral("/level.dat")) == "NEW-TARGET"
                  && readAll(conflict[1] + QStringLiteral("/level.dat")) == "ORIGINAL",
              "ZQ10 标记已检查仅归档自己的日志，两份数据原样保留", reviewProblem);
        const auto published = fixture(QStringLiteral("published"), QStringLiteral("published"));
        writeFileAt(published[0] + QStringLiteral("/level.dat"), "PUBLISHED");
        BackupFiles::recoverInterruptedRestores(root);
        check(readAll(published[0] + QStringLiteral("/level.dat")) == "PUBLISHED"
                  && QFileInfo::exists(published[1]), "ZQ10 已发布事务不回滚也不自动删旧目录");
        const auto outside = fixture(QStringLiteral("unsafe"), QStringLiteral("prepared"));
        auto forged = RestoreJournal::record(work + QStringLiteral("/outside-target"), outside[1], true);
        check(RestoreJournal::write(outside[2], forged), "ZQ10 写入越界测试日志");
        BackupFiles::recoverInterruptedRestores(root);
        check(!QFileInfo::exists(work + QStringLiteral("/outside-target"))
                  && QFileInfo::exists(outside[1]) && QFileInfo::exists(outside[2]),
              "ZQ10 越界日志不能移动任何原数据");
        const auto orphan = QDir(root).filePath(QStringLiteral(".wtfml-restore-previous-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        writeFileAt(orphan + QStringLiteral("/level.dat"), "UNREGISTERED");
        BackupFiles::recoverInterruptedRestores(root);
        check(readAll(orphan + QStringLiteral("/level.dat")) == "UNREGISTERED",
              "ZQ10 未登记旧目录不按前缀盲删");
    }
    section("ZQ11. 崩溃暂存：所有权、活动锁与分批清理");
    {
        const QString root = work + QStringLiteral("/owned-staging-tests");
        QDir().mkpath(root);
        QString orphan;
        {
            OwnedStaging::Lease lease;
            QTemporaryDir stage(QDir(root).filePath(QStringLiteral(".wtfml-backup-XXXXXX")));
            check(stage.isValid() && lease.attach(root, stage.path(), QStringLiteral("backup")),
                  "ZQ11 备份暂存所有权登记成功");
            orphan = stage.path();
            stage.setAutoRemove(false); // Simulate process exit without temporary cleanup.
            writeFileAt(orphan + QStringLiteral("/data/file"), "PARTIAL");
            OwnedStaging::cleanup(root);
            check(QFileInfo::exists(orphan + QStringLiteral("/data/file")),
                  "ZQ11 使用中的暂存受独立锁保护");
        }
        const auto cleared = OwnedStaging::cleanup(root);
        check(!cleared.isEmpty() && !QFileInfo::exists(orphan), "ZQ11 退出后清理已确认归属的暂存");
        check(QDir(OwnedStaging::directory(root)).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty(),
              "ZQ11 清理完成移除对应登记记录");
        check(OwnedStaging::cleanup(root).isEmpty(), "ZQ11 重复清理幂等");
        QString untrusted;
        {
            OwnedStaging::Lease lease;
            QTemporaryDir stage(QDir(root).filePath(QStringLiteral(".wtfml-restore-XXXXXX")));
            check(stage.isValid() && lease.attach(root, stage.path(), QStringLiteral("restore")),
                  "ZQ11 还原暂存所有权登记成功");
            untrusted = stage.path();
            stage.setAutoRemove(false);
            writeFileAt(untrusted + QStringLiteral("/payload/file"), "KEEP");
            writeFileAt(OwnedStaging::marker(untrusted), "{}");
        }
        OwnedStaging::cleanup(root);
        check(readAll(untrusted + QStringLiteral("/payload/file")) == "KEEP",
              "ZQ11 标记不一致不自动删除");
        const QString legacy = root + QStringLiteral("/.wtfml-backup-LEGACY");
        writeFileAt(legacy + QStringLiteral("/file"), "UNREGISTERED");
        OwnedStaging::cleanup(root);
        check(readAll(legacy + QStringLiteral("/file")) == "UNREGISTERED",
              "ZQ11 相同前缀的未登记目录不清理");
        QString large;
        {
            OwnedStaging::Lease lease;
            QTemporaryDir stage(QDir(root).filePath(QStringLiteral(".wtfml-backup-XXXXXX")));
            check(stage.isValid() && lease.attach(root, stage.path(), QStringLiteral("backup")),
                  "ZQ11 多文件暂存登记成功");
            large = stage.path();
            stage.setAutoRemove(false);
            for (int i = 0; i < 600; ++i) writeFileAt(large + QStringLiteral("/file-%1").arg(i), "x");
        }
        OwnedStaging::cleanup(root);
        check(QFileInfo::exists(large) && QFileInfo::exists(OwnedStaging::marker(large)),
              "ZQ11 清理有数量预算且保留所有权标记以便续扫");
        for (int i = 0; i < 8 && QFileInfo::exists(large); ++i) OwnedStaging::cleanup(root);
        check(!QFileInfo::exists(large), "ZQ11 后续批次可完成剩余清理");
        const QString retained = root + QStringLiteral("/.wtfml-restore-previous-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeFileAt(retained + QStringLiteral("/level.dat"), "ORIGINAL");
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject forged{{"schema", 1}, {"token", token}, {"path", retained}, {"kind", "restore"}};
        check(RestoreJournal::write(QDir(OwnedStaging::directory(root)).filePath(token + ".json"), forged),
              "ZQ11 写入旧目录伪登记测试记录");
        OwnedStaging::cleanup(root);
        check(readAll(retained + QStringLiteral("/level.dat")) == "ORIGINAL",
              "ZQ11 旧目录即使有伪登记也不在暂存清理范围内");
    }
    section("ZQ12. 人工恢复目录：用旧目录换回来，两边都留着");
    {
        const QString root = work + QStringLiteral("/manual-recovery-tests");
        const QString target = root + QStringLiteral("/instance");
        const QString oldDir = root + QStringLiteral("/.wtfml-restore-previous-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeFileAt(target + QStringLiteral("/level.dat"), "CURRENT");
        writeFileAt(oldDir + QStringLiteral("/level.dat"), "OLD");
        QDir().mkpath(RestoreJournal::directory(root));
        const QString journalPath = QDir(RestoreJournal::directory(root)).filePath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
        check(RestoreJournal::write(journalPath, RestoreJournal::record(target, oldDir, true)),
              "ZQ12 还原事务留下的现场记录写好了");
        const auto journalCount = [&] {
            return QDir(RestoreJournal::directory(root))
                .entryList({QStringLiteral("*.json")}, QDir::Files).size();
        };

        // 「不自动清理未知暂存」：这一趟一个 removeTree 都不该碰。
        const QString unknownStaging = root + QStringLiteral("/.wtfml-restore-UNKNOWN");
        writeFileAt(unknownStaging + QStringLiteral("/keep.txt"), "KEEP");
        const QByteArray journalBefore = readAll(journalPath);

        ManualRecovery::Request request;
        request.gameRoot = root;
        request.targetDir = target;
        request.replacementDir = oldDir;

        auto inUse = request;
        inUse.inUse = true;
        const auto blocked = ManualRecovery::swapDirectories(inUse);
        check(!blocked.ok && readAll(target + QStringLiteral("/level.dat")) == "CURRENT"
                  && readAll(oldDir + QStringLiteral("/level.dat")) == "OLD"
                  && readAll(journalPath) == journalBefore,
              "★★ ZQ12 游戏在跑 / 正被占用 ⇒ 一个目录都没换，记录也没动", blocked.problem);

        {
            const bool enabledBefore = DirectorySessionLock::enabled;
            DirectorySessionLock::enabled = true;
            QLockFile other(QDir::temp().filePath(
                QStringLiteral("wtfml-directory-%1.lock")
                    .arg(DirectorySessionLock::keyFor(root))));
            other.setStaleLockTime(0);
            check(other.tryLock(0), "ZQ12 数据目录占用夹具取得锁");
            const auto refused = ManualRecovery::swapDirectories(request);
            DirectorySessionLock::enabled = enabledBefore;
            check(!refused.ok && readAll(target + QStringLiteral("/level.dat")) == "CURRENT"
                      && readAll(oldDir + QStringLiteral("/level.dat")) == "OLD",
                  "★★ ZQ12 另一份启动器占着数据目录 ⇒ 不动盘", refused.problem);
        }

        // 另一条记录把这份旧目录当成自己的 retained：换走它会让那一条变成
        // 「目标和旧目录均缺失」，所以先拒绝。
        {
            const QString otherTarget = root + QStringLiteral("/other-instance");
            QDir().mkpath(otherTarget);
            const QString otherJournal = QDir(RestoreJournal::directory(root)).filePath(
                QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
            check(RestoreJournal::write(otherJournal,
                      RestoreJournal::record(otherTarget, oldDir, true)),
                  "ZQ12 另一条认领同一份旧目录的记录写好了");
            const auto claimed = ManualRecovery::swapDirectories(request);
            check(!claimed.ok && readAll(target + QStringLiteral("/level.dat")) == "CURRENT"
                      && readAll(oldDir + QStringLiteral("/level.dat")) == "OLD",
                  "★★ ZQ12 旧目录还被另一条记录登记着 ⇒ 不换", claimed.problem);
            QFile::remove(otherJournal);
        }
        // 同一个目标上有一条记录，但记的不是用户挑的这一份：先处理那条。
        {
            const QString otherRetained = root + QStringLiteral("/.wtfml-restore-previous-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces);
            QDir().mkpath(otherRetained);
            const QString otherJournal = QDir(RestoreJournal::directory(root)).filePath(
                QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
            check(RestoreJournal::write(otherJournal,
                      RestoreJournal::record(target, otherRetained, true)),
                  "ZQ12 同一目标的另一条记录写好了");
            const auto mismatch = ManualRecovery::swapDirectories(request);
            check(!mismatch.ok && readAll(target + QStringLiteral("/level.dat")) == "CURRENT"
                      && readAll(oldDir + QStringLiteral("/level.dat")) == "OLD",
                  "★★ ZQ12 和记录里那一份对不上 ⇒ 不换", mismatch.problem);
            QFile::remove(otherJournal);
            QDir().rmdir(otherRetained);
        }

        const auto swapped = ManualRecovery::swapDirectories(request);
        check(swapped.ok, "★★★ ZQ12 换回来了", swapped.problem);
        check(readAll(target + QStringLiteral("/level.dat")) == "OLD",
              "★★★ ZQ12 当前目录里现在是旧那一份");
        check(!swapped.retainedPath.isEmpty()
                  && readAll(swapped.retainedPath + QStringLiteral("/level.dat")) == "CURRENT",
              "★★★ ZQ12 换下来的「现在这一份」原样留在旁边（两边都留着）",
              swapped.retainedPath);
        check(!QFileInfo::exists(oldDir), "ZQ12 旧目录是被改名换上去的，不是拷过去再删");
        check(QFileInfo::exists(journalPath) && journalCount() == 1,
              "★★ ZQ12 同一个目标只留一条记录，而且记录没被删掉",
              QString::number(journalCount()));
        const RecoveryReview::Item review = RecoveryReview::read(root, journalPath);
        check(review.problem.isEmpty() && !swapped.retainedPath.isEmpty()
                  && review.target == InstanceFiles::canonicalPath(target)
                  && QFileInfo(review.retained).fileName()
                         == QFileInfo(swapped.retainedPath).fileName(),
              "ZQ12 记录改写成了这一趟的「换下来的那一份」", review.retained);
        check(readAll(unknownStaging + QStringLiteral("/keep.txt")) == "KEEP",
              "★★ ZQ12 没登记的暂存一个字节都没动");

        // 已发布事务：启动恢复不替用户决定，两份都原样。
        const auto messages = BackupFiles::recoverInterruptedRestores(root);
        check(readAll(target + QStringLiteral("/level.dat")) == "OLD"
                  && readAll(swapped.retainedPath + QStringLiteral("/level.dat")) == "CURRENT"
                  && !messages.isEmpty(),
              "★★★ ZQ12 启动恢复不替用户决定：两边都留着并留下提示",
              messages.join(QStringLiteral("；")));

        QString problem;
        check(RecoveryReview::acknowledge(root, RecoveryReview::read(root, journalPath), &problem)
                  && readAll(target + QStringLiteral("/level.dat")) == "OLD"
                  && readAll(swapped.retainedPath + QStringLiteral("/level.dat")) == "CURRENT",
              "ZQ12 标记已检查只归档日志，两份数据原样保留", problem);
    }
    {
        // 两段改名之间断掉：目标已经挪走、旧目录还没换上来。启动恢复要把「现在这一份」
        // 放回去 —— 也就是这一趟人工恢复等于没发生，挑中的旧目录原地不动。
        const QString root = work + QStringLiteral("/manual-recovery-crash");
        const QString target = root + QStringLiteral("/instance");
        const QString retained = root + QStringLiteral("/.wtfml-restore-previous-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString chosen = root + QStringLiteral("/chosen-old");
        writeFileAt(retained + QStringLiteral("/level.dat"), "CURRENT");
        writeFileAt(chosen + QStringLiteral("/level.dat"), "OLD");
        QDir().mkpath(RestoreJournal::directory(root));
        const QString journalPath = QDir(RestoreJournal::directory(root)).filePath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
        check(RestoreJournal::write(journalPath, RestoreJournal::record(target, retained, true)),
              "ZQ12 断在两次改名之间的记录写好了");
        BackupFiles::recoverInterruptedRestores(root);
        check(readAll(target + QStringLiteral("/level.dat")) == "CURRENT"
                  && readAll(chosen + QStringLiteral("/level.dat")) == "OLD",
              "★★★ ZQ12 断在两次改名之间 ⇒ 启动恢复把现在这一份放回去，挑的旧目录没动");
    }
    {
        // 目标本来就不在了：没有「换下来的」那一份，记录也没有旧目录可检查。
        const QString root = work + QStringLiteral("/manual-recovery-gone");
        const QString target = root + QStringLiteral("/instance");
        const QString chosen = root + QStringLiteral("/old-copy");
        writeFileAt(chosen + QStringLiteral("/level.dat"), "OLD");
        ManualRecovery::Request request;
        request.gameRoot = root;
        request.targetDir = target;
        request.replacementDir = chosen;
        const auto placed = ManualRecovery::swapDirectories(request);
        check(placed.ok && placed.retainedPath.isEmpty()
                  && readAll(target + QStringLiteral("/level.dat")) == "OLD",
              "★★ ZQ12 目标已经不在了 ⇒ 旧目录直接放回去，没有换下来的那一份",
              placed.problem);
        BackupFiles::recoverInterruptedRestores(root);
        check(QDir(RestoreJournal::directory(root))
                  .entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty(),
              "ZQ12 目标原本不存在的事务恢复后不留记录（没有旧目录要检查）");
    }
    AppPaths::setMinecraftDir(originalDir);
}

