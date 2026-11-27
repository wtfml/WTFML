/**
 * @file AgentAutoSafe.hpp
 * @brief Auto-safe 事务授权（docs/features/AGENT.md §2 D12、S5.7）：一次批准一个有范围、有
 *        时效的事务，事务内按已批准计划执行、不再逐次弹审批。
 *
 * 边界（D5 的例外，只在这个窗口里成立）：
 * - 只作用于当前锁定实例和它的事务工作区；范围来自已批准计划（工具、文件、设置键、模组、
 *   项目号）与 `repair.begin` 返回的事务/工作区；
 * - **开着就一直有效**：没有时长上限，也没有启动次数上限（2026-11-27 产品确认，D12 改口）。
 *   失效只有这几条：用户手动关、换模式、换会话、关窗、事务结束、实时复核失败；
 * - 计划审批始终要过：Auto-safe 跳的是「计划内每一步再点一次允许」，不是跳过计划；
 * - 硬禁止不随授权放开：`java.path` / `args.extraJvm`、`saves/` 下的文件、删除、`backup.restore`、
 *   `instance.repairCore`、范围外的工作区写入；
 * - 默认关闭、不随会话记忆；每次事务结束写审计（范围、实际操作、验证、回滚与结果）。
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
    /// 现在**不再限制**时长与启动次数（2026-11-27 产品确认）；两个字段留着只为审计旧事务。
    int maxMinutes = 0;   ///< 0 = 不限
    int maxLaunches = 0;  ///< 0 = 不限
};

class Authorizer {
public:
    void enable(const QString &instanceId, const QString &sessionId, const QDateTime &now,
                const Limits &limits = Limits{});
    /// @param reason 写进审计（用户关闭 / 换模式 / 换会话 / 过期 / 事务结束）。
    void disable(const QDateTime &now, const QString &reason = QString());
    bool enabled() const { return m_enabled; }
    /// 永远 false：不再有时效。保留这个函数是因为审计与界面还按「有没有到期」取一句话。
    bool expired(const QDateTime &now) const;
    bool matchesInstance(const QString &instanceId) const;
    QDateTime deadline() const { return m_expiresAt; }
    /// @returns 复核结果：开着、会话与实例都对得上。false = 当场失效（调用方要把它关掉）。
    /// 每次工具调用前调一次 —— 这比「只在打开那一刻判一次」结实，会话/实例被换掉时不会
    /// 拿着旧授权继续跑。
    bool revalidate(const QString &sessionId, const QString &instanceId);
    QString transactionId() const { return m_transactionId; }
    QString workspaceId() const { return m_workspaceId; }
    /// 本次事务共启动了几次（只给审计与界面看，不参与判定）。
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
