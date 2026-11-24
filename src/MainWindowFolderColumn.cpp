/**
 * @file MainWindowFolderColumn.cpp
 * @brief 左侧「游戏文件夹」那一列：选中哪一格、列表怎么刷、格子上写什么、下载页复用哪几个来源。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"现在用的是哪个游戏文件夹"：`onFolderSelectionChanged()` 把选中的那一格
 * 落到 `FolderManager` 并让整扇窗跟着换（路径、实例树、按钮、下拉框），
 * `refreshFolderList()` / `updateFolderButtons()` 画那一列，`folderTitle()` 写格子上那句话，
 * `applyCurrentFolderToPaths()` 把当前文件夹摆进几个"默认目录"，
 * `syncDownloadReuseSources()` 决定下载页能复用哪几个来源。
 *
 * 【判据不在这一层】
 *
 * 文件夹清单与增删在 `FolderManager`；显示名（含"目录名即身份"那个坑）在
 * `folderDisplayNameOf()` 里一条规则说完；默认目录的形状在 `util/AppPaths`。
 */

#include "MainWindow.hpp"

#include "..\ui\MainWindowUI.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/BackupFiles.hpp"
#include "manage/FolderManager.hpp"
#include "manage/VersionManager.hpp"
#include "operations/InstanceUpgrade.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QListWidget>

using namespace MainWindowViewData;

void MainWindow::onFolderSelectionChanged() {
    const QListWidgetItem *item = m_ui->getFolderList()->currentItem();
    if (item == nullptr) {
        return;
    }
    // 同一个目录时 FolderManager 直接返回、不发信号，所以这条链路是幂等的：
    // refreshFolderList() 里挡了信号，重灌不会把用户刚做的选择又跑一遍。
    m_folderManager->setCurrentFolder(item->data(Qt::UserRole).toString());
    updateFolderButtons();
}

/**
 * @brief 一个没起过名字的游戏文件夹在列表**上面那行**默认显示什么。
 * @param path 根目录的绝对路径。
 * @return 目录名（`D:\MC\.minecraft` → `.minecraft`）。取不出目录名时（`D:\` 这种根）
 * 返回整条路径 —— 那一行总得有字，空的会变成一个看不出是什么的空白行。
 *
 * NOTE: 这条路只在**没起过 customname** 时走到（起过名字的显示那个名字）。
 */
static QString folderDisplayNameOf(const QString &path) {
    const QString name = QDir(path).dirName();
    return name.isEmpty() ? path : name;
}

QString MainWindow::folderTitle(const QString &path) const {
    // 三档，从最具体的往最泛的排：用户起的名字 → 自带那份那个固定说法 → 目录名。
    const QString custom = m_folderManager->displayName(path);
    if (!custom.isEmpty()) {
        return custom;
    }
    if (FolderManager::isDefaultFolder(path)) {
        return MainWindowUI::folderAssistantTitle();
    }
    return folderDisplayNameOf(path);
}

void MainWindow::refreshFolderList() {
    const QString current = m_folderManager->currentFolder();
    const QStringList folders = m_folderManager->folders();
    QList<MainWindowUI::FolderRow> rows;
    for (const QString &path : folders) {
        // 隐藏不可用的默认入口；唯一入口与当前使用的目录保留，避免丢失选择上下文。
        const QFileInfo info(path);
        if (FolderManager::isDefaultFolder(path) && path != current && folders.size() > 1 &&
            (!info.isDir() || !info.isReadable())) {
            continue;
        }
        MainWindowUI::FolderRow row;
        row.path = path;
        row.assistant = FolderManager::isDefaultFolder(path);
        row.current = (path == current);
        // 上面那行：用户起的 customname 优先；没起过时是默认那个显示名（见 folderTitle()）。
        row.title = folderTitle(path);
        // 下面那行：完整路径（系统分隔符，别把 QDir 的 `/` 露给用户）。目录不在盘上时
        // 还要缀一句 —— 光看路径看不出"这一份现在扫不出东西"。
        row.subtitle = QDir::toNativeSeparators(path);
        if (!QFileInfo(path).isDir()) {
            row.subtitle += tr("（目录不在了）");
        }
        rows.append(row);
    }
    m_ui->setFolderRows(rows);
    updateFolderButtons();
}

