/**
 * @file AgentRepairCases.hpp
 * @brief 本地修复病例库（P2）：把「症状键 → 动作 → 验证结果」记在本地并按成功率排序。
 *
 * 落盘在 `%LOCALAPPDATA%/WTFML/agent/repair-cases/`，默认关闭（`AgentSettings::repairCasesEnabled()`）。
 * 只存结构化的东西：
 * - `signature`：归一化症状键（异常类型、崩溃阶段、命中的规则 id），不含日志原文；
 * - `actions`：有界词表的动作标签（`scope:*` / `setting:*` / `file:<目录>:<类别>`）；
 * - `files`：本机相对路径明细，**只进本地 JSON，不进模型上下文**；
 * - 结果统计：尝试次数、验证通过次数、最后一次结果、更新时间。
 *
 * 排序在本地做：写盘不排序，读的时候按拉普拉斯平滑成功率 + 最近使用加权排。模型只看
 * `ranked()` 里投影后的少量记录（`AgentLauncherTools` 负责投影），看不到原文和绝对路径。
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentRepairCase {

/// 一条病例。字段全是结构化事实，没有日志/报告正文。
struct Case {
    QString id;
    QString signature;
    QStringList actions;   ///< 有界标签，可以进模型上下文
    QStringList files;     ///< 相对路径明细，只在本地看
    /// verified / not_verified / committed / rolled_back
    QString outcome;
    bool verified = false;
    QString gameVersion;
    QString loader;
    int attempts = 1;
    int successes = 0;
    qint64 updatedMs = 0;

    QJsonObject toJson() const;
    static Case fromJson(const QJsonObject &object);
};

/// 崩溃快照 → 症状键：`crash:<异常类名>|phase:<阶段>|rule:<id>+<id>`。
/// 只用结构字段（异常类型名、阶段、规则 id），不碰报告原文、路径或用户输入。
QString crashSignature(const QString &exception, const QString &phase,
                       const QStringList &ruleIds);

/// 事务兜底键：模型没给症状键时用 `repair:<排序后的范围>`。
QString repairSignature(const QStringList &scopes);

/// 键的形状：1–160 个 `[A-Za-z0-9_.:+|-]`。模型给的键也要过这一条才落盘。
bool validSignature(const QString &signature);

/// 上限（唯一出处）：条数、动作标签、路径明细、症状键长度。
int maxCases();
int maxActions();
int maxFiles();
int maxSignatureChars();

struct Entry {
    Case item;
    double score = 0.0;

    /// 能进模型上下文的那一份：只有动作标签与结果统计，没有相对路径明细。
    QJsonObject toModelJson() const;
};

/// 本地排序分：拉普拉斯平滑成功率 `(successes + 1) / (attempts + 2)`，
/// 三年内用过的按天数加一点（最多 0.05），其它情况只有成功率。
double rankScore(const Case &item, qint64 nowMs);

class Store {
public:
    explicit Store(const QString &root = QString());

    QString root() const { return m_root; }
    /// 默认关闭：`AgentSettings::repairCasesEnabled()` 为 false 时不读不写。
    bool enabled() const;

    /// 记一次尝试：同签名同动作合并成一条，`attempts` 加一、`successes` 按验证结果加一。
    /// 关闭时什么都不做并返回 true。
    bool record(const Case &item, QString *problem = nullptr);
    /// 只改结果（验证通过 → 提交/回滚），不增加尝试次数。
    bool markOutcome(const QString &signature, const QStringList &actions,
                     const QString &outcome, QString *problem = nullptr);
    /// 同签名的历史病例按本地分数排，最多 @p max 条。
    QList<Entry> ranked(const QString &signature, int max) const;
    QList<Case> all() const;
    bool clear(QString *problem = nullptr);

private:
    QString m_root;
};

}  // namespace AgentRepairCase
