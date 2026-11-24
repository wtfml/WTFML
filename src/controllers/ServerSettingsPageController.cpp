/**
 * @file ServerSettingsPageController.cpp
 * @brief 「服务端设置」那一页的四页内容怎么摆、那几颗按钮按下去之后干什么（见 .hpp 那段）。
 *
 * 这一页上摆着四样东西，每一样背后都有一层判据：
 *
 * ① 常用设置 `ServerPropertiesForm`（那十几个旋钮的表与值域）
 * ② 原始文本 页面上那份**原文**（`ServerModel::parseProperties` 解它）
 * ③ 世界备份 `BackupFiles`（它那一套引擎 / 任务账本，和世界页**共用同一条路**）
 * ④ 体检 `ServerCheck`（纯值进、结论出）+ `ServerPortCheck`（真占一次端口）
 *
 * 会写盘的只有两处：`save()` 和那三颗备份按钮 —— 其余全是读。
 */

#include "controllers/ServerSettingsPageController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "ServerSettingsPage.hpp"
#include "controllers/ServerSettingsController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "controllers/ServerRunController.hpp"
#include "manage/BackupFiles.hpp"
#include "server/ServerModel.hpp"
#include "server/ServerPropertiesForm.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRun.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTabWidget>

#include <utility>

ServerSettingsPageController::ServerSettingsPageController(MainWindow *owner)
    : QObject(owner), m_owner(*owner) {}

