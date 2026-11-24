/**
 * @file AgentLauncherHost.hpp
 * @brief 启动器能力的接入口：智能体工具不自己重写业务判据，只调这一层。
 *
 * 实现放在 `src/controllers/`（那里才认识 VersionManager / MainWindow），核心层只认这张
 * 纯虚表，于是 `verify_agent` 能用假宿主逐格验工具行为。所有实现都不接受模型传入的实例
 * id —— 实例由会话锁定提供（D8）。
 *
 * 约束：实现必须同步返回、不得弹嵌套事件循环。重扫（完整性体检）要给时间上限并在超过时
 * 回部分结果，别在 UI 线程上无界读盘。
 */

#pragma once

#include "AgentBackupIndex.hpp"
#include "AgentDiagnosticProjection.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

struct AgentHostModEntry {
    QString fileName;   ///< 相对 `mods/` 的文件名（不含路径）
    bool enabled = true;
    qint64 bytes = 0;
};

struct AgentHostCrashReport {
    QString fileName;      ///< `crash-reports/` 下的文件名
    /// 本机绝对路径，**只在本地读盘用**；不要进模型输出（给出的是 fileName 与 label）。
    QString path;
    QString label;         ///< 来自哪个目录（`versions/<id>/crash-reports` 等）
    QDateTime modifiedAt;
    qint64 bytes = 0;
    QString headText;      ///< 开头一段（已脱敏、已去本机路径），可为空
};

/// 一份可被按行索引的日志（`logs.search` / `logs.readRange` 用它）。
struct AgentHostLogSource {
    QString key;      ///< instance / shared / launcher
    QString label;    ///< 相对标签，不含盘符
    QString path;     ///< 本机绝对路径，只在本地流式读盘时用
    qint64 bytes = 0;
    QDateTime modifiedAt;
};

struct AgentHostBackupRef {
    QString backupId;
    QString owner;          ///< agent / launcher / user / shared
    QString source;
    QString label;
    QString sourceSessionId;
    QDateTime createdAt;
    qint64 bytes = 0;
    bool restorable = true;
};

/// 「备份前先算一遍」：审批卡要显示范围、大小、保存位置（§9.4 `backup.create`）。
struct AgentHostBackupPlan {
    QString scope;
    QStringList relativePaths;
    qint64 bytes = 0;
    QString saveLocation;
};

struct AgentHostSettings {
    QJsonObject values;        ///< 白名单键的当前值（不含路径类字段）
    QStringList writableKeys;  ///< 允许 `settings.set` 写的键
};

class AgentLauncherHost {
public:
    virtual ~AgentLauncherHost() = default;

    // ---------- 实例事实 ----------
    virtual bool instanceList(QList<AgentDiagnosticProjection::InstanceFacts> *items,
                              QString *problem) = 0;
    virtual bool instanceFacts(const QString &instanceId,
                               AgentDiagnosticProjection::InstanceFacts *facts,
                               QString *problem) = 0;
    virtual bool startupDiagnosis(const QString &instanceId, StartupDiagnosis::Report *report,
                                  QString *problem) = 0;

    // ---------- 崩溃与日志 ----------
    virtual bool crashFacts(const QString &instanceId, GameExitDiagnosis::Facts *facts,
                            QStringList *hints, QString *problem) = 0;
    /// `logs/latest.log` 末尾；`relativeSource` 回一个不含绝对路径的来源标签。
    virtual bool logTail(const QString &instanceId, int lines, QString *text,
                         QString *relativeSource, QString *problem) = 0;
    /// 可搜索/可按行读取的日志候选（实例 → 数据根 → 启动器），带本机路径供本地索引。
    virtual bool logSources(const QString &instanceId, QList<AgentHostLogSource> *items,
                            QString *problem) = 0;
    virtual bool crashReports(const QString &instanceId, QList<AgentHostCrashReport> *items,
                              QString *problem) = 0;
    /// `mods/` 目录的本机路径（mixin 配置归属扫描用）；目录不存在时回 false。
    virtual bool modsDirectory(const QString &instanceId, QString *path, QString *problem) = 0;

    // ---------- 完整性 / 模组 ----------
    virtual bool integrity(const QString &instanceId, VersionIntegrity::Report *report,
                           QString *problem) = 0;
    virtual bool mods(const QString &instanceId, QList<AgentHostModEntry> *items,
                      QString *problem) = 0;
    /// 只改扩展名（`.jar` ↔ `.jar.disabled`），可原路改回；`previous` 回到原文件名。
    virtual bool setModEnabled(const QString &instanceId, const QString &fileName, bool enabled,
                               QString *previous, QString *problem) = 0;

    // ---------- 设置 / Java ----------
    virtual bool settings(AgentHostSettings *settings, QString *problem) = 0;
    virtual bool applySetting(const QString &key, const QJsonValue &value, QJsonObject *applied,
                              QString *problem) = 0;
    virtual bool javaVersions(QList<int> *majors, QString *problem) = 0;

    // ---------- 备份 ----------
    virtual bool backups(QList<AgentHostBackupRef> *items, QString *problem) = 0;
    virtual bool planBackup(const QString &instanceId, const QStringList &scopes,
                            AgentHostBackupPlan *plan, QString *problem) = 0;
    virtual bool createBackup(const QString &sessionId, const QString &instanceId,
                              const AgentHostBackupPlan &plan, QString *backupId,
                              QString *problem) = 0;
    virtual bool restoreBackup(const QString &sessionId, const QString &backupId,
                               QString *preRestoreBackupId, QString *problem) = 0;

    // ---------- 升级 / 页面 ----------
    virtual bool upgradeInspect(const QString &instanceId, QJsonObject *data,
                                QString *problem) = 0;
    /// 只打开已有入口，不执行写操作（§9.4 `launcher.openPage`）。
    virtual bool openPage(const QString &page, const QString &instanceId, QString *problem) = 0;
};
