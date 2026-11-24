/**
 * @file VersionManagerModpack.cpp
 * @brief `VersionManager` 的整合包导入那一摊：从用户选的那个 zip 到"实例立起来、覆盖文件摊好"。
 *
 * 【为什么是这一摊】
 *
 * 一次导入 = 好几件事（装加载器 → 落实例薄 json → 下清单那批文件 → 解压覆盖），每一步的
 * 接续点都在别人的信号槽里。`ModpackInstall` 那台状态机、它认的那份清单
 * （`ModpackImport`）、以及"失败不删半成品、下次能接着下"这几条规矩是一件事，摆在一个
 * 文件里才看得全。
 *
 * 同一个文件里还装着「装一个用户起了名字的实例」那台只有两步的小状态机
 * （`NamedInstance`）：它和导入共用的是"落实例 json + 补原版"这两段，形状一样、
 * 收尾的规矩也一样。
 *
 * 【不在这里的】
 *
 * 导出整合包在 `VersionManagerExport.cpp`；加载器安装本身在
 * `VersionManagerLoaderInstall.cpp` —— 这里只负责"叫它来装"。批次结束时该接哪一段，
 * 判断仍在核心那个文件的 `onAllDownloadsFinished()` 里。
 */

#include "VersionManager.hpp"
#include <QCoreApplication>
#include "VersionManagerInternals.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include "../download/DownloadRetryPolicy.hpp"
#include "../download/LoaderMeta.hpp"
#include "../util/AppPaths.hpp"
#include "../util/StorageAvailability.hpp"
#include "../util/WriteDirectoryGuard.hpp"
#include "../util/ZipUtil.hpp"
#include "InstanceFiles.hpp"
#include "ModpackCompletion.hpp"
#include "ModpackImport.hpp"
#include "ModpackInstallReport.hpp"

using namespace VersionManagerInternals;

// ---------------------------------------------------------------------------
// 整合包导入（Modrinth 格式）
// ---------------------------------------------------------------------------

bool VersionManager::isModpackImporting() const { return m_modpackInstall.active(); }

bool VersionManager::loaderKindForDependencyKey(const QString &key, LoaderKind *out) {
    // 逐条列出来（不是遍历比较）：将来加载器多一个时，编译器会替你盯一眼"这里该跟着改"。
    // 没见过的键落到最后那个 return false，是**正常路径的一部分** —— 界面在更早
    // 一步就用 ModpackImport::unsupportedLoaderText() 把原因说清楚了。Quilt 自 2.1.4
    // 起走 profile + 库的安装路径，所以这里也认 quilt-loader。
    if (key == ModpackImport::fabricLoaderKey()) {
        if (out != nullptr) {
            *out = LoaderKind::Fabric;
        }
        return true;
    }
    if (key == ModpackImport::forgeLoaderKey()) {
        if (out != nullptr) {
            *out = LoaderKind::Forge;
        }
        return true;
    }
    if (key == ModpackImport::neoforgeLoaderKey()) {
        if (out != nullptr) {
            *out = LoaderKind::NeoForge;
        }
        return true;
    }
    if (key == ModpackImport::quiltLoaderKey()) {
        if (out != nullptr) {
            *out = LoaderKind::Quilt;
        }
        return true;
    }
    return false;
}

