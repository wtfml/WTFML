#include "BackupController.hpp"
#include <QCoreApplication>
#include "manage/BackupFiles.hpp"

#include "ActivityCenterController.hpp"
#include "util/AppPaths.hpp"
#include "manage/VersionManager.hpp"
#include "manage/ManualRecovery.hpp"
#include "manage/RecoveryReview.hpp"
#include "InlineMessage.hpp"
#include "InstanceSettingsDialog.hpp"
#include "OverlayHost.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QThread>
#include <QUrl>
#include <QElapsedTimer>

#include <algorithm>
#include <functional>

BackupController::BackupController(VersionManager *versions, ActivityCenterController *activity,
                                   OverlayHost *overlay, Host host, QObject *parent)
    : QObject(parent), m_versions(versions), m_activity(activity), m_overlay(overlay),
      m_host(std::move(host)) {}

BackupController::~BackupController() {
    shutdown();
}

bool BackupController::busy() const {
    return m_scanning || m_taskId != Tasks::invalidTaskId();
}

bool BackupController::canCancelTask(Tasks::TaskId id) const {
    return id != Tasks::invalidTaskId() && id == m_taskId && m_thread != nullptr;
}

bool BackupController::cancelTask(Tasks::TaskId id) {
    if (!canCancelTask(id)) {
        return false;
    }
    // 先记"是用户按的"：工人看到 `m_abort` 就会停，收尾那一格靠这一位决定
    // 是记 Cancelled 还是照常报失败。
    m_userCancelled.store(true, std::memory_order_relaxed);
    m_abort.storeRelaxed(1);
    return true;
}

void BackupController::shutdown() {
    m_abort.storeRelaxed(1);
    if (m_thread != nullptr) {
        m_thread->wait();
    }
    // 指针可能已经被放掉了（见 startWrite / startRestore 里那段），而那条线程
    // 还在最后几条指令里。析构 QObject 会把子对象连同一条仍在跑的线程一起删掉 ⇒ qFatal。
    // 按子对象兜一遍底，保证收手时它们都已经退出。
    for (QThread *thread : findChildren<QThread *>()) {
        if (thread != m_thread) thread->wait();
    }
}

QString BackupController::displayName(const QString &id) const {
    return m_host.displayName ? m_host.displayName(id) : id;
}

void BackupController::log(const QString &message) const {
    if (m_host.log) {
        m_host.log(message);
    }
}

QString BackupController::sizeText(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(QString::number(double(bytes) / (1024.0 * 1024 * 1024), 'f', 2));
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(QString::number(double(bytes) / (1024.0 * 1024), 'f', 1));
    }
    return QStringLiteral("%1 KB").arg(QString::number(double(bytes) / 1024.0, 'f', 1));
}

