/**
 * @file ServerInstallController.cpp
 * @brief 服务端安装那四段真的跑起来。
 *
 * 四段是一条直线的状态机（见头文件那张图）。**每一段都只有一个入口、一个收尾**：
 *
 * ① beginResolve —— 还差的那一份元数据（多数档一个请求都不用发）
 * ② beginDownloads —— 计划里那几份文件，一份一份过 DownloadManager
 * ③ beginInstaller —— 跑安装器（那三档才跑）
 * ④ verifyInstall —— 核对装好了没有（判据是 `ServerRegistry::describe()`）
 *
 * 任何一段栽了都走 `fail()`：发一句人话 + `finished(Failed, 同一句)`。中途被取消走
 * `cancel()`：把在飞的东西掐掉，然后 `finished(Cancelled, 空)`。
 */

#include "ServerInstallController.hpp"
#include <QCoreApplication>
#include <QTimer>
#include "server/ServerWriteGuard.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

#include "../download/DownloadManager.hpp"
#include "../launcher/ProcessPathLimit.hpp"
#include "../net/ControlGet.hpp"
#include "../net/HttpClient.hpp"
#include "../util/AppPaths.hpp"
#include "ServerModel.hpp"
#include "ServerInstallRun.hpp"
#include "ServerRegistry.hpp"
#include "download/DownloadCheckpoint.hpp"

