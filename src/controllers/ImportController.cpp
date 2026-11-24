#include "ImportController.hpp"
#include <QCoreApplication>
#include "TextMarks.hpp"
#include "MainWindow.hpp"
#include "InlineMessage.hpp"
#include "InstanceFilesPage.hpp"
#include "InstanceSettingsDialog.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "download/ResourceCatalog.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/UpgradeAssessment.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/DroppedResource.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/ModpackImport.hpp"
#include "util/SevenZip.hpp"
#include "manage/ModpackInstallReport.hpp"
#include "manage/ModpackCompletion.hpp"
#include "manage/VersionManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "tasks/TaskManager.hpp"
#include "util/AppPaths.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include <QDir>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QListWidget>
#include <QLabel>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>

ImportController::ImportController(MainWindow *owner) : QObject(owner), m_owner(owner) {
    // PCL 那种"压缩包里套一份 .mrpack"解出来的临时包：导入整段（成功、失败、取消）结束时
    // 删掉，判据就是那个唯一的终态信号。提前退出那些路自己删（见 importModpackFromFile
    // 里那个守卫），不会登记到这里。
    if (owner != nullptr && owner->m_versionManager != nullptr) {
        connect(owner->m_versionManager, &VersionManager::versionDownloadFinished, this,
                [this](const QString &versionId, bool ok, const QString &) {
                    const QString temp = m_tempArchives.take(versionId);
                    if (!temp.isEmpty()) {
                        QFile::remove(temp);
                    }
                    const QString special = m_specialPackSources.take(versionId);
                    if (ok && !special.isEmpty()) {
                        preservePackSource(versionId, special);
                    }
                });
    }
}

namespace {
QString pageKeyForKind(ResourceCatalog::Kind kind) { return ResourceCatalog::kindKey(kind); }

/// 把一段阻塞的重活放到工作线程上跑，UI 线程用**嵌套事件循环**等它。
///
/// 唯一的用户是 CurseForge 整合包预览：那一发是同步 HTTP（每批 100 条、一个整合包
/// 通常两三类请求），整段跑在 UI 线程上的话网络慢的时候窗口不重绘，底栏那句
/// 「正在向 CurseForge 换取文件清单…」也只是一张不动的截图。
/// `resolveCurseForgeFiles()` 自己开 QNAM（不借主线程那个），所以能安全地放在这边。
///
/// 嵌套循环会把用户输入送回来 ⇒ 调用方要自己挡住"再来一次"（见 `m_importFlow`）。
template <typename Work>
void runOffUiThread(Work work) {
    QEventLoop loop;
    QThread *worker = QThread::create([work] { work(); });
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
}

/// 进流程时置真、离开（含所有 early return）复原。
class FlowGuard {
public:
    explicit FlowGuard(bool &flag) : m_flag(flag) { m_flag = true; }
    ~FlowGuard() { m_flag = false; }
    FlowGuard(const FlowGuard &) = delete;
    FlowGuard &operator=(const FlowGuard &) = delete;
private:
    bool &m_flag;
};
}

void ImportController::onImportModpackClicked() {
    if (m_owner->m_downloadInFlight) {
        m_owner->appendLog(tr("已有下载任务正在进行，请等它结束。"));
        return;
    }
    // 另一条导入流程还没走完（多半正卡在 CurseForge 预览那一发网络请求上，见
    // `m_importFlow`）：别开第二个文件选择框，直接说清楚。
    if (m_importFlow) {
        m_owner->appendLog(tr("已有导入流程正在进行，请等它结束。"));
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"),
                                   tr("另一个整合包还在导入流程里，等它结束之后再试。"));
        return;
    }

    // NOTE: 这一处也是独立窗（Windows 自己的文件选择框）—— 理由和取舍见
    // onAddFolderClicked() 上面那一段。
    const QString zipPath = QFileDialog::getOpenFileName(
        m_owner, tr("选择整合包"), QString(),
        tr("整合包 (*.zip *.mrpack *.rar *.7z);;继续补齐报告 (wtfml.import-report.json);;所有文件 (*)"));
    if (zipPath.isEmpty()) {
        return;  // 用户取消：什么都不做，也**不弹**任何提示
    }
    m_owner->importModpackFromFile(zipPath);
}

struct ImportController::PreparedModpack {
    QString effectiveZip;
    QString tempZip;
    QString sourceZip;
    // 壳里那一份解出来的临时文件：**只有这一趟提前退出时**才由下面的析构函数删掉；
    // 导入真的开始了就 releaseTempArchive() 并交给 m_tempArchives，等 versionDownloadFinished 再删。
    QString tempArchive;
    ModpackImport::ArchiveLayout layout;
    ModpackImport::Pack pack;
    QString manifestFingerprint;
    bool ok = false;
    QString failureSummary;

    void releaseTempArchive() { tempArchive.clear(); }

    ~PreparedModpack() {
        if (!tempArchive.isEmpty()) {
            QFile::remove(tempArchive);
        }
        if (!tempZip.isEmpty()) {
            QFile::remove(tempZip);
        }
    }
};

void ImportController::finishAbortedImportTask(Tasks::TaskId taskId, bool cancelled,
                                               const QString &reason) {
    if (taskId == Tasks::invalidTaskId() || m_owner->m_activityCenter == nullptr
        || !m_owner->m_activityCenter->tasks()->has(taskId)) {
        return;
    }
    Tasks::TaskManager *tasks = m_owner->m_activityCenter->tasks();
    if (cancelled) {
        tasks->cancel(taskId);
        m_owner->m_downloadTaskLedger->clearOwnerIf(taskId);
        return;
    }
    const QList<Tasks::TaskStep> steps = tasks->snapshot(taskId).steps;
    for (int i = 0; i < steps.size(); ++i) {
        if (steps.at(i).state == Tasks::TaskStepState::Running) {
            tasks->setStepState(taskId, i, Tasks::TaskStepState::Failed, reason);
        }
    }
    m_owner->finishTask(taskId, false, reason);
}

