/**
 * @file AgentCrashSnapshotTests.cpp
 * @brief S5.3 崩溃快照：内嵌规则加载与坏规则跳过、崩溃文本解析、嫌疑模组的角色与置信度、
 *        mixin 配置归属（只认 JAR 声明）。
 *
 * 夹具在 `tests/meta/agent/`，都是脱敏后的结构样本：报告头部写的是编造的模组名，没有账户、
 * 没有真实路径。
 */

#include "Support.hpp"
#include "fakejar.hpp"

#include "agent/AgentCrashSnapshot.hpp"
#include "agent/AgentKnowledgeRules.hpp"
#include "agent/AgentMixinOwnership.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>

namespace {

QString fixturePath(const QString &name) {
    return QStringLiteral(REPO_ROOT_PATH) + QStringLiteral("/tests/meta/agent/") + name;
}

QString readFixture(const QString &name) {
    QFile file(fixturePath(name));
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(file.readAll());
}

QStringList ruleIds(const QJsonObject &snapshot) {
    QStringList ids;
    for (const QJsonValue &value : snapshot.value(QStringLiteral("matchedRules")).toArray()) {
        ids.append(value.toString());
    }
    return ids;
}

QJsonObject suspectFor(const QJsonObject &snapshot, const QString &modId) {
    for (const QJsonValue &value : snapshot.value(QStringLiteral("suspects")).toArray()) {
        const QJsonObject suspect = value.toObject();
        if (suspect.value(QStringLiteral("modId")).toString().compare(modId, Qt::CaseInsensitive) == 0) {
            return suspect;
        }
    }
    return {};
}

QJsonArray recommendedTests(const QJsonObject &snapshot) {
    return snapshot.value(QStringLiteral("recommendedTests")).toArray();
}

AgentCrashSnapshot::Snapshot build(const QString &text,
                                   const QHash<QString, QString> &owners = {},
                                   bool truncated = false) {
    AgentCrashSnapshot::SnapshotInput input;
    input.parsed = AgentCrashSnapshot::parse(text, truncated);
    input.rules = AgentKnowledge::loadCrashRules();
    input.mixinOwners = owners;
    for (const AgentCrashSnapshot::ModEntry &entry : input.parsed.modList) {
        input.modVersions.insert(entry.modId.toLower(), entry.version);
    }
    input.sourceKind = input.parsed.kind;
    input.sourceLabel = QStringLiteral("tests/meta/agent");
    return AgentCrashSnapshot::build(input);
}

bool containsTest(const QJsonObject &snapshot, const QString &name) {
    for (const QJsonValue &value : recommendedTests(snapshot)) {
        if (value.toString() == name) return true;
    }
    return false;
}

}  // namespace