namespace ServerInstallRun {

namespace {

/// 安装器进程最多攒多少输出并进失败那句话里（**不是**日志 —— 日志那边收到的更多）。
constexpr int kInstallerTailChars = 1200;

/// 跑安装器那条路的失败措辞：**没有 java** 和 **java 跑了但退出码不是 0** 是两件不同的事，
/// 两句得分开写（前者用户要去「设置 → 启动 → Java 管理」看一眼，后者要看安装器自己说了什么）。
QString javaMissingMessage() {
    return QCoreApplication::translate("ServerInstallController", "未找到可运行服务端安装器的 Java。请在「设置 → 启动 → Java 管理」中选择兼容版本。");
}

/// 没有回复（排队到点 / 被掐 / 管理器先没）时的失败措辞：`what` 是"取 xxx"，
/// 拼出来是「取 xxx失败：等待网络请求超时。」—— 和以前超时走"网络错误"那条路一个收场。
QString unansweredText(const QString &what, const QString &jobError) {
    return QCoreApplication::translate("ServerInstallController", "%1失败：%2").arg(what, jobError);
}

}  // namespace

ServerInstallController::ServerInstallController(QObject *parent) : QObject(parent) {
    // 自己的 network manager（理由见头文件那段 NOTE）：元数据那几次 GET 是"问一句就完事"的，
    // 混进下载器那张回复表里会让两边的收尾互相算错。
    m_network = new QNetworkAccessManager(this);
    m_installerWatchdog = new QTimer(this);
    m_installerWatchdog->setInterval(1000);
    connect(m_installerWatchdog, &QTimer::timeout, this, [this] {
        if (!m_running || m_cancelling || !m_process || m_process->state() == QProcess::NotRunning) return;
        if (m_installerTotal.elapsed() >= m_installerTotalMs) {
            fail(QCoreApplication::translate("ServerInstallController", "服务端安装超过 %1 秒，已停止。部分文件已保留，请检查网络后修复。").arg(m_installerTotalMs / 1000)); return;
        }
        if (m_installerProgress.elapsed() < m_installerSilentMs || m_installerRestartPending) return;
        if (m_installerRetries == 0) {
            ++m_installerRetries; m_installerRestartPending = true;
            emit message(QCoreApplication::translate("ServerInstallController", "服务端安装器连续 %1 秒无输出，可能卡在内部下载；保留文件并重启安装器一次。").arg(m_installerSilentMs / 1000));
            m_process->kill();
        } else fail(QCoreApplication::translate("ServerInstallController", "重试后，服务端安装器仍连续 %1 秒没有输出。已停止安装，部分文件已保留。\n%2").arg(m_installerSilentMs / 1000).arg(m_installerTail));
    });
}

void ServerInstallController::setInstallerWatchdogLimits(int silentMs, int totalMs) {
    if (m_running) return;
    m_installerTotalMs = qBound(1, totalMs, 900000);
    m_installerSilentMs = qBound(1, silentMs, m_installerTotalMs);
    m_installerWatchdog->setInterval(qBound(1, m_installerSilentMs / 4, 1000));
}

// ---------------------------------------------------------------------------
// 开装
// ---------------------------------------------------------------------------

void ServerInstallController::start(const ServerInstall::Request &request,
                                    const QString &serverId, const QString &javaPath,
                                    const ServerInstall::Plan *resolved) {
    if (m_running) {
        emit finished(Outcome::Failed, QCoreApplication::translate("ServerInstallController", "已有服务端正在安装，请结束后再试。"));
        return;
    }
    m_installerRetries = 0; m_installerRestartPending = false; m_installerTotal.invalidate();
    QString directoryProblem;
    if (!ServerWriteGuard::acquire(serverId, &directoryProblem)) {
        emit finished(Outcome::Failed, directoryProblem);
        return;
    }
    if (!ServerInstallRun::flavorSupported(request.flavor)) {
        emit finished(Outcome::Failed, ServerInstallRun::unsupportedReason(request.flavor));
        return;
    }
    if (m_downloads == nullptr) {
        emit finished(Outcome::Failed, QCoreApplication::translate("ServerInstallController", "下载器未初始化，无法安装（程序内部错误）。"));
        return;
    }

    m_plan = resolved ? *resolved : ServerInstall::planFor(request);
    if (!m_plan.ok) {
        emit finished(Outcome::Failed, m_plan.problem);
        return;
    }
    // 先单任务聚合。下载那一层只有一份批次状态（镜像降级 / 按主机守卫 / 看门狗都在
    // 它身上），同时开两批会让两边的收尾互相算错 —— 所以忙的时候**拒掉**，并说清楚。
    if (m_downloads->isBusy()) {
        emit finished(Outcome::Failed,
                      QCoreApplication::translate("ServerInstallController", "已有下载任务正在进行（或刚排入队列），请结束后再安装这个服务端。"));
        return;
    }

    m_request = request;
    m_serverId = serverId;
    m_dir = AppPaths::serverDir(serverId);
    m_javaPath = javaPath;
    m_running = true;
    m_cancelling = false;
    m_batchOwned = false;
    m_downloaded = 0;
    m_failedDownload.clear();
    m_failedDownloadReason.clear();
    m_installerTail.clear();
    m_stepIndex = 0;

    if (!QDir().mkpath(m_dir)) {
        m_running = false;
        emit finished(Outcome::Failed,
                      QCoreApplication::translate("ServerInstallController", "无法创建服务端目录：%1").arg(QDir::toNativeSeparators(m_dir)));
        return;
    }

    emit message(QCoreApplication::translate("ServerInstallController", "开始装服务端「%1」（%2 %3）")
                     .arg(serverId, ServerInstall::flavorName(request.flavor),
                          request.gameVersion));

    // 起手先摆**两步**：眼前这一步 + 最后那一发核对。元数据一回来（多数档是立刻）就换成
    // 完整那几项 —— 分两次发是**故意的**：取元数据那一步在纸面上还不知道要下几个文件。
    const Need need = ServerInstallRun::needFor(m_request, m_plan);
    if (need.ok()) {
        emit stepsChanged(QStringList{need.label, QCoreApplication::translate("ServerInstallController", "核对安装结果")}, 0);
    } else {
        emit stepsChanged(QStringList{QCoreApplication::translate("ServerInstallController", "核对安装结果")}, 0);
    }

    if (resolved) {
        emit stepsChanged(ServerInstallRun::stepNames(m_request, m_plan), 0);
        beginDownloads();
    } else beginResolve();
}

QJsonObject ServerInstallController::downloadContinuation() const {
    if (!m_running || !m_batchOwned) return {};
    QJsonObject originals;
    for (const auto &name : {AppPaths::serverMetaFileName(), QStringLiteral("server.properties"),
             QStringLiteral("eula.txt"), QStringLiteral("ops.json"), QStringLiteral("whitelist.json")}) {
        const auto path = QDir(m_dir).filePath(name);
        const auto fingerprint = DownloadCheckpoint::fingerprint(path);
        if (fingerprint.isEmpty()) return {};
        originals.insert(path, fingerprint);
    }
    QJsonArray files;
    for (const auto &file : m_plan.downloads) files.append(QJsonObject{
        {"name", file.fileName}, {"url", file.url}, {"sha1", file.sha1},
        {"sha256", file.sha256}, {"size", QString::number(file.size)}});
    return {{"kind", "server-install"}, {"root", AppPaths::minecraftDir()}, {"server", m_serverId},
        {"flavor", int(m_request.flavor)}, {"game", m_request.gameVersion},
        {"loaderVersion", m_request.loaderVersion}, {"installerVersion", m_request.installerVersion},
        {"buildId", m_request.buildId}, {"java", m_javaPath}, {"originals", originals}, {"files", files}};
}

bool ServerInstallController::continueDownload(const QJsonObject &intent) {
    if (m_running || !m_downloads || m_downloads->isBusy() || intent.value("kind") != "server-install" ||
        QDir::cleanPath(intent.value("root").toString()) != QDir::cleanPath(AppPaths::minecraftDir())) return false;
    const auto id = intent.value("server").toString();
    QString problem;
    if (!ServerWriteGuard::acquire(id, &problem)) return false;
    const auto originals = intent.value("originals").toObject();
    if (originals.size() != 5) return false;
    for (auto it = originals.begin(); it != originals.end(); ++it)
        if (QFileInfo(it.key()).absolutePath() != QDir(AppPaths::serverDir(id)).absolutePath() ||
            DownloadCheckpoint::fingerprint(it.key()) != it.value().toObject()) return false;
    ServerInstall::Request request;
    const int flavor = intent.value("flavor").toInt(-1);
    bool known = false;
    for (const auto value : ServerInstall::allFlavors()) if (int(value) == flavor) { request.flavor = value; known = true; }
    if (!known || !flavorSupported(request.flavor)) return false;
    request.gameVersion = intent.value("game").toString();
    request.loaderVersion = intent.value("loaderVersion").toString();
    request.installerVersion = intent.value("installerVersion").toString();
    request.buildId = intent.value("buildId").toString();
    auto plan = ServerInstall::planFor(request);
    if (!plan.ok) return false;
    plan.downloads.clear();
    const auto files = intent.value("files").toArray();
    if (files.isEmpty() || files.size() > 16) return false;
    for (const auto &value : files) {
        const auto object = value.toObject();
        ServerInstall::Download file;
        file.fileName = object.value("name").toString(); file.url = object.value("url").toString();
        file.sha1 = object.value("sha1").toString(); file.sha256 = object.value("sha256").toString();
        file.size = object.value("size").toString().toLongLong();
        const QUrl url(file.url);
        if (!InstanceFiles::validatePathSegment(file.fileName).isEmpty() || !url.isValid() ||
            (url.scheme() != "https" && url.scheme() != "http") || !url.userInfo().isEmpty()) return false;
        plan.downloads.append(file);
    }
    start(request, id, intent.value("java").toString(), &plan);
    return true;
}

void ServerInstallController::beginResolve() {
    const Need need = ServerInstallRun::needFor(m_request, m_plan);
    if (!need.ok()) {
        // 计划里地址已经齐了（Purpur / Fabric / Forge / NeoForge / Quilt）。
        const Fill fill = ServerInstallRun::fillDownloads(m_request, m_plan, QByteArray());
        if (!fill.ok) {
            fail(fill.problem);
            return;
        }
        m_plan = fill.plan;
        emit stepsChanged(ServerInstallRun::stepNames(m_request, m_plan), 0);
        beginDownloads();
        return;
    }

    if (need.kind == Metadata::VersionJson) {
        // 原版那一档：**先看本地**（装服务端的人多半装过客户端那个版本，盘上那份 json 里
        // 就有 `downloads.server`）—— 本地有的话这一档一个网络请求都不用发。
        if (m_localVersionJson) {
            const QJsonObject local = m_localVersionJson(m_request.gameVersion);
            if (!local.isEmpty()) {
                onMetadataReady(QJsonDocument(local).toJson(QJsonDocument::Compact), QString());
                return;
            }
        }
        emit message(QCoreApplication::translate("ServerInstallController", "本地没有 %1 的版本信息，去版本清单里问一条。")
                         .arg(m_request.gameVersion));
        fetchVersionJson();
        return;
    }

    // Paper：那份构建列表。
    const QString what = QCoreApplication::translate("ServerInstallController", "取 %1 ").arg(need.url);
    QNetworkRequest request{QUrl(need.url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_metadataReply = RequestBudget::ControlGet::start(
        this, m_network, request, HttpClient::kDefaultTimeoutMs,
        [this, what](QNetworkReply *reply, const QString &jobError) {
            m_metadataReply = nullptr;
            if (reply == nullptr) {
                onMetadataReady(QByteArray(), unansweredText(what, jobError));
                return;
            }
            const QByteArray payload = reply->readAll();
            const QString problem =
                reply->error() == QNetworkReply::NoError
                    ? QString()
                    : QCoreApplication::translate("ServerInstallController", "取 %1 失败了：%2").arg(reply->url().toString(), reply->errorString());
            reply->deleteLater();
            if (m_cancelling) {
                return;  // 取消那条路自己会收尾，这里什么都不做
            }
            onMetadataReady(problem.isEmpty() ? payload : QByteArray(), problem);
        });
}

void ServerInstallController::fetchVersionJson() {
    // 版本清单 -> 这个版本那条 -> 它的 json。两步，但只有"本地没有"这一条冷路上才会走到。
    // 地址问 `DownloadManager::versionManifestUrl()`：它认 `Launcher/VersionManifestUrl`
    // 那条覆盖。原来这里拼 `endpoint(officialVersionManifestUrl())`，覆盖被绕过 ——
    // 没有预置本地版本 json 的测试会静默打真 Mojang/镜像。
    QNetworkRequest request{QUrl(m_downloads->versionManifestUrl())};
    m_metadataReply = RequestBudget::ControlGet::start(
        this, m_network, request, HttpClient::kDefaultTimeoutMs,
        [this](QNetworkReply *reply, const QString &jobError) {
            m_metadataReply = nullptr;
            onVersionManifest(reply, jobError);
        });
}

void ServerInstallController::onVersionManifest(QNetworkReply *reply, const QString &jobError) {
    if (reply == nullptr) {
        if (m_cancelling) {
            return;
        }
        fail(unansweredText(QCoreApplication::translate("ServerInstallController", "取版本清单"), jobError));
        return;
    }
    const QByteArray payload = reply->readAll();
    const QString networkProblem = reply->errorString();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const QUrl attempted = reply->request().url();
    reply->deleteLater();
    if (m_cancelling) {
        return;
    }
    if (!ok) {
        // 清单现在固定镜像优先，故冷安装也必须保留官方兜底。
        // 只识别默认镜像地址，测试覆盖地址失败时不得偷偷访问公网。
        const QString official = DownloadManager::officialVersionManifestUrl();
        if (attempted == QUrl(DownloadManager::mirrorUrl(official))) {
            QNetworkRequest fallback{QUrl(official)};
            m_metadataReply = RequestBudget::ControlGet::start(
                this, m_network, fallback, HttpClient::kDefaultTimeoutMs,
                [this](QNetworkReply *inner, const QString &innerError) {
                    m_metadataReply = nullptr;
                    onVersionManifest(inner, innerError);
                });
            return;
        }
        fail(QCoreApplication::translate("ServerInstallController", "取版本清单失败了：%1").arg(networkProblem));
        return;
    }

    const QJsonArray versions = QJsonDocument::fromJson(payload)
                                    .object()
                                    .value(QStringLiteral("versions"))
                                    .toArray();
    QString officialUrl;
    for (const QJsonValue &value : versions) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("id")).toString() == m_request.gameVersion) {
            officialUrl = entry.value(QStringLiteral("url")).toString();
            break;
        }
    }
    if (officialUrl.isEmpty()) {
        fail(QCoreApplication::translate("ServerInstallController", "版本清单里没有 %1 这个版本。").arg(m_request.gameVersion));
        return;
    }