void VersionManager::importModpack(const QString &zipPath, const QString &instanceId,
                                   const ModpackImport::Pack &pack,
                                   const QString &installerJavaPath,
                                   const QString &manifestFingerprint) {
    QString lockProblem;
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        emit versionDownloadFinished(instanceId, false, lockProblem);
        return;
    }
    // 三道闸和 installLoaderVersion() 的入口一样，理由也一样：一次只做一件事。不这么拦的话，
    // 两个状态机同时开着，谁先收到别人那批文件的信号就说不清了。
    if (m_modpackInstall.active() || m_loaderInstall.active() || downloadManager->isBusy()) {
        emit versionDownloadFinished(instanceId, false,
                                     tr("已有下载或者安装任务正在进行，请等它结束再导入。"));
        return;
    }

    // 加载器**先翻译出来再开工**。翻译不了就不开工（界面在更早一步已经用
    // unsupportedLoaderText() 拦过一次，这里是数据层的第二道：数据层不该被绕过去）。
    LoaderKind kind = LoaderKind::Fabric;
    const bool hasLoader = !pack.loaderKey.isEmpty();
    const bool loaderKnown = hasLoader && loaderKindForDependencyKey(pack.loaderKey, &kind);
    if (hasLoader && !loaderKnown) {
        emit versionDownloadFinished(
            instanceId, false,
            ModpackImport::unsupportedLoaderText(pack.loaderKey, pack.loaderVersion));
        return;
    }
    if (hasLoader && !loaderInstallSupported(kind)) {
        // 接口有、实现没有的那天（比如将来加上 Quilt 的键、还没接安装）。这里必须自己发
        // 终态：installLoaderVersion() 在这种情况下只发一句中间话**不**发终态，导入的状态机
        // 会永远停在 InstallingLoader 上，界面按钮再也亮不回来。
        emit versionDownloadFinished(instanceId, false,
                                     tr("当前启动器暂不支持 %1，整合包未导入。")
                                         .arg(loaderDisplayName(kind)));
        return;
    }

    // CurseForge 的清单在"换"之前 `files` 是空的（界面在弹确认框之前已经调过一次
    // resolveCurseForgeFiles，见它的注释）。这里这第二道**不是多余的**：漏了它，
    // 走到 startModpackFiles() 就是一个只有加载器、一个 mod 都没有的实例，
    // 而用户看到的终态是"导入好了"。
    // **但"清单本来就一条没写"不能按这个拦**（`declaredFileCount == 0`）：纯 overrides
    // 的包（整个 .minecraft 塞进压缩包、清单里 files 是空数组）就是要走"零下载、直接解"。
    // 判据看的是**清单点了几条**，不是解析之后还剩几条。
    if (pack.format == ModpackImport::Format::CurseForge && pack.files.isEmpty() &&
        pack.declaredFileCount != 0 && (!pack.pendingRefs.isEmpty() || pack.fileIssues.isEmpty())) {
        emit versionDownloadFinished(
            instanceId, false,
            tr("无法从 CurseForge 获取此整合包文件的下载地址，导入已取消。请重试。若问题持续，请在「设置 → 下载」中填写自己的 CurseForge API 密钥。"));
        return;
    }

    m_modpackInstall = ModpackInstall();
    m_modpackInstall.zipPath = zipPath;
    m_modpackInstall.instanceId = instanceId;
    m_modpackInstall.pack = pack;
    m_modpackInstall.manifestFingerprint = manifestFingerprint;

    const QString loaderText =
        hasLoader ? tr(" + %1 %2").arg(loaderKnown ? loaderDisplayName(kind) : pack.loaderKey,
                                      pack.loaderVersion)
                  : QString();
    emit modpackImportMessage(tr("开始导入整合包「%1」：Minecraft %2%3。")
                                  .arg(pack.name, pack.gameVersion, loaderText));

    if (!hasLoader || !pack.patchedRuntime.isEmpty()) {
        // 纯原版的整合包：没有加载器要塞，父版本就是原版那一份。原版也得先装齐 ——
        // 实例那份 json 的 inheritsFrom 指着它，指着一个不存在的版本启动时会报
        // "版本信息缺失"（见继承链那一段的报错）。
        m_modpackInstall.phase = ModpackInstall::Phase::WaitingParentJson;
        emit modpackImportStageChanged(
            ModpackStage::LoaderAndVanilla,
            pack.patchedRuntime.isEmpty()
                ? tr("正在准备原版 %1...").arg(pack.gameVersion)
                : tr("正在准备原版 %1 与专用运行库...").arg(pack.gameVersion));
        emit modpackImportMessage(pack.patchedRuntime.isEmpty()
            ? tr("整合包未声明加载器，正在安装原版 %1…").arg(pack.gameVersion)
            : tr("将使用整合包内的启动补丁。正在补齐原版 %1，随后安装专用运行库…").arg(pack.gameVersion));
        // 走的路和"装加载器时顺手补原版"完全一样（取 json → 下那一批），续接点分别在
        // onVersionJsonFetched 和 onAllDownloadsFinished 里，判据就是上面那个相位。
        fetchVersionDetails(pack.gameVersion);
        return;
    }

    m_modpackInstall.phase = ModpackInstall::Phase::InstallingLoader;
    emit modpackImportStageChanged(
        ModpackStage::LoaderAndVanilla,
        tr("正在安装 %1 %2 与原版 %3...")
            .arg(loaderKnown ? loaderDisplayName(kind) : pack.loaderKey, pack.loaderVersion, pack.gameVersion));
    // 钉死版本（最后一个参数）：整合包说装哪个就装哪个。装加载器那条路自己会把原版补齐
    // （它要基于原版才跑得起来），所以这一步不用再管父版本。
    installLoaderVersion(pack.gameVersion, kind, installerJavaPath, pack.loaderVersion);
}

