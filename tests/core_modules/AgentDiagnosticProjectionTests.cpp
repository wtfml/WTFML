/**
 * @file AgentDiagnosticProjectionTests.cpp
 * @brief 诊断字段白名单投影：允许字段保留、路径/账户/原始路径剔除、空结果不报成功。
 */

#include "Support.hpp"

#include "agent/AgentDiagnosticProjection.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace {

bool jsonContains(const QJsonObject &object, const QString &needle) {
    const QString text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return text.contains(needle);
}

QStringList allKeys(const QJsonObject &object) {
    QStringList keys = object.keys();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isObject()) keys += allKeys(it.value().toObject());
    }
    return keys;
}

AgentDiagnosticProjection::InstanceFacts sampleFacts() {
    AgentDiagnosticProjection::InstanceFacts facts;
    facts.instanceId = QStringLiteral("1.20.1-Fabric");
    facts.instanceName = QStringLiteral("我的整合包");
    facts.gameVersion = QStringLiteral("1.20.1");
    facts.loaderName = QStringLiteral("Fabric");
    facts.loaderVersion = QStringLiteral("0.15.11");
    facts.requiredJavaMajor = 17;
    facts.usedJavaMajor = 21;
    facts.xmxMb = 4096;
    facts.xmsMb = 1024;
    facts.modCount = 42;
    facts.disabledModCount = 3;
    return facts;
}

}  // namespace

