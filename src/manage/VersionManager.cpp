/**
 * @file VersionManager.cpp
 * @brief `VersionManager` 的核心那一份：版本发现、元数据获取、下载编排、批次收尾，
 *        以及实例自己的那份 `wtfml.meta`（读 / 写 / 改名 / 复制 / 删除）。
 *
 * 【这个类为什么在四个文件里】
 *
 * 它原来是一个 6,137 行的 .cpp。按**职责**（不是按行数）拆开之后：
 *
 * * 这里 —— 版本清单 / 版本 json / 下载编排 / 一批下载的收尾 / 实例元数据；
 * * `VersionManagerLoaderInstall.cpp` —— 四种加载器各自的安装路与安装器进程；
 * * `VersionManagerModpack.cpp` —— 整合包导入，以及"装一个起了名字的实例"；
 * * `VersionManagerExport.cpp` —— 从盘上认出实例是什么、把它打成整合包；
 * * `VersionManagerInternals.hpp` —— 上面几份共用的小工具（原来住在本文件的匿名命名空间里）。
 *
 * 拆的界线是**一份状态机一摊**：`LoaderInstall` 只在那一个文件里走动，`ModpackInstall` /
 * `NamedInstance` 只在这一个…… 唯一的例外是批次结束那个接续点（`onAllDownloadsFinished()`）：
 * 它是三条路共用的岔口，所以留在核心这一份里，谁要接着往下走都在那儿一眼看全。
 *
 * @note 网络和下载统一走 DownloadManager；UI 不直接访问网络。
 */

#include "VersionManager.hpp"
#include <QCoreApplication>
#include "util/StorageAvailability.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include "../download/AssetDirectorKnownSet.hpp"
#include "../download/DownloadRetryPolicy.hpp"
#include "../download/InstallerFailure.hpp"
#include "../net/RequestBudget.hpp"
#include "../download/DownloadConcurrency.hpp"
#include "../settings/GlobalJavaSettings.hpp"

#include "FolderManager.hpp"
#include "InstanceMetaFile.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QSet>
#include <QUrl>
#include <QUuid>

#include "../download/LegacyForgeInstall.hpp"
#include "../download/LoaderMeta.hpp"
#include "../download/ResourceCatalog.hpp"
#include "../launcher/VersionArguments.hpp"
#include "../launcher/VersionInheritance.hpp"
#include "../net/HttpClient.hpp"
#include "../util/AppPaths.hpp"
#include "../util/ZipUtil.hpp"
#include "FolderManager.hpp"
#include "InstanceFiles.hpp"
#include "ModpackInstallReport.hpp"
#include "ModpackCompletion.hpp"

#include "VersionManagerInternals.hpp"
#include "download/DownloadCheckpoint.hpp"

using namespace VersionManagerInternals;

QJsonObject VersionManager::downloadContinuation() const {
    if (!m_ownBatchOpen || m_resourceDownload.running || m_modpackInstall.active()) return {}; // Import reports own their continuation.
    QJsonObject intent{{"root", AppPaths::minecraftDir()}};
    QString instance;
    if (m_namedInstance.active()) {
        instance = m_namedInstance.instanceId;
        intent.insert("instance", instance);
        if (m_loaderInstall.active()) {
            intent.insert("kind", "named-loader"); intent.insert("game", m_loaderInstall.gameVersion);
            intent.insert("loader", int(m_loaderInstall.kind)); intent.insert("java", m_loaderInstall.javaPath);
            intent.insert("loaderVersion", m_loaderInstall.loaderVersion.isEmpty() ? m_loaderInstall.pinnedVersion : m_loaderInstall.loaderVersion);
        } else { intent.insert("kind", "named-vanilla"); intent.insert("game", m_namedInstance.parentId); }
    } else if (!m_currentVersionDownloading.isEmpty() && !m_loaderInstall.active() && !m_assetDirectorPrefetch.active) {
        instance = m_currentVersionDownloading; intent.insert("instance", instance); intent.insert("kind", "repair");
    } else return {};
    QJsonObject originals;
    const QString json = AppPaths::versionDir(instance) + '/' + instance + ".json";
    const QString meta = AppPaths::versionDir(instance) + "/wtfml.meta";
    for (const auto &path : {json, meta}) {
        const auto state = DownloadCheckpoint::fingerprint(path);
        if (state.isEmpty()) return {};
        originals.insert(path, state);
    }
    intent.insert("originals", originals);
    return intent;
}

VersionManager::VersionManager(QObject *parent)
    : QObject(parent), downloadManager(new DownloadManager(this)) {
    // `loaderVersionsFetched` 那一个信号带的是一份 LoaderVersionList（不是 QStringList）——
    // Q_DECLARE_METATYPE 在 LoaderMeta.hpp 里，这里把它登记进 Qt 的元类型表：不是为了让当下
    // 这条直连能用（直连不看元类型表），而是**排队的连接和测试里的 QSignalSpy 要认它** ——
    // 少了这一句，那两种用法会在运行期回一句"Type not registered"然后收不到东西。
    qRegisterMetaType<LoaderMeta::LoaderVersionList>("LoaderMeta::LoaderVersionList");
    connect(downloadManager, &DownloadManager::versionManifestFetched, this,
            &VersionManager::onVersionManifestFetched);
    connect(downloadManager, &DownloadManager::versionJsonFetched, this,
            &VersionManager::onVersionJsonFetched);
    // NOTE: 只认整批结束的信号。每个文件的完成都转发成 versionDownloadFinished 的话，
    // 下载一个版本会在日志里刷出几千行一模一样的话。
    connect(downloadManager, &DownloadManager::allDownloadsFinished, this,
            &VersionManager::onAllDownloadsFinished);
    // 单个文件的结果（更新任务里那一步"哪个资源成了/没成"）。★ **只管资源那一批**：
    // 上面那句 NOTE 说的"每完成一个文件转发一次"会刷屏，这里转发的前提是"这一批下的是
    // 用户在下载页/更新中心点名的那几个资源"（`m_resourceDownload.running`）——那种一批
    // 就是几个到几十个文件，正好是逐条报得完、也正需要逐条报的东西。
    connect(downloadManager, &DownloadManager::downloadFinished, this,
            [this](const QString &fileName, bool success, const QString &errorMessage) {
                if (m_resourceDownload.running) {
                    emit resourceDownloadFileFinished(m_resourceDownload.label, fileName, success,
                                                      errorMessage);
                }
            });
    connect(downloadManager, &DownloadManager::batchProgress, this,
            [this](int done, int total, int failed, const QString &currentFile) {
                emit versionDownloadStats(done, total, failed, currentFile);
            });
}

VersionManager::~VersionManager() {
    // 后台扫盘那一发：叫停（工作线程每个目录看一眼旗），不在此等它 —— 它只读盘。
    // watcher 是本对象的子对象，析构之后不会再有回调。
    if (m_installedScanCancelled) m_installedScanCancelled->store(true);
    // 工作线程只持有路径与结果值；退出前等它结束，避免程序退出后仍在写实例。
    // 先通知外部解压进程退出，避免析构时一直等完整解压超时。
    if (m_modpackOverridesCancelled) m_modpackOverridesCancelled->store(true);
    m_modpackOverridesWork.cancel();
    m_modpackOverridesWork.waitForFinished();
    // DownloadManager is automatically deleted as child
    if (installerProcess != nullptr && installerProcess->state() != QProcess::NotRunning) {
        if (installerTimer != nullptr) {
            installerTimer->stop();
        }
        installerProcess->kill();
        installerProcess->waitForFinished(5000);
    }
    // 那份临时工作目录（OptiFine 那条路才有）不是 QObject 的子对象，得自己收。正常路径下
    // 它在安装的终态里已经被删过了（finishLoaderInstall / failLoaderInstall），这里只是
    // 兜住"装到一半程序被关掉"：不收的话系统临时目录里会积一份几 MB 的假 .minecraft。
    delete optifineWorkDir;
}

namespace {

/// 一次扫盘的结果（后台那一版要把它整个带回主线程）。
struct InstalledScanResult {
    QStringList versions;
    QStringList warnings;
};

/// 扫 `.minecraft/versions`。工作线程直接调这个 —— 只读盘和 `InstanceFiles` 的纯判据，
/// 不碰 `VersionManager` 的任何成员。
///
/// @param cancelled 非空时每进一个目录看一眼：置真就收工（新请求把旧的叫停）。
QStringList scanInstalledVersions(const std::atomic_bool *cancelled, QStringList *warnings) {
    QStringList versions;

    // Scan for installed versions in .minecraft/versions
    QString versionsPath = AppPaths::versionsDir();
    QDir versionsDir(versionsPath);

    if (versionsDir.exists()) {
        QStringList versionDirs = versionsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &versionDir : versionDirs) {
            if (cancelled != nullptr && cancelled->load()) {
                return {};
            }
            // 判据是"认得出客户端 jar 在不在"，**不要求 jar 名和目录名一致** ——
            // 放宽的来龙去脉见 InstanceFiles.hpp 的块注释（一句话：用户自己在资源管理器里
            // 改过目录名的实例不能被静默丢掉）。
            //
            // 判据没有一路放松到"目录里有任意 .jar 就算已安装"：那样"顶层两个 jar、
            // 认不出哪个是客户端"的目录也会被收进来，点启动时才发现找不着 jar。
            // 宁可在这里说不认识，也不能让列表里躺着一个点了会炸的条目。
            const InstanceFiles::Resolved resolved = InstanceFiles::resolveInstance(versionDir);
            if (resolved.recognized()) {
                // **半成品导入不算装好了**。整合包导入第一步就把那份薄 json
                // （`id` + `inheritsFrom`）落盘，失败时目录刻意留着（已下的几百 MB 在里面）。
                // `recognized()` 只看"jar 在不在或有没有 inheritsFrom"，于是这种目录满足它 ——
                // 列进启动列表就能被启动（缺文件，崩在缺库上），而且启动时那一步
                // `updateVersionMeta()` 会写出 wtfml.meta，`isUnfinishedImport()` 从此再也
                // 认不出它，"接着上次的导入"那条路就断了。
                // 它该由 ImportController 在同一名字上认出来（`ImportNameVerdict::OkResume`）。
                if (InstanceFiles::isUnfinishedImport(versionDir)) {
                    if (warnings != nullptr) {
                        warnings->append(
                            VersionManager::tr("目录 %1 中的整合包尚未导入完成，缺少客户端 JAR，暂不显示在启动列表中。使用相同名称重新导入可延续下载。")
                                .arg(versionDir));
                    }
                    continue;
                }
                versions.append(versionDir);
                continue;
            }
            // 认不出来的目录里**有 jar** 就得说一声：用户看得见磁盘上确实有个 jar，
            // 却看不见它在列表里，不讲清楚就是又一次静默消失。
            // 一个 jar 都没有的目录（下载残骸、纯空目录、误丢进 versions/ 的整合包 zip）
            // 继续静默 —— 那些本来就没人指望它是实例，报了才知道是噪音。
            if (warnings != nullptr && !resolved.jarCandidates.isEmpty()) {
                warnings->append(VersionManager::tr("目录 %1 有 %2 个 JAR（%3），无法确定客户端文件，已跳过。")
                                     .arg(versionDir)
                                     .arg(resolved.jarCandidates.size())
                                     .arg(resolved.jarCandidates.join(QCoreApplication::translate("VersionManager", "、"))));
            }
        }
    }

    // NOTE: 没有已安装版本时这里返回的就是空列表。占位是界面层的职责：界面用下拉框的
    // placeholderText 表达"没有可选版本"；数据层塞一个假版本的话，每个调用方都得反过来
    // 判 versionId == "自定义版本" 才能把它拒掉。
    //
    // NOTE: versions/ 里除了真正的版本目录，还可能有用户随手丢进去的整合包 .zip
    // （真机 4 个、共 935 MB）和纯空目录（真机有个 0 字节的 `26.2hyperland`）。
    // 上面的 QDir::Dirs 过滤 + <id>.jar 判据已经把这两类都正确排除了，jars 为空也就
    // 不会上报 —— 别为了"看起来更宽松"把 .zip 也当版本收进来。
    return versions;
}

}  // namespace

QStringList VersionManager::installedVersions(QStringList *warnings) const {
    return scanInstalledVersions(nullptr, warnings);
}

void VersionManager::requestInstalledVersions(bool reportWarnings) {
    const quint64 generation = ++m_installedScanGeneration;
    // 上一发还在跑：叫停（它每个目录看一眼旗）。不 `waitForFinished` —— 旧结果本来就不
    // 打算用，等它只是把这次的请求拖住。
    if (m_installedScanCancelled) m_installedScanCancelled->store(true);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_installedScanCancelled = cancelled;

    auto *watcher = new QFutureWatcher<InstalledScanResult>(this);
    connect(watcher, &QFutureWatcher<InstalledScanResult>::finished, this,
            [this, watcher, generation] {
                const InstalledScanResult result = watcher->result();
                watcher->deleteLater();
                // 晚到的那一代直接丢（它已经被新请求盖掉）。析构之后这里根本不会进来：
                // watcher 是本对象的子对象，跟着一起销毁。
                if (generation != m_installedScanGeneration) {
                    return;
                }
                emit installedVersionsScanned(result.versions, result.warnings);
            });
    watcher->setFuture(QtConcurrent::run([cancelled, reportWarnings] {
        InstalledScanResult result;
        result.versions = scanInstalledVersions(cancelled.get(),
                                                reportWarnings ? &result.warnings : nullptr);
        return result;
    }));
}

QStringList VersionManager::availableVersions() const {
    QStringList versions;
    for (const MinecraftVersion &version : downloadManager->getAvailableVersions()) {
        versions.append(version.id);
    }
    return versions;
}

QList<VersionManager::VersionChannel>
VersionManager::groupIntoChannels(const QList<MinecraftVersion> &versions) {
    // 四堆**先全摆出来**（空的那几堆也在）：界面那四条分类是一条都不少的，点开是空的
    // 和"这一条压根不出现"是两回事。
    QList<VersionChannel> channels;
    channels.reserve(VersionCategory::channelCount());
    for (int i = 0; i < VersionCategory::channelCount(); ++i) {
        const VersionCategory::Channel channel = VersionCategory::channelAt(i);
        channels.append(VersionChannel{channel, VersionCategory::channelKey(channel),
                                       VersionCategory::channelText(channel), {}});
    }
    // channelFor() 的返回值是**一个**分类（不是一组标记），所以"一条版本只进一堆"和
    // "一条都不丢"是同一件事 —— 这里不用再去重、也不用再兜"漏掉的那些"。
    for (const MinecraftVersion &version : versions) {
        const int index = VersionCategory::channelIndex(
            VersionCategory::channelFor(version.id, version.type));
        channels[index].versions.append(version);
    }
    return channels;
}

QList<VersionManager::VersionChannel> VersionManager::availableVersionChannels() const {
    return groupIntoChannels(downloadManager->getAvailableVersions());
}

