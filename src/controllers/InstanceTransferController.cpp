#include "InstanceTransferController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/VersionManager.hpp"
#include "manage/WorldFiles.hpp"
#include "util/AppPaths.hpp"
#include "util/DirectorySessionLock.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>
#include <QJsonObject>
#include <QElapsedTimer>
#include <atomic>
#include <memory>

#include <utility>

// 后台那一份的返回值。`QFutureWatcher::finished` 不带结果，结果从 `result()` 里读 ——
// 不需要给这个结构注册元类型（那条路没走信号参数）。
struct CopyOutcome {
    bool ok = false;
    QString error;
};

namespace {

// 目标文件夹是不是**当前这个游戏文件夹**。两处要问这件事（复制成功那句提示的语气、
// 以及后台复制要不要把目标名字记进「文件操作中」），判据只能有一个。
//
// **不能**只拿 `QDir::cleanPath` 比字面量。用户完全可以把游戏文件夹
// 用另一条路指过来（junction / 符号链接）—— 那时两串字面量不同、而 `versions/<名字>/`
// 落的就是同一个目录。漏判的后果分两处：
// · 后台复制期间目标名字**没被记进 `FileOperationOccupancy`** ⇒ 拷到一半的实例
// 下一次刷新就能被启动 / 删除（那两条闸只查 `installingVersionIds()`）；
// · 复制完成那句提示说"在另一个游戏文件夹里"，而它明明就在列表里。
// `InstanceFiles::pathIsSameOrInside()` 会解 junction / 8.3 短名 / 符号链接
// （`canonicalFilePath()` 自己**不解 junction**，见 InstanceFiles.cpp 里那一大段），
// 所以拿它**两头各问一次**就是"同一个文件夹"。解不出来 / 两边只有大小写写法不同时
// 退回字面量比较 —— 那是这条判据的兜底，不能省。
bool isCurrentGameFolder(const QString &folder) {
    if (folder.isEmpty()) {
        return false;
    }
    const QString current = AppPaths::minecraftDir();
    if (!current.isEmpty() && InstanceFiles::pathIsSameOrInside(current, folder)
        && InstanceFiles::pathIsSameOrInside(folder, current)) {
        return true;
    }
    return !current.isEmpty()
           && QDir::cleanPath(folder).compare(QDir::cleanPath(current), Qt::CaseInsensitive) == 0;
}

}  // namespace

InstanceTransferController::InstanceTransferController(VersionManager *versions,
                                                       LauncherManager *launcher,
                                                       OverlayHost *overlay, Host host,
                                                       QObject *parent)
    : QObject(parent), m_versions(versions), m_launcher(launcher), m_overlay(overlay),
      m_host(std::move(host)) {}

QString InstanceTransferController::displayName(const QString &id) const {
    return m_host.displayName ? m_host.displayName(id) : id;
}
void InstanceTransferController::log(const QString &text) const {
    if (m_host.log) m_host.log(text);
}

bool InstanceTransferController::copyInstance(const QString &sourceId,
                                               const QString &destGameFolder,
                                               const QString &newName) {
    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("复制实例"), tr("副本得有个名字。"));
        return false;
    }
    const QString nameProblem = InstanceFiles::validateDirName(name);
    if (!nameProblem.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("复制实例"), nameProblem);
        return false;
    }
    if (m_launcher->isVersionRunning(sourceId)) {
        InlineMessage::warning(m_overlay, tr("复制实例"),
                               tr("「%1」正在运行。请退出游戏后再复制实例。")
                                   .arg(displayName(sourceId)));
        return false;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(sourceId)) {
        InlineMessage::warning(m_overlay, tr("复制实例"),
                               tr("「%1」正在下载，等它装完再复制。")
                                   .arg(displayName(sourceId)));
        return false;
    }
    const QString sourceDir = AppPaths::versionDir(sourceId);
    if (InstanceFiles::pathIsSameOrInside(sourceDir, destGameFolder)) {
        InlineMessage::warning(
            m_overlay, tr("复制实例"),
            tr("「%1」不能复制到其自身或子目录。\n\n选择路径：\n%2\n\n请选择有效的游戏目录。")
                .arg(displayName(sourceId), QDir::toNativeSeparators(destGameFolder)));
        return false;
    }
    QString error;
    if (!DirectorySessionLock::acquire(destGameFolder, &error)) {
        InlineMessage::warning(m_overlay, tr("复制实例"), error);
        return false;
    }
    if (!m_versions->copyInstance(sourceId, destGameFolder, name, &error)) {
        InlineMessage::critical(m_overlay, tr("复制实例失败"), error);
        return false;
    }
    if (m_host.refreshInstances) m_host.refreshInstances();
    announceCopySuccess(sourceId, destGameFolder, name);
    return true;
}

