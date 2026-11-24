/**
 * @file AgentLauncherToolsTests.cpp
 * @brief 启动器工具：只读自动、完整清单/写操作审批、拒绝不改状态、诊断结果经投影、
 *        `settings.get` 不外发路径、备份创建/恢复走审批。
 */

#include "Support.hpp"

#include "agent/AgentArtifactStore.hpp"
#include "agent/AgentLauncherTools.hpp"
#include "agent/AgentTool.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-1");
    call.name = name;
    call.arguments = arguments;
    return call;
}

/// 假宿主：只记调用，不碰真盘；每个方法的返回值都能逐格摆。
class FakeHost final : public AgentLauncherHost {
public:
    QList<AgentDiagnosticProjection::InstanceFacts> instances;
    StartupDiagnosis::Report startupReport;
    QList<AgentHostModEntry> modEntries;
    QList<AgentHostBackupRef> backupEntries;
    QList<AgentHostCrashReport> crashEntries;
    QString logPath;
    QString modsPath;
    QString settingsProblem;
    QJsonObject settingsValues;

    bool created = false;
    bool restored = false;
    bool renamed = false;
    QString lastSettingKey;
    QJsonValue lastSettingValue;
    QString lastPage;

    bool instanceList(QList<AgentDiagnosticProjection::InstanceFacts> *items,
                      QString *problem) override {
        Q_UNUSED(problem);
        *items = instances;
        return true;
    }
    bool instanceFacts(const QString &instanceId,
                       AgentDiagnosticProjection::InstanceFacts *facts,
                       QString *problem) override {
        Q_UNUSED(problem);
        for (const auto &facts0 : instances) {
            if (facts0.instanceId == instanceId) {
                *facts = facts0;
                return true;
            }
        }
        *facts = instances.value(0);
        return !facts->instanceId.isEmpty();
    }
    bool startupDiagnosis(const QString &instanceId, StartupDiagnosis::Report *report,
                          QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        *report = startupReport;
        return true;
    }
    bool crashFacts(const QString &instanceId, GameExitDiagnosis::Facts *facts,
                    QStringList *hints, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        *facts = GameExitDiagnosis::Facts{};
        hints->clear();
        return true;
    }
    bool logTail(const QString &instanceId, int lines, QString *text, QString *relativeSource,
                 QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(lines);
        Q_UNUSED(problem);
        if (!logPath.isEmpty()) {
            QFile file(logPath);
            if (file.open(QIODevice::ReadOnly)) {
                *text = QString::fromUtf8(file.readAll());
                *relativeSource = QStringLiteral("versions/1.20.1-Fabric/logs/latest.log");
                return true;
            }
        }
        *text = QStringLiteral("password = hunter2\n[12:00:00] [main/INFO]: 启动完成");
        *relativeSource = QStringLiteral("logs/latest.log");
        return true;
    }
    bool logSources(const QString &instanceId, QList<AgentHostLogSource> *items,
                    QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        items->clear();
        if (logPath.isEmpty()) return true;
        AgentHostLogSource source;
        source.key = QStringLiteral("instance");
        source.label = QStringLiteral("versions/1.20.1-Fabric/logs/latest.log");
        source.path = logPath;
        source.bytes = QFileInfo(logPath).size();
        source.modifiedAt = QFileInfo(logPath).lastModified();
        items->append(source);
        return true;
    }
    bool crashReports(const QString &instanceId, QList<AgentHostCrashReport> *items,
                      QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        items->clear();
        if (!crashEntries.isEmpty()) {
            *items = crashEntries;
            return true;
        }
        AgentHostCrashReport report;
        report.fileName = QStringLiteral("crash-2026-01-01_12.00.00-client.txt");
        report.label = QStringLiteral("versions/1.20.1-Fabric/crash-reports");
        report.modifiedAt = QDateTime::currentDateTimeUtc();
        report.bytes = 1024;
        report.headText = QStringLiteral("java.lang.OutOfMemoryError");
        items->append(report);
        return true;
    }
    bool modsDirectory(const QString &instanceId, QString *path, QString *problem) override {
        Q_UNUSED(instanceId);
        if (modsPath.isEmpty()) {
            if (problem != nullptr) *problem = QStringLiteral("mods 目录不存在。");
            return false;
        }
        if (path != nullptr) *path = modsPath;
        return true;
    }
    bool integrity(const QString &instanceId, VersionIntegrity::Report *report,
                   QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        report->checked = 100;
        return true;
    }
    bool mods(const QString &instanceId, QList<AgentHostModEntry> *items,
              QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        *items = modEntries;
        return true;
    }
    bool setModEnabled(const QString &instanceId, const QString &fileName, bool enabled,
                       QString *previous, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(enabled);
        Q_UNUSED(problem);
        renamed = true;
        *previous = fileName;
        return true;
    }
    bool settings(AgentHostSettings *settings, QString *problem) override {
        Q_UNUSED(problem);
        settings->values = settingsValues;
        settings->writableKeys = {QStringLiteral("memory.xmx")};
        return true;
    }
    bool applySetting(const QString &key, const QJsonValue &value, QJsonObject *applied,
                      QString *problem) override {
        Q_UNUSED(applied);
        Q_UNUSED(problem);
        lastSettingKey = key;
        lastSettingValue = value;
        return true;
    }
    bool javaVersions(QList<int> *majors, QString *problem) override {
        Q_UNUSED(problem);
        *majors = {8, 17, 21};
        return true;
    }
    bool backups(QList<AgentHostBackupRef> *items, QString *problem) override {
        Q_UNUSED(problem);
        *items = backupEntries;
        return true;
    }
    bool planBackup(const QString &instanceId, const QStringList &scopes,
                    AgentHostBackupPlan *plan, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        plan->scope = scopes.join(QStringLiteral("+"));
        plan->relativePaths = {QStringLiteral("config/a.toml")};
        plan->bytes = 128;
        plan->saveLocation = QStringLiteral("backups/maintenance");
        return true;
    }
    bool createBackup(const QString &sessionId, const QString &instanceId,
                      const AgentHostBackupPlan &plan, QString *backupId,
                      QString *problem) override {
        Q_UNUSED(sessionId);
        Q_UNUSED(instanceId);
        Q_UNUSED(plan);
        Q_UNUSED(problem);
        created = true;
        *backupId = QStringLiteral("backup-1");
        return true;
    }
    bool restoreBackup(const QString &sessionId, const QString &backupId,
                       QString *preRestoreBackupId, QString *problem) override {
        Q_UNUSED(sessionId);
        Q_UNUSED(backupId);
        Q_UNUSED(problem);
        restored = true;
        *preRestoreBackupId = QStringLiteral("snapshot-1");
        return true;
    }
    bool upgradeInspect(const QString &instanceId, QJsonObject *data, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        *data = QJsonObject{{QStringLiteral("targets"), QJsonArray{}}};
        return true;
    }
    bool openPage(const QString &page, const QString &instanceId, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        lastPage = page;
        return true;
    }
};