void ServerSettingsPageController::open(const QString &serverId) {
    OverlayHost *overlay = m_owner.m_ui == nullptr ? nullptr : m_owner.m_ui->overlay();
    if (overlay == nullptr || serverId.isEmpty()) {
        return;
    }
    if (m_page == nullptr) {
        // 父对象是主窗口、不是这一层：这一页在覆盖层里的生命周期和别的那几页一样，
        // 归主窗口管（这一层只是拿着一个不拥有的指针）。
        m_page = new ServerSettingsPage(&m_owner);
        // NOTE: 这一批只接一次（页面复用）。**认人认 `m_serverId`，不认"列表上现在
        // 选中谁"** —— 同控制台那一页那条理由。
        connect(m_page, &ServerSettingsPage::sectionChanged, this,
                [this](int section) { mergeEdits(-1, section); });
        connect(m_page, &ServerSettingsPage::edited, this, [this] { updateSaveState(); });
        connect(m_page, &ServerSettingsPage::saveRequested, this, [this] { save(); });
        connect(m_page, &ServerSettingsPage::startRequested, this, [this] {
            if (!m_page || !m_editable || m_serverId.isEmpty() || m_page->rawEdited() ||
                !m_page->changedOnly().isEmpty()) return;
            m_owner.m_serverRun->start(m_serverId);
        });
        connect(m_page, &ServerSettingsPage::revertRequested, this, [this] { revert(); });
        connect(m_page, &ServerSettingsPage::backupNowRequested, this,
                [this] { m_owner.backupServerWorld(m_serverId); });
        connect(m_page, &ServerSettingsPage::restoreBackupRequested, this,
                [this](const QString &zipPath) {
                    m_owner.restoreServerWorldBackup(m_serverId, zipPath);
                });
        connect(m_page, &ServerSettingsPage::deleteBackupRequested, this,
                [this](const QString &zipPath) { m_owner.deleteServerBackup(zipPath); });
        connect(m_page, &ServerSettingsPage::openBackupFolderRequested, this,
                [this] { m_owner.openBackupFolder(BackupFiles::Kind::World); });
        connect(m_page, &ServerSettingsPage::recheckRequested, this, [this] { runCheck(); });
        connect(m_page, &ServerSettingsPage::actionRequested, this,
                [this](const QString &token) { m_owner.runServerCheckAction(token); });
        // 玩家名单：换一份名单就地重摆；加 / 移除由主窗口那一层问话、写盘。
        connect(m_page, &ServerSettingsPage::playerListChanged, this,
                [this](int) { m_owner.refreshServerPlayers(); });
        connect(m_page, &ServerSettingsPage::playerAddRequested, this,
                [this] { m_owner.addServerPlayer(); });
        connect(m_page, &ServerSettingsPage::playerRemoveRequested, this,
                [this](const QString &identity) { m_owner.removeServerPlayer(identity); });
        // 资源包那一颗：选本地 zip、算 SHA-1。
        connect(m_page, &ServerSettingsPage::resourcePackPickRequested, this,
                [this] { m_owner.pickServerResourcePack(); });
        // 内容那一页：换类就地重摆；加 / 启用禁用 / 打开文件夹 / 刷新由主窗口那一层做。
        connect(m_page, &ServerSettingsPage::contentKindChanged, this,
                [this](int) { m_owner.refreshServerContent(); });
        connect(m_page, &ServerSettingsPage::contentAddRequested, this,
                [this] { m_owner.addServerContent(); });
        connect(m_page, &ServerSettingsPage::contentToggleRequested, this,
                [this](const QString &name, bool enable) {
                    m_owner.toggleServerContent(name, enable);
                });
        connect(m_page, &ServerSettingsPage::contentOpenFolderRequested, this,
                [this] { m_owner.openServerContentFolder(); });
        connect(m_page, &ServerSettingsPage::contentRefreshRequested, this,
                [this] { m_owner.refreshServerContent(); });
        // 「踢出…」：送一条 kick 命令进正在跑的服务端（必须走控制台、回显结果）。
        connect(m_page, &ServerSettingsPage::playerKickRequested, this,
                [this](const QString &name) { m_owner.kickServerPlayer(name); });
        // 收页钩子（见 `OverlayHost::pageClosed()`）：这一页收掉之后"说的是谁"就没意义了
        // （同控制台那一页）；以前靠 `execPage()` 返回之后那一句清，现在开页不等它了。
        connect(overlay, &OverlayHost::pageClosed, this, [this](QWidget *page) {
            if (page == m_page) {
                m_serverId.clear();
            }
        });
    }
    m_serverId = serverId;
    m_page->setSection(ServerSettingsPage::Section::Common);
    refresh();

    // 开页要说清 `ownerTab`、并且**先切过去**（见 `OverlayHost::showPage` 那段）：这一页
    // 是从服务端列表上开出来的，那一位就是"实例"这一颗。
    const int instancesTab = MainWindowUI::instancesTabIndex();
    if (QTabWidget *tabs = m_owner.m_ui->getTabWidget()) {
        if (instancesTab >= 0 && instancesTab < tabs->count()) {
            tabs->setCurrentIndex(instancesTab);
        }
    }
    // 非阻塞开页（见 `OverlayHost::showPage()`）：这一页后面没有一行代码要等它收掉，
    // 收尾（清 `m_serverId`）在上面那条钩子上。
    if (!overlay->showPage(m_page, ServerSettingsPage::windowTitle(), instancesTab)) {
        // 摆不上（这一层刚被别人占上）：这一页说的服务端当场作废，留一句日志。
        m_serverId.clear();
        m_owner.appendLog(MainWindow::tr("无法打开服务器设置页：界面已被另一页占用。"));
    }
}

void ServerSettingsPageController::refreshIfShowing(const QString &serverId) {
    if (isShowing(serverId)) {
        refresh();
    }
}

void ServerSettingsPageController::refreshRunStateIfShowing(const QString &serverId) {
    if (!isShowing(serverId)) {
        return;
    }
    applyRunState(m_page, serverId);
    // 体检单里"能不能启动 / 端口通不通"这两条随在跑变（`checkFacts()` 会看进程）；
    // 保存带那句话也得在 `setEditable()` 之后重算（在跑时它归只读那句管）。
    runCheck();
    updateSaveState();
}