void VersionManager::startModpackFiles() {
    if (!m_modpackInstall.active()) {
        // 状态机已经被收掉了（导入失败、或者用户又点了别的）：这份续接没人要了。
        return;
    }
    const QString id = m_modpackInstall.instanceId;
    m_modpackInstall.phase = ModpackInstall::Phase::WritingVersionJson;

    // 实例自己那份 json：**只有 id + inheritsFrom 两个字段**。薄成这样是刻意的 —— Fabric
    // 装出来的那份也这么薄：主类、assetIndex、javaVersion、游戏参数全在加载器/原版那一层，
    // getVersionJson() 会顺着继承链合并（真机上那个包是 3 层：整合包 → fabric-loader → 原版）。
    // 自己抄一份完整 json 的坏处是它会变旧账：加载器换了、原版升了，抄来的那份对不上。
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("inheritsFrom")] = m_modpackInstall.loaderId;
    json[QStringLiteral("type")] = QStringLiteral("release");

    if (!m_modpackInstall.pack.patchedRuntime.isEmpty()) {
        // Standalone runtime: inheriting the vanilla array would reintroduce LWJGL2.
        json = m_modpackInstall.pack.patchedRuntime;
        json[QStringLiteral("id")] = id;
        json[QStringLiteral("type")] = QStringLiteral("release");
        json.remove(QStringLiteral("inheritsFrom"));
        const QJsonObject original = getVersionJson(m_modpackInstall.pack.gameVersion);
        const QString originalHash = original.value(QStringLiteral("downloads")).toObject()
            .value(QStringLiteral("client")).toObject().value(QStringLiteral("sha1")).toString();
        const QString requestedHash = json.value(QStringLiteral("downloads")).toObject()
            .value(QStringLiteral("client")).toObject().value(QStringLiteral("sha1")).toString();
        if (originalHash.isEmpty() || originalHash.compare(requestedHash, Qt::CaseInsensitive) != 0) {
            failModpackImport(tr("启动补丁引用的客户端与官方原版不一致，未安装修改过的客户端。")); return;
        }
        QString libraryProblem;
        if (!ModpackImport::publishBundledLibraries(m_modpackInstall.pack, AppPaths::minecraftDir(), &libraryProblem)) {
            failModpackImport(libraryProblem); return;
        }
    }

    QString error;
    // 导入开始的标记先落下（`isUnfinishedImport()` 靠它区分"我们没导完的"
    // 和"别的启动器装好的加载器实例"，见那边的 ④）。原子写入失败就停止，
    // 不继续产生缺少归属凭据的新文件。
    // 标记里顺手记下**这一份清单的指纹**：同名再导入时，`checkImportTargetName()` 靠它
    // 认出"上一次没导完的是这个包"还是"另一个包撞了名字"。标记里没有指纹（读侧解析
    // 不出来的当成"没有指纹"）时照常放行，盘上已有的半成品照样能接着导入。
    QDir().mkpath(AppPaths::versionDir(id));
    QJsonObject markerJson;
    markerJson[QStringLiteral("attempt")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    markerJson[QStringLiteral("startedAt")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!m_modpackInstall.manifestFingerprint.isEmpty()) {
        markerJson[QStringLiteral("manifestSha256")] = m_modpackInstall.manifestFingerprint;
    }
    const QByteArray markerBytes = QJsonDocument(markerJson).toJson(QJsonDocument::Compact);
    QSaveFile marker(InstanceFiles::importMarkerPath(id));
    if (!marker.open(QIODevice::WriteOnly) || marker.write(markerBytes) != markerBytes.size() || !marker.commit()) {
        failModpackImport(tr("无法保存未完成导入标记，未继续写入实例：%1").arg(marker.errorString()));
        return;
    }
    if (!writeVersionJson(id, AppPaths::versionDir(id), json, &error)) {
        failModpackImport(tr("无法将版本文件保存到 %1：%2\n\n导入未完成，请修复目录权限后重试。")
                              .arg(AppPaths::versionDir(id), error));
        return;
    }
    // 刚写下的这份是**新的**：在这之前读到的任何缓存（合并结果那张表尤其）都是旧的。
    versionJsons[id] = json;
    mergedVersionJsons.remove(id);

    const QList<ModpackImport::PackFile> files = m_modpackInstall.pack.files;
    m_modpackInstall.phase = ModpackInstall::Phase::DownloadingFiles;
    m_currentVersionDownloading = id;  // 这一批的"名字"（兜底终态那条路要用）
    emit modpackImportStageChanged(
        ModpackStage::ModpackFiles,
        files.isEmpty() ? tr("无额外模组文件，准备解压覆盖文件...")
                        : tr("正在下载整合包清单文件（共 %1 个）...").arg(files.size()));
    if (!m_modpackInstall.pack.patchedRuntime.isEmpty()) {
        emit modpackImportMessage(tr("正在补齐专用运行库和客户端文件，之后将解压整合包…"));
    } else if (files.isEmpty()) {
        // 零下载是合法的（纯 overrides 的包、MultiMC 实例、直接压进来的 .minecraft）。
        // 这批作业一条都不发，直接走 endOwnBatch() → onAllDownloadsFinished() 那条路。
        emit modpackImportMessage(
            tr("整合包没有待下载文件，正在解压内置文件…"));
    } else {
        emit modpackImportMessage(tr("开始下载清单中的 %1 个文件。通过 SHA-1 校验的本地文件会复用…")
                                      .arg(files.size()));
    }

    const QString base = AppPaths::versionDir(id);
    beginOwnBatch();
    if (!m_modpackInstall.pack.patchedRuntime.isEmpty()) {
        registerVersionFiles(json, m_modpackInstall.pack.gameVersion,
            AppPaths::versionDir(id) + QLatin1Char('/') + id + QStringLiteral(".jar"));
    }
    // NOTE: beginBatch() 与 endBatch() 之间**不允许**任何提前 return（见 downloadVersion
    // 顶部那段）：下面这个循环里一句早退都没有，就是为了这个。
    for (const ModpackImport::PackFile &file : files) {
        // 先试哪一条由 preferredUrlOrder() 定（**优先 cdn.modrinth.com**，实测差 3~10 倍，
        // 数字在那边）：换完次序后第二条仍然是"取不回来时再试一次"那条。清单里几条地址是
        // 同一份文件的镜像，对调本身是安全的。
        const QStringList urls = ModpackImport::preferredUrlOrder(file.urls);
        downloadManager->downloadFile(urls.value(0), base + QLatin1Char('/') + file.path,
                                      file.sha1, file.size, VerifyMode::SizeAndSha1,
                                      urls.value(1));
    }
    endOwnBatch();
}

