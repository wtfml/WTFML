/**
 * @file MainWindowSessionExport.cpp
 * @brief 把"一次会话的现场"留下 / 交出去：日志窗那两颗导出、异常退出的取证、启动脚本与整合包导出。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"刚刚那一次到底发生了什么"：`reportGameCrash()`（崩溃之后把这一趟的启动
 * 事实与日志摆成 `CrashReportController` 要的那一份）、`exportCrashReport()` /
 * `exportGameLog()` / `exportGameErrorReport()` / `exportLaunchScript()` / `exportModpack()`。
 * 它只在"已经结束或正在出问题"的时候被调用，和启动 / 实例管理那些常驻路径不重叠 ——
 * 分出来，取证相关的东西（默认存哪儿、哪些字节要涂掉）只在一个文件里审。
 *
 * 【保存框为什么在这一层】
 *
 * `ui/` 不认识数据目录（那一层的规矩，见 `GameLogWindow` 的文件头），而"默认存哪儿"是
 * 一个和数据目录同级的问题。所以匿名命名空间里那两个默认目录助手跟着这一片走。
 *
 * 【判据不在这一层】
 *
 * 会话事实留在 `GameSessionController`（会话一结束，`LauncherManager` 那份记录和日志窗里
 * 的两段日志就都没了，所以必须在活的时侯留）；报告文件格式在 `CrashReportController` /
 * `ErrorReport`；涂凭据在 `util/SecretRedactor`。
 */

#include "MainWindow.hpp"

#include "TextMarks.hpp"
#include "..\ui\GameLogWindow.hpp"
#include "..\ui\InlineMessage.hpp"
#include "auth/AuthManager.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/GameSessionController.hpp"
#include "controllers/ModpackExportController.hpp"
#include "manage/InstanceExport.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/ModpackExport.hpp"
#include "launcher/LaunchScriptExport.hpp"
#include "manage/VersionManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"
#include "util/ErrorReport.hpp"
#include "util/LaunchScriptCredentials.hpp"
#include "util/SecretRedactor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QFileDialog>
#include <QIODevice>
#include <QSaveFile>
#include <QStandardPaths>

// ============================ 日志窗那两颗导出 ============================
//
// 为什么保存框在这一层而不是窗那一层：`ui/` 不认识数据目录（那一层的规矩，见
// GameLogWindow 的文件头），而"默认存哪儿"是个和数据目录一样属于这一层的问题。

namespace {

/// 导出那两份文件时保存框默认落在哪儿：桌面。
///
/// 退路是"我的文档"，再拿不到就给个空串（空串 = 交给保存框自己决定用上次那个地方）——
/// 比硬编一个绝对路径强：那台机器上不一定有那个目录。
QString exportDefaultDirectory() {
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!desktop.isEmpty()) {
        return desktop;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

/**
 */
int lineCountOf(const QString &text) {
    if (text.isEmpty()) {
        return 0;
    }
    int count = text.count(QLatin1Char('\n'));
    if (!text.endsWith(QLatin1Char('\n'))) {
        ++count;
    }
    return count;
}

/**
 * @brief 把一段文本写进那个文件。
 * @return 空串 = 写成功了；否则是给用户看的原因（原样来自系统，已经能读懂）。
 */
QString writeExportFile(const QString &path, const QString &text) {
    // 换行写成 CRLF：这两份文件的主要用途是在 Windows 上双击打开（记事本）——
    // 纯 LF 在旧一点的记事本里是一整坨。**不改内存里那一份**，只改写出去的那一份。
    QString body = SecretRedactor::redact(text);
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const QByteArray bytes = body.toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return file.errorString();
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        return file.errorString();
    }
    return QString();
}

}  // namespace

// ========================= 游戏异常退出 =========================

