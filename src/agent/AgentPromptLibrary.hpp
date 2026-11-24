/**
 * @file AgentPromptLibrary.hpp
 * @brief 技能提示词的唯一来源：从 Qt 资源加载、组合、算哈希、校验大小。
 *
 * 提示词正文在 `assets/agent/prompts/` 下的 md 文件，`AgentSkill.cpp` 只留技能 id、工具白名单和
 * 提示词 id。组合顺序固定为 `common` + 技能 +（崩溃急救/模组维护）`repair_protocol`。
 * 资源缺失、为空、不是 UTF-8 或超过上限时返回 `promptProblem`，调用方把技能标成不可用，
 * 不回退成空提示。
 */

#pragma once

#include <QString>

struct AgentPrompt {
    QString text;
    QString hash;

    bool ok() const { return !text.isEmpty() && !hash.isEmpty(); }
};

class AgentPromptLibrary {
public:
    /// 加载一份提示词；@p problem 非空表示失败。
    static AgentPrompt load(const QString &id, QString *problem = nullptr);

    /// 组合一份技能的完整系统提示；@p includeRepairProtocol 只在崩溃急救/模组维护为 true。
    static AgentPrompt compose(const QString &skillId, bool includeRepairProtocol,
                               QString *problem = nullptr);

    /// `:/agent/prompts/<id>.md`；id 不合法时返回空串。
    static QString resourcePath(const QString &id);

    /// 单份提示词的硬上限（字节；资源被截断/拒读的最后一道闸）。
    static int maxPromptBytes();
    /// 预算（docs/dev/AGENT_AUTONOMY.md S5.6）：`common` ≤4 KB、单个技能 ≤8 KB、
    /// `common + 技能 + repair_protocol` ≤16 KB。超预算的规则应进知识库或工具。
    static int maxCommonPromptBytes();
    static int maxSkillPromptBytes();
    static int maxComposedPromptBytes();

    /// 只允许小写字母、数字、下划线和连字符，防止 id 拼出资源路径之外的名字。
    static bool validId(const QString &id);
};