    // 镜像那一支是按版本号拼的（见 mirrorVersionJsonUrl），走官方那条时才用清单给的那一条。
    const QString url = m_downloads->mirrorPreferred()
                            ? DownloadManager::mirrorVersionJsonUrl(m_request.gameVersion)
                            : officialUrl;
    QNetworkRequest request{QUrl(url)};
    const QString what = QCoreApplication::translate("ServerInstallController", "取 %1 的版本信息 ").arg(m_request.gameVersion);
    m_metadataReply = RequestBudget::ControlGet::start(
        this, m_network, request, HttpClient::kDefaultTimeoutMs,
        [this, what](QNetworkReply *inner, const QString &jobError) {
            m_metadataReply = nullptr;
            if (inner == nullptr) {
                onMetadataReady(QByteArray(), unansweredText(what, jobError));
                return;
            }
            const QByteArray bytes = inner->readAll();
            const QString problem =
                inner->error() == QNetworkReply::NoError
                    ? QString()
                    : QCoreApplication::translate("ServerInstallController", "取 %1 的版本信息失败了：%2")
                          .arg(m_request.gameVersion, inner->errorString());
            inner->deleteLater();
            if (m_cancelling) {
                return;
            }
            onMetadataReady(problem.isEmpty() ? bytes : QByteArray(), problem);
        });
}

