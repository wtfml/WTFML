/**
 * @file AgentTool.hpp
 * @brief 工具接口/风险等级/注册表/执行引擎抽象（Stage 1B 文件工具）。
 *
 * 工具结果一律视为不可信数据；参数 JSON Schema 只支持所需子集，未知字段默认拒绝。
 * 写入/外发确认由引擎返回 ApprovalRequired，AgentLoop 暂停等待用户决定。
 */

#pragma once

#include "AgentTypes.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class AgentRisk { ReadOnly, NetworkRead, Write, Destructive, Command, ExternalMcp };

/**
 * 一次工具调用所处的位置：会话、锁定的实例、根目录、当前模型服务。
 * 实例 id 只由控制器填（会话锁定的结果），模型参数里的 instanceId 一律不看。
 */
struct AgentToolContext {
    QString sessionId;
    QString instanceId;
    QString instanceName;
    QString gameRootId;
    QString rootPath;
    QString providerHost;
    bool strictEgress = false;
};

struct AgentToolSpec {
    QString name;
    QString displayName;
    QString description;
    AgentRisk risk = AgentRisk::ExternalMcp;
    QJsonObject schema;
};

struct AgentToolResult {
    enum class Status { Ok, Error, ApprovalRequired, Blocked, Cancelled, Pending };
    Status status = Status::Error;
    QString summary;
    QJsonObject data;
    QJsonObject approval;
    Agent::Error error;

    bool ok() const { return status == Status::Ok; }
};

/// 异步工具的交付回调：`invoke()`/`resolveApproval()` 可以先回 `Status::Pending`，
/// 之后由引擎通过这个回调交最终结果（MCP 的工具调用就是这样）。
using AgentToolSink = std::function<void(const Agent::ToolCall &, const AgentToolResult &)>;

class AgentToolEngine {
public:
    virtual ~AgentToolEngine() = default;
    virtual QJsonArray schemas() const = 0;
    virtual AgentToolResult invoke(const Agent::ToolCall &call) = 0;
    virtual AgentToolResult resolveApproval(const QString &approvalId, bool approved) = 0;
    /// 用户点“本会话允许”：这一次照常执行，同一个来源在本会话里不再逐次确认。
    /// 只有 `AgentApprovalPolicy::allowsSessionApproval()` 的工具会让 UI 给出这个按钮；
    /// 默认实现等同于“允许本次”，不改变其它工具的行为。
    virtual AgentToolResult resolveSessionApproval(const QString &approvalId) {
        return resolveApproval(approvalId, true);
    }
    virtual void cancelPending() = 0;
    /// 文件提交阶段：取消只能登记“提交后停止”，不能强杀。
    virtual bool committing() const { return false; }
    /// 这个引擎认不认这个内部工具名（复合引擎按它分派，不用试错）。
    virtual bool handles(const QString &internalName) const = 0;
    /// 只有会回 `Pending` 的引擎需要接这条线；同步引擎忽略。
    virtual void setCompletionSink(AgentToolSink sink) { Q_UNUSED(sink); }
};

class AgentToolRegistry {
public:
    /// 文件工具（Stage 1B）与全部静态内建工具（含 Stage 2 的启动器/工作流工具）。
    static QList<AgentToolSpec> fileBuiltins();
    static QList<AgentToolSpec> builtins();
    static AgentToolSpec find(const QString &name);
    /// 线上 function name 只允许 `[A-Za-z0-9_-]`：内部名里的点在这里换成下划线。
    static QString wireName(const QString &internal);
    static QString internalName(const QString &wire);
    static QString riskKey(AgentRisk risk);

    /// JSON Schema 子集：type/required/properties/enum/maxLength/maximum/additionalProperties。
    static bool validateArguments(const QJsonObject &schema, const QJsonObject &arguments,
                                  QString *problem);
};
