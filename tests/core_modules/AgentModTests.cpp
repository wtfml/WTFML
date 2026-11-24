/**
 * @file AgentModTests.cpp
 * @brief S5.4：本地声明解析、兼容性判定、模组工具的审批/异步交付/失败不改工作区。
 */

#include "Support.hpp"
#include "fakejar.hpp"

#include "agent/AgentApprovalPolicy.hpp"
#include "agent/AgentLauncherHost.hpp"
#include "agent/AgentModCompat.hpp"
#include "agent/AgentModHost.hpp"
#include "agent/AgentModToolEngine.hpp"
#include "agent/AgentRepairTransaction.hpp"
#include "agent/AgentWorkspaceStore.hpp"
#include "agent/AgentTool.hpp"
#include "operations/ModDeclarations.hpp"
#include "operations/ModVersionDeclaration.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

namespace {

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-mod");
    call.name = name;
    call.arguments = arguments;
    return call;
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

QString sha1Of(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

ModDeclarations::Declaration fabricDeclaration(const QString &game, const QString &java = QString()) {
    ModDeclarations::Declaration declaration;
    declaration.loader = ModDeclarations::Loader::Fabric;
    declaration.basis = QStringLiteral("fabric.mod.json / depends.minecraft");
    declaration.game = game;
    if (!java.isEmpty()) declaration.javaRange = java;
    return declaration;
}

AgentModCompat::InstalledFile installed(const QString &fileName, bool enabled,
                                        const QString &modId,
                                        const QList<ModDeclarations::Declaration> &declarations,
                                        const QStringList &requiredModIds = {},
                                        const QString &readProblem = QString()) {
    AgentModCompat::InstalledFile file;
    file.fileName = fileName;
    file.enabled = enabled;
    file.bytes = 1024;
    file.sha1 = QStringLiteral("sha1-%1").arg(fileName);
    file.readProblem = readProblem;
    file.declaration.fileName = fileName;
    file.declaration.modId = modId;
    file.declaration.declarations = declarations;
    file.declaration.requiredModIds = requiredModIds;
    return file;
}

const AgentModCompat::Issue *findIssue(const AgentModCompat::Analysis &analysis,
                                       const QString &type, const QString &fileName) {
    for (const AgentModCompat::Issue &issue : analysis.issues) {
        if (issue.type == type && (fileName.isEmpty() || issue.fileName == fileName)) return &issue;
    }
    return nullptr;
}

bool hasIssue(const AgentModCompat::Analysis &analysis, const QString &type,
              const QString &fileName = QString()) {
    return findIssue(analysis, type, fileName) != nullptr;
}

// ---------------------------------------------------------------- 假宿主

class FakeLauncherHost final : public AgentLauncherHost {
public:
    QList<AgentHostModEntry> modEntries;
    QString modsDirectoryPath;

    bool instanceList(QList<AgentDiagnosticProjection::InstanceFacts> *items,
                      QString *problem) override {
        Q_UNUSED(problem);
        items->clear();
        return true;
    }
    bool instanceFacts(const QString &instanceId,
                       AgentDiagnosticProjection::InstanceFacts *facts,
                       QString *problem) override {
        Q_UNUSED(problem);
        facts->instanceId = instanceId;
        facts->instanceName = QStringLiteral("测试实例");
        facts->gameVersion = QStringLiteral("1.20.1");
        facts->loaderName = QStringLiteral("Fabric");
        facts->loaderVersion = QStringLiteral("0.15.11");
        facts->usedJavaMajor = 17;
        return true;
    }
    bool startupDiagnosis(const QString &instanceId, StartupDiagnosis::Report *report,
                          QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(report);
        Q_UNUSED(problem);
        return true;
    }
    bool crashFacts(const QString &instanceId, GameExitDiagnosis::Facts *facts,
                    QStringList *hints, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(facts);
        Q_UNUSED(hints);
        Q_UNUSED(problem);
        return true;
    }
    bool logTail(const QString &instanceId, int lines, QString *text, QString *relativeSource,
                 QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(lines);
        Q_UNUSED(text);
        Q_UNUSED(relativeSource);
        Q_UNUSED(problem);
        return false;
    }
    bool logSources(const QString &instanceId, QList<AgentHostLogSource> *items,
                    QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(items);
        Q_UNUSED(problem);
        return true;
    }
    bool crashReports(const QString &instanceId, QList<AgentHostCrashReport> *items,
                      QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        items->clear();
        return true;
    }
    bool modsDirectory(const QString &instanceId, QString *path, QString *problem) override {
        Q_UNUSED(instanceId);
        if (modsDirectoryPath.isEmpty()) {
            if (problem != nullptr) *problem = QStringLiteral("mods 目录不存在。");
            return false;
        }
        *path = modsDirectoryPath;
        return true;
    }
    bool integrity(const QString &instanceId, VersionIntegrity::Report *report,
                   QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(report);
        Q_UNUSED(problem);
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
        Q_UNUSED(fileName);
        Q_UNUSED(enabled);
        Q_UNUSED(previous);
        Q_UNUSED(problem);
        return false;
    }
    bool settings(AgentHostSettings *settings, QString *problem) override {
        Q_UNUSED(problem);
        settings->values = QJsonObject{};
        return true;
    }
    bool applySetting(const QString &key, const QJsonValue &value, QJsonObject *applied,
                      QString *problem) override {
        Q_UNUSED(key);
        Q_UNUSED(value);
        Q_UNUSED(applied);
        Q_UNUSED(problem);
        return false;
    }
    bool javaVersions(QList<int> *majors, QString *problem) override {
        Q_UNUSED(problem);
        majors->clear();
        return true;
    }
    bool backups(QList<AgentHostBackupRef> *items, QString *problem) override {
        Q_UNUSED(problem);
        items->clear();
        return true;
    }
    bool planBackup(const QString &instanceId, const QStringList &scopes,
                    AgentHostBackupPlan *plan, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(scopes);
        Q_UNUSED(plan);
        Q_UNUSED(problem);
        return false;
    }
    bool createBackup(const QString &sessionId, const QString &instanceId,
                      const AgentHostBackupPlan &plan, QString *backupId,
                      QString *problem) override {
        Q_UNUSED(sessionId);
        Q_UNUSED(instanceId);
        Q_UNUSED(plan);
        Q_UNUSED(backupId);
        Q_UNUSED(problem);
        return false;
    }
    bool restoreBackup(const QString &sessionId, const QString &backupId,
                       QString *preRestoreBackupId, QString *problem) override {
        Q_UNUSED(sessionId);
        Q_UNUSED(backupId);
        Q_UNUSED(preRestoreBackupId);
        Q_UNUSED(problem);
        return false;
    }
    bool upgradeInspect(const QString &instanceId, QJsonObject *data, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        *data = QJsonObject{};
        return true;
    }
    bool openPage(const QString &page, const QString &instanceId, QString *problem) override {
        Q_UNUSED(page);
        Q_UNUSED(instanceId);
        Q_UNUSED(problem);
        return false;
    }
};

class FakeModHost final : public AgentModHost {
public:
    QList<AgentModCompat::PlatformEntry> platformEntries;
    QString platformProblem;
    QStringList platformSha1s;
    int platformCalls = 0;
    AgentHostModTarget resolvedTarget;
    QString resolveProblem;
    int resolveCalls = 0;
    QStringList resolvedProjects;
    QByteArray downloadBody = QByteArrayLiteral("mod-bytes");
    bool downloadSucceeds = true;
    QString downloadSha1Override;
    int downloadCalls = 0;
    QString stagingPathSeen;
    bool repairOk = true;
    int repairCalls = 0;

    static void queue(const std::function<void()> &work) {
        QObject *context = QCoreApplication::instance();
        if (context == nullptr) {
            work();
            return;
        }
        QMetaObject::invokeMethod(context, work, Qt::QueuedConnection);
    }

    bool lookupModPlatform(const QString &instanceId, const QString &gameVersion,
                           const QString &loaderVersion, const QString &loader,
                           const QStringList &sha1s, const PlatformCompletion &done,
                           QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(gameVersion);
        Q_UNUSED(loaderVersion);
        Q_UNUSED(loader);
        Q_UNUSED(problem);
        ++platformCalls;
        platformSha1s = sha1s;
        queue([this, done] { done(platformEntries, platformProblem); });
        return true;
    }
    bool resolveModTarget(const QString &instanceId, const QString &projectId,
                          const QString &versionId, const QString &expectedSha1,
                          const ResolveCompletion &done, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(versionId);
        Q_UNUSED(expectedSha1);
        Q_UNUSED(problem);
        ++resolveCalls;
        resolvedProjects.append(projectId);
        const AgentHostModTarget target = resolvedTarget;
        const QString failure = resolveProblem;
        queue([done, target, failure] { done(target, failure); });
        return true;
    }
    bool downloadModFile(const AgentHostModTarget &target, const QString &stagingDir,
                         const DownloadCompletion &done, QString *problem) override {
        Q_UNUSED(problem);
        ++downloadCalls;
        const QString path = stagingDir + QLatin1Char('/') + target.fileName;
        writeFile(path, downloadBody);
        stagingPathSeen = path;
        AgentModHost::DownloadOutcome outcome;
        outcome.ok = downloadSucceeds;
        outcome.stagedPath = path;
        outcome.bytes = downloadBody.size();
        outcome.sha1 = downloadSha1Override.isEmpty() ? sha1Of(downloadBody) : downloadSha1Override;
        if (!downloadSucceeds) outcome.problem = QStringLiteral("模拟下载失败");
        queue([done, outcome] { done(outcome); });
        return true;
    }
    bool repairCoreFiles(const QString &instanceId, const QStringList &flaggedPaths,
                         const RepairCompletion &done, QString *problem) override {
        Q_UNUSED(instanceId);
        Q_UNUSED(flaggedPaths);
        Q_UNUSED(problem);
        ++repairCalls;
        AgentModHost::RepairOutcome outcome;
        outcome.ok = repairOk;
        if (!repairOk) outcome.problem = QStringLiteral("模拟修复失败");
        queue([done, outcome] { done(outcome); });
        return true;
    }
};

AgentToolContext testContext(bool strict = false) {
    AgentToolContext context;
    context.sessionId = QStringLiteral("session-mod");
    context.instanceId = QStringLiteral("1.20.1-fabric");
    context.instanceName = QStringLiteral("测试实例");
    context.strictEgress = strict;
    return context;
}

struct Collector {
    bool done = false;
    AgentToolResult result;
    Agent::ToolCall call;
    void attach(AgentModToolEngine *engine) {
        engine->setCompletionSink([this](const Agent::ToolCall &call, const AgentToolResult &result) {
            this->call = call;
            this->result = result;
            this->done = true;
        });
    }
};

bool waitFor(Collector &collector, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!collector.done && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return collector.done;
}

QString approvalIdOf(const AgentToolResult &result) {
    return result.approval.value(QStringLiteral("approvalId")).toString();
}

}  // namespace

void runAgentModTests() {
    // ---------------------------------------------------------------- 风险等级与审批
    {
        const AgentToolSpec analyze =
            AgentToolRegistry::find(QStringLiteral("mods.analyzeCompatibility"));
        const AgentToolSpec verify =
            AgentToolRegistry::find(QStringLiteral("mods.verifyFiles"));
        const AgentToolSpec install = AgentToolRegistry::find(QStringLiteral("mods.installFix"));
        const AgentToolSpec repair =
            AgentToolRegistry::find(QStringLiteral("instance.repairCore"));
        check(!analyze.name.isEmpty() && !verify.name.isEmpty() && !install.name.isEmpty() &&
                  !repair.name.isEmpty(),
              "模组工具：四个工具都注册了 schema");
        check(analyze.risk == AgentRisk::NetworkRead && verify.risk == AgentRisk::NetworkRead,
              "模组工具：分析/校验声明成 NetworkRead（平台查询走工具自己的审批）");
        check(install.risk == AgentRisk::Write, "模组工具：installFix 是写风险");
        check(repair.risk == AgentRisk::Destructive, "模组工具：repairCore 是破坏性风险");
        check(AgentApprovalPolicy::decide(QStringLiteral("mods.installFix"), install.risk, false) ==
                  AgentApprovalPolicy::Decision::PerAction,
              "模组工具：installFix 逐次审批");
        check(AgentApprovalPolicy::decide(QStringLiteral("instance.repairCore"), repair.risk,
                                          false) == AgentApprovalPolicy::Decision::PerAction,
              "模组工具：repairCore 逐次审批");
        check(AgentApprovalPolicy::decide(QStringLiteral("mods.analyzeCompatibility"),
                                          analyze.risk, false) ==
                  AgentApprovalPolicy::Decision::Auto,
              "模组工具：本地分析与平台查询的确认由工具自己的审批卡负责");
    }

    // ---------------------------------------------------------------- 声明解析
    {
        const QByteArray fabric = R"({
            "id": "examplemod", "name": "Example Mod", "version": "1.2.0",
            "depends": {"minecraft": ">=1.20 <1.21", "fabricloader": ">=0.15", "java": ">=17",
                        "libx": "*"},
            "breaks": {"minecraft": "1.19.2"}
        })";
        ModDeclarations::Declaration declaration;
        QStringList requiredMods;
        QStringList requiredLoaders;
        check(ModDeclarations::parseEntry(QStringLiteral("fabric.mod.json"), fabric, &declaration,
                                          &requiredMods, &requiredLoaders),
              "模组声明：fabric.mod.json 解析成功", declaration.problem);
        check(declaration.loader == ModDeclarations::Loader::Fabric, "模组声明：认出 Fabric");
        check(declaration.game.toString() == QStringLiteral(">=1.20 <1.21"),
              "模组声明：读到 depends.minecraft", declaration.game.toString());
        check(declaration.javaRange.toString() == QStringLiteral(">=17"),
              "模组声明：读到 depends.java", declaration.javaRange.toString());
        check(requiredMods == QStringList{QStringLiteral("libx")},
              "模组声明：required 前置只留模组", requiredMods.join(QStringLiteral(",")));
        check(requiredLoaders.contains(QStringLiteral("fabricloader")),
              "模组声明：加载器单独记", requiredLoaders.join(QStringLiteral(",")));

