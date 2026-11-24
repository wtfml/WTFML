/**
 * @file AgentCrashSnapshot.hpp
 * @brief 崩溃报告/日志的结构化解析与崩溃快照组装（docs/dev/AGENT_AUTONOMY.md S5.3）。
 *
 * 两段职责：
 * 1. `parse()`：把一份崩溃报告、`hs_err_pid` 或游戏日志切成可断言的事实（环境、异常链、
 *    模组清单、mixin 配置名、失败区文本）。纯函数，不读盘。
 * 2. `build()`：把事实 + 命中规则 + mixin 配置归属组装成工具输出的 JSON，并按证据类型算
 *    嫌疑模组的 `role` 与 `confidence`（封顶 0.95；只被提到的模组不超过 0.4）。
 *
 * 约束（和整个 agent 层一致）：不返回绝对路径、不返回原始 stack trace，只给类型名、行数、
 * 计数和脱敏后的摘录。报告头里的 `Suspected Mods` 只算线索，不当根因。
 */

#pragma once

#include "AgentKnowledgeRules.hpp"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentCrashSnapshot {

struct ModEntry {
    QString modId;
    QString name;
    QString version;
    QString fileName;
};

struct Environment {
    QString minecraftVersion;
    QString loader;        ///< forge / neoforge / fabric / quilt；空 = 不认识
    QString loaderVersion;
    int javaMajor = 0;     ///< 0 = 不知道
};

struct Parsed {
    QString kind;          ///< crash-report / native-log / game-log
    /// 喂进来的原文（QString 隐式共享，不拷贝）；规则匹配用它，分段事实用它派生。
    QString rawText;
    Environment environment;
    QString description;   ///< 报告头的 `Description:`
    QString exception;     ///< 顶层异常类型（全限定名）
    QStringList causeChain;///< 顶层 + `Caused by:` 的类型名
    QStringList suspectedMods;  ///< 报告头 `Suspected Mods` 里的 mod id
    QList<ModEntry> modList;
    QStringList mixinMods;     ///< `Mixin apply for mod X failed`
    QStringList mixinConfigs;  ///< 文本里点名的 `xxx.mixins.json`
    QStringList stackClasses;  ///< 失败区栈帧的类名（去重，最多 40）
    QString failureRegion;     ///< 首处异常行起的一小段文本（给“被提到”判定用，已截断）
    int totalLines = 0;
    qint64 totalBytes = 0;
    bool truncated = false;    ///< 输入本身就是截断过的
};

/// @param truncated 输入是否只截了一段（日志尾、报告前 8 MB）。
Parsed parse(const QString &text, bool truncated = false);

struct SnapshotInput {
    Parsed parsed;
    AgentKnowledge::CrashRuleSet rules;
    /// mixin 配置名（归一化后）→ mod id；读不到归属时不要放进来。
    QHash<QString, QString> mixinOwners;
    /// mod id（**小写**）→ 版本（报告里的模组清单；解析不到就不放）
    QHash<QString, QString> modVersions;
    /// 环境里报告缺的部分用会话事实补（例如实例声明的加载器）。
    Environment fallback;
    QString sourceKind;    ///< 传给报告的 `source.kind`
    QString sourceLabel;   ///< 相对标签（不含盘符）
    QString reportId;
    qint64 bytes = 0;
    /// 证据 id 从 E<n+1> 开始（同一工具里前面已经有证据时用）。
    int firstEvidenceIndex = 0;
};

/// 组装后的快照：`json` 直接进工具输出，`evidence` 已带 id。
struct Snapshot {
    QJsonObject json;
    QStringList matchedRuleIds;
};

Snapshot build(const SnapshotInput &input);

}  // namespace AgentCrashSnapshot
