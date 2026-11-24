#include "controllers/ServerConsoleController.hpp"
#include "TextMarks.hpp"
#include "util/SecretRedactor.hpp"

#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "ServerConsolePage.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "server/ServerProcess.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRun.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>

#include <utility>

ServerConsoleController::ServerConsoleController(ServerWorkspaceController *workspace,
                                                 MainWindowUI *ui, QWidget *pageParent, Host host,
                                                 QObject *parent)
    : QObject(parent), m_workspace(workspace), m_ui(ui), m_pageParent(pageParent),
      m_host(std::move(host)) {}

void ServerConsoleController::open(const QString &serverId) {
    OverlayHost *overlay = m_ui == nullptr ? nullptr : m_ui->overlay();
    if (overlay == nullptr || serverId.isEmpty()) {
        return;
    }
    if (m_page == nullptr) {
        m_page = new ServerConsolePage(m_pageParent);
        m_page->setMaxLines(ServerRun::ServerProcess::maxLogLines());
        // NOTE: 这三条只接一次（页面复用）。**认人认 `m_serverId`，不认"列表上现在
        // 选中谁"** —— 用户完全可能边看控制台边点别处（同 m_updateCenterInstanceId
        // 那一段的理由）。
        connect(m_page, &ServerConsolePage::commandSubmitted, this,
                [this](const QString &command) {
                    ServerRun::ServerProcess *process = m_workspace->process(m_serverId);
                    if (process == nullptr || !process->sendCommand(command)) {
                        if (m_host.log) m_host.log(tr("这一条没有送到服务端（它已经不在跑了）。"));
                    }
                });
        connect(m_page, &ServerConsolePage::stopRequested, this, [this] {
            if (m_host.stop) m_host.stop(m_serverId);
        });
        connect(m_page, &ServerConsolePage::forceKillRequested, this, [this] {
            if (m_host.forceKill) m_host.forceKill(m_serverId);
        });
        connect(m_page, &ServerConsolePage::exportLogRequested, this,
                &ServerConsoleController::exportLog);
        // 收页钩子（见 `OverlayHost::pageClosed()`）：这一页收掉之后"说的是谁"就没意义了
        // —— 留着的话，下一行日志还会往一个已经收起来的页面上写。以前靠 `execPage()`
        // 返回之后那一句清，现在开页不等它了，就在收页这一发上清。
        connect(overlay, &OverlayHost::pageClosed, this, [this](QWidget *page) {
            if (page == m_page) {
                m_serverId.clear();
            }
        });
    }
    m_serverId = serverId;
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    m_page->setServerName(entry.name.isEmpty() ? serverId : entry.name);
    const ServerRun::ServerProcess *process = m_workspace->process(serverId);
    // 开页时把**攒下来的**那些行灌一遍：这一页关着的时候它也一直在跑，重新打开不该
    // 只剩"从打开这一刻起"的日志。
    m_page->setLog(process != nullptr ? process->logLines() : QStringList());
    refresh();  // 状态那几行的唯一出处
    // 开页要说清 `ownerTab`、并且**先切过去**（见 OverlayHost::showPage 那段）：这一页
    // 是从服务端列表上开出来的，那一位就是"实例"这一颗。不切的话，页眉上那颗返回箭头
    // 收页之后用户会落在一个跟刚才没关系的选项卡上。
    const int instancesTab = MainWindowUI::instancesTabIndex();
    if (QTabWidget *tabs = m_ui->getTabWidget()) {
        if (instancesTab >= 0 && instancesTab < tabs->count()) {
            tabs->setCurrentIndex(instancesTab);
        }
    }
    // 非阻塞开页（见 `OverlayHost::showPage()`）：这一页后面没有一行代码要等它收掉，
    // 收尾（清 `m_serverId`）在上面那条钩子上。
    if (!overlay->showPage(m_page, ServerConsolePage::windowTitle(), instancesTab)) {
        // 摆不上（这一层刚被别人占上）：这一页说的服务端当场作废，留一句日志。
        m_serverId.clear();
        if (m_host.log) {
            m_host.log(tr("无法打开服务器控制台：界面已被另一页占用。"));
        }
    }
}

void ServerConsoleController::refresh() {
    if (m_page == nullptr || m_serverId.isEmpty()) {
        return;
    }
    const ServerRun::ServerProcess *process = m_workspace->process(m_serverId);
    const ServerModel::Status status =
        process != nullptr ? process->status() : ServerModel::Status::Stopped;
    const bool running = process != nullptr && process->isRunning();
    const bool stopping = status == ServerModel::Status::Stopping;
    m_page->setStatusText(ServerModel::statusName(status));
    // 「停止」在停止中就不给按了（已经在停了，再点只是催它）；「强制结束」照宽限那一套来。
    m_page->setStopAvailable(running && !stopping);
    m_page->setForceKillAvailable(process != nullptr && process->forceKillAllowed());
    // 没在跑的时候底下那格输入不能用 —— 写进去没人读（那比灰着更容易让人以为坏了）。
    m_page->setCommandAvailable(running);
}

void ServerConsoleController::appendLine(const QString &serverId, const QString &line) {
    if (m_page != nullptr && m_serverId == serverId) {
        m_page->appendLine(line);
    }
}

void ServerConsoleController::announceForceKill(const QString &serverId, int seconds) {
    if (m_page == nullptr || m_serverId != serverId) {
        return;
    }
    // 往控制台里也插一行：用户正盯着这一页看，那颗按钮为什么突然亮了得有个说法。
    // `[wtfml]` 前缀把它和服务端自己吐的那些行分开（它不会吐这个前缀）。
    m_page->appendLine(QStringLiteral("[wtfml] ") + ServerRun::forceKillNote(seconds));
    refresh();
}

void ServerConsoleController::forget(const QString &serverId) {
    if (m_serverId == serverId) {
        m_serverId.clear();
    }
}

void ServerConsoleController::exportLog() {
    if (m_page == nullptr || m_serverId.isEmpty()) {
        return;
    }
    OverlayHost *overlay = m_ui == nullptr ? nullptr : m_ui->overlay();
    const QString text = SecretRedactor::redact(m_page->allLogText());
    if (text.isEmpty()) {
        InlineMessage::warning(overlay, tr("暂无日志"),
                               tr("控制台尚无输出。导出的日志文件将为空。"));
        return;
    }
    const ServerRegistry::Entry entry = ServerRegistry::describe(m_serverId);
    // 文件名里不许出现路径分隔符和 Windows 的那几个非法字符 —— 服务端名是用户自己起的。
    QString safeName = entry.name.isEmpty() ? m_serverId : entry.name;
    safeName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                     QStringLiteral("_"));
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString suggested =
        QDir(desktop.isEmpty() ? QDir::homePath() : desktop)
            .filePath(QStringLiteral("server-%1-%2.log")
                          .arg(safeName, QDateTime::currentDateTime().toString(
                                             QStringLiteral("yyyyMMdd-HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        m_pageParent, tr("导出服务端日志"), suggested,
        tr("文本文件 (*.txt *.log);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    const auto bytes = text.toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        const QString error = file.errorString();
        if (m_host.log) {
            m_host.log(tr("导出服务端日志失败：%1").arg(error));
        }
        InlineMessage::warning(overlay, tr("导出失败"),
                               tr("无法写入文件（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), error));
        return;
    }
    if (m_host.log) {
        m_host.log(TextMarks::done(tr("服务端日志已导出：%1").arg(QDir::toNativeSeparators(path))));
    }
}
