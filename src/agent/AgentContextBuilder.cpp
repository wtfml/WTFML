/**
 * @file AgentContextBuilder.cpp
 * @brief 确定性裁剪（没有 LLM 摘要，见 docs/features/AGENT.md §7.3）。
 */

#include "AgentContextBuilder.hpp"

#include "../../ui/UiLanguage.hpp"

#include <cmath>

namespace {

bool isCjk(uint ucs4) {
    return (ucs4 >= 0x3040 && ucs4 <= 0x30ff) ||   // 日文假名
           (ucs4 >= 0x3400 && ucs4 <= 0x9fff) ||   // CJK 扩展/基本区
           (ucs4 >= 0xf900 && ucs4 <= 0xfaff) ||   // 兼容表意
           (ucs4 >= 0xac00 && ucs4 <= 0xd7af);     // 韩文
}

}  // namespace

int AgentContextBuilder::estimateTokens(const QString &text) {
    // 保守启发式：CJK 一个字 1 token，其余字符 0.3 token（略高于常见 BPE 的英文比例）。
    double ascii = 0.0;
    double cjk = 0.0;
    for (const QChar ch : text) {
        if (isCjk(ch.unicode())) {
            cjk += 1.0;
        } else if (ch.unicode() < 0x80) {
            ascii += 0.3;
        } else {
            // emoji / 其他非 ASCII：保守按 1 token 算。
            cjk += 1.0;
        }
    }
    return int(ascii + cjk + 0.999);
}

int AgentContextBuilder::reservedOutputTokens(int maxOutput) {
    return qMax(1, maxOutput);
}

int AgentContextBuilder::budgetFor(int contextWindow, int maxOutput) {
    return qMax(0, contextWindow - reservedOutputTokens(maxOutput) - 512);
}

int AgentContextBuilder::usedPercent(int usedTokens, int contextWindow, int maxOutput) {
    const int budget = budgetFor(contextWindow, maxOutput);
    if (budget <= 0) return 100;
    return qBound(0, int(std::ceil(usedTokens * 100.0 / budget)), 100);
}

QString AgentContextBuilder::omissionMarker(int omittedCount) {
    return QStringLiteral("(Earlier %1 message(s) omitted)").arg(qMax(0, omittedCount));  // i18n-ignore
}

QString AgentContextBuilder::interfaceLanguageName() {
    return UiLanguage::isEnglish() ? QStringLiteral("English") : QStringLiteral("Simplified Chinese");
}

QString AgentContextBuilder::runtimeContextLine() {
    return QStringLiteral("Interface language: %1").arg(interfaceLanguageName());
}

QString AgentContextBuilder::withRuntimeContext(const QString &systemPrompt) {
    const QString contextLine = runtimeContextLine();
    if (systemPrompt.isEmpty()) return contextLine;
    return systemPrompt + QStringLiteral("\n\n") + contextLine;
}

QList<Agent::Message> AgentContextBuilder::build(const QList<Agent::Message> &history,
                                                 const QString &systemPrompt, int contextWindow,
                                                 int maxOutput, AgentContextReport *report) {
    AgentContextReport result;
    result.budgetTokens = budgetFor(contextWindow, maxOutput);
    QList<Agent::Message> picked;
    if (history.isEmpty()) {
        if (report != nullptr) *report = result;
        return picked;
    }

    // 从最新往回装；永远保留最后一条（通常是 user 消息）。
    double used = estimateTokens(systemPrompt);
    int firstKept = history.size();
    for (int i = history.size() - 1; i >= 0; --i) {
        const double cost = estimateTokens(history.at(i).content) +
                             estimateTokens(history.at(i).reasoning) +
                             estimateTokens(history.at(i).modelContext) + 4;
        const bool mustKeep = i == history.size() - 1;
        if (!mustKeep && used + cost > result.budgetTokens) {
            break;
        }
        used += cost;
        firstKept = i;
    }
    // assistant/tool 配对不能拆开：截断点落在 tool 消息上时，把产生它的 assistant
    // 调用一起带上；中间不允许只剩孤立 tool 结果。
    while (firstKept > 0 && history.at(firstKept).role == Agent::Role::Tool) {
        --firstKept;
        while (firstKept > 0 && history.at(firstKept).role != Agent::Role::Assistant) {
            --firstKept;
        }
    }
    if (firstKept > 0) {
        Agent::Message marker;
        marker.role = Agent::Role::System;
        marker.content = omissionMarker(firstKept);
        marker.status = Agent::MessageStatus::Complete;
        marker.createdAt = history.at(0).createdAt;
        picked.append(marker);
        result.omittedMessages = firstKept;
        result.truncated = true;
        used += estimateTokens(marker.content);
    }
    for (int i = firstKept; i < history.size(); ++i) {
        picked.append(history.at(i));
    }
    result.messageCount = picked.size();
    result.estimatedTokens = int(used + 0.999);
    result.usedPercent = usedPercent(result.estimatedTokens, contextWindow, maxOutput);
    if (report != nullptr) *report = result;
    return picked;
}