void ServerInstallController::onMetadataReady(const QByteArray &payload, const QString &problem) {
    if (m_cancelling) {
        return;
    }
    if (!problem.isEmpty()) {
        fail(problem);
        return;
    }
    const Fill fill = ServerInstallRun::fillDownloads(m_request, m_plan, payload);
    if (!fill.ok) {
        fail(fill.problem);
        return;
    }
    m_plan = fill.plan;

    // 元数据取回来了 ⇒ 这会儿才知道一共要下几个文件 —— 完整的步骤表在这里换上去。
    emit stepsChanged(ServerInstallRun::stepNames(m_request, m_plan), 0);
    // 上一步（取元数据）标记完成。取回来的那条构建信息就在小字里（比如 Paper 的构建号）。
    const Need need = ServerInstallRun::needFor(m_request, m_plan);
    if (need.ok()) {
        emit stepChanged(0, true, fill.detail);
        m_stepIndex = 1;
    }
    beginDownloads();
}

// ---------------------------------------------------------------------------
// 第二段：下文件
// ---------------------------------------------------------------------------

void ServerInstallController::beginDownloads() {
    if (m_cancelling) {
        return;
    }
    if (m_plan.downloads.isEmpty()) {
        // 没有要下的（理论上不会走到：八档里每一档都得下点什么）。
        m_stepIndex = qMax(0, m_stepIndex - 1);
        beginInstaller();
        return;
    }

    // **这里必须再看一眼忙不忙**。`start()` 那道闸问的是"用户点下去的那一刻"，
    // 而中间隔着取元数据那一段网络来回（Paper 要问构建列表、原版本地没装过时要问版本清单）
    // —— 那几秒钟里用户完全可能在下载页上排了一批。`beginBatch()` 碰到开着的批次会
    // **静默把它关掉**（`DownloadManager::beginBatch()` 里那句 qWarning 后面跟的就是
    // `endBatch()`），别人的那一批于是永远等不到 `allDownloadsFinished`：任务中心那一行
    // 停在「进行中」，取消也取消不掉（收尾信号已经不会来了）。
    if (m_downloads->isBusy()) {
        fail(QCoreApplication::translate("ServerInstallController", "检查版本期间已有其他下载开始。请等待下载完成后再安装服务端。"));
        return;
    }

    connect(m_downloads, &DownloadManager::downloadFinished, this,
            &ServerInstallController::onDownloadFinished, Qt::UniqueConnection);
    connect(m_downloads, &DownloadManager::downloadProgress, this,
            [this](const QString &, qint64 received, qint64 total) {
                emit bytesChanged(received, total);
            });

    m_downloadNames.clear();
    // 这一趟**必须自己开一批**。下载器是共用的，那几个计数器
    // （`m_totalJobs` / `m_doneJobs` / `m_failedJobs`）只有 `beginBatch()` 会清 ——
    // 不在这儿开的话，这一趟拿到的是**上一批留下的**那几个数：`stats()` 报出来的总数和
    // 失败数都是别人的（`m_failedJobs` 非零时整批还会被判成"有文件没下下来"）。
    // 顺带把"整批结束"那一发也挪到了这一批自己的收尾上（信号是谁接走的，见
    // `VersionManager::beginOwnBatch()` 那一大段）。
    m_batchOwned = true;
    m_downloadOriginals = downloadContinuation().value("originals").toObject();
    m_downloads->beginBatch();
    for (const ServerInstall::Download &download : m_plan.downloads) {
        const QString path = m_dir + QLatin1Char('/') + download.fileName;
        m_downloadNames.insert(download.fileName);
        emit message(QCoreApplication::translate("ServerInstallController", "下 %1").arg(download.fileName));
        // 校验值：`DownloadManager` 手上只有 sha1 这一栏。上游给 sha256 的那两条（Paper /
        // Purpur）本来也没有 sha1，所以那两条按"比一比大小"过 —— 和资源下载那边同一条口径。
        m_downloads->downloadFile(download.url, path, download.sha1, download.size,
                                  VerifyMode::SizeAndSha1);
    }
    m_downloads->endBatch();
    // NOTE: 起点是 m_stepIndex，下完一个往后挪一格；`onDownloadFinished` 收尾时就是按这个
    // 顺序一格一格报的（步骤表里"取元数据"那一格已经在上一步报过了）。
}

