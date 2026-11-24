/**
 * @file MainWindowLaunch.cpp
 * @brief 启动那条路：版本清单到齐、按「启动」、启动前后的那几件、以及主页「最近玩的」那一条。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"这一次要不要启动 / 启动之后窗口怎么收"。`onVersionsFetched()` 把清单摆进
 * 界面；`launchIdentityKey()` / `confirmLaunchWhileRunning()` / `launchArguments()` 是按下
 * 那颗按钮之前的判据；`onStartButtonClicked()` 是唯一入口（真正起进程在
 * `GameSessionController`）；`appendLaunchRecord()` + `updateHomeRecent()` +
 * `openHomeInstancePicker()`（实例切换弹层的数据）是主页上"最近玩的"那一条，`applyLauncherBehaviourAfterLaunch()`
 * 是窗口行为（最小化 / 退出）。分出来之后，"点了启动到底会发生什么"不必在整扇窗里翻。
 *
 * 【判据不在这一层】
 *
 * 参数怎么拼在 `launchArguments()` 与 `launch/QuickPlay`；令牌 / Java 挑选 / 进程在
 * `GameSessionController`；"能不能启动"的占用判据只有 `installingVersionIds()` 一家。
 */

#include "MainWindow.hpp"

#include "..\ui\MainWindowUI.hpp"
#include "..\ui\VersionIcons.hpp"
#include "LoadingState.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/HomeQuickController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "launcher/QuickPlay.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/LaunchHistory.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QFileInfo>

using namespace MainWindowViewData;

void MainWindow::applyLauncherBehaviourAfterLaunch() {
    switch (m_settings->launcherBehaviour()) {
    case LauncherBehaviour::Minimize:
        showMinimized();
        break;
    case LauncherBehaviour::Close:
        // hide() 而不是 close()：close() 是"关窗口"，启动器是最后一个窗口时那等于
        // 退出进程，而游戏是它的子进程 —— 用户以为只是把启动器收起来，游戏跟着没了。
        hide();
        break;
    case LauncherBehaviour::CloseAndRestore:
        m_gameSessionController->noteHiddenForGame();
        hide();
        break;
    case LauncherBehaviour::Unchanged:
        break;
    }
}

void MainWindow::onVersionsFetched(const QStringList &versions) {
    // 下载页「游戏」页头刷新那一行加载态（没挂过就什么都不做）。失败时这一发带的是上一份好清单。
    LoadingState::finish(m_ui->getVersionRefreshStatus());
    refreshAvailableVersions();
    // 下载页那五页的「版本」筛子也灌一遍：它的选项就是这份在线清单（用户要"按版本搜"，
    // 而"有哪些版本"这件事只有这一份是一手来源）。放在**失败也照灌**的位置 —— 空清单时
    // 灌出来是空框，而空框仍然能自己打字（这一格本来就是可输入的）。
    fillDownloadFilterVersionOptions();

    // 顺带把已安装的也刷一遍：一次网络往返之后两个下拉框都是最新的，这条不变式
    // 很便宜（就是一次 QDir 扫描），但能兜住"清单在拉的这段时间用户往
    // .minecraft/versions 里拷了个版本目录"这种情况。
    //
    // 这一趟走后台：扫盘在实例多的机器上要 60–120 ms，正好压在"用户等着下载页"那一段。
    // 结果回来之前界面保持上一次的样子（见 refreshInstalledVersionsAsync()）。
    refreshInstalledVersionsAsync();

    if (versions.isEmpty()) {
        // 拉清单失败时 DownloadManager 也会发这个信号，发的是**上一次成功的结果**
        // （首次拉取就失败才是空的），所以这一支实际拦的是"从来没拉到过"。别让用户
        // 对着一棵永远空的树等下去 —— 日志里说清楚，树上那四条照样在（它们本来就是空的）。
        appendLog(tr("在线版本列表获取失败（网络异常？）。"));
        return;
    }
    appendLog(tr("已更新在线版本列表 (%1 个版本)。").arg(versions.size()));
}

QString MainWindow::launchIdentityKey() const {
    return m_launchController->launchIdentityKey();
}

bool MainWindow::confirmLaunchWhileRunning(const QString &versionId) {
    return m_launchController->confirmLaunchWhileRunning(versionId);
}

