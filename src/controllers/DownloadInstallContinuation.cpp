#include "MainWindow.hpp"
#include "download/DownloadCheckpoint.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/VersionManager.hpp"
#include "util/AppPaths.hpp"
#include "presets/PresetTargetSnapshot.hpp"
#include "server/ServerInstallController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "download/DownloadManager.hpp"
#include "controllers/PresetController.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/ImportController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include <QJsonArray>
#include <QTimer>

QJsonObject MainWindow::snapshotDownloadContinuation() const {
    if (m_serverInstall && m_serverInstall->ownsDownloadBatch()) return m_serverInstall->downloadContinuation();
    const auto pending = m_pendingModpackImports.constFind(m_downloadTaskLedger->resourceLabel());
    if (pending != m_pendingModpackImports.cend() && pending->taskId == m_downloadTaskLedger->task()) {
        return {{"kind", "modpack"}, {"root", pending->gameRoot}, {"instance", pending->instanceName},
                {"archive", QFileInfo(pending->path).absoluteFilePath()}, {"coverCacheId", pending->coverCacheId},
                {"packTitle", pending->packTitle}};
    }
    auto intent = m_versionManager->downloadContinuation();
    if (intent.isEmpty() || intent.value("kind") == "modpack") return intent;
    QJsonArray presets;
    for (const auto &choice : m_downloadCompletion->presets()) presets.append(QJsonObject{{"id", choice.first}, {"force", choice.second}});
    QJsonObject fingerprints;
    const QMap<QString, QString> &storedFingerprints = m_downloadCompletion->fingerprints();
    for (auto it = storedFingerprints.cbegin(); it != storedFingerprints.cend(); ++it) fingerprints.insert(it.key(), it.value());
    intent.insert("presets", presets); intent.insert("presetFingerprints", fingerprints);
    // `presetTargets`（目标实例配置目录那一整棵树的指纹）**不在这里拍**：那一拍要把
    // config/、defaultconfigs/、resourcepacks/、shaderpacks/ 和每个存档都走一遍、每份
    // 整文件算 SHA-256，而这里是"按下安装、第一份文件开始下"的那一刻，同步跑在界面
    // 线程上。改由 `DownloadBatchController` 拿到意图之后在后台算完补进恢复记录
    // （代价量在 `tests/core_modules/PresetSnapshotScaleTests.cpp`）。
    return intent;
}

bool MainWindow::continueDownloadInstallation(Tasks::TaskId id, const QJsonObject &intent, bool targetsVerified) {
    if (intent.value("kind") == "preset-mods") return m_presetController && m_presetController->continueModInstallation(id, intent);
    if (intent.value("kind") == "server-install") {
        if (!m_serverInstall || m_serverInstall->isRunning() || m_downloadInFlight ||
            m_versionManager->downloads()->isBusy()) return false;
        const auto server = intent.value("server").toString();
        const auto process = m_serverWorkspace->process(server);
        if (process && process->isRunning()) return false;
        const auto previous = m_serverInstallTaskId;
        m_serverInstallTaskId = id;
        if (m_serverInstall->continueDownload(intent)) return true;
        m_serverInstallTaskId = previous;
        return false;
    }
    const auto instance = intent.value("instance").toString(), game = intent.value("game").toString();
    const auto kind = intent.value("kind").toString();
    if (m_downloadInFlight || m_versionManager->isDownloading() || m_versionManager->isLoaderInstalling() ||
        m_versionManager->isModpackImporting() || m_versionManager->isNamedInstanceInstalling() ||
        QDir::cleanPath(intent.value("root").toString()) != QDir::cleanPath(AppPaths::minecraftDir()) ||
        !InstanceFiles::validateDirName(instance).isEmpty()) return false;
    if (kind == "modpack") {
        const QString archive = intent.value("archive").toString();
        const QFileInfo info(archive);
        if (!DownloadCheckpoint::safePath(archive) || !info.isFile()) return false;
        if (intent.contains("archiveSize") &&
            (QString::number(info.size()) != intent.value("archiveSize").toString() ||
             QString::number(info.lastModified().toMSecsSinceEpoch()) != intent.value("archiveModifiedMs").toString())) return false;
        if (intent.contains("importMarker") && DownloadCheckpoint::fingerprint(InstanceFiles::importMarkerPath(instance)) !=
            intent.value("importMarker").toObject()) return false;
        auto steps = MainWindowViewData::modpackOneClickSteps();
        steps[0].state = Tasks::TaskStepState::Done;
        steps[1].state = Tasks::TaskStepState::Running;
        m_activityCenter->tasks()->setSteps(id, steps);
        beginFolderSwitchHold();
        const QString coverCacheId = intent.value("coverCacheId").toString();
        const QString packTitle = intent.value("packTitle").toString();
        QTimer::singleShot(0, this, [this, archive, instance, id, coverCacheId, packTitle] {
            m_importController->importModpackOneClick(archive, instance, id, /*allowRename=*/false, coverCacheId,
                                                      packTitle);
            endFolderSwitchHold();
        });
        return true;
    }
    if (kind != "repair" && kind != "named-vanilla" && kind != "named-loader") return false;
    if (kind != "repair" && !InstanceFiles::validateDirName(game).isEmpty()) return false;
    if (!targetsVerified && !Presets::unchangedInitializationContinuation(intent)) return false;
    const int loader = intent.value("loader").toInt(-1);
    if (kind == "named-loader" && (loader < 0 || loader > int(VersionManager::LoaderKind::Quilt) ||
        !VersionManager::loaderInstallSupported(VersionManager::LoaderKind(loader)))) return false;

    QList<QPair<QString, bool>> presets;
    for (const auto &value : intent.value("presets").toArray()) {
        const auto row = value.toObject();
        presets.append({row.value("id").toString(), row.value("force").toBool()});
    }
    QMap<QString, QString> fingerprints;
    const auto storedFingerprints = intent.value("presetFingerprints").toObject();
    for (auto it = storedFingerprints.begin(); it != storedFingerprints.end(); ++it) fingerprints.insert(it.key(), it.value().toString());
    m_downloadCompletion->begin(kind == "repair" ? QString() : instance, presets, fingerprints);
    m_downloadTaskLedger->adopt(id);
    if (kind == "repair") m_installOccupancy.setBatch(InstallOccupancy::Batch::Repair, instance);
    else m_installOccupancy.setBatch(InstallOccupancy::Batch::Install, instance);
    setDownloadUiBusy(true);
    m_downloadTaskLedger->resetProgressThrottle();
    if (kind == "repair") m_versionManager->repairVersionFiles(instance);
    else if (kind == "named-vanilla") m_versionManager->installNamedVanilla(instance, game);
    else m_versionManager->installNamedLoader(instance, game, VersionManager::LoaderKind(loader),
        intent.value("loaderVersion").toString(), intent.value("java").toString());
    return true;
}
