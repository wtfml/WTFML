/**
 * @file MainWindowDropImport.cpp
 * @brief 导入整合包与"拖一个文件进来"：窗口拖放、实例设置页的拖放、以及导入那一串转交。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"用户扔进来一个文件"：`dragEnterEvent()` / `dropEvent()` / `handleWindowDrop()`
 * 是窗口那一层，`handleInstanceSettingsDrop()` 是实例设置页那一层（同一个文件落在不同页上
 * 意思不同），`onImportModpackClicked()` / `importModpackFromFile()` / `applyModpackImport()` /
 * `importDroppedModpack()` 是整合包那一路，`installLocalResourceFile()` 是单个资源文件。
 *
 * 【判据不在这一层】
 *
 * 拖进来的是什么（`.jar` 只看后缀、`.zip` 才读条目表）在 `ModpackImport` 那一层判；
 * 落在哪一页由 `InstanceSettingsDialog` 的页键决定；安装/导入的落盘在 `ImportController`。
 */

#include "MainWindow.hpp"
#include "../ui/SchematicInfoDialog.hpp"
#include "../ui/InlineMessage.hpp"
#include <QFileInfo>

#include "..\ui\InstanceSettingsDialog.hpp"
#include "controllers/ImportController.hpp"
#include "download/ResourceCatalog.hpp"
#include "manage/ModpackImport.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

namespace {
/// 原理图/结构文件：拖进来和命令行带进来都是"打开摘要看一眼"，不装进实例。
bool isSchematicFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.isFile()) return false;
    const QString extension = QLatin1Char('.') + info.suffix().toLower();
    return extension == QLatin1String(".nbt") || extension == QLatin1String(".litematic")
        || extension == QLatin1String(".schematic") || extension == QLatin1String(".schem");
}
}  // namespace

void MainWindow::onImportModpackClicked() {
    m_importController->onImportModpackClicked();
}

void MainWindow::importModpackFromFile(const QString &zipPath, const QString &namePrefill) {
    m_importController->importModpackFromFile(zipPath, namePrefill);
}

void MainWindow::importModpackOneClick(const QString &zipPath,
                                       const QString &confirmedInstanceName,
                                       Tasks::TaskId taskId) {
    m_importController->importModpackOneClick(zipPath, confirmedInstanceName, taskId);
}

bool MainWindow::applyModpackImport(const QString &zipPath, const QString &instanceName,
                                    const QByteArray &manifestJson,
                                    ModpackImport::Format format) {
    return m_importController->applyModpackImport(zipPath, instanceName, manifestJson, format);
}

bool MainWindow::installLocalResourceFile(const QString &versionId,
                                          ResourceCatalog::Kind kind, const QString &path,
                                          QString *problemOut) {
    return m_importController->installLocalResourceFile(versionId, kind, path, problemOut);
}

void MainWindow::handleInstanceSettingsDrop(QWidget *page, const QString &versionId,
                                            const QStringList &paths) {
    m_importController->handleInstanceSettingsDrop(page, versionId, paths);
}

void MainWindow::importDroppedModpack(const QString &zipPath, const QString &namePrefill) {
    m_importController->importDroppedModpack(zipPath, namePrefill);
}

void MainWindow::handleWindowDrop(const QStringList &paths) {
    // 原理图/结构文件是**看一眼**，不是装进某个实例：和命令行带进来那一路同一个入口。
    // 其余几类才交给导入控制器（它要问"装到哪个实例"）。
    QStringList installables;
    for (const QString &path : paths) {
        if (isSchematicFile(path)) {
            openExternalFile(path);
        } else {
            installables.append(path);
        }
    }
    if (!installables.isEmpty()) {
        m_importController->handleWindowDrop(installables);
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    // 自己拖出来的（源就在本进程里）一律不接：拖回启动器窗口会被当成"拖入文件"，
    // 触发导入/安装 —— 看起来就是"没拖出窗口就执行导入"。
    if (event->source() != nullptr) {
        event->ignore();
        return;
    }
    // 准入判据**就是**实例设置那一页用的那一个（同一个函数，见 .hpp 里那段 NOTE）：
    // 拖的是一串本地文件才接。
    if (InstanceSettingsDialog::droppedFilePaths(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    if (event->source() != nullptr) {
        event->ignore();
        return;
    }
    const QStringList paths = InstanceSettingsDialog::droppedFilePaths(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    handleWindowDrop(paths);
}

void MainWindow::openExternalFile(const QString &path) {
    if (isSchematicFile(path)) {
        SchematicInfoDialog dialog(path, this);
        dialog.exec();
        return;
    }
    handleWindowDrop({path});
}