void runAgentCrashSnapshotTests() {
    // ---------------------------------------------------------------- 规则加载
    const AgentKnowledge::CrashRuleSet rules = AgentKnowledge::loadCrashRules();
    check(rules.ok(), "崩溃规则：内嵌 crash_rules.json 能加载", rules.resourceProblem);
    check(rules.problems.isEmpty(), "崩溃规则：内置文件没有坏规则", rules.problems.join(QStringLiteral("; ")));
    check(!rules.version.isEmpty() && rules.version != QStringLiteral("unknown"),
          "崩溃规则：带版本号", rules.version);
    check(rules.rules.size() >= 14, "崩溃规则：覆盖 14 类以上",
          QString::number(rules.rules.size()));
    bool sorted = true;
    for (int i = 1; i < rules.rules.size(); ++i) {
        if (rules.rules.at(i - 1).priority < rules.rules.at(i).priority) sorted = false;
    }
    check(sorted, "崩溃规则：按 priority 从大到小排好");
    const QStringList allowedPhases = AgentKnowledge::phases();
    bool phasesOk = true;
    for (const AgentKnowledge::CrashRule &rule : rules.rules) {
        if (!allowedPhases.contains(rule.phase)) phasesOk = false;
    }
    check(phasesOk, "崩溃规则：每条规则的 phase 都在允许集合里");

    // 坏规则跳过，其余照常加载。
    const QByteArray broken = R"({
        "version": "test",
        "rules": [
            {"id": "GOOD_RULE", "priority": 10, "phase": "runtime",
             "match": ["boom"], "confidence": 0.5, "confidenceBasis": "test",
             "recommendedTests": ["t1"], "suggestedActions": ["a1"]},
            {"id": "GOOD_RULE", "priority": 20, "phase": "runtime",
             "match": ["zzz_duplicate"], "confidence": 0.5, "confidenceBasis": "test"},
            {"id": "bad-id", "priority": 10, "phase": "runtime",
             "match": ["x"], "confidence": 0.5, "confidenceBasis": "test"},
            {"id": "BAD_PHASE", "priority": 10, "phase": "somewhere",
             "match": ["x"], "confidence": 0.5, "confidenceBasis": "test"},
            {"id": "EMPTY_MATCH", "priority": 10, "phase": "runtime",
             "match": [], "confidence": 0.5, "confidenceBasis": "test"},
            {"id": "BAD_CONFIDENCE", "priority": 10, "phase": "runtime",
             "match": ["x"], "confidence": 3.0, "confidenceBasis": "test"}
        ]
    })";
    const AgentKnowledge::CrashRuleSet parsedRules = AgentKnowledge::parseCrashRules(broken);
    check(parsedRules.rules.size() == 1, "崩溃规则：坏规则被跳过，好规则保留",
          QString::number(parsedRules.rules.size()));
    check(parsedRules.problems.size() == 5, "崩溃规则：每条坏规则都记了原因",
          QString::number(parsedRules.problems.size()));
    check(parsedRules.match(QStringLiteral("boom")).size() == 1,
          "崩溃规则：字面量命中");
    check(parsedRules.match(QStringLiteral("BOOM")).size() == 1,
          "崩溃规则：匹配不看大小写");
    check(parsedRules.match(QStringLiteral("zzz_duplicate")).isEmpty(),
          "崩溃规则：重复 id 的第二条不生效");
    check(AgentKnowledge::parseCrashRules(QByteArrayLiteral("not json")).rules.isEmpty(),
          "崩溃规则：坏 JSON 回空规则集");

    // ---------------------------------------------------------------- 夹具：每类一条规则
    struct FixtureCase {
        const char *file;
        const char *ruleId;
        const char *phase;
        const char *action;
    };
    const FixtureCase cases[] = {
        {"forge-mixin-crash.txt", "MIXIN_APPLY_FAILED", "mod_initialization", "check_mod_compatibility"},
        {"fabric-heap-oom.txt", "HEAP_OOM", "runtime", "raise_memory"},
        {"metaspace-oom.log", "METASPACE_OOM", "runtime", "raise_memory"},
        {"direct-buffer-oom.log", "DIRECT_BUFFER_OOM", "runtime", "check_graphics_settings"},
        {"native-thread-oom.log", "NATIVE_THREAD_OOM", "runtime", "raise_memory"},
        {"java-version-mismatch.log", "JAVA_VERSION_MISMATCH", "startup", "set_java_runtime"},
        {"missing-dependency.log", "MISSING_DEPENDENCY", "mod_initialization", "install_missing_dependency"},
        {"duplicate-mod.log", "DUPLICATE_MOD", "mod_initialization", "disable_duplicate_mod"},
        {"corrupt-mod-jar.log", "CORRUPT_MOD_FILE", "mod_initialization", "reinstall_mod_file"},
        {"config-parse-error.log", "CONFIG_PARSE_ERROR", "config_load", "restore_config_backup"},
        {"hs-err-access-violation.log", "NATIVE_CRASH", "native", "check_graphics_settings"},
        {"world-entity-tick.txt", "ENTITY_TICK_CRASH", "world_load", "check_mod_compatibility"},
    };
    for (const FixtureCase &item : cases) {
        const QString text = readFixture(QString::fromLatin1(item.file));
        check(!text.isEmpty(), QStringLiteral("崩溃夹具：读得到 %1").arg(QString::fromLatin1(item.file)));
        if (text.isEmpty()) continue;
        const AgentCrashSnapshot::Snapshot snapshot = build(text);
        const QJsonObject json = snapshot.json;
        check(ruleIds(json).contains(QString::fromLatin1(item.ruleId)),
              QStringLiteral("崩溃夹具 %1 命中 %2").arg(QString::fromLatin1(item.file),
                                                        QString::fromLatin1(item.ruleId)),
              ruleIds(json).join(QStringLiteral(",")));
        const QString phase =
            json.value(QStringLiteral("crash")).toObject().value(QStringLiteral("phase")).toString();
        check(phase == QString::fromLatin1(item.phase),
              QStringLiteral("崩溃夹具 %1 的阶段是 %2").arg(QString::fromLatin1(item.file),
                                                          QString::fromLatin1(item.phase)),
              phase);
        const QString action = json.value(QStringLiteral("suggestedAction")).toString();
        const QString expectedAction = QString::fromLatin1(item.action);
        check(action == expectedAction,
              QStringLiteral("崩溃夹具 %1 的建议动作是 %2").arg(QString::fromLatin1(item.file),
                                                            expectedAction),
              action);
    }

    // ---------------------------------------------------------------- Mixin 报告解析
    const QString mixinText = readFixture(QStringLiteral("forge-mixin-crash.txt"));
    const AgentCrashSnapshot::Parsed parsed = AgentCrashSnapshot::parse(mixinText);
    check(parsed.kind == QStringLiteral("crash-report"), "崩溃解析：认出崩溃报告");
    check(parsed.environment.minecraftVersion == QStringLiteral("1.20.1"),
          "崩溃解析：读得到游戏版本", parsed.environment.minecraftVersion);
    check(parsed.environment.loader == QStringLiteral("forge"),
          "崩溃解析：认出 Forge", parsed.environment.loader);
    check(parsed.environment.loaderVersion == QStringLiteral("47.3.0"),
          "崩溃解析：从模组清单取到 Forge 版本", parsed.environment.loaderVersion);
    check(parsed.environment.javaMajor == 17, "崩溃解析：Java 主版本 17",
          QString::number(parsed.environment.javaMajor));
    check(parsed.exception == QStringLiteral("java.lang.RuntimeException"),
          "崩溃解析：顶层异常类型", parsed.exception);
    check(parsed.causeChain.contains(QStringLiteral("org.spongepowered.asm.mixin.throwables.MixinApplyError")),
          "崩溃解析：Caused by 链里有 MixinApplyError");
    check(parsed.causeChain.contains(
              QStringLiteral("org.spongepowered.asm.mixin.injection.throwables.InvalidInjectionException")),
          "崩溃解析：Caused by 链里有 InvalidInjectionException");
    check(parsed.modList.size() == 4, "崩溃解析：Forge Mod List 读到 4 个模组",
          QString::number(parsed.modList.size()));
    bool sawExample = false;
    for (const AgentCrashSnapshot::ModEntry &entry : parsed.modList) {
        if (entry.modId == QStringLiteral("examplemod")) {
            sawExample = true;
            check(entry.version == QStringLiteral("1.2.0"), "崩溃解析：examplemod 版本 1.2.0",
                  entry.version);
            check(entry.fileName.startsWith(QStringLiteral("examplemod-1.2.0.jar")),
                  "崩溃解析：examplemod 文件名", entry.fileName);
        }
    }
    check(sawExample, "崩溃解析：Mod List 里有 examplemod");
    check(parsed.mixinConfigs.contains(QStringLiteral("examplemod.mixins.json")),
          "崩溃解析：抽到 mixin 配置名", parsed.mixinConfigs.join(QStringLiteral(",")));
    check(parsed.suspectedMods.contains(QStringLiteral("examplemod")),
          "崩溃解析：Suspected Mods 拆出 mod id", parsed.suspectedMods.join(QStringLiteral(",")));
    check(parsed.suspectedMods.contains(QStringLiteral("sodium")),
          "崩溃解析：Suspected Mods 里的第二个也拆出来");

    // ---------------------------------------------------------------- 快照：角色与置信度
    const AgentCrashSnapshot::Snapshot mixinSnapshot =
        build(mixinText, {{QStringLiteral("examplemod.mixins"), QStringLiteral("examplemod")}});
    const QJsonObject mixinJson = mixinSnapshot.json;
    check(mixinJson.value(QStringLiteral("source")).toObject().value(QStringLiteral("kind")).toString() ==
              QStringLiteral("crash-report"),
          "崩溃快照：来源类型是崩溃报告");
    const QJsonObject exampleSuspect = suspectFor(mixinJson, QStringLiteral("examplemod"));
    check(!exampleSuspect.isEmpty(), "崩溃快照：examplemod 进嫌疑列表");
    check(exampleSuspect.value(QStringLiteral("role")).toString() == QStringLiteral("candidate"),
          "崩溃快照：有 mixin 配置归属的标 candidate",
          exampleSuspect.value(QStringLiteral("role")).toString());
    const double exampleConfidence = exampleSuspect.value(QStringLiteral("confidence")).toDouble();
    check(exampleConfidence > 0.4, "崩溃快照：有归属证据的置信度高于 0.4",
          QString::number(exampleConfidence));
    check(exampleConfidence <= 0.95, "崩溃快照：置信度封顶 0.95",
          QString::number(exampleConfidence));
    check(exampleSuspect.value(QStringLiteral("confidenceBasis"))
              .toString()
              .contains(QStringLiteral("MIXIN_APPLY_FAILED")),
          "崩溃快照：置信度写明命中规则",
          exampleSuspect.value(QStringLiteral("confidenceBasis")).toString());
    check(exampleSuspect.value(QStringLiteral("confidenceBasis"))
              .toString()
              .contains(QStringLiteral("mixin config")),
          "崩溃快照：置信度写明 mixin 配置归属");
    check(exampleSuspect.value(QStringLiteral("version")).toString() == QStringLiteral("1.2.0"),
          "崩溃快照：嫌疑带版本号",
          exampleSuspect.value(QStringLiteral("version")).toString());
    const QJsonObject sodiumSuspect = suspectFor(mixinJson, QStringLiteral("sodium"));
    check(!sodiumSuspect.isEmpty(), "崩溃快照：Suspected Mods 里的 sodium 也列出（线索）");
    check(sodiumSuspect.value(QStringLiteral("role")).toString() == QStringLiteral("related"),
          "崩溃快照：只被提到的模组标 related",
          sodiumSuspect.value(QStringLiteral("role")).toString());
    check(sodiumSuspect.value(QStringLiteral("confidence")).toDouble() <= 0.4,
          "崩溃快照：只被提到的模组置信度不超过 0.4",
          QString::number(sodiumSuspect.value(QStringLiteral("confidence")).toDouble()));
    check(exampleConfidence > sodiumSuspect.value(QStringLiteral("confidence")).toDouble(),
          "崩溃快照：有归属证据的排在只被提到的前面");
    check(suspectFor(mixinJson, QStringLiteral("anothermod")).isEmpty(),
          "崩溃快照：只在模组清单里的模组不算嫌疑");
    check(containsTest(mixinJson, QStringLiteral("inspect_mixin_ownership")),
          "崩溃快照：Mixin 夹具给 inspect_mixin_ownership");
    check(containsTest(mixinJson, QStringLiteral("bisect_mod_pair")),
          "崩溃快照：Mixin 夹具给 bisect_mod_pair");
    check(mixinJson.value(QStringLiteral("knowledgeVersion")).toString() == rules.version,
          "崩溃快照：带知识版本");
    const QJsonArray mixinEvidence = mixinJson.value(QStringLiteral("evidence")).toArray();
    check(mixinEvidence.size() >= 2, "崩溃快照：报告与 mixin 归属各有一条证据",
          QString::number(mixinEvidence.size()));
    check(mixinEvidence.first().toObject().value(QStringLiteral("id")).toString() ==
              QStringLiteral("E1"),
          "崩溃快照：证据 id 从 E1 开始");
    check(!mixinEvidence.first().toObject().value(QStringLiteral("excerpt")).toString().isEmpty(),
          "崩溃快照：证据带摘录");
    check(!mixinEvidence.first().toObject().contains(QStringLiteral("path")),
          "崩溃快照：证据不带本机路径字段");
    check(mixinJson.value(QStringLiteral("truncated")).toBool() == false,
          "崩溃快照：完整报告不算截断");

    // 读不到 mixin 归属：标 unknown，不按文件名猜模组。
    const AgentCrashSnapshot::Snapshot unresolved = build(mixinText);
    const QJsonObject unknown = suspectFor(unresolved.json, QStringLiteral("unknown"));
    check(!unknown.isEmpty(), "崩溃快照：mixin 配置无归属时给 unknown 嫌疑");
    check(unknown.value(QStringLiteral("role")).toString() == QStringLiteral("related"),
          "崩溃快照：unknown 不算 candidate",
          unknown.value(QStringLiteral("role")).toString());
    check(unknown.value(QStringLiteral("confidence")).toDouble() <= 0.4,
          "崩溃快照：unknown 置信度不超过 0.4");

    // 日志夹具：第一个被提到的模组不是责任模组。
    const QString logText = readFixture(QStringLiteral("latest-mixin.log"));
    const AgentCrashSnapshot::Parsed logParsed = AgentCrashSnapshot::parse(logText);
    check(logParsed.kind == QStringLiteral("game-log"), "崩溃解析：日志夹具认成 game-log",
          logParsed.kind);
    check(logParsed.mixinMods.contains(QStringLiteral("examplemod")),
          "崩溃解析：日志里的 Mixin apply for mod 抽到 mod id",
          logParsed.mixinMods.join(QStringLiteral(",")));
    check(logParsed.modList.size() >= 4, "崩溃解析：日志里的 Loading N mods 清单也读了",
          QString::number(logParsed.modList.size()));
    const AgentCrashSnapshot::Snapshot logSnapshot = build(logText);
    const QJsonObject logSuspect = suspectFor(logSnapshot.json, QStringLiteral("examplemod"));
    check(!logSuspect.isEmpty(), "崩溃快照：日志里点名的模组进嫌疑列表");
    check(logSuspect.value(QStringLiteral("role")).toString() == QStringLiteral("candidate"),
          "崩溃快照：日志里 Mixin apply for mod 标 candidate",
          logSuspect.value(QStringLiteral("role")).toString());
    check(suspectFor(logSnapshot.json, QStringLiteral("sodium")).isEmpty(),
          "崩溃快照：不把日志里先出现的模组标成嫌疑");
    check(suspectFor(logSnapshot.json, QStringLiteral("anothermod")).isEmpty(),
          "崩溃快照：失败区外的模组不进嫌疑列表");

    // 两种内存问题给不同建议。
    const AgentCrashSnapshot::Snapshot heap =
        build(readFixture(QStringLiteral("fabric-heap-oom.txt")));
    const AgentCrashSnapshot::Snapshot direct =
        build(readFixture(QStringLiteral("direct-buffer-oom.log")));
    check(containsTest(heap.json, QStringLiteral("raise_heap_and_retest")),
          "崩溃快照：堆 OOM 给 raise_heap_and_retest");
    check(!containsTest(heap.json, QStringLiteral("inspect_render_backend")),
          "崩溃快照：堆 OOM 不给渲染后端的建议");
    check(containsTest(direct.json, QStringLiteral("inspect_render_backend")),
          "崩溃快照：Direct Buffer OOM 给 inspect_render_backend");
    check(heap.json.value(QStringLiteral("suggestedAction")).toString() ==
              QStringLiteral("raise_memory"),
          "崩溃快照：堆 OOM 的建议动作是 raise_memory");
    check(direct.json.value(QStringLiteral("suggestedAction")).toString() ==
              QStringLiteral("check_graphics_settings"),
          "崩溃快照：Direct Buffer OOM 的建议动作是检查图形设置");

    // 截断标记跟着输入走。
    check(build(mixinText, {}, true).json.value(QStringLiteral("truncated")).toBool(),
          "崩溃快照：输入截断时输出标 truncated");

    // ---------------------------------------------------------------- mixin 配置归属
    const QHash<QString, QByteArray> fabricDecl{
        {QStringLiteral("fabric.mod.json"),
         QByteArrayLiteral(R"({"id":"examplemod","mixins":["examplemod.mixins.json",{"config":"examplemod.client.mixins.json"}]})")}};
    QList<AgentMixinOwnership::Entry> owners =
        AgentMixinOwnership::fromDeclarations(fabricDecl, QStringLiteral("examplemod-1.2.0.jar"));
    check(owners.size() == 2, "mixin 归属：fabric.mod.json 两种写法都读到",
          QString::number(owners.size()));
    check(owners.value(0).config == QStringLiteral("examplemod.mixins") &&
              owners.value(0).modId == QStringLiteral("examplemod"),
          "mixin 归属：配置名归一化后去 .json",
          owners.value(0).config);
    check(owners.value(1).config == QStringLiteral("examplemod.client.mixins"),
          "mixin 归属：对象写法的 config 也读", owners.value(1).config);

    const QHash<QString, QByteArray> forgeDecl{
        {QStringLiteral("META-INF/mods.toml"),
         QByteArrayLiteral("modLoader=\"javafml\"\nloaderVersion=\"[47,)\"\n[[mods]]\nmodId=\"examplemod\"\n"
                           "version=\"1.2.0\"\n[[mixins]]\nconfig=\"examplemod.mixins.json\"\n"
                           "[[dependencies.examplemod]]\nconfig=\"not-a-mixin.json\"\n")}};
    owners = AgentMixinOwnership::fromDeclarations(forgeDecl, QStringLiteral("examplemod-1.2.0.jar"));
    check(owners.size() == 1 && owners.constFirst().config == QStringLiteral("examplemod.mixins"),
          "mixin 归属：mods.toml 只在 [[mixins]] 段里取 config",
          QString::number(owners.size()));
    check(owners.constFirst().modId == QStringLiteral("examplemod"),
          "mixin 归属：mods.toml 的 modId");

    const QHash<QString, QByteArray> manifestDecl{
        {QStringLiteral("META-INF/MANIFEST.MF"),
         QByteArrayLiteral("Manifest-Version: 1.0\nMixinConfigs: examplemod.mixins.json,examplemod.access\n")}};
    owners = AgentMixinOwnership::fromDeclarations(manifestDecl, QStringLiteral("plain.jar"));
    check(owners.size() == 2, "mixin 归属：MANIFEST 的 MixinConfigs 也读",
          QString::number(owners.size()));
    check(owners.constFirst().modId.isEmpty(), "mixin 归属：读不到 mod id 时留空，不猜");

    check(AgentMixinOwnership::normalizeConfig(QStringLiteral("assets/Example.Mixins.JSON")) ==
              QStringLiteral("example.mixins"),
          "mixin 归属：归一化取文件名、去 .json、转小写",
          AgentMixinOwnership::normalizeConfig(QStringLiteral("assets/Example.Mixins.JSON")));
    check(AgentMixinOwnership::normalizeConfig(QStringLiteral("example.mixin.json")) ==
              QStringLiteral("example.mixin"),
          "mixin 归属：单数 .mixin.json 也归一化",
          AgentMixinOwnership::normalizeConfig(QStringLiteral("example.mixin.json")));

    // 真的读一个 JAR：扫描只认声明，不认文件名。
    QTemporaryDir modsDir;
    check(modsDir.isValid(), "mixin 归属：临时 mods 目录可用");
    const QString jarPath = modsDir.filePath(QStringLiteral("misleading-name-1.0.jar"));
    const bool made = makeFakeJar(jarPath,
                                  {{QStringLiteral("fabric.mod.json"),
                                    QByteArrayLiteral(R"({"id":"realfabricmod","mixins":["realfabricmod.mixins.json"]})")}});
    check(made, "mixin 归属：造出测试 JAR");
    if (made) {
        int skipped = 0;
        QString problem;
        const QList<AgentMixinOwnership::Entry> scanned =
            AgentMixinOwnership::scan(modsDir.path(), 50, &skipped, &problem);
        check(scanned.size() == 1, "mixin 归属：扫到一条声明",
              QString::number(scanned.size()));
        check(scanned.constFirst().modId == QStringLiteral("realfabricmod"),
              "mixin 归属：归属来自声明而不是文件名", scanned.constFirst().modId);
        check(scanned.constFirst().fileName == QStringLiteral("misleading-name-1.0.jar"),
              "mixin 归属：记下是哪个文件");
        check(skipped == 0, "mixin 归属：没有跳过的 JAR", QString::number(skipped));
    }
    int skipped = 0;
    QString problem;
    check(AgentMixinOwnership::scan(modsDir.filePath(QStringLiteral("missing")), 50, &skipped,
                                    &problem)
                  .isEmpty() &&
              !problem.isEmpty(),
          "mixin 归属：目录不存在时回空表并给原因");
}
