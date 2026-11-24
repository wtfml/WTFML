/**
 * @file AgentKnowledgeRules.hpp
 * @brief 内嵌诊断知识：崩溃规则与启动里程碑标记的加载、校验、匹配
 *        （docs/dev/AGENT_AUTONOMY.md S5.3，字段约定见 assets/agent/knowledge/README.md）。
 *
 * 规则是**数据**，不是 C++ 常量：崩溃签名会随加载器和模组生态变，改一次 JSON 就够了。
 * 这一层只认字面量匹配，不做正则、不猜模组责任；置信度由规则的基础值和证据类型算出，
 * 上限 0.95。
 *
 * 坏规则（字段缺失、id 重复、字符串超长）跳过并记进 `problems`：一条规则写错不该让
 * 整个工具失去结论。文件本身读不到时 `rules` 为空，调用方只给事实、不给置信度。
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentKnowledge {

/// 一条崩溃规则（`crash_rules.json` 的 `rules[]`）。
struct CrashRule {
    QString id;
    int priority = 0;
    QString phase;
    QStringList match;      ///< 任一命中即算这条规则命中
    QStringList required;   ///< 额外条件：每条都要出现
    QStringList matchLower;     ///< 小写副本：匹配时整段文本只降一次
    QStringList requiredLower;
    QStringList suspects;   ///< 规则自带的模组线索；只算“被提到”
    double confidence = 0.0;
    QString confidenceBasis;
    QStringList recommendedTests;
    QStringList suggestedActions;

    bool matches(const QString &text) const;
};

struct CrashRuleSet {
    QString version;
    QList<CrashRule> rules;   ///< 已按 priority 从大到小排好
    QStringList problems;     ///< 坏规则/坏字段；空表示整份文件都认得
    QString resourceProblem;  ///< 资源读不到/不是有效 JSON

    bool ok() const { return !rules.isEmpty(); }
    /// 命中的规则，保持 priority 顺序。
    QList<CrashRule> match(const QString &text) const;
};

/// 从 `:/agent/knowledge/crash_rules.json` 加载（进程内缓存一份）。
CrashRuleSet loadCrashRules();
/// 纯解析：给一段 JSON 字节，返回规则集；测试按它逐格断言。
CrashRuleSet parseCrashRules(const QByteArray &json);

/// 允许的 `phase` 值（加载时用它筛规则）。
QStringList phases();

struct MilestoneRuleSet {
    QHash<QString, QStringList> milestones;
    QString version;
    QStringList problems;
    QString resourceProblem;

    bool ok() const;
    QStringList markers(const QString &milestone) const { return milestones.value(milestone); }
};

MilestoneRuleSet loadMilestones();
MilestoneRuleSet parseMilestones(const QByteArray &json);

/// 一行日志是否命中某个里程碑的标记（大小写敏感：游戏原文就是这么打的）。
bool milestoneMatches(const QString &milestone, const QString &line);

}  // namespace AgentKnowledge