bool ImportController::prepareModpackPackage(const QString &zipPath, PreparedModpack *out) {
    if (out == nullptr) {
        return false;
    }
    out->effectiveZip = zipPath;
    // rar/7z：先让 7-Zip 解到临时目录并压成 zip，再走下面同一条路。
    if (SevenZip::isArchive(zipPath)) {
        m_owner->appendLog(tr("正在用 7-Zip 解压 %1…").arg(QFileInfo(zipPath).fileName()));
        QString error;
        out->tempZip = SevenZip::toZip(zipPath, &error);
        if (out->tempZip.isEmpty()) {
            out->failureSummary = error.isEmpty() ? tr("无法解压这个包。") : error;
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("解压失败"),
                                   out->failureSummary);
            return false;
        }
        out->effectiveZip = out->tempZip;
    }

    // 先把**条目名**列一遍（不解压任何东西）。这一步代替了以前"按两个固定名字各读一次"：
    // 多花一个 PowerShell 进程，换来的是能认出没有固定清单的那几种包 —— MultiMC / Prism
    // 导出的实例、直接压进来的 .minecraft、PCL 那种"里面套一份 .mrpack"的壳。
    //
    // NOTE: 动作名是**给用户看的**：Failed 那一路会把 error 原样弹进那个警告框
    // （「读这个压缩包的时候出错了：…」），所以这里写「读压缩包的条目表」，而不是
    // ZipUtil 那个通用的默认说法。
    QString error;
    QStringList entries;
    ZipUtil::EntryStatus status = ZipUtil::listEntries(
        out->effectiveZip, &entries, &error, tr("读压缩包的条目表"));
    if (status != ZipUtil::EntryStatus::Ok) {
        // "里面没有清单"和"这个文件根本打不开"是**两句完全不同的话**：前者多半是选错了
        // 文件，后者是文件坏了/不是 zip。
        QString text;
        if (status == ZipUtil::EntryStatus::NotAnArchive) {
            text = tr("无法打开这个文件：它不是 zip 压缩包（也可能已损坏）。\n\n"
                      "整合包得是 zip：Modrinth 的 .mrpack 也是 zip，只是换了个后缀。");
        } else {
            text = tr("读这个压缩包的时候出错了：\n%1").arg(error);
        }
        out->failureSummary = text;
        m_owner->appendLog(tr("没有导入 %1。").arg(QFileInfo(out->effectiveZip).fileName()));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("这不是整合包"), text);
        return false;
    }

    out->layout = ModpackImport::detectArchiveLayout(entries);
    // 真正拿去解压的那一份。PCL 那种壳要先把里面那份 .mrpack 解出来，之后一律用它。
    // 壳里那一份解出来的临时文件：**只有这一趟提前退出时**才由 PreparedModpack 析构删掉；
    // 导入真的开始了就交给 m_tempArchives，等 versionDownloadFinished 再删。
    out->sourceZip = out->effectiveZip;

    if (out->layout.format == ModpackImport::Format::Wrapper) {
        QDir().mkpath(AppPaths::modpackCacheDir());
        out->tempArchive = AppPaths::modpackCacheDir() + QStringLiteral("/wrapper-") +
                           QUuid::createUuid().toString(QUuid::WithoutBraces) +
                           QStringLiteral(".mrpack");
        status = ZipUtil::extractEntryToFile(out->sourceZip, out->layout.innerPackEntry,
                                             out->tempArchive, &error,
                                             tr("解出压缩包里的整合包"));
        if (status != ZipUtil::EntryStatus::Ok) {
            out->failureSummary = tr("这个压缩包里那份 %1 没能解出来：\n%2")
                                      .arg(out->layout.innerPackEntry, error);
            m_owner->appendLog(tr("没有导入 %1：解不出里面的 %2。")
                                   .arg(QFileInfo(out->effectiveZip).fileName(),
                                        out->layout.innerPackEntry));
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("导入整合包"),
                                   out->failureSummary);
            return false;
        }
        m_owner->appendLog(tr("「%1」是一个 PCL 打包的整合包，先解出里面的 %2。")
                               .arg(QFileInfo(out->effectiveZip).fileName(),
                                    out->layout.innerPackEntry));

        QStringList innerEntries;
        status = ZipUtil::listEntries(out->tempArchive, &innerEntries, &error,
                                      tr("读压缩包的条目表"));
        const ModpackImport::ArchiveLayout inner =
            status == ZipUtil::EntryStatus::Ok
                ? ModpackImport::detectArchiveLayout(innerEntries)
                : ModpackImport::ArchiveLayout();
        if (status != ZipUtil::EntryStatus::Ok ||
            inner.format == ModpackImport::Format::Unknown ||
            inner.format == ModpackImport::Format::Wrapper) {
            out->failureSummary =
                status != ZipUtil::EntryStatus::Ok
                    ? tr("这个压缩包里那份 %1 打不开：\n%2")
                          .arg(out->layout.innerPackEntry, error)
                    : ModpackImport::unsupportedFormatText(inner.format);
            m_owner->appendLog(tr("没有导入 %1：里面的 %2 认不出是什么格式。")
                                   .arg(QFileInfo(out->effectiveZip).fileName(),
                                        out->layout.innerPackEntry));
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("这不是整合包"),
                                   out->failureSummary);
            return false;
        }
        entries = innerEntries;
        out->layout = inner;
        out->sourceZip = out->tempArchive;
    }

    const auto refuse = [this, out](const QString &title, const QString &text) {
        out->failureSummary = text;
        m_owner->appendLog(tr("没有导入 %1。").arg(QFileInfo(out->effectiveZip).fileName()));
        InlineMessage::warning(m_owner->m_ui->overlay(), title, text);
    };

    // MultiMC / Prism / .minecraft 这类包的 overridesDir 要等解析之后才知道，形状那两种
    // 拒绝（服务端 / 存档）也能直接从条目表看出来，且**必须**在解析之前判。
    if (out->layout.serverPack) {
        refuse(tr("这不是客户端整合包"),
               tr("这个压缩包里是一个服务端包（里面没有客户端要的 config、"
                           "资源包这些，只有服务端启动用的文件）。\n\n"
                           "客户端无法安装它。要开服的话用「新建服务端」，把这份包解到"
                           "服务端目录里。"));
        return false;
    }
    if (out->layout.worldSave) {
        refuse(tr("这是一个世界存档"),
               tr("这个压缩包里是一个世界存档（里面有 level.dat），不是整合包。\n\n"
                           "存档请放进实例的 saves/ 目录：在「存档」页点「导入存档…」，"
                           "或者直接把压缩包拖到那一页上。"));
        return false;
    }
    if (out->layout.modsBundle) {
        refuse(tr("这不是整合包"),
               tr("这个压缩包里只有一堆模组（mods/ 或者几份 .jar），没有整合包清单。\n\n"
                           "它不是整合包，按整合包导进来只会建一个空实例。要用这些模组的话："
                           "在实例的「模组」页点「安装模组…」逐份添加，或者把它解压后把 "
                           "jar 拖到那一页上。"));
        return false;
    }
    if (out->layout.format == ModpackImport::Format::Unknown) {
        refuse(tr("这不是整合包"), ModpackImport::unsupportedFormatText(out->layout.format));
        return false;
    }

    // 按格式读那一份"身份文件"。MultiMC / 懒人包没有清单，身份在 mmc-pack.json /
    // 版本 json 里 —— 指纹照样对那份字节算。
    QByteArray manifestJson;
    ModpackImport::Result result;
    if (out->layout.format == ModpackImport::Format::MultiMC) {
        QByteArray mmcJson;
        status = ZipUtil::readEntry(out->sourceZip, out->layout.mmcPackEntry, &mmcJson, &error,
                                    tr("读整合包的 mmc-pack.json"), 4 * 1024 * 1024);
        if (status != ZipUtil::EntryStatus::Ok) {
            refuse(tr("导入整合包"),
                   status == ZipUtil::EntryStatus::NotAnArchive
                       ? tr("无法打开这个文件：它不是 zip 压缩包（也可能已损坏）。")
                       : tr("读这个压缩包里的 mmc-pack.json 时出错了：\n%1").arg(error));
            return false;
        }
        QByteArray instanceCfg;
        if (!out->layout.instanceCfgEntry.isEmpty()) {
            QString cfgError;
            // 读不到 instance.cfg **不算失败**：它只提供实例名，名字还有压缩包文件名兜底。
            ZipUtil::readEntry(out->sourceZip, out->layout.instanceCfgEntry, &instanceCfg,
                               &cfgError, tr("读整合包的 instance.cfg"), 1024 * 1024);
        }
        manifestJson = mmcJson;
        if (ModpackImport::archiveHasEntryPrefix(
                entries, out->layout.rootPrefix + QStringLiteral("patches/"))) {
            QHash<QString, QByteArray> patches, libraries;
            qint64 total = 0;
            const QString patchPrefix = out->layout.rootPrefix + QStringLiteral("patches/");
            const QString libraryPrefix = out->layout.rootPrefix + QStringLiteral("libraries/");
            for (const QString &entry : entries) {
                const bool patch = entry.startsWith(patchPrefix) && entry.endsWith(QLatin1String(".json"));
                const bool library = entry.startsWith(libraryPrefix) && entry.endsWith(QLatin1String(".jar"));
                if (!patch && !library) continue;
                const QString relative = entry.mid(patch ? patchPrefix.size() : libraryPrefix.size());
                if (relative.contains(QLatin1Char('/')) || !ModpackImport::isSafeRelativePath(relative) ||
                    patches.size() >= 64 || libraries.size() >= 64) {
                    refuse(tr("启动配置不支持"), tr("补丁/专用库路径不规范或数量超过限制。"));
                    return false;
                }
                QByteArray bytes;
                if (ZipUtil::readEntry(out->sourceZip, entry, &bytes, &error, tr("读取启动补丁及专用库"),
                                      patch ? 2 * 1024 * 1024 : 64 * 1024 * 1024) != ZipUtil::EntryStatus::Ok ||
                    (total += bytes.size()) > 128 * 1024 * 1024) {
                    refuse(tr("读取启动配置失败"), error.isEmpty() ? tr("启动配置超过大小限制。") : error);
                    return false;
                }
                if (patch) {
                    const QString uid = relative.left(relative.size() - 5);
                    if (patches.contains(uid)) {
                        refuse(tr("补丁重复"), uid);
                        return false;
                    }
                    patches.insert(uid, bytes);
                } else {
                    libraries.insert(entry, bytes);
                }
            }
            result = ModpackImport::parsePatchedMultiMCInstance(mmcJson, instanceCfg, patches, libraries,
                out->layout.gameDirPrefix, QFileInfo(out->effectiveZip).completeBaseName());
            // Resume identity includes executable dependencies, not just mmc-pack.json.
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(mmcJson);
            for (const auto *parts : {&patches, &libraries}) {
                QStringList keys = parts->keys();
                keys.sort();
                for (const QString &key : keys) {
                    hash.addData(key.toUtf8());
                    hash.addData(QByteArray(1, '\0'));
                    hash.addData(QCryptographicHash::hash(parts->value(key), QCryptographicHash::Sha256));
                }
            }
            manifestJson += hash.result();
        } else {
            result = ModpackImport::parseMultiMCInstance(
                mmcJson, instanceCfg, out->layout.gameDirPrefix,
                QFileInfo(out->effectiveZip).completeBaseName());
        }
    } else if (out->layout.format == ModpackImport::Format::GameFolder) {
        QByteArray versionJson;
        if (!out->layout.versionJsonEntry.isEmpty()) {
            // 版本 json 通常几 KB；给一个上限免得有人把几百 MB 的东西塞成这个文件名。
            QString jsonError;
            if (ZipUtil::readEntry(out->sourceZip, out->layout.versionJsonEntry, &versionJson,
                                   &jsonError, tr("读整合包的版本 json"),
                                   4 * 1024 * 1024) == ZipUtil::EntryStatus::Ok) {
                manifestJson = versionJson;
            }
        }
        result = ModpackImport::parseGameFolderInstance(entries, versionJson, out->layout.rootPrefix,
                                                        QFileInfo(out->effectiveZip).completeBaseName());
        if (out->layout.legacyLauncherJar) {
            // 旧版 Technic / 老式 Forge 包：加载器在 `bin/modpack.jar` 里，不写进版本 json。
            // 这一层不做推断（见 ArchiveLayout::legacyLauncherJar 的说明），但要在日志里留下
            // 一句话 —— 否则用户装完点启动，看到的是"找不到类"，而不是"加载器还没装"。
            m_owner->appendLog(tr("此整合包包含内置加载器。导入后请在实例中"
                                           "补装加载器，否则游戏无法启动。"));
        }
    } else {
        status = ZipUtil::readEntry(out->sourceZip, out->layout.manifestEntry, &manifestJson, &error,
                                    tr("读整合包清单"), 16 * 1024 * 1024);
        if (status != ZipUtil::EntryStatus::Ok) {
            refuse(tr("导入整合包"),
                   status == ZipUtil::EntryStatus::NotAnArchive
                       ? tr("无法打开这个文件：它不是 zip 压缩包（也可能已损坏）。")
                       : tr("读这个压缩包里的清单时出错了：\n%1").arg(error));
            return false;
        }
        // 空 `files` 先放行，**解析完再核一次**：只有压缩包里真有清单说的那一层
        // （overrides / overrides-1.20.1 …）才算合法的"纯 overrides 包"。这里不能直接
        // 拒 —— 那份目录名写在清单里，得先解析才知道。
        const bool allowEmpty = true;
        result = out->layout.format == ModpackImport::Format::CurseForge
                     ? ModpackImport::parseCurseForgeManifest(manifestJson, allowEmpty)
                     : ModpackImport::parseModrinthIndex(manifestJson, allowEmpty);
    }

    // 解析（纯函数）。清单读不懂、不是 Minecraft 的包、路径不安全、一条文件都没有，
    // 四种情况的原因都不一样，都由它一句话说清 —— 界面不自己编第二句。
    if (!result.ok) {
        out->failureSummary = result.problem;
        m_owner->appendLog(tr("整合包清单无法解析：%1").arg(result.problem));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("导入整合包"), result.problem);
        return false;
    }
    out->pack = result.pack;

    // 零文件的包：清单说"都在这份压缩包里"，那就得真的在里面。这一条是"坏清单被当成
    // 纯 overrides 包装进去"最后一道闸。
    if (out->pack.fileCount() == 0 &&
        (out->layout.format == ModpackImport::Format::Modrinth ||
         out->layout.format == ModpackImport::Format::CurseForge)) {
        const QString contentPrefix = out->layout.rootPrefix + out->pack.overridesDir;
        const bool hasClientOverrides = !out->pack.clientOverridesDir.isEmpty() &&
            ModpackImport::archiveHasEntryPrefix(entries, out->layout.rootPrefix + out->pack.clientOverridesDir);
        if (!ModpackImport::archiveHasEntryPrefix(entries, contentPrefix) && !hasClientOverrides) {
            refuse(tr("导入整合包"),
                   tr("这份整合包的清单里一条文件都没有，压缩包里也没有它说的那一层"
                               "（%1）。\n\n清单可能被改坏了，没有导入。")
                       .arg(out->pack.overridesDir));
            return false;
        }
    }

    if ((out->layout.format == ModpackImport::Format::Modrinth ||
         out->layout.format == ModpackImport::Format::CurseForge) && !out->layout.rootPrefix.isEmpty()) {
        out->pack.overridesDir.prepend(out->layout.rootPrefix);
        if (!out->pack.clientOverridesDir.isEmpty()) {
            out->pack.clientOverridesDir.prepend(out->layout.rootPrefix);
        }
    }

    // 清单指纹（原始字节的 sha256）：问名字那一步拿它判断"同名的那一份是不是同一个包"，
    // 导入开始后写进 `wtfml.importing` 标记，供下一次同名导入比较。同一个包重新打一次
    // zip，里面那份清单的字节不变 ⇒ 仍然认得出是同一个包。
    out->manifestFingerprint = InstanceFiles::importManifestFingerprint(manifestJson);

    // 加载器要不上，在**问名字之前**就拦掉：等问完名字才说装不了，只是白耽误用户。
    const QString loaderProblem =
        ModpackImport::unsupportedLoaderText(out->pack.loaderKey, out->pack.loaderVersion);
    if (!loaderProblem.isEmpty()) {
        out->failureSummary = loaderProblem;
        m_owner->appendLog(tr("未导入 %1：需要 %2，当前启动器无法安装。")
                               .arg(QFileInfo(out->effectiveZip).fileName(), out->pack.loaderKey));
        InlineMessage::information(m_owner->m_ui->overlay(), tr("无法安装这个整合包"),
                                   loaderProblem);
        return false;
    }

    out->ok = true;
    return true;
}

