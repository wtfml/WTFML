/**
 * @file ServerRunController.cpp
 * @brief 起 / 停 / 强杀 / 退出之后（见 .hpp 那段）。
 *
 * 这一片是**接线**，不是判据："谁在什么时候按了什么 → 调哪一个"，以及"崩了之后要不要
 * 再拉起来"。判据全在 `ServerRun.hpp` 与 `ServerRestart.hpp`。
 */

#include "controllers/ServerRunController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
#include "download/ServerInstall.hpp"
#include "manage/VersionManager.hpp"
#include "server/ServerRegistry.hpp"
#include "server/ServerRestart.hpp"
#include "server/ServerRun.hpp"
#include "settings/GlobalJavaSettings.hpp"

#include <QJsonObject>
#include <QTimer>

ServerRunController::ServerRunController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

void ServerRunController::setFileBusy(const QString &serverId, bool busy) {
    if (serverId.isEmpty()) {
        return;
    }
    if (busy) {
        m_fileBusy.insert(serverId);
    } else {
        m_fileBusy.remove(serverId);
    }
}

void ServerRunController::start(const QString &serverId, bool automatic) {
    if (serverId.isEmpty() || m_startupTasks.contains(serverId)) {
        return;
    }
    // 闸⓪：这一份的世界正在备份 / 还原。那一趟现在读 / 换的正是这个世界目录 —— 这时候
    // 起服，它读写到的是"一半新一半旧"的存档，收尾还会把那几份文件当成新内容再盖一遍。
    if (m_fileBusy.contains(serverId)) {
        if (automatic) {
            // 自动重启是它自己起的（用户没在等这一下），不弹窗，记一行就够。
            m_owner.appendLog(MainWindow::tr("%1 的世界正在备份或者还原，这一次没有自动重启。").arg(serverId));
        } else {
            InlineMessage::information(
                m_owner.m_ui->overlay(), MainWindow::tr("暂时不能启动"),
                MainWindow::tr("世界存档正在备份或还原。请等待操作完成后再启动。"));
        }
        return;
    }
    // 闸①：装好了没有。判据是 `ServerRegistry::describe()`（= `ServerModel::isInstalled()`），
    // 和那一行上写着「未安装」的是同一个出处。
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    if (!entry.installed) {
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("服务端暂时无法启动"),
                               ServerRun::notInstalledMessage());
        return;
    }
    ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->ensureProcess(serverId);
    if (process == nullptr) {
        return;
    }
    // 闸②：已经在跑了（同一行连点两下、或者右键菜单里又点了一次）。
    if (process->isRunning()) {
        m_owner.appendLog(MainWindow::tr("%1 已经在跑了。").arg(serverId));
        return;
    }
    // 闸③：EULA。**只有用户自己勾了才写**（见 ensureServerEula）。
    // 同意框会处理界面事件，先占住这一台的启动位置。
    m_startupTasks.insert(serverId, Tasks::invalidTaskId());
    if (!m_owner.ensureServerEula(serverId)) {
        m_startupTasks.remove(serverId);
        return;
    }
    // 用户亲手起的 = 新的一轮。自动重启必须沿用前一轮的计数（见 `ServerRestart::decide()`）。
    if (!automatic) {
        m_restartAttempts.remove(serverId);
    }
    m_stopRequested.remove(serverId);
    const QString shown = entry.name.isEmpty() ? serverId : entry.name;
    const Tasks::TaskId task = m_owner.beginTask(Tasks::TaskKind::ServerLaunch,
        MainWindow::tr("启动服务端 %1").arg(shown), serverId, {});
    auto *tasks = m_owner.m_activityCenter->tasks();
    m_startupTasks.insert(serverId, task);
    tasks->setDetail(task, MainWindow::tr("正在准备服务端启动参数…"));
    // 闸④：那条命令拼得出来吗（这一档要的信息在不在我们记的那一份里）。
    QString problem;
    const ServerInstall::Plan plan = m_owner.serverLaunchPlan(serverId, &problem);
    if (!plan.ok) {
        m_owner.finishTask(m_startupTasks.take(serverId), false, problem);
        InlineMessage::critical(m_owner.m_ui->overlay(), MainWindow::tr("起不了这个服务端"), problem);
        return;
    }
    // 闸⑤：java。判据和装加载器/装服务端那两条路**同一套**（版本 json 里的 `javaVersion`
    // 说了算），拿不到就说清楚为什么。
    const ServerModel::Info info = ServerRegistry::readMeta(serverId);
    // 这一个自己指定过 Java 就用它（`Info::javaPath` 的原话），否则跟全局那一套。
    const QString configuredJava =
        info.javaPath.isEmpty() ? m_owner.m_javaSettings->javaPath() : info.javaPath;
    QString java = configuredJava;
    int requiredJavaMajor = 0;
    const QJsonObject gameJson = m_owner.m_versionManager->getVersionJson(info.gameVersion);
    if (!gameJson.isEmpty()) {
        requiredJavaMajor = gameJson[QStringLiteral("javaVersion")]
                                .toObject()[QStringLiteral("majorVersion")]
                                .toInt();
    }
    QString javaReason;
    if (!m_owner.m_javaSettings->installerJavaFor(requiredJavaMajor, configuredJava, &java, &javaReason)) {
        m_owner.appendLog(MainWindow::tr("起这个服务端要的 Java 拿不到：%1").arg(javaReason));
        m_owner.finishTask(m_startupTasks.take(serverId), false, javaReason);
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("Java 版本不够"), javaReason);
        return;
    }
    if (java != configuredJava) {
        m_owner.appendLog(MainWindow::tr("这个服务端要 Java %1，起的时候用 %2").arg(requiredJavaMajor).arg(java));
    }

    // 那条命令 + 宽限那两个入参（模组数量 / 存档大小，见 ServerRun.hpp 那张表）。
    // 内存：这一个自己配过就用它，没配过（`memoryMb <= 0`，见 `ServerModel::Info` 那句）
    // 就跟全局那一档 —— 和上面 java 的规矩一样。
    const int memoryMb = info.memoryMb > 0 ? info.memoryMb : m_owner.m_javaSettings->xmx();
    const QStringList arguments = ServerRun::launchArguments(plan, memoryMb);
    const int mods = m_owner.countServerMods(entry.dirPath);
    const qint64 worldBytes = m_owner.serverWorldBytes(entry.dirPath);
    const int gracefulMs = ServerRun::gracefulStopMs(mods, worldBytes);
    m_owner.appendLog(MainWindow::tr("起服务端 %1：%2 个模组、存档 %3 MB，停下来最多等 %4 秒。")
                  .arg(shown)
                  .arg(mods)
                  .arg(worldBytes / (1024 * 1024))
                  .arg(gracefulMs / 1000));
    if (!plan.note.isEmpty()) {
        // 那一档自己的补充说明（"第一趟要联网""入口是两张参数文件"…）——**唯一出处**，
        // 照抄，别在这儿另编一句。
        m_owner.appendLog(plan.note);
    }
    m_startedAt.insert(serverId, QDateTime::currentDateTime());
    tasks->setDetail(task, MainWindow::tr("正在等待服务端就绪…"));
    process->start(serverId, entry.dirPath, java, arguments, gracefulMs);
}