void MainWindow::updateFolderButtons() {
    // **整列**的可用性只有这一处算（列表本体 + 「添加」那颗），两个来源：
    // · `m_downloadInFlight`：下载期间换文件夹会把这一批文件写进另一份 .minecraft，
    //   半批在里面半批在外面，比"这次下不了"难查得多（见 setDownloadUiBusy）。
    // · `m_folderSwitchHold`：非下载类长流程举的牌子（见 beginFolderSwitchHold()）。
    // 以前列表本体是在 setDownloadUiBusy() 里单独 setEnabled 的 —— 那样任何"还没进忙态"
    // 的长流程都留着一个能点的窗口，而且两处各判一次迟早会漏一处。
    const bool locked = m_downloadInFlight || m_folderSwitchHold > 0;
    m_ui->getFolderList()->setEnabled(!locked);
    // NOTE: 「移除」不在这儿管：齿轮菜单里那一项的灰/亮由它自己按"这颗齿轮属于哪一行"
    // 算（见 FolderMenu 里那段 protectedFolder）—— 菜单是照着**哪一行那颗齿轮**弹的，
    // 和"列表当前选中哪一行"可以不是同一行，这是它比按"当前选中行"算准的地方。
    m_ui->getAddFolderButton()->setEnabled(!locked);
}

void MainWindow::beginFolderSwitchHold() {
    ++m_folderSwitchHold;
    updateFolderButtons();
}

void MainWindow::endFolderSwitchHold() {
    // 多出来的 release 直接忽略：那样说明调用方漏了成对，但别把最后一次真牌子也摘掉。
    if (m_folderSwitchHold <= 0) {
        return;
    }
    --m_folderSwitchHold;
    updateFolderButtons();
}

void MainWindow::applyCurrentFolderToPaths() {
    // 全局路径的**唯一**切换点。此后 AppPaths 的每一个函数（versions / assets /
    // libraries / java）都对着这一份 .minecraft，直到下一次切换。
    const QString root = m_folderManager->currentFolder();
    // 自带那份**不设覆盖**（传空串 = 用默认那份）：启动器被整个拷到别处之后，默认路径
    // 跟着 exe 走，而不是钉死在上次记下的那个老位置。
    AppPaths::setMinecraftDir(FolderManager::isDefaultFolder(root) ? QString() : root);
    for (const QString &message : BackupFiles::recoverInterruptedRestores(AppPaths::minecraftDir()))
        appendLog(message);
    // 切到一个“之后才被搬过来”的游戏根时也补一次；启动时已经扫过全部根，这里是幂等的。
    for (const QString &message : InstanceUpgrade::recoverInterruptedInRoots({root}))
        appendLog(message);
    appendLog(tr("当前游戏目录：%1").arg(QDir::toNativeSeparators(root)));
    // 借文件的来源跟着当前文件夹一起变（当前那一份由下载器自己跳过）。
    syncDownloadReuseSources();
}

void MainWindow::syncDownloadReuseSources() {
    if (m_versionManager == nullptr || m_folderManager == nullptr) {
        return;
    }
    // 唯一出处见 FolderManager::reuseSourceFolders()：用户添加的 + 官方启动器那份。
    // 这里**不做**任何"再补几个可能的位置"的加工 —— 用户明确要求不许扫盘。
    m_versionManager->setReuseSourceFolders(m_folderManager->reuseSourceFolders());
}

/**
 * @brief 日志行前面那个"[实例名] "。
 * @param sessionId 哪一份会话；0 或者查不到（会话已经结束了）时返回空串。
 *
 * 同时开着好几份时，一串"[游戏输出] xxx"根本分不出是谁说的 —— 多开这件事本身就把
 * "哪一行是谁"从显然变成了不显然。
 *
 * NOTE: 查不到就**不加前缀**、也不写"(未知)"：那种行是"又开一个被拦下了"这类不挂在
 * 任何实例上的话（sessionId == 0），或者会话刚好在这一行之前结束了。硬安一个名字
 * 比不写更糟。
 */
[[maybe_unused]] static QString sessionLogTag(const LauncherManager::GameSession &game) {
    if (game.sessionId == 0 || game.displayName.isEmpty()) {
        return QString();
    }
    return QStringLiteral("[%1] ").arg(game.displayName);
}