QList<VersionManager::LatestVersion> VersionManager::latestVersions(
    const QList<MinecraftVersion> &versions) {
    // 一条一趟扫完就地收工：清单几百条，两个坑（正式版、快照）都填上就不用再往下看了。
    // NOTE: 不能"边扫边 append" —— 清单是新到旧交错排的（快照常常排在正式版前面，比如
    // 26.4-snapshot-1 就压在 26.3 上面），那样出来的顺序会随清单变。这里先各自记下
    // 来，最后按**固定顺序**（正式版、快照）append，界面上那两行的位置才是稳的。
    bool gotRelease = false;
    bool gotSnapshot = false;
    LatestVersion release;
    LatestVersion snapshot;
    for (const MinecraftVersion &version : versions) {
        const VersionCategory::Channel channel =
            VersionCategory::channelFor(version.id, version.type);
        if (channel == VersionCategory::Channel::Release) {
            if (gotRelease) {
                continue;
            }
            release.channel = channel;
            release.version = version;
            gotRelease = true;
        } else if (channel == VersionCategory::Channel::Snapshot) {
            if (gotSnapshot) {
                continue;
            }
            snapshot.channel = channel;
            snapshot.version = version;
            gotSnapshot = true;
        }
        if (gotRelease && gotSnapshot) {
            break;
        }
    }

    QList<LatestVersion> latest;
    if (gotRelease) {
        latest.append(release);
    }
    if (gotSnapshot) {
        latest.append(snapshot);
    }
    return latest;
}

QList<VersionManager::LatestVersion> VersionManager::availableLatestVersions() const {
    return latestVersions(downloadManager->getAvailableVersions());
}

// ---------------------------------------------------------------------------
// 从信号里往回接续：排到下一轮事件循环（用法和理由见 .hpp 里 deferNextStep 的 NOTE）
// ---------------------------------------------------------------------------

void VersionManager::deferNextStep(std::function<void()> step) {
    // context 传 this：这个对象没了的话，排着的这一步会自动作废（Qt 的行为，不用自己判）。
    QMetaObject::invokeMethod(
        this,
        [this, step = std::move(step)] {
            try {
                step();
            } catch (const std::exception &e) {
                recoverFromInternalError(QString::fromUtf8(e.what()));
            } catch (...) {
                // 不是 std::exception 派生的一律叫不出名字，但也不能就这么穿出去。
                recoverFromInternalError(tr("未知异常"));
            }
        },
        Qt::QueuedConnection);
}

void VersionManager::deferLoaderStep(LoaderInstall::Phase expected, std::function<void()> step) {
    deferNextStep([this, expected, step = std::move(step)] {
        if (m_loaderInstall.phase != expected) {
            return;
        }
        step();
    });
}

void VersionManager::deferNamedStep(NamedInstance::Phase expected, std::function<void()> step) {
    deferNextStep([this, expected, step = std::move(step)] {
        // 状态已经被收掉了（用户又点了一次、或者上一步失败了）：这一份续接没人要了。
        // 少了这道守卫，收尾那句会对着一个已经结束的安装再说一遍"装好了"。
        if (m_namedInstance.phase != expected) {
            return;
        }
        step();
    });
}

void VersionManager::recoverFromInternalError(const QString &what) {
    qWarning() << "Internal error while continuing the next step:" << what;

    abandonOwnBatch();

    // 用既有的终态出口把话说出去：界面那边照常弹窗（错误要弹窗），用户知道出了事、
    // 也还能接着用别的功能。
    const QString reason =
        tr("下载发生内部错误（%1），任务已停止。请重启启动器后重试。").arg(what);
    // 内部错误之后不给「重试」：那一条路重下几个文件修不好（这次是代码里的毛病）。
    m_failedBatch.reset();
    unwindDownloadBatch(reason);
}

void VersionManager::unwindDownloadBatch(const QString &reason) {
    // 整合包导入排在加载器**前面**：导入的第一步就是装加载器，所以这两件事可能同时是活的。
    // 那种时候要收的是整件事 —— 只收掉加载器那半截的话，导入的状态机还停在 InstallingLoader
    // 上，而此后每一次导入都会被"已有任务正在进行"顶回来，只能重启启动器。
    if (m_modpackInstall.active()) {
        failModpackImport(reason);
        return;
    }
    // 排在加载器**前面**：装一个带名字的实例时，父版本是加载器，于是这两件事同时是活的，
    // 而用户等的是他起的那个名字。先收掉它（它会连加载器那半截一起收，见 failNamedInstance），
    // 否则那个状态就永远留在 WaitingLoader 上，之后每次装都"已有任务正在进行"。
    if (m_namedInstance.active()) {
        failNamedInstance(reason);
        return;
    }
    if (m_loaderInstall.active()) {
        failLoaderInstall(reason);
        return;
    }
    // 资源下载那一路（下载页那五类里挑一个装）**也在这条收尾路上**：它不属于上面任何
    // 一套安装流程，`m_resourceDownload.running` 一旦留在 true 上，之后
    // 每一次资源下载都会被"已有下载任务正在进行"顶回来，只能重启启动器 —— 和上面那三个
    // 状态机是同一个病，只是它没有"相位"可以看，更容易被漏掉。
    if (m_resourceDownload.running) {
        const QString label = m_resourceDownload.label;
        m_resourceDownload.reset();
        emit resourceDownloadFinished(label, false, reason);
        return;
    }
    emit versionDownloadFinished(m_currentVersionDownloading, false, reason);
}

bool VersionManager::canCancelDownload() const {
    return downloadManager->isBusy() || (m_modpackInstall.active() && m_modpackOverridesWork.isRunning());
}

bool VersionManager::cancelActiveDownload() {
    if (m_modpackInstall.active() && m_modpackOverridesWork.isRunning()) {
        failModpackImport(tr("已取消最终解压或校验。已安装文件保留，实例仍未完成。"));
        return true;
    }
    if (!downloadManager->isBusy()) {
        return false;
    }
    // 先把批次掐掉，再收尾。顺序不能换：unwindDownloadBatch() 里那几段终态一发出去，界面
    // 就会立刻解禁并且允许用户再点一次下载 —— 那一刻批次必须已经是"没在忙"。
    cancelOwnBatch();
    m_failedBatch.reset();
    unwindDownloadBatch(tr("下载已取消，已下载的文件会保留。"));
    return true;
}

bool VersionManager::canRetryDownload() const { return m_failedBatch.valid; }

QString VersionManager::retryBatchLabel() const {
    return m_failedBatch.valid ? m_failedBatch.label : QString();
}

bool VersionManager::retryDownload() {
    if (!m_failedBatch.valid || m_failedBatch.jobs.isEmpty()) {
        return false;
    }
    if (downloadManager->isBusy()) {
        // 一次一批（和 downloadVersion / repairVersionFiles / downloadResourceFiles 是同一条
        // 规矩）。硬叠上去的话两份进度会互相盖，而"取消"那一刻也说不清在取消谁。
        qWarning() << "A download batch is already running, refusing to retry.";
        return false;
    }
    const FailedBatch batch = m_failedBatch;
    m_failedBatch.reset();

    // 把"这一批叫什么"照原样摆回去。收尾那一段（`onAllDownloadsFinished()` 的末尾）就是靠
    // 这两个成员决定发哪个终态的 —— 这两句是"重试之后终态还能报对"的全部前提。不摆的话，
    // 一次模组补下会被报成「版本 <上一个实例> 的文件已全部就绪」。
    if (batch.resource) {
        m_resourceDownload.running = true;
        m_resourceDownload.label = batch.label;
    }
    m_currentVersionDownloading = batch.id;

    beginOwnBatch();
    for (const DownloadManager::FailedJob &job : batch.jobs) {
        // 校验档一律用 SizeAndSha1：失败的那几个在盘上根本不存在，这个档位只影响
        // fileAlreadyValid() 那一次问话，而那一次必然回 false。真正的判据（sha1 / 字节数）
        // 在收尾那一步，和档位无关（见 onDownloadReplyFinished）。
        downloadManager->downloadFile(job.url, job.destinationPath, job.expectedSha1,
                                       job.expectedSize, VerifyMode::SizeAndSha1,
                                       job.officialUrl);
    }
    endOwnBatch();
    qInfo() << "Retrying" << batch.jobs.size() << "failed download(s).";
    return true;
}

void VersionManager::fetchAvailableVersions() { downloadManager->fetchVersionManifest(); }

void VersionManager::fetchVersionDetails(const QString &versionId) {
    m_pendingVersionId = versionId;
    if (downloadManager->getAvailableVersions().isEmpty()) {
        m_fetchJsonAfterManifest = versionId;
        downloadManager->fetchVersionManifest();
    } else {
        downloadManager->fetchVersionJson(versionId);
    }
}

bool VersionManager::isDownloading() const { return downloadManager->isBusy(); }

QString VersionManager::busyVersionId() const {
    // 门控的理由见头里那条 NOTE（那个成员收尾后不清空）。★ 门控**用的是"这一批是不是我开的"**
    // 而不是 `isDownloading()`：下载器是共用的，服务端安装那四段也在这上面开批次 ——
    // 那一段时间里 isDownloading() 是真的，可这一格在写的是**上一次下过的那个版本号**，
    // 照它答的话就是"用户在装服务端，1.20.1 那个实例忽然点不动了"。
    return m_ownBatchOpen ? m_currentVersionDownloading : QString();
}

void VersionManager::setUseMirror(bool useMirror) { downloadManager->setUseMirror(useMirror); }

void VersionManager::setDownloadConcurrency(int count) {
    RequestBudget::setBulkLimit(DownloadConcurrency::normalize(count));
    downloadManager->setMaxConcurrentDownloads(count);
}

void VersionManager::setDownloadSpeedLimit(qint64 bytesPerSecond) {
    downloadManager->setSpeedLimitBytesPerSecond(bytesPerSecond);
}

void VersionManager::setCurseForgeApiKey(const QString &apiKey) {
    m_curseForgeApiKey = apiKey.trimmed();
}

void VersionManager::setCurseForgeApiBaseOverride(const QString &base) {
    m_curseForgeApiBaseOverride = base.trimmed();
}

void VersionManager::setModrinthApiBaseOverride(const QString &base) {
    m_modrinthApiBaseOverride = base.trimmed();
}

QString VersionManager::modrinthApiRoot() const {
    // 官方那一份的唯一出处仍是 ResourceCatalog（见 `ResourceCatalog::modrinthApiRoot()`）。
    return m_modrinthApiBaseOverride.isEmpty() ? ResourceCatalog::modrinthApiRoot()
                                               : m_modrinthApiBaseOverride;
}

void VersionManager::setVersionManifestUrlOverride(const QString &url) {
    downloadManager->setVersionManifestUrlOverride(url);
}

QString VersionManager::resolveCurseForgeFiles(ModpackImport::Pack *pack) {
    return resolveCurseForgeFiles(pack, curseForgeResolveSettings());
}

QString VersionManager::resolveCurseForgeFiles(ModpackImport::Pack *pack,
                                               const CurseForgeResolveSettings &settings) {
    if (pack == nullptr || pack->format != ModpackImport::Format::CurseForge ||
        pack->pendingRefs.isEmpty()) {
        // 不是 CF 的包、或者已经换过一次（pendingRefs 空了）：调用点可以对所有格式无脑调一次。
        return QString();
    }
    const QString base =
        ModpackImport::curseForgeApiBase(settings.apiKey, settings.apiBaseOverride);
    const bool hasKey = !settings.apiKey.isEmpty();
    QList<QPair<QByteArray, QByteArray>> headers;
    if (hasKey) {
        headers.append({QByteArrayLiteral("x-api-key"), settings.apiKey.toUtf8()});
    }

    /// 这一发没成时那句话（**必须给出一条用户自己能走的路**：镜像是别人的服务）。
    const auto failureText = [hasKey](const HttpClient::Reply &reply) {
        const QString hint = curseForgeKeyHint(hasKey);
        if (!reply.networkError.isEmpty()) {
            return tr("问 CurseForge 要文件清单时没连上：%1\n\n%2").arg(reply.networkError, hint);
        }
        if (reply.status == HttpClient::kStatusTooManyRequests) {
            return HttpClient::throttledText(tr("问 CurseForge 要文件清单"));
        }
        const QString apiError =
            reply.json.value(QStringLiteral("error")).toString().trimmed();
        return tr("CurseForge 的接口回了 HTTP %1%2\n\n%3")
            .arg(reply.status)
            .arg(apiError.isEmpty() ? QString() : tr("（%1）").arg(apiError), hint);
    };

    // 一发问多少条。接口那一侧对一批的条数有上限（官方文档是 1000，镜像更保守），
    // 我们自己定 100：正文小、重来代价小，而一个大包（几百条）也就三四发。
    constexpr int kChunk = 100;

    // 这一发**不借 `loaderNetworkManager()`**：那个 QNAM 归主线程（`new` 在
    // VersionManager 身上），而导入整合包这条路的预览现在会整段放到工作线程上跑
    // （见 ImportController 里 `runOffUiThread()` 那两处）。跨线程用同一个 QNAM 是
    // 数据竞争，Qt 的文档也明说一个 QNAM 只能在一个线程里用。这里自己开一个：
    // resolve 一次只发两三类请求（每类一两发），不依赖连接池复用；应用级代理
    // （QNetworkProxy::setApplicationProxy）对新开的 QNAM 一样生效。
    QNetworkAccessManager network;
    // ---- 第一发：号 → 文件（地址、字节数、sha1 都在这一发的答复里）----
    const QList<ModpackImport::PackRef> refs = pack->pendingRefs;
    ModpackImport::CurseForgeFiles files;
    for (int offset = 0; offset < refs.size(); offset += kChunk) {
        const QList<ModpackImport::PackRef> chunk = refs.mid(offset, kChunk);
        const HttpClient::Reply reply = HttpClient::send(
            &network, QUrl(base + QStringLiteral("/mods/files")),
            HttpClient::Verb::Post, ModpackImport::curseForgeFilesRequestBody(chunk),
            QByteArrayLiteral("application/json"), QByteArray(), HttpClient::kDefaultTimeoutMs,
            headers);
        if (!reply.ok()) {
            // **整件事停下**：一条地址都没换回来的话，"导入"就是装一个没有 mod 的实例，
            // 而用户看到的是"导入好了"。这里是"会挡住用户操作"的那一类，如实报。
            return failureText(reply);
        }
        const ModpackImport::CurseForgeFiles part = ModpackImport::parseCurseForgeFileInfo(reply.raw, chunk);
        if (!part.problem.isEmpty()) {
            return part.problem;
        }
        files.resolved += part.resolved;
        files.issues += part.issues;
        files.projectIds += part.projectIds;
        files.notDistributable += part.notDistributable;
        files.missing += part.missing;
    }

    // ---- 第二发：这些项目是哪一类（决定文件落到哪个子目录）----
    //
    // 去重：同一个项目可能在不同的分块里各出现一次（一个 mod 有两个文件），问两遍白花时间。
    QList<int> projectIds;
    {
        QSet<int> seen;
        for (const auto &issue : files.issues) {
            if (issue.ref.projectId > 0) files.projectIds.append(issue.ref.projectId);
        }
        for (const int projectId : files.projectIds) {
            if (!seen.contains(projectId)) {
                seen.insert(projectId);
                projectIds.append(projectId);
            }
        }
    }
    QHash<int, int> classIds;
    QHash<int, QString> projectWebUrls;
    QString classIdProblem;
    for (int offset = 0; offset < projectIds.size(); offset += kChunk) {
        const QList<int> chunk = projectIds.mid(offset, kChunk);
        const HttpClient::Reply reply = HttpClient::send(
            &network, QUrl(base + QStringLiteral("/mods")), HttpClient::Verb::Post,
            ModpackImport::curseForgeModsRequestBody(chunk),
            QByteArrayLiteral("application/json"), QByteArray(), HttpClient::kDefaultTimeoutMs,
            headers);
        if (!reply.ok()) {
            // NOTE: 这一发没成**不拦整件事** —— 缺的只是"落到哪个目录"（退成 mods/，
            // 见 placeCurseForgeFiles），文件本身下得下来。停在这里等于因为一个
            // "放哪儿"的问题把一个能装的包拒了。
            classIdProblem = failureText(reply);
            break;
        }
        QString problem;
        const QHash<int, int> part = ModpackImport::parseCurseForgeClassIds(reply.raw, &problem);
        const auto urls = ModpackImport::parseCurseForgeProjectWebUrls(reply.raw);
        for (const int id : chunk) if (urls.contains(id)) projectWebUrls.insert(id, urls.value(id));
        if (!problem.isEmpty()) {
            classIdProblem = problem;
            break;
        }
        for (auto it = part.constBegin(); it != part.constEnd(); ++it) {
            classIds.insert(it.key(), it.value());
        }
    }

    // ---- 落地：补上目录，写回 pack ----
    const ModpackImport::CurseForgePlacement placement =
        ModpackImport::placeCurseForgeFiles(files.resolved, classIds);
    pack->files = placement.files;
    pack->fileIssues = files.issues;
    for (auto &issue : pack->fileIssues) {
        // 不知道类目时不猜手动补齐位置；重新查询后再校验，避免把资源包误当模组。
        const QString dir = classIds.contains(issue.ref.projectId)
            ? ModpackImport::curseForgeFilePath(classIds.value(issue.ref.projectId)) : QString();
        if (!issue.fileName.isEmpty() && !dir.isEmpty() &&
            ModpackImport::isSafeRelativePath(dir + issue.fileName)) issue.path = dir + issue.fileName;
    }
    pack->fileIssues += placement.issues;
    for (auto &issue : pack->fileIssues) issue.projectWebUrl = projectWebUrls.value(issue.ref.projectId);
    pack->pendingRefs.clear();
    pack->filesNotDistributable = files.notDistributable;
    // 两类"没换到"合在一起：接口说没有的 + 拼不出安全路径的（多半是路径太长）。
    pack->filesMissing = files.missing + placement.missing;
    pack->filesSkipped += placement.skipped;

    if (!classIdProblem.isEmpty()) {
        emit modpackImportMessage(
            tr("%1\n\n有 %2 个文件的目标目录未能识别，已放入 mods/。")
                .arg(classIdProblem)
                .arg(placement.files.size()));
    }
    return QString();
}

