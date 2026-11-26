#include "controllers/CrashReportController.hpp"
#include "TextMarks.hpp"

#include "controllers/ActivityCenterController.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "util/AppPaths.hpp"
#include "util/ErrorReport.hpp"
#include "util/SystemMemory.hpp"
#include "settings/GlobalJavaSettings.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <utility>

CrashReportController::CrashReportController(ActivityCenterController *activity,
                                             OverlayHost *overlay, QWidget *dialogParent,
                                             Host host, QObject *parent)
    : QObject(parent), m_activity(activity), m_overlay(overlay),
      m_dialogParent(dialogParent), m_host(std::move(host)) {}

void CrashReportController::log(const QString &message) const {
    if (m_host.log) m_host.log(message);
}

int CrashReportController::lineCount(const QString &text) {
    if (text.isEmpty()) return 0;
    int count = text.count(QLatin1Char('\n'));
    return count + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);
}

QString CrashReportController::lastLines(const QString &text, int maxLines) {
    if (text.isEmpty() || maxLines <= 0) return {};
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int total = !lines.isEmpty() && lines.last().isEmpty() ? lines.size() - 1 : lines.size();
    if (total <= maxLines) return text;
    return lines.mid(total - maxLines, maxLines).join(QLatin1Char('\n')) + QLatin1Char('\n');
}

CrashReportController::Record CrashReportController::buildRecord(
    int sessionId, int exitCode, bool crashed, const LaunchFacts &launch,
    const QString &errorLog, const QString &fullLog) const {
    Record record;
    record.sessionId = sessionId;
    record.facts.exitCode = exitCode;
    record.facts.crashed = crashed;
    record.facts.instanceId = launch.instanceId;
    record.facts.instanceName = launch.instanceName;
    record.facts.requiredJavaMajor = launch.requiredJavaMajor;
    record.facts.javaPath = launch.javaPath;
    record.facts.xmxMb = launch.xmxMb;
    record.facts.xmsMb = launch.xmsMb;
    record.facts.loaderName = launch.loaderName;
    record.facts.loaderVersion = launch.loaderVersion;
    record.accountName = launch.accountName;
    record.processId = launch.processId;
    if (launch.startedAtMs > 0) record.startedAt = QDateTime::fromMSecsSinceEpoch(launch.startedAtMs);
    if (!launch.javaPath.isEmpty())
        record.facts.usedJavaMajor = GlobalJavaSettings::probeJavaMajorVersion(launch.javaPath);
    record.facts.totalMemoryMb = SystemMemory::read().totalMb;
    record.errorLines = lineCount(errorLog);
    record.totalLines = lineCount(fullLog);
    record.errorExcerpt = lastLines(errorLog, GameExitDiagnosis::kExcerptLineLimit);
    record.logTail = lastLines(fullLog, GameExitDiagnosis::kLogTailLineLimit);
    // 「导出完整日志」要的是整份，不是末尾 500 行；会话结束后日志窗和 LauncherManager
    // 的记录都不在了，只能在这里留一份。只在崩溃/GUI 退出时留，数量很小。
    record.fullLog = fullLog;
    record.facts.errorLineCount = record.errorLines;
    record.facts.logCaptured = !record.logTail.isEmpty();
    // 这两样用**整份日志**判，不用截过的 `logTail`。日志前部（加载器列前置、
    // 模组初始化早期）出现 `OutOfMemoryError` / `Missing or unsupported mandatory
    // dependencies` 时，末尾 500 行里通常一个字都不剩 —— 症状是"明明日志里有这句话，
    // 报告的「可能的原因」里却一条都不提"。展示可以截，判据不能截。
    record.facts.sawOutOfMemory = GameExitDiagnosis::logShowsOutOfMemory(fullLog);
    record.facts.sawMissingDependency = GameExitDiagnosis::logShowsMissingDependency(fullLog);
    record.facts.sawManualDebugCrash = GameExitDiagnosis::logShowsManualDebugCrash(fullLog);
    return record;
}

