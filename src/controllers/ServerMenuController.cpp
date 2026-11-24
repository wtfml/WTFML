#include "controllers/ServerMenuController.hpp"

#include "InlineMessage.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "controllers/ServerRunController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "server/ServerModel.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRun.hpp"

#include <QAction>
#include <QMenu>
#include <QTreeWidget>

ServerMenuController::ServerMenuController(MainWindow *owner)
    : QObject(owner), m_owner(*owner) {}

void ServerMenuController::showAt(const QPoint &pos) {
    QTreeWidget *tree = m_owner.m_ui == nullptr ? nullptr : m_owner.m_ui->getServerList();
    if (tree == nullptr) {
        return;
    }
    QTreeWidgetItem *item = tree->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    const QString serverId = item->data(0, Qt::UserRole).toString();
    if (serverId.isEmpty()) {
        return;  // 空白处右键：什么都不弹（这一页没有"对整个列表做的事"）
    }
    tree->setCurrentItem(item);
    showFor(serverId, tree->viewport()->mapToGlobal(pos));
}

void ServerMenuController::showFor(const QString &serverId, const QPoint &globalPos) {
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    const ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->process(serverId);
    const bool running = process != nullptr && process->isRunning();

    QMenu menu(&m_owner);
    for (int i = 0; i < MainWindowUI::serverMenuActionCount(); ++i) {
        const auto action = static_cast<MainWindowUI::ServerMenuAction>(i);
        // 用不上的那几项**不摆**（而不是摆一颗灰的）：
        // 装好了就没有「修复」；没在跑就没有「强制结束」—— 摆一颗永远点不动的项，
        // 比不摆它更容易让人以为界面坏了。
        if (action == MainWindowUI::ServerMenuAction::Repair && entry.installed) {
            continue;
        }
        if (action == MainWindowUI::ServerMenuAction::ForceKill && !running) {
            continue;
        }
        QAction *entryAction = menu.addAction(MainWindowUI::serverMenuText(action));
        if (action == MainWindowUI::ServerMenuAction::RestartOnCrash) {
            // 可勾选：勾没勾读的是盘上那一份（`.wtfml-server` 的 `restartOnCrash`）——
            // 这一项是**长期设置**，不是"这一次会话"的开关。
            entryAction->setCheckable(true);
            entryAction->setChecked(ServerRegistry::readMeta(serverId).restartOnCrash);
            entryAction->setToolTip(MainWindowUI::serverRestartMenuNote());
        }
        if (action == MainWindowUI::ServerMenuAction::ForceKill) {
            // 宽限时间那一条：没到点之前是不给按的（存盘中途被打死正是丢存档的姿势）。
            const bool allowed = process != nullptr && process->forceKillAllowed();
            entryAction->setEnabled(allowed);
            entryAction->setToolTip(allowed
                                        ? ServerRun::forceKillNote(0)
                                        : ServerRun::stoppingNote(
                                              (process != nullptr ? process->gracefulMs() : 0)
                                              / 1000));
        }
        connect(entryAction, &QAction::triggered, this,
                [this, serverId, i] { run(serverId, i); });
    }
    if (menu.isEmpty()) {
        return;
    }
    menu.exec(globalPos);
}

void ServerMenuController::run(const QString &serverId, int action) {
    switch (static_cast<MainWindowUI::ServerMenuAction>(action)) {
    case MainWindowUI::ServerMenuAction::Console:
        m_owner.openServerConsole(serverId);
        break;
    case MainWindowUI::ServerMenuAction::Folder:
        m_owner.openServerFolder(serverId);
        break;
    case MainWindowUI::ServerMenuAction::Settings:
        m_owner.openServerSettingsPage(serverId);
        break;
    case MainWindowUI::ServerMenuAction::Repair:
        m_owner.repairServer(serverId);
        break;
    case MainWindowUI::ServerMenuAction::ForceKill:
        m_owner.m_serverRun->forceKill(serverId);
        break;
    case MainWindowUI::ServerMenuAction::RestartOnCrash: {
        // 勾 / 取消勾：写进 `.wtfml-server`（下一次开菜单读的就是它）。写不进去要说一声 ——
        // 一项"看起来勾上了、其实没记住"的设置比没有这一项更糟。
        //
        // ★ 走 `ServerRegistry::setRestartOnCrash()` 而不是自己 readMeta + writeMeta：
        // 那一份读不懂时 `readMeta()` 回的是**空** `Info`，照它写回去等于用一份最小合法
        // 记录顶掉盘上那份（游戏版本 / 内存 / 端口全没）。判据在 registry 那一层。
        const bool on = !ServerRegistry::readMeta(serverId).restartOnCrash;
        QString problem;
        if (!ServerRegistry::setRestartOnCrash(serverId, on, &problem)) {
            InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("没记住"), problem);
            return;
        }
        if (!on) {
            // 关掉 = 这一轮到此为止（再打开时从第 1 次算起）。
            m_owner.m_serverRun->forgetRestartAttempts(serverId);
        }
        m_owner.appendLog(MainWindow::tr("服务端 %1：异常退出后自动重启%2。")
                              .arg(serverId, on ? MainWindow::tr("已打开")
                                                : MainWindow::tr("已关闭")));
        break;
    }
    case MainWindowUI::ServerMenuAction::Remove:
        m_owner.removeServer(serverId);
        break;
    }
}