        const QByteArray quilt = R"({
            "quilt_loader": {"id": "quiltmod", "version": "2.0.0",
                             "metadata": {"name": "Quilt Mod"}},
            "depends": [{"id": "minecraft", "versions": ">=1.20"},
                        {"id": "quilt_loader", "versions": ">=0.20"},
                        {"id": "libz", "versions": "*"}]
        })";
        ModDeclarations::Declaration quiltDeclaration;
        requiredMods.clear();
        check(ModDeclarations::parseEntry(QStringLiteral("quilt.mod.json"), quilt, &quiltDeclaration,
                                          &requiredMods),
              "模组声明：quilt.mod.json 解析成功", quiltDeclaration.problem);
        check(quiltDeclaration.game.toString() == QStringLiteral(">=1.20"),
              "模组声明：Quilt 的 minecraft 范围", quiltDeclaration.game.toString());
        check(requiredMods == QStringList{QStringLiteral("libz")},
              "模组声明：Quilt 的模组前置", requiredMods.join(QStringLiteral(",")));

        const QByteArray toml = R"TOML(
modLoader="javafml"
loaderVersion="[47,)"
[[mods]]
modId="forgeexample"
version="3.1.0"
displayName="Forge Example"
[[dependencies.forgeexample]]
    modId="minecraft"
    mandatory=true
    versionRange="[1.20,1.20.2)"
    ordering="NONE"
    side="BOTH"
