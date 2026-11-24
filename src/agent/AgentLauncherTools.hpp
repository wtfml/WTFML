/**
 * @file AgentLauncherTools.hpp
 * @brief Stage 2 启动器工具（只读诊断 + 受控设置/模组/备份），全部经 `AgentLauncherHost`。
 *
 * 分派与实现见 docs/features/AGENT.md §9.4：只读工具自动执行；完整实例清单、完整模组
 * 文件名、设置写、模组启停、备份创建与恢复逐次审批。诊断结果先过
 * `AgentDiagnosticProjection` 再进模型上下文。
 *
 * 与文件工具一样：实例来自会话锁定，工具参数里没有实例 id；拒绝审批时一个字节不改。
 */

#pragma once

#include "AgentEgressPolicy.hpp"
#include "AgentLauncherHost.hpp"
#include "AgentTool.hpp"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class AgentLauncherTools : public AgentToolEngine {
public:
    using ContextProvider = std::function<AgentToolContext(QString *problem)>;

    AgentLauncherTools(AgentLauncherHost *host, ContextProvider provider);

    static QList<AgentToolSpec> specs();
    /// `settings.set` 允许写的键（唯一出处：schema 与执行共用这一份）。
    static QStringList settingsKeys();
    static bool isLauncherTool(const QString &internalName);

    QJsonArray schemas() const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    void cancelPending() override;
    bool handles(const QString &internalName) const override;

    void setStrictEgress(bool strict) { m_strictEgress = strict; }
    void setHost(AgentLauncherHost *host) { m_host = host; }

private:
    struct Pending {
        enum class Kind {
            Generic,        ///< 只读结果：算好了，等用户确认外发
            SettingsWrite,
            ModToggle,
            BackupCreate,
            BackupRestore,
        };
        Kind kind = Kind::Generic;
        Agent::ToolCall call;
        AgentToolContext context;
        QJsonObject egress;
        QJsonObject data;
        QString displayName;
        QString summary;
        QString preview;
        QString backupHint;
        // SettingsWrite
        QString key;
        QJsonValue value;
        QJsonValue oldValue;
        // ModToggle
        QString fileName;
        bool enable = true;
        // BackupCreate / BackupRestore
        AgentHostBackupPlan plan;
        QString reason;
        AgentHostBackupRef backup;
    };

    AgentToolResult handleInstanceList(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleInstanceContext(const Agent::ToolCall &call,
                                          const AgentToolContext &context);
    AgentToolResult handleStartupDiagnosis(const Agent::ToolCall &call,
                                           const AgentToolContext &context);
    AgentToolResult handleCrashDiagnosis(const Agent::ToolCall &call,
                                         const AgentToolContext &context);
    AgentToolResult handleLogsTail(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleCrashList(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleIntegrity(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleModsList(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleModsSetEnabled(const Agent::ToolCall &call,
                                         const AgentToolContext &context);
    AgentToolResult handleSettingsGet(const Agent::ToolCall &call,
                                      const AgentToolContext &context);
    AgentToolResult handleSettingsSet(const Agent::ToolCall &call,
                                      const AgentToolContext &context);
    AgentToolResult handleJavaList(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleMemorySuggest(const Agent::ToolCall &call,
                                        const AgentToolContext &context);
    AgentToolResult handleBackupList(const Agent::ToolCall &call, const AgentToolContext &context);
    AgentToolResult handleBackupCreate(const Agent::ToolCall &call,
                                       const AgentToolContext &context);
    AgentToolResult handleBackupRestore(const Agent::ToolCall &call,
                                        const AgentToolContext &context);
    AgentToolResult handleUpgradeInspect(const Agent::ToolCall &call,
                                         const AgentToolContext &context);
    AgentToolResult handleOpenPage(const Agent::ToolCall &call, const AgentToolContext &context);

    /// 只读类的统一出口：D11 auto → 直接返回；confirm → 审批卡；block → 拒绝。
    AgentToolResult deliverRead(const AgentToolContext &context, const Agent::ToolCall &call,
                                AgentEgressPolicy::Kind kind, const QString &relative,
                                const QString &content, const QString &displayName,
                                const QString &summary, const QString &preview,
                                const QJsonObject &data);
    AgentToolResult autoResult(const QJsonObject &data, const QString &summary,
                               const QJsonObject &egress, bool notice);
    AgentToolResult approvalRequired(const Pending &pending, const QString &displayName,
                                     const QString &summary, const QString &preview,
                                     const QString &diff, const QString &backupHint,
                                     const QJsonObject &egress);
    QJsonObject egressJson(const AgentEgressPolicy::Result &result,
                           const AgentEgressPolicy::Kind kind) const;
    bool autoNotice(const AgentEgressPolicy::Result &result) const;
    AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message) const;

    AgentLauncherHost *m_host = nullptr;
    ContextProvider m_provider;
    QHash<QString, Pending> m_pending;
    bool m_strictEgress = false;
};
