/**
 * @file AgentAutoSafe.hpp
 * @brief Auto-safe 事务授权（docs/features/AGENT.md §2 D12、S5.7）：一次批准一个有范围、有
 *        时效的事务，事务内按已批准计划执行、不再逐次弹审批。
 *
 * 边界（D5 的例外，只在这个窗口里成立）：
 * - 只作用于当前锁定实例和它的事务工作区；范围来自已批准计划（工具、文件、设置键、模组、
 *   项目号）与 `repair.begin` 返回的事务/工作区；
 * - 到期（默认 60 分钟）或用满启动次数（默认 20）就退回逐次确认，不静默续期；
 * - 硬禁止不随授权放开：`java.path` / `args.extraJvm`、`saves/` 下的文件、删除、`backup.restore`、
 *   `instance.repairCore`、范围外的工作区写入；
 * - 默认关闭、不随会话记忆；关窗/换模式/换会话即失效；每次事务结束写审计（范围、实际操作、
 *   验证、回滚与结果）。
 *
 * 这一层是纯记帐 + 判定：不执行工具、不读盘（审计落盘除外），时间由调用方传入。
 */

#pragma once

#include "AgentTool.hpp"
#include "AgentWorkflow.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentAutoSafe {

enum class Decision { Allow, RequireApproval, Deny };

struct Limits {
    int maxMinutes = 60;   ///< 1–60
    int maxLaunches = 20;  ///< 1–40
};

class Authorizer {
public:
    void enable(const QString &instanceId, const QString &sessionId, const QDateTime &now,
                const Limits &limits = Limits{});
    /// @param reason 写进审计（用户关闭 / 换模式 / 换会话 / 过期 / 事务结束）。
    void disable(const QDateTime &now, const QString &reason = QString());
    bool enabled() const { return m_enabled; }
    bool expired(const QDateTime &now) const;
    bool matchesInstance(const QString &instanceId) const;
    QDateTime deadline() const { return m_expiresAt; }
    QString transactionId() const { return m_transactionId; }
    QString workspaceId() const { return m_workspaceId; }
    int launchesUsed() const { return m_launches; }
    int maxLaunches() const { return m_limits.maxLaunches; }
    int remainingLaunches() const { return qMax(0, m_limits.maxLaunches - m_launches); }
    QStringList coveredTools() const { return m_tools; }
    QStringList coveredPaths() const { return m_paths; }
    QStringList coveredKeys() const { return m_keys; }
    QStringList coveredModFiles() const { return m_modFiles; }
    QStringList repairScopes() const { return m_scopes; }

    /// 计划批准后调用：把计划里的写范围记下来。
    void bindPlan(const QList<AgentPlanStep> &steps);
    /// `repair.begin` 完成后调用：绑定事务与工作区。
    void bindTransaction(const QString &transactionId, const QString &workspaceId);
    void setRepairScopes(const QStringList &scopes);
    void noteVerification(const QJsonObject &verification);

    /// 判定一次工具调用。@p launchCount 回这次要消耗的启动次数（0 = 不消耗）。
    Decision authorize(const Agent::ToolCall &call, const QDateTime &now, QString *reason,
                       int *launchCount = nullptr) const;

    /// 记一条实际操作（给审计）；@p launches 是这次调用实际消耗的启动次数。
    void record(const Agent::ToolCall &call, const AgentToolResult &result, int launches,
                const QDateTime &now);

    /// 事务结束：写审计到 `agent/transactions/<id>/audit.json` 并失效；返回审计 JSON。
    QJsonObject finish(const QString &finalState, const QString &rollbackResult,
                       const QDateTime &now);

    QJsonObject scopeJson() const;
    QJsonObject audit() const;

    static QString decisionName(Decision decision);

private:
    void note(const QString &text);

    bool m_enabled = false;
    QString m_instanceId;
    QString m_sessionId;
    QString m_transactionId;
    QString m_workspaceId;
    QDateTime m_startedAt;
    QDateTime m_expiresAt;
    Limits m_limits;
    QStringList m_tools;
    QStringList m_paths;
    QStringList m_keys;
    QStringList m_modFiles;
    QStringList m_projectIds;
    QStringList m_scopes;
    int m_launches = 0;
    QJsonArray m_operations;
    QJsonObject m_verification;
    QStringList m_notes;
    QJsonObject m_finishedAudit;
};

}  // namespace AgentAutoSafe
