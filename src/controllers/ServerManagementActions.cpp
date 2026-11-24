/**
 * @file ServerManagementActions.cpp
 * @brief 服务端设置页的玩家、内容、检查和世界备份操作。
 */

#include "ServerManagementActions.hpp"
#include <QCoreApplication>
#include "MainWindow.hpp"
#include "manage/BackupFiles.hpp"
#include "MainWindowUI.hpp"
#include "ServerSettingsPage.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "controllers/ServerSettingsController.hpp"
#include "controllers/ServerSettingsPageController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "controllers/ServerConsoleController.hpp"
#include "controllers/BackupController.hpp"
#include "manage/FolderManager.hpp"
#include "server/ServerContent.hpp"
#include "server/ServerResourcePack.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRun.hpp"
#include "server/ServerPropertiesForm.hpp"
#include "util/AppPaths.hpp"
#include "server/ServerWriteGuard.hpp"
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

void ServerManagementActions::refreshServerPlayers(MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    const int index = qBound(0, page->playerList(), ServerPlayers::listCount() - 1);
    const auto list = static_cast<ServerPlayers::List>(index);
    // 「踢出…」只对正在跑的服务端有意义（和写名单那两颗正好相反 —— 见页面那段）。
    page->setPlayerKickAvailable(!owner->m_serverSettings->editable());
    const ServerPlayers::Content content =
        owner->m_serverSettingsController->readPlayers(serverId, list);
    QString note = MainWindowUI::serverSettingsPlayerNoteText(index, !owner->m_serverSettings->editable());
    if (!content.ok) {
        // ★ 读不懂就**什么都不摆、也不许写**：那份文件里可能还有用户自己的记录，
        //   拿空名单覆盖它比"这一页现在看不了"糟得多。
        page->setPlayerRows({});
        page->setPlayerNote(content.problem);
        return;
    }
    if (content.entries.isEmpty()) {
        note += MainWindow::tr("\n暂无记录。");
    }
    const QDateTime now = QDateTime::currentDateTime();
    QList<ServerSettingsPage::PlayerRow> rows;
    for (const ServerPlayers::Entry &entry : content.entries) {
        ServerSettingsPage::PlayerRow row;
        row.identity = ServerPlayers::identityOf(list, entry);
        if (list == ServerPlayers::List::BannedIps) {
            row.name = entry.ip;
            row.detail = QString();
            // 封禁 IP 那一档没有"玩家名"可踢 —— `kickName` 留空，那颗按钮就是灰的。
        } else {
            row.name = entry.name.isEmpty() ? entry.uuid : entry.name;
            row.detail = entry.uuid;
            row.kickName = entry.name;
        }
        QStringList notes;
        if (list == ServerPlayers::List::Ops) {
            notes << MainWindow::tr("等级 %1").arg(entry.level);
            if (entry.bypassesPlayerLimit) notes << MainWindow::tr("可以绕过人数上限");
        } else if (list == ServerPlayers::List::BannedPlayers
                   || list == ServerPlayers::List::BannedIps) {
            notes << ServerPlayers::expiryText(entry, now);
            if (!entry.reason.isEmpty()) notes << entry.reason;
            row.expired = ServerPlayers::isExpired(entry, now);
        }
        row.note = notes.join(QStringLiteral(" / "));
        rows.append(row);
    }
    page->setPlayerRows(rows);
    page->setPlayerNote(note);
}