void ServerSettingsPageController::applyRunState(ServerSettingsPage *page,
                                                 const QString &serverId) {
    const ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->process(serverId);
    const bool running = process != nullptr && process->isRunning();
    m_editable = !running;
    // 顶上只写状态本身（"运行中"）；"为什么改不了"由下面那行只读说明讲，不再重复一遍。
    page->setStatusText(ServerModel::statusName(
        process != nullptr ? process->status() : ServerModel::Status::Stopped));
    page->setEditable(m_editable,
                      running ? MainWindowUI::serverSettingsRunningNote() : QString());
}

void ServerSettingsPageController::refresh() {
    ServerSettingsPage *page = m_page;
    const QString serverId = m_serverId;
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    page->setServerName(entry.name.isEmpty() ? serverId : entry.name);

    // 在跑的时候整页只读（见 .hpp 里那段：它退出时会把自己内存里那份写回盘上）。
    applyRunState(page, serverId);

    // ---------- ① 常用设置 + ② 原始文本 ----------
    //
    // 两样**一起**摆：表单里那几格是叠在原文上的补丁，所以"原文"和"基线"必须是同一份
    // 盘上读出来的东西（`setValues()` 自己就把基线重设到刚摆进去的那一份）。
    const QString raw = m_owner.m_serverSettingsController->readRaw(serverId);
    const ServerModel::Properties properties = ServerModel::parseProperties(raw);

    QList<ServerSettingsPage::FieldView> views;
    const QList<ServerPropertiesForm::Field> &fields = ServerPropertiesForm::commonFields();
    for (const ServerPropertiesForm::Field &field : fields) {
        ServerSettingsPage::FieldView view;
        view.key = field.key;
        view.label = field.label;
        view.hint = field.hint;
        switch (field.kind) {
        case ServerPropertiesForm::Kind::Text:
            view.kind = ServerSettingsPage::Kind::Text;
            break;
        case ServerPropertiesForm::Kind::Number:
            view.kind = ServerSettingsPage::Kind::Number;
            break;
        case ServerPropertiesForm::Kind::Switch:
            view.kind = ServerSettingsPage::Kind::Switch;
            break;
        case ServerPropertiesForm::Kind::Choice:
            view.kind = ServerSettingsPage::Kind::Choice;
            break;
        }
        view.min = field.min;
        view.max = field.max;
        // 哪一行要挂"游戏里长什么样"的预览、哪一行旁边摆「选择本地 zip…」，都由字段表
        // 说了算（目前分别是 `motd` 与 `resource-pack-sha1`）。
        view.preview = field.preview;
        view.zipPick = field.zipPick;
        for (const ServerPropertiesForm::Choice &choice : field.choices) {
            view.choiceTokens.append(choice.token);
            view.choiceNames.append(choice.name);
        }
        views.append(view);
    }
    page->setFields(views);
    page->setValues(ServerPropertiesForm::valuesOf(properties, fields));
    page->setRawText(raw);

    // ---------- ③ 世界备份 ----------
    const QString worldName = m_owner.serverWorldName(serverId);
    const QString worldDir = m_owner.serverWorldDir(serverId);
    const QString backupDir = BackupFiles::backupDir(BackupFiles::Kind::World);
    QList<ServerSettingsPage::BackupRow> rows;
    if (!worldName.isEmpty()) {
        const QString prefix =
            BackupFiles::backupFilePrefix(m_owner.m_serverSettingsController->backupLabel(serverId));
        const QFileInfoList entries = QDir(backupDir).entryInfoList(
            QStringList{QStringLiteral("*.zip")}, QDir::Files, QDir::Name | QDir::Reversed);
        QStringList matching;
        for (const QFileInfo &info : entries) {
            if (info.fileName().startsWith(prefix, Qt::CaseInsensitive)) {
                matching.append(info.absoluteFilePath());
            }
        }
        // 不能直接按整名字典序倒排（`QDir::Name | QDir::Reversed`）：自动留的
        // 「恢复前-」安全快照名字里多一段汉字，码位在数字之后 ⇒ 它们全被排到最新的常规
        // 备份前面，这一列"最新在上面"就不成立了。和挑备份那一段同一个出处。
        matching = BackupFiles::sortBackupsNewestFirst(matching, prefix);
        for (const QString &path : std::as_const(matching)) {
            const QFileInfo info(path);
            ServerSettingsPage::BackupRow row;
            row.path = info.absoluteFilePath();
            row.when = info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            row.size = BackupFiles::sizeText(info.size());
            row.safety = BackupFiles::nameIsSafety(info.fileName(), prefix);
            row.scope = row.safety ? MainWindow::tr("还原前的自动快照")
                                   : MainWindow::tr("这个世界的整个文件夹");
            rows.append(row);
        }
    }
    page->setBackupRows(rows);
    page->setBackupNote(
        worldDir.isEmpty()
            ? MainWindow::tr("level-name 无效，无法确定世界目录。请在「常用设置」中填写有效的世界目录名。")
            : MainWindow::tr("世界目录：%1\n备份目录：%2")
                  .arg(QDir::toNativeSeparators(worldDir),
                       QDir::toNativeSeparators(backupDir)));

    // ---------- ④ 体检 ----------
    runCheck();

    // 保存带：刚摆完 = 什么都没改。
    updateSaveState();
}