[[dependencies.forgeexample]]
    modId="forge"
    mandatory=true
    versionRange="[47,)"
    ordering="NONE"
    side="BOTH"
[[dependencies.forgeexample]]
    modId="libw"
    mandatory=true
    versionRange="[1.0,)"
    ordering="NONE"
    side="BOTH"
)TOML";
        ModDeclarations::Mod parsedMod;
        check(ModDeclarations::parseEntry(QStringLiteral("META-INF/mods.toml"), toml, &declaration),
              "模组声明：mods.toml 解析成功", declaration.problem);
        check(declaration.loader == ModDeclarations::Loader::Forge, "模组声明：认出 Forge");
        check(declaration.game.toString() == QStringLiteral("[1.20,1.20.2)"),
              "模组声明：toml 的 minecraft 范围", declaration.game.toString());
        requiredMods.clear();
        ModDeclarations::parseEntry(QStringLiteral("META-INF/mods.toml"), toml, &declaration,
                                    &requiredMods);
        check(requiredMods == QStringList{QStringLiteral("libw")},
              "模组声明：toml 的 required 前置", requiredMods.join(QStringLiteral(",")));
        QHash<QString, QByteArray> entries{{QStringLiteral("META-INF/mods.toml"), toml}};
        QHash<QString, QString> problems;
        ModDeclarations::parse(QStringLiteral("forgeexample-3.1.0.jar"), entries, problems,
                               &parsedMod);
        check(parsedMod.modId == QStringLiteral("forgeexample"), "模组声明：toml 的 modId",
              parsedMod.modId);
        check(parsedMod.version == QStringLiteral("3.1.0"), "模组声明：toml 的版本",
              parsedMod.version);
        check(parsedMod.name == QStringLiteral("Forge Example"), "模组声明：toml 的显示名",
              parsedMod.name);

        const QByteArray legacy = R"LEGACY([{"modid":"oldmod","name":"Old","version":"1.0",
                                     "acceptedMinecraftVersions":"[1.12,1.13)",
                                     "requiredMods":["libold@[1.0]","forge"]}])LEGACY";
        requiredMods.clear();
        check(ModDeclarations::parseEntry(QStringLiteral("mcmod.info"), legacy, &declaration,
                                          &requiredMods),
              "模组声明：mcmod.info 解析成功", declaration.problem);
        check(declaration.game.toString() == QStringLiteral("[1.12,1.13)"),
              "模组声明：mcmod.info 的版本范围", declaration.game.toString());
        check(requiredMods == QStringList{QStringLiteral("libold")},
              "模组声明：mcmod.info 的 requiredMods 去掉 @ 范围",
              requiredMods.join(QStringLiteral(",")));

        ModDeclarations::Mod empty;
        check(!ModDeclarations::parseEntry(QStringLiteral("fabric.mod.json"),
                                           QByteArrayLiteral("not json"), &declaration) &&
                  !declaration.problem.isEmpty(),
              "模组声明：坏 JSON 给原因");
        entries.clear();
        ModDeclarations::parse(QStringLiteral("x.jar"), entries, problems, &empty);
        check(!empty.problem.isEmpty(), "模组声明：没有声明文件时记问题");
    }

    // ---------------------------------------------------------------- 兼容性判定
    {
        AgentModCompat::Target target;
        target.gameVersion = QStringLiteral("1.20.1");
        target.loader = QStringLiteral("forge");
        target.loaderVersion = QStringLiteral("47.3.0");
        target.javaMajor = 17;
        QList<AgentModCompat::InstalledFile> files{
            installed(QStringLiteral("examplemod-1.2.0.jar"), true, QStringLiteral("examplemod"),
                      {fabricDeclaration(QStringLiteral(">=1.20"))})};
        const AgentModCompat::Analysis analysis = AgentModCompat::analyzeLocal(files, target);
        check(hasIssue(analysis, QStringLiteral("loader_mismatch")),
              "兼容判定：Fabric 模组装进 Forge 实例报 loader_mismatch");
        check(analysis.unknownCount == 0, "兼容判定：加载器不匹配不算 unknown",
              QString::number(analysis.unknownCount));

        AgentModCompat::Target quiltTarget = target;
        quiltTarget.loader = QStringLiteral("quilt");
        const AgentModCompat::Analysis quiltAnalysis =
            AgentModCompat::analyzeLocal(files, quiltTarget);
        check(!hasIssue(quiltAnalysis, QStringLiteral("loader_mismatch")),
              "兼容判定：Quilt 实例能加载 Fabric 模组");

        AgentModCompat::Target forgeTarget = target;
        QList<AgentModCompat::InstalledFile> forgeFiles{
            installed(QStringLiteral("examplemod.jar"), true, QStringLiteral("examplemod"),
                      {fabricDeclaration(QStringLiteral(">=1.19 <1.20"), QStringLiteral(">=21"))})};
        forgeTarget.loader = QStringLiteral("fabric");
        const AgentModCompat::Analysis incompatible =
            AgentModCompat::analyzeLocal(forgeFiles, forgeTarget);
        check(hasIssue(incompatible, QStringLiteral("incompatible_version")),
              "兼容判定：游戏版本范围不满足报 incompatible_version");
        check(hasIssue(incompatible, QStringLiteral("incompatible_version")) &&
                  findIssue(incompatible, QStringLiteral("incompatible_version"), QString())
                          ->detail.contains(QStringLiteral("Minecraft")),
              "兼容判定：说明是哪一段范围不满足");

        QList<AgentModCompat::InstalledFile> javaFiles{
            installed(QStringLiteral("needs21.jar"), true, QStringLiteral("needs21"),
                      {fabricDeclaration(QStringLiteral(">=1.20"), QStringLiteral(">=21"))})};
        const AgentModCompat::Analysis javaAnalysis =
            AgentModCompat::analyzeLocal(javaFiles, forgeTarget);
        bool sawJava = false;
        for (const AgentModCompat::Issue &issue : javaAnalysis.issues) {
            if (issue.type == QStringLiteral("incompatible_version") &&
                issue.detail.contains(QStringLiteral("Java"))) {
                sawJava = true;
            }
        }
        check(sawJava, "兼容判定：Java 范围不满足报 incompatible_version");

        QList<AgentModCompat::InstalledFile> missing{
            installed(QStringLiteral("a.jar"), true, QStringLiteral("a"),
                      {fabricDeclaration(QStringLiteral(">=1.20"))},
                      {QStringLiteral("libx"), QStringLiteral("liby")}),
            installed(QStringLiteral("liby.jar.disabled"), false, QStringLiteral("liby"),
                      {fabricDeclaration(QStringLiteral(">=1.20"))})};
        const AgentModCompat::Analysis missingAnalysis =
            AgentModCompat::analyzeLocal(missing, forgeTarget);
        const AgentModCompat::Issue *notInstalled =
            findIssue(missingAnalysis, QStringLiteral("missing_dependency"), QStringLiteral("a.jar"));
        check(notInstalled != nullptr, "兼容判定：缺前置报 missing_dependency");
        if (notInstalled != nullptr) {
            check(notInstalled->detail.contains(QStringLiteral("libx")) &&
                      notInstalled->detail.contains(QStringLiteral("未安装")),
                  "兼容判定：说明缺的是哪个前置", notInstalled->detail);
        }
        bool sawDisabledOnly = false;
        for (const AgentModCompat::Issue &issue : missingAnalysis.issues) {
            if (issue.type == QStringLiteral("missing_dependency") &&
                issue.detail.contains(QStringLiteral("liby")) &&
                issue.detail.contains(QStringLiteral("停用"))) {
                sawDisabledOnly = true;
            }
        }
        check(sawDisabledOnly, "兼容判定：只有停用文件时说明这一点");

        QList<AgentModCompat::InstalledFile> duplicates{
            installed(QStringLiteral("x-1.0.jar"), true, QStringLiteral("x"),
                      {fabricDeclaration(QStringLiteral(">=1.20"))}),
            installed(QStringLiteral("x-2.0.jar"), true, QStringLiteral("x"),
                      {fabricDeclaration(QStringLiteral(">=1.20"))})};
        const AgentModCompat::Analysis duplicateAnalysis =
            AgentModCompat::analyzeLocal(duplicates, forgeTarget);
        check(hasIssue(duplicateAnalysis, QStringLiteral("duplicate_mod")),
              "兼容判定：同一个 mod id 两个文件报 duplicate_mod");

        QList<AgentModCompat::InstalledFile> corrupt{
            installed(QStringLiteral("broken.jar"), true, QStringLiteral("broken"), {},
                      {}, QStringLiteral("invalid LOC header"))};
        const AgentModCompat::Analysis corruptAnalysis =
            AgentModCompat::analyzeLocal(corrupt, forgeTarget);
        check(hasIssue(corruptAnalysis, QStringLiteral("corrupt_file")),
              "兼容判定：读不动 JAR 报 corrupt_file");

        QList<AgentModCompat::InstalledFile> unknown{
            installed(QStringLiteral("nodecl.jar"), true, QString(), {})};
        const AgentModCompat::Analysis unknownAnalysis =
            AgentModCompat::analyzeLocal(unknown, forgeTarget);
        check(hasIssue(unknownAnalysis, QStringLiteral("unknown")) &&
                  unknownAnalysis.unknownCount == 1,
              "兼容判定：没有声明报 unknown");

        // 平台结果：版本只支持 1.19、加载器不对、缺必需前置。
        AgentModCompat::Analysis platformAnalysis =
            AgentModCompat::analyzeLocal(files, forgeTarget);
        AgentModCompat::PlatformEntry entry;
        entry.sha1 = QStringLiteral("sha1-examplemod-1.2.0.jar");
        entry.projectId = QStringLiteral("proj-example");
        entry.versionNumber = QStringLiteral("1.2.0");
        entry.gameVersions = QStringList{QStringLiteral("1.19.2")};
        entry.loaders = QStringList{QStringLiteral("fabric")};
        entry.requiredProjectIds = QStringList{QStringLiteral("proj-lib")};
        AgentModCompat::applyPlatform(&platformAnalysis, files, {entry}, target, true);
        bool sawGame = false;
        bool sawLoader = false;
        bool sawProject = false;
        for (const AgentModCompat::Issue &issue : platformAnalysis.issues) {
            if (issue.detail.contains(QStringLiteral("1.19.2"))) sawGame = true;
            if (issue.type == QStringLiteral("loader_mismatch") &&
                issue.detail.contains(QStringLiteral("平台"))) {
                sawLoader = true;
            }
            if (issue.type == QStringLiteral("missing_dependency") &&
                issue.detail.contains(QStringLiteral("proj-lib"))) {
                sawProject = true;
            }
        }
        check(sawGame, "平台结果：版本不匹配写进 incompatible_version");
        check(sawLoader, "平台结果：加载器不匹配写进 loader_mismatch");
        check(sawProject, "平台结果：平台必需前置不在已装清单报 missing_dependency");

        AgentModCompat::Analysis incomplete =
            AgentModCompat::analyzeLocal(files, forgeTarget);
        AgentModCompat::applyPlatform(&incomplete, files, {}, target, false);
        bool platformUnknown = false;
        for (const AgentModCompat::Issue &issue : incomplete.issues) {
            if (issue.detail.contains(QStringLiteral("平台没有"))) platformUnknown = true;
        }
        check(!platformUnknown, "平台结果：没问成时不逐文件报“平台没有记录”");
    }

    // ---------------------------------------------------------------- 工具：假宿主
    QTemporaryDir temp;
    check(temp.isValid(), "模组工具：临时目录可建");
    if (!temp.isValid()) return;

    const QString modsDir = temp.filePath(QStringLiteral("mods"));
    QDir().mkpath(modsDir);
    const QString goodJar = modsDir + QStringLiteral("/examplemod-1.2.0.jar");
    const bool madeGood = makeFakeJar(
        goodJar,
        {{QStringLiteral("fabric.mod.json"),
          QByteArrayLiteral(R"({"id":"examplemod","name":"Example Mod","version":"1.2.0",
                                "depends":{"minecraft":">=1.20","fabricloader":">=0.15","java":">=17"}})")}});
    check(madeGood, "模组工具：造出测试 JAR");
    const QString brokenJar = modsDir + QStringLiteral("/broken.jar");
    writeFile(brokenJar, QByteArrayLiteral("this is not a zip"));

    FakeLauncherHost launcher;
    launcher.modsDirectoryPath = modsDir;
    AgentHostModEntry goodEntry;
    goodEntry.fileName = QStringLiteral("examplemod-1.2.0.jar");
    goodEntry.enabled = true;
    goodEntry.bytes = QFileInfo(goodJar).size();
    goodEntry.path = goodJar;
    launcher.modEntries.append(goodEntry);
    AgentHostModEntry brokenEntry;
    brokenEntry.fileName = QStringLiteral("broken.jar");
    brokenEntry.enabled = true;
    brokenEntry.bytes = QFileInfo(brokenJar).size();
    brokenEntry.path = brokenJar;
    launcher.modEntries.append(brokenEntry);

    FakeModHost modHost;
    AgentModToolEngine engine(&launcher, &modHost,
                              [](QString *) { return testContext(); },
                              QCoreApplication::instance());
    Collector collector;
    collector.attach(&engine);

    // 本地分析：offline=true 不问平台。
    AgentToolResult result = engine.invoke(
        callFor(QStringLiteral("mods.analyzeCompatibility"),
                QJsonObject{{QStringLiteral("offline"), true}}));
    check(result.status == AgentToolResult::Status::Pending, "模组工具：兼容分析是异步工具",
          result.summary);
    check(waitFor(collector), "模组工具：兼容分析在时限内交付");
    check(collector.result.status == AgentToolResult::Status::Ok,
          "模组工具：离线兼容分析成功", collector.result.summary);
    check(collector.result.data.value(QStringLiteral("scanned")).toInt() == 2,
          "模组工具：扫到 2 个文件");
    check(modHost.platformCalls == 0, "模组工具：offline=true 不发平台请求");
    const QJsonArray issues = collector.result.data.value(QStringLiteral("issues")).toArray();
    bool sawCorrupt = false;
    for (const QJsonValue &value : issues) {
        if (value.toObject().value(QStringLiteral("type")).toString() ==
            QStringLiteral("corrupt_file")) {
            sawCorrupt = true;
        }
    }
    check(sawCorrupt, "模组工具：坏 JAR 报 corrupt_file");
    check(!QString::fromUtf8(QJsonDocument(collector.result.data).toJson(QJsonDocument::Compact))
               .contains(modsDir),
          "模组工具：工具输出不含本机路径");

    // 平台查询：第一次要确认；拒绝后一个请求都不发。
    collector.done = false;
    result = engine.invoke(callFor(QStringLiteral("mods.analyzeCompatibility")));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：首次平台查询要确认");
    check(result.approval.value(QStringLiteral("egress")).toObject().value(QStringLiteral("kind")).toString() ==
              QStringLiteral("platform-query"),
          "模组工具：审批卡写明是平台查询");
    const AgentToolResult denied = engine.resolveApproval(approvalIdOf(result), false);
    check(denied.error.kind == Agent::ErrorKind::ApprovalDenied,
          "模组工具：拒绝平台查询");
    check(modHost.platformCalls == 0, "模组工具：拒绝后一个平台请求都没发");

    // 批准后发请求，结果合并。
    modHost.platformEntries.clear();
    collector.done = false;
    result = engine.invoke(callFor(QStringLiteral("mods.analyzeCompatibility")));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：同一实例再次确认前仍要审批");
    const AgentToolResult approved = engine.resolveApproval(approvalIdOf(result), true);
    check(approved.status == AgentToolResult::Status::Pending,
          "模组工具：批准后进入异步平台查询");
    check(waitFor(collector), "模组工具：平台查询结果在时限内交付");
    check(collector.result.status == AgentToolResult::Status::Ok,
          "模组工具：带平台的兼容分析成功", collector.result.summary);
    check(modHost.platformCalls == 1, "模组工具：发了一次平台查询",
          QString::number(modHost.platformCalls));
    check(collector.result.data.value(QStringLiteral("platform"))
              .toObject()
              .value(QStringLiteral("complete"))
              .toBool(),
          "模组工具：平台查询完成标记");
    check(collector.result.data.value(QStringLiteral("platform"))
              .toObject()
              .value(QStringLiteral("entries"))
              .toInt() == 0,
          "模组工具：平台没认出文件时不编条目");

    // 非严格模式：同一实例第二次不再审批。
    collector.done = false;
    result = engine.invoke(callFor(QStringLiteral("mods.analyzeCompatibility")));
    check(result.status == AgentToolResult::Status::Pending,
          "模组工具：同一实例已确认过，不再弹审批");
    check(waitFor(collector), "模组工具：第二次平台查询在时限内交付");
    engine.cancelPending();

    // 严格模式：每次都确认。
    AgentModToolEngine strictEngine(&launcher, &modHost,
                                    [](QString *) { return testContext(true); },
                                    QCoreApplication::instance());
    result = strictEngine.invoke(callFor(QStringLiteral("mods.analyzeCompatibility")));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：严格模式下平台查询每次都确认");
    strictEngine.cancelPending();

    // 文件校验：本地结果带 sha1 与损坏标记。
    const int platformCallsBeforeVerify = modHost.platformCalls;
    collector.done = false;
    result = engine.invoke(callFor(QStringLiteral("mods.verifyFiles")));
    check(result.status == AgentToolResult::Status::Pending, "模组工具：文件校验是异步工具");
    check(waitFor(collector), "模组工具：文件校验在时限内交付");
    check(collector.result.status == AgentToolResult::Status::Ok,
          "模组工具：文件校验成功", collector.result.summary);
    check(collector.result.data.value(QStringLiteral("corrupt")).toInt() == 1,
          "模组工具：校验数出 1 个损坏文件",
          QString::number(collector.result.data.value(QStringLiteral("corrupt")).toInt()));
    const QJsonArray verifiedFiles = collector.result.data.value(QStringLiteral("files")).toArray();
    bool sawSha1 = false;
    for (const QJsonValue &value : verifiedFiles) {
        const QJsonObject file = value.toObject();
        if (file.value(QStringLiteral("fileName")).toString() ==
                QStringLiteral("examplemod-1.2.0.jar") &&
            file.value(QStringLiteral("sha1")).toString().size() == 40) {
            sawSha1 = true;
        }
    }
    check(sawSha1, "模组工具：校验结果带 SHA-1");
    check(modHost.platformCalls == platformCallsBeforeVerify,
          "模组工具：platform=false 时校验不发平台请求");

    // ---------------------------------------------------------------- 核心文件修复
    modHost.resolveProblem.clear();
    collector.done = false;
    result = engine.invoke(callFor(QStringLiteral("instance.repairCore")));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：核心修复每次审批");
    const AgentToolResult repairApproved = engine.resolveApproval(approvalIdOf(result), true);
    check(repairApproved.status == AgentToolResult::Status::Pending,
          "模组工具：批准后开始修核心文件");
    check(waitFor(collector), "模组工具：核心修复在时限内交付");
    check(collector.result.status == AgentToolResult::Status::Ok,
          "模组工具：核心修复成功", collector.result.summary);
    check(modHost.repairCalls == 1, "模组工具：宿主被要求修核心文件");

    // ---------------------------------------------------------------- 安装修复
    AgentWorkspaceStore workspaces;
    AgentWorkspaceRecord workspace;
    workspace.id = AgentWorkspaceStore::newId();
    workspace.sourceInstanceId = testContext().instanceId;
    workspace.versionId = AgentWorkspaceStore::newVersionId(workspace.id);
    workspace.rootPath = workspaces.workspaceRoot(workspace.id);
    workspace.status = QStringLiteral("ready");
    QDir().mkpath(workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId +
                  QStringLiteral("/mods"));
    QString problem;
    check(workspaces.create(workspace, &problem), "模组工具：建测试工作区", problem);
    AgentRepairTransactionStore transactions;
    AgentRepairTransactionRecord record;
    record.id = AgentRepairTransactionStore::newId();
    record.instanceId = testContext().instanceId;
    record.workspaceId = workspace.id;
    record.state = QStringLiteral("ready");
    record.scopes = QStringList{QStringLiteral("mods")};
    check(transactions.create(record, &problem), "模组工具：建测试事务", problem);

    // 没有事务时拒绝。
    result = engine.invoke(callFor(
        QStringLiteral("mods.installFix"),
        QJsonObject{{QStringLiteral("projectId"), QStringLiteral("proj-fixed")},
                    {QStringLiteral("workspaceId"), QStringLiteral("ws-unknown")}}));
    check(result.status == AgentToolResult::Status::Error,
          "模组工具：未知工作区时拒绝安装");

    const QByteArray body = QByteArrayLiteral("fixed-mod-bytes");
    modHost.resolvedTarget.projectId = QStringLiteral("proj-fixed");
    modHost.resolvedTarget.versionId = QStringLiteral("ver-1");
    modHost.resolvedTarget.versionNumber = QStringLiteral("1.2.1");
    modHost.resolvedTarget.fileName = QStringLiteral("fixedmod-1.2.1.jar");
    modHost.resolvedTarget.url = QStringLiteral("https://cdn.example.invalid/fixedmod.jar");
    modHost.resolvedTarget.sha1 = sha1Of(body);
    modHost.resolvedTarget.size = body.size();
    modHost.downloadBody = body;
    modHost.downloadSha1Override.clear();
    modHost.resolveCalls = 0;
    modHost.downloadCalls = 0;
    collector.done = false;
    result = engine.invoke(callFor(
        QStringLiteral("mods.installFix"),
        QJsonObject{{QStringLiteral("projectId"), QStringLiteral("proj-fixed")},
                    {QStringLiteral("versionId"), QStringLiteral("ver-1")},
                    {QStringLiteral("expectedSha1"), sha1Of(body)},
                    {QStringLiteral("workspaceId"), workspace.id}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：安装修复每次审批");
    const AgentToolResult installApproved =
        engine.resolveApproval(approvalIdOf(result), true);
    check(installApproved.status == AgentToolResult::Status::Pending,
          "模组工具：批准后开始解析与下载");
    check(waitFor(collector), "模组工具：安装修复在时限内交付");
    check(collector.result.status == AgentToolResult::Status::Ok,
          "模组工具：安装修复成功", collector.result.summary);
    check(modHost.resolveCalls == 1 && modHost.downloadCalls == 1,
          "模组工具：解析一次、下载一次");
    const QString installedPath = workspace.rootPath + QStringLiteral("/versions/") +
                                  workspace.versionId +
                                  QStringLiteral("/mods/fixedmod-1.2.1.jar");
    check(QFileInfo::exists(installedPath), "模组工具：文件写进了工作区 mods/");
    {
        QFile installedFile(installedPath);
        check(installedFile.open(QIODevice::ReadOnly) && installedFile.readAll() == body,
              "模组工具：工作区文件内容与下载一致");
    }
    check(collector.result.data.value(QStringLiteral("transactionId")).toString() == record.id,
          "模组工具：报告里带事务 id");

    // 哈希不一致：整批失败，工作区不变。
    check(QDir(workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId +
               QStringLiteral("/mods"))
              .removeRecursively(),
          "模组工具：清掉工作区里的旧文件");
    modHost.downloadBody = QByteArrayLiteral("other-bytes");
    modHost.downloadSha1Override = QStringLiteral("0000000000000000000000000000000000000000");
    collector.done = false;
    result = engine.invoke(callFor(
        QStringLiteral("mods.installFix"),
        QJsonObject{{QStringLiteral("projectId"), QStringLiteral("proj-fixed")},
                    {QStringLiteral("workspaceId"), workspace.id}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "模组工具：第二次安装同样审批");
    engine.resolveApproval(approvalIdOf(result), true);
    check(waitFor(collector), "模组工具：哈希不一致时在时限内交付失败");
    check(collector.result.status == AgentToolResult::Status::Error,
          "模组工具：哈希不一致整批失败", collector.result.summary);
    check(!QFileInfo::exists(workspace.rootPath + QStringLiteral("/versions/") +
                             workspace.versionId +
                             QStringLiteral("/mods/fixedmod-1.2.1.jar")),
          "模组工具：失败时工作区没有这个文件");

    // 缺前置在平台找不到时整批停止。
    modHost.downloadBody = body;
    modHost.downloadSha1Override.clear();
    modHost.resolvedTarget.sha1 = sha1Of(body);
    modHost.resolveProblem = QStringLiteral("平台找不到这个前置");
    collector.done = false;
    result = engine.invoke(callFor(
        QStringLiteral("mods.installFix"),
        QJsonObject{{QStringLiteral("projectId"), QStringLiteral("proj-fixed")},
                    {QStringLiteral("workspaceId"), workspace.id}}));
    engine.resolveApproval(approvalIdOf(result), true);
    check(waitFor(collector), "模组工具：前置缺失时在时限内交付失败");
    check(collector.result.status == AgentToolResult::Status::Error,
          "模组工具：前置缺失整批失败", collector.result.summary);
    check(!QFileInfo::exists(workspace.rootPath + QStringLiteral("/versions/") +
                             workspace.versionId +
                             QStringLiteral("/mods/fixedmod-1.2.1.jar")),
          "模组工具：前置失败时工作区没有这个文件");

    // 事务活动期间拒绝核心修复。
    result = engine.invoke(callFor(QStringLiteral("instance.repairCore")));
    check(result.status == AgentToolResult::Status::Error,
          "模组工具：事务活动期间拒绝核心修复", result.summary);

    // 清理：事务标成完成，免得留下活动事务影响后面的用例。
    transactions.updateState(record.id, QStringLiteral("committed"), QString(), &problem);
    workspaces.remove(workspace.id, true, &problem);
}