void ServerRunController::stop(const QString &serverId) {
    ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->process(serverId);
    if (process == nullptr || !process->isRunning()) {
        return;
    }
    // "这一次是我们让它停的"：退出回调要拿这个把 `Crash` 那一档排掉（见 `onServerExited`）。
    m_stopRequested.insert(serverId);
    if (process->requestStop()) {
        return;
    }
    // 那一句写不进去（它的 stdin 已经关了 / 它根本不在读）⇒ 只剩强杀一条路。这时候还
    // 摆着"再等 X 秒"就是骗人，直接说实话。
    m_owner.appendLog(MainWindow::tr("%1：无法发送停止命令，将强制结束服务端。").arg(serverId));
    process->forceKill();
}

void ServerRunController::forceKill(const QString &serverId) {
    ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->process(serverId);
    if (process == nullptr || !process->isRunning()) {
        return;
    }
    // 同上：这是用户让它死的，退出码再难看也不是"崩了"。
    m_stopRequested.insert(serverId);
    process->forceKill();
}

bool ServerRunController::stopAndWait(const QString &serverId) {
    auto *process = m_owner.m_serverWorkspace->process(serverId);
    if (process == nullptr) {
        return false;
    }
    if (process->isRunning()) {
        m_stopRequested.insert(serverId);
    }
    return process->stopAndWait();
}

void ServerRunController::onReady(const QString &serverId) {
    if (m_stopRequested.contains(serverId)) {
        return;
    }
    if (const auto task = m_startupTasks.take(serverId); task != Tasks::invalidTaskId()) {
        m_owner.finishTask(task, true);
    }
}