void ServerManagementActions::addServerPlayer(MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    if (!owner->m_serverSettings->editable()) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("现在改不了"),
            MainWindowUI::serverSettingsPlayerNoteText(page->playerList(), true));
        return;
    }
    const int index = qBound(0, page->playerList(), ServerPlayers::listCount() - 1);
    const auto list = static_cast<ServerPlayers::List>(index);
    const QString listTitle = MainWindowUI::serverSettingsPlayerListTitle(index);
    const bool ipList = list == ServerPlayers::List::BannedIps;
    bool ok = false;
    const QString typed =
        InlineMessage::getText(
            owner->m_ui->overlay(), MainWindow::tr("加进%1").arg(listTitle),
            ipList ? MainWindow::tr("要封哪个地址？\n\n只写一个 IPv4 / IPv6 地址（不带端口）。")
                   : MainWindow::tr("输入玩家名称。\n\n离线服务端根据名称计算 UUID。在线服务端请在控制台使用 whitelist add 或 op，以查询玩家的 Mojang UUID。"),
            QString(), &ok)
            .trimmed();
    if (!ok || typed.isEmpty()) {
        return;
    }
    // 大致的形状闸（真正的判据在服务端那边 —— 这一层只挡明显的手滑）。
    if (ipList && (typed.contains(QLatin1Char('/')) || typed.contains(QLatin1Char(' ')))) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("加进%1").arg(listTitle),
                               MainWindow::tr("地址里不该有空格或者斜杠。"));
        return;
    }
    const ServerPlayers::Content content =
        owner->m_serverSettingsController->readPlayers(serverId, list);
    if (!content.ok) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("加不进%1").arg(listTitle),
                                content.problem);
        return;
    }
    QList<ServerPlayers::Entry> entries = content.entries;
    for (const ServerPlayers::Entry &entry : entries) {
        const bool same = ipList
                              ? entry.ip.compare(typed, Qt::CaseInsensitive) == 0
                              : entry.name.compare(typed, Qt::CaseInsensitive) == 0;
        if (same) {
            InlineMessage::information(owner->m_ui->overlay(), MainWindow::tr("加进%1").arg(listTitle),
                                       MainWindow::tr("「%1」已在名单中。").arg(typed));
            return;
        }
    }
    entries.append(ServerPlayers::makeEntry(list, typed, QStringLiteral("WTFML"),
                                            QDateTime::currentDateTime()));
    QString problem;
    if (!owner->m_serverSettingsController->writePlayers(serverId, list, entries, &problem)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("加不进%1").arg(listTitle), problem);
        return;
    }
    owner->appendLog(MainWindow::tr("服务端 %1 的%2里加上了「%3」。").arg(serverId, listTitle, typed));
    owner->refreshServerPlayers();
}

void ServerManagementActions::removeServerPlayer(MainWindow *owner, const QString &identity) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty() || identity.isEmpty()) {
        return;
    }
    if (!owner->m_serverSettings->editable()) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("现在改不了"),
            MainWindowUI::serverSettingsPlayerNoteText(page->playerList(), true));
        return;
    }
    const int index = qBound(0, page->playerList(), ServerPlayers::listCount() - 1);
    const auto list = static_cast<ServerPlayers::List>(index);
    const QString listTitle = MainWindowUI::serverSettingsPlayerListTitle(index);
    const ServerPlayers::Content content =
        owner->m_serverSettingsController->readPlayers(serverId, list);
    if (!content.ok) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("从%1中移除").arg(listTitle),
                                content.problem);
        return;
    }
    QList<ServerPlayers::Entry> entries = content.entries;
    const int at = ServerPlayers::indexOf(entries, list, identity);
    if (at < 0) {
        // 页面停着的时候文件被别处改了：不硬来，重摆一遍让他看到现在的样子。
        owner->refreshServerPlayers();
        return;
    }
    const ServerPlayers::Entry entry = entries.at(at);
    const QString who = list == ServerPlayers::List::BannedIps
                            ? entry.ip
                            : (entry.name.isEmpty() ? entry.uuid : entry.name);
    if (!InlineMessage::question(
            owner->m_ui->overlay(), MainWindow::tr("从%1中移除").arg(listTitle),
            list == ServerPlayers::List::BannedIps
                ? MainWindow::tr("将解除对地址「%1」的封禁。").arg(who)
                : list == ServerPlayers::List::BannedPlayers
                      ? MainWindow::tr("将解除对玩家「%1」的封禁。").arg(who)
                      : MainWindow::tr("将从服务器列表中移除「%1」。").arg(who),
            MainWindow::tr("移除条目"), MainWindow::tr("取消"), /*defaultYes=*/false, /*destructive=*/false)) {
        return;
    }
    entries.removeAt(at);
    QString problem;
    if (!owner->m_serverSettingsController->writePlayers(serverId, list, entries, &problem)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("从%1中移除").arg(listTitle), problem);
        return;
    }
    owner->appendLog(MainWindow::tr("已从服务端 %1 的%2中移除「%3」。").arg(serverId, listTitle, who));
    owner->refreshServerPlayers();
}

