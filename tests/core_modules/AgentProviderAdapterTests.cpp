/**
 * @file AgentProviderAdapterTests.cpp
 * @brief 枚举化协议适配与 URL 边界（本地假服务不会发请求）。
 */

#include "Support.hpp"

#include "agent/AgentProviderAdapters.hpp"

void runAgentProviderAdapterTests() {
    QString problem;
    bool loopback = false;
    check(!AgentProviderAdapters::validateBaseUrl("http://api.example.com/v1", false, &loopback,
                                                  &problem),
          "URL：远程 http 被拒绝");
    check(AgentProviderAdapters::validateBaseUrl("http://127.0.0.1:11434/v1", true, &loopback,
                                                 &problem) &&
              loopback,
          "URL：显式允许的 loopback http 通过");
    check(!AgentProviderAdapters::validateBaseUrl("http://127.0.0.1:11434/v1", false, &loopback,
                                                  &problem),
          "URL：loopback 没勾选“允许本地明文 HTTP”也拒绝");
    check(!AgentProviderAdapters::validateBaseUrl("https://user:pass@api.example.com/v1", false,
                                                  &loopback, &problem),
          "URL：userinfo 被拒绝");
    check(!AgentProviderAdapters::validateBaseUrl("https://api.example.com/v1?x=1", false,
                                                  &loopback, &problem),
          "URL：查询串被拒绝");
    check(!AgentProviderAdapters::validateBaseUrl("https://api.example.com/v1#frag", false,
                                                  &loopback, &problem),
          "URL：片段被拒绝");
    check(AgentProviderAdapters::validateBaseUrl("https://api.example.com/v1/", false, &loopback,
                                                 &problem),
          "URL：https 通过");

    AgentProviderProfile profile;
    profile.id = QStringLiteral("p");
    profile.baseUrl = QStringLiteral("https://api.example.com/v1");
    profile.model = QStringLiteral("m");
    check(AgentProviderAdapters::validateProfile(profile, &problem),
          "profile：基础字段有效", problem);
    profile.authMode = QStringLiteral("api-key-header");
    const QByteArray name = AgentProviderAdapters::authHeaderName(profile);
    const QByteArray value = AgentProviderAdapters::authHeaderValue(profile, QStringLiteral("secret-key"));
    check(name == QByteArray("api-key") && value == QByteArray("secret-key"),
          "adapter：api-key-header 固定头名");
    profile.authMode = QStringLiteral("bearer");
    check(AgentProviderAdapters::authHeaderValue(profile, QStringLiteral("secret-key")) ==
              QByteArray("Bearer secret-key"),
          "adapter：bearer 头");
    profile.authMode = QStringLiteral("bogus");
    check(!profile.validateEnumFields(&problem), "adapter：未知认证模式拒绝");

    profile.authMode = QStringLiteral("bearer");
    profile.maxTokensField = QStringLiteral("max_completion_tokens");
    profile.systemRole = QStringLiteral("developer");
    profile.reasoningEcho = QStringLiteral("send-back");
    profile.sendTemperature = false;
    profile.sendToolChoice = false;
    profile.stream = false;
    Agent::Message user;
    user.role = Agent::Role::User;
    user.content = QStringLiteral("hi");
    Agent::Message assistant;
    assistant.role = Agent::Role::Assistant;
    assistant.content = QStringLiteral("hello");
    assistant.reasoning = QStringLiteral("think");
    QJsonArray tools;
    tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                             {QStringLiteral("function"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("fs.list")}}}});
    const QJsonObject request = AgentProviderAdapters::buildRequest(
        profile, {user, assistant}, QStringLiteral("system prompt"), tools);
    check(request.value(QStringLiteral("stream")).toBool(true) == false,
          "adapter：stream=off 写进请求");
    check(request.contains(QStringLiteral("max_completion_tokens")) &&
              !request.contains(QStringLiteral("max_tokens")),
          "adapter：max_tokens 字段名切换");
    check(!request.contains(QStringLiteral("temperature")),
          "adapter：temperature 可以省略");
    check(!request.contains(QStringLiteral("tool_choice")),
          "adapter：tool_choice 可以省略");
    const QJsonArray messages = request.value(QStringLiteral("messages")).toArray();
    check(messages.size() == 3, "adapter：system + user + assistant", QString::number(messages.size()));
    check(messages.at(0).toObject().value(QStringLiteral("role")).toString() ==
              QStringLiteral("developer"),
          "adapter：system role 可映射成 developer");
    check(messages.at(2).toObject().value(QStringLiteral("reasoning_content")).toString() ==
              QStringLiteral("think"),
          "adapter：reasoning 按 send-back 回传");

    profile.baseUrl = QStringLiteral("https://api.openai.com/v1");
    profile.stream = true;
    const auto openAi = AgentProviderAdapters::buildRequest(profile, {user}, {}, {});
    check(openAi.value(QStringLiteral("stream_options")).toObject()
              .value(QStringLiteral("include_usage")).toBool(),
          "#163 OpenAI 流式请求要求返回用量");
    profile.baseUrl = QStringLiteral("https://api.example.com/v1");
    check(!AgentProviderAdapters::buildRequest(profile, {user}, {}, {})
              .contains(QStringLiteral("stream_options")),
          "#163 兼容端点保留原有流式请求选项");
}