void ServerSettingsPageController::updateSaveState() {
    ServerSettingsPage *page = m_page;
    if (page == nullptr) {
        return;
    }
    if (!m_editable) {
        // 在跑的时候那句话由 `setEditable()` 那一句管（`refreshSaveBar()` 会拿它盖住）。
        page->setSaveState(false, MainWindowUI::serverSettingsRunningNote());
        return;
    }
    const int changed = page->changedOnly().size();
    const bool raw = page->rawEdited();
    if (changed > 0 && raw) {
        page->setSaveState(true, MainWindowUI::serverSettingsDirtyBothNote(changed));
    } else if (changed > 0) {
        page->setSaveState(true, MainWindowUI::serverSettingsDirtyFieldsNote(changed));
    } else if (raw) {
        page->setSaveState(true, MainWindowUI::serverSettingsDirtyRawNote());
    } else {
        page->setSaveState(false, MainWindowUI::serverSettingsCleanNote());
    }
}

void ServerSettingsPageController::mergeEdits(int fromSection, int toSection) {
    ServerSettingsPage *page = m_page;
    if (page == nullptr || m_serverId.isEmpty()) {
        return;
    }
    Q_UNUSED(fromSection);
    if (toSection == static_cast<int>(ServerSettingsPage::Section::RawText)) {
        // **离开表单、去看原文**：把表单里改过的那几项先合进文本框 —— 用户马上要看的正是
        // "我改完之后这份文件长什么样"，这时候还摆着旧的那一份就是骗人（而且他很可能顺手
        // 就在这一页上接着改，两边不一致会丢改动）。
        //
        // 合的方式是"解析 → 叠补丁 → 吐回文本"——**和保存走的是同一条路**
        // （`save()` 前一半），所以"他在文本框里看到的那一份"和"按保存会写下去的那一份"
        // 是同一份，一个字节都不差。
        const ServerSettingsPage::Values edits = page->changedOnly();
        if (!edits.isEmpty()) {
            page->setRawText(
                m_owner.m_serverSettingsController->mergeIntoRaw(page->rawText(), edits));
            // 合完就算"用户动过文本框"了（内容真的变了、而且这一份是未保存的状态）——
            // `setRawText()` 把那个记号清掉了，所以在这儿补回来。补丁那边**不动基线**：
            // 它还摆在表单上，切回去的时候两边一比还是"改过那几项"。
            page->markRawEdited();
        }
        return;
    }
    if (toSection == static_cast<int>(ServerSettingsPage::Section::Common)) {
        // **回到表单**：按文本框里**现在**那一份重摆表单，并把基线重设到它 —— 用户在文本框
        // 里删了一行、改了一个值，切回来时就该看得见。
        //
        // NOTE: 这一步**只动表单和基线，不写盘**（保存还是那唯一一条写盘的路）。
        page->setValues(m_owner.m_serverSettingsController->valuesFromRaw(page->rawText()));
    }
    // 「世界备份」「体检」两页和这两样都不搭界，切过去什么都不用做。
}

