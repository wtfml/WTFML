/**
 * @file AgentModCompat.hpp
 * @brief 模组兼容性分析（docs/dev/AGENT_AUTONOMY.md S5.4）：本地声明 + 已装依赖图，可选平台结果。
 *
 * 纯函数：不读盘、不联网。调用方把每个 JAR 的声明、大小、SHA-1 和实例的游戏版本/加载器递进来，
 * 这里只按声明判 `loader_mismatch` / `incompatible_version` / `missing_dependency` /
 * `duplicate_mod` / `corrupt_file` / `unknown`，并说明判据来自哪一条声明。
 *
 * 责任边界：声明是包作者的声明，不是运行时保证；范围读不懂时记 `unknown`，不当成"不兼容"。
 */

#pragma once

#include "../operations/ModDeclarations.hpp"

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentModCompat {

struct InstalledFile {
    QString fileName;   ///< `mods/` 下的文件名（含 `.jar.disabled`）
    bool enabled = true;
    qint64 bytes = 0;
    QString sha1;               ///< 小写；空 = 没算出来
    QString path;               ///< 本机路径，只在本地用
    ModDeclarations::Mod declaration;
    QString readProblem;        ///< 读不到条目表 / zip 坏了 / 文件为空
};

struct Target {
    QString gameVersion;
    QString loader;         ///< fabric / forge / neoforge / quilt；空 = 实例没有加载器
    QString loaderVersion;
    int javaMajor = 0;      ///< 0 = 不知道
};

struct PlatformEntry {
    QString sha1;
    QString projectId;
    QString versionId;
    QString versionNumber;
    QStringList gameVersions;
    QStringList loaders;
    QStringList requiredProjectIds;
    QString problem;        ///< 平台没认出这个文件
};

struct Issue {
    QString type;       ///< missing_dependency / incompatible_version / loader_mismatch / duplicate_mod / corrupt_file / unknown
    QString severity;   ///< problem / warning / info
    QString modId;
    QString fileName;
    QString detail;
    QStringList evidence;   ///< 判据字段（给人看，不含路径）
};

struct Analysis {
    QList<Issue> issues;
    int scanned = 0;
    int unknownCount = 0;
    bool truncated = false;
    QStringList notes;
};

/// 本地分析（只用盘上的声明）。
Analysis analyzeLocal(const QList<InstalledFile> &files, const Target &target);

/// 合并平台结果。@p platformComplete 为 false（平台查询失败/被拒绝）时**不**逐文件报
/// "平台没有记录" —— 那会把"没问成"说成"平台不认识"。
void applyPlatform(Analysis *analysis, const QList<InstalledFile> &files,
                   const QList<PlatformEntry> &platform, const Target &target,
                   bool platformComplete);

/// 声明范围与目标值的比较（`Yes` / `No` / `Unknown`）。公开出来给工具复用。
QString matchText(const ModDeclarations::Declaration &declaration, const QJsonValue &rule,
                  const QString &target);

/// 文件里能用于依赖图的 mod id（空 = 读不到）。
QString modIdOf(const InstalledFile &file);

QStringList issueTypes();

}  // namespace AgentModCompat