void VersionManager::downloadVersion(const QString &versionId, const QString &destinationPath) {
    // NOTE: 这两道早退必须在 beginBatch() 之前。beginBatch() 与 endBatch() 之间
    // 不允许有任何提前 return —— 一旦漏掉 endBatch()，m_batchOpen 就永远是 true，
    // allDownloadsFinished 永不发射，UI 会一直停在"下载中"、按钮永久变灰。
    if (!versionJsons.contains(versionId)) {
        qWarning() << "Version JSON not available for" << versionId << ". Fetch details first.";
        // 也必须给 UI 一个终态，否则它会一直等这批下载结束。
        emit versionDownloadFinished(versionId, false, tr("尚未获取到该版本的 version.json。"));
        return;
    }
    if (downloadManager->isBusy()) {
        qWarning() << "A download batch is already running, refusing to start another.";
        emit versionDownloadFinished(versionId, false, tr("已有下载任务正在进行，请等它结束。"));
        return;
    }

    // 落点那块盘不在（移动硬盘拔了 / 网络盘断了）：这时候开批次，只会让每一个文件各自
    // 报一次 I/O 失败 —— 失败清单上是一串路径，用户看不出是盘没了。先问一次卷，
    // 一个字节都不写；提示里点名"要装到的那块盘"，而不是某个具体文件。
    const QString storageProblem = StorageAvailability::unavailableReason(
        destinationPath.isEmpty() ? AppPaths::minecraftDir() : destinationPath);
    if (!storageProblem.isEmpty()) {
        emit versionDownloadFinished(
            versionId, false,
            tr("目标磁盘当前不可用，本次未下载任何文件：\n%1").arg(storageProblem));
        return;
    }

    const QJsonObject &versionJson = versionJsons[versionId];

    // 把 version.json 落盘。缺了它，下一次启动时 getVersionJson() 取不到
    // javaVersion / assetIndex，会静默退回错误的 Java 版本和资产索引 ——
    // 26.1 需要 Java 25，退成 Java 21 就是 UnsupportedClassVersionError。
    // 保存失败必须在开批次前终止，避免下载成功却留下不能启动的实例。
    QString writeProblem;
    if (!writeVersionJson(versionId, destinationPath, versionJson, &writeProblem)) {
        emit versionDownloadFinished(versionId, false, writeProblem);
        return;
    }

    m_currentVersionDownloading = versionId;
    beginOwnBatch();

    registerVersionFiles(versionJson, versionId, destinationPath + "/" + versionId + ".jar");

    endOwnBatch();
}

void VersionManager::registerVersionFiles(const QJsonObject &versionJson,
                                          const QString &jarOwnerId, const QString &jarPath) {
    // Download client jar
    if (versionJson.contains("downloads") && versionJson["downloads"].isObject()) {
        const QJsonObject downloads = versionJson["downloads"].toObject();
        if (downloads.contains("client") && downloads["client"].isObject()) {
            const QJsonObject client = downloads["client"].toObject();
            const QString url = client["url"].toString();
            const QString sha1 = client["sha1"].toString();
            const qint64 size = client["size"].toVariant().toLongLong();

            // 带上 size：重新点下载时先比字节数，对得上才算"已经下过"，
            // 省掉一次 41 MB 的全文件哈希。
            // 版本号要一起传：镜像源下客户端 jar 是按版本号取的，不是按 sha1。
            downloadManager->downloadClientJar(jarOwnerId, url, jarPath, sha1, size);
        }
    }

    // Download libraries
    // NOTE: 库的目标目录走 AppPaths::libraryDir()（= <数据目录>/libraries），是**全体实例
    // 共用**的那一份 —— 和官方启动器、和别的启动器都共用同一份。
    // 库是**全体版本共用**的一份，落在 AppPaths::libraryDir()：给每个实例各下一份库
    // 要多花几百 MB，而且和共用那份不一致。真正"这个版本自己的"只有 versionDir()
    // 底下那两样（<id>.jar / <id>.json）。资源（assets）也共用，那边的目录由
    // AppPaths::assetsDir() 说了算。
    QList<Library> libraries = downloadManager->extractLibraries(versionJson);
    downloadManager->downloadLibraries(libraries, AppPaths::libraryDir());

    // Download assets
    AssetIndex assetIndex = downloadManager->extractAssetIndex(versionJson);
    downloadManager->downloadAssets(assetIndex);
}

void VersionManager::repairVersionFiles(const QString &versionId,
                                        const QStringList &flaggedPaths) {
    // 盘不在时，下面那句 `getVersionJson()` 只会回一个空对象，报"读不出 version.json"——
    // 用户会以为这个实例的 json 坏了，去别处找修法。先问一次卷，直说盘没了。
    const QString storageProblem =
        StorageAvailability::unavailableReason(AppPaths::minecraftDir());
    if (!storageProblem.isEmpty()) {
        emit versionDownloadFinished(
            versionId, false,
            tr("游戏目录所在的磁盘当前不可用，本次未执行任何操作：\n%1").arg(storageProblem));
        return;
    }
    // 这几道早退都在 beginBatch() 之前，而且都要发终态（理由见 downloadVersion() 顶部那段 NOTE）。
    const QJsonObject versionJson = getVersionJson(versionId);
    if (versionJson.isEmpty()) {
        qWarning() << "No version json on disk for" << versionId << ". Cannot repair.";
        emit versionDownloadFinished(versionId, false,
                                     tr("无法读取实例的 version.json，无法校验所需文件。"));
        return;
    }
    if (downloadManager->isBusy()) {
        qWarning() << "A download batch is already running, refusing to start another.";
        emit versionDownloadFinished(versionId, false, tr("已有下载任务正在进行，请等它结束。"));
        return;
    }
    // 这一批会覆盖 `versions/<id>/<id>.jar`，还会动**全体版本共用**的 `libraries/`
    // 与 `assets/`。和 `downloadVersion()` / 资源下载那几个入口同一条：先取所属数据根的
    // 生命周期锁，拿不到就发终态、一个字节都不写（不半截开始）。
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        emit versionDownloadFinished(versionId, false, lockProblem);
        return;
    }

    m_currentVersionDownloading = versionId;
    beginOwnBatch();

    // 体检点过名的那几条：这一批不问盘上那份，直接重下（见 .hpp 的 @param 和
    // DownloadManager::setFlaggedPaths()）。**必须在 beginBatch() 之后**——它会清掉上一批
    // 留下的那张表。体检没跑过 / 体检的不是这个实例时这里是一张空表，等于没点名任何文件。
    downloadManager->setFlaggedPaths(flaggedPaths);

    // 不写 json（见头文件里的 ②）；jar 的主人可能是父版本（见 ③）。
    registerVersionFiles(versionJson, InstanceFiles::jarOwnerId(versionId),
                         InstanceFiles::jarPath(versionId));

    endOwnBatch();
}

VersionManager::AssetDirectorPrefetchPlan
VersionManager::assetDirectorPrefetchPlan(const QString &versionId) const {
    AssetDirectorPrefetchPlan summary;
    AssetDirectorKnownSet::Manifest manifest;
    QString manifestProblem;
    if (!AssetDirectorKnownSet::loadEmbedded(&manifest, &manifestProblem)) {
        summary.refusal = tr("无法读取启动器内置的 AssetDirector 清单（%1）。")
                             .arg(manifestProblem);
        return summary;
    }
    // 消费者靠 `mods/` 下的**文件名**认（不读 jar：那要为一次检查解几百个包）。
    const AssetDirectorKnownSet::Set *set =
        AssetDirectorKnownSet::matchConsumer(manifest, modJarNames(versionId));
    if (set == nullptr) {
        return summary;  // applicable = false：这个实例不是已知集合的消费者
    }
    const AssetDirectorKnownSet::Plan plan =
        AssetDirectorKnownSet::plan(*set, AppPaths::assetsDir());
    summary.applicable = true;
    summary.setId = set->id;
    summary.note = set->note;
    summary.minecraftVersion = set->minecraftVersion;
    summary.objects = plan.objects;
    summary.reuseOverlay = plan.reuseOverlay;
    summary.reuseAssets = plan.reuseAssets;
    summary.toDownload = plan.toDownload;
    summary.bytesToDownload = plan.bytesToDownload;
    summary.bytesTotal = plan.bytesTotal;
    summary.refusal = plan.refusal;
    summary.upToDate = plan.refusal.isEmpty() && plan.toDownload == 0 && !plan.needVersionJson
                       && !plan.needAssetIndex;
    return summary;
}

bool VersionManager::prefetchAssetDirectorResources(const QString &versionId) {
    // 盘不在时一个字节都别写（和 repairVersionFiles() 同一条）。这里**不发终态**：
    // 调用方只把它当"这次没做"，不该弹一个框说盘没了。
    if (!StorageAvailability::unavailableReason(AppPaths::minecraftDir()).isEmpty()) {
        qWarning() << "Skipping AssetDirector prefetch: game storage is unavailable.";
        return false;
    }
    // 还没导完的实例连游戏目录都不完整，读 mods/ 没有意义。
    if (InstanceFiles::isUnfinishedImport(versionId)) {
        return false;
    }
    // 一次只有一批：别的下载/安装正在跑就不插队（预取只是省时间，不值得抢夺批次状态）。
    // NOTE: 判据只问"现在有没有一批在跑"，**不问** `m_currentVersionDownloading` ——
    // 那个成员是"上一次这一批叫什么"，收尾后**故意不清空**（busyVersionId() 那道门控要用它），
    // 拿它当"还有批次在跑"会把导入刚结束之后的预取永远挡在外面（实测就是这样：
    // 导入成功、日志里连一句"正在预取"都没有）。
    if (downloadManager->isBusy() || m_loaderInstall.active() || m_modpackInstall.active()
        || m_namedInstance.active()) {
        qInfo() << "Skipping AssetDirector prefetch: another download batch is running.";
        return false;
    }

    const AssetDirectorPrefetchPlan summary = assetDirectorPrefetchPlan(versionId);
    if (!summary.applicable) {
        qInfo() << "No AssetDirector known set matches" << versionId << "; nothing to prefetch.";
        return false;
    }
    if (!summary.refusal.isEmpty()) {
        // 超上限：**一个字节都不写**，而且要说清是哪套、多大。
        emit versionDownloadFinished(versionId, false,
                                     tr("「%1」没有预取：%2").arg(summary.note, summary.refusal));
        return false;
    }
    if (summary.upToDate) {
        qInfo() << "AssetDirector resources already complete for" << versionId
                << "; no download needed.";
        return false;  // 全都在盘上：不需要惊动用户
    }

    AssetDirectorKnownSet::Manifest manifest;
    QString manifestProblem;
    if (!AssetDirectorKnownSet::loadEmbedded(&manifest, &manifestProblem)) {
        // 上面那一次已经读过一遍，能走到这里说明这份清单在这两次之间变不了 —— 兜底报错。
        emit versionDownloadFinished(versionId, false, manifestProblem);
        return false;
    }
    const AssetDirectorKnownSet::Set *set =
        AssetDirectorKnownSet::matchConsumer(manifest, modJarNames(versionId));
    if (set == nullptr) {
        return false;  // 两次之间用户删掉了那个模组：不适用
    }
    const QString assetsRoot = AppPaths::assetsDir();
    const AssetDirectorKnownSet::Plan plan = AssetDirectorKnownSet::plan(*set, assetsRoot);
    if (!plan.refusal.isEmpty()) {
        emit versionDownloadFinished(versionId, false,
                                     tr("「%1」没有预取：%2").arg(set->note, plan.refusal));
        return false;
    }

    qInfo().nospace() << "AssetDirector known set " << set->id << " (" << set->minecraftVersion
                      << "): objects=" << plan.objects << " reuseOverlay=" << plan.reuseOverlay
                      << " reuseAssets=" << plan.reuseAssets << " toDownload=" << plan.toDownload
                      << " bytesToDownload=" << plan.bytesToDownload;

    // 写的是**全体实例共用**的 `<数据目录>/assets/asset_director`，和写共享 assets/ 同一条
    // 规矩：取数据根的生命周期锁，拿不到就一个字节都不写。
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        emit versionDownloadFinished(versionId, false, lockProblem);
        return false;
    }

    m_assetDirectorPrefetch.active = true;
    m_assetDirectorPrefetch.versionId = versionId;
    m_assetDirectorPrefetch.setId = set->id;
    m_assetDirectorPrefetch.objects = plan.objects;
    m_assetDirectorPrefetch.reuseOverlay = plan.reuseOverlay;
    m_assetDirectorPrefetch.reuseAssets = plan.reuseAssets;
    m_assetDirectorPrefetch.toDownload = plan.toDownload;
    m_assetDirectorPrefetch.bytesToDownload = plan.bytesToDownload;
    m_currentVersionDownloading = versionId;

    beginOwnBatch();
    // 两份 JSON 是 AD **自己**的副本（它不走 overlay 复用）：落在它读的那两条路径上，
    // 校验不过就重下。
    if (plan.needVersionJson) {
        downloadManager->downloadFile(
            set->versionJson.url,
            AssetDirectorKnownSet::overlayVersionJsonPath(assetsRoot, set->minecraftVersion),
            set->versionJson.sha1, set->versionJson.size);
    }
    if (plan.needAssetIndex) {
        downloadManager->downloadFile(
            set->assetIndex.url,
            AssetDirectorKnownSet::overlayAssetIndexPath(assetsRoot, set->assetIndexId),
            set->assetIndex.sha1, set->assetIndex.size);
    }
    for (const AssetDirectorKnownSet::Object &object : plan.missing) {
        downloadManager->downloadFile(
            AssetDirectorKnownSet::objectUrl(object.hash),
            AssetDirectorKnownSet::overlayObjectPath(assetsRoot, object.hash), object.hash,
            object.size);
    }
    endOwnBatch();
    return true;
}

