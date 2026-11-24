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
    /// 工具内部名白名单。空 = 不限制（自由对话沿用 Stage 1B 的全部工具）。
    QStringList allowedTools;
    /// 完成这条工作流至少要碰过的工具（缺了 `workflow.finish` 就拒绝）。
    QStringList requiredTools;
    bool readOnly = true;
    int maxSteps = 20;
    /// 必须先 `workflow.submitPlan` 并通过计划审批，才能调写类工具。
    bool requiresPlan = false;
    QString completionCriteria;
    int version = 1;

    bool restrictsTools() const { return !allowedTools.isEmpty(); }
    bool allows(const QString &toolName) const {
        return allowedTools.isEmpty() || allowedTools.contains(toolName);
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