bool ImportController::resolveCurseForgeManifestIfNeeded(ModpackImport::Pack *pack,
                                                       ModpackImport::Format format,
                                                       const QString &sourceName,
                                                       QString *problemOut) {
    // 这一步只有 CurseForge 需要：那一份清单里**一个下载地址都没有**，只有一串编号
    // （projectID / fileID），得先问一次接口才换得出来（判据与实测见
    // ModpackImport.hpp 顶上那段）。放在这里、而不是数据层里，是因为**用户要在点确认
    // 之前看到真实数字**（手动导入确认框要报出下载数，一键安装要在开始前决定是否提示
    // fileIssues）：这个包要下多少个文件、其中几个作者不许第三方代下 —— 那些数只有
    // 换完才知道。其他格式这一句立刻返回，不联网。
    if (format != ModpackImport::Format::CurseForge) {
        return true;
    }
    // 等一两发请求（通常一两秒）。底栏那格要顶着"在等什么"，不然用户看到的是
    // 点完确认之后界面卡住（和启动路径那三段同步等是同一个手法）。
    // 请求本身挪到工作线程上（`runOffUiThread()`）。网络慢的时候
    // UI 线程不再被钉住 —— 窗口照常重绘，底栏那句一直看得见。
    QString problem;
    VersionManager *versions = m_owner->m_versionManager;
    const VersionManager::CurseForgeResolveSettings settings = versions->curseForgeResolveSettings();
    m_owner->enterSyncWait(tr("正在向 CurseForge 换取文件清单..."));
    runOffUiThread([&] { problem = versions->resolveCurseForgeFiles(pack, settings); });
    m_owner->leaveSyncWait();
    if (!problem.isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = problem;
        }
        m_owner->appendLog(tr("没有导入 %1：%2").arg(sourceName, problem));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("拿不到文件清单"), problem);
        return false;
    }
    return true;
}

bool ImportController::promptCurseForgeFileIssuesIfNeeded(const ModpackImport::Pack &pack,
                                                          const QString &instanceName) {
    if (pack.fileIssues.isEmpty()) {
        return true;
    }
    QStringList entries;
    for (const auto &issue : pack.fileIssues) {
        entries.append(QStringLiteral("%1 — %2").arg(
            issue.fileName.isEmpty() ? tr("文件 %1").arg(issue.ref.fileId)
                                     : issue.fileName.simplified(),
            issue.reason.simplified()));
    }
    int selected = 0;
    for (;;) {
        InlineMessageBox box(MessageKind::Warning, tr("有文件需要补齐"),
            tr("选中文件可打开对应网页。可以先安装其余内容，但文件未补齐时不会标记为成功。"
                "\n补齐位置：%1\n重新导入同一个包和实例名可重新校验。").arg(
                QDir::toNativeSeparators(AppPaths::versionDir(instanceName))),
            {tr("继续安装其余内容"), tr("打开所选文件网页"), tr("取消")},
            2, -1);
        auto *list = box.setList(entries, selected);
        const auto updateDetails = [&](int row) {
            if (row < 0 || row >= pack.fileIssues.size()) return;
            box.setNote(ModpackInstallReport::description(pack.fileIssues.at(row)));
            // 元数据来自外部，文件名中的标签也只能当普通文本显示。
            if (auto *label = box.findChild<QLabel *>(InlineMessageBox::noteLabelName()))
                label->setTextFormat(Qt::PlainText);
        };
        updateDetails(selected);
        QObject::connect(list, &QListWidget::currentRowChanged, &box, updateDetails);
        m_owner->m_ui->overlay()->execPrompt(&box);
        selected = box.listRow();
        if (box.clickedIndex() == 0) return true;
        if (box.clickedIndex() != 1) return false;
        if (selected < 0 || selected >= pack.fileIssues.size()) continue;
        const auto url = ModpackImport::curseForgeFileWebUrl(pack.fileIssues.at(selected));
        if (!url.isEmpty()) {
            QDesktopServices::openUrl(QUrl(url));
        } else {
            InlineMessage::information(m_owner->m_ui->overlay(), tr("没有文件网页"),
                tr("平台未提供规范项目网址，无法可靠打开文件页。请按报告中的项目和文件编号手动查找；这不代表文件已下架。"));
        }
    }
}

bool ImportController::resolveInstallerJavaForPack(const ModpackImport::Pack &pack,
                                                   bool allowInteractivePrompt,
                                                   QString *installerJavaOut,
                                                   bool *cancelledOut,
                                                   QString *reasonOut) {
    // 判据说明见 applyResolvedModpackImport() 里安装器 Java 那段；
    // 这里额外支持一键安装路径在未自动找到 Java 时交互下载或手动选择 java.exe。
    if (cancelledOut != nullptr) {
        *cancelledOut = false;
    }
    QString installerJava = m_owner->m_javaSettings->javaPath();
    VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
    const bool hasLoader =
        !pack.loaderKey.isEmpty() &&
        VersionManager::loaderKindForDependencyKey(pack.loaderKey, &kind);
    if (!hasLoader || kind == VersionManager::LoaderKind::Fabric
        || kind == VersionManager::LoaderKind::Quilt || !pack.patchedRuntime.isEmpty()) {
        if (installerJavaOut != nullptr) {
            *installerJavaOut = installerJava;
        }
        return true;
    }

    int requiredJavaMajor = 0;
    const QJsonObject gameJson = m_owner->m_versionManager->getVersionJson(pack.gameVersion);
    if (!gameJson.isEmpty()) {
        requiredJavaMajor = gameJson["javaVersion"].toObject()["majorVersion"].toInt();
    }
    const QString configuredJava = installerJava;
    QString javaReason;
    if (m_owner->m_javaSettings->installerJavaFor(requiredJavaMajor, configuredJava, &installerJava,
                                                  &javaReason)) {
        if (installerJava != configuredJava) {
            m_owner->appendLog(tr("这个版本要 Java %1，装的时候改用 %2")
                                   .arg(requiredJavaMajor)
                                   .arg(installerJava));
        }
        if (installerJavaOut != nullptr) {
            *installerJavaOut = installerJava;
        }
        return true;
    }

    if (!allowInteractivePrompt) {
        if (reasonOut != nullptr) {
            *reasonOut = javaReason;
        }
        m_owner->appendLog(tr("这个版本要的 Java 拿不到：%1").arg(javaReason));
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("Java 版本不够"), javaReason);
        return false;
    }

    while (true) {
        const QString downloadText =
            requiredJavaMajor > 0 ? tr("自动下载 Java %1").arg(requiredJavaMajor) : QString();
        QStringList buttons;
        if (!downloadText.isEmpty()) {
            buttons << downloadText;
        }
        const int browseIndex = buttons.size();
        buttons << tr("手动选择 java.exe...") << tr("取消");
        const int choice = InlineMessage::ask(
            m_owner->m_ui->overlay(), MessageKind::Question, tr("需要 Java 环境"),
            tr("安装此整合包的加载器需要合适的 Java，但本机未自动找到可用版本。\n\n%1")
                .arg(javaReason),
            buttons, 0, -1);
        if (!downloadText.isEmpty() && choice == 0) {
            GlobalJavaSettings *javaStore = m_owner->m_javaSettings;
            EffectiveJavaSettings io = javaStore->effective();
            bool ready = false;
            QString downloadError;
            m_owner->enterSyncWait(tr("正在下载 Java %1 运行时...").arg(requiredJavaMajor));
            runOffUiThread([&] {
                ready = javaStore->ensureJavaForVersion(io, requiredJavaMajor, QString(), true,
                                                        &downloadError, false, true);
            });
            m_owner->leaveSyncWait();
            if (ready && m_owner->m_javaSettings->installerJavaFor(
                             requiredJavaMajor, io.javaPath, &installerJava, &javaReason)) {
                if (installerJavaOut != nullptr) {
                    *installerJavaOut = installerJava;
                }
                return true;
            }
            if (!downloadError.isEmpty()) {
                javaReason = downloadError;
            }
            continue;
        }
        if (choice == browseIndex) {
            const QString picked = QFileDialog::getOpenFileName(
                m_owner, tr("选择 java.exe"), QString(),
                tr("Java 可执行文件 (java.exe);;所有文件 (*)"));
            if (picked.isEmpty()) {
                continue;
            }
            if (m_owner->m_javaSettings->installerJavaFor(
                    requiredJavaMajor, picked, &installerJava, &javaReason)) {
                if (installerJavaOut != nullptr) {
                    *installerJavaOut = installerJava;
                }
                return true;
            }
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("Java 版本不匹配"),
                                   javaReason);
            continue;
        }
        if (cancelledOut != nullptr) {
            *cancelledOut = true;
        }
        if (reasonOut != nullptr) {
            *reasonOut = javaReason;
        }
        return false;
    }
}