void InstanceTransferController::announceCopySuccess(const QString &sourceId,
                                                      const QString &destGameFolder,
                                                      const QString &name) {
    const QString landed = QDir::toNativeSeparators(
        QDir(destGameFolder).absoluteFilePath(QStringLiteral("versions/") + name));
    log(tr("已复制实例 %1 → %2").arg(displayName(sourceId), landed));
    const bool sameFolder = isCurrentGameFolder(destGameFolder);
    if (sameFolder) {
        InlineMessage::information(m_overlay, tr("复制实例"),
                                   tr("「%1」已经复制好了，就在实例列表里。").arg(name));
    } else {
        InlineMessage::information(
            m_overlay, tr("复制实例"),
            tr("实例「%1」已复制至游戏目录：\n%2\n\n切换至该游戏目录后即可在列表中查看。")
                .arg(name, landed));
    }
}

void InstanceTransferController::copyInstanceInBackground(
    const QString &sourceId, const QString &destGameFolder, const QString &newName,
    std::function<void(bool, const QString &)> whenDone, bool announce) {
    const auto done = [whenDone](bool ok, const QString &error) {
        if (whenDone) whenDone(ok, error);
    };
    const auto failNow = [this, &done](const QString &title, const QString &text) {
        InlineMessage::warning(m_overlay, title, text);
        done(false, text);
    };
    const QString name = newName.trimmed();
    if (name.isEmpty()) {
        failNow(tr("复制实例"), tr("副本得有个名字。"));
        return;
    }
    const QString nameProblem = InstanceFiles::validateDirName(name);
    if (!nameProblem.isEmpty()) {
        failNow(tr("复制实例"), nameProblem);
        return;
    }
    if (m_copyBusy) {
        // NOTE: 这不是"忙"的那一条正常路径 —— 界面上那颗按钮在复制期间还能按，所以得挡。
        InlineMessage::information(m_overlay, tr("复制实例"),
                                   tr("已经有一份实例正在复制，等它完成后再试。"));
        done(false, tr("已经有一份实例正在复制。"));
        return;
    }
    if (m_launcher->isVersionRunning(sourceId)) {
        failNow(tr("复制实例"),
                tr("「%1」正在运行。请退出游戏后再复制实例。")
                    .arg(displayName(sourceId)));
        return;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(sourceId)) {
        failNow(tr("复制实例"),
                tr("「%1」正在下载，等它装完再复制。").arg(displayName(sourceId)));
        return;
    }
    const QString sourceDir = AppPaths::versionDir(sourceId);
    if (InstanceFiles::pathIsSameOrInside(sourceDir, destGameFolder)) {
        failNow(tr("复制实例"),
                tr("「%1」不能复制到其自身或子目录。\n\n选择路径：\n%2\n\n请选择有效的游戏目录。")
                    .arg(displayName(sourceId), QDir::toNativeSeparators(destGameFolder)));
        return;
    }

    // ---------- 任务中心：规划 → 执行 → 验证 ----------
    QString lockProblem;
    if (!DirectorySessionLock::acquire(destGameFolder, &lockProblem)) {
        failNow(tr("复制实例"), lockProblem);
        return;
    }
    const QList<Tasks::TaskStep> steps{
        Tasks::TaskStep{tr("规划：检查名字与目标文件夹"), Tasks::TaskStepState::Done, {}},
        Tasks::TaskStep{tr("执行：复制实例目录"), Tasks::TaskStepState::Running, {}},
        Tasks::TaskStep{tr("验证：核对副本的 jar / json / meta"),
                        Tasks::TaskStepState::Pending, {}},
    };
    if (m_host.beginTask) {
        m_copyTaskId = m_host.beginTask(
            Tasks::TaskKind::Other,
            tr("复制实例 %1 → %2").arg(displayName(sourceId), name), name, steps);
        if (m_host.setTaskProgress) m_host.setTaskProgress(m_copyTaskId, 1, 3);
        if (m_host.setTaskDetail) {
            m_host.setTaskDetail(m_copyTaskId, tr("正在后台复制实例目录…"));
        }
        // 复制**不提供取消**：拷到一半停下，目标目录里是半份实例，而实例识别只看
        // 主 jar 在不在 —— 那一份半截目录下一次刷新就能被启动。说清原因，别只藏按钮。
        if (m_host.setTaskCapabilities) {
            Tasks::TaskCapabilities caps;
            caps.cancelBlockedReason =
                tr("复制要保证副本是一整份，中途停下会留下半份实例目录；这一项不支持取消。");
            m_host.setTaskCapabilities(m_copyTaskId, caps);
        }
    }
    log(tr("开始复制实例 %1 → %2 …").arg(displayName(sourceId), name));
    m_copyBusy = true;
    // 拷树在后台跑，这段时间必须有"忙"标记：否则源实例照样能被
    // 启动（游戏一边写存档、这一边读 ⇒ 副本里是半截世界）、能被删、能再改设置。
    // 开始前的 `isVersionRunning` / `isBeingModified` 两道闸只管"按下去那一刻"。
    // **源**必须记；**目标**在"目标游戏文件夹就是当前游戏文件夹"时也得分两说：
    // 实例识别只看 `versions/<名字>/` 里有没有主 jar（`InstanceFiles::resolveInstance`），
    // 而拷树是边拷边落盘的 —— 拷到一半时 `<name>/<sourceId>.jar` 已经躺在那儿，下一次
    // 任何全量刷新都能把它认成一份实例（`instanceId` 还是源的、meta 也没改）。启动 /
    // 删除的闸只查 `installingVersionIds()`，所以这一段名字也得记进去。
    // 目标在**别的**游戏文件夹时不能记这个名字：那是另一个文件夹里的相对名，当前文件夹
    // 里可能有同名实例，记了会误伤它。
    const bool copyIntoCurrentFolder = isCurrentGameFolder(destGameFolder);
    const bool markCopyTarget = copyIntoCurrentFolder && name != sourceId;
    if (m_host.setFileOperationBusy) {
        m_host.setFileOperationBusy(sourceId, true);
        if (markCopyTarget) m_host.setFileOperationBusy(name, true);
    }

    // 为什么在后台线程里**另起一个** `VersionManager`：`copyInstance()` 最后会动
    // `versionJsons` / `mergedVersionJsons` 两份缓存，拿主线程那一份在别的线程里跑就是
    // 数据竞争。导出那一层（`ModpackExportController`）用的是同一条写法。
    auto *watcher = new QFutureWatcher<CopyOutcome>(this);
    connect(watcher, &QFutureWatcher<CopyOutcome>::finished, this,
            [this, watcher, sourceId, sourceDir, destGameFolder, name, markCopyTarget, done, announce] {
                const CopyOutcome outcome = watcher->result();
                watcher->deleteLater();
                m_copyBusy = false;
                // 先摘忙标记，再走下面任何一条早退（复制失败 / 校验没对上）。
                if (m_host.setFileOperationBusy) {
                    m_host.setFileOperationBusy(sourceId, false);
                    if (markCopyTarget) m_host.setFileOperationBusy(name, false);
                }
                // 刷新必须**跟着摘标记走**，不能只挂在成功那一条路上。
                // 复制期间源实例被 `installingVersionIds()` 滤掉；万一这中间别处
                // （换文件夹 / 下载收尾 / 别的对话框）刷新过一次列表，屏幕上就少了
                // 这一行。复制 / 校验失败时用户更需要看见它还在。
                if (m_host.refreshInstances) m_host.refreshInstances();
                const auto finishTask = [this](bool success, const QString &error) {
                    if (m_host.finishTask && m_copyTaskId != Tasks::invalidTaskId()) {
                        m_host.finishTask(m_copyTaskId, success, error);
                    }
                    m_copyTaskId = Tasks::invalidTaskId();
                };
                if (!outcome.ok) {
                    if (m_host.setTaskStepState && m_copyTaskId != Tasks::invalidTaskId()) {
                        m_host.setTaskStepState(m_copyTaskId, 1,
                                                Tasks::TaskStepState::Failed);
                    }
                    finishTask(false, outcome.error);
                    InlineMessage::critical(m_overlay, tr("复制实例失败"), outcome.error);
                    done(false, outcome.error);
                    return;
                }
                if (m_host.setTaskStepState && m_copyTaskId != Tasks::invalidTaskId()) {
                    m_host.setTaskStepState(m_copyTaskId, 1, Tasks::TaskStepState::Done);
                    m_host.setTaskStepState(m_copyTaskId, 2, Tasks::TaskStepState::Running);
                    if (m_host.setTaskProgress) m_host.setTaskProgress(m_copyTaskId, 2, 3);
                }
                // 验证：目录在、该跟着改名的 json 跟着改了、meta 也在。只看"做出来的这一份
                // 能不能被认出来"，不再读一遍内容。
                const QString destDir =
                    QDir(destGameFolder).absoluteFilePath(QStringLiteral("versions/") + name);
                const bool sourceHadJson =
                    QFileInfo::exists(sourceDir + QLatin1Char('/') + sourceId
                                      + QStringLiteral(".json"));
                QString verifyProblem;
                if (!QFileInfo(destDir).isDir()) {
                    verifyProblem = tr("后台复制说完成了，但目标目录不在：%1")
                                        .arg(QDir::toNativeSeparators(destDir));
                } else if (sourceHadJson
                           && !QFileInfo::exists(destDir + QLatin1Char('/') + name
                                                 + QStringLiteral(".json"))) {
                    verifyProblem = tr("副本里的版本 json 没跟着改名（%1.json 不在）。")
                                        .arg(name);
                } else if (!QFileInfo::exists(destDir + QLatin1Char('/')
                                              + AppPaths::versionMetaFileName())) {
                    verifyProblem = tr("副本缺少 %1，无法识别实例。")
                                        .arg(AppPaths::versionMetaFileName());
                }
                if (!verifyProblem.isEmpty()) {
                    if (m_host.setTaskStepState && m_copyTaskId != Tasks::invalidTaskId()) {
                        m_host.setTaskStepState(m_copyTaskId, 2,
                                                Tasks::TaskStepState::Failed);
                    }
                    finishTask(false, verifyProblem);
                    InlineMessage::warning(m_overlay, tr("复制实例"), verifyProblem);
                    done(false, verifyProblem);
                    return;
                }
                if (m_host.setTaskStepState && m_copyTaskId != Tasks::invalidTaskId()) {
                    m_host.setTaskStepState(m_copyTaskId, 2, Tasks::TaskStepState::Done);
                    if (m_host.setTaskProgress) m_host.setTaskProgress(m_copyTaskId, 3, 3);
                }
                finishTask(true, {});
                if (announce) announceCopySuccess(sourceId, destGameFolder, name);
                done(true, {});
            });
    // sourceDir 在**主线程**算好再捕进来（上面 `const QString sourceDir`）。
    // `copyInstance()` 内部会现读 `AppPaths::versionDir()`，而那是进程级可变状态 ——
    // 用户在这趟跑着的时候能切换游戏文件夹（这页复制期间没有禁用），worker 里去读
    // 就是数据竞争，而且可能拷错目录。后台一律走 `copyInstanceFrom()`。
    watcher->setFuture(QtConcurrent::run([sourceDir, sourceId, destGameFolder, name] {
        VersionManager worker;
        CopyOutcome outcome;
        outcome.ok = worker.copyInstanceFrom(sourceDir, sourceId, destGameFolder, name,
                                             &outcome.error);
        return outcome;
    }));
}