void MainWindow::reportGameCrash(int sessionId, int exitCode, bool crashed) {
    // 这一趟的启动事实和那份日志都**在会话还活着的时候**留在了控制器那边（会话一结束，
    // LauncherManager 那份记录和日志窗里的两段日志就都没了），这里只把它们摆成
    // `CrashReportController` 要的那一份。
    const GameSessionController::GameSessionFacts launch =
        m_gameSessionController->sessionFacts(sessionId);
    CrashReportController::LaunchFacts facts;
    facts.instanceId = launch.instanceId;
    facts.instanceName = launch.instanceName;
    facts.accountName = launch.accountName;
    facts.processId = launch.processId;
    facts.startedAtMs = launch.startedAtMs;
    facts.requiredJavaMajor = launch.requiredJavaMajor;
    facts.javaPath = launch.javaPath;
    facts.xmxMb = launch.xmxMb;
    facts.xmsMb = launch.xmsMb;
    facts.loaderName = launch.loaderName;
    facts.loaderVersion = launch.loaderVersion;
    QString errors;
    QString all;
    if (const GameLogWindow *window = m_gameSessionController->gameLogWindow(sessionId)) {
        errors = window->logTextFrom(GameLogWindow::LogLevel::Error);
        all = window->allLogText();
    } else {
        // **没有日志窗才是常态** ——「显示游戏日志」默认关
        // （`LauncherSettings::m_showGameLog = false`），用户也可以把窗随手关掉。
        // 窗不在时读控制器那边留的每份会话最近 4000 行原文（见 sessionLogTail 的说明）；
        // 窗在的时候仍然读窗（那边留 20000 行，还带着没换行的半行）—— 这份报告正是给崩溃看的。
        errors = m_gameSessionController->sessionLogTailErrors(sessionId);
        all = m_gameSessionController->sessionLogTail(sessionId);
    }
    m_crashReports->report(sessionId, exitCode, crashed, facts, errors, all);
}

void MainWindow::exportCrashReport(const QString &key) {
    m_crashReports->exportReport(key);
}