QString BackupController::pickLine(const QString &zipPath, const QString &from) {
    const QFileInfo info(zipPath);
    QString line = tr("%1（%2，%3）")
                       .arg(info.fileName(), sizeText(info.size()),
                            info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (!from.isEmpty()) {
        line += tr(" / 来自「%1」").arg(from);
    }
    return line;
}

void BackupController::openFolder(BackupFiles::Kind kind) {
    const QString dir = BackupFiles::backupDir(kind);
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void BackupController::inspectRecovery() {
    const QString root = AppPaths::minecraftDir();
    const auto items = RecoveryReview::inspect(root);
    QStringList labels{tr("浏览游戏目录（人工检查未登记残留）"), tr("浏览暂存登记（不自动删除）")};
    for (const auto &item : items) labels.append(item.target.isEmpty()
        ? QFileInfo(item.journal).fileName() + tr("（日志异常）") : QDir::toNativeSeparators(item.target));
    int index = -1;
    if (!InlineMessage::pickFromList(m_overlay, tr("中断恢复检查"),
        tr("待检查还原事务：%1。历史未登记或标记异常目录需人工核对，不按名称自动删除。")
            .arg(items.size()), labels, 0, &index)) return;
    if (index == 0 || index == 1) {
        QString directory = index == 0 ? root : QDir(root).filePath(QStringLiteral("cache/staging-records"));
        if (!QFileInfo::exists(directory)) directory = root;
        if (!RestoreJournal::ordinaryPath(InstanceFiles::canonicalPath(root), directory)
            || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            InlineMessage::warning(m_overlay, tr("无法打开检查目录"), tr("目录不可访问或含链接，未打开。"));
        return;
    }
    index -= 2;
    if (index < 0 || index >= items.size()) return;
    const auto item = items.at(index);
    // 能换回旧目录的那一种现场：记录读得懂、旧目录还在、目标要么是个目录要么已经不在了。
    // 目标那一格是个**文件**、或者两边哪个是链接（记录会判成 problem）都不给这一颗按钮。
    const bool canSwap = item.problem.isEmpty() && QFileInfo(item.retained).isDir()
                         && (!QFileInfo::exists(item.target) || QFileInfo(item.target).isDir());
    const QString text = item.problem.isEmpty()
        ? tr("当前目录：\n%1\n\n还原前目录：\n%2\n\n「标记已检查」将归档此记录并保留两个目录。「用旧目录替换」将恢复还原前的目录，另存当前目录，并记录恢复操作。")
              .arg(QDir::toNativeSeparators(item.target), QDir::toNativeSeparators(item.retained))
        : item.problem + tr("\n\n日志：%1").arg(QDir::toNativeSeparators(item.journal));
    QStringList buttons{tr("打开所在目录")};
    const int swapIndex = canSwap ? int(buttons.size()) : -1;
    if (canSwap) buttons.append(tr("用旧目录替换（两份都保留）"));
    const int ackIndex = int(buttons.size());
    buttons.append(tr("保留两份，标记已检查"));
    const int closeIndex = int(buttons.size());
    buttons.append(tr("关闭"));
    InlineMessageBox box(MessageKind::Information, tr("检查还原事务"), text, buttons,
                         closeIndex, -1);
    m_overlay->execPrompt(&box);
    const int clicked = box.clickedIndex();
    if (clicked == 0) {
        const QString directory = item.target.isEmpty() ? QFileInfo(item.journal).absolutePath()
                                                       : QFileInfo(item.target).absolutePath();
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            InlineMessage::warning(m_overlay, tr("打开目录失败"), directory);
    } else if (clicked == swapIndex) {
        // 「人工恢复目录」：换之前先量运行状态；确认框之后再过一遍（那一段是嵌套事件循环，
        // 用户在这十几秒里照样能去启动游戏）。
        const QString instanceId =
            m_host.instanceIdOfPath ? m_host.instanceIdOfPath(item.target) : QString();
        const auto inUse = [this, &instanceId] {
            return busy()
                   || (!instanceId.isEmpty() && m_host.isRunning
                       && m_host.isRunning(instanceId))
                   || (!instanceId.isEmpty() && m_host.isBeingModified
                       && m_host.isBeingModified(instanceId));
        };
        if (inUse()) {
            InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                       tr("这一份正在运行，或者正被复制 / 备份 / 还原 / 安装占用。请等它结束后再替换。"));
            return;
        }
        if (!InlineMessage::question(
                m_overlay, tr("用旧目录替换"),
                tr("将使用旧目录替换当前目录。\n\n旧目录：\n%1\n\n当前目录：\n%2\n\n当前目录将另存于同级目录。启动器会记录本次替换，以便恢复。")
                    .arg(QDir::toNativeSeparators(item.retained),
                         QDir::toNativeSeparators(item.target)),
                tr("替换"), tr("取消"), false, true)) {
            return;
        }
        if (AppPaths::minecraftDir() != root || inUse()) return;
        ManualRecovery::Request request;
        request.gameRoot = root;
        request.targetDir = item.target;
        request.replacementDir = item.retained;
        request.inUse = false;  // 上面两趟已经量过；这一层自己还会守数据目录会话锁。
        const ManualRecovery::Result swapped = ManualRecovery::swapDirectories(request);
        if (!swapped.ok) {
            log(tr("人工恢复失败：%1").arg(swapped.problem));
            InlineMessage::warning(m_overlay, tr("替换失败"), swapped.problem);
            return;
        }
        log(tr("人工恢复：用 %1 换掉了 %2").arg(QDir::toNativeSeparators(item.retained),
                                                QDir::toNativeSeparators(item.target)));
        QString done = tr("替换完成。");
        if (!swapped.retainedPath.isEmpty()) {
            done += tr("\n\n已保留替换前的目录：\n%1")
                        .arg(QDir::toNativeSeparators(swapped.retainedPath));
        }
        if (!swapped.journalPath.isEmpty()) {
            done += tr("\n\n恢复记录也留着（%1），在「中断恢复检查」里能看到，核对完再标记已检查。")
                        .arg(QDir::toNativeSeparators(swapped.journalPath));
        }
        if (!swapped.notes.isEmpty()) {
            done += QStringLiteral("\n\n") + swapped.notes.join(QStringLiteral("\n"));
        }
        if (m_host.refreshInstances) m_host.refreshInstances();
        InlineMessage::information(m_overlay, tr("替换完成"), done);
    } else if (clicked == ackIndex) {
        QString problem;
        if (!RecoveryReview::acknowledge(root, item, &problem))
            InlineMessage::warning(m_overlay, tr("未标记已检查"), problem);
        else InlineMessage::information(m_overlay, tr("已标记检查"),
            tr("两份目录均未改动，日志已保存在 cache/restore-reviewed。"));
    }
}

void BackupController::pickZip(BackupFiles::Kind kind, const QString &label, const QString &what,
                               std::function<void(QString)> selected) {
    const QString dir = BackupFiles::backupDir(kind);
    const QString prefix = BackupFiles::backupFilePrefix(label);
    const QString gameRoot = AppPaths::minecraftDir();
    m_scanning = true;
    m_abort.storeRelaxed(0);
    log(tr("正在后台检查备份清单……"));
    auto *scanner = QThread::create([this, kind, label, what, dir, prefix, gameRoot,
                                    selected = std::move(selected)] {
    QElapsedTimer deadline;
    deadline.start();
    QStringList zips;
    QStringList rejected;
    QHash<QString, QString> sourceNames;
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QStringList{QStringLiteral("*.zip")}, QDir::Files, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (m_abort.loadRelaxed() || deadline.elapsed() > 240000) break;
        if (!entry.fileName().startsWith(prefix, Qt::CaseInsensitive)) {
            continue;
        }
        // 只比名字前缀不够 ⇒ 三件事都能漏进来：从别处拷进备份目录、恰好叫得
        // 像模像样的 zip；改了名/换过实例之后留下来的旧包；以及**世界备份**被挪到
        // `instances/` 目录里（前缀碰巧一样时）。还原的最后一步是拿 `manifest.root`
        // 拼落点、把那个目录**整个换掉**，所以"名字对不对"必须在**列出来之前**问，
        // 不是等用户挑完、点了"还原"再由 `readManifest` 报错。
        bool readable = false;
        QString problem;
        const BackupFiles::Manifest manifest =
            BackupFiles::readManifest(entry.absoluteFilePath(), &readable, &problem);
        if (!readable || manifest.kind != kind
            || manifest.root.compare(label, Qt::CaseInsensitive) != 0) {
            rejected.append(entry.fileName());
            continue;
        }
        zips.append(entry.absoluteFilePath());
        if (kind == BackupFiles::Kind::World) {
            sourceNames.insert(entry.absoluteFilePath(), manifest.sourceInstanceName);
        }
    }
    QMetaObject::invokeMethod(this, [this, kind, what, dir, prefix, gameRoot,
                                   timedOut = deadline.elapsed() > 240000,
                                   zips = std::move(zips), rejected = std::move(rejected),
                                   sourceNames = std::move(sourceNames), selected]() mutable {
    m_scanning = false;
    if (m_abort.loadRelaxed() || AppPaths::minecraftDir() != gameRoot) return;
    if (timedOut) {
        InlineMessage::information(m_overlay, tr("备份检查超时"),
            tr("检查已停止，未还原任何文件。请检查备份所在磁盘，或减少目录中的归档后重试。"));
        return;
    }
    if (zips.isEmpty()) {
        if (!rejected.isEmpty()) {
            InlineMessage::information(
                m_overlay, tr("没有能还原的备份"),
                tr("找到 %1 个文件名匹配的备份，但其中的元数据与%2不匹配、无效或来自其他工具。以下文件未列入可还原备份：\n%3\n\n备份目录：%4")
                    .arg(QString::number(rejected.size()), what,
                         rejected.join(QLatin1Char('\n')), QDir::toNativeSeparators(dir)));
            return;
        }
        InlineMessage::information(
            m_overlay, tr("没有可还原的备份"),
            tr("%1 暂无备份。请先在此页面创建备份。\n\n备份目录：%2")
                .arg(what, QDir::toNativeSeparators(dir)));
        return;
    }
    // 文件名不能按字典序排（`std::greater<QString>`）：自动留下的「恢复前-」
    // 安全快照在名字里多插了一段汉字，而汉字的码位在数字之后 ⇒ 每一份安全快照都会
    // 排到**最新的常规备份前面**。刚还原过一次，列表最上面就是刚才自动留的那一份，
    // "越上面越新"当场不成立，点下去还原的其实是很久以前那份。
    // 排序挪进 `BackupFiles::sortBackupsNewestFirst()`（纯函数，verify 里有一条钉子）。
    zips = BackupFiles::sortBackupsNewestFirst(zips, prefix);
    QStringList items;
    for (const QString &zip : std::as_const(zips)) {
        items.append(pickLine(zip, sourceNames.value(zip)));
    }
    int row = -1;
    const QPointer<BackupController> alive(this);
    if (!InlineMessage::pickFromList(m_overlay, tr("选择备份"),
                                     tr("选择要还原的备份。列表按时间从新到旧排列。"), items, 0, &row)
        || row < 0 || row >= zips.size()) {
        return;
    }
    if (!alive) return;
    selected(zips.at(row));
    }, Qt::QueuedConnection);
    });
    connect(scanner, &QThread::finished, scanner, &QObject::deleteLater);
    scanner->setParent(this);
    scanner->start();
}

void BackupController::backupWorld(const QString &worldDir) {
    if (worldDir.isEmpty()) return;
    if (busy()) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    const QString instanceId = m_host.instanceIdOfPath ? m_host.instanceIdOfPath(worldDir) : QString();
    if (!instanceId.isEmpty() && m_host.isBeingModified
        && m_host.isBeingModified(instanceId)) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("「%1」正在复制、备份、还原或安装，请完成后再备份这个世界。")
                                       .arg(displayName(instanceId)));
        return;
    }
    if (!instanceId.isEmpty() && m_host.isRunning && m_host.isRunning(instanceId)) {
        InlineMessage::warning(m_overlay, tr("备份世界"),
                               tr("游戏正在运行。为防止存档损坏，请退出游戏后再备份。"));
        return;
    }
    const QString worldName = QFileInfo(worldDir).fileName();
    const QDateTime now = QDateTime::currentDateTime();
    BackupFiles::Manifest manifest;
    manifest.kind = BackupFiles::Kind::World;
    manifest.root = worldName;
    manifest.label = worldName;
    manifest.worldName = worldName;
    manifest.sourceInstanceId = instanceId;
    manifest.sourceInstanceName = instanceId.isEmpty() ? QString() : displayName(instanceId);
    manifest.gameVersion = instanceId.isEmpty() ? QString() : m_versions->instanceGameVersion(instanceId);
    manifest.createdAt = now;
    manifest.scope = QStringList{tr("这个存档的整个文件夹")};
    const QString zip = QDir(BackupFiles::backupDir(BackupFiles::Kind::World))
                            .filePath(BackupFiles::backupFileName(worldName, now));
    // 这一趟在**读整棵目录树**（世界那一棵，连同它所在的实例）。跑着的
    // 这十几秒里实例如果被删 / 改名 / 启动，包里会静默少掉正被改的那几个文件，收尾
    // 却照样报"备份完成"。记进和复制 / 还原同一个集合 —— 那一条是 `installingVersionIds()`
    // 的唯一出处，启动、改名、删除三处读的都是它。收尾在 writeFinished() 里摘。
    m_writeInstanceId = instanceId;
    if (!instanceId.isEmpty() && m_host.setFileOperationBusy) {
        m_host.setFileOperationBusy(instanceId, true);
    }
    startWrite(manifest, worldDir, zip, tr("备份世界「%1」").arg(worldName), instanceId,
               tr("备份完成"), QString(), tr("备份没有完成"));
}