bool InstanceTransferController::importWorld(const QString &instanceId,
                                             const QString &worldDir,
                                             const QString &name) {
    if (m_launcher && m_launcher->isVersionRunning(instanceId)) {
        InlineMessage::warning(m_overlay, tr("导入世界"),
                               tr("游戏正在运行，先关闭游戏再导入存档。"));
        return false;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(instanceId)) {
        InlineMessage::warning(m_overlay, tr("导入世界"),
                               tr("实例正在安装或处理文件，完成后再导入存档。"));
        return false;
    }
    const QString savesDir = m_host.instanceDirForKey
        ? m_host.instanceDirForKey(instanceId, QStringLiteral("save")) : QString();
    if (savesDir.isEmpty()) {
        InlineMessage::critical(m_overlay, tr("导入世界失败"), tr("无法识别实例的存档目录。"));
        return false;
    }
    QString error;
    if (!WorldFiles::copyWorldInto(worldDir, savesDir, name, &error)) {
        InlineMessage::critical(m_overlay, tr("导入世界失败"), error);
        return false;
    }
    log(tr("导入世界：%1 → %2").arg(QDir::toNativeSeparators(worldDir),
                                      QDir::toNativeSeparators(savesDir)));
    return true;
}

void InstanceTransferController::importWorldInBackground(
    const QString &instanceId, const QString &worldDir, const QString &name,
    std::function<void(bool, const QString &)> whenDone) {
    const auto reject = [this, &whenDone](const QString &problem) {
        if (whenDone) whenDone(false, problem);
        InlineMessage::warning(m_overlay, tr("导入世界失败"), problem);
    };
    if ((m_launcher && m_launcher->isVersionRunning(instanceId)) ||
        (m_host.isBeingModified && m_host.isBeingModified(instanceId))) {
        reject(tr("实例正在运行或处理文件，完成后再导入存档。"));
        return;
    }
    const QString savesDir = m_host.instanceDirForKey
        ? m_host.instanceDirForKey(instanceId, QStringLiteral("save")) : QString();
    if (savesDir.isEmpty()) {
        reject(tr("无法识别实例的存档目录。"));
        return;
    }
    // 同名冲突（2.1.2）：默认「取消」，另一个选项是「跳过已有项」。
    // 世界存档**没有“覆盖”**这一档 —— 覆盖一个世界等于把用户进度静默换掉。
    OperationReport::ConflictPolicy policy = OperationReport::ConflictPolicy::Cancel;
    if (QFileInfo::exists(savesDir + QLatin1Char('/') + name.trimmed())) {
        const int choice = InlineMessage::ask(
            m_overlay, MessageKind::Question, tr("已经有一个同名的世界"),
            tr("此实例中已存在世界「%1」。\n\n选择「跳过已有项」会保留原存档。")
                .arg(name.trimmed()),
            {tr("取消"), tr("跳过已有项")}, 0, -1, false);
        if (choice != 1) {
            if (whenDone) whenDone(false, tr("已取消导入世界。"));
            return;
        }
        policy = OperationReport::ConflictPolicy::SkipExisting;
    }
    const QString dataRoot = AppPaths::minecraftDir();
    const QString reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 暂存必须和目标在**同一个卷**上：发布是 `QDir::rename()`（同卷改名），
    // 跨卷会失败 —— 而数据根和 saves 在正常产品里本来就在一起，测试/换根时才可能分开。
    const QString stagingDir = savesDir + QStringLiteral("/.wtfml-world-staging/") + reportId;
    // Kind::Import 而不是 Other：账本按种类给的默认能力位里只有三类能取消，
    // 「导入世界」本来就是其中的一类 —— 手动取消那颗按钮（见 cancelTask()）靠它才露出来。
    const auto task = m_host.beginTask
        ? m_host.beginTask(Tasks::TaskKind::Import, tr("导入世界：%1").arg(name), instanceId,
            {{tr("复制世界文件到暂存"), Tasks::TaskStepState::Running, {}},
             {tr("校验并发布世界"), Tasks::TaskStepState::Pending, {}}})
        : Tasks::invalidTaskId();
    const auto state = std::make_shared<CancelState>();
    if (task != Tasks::invalidTaskId()) m_cancelStates.insert(task, state);
    if (m_host.setTaskDetail) m_host.setTaskDetail(task, tr("正在后台导入存档，请勿修改源世界。"));
    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, true);
    struct Progress {
        std::atomic<int> done{0};
        std::atomic<int> total{0};
        std::atomic<qint64> copiedBytes{0};
        std::atomic<qint64> totalBytes{0};
    };
    const auto progress = std::make_shared<Progress>();
    auto *timer = new QTimer(this);
    timer->setInterval(150);
    connect(timer, &QTimer::timeout, this, [this, task, progress] {
        if (m_host.setTaskProgress) {
            m_host.setTaskProgress(task, progress->done.load(), progress->total.load());
        }
        if (m_host.setTaskBytes) {
            m_host.setTaskBytes(task, progress->copiedBytes.load(),
                                progress->totalBytes.load());
        }
    });
    timer->start();
    auto *watcher = new QFutureWatcher<WorldFiles::ImportResult>(this);
    connect(watcher, &QFutureWatcher<WorldFiles::ImportResult>::finished, this,
        [this, watcher, timer, instanceId, savesDir, worldDir, name, task, state, progress,
         dataRoot, reportId, stagingDir, policy, whenDone = std::move(whenDone)] {
            timer->stop();
            timer->deleteLater();
            const auto result = watcher->result();
            watcher->deleteLater();
            if (task != Tasks::invalidTaskId()) m_cancelStates.remove(task);
            if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, false);
            // 「用户按了取消」只在**真的没做成**时才算数：复制已经完整跑完、发布也成功了，
            // 那一下点晚了 —— 该报成功（存档确实在那儿），不能把它记成取消。
            const bool cancelledByUser =
                state->cancelledByUser.load(std::memory_order_relaxed) && !result.ok;
            // ---------- 统一报告 + 恢复入口（2.1.2） ----------
            OperationReport::Report report;
            report.id = reportId;
            report.kind = OperationReport::Kind::WorldImport;
            report.title = tr("导入世界：%1").arg(name);
            report.instanceId = instanceId;
            report.targetRoot = savesDir;
            report.sourceRoot = worldDir;
            report.stagingDir = result.stagingDir;
            report.conflictPolicy = policy;
            report.totalFiles = result.totalFiles;
            report.totalBytes = result.totalBytes;
            report.doneFiles = result.copiedFiles + result.skippedFiles;
            report.doneBytes = result.copiedBytes;
            report.skippedFiles = result.skippedFiles;
            report.failedFiles = result.failedFiles;
            report.complete = result.ok;
            report.cancelled = cancelledByUser || result.cancelled;
            report.failureReason = result.problem;
            report.startedAt = QDateTime::currentDateTime();
            report.updatedAt = report.startedAt;
            report.context = QJsonObject{
                {QStringLiteral("dataRoot"), dataRoot},
                {QStringLiteral("worldDir"), worldDir},
                {QStringLiteral("savesDir"), savesDir},
                {QStringLiteral("name"), name},
            };
            report.entries.append(OperationReport::Entry{
                name, result.totalBytes,
                result.ok ? (result.skipped ? OperationReport::EntryState::Skipped
                                            : OperationReport::EntryState::Done)
                          : (cancelledByUser || result.cancelled
                                 ? OperationReport::EntryState::Pending
                                 : OperationReport::EntryState::Failed),
                result.problem});
            for (const QString &failure : result.failures) {
                const int split = failure.indexOf(QCoreApplication::translate("InstanceTransferController", "："));
                report.entries.append(OperationReport::Entry{
                    split > 0 ? failure.left(split) : failure, 0,
                    OperationReport::EntryState::Failed,
                    split > 0 ? failure.mid(split + 1) : QString()});
            }
            QString recoveryToken;
            QString failureReport;
            const bool unfinished = !result.ok || result.cancelled || cancelledByUser;
            if (unfinished && m_host.registerRecovery && m_host.registerRecovery(report)) {
                recoveryToken = reportId;
                // 一开始就知道做不完时也留一份“为什么 + 怎么办”，不必等用户去按
                // 「继续未完成操作」才发现原因。
                failureReport = OperationReport::writeFailureText(
                    OperationReport::reportPath(dataRoot, reportId), report);
            }
            const QString recoveryTokenFinal = recoveryToken;
            const QString failureReportFinal = failureReport;
            if (m_host.setTaskStepState) {
                const bool copyDone = result.copiedFiles + result.skippedFiles > 0
                                      || result.totalFiles == 0;
                m_host.setTaskStepState(task, 0, copyDone ? Tasks::TaskStepState::Done
                                                          : Tasks::TaskStepState::Failed);
                if (result.ok) {
                    m_host.setTaskStepState(task, 1, Tasks::TaskStepState::Done);
                }
            }
            if (cancelledByUser) {
                // 取消不是失败：账本收成 Cancelled（不亮红点），也不弹失败提示。
                if (m_host.markTaskCancelled) m_host.markTaskCancelled(task);
                log(tr("导入世界已取消：%1（已拷好的暂存保留，可继续）").arg(name));
            } else {
                if (m_host.finishTask) m_host.finishTask(task, result.ok, result.problem);
                log(result.ok ? tr("导入世界：%1 → %2（%3）")
                                    .arg(worldDir, savesDir, report.summaryText())
                              : tr("导入世界未完成：%1（%2）").arg(name, result.problem));
            }
            // 「继续未完成操作」只能挂在**已经收尾**的那一项上 —— 必须排在收尾之后。
            if (m_host.setTaskRecovery) m_host.setTaskRecovery(task, recoveryTokenFinal);
            if (m_host.setTaskDetail && task != Tasks::invalidTaskId()) {
                QString detailText = report.summaryText();
                if (!result.problem.isEmpty()) detailText += QStringLiteral("\n") + result.problem;
                if (!failureReportFinal.isEmpty()) {
                    detailText += QStringLiteral("\n") + tr("原因报告：%1").arg(failureReportFinal);
                }
                m_host.setTaskDetail(task, detailText);
            }
            if (m_host.refreshInstances) m_host.refreshInstances();
            if (whenDone) {
                whenDone(result.ok, cancelledByUser ? tr("已取消，未发布的暂存保留着（可以继续）。")
                                                    : result.problem);
            }
            if (!result.ok && !cancelledByUser) {
                InlineMessage::warning(m_overlay, tr("导入世界失败"), result.problem);
            }
        });
    // 不在 worker 读取 AppPaths / 当前实例选择，也不捕获控制器或页面。
    const auto destroyed = connect(this, &QObject::destroyed, [state] {
        state->abort.store(true, std::memory_order_relaxed);
    });
    connect(watcher, &QFutureWatcher<WorldFiles::ImportResult>::finished, watcher, [destroyed] {
        QObject::disconnect(destroyed);
    });
    WorldFiles::ImportOptions options;
    options.conflictPolicy = policy;
    options.stagingDir = stagingDir;
    watcher->setFuture(QtConcurrent::run(
        [worldDir, savesDir, name, options, state, progress] {
            QElapsedTimer elapsed;
            elapsed.start();
            return WorldFiles::importWorld(
                worldDir, savesDir, name, options,
                [&] {
                    return state->abort.load(std::memory_order_relaxed)
                           || elapsed.elapsed() >= 600000;
                },
                [progress](int done, int total, qint64 bytes, qint64 totalBytes) {
                    progress->done.store(done);
                    progress->total.store(total);
                    progress->copiedBytes.store(bytes);
                    progress->totalBytes.store(totalBytes);
                });
        }));
}


