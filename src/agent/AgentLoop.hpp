/**
 * @file AgentLoop.hpp
 * @brief Stage 1A 的**无工具**对话循环：校验 → 构建上下文 → 流式请求 → 完成/失败/取消。
 *
 * 状态机按 docs/features/AGENT.md §8.1 收紧到本阶段实际会走的那几档：
 *   Idle → Preparing → Requesting → Completed / Failed / Cancelled
 * 不执行任何工具；`m_history` 里只会出现 user / assistant。
 *
 * 重试只发生在**首字节前**（网络/429/5xx），最多两次；流已经开始后失败保留部分文本，
 * 不自动重发。取消后旧请求的回调一律按 requestId 丢弃。
 */

#pragma once

#include "AgentModelClient.hpp"
#include "AgentSettings.hpp"
#include "AgentTool.hpp"
#include "AgentTypes.hpp"

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

class QTimer;

class AgentLoop : public QObject {
    Q_OBJECT

public:
    enum class State { Idle, Preparing, Requesting, AwaitingApproval, ExecutingTool, Completed, Failed, Cancelled };
    Q_ENUM(State)

    struct Config {
        AgentProviderProfile profile;
        QString apiKey;
        QString systemPrompt;
        int maxRetry = 2;
        /// 单个回合最多几轮工具（模型 → 工具 → 模型算一轮）。**0 = 不设上限**（默认）。
        int maxToolSteps = 0;
        /// 单个会话累计最多几次工具调用；**0 = 不设上限**（默认）。跨回合累计，切会话时由
        /// 控制器写入 —— 计数留着给审计和界面看，不再拦调用。
        int maxSessionToolCalls = 0;
        QList<int> retryDelaysMs{1000, 4000};
    };

    explicit AgentLoop(AgentModelClient *client, QObject *parent = nullptr);

    void setClient(AgentModelClient *client);
    void setToolEngine(AgentToolEngine *engine);
    AgentToolEngine *toolEngine() const { return m_toolEngine; }
    AgentModelClient *client() const { return m_client; }

    void setConfig(const Config &config);
    Config config() const { return m_config; }

    /// 用完整历史起一轮。历史最后一条必须是 user；不会重复追加。
    void setHistory(const QList<Agent::Message> &history);
    QList<Agent::Message> history() const { return m_history; }

    void start();
    /// 用户点“重试”：丢掉最后一条失败/中断的 assistant，再发一次。
    void retry();
    void cancel();
    /// 用户对当前待审批请求作出决定；只有 AwaitingApproval 状态才生效。
    void resolveApproval(bool approved);
    /// 用户点“本会话允许”：这一次照常执行，同一个来源在本会话里不再逐次确认。
    void resolveSessionApproval();

    bool busy() const;
    State state() const { return m_state; }
    Agent::Message assistantMessage() const { return m_assistant; }

    /// 切会话时写入这个会话已经用掉的工具调用次数（只用于显示与审计，不再拦调用）。
    void setSessionToolCalls(int used);
    int sessionToolCalls() const { return m_sessionToolCalls; }

    /// 本轮内相同参数的只读工具可以复用最近的成功结果。连续整轮只复用达到此次数时停止循环。
    static constexpr int kRepeatReuseWindow = 3;

    /// 测试专用：把退避压成 0，避免单测等 1s/4s。
    void setRetryDelaysForTest(const QList<int> &delays);

    static QString stateName(State state);

signals:
    void stateChanged(AgentLoop::State state);
    void assistantUpdated(const Agent::Message &message);
    void turnFinished(const Agent::Message &message);
    void turnFailed(const Agent::Error &error);
    void usageUpdated(const Agent::Usage &usage);
    /// 这一轮发出去的那份上下文用掉几分满（0–100）。控制器按 ≥90 另起一段对话。
    void contextBudgetUpdated(int usedPercent, int estimatedTokens);
    void messageAdded(const Agent::Message &message);
    void toolCallStarted(const Agent::ToolCall &call);
    void toolCallFinished(const Agent::ToolCall &call, const AgentToolResult &result);
    void approvalRequested(const QString &approvalId, const Agent::ToolCall &call,
                           const QJsonObject &request);

private:
    void setState(State state);
    void beginRequest(bool isRetry);
    void handleFinished(const QString &requestId, const Agent::ModelReply &reply);
    void handleFailed(const QString &requestId, const Agent::Error &error);
    void deliverFailure(const Agent::Error &error);
    void appendAssistantToHistory();
    void processToolCalls();
    void handleToolOutcome(const Agent::ToolCall &call, const AgentToolResult &result,
                           bool recordForReuse = true);
    /// 上面那次工具调用的签名（工具名 + 规范化参数）在最近几次里有没有成功过；有就把结果
    /// 抄一份回来，summary 标一句「重复调用，已复用上次结果」。
    bool reuseRepeatedResult(const Agent::ToolCall &call);
    static bool canReuseResult(const Agent::ToolCall &call);
    static QString callSignature(const Agent::ToolCall &call);
    void invalidateRequest();
    /// 异步工具（MCP）的最终结果从这里回来；generation 或状态不对就丢弃。
    void onAsyncToolDone(const Agent::ToolCall &call, const AgentToolResult &result);
    Agent::Error configurationError() const;

    AgentModelClient *m_client = nullptr;
    AgentToolEngine *m_toolEngine = nullptr;
    Config m_config;
    QList<Agent::Message> m_history;
    Agent::Message m_assistant;
    Agent::Usage m_usage;
    Agent::Error m_lastError;
    QString m_requestId;
    State m_state = State::Idle;
    int m_retryAttempt = 0;
    int m_generation = 0;
    int m_toolSteps = 0;
    int m_sessionToolCalls = 0;
    /// 最近几次实际执行的只读调用。写入或其他有副作用的调用会清空这份结果。
    QList<QPair<QString, AgentToolResult>> m_recentResults;
    int m_reusedToolRounds = 0;
    bool m_invokedToolThisRound = false;
    int m_nextToolCall = 0;
    QList<Agent::ToolCall> m_toolQueue;
    QString m_approvalId;
    Agent::ToolCall m_pendingCall;
    bool m_cancelled = false;
    bool m_assistantAppended = false;
    /// 正在等一个异步工具的结果：期间不能再发请求，也不能按"同步完成"推进。
    bool m_asyncPending = false;
    int m_asyncGeneration = 0;
};