void ImportController::importModpackOneClick(const QString &zipPath,
                                             const QString &confirmedInstanceName,
                                             Tasks::TaskId taskId) {
    // 同 importModpackFromFile() 那两道闸（m_downloadInFlight / m_importFlow），见那里。
    if (m_owner->m_downloadInFlight) {
        const QString reason = tr("已有下载或导入任务正在进行，等它结束之后再试。");
        m_owner->appendLog(tr("已有下载或导入任务正在进行，%1 没有导入。")
                               .arg(QFileInfo(zipPath).fileName()));
        finishAbortedImportTask(taskId, false, reason);
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"), reason);
        return;
    }
    if (m_importFlow) {
        const QString reason = tr("另一个整合包还在导入流程里，等它结束之后再试。");
        m_owner->appendLog(tr("已有导入流程正在进行，%1 没有导入。")
                               .arg(QFileInfo(zipPath).fileName()));
        finishAbortedImportTask(taskId, false, reason);
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"), reason);
        return;
    }
    const FlowGuard flowGuard(m_importFlow);

    // 同 importModpackFromFile()：进流程到离开前锁住游戏文件夹切换，所有 early return 由 folderHold 摘掉。
    m_owner->beginFolderSwitchHold();
    const auto folderHold = qScopeGuard([this] { m_owner->endFolderSwitchHold(); });

    PreparedModpack prepared;
    if (!prepareModpackPackage(zipPath, &prepared)) {
        finishAbortedImportTask(taskId, false, prepared.failureSummary);
        return;
    }

    QString problem;
    if (!resolveCurseForgeManifestIfNeeded(&prepared.pack, prepared.layout.format,
                                           QFileInfo(prepared.effectiveZip).fileName(), &problem)) {
        finishAbortedImportTask(taskId, false, problem);
        return;
    }

    QString name = confirmedInstanceName.trimmed();
    QString nameProblem;
    InstanceFiles::ImportNameVerdict verdict =
        InstanceFiles::checkImportTargetName(name, &nameProblem, prepared.manifestFingerprint);
    if (verdict == InstanceFiles::ImportNameVerdict::Taken) {
        QString candidate = ModpackImport::suggestUniqueInstanceName(
            name.isEmpty() ? prepared.pack.name : name);
        const QString conflictPrompt =
            (InstanceFiles::validateDirName(name).isEmpty() && InstanceFiles::isUnfinishedImport(name))
                ? tr("「%1」中是另一个整合包（或另一版本）未完成的安装，不能装入同一目录。\n\n"
                              "请另取一个实例名称，原有文件不受影响。")
                      .arg(name)
                : tr("%1\n\n请输入新的实例名称。").arg(nameProblem);
        while (true) {
            bool ok = false;
            const QString input = InlineMessage::getText(
                m_owner->m_ui->overlay(), tr("换个名字"), conflictPrompt, candidate, &ok);
            if (!ok) {
                finishAbortedImportTask(taskId, true, QString());
                return;
            }
            candidate = input.trimmed();
            verdict = InstanceFiles::checkImportTargetName(
                candidate, &nameProblem, prepared.manifestFingerprint);
            if (verdict != InstanceFiles::ImportNameVerdict::Taken) {
                name = candidate;
                break;
            }
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("换个名字"), nameProblem);
        }
        if (taskId != Tasks::invalidTaskId() && m_owner->m_activityCenter != nullptr) {
            m_owner->m_activityCenter->tasks()->setTitleAndTarget(
                taskId, tr("安装整合包 %1").arg(name), name);
        }
    }
    if (verdict == InstanceFiles::ImportNameVerdict::OkResume) {
        // 下载前命名框已明确告知会继续安装，此处不再二次弹窗确认，仅留日志。
        m_owner->appendLog(tr("继续安装上次未完成的「%1」。").arg(name));
    }

    if (!promptCurseForgeFileIssuesIfNeeded(prepared.pack, name)) {
        finishAbortedImportTask(taskId, true, QString());
        return;
    }

    QString installerJava;
    bool javaCancelled = false;
    QString javaReason;
    if (!resolveInstallerJavaForPack(prepared.pack, true, &installerJava, &javaCancelled, &javaReason)) {
        finishAbortedImportTask(taskId, javaCancelled, javaReason);
        return;
    }

    if (taskId != Tasks::invalidTaskId() && m_owner->m_activityCenter != nullptr
        && m_owner->m_activityCenter->tasks()->has(taskId)) {
        Tasks::TaskManager *tasks = m_owner->m_activityCenter->tasks();
        tasks->setStepState(taskId, 1, Tasks::TaskStepState::Done);
        tasks->setStepState(taskId, 2, Tasks::TaskStepState::Running);
        m_owner->m_downloadTaskLedger->adopt(taskId);
    }

    m_owner->markOneClickModpack(name);
    if (!prepared.tempArchive.isEmpty()) {
        m_tempArchives.insert(name, prepared.tempArchive);
    }
    const bool started = applyResolvedModpackImport(
        prepared.sourceZip, name, prepared.pack, prepared.manifestFingerprint, installerJava);
    if (started && !prepared.tempArchive.isEmpty()) {
        // 同 importModpackFromFile()：移交给 versionDownloadFinished 清理。
        prepared.releaseTempArchive();
    } else if (!started) {
        m_tempArchives.remove(name);
        m_owner->takeOneClickModpack(name);
        if (m_owner->m_downloadTaskLedger->task() == taskId) {
            m_owner->m_downloadTaskLedger->clear();
        }
        finishAbortedImportTask(taskId, false, tr("未能启动整合包安装。"));
    }
}

void ImportController::importModpackFromFile(const QString &zipPath, const QString &namePrefill) {
    // 拖进来的这一路也得过忙态这道闸。上面那颗「导入整合包…」按钮在忙的
    // 时候是灰的（`setDownloadUiBusy()`），但主窗口的拖放照收不误（`dropEvent()` 不看
    // 忙态）。不拦的话：导入 A 的过程中再拖进来一个 B，B 会从问名字那一步一路走到下面
    // 把「导入」那一档占用改成 B —— A 的 id 被顶掉（A 下完时 `clearFor()` 对不上它，
    // 连忙态都不收）、任务中心当前那条任务还是 A 的，而两条
    // 导入同时在往 versions 下面写。判据和按钮那一处**是同一句话**（`m_downloadInFlight`），
    // 不另立一个。
    if (m_owner->m_downloadInFlight) {
        m_owner->appendLog(tr("已有下载或导入任务正在进行，%1 没有导入。")
                               .arg(QFileInfo(zipPath).fileName()));
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"),
                                   tr("已有下载或导入任务正在进行，等它结束之后再试。"));
        return;
    }
    // 另一条导入流程还没走完（`m_downloadInFlight` 管不到这一段：CurseForge 预览在
    // 真正开始下载**之前**）。拖放 / 再点一次按钮都会走到这里 —— 两趟同时问名字、
    // 同时写「导入」那一档占用的后果见头文件里 `m_importFlow` 那段。
    if (m_importFlow) {
        m_owner->appendLog(tr("已有导入流程正在进行，%1 没有导入。")
                               .arg(QFileInfo(zipPath).fileName()));
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"),
                                   tr("另一个整合包还在导入流程里，等它结束之后再试。"));
        return;
    }
    const FlowGuard flowGuard(m_importFlow);

    // 从这一刻到本函数返回，**游戏文件夹那一列灰着**。
    //
    // 为什么非要单独举这块牌子：下面 CurseForge 清单是在 `runOffUiThread()` 的嵌套事件
    // 循环里解析的，那几秒里用户输入照常送回来，而 `setDownloadUiBusy(true)` 还要再往后
    // （真正开始下载时才置）。这中间切一下游戏文件夹，后面所有 `AppPaths::versionDir()` 就
    // 落到另一份 .minecraft 里 —— 用户明明选了 A，实例却建在 B，屏幕上什么异常都没有。
    // 已经有忙态的那一段照样灰着，这里管的是"还没有忙态"的那一段。
    m_owner->beginFolderSwitchHold();
    // RAII：本函数所有 early return（含下面那些弹框后返回）都必须把牌子摘掉，
    // 否则这一列会一直灰到关窗口。
    const auto folderHold = qScopeGuard([this] { m_owner->endFolderSwitchHold(); });

    if (QFileInfo(zipPath).fileName() == QStringLiteral("wtfml.import-report.json")) {
        repairFromReport(zipPath);
        return;
    }

    PreparedModpack prepared;
    if (!prepareModpackPackage(zipPath, &prepared)) {
        return;
    }
    ModpackImport::Pack &pack = prepared.pack;
    const ModpackImport::Format format = prepared.layout.format;
    const QString &effectiveZip = prepared.effectiveZip;
    const QString &sourceZip = prepared.sourceZip;
    const QString &manifestFingerprint = prepared.manifestFingerprint;

    // 名字：先推一个候选（ModpackImport::suggestInstanceName 会把建不出目录的字符换掉、
    // 收长度），用户改不改都行。校验用**数据层那一份**
    // （InstanceFiles::validateRenameTarget），界面绝不自己抄一遍规则。
    //
    // 推的那个候选有两种来源：调用方给了 @p namePrefill 就用它
    // （"拖进来的那个整合包"给的是**文件名**，见 .hpp），没给才按包名推
    // （「导入整合包…」那颗按钮那一档）。**两路都过同一个清洗** —— 文件名里有
    // `/` `:` 那些字符的时候，不清洗的话下一步 `checkImportTargetName` 会报
    // "这个名字不能用"，而用户会觉得"我明明什么都没填"。
    QString name = namePrefill.isEmpty() ? ModpackImport::suggestInstanceName(pack.name)
                                         : ModpackImport::suggestInstanceName(namePrefill);
    while (true) {
        bool ok = false;
        const QString input = InlineMessage::getText(
            m_owner->m_ui->overlay(), tr("导入整合包"),
            tr("实例叫什么名字？这就是 versions 下面那个目录名，\n整合包「%1」的东西会装进它里面。").arg(pack.name),
            name, &ok);
        if (!ok) {
            return;
        }
        name = input.trimmed();
        // currentId 传空串 = "这是个新实例，盘上没有它"（见 checkImportTargetName）。
        QString problem;
        const InstanceFiles::ImportNameVerdict verdict =
            InstanceFiles::checkImportTargetName(name, &problem, manifestFingerprint);
        if (verdict == InstanceFiles::ImportNameVerdict::Ok) {
            break;
        }
        if (verdict == InstanceFiles::ImportNameVerdict::OkResume) {
            // 撞上的是**上次没下完的那一份**（下到一半网断了那种）。这是唯一一条不逼用户
            // 改名的路：改名重导等于把已经下好的几百 MB 再下一遍。仍然要问一句 ——
            // 这一步会重新解开包里的 config / 资源包，压在用户改过的文件上。
            const auto resumeAnswer = InlineMessage::question(m_owner->m_ui->overlay(), tr("接着上次的导入"),
                tr("versions 目录中已有未完成的导入「%1」。\n\n继续时会复用通过 SHA-1 校验的文件，并下载缺失文件。选择「否」将以其他名称重新导入。")
                    .arg(name), tr("是"), tr("否"), true);
            if (resumeAnswer) {
                break;
            }
            continue;  // 回上面重问名字
        }
        // 循环重问（和重命名那条路不同，那边是弹一次就要用户自己再点一次）：这里退回
        // 重来的代价大得多 —— 要从"选文件"重新开始，而选的是哪个包用户未必记得住。
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("换个名字"), problem);
    }

    if (!resolveCurseForgeManifestIfNeeded(&pack, format, QFileInfo(effectiveZip).fileName(), nullptr)) {
        return;
    }

    // 确认摘要。点下去就是几百 MB 和十几分钟，这是用户**最后**能反悔的地方，
    // 所以四件事都要说全：装的是哪个包、基于哪个版本、带什么加载器、装到哪去。
    VersionManager::LoaderKind loaderKind = VersionManager::LoaderKind::Fabric;
    const QString loaderText =
        pack.loaderKey.isEmpty()
            ? tr("（纯原版，没有模组加载器）")
            : (VersionManager::loaderKindForDependencyKey(pack.loaderKey, &loaderKind)
                   ? tr(" + %1 %2").arg(VersionManager::loaderDisplayName(loaderKind),
                                       pack.loaderVersion)
                   : tr(" + %1 %2").arg(pack.loaderKey, pack.loaderVersion));
    // 判据是 fileCount()（= 要下的 + 还没换成地址的）：CurseForge 那一档走到这里已经换过了
    // （上面那一发），两者相等；**万一**换完一条都不剩（文件全不可用），这里报出来
    // 的就是 0 —— 那正是用户该看到的数。**全是 0 的时候要说清那不等于什么都没装**：
    // 这类包的内容全在压缩包里，接下来要做的是解开它。
    const int downloadCount = pack.fileCount();
    const QString skippedText =
        pack.filesSkipped > 0
            ? tr("（另有 %1 个按包里的说明跳过）").arg(pack.filesSkipped)
            : QString();
    const QString downloadText =
        downloadCount == 0
            ? (pack.fileIssues.isEmpty()
                ? tr("要下载的文件：0 个（内容都在压缩包里，直接解进实例）%1").arg(skippedText)
                : tr("当前可自动下载：0 个；另有 %1 个文件需要补齐。").arg(pack.fileIssues.size()))
            : tr("要下载的文件：%1 个%2").arg(downloadCount).arg(skippedText);
    // `supportNote` 是 MultiMC / 懒人包那几种格式自带的说明（导入了什么、没导入什么，
    // lwjgl3ify 为什么会起不来）—— 必须**在点确认之前**让用户看见。
    const QString noteText =
        pack.supportNote.isEmpty() ? QString()
                                   : QStringLiteral("\n\n") + pack.supportNote;
    const auto answer = InlineMessage::question(m_owner->m_ui->overlay(), tr("开始导入"),
        tr("整合包：%1\nMinecraft：%2%3\n%4\n装到：%5%6\n\n开始导入吗？")
            .arg(pack.name, pack.gameVersion, loaderText, downloadText,
                 QDir::toNativeSeparators(AppPaths::versionDir(name)), noteText),
        tr("是"), tr("否"), true);
    if (!answer) {
        m_owner->appendLog(tr("已取消导入 %1。").arg(pack.name));
        return;
    }
    if (!promptCurseForgeFileIssuesIfNeeded(pack, name)) {
        return;
    }

    if (!prepared.tempArchive.isEmpty()) {
        m_tempArchives.insert(name, prepared.tempArchive);
    }
    const bool started = applyResolvedModpackImport(sourceZip, name, pack, manifestFingerprint);
    if (started && !prepared.tempArchive.isEmpty()) {
        // 壳里那一份解出来的临时包要留到导入结束（数据层还在读它）。
        // 谁删：构造函数里接的 versionDownloadFinished（见 .hpp）。
        prepared.releaseTempArchive();
    } else if (!started) {
        m_tempArchives.remove(name);
    }
}

