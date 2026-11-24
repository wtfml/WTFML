/**
 * @file AgentInstancePaths.hpp
 * @brief 崩溃报告与游戏日志的**目录查找规则**（agent 工具读盘前先问这一层）。
 *
 * 为什么单独一层：游戏是用 `--gameDir <数据根>/versions/<实例>` 起的（见 LauncherManager），
 * 所以 `launch.log` / `crash-reports/` 落在**实例目录**里，不是数据根。数据根下那一份只在
 * 别的启动器/旧版本启动过时才有东西。两处都要看，而且实例目录优先 —— 读错了的症状就是
 * 「明明有崩溃报告，智能体说读不到」。
 *
 * 这一层只算路径、只判存不存在，不读内容、不写盘。返回的 `label` 是给模型/卡片看的
 * **相对**标签（不含绝对路径，D11）。
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

namespace AgentInstancePaths {

struct Source {
    QString key;     ///< `instance` / `shared` / `launcher`（日志源）；崩溃报告目录只用前两个
    QString path;    ///< 绝对路径（只在本地用）
    QString label;   ///< 相对标签：`versions/<id>/crash-reports` / `crash-reports`
};

/// 崩溃报告目录：实例目录优先，其次是数据根。只回**存在**的目录。
QList<Source> crashReportDirs(const QString &instanceId);

/// 日志候选（按优先级）：实例 `logs/latest.log` → 数据根 `logs/latest.log` →
/// 数据根 `logs/wtfml.log`。只回**存在**的文件。
QList<Source> logSources(const QString &instanceId);

struct CrashReportFile {
    QString path;
    QString fileName;
    QString label;   ///< 来自哪个目录（`versions/<id>/crash-reports` 等）
    QString directoryKey;  ///< `instance` / `shared`
    QDateTime modifiedAt;
    qint64 bytes = 0;
};

/// 两处目录里的 `crash-*.txt` 合并，同名只留实例目录那一份，按修改时间新到旧，最多 @p limit 条。
QList<CrashReportFile> collectCrashReports(const QString &instanceId, int limit = 20);

}  // namespace AgentInstancePaths
