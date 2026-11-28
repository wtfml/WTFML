/**
 * @file AgentOpenAiClientTests.cpp
 * @brief 本地假服务器：流式请求遇到 400 时保留供应商 JSON 错误体；工具线上名不含点号。
 */

#include "Support.hpp"

#include "agent/AgentArtifactStore.hpp"
#include "agent/AgentFileToolEngine.hpp"
#include "agent/AgentFileTools.hpp"
#include "agent/OpenAiCompatibleClient.hpp"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

void runAgentOpenAiClientTests() {
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost, 0), "客户端：本地假服务器启动");
    QByteArray requestBody;
    QByteArray response;
    const auto prepareResponse = [&response](const QByteArray &status, const QByteArray &type,
                                             const QByteArray &body) {
        response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                   "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) +
                   "\r\n\r\n" + body;
    };
    prepareResponse("400 Bad Request", "application/json",
        R"({"error":{"message":"Invalid 'tools[0].function.name': string does not match pattern. Expected a string that matches the pattern '^[a-zA-Z0-9_-]+$'.","type":"invalid_request_error"}})");
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &requestBody, &response] {
        QTcpSocket *socket = server.nextPendingConnection();
        if (socket == nullptr) return;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &requestBody, &response] {
            QByteArray data = socket->readAll();
            requestBody.append(data);
            const int headerEnd = requestBody.indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            const QByteArray headers = requestBody.left(headerEnd);
            int contentLength = 0;
            for (const QByteArray &line : headers.split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    contentLength = line.mid(15).trimmed().toInt();
                }
            }
            if (requestBody.size() < headerEnd + 4 + contentLength) return;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    QTemporaryDir fixture;
    AgentFileTools files;
    AgentArtifactStore artifacts(fixture.path());
    AgentFileToolEngine engine(&files, &artifacts, [](QString *) { return AgentToolContext{}; });

    OpenAiCompatibleClient client;
    Agent::ModelRequest request;
    request.requestId = QStringLiteral("req-1");
    request.baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    request.model = QStringLiteral("deepseek-flash");
    request.apiKey = QStringLiteral("test-key-123456");
    request.localHttpAllowed = true;
    request.stream = true;
    request.tools = engine.schemas();
    request.systemPrompt = QStringLiteral("system");
    Agent::Message user;
    user.role = Agent::Role::User;
    user.content = QStringLiteral("hi");
    request.messages = {user};

    Agent::Error captured;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&client, &AgentModelClient::failed, &client,
                     [&](const QString &id, const Agent::Error &error) {
                         if (id != request.requestId) return;
                         captured = error;
                         loop.quit();
                     });
    deadline.start(4000);
    client.start(request);
    if (captured.isEmpty()) loop.exec();
    check(captured.kind == Agent::ErrorKind::Protocol,
          "客户端：HTTP 400 分类为 Protocol", Agent::errorKey(captured.kind));
    check(captured.message.contains(QStringLiteral("tools[0].function.name")),
          "客户端：400 的非流式错误体不会被 SSE 解析丢掉", captured.message);

    const int bodyStart = requestBody.indexOf("\r\n\r\n");
    const QJsonDocument document =
        QJsonDocument::fromJson(requestBody.mid(bodyStart + 4));
    const QJsonArray tools = document.object().value(QStringLiteral("tools")).toArray();
    bool allWireNamesSafe = !tools.isEmpty();
    for (const QJsonValue &value : tools) {
        const QString name = value.toObject()
                                 .value(QStringLiteral("function"))
                                 .toObject()
                                 .value(QStringLiteral("name"))
                                 .toString();
        allWireNamesSafe = allWireNamesSafe && !name.contains(QLatin1Char('.'));
    }
    check(allWireNamesSafe, "客户端：请求里的 function name 不含点号，符合 DeepSeek/OpenAI 规则");

    Agent::Usage usage;
    QObject::connect(&client, &AgentModelClient::finished, &loop,
                     [&](const QString &id, const Agent::ModelReply &reply) {
                         if (id != request.requestId) return;
                         usage = reply.usage;
                         loop.quit();
                     });
    prepareResponse("200 OK", "application/json",
        R"({"choices":[{"message":{"content":"ready"},"finish_reason":"stop"}],"usage":{"prompt_tokens":1200,"completion_tokens":2,"prompt_cache_hit_tokens":1024}})");
    requestBody.clear();
    request.requestId = QStringLiteral("req-cache-json");
    request.stream = false;
    request.reasoningEcho = QStringLiteral("send-back");
    request.messages.first().modelContext = QStringLiteral("Workflow state: collect");
    Agent::Message assistant;
    assistant.role = Agent::Role::Assistant;
    assistant.reasoning = QStringLiteral("previous reasoning");
    assistant.content = QStringLiteral("previous reply");
    request.messages.append(assistant);
    deadline.start(4000);
    client.start(request);
    loop.exec();
    check(usage.cachedPromptTokens == 1024 && usage.promptTokens == 1200,
          "#163 客户端读取 DeepSeek 非流式缓存命中");
    check(Agent::Usage::fromJson(usage.toJson()).cachedPromptTokens == 1024 &&
              Agent::Usage::fromJson({}).cachedPromptTokens == -1,
          "#163 缓存统计可往返保存，旧会话缺省为未知");
    const auto jsonRequest = QJsonDocument::fromJson(requestBody.mid(requestBody.indexOf("\r\n\r\n") + 4)).object();
    const auto jsonMessages = jsonRequest.value(QStringLiteral("messages")).toArray();
    check(jsonRequest.value(QStringLiteral("stream")).isBool() && !jsonRequest.value(QStringLiteral("stream")).toBool() &&
              jsonMessages.at(2).toObject().value(QStringLiteral("reasoning_content")).toString() == assistant.reasoning,
          "#163 实际 API 请求按配置发送非流式与历史推理");

    prepareResponse("200 OK", "text/event-stream",
        "data: {\"choices\":[{\"delta\":{\"content\":\"ready\"},\"finish_reason\":\"stop\"}],\"usage\":null}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1200,\"completion_tokens\":2,\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n\n"
        "data: [DONE]\n\n");
    requestBody.clear();
    request.requestId = QStringLiteral("req-cache-sse");
    request.stream = true;
    user.content = QStringLiteral("continue");
    user.modelContext = QStringLiteral("Workflow state: execute");
    request.messages.append(user);
    usage = {};
    deadline.start(4000);
    client.start(request);
    loop.exec();
    check(usage.cachedPromptTokens == 0 && usage.promptTokens == 1200,
          "#163 客户端读取 OpenAI 流式缓存明细，零命中区别于未知");
    const auto sseRequest = QJsonDocument::fromJson(requestBody.mid(requestBody.indexOf("\r\n\r\n") + 4)).object();
    auto sseMessages = sseRequest.value(QStringLiteral("messages")).toArray();
    const bool grewByOne = sseMessages.size() == jsonMessages.size() + 1 && !jsonMessages.isEmpty();
    if (grewByOne) sseMessages.removeLast();
    check(grewByOne && sseMessages == jsonMessages &&
              sseRequest.value(QStringLiteral("tools")) == jsonRequest.value(QStringLiteral("tools")),
          "#163 实际 API 请求保留已有消息和工具定义");
}