QList<VersionIntegrity::ExpectedFile> VersionManager::integrityPlan(const QString &versionId,
                                                                   QString *error) const {
    if (error != nullptr) {
        error->clear();
    }
    // 和 repairVersionFiles() 同一条：**盘上那份**（走继承链合并）才是权威，不是 versionJsons
    // 那张内存表 —— 体检一个早就装好的实例恰恰最常发生在重开启动器之后，那时候表是空的。
    const QJsonObject versionJson = getVersionJson(versionId);
    if (versionJson.isEmpty()) {
        if (error != nullptr) {
            *error = tr("无法读取实例的 version.json，无法列出所需文件。");
        }
        return {};
    }

    // 两个解析结果由调用方递进去（VersionIntegrity::plan 不自己 new 一个 DownloadManager）：
    // 「哪些库、哪个资源索引」这件事只有一个出处 —— 就是装配侧用的这两个函数。
    return VersionIntegrity::plan(versionJson, versionId,
                                  downloadManager->extractLibraries(versionJson),
                                  downloadManager->extractAssetIndex(versionJson));
}

VersionIntegrity::Report VersionManager::checkInstanceIntegrity(const QString &versionId) const {
    VersionIntegrity::Report report;

    // 清单那一半搬去了 integrityPlan()（界面那条后台路要单独取它）。这一条同步路照旧：
    // 取清单 → 逐条比 → 只收不好的那些。
    QString error;
    const QList<VersionIntegrity::ExpectedFile> files = integrityPlan(versionId, &error);
    if (!error.isEmpty()) {
        report.error = error;
        return report;
    }

    report.checked = static_cast<int>(files.size());
    for (const VersionIntegrity::ExpectedFile &file : files) {
        if (VersionIntegrity::Verdict verdict = VersionIntegrity::check(file); !verdict.ok()) {
            report.problems.append(verdict);
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// 下载页那几类资源：把一个文件下到实例里（模组 / 数据包 / 资源包 / 光影）
// ---------------------------------------------------------------------------

bool VersionManager::downloadResourceFiles(const QString &destinationDir,
                                           const QString &busyInstanceId, const QString &label,
                                           const QList<ResourceFile> &files) {
    // 下面这几道早退**必须在 beginBatch() 之前**（理由见 downloadVersion() 顶部那段 NOTE：
    // beginBatch() 与 endBatch() 之间提前 return，m_batchOpen 就永远是 true）。
    // 每一道都要发终态信号 —— 界面那边点一下就在等一个结果，不发就等于"点了没反应"。
    if (destinationDir.isEmpty()) {
        emit resourceDownloadFinished(label, false, tr("没有说清要下到哪儿去。"));
        return false;
    }
    if (downloadManager->isBusy()) {
        qWarning() << "A download batch is already running, refusing to start another.";
        emit resourceDownloadFinished(label, false, tr("已有下载任务正在进行，请等它结束。"));
        return false;
    }
    // 那三套安装状态机在**它们的批次之间**是不忙的（比如正在拉版本表、跑安装器），
    // 光看 isBusy() 拦不住那一刻 —— 那时候插进来一批模组，DownloadManager 那份唯一的批次
    // 状态就会被两件事同时用（它们的接续点在 allDownloadsFinished 上，谁先收到就说不清了）。
    // 和 importModpack() / installLoaderVersion() 入口那三道闸同一个道理。
    if (m_loaderInstall.active() || m_modpackInstall.active() || m_namedInstance.active()) {
        emit resourceDownloadFinished(label, false,
                                      tr("已有下载或者安装任务正在进行，请等它结束再装。"));
        return false;
    }
    if (files.isEmpty()) {
        // 正常走不到（能点的那条版本一定有个 primary 文件），但"这个版本一个文件都没有"
        // 是接口允许的状态 —— 那时候要当场说清，而不是开一个空批次等一个永远不来的收尾。
        emit resourceDownloadFinished(label, false, tr("这个版本没有可下载的文件。"));
        return false;
    }
    // 落点那块盘不在：同 downloadVersion() 里那一段。资源下载的目的地可能是
    // mods/、resourcepacks/、shaderpacks/，各自跟着实例走 —— 问的就是这一个。
    const QString storageProblem = StorageAvailability::unavailableReason(destinationDir);
    if (!storageProblem.isEmpty()) {
        emit resourceDownloadFinished(
            label, false,
            tr("目标磁盘当前不可用，本次未下载任何文件：\n%1").arg(storageProblem));
        return false;
    }
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(destinationDir, &lockProblem)) {
        emit resourceDownloadFinished(label, false, lockProblem);
        return false;
    }

    QStringList paths;
    paths.reserve(files.size());
    for (const ResourceFile &file : files) {
        // 文件名来自接口，**不能**直接往盘上拼：里面出现 `../` 或者一个绝对路径，写出去的
        // 位置就不在目标目录里了。这不是防谁，是"接口给的东西要当数据看"这条底线 ——
        // 一个作者把文件名写成 "../../../startup.bat" 是完全做得到的。
        //
        // 判据本体在 `InstanceFiles::validateFileName()`（和"一段路径"同一套规矩，另加
        // 文件名那一条长度上限）：`:`（NTFS 的备用数据流）、结尾的点和空格（Windows 会
        // 静默吃掉）、保留设备名（`CON`）这些也都一并挡在门外。
        const QString nameProblem = InstanceFiles::validateFileName(file.filename);
        if (!nameProblem.isEmpty()) {
            emit resourceDownloadFinished(
                label, false,
                tr("版本文件名无效（%1）：%2\n安装已取消。").arg(file.filename, nameProblem));
            return false;
        }
        // 落点：**每一条自己可以带一个目录**（见 `ResourceFile::dir` 那段 —— 更新中心那一批
        // 是跨类勾出来的）。空 = 还是用这一批的 `destinationDir`，也就是默认那条路。
        // NOTE: `dir` 是我们自己拼的绝对路径（来自 `QFileInfo::absolutePath()`），**不是**
        // 接口给的东西 —— 所以这里不需要再防它一次（要防的是 `filename`，上面刚防过）。
        const QString dir = file.dir.isEmpty() ? destinationDir : file.dir;
        paths.append(dir + QLatin1Char('/') + file.filename);
    }

    // 这一批在写哪个实例记进 m_currentVersionDownloading（缓存那一类传空串 = 谁都不锁）。
    // 界面靠它（busyVersionId()）把这一个实例从"能启动"里滤掉 —— 往 mods/ 里写文件的
    // 时候用户点了启动，JVM 可能正好在读那半截文件。
    // NOTE: 写上**空串**也是一次有意为之的赋值：不赋的话，这一批期间 busyVersionId() 会
    // 回答**上一批版本下载**留下的那个名字（那个成员收尾后不清空），于是"往缓存里下个
    // 整合包"会把一个毫不相干的实例锁住 —— 而真正在写的那一个照样能启动，两件事都错。
    m_currentVersionDownloading = busyInstanceId;

    m_resourceDownload.running = true;
    m_resourceDownload.label = label;

    beginOwnBatch();
    for (int i = 0; i < files.size(); ++i) {
        downloadManager->downloadFile(files.at(i).url, paths.at(i), files.at(i).sha1,
                                       files.at(i).size, VerifyMode::SizeAndSha1);
    }
    endOwnBatch();
    // 跑到这儿才是"这一批真的开起来了"。上面那五道早退各自发过一次
    // `resourceDownloadFinished(label, false, …)`，而那个 label 和"真正跑完"那条路带的是
    // **同一个字符串** —— 调用方只能靠这个返回值分辨"失败了"和"根本没开始"（见头文件）。
    return true;
}


void VersionManager::clearVersionJsonCache() {
    // 换了一整份 .minecraft 之后**必须**喊这一句。
    //
    // 下面那两张表的键**只有版本 id**（就是目录名），而目录名在换文件夹之后不再是唯一的
    // —— 两份目录里各有一个 `1.20.1` 是常态（一份原版、一份装过加载器，或者两份的
    // javaVersion / assetIndex 完全不同）。换文件夹那条路若不喊这一句，这两张表就会继续
    // 拿旧的：在 A 里启动过 `1.20.1` → 切到 B → 启动 B 里的 `1.20.1`，
    // rawVersionJson() / mergedVersionJsons 命中的仍是 **A 那份**，于是 classpath、natives、
    // `javaVersion`、`assetIndex` 全按 A 算，而游戏自己去读 `B/versions/1.20.1/1.20.1.json`
    // —— 轻则 UnsupportedClassVersionError，重则两套库混着启动，而且屏幕上没有任何
    // 一句话说得通。同一条路还会污染 inheritanceProblem() 和实例列表的 channel 判定。
    //
    // NOTE: 别改成"按 (根目录, id) 做键" —— 那要在每一处读缓存的地方都带上根目录，
    // 而这里丢一次的成本只是下次读一遍那几个 json（几毫秒）。
    versionJsons.clear();
    mergedVersionJsons.clear();
    // 元数据缓存按**完整路径**做键，换文件夹本来就不会串；这里一起丢掉是为了别让
    // 上一份目录的那几百格留在表里（那些路径在接下来的会话里不会再被读到）。
    m_metaCache.clear();
}

QJsonObject VersionManager::rawVersionJson(const QString &versionId) const {
    const auto cached = versionJsons.constFind(versionId);
    if (cached != versionJsons.constEnd()) {
        return cached.value();
    }

    // 内存里没有就回读磁盘。下载时已经把 JSON 落盘，所以跨会话启动也能拿到
    // javaVersion 和 assetIndex，不需要先在同一次会话里点一次"下载"。
    //
    // NOTE: 路径走 InstanceFiles 解析，不按 <id>.json 硬拼 —— 目录名被外部改过之后，
    // 里面的 json 还叫老名字，硬拼会读不到，表现就是"实例认出来了、启动却说
    // 版本信息缺失"。认出来和还能启动是两件事。
    const QString jsonPath = InstanceFiles::jsonPath(versionId);
    QFile jsonFile(jsonPath);
    if (!jsonFile.exists() || !jsonFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Version JSON not found on disk:" << jsonPath;
        return QJsonObject();
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);
    jsonFile.close();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Failed to parse" << jsonPath << ":" << parseError.errorString();
        return QJsonObject();
    }

    // 填回缓存，同一次会话里不再重复读盘
    versionJsons[versionId] = doc.object();
    return doc.object();
}

QJsonObject VersionManager::getVersionJson(const QString &versionId) const {
    const auto cached = mergedVersionJsons.constFind(versionId);
    if (cached != mergedVersionJsons.constEnd()) {
        return cached->json;
    }

    QJsonObject merged = rawVersionJson(versionId);
    if (merged.isEmpty()) {
        // 取不到就不缓存（同 rawVersionJson 的理由：这次取不到可能只是"还没下载完"，
        // 缓存一个空对象会让之后下载好了也还是空的）。
        return merged;
    }

    // 顺着 inheritsFrom 往上合。三道闸：
    // - 访问过的 id 集合：挡环（手写的 json 可以 A 继承 B、B 继承 A）；
    // - 深度上限：挡一条长得离谱的链（光有集合挡不住它）；
    // - 父版本取不到：停在这儿，把手上这份"合了一部分"的交出去 —— 问题由
    // inheritanceProblem() 去说，这一层的职责只是尽量给出能用的东西。
    //
    // NOTE: 每一轮都是 `merge(当前结果, 上一层父版本)`，这是在**已经合过**的东西上再合
    // 一层。这样做是对的（父在前、子覆盖，一层层往外套）而且不会重复拼接 ——
    // 靠的是 merge() 把 inheritsFrom 丢掉、下一层的 id 从**父版本自己那份**里读。
    QSet<QString> seen;
    seen.insert(versionId);
    QString parentId = VersionInheritance::parentIdOf(merged);
    for (int depth = 0; depth < VersionInheritance::maxDepth() && !parentId.isEmpty(); ++depth) {
        if (seen.contains(parentId)) {
            qWarning() << "Inheritance cycle in version JSON:" << versionId << "-> ... ->"
                       << parentId;
            break;
        }
        seen.insert(parentId);

        const QJsonObject parent = rawVersionJson(parentId);
        if (parent.isEmpty()) {
            qWarning() << "Version" << versionId << "inherits from" << parentId
                       << "which is not installed — cannot merge";
            break;
        }
        merged = VersionInheritance::merge(merged, parent);
        parentId = VersionInheritance::parentIdOf(parent);
    }

    // 最后按"不看版本号的坐标"再去一次重（PCL 合并出来的 json 里同一个库会有两个版本）。
    //
    // NOTE: 位置在**这里**、而不是 mergeLibraries() 里 —— 有两种 json 根本不会走到 merge()：
    // 自己就是完整的那份（原版、以及 PCL 合并好之后**没有 inheritsFrom** 的那些，
    // 真机上崩得最狠的 `1.21.4-Fabric 0.16.14` 正是这一类：它顶层两个 asm 版本全在
    // 自己的数组里）。放在这条唯一的出口上，两种输入走同一条规矩。
    // 判据和证据见 VersionInheritance::dedupeLibraries() 的 NOTE。
    const QJsonArray libraries = merged.value(QStringLiteral("libraries")).toArray();
    if (!libraries.isEmpty()) {
        merged.insert(QStringLiteral("libraries"), VersionInheritance::dedupeLibraries(libraries));
    }

    // 只缓存**链正常走完**的那一份。
    //
    // NOTE: 上面两种 break（环、父版本没装）都会留下一份"合了一半"的 json。若不管走没走完
    // 都往缓存里塞，就会：启动前 MainWindow 先 getVersionJson() 拿到半份、再问
    // inheritanceProblem()，此时用户把缺的那个父版本装上 —— 同一个会话里第二次
    // getVersionJson() 直接命中半份缓存（inheritanceProblem() 是重新读盘、这时已经不抱怨了），
    // 启动器就拿着缺 assets/librarys 的半份 json 去启动，且不报任何错。
    // 环和深度超限同理：留在盘上的东西没变过，重读一次盘比用错的强。
    if (parentId.isEmpty()) {
        MergedJsonEntry entry;
        entry.json = merged;
        mergedVersionJsons.insert(versionId, entry);
    }
    return merged;
}

const VersionManager::MergedJsonEntry *VersionManager::mergedJsonEntry(
    const QString &versionId) const {
    if (!mergedVersionJsons.contains(versionId)) {
        // 取一次：取到了自然就进了缓存。取不到（文件不在 / 继承链没走完）**不缓存**，
        // 所以这里下一次问还会再试一遍 —— 和 getVersionJson() 的规矩一致。
        getVersionJson(versionId);
    }
    const auto it = mergedVersionJsons.constFind(versionId);
    return it == mergedVersionJsons.constEnd() ? nullptr : &it.value();
}

QString VersionManager::inheritanceProblem(const QString &versionId) const {
    QJsonObject json = rawVersionJson(versionId);
    if (json.isEmpty()) {
        // 连自己那份都没有：这是"版本信息缺失"那条路的事，不是继承的问题。
        return QString();
    }

    QSet<QString> seen;
    seen.insert(versionId);
    for (int depth = 0; depth < VersionInheritance::maxDepth(); ++depth) {
        const QString parentId = VersionInheritance::parentIdOf(json);
        if (parentId.isEmpty()) {
            return QString();
        }
        if (seen.contains(parentId)) {
            return tr("版本「%1」的继承链中出现循环，指向 %2。请重新安装此版本以恢复版本清单。")
                .arg(versionId, parentId);
        }
        seen.insert(parentId);

        json = rawVersionJson(parentId);
        if (json.isEmpty()) {
            // 这句话必须点明"要去装原版"，而不是套用那句"请重新下载该版本" —— 用户照
            // 后者做，下载的还是这个加载器版本（它的 json 本来就不含原版那份东西），
            // 于是问题原样复现。
            return tr("版本「%1」基于「%2」，但本机没有安装「%2」，启动不了。\n请先安装原版「%2」，再启动这个版本。")
                .arg(versionId, parentId);
        }
    }

    return tr("版本「%1」的继承链超出 %2 层，无法确定基础版本。")
        .arg(versionId)
        .arg(VersionInheritance::maxDepth());
}

QJsonObject VersionManager::readVersionMeta(const QString &versionId, QString *metaPathOut,
                                            bool *unreadableOut) const {
    const QString metaPath = AppPaths::versionMetaPath(versionId);
    if (metaPathOut != nullptr) {
        *metaPathOut = metaPath;
    }
    // 先看一眼盘上那一份还在不在、改没改过（`QFileInfo` 一次 stat），没变就用上一次读
    // 回来的那一份 —— 为什么要缓存、缓存又靠什么看见"改了名"，见 `m_metaCache` 那段。
    const QFileInfo info(metaPath);
    const bool exists = info.exists();
    const qint64 modifiedMs =
        exists ? info.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch() : 0;
    const qint64 size = exists ? info.size() : -1;
    auto cached = m_metaCache.find(metaPath);
    if (cached == m_metaCache.end() || cached->exists != exists
        || cached->modifiedMs != modifiedMs || cached->size != size) {
        MetaCacheEntry entry;
        entry.exists = exists;
        entry.modifiedMs = modifiedMs;
        entry.size = size;
        entry.meta = InstanceMetaFile::read(metaPath, &entry.status);
        if (m_metaCache.size() >= kMaxMetaCacheEntries) {
            m_metaCache.clear();
        }
        cached = m_metaCache.insert(metaPath, entry);
    }
    if (unreadableOut != nullptr) {
        // "改不动"的两种都算：内容读不懂，或者版本比这个启动器新。写侧两种都会拒写，
        // 想知道"能不能盖"的调用方看这一个标志就够了。
        *unreadableOut = cached->status == InstanceMetaFile::Status::Unreadable
                         || cached->status == InstanceMetaFile::Status::BadVersion;
    }
    return cached->meta;
}

bool VersionManager::updateVersionMeta(const QString &versionId, const QJsonObject &fields,
                                       QString *errorOut) {
    const QString invalid = InstanceFiles::validatePathSegment(versionId);
    if (!invalid.isEmpty()) { if (errorOut) *errorOut = invalid; return false; }

    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), errorOut)) return false;
    // 合并写的规矩只有一份（见 InstanceMetaFile）：读不懂 / 版本不认识就不覆盖、
    // null 删键、QSaveFile、缩进输出。这里只管"当前数据目录下的哪一份"和目录锁。
    const QString metaPath = AppPaths::versionMetaPath(versionId);
    const bool written = InstanceMetaFile::mergeWrite(metaPath, fields, errorOut);
    // 写完当场把缓存那一格丢掉：成功的那一下要立刻看得见新值（同一毫秒里同样的字节
    // 数那种改写，光靠"修改时刻 + 大小"是分不出来的）；失败那一下也丢掉，下一次读
    // 重新问盘，免得把"写失败了"和"盘上那份还是老样子"搅在一起。
    m_metaCache.remove(metaPath);
    return written;
}