QStringList MainWindow::launchArguments(const QString &versionId, const QJsonObject &versionJson,
                                        const EffectiveJavaSettings &javaSettings,
                                        const QString &token, bool offlineMode,
                                        const QuickPlay::Target &quickPlay) const {
    return m_launchController->launchArguments(versionId, versionJson, javaSettings, token,
                                               offlineMode, quickPlay);
}

void MainWindow::onStartButtonClicked() {
    m_launchController->onStartButtonClicked();
}

void MainWindow::appendLaunchRecord(const QString &versionId, const QuickPlay::Target &quickPlay,
                                    qint64 startedAtMs) {
    m_launchController->appendLaunchRecord(versionId, quickPlay, startedAtMs);
}

void MainWindow::updateHomeRecent() {
    if (m_ui == nullptr) {
        return;  // 构造还没走到那儿（和那几个访问器拿空指针当判据是同一个意思）
    }
    QComboBox *combo = m_ui->getInstalledVersionCombo();
    const QString currentId = combo != nullptr ? combo->currentData().toString() : QString();
    QList<HomeQuickController::Instance> installed;
    if (combo != nullptr) {
        installed.reserve(combo->count());
        for (int row = 0; row < combo->count(); ++row) {
            HomeQuickController::Instance instance;
            instance.id = combo->itemData(row).toString();
            if (instance.id.isEmpty()) {
                continue;
            }
            instance.name = combo->itemText(row);
            if (m_versionManager != nullptr) {
                const VersionDisplayParts parts = m_versionManager->displayParts(instance.id);
                QString loaderVersion;
                const QString loader = instanceLoaderName(parts, m_versionManager, &loaderVersion);
                instance.version = QStringLiteral("Minecraft %1 / %2")
                    .arg(parts.realVersion.isEmpty() ? parts.id : parts.realVersion,
                         loader.isEmpty() ? QStringLiteral("原版")
                                          : (loader + QLatin1Char(' ') + loaderVersion).trimmed());
            }
            // 主页封面：用户挑过图标就用挑的那张，没挑就按实例名稳定配色的等距方块（设计稿 cover）。
            const VersionDisplayParts iconParts =
                m_versionManager != nullptr ? m_versionManager->displayParts(instance.id) : VersionDisplayParts{};
            instance.icon = !iconParts.icon.isEmpty()
                ? QIcon(instanceRowMark(iconParts, m_versionManager, 32))
                : QIcon(VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(instance.name), 64));
            instance.favorite = m_groupStore != nullptr && m_groupStore->isPinned(instanceKey(instance.id));
            installed.append(instance);
        }
    }
    const HomeQuickController::Rows rows = HomeQuickController::build(
        installed, m_launchHistory->records(), currentId, m_settings->homeFavoriteCount(),
        m_settings->homeRecentCount());
    m_ui->setHomeQuickRows(rows.current, rows.favorites, rows.recent);
}

QList<MainWindowUI::HomePickerInstance> MainWindow::currentFolderInstancePickerRows(const QString &currentId) const {
    QList<MainWindowUI::HomePickerInstance> instances;
    if (m_folderManager == nullptr || m_versionManager == nullptr) {
        return instances;
    }
    const QString folderPath = m_folderManager->currentFolder();
    const QString title = folderTitle(folderPath);

    // 最近启动的名次（记录新的在前）：弹层「最近」那一段按它排。
    QHash<QString, int> recentRank;
    for (const LaunchHistory::Record &record : m_launchHistory->records()) {
        if (record.isValid() && record.result != LaunchHistory::Result::StartFailed
            && !recentRank.contains(record.instanceId)) {
            recentRank.insert(record.instanceId, int(recentRank.size()));
        }
    }

    instances.reserve(m_instancePageParts.size());
    for (const VersionDisplayParts &part : m_instancePageParts) {
        MainWindowUI::HomePickerInstance row;
        row.folderPath = folderPath;
        row.id = part.id;
        row.title = formatVersionName(part);
        QString loaderVersion;
        const QString loader = instanceLoaderName(part, m_versionManager, &loaderVersion);
        row.loaderKey = loaderGroupKey(loader);
        row.searchKeywords =
            QStringList{part.customName, part.realVersion, loader, loaderVersion, title}.join(QLatin1Char(' '));
        row.subtitle = QStringLiteral("%1 %2")
                           .arg(loader.isEmpty() ? QStringLiteral("原版") : loader,
                                part.realVersion.isEmpty() ? part.id : part.realVersion)
                           .trimmed();
        row.icon = m_instancePageMarks.value(part.id);
        row.favorite = m_groupStore != nullptr && m_groupStore->isPinned(instanceKey(part.id));
        row.recentRank = recentRank.value(part.id, -1);
        row.recent = row.recentRank >= 0;
        row.current = part.id == currentId;
        instances.append(row);
    }
    return instances;
}