void ServerManagementActions::pickServerResourcePack(MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    if (page == nullptr || owner->m_serverSettings->serverId().isEmpty()) {
        return;
    }
    if (!owner->m_serverSettings->editable()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("现在改不了"),
                               MainWindowUI::serverSettingsRunningNote());
        return;
    }
    // 上一次选过的那一份就在它旁边接着选（`QFileDialog` 的"上次目录"由系统记，这里只
    // 给一个起点）。筛子允许 `.zip` 和 `.jar`：资源包有时候是别人从模组里剥出来的，
    // 扩展名不一定对 —— 真正的判据在 `ServerResourcePack::inspect()` 里（看条目表）。
    const QString chosen = QFileDialog::getOpenFileName(
        owner, MainWindow::tr("选一份资源包 zip"), QString(),
        MainWindow::tr("资源包 (*.zip *.jar);;所有文件 (*.*)"));
    if (chosen.isEmpty()) {
        return;
    }
    const ServerResourcePack::Inspect inspect = ServerResourcePack::inspect(chosen);
    if (!inspect.ok) {
        // 选错了 / 包坏了 / pack.mcmeta 套在文件夹里 —— 都要当场说清楚，不能只记日志。
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("这个压缩包不能当资源包用"),
                               inspect.problem);
        return;
    }
    // 填的是**那一格**（算"用户改过"，保存时会写回 `server.properties`）。
    page->setFieldValue(QStringLiteral("resource-pack-sha1"), inspect.sha1);
    owner->appendLog(MainWindow::tr("从本地资源包算出了 SHA-1：%1（%2，%3 字节）")
                  .arg(inspect.sha1, ServerResourcePack::fileCountText(inspect),
                       QString::number(inspect.bytes)));
    // ★ 说清两件事：算的是哪一份、以及**地址还得你自己托管** —— 启动器不会把本机路径
    //   填进 `resource-pack`（别人连不到你的硬盘，那会让每个人进来都卡一下）。
    InlineMessage::information(
        owner->m_ui->overlay(), MainWindow::tr("SHA-1 算好了"),
        MainWindow::tr("已将「%1」的 SHA-1 填入「资源包 SHA-1」字段：\n\n%2\n\n请在「资源包地址」中填写其他玩家可访问的 HTTP(S) 链接。更换 ZIP 文件后需重新计算 SHA-1。设置将在服务端重启后生效。")
            .arg(QFileInfo(chosen).fileName(), inspect.sha1));
}

QString ServerManagementActions::serverContentFolder(const MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return QString();
    }
    const int kindIndex = qBound(0, page->contentKind(), ServerContent::kindCount() - 1);
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    if (static_cast<ServerContent::Kind>(kindIndex) == ServerContent::Kind::Mods) {
        return ServerContent::modsDir(entry.dirPath);
    }
    // 数据包跟着世界走：`level-name` 认不出来 / 还没建过世界 ⇒ 这一页什么都不列。
    return ServerContent::datapacksDir(owner->serverWorldDir(serverId));
}

void ServerManagementActions::refreshServerContent(MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    const int kindIndex = qBound(0, page->contentKind(), ServerContent::kindCount() - 1);
    const auto kind = static_cast<ServerContent::Kind>(kindIndex);
    const QString folder = owner->serverContentFolder();
    const bool running = !owner->m_serverSettings->editable();
    if (folder.isEmpty()) {
        // 数据包那一类：世界在哪儿都还不知道 —— 别摆一个假路径，说清楚该去哪儿改。
        page->setContentRows({});
        page->setContentNote(
            MainWindow::tr("level-name 无效，无法确定世界目录。请在「常用设置」中填写有效的世界目录名。"));
        return;
    }
    page->setContentNote(MainWindowUI::serverSettingsContentNoteText(
        QDir::toNativeSeparators(folder), running));

    QDir dir(folder);
    QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    // ★ 排序在后、认名在前：`entriesFromNames()` 会把别的文件（配置、备份、`.jar.bak`）
    //   筛掉，禁用着的（`.jar.disabled`）留下来。
    const QList<ServerContent::Entry> entries = ServerContent::entriesFromNames(names, kind);
    QList<ServerSettingsPage::ContentRow> rows;
    for (const ServerContent::Entry &entry : entries) {
        ServerSettingsPage::ContentRow row;
        row.name = entry.name;
        row.size = BackupFiles::sizeText(QFileInfo(dir.filePath(entry.name)).size());
        row.disabled = entry.disabled;
        rows.append(row);
    }
    page->setContentRows(rows);
}