QString VersionManager::instanceId(const QString &versionId) const {
    const QJsonValue value = readVersionMeta(versionId).value(kInstanceIdField);
    // 类型不对就当没有（可能是用户手改坏的）—— 调用方会去生成一个新的覆盖掉它。
    return value.isString() ? value.toString() : QString();
}

QString VersionManager::ensureInstanceId(const QString &versionId) {
    const QString existing = instanceId(versionId);
    if (!existing.isEmpty()) {
        return existing;
    }

    // QUuid 的 WithoutBraces 形式（36 字符带横线）。用它而不是自增整数：分组归属
    // 存在另一个文件里，自增 id 在"删了一个组再加一个"时会和残留的归属撞上，
    // 而 UUID 不会。
    const QString generated = newInstanceId();

    QJsonObject fields;
    fields.insert(kInstanceIdField, generated);
    QString error;
    if (!updateVersionMeta(versionId, fields, &error)) {
        // 走到这里说明磁盘有问题（目录不可写之类）。返回空串让调用方知道"没拿到"，
        // 而不是返回一个**没落盘**的 id —— 那个 id 下次会话就对不上了，属于静默错。
        qWarning() << "Failed to persist instance id for" << versionId << ":" << error;
        return QString();
    }
    return generated;
}

bool VersionManager::renameInstance(const QString &oldId, const QString &newName,
                                    QString *errorOut) {
    const auto fail = [errorOut](const QString &reason) {
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    };

    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), errorOut)) return false;
    const QString oldProblem = InstanceFiles::validatePathSegment(oldId);
    if (!oldProblem.isEmpty()) return fail(oldProblem);
    const QString newId = newName.trimmed();

    // ---------- 第 1 步：全部前置检查，磁盘一个字节都不动 ----------
    //
    // "正在跑"不在这里判，见 .hpp 里的 NOTE —— 那是 MainWindow 的闸口，必须更早。
    const QString problem = InstanceFiles::validateRenameTarget(newId, oldId);
    if (!problem.isEmpty()) {
        return fail(problem);
    }

    const QString oldDir = AppPaths::versionDir(oldId);
    if (!QDir(oldDir).exists()) {
        return fail(tr("找不到实例目录 %1。").arg(QDir::toNativeSeparators(oldDir)));
    }
    // 实例目录本身是 junction / 符号链接时**拒绝改名**。
    //
    // 下面这几步对链接的含义全是"穿过去":写 meta、把 `<旧名>.jar` / `<旧名>.json`
    // 改成新名字 —— 动的是**链接那一头**的文件；最后 `QDir().rename(oldDir, newDir)`
    // 又只挪链接本身（目标原地不动）。两头一对，用户看到的是"改名成功了"，而实际状态是
    // "那一头的 jar/json 被改了名、链接换了个名字指着它" —— 如果那一头是几个启动器/
    // 几个游戏文件夹共用的目录，别名启动器也会跟着坏；更常见的是用户把另一个盘上的实例
    // 挂进来，点一下改名就把那边动了。
    //
    // 和删除（`removeTreeWithoutFollowingLinks`）、复制（`copyTreeWithoutFollowingLinks`
    // 直接拒收链接）同一条规矩：**链接不追**，说不清楚就不动手。想改就去那一头改。
    {
        const QFileInfo oldDirInfo(oldDir);
        if (oldDirInfo.isJunction() || oldDirInfo.isSymLink()) {
            return fail(tr("「%1」是目录链接。请在原目录所在位置重命名，或先将链接替换为普通目录。")
                            .arg(QDir::toNativeSeparators(oldDir)));
        }
    }
    // NOTE: 这里**不判** `QDir(newDir).exists()`。目标已经存在的情况
    // validateRenameTarget() 里按重名挡过了，而它挡不了、这里也不该挡的正是
    // "只差大小写"那一种 —— 那种目标就是它自己，判存在会把唯一合法的改名拒掉。
    const QString newDir = AppPaths::versionDir(newId);

    // 改名要回滚时得把显示名放回去，所以先把老值记下来（可能是"压根没有"）。
    const QJsonValue oldDisplayName = readVersionMeta(oldId).value(kDisplayNameField);

    // ---------- 第 2 步：写 meta ----------
    //
    // 排在改名字前面：它可能失败，而它失败时我们还什么都没破坏；反过来先改目录名，
    // 写完 meta 却失败，就留下一个改完了名、显示名还指着老名字的实例。
    // 顺手把稳定 id 定下来（没有才生成）—— 这一句正是"被启动器碰过一次"的那一次，
    // 从今往后这个实例的分组归属不再靠目录名认人。两件事合成一次原子写。
    QJsonObject fields;
    fields.insert(kDisplayNameField, newId);
    if (instanceId(oldId).isEmpty()) {
        fields.insert(kInstanceIdField, newInstanceId());
    }
    QString error;
    if (!updateVersionMeta(oldId, fields, &error)) {
        return fail(error);
    }

    // ---------- 第 3~5 步：jar、json、目录，从里往外改 ----------
    const QString oldJarPath = oldDir + QLatin1Char('/') + InstanceFiles::jarName(oldId);
    const QString newJarPath = oldDir + QLatin1Char('/') + newId + QStringLiteral(".jar");
    const QString oldJsonPath = oldDir + QLatin1Char('/') + InstanceFiles::jsonName(oldId);
    const QString newJsonPath = oldDir + QLatin1Char('/') + newId + QStringLiteral(".json");

    // 只回滚**真做成了**的那几步。两个标志必须在回滚 lambda 之前声明，好让它捕获。
    bool jarRenamed = false;
    bool jsonRenamed = false;

    /// 逆序回滚，然后把"磁盘现在到底怎么样了"告诉用户。
    ///
    /// 回滚本身也可能失败（文件被别的程序开着，正是刚才那一步失败的原因）。那时
    /// 绝不能只说一句"失败了" —— 用户需要知道**哪些东西还停在半路上**，否则他
    /// 在资源管理器里看到的是一个自相矛盾的目录，而且不知道该不该再点一次。
    const auto rollbackAndFail = [&](const QString &reason) {
        QStringList stuck;
        if (jsonRenamed && !QFile::rename(newJsonPath, oldJsonPath)) {
            stuck.append(QDir::toNativeSeparators(newJsonPath));
        }
        if (jarRenamed && !QFile::rename(newJarPath, oldJarPath)) {
            stuck.append(QDir::toNativeSeparators(newJarPath));
        }
        // 显示名跟着回滚：原来是什么就写回什么，原来没有这个键就删掉它。
        // 稳定 id **留着** —— 改名失败不改变"它是哪个实例"，而 id 正是用来扛这个的。
        QJsonObject restore;
        restore.insert(kDisplayNameField, oldDisplayName.isString()
                                              ? QJsonValue(oldDisplayName.toString())
                                              : QJsonValue(QJsonValue::Null));
        QString metaError;
        if (!updateVersionMeta(oldId, restore, &metaError)) {
            stuck.append(QDir::toNativeSeparators(AppPaths::versionMetaPath(oldId)));
        }

        if (stuck.isEmpty()) {
            return fail(tr("%1\n\n已恢复原有文件，可重新导入。").arg(reason));
        }
        return fail(tr("%1\n\n自动回滚未完成。请检查以下项目：\n%2")
                        .arg(reason, stuck.join(QStringLiteral("\n"))));
    };

    // 只改**真在盘上**的那几个文件。`InstanceFiles::jarName()` 对"主 jar 在
    // 父版本目录里"的加载器实例返回的是**父版本**那个文件名（见它的说明），那个文件本来
    // 就不在 oldDir 里；`jsonName()` 也有"认不出来就退回 `<id>.json`"的那一档。照着一个
    // 不存在的位置去 rename，失败信息会指着一个**从来没存在过**的路径，用户照着去查只会
    // 更糊涂（而且那本来就不该算一次失败 —— 没有自己的 jar 不是错误）。
    if (QFileInfo::exists(oldJarPath)) {
        if (!renameOnto(oldJarPath, newJarPath)) {
            return rollbackAndFail(tr("无法把 %1 改名成 %2（可能有别的程序正开着它）。")
                                       .arg(QDir::toNativeSeparators(oldJarPath),
                                            QDir::toNativeSeparators(newJarPath)));
        }
        jarRenamed = true;
    }

    if (QFileInfo::exists(oldJsonPath)) {
        if (!renameOnto(oldJsonPath, newJsonPath)) {
            return rollbackAndFail(tr("无法把 %1 改名成 %2（可能有别的程序正开着它）。")
                                       .arg(QDir::toNativeSeparators(oldJsonPath),
                                            QDir::toNativeSeparators(newJsonPath)));
        }
        jsonRenamed = true;
    }

    if (!QDir().rename(oldDir, newDir)) {
        return rollbackAndFail(tr("无法把目录 %1 改名成 %2（可能有别的程序正开着它）。")
                                   .arg(QDir::toNativeSeparators(oldDir),
                                        QDir::toNativeSeparators(newDir)));
    }

    return true;
}

bool VersionManager::copyInstance(const QString &sourceId, const QString &destGameFolder,
                                  const QString &newName, QString *errorOut) {
    // 主线程上的调用方走这一份：源目录就在**当前**数据目录底下，现算没问题。
    // 后台线程的调用方必须走 copyInstanceFrom() —— 见 .hpp 里那段（AppPaths 是进程级
    // 可变状态，worker 里现算会和"切换游戏文件夹"撞成数据竞争）。
    return copyInstanceFrom(AppPaths::versionDir(sourceId.trimmed()), sourceId,
                            destGameFolder, newName, errorOut);
}