void VersionManager::extractModpackOverrides() {
    if (!m_modpackInstall.active()) {
        return;
    }
    const QString id = m_modpackInstall.instanceId;
    m_modpackInstall.phase = ModpackInstall::Phase::ExtractingOverrides;
    emit modpackImportStageChanged(ModpackStage::ExtractingOverrides, tr("正在解压覆盖文件（config、资源包、光影等）..."));

    // 两个前缀一起解，顺序 = 优先级：Modrinth 的规矩是 client-overrides **盖过** overrides，
    // "谁在后谁赢"这一条是格式的一部分，不能调换。
    // 只把明确路径交给后台，所有状态机和 UI 信号仍在主线程收尾。
    //
    // NOTE: 这两个目录名来自清单，**不是写死的**：CurseForge 的清单里
    // `overrides` 那一项是自己写的（真包见过 `overrides` / `overrides-1.20.1` 这种），
    // 而它**没有** client-overrides 这个规矩 —— 那种包的 clientOverridesDir 是空串。
    //
    // **主前缀是空串 = 整包解**：MultiMC 实例和"直接把 .minecraft 压进来"的懒人包里
    // 没有 `overrides/` 这一层，要解的就是压缩包里那一棵树（前缀由 overridesDir 表达：
    // 包被套在一层目录里时它是 `包名/.minecraft` 这类）。这一档下 clientOverridesDir
    // 一定也是空的（那两种包没有这个概念），所以不用考虑"整包 + 再盖一层"。
    const ModpackImport::Pack &pack = m_modpackInstall.pack;
    QStringList layers;
    if (pack.overridesDir.isEmpty()) {
        layers.append(QString());  // 整包
    } else {
        layers.append(pack.overridesDir + QLatin1Char('/'));
        if (!pack.clientOverridesDir.isEmpty()) {
            layers.append(pack.clientOverridesDir + QLatin1Char('/'));
        }
    }
    emit modpackImportMessage(
        pack.overridesDir.isEmpty()
            ? tr("正在将 mods、config 和资源包等目录解压到实例…")
            : tr("正在解压整合包的配置、资源包和光影包等覆盖文件…"));
    struct Result { bool ok = false; int extracted = 0; QString error; };
    const auto result = std::make_shared<Result>();
    const QString archive = m_modpackInstall.zipPath;
    const QString destination = AppPaths::versionDir(id);
    const QStringList excludes = m_modpackInstall.pack.extractExcludes;
    m_modpackOverridesFailure.clear();
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, watcher, result, id, destination] {
        watcher->deleteLater();
        if (m_modpackInstall.phase != ModpackInstall::Phase::ExtractingOverrides
            || m_modpackInstall.instanceId != id) return;
        if (!m_modpackOverridesFailure.isEmpty()) {
            const QString reason = m_modpackOverridesFailure;
            m_modpackOverridesFailure.clear();
            failModpackImport(reason);
            return;
        }
        if (!result->ok) {
            failModpackImport(tr("无法解压整合包中的覆盖文件：%1\n\n已下载的内容位于 %2。配置、资源包或光影包等覆盖文件可能尚未安装。")
                                  .arg(result->error, destination));
            return;
        }
        m_modpackInstall.extractedEntries = result->extracted;
        finishModpackImport();
    });
    m_modpackOverridesCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_modpackOverridesCancelled;
    m_modpackOverridesWork = QtConcurrent::run(
        [result, archive, destination, layers, excludes, cancelled] {
            result->ok = ZipUtil::extractSubtrees(
                {archive}, destination, layers, &result->extracted, &result->error,
                [cancelled] { return cancelled->load(); }, excludes);
        });
    watcher->setFuture(m_modpackOverridesWork);
}

void VersionManager::finishModpackImport() {
    const QString id = m_modpackInstall.instanceId;
    const auto pack = m_modpackInstall.pack;
    const auto fingerprint = m_modpackInstall.manifestFingerprint;
    const auto destination = AppPaths::versionDir(id);
    const auto dataRoot = AppPaths::minecraftDir();
    QString planError;
    const auto core = integrityPlan(id, &planError);
    m_modpackInstall.phase = ModpackInstall::Phase::VerifyingCompletion;
    emit modpackImportStageChanged(ModpackStage::Verifying, tr("正在校验文件并完成实例写入..."));
    m_modpackOverridesCancelled = std::make_shared<std::atomic_bool>(false);
    m_modpackOverridesFailure.clear();
    const auto cancelled = m_modpackOverridesCancelled;
    struct Result { QList<ModpackImport::FileIssue> issues; QString error; };
    const auto result = std::make_shared<Result>();
    emit modpackImportMessage(tr("正在校验文件并保存安装记录（可取消，最长 900 秒）…"));
    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, result, id] {
        watcher->deleteLater();
        if (m_modpackInstall.phase != ModpackInstall::Phase::VerifyingCompletion || m_modpackInstall.instanceId != id) return;
        if (!m_modpackOverridesFailure.isEmpty()) {
            const auto reason = m_modpackOverridesFailure; m_modpackOverridesFailure.clear();
            failModpackImport(reason); return;
        }
        if (!result->error.isEmpty()) { failModpackImport(result->error); return; }
        commitModpackImport(result->issues);
    });
    m_modpackOverridesWork = QtConcurrent::run([=] {
        QElapsedTimer timer; timer.start();
        const auto stop = [&] { return cancelled->load() || timer.elapsed() >= 900000; };
        if (!fingerprint.isEmpty() && (!planError.isEmpty() ||
            !ModpackCompletion::capture(destination, dataRoot, pack, fingerprint, core, &result->error, stop))) {
            if (result->error.isEmpty()) result->error = planError;
            return;
        }
        const auto checked = ModpackInstallReport::verifyExisting(destination, pack.fileIssues, stop);
        if (checked.cancelled || stop()) { result->error = tr("最终校验已取消或超时，安装保持未完成"); return; }
        for (const auto &issue : pack.fileIssues)
            if (!checked.suppliedPaths.contains(issue.path)) result->issues.append(issue);
    });
    watcher->setFuture(m_modpackOverridesWork);
}