void MainWindow::openHomeInstancePicker() {
    if (m_ui == nullptr || m_folderManager == nullptr || m_versionManager == nullptr) {
        return;
    }

    const QString currentFolder = m_folderManager->currentFolder();
    const QString currentId = selectedVersionId(m_ui->getInstalledVersionCombo());
    QList<MainWindowUI::HomePickerFolder> folders;
    QList<MainWindowUI::HomePickerInstance> instances;

    const auto loaderFromId = [](const QString &id) {
        const QString lower = id.toLower();
        // NeoForge 必须排在 Forge 前面，否则名字里那段 "forge" 会先误命中。
        if (lower.contains(QStringLiteral("neoforge"))) {
            return QStringLiteral("neoforge");
        }
        if (lower.contains(QStringLiteral("fabric"))) {
            return QStringLiteral("fabric");
        }
        if (lower.contains(QStringLiteral("optifine"))) {
            return QStringLiteral("optifine");
        }
        if (lower.contains(QStringLiteral("forge"))) {
            return QStringLiteral("forge");
        }
        return QString();
    };

    for (const QString &folderPath : m_folderManager->folders()) {
        MainWindowUI::HomePickerFolder folder;
        folder.path = folderPath;
        folder.title = folderTitle(folderPath);
        folder.subtitle = QDir::toNativeSeparators(folderPath);
        folder.current = (folderPath == currentFolder);
        folders.append(folder);

        if (!QFileInfo(folderPath).isDir()) {
            continue;
        }
        if (folder.current) {
            instances.append(currentFolderInstancePickerRows(currentId));
            continue;
        }

        // 非当前文件夹不能借用 VersionManager（它绑定着当前 AppPaths），这里只做一次轻量目录
        // 枚举；真正选中后 FolderManager 会切根并由 refreshInstalledVersions() 完整校验。
        QDir versionsDir(QDir(folderPath).filePath(QStringLiteral("versions")));
        const QFileInfoList entries = versionsDir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &entry : entries) {
            const QString id = entry.fileName();
            const QDir versionDir(entry.absoluteFilePath());
            if (!QFileInfo::exists(versionDir.filePath(id + QStringLiteral(".json")))
                && !QFileInfo::exists(versionDir.filePath(id + QStringLiteral(".jar")))) {
                continue;
            }
            MainWindowUI::HomePickerInstance row;
            row.folderPath = folderPath;
            row.id = id;
            row.title = id;
            row.loaderKey = loaderFromId(id);
            row.searchKeywords = folder.title;
            row.subtitle = row.loaderKey.isEmpty() ? QStringLiteral("原版") : row.loaderKey;
            row.icon = QIcon(VersionIcons::grassBlock(MainWindowUI::instanceTreeIconSize()));
            instances.append(row);
        }
    }

    m_ui->openHomeInstancePicker(
        folders, instances,
        [this](const QString &folderPath) {
            if (m_folderManager != nullptr) {
                m_folderManager->setCurrentFolder(folderPath);
            }
        },
        [this](const QString &folderPath, const QString &instanceId) {
            if (m_folderManager == nullptr || m_ui == nullptr) {
                return;
            }
            if (folderPath != m_folderManager->currentFolder()
                && !m_folderManager->setCurrentFolder(folderPath)) {
                return;
            }
            QComboBox *combo = m_ui->getInstalledVersionCombo();
            const int row = combo == nullptr ? -1 : combo->findData(instanceId);
            if (row >= 0) {
                combo->setCurrentIndex(row);
            }
        });
}