bool VersionManager::copyInstanceFrom(const QString &sourceDir, const QString &sourceId,
                                      const QString &destGameFolder, const QString &newName,
                                      QString *errorOut) {
    const auto fail = [errorOut](const QString &reason) {
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    };

    const QString id = sourceId.trimmed();
    const QString name = newName.trimmed();
    const QString target = destGameFolder.trimmed();

    // 空的"目标游戏文件夹"必须先挡掉：`QDir(QString()).absoluteFilePath("versions")` 在
    // 当前工作目录下算，而启动器的工作目录不一定是 exe 旁边那一份 —— 那就会往一个谁也
    // 说不清的地方拷一份实例出来。这一条不是形式。
    if (target.isEmpty()) {
        return fail(tr("没有指定要复制到哪个游戏目录。"));
    }
    if (!WriteDirectoryGuard::acquire(sourceDir, errorOut)
        || !DirectorySessionLock::acquire(target, errorOut)) return false;

    // 源 id 必须是"一个普通的目录名"。这一条和 deleteInstance() 开头那道闸**逐字同款**，
    // 而且同样不是形式主义：id 直接拼成一条路径，空串拼出来是 `versions/` **本身**
    // （它当然"存在"），于是空 id 会把**整个 versions 目录**拷成一份副本 —— 一次点击
    // 复制出几十 GB，而且不报任何错。`..` 和带分隔符的名字同理（指到实例目录外面去）。
    if (id.isEmpty() || id == QStringLiteral(".") || id == QStringLiteral("..")
        || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\'))) {
        return fail(tr("「%1」不是合法的实例名，未复制任何文件。").arg(sourceId));
    }

    // ---------- 第 1 步：全部前置检查，磁盘一个字节都不动 ----------
    //
    // 名字走的是"实例目录名"那一套（长度 / 分隔符 / 尾随点 / Windows 保留设备名），
    // 和 renameInstance() 用的是同一份 —— 目标目录名和实例目录名本来就是同一个东西。
    // NOTE: 这里的"重名"由下面那个 QFileInfo::exists() 现问盘，**不能用**
    // InstanceFiles::validateRenameTarget()：它比的是"当前数据目录的 versions/ 底下
    // 有没有同名的"，而这份副本落在**另一个**游戏文件夹里，那一边有没有人叫这个名字
    // 它看不见。sourceId 同名更是完全不冲突（复制到别的文件夹里，名字照旧是常态）。
    const QString nameProblem = InstanceFiles::validateDirName(name);
    if (!nameProblem.isEmpty()) {
        return fail(nameProblem);
    }

    if (!QDir(sourceDir).exists()) {
        return fail(tr("找不到实例目录 %1。").arg(QDir::toNativeSeparators(sourceDir)));
    }

    const QDir root(target);
    if (!root.exists()) {
        return fail(tr("找不到目标目录 %1。").arg(QDir::toNativeSeparators(target)));
    }

    // **目标不能落在源实例目录里面**（含"就是它自己"这一档）。这一条在
    // 这儿是兜底，界面那一层（MainWindow::copyInstanceTo）有一条同款的、话术更长的。
    //
    // 它是这一整条路上唯一会**把自己写爆**的洞，而且不需要用户做任何怪事：挑游戏文件夹
    // 那一步是个普通的目录选择框，一路点进 `versions/我的整合包` 再按确定就行。那一刻
    // versionsDir = `…/versions/我的整合包/versions`、destDir 在它底下，下面两道闸都拦不住
    // （QDir 那条比的是"是不是同一个目录"、exists() 那条比的是"有没有重名"）—— 然后
    // 第 1 步末尾的 mkpath(versionsDir) 会**在源实例目录内部**造出一个 `versions/`，
    // 紧接着 copyTreeWithoutFollowingLinks 先 mkpath(目标) 再列**源**目录（那个函数的
    // 顺序就是这么写的），列出来的东西里包含它刚造出来的那个 `versions/` ⇒ 每层再往深
    // 一层、每层重抄整个实例（mods / saves / assets…），直到路径长度或盘容量撑爆为止。
    if (InstanceFiles::pathIsSameOrInside(sourceDir, target)) {
        return fail(tr("目标路径 %2 与实例「%1」的目录相同，或位于该目录内。请选择其他游戏目录后重试。")
                        .arg(name, QDir::toNativeSeparators(target)));
    }

    const QString versionsDir = root.absoluteFilePath(QStringLiteral("versions"));
    const QString destDir = versionsDir + QLatin1Char('/') + name;
    // QDir 比较是规范化之后比的（大小写按本机文件系统的规矩），所以"挑的就是自己这一份
    // 所在的那个游戏文件夹 + 同一个名字"这条路在这里被挡下来 —— 不然下面那个 exists()
    // 也会挡，但报的是"已经有一个叫 X 的实例了"，而真正的原因是"那就是它自己"。
    if (QDir(destDir) == QDir(sourceDir)) {
        return fail(tr("「%1」就是这一份自己，没有复制。").arg(name));
    }
    if (QFileInfo::exists(destDir)) {
        return fail(tr("目标游戏目录中已存在名为「%1」的实例，请更换名称后重试。").arg(name));
    }
    if (!QDir().mkpath(versionsDir)) {
        return fail(tr("无法创建目录 %1。").arg(QDir::toNativeSeparators(versionsDir)));
    }

    // ---------- 第 2 步：先拷到 versions/ 里的暂存，成功才发布 ----------
    //
    // 不能直接往 `versions/<新名字>` 里拷：大整合包在慢盘上要拷几分钟，这期间用户（或者
    // 另一份启动器）可能在那个名字下放了东西；拷到一半失败时再把整个 destDir 删掉，就会
    // 把不是我们写的东西一起删了。暂存放成 `versions/.wtfml-staging/<uuid>`：
    // · 和 destDir 在同一卷（`versions/` 自己可能是 junction，挂到另一块盘上，所以
    //   暂存不能放 cache/），发布是原子改名；
    // · 那一层没有顶层 jar / json，`installedVersions()` 不会把它当成实例列出来。
    // 失败只清暂存；destDir 从头到尾只由最后一次改名创建。
    const QString stagingParent = versionsDir + QStringLiteral("/.wtfml-staging");
    const QFileInfo stagingParentInfo(stagingParent);
    if (stagingParentInfo.isJunction() || stagingParentInfo.isSymLink()) {
        return fail(tr("暂存目录 %1 是一根链接，没有往那里拷副本。")
                        .arg(QDir::toNativeSeparators(stagingParent)));
    }
    if (!QDir().mkpath(stagingParent)) {
        return fail(tr("无法创建暂存目录 %1。").arg(QDir::toNativeSeparators(stagingParent)));
    }
    const QString stagingDir = stagingParent + QLatin1Char('/') + newInstanceId();
    // 删的只能是**本次自己刚建出来的暂存**（见 .hpp 里那条 NOTE：为什么和整合包导入那条
    // 「失败不删半成品」是相反的）。目标位置一个字节都没动过。删不掉也照实说 ——
    // 那是一句"你得自己去删掉它"，不能算复制成功。
    const auto removePartial = [&]() -> QString {
        if (!removeTreeWithoutFollowingLinks(stagingDir)) {
            return tr("\n（未能删除未完成复制的暂存 %1，请手动删除。）")
                .arg(QDir::toNativeSeparators(stagingDir));
        }
        QDir().rmdir(stagingParent);  // 空了就一起收掉；还有别的拷贝在用时失败也无妨
        return QString();
    };

    QString copyProblem;
    if (!copyTreeWithoutFollowingLinks(sourceDir, stagingDir, {}, &copyProblem)) {
        return fail(tr("复制 %1 的时候中断了，这一份没有做出来。\n原因：%2%3")
                        .arg(QDir::toNativeSeparators(sourceDir), copyProblem, removePartial()));
    }

    // ---------- 第 3 步：jar / json 跟着目录名改 ----------
    //
    // 版本 json 和主 jar 的名字必须和**目录名**一致（InstanceFiles::resolve() 那三条
    // 判据里的第 ② 条）：光拷不改，那一份在列表里是认不出来的 —— 用户在目标文件夹里
    // 看到的是一个"不像实例的目录"，而启动器那边连列都不列它。
    //
    // NOTE: 用 renameOnto() 而不是 QFile::rename：只差大小写的那种"目标已存在"其实是
    // 同一个文件（Windows 的路径比较本来就不区分大小写），那种改名是合法的。
    // 这里的 renameOnto 落到的是**刚拷出来的那一份**里，不是源。
    const auto moveInCopy = [&](const QString &suffix, const QString &oldName) -> bool {
        const QString from = stagingDir + QLatin1Char('/') + oldName;
        const QString to = stagingDir + QLatin1Char('/') + name + suffix;
        if (from.compare(to, Qt::CaseSensitive) == 0 || !QFile::exists(from)) {
            // 名字一样（副本叫原来的名）或者这一份本来就没有那个文件（比如只有 json、
            // 没有 jar 的半成品）—— 都不是错误，什么都不用做。
            return true;
        }
        return renameOnto(from, to);
    };
    // **源文件名要现扫源目录**，不能拿 `id` 现拼。用户在资源管理器里
    // 把目录改过名的那种实例，jar / json 还叫旧名字（真机上就有：目录 `原版ATT2地图（无模组）`
    // 里躺着 `ATT2原版地图（无模组）.jar`）。按 `id + 后缀` 找的话 from 不存在，这一句
    // 会当"本来就没有"静默跳过 —— 副本里那份 jar 于是还叫旧名，而下面第 4 步的 meta 与
    // 整个副本都按新名成立，只有这一对名字没跟上。
    //
    // NOTE: 不能用 `InstanceFiles::jarName(id)` —— 那个算的是**当前数据目录**下的
    // `versions/<id>`，而这一份的源可能是另一个游戏文件夹（见 `copyInstanceFrom()`
    // 的 sourceDir）。这里要的是"源目录自己那三个清单"，所以只借纯函数 `resolve()`。
    InstanceFiles::DirListing sourceListing;
    sourceListing.dirName = id;
    {
        const QDir source(sourceDir);
        sourceListing.jarNames = source.entryList(QStringList() << QStringLiteral("*.jar"), QDir::Files);
        sourceListing.jsonNames = source.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files);
    }
    const InstanceFiles::Resolved sourceNames = InstanceFiles::resolve(sourceListing);
    const QString sourceJar = sourceNames.jarName.isEmpty() ? id + QStringLiteral(".jar")
                                                            : sourceNames.jarName;
    const QString sourceJson = sourceNames.jsonName.isEmpty() ? id + QStringLiteral(".json")
                                                              : sourceNames.jsonName;
    if (!moveInCopy(QStringLiteral(".jar"), sourceJar)
        || !moveInCopy(QStringLiteral(".json"), sourceJson)) {
        return fail(tr("实例已复制，但未能重命名副本中的 JAR 或 JSON，副本无法使用。%1")
                        .arg(removePartial()));
    }

    // ---------- 第 4 步：改这一份自己的 wtfml.meta ----------
    //
    // 两件事，缺一不可：
    // - `instanceId`（稳定 id）**必须换一个**：它是"从生到死跟着这个实例"的，两份实例
    // 共用一个 id，分组归属、图标这些认人的地方就会把副本当成源（那正是这个字段存在
    // 要挡的事）。
    // - 显示名改成新目录名（和 renameInstance() 里那句同一个写法）。
    //
    // 写在拷贝**之后**：拷过来的那份 meta 里带着源的 id 和旧名字，这里就地覆盖掉。
    // 目标在别的游戏文件夹里，所以这一句不能走 updateVersionMeta()（它算的是**当前**
    // 数据目录下的路径）—— 只有那个文件名从 AppPaths 问，其余自己拼。
    {
        const QString metaPath = stagingDir + QLatin1Char('/') + AppPaths::versionMetaFileName();
        QString metaError;
        if (!InstanceMetaFile::mergeWrite(
                metaPath,
                QJsonObject{{kDisplayNameField, name}, {kInstanceIdField, newInstanceId()}},
                &metaError)) {
            // 读不懂就不合并（reason 由 mergeWrite 给：读不懂 / 版本不认识 / 写不进）。
            // 源目录里那份一个字都没动过，用户回去还看得见它。
            return fail(tr("更新副本中的 %1 失败：%2%3")
                            .arg(AppPaths::versionMetaFileName(), metaError, removePartial()));
        }
    }

    // ---------- 第 5 步：发布 ----------
    // 发布前再问一次盘：慢盘上拷了几分钟，这个名字可能已经被用户或另一份启动器占用了。
    // 那时只清暂存，绝不碰对方那个目录。
    if (QFileInfo::exists(destDir)) {
        return fail(tr("复制完成时，游戏目录中已存在名为「%1」的实例；本次未发布。%2")
                        .arg(name, removePartial()));
    }
    if (!QDir().rename(stagingDir, destDir)) {
        return fail(tr("复制完成，但无法将副本移至 %1。请检查目标是否被其他程序占用。%2")
                        .arg(QDir::toNativeSeparators(destDir), removePartial()));
    }
    QDir().rmdir(stagingParent);  // 空了就一起收掉；还有别的拷贝在用时失败也无妨

    // 新目录要是有缓存（同名的那一份刚刚被读过），那是**另一份**的缓存。
    versionJsons.remove(name);
    mergedVersionJsons.remove(name);
    return true;
}

bool VersionManager::deleteInstance(const QString &versionId, QString *errorOut) {
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), errorOut)) return false;
    const QString id = versionId.trimmed();

    // 这个入参直接拼成一条**要删掉的路径**，所以校验不是形式主义：空串会拼出 versions/
    // 本身，`..` 或者带分隔符的名字会指到那一份 .minecraft 外面去。只认"一个普通的目录名"。
    if (id.isEmpty() || id == QStringLiteral(".") || id == QStringLiteral("..")
        || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\'))) {
        if (errorOut != nullptr) {
            *errorOut = tr("「%1」不是合法的实例名，未删除任何文件。").arg(versionId);
        }
        return false;
    }

    const QString dir = AppPaths::versionDir(id);
    if (!QFileInfo::exists(dir)) {
        // 已经不在了 —— 调用方要的结果（这个实例不在了）其实已经达成，但**不当成功**：
        // 用户点的是"删掉它"，而它不在盘上说明列表和磁盘不是同一份东西，说清楚比装作
        // 删成功了好（不然"我明明删了"和"它本来就没有"从此分不开）。
        if (errorOut != nullptr) {
            *errorOut = tr("这个实例的目录已经不在了：%1").arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }

    // 进**回收站**而不是永久删除：删实例是用户最容易手滑的一步，删掉之后得有退路。
    // 不追链接的规矩由 moveToRecycleBin() 接着守（见 FolderManager.hpp）。
    QString recycleProblem;
    if (!moveToRecycleBin(dir, &recycleProblem)) {
        if (errorOut != nullptr) {
            *errorOut = tr("未能完全将 %1 移入回收站：%2\n已移入的部分可从回收站恢复。")
                            .arg(QDir::toNativeSeparators(dir), recycleProblem);
        }
        return false;
    }

    // 内存里那两份缓存也要跟着清。不清的后果很具体：删掉之后**重装同一个版本号**，
    // getVersionJson() 会先看缓存，于是启动用的是上一次那份 json（老的 javaVersion、
    // 老的 classpath）—— 而那一次下载明明写了新的一份在盘上。
    versionJsons.remove(id);
    mergedVersionJsons.remove(id);
    return true;
}

QString VersionManager::jvmArgsField() { return QStringLiteral("jvmArgs"); }

QString VersionManager::iconField() { return QStringLiteral("icon"); }