void VersionManager::commitModpackImport(const QList<ModpackImport::FileIssue> &issues) {
    // 要用的字段先取出来：reset() 会把它们一起清掉（忘了取的表现是那句话里名字变空的）。
    const QString id = m_modpackInstall.instanceId;
    const ModpackImport::Pack pack = m_modpackInstall.pack;
    const int skipped = pack.filesSkipped;
    const int notDistributable = pack.filesNotDistributable;
    const int missing = pack.filesMissing;
    const int extracted = m_modpackInstall.extractedEntries;
    const QString archivePath = m_modpackInstall.zipPath;

    const bool complete = issues.isEmpty() && notDistributable + missing <= pack.fileIssues.size();
    QString reportProblem;
    if (!ModpackInstallReport::write(AppPaths::versionDir(id), pack, issues, complete, &reportProblem)) {
        failModpackImport(tr("无法保存导入结果，保留未完成状态：%1").arg(reportProblem));
        return;
    }
    if (!complete) {
        // 必须在写 meta、移除 importing 标记之前终止，否则缺文件也会变成正常实例。
        QStringList details;
        for (const auto &issue : issues) details.append(ModpackInstallReport::description(issue));
        failModpackImport(tr("整合包安装尚未完成。已下载的文件和已应用的覆盖文件均已保留。\n\n%1\n\n逐项清单：%2\n补齐后可在报告中选择「完成安装」。如果旧安装缺少有效的安装记录，请用原整合包和原实例名称继续导入。")
            .arg(details.join(QStringLiteral("\n\n")),
                 QDir::toNativeSeparators(AppPaths::versionDir(id) + QStringLiteral("/wtfml.import-report.json"))));
        return;
    }

    // 自检：装完的东西得真的能被列表认出来。判据用**读侧那一份**（InstanceFiles::
    // resolveInstance），不自己再判一遍 —— "装是装了、列表里看不见"这个症状这个项目已经
    // 撞过两次（见 InstanceFiles.hpp 里 recognized() 那段），所以这里宁可多问一句。
    const InstanceFiles::Resolved resolved = InstanceFiles::resolveInstance(id);
    if (!resolved.recognized()) {
        failModpackImport(tr("文件已全部安装，但 %1 未被识别为实例，启动列表中不会显示。请检查 %2 中的版本 JSON，或移走该目录后重新导入。")
                              .arg(id, AppPaths::versionDir(id)));
        return;
    }

    // meta 也是安装收尾的凭据；写入成功后才允许移除最后的导入标记。
    // 写入显示名 `name`（= id），否则 displayParts().customName 为空，实例列表里会退化成
    // `<realVersion>-<loaderName>`（例如 `1.20.1-Fabric`），与同目录里的加载器版本撞名。
    QJsonObject fields;
    fields[kDisplayNameField] = id;
    if (instanceId(id).isEmpty()) {
        fields[kInstanceIdField] = newInstanceId();
    }
    fields[QStringLiteral("realVersion")] = pack.gameVersion;
    if (!pack.loaderKey.isEmpty()) {
        LoaderKind kind = LoaderKind::Fabric;
        if (loaderKindForDependencyKey(pack.loaderKey, &kind)) {
            fields[QStringLiteral("loaderName")] = loaderDisplayName(kind);
            fields[QStringLiteral("loaderVersion")] = pack.loaderVersion;
        }
    }
    QString metaError;
    if (!updateVersionMeta(id, fields, &metaError)) {
        failModpackImport(tr("文件已保留，但实例元数据写入失败，安装尚未完成：%1").arg(metaError));
        return;
    }
    // 标记是最后提交点。失败时即使 meta 已写入，读侧也必须继续阻止启动。
    if (!QFile::remove(InstanceFiles::importMarkerPath(id))) {
        failModpackImport(tr("文件已保留。因无法移除未完成标记，安装仍显示为未完成。"));
        return;
    }

    // MultiMC/Prism 包：把原包身份留在实例里（导出时原样带回 patches）。
    if (!pack.patchedRuntime.isEmpty()) {
        if (ModpackImport::preserveMultiMCSource(archivePath, AppPaths::versionDir(id))) {
            emit modpackImportMessage(tr("已把原包的启动补丁保存在实例目录（导出时可以原样带回）。"));
        }
    }
    // 少装了什么必须说出来（跳过的文件、解开的覆盖文件）—— 用户看不到的那部分最容易
    // 变成"进了游戏才发现少个 mod"。
    QString extra;
    if (skipped > 0) {
        extra += tr("（清单里有 %1 个文件按包里的说明跳过了：只给服务端用、没有下载地址、或者它自己是一个要单独装的整合包）")
                     .arg(skipped);
    }
    if (extracted > 0) {
        extra += tr("（解开了 %1 个覆盖文件）").arg(extracted);
    }
    // 到这里已通过逐项校验；不要仍把用户手动补齐的内容称作缺失。
    if (notDistributable > 0) {
        extra += tr("\n\n已校验通过 %1 个本地手动补齐文件。").arg(notDistributable);
    }
    // 这一种格式自带的话：MultiMC 实例导入了什么、没导入什么，lwjgl3ify 为什么会起不来。
    // 这条必须落在**终态**里 —— 用户装完就往下走，不会再翻日志。
    if (!pack.supportNote.isEmpty()) {
        extra += QStringLiteral("\n\n") + pack.supportNote;
    }

    const QString displayName = formatVersionName(displayParts(id));
    m_modpackInstall.reset();
    emit versionDownloadFinished(id, true,
                                 tr("整合包「%1」导入好了，实例列表里叫「%2」。%3")
                                     .arg(pack.name, displayName.isEmpty() ? id : displayName, extra));
}