void ImportController::openImportReport(const QString &instanceId) {
    if (m_importFlow || m_owner->m_downloadInFlight) {
        InlineMessage::information(m_owner->m_ui->overlay(), tr("现在正忙"), tr("请等当前导入或下载结束后再补齐文件。"));
        return;
    }
    if (instanceId.isEmpty() || !InstanceFiles::validatePathSegment(instanceId).isEmpty()) return;
    const FlowGuard flowGuard(m_importFlow);
    repairFromReport(AppPaths::versionDir(instanceId) + QStringLiteral("/wtfml.import-report.json"));
}

void ImportController::repairFromReport(const QString &reportPath) {
    const QString root = QFileInfo(reportPath).absolutePath();
    const QString id = QFileInfo(root).fileName();
    auto *overlay = m_owner->m_ui->overlay();
    // 不把报告中声明的路径当作写入授权：只允许当前数据目录下的直接实例目录。
    if (InstanceFiles::canonicalPath(root) != InstanceFiles::canonicalPath(AppPaths::versionDir(id))) {
        InlineMessage::warning(overlay, tr("不能补齐"), tr("请先切换至报告对应的数据目录。"));
        return;
    }
    ModpackInstallReport::Report report;
    QString problem;
    if (!ModpackInstallReport::read(root, &report, &problem)) {
        InlineMessage::warning(overlay, tr("读不了补齐报告"), problem);
        return;
    }
    QString lastResult;
    QSet<int> checked;
    int selected = 0;
    for (;;) {
        QStringList names;
        for (int index = 0; index < report.issues.size(); ++index) {
            const auto &issue = report.issues.at(index);
            names.append((checked.contains(index) ? TextMarks::done(tr("已校验 / ")) : QString()) +
                (issue.fileName.isEmpty() ? tr("文件 %1").arg(issue.ref.fileId) : issue.fileName));
        }
        InlineMessageBox box(MessageKind::Information, tr("补齐：%1").arg(report.packName),
            tr("选择缺项，再选择已下载的本地文件。校验通过后放入对应位置，不覆盖已有文件。\n补齐后点击“完成安装”：重新核对核心、加载器及覆盖文件。旧安装没有安装凭据时仍需原包。"),
            {tr("选择文件"), tr("批量匹配"), tr("文件网页"), tr("重新校验"), tr("完成安装"), tr("关闭")}, 5, -1, m_owner);
        auto *list = box.setList(names, selected);
        auto show = [&](int row) {
            QString note = lastResult;
            if (row >= 0 && row < report.issues.size())
                note += QLatin1Char('\n') + ModpackInstallReport::description(report.issues.at(row));
            box.setNote(note);
            if (auto *label = box.findChild<QLabel *>(InlineMessageBox::noteLabelName()))
                label->setTextFormat(Qt::PlainText);
        };
        connect(list, &QListWidget::currentRowChanged, &box, show);
        show(list->currentRow());
        overlay->execPrompt(&box);
        selected = box.listRow();
        if (box.clickedIndex() < 0 || box.clickedIndex() == 5) return;
        const bool batch = box.clickedIndex() == 1;
        const bool verify = box.clickedIndex() == 3;
        const bool finish = box.clickedIndex() == 4;
        if (!batch && !verify && !finish && (selected < 0 || selected >= report.issues.size())) continue;
        const auto issue = selected >= 0 && selected < report.issues.size()
            ? report.issues.at(selected) : ModpackImport::FileIssue{};
        if (box.clickedIndex() == 2) {
            const auto url = ModpackImport::curseForgeFileWebUrl(issue);
            if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
            else lastResult = tr("报告缺少可用的项目链接。请按项目编号和文件编号查找，或重新导入整合包以更新记录。");
            continue;
        }
        const QStringList sources = verify || finish ? QStringList{} : batch
            ? QFileDialog::getOpenFileNames(m_owner, tr("选择文件，按大小和 SHA-1 自动匹配缺项"), {}, tr("所有文件 (*)"))
            : QStringList{QFileDialog::getOpenFileName(m_owner, tr("选择补齐文件"), {}, tr("所有文件 (*)"))};
        if (!verify && !finish && (sources.isEmpty() || sources.first().isEmpty())) continue;
        if (m_owner->m_downloadInFlight || m_owner->m_launcherManager->isVersionRunning(id) ||
            !WriteDirectoryGuard::acquire(root, &problem)) {
            lastResult = tr("实例运行中、安装任务繁忙或无法取得写锁，未写入。\n%1").arg(problem);
            continue;
        }
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        InlineMessageBox progress(MessageKind::Information, tr("正在校验并补齐"),
            tr("%1\n核对大小与 SHA-1；最多运行 900 秒，可随时取消。")
                .arg(finish ? tr("核对安装凭据并完成安装") : verify ? tr("只读核对报告中的全部 %1 项").arg(report.issues.size())
                           : batch ? tr("批量匹配 %1 个文件").arg(sources.size()) : issue.fileName),
            {tr("取消")}, -1, -1, m_owner);
        QFutureWatcher<ModpackInstallReport::BatchResult> watcher;
        connect(&watcher, &QFutureWatcher<ModpackInstallReport::BatchResult>::finished, &progress, &QDialog::accept);
        const auto issues = batch || verify ? report.issues : QList<ModpackImport::FileIssue>{issue};
        watcher.setFuture(QtConcurrent::run([root, issues, sources, cancelled, batch, verify, finish] {
            QElapsedTimer timer; timer.start();
            const auto stop = [cancelled, &timer] { return cancelled->load() || timer.elapsed() >= 900000; };
            if (finish) {
                ModpackInstallReport::BatchResult result;
                QString error;
                if (ModpackCompletion::finish(root, &error, stop)) result.suppliedPaths.append(QStringLiteral("installation-complete"));
                else result.problems.append(error);
                result.cancelled = stop();
                return result;
            }
            if (verify) return ModpackInstallReport::verifyExisting(root, issues, stop);
            if (batch) return ModpackInstallReport::supplyMatching(root, issues, sources, stop);
            ModpackInstallReport::BatchResult result;
            QString error;
            if (ModpackInstallReport::supply(root, issues.first(), sources.first(), &error, stop))
                result.suppliedPaths.append(issues.first().path);
            else result.problems.append(error);
            result.cancelled = stop();
            return result;
        }));
        overlay->execPrompt(&progress);
        if (!watcher.isFinished()) {
            cancelled->store(true);
            // 不销毁仍在访问的数据；继续重绘但暂不放行其它输入，等一个文件块返回。
            QEventLoop wait;
            connect(&watcher, &QFutureWatcher<ModpackInstallReport::BatchResult>::finished, &wait, &QEventLoop::quit);
            if (!watcher.isFinished()) wait.exec(QEventLoop::ExcludeUserInputEvents);
        }
        const auto result = watcher.result();
        if (finish && result.suppliedPaths.contains(QStringLiteral("installation-complete"))) {
            m_owner->refreshInstalledVersions();
            m_owner->appendLog(tr("%1 已完成安装，无需重新选择原包。").arg(id));
            InlineMessage::information(overlay, tr("安装完成"), tr("所有阶段文件与补齐项已通过校验，现在可以启动 %1。").arg(id));
            return;
        }
        if (verify) checked.clear(); // 外部删除/修改后，不沿用旧的已校验状态。
        for (int index = 0; index < report.issues.size(); ++index)
            if (result.suppliedPaths.contains(report.issues.at(index).path)) checked.insert(index);
        lastResult = tr("本次校验通过 %1 项，未处理/失败 %2 项。%3\n补齐后可点击“完成安装”；没有安装凭据的旧安装仍需原包。")
            .arg(result.suppliedPaths.size()).arg(result.problems.size())
            .arg(result.cancelled ? tr("已取消或超时，未改变安装状态。") : QString());
        if (!result.problems.isEmpty()) lastResult += QLatin1Char('\n') + result.problems.mid(0, 5).join(QLatin1Char('\n'));
        m_owner->appendLog(lastResult);
        for (const auto &error : result.problems) m_owner->appendLog(error);
    }
}