QString VersionManager::settingsPagesField() { return QStringLiteral("settingsPages"); }

QString VersionManager::versionIcon(const QString &versionId) const {
    QString metaPath;
    const QJsonObject meta = readVersionMeta(versionId, &metaPath);
    if (meta.isEmpty()) {
        return QString();
    }

    const QJsonValue value = meta.value(iconField());
    if (value.isUndefined() || value.isNull()) {
        // 没写过这个键 = 没挑过 = 用默认那张（和 skipJavaVersionWarning 同一套态度：
        // 缺省的那一侧是"什么都没发生"）。
        return QString();
    }
    if (!value.isString()) {
        // 写成数字/对象：按"没挑过"处理，但要说一声 —— 这是用户手写的文件，写错了
        // 不吭声就表现成"明明写了却不生效"。
        qWarning() << "Version metadata field icon is not a string in" << metaPath
                   << "— treated as \"default\"";
        return QString();
    }
    return value.toString();
}

QJsonObject VersionManager::versionMeta(const QString &versionId) const {
    // readVersionMeta() 已经把所有"读不到"的形态（文件不在 / 打不开 / 解析不了）
    // 收口成空对象了，这里不再重复判断 —— 那些分支的措辞和告警都只该有一处。
    return readVersionMeta(versionId);
}

QString VersionManager::skipJavaVersionWarningField() {
    return QStringLiteral("skipJavaVersionWarning");
}

bool VersionManager::skipJavaVersionWarning(const QString &versionId) const {
    QString metaPath;
    const QJsonObject meta = readVersionMeta(versionId, &metaPath);
    if (meta.isEmpty()) {
        return false;
    }

    const QJsonValue value = meta.value(skipJavaVersionWarningField());
    if (value.isUndefined() || value.isNull()) {
        // 没写过这个键 = 没表过态 = 提醒。
        return false;
    }
    if (!value.isBool()) {
        // 写成字符串/数字：按"没表态"处理，但要说一声（见头文件里那条 NOTE）。
        qWarning() << "Version metadata field skipJavaVersionWarning is not a bool in" << metaPath
                   << "— treated as \"warn\"";
        return false;
    }
    return value.toBool();
}

QStringList VersionManager::versionJvmArgs(const QString &versionId) const {
    QString metaPath;
    const QJsonObject meta = readVersionMeta(versionId, &metaPath);
    if (meta.isEmpty()) {
        return QStringList();
    }

    const QJsonValue value = meta.value(jvmArgsField());
    if (value.isUndefined() || value.isNull()) {
        return QStringList();
    }
    // 用数组而不是 PCL 那种空格分隔的字符串：JVM 参数本身可能含空格
    // （`-Dfoo=bar baz` 这种），切成数组无歧义。
    if (!value.isArray()) {
        qWarning() << "Version metadata field jvmArgs is not an array in" << metaPath
                   << "— ignored";
        return QStringList();
    }

    // 元素类型不对就跳过那一个、并说一声 —— 和 readString 对类型错误的态度一致：
    // 这是用户手写的文件，写错了必须告诉他，否则表现成"明明写了却不生效"。
    QStringList args;
    const QJsonArray entries = value.toArray();
    for (const QJsonValue &entry : entries) {
        if (!entry.isString()) {
            qWarning() << "Version metadata jvmArgs has a non-string entry in" << metaPath
                       << "— skipped";
            continue;
        }
        const QString arg = entry.toString();
        if (!arg.isEmpty()) {
            args.append(arg);
        }
    }
    return args;
}

VersionDisplayParts VersionManager::displayParts(const QString &versionId) const {
    // id 永远用参数填，不从文件里取：元数据文件是可以被用户手改的，而 id 是真正
    // 进 --version 和文件路径的那个值，必须由调用方（本地扫描 / 在线清单）决定。
    VersionDisplayParts parts;
    parts.id = versionId;

    QString metaPath;
    const QJsonObject meta = readVersionMeta(versionId, &metaPath);
    if (meta.isEmpty()) {
        return parts;
    }

    // 逐字段读。类型不对时给一句告警：QJsonValue::toString() 对数字/对象会安静地
    // 返回空串，手写 JSON 时把 "name" 写成数字是最容易犯的错，不提示的话表现成
    // "明明写了却不生效"。
    // NOTE: key 收 QString（不是 const char*）是为了让调用方能把 iconField() 这类
    // 字段名函数的结果直接递进来 —— 字面量写两遍正是那几个 Field() 存在的理由。
    const auto readString = [&meta, &metaPath](const QString &key) -> QString {
        const QJsonValue value = meta.value(key);
        if (value.isUndefined() || value.isNull()) {
            return QString();
        }
        if (!value.isString()) {
            qWarning() << "Version metadata field" << key << "is not a string in" << metaPath
                       << "— ignored";
            return QString();
        }
        return value.toString();
    };

    parts.customName = readString(QStringLiteral("name"));
    parts.realVersion = readString(QStringLiteral("realVersion"));
    parts.loaderName = readString(QStringLiteral("loaderName"));
    parts.loaderVersion = readString(QStringLiteral("loaderVersion"));
    // 走 iconField() 而不是再写一遍字面量（理由同上）。
    parts.icon = readString(iconField());

    // NOTE: 这里刻意**不缓存**。getVersionJson() 能缓存是因为版本 JSON 一旦下载就不再变，
    // 而这个文件会被"重命名"功能改写——缓存就会变成"改了名不生效"的 bug。
    // 开销不是不缓存的理由：调用方 refreshInstalledVersions() 每个已安装版本都要过这里
    // 一遍，而它一次会话会被调很多次且**没有上界** —— 构造函数、版本清单到达、下载完成，
    // 以及每装完 / 删掉一个实例都会重跑。
    // 每次每个版本一个 exists() 加（有文件时才有的）几百字节读取；真机上实例数可以到
    // 61 个，也仍然是可忽略的量级 —— 所以别再拿"数量少"当论据，真正的理由是上面那条。
    return parts;
}

void VersionManager::onVersionManifestFetched(const QList<MinecraftVersion> &versions) {
    QStringList versionIds;
    for (const MinecraftVersion &version : versions) {
        versionIds.append(version.id);
    }
    emit versionsFetched(versionIds);

    if (m_fetchJsonAfterManifest.isEmpty()) {
        return;
    }
    const QString pending = m_fetchJsonAfterManifest;
    m_fetchJsonAfterManifest.clear();
    // 列表里没有这个版本（或者列表压根没拉到）时，fetchVersionJson 会立刻回一个
    // 空 JSON，由 onVersionJsonFetched 转成终态信号 —— 请求方总能拿到结果。
    downloadManager->fetchVersionJson(pending);
}

void VersionManager::onVersionJsonFetched(const QJsonObject &versionJson) {
    if (versionJson.contains("id")) {
        QString versionId = versionJson["id"].toString();
        versionJsons[versionId] = versionJson;
        m_pendingVersionId.clear();

        // 加载器安装里"顺便把原版装好"那一步：这次取 json 是**安装流程自己要的**，不是
        // 用户在界面上下载这个版本。不能再发 versionDetailsFetched —— 发出去界面会当成
        // "用户点了下载原版"，于是同一个版本下两遍、两个批次互相打架。这里直接接手。
        if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingParentJson &&
            versionId == m_loaderInstall.gameVersion) {
            // 老 Forge 那条路：这一步的原版 json 是**写我们那份版本 json 的输入**（说明里
            // 那些"没有 url、但原版已经带着"的库要靠它认出来），所以接的是"写 profile"，
            // 不是"下原版的文件"。
            if (m_loaderInstall.legacySelfInstall) {
                m_loaderInstall.phase = LoaderInstall::Phase::WritingLegacyProfile;
                deferLoaderStep(LoaderInstall::Phase::WritingLegacyProfile,
                                [this, versionJson] { writeLegacyProfile(versionJson); });
                return;
            }
            // OptiFine 这一路是"**装之前**先把原版下全"（它的安装器要拿原版的客户端 jar 当
            // 输入），所以它用另一个 phase 记着：那一批结束时该起安装器，而不是收尾。
            // 另外三个都是"装完再补原版"，走 WaitingParentFiles。
            const bool beforeOptifineInstaller = m_loaderInstall.kind == LoaderKind::OptiFine &&
                                                 m_loaderInstall.loaderId.isEmpty();
            const LoaderInstall::Phase parentPhase =
                beforeOptifineInstaller ? LoaderInstall::Phase::WaitingParentBefore
                                        : LoaderInstall::Phase::WaitingParentFiles;
            m_loaderInstall.phase = parentPhase;
            // 这批原版文件是从"刚收到原版那份 json"的这条栈上起的（上面那条信号链一路都是
            // 同步的）：排到下一轮再起。相位上面已经定好了，守卫要认的就是它。
            deferLoaderStep(parentPhase, [this, versionId] {
                downloadVersion(versionId, AppPaths::versionDir(versionId));
            });
            return;
        }

        // 纯原版的整合包那一条：这次取 json 是**导入流程自己要的**，不是用户在下载这个
        // 版本。和上面加载器那段同一个理由 —— 发 versionDetailsFetched 出去，界面会当成
        // "用户点了下载原版"，于是同一个版本下两遍、两个批次互相打架。
        if (m_modpackInstall.active() &&
            m_modpackInstall.phase == ModpackInstall::Phase::WaitingParentJson &&
            versionId == m_modpackInstall.pack.gameVersion) {
            // 纯原版包的父版本就是原版那一份 —— 实例 json 的 inheritsFrom 指它。
            m_modpackInstall.loaderId = versionId;
            m_modpackInstall.phase = ModpackInstall::Phase::WaitingParentFiles;
            deferNextStep([this, versionId] {
                downloadVersion(versionId, AppPaths::versionDir(versionId));
            });
            return;
        }

        // "装一个带名字的实例"里**纯原版**那一条：父版本就是原版自己，这里接手去下它的
        // 文件。理由同上（不能发 versionDetailsFetched）。
        //
        // NOTE: 父版本是**加载器**的那一条永远走不到这里 —— 那整段在 m_loaderInstall 里跑，
        // 上面的加载器分支先认领了（那时 m_namedInstance.parentId 还空着，是装完才填的）。
        if (m_namedInstance.active() &&
            m_namedInstance.phase == NamedInstance::Phase::WaitingParentJson &&
            versionId == m_namedInstance.parentId) {
            m_namedInstance.phase = NamedInstance::Phase::WaitingParentFiles;
            deferNamedStep(NamedInstance::Phase::WaitingParentFiles, [this, versionId] {
                downloadVersion(versionId, AppPaths::versionDir(versionId));
            });
            return;
        }

        emit versionDetailsFetched(versionId, versionJson);
        return;
    }

    // 拿不到版本 JSON（manifest 里没有这个版本，或者网络失败）：这里必须回一个终态。
    // 静默丢弃会让 UI 一直停在"正在获取版本信息"，而按钮已经被禁用 ——
    // 那就成了死界面。
    if (m_pendingVersionId.isEmpty()) {
        qDebug() << "Ignored a version JSON reply that nobody asked for.";
        return;
    }
    const QString pending = m_pendingVersionId;
    m_pendingVersionId.clear();
    qWarning() << "Failed to fetch version JSON for" << pending;

    // 整合包导入在等原版那份 json（纯原版包那一条）：这次失败就是导入失败。
    if (m_modpackInstall.active() &&
        m_modpackInstall.phase == ModpackInstall::Phase::WaitingParentJson &&
        pending == m_modpackInstall.pack.gameVersion) {
        failModpackImport(tr("拿不到原版 %1 的版本信息，整合包没有导入（检查网络后重试）。")
                              .arg(pending));
        return;
    }

    // 安装流程在等原版那份 json：这次失败就是安装失败，而不是"某个人请求的版本没取到"。
    if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingParentJson &&
        pending == m_loaderInstall.gameVersion) {
        failLoaderInstall(tr("拿不到原版 %1 的版本信息，%2 没有安装。").arg(
            pending, m_loaderInstall.loaderName));
        return;
    }

    // 带名字的实例（纯原版那一条）在等原版那份 json：这次失败就是安装失败。
    // 和上面加载器那一段不会撞车：installNamedVanilla 的入口就拒掉了 m_loaderInstall 活着
    // 的情况，两者不会同时是活的。父版本是加载器的那一条也走不到这里 —— 那一条是加载器在等
    // json，由上面那一小段转给 failLoaderInstall()。
    if (m_namedInstance.active() &&
        m_namedInstance.phase == NamedInstance::Phase::WaitingParentJson &&
        pending == m_namedInstance.parentId) {
        failNamedInstance(tr("无法获取原版 %1 的版本信息，安装已取消。请检查网络后重试。")
                              .arg(pending));
        return;
    }

    emit versionDetailsFetched(pending, QJsonObject());
}

// ---------------------------------------------------------------------------
// 批次的开与收："这一批是不是我开的"
// ---------------------------------------------------------------------------
//
// NOTE: 下载器是**共用**的：服务端安装那四段
// （`src/server/ServerInstallController.cpp`）也在同一个 `DownloadManager` 上开批次，
// 而 `allDownloadsFinished` 只有一个接收方（下面那个 `onAllDownloadsFinished`）。
// 没有"这一批是谁的"这道判据的话，一条**外来的**批次结束之后，它也会
// 往下走到最后那一句，照着 `m_currentVersionDownloading`（**上一次**下过的那个版本号，
// 收尾之后故意不清空）报一句「版本 X 的文件已全部就绪」——装服务端下完一个文件，
// 界面上会弹一次"版本下载完成"（真机上还会**挡住用户**：`InlineMessage` 是模态的）。
// `m_failedJobs` 还会是上一批留下的（只有 `beginBatch()` 清它），
// 于是那条外来的批次被报成"有文件未能下载完成"。
//
// 所以判据只能是"开批次的时候我就记住了"。四个包装把 `DownloadManager` 那四个
// 批次动作各收成一处 —— 全仓调用它们的**只有这四个函数**，别处再写一遍就漏记了。

void VersionManager::beginOwnBatch() {
    m_ownBatchOpen = true;
    downloadManager->beginBatch();
}

void VersionManager::endOwnBatch() {
    // NOTE: 这里**不**清 `m_ownBatchOpen`。`endBatch()` 通常一个信号都不发（文件还在飞，
    // `checkAllDownloadsFinished()` 会被 `pendingDownloads` 顶回来），真正那一发是在
    // 最后一个回复回来时才到的 —— 那一位由 `onAllDownloadsFinished()` 自己清。
    downloadManager->endBatch();
}

void VersionManager::abandonOwnBatch() {
    // `abandonBatch()` 把 `m_batchEmitted` 置真 ⇒ 这一批**永远不会有**完成信号了：销号。
    // 不清的话，下一批**别人的**下载结束时会认成自己的（那一批的成败、总数都跟我们无关）。
    m_ownBatchOpen = false;
    downloadManager->abandonBatch();
}

void VersionManager::cancelOwnBatch() {
    // 同上：`cancelBatch()` 也是封口 + `m_batchEmitted` 置真（见它自己那四步）。
    m_ownBatchOpen = false;
    downloadManager->cancelBatch();
}