void VersionManager::failModpackImport(const QString &reason) {
    if (m_modpackOverridesWork.isRunning()) {
        // 还在写盘时不释放占用，避免第二趟安装与尚未结束的解压互相覆盖。
        m_modpackOverridesFailure = reason;
        if (m_modpackOverridesCancelled) m_modpackOverridesCancelled->store(true);
        return;
    }
    // 先把加载器那半截收掉。LoaderInstall::reset() **不发**任何信号正是为了这里：整件事的
    // 终态只能发一个，否则界面会先弹一次"加载器没装成"、再弹一次"整合包没导入"。
    m_loaderInstall.reset();
    // 批次可能正开着（失败正好打在两批之间）：不放弃的话 m_batchOpen 永远是 true，之后每次
    // 下载都被"已有下载任务正在进行"顶回来（同 recoverFromInternalError 里那段）。
    abandonOwnBatch();

    const QString id = m_modpackInstall.instanceId;
    QString finalReason = reason;
    if (QFileInfo::exists(InstanceFiles::importMarkerPath(id))) {
        QList<ModpackImport::FileIssue> candidates = m_modpackInstall.pack.fileIssues;
        for (const auto &file : m_modpackInstall.pack.files) candidates.append({
            {file.curseForgeProjectId, file.curseForgeFileId}, QFileInfo(file.path).fileName(),
            file.path, file.sha1, file.size, tr("下载未完成或文件校验未通过")});
        // 失败路径不再在 UI 线程哈希几 GB 文件；报告保留候选，后台“重新校验”确认哪些已在盘上。
        const auto issues = candidates;
        QString reportProblem;
        if (!ModpackInstallReport::write(AppPaths::versionDir(id), m_modpackInstall.pack,
                                         issues, false, &reportProblem, reason)) {
            finalReason += tr("\n无法保存逐项报告：%1").arg(reportProblem);
        } else {
            finalReason += tr("\n逐项报告：%1").arg(QDir::toNativeSeparators(
                AppPaths::versionDir(id) + QStringLiteral("/wtfml.import-report.json")));
        }
    }
    m_modpackInstall.reset();
    emit versionDownloadFinished(id, false, finalReason);
}

// ---------------------------------------------------------------------------
// 装一个"用户起了名字的实例"（安装页那个「版本名称」框）
// ---------------------------------------------------------------------------

bool VersionManager::isNamedInstanceInstalling() const { return m_namedInstance.active(); }

void VersionManager::installNamedVanilla(const QString &instanceId, const QString &gameVersion) {
    if (m_namedInstance.active() || m_loaderInstall.active() || m_modpackInstall.active() ||
        downloadManager->isBusy()) {
        // NOTE: 忙的时候除了那句话，**也必须发终态信号** —— 界面在点下去那一刻就进了忙态，
        // 只发一条消息的话按钮再也亮不回来（"加载器安装还没接通"那一段踩过同样的坑，
        // 见 installLoaderVersion() 里 loaderInstallSupported 那道闸口旁边的注释）。
        emit instanceInstallMessage(tr("已有安装或下载正在进行，请等它结束。"));
        emit versionDownloadFinished(instanceId, false, tr("已有安装或下载正在进行，请等它结束。"));
        return;
    }
    if (instanceId.trimmed().isEmpty() || gameVersion.isEmpty()) {
        // 名字和原版号都该由调用方验过。走到这里说明调用方漏了 —— 那也不能往下走：
        // 一个空名字会变成 `versions/` 本身，那是把整个游戏目录当实例目录用。
        qWarning() << "installNamedVanilla called with an empty name or game version";
        emit versionDownloadFinished(instanceId, false,
                                     tr("实例名或原版版本号是空的，没有开始安装。"));
        return;
    }

    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(AppPaths::versionDir(instanceId), &lockProblem)) {
        emit instanceInstallMessage(lockProblem);
        emit versionDownloadFinished(instanceId, false, lockProblem);
        return;
    }
    m_namedInstance = NamedInstance();
    m_namedInstance.instanceId = instanceId;
    m_namedInstance.parentId = gameVersion;
    m_namedInstance.phase = NamedInstance::Phase::WaitingParentJson;

    // 名字和原版号一样 = 用户没改名字、装的就是原版本身。收尾时 writeNamedInstanceJson()
    // 会跳过那一份薄 json（写出来就是一份"指着自己"的 json），所以这里不用分开走。
    emit instanceInstallMessage(instanceId == gameVersion
                                    ? tr("正在安装原版 %1…").arg(gameVersion)
                                    : tr("正在安装 %1（基于原版 %2）…").arg(instanceId, gameVersion));

    // 这一次取 json 是**安装流程自己要的**，不是用户在下这个版本：相位上面已经摆好，
    // onVersionJsonFetched 里那一段会接手（它不发 versionDetailsFetched —— 发了界面会当成
    // "用户点了下载原版"，于是同一个版本下两遍、两个批次互相打架）。
    fetchVersionDetails(gameVersion);
}