bool ImportController::applyModpackImport(const QString &zipPath, const QString &instanceName,
                                    const QByteArray &manifestJson,
                                    ModpackImport::Format format) {
    // 把界面上确认过的**同一串字节**再解析一遍（纯函数，一瞬）。好处是"确认框里说的
    // 那个包"和"真的去装的那个包"不可能不是同一个 —— 不必去维护一份"解析结果"的传递。
    ModpackImport::Result result =
        format == ModpackImport::Format::CurseForge
            ? ModpackImport::parseCurseForgeManifest(manifestJson)
            : ModpackImport::parseModrinthIndex(manifestJson);
    if (!result.ok) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("导入整合包"), result.problem);
        return false;
    }
    // 和上面问名字那一步同一份指纹（同一个函数、同一串字节）：这一半要拿它再查一遍重名，
    // 还要交给数据层写进标记。这里重算而不是从外面传，是为了守住"确认框说的包"和"真的
    // 去装的那个包"是同一个 —— 外面传进来的是解析结果，指纹只认字节。
    const QString manifestFingerprint = InstanceFiles::importManifestFingerprint(manifestJson);
    return applyResolvedModpackImport(zipPath, instanceName, result.pack, manifestFingerprint);
}

bool ImportController::applyResolvedModpackImport(const QString &zipPath,
                                                  const QString &instanceName,
                                                  ModpackImport::Pack pack,
                                                  const QString &manifestFingerprint,
                                                  const QString &preselectedInstallerJava) {
    const QString loaderProblem =
        ModpackImport::unsupportedLoaderText(pack.loaderKey, pack.loaderVersion);
    if (!loaderProblem.isEmpty()) {
        InlineMessage::information(m_owner->m_ui->overlay(), tr("无法安装这个整合包"), loaderProblem);
        return false;
    }

    // CurseForge 那一档在这里要**再换一次**地址（上面确认框那一步换过一次）。代价是
    // 一发重复的请求，换来的是和上面那句"同一串字节再解析一遍"一模一样的东西：这一半
    // 自己手里的那份 `pack` 是完整的，不靠外面传进来的任何解析结果。数据层那边也有
    // 第二道闸（importModpack 里那句"没换过就报错"），所以这里漏了也不会装出一个空实例。
    // NOTE: `pendingRefs` 空了（换过了，或者清单本来就是空的）时它一句话不说就返回 ——
    // 零下载的包走的就是这一档。
    if (pack.format == ModpackImport::Format::CurseForge) {
        QString problem;
        VersionManager *versions = m_owner->m_versionManager;
        const VersionManager::CurseForgeResolveSettings settings =
            versions->curseForgeResolveSettings();
        m_owner->enterSyncWait(tr("正在获取 CurseForge 文件清单…"));
        runOffUiThread([&] { problem = versions->resolveCurseForgeFiles(&pack, settings); });
        m_owner->leaveSyncWait();
        if (!problem.isEmpty()) {
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("拿不到文件清单"), problem);
            return false;
        }
    }

    // 重名**在这里再查一遍**：上面那条路（问名字那个循环）查过一次，但两次之间隔着用户
    // 的输入，而且验证工程直接就能进这一半 —— 而数据层那边是有约定的（见
    // VersionManager::importModpack 的说明：调用方必须先查过重名，它自己不查）。
    // 判据是同一份 checkImportTargetName()，不会漂 —— 包括"接着上次没下完的那一份"
    // 这条口子：上面那个循环放行了、这一半却按重名拒掉的话，用户看到的是一句
    // 「已经有一个叫…的实例了」，而他刚才明明点了「接着下」。
    QString nameProblem;
    const InstanceFiles::ImportNameVerdict nameVerdict =
        InstanceFiles::checkImportTargetName(instanceName, &nameProblem, manifestFingerprint);
    if (nameVerdict == InstanceFiles::ImportNameVerdict::Taken) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("换个名字"), nameProblem);
        return false;
    }
    const bool resumingImport =
        nameVerdict == InstanceFiles::ImportNameVerdict::OkResume;

    // 安装器（Forge/NeoForge）自己就是个 Java 程序（`java -jar <安装器>.jar ...`），所以
    // 要的运行环境和"这个版本要的 Java"是同一件事：版本 json 里的 javaVersion。这一段和
    // VersionManager::installLoaderVersion() 里那段是同一件事、同一个判据 —— 拿不到版本 json 就不猜主版本
    // （猜错比不猜更危险）。Fabric 和纯原版那条路一个进程都不起，用不上它。
    QString installerJava = preselectedInstallerJava;
    if (installerJava.isEmpty() && !resolveInstallerJavaForPack(pack, false, &installerJava, nullptr, nullptr)) {
        return false;  // 还没进忙态，直接退出就是干净的状态
    }

    // 忙要在**点击这一刻**置上：导入的头两步（拉版本表 / 拉 profile）是异步的，中间那段
    // 时间按钮还能点的话用户会重复触发（和下载、装加载器两条路同一个理由）。
    m_owner->m_installOccupancy.setBatch(InstallOccupancy::Batch::Import, instanceName);
    m_owner->setDownloadUiBusy(true);
    m_owner->m_downloadTaskLedger->resetProgressThrottle();
    m_owner->appendLog(tr("开始导入整合包 %1 ...").arg(QFileInfo(zipPath).fileName()));
    if (resumingImport) {
        // 用户点的是「接着下」，日志里得留下这句 —— 不然排查的人只会看到"导入了一个已经
        // 存在的实例"，而那看起来像是覆盖了别人的东西。
        m_owner->appendLog(tr("接着「%1」上次没下完的那一份往下下（已经下好的按 sha1 跳过）。")
                      .arg(instanceName));
    }
    if (!pack.patchedRuntime.isEmpty()) {
        m_specialPackSources.insert(instanceName, zipPath);
    }
    m_owner->m_versionManager->importModpack(zipPath, instanceName, pack, installerJava,
                                             manifestFingerprint);
    return true;
}

bool ImportController::installLocalResourceFile(const QString &versionId,
                                          ResourceCatalog::Kind kind, const QString &path,
                                          QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    // 目录名的唯一出处是 ResourceCatalog 那张表（mods / datapacks / resourcepacks /
    // shaderpacks）。整合包那一路回空串 —— 它不走这一条（要整包解开），调用方已经在
    // 更早一步把它分出去了。
    const QString folder = ResourceCatalog::folderForKind(kind);
    if (folder.isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = tr("这一类不能直接放进实例目录。");
        }
        return false;
    }
    const QString root = AppPaths::versionDir(versionId);
    return copyLocalResourceFile(versionId, root, QDir(root).filePath(folder), path, problemOut);
}

bool ImportController::copyLocalResourceFile(const QString &versionId, const QString &lockRoot,
                                             const QString &dir, const QString &path,
                                             QString *problemOut) {
    if (problemOut != nullptr) problemOut->clear();
    const QString target = QDir(dir).filePath(QFileInfo(path).fileName());
    // 同名文件：**问一句**（不静默盖）。2.1.2 起这一问统一成三档：
    // 「取消」/「跳过已有项」（两者都不动目标，前者顺带停掉这一批）/「盖掉」。
    OperationReport::ConflictPolicy policy = OperationReport::ConflictPolicy::Overwrite;
    bool skippedExisting = false;
    if (QFileInfo::exists(target)) {
        // 先看一眼"拖进来的这一份**就是**这一格自己"：从实例自己的
        // `mods/` 里把文件再拖回启动器是最容易中的一路，而往下走的话，答完「盖掉」就会
        // 把 target —— 也就是**源文件本身** —— 删掉，源没了，文件就这么从盘上消失。
        // 比的是盘上那个**真路径**（`canonicalFilePath()` 解开大小写 / 8.3 短名那些别名）。
        // 它不解 junction，但那种情形也不丢东西：下面写的是临时文件、最后一步才改名字换
        // 上去，读与写不会互相踩。
        const QString dropped = QFileInfo(path).canonicalFilePath();
        if (!dropped.isEmpty() && dropped == QFileInfo(target).canonicalFilePath()) {
            return true;  // 它本来就在那儿了，一个字都不用动
        }
        const int choice = InlineMessage::ask(
            m_owner->m_ui->overlay(), MessageKind::Question,
            tr("已经有一个同名的"),
            tr("%1 中已存在「%2」。\n\n「跳过已有项」会保留原文件。「取消」将停止处理剩余文件。")
                                .arg(QDir::toNativeSeparators(dir),
                     QFileInfo(path).fileName()),
            {tr("取消"), tr("跳过已有项"), tr("盖掉")},
            /*defaultIndex=*/0, /*destructiveIndex=*/2, false);
        if (choice == 0) {
            m_batchCancelled = true;
            if (problemOut != nullptr) problemOut->clear();
            return false;
        }
        if (choice != 2) {
            // 「跳过已有项」= 用户明确不要这一份；**不是错误**（调用方按“用户没要”处理，
            // 不弹失败框、也不打断同一批里剩下的文件）。
            skippedExisting = true;
            policy = OperationReport::ConflictPolicy::SkipExisting;
            if (problemOut != nullptr) problemOut->clear();
            recordFileCopyReport(versionId, path, target, policy, true, false, QString());
            return false;
        }
    }

    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(lockRoot, &lockProblem)) {
        if (problemOut != nullptr) *problemOut = lockProblem;
        recordFileCopyReport(versionId, path, target, policy, false, false, lockProblem);
        return false;
    }
    if (!QDir().mkpath(dir)) {
        if (problemOut != nullptr) {
            *problemOut = tr("无法创建 %1。").arg(QDir::toNativeSeparators(dir));
        }
        recordFileCopyReport(versionId, path, target, policy, false, false,
                             tr("无法创建导入目录。"));
        return false;
    }
    // 写盘这一步走 `QSaveFile`：它先写到旁边那个临时文件、`commit()`
    // 那一刻才**原子地**换上去。"先 `QFile::remove(target)`，再
    // `QFile::copy(path, target)`" 不行 —— 用户明明点的是「盖掉」，可删成功、拷失败（盘满 /
    // 权限 / 杀软锁着那一下）的时候，盘上**一份都没有了**：旧的已经删掉、新的没到位，
    // 而且没有退路（比不点「盖掉」还糟）。
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        if (problemOut != nullptr) {
            *problemOut = tr("写入 %1 失败：%2")
                              .arg(QDir::toNativeSeparators(target), out.errorString());
        }
        recordFileCopyReport(versionId, path, target, policy, false, false, out.errorString());
        return false;
    }
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (problemOut != nullptr) {
            *problemOut = tr("无法读取拖入的 %1。")
                              .arg(QDir::toNativeSeparators(path));
        }
        recordFileCopyReport(versionId, path, target, policy, false, false,
                             tr("读不了源文件。"));
        return false;
    }
    while (!in.atEnd()) {
        // 一块一块搬（1 MiB）：模组 jar 有几十上百兆，整份读进内存没有必要。
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty()) {
            if (problemOut != nullptr) {
                *problemOut = tr("无法读取拖入的 %1（%2）。")
                                  .arg(QDir::toNativeSeparators(path), in.errorString());
            }
            recordFileCopyReport(versionId, path, target, policy, false, false, in.errorString());
            return false;  // 析构会把临时文件收掉，盘上原来那一份还在
        }
        if (out.write(chunk) != chunk.size()) {
            if (problemOut != nullptr) {
                *problemOut = tr("复制不过去：%1 → %2（%3）")
                                  .arg(QDir::toNativeSeparators(path),
                                       QDir::toNativeSeparators(target), out.errorString());
            }
            recordFileCopyReport(versionId, path, target, policy, false, false, out.errorString());
            return false;
        }
    }
    if (!out.commit()) {
        if (problemOut != nullptr) {
            *problemOut = tr("复制不过去：%1 → %2（%3）")
                              .arg(QDir::toNativeSeparators(path),
                                   QDir::toNativeSeparators(target), out.errorString());
        }
        recordFileCopyReport(versionId, path, target, policy, false, false, out.errorString());
        return false;
    }
    recordFileCopyReport(versionId, path, target, policy, false, true, QString());
    Q_UNUSED(skippedExisting);
    return true;
}

