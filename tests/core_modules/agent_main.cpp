/**
 * @file agent_main.cpp
 * @brief 智能体核心验证入口（Stage 1A A 组）：SSE/适配/会话/加密/凭据/上下文/Loop/技能/Markdown。
 *
 * 不联网、不读真实用户目录；每个用例用临时沙盒。产品入口不设置这些测试属性。
 */

#include "Support.hpp"
#include "support/SettingsIsolation.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

void runAgentSseParserTests();
void runAgentProviderAdapterTests();
void runAgentSessionStoreTests();
void runAgentSessionCryptoTests();
void runAgentCredentialStoreTests();
void runAgentContextTests();
void runAgentLoopTests();
void runAgentSkillsTests();
void runAgentMarkdownTests();
void runAgentPathPolicyTests();
void runAgentEgressPolicyTests();
void runAgentApprovalTests();
void runAgentArtifactTests();
void runAgentInstanceLockTests();
void runAgentBackupLifecycleTests();
void runAgentToolTests();
void runAgentOpenAiClientTests();
void runAgentDiagnosticProjectionTests();
void runAgentLauncherToolsTests();
void runAgentWorkflowTests();
void runAgentMcpProtocolTests();
void runAgentMcpTrustTests();
void runAgentMcpClientTests();
void runAgentWebPolicyTests();
void runAgentWebToolsTests();
void runAgentCommandToolsTests();
void runAgentPerformanceTests();


// ---------------------------------------------------------------------------
// 假 MCP 服务器：同一个可执行文件带 `--mcp-fake-server` 起一个 stdio JSON-RPC 循环。
//
// 单独建一个测试可执行文件要再加一个 CMake 目标；这里直接复用 verify_agent 自己，
// 客户端测试用 QCoreApplication::applicationFilePath() 起它。行为由环境变量微调：
//   WTFML_FAKE_MCP_CRASH=1   握手完直接退出（测崩溃不自动重启）
//   WTFML_FAKE_MCP_GARBAGE=1 每次响应前吐一行非 JSON（测 stdout 垃圾行）
// ---------------------------------------------------------------------------
namespace {

void writeFakeLine(const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

QJsonObject fakeToolsPage(int page) {
    QJsonArray tools;
    if (page == 1) {
        tools.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("echo")},
                                 {QStringLiteral("description"), QStringLiteral("原样回显")},
                                 {QStringLiteral("inputSchema"),
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                              {QStringLiteral("properties"),
                                               QJsonObject{{QStringLiteral("text"),
                                                            QJsonObject{{QStringLiteral("type"),
                                                                         QStringLiteral("string")}}}}},
                                              {QStringLiteral("required"),
                                               QJsonArray{QStringLiteral("text")}}}}});
        tools.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("fail")},
                                 {QStringLiteral("description"), QStringLiteral("总是报错")},
                                 {QStringLiteral("inputSchema"),
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}});
    } else {
        tools.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("second.page")},
                                 {QStringLiteral("description"), QStringLiteral("第二页的工具")},
                                 {QStringLiteral("inputSchema"),
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}});
    }
    QJsonObject result{{QStringLiteral("tools"), tools}};
    if (page == 1) result.insert(QStringLiteral("nextCursor"), QStringLiteral("page2"));
    return result;
}

int runFakeMcpServer() {
    const bool crash = qEnvironmentVariableIsSet("WTFML_FAKE_MCP_CRASH");
    const bool garbage = qEnvironmentVariableIsSet("WTFML_FAKE_MCP_GARBAGE");
    std::fprintf(stderr, "fake-mcp: started password=stderr-secret\n");
    std::fflush(stderr);
    char buffer[1 << 20];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
        if (garbage) {
            std::fputs("this line is not json\n", stdout);
            std::fflush(stdout);
        }
        const QJsonObject request = QJsonDocument::fromJson(QByteArray(buffer)).object();
        const QString method = request.value(QStringLiteral("method")).toString();
        const QJsonValue id = request.value(QStringLiteral("id"));
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        if (method.isEmpty()) continue;
        if (!id.isUndefined() && method == QStringLiteral("initialize")) {
            writeFakeLine(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("result"),
                 QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")},
                             {QStringLiteral("capabilities"), QJsonObject{}},
                             {QStringLiteral("serverInfo"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("fake")}}}}}});
            if (crash) return 0;
            continue;
        }
        if (method == QStringLiteral("tools/list")) {
            const int page = params.value(QStringLiteral("cursor")).toString() ==
                                     QStringLiteral("page2")
                                 ? 2
                                 : 1;
            writeFakeLine(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                      {QStringLiteral("id"), id},
                                      {QStringLiteral("result"), fakeToolsPage(page)}});
            continue;
        }
        if (method == QStringLiteral("tools/call")) {
            const QString name = params.value(QStringLiteral("name")).toString();
            if (name == QStringLiteral("fail")) {
                writeFakeLine(QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("isError"), true},
                                 {QStringLiteral("content"),
                                  QJsonArray{QJsonObject{{QStringLiteral("type"),
                                                          QStringLiteral("text")},
                                                         {QStringLiteral("text"),
                                                          QStringLiteral("故意失败")}}}}}}});
                continue;
            }
            const QString text = params.value(QStringLiteral("arguments"))
                                     .toObject()
                                     .value(QStringLiteral("text"))
                                     .toString();
            writeFakeLine(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("result"),
                 QJsonObject{{QStringLiteral("content"),
                              QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                     {QStringLiteral("text"),
                                                      QStringLiteral("echo: %1 token=sk-fake-123456")
                                                          .arg(text)}}}}}}});
            continue;
        }
        writeFakeLine(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("code"), -32601},
                                               {QStringLiteral("message"),
                                                QStringLiteral("unsupported")}}}});
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]) == QByteArray("--mcp-fake-server")) {
        return runFakeMcpServer();
    }
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("WTFML-check"));
    app.setApplicationName(QStringLiteral("verify-agent"));
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        std::fprintf(stderr, "agent fixture unavailable\n");
        return 2;
    }
    const QString settingsDir = fixture.filePath(QStringLiteral("settings"));
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       fixture.filePath(QStringLiteral("system-settings")));
    requireIsolatedSettings(fixture.path(), "verify agent");
    app.setProperty("wtfml.testDefaultMinecraftDir", fixture.filePath(QStringLiteral("minecraft")));
    app.setProperty("wtfml.testAgentDataDir", fixture.filePath(QStringLiteral("agent")));

    runAgentSseParserTests();
    runAgentProviderAdapterTests();
    runAgentSessionStoreTests();
    runAgentSessionCryptoTests();
    runAgentCredentialStoreTests();
    runAgentContextTests();
    runAgentLoopTests();
    runAgentSkillsTests();
    runAgentMarkdownTests();
    runAgentPathPolicyTests();
    runAgentEgressPolicyTests();
    runAgentApprovalTests();
    runAgentArtifactTests();
    runAgentInstanceLockTests();
    runAgentBackupLifecycleTests();
    runAgentToolTests();
    runAgentOpenAiClientTests();
    runAgentDiagnosticProjectionTests();
    runAgentLauncherToolsTests();
    runAgentWorkflowTests();
    runAgentMcpProtocolTests();
    runAgentMcpTrustTests();
    runAgentMcpClientTests();
    runAgentWebPolicyTests();
    runAgentWebToolsTests();
    runAgentCommandToolsTests();
    runAgentPerformanceTests();

    out() << "agent: " << g_checks - g_failures << "/" << g_checks << "\n";
    return g_failures == 0 ? 0 : 1;
}