void BackupController::restoreWorld(const QString &worldDir, InstanceSettingsDialog *dialog) {
    if (worldDir.isEmpty()) return;
    if (busy()) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    const QString instanceId = m_host.instanceIdOfPath ? m_host.instanceIdOfPath(worldDir) : QString();
    if (!instanceId.isEmpty() && m_host.isRunning && m_host.isRunning(instanceId)) {
        InlineMessage::warning(m_overlay, tr("恢复备份"),
                               tr("游戏正在运行，请退出游戏后再还原存档，避免数据覆盖。"));
        return;
    }
    const QString worldName = QFileInfo(worldDir).fileName();
    const QPointer<InstanceSettingsDialog> guardedDialog(dialog);
    const QString root = AppPaths::minecraftDir();
    pickZip(BackupFiles::Kind::World, worldName, tr("这个世界"),
            [this, worldDir, worldName, instanceId, guardedDialog, root](const QString &zip) {
    auto *dialog = guardedDialog.data();
    if (AppPaths::minecraftDir() != root || busy()
        || (m_host.isRunning && m_host.isRunning(instanceId))
        || (m_host.isBeingModified && m_host.isBeingModified(instanceId))) return;
    if (zip.isEmpty()) return;
    const QPointer<BackupController> alive(this);
    if (!InlineMessage::question(
            m_overlay, tr("恢复备份"),
            tr("还原将替换「%1」的文件。启动器会先创建安全快照，以便撤销还原。\n\n待还原备份：\n%2")
                .arg(worldName, QDir::toNativeSeparators(zip)),
            tr("还原"), tr("取消"), false, true)) {
        return;
    }
    if (!alive || AppPaths::minecraftDir() != root || busy()
        || (m_host.isRunning && m_host.isRunning(instanceId))
        || (m_host.isBeingModified && m_host.isBeingModified(instanceId))) return;
    m_refreshWorldDir = worldDir;
    m_refreshDialog = dialog;
    // 和「还原实例」同一个理由（见 restoreInstance() 那段）—— 这一趟
    // 要把存档目录整个换掉，跑着的时候这一份实例不能启动、不能删（启动器读到的
    // 会是换到一半的存档；删除则会让解压往一个正在被删的目录里写）。
    // 复用 `m_restoreInstanceId` 记账：上面 `busy()` 挡着，世界还原和实例还原
    // 不会同时跑。**不收那一页**（`m_restoreDialog` 这里刻意不设）—— 世界还原
    // 只需要重列那几行存档，页面留着才是用户想看的。
    m_restoreInstanceId = instanceId;
    if (!instanceId.isEmpty() && m_host.setFileOperationBusy) {
        m_host.setFileOperationBusy(instanceId, true);
    }
    startRestore(zip, QFileInfo(worldDir).absolutePath(), tr("还原世界「%1」").arg(worldName),
                 instanceId, tr("还原完成"), QString(), tr("还原没有完成"));
    });
}