Tasks::TaskId CrashReportController::report(int sessionId, int exitCode, bool crashed,
                                            const LaunchFacts &launch,
                                            const QString &errorLog,
                                            const QString &fullLog) {
    for (Tasks::TaskId id : m_records.keys())
        if (!m_activity->tasks()->has(id)) m_records.remove(id);
    const Record record = buildRecord(sessionId, exitCode, crashed, launch, errorLog, fullLog);
    const QString where = record.facts.instanceName.isEmpty() ? record.facts.instanceId
                                                              : record.facts.instanceName;
    const QString title = where.isEmpty() ? tr("游戏异常退出") : tr("游戏异常退出 / %1").arg(where);
    const QStringList hints = GameExitDiagnosis::hints(record.facts);
    const QString exitText = GameExitDiagnosis::exitDescription(exitCode);
    QList<Tasks::TaskStep> steps{
        {tr("收集退出信息"), Tasks::TaskStepState::Done, exitText},
        {tr("比对已知原因"), Tasks::TaskStepState::Done,
         hints.isEmpty() ? tr("未匹配到已知原因") : tr("%1 条").arg(hints.size())}};
    const Tasks::TaskId id = m_activity->begin(Tasks::TaskKind::Crash, title,
                                               record.facts.instanceId, steps);
    if (id == Tasks::invalidTaskId()) return id;
    if (sessionId > 0) m_activity->bindSession(id, sessionId);
    m_activity->tasks()->setDetail(id, tr("退出码 %1").arg(exitText));
    m_records.insert(id, record);
    QString summary = GameExitDiagnosis::summary(record.facts);
    if (!hints.isEmpty()) summary += QStringLiteral("\n\n") + hints.join(QLatin1Char('\n'));
    m_activity->finish(id, false, summary, false);
    // 账本里那一条照旧记着；这里再给一张人可以直接读的卡片。排到下一轮，
    // 不在游戏进程 finished 的信号栈里开嵌套事件循环。
    QTimer::singleShot(0, this, [this, id] { showCrashDialog(id); });
    return id;
}

QSet<Tasks::TaskId> CrashReportController::recordIds() const {
    return QSet<Tasks::TaskId>(m_records.keyBegin(), m_records.keyEnd());
}

QSet<Tasks::TaskId> CrashReportController::pendingRecordIds() const {
    // 判据只有"账本里还有没有这个号"一条（理由写在头文件那段）。**不在这里删 m_records**：
    // 这个函数是 const、每个进度帧都可能被调到（底栏），而清理那一趟归 report() ——
    // 两处都删会让"谁负责清"这件事没有出处。
    QSet<Tasks::TaskId> ids;
    for (Tasks::TaskId id : m_records.keys())
        if (m_activity->tasks()->has(id)) ids.insert(id);
    return ids;
}

QString CrashReportController::latestPendingInstanceName() const {
    const QSet<Tasks::TaskId> pending = pendingRecordIds();
    Tasks::TaskId latest = Tasks::invalidTaskId();
    for (Tasks::TaskId id : pending)
        if (id > latest) latest = id;
    if (latest == Tasks::invalidTaskId()) return QString();
    // 名字可能有空的那一天（记录里那份是从启动那一刻的事实抄来的，理论上非空）——
    // 空的时候退回实例 id，别让第二行变成一句空白（那看着像界面坏了）。
    const Record record = m_records.value(latest);
    return record.facts.instanceName.isEmpty() ? record.facts.instanceId
                                              : record.facts.instanceName;
}

bool CrashReportController::hasRecord(Tasks::TaskId id) const { return m_records.contains(id); }

int CrashReportController::sessionIdForTask(Tasks::TaskId id) const {
    const auto it = m_records.constFind(id);
    return it == m_records.cend() ? 0 : it->sessionId;
}

QString CrashReportController::exportDirectory() {
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return desktop.isEmpty() ? QDir::homePath() : desktop;
}

QString CrashReportController::writeExportFile(const QString &path, const QString &text) {
    QString body = text;
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const QByteArray bytes = body.toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return file.errorString();
    if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
    return {};
}