void ImportController::recordFileCopyReport(const QString &versionId, const QString &sourcePath,
                                            const QString &targetPath,
                                            OperationReport::ConflictPolicy policy,
                                            bool skipped, bool done, const QString &problem) {
    OperationReport::Report report;
    report.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    report.kind = OperationReport::Kind::FileCopy;
    const QString fileName = QFileInfo(targetPath).fileName();
    report.title = tr("复制文件：%1").arg(fileName);
    report.instanceId = versionId;
    report.sourceRoot = sourcePath;
    report.targetRoot = QFileInfo(targetPath).absolutePath();
    report.conflictPolicy = policy;
    report.totalFiles = 1;
    report.doneFiles = done ? 1 : 0;
    report.skippedFiles = skipped ? 1 : 0;
    report.failedFiles = done || skipped ? 0 : 1;
    report.complete = done || skipped;
    report.failureReason = problem;
    report.startedAt = QDateTime::currentDateTime();
    report.updatedAt = report.startedAt;
    report.context = QJsonObject{
        {QStringLiteral("dataRoot"), AppPaths::minecraftDir()},
        {QStringLiteral("sourcePath"), sourcePath},
        {QStringLiteral("targetPath"), targetPath}};
    report.entries.append(OperationReport::Entry{
        fileName, QFileInfo(sourcePath).size(),
        done ? OperationReport::EntryState::Done
             : (skipped ? OperationReport::EntryState::Skipped
                        : OperationReport::EntryState::Failed),
        problem});
    QString saveProblem;
    const QString path = OperationReport::reportPath(AppPaths::minecraftDir(), report.id);
    if (!OperationReport::saveReport(path, report, &saveProblem)) {
        m_owner->appendLog(tr("文件复制报告没能保存：%1").arg(saveProblem));
        return;
    }
    if (!report.complete) {
        // 单文件复制没有任务中心那一行，所以把“为什么做不完 + 怎么办”直接写成一份文本，
        // 由调用方把路径带进提示里（和任务中心那颗「继续未完成操作」读的是同一套说明）。
        m_lastFileCopyFailureReport = OperationReport::writeFailureText(path, report);
        if (!m_lastFileCopyFailureReport.isEmpty()) {
            m_owner->appendLog(tr("文件复制未完成，原因报告：%1")
                                   .arg(m_lastFileCopyFailureReport));
        }
    }
}

void ImportController::handleInstanceSettingsDrop(QWidget *page, const QString &versionId,
                                            const QStringList &paths) {
    // 先分类（一次拖放最多几个文件；每个压缩包最多读一遍条目表）。
    QList<DroppedResource::Result> results;
    results.reserve(paths.size());
    for (const QString &path : paths) {
        results.append(DroppedResource::classify(path));
    }

    // ---------- ① 整合包：**一次只处理一个** ----------
    //
    // 用户点名的那一步："让用户选择安装的文件夹及实例名…并离开实例设置（相当于点了返回）"。
    // 多个整合包一起拖进来时只认第一个 —— 装一个整合包要选文件夹、问名字、下几百 MB，
    // 一次排三个队只会让"现在轮到谁"变得说不清。剩下的在下面那一句日志里说清楚。
    for (const DroppedResource::Result &result : results) {
        if (!result.known || result.kind != ResourceCatalog::Kind::Modpack) {
            continue;
        }
        // 关页**可能被挡住**（配置编辑器 / 世界设置里压着改了一半的东西，见
        // `leaveBlockedByUnsavedEdits()`）。挡住时**不当作关掉了**：底下那一格要是被
        // 静默丢掉，用户丢的是自己手改的配置。
        auto *dialog = qobject_cast<InstanceSettingsDialog *>(page);
        if (dialog != nullptr && dialog->leaveBlockedByUnsavedEdits()) {
            InlineMessage::information(
                m_owner->m_ui->overlay(), tr("有未保存的修改"),
                tr("当前设置有未保存的修改。保存或丢弃修改后，再拖入文件。"));
            m_owner->appendLog(tr("没有导入 %1：实例设置里还有没保存的改动。")
                          .arg(QFileInfo(result.path).fileName()));
            return;
        }
        const QString prefill = QFileInfo(result.path).completeBaseName();
        // 「离开实例设置（相当于点了返回）」用的**就是**返回箭头那一发：`m_owner->runWhenOverlayFree()`
        // 让位那一步走的是 `OverlayHost::evict()`，而页那一档的 evict 发的正是
        // `backPressed` —— `showPage()` 把它接在 `dialog->reject()` 上（见
        // OverlayHost.cpp 那段），一字不差就是"点了一下返回箭头"。
        //
        // 这里**不要**再自己补一发 `dialog->reject()`：evict() 那一发当场就走到 reject
        // 了，补了只是让它再跑一遍（两次 done() 不报错，但"关页这件事有两个发起人"是
        // 白添的麻烦）。
        //
        // 也不能"让位那一步"省掉（直接开问）：这一层还占着，`execPrompt()` 会被
        // `OverlayHost` 挡掉（一层只放得下一件，见它的头文件），用户看到的是"拖了半天
        // 什么也没发生"。`m_owner->runWhenOverlayFree()` 排的下一轮才真的空出来。
        m_owner->runWhenOverlayFree([this, path = result.path, prefill] {
            m_owner->importDroppedModpack(path, prefill);
        });
        if (results.size() > 1) {
            m_owner->appendLog(tr("一次拖进来 %1 个文件，先处理整合包「%2」，其余的先放着。")
                          .arg(results.size())
                          .arg(QFileInfo(result.path).fileName()));
        }
        return;
    }

    // ---------- ② 其余几类：按类归堆，一类问一次 ----------
    //
    // 归堆是用户的体验问题：拖三个模组进来问三遍"是否继续"是骚扰，而"这一类装不装"
    // 本来就是一个问题。
    QList<QPair<ResourceCatalog::Kind, QStringList>> byKind;
    QStringList unrecognized;
    for (const DroppedResource::Result &result : results) {
        if (!result.known || ResourceCatalog::folderForKind(result.kind).isEmpty()) {
            unrecognized.append(tr("%1（%2）")
                                    .arg(QFileInfo(result.path).fileName(),
                                         result.detail.isEmpty() ? tr("无法识别文件类型") : result.detail));
            continue;
        }
        int slot = -1;
        for (int i = 0; i < byKind.size(); ++i) {
            if (byKind.at(i).first == result.kind) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            byKind.append(qMakePair(result.kind, QStringList{}));
            slot = byKind.size() - 1;
        }
        byKind[slot].second.append(result.path);
    }

    auto *dialog = qobject_cast<InstanceSettingsDialog *>(page);
    const QString currentPage = dialog != nullptr ? dialog->currentPageKey() : QString();
    bool installedAnything = false;
    for (const QPair<ResourceCatalog::Kind, QStringList> &entry : byKind) {
        const ResourceCatalog::Kind kind = entry.first;
        // 用户点名的那一条：**正好在对应那一页就直接塞进去，不问**（"如果是模组且正好
        // 在模组设置那一页，那就直接塞进去"）。不在对应页才问那一句"是否继续"。
        if (pageKeyForKind(kind) != currentPage) {
            // 那一句话就是用户点名要的：「将在此实例中安装 资源包/...（判断出的文件类型），
            // 是否继续」。多个文件时把个数说出来 —— 一次拖三个模组，那句"安装模组"要是
            // 不报数，用户没法知道里面是三个还是一个。
            const QString what = entry.second.size() == 1
                                     ? ResourceCatalog::kindLabel(kind)
                                     : tr("%1 个%2")
                                           .arg(entry.second.size())
                                           .arg(ResourceCatalog::kindLabel(kind));
            const bool proceed = InlineMessage::question(m_owner->m_ui->overlay(), tr("装进这个实例"),
                                                         tr("将在此实例中安装 %1，是否继续？")
                                                             .arg(what),
                                                         tr("继续"), tr("不装"), true);
            if (!proceed) {
                continue;
            }
        }
        int done = 0;
        QStringList problems;
        m_batchCancelled = false;
        m_lastFileCopyFailureReport.clear();
        for (const QString &path : entry.second) {
            if (m_batchCancelled) break;
            QString problem;
            if (m_owner->installLocalResourceFile(versionId, kind, path, &problem)) {
                ++done;
            } else if (!problem.isEmpty()) {
                problems.append(problem);
                // NOTE: 返回 false 而 problem 为空 = 用户自己答了"取消 / 跳过已有项" ——
                // 那不是错，不进这一张表。
            }
        }
        if (!m_lastFileCopyFailureReport.isEmpty()) {
            problems.append(tr("未能完成的文件操作原因报告：%1")
                                .arg(QDir::toNativeSeparators(m_lastFileCopyFailureReport)));
        }
        if (done > 0) {
            installedAnything = true;
            m_owner->appendLog(tr("已把 %1 个%2装进「%3」。")
                          .arg(done)
                          .arg(ResourceCatalog::kindLabel(kind),
                               m_owner->instanceDisplayName(versionId)));
        }
        if (!problems.isEmpty()) {
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("有几个没装进去"),
                                   problems.join(QLatin1String("\n\n")));
        }
    }

    if (!unrecognized.isEmpty()) {
        InlineMessage::warning(
            m_owner->m_ui->overlay(), tr("无法识别拖入的文件"),
            tr("下面这几个不是能装进实例的东西：\n\n%1")
                .arg(unrecognized.join(QLatin1String("\n"))));
    }

    // 装完了要让那一页自己重读一遍目录 —— 不刷的话用户刚拖进去的模组不出现在
    // 「模组」那一页里，看着就像"没装进去"。
    if (installedAnything && dialog != nullptr) {
        dialog->refreshFilePages();
    }
}