void ServerInstallController::onDownloadFinished(const QString &fileName, bool success,
                                                 const QString &error) {
    if (!m_running || m_cancelling) {
        return;
    }
    // 只认**这一趟真正要下的**那几个名字 —— 下载器是共用的，别人下的文件也会从这儿过。
    if (!m_downloadNames.contains(fileName)) {
        return;
    }
    if (!success && m_failedDownload.isEmpty()) {
        m_failedDownload = fileName;
        m_failedDownloadReason = error;
    }
    ++m_downloaded;
    // 步骤表里那几格"下载 X"是挨着排的，起点是 `m_stepIndex`（取元数据那一格之后）。
    emit stepChanged(m_stepIndex + m_downloaded - 1, success,
                     success ? QString() : QCoreApplication::translate("ServerInstallController", "未下载"));
    if (m_downloaded < m_plan.downloads.size()) {
        return;
    }

    m_batchOwned = false;

    if (!m_failedDownload.isEmpty()) {
        fail(QCoreApplication::translate("ServerInstallController", "%1 未能下载：%2")
                 .arg(m_failedDownload,
                      m_failedDownloadReason.isEmpty() ? QCoreApplication::translate("ServerInstallController", "原因不明")
                                                       : m_failedDownloadReason));
        return;
    }
    m_stepIndex += m_plan.downloads.size();
    beginInstaller();
}