void ServerManagementActions::addServerContent(MainWindow *owner) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    if (!owner->m_serverSettings->editable()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("现在改不了"),
                               MainWindowUI::serverSettingsRunningNote());
        return;
    }
    const QString folder = owner->serverContentFolder();
    if (folder.isEmpty()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("加不了"),
                               MainWindow::tr("无法根据服务端的 level-name 确定世界存档目录。"));
        return;
    }
    const int kindIndex = qBound(0, page->contentKind(), ServerContent::kindCount() - 1);
    const auto kind = static_cast<ServerContent::Kind>(kindIndex);
    const QString suffix = ServerContent::fileSuffix(kind);
    const QStringList chosen = QFileDialog::getOpenFileNames(
        owner, kind == ServerContent::Kind::Datapacks ? MainWindow::tr("选数据包 zip") : MainWindow::tr("选模组 jar"),
        QString(),
        kind == ServerContent::Kind::Datapacks ? MainWindow::tr("数据包 (*.zip);;所有文件 (*.*)")
                                               : MainWindow::tr("模组 (*.jar);;所有文件 (*.*)"));
    if (chosen.isEmpty()) {
        return;
    }
    QString lockProblem;
    if (!ServerWriteGuard::acquire(serverId, &lockProblem)) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("加不了"), lockProblem);
        return;
    }
    // 文件夹可能还不存在（服务端还没跑过）—— 这一次"要往里放东西"了，建出来。
    if (!QDir().mkpath(folder)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("加不了"),
                                MainWindow::tr("无法创建文件夹：%1").arg(QDir::toNativeSeparators(folder)));
        return;
    }
    QStringList added;
    QStringList skipped;
    for (const QString &source : chosen) {
        const QFileInfo info(source);
        // 后缀不对的不硬拦（别人给的包偶尔被改成 `.zip` / `.jar`），但要说一句。
        if (!info.fileName().endsWith(suffix, Qt::CaseInsensitive)) {
            skipped.append(MainWindow::tr("%1（后缀不是 %2）").arg(info.fileName(), suffix));
            continue;
        }
        const QString target = QDir(folder).filePath(info.fileName());
        if (QFileInfo::exists(target)) {
            // ★ **重名不覆盖**：用户手上那一份可能正是他调过的。
            skipped.append(MainWindow::tr("%1（文件夹里已经有一份了）").arg(info.fileName()));
            continue;
        }
        if (!QFile::copy(source, target)) {
            skipped.append(MainWindow::tr("%1（拷不进去）").arg(info.fileName()));
            continue;
        }
        added.append(info.fileName());
    }
    if (!added.isEmpty()) {
        owner->appendLog(MainWindow::tr("服务端 %1 的%2里加进了 %3 份：%4")
                      .arg(serverId, MainWindowUI::serverSettingsContentKindTitle(kindIndex),
                           QString::number(added.size()), added.join(QCoreApplication::translate("ServerManagementActions", "、"))));
        owner->refreshServerContent();
    }
    if (!skipped.isEmpty()) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("有几份没有加进去"),
            skipped.join(QStringLiteral("\n"))
                + MainWindow::tr("\n\n同名文件已保留。若要替换，选择「打开文件夹」并移走旧文件。"));
    } else if (!added.isEmpty()) {
        InlineMessage::information(
            owner->m_ui->overlay(), MainWindow::tr("加好了"),
            MainWindow::tr("放进去了 %1 份。服务端下一次启动时才会加载它们。").arg(added.size()));
    }
}