void CrashReportController::showCrashDialog(Tasks::TaskId id, int overlayRetry) {
    // 验证工程会主动制造崩溃；那里只量账本，不等人点卡片。
    if (qEnvironmentVariableIntValue("WTFML_SUPPRESS_CRASH_DIALOG") == 1
        || (QCoreApplication::instance() != nullptr
            && QCoreApplication::instance()->property("wtfml.suppressCrashDialog").toBool())) {
        return;
    }
    const auto found = m_records.constFind(id);
    if (found == m_records.cend()) {
        return;
    }
    // 崩溃时用户可能正停在任务中心/实例设置上。提示不能叠在整页上，也不能因此丢掉；
    // 等覆盖层空出来再摆（最多约 10 秒，之后照旧记在任务账本里）。
    if (m_overlay != nullptr && m_overlay->isOpen()) {
        if (overlayRetry < 40) {
            QTimer::singleShot(250, this, [this, id, overlayRetry] {
                showCrashDialog(id, overlayRetry + 1);
            });
        }
        return;
    }
    const Record record = found.value();
    const QString where = record.facts.instanceName.isEmpty()
                              ? record.facts.instanceId
                              : record.facts.instanceName;
    const QStringList hints = GameExitDiagnosis::hints(record.facts);
    QString text = tr("「%1」异常退出：%2。")
                       .arg(where, GameExitDiagnosis::exitDescription(record.facts.exitCode));
    if (hints.isEmpty()) {
        text += tr("\n\n未从退出码或日志中识别出明确原因。可导出报告，或将日志交给智能体分析。");
    } else {
        text += tr("\n\n可能的原因与建议：");
        for (const QString &hint : hints) {
            text += QStringLiteral("\n• ") + hint;
        }
    }
    text += tr("\n\n如为偶发，可先重启游戏；反复出现时，建议先按上述提示处理，再导出报告或完整日志。");

    QString context = tr("实例：%1\n退出码：%2\n\n").arg(where, GameExitDiagnosis::exitDescription(record.facts.exitCode));
    context += hints.isEmpty() ? tr("日志中未识别出明确原因。") : hints.join(QLatin1Char('\n'));
    if (context.size() > 6000) context = context.left(6000) + tr("\n…（已截断）");

    for (;;) {
        InlineMessageBox box(MessageKind::Warning, tr("游戏已崩溃"), text,
                             {tr("确定"), tr("导出错误报告"), tr("导出完整日志"),
                              tr("使用智能体询问")},
                             0, -1);
        m_overlay->execPrompt(&box);
        const int clicked = box.clickedIndex();
        if (clicked <= 0) return;
        if (clicked == 1) {
            exportReport(QString::number(id));
            continue;
        }
        if (clicked == 2) {
            exportFullLog(QString::number(id));
            continue;
        }
        if (clicked == 3) {
            if (m_host.askAgent) m_host.askAgent(context, record.facts.instanceId);
            return;
        }
    }
}