void BackupController::snapshotInstance(const QString &versionId) {
    if (versionId.isEmpty()) return;
    if (busy()) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(versionId)) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("「%1」的文件正在修改。请等待安装、导入或检查完成后再创建快照。")
                                       .arg(displayName(versionId)));
        return;
    }
    if (m_host.isRunning && m_host.isRunning(versionId)) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("「%1」正在运行。请退出游戏后再创建快照，避免备份尚未写完的存档和日志。")
                                       .arg(displayName(versionId)));
        return;
    }
    const int picked = InlineMessage::ask(
        m_overlay, MessageKind::Question, tr("创建快照"),
        tr("快照将「%1」的实例目录打包，以便整体还原。\n\n默认跳过可重新下载的 assets 和 libraries。")
            .arg(displayName(versionId)),
        {tr("创建快照"), tr("完整快照（含可重新下载的部分）"), tr("取消")}, 0, -1, false);
    if (picked != 0 && picked != 1) return;
    // 这一问可能开着很久。开头那两道闸只管"按下去那一刻"；问答期间另一份操作
    // （备份 / 还原 / 复制 / 安装）完全可能已经开始。动盘之前按同一套判据再问一次，
    // 不能拿旧结论去读一棵正在被改的树。
    if (busy()
        || (m_host.isBeingModified && m_host.isBeingModified(versionId))
        || (m_host.isRunning && m_host.isRunning(versionId))) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("刚才有另一项操作开始了（复制 / 备份 / 还原 / 安装 / 游戏启动）。等它完成后再做快照。"));
        return;
    }
    const bool complete = picked == 1;
    const QString instanceDir = AppPaths::versionDir(versionId);
    if (!QFileInfo(instanceDir).isDir()) {
        InlineMessage::warning(m_overlay, tr("创建快照"),
                               tr("找不到这个实例的目录：\n%1")
                                   .arg(QDir::toNativeSeparators(instanceDir)));
        return;
    }
    const QString dirName = QFileInfo(instanceDir).fileName();
    const QDateTime now = QDateTime::currentDateTime();
    BackupFiles::Manifest manifest;
    manifest.kind = BackupFiles::Kind::Instance;
    manifest.root = dirName;
    manifest.label = dirName;
    manifest.sourceInstanceId = versionId;
    manifest.sourceInstanceName = displayName(versionId);
    manifest.gameVersion = m_versions->instanceGameVersion(versionId);
    manifest.complete = complete;
    manifest.createdAt = now;
    if (complete) {
        manifest.scope = QStringList{tr("整个实例目录（包含能重新下回来的资源文件和游戏库）")};
    } else {
        manifest.excluded = QStringList{QStringLiteral("assets"), QStringLiteral("libraries")};
        manifest.scope = QStringList{tr("实例目录（模组、配置、存档、日志…）"),
                                     tr("没打包：assets、libraries（能重新下回来）")};
    }
    const QString zip = QDir(BackupFiles::backupDir(BackupFiles::Kind::Instance))
                            .filePath(BackupFiles::backupFileName(dirName, now));
    // 开头那两道闸（isBeingModified / isRunning）只管**按下这一颗那一刻**。
    // 快照的这十几秒一直在读这棵目录树；期间这一份实例照样能被删掉、被改名、被启动
    // （启动后游戏往存档 / 日志里写），结果就是包里少了那些文件、而收尾照样报
    // "快照创建完成" —— 一份看着成功、其实是残的备份比失败更糟。记进和复制 / 还原
    // 同一个集合（`installingVersionIds()` 的唯一出处），收尾在 writeFinished() 里摘。
    m_writeInstanceId = versionId;
    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(versionId, true);
    startWrite(manifest, instanceDir, zip,
               tr("创建「%1」的快照").arg(displayName(versionId)), versionId,
               tr("快照创建完成"), QString(), tr("快照没有做成"));
}