void ServerManagementActions::toggleServerContent(MainWindow *owner, const QString &name, bool enable) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty() || name.isEmpty()) {
        return;
    }
    if (!owner->m_serverSettings->editable()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("现在改不了"),
                               MainWindowUI::serverSettingsRunningNote());
        return;
    }
    const int kindIndex = qBound(0, page->contentKind(), ServerContent::kindCount() - 1);
    const auto kind = static_cast<ServerContent::Kind>(kindIndex);
    const QString folder = owner->serverContentFolder();
    const QString target = ServerContent::toggledName(kind, name, enable);
    if (folder.isEmpty() || target.isEmpty() || target == name) {
        // 认不出来的名字（不是这一类的文件）—— 不猜，重摆一遍让用户看到现在是什么。
        owner->refreshServerContent();
        return;
    }
    const QString sourcePath = QDir(folder).filePath(name);
    const QString targetPath = QDir(folder).filePath(target);
    if (!QFileInfo(sourcePath).isFile()) {
        // 页面停着的时候文件夹被别处动了：重摆一遍，别对着空气改名。
        owner->refreshServerContent();
        return;
    }
    if (QFileInfo::exists(targetPath)) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("改不了"),
            MainWindow::tr("目标名称「%1」已存在，请选择其他名称。").arg(target));
        return;
    }
    QString lockProblem;
    if (!ServerWriteGuard::acquire(serverId, &lockProblem)) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("改不了"), lockProblem);
        return;
    }
    QFile file(sourcePath);
    // ★ 改名而不是删除 / 移动：文件内容一个字节都不动，用户想恢复就再点一次。
    if (!file.rename(targetPath)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("改不了"),
                                MainWindow::tr("「%1」改不成「%2」：%3")
                                    .arg(name, target, file.errorString()));
        return;
    }
    owner->appendLog(MainWindow::tr("服务端 %1 的「%2」%3了（现在是「%4」）。")
                  .arg(serverId, name, enable ? MainWindow::tr("启用") : MainWindow::tr("禁用"), target));
    owner->refreshServerContent();
}

void ServerManagementActions::openServerContentFolder(MainWindow *owner) {
    const QString folder = owner->serverContentFolder();
    if (folder.isEmpty()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("找不到这个文件夹"),
                               MainWindow::tr("无法根据服务端的 level-name 确定世界存档目录。"));
        return;
    }
    if (!QFileInfo(folder).isDir()) {
        // 还没往里放过东西（服务端还没跑过 / 还没装过任何内容）——说清楚它该在哪儿，
        // 别弹一个"打开失败"（`QDesktopServices` 对不存在的路径只会静默不受理）。
        InlineMessage::information(
            owner->m_ui->overlay(), MainWindow::tr("游戏目录尚未创建"),
            MainWindow::tr("先往里放一份文件（「添加…」），或者自己建一个：\n%1")
                .arg(QDir::toNativeSeparators(folder)));
        return;
    }
    // 走 `QDesktopServices`（和"打开实例文件夹"同一条路：验证工程能换掉 url handler 拦下来）。
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("打开不了"),
                               MainWindow::tr("系统没有受理这个文件夹：%1")
                                   .arg(QDir::toNativeSeparators(folder)));
    }
}

void ServerManagementActions::kickServerPlayer(MainWindow *owner, const QString &name) {
    const QString serverId = owner->m_serverSettings->serverId();
    if (serverId.isEmpty() || name.isEmpty()) {
        return;
    }
    ServerRun::ServerProcess *process = owner->m_serverWorkspace->process(serverId);
    if (process == nullptr || !process->isRunning()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("没有在跑的服务端"),
                               MainWindow::tr("服务端已停止，无法踢出玩家。启动服务端后再试。"));
        return;
    }
    // 在线动作**必须走运行控制台**（不是偷偷改名单 —— 那要等它重启才生效，
    // 而且用户会以为人已经踢下去了）。命令和用户自己敲的那一条完全相同。
    const QString command = QStringLiteral("kick ") + name;
    if (!process->sendCommand(command)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("命令没有送出去"),
                                MainWindow::tr("服务端命令输入已关闭。"));
        return;
    }
    // 回显：服务端答的那一行会走日志通道进控制台；这一条 `[wtfml]` 只是把"我们发了什么"
    // 摆在同一页上（和"现在可以强杀了"那一句同一个前缀）。
    owner->m_serverConsole->appendLine(serverId, QStringLiteral("[wtfml] ") + MainWindow::tr("已发送：%1").arg(command));
    owner->appendLog(MainWindow::tr("服务端 %1：%2").arg(serverId, command));
    InlineMessage::information(
        owner->m_ui->overlay(), MainWindow::tr("命令送出去了"),
        MainWindow::tr("已向服务端发送「%1」。可在控制台查看执行结果。").arg(command));
}