void VersionManager::installNamedLoader(const QString &instanceId, const QString &gameVersion,
                                       LoaderKind kind, const QString &pinnedLoaderVersion,
                                       const QString &javaPath) {
    if (m_namedInstance.active() || m_loaderInstall.active() || m_modpackInstall.active() ||
        downloadManager->isBusy()) {
        emit instanceInstallMessage(tr("已有安装或下载正在进行，请等它结束。"));
        emit versionDownloadFinished(instanceId, false, tr("已有安装或下载正在进行，请等它结束。"));
        return;
    }
    if (instanceId.trimmed().isEmpty() || gameVersion.isEmpty()) {
        qWarning() << "installNamedLoader called with an empty name or game version";
        emit versionDownloadFinished(instanceId, false,
                                     tr("实例名或原版版本号是空的，没有开始安装。"));
        return;
    }
    if (!loaderInstallSupported(kind)) {
        // 和 installLoaderVersion() 里那道守卫同一个理由：界面也拦了一道，但数据层不该被
        // 绕过去 —— 绕过去的下场是"点了某个还没接通的加载器，结果装出来一个纯原版"。
        const QString loaderName = loaderDisplayName(kind);
        emit instanceInstallMessage(tr("暂不支持安装 %1。").arg(loaderName));
        emit versionDownloadFinished(instanceId, false,
                                     tr("当前版本暂不支持安装 %1。").arg(loaderName));
        return;
    }

    m_namedInstance = NamedInstance();
    m_namedInstance.instanceId = instanceId;
    m_namedInstance.phase = NamedInstance::Phase::WaitingLoader;
    // parentId 现在**还不知道**：加载器那个目录名是安装器自己拼的（OptiFine）或者我们按
    // 版本号算的，无论如何都要等装完。收尾的人（finishLoaderInstall）会现取 loaderId 填进来。

    const QString loaderName = loaderDisplayName(kind);
    emit instanceInstallMessage(pinnedLoaderVersion.isEmpty()
                                    ? tr("正在安装 %1（%2，基于原版 %3）…")
                                          .arg(instanceId, loaderName, gameVersion)
                                    : tr("正在安装 %1（%2 %3，基于原版 %4）…")
                                          .arg(instanceId, loaderName, pinnedLoaderVersion,
                                               gameVersion));

    // 整段交给那条已经验过的路。它自己会把原版补齐（加载器版本要基于原版才跑得起来），
    // 所以这里不用先下原版。收尾在 finishLoaderInstall() 里 —— 那里会先写实例那份薄 json，
    // 再发**唯一**那个终态信号。
    installLoaderVersion(gameVersion, kind, javaPath, pinnedLoaderVersion);
}

bool VersionManager::writeNamedInstanceJson() {
    if (!m_namedInstance.active()) {
        return false;
    }
    const QString id = m_namedInstance.instanceId;
    const QString parentId = m_namedInstance.parentId;
    if (id == parentId) {
        // 用户没改名字：实例就是父版本本身。旁边多写一份"指着自己的"json 只会在实例列表里
        // 多出一个和原版重名的条目（而且那个目录已经存在、是同一份）。
        return true;
    }

    // 和整合包实例、和 Fabric 装出来的那份**同一个形状**（见 startModpackFiles 上面那段）：
    // 只有 id + inheritsFrom。主类、客户端 jar、assetIndex、游戏参数全在父版本那一层，
    // getVersionJson() 会顺着继承链合并。抄一份完整 json 的坏处是它会变旧账。
    QJsonObject json;
    json[QStringLiteral("id")] = id;
    json[QStringLiteral("inheritsFrom")] = parentId;
    json[QStringLiteral("type")] = QStringLiteral("release");

    QString error;
    if (!writeVersionJson(id, AppPaths::versionDir(id), json, &error)) {
        failNamedInstance(tr("无法将版本文件保存到 %1：%2\n\n安装未完成，请检查目录权限后重试。")
                              .arg(AppPaths::versionDir(id), error));
        return false;
    }
    // 刚写下的这份是**新的**：在这之前读到的任何缓存（合并结果那张表尤其）都是旧的，
    // 留着会让"刚装好的实例"用回写之前的样子启动（或者压根读不到）。
    versionJsons[id] = json;
    mergedVersionJsons.remove(id);
    return true;
}