void InstanceTransferController::deleteWorldInBackground(
    const QString &instanceId, const QString &worldDir,
    std::function<void(bool, const QString &)> whenDone) {
    const auto reject = [this, &whenDone](const QString &problem) {
        if (whenDone) whenDone(false, problem);
        InlineMessage::warning(m_overlay, tr("删除失败"), problem);
    };
    if (worldDir.isEmpty()) {
        reject(tr("未找到要删除的世界存档。"));
        return;
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        reject(lockProblem);
        return;
    }
    const QString saves = AppPaths::versionDir(instanceId) + QStringLiteral("/saves");
    if (instanceId.isEmpty() || instanceId.contains('/') || instanceId.contains('\\')
        || instanceId == QLatin1String(".") || instanceId == QLatin1String("..")
        || InstanceFiles::canonicalPath(QFileInfo(worldDir).absolutePath()) != InstanceFiles::canonicalPath(saves)) {
        reject(tr("要删除的目录不是当前实例的直接存档目录，未执行删除。"));
        return;
    }
    // 用户确认那一问和真正开删之间隔着一次事件循环：这中间游戏可能刚被启动。
    // 游戏退出（或者在游戏里存一次）的时候会把整个世界重新写一遍，而那时目录已经
    // 没了 —— 所以和 `InstanceDialogController` 里那道闸同一条理由，这里**再查一次**。
    if (!instanceId.isEmpty() && m_launcher && m_launcher->isVersionRunning(instanceId)) {
        reject(tr("游戏正在运行，请退出游戏后再删除此存档。"));
        return;
    }
    if (m_host.isBeingModified && m_host.isBeingModified(instanceId)) {
        const QString who =
            m_host.displayName ? m_host.displayName(instanceId) : instanceId;
        reject(tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），等它结束之后再删这个世界。")
                   .arg(who));
        return;
    }
    const QString worldName = QFileInfo(worldDir).fileName();
    // Kind::Other（不是 Import）：删除**不提供取消** —— 删到一半停下留下的是半截世界，
    // 比删完更难收拾。Other 的默认能力位里本来就没有「取消」，和这里的语义对得上。
    const auto task = m_host.beginTask
        ? m_host.beginTask(Tasks::TaskKind::Other, tr("删除世界：%1").arg(worldName), instanceId,
            {{tr("删除世界文件"), Tasks::TaskStepState::Running, {}}})
        : Tasks::invalidTaskId();
    // 删除**不提供取消**：删到一半停下留下的是半截世界，比删完更难收拾。原因写进
    // 能力位，详情页会摆出来 —— 用户至少知道"不是我点不了，是这一项不支持"。
    if (m_host.setTaskCapabilities) {
        Tasks::TaskCapabilities caps;
        caps.cancelBlockedReason =
            tr("删除到一半停下会留下不完整的世界，比删完更难收拾；这一项不支持取消。");
        m_host.setTaskCapabilities(task, caps);
    }
    if (m_host.setTaskDetail) m_host.setTaskDetail(task, tr("正在删除世界文件。"));
    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, true);
    auto *watcher = new QFutureWatcher<CopyOutcome>(this);
    connect(watcher, &QFutureWatcher<CopyOutcome>::finished, this,
        [this, watcher, instanceId, worldDir, task, whenDone = std::move(whenDone)] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, false);
            // 删不干净时的说法要回答"为什么没删掉 + 接下来做什么"。
            const QString error =
                result.ok
                    ? QString()
                    // 原因为什么由 `moveToRecycleBin()` 给全（权限 / 被占着 / 回收站本身）；
                    // 这里不替它补「先把游戏关掉再试」—— 权限不够时那句话是错的。
                    : tr("移入回收站失败：「%1」。\n\n%2")
                          .arg(QDir::toNativeSeparators(worldDir), result.error);
            if (m_host.setTaskStepState) m_host.setTaskStepState(task, 0,
                result.ok ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Failed);
            if (m_host.finishTask) m_host.finishTask(task, result.ok, error);
            if (m_host.refreshInstances) m_host.refreshInstances();
            log(result.ok ? tr("删除世界：%1").arg(QDir::toNativeSeparators(worldDir))
                          : tr("删除世界失败：%1").arg(QDir::toNativeSeparators(worldDir)));
            if (whenDone) whenDone(result.ok, error);
            if (!result.ok) InlineMessage::warning(m_overlay, tr("删除失败"), error);
        });
    // 工人只碰路径：不读 AppPaths、不碰当前实例选择、不捕获控制器或页面。
    watcher->setFuture(QtConcurrent::run([worldDir] {
        CopyOutcome result;
        // 走回收站（不是永久删除），并且不追 junction / 符号链接 —— 两条都收在
        // `moveToRecycleBin()` 里（见 FolderManager.hpp）。它自己会在这个线程上补一次
        // COM 初始化。
        result.ok = moveToRecycleBin(worldDir, &result.error);
        return result;
    }));
}