void ServerManagementActions::runServerCheckAction(MainWindow *owner, const QString &token) {
    ServerSettingsPage *page = owner->m_serverSettings->settingsPage();
    const QString serverId = owner->m_serverSettings->serverId();
    if (page == nullptr || serverId.isEmpty()) {
        return;
    }
    if (token == QStringLiteral("reinstall")) {
        // 重装是那一行行尾那颗「修复」做的同一件事（`repairServer()`）——**先收掉这一页**：
        // 这一发正站在页面自己的槽里，收掉之后用户才看得见任务中心里那一项在跑
        // （同"开新页之前先收页面"那条规矩）。
        //
        // ★ 收页走 `dismiss()`（不是 `closeOverlay()`）：页面那一档发的是"返回箭头那一发"
        //   （`backPressed` ⇒ `reject()` ⇒ `finished` ⇒ `closeOverlay()`，页面自己的收尾和
        //   收页钩子都会跑）；`closeOverlay()` 只拆外壳、不经过 reject
        //   （见 `OverlayHost::dismiss()` 那段 ★）。
        owner->m_ui->overlay()->dismiss();
        owner->repairServer(serverId);
        return;
    }
    if (token == QStringLiteral("accept-eula")) {
        // EULA 那道闸就是 `ensureServerEula()`（那一发问的就是"同意并启动"）——这一页上
        // 并没有"启动"这回事，所以这里借它把那段话问出来、把 `eula.txt` 写下去。
        owner->ensureServerEula(serverId);
        owner->m_serverSettings->refresh();
        return;
    }
    if (token == QStringLiteral("fix-values")) {
        // 把文件里不合法的那几项改回表里的缺省（**只动那几项**，别的行一个字节都不碰）。
        const ServerModel::Properties properties = ServerRegistry::readProperties(serverId);
        const QList<ServerPropertiesForm::ValueProblem> problems =
            ServerPropertiesForm::problemsIn(properties);
        if (problems.isEmpty()) {
            return;  // 已经自己好了（另一处改的）：什么都不做
        }
        QStringList lines;
        for (const ServerPropertiesForm::ValueProblem &problem : problems) {
            const ServerPropertiesForm::Field *field =
                ServerPropertiesForm::fieldFor(problem.key);
            if (field != nullptr) {
                lines.append(MainWindow::tr("•「%1」：%2 → %3")
                                 .arg(field->label, problem.value.isEmpty() ? MainWindow::tr("（空的）")
                                                                            : problem.value,
                                      field->fallback));
            }
        }
        if (!InlineMessage::question(
                owner->m_ui->overlay(), MainWindow::tr("恢复默认值"),
                MainWindow::tr("以下设置将恢复默认值，其余设置保留：\n%1\n\n确认恢复？")
                    .arg(lines.join(QStringLiteral("\n"))),
                MainWindow::tr("恢复默认值"), MainWindow::tr("取消"), /*defaultYes=*/true, /*destructive=*/false)) {
            return;
        }
        const ServerSettingsController::SaveResult result = owner->m_serverSettingsController->save(
            serverId, owner->m_serverSettingsController->readRaw(serverId),
            owner->m_serverSettingsController->fallbackEdits(properties));
        if (!result.ok) {
            InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("改不了"), result.problem);
            return;
        }
        owner->appendLog(MainWindow::tr("服务端 %1 的 server.properties 已恢复 %2 项默认值。")
                      .arg(serverId)
                      .arg(result.changed));
        owner->m_serverSettings->refresh();
        return;
    }
    if (token == QStringLiteral("open-settings")) {
        page->setSection(ServerSettingsPage::Section::Common);
        return;
    }
    if (token == QStringLiteral("open-backups")) {
        page->setSection(ServerSettingsPage::Section::Backups);
        return;
    }
}

QString ServerManagementActions::serverWorldName(const MainWindow *owner, const QString &serverId) {
    return owner->m_serverSettingsController->worldName(serverId);
}

QString ServerManagementActions::serverWorldDir(const MainWindow *owner, const QString &serverId) {
    return owner->m_serverSettingsController->worldDir(serverId);
}

int ServerManagementActions::serverWorldBackupCount(const MainWindow *owner, const QString &serverId) {
    return owner->m_serverSettingsController->worldBackupCount(serverId);
}