void BackupController::restoreInstance(const QString &versionId, InstanceSettingsDialog *dialog) {
    if (versionId.isEmpty()) return;
    if (busy()) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    if ((m_host.isBeingModified && m_host.isBeingModified(versionId))
        || (m_host.isRunning && m_host.isRunning(versionId))) {
        InlineMessage::information(m_overlay, tr("暂时无法操作"),
                                   tr("「%1」正在安装或运行。还原将替换实例目录，请先停止相关操作。")
                                       .arg(displayName(versionId)));
        return;
    }
    const QString instanceDir = AppPaths::versionDir(versionId);
    const QPointer<InstanceSettingsDialog> guardedDialog(dialog);
    const QString root = AppPaths::minecraftDir();
    pickZip(BackupFiles::Kind::Instance, QFileInfo(instanceDir).fileName(), tr("这个实例"),
            [this, versionId, guardedDialog, root](const QString &zip) {
    auto *dialog = guardedDialog.data();
    if (AppPaths::minecraftDir() != root || busy()
        || (m_host.isRunning && m_host.isRunning(versionId))
        || (m_host.isBeingModified && m_host.isBeingModified(versionId))) return;
    if (zip.isEmpty()) return;
    const QPointer<BackupController> alive(this);
    if (!InlineMessage::question(
            m_overlay, tr("从快照恢复"),
            tr("还原快照将替换「%1」的模组、配置和存档。启动器会先备份当前实例，以便撤销还原。\n\n待还原快照：\n%2")
                .arg(displayName(versionId), QDir::toNativeSeparators(zip)),
            tr("还原"), tr("取消"), false, true)) {
        return;
    }
    // 记进和「复制实例」同一个集合（`MainWindow::installingVersionIds()` 的唯一出处），
    // 收尾时先摘掉它再刷新实例清单。页面先禁用：这一趟结束之后它上面的值全是旧的。
    if (!alive || AppPaths::minecraftDir() != root || busy()
        || (m_host.isRunning && m_host.isRunning(versionId))
        || (m_host.isBeingModified && m_host.isBeingModified(versionId))) return;
    m_restoreInstanceId = versionId;
    m_restoreDialog = dialog;
    if (m_restoreDialog != nullptr) m_restoreDialog->setEnabled(false);
    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(versionId, true);
    startRestore(zip, AppPaths::versionsDir(),
                 tr("还原「%1」的快照").arg(displayName(versionId)), versionId,
                 tr("还原完成"), QString(), tr("还原没有完成"));
    });
}