void ServerRunController::onExited(const QString &serverId, int exitCode) {
    const ServerRun::ServerProcess *process = m_owner.m_serverWorkspace->process(serverId);
    const ServerRegistry::Entry entry = ServerRegistry::describe(serverId);
    const QString shown = entry.name.isEmpty() ? serverId : entry.name;
    // ---------- 这一次是怎么退的（重启策略）----------
    const bool userStopped = m_stopRequested.remove(serverId) > 0;
    const bool forceKilled = process != nullptr && process->wasForceKilled();
    const bool zeroStartupFailure = exitCode == 0 && process != nullptr
        && process->status() == ServerModel::Status::Crashed;
    const ServerRestart::Why why = zeroStartupFailure && !userStopped && !forceKilled
        ? ServerRestart::Why::Crash : ServerRestart::whyFor(userStopped, forceKilled, exitCode);
    // 跑够久（`kResetAfterSeconds()`）再崩算新的一轮：一台"每周崩一次"的服务端不该在
    // 第三次之后永远不再自动起来。
    const QDateTime startedAt = m_startedAt.take(serverId);
    if (startedAt.isValid()
        && startedAt.secsTo(QDateTime::currentDateTime())
               >= ServerRestart::kResetAfterSeconds) {
        m_restartAttempts.remove(serverId);
    }
    if (!ServerRestart::isAbnormal(why)) {
        // 正常退出 / 我们自己停的：这一轮结束，计数没必要留着。
        m_restartAttempts.remove(serverId);
    }
    if (const auto task = m_startupTasks.take(serverId); task != Tasks::invalidTaskId()) {
        if (userStopped) {
            m_owner.m_activityCenter->tasks()->cancel(task);
        } else {
            m_owner.finishTask(task, false,
                MainWindow::tr("服务端尚未就绪，进程已退出（退出码 %1）。").arg(exitCode));
        }
    }
    m_owner.updateServerRow(serverId);
    m_owner.updateServerConsole();
    if (exitCode == 0 && !zeroStartupFailure) {
        m_owner.appendLog(MainWindow::tr("%1 已经退出。").arg(shown));
        return;
    }
    if (process != nullptr && process->wasForceKilled()) {
        // "你让我强杀的"和"它自己崩了"是两件事 —— 后者才该弹一个预警框。
        m_owner.appendLog(MainWindow::tr("%1 已经被强制结束。").arg(shown));
        return;
    }
    const QString exitReason = zeroStartupFailure ? MainWindow::tr("未完成启动（进程退出码 0）")
        : MainWindow::tr("异常退出（退出码 %1）").arg(exitCode);
    m_owner.appendLog(MainWindow::tr("%1：%2。").arg(shown, exitReason));
    // ---------- 崩了之后要不要自动拉起（重启策略 + 上限）----------
    //
    // 判据全在 `ServerRestart::decide()`（verify ZAF 一节钉死）。这一层只做两件事：
    // 把盘上记的策略读出来、按它的结论排那一发 `singleShot`。
    const ServerModel::Info info = ServerRegistry::readMeta(serverId);
    ServerRestart::Policy policy;
    policy.enabled = info.restartOnCrash;
    policy.maxAttempts = info.restartMaxAttempts;
    const int used = m_restartAttempts.value(serverId);
    const ServerRestart::Decision decision = ServerRestart::decide(policy, why, used);
    if (decision.restart) {
        m_restartAttempts.insert(serverId, used + 1);
        m_owner.appendLog(MainWindow::tr("%1：%2").arg(shown, decision.note));
        // 这一步**不弹**那张阻塞的预警卡：用户多半不在跟前，弹了他不点，重启就一直等着
        // 那层嵌套事件循环（"自动重启"变成"等用户回来才重启"）。日志里有原话，控制台里
        // 也有它退出前最后那几行。
        QTimer::singleShot(decision.delaySeconds * 1000, this, [this, serverId, shown] {
            const ServerRun::ServerProcess *fresh = m_owner.m_serverWorkspace->process(serverId);
            if (fresh != nullptr && fresh->isRunning()) {
                return;  // 用户在这几秒里自己又起来了
            }
            m_owner.appendLog(MainWindow::tr("%1：自动重启来了。").arg(shown));
            start(serverId, /*automatic=*/true);
        });
        return;
    }
    if (!decision.note.isEmpty()) {
        // 次数用完了：这一句必须说出来，不然用户会以为它还会自己起来。
        m_owner.appendLog(MainWindow::tr("%1：%2").arg(shown, decision.note));
    }
    // 「错误既进入日志，也在用户当前流程中可见」：日志那一半上面已经写了，这一半是
    // 弹窗 —— 用户点了启动之后常常就去干别的了，不弹一下他看不见。
    InlineMessage::warning(
        m_owner.m_ui->overlay(), MainWindow::tr("这个服务端退出了"),
        MainWindow::tr("「%1」%2。\n\n右键该服务端并选择「打开控制台」，可查看退出前的日志。")
            .arg(shown, exitReason));
}