// ---------------------------------------------------------------------------
// 第三段：跑安装器
// ---------------------------------------------------------------------------

void ServerInstallController::beginInstaller() {
    if (m_cancelling) {
        return;
    }
    for (auto it = m_downloadOriginals.begin(); it != m_downloadOriginals.end(); ++it) {
        if (DownloadCheckpoint::fingerprint(it.key()) != it.value().toObject()) {
            fail(QCoreApplication::translate("ServerInstallController", "服务端配置在下载期间已变化，请重新预览；未执行安装器。")); return;
        }
    }
    const QString jarName = ServerInstallRun::installerJarName(m_plan);
    if (jarName.isEmpty()) {
        verifyInstall();  // 那五档下下来就是服务端，不用跑工具
        return;
    }
    if (m_javaPath.isEmpty()) {
        fail(javaMissingMessage());
        return;
    }

    const QString jarPath = m_dir + QLatin1Char('/') + jarName;
    if (!QFileInfo::exists(jarPath)) {
        fail(QCoreApplication::translate("ServerInstallController", "找不到安装器 %1。请重新下载安装器。").arg(jarName));
        return;
    }

    m_process = new QProcess(this);
    if (!m_installerTotal.isValid()) m_installerTotal.start();
    m_installerProgress.start(); m_installerWatchdog->start();
    m_process->setProgram(m_javaPath);
    m_process->setArguments(ServerInstallRun::toolCommand(m_plan, jarPath));
    m_process->setWorkingDirectory(m_dir);
    // 两路合并：安装器把进度写在 stdout、报错常写在 stderr，分开收的话那句话的上下文会缺一半
    // （VersionManager 跑加载器安装器时也是这么合的）。
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &ServerInstallController::onInstallerOutput);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onInstallerFinished(exitCode); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError code) {
        if (m_cancelling) {
            return;
        }
        if (code == QProcess::FailedToStart) {
            fail(QCoreApplication::translate("ServerInstallController", "安装器起不来：%1").arg(m_javaPath) +
                 ProcessPathLimit::tooLongHint({m_javaPath, m_dir}));
        }
    });

    emit message(QCoreApplication::translate("ServerInstallController", "跑安装器：%1 %2")
                     .arg(QFileInfo(m_javaPath).fileName(),
                          ServerInstallRun::toolCommand(m_plan, jarPath).join(QLatin1Char(' '))));
    m_process->start();
}

void ServerInstallController::onInstallerOutput() {
    if (m_process == nullptr) {
        return;
    }
    const QString text = QString::fromLocal8Bit(m_process->readAllStandardOutput());
    if (!text.isEmpty()) m_installerProgress.restart();
    // 日志那一头收到全的；失败时并进那句话里的只留尾巴（整篇塞进去没人读得下去）。
    emit message(text.trimmed());
    m_installerTail.append(text);
    if (m_installerTail.size() > kInstallerTailChars) {
        m_installerTail = m_installerTail.right(kInstallerTailChars);
    }
}

