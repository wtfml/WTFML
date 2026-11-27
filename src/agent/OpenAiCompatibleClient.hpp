/**
 * @file OpenAiCompatibleClient.hpp
 * @brief OpenAI-compatible `/chat/completions` 的真实网络实现（流式 / 非流式）。
 *
 * 网络边界（docs/features/AGENT.md §5.3）：
 * - URL 先在 ProviderAdapters 里校验；这里不跟随重定向；
 * - 不忽略 TLS 错误；key 只进认证头，不进 URL/日志/错误正文；
 * - 响应总额 16 MB、SSE 单行 1 MB；
 * - 只记录方法/host/状态码/耗时/错误分类，不记 Authorization。
 */

#pragma once

#include "AgentModelClient.hpp"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class AgentSseParser;

class OpenAiCompatibleClient : public AgentModelClient {
    Q_OBJECT

public:
    explicit OpenAiCompatibleClient(QObject *parent = nullptr);
    ~OpenAiCompatibleClient() override;

    void start(const Agent::ModelRequest &request) override;
    void cancel(const QString &requestId) override;
    void testConnection(const Agent::ModelRequest &request) override;
    void listModels(const Agent::ModelRequest &request) override;

    /// 测试专用：注入自己的 QNetworkAccessManager（本地假服务器）。
    void setNetworkAccessManagerForTest(QNetworkAccessManager *manager);

    static int maxResponseBytes();
    static int maxSseLineBytes();

private:
    struct ActiveRequest {
        QNetworkReply *reply = nullptr;
        Agent::ModelRequest request;
        bool streaming = false;
        bool receivedAny = false;
        bool finishedSeen = false;
        bool aborted = false;
        QByteArray buffer;
        QString text;
        QString reasoning;
        QHash<int, Agent::ToolCall> toolCalls;
        Agent::Usage usage;
        QString finishReason;
        AgentSseParser *parser = nullptr;
        bool isConnectionTest = false;
        bool isModelList = false;
        qint64 startedAt = 0;
        qint64 totalBytes = 0;
    };

    void begin(const Agent::ModelRequest &request, bool connectionTest);
    void beginModelList(const Agent::ModelRequest &request);
    void feedReply(const QString &requestId, QNetworkReply *reply);
    void processStreamJson(const QString &requestId, ActiveRequest *active, const QByteArray &data);
    void processNonStreamBody(const QString &requestId, ActiveRequest *active, const QByteArray &body);
    void finishReply(const QString &requestId, ActiveRequest *active);
    void failRequest(const QString &requestId, const Agent::Error &error);
    Agent::Error classifyNetworkError(QNetworkReply *reply) const;
    Agent::Error classifyHttpStatus(int status, const QByteArray &body) const;
    ActiveRequest *active(const QString &requestId);
    void cleanup(ActiveRequest *active);
    Agent::ModelReply replyFromActive(const ActiveRequest &active) const;

    QNetworkAccessManager *m_manager = nullptr;
    bool m_ownsManager = false;
    QHash<QString, ActiveRequest *> m_active;
};
