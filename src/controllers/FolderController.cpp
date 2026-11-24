#include "FolderController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/FolderManager.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QUrl>

#include <utility>

FolderController::FolderController(FolderManager *folders, LauncherManager *launcher,
                                   MainWindowUI *ui, QWidget *dialogParent, Host host,
                                   QObject *parent)
    : QObject(parent), m_folders(folders), m_launcher(launcher), m_ui(ui),
      m_dialogParent(dialogParent), m_host(std::move(host)) {}

void FolderController::log(const QString &text) const {
    if (m_host.log) m_host.log(text);
}

void FolderController::add() {
    const QString folder = QFileDialog::getExistingDirectory(
        m_dialogParent, tr("选择 Minecraft 游戏目录"), QDir::homePath());
    if (folder.isEmpty()) return;
    if (!m_folders->addFolder(folder)) {
        InlineMessage::warning(m_ui->overlay(), tr("添加文件夹失败"),
                               tr("「%1」不是一个存在的目录。").arg(folder));
        return;
    }
    log(tr("已添加文件夹：%1").arg(folder));
    m_folders->setCurrentFolder(folder);
}

void FolderController::removeFromList(const QString &folder) {
    if (m_folders->isDefaultFolder(folder)) {
        InlineMessage::information(
            m_ui->overlay(), tr("移除文件夹"),
            tr("「%1」为默认游戏目录，不可移除。")
                .arg(QDir::toNativeSeparators(folder)));
        return;
    }
    QString question = tr("将从启动器列表中移除「%1」。该目录和其中的文件将保留。")
                           .arg(QDir::toNativeSeparators(folder));
    if (m_folders->currentFolder() == folder) {
        question += tr("\n\n移除当前游戏目录后，将使用「%1」。")
                        .arg(QDir::toNativeSeparators(FolderManager::defaultFolder()));
    }
    if (!InlineMessage::question(m_ui->overlay(), tr("从列表里移除文件夹"), question,
                                 tr("是"), tr("否"), true)) return;
    if (!m_folders->removeFolder(folder)) {
        InlineMessage::warning(m_ui->overlay(), tr("移除文件夹"),
                               tr("无法从列表中移除「%1」。")
                                   .arg(QDir::toNativeSeparators(folder)));
        return;
    }
    log(tr("已从列表里移除文件夹：%1").arg(folder));
}

void FolderController::action(const QString &path, int action) {
    switch (action) {
    case MainWindowUI::FolderActionRefresh: refresh(path); return;
    case MainWindowUI::FolderActionRename: rename(path); return;
    case MainWindowUI::FolderActionOpen: open(path); return;
    case MainWindowUI::FolderActionRemove: removeFromList(path); return;
    case MainWindowUI::FolderActionDelete: removeFromDisk(path); return;
    }
    log(tr("未知的游戏目录操作：%1").arg(action));
}

void FolderController::refresh(const QString &path) {
    const bool onDisk = QFileInfo(path).isDir();
    if (m_host.refreshList) m_host.refreshList();
    if (path == m_folders->currentFolder() && m_host.refreshInstances) m_host.refreshInstances();
    log(onDisk ? tr("已刷新游戏目录：%1").arg(QDir::toNativeSeparators(path))
               : tr("游戏目录不存在：%1").arg(QDir::toNativeSeparators(path)));
}

void FolderController::rename(const QString &path) {
    const QString custom = m_folders->displayName(path);
    bool ok = false;
    const QString shown = m_host.title ? m_host.title(path) : QFileInfo(path).fileName();
    const QString input = InlineMessage::getText(
        m_ui->overlay(), tr("重命名游戏目录"),
        tr("当前显示名称为「%1」。仅修改启动器列表中的名称。留空可恢复默认名称。")
            .arg(shown), custom, &ok);
    if (!ok) return;
    const QString name = input.trimmed();
    if (name == custom) return;
    const QString problem = FolderManager::validateDisplayName(name);
    if (!problem.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), tr("重命名游戏目录"), problem);
        return;
    }
    if (!m_folders->setDisplayName(path, name)) {
        InlineMessage::warning(m_ui->overlay(), tr("重命名游戏目录"),
                               tr("「%1」已经不在列表里了。")
                                   .arg(QDir::toNativeSeparators(path)));
        return;
    }
    log(name.isEmpty() ? tr("游戏目录恢复默认名称：%1").arg(QDir::toNativeSeparators(path))
                       : tr("游戏目录已重命名为「%1」：%2").arg(name, QDir::toNativeSeparators(path)));
}

void FolderController::open(const QString &path) {
    if (!QFileInfo(path).isDir()) {
        InlineMessage::warning(m_ui->overlay(), tr("打开游戏目录"),
                               tr("找不到「%1」。请确认目录仍在原位置，且存储设备已连接。")
                                   .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        InlineMessage::warning(m_ui->overlay(), tr("打开游戏目录"),
                               tr("无法打开「%1」。").arg(QDir::toNativeSeparators(path)));
    }
}

void FolderController::removeFromDisk(const QString &path) {
    if (m_folders->isDefaultFolder(path)) {
        InlineMessage::information(
            m_ui->overlay(), tr("删除游戏目录"),
            tr("「%1」为默认游戏目录，不可删除。")
                .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (m_launcher->hasRunningGame()) {
        InlineMessage::warning(m_ui->overlay(), tr("删除游戏目录"),
                               tr("游戏正在运行，请退出游戏后再删除此目录。"));
        return;
    }
    QString question = tr("「%1」及其中的存档、截图、模组等文件将移入回收站，可从回收站恢复。\n\n如需保留磁盘上的文件，请改用「移除」。")
                           .arg(QDir::toNativeSeparators(path));
    if (m_folders->currentFolder() == path) {
        question += tr("\n\n删除当前游戏目录后，将切换到「%1」。")
                        .arg(QDir::toNativeSeparators(FolderManager::defaultFolder()));
    }
    if (!InlineMessage::question(m_ui->overlay(), tr("删除游戏目录"), question,
                                 tr("是"), tr("否"), false, true)) return;
    QString error;
    if (!m_folders->deleteFolder(path, &error)) {
        InlineMessage::critical(m_ui->overlay(), tr("删除游戏目录失败"), error);
        return;
    }
    log(tr("已删除游戏目录：%1").arg(QDir::toNativeSeparators(path)));
}
