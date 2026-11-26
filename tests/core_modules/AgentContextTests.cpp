/**
 * @file AgentContextTests.cpp
 * @brief 预算裁剪、CJK 估算、保留最后 user、省略标记。
 */

#include "Support.hpp"

#include "agent/AgentContextBuilder.hpp"

#include <QCoreApplication>
#include <QTranslator>

namespace {

class EnglishUiTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (qstrcmp(context, "UiLanguage") == 0 && qstrcmp(sourceText, "zh") == 0) {
            return QStringLiteral("en");
        }
        return {};
    }
};

}  // namespace

void runAgentContextTests() {
    check(AgentContextBuilder::estimateTokens(QStringLiteral("hello")) >= 1,
          "上下文：ASCII 估算非零");
    const int cjk = AgentContextBuilder::estimateTokens(QStringLiteral("启动崩溃"));
    const int ascii = AgentContextBuilder::estimateTokens(QStringLiteral("abcd"));
    check(cjk >= 4 && cjk > ascii, "上下文：CJK 权重高于同长度 ASCII",
          QString("%1 vs %2").arg(cjk).arg(ascii));
    check(!AgentContextBuilder::omissionMarker(3).isEmpty() &&
              AgentContextBuilder::omissionMarker(3).contains(QStringLiteral("3")),
          "上下文：省略标记带条数");

    QList<Agent::Message> history;
    for (int i = 0; i < 20; ++i) {
        Agent::Message message;
        message.id = QStringLiteral("m%1").arg(i);
        message.role = i % 2 == 0 ? Agent::Role::User : Agent::Role::Assistant;
        message.content = QString(200, QLatin1Char('a')) + QString::number(i);
        message.status = Agent::MessageStatus::Complete;
        history.append(message);
    }
    AgentContextReport report;
    const QList<Agent::Message> kept = AgentContextBuilder::build(
        history, QStringLiteral("system"), 1200, 200, &report);
    check(!kept.isEmpty() && kept.constLast().id == QStringLiteral("m19"),
          "上下文：永远保留最后一条消息");
    check(report.omittedMessages > 0 && report.truncated, "上下文：超预算时裁剪并标记",
          QString::number(report.omittedMessages));
    check(kept.constFirst().role == Agent::Role::System &&
              kept.constFirst().content.contains(QStringLiteral("omitted")),
          "上下文：旧消息位置插入省略标记");
    check(report.estimatedTokens <= report.budgetTokens ||
              kept.size() == 1,
          "上下文：裁剪后 (近似) 在预算内",
          QString("%1/%2").arg(report.estimatedTokens).arg(report.budgetTokens));

    AgentContextReport smallReport;
    const QList<Agent::Message> one = AgentContextBuilder::build(
        {history.constLast()}, QStringLiteral("system"), 1200, 200, &smallReport);
    check(one.size() == 1 && one.constFirst().id == QStringLiteral("m19"),
          "上下文：不需要裁剪时不插标记");
    check(!smallReport.truncated, "上下文：未裁剪时 truncated=false");

    QList<Agent::Message> huge;
    Agent::Message hugeMessage = history.constLast();
    hugeMessage.content = QString(100000, QLatin1Char('x'));
    huge.append(hugeMessage);
    AgentContextReport hugeReport;
    const QList<Agent::Message> keptHuge = AgentContextBuilder::build(
        huge, QStringLiteral("system"), 1200, 200, &hugeReport);
    check(keptHuge.size() == 1 && keptHuge.constLast().content.size() == 100000,
          "上下文：最后一条 user 即使超预算也保留（交给上层报 ContextOverflow）");

    {
        check(AgentContextBuilder::interfaceLanguageName() == QStringLiteral("Simplified Chinese"),
              "上下文：未装英文翻译时界面语言为 Simplified Chinese");
        check(AgentContextBuilder::withRuntimeContext(QStringLiteral("sys")) ==
                  QStringLiteral("sys\n\nInterface language: Simplified Chinese"),
              "上下文：运行时系统提示带 Simplified Chinese");
        EnglishUiTranslator translator;
        QCoreApplication::installTranslator(&translator);
        check(AgentContextBuilder::interfaceLanguageName() == QStringLiteral("English"),
              "上下文：安装英文翻译后动态切为 English");
        check(AgentContextBuilder::withRuntimeContext(QStringLiteral("sys")) ==
                  QStringLiteral("sys\n\nInterface language: English"),
              "上下文：运行时系统提示即时切为 English");
        QCoreApplication::removeTranslator(&translator);
        check(AgentContextBuilder::interfaceLanguageName() == QStringLiteral("Simplified Chinese"),
              "上下文：移除英文翻译后即时回到 Simplified Chinese");
    }
}