void ServerSettingsPageController::save() {
    ServerSettingsPage *page = m_page;
    const QString serverId = m_serverId;
    if (page == nullptr || serverId.isEmpty() || !m_editable) {
        return;
    }
    const ServerSettingsController::SaveResult result = m_owner.m_serverSettingsController->save(
        serverId, page->rawText(), page->changedOnly());
    if (!result.rejected.isEmpty()) {
        // **整份都不写**（而不是"能写的那几项先写下去"）：写盘是一发原子的 `QSaveFile`，
        // 写一半再报错，用户看着的是"保存了"而盘上是半份 —— 宁可让他把那一格改对再来。
        InlineMessage::warning(
            m_owner.m_ui->overlay(), MainWindow::tr("有几项改不了"),
            MainWindow::tr("以下设置值无效，未保存：\n%1\n\n请在表单或「原始文本」中更正后重新保存。")
                .arg(result.rejected.join(QStringLiteral("\n"))));
        return;
    }
    if (!result.ok) {
        InlineMessage::critical(m_owner.m_ui->overlay(), MainWindow::tr("保存不了"), result.problem);
        return;
    }
    m_owner.appendLog(MainWindow::tr("服务端 %1 的 server.properties 已保存（改了 %2 项）。")
                          .arg(result.serverName)
                          .arg(result.changed));
    // 存完之后把这一页整个按**盘上那份新文件**重摆一遍（原文、表单基线、备份那一列、
    // 体检单都跟着换）—— 然后在那行小字上说明"刚刚存过"。
    refresh();
    page->setSaveState(false, MainWindowUI::serverSettingsSavedNote());
}

void ServerSettingsPageController::revert() {
    ServerSettingsPage *page = m_page;
    if (page == nullptr || m_serverId.isEmpty() || !m_editable) {
        return;
    }
    // 放弃改动 = 把这一页按盘上那一份重新摆一遍（原文、表单基线、保存带全回到开页那一刻）。
    // NOTE: 这一路**不碰盘**，所以什么都不用问。
    refresh();
    page->setSaveState(false, MainWindowUI::serverSettingsCleanNote());
}

ServerCheck::Facts ServerSettingsPageController::checkFacts() const {
    return m_owner.m_serverSettingsController->checkFacts(m_serverId, m_portState);
}

void ServerSettingsPageController::runCheck() {
    ServerSettingsPage *page = m_page;
    if (page == nullptr || m_serverId.isEmpty()) {
        return;
    }
    // 端口那一次真监听**先做**（它是个同步的、几十微秒的动作）——体检单里那一条要的正是
    // 它的结论。
    const ServerModel::Properties properties = ServerRegistry::readProperties(m_serverId);
    const int port = ServerModel::serverPortOf(properties);
    m_portState = port > 0 ? ServerPortCheck::stateOf(port, nullptr)
                           : ServerPortCheck::State::Unknown;

    const ServerCheck::Facts facts = checkFacts();
    const QList<ServerCheck::Finding> findings = ServerCheck::inspect(facts);
    QList<ServerSettingsPage::CheckRow> rows;
    rows.reserve(findings.size());
    for (const ServerCheck::Finding &finding : findings) {
        ServerSettingsPage::CheckRow row;
        // 两边的档位是按下标对齐的（0 要处理 / 1 提醒 / 2 说明）。
        row.severity = static_cast<int>(finding.severity);
        row.title = finding.title;
        row.detail = finding.detail;
        row.actionToken = ServerCheck::actionToken(finding.action);
        rows.append(row);
    }
    page->setCheckRows(rows);
    page->setCheckSummary(ServerCheck::summaryText(findings));

    // ---------- ④ 玩家名单 ----------
    // **摆在最后**：它要读"这一页现在能不能改"（上面 `setEditable()` 那一发算出来的），
    // 而那一句又回头会重摆那两颗按钮的灰亮。
    m_owner.refreshServerPlayers();
    // ---------- ⑤ 内容 ----------
    m_owner.refreshServerContent();
}