void ServerManagementActions::backupServerWorld(MainWindow *owner, const QString &serverId) {
    if (serverId.isEmpty()) {
        return;
    }
    if (owner->m_backupController->busy()) {
        InlineMessage::information(owner->m_ui->overlay(), MainWindow::tr("暂时无法操作"),
                                   MainWindow::tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    const ServerRun::ServerProcess *process = owner->m_serverWorkspace->process(serverId);
    if (process != nullptr && process->isRunning()) {
        // 和"游戏在跑不给备份世界"同一条理由：它每隔几秒就在往存档里写，这时候抄出来的
        // 那一份可能正好截在半截写入上。
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("备份世界"),
                               MainWindow::tr("请先停止服务端再备份，以避免读取正在写入的存档数据。"));
        return;
    }
    const QString worldName = owner->serverWorldName(serverId);
    const QString worldDir = owner->serverWorldDir(serverId);
    if (worldName.isEmpty()) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("无法备份"),
            MainWindow::tr("level-name 无效，无法确定世界目录。哪儿。\n请在「常用设置」中填写有效的世界目录名。"));
        return;
    }
    if (!QFileInfo(worldDir).isDir()) {
        InlineMessage::information(
            owner->m_ui->overlay(), MainWindow::tr("世界尚未生成"),
            MainWindow::tr("「%1」尚未生成。首次启动服务端后再创建备份。")
                .arg(worldName));
        return;
    }

    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    const QDateTime now = QDateTime::currentDateTime();
    BackupFiles::Manifest manifest;
    manifest.kind = BackupFiles::Kind::World;
    // `root` 是 zip 里那一层前缀、也是还原时落回去的目录名 ⇒ 就是这个世界目录名。
    manifest.root = worldName;
    // `label` 只用来拼文件名 —— 带上 serverId（理由见 `serverBackupLabel()` 那段）。
    manifest.label = owner->m_serverSettingsController->backupLabel(serverId);
    manifest.worldName = worldName;
    // NOTE: 这两格说的是"这个世界是从哪个**实例**里备出来的"，而服务端不是实例 ——
    //       留空。它在界面上只有"打开目标位置"那一处读者，而那一处走的是 `targetId`
    //       那一位（下面传的是服务端目录）。
    manifest.sourceInstanceId = QString();
    manifest.sourceInstanceName = entry.name.isEmpty() ? serverId : entry.name;
    manifest.gameVersion = entry.gameVersion;
    manifest.createdAt = now;
    manifest.scope = QStringList{MainWindow::tr("这个服务端的整个世界文件夹")};

    const QString dir = BackupFiles::backupDir(BackupFiles::Kind::World);
    const QString zip = QDir(dir).filePath(
        BackupFiles::backupFileName(manifest.label, now));
    // ★ 跑完之后要把这一页的备份那一列**重摆一遍**（多了一份）—— 记下是哪个服务端。
    owner->m_backupController->startWrite(manifest, worldDir, zip,
                                   MainWindow::tr("备份世界「%1」").arg(worldName), entry.dirPath,
                                   MainWindow::tr("备份完成"), serverId, MainWindow::tr("备份没有完成"));
}