void CrashReportController::exportFullLog(const QString &key) {
    bool parsed = false;
    const Tasks::TaskId id = key.toULongLong(&parsed);
    const auto found = parsed ? m_records.constFind(id) : m_records.cend();
    if (found == m_records.cend()) {
        InlineMessage::warning(m_overlay, tr("无完整日志"),
                               tr("本次退出未留下可导出的完整日志。"));
        return;
    }
    const Record record = found.value();
    // 真正完整的那一份在实例目录的 logs/latest.log；进程刚退出时优先拿它。
    // 拿不到（日志关着 / 被移到别处 / 实例已删）才退回会话期间留在内存里的全文。
    QString completeLog = record.fullLog;
    const QString latestLog = QDir(AppPaths::versionDir(record.facts.instanceId))
                                  .filePath(QStringLiteral("logs/latest.log"));
    QFile gameLog(latestLog);
    if (gameLog.open(QIODevice::ReadOnly) && gameLog.size() > 0) {
        const QByteArray bytes = gameLog.readAll();
        if (!bytes.isEmpty()) completeLog = QString::fromUtf8(bytes);
    }
    if (completeLog.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("无完整日志"),
                               tr("本次退出未留下可导出的完整日志。"));
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const QString suggested = QDir(exportDirectory())
                                  .filePath(ErrorReport::suggestedLogFileName(
                                      record.facts.instanceName, now));
    const QString path = QFileDialog::getSaveFileName(
        m_dialogParent, tr("导出完整日志"), suggested,
        tr("日志文件 (*.log *.txt);;所有文件 (*)"));
    if (path.isEmpty()) return;
    const QString error = writeExportFile(path, completeLog);
    if (!error.isEmpty()) {
        log(tr("导出完整日志失败：%1").arg(error));
        InlineMessage::warning(m_overlay, tr("导出失败"),
                               tr("无法写入文件（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), error));
        return;
    }
    log(TextMarks::done(tr("完整日志已导出：%1").arg(QDir::toNativeSeparators(path))));
}

void CrashReportController::exportReport(const QString &key) {
    bool parsed = false;
    // 行钥匙是 `TaskId`（64 位，见 `ActivityCenterController::markViewed()` 那段的 NOTE）。
    const Tasks::TaskId id = key.toULongLong(&parsed);
    const auto found = parsed ? m_records.constFind(id) : m_records.cend();
    if (found == m_records.cend()) return;
    // 这里必须是**一份拷贝**，不能是 `m_records` 里那个引用。下面
    // `QFileDialog::getSaveFileName()` 是一层嵌套事件循环，这期间别处再报一次异常退出
    // （游戏正好崩了）就会往同一个 `QHash` 里插、可能 rehash ⇒ 那个引用回头就是野的，
    // 而它还要被用来拼报告正文（`record.errorExcerpt` / `record.logTail`）。
    const Record record = found.value();
    ErrorReport::Facts facts;
    facts.launcherVersion = QCoreApplication::applicationVersion();
    facts.instanceName = record.facts.instanceName;
    facts.instanceId = record.facts.instanceId;
    facts.accountName = record.accountName;
    facts.systemDescription = ErrorReport::systemDescription();
    facts.processId = record.processId;
    facts.startedAt = record.startedAt;
    facts.finished = true;
    facts.crashed = record.facts.crashed;
    facts.exitCode = record.facts.exitCode;
    facts.errorLines = record.errorLines;
    facts.totalLines = record.totalLines;
    // 截了多少要**告诉报告**。下面递过去的是末尾 200 / 500 行，标题若照样写
    // 「共 N 行」，玩家把一份 8000 行的日志导出、发给别人排查时，前面真正的起因
    // 已经不在文件里，而文件自己说它是完整的。（用户按「导出错误报告」可能是很久以后，
    // 那时会话底账早就删了 —— 这一份内存里的记录是唯一还能说明"截过"的地方，所以判据
    // 留在这里、从记录里那两个总数和实际留下来的行数比出来。）
    facts.errorTruncated = record.errorLines > lineCount(record.errorExcerpt);
    facts.logTruncated = record.totalLines > lineCount(record.logTail);
    if (record.facts.usedJavaMajor > 0 && !record.facts.javaPath.isEmpty())
        facts.javaVersion = tr("%1（%2）").arg(record.facts.usedJavaMajor).arg(record.facts.javaPath);
    else if (record.facts.usedJavaMajor > 0)
        facts.javaVersion = QString::number(record.facts.usedJavaMajor);
    else
        facts.javaVersion = record.facts.javaPath;
    if (!record.facts.loaderName.isEmpty())
        facts.loaderVersion = record.facts.loaderVersion.isEmpty()
                                  ? record.facts.loaderName
                                  : QStringLiteral("%1 %2").arg(record.facts.loaderName,
                                                                 record.facts.loaderVersion);
    if (record.facts.xmxMb > 0 || record.facts.xmsMb > 0) {
        facts.memoryArgs = tr("-Xmx %1 MB / -Xms %2 MB").arg(record.facts.xmxMb).arg(record.facts.xmsMb);
        if (record.facts.totalMemoryMb > 0)
            facts.memoryArgs += tr("（本机共 %1 MB）").arg(record.facts.totalMemoryMb);
    }
    facts.hints = GameExitDiagnosis::hints(record.facts);
    const QDateTime now = QDateTime::currentDateTime();
    const QString suggested = QDir(exportDirectory()).filePath(
        ErrorReport::suggestedFileName(record.facts.instanceName, now));
    const QString path = QFileDialog::getSaveFileName(m_dialogParent, tr("导出错误报告"),
                                                      suggested, tr("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) return;
    const QString error = writeExportFile(path, ErrorReport::text(
        facts, now, record.errorExcerpt, record.logTail));
    if (!error.isEmpty()) {
        log(tr("导出错误报告失败：%1").arg(error));
        InlineMessage::warning(m_overlay, tr("导出失败"),
                               tr("无法写入文件（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), error));
        return;
    }
    log(TextMarks::done(tr("错误报告已导出：%1").arg(QDir::toNativeSeparators(path))));
}

void CrashReportController::openReportsDirectory() {
    const QString dir = AppPaths::crashReportsDir();
    if (!QDir().exists(dir)) {
        InlineMessage::warning(m_overlay, tr("暂无崩溃报告"),
            tr("未在 %1 中找到崩溃报告。\n\n进程被强制终止或在生成报告前退出时，可能未生成报告。")
                .arg(QDir::toNativeSeparators(dir)));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