void MainWindow::openCrashReportsDir(const QString &key) {
    Q_UNUSED(key);
    m_crashReports->openReportsDirectory();
}
void MainWindow::exportGameLog(int sessionId) {
    GameLogWindow *window = m_gameSessionController->gameLogWindow(sessionId);
    if (window == nullptr) {
        return;  // 走不到：这两颗键就长在那个窗上
    }
    const QDateTime now = QDateTime::currentDateTime();
    // 窗口是**带 WA_DeleteOnClose 的**（见 `GameSessionController.hpp` 的
    // 说明），而下面那个保存框是一层嵌套事件循环 —— 这期间游戏可能正好以 0 退出，
    // `autoCloseOnExit()` 会把这一扇 close() 掉、随之销毁。所以：要读的东西**在弹框
    // 之前**读好（那一份日志也正好是用户点这颗键那一刻看到的），框回来之后只用
    // `QPointer` 判一下还能不能写回那行"已导出"。
    const QString instanceName = window->instanceName();
    const QString logText = window->allLogText();
    QPointer<GameLogWindow> windowGuard(window);
    const QString suggested =
        QDir(exportDefaultDirectory())
            .filePath(ErrorReport::suggestedLogFileName(instanceName, now));
    // 保存框**挂在那个日志窗上**（不是主窗）：它是用户刚点过的那扇窗，弹出来的框该压着它，
    // 而不是被它盖住（日志窗没有父窗，是独立的顶层窗口，见它的文件头）。
    // NOTE: 不用 `DontUseNativeDialog` —— 这一颗是**导出到哪儿**，要的是那个熟悉的
    // Windows 框（四处 QFileDialog 里另外那几处也是这么留的，见 setupConnections 里
    // 那一段说明）。
    const QString path = QFileDialog::getSaveFileName(
        window, tr("导出日志"), suggested,
        tr("文本文件 (*.log *.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;  // 用户取消
    }
    const QString writeError = writeExportFile(path, logText);
    if (!writeError.isEmpty()) {
        appendLog(tr("导出日志失败：%1").arg(writeError));
        // 错误要弹窗（这一条是产品的硬约束）—— 弹的是启动器那一层，日志窗这时还在旁边
        // 摆着，用户回头看得到。
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("文件写入失败（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), writeError));
        return;
    }
    const QString shown = QDir::toNativeSeparators(path);
    appendLog(TextMarks::done(tr("日志已导出：%1").arg(shown)));
    // 成功那一下**写回那个日志窗**：用户就在那儿点的那颗键，眼睛也在那儿 —— 弹一个框
    // 还要他再点一次"知道了"，而这件事没有需要他决定的东西。写法跟着窗里已有的那几行
    // 自己补的话（"[游戏已退出，退出码：0]"），一样带方括号。
    // 窗在保存框开着的时候被自动关掉的话就不写了（文件已经导出成功，主日志里也有记录）。
    if (!windowGuard.isNull()) {
        windowGuard->appendText(tr("\n[日志已导出：%1]\n").arg(shown));
    }
}

void MainWindow::exportGameErrorReport(int sessionId) {
    GameLogWindow *window = m_gameSessionController->gameLogWindow(sessionId);
    if (window == nullptr) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    // 同上：窗带 WA_DeleteOnClose，保存框那一层嵌套循环里游戏可能以 0 退出、窗跟着没了。
    // 报告正文全在弹框之前读好；框回来之后只用 `QPointer` 决定要不要写回那行"已导出"。
    QPointer<GameLogWindow> windowGuard(window);

    const GameSessionController::GameLogFacts saved = m_gameSessionController->logFacts(sessionId);
    ErrorReport::Facts facts;
    facts.launcherVersion = QCoreApplication::applicationVersion();
    facts.instanceName = window->instanceName();
    facts.instanceId = saved.instanceId;
    facts.accountName = saved.accountName;
    facts.systemDescription = ErrorReport::systemDescription();
    facts.processId = saved.processId;
    facts.startedAt = saved.startedAtMs > 0 ? QDateTime::fromMSecsSinceEpoch(saved.startedAtMs)
                                            : QDateTime();
    facts.finished = saved.finished;
    facts.crashed = saved.crashed;
    facts.exitCode = saved.exitCode;

    // 两段正文都从窗里读**它自己那一份模型**：错误摘录走 `logTextFrom(Error)`（哪几档算
    // 错误只有那边一个出处），完整那份走 `allLogText()`（屏幕上筛掉的行也在里面 ——
    // 用户勾着 ERROR 的时候导出，报告里照样要是完整日志）。
    const QString errors = window->logTextFrom(GameLogWindow::LogLevel::Error);
    const QString log = window->allLogText();
    const int droppedErrors = window->droppedLineCountFor(GameLogWindow::LogLevel::Error);
    const int droppedAll = window->droppedLineCount();
    facts.errorLines = lineCountOf(errors) + droppedErrors;
    facts.totalLines = lineCountOf(log) + droppedAll;
    facts.errorTruncated = droppedErrors > 0;
    facts.logTruncated = droppedAll > 0;

    const QString suggested =
        QDir(exportDefaultDirectory())
            .filePath(ErrorReport::suggestedFileName(window->instanceName(), now));
    const QString path = QFileDialog::getSaveFileName(
        window, tr("导出错误报告"), suggested,
        tr("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    const QString writeError =
        writeExportFile(path, ErrorReport::text(facts, now, errors, log));
    if (!writeError.isEmpty()) {
        appendLog(tr("导出错误报告失败：%1").arg(writeError));
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("文件写入失败（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), writeError));
        return;
    }
    const QString shown = QDir::toNativeSeparators(path);
    appendLog(TextMarks::done(tr("错误报告已导出：%1").arg(shown)));
    if (!windowGuard.isNull()) {
        windowGuard->appendText(tr("\n[错误报告已导出：%1]\n").arg(shown));
    }
}

void MainWindow::exportLaunchScript(const QString &versionId, const QString &path) {
    if (versionId.isEmpty() || path.isEmpty()) {
        // 产品里走不到（对话框那边是"问出来的路径非空才算数"）。验证工程打一条空路径进来时
        // 什么都不做，比去写一个叫 "" 的文件强。
        return;
    }

    const QJsonObject versionJson = m_versionManager->getVersionJson(versionId);
    if (versionJson.isEmpty()) {
        appendLog(tr("导出启动脚本失败：找不到版本 %1 的 version.json").arg(versionId));
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("找不到版本 %1 的 version.json，导出的启动脚本无法运行。")
                                   .arg(versionId));
        return;
    }

    // 身份照**启动那条路**判（唯一出处 offlineIdentity()），令牌也一样要换 —— 这一份脚本
    // 要能直接跑起来，就得带着和启动那一刻同一个身份。
    const bool offlineMode = offlineIdentity();
    QString token;
    if (!offlineMode) {
        QString tokenError;
        // 这一段是同步的（要换令牌时它要在 UI 线程上发请求，单次超时 15 秒）—— 和启动一样
        // 先给一句话再进去。
        enterSyncWait(tr("正在检查登录状态…"));
        token = m_authManager->loadSavedToken(&tokenError);
        leaveSyncWait();
        if (token.isEmpty()) {
            // 导一份没有令牌的脚本 = 导一份起不来的脚本，所以这一档**不导**，说清为什么。
            appendLog(tr("导出启动脚本失败：%1").arg(tokenError));
            InlineMessage::warning(
                m_ui->overlay(), tr("导出失败"),
                tokenError.isEmpty()
                    ? tr("导出启动脚本要先能拿到登录令牌。请先登录微软账号，"
                         "或者在「账号与外观…」里添加一个离线账号。")
                    : tr("%1\n\n导出启动脚本要先能拿到登录令牌，请重新登录后再试。")
                          .arg(tokenError));
            return;
        }
    }

    // 这次要用的那一套 Java 值：和启动一样合成（全局 + 这个实例的覆盖），自动档的内存也在
    // 这一步按**这一刻**的机器状态定下来。
    // NOTE: 这里**不挑 Java、也不下运行时**（ensureJavaForVersion 那一整套）：用户点的是
    // "导出这个文件"，顺手在后台下几百 MB 的运行时不是他要的。路径空着就写裸 `java`，
    // 和 LauncherManager 那边"从 PATH 找"是同一个兜底。
    const JavaSettingsOverride override =
        javaOverrideFromJson(m_versionManager->versionMeta(versionId));
    EffectiveJavaSettings javaSettings =
        resolveJavaSettings(m_javaSettings->effective(), override);
    resolveMemoryForLaunch(versionId, javaSettings, override);

    // 正文（引号规矩、`%` 写两遍、本地代码页那句注释、令牌换成环境变量引用、拼完再过一道
    // 已知凭据）归 `LaunchScriptExport::compose()` —— 那些规矩错一个字节都要等用户双击脚本
    // 才看得见，那边验证工程能逐条钉住。这一层只凑值：数据目录、Java 程序、和启动器这一趟
    // 同一份参数、以及这个身份。返回空串 = 参数里带着已知凭据，这一份不写。
    LaunchScriptExport::Input scriptInput;
    scriptInput.dataDir = AppPaths::minecraftDir();
    scriptInput.program = javaSettings.javaPath;
    scriptInput.args = launchArguments(
        versionId, versionJson, javaSettings,
        offlineMode ? token : LaunchScriptCredentials::placeholder(), offlineMode);
    scriptInput.token = token;
    scriptInput.offlineMode = offlineMode;
    const QString script = LaunchScriptExport::compose(scriptInput);
    if (script.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
            tr("启动参数中仍包含已知登录凭据，未写入脚本。请移除自定义参数中的凭据后重试。"));
        return;
    }

    // QSaveFile：写不下去时**不会留下一个半截的 .bat**（那个东西双击起来的行为是随机的，
    // 而用户会以为"导出的就是用不了的"）。和写版本 json 那边同一个理由。
    const QByteArray bytes = script.toLocal8Bit();
    QSaveFile file(path);
    bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (written) {
        written = file.write(bytes) == bytes.size() && file.commit();
    }
    if (!written) {
        QString writeError = file.errorString();
        if (writeError.isEmpty()) {
            writeError = tr("系统没给出原因。");
        }
        appendLog(tr("导出启动脚本失败：%1").arg(writeError));
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("文件写入失败（%1）：\n%2")
                                   .arg(QDir::toNativeSeparators(path), writeError));
        return;
    }

    const QString shown = QDir::toNativeSeparators(path);
    appendLog(TextMarks::done(tr("启动脚本已导出：%1").arg(shown)));
    QStringList notes;
    notes.append(offlineMode ? tr("这一份用的是离线身份（游戏里就是这个名字）。")
                             : tr("脚本不保存登录令牌。正版建议从启动器启动；高级用法需在运行环境中提供 WTFML_MC_ACCESS_TOKEN。"));
    if (javaSettings.javaPath.isEmpty()) {
        notes.append(tr("此实例未指定 Java。脚本将使用 PATH 中的 java 命令。"));
    }
    notes.append(tr("导出的是本次启动使用的参数。修改后可重新导出。"));
    InlineMessage::information(
        m_ui->overlay(), tr("导出完成"),
        tr("已经写到：\n%1\n\n%2").arg(shown, notes.join(QStringLiteral("\n"))));
}

void MainWindow::exportModpack(const QString &versionId, const QString &targetPath) {
    // 默认还是原来的行为：`.mrpack` → Modrinth 清单，`.zip` → CurseForge 清单（需要联网反查）。
    // 只有"这个实例改过主类/JVM 参数"（lwjgl3ify 那类）才多问一句 —— 那种包写成 CurseForge
    // 清单会把自定义启动配置丢掉，导出的包对方装回去起不来。
    bool customLaunch = false;
    QFile json(InstanceFiles::jsonPath(versionId));
    if (json.open(QIODevice::ReadOnly)) {
        const QString mainClass = QJsonDocument::fromJson(json.readAll())
                                      .object().value(QStringLiteral("mainClass")).toString();
        static const QSet<QString> standard{
            QStringLiteral("net.minecraft.client.main.Main"),
            QStringLiteral("net.minecraft.launchwrapper.Launch"),
            QStringLiteral("cpw.mods.bootstraplauncher.BootstrapLauncher"),
            QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient")};
        customLaunch = !mainClass.isEmpty() && !standard.contains(mainClass);
    }
    bool instanceExport = false;
    if (customLaunch) {
        instanceExport = InlineMessage::question(
            m_ui->overlay(), tr("这个实例带自定义启动配置"),
            tr("此实例修改了主类或 JVM 参数。使用 CurseForge 格式会丢失启动配置，导入后可能无法运行。\n\n可导出为 Prism / MultiMC 可导入的普通实例，或继续导出 CurseForge 清单。"),
            tr("普通实例（Prism）"), tr("仍然导出 CurseForge 清单"), true);
    }
    if (!instanceExport) {
        m_modpackExportController->exportPack(versionId, targetPath);
        return;
    }
    const QString gameVersion = m_versionManager->instanceGameVersion(versionId);
    QString loaderKey, loaderVersion;
    m_versionManager->instanceLoaderInfo(versionId, &loaderKey, &loaderVersion);
    const auto result = InstanceExport::exportInstance(versionId, gameVersion, loaderKey, loaderVersion,
                                                       versionId, customLaunch, targetPath);
    if (!result.ok) {
        InlineMessage::warning(m_ui->overlay(), tr("导出实例"), result.problem);
        return;
    }
    QString details = tr("已写入：\n%1\n\n%2 个文件，约 %3 MB。")
                          .arg(QDir::toNativeSeparators(targetPath))
                          .arg(result.files)
                          .arg(result.bytes / (1024 * 1024));
    if (!result.warning.isEmpty()) details += QStringLiteral("\n\n") + result.warning;
    InlineMessage::information(m_ui->overlay(), tr("导出完成"), details);
}