void ServerInstallController::onInstallerFinished(int exitCode) {
    if (!m_running || m_cancelling) {
        return;
    }
    if (m_installerRestartPending) {
        m_installerRestartPending = false;
        m_process->disconnect(this); m_process->deleteLater(); m_process = nullptr;
        beginInstaller(); return;
    }
    m_installerWatchdog->stop();
    if (exitCode != 0) {
        QString problem = QCoreApplication::translate("ServerInstallController", "安装失败。安装器退出码：%1。").arg(exitCode);
        const QString tail = m_installerTail.trimmed();
        if (!tail.isEmpty()) {
            problem += QLatin1Char('\n') + tail.right(600);
        }
        fail(problem);
        return;
    }
    emit stepChanged(m_stepIndex, true, QString());
    m_stepIndex += 1;
    verifyInstall();
}

// ---------------------------------------------------------------------------
// 第四段：核对装好了没有
// ---------------------------------------------------------------------------

void ServerInstallController::verifyInstall() {
    if (m_cancelling) {
        return;
    }
    // 判据**复用列表那一份**（`ServerRegistry::describe()` 里最后那一段）—— 这里要是自己
    // 另判一遍，就会出现"任务中心说装好了、列表那一行还写着未安装"，而两边都没报错。
    const ServerRegistry::Entry entry = ServerRegistry::describe(m_serverId, nullptr);
    if (!entry.installed) {
        fail(QCoreApplication::translate("ServerInstallController", "安装校验失败：%1")
                 .arg(entry.note.isEmpty() ? QCoreApplication::translate("ServerInstallController", "缺的东西说不清") : entry.note));
        return;
    }
    emit stepChanged(m_stepIndex, true, QString());
    emit message(QCoreApplication::translate("ServerInstallController", "「%1」安装完成。").arg(m_serverId));
    finishWith(Outcome::Succeeded, QString());
}

// ---------------------------------------------------------------------------
// 收尾
// ---------------------------------------------------------------------------

void ServerInstallController::fail(const QString &problem) {
    if (m_running) {
        emit message(problem);
    }
    finishWith(Outcome::Failed, problem);
}

void ServerInstallController::finishWith(Outcome outcome, const QString &errorSummary) {
    m_installerWatchdog->stop(); m_installerRestartPending = false;
    if (!m_running) {
        return;  // 已经收过尾了（终态只落一次，和 TaskManager 那边同一条规矩）
    }
    m_running = false;
    m_batchOwned = false;
    if (m_process != nullptr) {
        auto *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process, &QObject::deleteLater);
            process->kill();
            QTimer::singleShot(5000, process, &QObject::deleteLater);
        } else {
            process->deleteLater();
        }
    }
    // 元数据作业一起掐掉：删作业 = 排队的字节不发、在飞的中止、也不回调
    // （取消那条路的收场由 `finishWith()` 自己发 `finished()`）。
    delete m_metadataReply;
    m_metadataReply = nullptr;
    // 附件那几条连线只在这一趟里用（下载器是共用的，留着的话下一趟会接两遍）。
    if (m_downloads != nullptr) {
        disconnect(m_downloads, nullptr, this, nullptr);
    }
    emit finished(outcome, errorSummary);
}

void ServerInstallController::cancel() {
    if (!m_running) {
        return;
    }
    m_cancelling = true;
    emit message(QCoreApplication::translate("ServerInstallController", "已停止。已下载的文件保留在目录中，再点「修复」将继续使用。"));
    // 在飞的回复 / 排着队的作业一起掐掉（`cancelBatch()` 那四个动作见它自己的说明）。
    // 判据是**这一批是不是我们开的**，不是"下载器现在忙不忙"。下载器是共用的：
    // 安装器那一段（可能好几分钟）里就是空闲的，而用户完全可能已经开了一批新的 ——
    // 按 `isBusy()` 掐的话，取消一次服务端安装会顺手把人家那一批也掐掉。
    if (m_downloads != nullptr && m_batchOwned) {
        m_downloads->cancelBatch();
    }
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
    finishWith(Outcome::Cancelled, QString());
}

}  // namespace ServerInstallRun