void ServerManagementActions::restoreServerWorldBackup(MainWindow *owner, const QString &serverId, const QString &zipPath) {
    if (serverId.isEmpty() || zipPath.isEmpty()) {
        return;
    }
    if (owner->m_backupController->busy()) {
        InlineMessage::information(owner->m_ui->overlay(), MainWindow::tr("暂时无法操作"),
                                   MainWindow::tr("已有备份或还原任务正在进行，请结束后再试。"));
        return;
    }
    const ServerRun::ServerProcess *process = owner->m_serverWorkspace->process(serverId);
    if (process != nullptr && process->isRunning()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("恢复备份"),
                               MainWindow::tr("服务端仍在运行。停止服务端后再还原，以免服务端保存时覆盖还原的存档。"));
        return;
    }
    const QString worldName = owner->serverWorldName(serverId);
    const QString serverDir = ServerRegistry::describe(serverId).dirPath;
    // 这一页的备份列只按**文件名前缀**筛（读清单要起一次 PowerShell，那一列
    //   可能摆着几十份，见 `BackupFiles::backupFilePrefix()` 那段）。可按下「还原…」之后
    //   要拿清单里的 `root` 去解压、把那个目录整个换掉 —— 名字能凑出来（从别处拷来的、
    //   改过名的、碰巧和前缀一样的），而清单说的不是这个世界的包，必须在这里拦住，不能
    //   等解到一半。`BackupController::pickZip()` 那条路有同一道闸。
    bool readable = false;
    QString problem;
    const BackupFiles::Manifest manifest = BackupFiles::readManifest(zipPath, &readable, &problem);
    if (!readable || manifest.kind != BackupFiles::Kind::World
        || manifest.root.compare(worldName, Qt::CaseInsensitive) != 0) {
        InlineMessage::warning(
            owner->m_ui->overlay(), MainWindow::tr("备份与当前世界不匹配"),
            readable ? MainWindow::tr("备份世界为「%1」，当前世界为「%2」。为避免覆盖其他世界，已取消恢复。")
                           .arg(manifest.root.isEmpty() ? MainWindow::tr("（没写）") : manifest.root,
                                worldName.isEmpty() ? serverId : worldName)
                     : MainWindow::tr("无法读取备份清单，已取消恢复。\n\n%1")
                           .arg(problem));
        owner->m_serverSettings->refresh();
        return;
    }
    // 默认**不**落在「还原」上：这一步会把这个世界现在的文件整个换掉（和删世界同一档）。
    if (!InlineMessage::question(
            owner->m_ui->overlay(), MainWindow::tr("恢复备份"),
            MainWindow::tr("将使用备份替换世界「%1」的当前文件。\n\n恢复前将自动创建安全快照，以便回滚。\n\n备份文件：\n%2")
                .arg(worldName.isEmpty() ? serverId : worldName,
                     QDir::toNativeSeparators(zipPath)),
            MainWindow::tr("还原"), MainWindow::tr("取消"), /*defaultYes=*/false, /*destructive=*/true)) {
        return;
    }
    // `root`（zip 里那一层前缀）就是世界目录名 ⇒ 解到**服务端目录**底下正好落回原地。
    owner->m_backupController->startRestore(zipPath, serverDir,
                                     MainWindow::tr("还原世界「%1」").arg(worldName), serverDir,
                                     MainWindow::tr("还原完成"), serverId, MainWindow::tr("还原没有完成"));
}

void ServerManagementActions::deleteServerBackup(MainWindow *owner, const QString &zipPath) {
    if (zipPath.isEmpty()) {
        return;
    }
    const QFileInfo info(zipPath);
    if (!info.exists()) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("删不掉"),
                               MainWindow::tr("备份文件已不存在：\n%1")
                                   .arg(QDir::toNativeSeparators(zipPath)));
        owner->m_serverSettings->refresh();
        return;
    }
    // 这一份进**回收站**（不是永久删除）：备份就是留着以防万一的，手滑删掉更该有退路。
    // 默认那一边仍然是「取消」（和删服务端 / 删世界同一档）。
    if (!InlineMessage::question(
            owner->m_ui->overlay(), MainWindow::tr("删除备份"),
            MainWindow::tr("将以下备份移至回收站：\n%1\n\n确认删除？")
                .arg(QDir::toNativeSeparators(zipPath)),
            MainWindow::tr("删除"), MainWindow::tr("取消"), /*defaultYes=*/false, /*destructive=*/true)) {
        return;
    }
    QString lockProblem;
    if (!ServerWriteGuard::acquire(owner->m_serverSettings->serverId(), &lockProblem)) {
        InlineMessage::warning(owner->m_ui->overlay(), MainWindow::tr("删不掉"), lockProblem);
        return;
    }
    QString recycleProblem;
    if (!moveToRecycleBin(zipPath, &recycleProblem)) {
        InlineMessage::critical(owner->m_ui->overlay(), MainWindow::tr("删不掉"),
                                MainWindow::tr("无法将备份移至回收站：%1\n%2")
                                    .arg(QDir::toNativeSeparators(zipPath), recycleProblem));
        return;
    }
    owner->appendLog(MainWindow::tr("已删除一份备份：%1").arg(QDir::toNativeSeparators(zipPath)));
    owner->m_serverSettings->refresh();
}