bool InstanceTransferController::canCancelTask(Tasks::TaskId id) const {
    return id != Tasks::invalidTaskId() && m_cancelStates.contains(id);
}

bool InstanceTransferController::cancelTask(Tasks::TaskId id) {
    const auto it = m_cancelStates.constFind(id);
    if (it == m_cancelStates.cend()) {
        return false;
    }
    // cancelledByUser 先落：工人看到 abort 就返回，收尾那一格读的就是这两位的先后。
    (*it)->cancelledByUser.store(true, std::memory_order_relaxed);
    (*it)->abort.store(true, std::memory_order_release);
    // 说清"取消之后会怎样"：导入是没拷完的不发布，升级 / 回滚是原实例不变。
    log(tr("已请求取消。当前项目将在安全时停止，未完成的数据不会写入目标。"));
    return true;
}

bool InstanceTransferController::renameWorld(const QString &instanceId,
                                             const QString &worldDir,
                                             const QString &newName) {
    if (m_launcher->isVersionRunning(instanceId)) {
        InlineMessage::warning(m_overlay, tr("重命名世界"),
                               tr("游戏正在运行，请退出游戏后再重命名存档。"));
        return false;
    }
    // 这一份实例正在被别的作业占着（快照 / 世界备份 / 还原 / 复制 / 安装）
    // 时不许改世界目录的名字。那些作业按**改名前算好的绝对路径**逐个读整棵目录树，
    // 目录在扫描途中被改名，它们要么静默少读一批文件、要么把结果写到一半就找不到落点，
    // 收尾却照样报成功 —— 一份看着成功、其实是残的备份比失败更糟。
    if (m_host.isBeingModified && m_host.isBeingModified(instanceId)) {
        InlineMessage::warning(m_overlay, tr("重命名世界"),
                               tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），等它结束之后再改这个世界。")
                                   .arg(displayName(instanceId)));
        return false;
    }
    QString error;
    if (!WorldFiles::renameWorld(worldDir, newName, &error)) {
        InlineMessage::warning(m_overlay, tr("重命名世界"), error);
        return false;
    }
    log(tr("世界改名：%1 → %2").arg(QDir::toNativeSeparators(worldDir), newName.trimmed()));
    return true;
}

