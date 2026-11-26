/**
 * @file AgentContextBuilder.hpp
 * @brief 上下文预算：保守估算 token、保留最近轮次、确定性裁剪 + “更早的 N 条已省略”。
 *
 * Stage 1A 不接文件/工具结果；只处理系统提示、消息、预算。永远保留最后一条 user 消息
 * 和最近一段 assistant/tool 配对（本阶段只有 user/assistant）。
 */

#pragma once

#include "AgentTypes.hpp"

#include <QList>
#include <QString>

struct AgentContextReport {
    int estimatedTokens = 0;
    int budgetTokens = 0;
    int omittedMessages = 0;
    bool truncated = false;
    int messageCount = 0;
};

class AgentContextBuilder {
public:
    static int estimateTokens(const QString &text);
    static QString omissionMarker(int omittedCount);
    static int reservedOutputTokens(int maxOutput);
    /// 运行期读取当前实际加载的界面语言名（不缓存，切换后下一轮生效）。
    static QString interfaceLanguageName();
    static QString runtimeContextLine();
    static QString withRuntimeContext(const QString &systemPrompt);

    /// @param history 完整历史；返回的是准备发给模型的那一份（不含系统提示）。
    static QList<Agent::Message> build(const QList<Agent::Message> &history,
                                       const QString &systemPrompt, int contextWindow,
                                       int maxOutput, AgentContextReport *report = nullptr);
};