void VersionManager::onAllDownloadsFinished(bool success) {
    // 这一批**不是我开的** ⇒ 一个字节都别管（来龙去脉见上面那一段 NOTE）。
    if (!m_ownBatchOpen) {
        return;
    }
    m_ownBatchOpen = false;

    const DownloadStats stats = downloadManager->stats();

    // 「GTNH 首启资源预取」那一批：**排在最前**。它不是版本文件、也不是加载器安装，
    // 落到下面任何一条分支里都会报出「版本 X 的文件已全部就绪」这种错话，而且没有
    // “重试那颗按钮”的语义（重试 = 界面重新发起一次预取，见 MainWindow）。
    if (m_assetDirectorPrefetch.active) {
        const AssetDirectorPrefetch prefetch = m_assetDirectorPrefetch;
        m_assetDirectorPrefetch.reset();
        m_failedBatch.reset();
        QString prefetchMessage;
        if (success) {
            if (stats.total > 0) {
                prefetchMessage =
                    tr("首次启动资源预取完成：已下载 %1 个文件（%2 MiB），复用 %3 个文件（其中 %4 个来自启动器共享的 assets/objects）。")
                        .arg(stats.total)
                        .arg(prefetch.bytesToDownload / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(prefetch.reuseOverlay + prefetch.reuseAssets)
                        .arg(prefetch.reuseAssets);
            }
            // stats.total == 0：一个字节都没下、也没有要补的 JSON —— 不用惊动用户。
        } else {
            const QStringList shown = stats.failedPaths.mid(0, 10);
            QString list = shown.join(QLatin1Char('\n'));
            if (stats.failedPaths.size() > shown.size()) {
                list += QLatin1Char('\n')
                        + tr("……还有 %1 个").arg(stats.failedPaths.size() - shown.size());
            }
            prefetchMessage =
                stats.failed > 0
                    ? tr("%1 个文件未能下载；已下载的文件保留在 %2，下次预取会跳过校验通过的部分。游戏首次启动时 AssetDirector 会继续下载。\n%3")
                          .arg(stats.failed)
                          .arg(QDir::toNativeSeparators(
                              AssetDirectorKnownSet::overlayDir(AppPaths::assetsDir())))
                          .arg(list)
                    : tr("预取已中止；已下载的文件保留在 %1，下次预取会跳过校验通过的部分。")
                          .arg(QDir::toNativeSeparators(
                              AssetDirectorKnownSet::overlayDir(AppPaths::assetsDir())));
        }
        qInfo().noquote() << "AssetDirector prefetch finished (" << prefetch.setId
                          << "): downloaded=" << stats.total << " reuseOverlay="
                          << prefetch.reuseOverlay << " reuseAssets=" << prefetch.reuseAssets
                          << " failed=" << stats.failed << " success=" << success;
        emit versionDownloadFinished(prefetch.versionId, success, prefetchMessage);
        return;
    }

    QString message;
    if (success) {
        if (stats.total == 0) {
            // 一个都没下 = 全都好好的（补全那颗按钮点在一个装好的实例上就是这个形状）。
            // 另有 skipped == 0 的情况：这一批压根没有作业登记上来。
            if (stats.reused > 0 && stats.skipped > 0) {
                message = tr("无需下载：%1 个已从本机其他游戏目录复用，%2 个已存在。")
                              .arg(stats.reused)
                              .arg(stats.skipped);
            } else if (stats.reused > 0) {
                message = tr("无需下载：%1 个文件已从本机其他游戏目录复用。")
                              .arg(stats.reused);
            } else {
                message = stats.skipped > 0
                              ? tr("无需下载：%1 个文件均已存在（已跳过）。")
                                    .arg(stats.skipped)
                              : tr("没有需要下载的文件。");
            }
        } else if (stats.reused > 0 && stats.skipped > 0) {
            message = tr("已下载 %1 个文件，另有 %2 个从本机其他游戏目录复用、%3 个已存在。全部就绪。")
                          .arg(stats.total)
                          .arg(stats.reused)
                          .arg(stats.skipped);
        } else if (stats.reused > 0) {
            message = tr("已下载 %1 个文件，另有 %2 个从本机其他游戏目录复用。全部就绪。")
                          .arg(stats.total)
                          .arg(stats.reused);
        } else if (stats.skipped > 0) {
            message = tr("%1 个文件已全部就绪（另有 %2 个已存在，已跳过）。")
                          .arg(stats.total)
                          .arg(stats.skipped);
        } else {
            message = tr("%1 个文件已全部就绪。").arg(stats.total);
        }
    } else {
        // 失败清单只列前 10 项，剩下的折成一行 —— 5000 个 asset 全列出来没人看。
        const QStringList shown = stats.failedPaths.mid(0, 10);
        QString list = shown.join(QLatin1Char('\n'));
        if (stats.failedPaths.size() > shown.size()) {
            list += QLatin1Char('\n') +
                    tr("……还有 %1 个").arg(stats.failedPaths.size() - shown.size());
        }
        message = tr("%1 个文件下载失败：").arg(stats.failed) + QLatin1Char('\n') + list;

        // 用户从那份清单里看不出「盘被拔了」和「网站挂了」的区别：前者接回去再试就好，
        // 后者得换源/等一会儿。失败时探一次卷，盘不在就把这句话补在后面 ——
        // 上面那份清单一个字节都不动，那是这一批的证据。
        // NOTE: 只在失败时探；成功批次多探一次卷没有意义。
        const QString storageProblem =
            StorageAvailability::unavailableReason(AppPaths::minecraftDir());
        if (!storageProblem.isEmpty()) {
            message += QLatin1String("\n\n")
                       + tr("目标磁盘现在不可用：%1\n接回去之后再试。").arg(storageProblem);
        }
    }

    // 「失败任务保存失败文件列表和原始 URL 来源，重试只重做失败项」：在这里把那一份
    // 记下来（DownloadManager 那边刚收完尾，失败清单还是这一批的）。
    //
    // **只有"纯下文件"的批次留**：加载器安装 / 整合包导入 / 带名字的实例那三条路上，
    // 重下几个文件补不回来（加载器的安装器进程还得再跑一次、导入还得解压覆盖），
    // 露一颗点了修不好的「重试」比不露更糟。判据问的就是那三个状态机活没活着。
    if (success) {
        m_failedBatch.reset();
    } else {
        const bool plainFilesOnly = !m_loaderInstall.active() && !m_modpackInstall.active()
                                    && !m_namedInstance.active();
        m_failedBatch.reset();
        if (plainFilesOnly) {
            m_failedBatch.jobs = downloadManager->failedJobs();
            // 一个失败作业都没登记上（比如 `scheduleFile()` 那两道"地址是空的"早退）⇒
            // 没有可重做的，别露那颗按钮。
            m_failedBatch.valid = !m_failedBatch.jobs.isEmpty();
            m_failedBatch.resource = m_resourceDownload.running;
            m_failedBatch.label = m_resourceDownload.label;
            m_failedBatch.id = m_currentVersionDownloading;
        }
    }

    // 加载器安装的几段在这里接续（一次点击 = 好几件事，见 .hpp 里 installLoaderVersion 的
    // NOTE）。这一段**必须排在下面那条信号之前**：用户点的是"安装 Forge"，中间那批原版
    // 文件不该单独报一次"下载完成"（日志里会多一句莫名其妙的原版、按钮也会中途解禁）。
    if (m_loaderInstall.active()) {
        if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingInstallerJar) {
            if (!success) {
                // Forge 有两种 maven 坐标，老的游戏版本只有第二种（实测表见
                // LoaderMeta::forgeInstallerUrlAlternate）。常规那种 404 时换一次再下，
                // 换过还不行才按失败报。
                if (retryInstallerDownloadAlternate()) {
                    return;
                }
                failLoaderInstall(tr("%1 的安装器未能下载，%2 未安装。请检查网络后重试。\n%3")
                                      .arg(m_loaderInstall.loaderName,
                                           m_loaderInstall.gameVersion, message));
                return;
            }
            // 这一批结束了，但事情远没完：安装器那个 Java 进程现在才开始跑。
            // 这一句本来就在 allDownloadsFinished 的发射里，起进程也一并排到下一轮。
            deferLoaderStep(LoaderInstall::Phase::WaitingInstallerJar, [this] { runInstaller(); });
            return;
        }

        // OptiFine 独有的一段：这一批是"装之前先把原版下全"那一次，下完才轮到安装器
        // （它的安装器要拿原版的客户端 jar 当输入）。Forge/NeoForge 那边原版是**最后**
        // 补的，所以它们永远走不到这里。
        if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingParentBefore) {
            if (!success) {
                failLoaderInstall(tr("原版 %1 文件不完整，未安装 %2。请先补齐原版文件再重试。\n%3")
                                      .arg(m_loaderInstall.gameVersion,
                                           m_loaderInstall.loaderName, message));
                return;
            }
            emit loaderInstallMessage(tr("原版 %1 已就绪，正在启动 %2 安装器…")
                                          .arg(m_loaderInstall.gameVersion,
                                               m_loaderInstall.loaderName));
            deferLoaderStep(LoaderInstall::Phase::WaitingParentBefore,
                            [this] { startInstallerDownload(); });
            return;
        }

        if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingParentFiles) {
            if (!success) {
                failLoaderInstall(tr("原版 %1 文件不完整，未安装 %2。请先补齐原版文件再重试。\n%3")
                                      .arg(m_loaderInstall.gameVersion,
                                           m_loaderInstall.loaderName, message));
                return;
            }
            // Fabric 那条路是"加载器自己那份 json 在内存里、库还没下"，所以这里接着下；
            // 老 Forge（legacySelfInstall）这条**也是** —— 那个年代的安装器本来会替我们把那
            // 十几个库摆好，我们没让它跑，所以一样得下（那份 json 和 universal jar 已经摆好了）。
            if (m_loaderInstall.kind == LoaderKind::Fabric
                || m_loaderInstall.kind == LoaderKind::Quilt
                || m_loaderInstall.legacySelfInstall) {
                emit loaderInstallMessage(tr("原版 %1 已就绪，正在安装 %2 文件…")
                                              .arg(m_loaderInstall.gameVersion,
                                                   m_loaderInstall.loaderName));
                // NOTE: 不能在这里直接调 startLoaderFiles()：加载器那个批次会在**上一批的
                // 信号栈里**起来。排到下一轮，让它从干净栈上开始。
                deferLoaderStep(LoaderInstall::Phase::WaitingParentFiles,
                                [this] { startLoaderFiles(); });
                return;
            }
            // Forge / NeoForge / OptiFine：加载器自己那份 json 和库**安装器已经放好了**
            // （OptiFine 那两份是我们从临时目录里搬回来的），这一批只是把原版补齐，补完就
            // 结束了 —— 没有"加载器自己那几个库"要再下一遍。
            // （老 Forge 那条自己装的路走不到这儿，上面那个 legacySelfInstall 已经把它接走了。）
            finishLoaderInstall(true, message);
            return;
        }

        if (m_loaderInstall.phase == LoaderInstall::Phase::WaitingLoaderFiles) {
            finishLoaderInstall(success, message);
            return;
        }
    }

    // 整合包导入的两段批次（和加载器同一个理由：一次点击 = 好几件事，中间那批文件不该
    // 单独报一次"下载完成"）。
    // NOTE: 排在加载器那一段**之后** —— 带加载器的整合包在装加载器时，导入的相位是
    // InstallingLoader，那一段由上面的加载器分支收尾、再由 finishLoaderInstall()
    // 接着往下走。这里管的是另外两段：纯原版包的原版批次、清单里那批文件。
    if (m_modpackInstall.active()) {
        if (m_modpackInstall.phase == ModpackInstall::Phase::WaitingParentFiles) {
            if (!success) {
                failModpackImport(tr("原版 %1 文件不完整，整合包未导入。请先安装原版后重试。\n%2")
                                      .arg(m_modpackInstall.pack.gameVersion, message));
                return;
            }
            emit modpackImportMessage(tr("原版 %1 已就绪，正在下载整合包文件…")
                                          .arg(m_modpackInstall.pack.gameVersion));
            // 这一批是从"上一批下完"的信号栈里起来的：排到下一轮（理由同上）。
            deferNextStep([this] { startModpackFiles(); });
            return;
        }
        if (m_modpackInstall.phase == ModpackInstall::Phase::DownloadingFiles) {
            if (!success) {
                // 半成品目录**刻意留着**（几百 MB 已经下好的文件在里面，删是不逆的）：
                // 失败那句话得说清它在哪、以及**怎么接着下**。
                // NOTE: 不能做成「先把那个目录删掉，再导入一次」：照它做等于把几百 MB
                // 白删、还把唯一能续的点毁了。这一支失败时
                // 实例那份薄 json 已经落盘（`startModpackFiles()` 的第一步就是写它），
                // 所以 `isUnfinishedImport()` 认得出这个目录 ⇒ 同一个包、同一个名字
                // 再导入一次就走 OkResume，已经下好的按 sha1 跳过。
                failModpackImport(
                    tr("整合包中有 %1 个文件未能下载，安装未完成。\n%2\n\n已下载的文件保留在 %3 中；使用同一个整合包、同一个实例名再导入一次即可继续下载（已下载的文件按 sha1 跳过）。")
                        .arg(stats.failed)
                        .arg(message, AppPaths::versionDir(m_modpackInstall.instanceId)));
                return;
            }
            // 解压是同步起 PowerShell 等它（最慢的一段）：排在下一轮干净栈上跑 ——
            // 这一步也是从批次的信号栈里回来的。
            deferNextStep([this] { extractModpackOverrides(); });
            return;
        }
    }

    // 带名字的实例（纯原版那一条）下完了：写实例那份薄 json、发终态。
    // 排在加载器那一段之后 —— 父版本是加载器时这一段压根不参与（那整件事在
    // m_loaderInstall 里跑，收尾在 finishLoaderInstall()）。这里只管纯原版。
    if (m_namedInstance.active() &&
        m_namedInstance.phase == NamedInstance::Phase::WaitingParentFiles) {
        if (!success) {
            failNamedInstance(tr("原版文件不完整，实例安装失败。\n%1").arg(message));
            return;
        }
        // 这一批是从"上一批下完"的信号栈里起来的：排到下一轮（理由同上面几处）。
        const QString parentId = m_namedInstance.parentId;
        deferNamedStep(NamedInstance::Phase::WaitingParentFiles,
                       [this, parentId, message] { finishNamedInstance(parentId, message); });
        return;
    }

    // 下载页那几类资源（模组 / 数据包 / 资源包 / 光影）的那一批：它不属于上面任何一套安装流程，
    // 自己报一次就完事。**必须排在最后那一句之前 return** —— 不然一批模组会被报成
    // 「版本 <那个实例> 的文件已全部就绪」，界面上看是"下完一个模组，实例的安装也完成了"。
    if (m_resourceDownload.running) {
        const QString label = m_resourceDownload.label;
        m_resourceDownload.reset();
        emit resourceDownloadFinished(label, success, message);
        return;
    }

    // 一批只发一次（每个文件的 downloadFinished 都触发的话，
    // 下载一个版本会在日志里刷出几千行一模一样的话）。
    emit versionDownloadFinished(m_currentVersionDownloading, success, message);
}