void runAgentDiagnosticProjectionTests() {
    // ---------- 启动诊断 ----------
    StartupDiagnosis::Report report;
    StartupDiagnosis::Finding problem;
    problem.severity = StartupDiagnosis::Severity::Problem;
    problem.title = QStringLiteral("没有指定 Java");
    problem.detail = QStringLiteral("读的是 D:\\mc\\java\\bin\\java.exe");
    problem.hint = QStringLiteral("在实例设置里选一个 Java 21");
    report.findings.append(problem);
    StartupDiagnosis::Finding warning;
    warning.severity = StartupDiagnosis::Severity::Warning;
    warning.title = QStringLiteral("Xmx 偏小");
    warning.detail = QStringLiteral("Xmx 1024 MB，模组 42 个");
    report.findings.append(warning);
    report.problems = 1;
    report.warnings = 1;

    QList<AgentDiagnosticProjection::Evidence> evidence;
    const QJsonObject startup =
        AgentDiagnosticProjection::startup(report, sampleFacts(), &evidence);
    check(AgentDiagnosticProjection::hasContent(startup), "投影：启动诊断有内容");
    check(startup.value(QStringLiteral("problems")).toInt() == 1, "投影：问题计数保留");
    check(startup.value(QStringLiteral("warnings")).toInt() == 1, "投影：提醒计数保留");
    const QJsonArray findings = startup.value(QStringLiteral("findings")).toArray();
    check(findings.size() == 2, "投影：两条发现都留下", QString::number(findings.size()));
    const QJsonObject first = findings.at(0).toObject();
    check(!first.contains(QStringLiteral("detail")), "投影：不把原始 detail 原文塞进发现项");
    check(!first.value(QStringLiteral("code")).toString().isEmpty(), "投影：每条发现带稳定 code");
    check(!jsonContains(startup, QStringLiteral("D:\\mc")), "投影：绝对路径不出现在启动诊断里");
    check(!jsonContains(startup, QStringLiteral("java.exe")), "投影：Java 可执行文件不出现在结果里");
    check(evidence.size() == 2, "投影：每条发现一条证据", QString::number(evidence.size()));
    check(evidence.constFirst().id == QStringLiteral("E1"), "投影：证据 id 从 E1 起");
    check(evidence.constFirst().excerpt.isEmpty(),
          "投影：带本机路径的 detail 整段不进入证据片段");
    check(!evidence.at(1).excerpt.isEmpty(), "投影：不含路径的 detail 保留成证据片段");

    // ---------- 崩溃诊断 ----------
    GameExitDiagnosis::Facts crash;
    crash.crashed = true;
    crash.exitCode = -1073741819;  // 0xC0000005
    crash.instanceId = QStringLiteral("1.20.1-Fabric");
    crash.instanceName = QStringLiteral("我的整合包");
    crash.requiredJavaMajor = 17;
    crash.usedJavaMajor = 8;
    crash.javaPath = QStringLiteral("C:/Program Files/Java/jre8/bin/java.exe");
    crash.xmxMb = 1024;
    crash.xmsMb = 512;
    crash.errorLineCount = 12;
    crash.sawOutOfMemory = true;
    crash.logCaptured = true;
    QStringList hints{QStringLiteral("实际用的 Java 8 低于要求的 17"),
                      QStringLiteral("看 C:\\Users\\张三\\AppData\\Local\\Temp\\hs_err.log")};
    QList<AgentDiagnosticProjection::Evidence> crashEvidence;
    const QJsonObject crashJson =
        AgentDiagnosticProjection::crash(crash, hints, sampleFacts(), &crashEvidence);
    check(crashJson.value(QStringLiteral("crashed")).toBool(), "投影：崩溃标记保留");
    check(crashJson.value(QStringLiteral("exitCode")).toInt() == -1073741819,
          "投影：退出码保留");
    check(crashJson.value(QStringLiteral("sawOutOfMemory")).toBool(),
          "投影：OOM 事实保留");
    check(crashJson.value(QStringLiteral("requiredJavaMajor")).toInt() == 17,
          "投影：要求的 Java 主版本保留");
    check(crashJson.value(QStringLiteral("usedJavaMajor")).toInt() == 8,
          "投影：实际用的 Java 主版本保留");
    const QJsonArray keptHints = crashJson.value(QStringLiteral("hints")).toArray();
    check(keptHints.size() == 1, "投影：含本机路径的提示被丢掉", QString::number(keptHints.size()));
    check(!jsonContains(crashJson, QStringLiteral("Program Files")),
          "投影：Java 路径不出现");
    check(!jsonContains(crashJson, QStringLiteral("张三")), "投影：用户名不出现");
    check(crashEvidence.size() == 1, "投影：崩溃诊断带证据");

    // ---------- 完整性 ----------
    VersionIntegrity::Report integrity;
    integrity.checked = 3120;
    VersionIntegrity::Verdict missing;
    missing.status = VersionIntegrity::Status::Missing;
    missing.label = QStringLiteral("客户端 jar");
    missing.path = QStringLiteral("D:/mc/versions/x/x.jar");
    missing.group = VersionIntegrity::Group::ClientJar;
    integrity.problems.append(missing);
    QList<AgentDiagnosticProjection::Evidence> integrityEvidence;
    const QJsonObject integrityJson =
        AgentDiagnosticProjection::integrity(integrity, sampleFacts(), &integrityEvidence);
    check(integrityJson.value(QStringLiteral("checked")).toInt() == 3120,
          "投影：体检条数保留");
    check(integrityJson.value(QStringLiteral("missing")).toInt() == 1, "投影：缺失计数保留");
    check(!jsonContains(integrityJson, QStringLiteral("D:/mc")), "投影：体检结果不含绝对路径");
    check(integrityJson.value(QStringLiteral("groups")).toArray().size() == 4,
          "投影：四组都列出来");

    // ---------- 空结果 ----------
    StartupDiagnosis::Report empty;
    QList<AgentDiagnosticProjection::Evidence> emptyEvidence;
    const QJsonObject emptyJson =
        AgentDiagnosticProjection::startup(empty, sampleFacts(), &emptyEvidence);
    check(!AgentDiagnosticProjection::hasContent(emptyJson),
          "投影：空诊断不报成功（调用方必须回错误）");
    check(emptyEvidence.isEmpty(), "投影：空诊断没有证据");

    // ---------- 路径/脱敏工具 ----------
    check(AgentDiagnosticProjection::containsLocalPath(QStringLiteral("在 C:\\mc 里")),
          "投影：认得出盘符路径");
    check(AgentDiagnosticProjection::containsLocalPath(QStringLiteral("见 /Users/bob/x")),
          "投影：认得出用户目录");
    check(!AgentDiagnosticProjection::containsLocalPath(
              QStringLiteral("logs/latest.log:120")),
          "投影：相对路径不算本机路径");
    const QString scrubbed = AgentDiagnosticProjection::scrubPaths(
        QStringLiteral("at C:\\mc\\versions\\a.jar:12"));
    check(!scrubbed.contains(QStringLiteral("C:\\mc")), "投影：scrubPaths 换掉盘符路径");
    check(scrubbed.contains(QStringLiteral("<path>")), "投影：scrubPaths 留占位符");

    // ---------- 日志字段投影：启动参数、MAC、SID、硬件标识、裸用户名 ----------
    const QString raw = QStringLiteral(
        "Setting user: Alice, launch --accessToken abcdef1234567890 --uuid "
        "069a79f444e94726a5befca90e38aaf5\\n"
        "eth0 01:23:45:67:89:AB, sid S-1-5-21-1111111-2222222-3333333-4444, "
        "HWID=ABC123DEF, MachineGuid: 9f8e7d6c, C:\\\\Users\\\\Alice\\\\x.log");
    const QString projected = AgentDiagnosticProjection::scrubIdentifiers(
        raw, {QStringLiteral("Alice")});
    check(!projected.contains(QStringLiteral("abcdef1234567890")),
          "投影：启动参数里的令牌被遮", projected);
    check(projected.contains(QStringLiteral("--accessToken")),
          "投影：启动参数名保留（看得出是谁在启动）");
    check(!projected.contains(QStringLiteral("069a79f4")) &&
              projected.contains(QStringLiteral("--uuid <redacted>")),
          "投影：启动参数里的 uuid 被遮");
    check(!projected.contains(QStringLiteral("01:23:45:67:89:AB")) &&
              projected.contains(QStringLiteral("<mac>")),
          "投影：MAC 地址被遮");
    check(!projected.contains(QStringLiteral("S-1-5-21-1111111")) &&
              projected.contains(QStringLiteral("<sid>")),
          "投影：Windows SID 被遮");
    check(!projected.contains(QStringLiteral("ABC123DEF")) &&
              !projected.contains(QStringLiteral("9f8e7d6c")) &&
              projected.contains(QStringLiteral("<redacted>")),
          "投影：硬件标识字段被遮");
    check(!projected.contains(QStringLiteral("Alice")) &&
              projected.contains(QStringLiteral("<user>")),
          "投影：点名的账户名换成 <user>");
    check(AgentDiagnosticProjection::scrubIdentifiers(QStringLiteral("bin 目录"),
                                                      {QStringLiteral("bin")}) ==
              QStringLiteral("bin 目录"),
          "投影：短名字不参与整词替换");
    const QString logText =
        AgentDiagnosticProjection::scrubLogText(QStringLiteral("在 C:\\\\Users\\\\bob\\\\a.log 里"));
    check(logText.contains(QStringLiteral("<path>")) && !logText.contains(QStringLiteral("C:\\")),
          "投影：scrubLogText 先换路径再过字段规则", logText);
    check(!AgentDiagnosticProjection::localAccountNames().isEmpty(),
          "投影：本机账户名候选读得到（测试环境至少有一个）");
}