AgentToolContext contextFor() {
    AgentToolContext context;
    context.sessionId = QStringLiteral("session-1");
    context.instanceId = QStringLiteral("1.20.1-Fabric");
    context.instanceName = QStringLiteral("我的整合包");
    context.gameRootId = QStringLiteral("root-1");
    context.rootPath = QStringLiteral("C:/game/versions/1.20.1-Fabric");
    context.providerHost = QStringLiteral("api.example.com");
    return context;
}

QString approvalIdOf(const AgentToolResult &result) {
    return result.approval.value(QStringLiteral("approvalId")).toString();
}

}  // namespace

void runAgentLauncherToolsTests() {
    FakeHost host;
    AgentDiagnosticProjection::InstanceFacts facts;
    facts.instanceId = QStringLiteral("1.20.1-Fabric");
    facts.instanceName = QStringLiteral("我的整合包");
    facts.gameVersion = QStringLiteral("1.20.1");
    facts.loaderName = QStringLiteral("Fabric");
    facts.modCount = 2;
    host.instances.append(facts);

    StartupDiagnosis::Finding finding;
    finding.severity = StartupDiagnosis::Severity::Problem;
    finding.title = QStringLiteral("没有指定 Java");
    finding.detail = QStringLiteral("读的是 D:\\mc\\java\\bin\\java.exe");
    host.startupReport.findings.append(finding);
    host.startupReport.problems = 1;

    AgentHostModEntry mod;
    mod.fileName = QStringLiteral("sodium.jar");
    mod.enabled = true;
    mod.bytes = 1024;
    host.modEntries.append(mod);
    AgentHostModEntry disabled;
    disabled.fileName = QStringLiteral("old.jar.disabled");
    disabled.enabled = false;
    host.modEntries.append(disabled);

    AgentHostBackupRef backup;
    backup.backupId = QStringLiteral("backup-1");
    backup.owner = QStringLiteral("agent");
    backup.label = QStringLiteral("backup.create：config");
    host.backupEntries.append(backup);

    host.settingsValues = QJsonObject{{QStringLiteral("memory.xmx"), 4096},
                                      {QStringLiteral("java.path"),
                                       QStringLiteral("C:/Java/bin/java.exe")}};

    AgentLauncherTools tools(&host, [](QString *) { return contextFor(); });

    // ---------- 只读：实例清单 ----------
    AgentToolResult result = tools.invoke(callFor(QStringLiteral("instance.list")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：instance.list 默认自动执行");
    check(result.data.value(QStringLiteral("count")).toInt() == 1, "启动器工具：instance.list 给总数");
    check(!result.data.contains(QStringLiteral("instances")),
          "启动器工具：instance.list 默认不带名称清单");

    result = tools.invoke(callFor(QStringLiteral("instance.list"),
                                 QJsonObject{{QStringLiteral("detail"), QStringLiteral("full")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：完整实例清单要逐次确认");
    {
        const QString approvalId = approvalIdOf(result);
        const AgentToolResult resolved = tools.resolveApproval(approvalId, false);
        check(resolved.error.kind == Agent::ErrorKind::ApprovalDenied,
              "启动器工具：拒绝完整清单后不交付数据");
    }
    result = tools.invoke(callFor(QStringLiteral("instance.list"),
                                 QJsonObject{{QStringLiteral("detail"), QStringLiteral("full")}}));
    {
        const QString approvalId = approvalIdOf(result);
        const AgentToolResult resolved = tools.resolveApproval(approvalId, true);
        check(resolved.status == AgentToolResult::Status::Ok,
              "启动器工具：批准后才交付完整清单");
        check(resolved.data.value(QStringLiteral("instances")).toArray().size() == 1,
              "启动器工具：批准后清单里有那一项");
    }

    // ---------- 未锁定实例 ----------
    AgentLauncherTools noLock(&host, [](QString *) { return AgentToolContext{}; });
    result = noLock.invoke(callFor(QStringLiteral("instance.context")));
    check(result.status == AgentToolResult::Status::Error, "启动器工具：未锁定实例时拒绝");
    check(result.data.value(QStringLiteral("needsInstanceLock")).toBool(),
          "启动器工具：回 NeedsInstanceLock 标记");

    // ---------- 诊断投影 ----------
    result = tools.invoke(callFor(QStringLiteral("diagnostics.startup")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：启动诊断自动执行");
    const QString startupText = QString::fromUtf8(
        QJsonDocument(result.data).toJson(QJsonDocument::Compact));
    check(!startupText.contains(QStringLiteral("D:\\mc")), "启动器工具：诊断结果不含本机路径");
    check(result.data.value(QStringLiteral("evidence")).toArray().size() == 1,
          "启动器工具：诊断带一条证据");

    // ---------- 日志：自动执行 + 凭据遮挡 ----------
    result = tools.invoke(callFor(QStringLiteral("logs.tail"), QJsonObject{{QStringLiteral("lines"), 20}}));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：日志结尾自动执行");
    const QString logContent = result.data.value(QStringLiteral("content")).toString();
    check(!logContent.contains(QStringLiteral("hunter2")), "启动器工具：日志里的口令被遮挡");
    check(result.data.value(QStringLiteral("autoReadNotice")).toBool(),
          "启动器工具：自动读取带“可能发送给模型服务”的告知");

    // ---------- 崩溃报告清单 ----------
    result = tools.invoke(callFor(QStringLiteral("crash.list")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：崩溃报告概况自动执行");
    result = tools.invoke(callFor(QStringLiteral("crash.list"),
                                  QJsonObject{{QStringLiteral("detail"), QStringLiteral("full")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：崩溃报告文件名清单要确认");
    tools.cancelPending();

    // ---------- 模组：概况自动、清单确认 ----------
    result = tools.invoke(callFor(QStringLiteral("mods.list")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：模组概况自动执行");
    check(result.data.value(QStringLiteral("enabled")).toInt() == 1,
          "启动器工具：模组启用数正确");
    check(result.data.value(QStringLiteral("disabled")).toInt() == 1,
          "启动器工具：模组停用数正确");
    check(!result.data.contains(QStringLiteral("names")), "启动器工具：概况不带文件名");
    result = tools.invoke(callFor(QStringLiteral("mods.list"),
                                  QJsonObject{{QStringLiteral("detail"), QStringLiteral("full")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：完整模组文件名要确认");
    tools.cancelPending();

    // ---------- 模组启停：必须审批，拒绝不改盘 ----------
    host.renamed = false;
    result = tools.invoke(callFor(QStringLiteral("mods.setEnabled"),
                                  QJsonObject{{QStringLiteral("fileName"), QStringLiteral("sodium.jar")},
                                              {QStringLiteral("enabled"), false}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：启停模组要审批");
    {
        const AgentToolResult denied = tools.resolveApproval(approvalIdOf(result), false);
        check(denied.error.kind == Agent::ErrorKind::ApprovalDenied, "启动器工具：拒绝启停");
        check(!host.renamed, "启动器工具：拒绝后没有改任何文件");
    }
    result = tools.invoke(callFor(QStringLiteral("mods.setEnabled"),
                                  QJsonObject{{QStringLiteral("fileName"), QStringLiteral("sodium.jar")},
                                              {QStringLiteral("enabled"), false}}));
    {
        const AgentToolResult approved = tools.resolveApproval(approvalIdOf(result), true);
        check(approved.status == AgentToolResult::Status::Ok, "启动器工具：批准后执行启停");
        check(host.renamed, "启动器工具：批准后宿主被调用");
    }
    result = tools.invoke(callFor(QStringLiteral("mods.setEnabled"),
                                  QJsonObject{{QStringLiteral("fileName"), QStringLiteral("sodium.jar")},
                                              {QStringLiteral("enabled"), true}}));
    check(result.status == AgentToolResult::Status::Error,
          "启动器工具：状态没变时拒绝（已经是启用）");

    // ---------- 设置：读不外发路径、写要审批 ----------
    result = tools.invoke(callFor(QStringLiteral("settings.get")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：settings.get 自动执行");
    const QJsonObject values = result.data.value(QStringLiteral("values")).toObject();
    check(!values.contains(QStringLiteral("java.path")), "启动器工具：settings.get 不返回 java.path");
    check(values.value(QStringLiteral("memory.xmx")).toInt() == 4096,
          "启动器工具：settings.get 返回非路径字段");

    host.lastSettingKey.clear();
    result = tools.invoke(callFor(QStringLiteral("settings.set"),
                                  QJsonObject{{QStringLiteral("key"), QStringLiteral("memory.xmx")},
                                              {QStringLiteral("value"), QStringLiteral("6144")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：settings.set 要审批");
    check(result.approval.value(QStringLiteral("summary")).toString().contains(QStringLiteral("6144")),
          "启动器工具：审批卡写出新值");
    {
        const AgentToolResult approved = tools.resolveApproval(approvalIdOf(result), true);
        check(approved.status == AgentToolResult::Status::Ok, "启动器工具：批准后写入设置");
        check(host.lastSettingKey == QStringLiteral("memory.xmx"), "启动器工具：写入的是那个键");
        check(host.lastSettingValue.toInt() == 6144, "启动器工具：写入的是新值");
    }
    result = tools.invoke(callFor(QStringLiteral("settings.set"),
                                  QJsonObject{{QStringLiteral("key"), QStringLiteral("world.root")},
                                              {QStringLiteral("value"), QStringLiteral("x")}}));
    check(result.status == AgentToolResult::Status::Error, "启动器工具：白名单外的设置键被拒");
    result = tools.invoke(callFor(QStringLiteral("settings.set"),
                                  QJsonObject{{QStringLiteral("key"), QStringLiteral("memory.xmx")},
                                              {QStringLiteral("value"), QStringLiteral("12")}}));
    check(result.status == AgentToolResult::Status::Error, "启动器工具：不合理的内存值被拒");

    // ---------- 备份 ----------
    result = tools.invoke(callFor(QStringLiteral("backup.list")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：backup.list 自动执行");
    check(result.data.value(QStringLiteral("count")).toInt() == 1, "启动器工具：列出备份条数");

    host.created = false;
    result = tools.invoke(callFor(QStringLiteral("backup.create"),
                                  QJsonObject{{QStringLiteral("reason"), QStringLiteral("改配置前")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：backup.create 必须审批");
    {
        const AgentToolResult denied = tools.resolveApproval(approvalIdOf(result), false);
        check(denied.error.kind == Agent::ErrorKind::ApprovalDenied, "启动器工具：拒绝备份");
        check(!host.created, "启动器工具：拒绝后没有创建备份");
    }
    result = tools.invoke(callFor(QStringLiteral("backup.create"),
                                  QJsonObject{{QStringLiteral("reason"), QStringLiteral("改配置前")}}));
    {
        const AgentToolResult approved = tools.resolveApproval(approvalIdOf(result), true);
        check(approved.status == AgentToolResult::Status::Ok, "启动器工具：批准后创建备份");
        check(host.created, "启动器工具：宿主被要求创建备份");
        check(approved.data.value(QStringLiteral("owner")).toString() == QStringLiteral("agent"),
              "启动器工具：备份 owner=agent");
    }

    host.restored = false;
    result = tools.invoke(callFor(QStringLiteral("backup.restore"),
                                  QJsonObject{{QStringLiteral("backupId"), QStringLiteral("backup-1")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "启动器工具：backup.restore 必须审批");
    {
        const AgentToolResult approved = tools.resolveApproval(approvalIdOf(result), true);
        check(approved.status == AgentToolResult::Status::Ok, "启动器工具：批准后恢复");
        check(host.restored, "启动器工具：宿主被要求恢复");
        check(approved.data.value(QStringLiteral("preRestoreBackupId")).toString() ==
                  QStringLiteral("snapshot-1"),
              "启动器工具：恢复前留了快照");
    }
    result = tools.invoke(callFor(QStringLiteral("backup.restore"),
                                  QJsonObject{{QStringLiteral("backupId"), QStringLiteral("nope")}}));
    check(result.status == AgentToolResult::Status::Error, "启动器工具：未知备份 id 被拒");

    // ---------- 只读检查与打开页面 ----------
    result = tools.invoke(callFor(QStringLiteral("upgrade.inspect")));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：upgrade.inspect 只读自动");
    result = tools.invoke(callFor(QStringLiteral("launcher.openPage"),
                                  QJsonObject{{QStringLiteral("page"), QStringLiteral("logs")}}));
    check(result.status == AgentToolResult::Status::Ok, "启动器工具：openPage 打开页面");
    check(host.lastPage == QStringLiteral("logs"), "启动器工具：页面名传到宿主");
    result = tools.invoke(callFor(QStringLiteral("launcher.openPage"),
                                  QJsonObject{{QStringLiteral("page"), QStringLiteral("settings")}}));
    check(result.status == AgentToolResult::Status::Error,
          "启动器工具：未知页面被 schema 拒绝");

    // ---------- S5.3: 崩溃快照 / 日志检索 ----------
    QTemporaryDir snapshotDir;
    check(snapshotDir.isValid(), "启动器工具：S5.3 临时目录可用");
    if (snapshotDir.isValid()) {
        const QString logPath = snapshotDir.filePath(QStringLiteral("latest.log"));
        {
            QFile file(logPath);
            check(file.open(QIODevice::WriteOnly), "启动器工具：写日志夹具");
            file.write(QStringLiteral("[12:00:00] [main/INFO]: Loading 3 mods:\n"
                                      "[12:00:00] [main/INFO]: - examplemod 1.2.0\n"
                                      "[12:00:01] [main/INFO]: password = hunter2\n"
                                      "[12:00:02] [main/ERROR]: Mixin apply for mod examplemod failed "
                                      "examplemod.mixins.json:ExampleMixin\n")
                           .toUtf8());
            file.close();
        }
        host.logPath = logPath;
        const QString reportId = QStringLiteral("crash-2026-01-01_12.00.00-client.txt");
        const QString crashPath = snapshotDir.filePath(reportId);
        {
            QFile file(crashPath);
            check(file.open(QIODevice::WriteOnly), "启动器工具：写崩溃报告夹具");
            file.write(QStringLiteral("---- Minecraft Crash Report ----\n"
                                      "// Oops.\n"
                                      "\n"
                                      "Time: 2026-01-01 12:00:00\n"
                                      "Description: Mod loading failed\n"
                                      "\n"
                                      "java.lang.RuntimeException: Failed to load mod file examplemod-1.2.0.jar\n"
                                      "\tat cpw.mods.modlauncher.LaunchServiceHandlerDecorator.launch(LaunchServiceHandlerDecorator.java:39)\n"
                                      "Caused by: org.spongepowered.asm.mixin.throwables.MixinApplyError: "
                                      "Error applying mixin examplemod.mixins.json:ExampleMixin from mod examplemod\n"
                                      "\tat org.spongepowered.asm.mixin.transformer.MixinProcessor.applyMixins(MixinProcessor.java:392)\n"
                                      "\n"
                                      "-- System Details --\n"
                                      "Details:\n"
                                      "\tMinecraft Version: 1.20.1\n"
                                      "\tJava Version: 17.0.8, Microsoft\n"
                                      "\tMemory: 2147483648 bytes (2048 MiB) up to 4294967296 bytes (4096 MiB)\n"
                                      "\tSuspected Mods: examplemod (Example Mod 1.2.0)\n"
                                      "\tMod List: \n"
                                      "\t\texamplemod-1.2.0.jar |Example Mod |examplemod |1.2.0 |DONE |Manifest: NOSIGNATURE\n"
                                      "\tLevel spawn location: World: (0,64,0) in C:\\\\Users\\\\player\\\\saves\\\\world\n")
                           .toUtf8());
            file.close();
        }
        AgentHostCrashReport hostReport;
        hostReport.fileName = reportId;
        hostReport.path = crashPath;
        hostReport.label = QStringLiteral("versions/1.20.1-Fabric/crash-reports");
        hostReport.modifiedAt = QDateTime::currentDateTimeUtc();
        hostReport.bytes = QFileInfo(crashPath).size();
        host.crashEntries.append(hostReport);

        // 日志检索：自动执行、带行号、遮凭据。
        result = tools.invoke(callFor(QStringLiteral("logs.search"),
                                      QJsonObject{{QStringLiteral("query"), QStringLiteral("mixin")}}));
        check(result.status == AgentToolResult::Status::Ok, "启动器工具：logs.search 自动执行",
              result.summary);
        check(result.data.value(QStringLiteral("totalMatches")).toInt() == 1,
              "启动器工具：logs.search 命中 1 处");
        const QJsonArray searchMatches = result.data.value(QStringLiteral("matches")).toArray();
        check(searchMatches.size() == 1 &&
                  searchMatches.first().toObject().value(QStringLiteral("line")).toInt() == 4,
              "启动器工具：logs.search 带行号");
        result = tools.invoke(callFor(QStringLiteral("logs.search"),
                                      QJsonObject{{QStringLiteral("query"), QStringLiteral("password")},
                                                  {QStringLiteral("before"), 1}}));
        const QString searchText =
            QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact));
        check(!searchText.contains(QStringLiteral("hunter2")),
              "启动器工具：日志搜索里的口令被遮挡");
        result = tools.invoke(callFor(QStringLiteral("logs.search")));
        check(result.status == AgentToolResult::Status::Error,
              "启动器工具：logs.search 缺 query 被 schema 拒");
        result = tools.invoke(callFor(QStringLiteral("logs.search"),
                                      QJsonObject{{QStringLiteral("query"), QStringLiteral("x")},
                                                  {QStringLiteral("source"), QStringLiteral("nope")}}));
        check(result.status == AgentToolResult::Status::Error,
              "启动器工具：logs.search 的 source 只认枚举值");

        // 严格模式下日志读取要逐次确认，拒绝后不给内容。
        tools.setStrictEgress(true);
        result = tools.invoke(callFor(QStringLiteral("logs.search"),
                                      QJsonObject{{QStringLiteral("query"), QStringLiteral("mixin")}}));
        check(result.status == AgentToolResult::Status::ApprovalRequired,
              "启动器工具：严格模式下日志搜索要确认");
        {
            const AgentToolResult denied = tools.resolveApproval(approvalIdOf(result), false);
            check(denied.error.kind == Agent::ErrorKind::ApprovalDenied,
                  "启动器工具：拒绝后不给搜索结果");
        }
        tools.setStrictEgress(false);

        // 按行读取。
        result = tools.invoke(callFor(QStringLiteral("logs.readRange"),
                                      QJsonObject{{QStringLiteral("startLine"), 2},
                                                  {QStringLiteral("lineCount"), 2}}));
        check(result.status == AgentToolResult::Status::Ok, "启动器工具：logs.readRange 自动执行",
              result.summary);
        check(result.data.value(QStringLiteral("firstLine")).toInt() == 2 &&
                  result.data.value(QStringLiteral("lastLine")).toInt() == 3,
              "启动器工具：logs.readRange 回行号范围");
        check(result.data.value(QStringLiteral("totalLines")).toInt() == 4,
              "启动器工具：logs.readRange 回总行数");
        result = tools.invoke(callFor(QStringLiteral("logs.readRange"),
                                      QJsonObject{{QStringLiteral("startLine"), 0}}));
        check(result.status == AgentToolResult::Status::Error,
              "启动器工具：logs.readRange 拒绝 startLine=0");
        result = tools.invoke(callFor(QStringLiteral("logs.readRange"),
                                      QJsonObject{{QStringLiteral("startLine"), 40}}));
        check(result.status == AgentToolResult::Status::Ok &&
                  result.data.value(QStringLiteral("truncated")).toBool(),
              "启动器工具：越界读取回空表并标截断");

        // 崩溃快照：默认最新一份报告。
        result = tools.invoke(callFor(QStringLiteral("diagnostics.crashSnapshot")));
        check(result.status == AgentToolResult::Status::Ok, "启动器工具：crashSnapshot 自动执行",
              result.summary);
        const QJsonObject crash = result.data.value(QStringLiteral("crash")).toObject();
        check(crash.value(QStringLiteral("phase")).toString() == QStringLiteral("mod_initialization"),
              "启动器工具：crashSnapshot 给出崩溃阶段",
              crash.value(QStringLiteral("phase")).toString());
        check(crash.value(QStringLiteral("exception")).toString() ==
                  QStringLiteral("java.lang.RuntimeException"),
              "启动器工具：crashSnapshot 给出顶层异常",
              crash.value(QStringLiteral("exception")).toString());
        bool sawMixinRule = false;
        for (const QJsonValue &value : result.data.value(QStringLiteral("matchedRules")).toArray()) {
            if (value.toString() == QStringLiteral("MIXIN_APPLY_FAILED")) sawMixinRule = true;
        }
        check(sawMixinRule, "启动器工具：crashSnapshot 命中 Mixin 规则");
        const QJsonArray snapshotSuspects = result.data.value(QStringLiteral("suspects")).toArray();
        check(!snapshotSuspects.isEmpty(), "启动器工具：crashSnapshot 给出嫌疑模组");
        const QString environmentLoader = result.data.value(QStringLiteral("environment"))
                                              .toObject()
                                              .value(QStringLiteral("loader"))
                                              .toString();
        check(environmentLoader == QStringLiteral("forge"),
              "启动器工具：crashSnapshot 的环境来自报告", environmentLoader);
        check(result.data.value(QStringLiteral("evidence")).toArray().size() >= 1,
              "启动器工具：crashSnapshot 带证据");
        const QString snapshotText =
            QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact));
        check(!snapshotText.contains(QStringLiteral("C:\\\\Users")),
              "启动器工具：crashSnapshot 不含本机路径");
        result = tools.invoke(callFor(QStringLiteral("diagnostics.crashSnapshot"),
                                      QJsonObject{{QStringLiteral("reportId"),
                                                   QStringLiteral("nope.txt")}}));
        check(result.status == AgentToolResult::Status::Error,
              "启动器工具：未知 reportId 被拒");

        // 崩溃报告全文：只回分段摘要，原文进 artifact 且已脱敏。
        result = tools.invoke(callFor(QStringLiteral("crash.readFull"),
                                      QJsonObject{{QStringLiteral("reportId"), reportId}}));
        check(result.status == AgentToolResult::Status::Ok, "启动器工具：crash.readFull 自动执行",
              result.summary);
        const QString artifactId = result.data.value(QStringLiteral("artifactId")).toString();
        check(!artifactId.isEmpty(), "启动器工具：crash.readFull 存了 artifact");
        check(!result.data.value(QStringLiteral("segments")).toArray().isEmpty(),
              "启动器工具：crash.readFull 给分段目录");
        check(result.data.value(QStringLiteral("totalLines")).toInt() > 5,
              "启动器工具：crash.readFull 回总行数");
        const QString fullText =
            QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact));
        check(!fullText.contains(QStringLiteral("C:\\\\Users")),
              "启动器工具：crash.readFull 的输出不含本机路径");
        if (!artifactId.isEmpty()) {
            AgentArtifactStore store;
            QString stored;
            QString storeProblem;
            check(store.readModel(QStringLiteral("session-1"), artifactId, 0, 100000, &stored,
                                  &storeProblem),
                  "启动器工具：crash.readFull 的 artifact 读得回来", storeProblem);
            check(stored.contains(QStringLiteral("MixinApplyError")),
                  "启动器工具：artifact 里有报告原文");
            check(!stored.contains(QStringLiteral("C:\\\\Users")),
                  "启动器工具：artifact 里的本机路径已换掉");
        }
        result = tools.invoke(callFor(QStringLiteral("crash.readFull")));
        check(result.status == AgentToolResult::Status::Error,
              "启动器工具：crash.readFull 必须给 reportId");
        host.crashEntries.clear();
    }

    // ---------- schema / 未知参数 ----------
    result = tools.invoke(callFor(QStringLiteral("diagnostics.startup"),
                                  QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("other")}}));
    check(result.status == AgentToolResult::Status::Error,
          "启动器工具：模型传入 instanceId 被拒（只认会话锁定）");
    result = tools.invoke(callFor(QStringLiteral("no.such.tool")));
    check(result.status == AgentToolResult::Status::Error, "启动器工具：未知工具被拒");
}
