/**
 * @file AgentSkill.hpp
 * @brief 内置技能（模式）注册表：自由对话 + Stage 2 的四条维护工作流。
 *
 * 技能编译进程序、版本随程序走，不从远程下载。技能**只能缩小**工具集合，改不了审批策略
 * 和外发策略（docs/features/AGENT.md §11.1）。工作流状态机在 `AgentWorkflow`，这里只放
 * 定义：白名单、是否只读、是否必须先提交计划、最大步数、完成判据。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct AgentSkill {
    QString id;
    QString name;
    QString description;
    QString systemPrompt;
    /// 提示词资源 id（`assets/agent/prompts/<id>.md`），和技能 id 分开是为了
    /// `crash-rescue` → `crash_rescue.md` 这类命名差异。
    QString promptId;
    /// 加载时的 SHA-256（十六进制）；会话与报告可据此判断用的是哪一版提示词。
    QString promptHash;
    /// 人工版本号，改提示词时递增；hash 是内容算出来的，两者一起用。
    int promptVersion = 1;
    /// 非空 = 资源缺失/为空/超限/编码错误，技能不可用；调用方不要下发空提示。
    QString promptProblem;
    /// 工具内部名白名单。空 = 不限制（只有"没有模式"的测试夹具会这样）；
    /// 生产里每个模式都有显式清单（S5.6）。末尾带 `*` 的条目按前缀放行（`mcp__*`）。
    QStringList allowedTools;
    /// 完成这条工作流至少要碰过的工具（缺了 `workflow.finish` 就拒绝）。
    QStringList requiredTools;
    bool readOnly = true;
    int maxSteps = 20;
    /// 必须先 `workflow.submitPlan` 并通过计划审批，才能调写类工具。
    bool requiresPlan = false;
    QString completionCriteria;
    int version = 1;

    bool promptAvailable() const { return promptProblem.isEmpty() && !systemPrompt.isEmpty(); }
    bool restrictsTools() const { return !allowedTools.isEmpty(); }
    bool allows(const QString &toolName) const {
        if (allowedTools.isEmpty()) return true;
        if (allowedTools.contains(toolName)) return true;
        for (const QString &entry : allowedTools) {
            if (entry.endsWith(QLatin1Char('*')) && !entry.isEmpty() &&
                toolName.startsWith(entry.left(entry.size() - 1))) {
                return true;
            }
        }
        return false;
    }
};

class AgentSkillRegistry {
public:
    /// 编译进程序的技能：`general` + 体检 / 崩溃急救 / 版本升级 / 模组维护。
    static QList<AgentSkill> builtins();
    static bool contains(const QString &id);
    static AgentSkill find(const QString &id);
    /// 界面上按这个顺序摆模式卡片（`general` 在最前）。
    static QStringList orderedIds();
};