void ImportController::importDroppedModpack(const QString &zipPath, const QString &namePrefill) {
    // 用户点名要的第一步：「让用户选择安装的文件夹」。手法和「复制实例」那一处**逐字
    // 相同**（原生目录选择框 —— `ui/` 不碰 AppPaths，"该默认到哪儿"只有这一层知道；
    // 那一处那段 NOTE 就是这条的出处）。
    const QString folder = QFileDialog::getExistingDirectory(
        m_owner, tr("选择目标游戏目录"), AppPaths::minecraftDir(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (folder.isEmpty()) {
        m_owner->appendLog(tr("没有导入 %1：用户取消了。").arg(QFileInfo(zipPath).fileName()));
        return;  // 用户取消
    }

    // 挑的那一份**必须真的切过去**：整条导入路（`checkImportTargetName`、
    // `applyModpackImport`、`AppPaths::versionDir()`）看的是**当前**这一份 .minecraft，
    // 不切的话整合包会装到用户没挑的那一份里 —— 而屏幕上什么异常都没有。
    // （"不切也行"是不可能的：那要在导入路上把"装到哪一份"从头传到尾，见
    // `AppPaths::versionDir()` 那段。）
    if (folder != m_owner->m_folderManager->currentFolder()) {
        // 不在列表里就先加进去（`setCurrentFolder()` 只认 `folders()` 里的那几份）。
        // 加不进去 = 那个路径不是个目录（也不该发生：刚从这个框里选出来的）。
        if (!m_owner->m_folderManager->addFolder(folder) || !m_owner->m_folderManager->setCurrentFolder(folder)) {
            InlineMessage::warning(m_owner->m_ui->overlay(), tr("换不到那个文件夹"),
                                   tr("设置游戏目录「%1」失败，整合包导入取消。")
                                       .arg(QDir::toNativeSeparators(folder)));
            return;
        }
    }

    // 名字（预填文件名）+ 装载 + 下载，全是既有那一条路（见 importModpackFromFile）。
    m_owner->importModpackFromFile(zipPath, namePrefill);
}

void ImportController::handleWindowDrop(const QStringList &paths) {
    if (m_importFlow) return;
    QList<DroppedResource::Result> results;
    results.reserve(paths.size());
    for (const QString &path : paths) {
        results.append(DroppedResource::classify(path));
    }

    // 整合包：和实例设置那一档走同一条（选文件夹 + 问名字）。这里不用"离开某一页"，
    // 本来就没有页开着。
    for (const DroppedResource::Result &result : results) {
        if (result.known && result.kind == ResourceCatalog::Kind::Modpack) {
            m_owner->importDroppedModpack(result.path, QFileInfo(result.path).completeBaseName());
            return;
        }
    }
    const FlowGuard flow(m_importFlow);

    // 其余几类：**装到哪个实例**要问（这一档没有上下文可推，见文件头那段 ★）。
    // NOTE: 分类结果**复用上面那一趟**（不在这里再 classify 一遍）：读一个压缩包的条目表
    // 要起一次 PowerShell，重跑一遍就是纯浪费，而且两次的答案理论上可以不一样
    // （文件在这中间被换了）—— 一份输入只认一次。
    QStringList installable;
    QStringList unrecognized;
    for (const DroppedResource::Result &result : results) {
        if (!result.known || ResourceCatalog::folderForKind(result.kind).isEmpty()) {
            unrecognized.append(tr("%1（%2）")
                                    .arg(QFileInfo(result.path).fileName(),
                                         result.detail.isEmpty() ? tr("无法识别文件类型") : result.detail));
            continue;
        }
        installable.append(result.path);
    }
    if (!unrecognized.isEmpty()) {
        InlineMessage::warning(
            m_owner->m_ui->overlay(), tr("无法识别拖入的文件"),
            tr("下面这几个不是能装进实例的东西：\n\n%1")
                .arg(unrecognized.join(QLatin1String("\n"))));
    }
    if (installable.isEmpty()) {
        return;
    }

    m_owner->beginFolderSwitchHold();
    const auto folderHold = qScopeGuard([this] { m_owner->endFolderSwitchHold(); });
    const auto batch = QScopedValueRollback(m_batchCancelled, false);
    const auto *combo = m_owner->m_ui->getInstalledVersionCombo();
    const QString defaultId = combo == nullptr ? QString() : combo->currentData().toString();
    QString versionId;
    QString customDirectory;
    bool otherFolder = !m_owner->m_settings->downloadAutoIntoInstance();
    if (!otherFolder) {
        auto rows = m_owner->currentFolderInstancePickerRows(defaultId);
        QStringList mods;
        for (const auto &result : results) {
            if (result.known && result.kind == ResourceCatalog::Kind::Mod) mods.append(result.path);
        }
        if (!mods.isEmpty() && !rows.isEmpty()) {
            QList<UpgradeAssessment::PickerTarget> targets;
            targets.reserve(rows.size());
            for (const auto &row : rows) {
                targets.append({row.id, m_owner->m_versionManager->instanceGameVersion(row.id), row.loaderKey});
            }
            QHash<QString, UpgradeAssessment::PickerMismatch> mismatches;
            runOffUiThread([&] { mismatches = UpgradeAssessment::checkDroppedMods(mods, targets); });
            for (auto &row : rows) {
                const auto mismatch = mismatches.value(row.id);
                if (mismatch.gameVersion) row.subtitle += QStringLiteral(" · ") + MainWindow::tr("版本不符");
                if (mismatch.loader) row.subtitle += QStringLiteral(" · ") + MainWindow::tr("加载器不符");
            }
        }
        versionId = m_owner->m_ui->chooseResourceDownloadInstance(rows, &otherFolder, /*importing=*/true);
    }
    if (otherFolder) {
        customDirectory = QFileDialog::getExistingDirectory(
            m_owner, tr("选择安装文件夹"),
            defaultId.isEmpty() ? AppPaths::minecraftDir() : AppPaths::versionDir(defaultId));
        if (customDirectory.isEmpty()) return;
    } else if (versionId.isEmpty()) {
        return;
    } else if (!QDir(AppPaths::versionDir(versionId)).exists()) {
        InlineMessage::warning(m_owner->m_ui->overlay(), MainWindow::tr("目标实例已不可用"),
                               MainWindow::tr("游戏目录已切换或该实例已移除，请重新选择下载目标。"));
        return;
    }

    const QString destination = versionId.isEmpty() ? QDir::toNativeSeparators(customDirectory)
                                                    : m_owner->instanceDisplayName(versionId);
    int total = 0;
    QStringList problems;
    for (const auto &result : results) {
        if (!result.known || ResourceCatalog::folderForKind(result.kind).isEmpty()) continue;
        QString problem;
        const bool copied = versionId.isEmpty()
            ? copyLocalResourceFile({}, customDirectory, customDirectory, result.path, &problem)
            : installLocalResourceFile(versionId, result.kind, result.path, &problem);
        if (copied) ++total;
        else if (!problem.isEmpty()) problems.append(problem);
        if (m_batchCancelled) break;
    }
    if (!problems.isEmpty()) {
        InlineMessage::warning(m_owner->m_ui->overlay(), tr("有几个没装进去"),
                               problems.join(QLatin1String("\n\n")));
    }
    if (total > 0) {
        m_owner->appendLog(tr("已把 %1 个文件装进「%2」。").arg(total).arg(destination));
        InlineMessage::information(m_owner->m_ui->overlay(), tr("安装完成"),
                                   tr("已安装到「%1」。").arg(destination));
    }
}

void ImportController::preservePackSource(const QString &instanceId, const QString &zipPath) {
    const QString target = AppPaths::versionDir(instanceId) + QStringLiteral("/.wtfml/pack-source");
    if (!QDir().mkpath(target)) return;
    const QString prefix = QFileInfo(zipPath).completeBaseName().isEmpty() ? QString() : QString();
    Q_UNUSED(prefix);
    QStringList entries;
    QString error;
    if (ZipUtil::listEntries(zipPath, &entries, &error) != ZipUtil::EntryStatus::Ok) return;
    // 找 MultiMC 包的那一层根目录（`<名字>/mmc-pack.json` 或根上的 mmc-pack.json）。
    QString root;
    for (const QString &entry : entries) {
        if (entry.endsWith(QStringLiteral("mmc-pack.json"))) {
            root = entry.left(entry.size() - int(QStringLiteral("mmc-pack.json").size()));
            break;
        }
    }
    if (root.isEmpty()) return;
    for (const QString &entry : entries) {
        const bool wanted = entry.startsWith(root + QStringLiteral("patches/"))
            || entry == root + QStringLiteral("mmc-pack.json")
            || entry == root + QStringLiteral("instance.cfg");
        if (!wanted) continue;
        QByteArray bytes;
        if (ZipUtil::readEntry(zipPath, entry, &bytes, &error, QCoreApplication::translate("ImportController", "保存包身份"), 8 * 1024 * 1024)
            != ZipUtil::EntryStatus::Ok) continue;
        const QString relative = entry.mid(root.size());
        const QString out = target + QLatin1Char('/') + relative;
        if (!QDir().mkpath(QFileInfo(out).absolutePath())) continue;
        QFile file(out);
        if (file.open(QIODevice::WriteOnly)) file.write(bytes);
    }
    m_owner->appendLog(tr("已把原包的启动补丁保存在实例目录（导出时可以原样带回）。"));
}