Tasks::TaskId BackupController::startWrite(const BackupFiles::Manifest &manifest,
                                            const QString &sourceDir, const QString &targetZip,
                                            const QString &title, const QString &targetId,
                                            const QString &successTitle,
                                            const QString &refreshServerId,
                                            const QString &failureTitle) {
    QList<Tasks::TaskStep> steps{{tr("统计并核对文件"), Tasks::TaskStepState::Running, {}},
                                 {tr("打包"), Tasks::TaskStepState::Pending, {}}};
    m_taskId = m_activity->begin(Tasks::TaskKind::Backup, title, targetId, steps);
    // 备份 / 还原都能在安全点停下（见 BackupFiles 里那两个停止条件），所以把「取消」开出来：
    // `Backup` 的默认能力位是关的。
    if (m_taskId != Tasks::invalidTaskId()) {
        Tasks::TaskCapabilities caps = m_activity->tasks()->snapshot(m_taskId).capabilities;
        caps.canCancel = true;
        m_activity->tasks()->setCapabilities(m_taskId, caps);
    }
    // 服务端世界的那一趟（备份 / 还原）读 / 换的是**这一个服务端**的目录。
    m_refreshServerId = refreshServerId;
    // 跑着的时候它不能启动、不能删 —— 实例那一侧和这条服务端路都必须记"动不得"
    // （见 `ServerRunController::fileBusy()` / `removeServer()` 那两道闸）。
    m_busyServerId = refreshServerId;
    if (!m_busyServerId.isEmpty() && m_host.setServerFileOperationBusy) {
        m_host.setServerFileOperationBusy(m_busyServerId, true);
    }
    log(tr("%1：%2 → %3").arg(title, QDir::toNativeSeparators(sourceDir),
                              QDir::toNativeSeparators(targetZip)));
    m_abort.storeRelaxed(0);
    m_userCancelled.store(false, std::memory_order_relaxed);
    if (m_thread != nullptr) {
        // **不能**在这里对旧的那条调 deleteLater()。能走到这儿，是因为上一趟的
        // 收尾函数已经跑过了（任务号收了、忙态放了）—— 但那一条 `QThread` 这会儿往往还差
        // 最后几条指令才退出 `run()`。对一条仍在跑的 QThread 调 deleteLater() 会让 Qt 处理
        // DeferredDelete 时 `qFatal("QThread: Destroyed while thread is still running")`，
        // 整个启动器当场消失（ServerPing 那边 、IntegrityController 那边今天都
        // 踩过同一个坑）。线程对象自己接好了 finished → deleteLater（见下面创建处），
        // 这里只放掉指针。
        //
        // **先等它真的退出**：只放指针的话，上一趟可能还在最后几条指令里，而这一趟已经
        // 开始动同一个实例目录 —— 两条线程撞在一起，症状是"这一趟报成功、文件其实没回来"
        // （2026-11-04 全量跑里 T35.3 偶发过一次，单跑复跑都绿）。`wait()` 等到的是 run()
        // 返回，不是 finished 信号，所以那几条收尾指令一定已经做完。
        m_thread->wait();
        m_thread = nullptr;
    }
    m_thread = QThread::create([this, manifest, sourceDir, targetZip, successTitle, failureTitle] {
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = BackupFiles::writeBackup(manifest, sourceDir, targetZip,
            [this, &elapsed] { return m_abort.loadRelaxed() != 0 || elapsed.elapsed() >= 600000; });
        // 关窗（`shutdown()` 只置 `m_abort`）时**不许**再碰界面；用户按的取消要照常收尾。
        if (m_abort.loadRelaxed() == 0 || m_userCancelled.load(std::memory_order_relaxed)) {
            QMetaObject::invokeMethod(this, [this, result, successTitle, failureTitle] {
                writeFinished(result, successTitle, failureTitle);
            }, Qt::QueuedConnection);
        }
    });
    m_thread->setParent(this);
    // 先放指针、再排删除（理由同上；`shutdown()` 里会用这个指针 wait()）。
    connect(m_thread, &QThread::finished, this, [this, thread = m_thread] {
        if (m_thread == thread) m_thread = nullptr;
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
    return m_taskId;
}

Tasks::TaskId BackupController::startRestore(const QString &zipPath,
                                              const QString &targetParentDir,
                                              const QString &title, const QString &targetId,
                                              const QString &successTitle,
                                              const QString &refreshServerId,
                                              const QString &failureTitle) {
    QList<Tasks::TaskStep> steps{{tr("读清单"), Tasks::TaskStepState::Running, {}},
                                 {tr("备份当前实例"), Tasks::TaskStepState::Pending, {}},
                                 {tr("解出备份里的文件"), Tasks::TaskStepState::Pending, {}},
                                 {tr("核对并替换目录"), Tasks::TaskStepState::Pending, {}}};
    m_taskId = m_activity->begin(Tasks::TaskKind::Backup, title, targetId, steps);
    if (m_taskId != Tasks::invalidTaskId()) {
        Tasks::TaskCapabilities caps = m_activity->tasks()->snapshot(m_taskId).capabilities;
        caps.canCancel = true;
        m_activity->tasks()->setCapabilities(m_taskId, caps);
    }
    m_refreshServerId = refreshServerId;
    m_busyServerId = refreshServerId;
    if (!m_busyServerId.isEmpty() && m_host.setServerFileOperationBusy) {
        m_host.setServerFileOperationBusy(m_busyServerId, true);
    }
    log(tr("%1：%2 → %3").arg(title, QDir::toNativeSeparators(zipPath),
                              QDir::toNativeSeparators(targetParentDir)));
    m_abort.storeRelaxed(0);
    m_userCancelled.store(false, std::memory_order_relaxed);
    if (m_thread != nullptr) {
        // 先等上一趟真的退出，再开这一趟（理由逐字同 startWrite() 里那一段：
        // 只放指针会让两条线程同时动同一个实例目录）。
        m_thread->wait();
        m_thread = nullptr;
    }
    m_thread = QThread::create([this, zipPath, targetParentDir, successTitle, failureTitle] {
        // 还原也接上和创建备份同一条停止条件（关窗 / 整项 600 秒预算）。
        // 快照、暂存解压和校验均可停；仅最终改名发布/回滚的短临界区不可取消。
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = BackupFiles::restoreBackup(
            zipPath, targetParentDir, [this, &elapsed] {
                return m_abort.loadRelaxed() != 0 || elapsed.elapsed() >= 600000;
            });
        // 同 startWrite()：关窗不发，用户取消照常收尾。
        if (m_abort.loadRelaxed() == 0 || m_userCancelled.load(std::memory_order_relaxed)) {
            QMetaObject::invokeMethod(this, [this, result, successTitle, failureTitle] {
                restoreFinished(result, successTitle, failureTitle);
            }, Qt::QueuedConnection);
        }
    });
    m_thread->setParent(this);
    // 先放指针、再排删除（同 startWrite()）。
    connect(m_thread, &QThread::finished, this, [this, thread = m_thread] {
        if (m_thread == thread) m_thread = nullptr;
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
    return m_taskId;
}

void BackupController::writeFinished(const BackupFiles::WriteResult &result,
                                     const QString &successTitle,
                                     const QString &failureTitle) {
    // 先摘"正在读这一份实例"的标记、再刷新（理由和 restoreFinished() 那一段逐字
    // 相同）：`refreshInstalledVersions()` 会按 `installingVersionIds()` 把忙的那一份
    // 从启动列表里滤掉，反过来的话刚摘掉标记这一趟刷新还看不见它。快照本身没动盘，
    // 但这一趟期间别的刷新可能已经把这一份滤掉过，所以这里补一发。
    const QString writtenId = m_writeInstanceId;
    m_writeInstanceId.clear();
    if (!writtenId.isEmpty() && m_host.setFileOperationBusy) {
        m_host.setFileOperationBusy(writtenId, false);
        if (m_host.refreshInstances) m_host.refreshInstances();
    }
    const Tasks::TaskId id = m_taskId;
    m_taskId = Tasks::invalidTaskId();
    // 「用户按了取消」只在**真的没做成**时才算数：按下的那一瞬间引擎可能正好走完，
    // 包已经发布出去了 —— 那时该报成功。
    const bool cancelled = m_userCancelled.exchange(false, std::memory_order_relaxed) && !result.ok;
    if (id != Tasks::invalidTaskId() && m_activity->tasks()->has(id)) {
        if (result.ok) {
            m_activity->tasks()->setStepState(id, 0, Tasks::TaskStepState::Done);
            m_activity->tasks()->setStepState(id, 1, Tasks::TaskStepState::Done);
            m_activity->tasks()->setDetail(id, tr("共 %1 个文件，%2")
                                                   .arg(result.manifest.fileCount())
                                                   .arg(sizeText(result.manifest.totalBytes)));
            m_activity->finish(id, true, {}, false);
        } else if (cancelled) {
            // 取消不是失败：收成 Cancelled（不亮红点），也不写失败原因。
            m_activity->tasks()->cancel(id);
        } else {
            const int failedStep = result.manifest.files.isEmpty() ? 0 : 1;
            m_activity->tasks()->setStepState(id, failedStep, Tasks::TaskStepState::Failed,
                                              result.problem);
            m_activity->finish(id, false, result.problem, false);
        }
    }
    if (result.ok) {
        log(tr("%1：%2").arg(successTitle, tr("共 %1 个文件，%2")
                                               .arg(result.manifest.fileCount())
                                               .arg(sizeText(result.manifest.totalBytes))));
        InlineMessage::information(m_overlay, successTitle,
                                   tr("已经打好一份了：\n%1\n\n共 %2 个文件，%3。")
                                       .arg(QDir::toNativeSeparators(result.zipPath))
                                       .arg(result.manifest.fileCount())
                                       .arg(sizeText(result.manifest.totalBytes)));
    } else if (cancelled) {
        log(tr("%1：已取消，未保留不完整的文件。").arg(successTitle));
    } else {
        // 失败这一路必须用失败标题：拿**成功标题**去弹，「备份完成」下面的正文却说
        // "没打成"。标题和正文自相矛盾，用户扫一眼标题就当成功了。
        const QString title = failureTitle.isEmpty() ? tr("没有完成") : failureTitle;
        log(tr("%1：%2").arg(title, result.problem));
        InlineMessage::warning(m_overlay, title, result.problem);
    }
    if (!m_refreshServerId.isEmpty() && m_host.refreshServer) {
        m_host.refreshServer(m_refreshServerId);
    }
    m_refreshServerId.clear();
    if (!m_busyServerId.isEmpty() && m_host.setServerFileOperationBusy) {
        m_host.setServerFileOperationBusy(m_busyServerId, false);
    }
    m_busyServerId.clear();
}

void BackupController::restoreFinished(const BackupFiles::RestoreResult &result,
                                       const QString &successTitle,
                                       const QString &failureTitle) {
    const Tasks::TaskId id = m_taskId;
    m_taskId = Tasks::invalidTaskId();
    // 同 writeFinished()：只有"真的没做成"才记成取消；已经发布出去的那一份该报成功。
    const bool cancelled = m_userCancelled.exchange(false, std::memory_order_relaxed)
        && !result.ok && !result.recoveryFailed;
    // 先摘忙标记、再把那一页解冻 —— 下面 `refreshInstances()` 里 `installingVersionIds()`
    // 还判这一位（顺序反了的话，刚还原好的那一份会在列表里被滤掉一帧）。
    const QString restoredId = m_restoreInstanceId;
    const QPointer<InstanceSettingsDialog> restoredDialog = m_restoreDialog;
    m_restoreInstanceId.clear();
    m_restoreDialog = nullptr;
    if (!restoredId.isEmpty() && m_host.setFileOperationBusy) {
        m_host.setFileOperationBusy(restoredId, false);
    }
    if (!restoredDialog.isNull()) restoredDialog->setEnabled(true);
    const auto stepOf = [](BackupFiles::RestoreResult::Stage stage) {
        switch (stage) {
        case BackupFiles::RestoreResult::Stage::ReadManifest: return 0;
        case BackupFiles::RestoreResult::Stage::Space:
        case BackupFiles::RestoreResult::Stage::SafetySnapshot: return 1;
        case BackupFiles::RestoreResult::Stage::Extract: return 2;
        case BackupFiles::RestoreResult::Stage::RemoveOld:
        case BackupFiles::RestoreResult::Stage::Verify:
        case BackupFiles::RestoreResult::Stage::Ok: return 3;
        }
        return 3;
    };
    if (id != Tasks::invalidTaskId() && m_activity->tasks()->has(id)) {
        if (result.ok) {
            for (int i = 0; i < 4; ++i) m_activity->tasks()->setStepState(id, i, Tasks::TaskStepState::Done);
            m_activity->tasks()->setDetail(id, tr("已校验 %1 个文件，全部通过。").arg(result.checked));
            m_activity->finish(id, true, {}, false);
        } else if (cancelled) {
            // 取消不是失败：账本收成 Cancelled，不把"停下"写成一次失败。
            m_activity->tasks()->cancel(id);
        } else {
            const int failed = stepOf(result.stage);
            for (int i = 0; i < 4; ++i) {
                m_activity->tasks()->setStepState(
                    id, i, i < failed ? Tasks::TaskStepState::Done
                                      : i == failed ? Tasks::TaskStepState::Failed
                                                    : Tasks::TaskStepState::Pending,
                    i == failed ? result.problem : QString());
            }
            m_activity->finish(id, false, result.problem, false);
        }
    }
    if (result.ok) {
        QString text = tr("还原完成。已校验 %1 个文件，全部通过。").arg(result.checked);
        if (!result.retainedPath.isEmpty()) {
            const QString warning = tr("旧目录未能清理，已保留在：%1")
                .arg(QDir::toNativeSeparators(result.retainedPath));
            text += QStringLiteral("\n\n") + warning;
            log(warning);
        }
        if (!result.safetyZip.isEmpty()) {
            text += tr("\n\n还原前的快照已保存在：\n%1")
                        .arg(QDir::toNativeSeparators(result.safetyZip));
        }
        log(tr("%1：%2").arg(successTitle, tr("已校验 %1 个文件").arg(result.checked)));
        InlineMessage::information(m_overlay, successTitle, text);
    } else if (cancelled) {
        log(tr("%1：已取消。").arg(failureTitle.isEmpty() ? tr("还原") : failureTitle));
    } else {
        const QString title = failureTitle.isEmpty() ? tr("没有完成") : failureTitle;
        log(tr("%1：%2").arg(title, result.problem));
        InlineMessage::warning(m_overlay, title, result.problem);
    }
    const bool diskChanged = result.ok || result.stage == BackupFiles::RestoreResult::Stage::RemoveOld
                             || result.stage == BackupFiles::RestoreResult::Stage::Extract
                             || result.stage == BackupFiles::RestoreResult::Stage::Verify;
    if (diskChanged && !m_refreshWorldDir.isEmpty() && !m_refreshDialog.isNull()
        && m_host.refreshWorld) {
        m_host.refreshWorld(m_refreshDialog.data(), m_refreshWorldDir);
    }
    m_refreshWorldDir.clear();
    m_refreshDialog = nullptr;
    if (diskChanged && !m_refreshServerId.isEmpty() && m_host.refreshServer) {
        m_host.refreshServer(m_refreshServerId);
    }
    m_refreshServerId.clear();
    // 标记一定要摘：还原在动盘之前就失败的话（读清单 / 空间不够）上面那一页不刷新，
    // 但"动不得"是这一趟自己记的，不能指望别人来摘。
    if (!m_busyServerId.isEmpty() && m_host.setServerFileOperationBusy) {
        m_host.setServerFileOperationBusy(m_busyServerId, false);
    }
    m_busyServerId.clear();

    // 还原的是**一整个实例**（不是某个世界）：盘上的 json / meta / 图标全换了 ⇒
    // 实例清单要重扫一遍；开着的那一页控件的值还是还原之前的，动过盘就把它收掉
    // （`reject()` 和顶上那颗返回是同一条路）。一个字节都没动过的话留着它，
    // 只把文件页重列一次（那种失败多半是"快照读不出来 / 空间不够"）。
    if (diskChanged && !restoredId.isEmpty() && m_host.refreshInstances) {
        m_host.refreshInstances();
    }
    if (!restoredDialog.isNull()) {
        if (diskChanged) {
            restoredDialog->reject();
        } else {
            restoredDialog->refreshFilePages();
        }
    }
}