void VersionManager::finishNamedInstance(const QString &parentId, const QString &parentMessage) {
    if (!m_namedInstance.active()) {
        // 状态已经被收掉了（用户又点了一次、或者上一步失败了）：这份续接没人要了。
        return;
    }
    const QString id = m_namedInstance.instanceId;
    m_namedInstance.parentId = parentId;

    if (!writeNamedInstanceJson()) {
        // 写不进去时它自己已经发过失败终态了（见上面那个函数）。这里只能退出。
        return;
    }

    // A named alias has the same thin JSON shape as an interrupted legacy
    // import. Publish its normal instance identity only after all parent files
    // have completed, so the installed-list/launch guards can recognize it.
    if (id != parentId && ensureInstanceId(id).isEmpty()) {
        failNamedInstance(tr("文件已保留，但实例身份未能保存；未将本次安装报告为成功。"));
        return;
    }

    m_namedInstance.reset();
    emit versionDownloadFinished(
        id, true,
        id == parentId ? tr("原版 %1 已就绪。%2").arg(parentId, parentMessage)
                       : tr("实例「%1」安装完成（基于 %2）。%3").arg(id, parentId, parentMessage));
}

void VersionManager::failNamedInstance(const QString &reason) {
    // 父版本是加载器时，加载器那半截可能还在跑：一起收掉。LoaderInstall::reset() **不发**
    // 任何信号正是为了这里 —— 整件事的终态只能发一个，否则界面会先弹一次"加载器没装成"、
    // 再弹一次"实例没装成"。
    m_loaderInstall.reset();
    // 批次可能正开着（失败正好打在两批之间）：不放弃的话 m_batchOpen 永远是 true，之后每次
    // 下载都被"已有下载任务正在进行"顶回来（同 failModpackImport 里那段）。
    abandonOwnBatch();

    const QString id = m_namedInstance.instanceId;
    m_namedInstance.reset();
    emit versionDownloadFinished(id, false, reason);
}

bool VersionManager::writeVersionJson(const QString &versionId, const QString &destinationPath,
                                      const QJsonObject &json, QString *errorOut) {
    const QString invalid = InstanceFiles::validatePathSegment(versionId);
    if (!invalid.isEmpty()) { if (errorOut) *errorOut = invalid; return false; }
    if (!WriteDirectoryGuard::acquire(destinationPath, errorOut)) return false;
    if (!QDir().mkpath(destinationPath)) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法创建目录 %1。").arg(destinationPath);
        }
        return false;
    }
    const QString jsonPath =
        destinationPath + QLatin1Char('/') + versionId + QStringLiteral(".json");
    // QSaveFile：先写同目录下的临时文件、commit() 那一下才改名盖上去 —— 断电或者崩溃只会
    // 留下旧内容，不会留下半截 json（半截 json 下次启动时读不到 assetIndex / javaVersion，
    // 症状是"这个版本突然启动不了"，而且看不出是谁弄坏的）。
    QSaveFile file(jsonPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法写入目标路径 %1（%2）。").arg(jsonPath, file.errorString());
        }
        qWarning() << "Failed to save version JSON to" << jsonPath << file.errorString();
        return false;
    }
    const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (errorOut) *errorOut = tr("版本 JSON 写入不完整，原文件未替换。");
        return false;
    }
    if (!file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = tr("写 %1 的时候失败了（%2）。").arg(jsonPath, file.errorString());
        }
        qWarning() << "Failed to commit version JSON to" << jsonPath;
        return false;
    }
    qDebug() << "Saved version JSON to" << jsonPath;
    return true;
}

void VersionManager::adoptInstallerVersion(const QString &version) {
    // 到这一步相位是 FetchingInstallerList：版本号定了、安装器还没下。两条路（版本表挑出来的、
    // 整合包钉死的）都在这个相位上进来，下面那句 deferLoaderStep 的守卫认的就是它。
    // NOTE: 消息**不在这儿发** —— "挑中 xxx"和"正在装指定的 xxx"是两句不同的话，由调用方
    // 在调它之前自己说。
    const bool isForge = m_loaderInstall.kind == LoaderKind::Forge;
    m_loaderInstall.loaderVersion = version;
    m_loaderInstall.loaderId =
        isForge ? LoaderMeta::forgeLoaderId(m_loaderInstall.gameVersion, version)
                : LoaderMeta::neoforgeLoaderId(version);
    m_loaderInstall.installerUrl =
        isForge ? LoaderMeta::forgeInstallerUrl(m_loaderInstall.gameVersion, version)
                : LoaderMeta::neoforgeInstallerUrl(version);
    // 盘上那个 jar 就叫上游那个名字（forge-1.20.1-47.4.10-installer.jar /
    // neoforge-21.1.215-installer.jar），用户去 cache/installers/ 里看一眼能对上。OptiFine
    // 那条路不走这里（它的地址最后两段是 type/patch，不是文件名），由它自己填。
    m_loaderInstall.installerFileName =
        QFileInfo(QUrl(m_loaderInstall.installerUrl).path()).fileName();

    deferLoaderStep(LoaderInstall::Phase::FetchingInstallerList,
                    [this] { startInstallerDownload(); });
}

