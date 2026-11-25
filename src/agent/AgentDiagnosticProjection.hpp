/**
 * @file AgentDiagnosticProjection.hpp
 * @brief 诊断结果的字段白名单投影（docs/features/AGENT.md §9.4.1）。
 *
 * `StartupDiagnosis` / `GameExitDiagnosis` / `VersionIntegrity` 的报告是**给人看的**，
 * 里面带绝对路径、Java 路径、原始日志片段。这些不能原样进模型上下文，所以工具执行完
 * 报告之后先过这一层纯函数：只留白名单字段，证据另给一条本地 id。
 *
 * 这里是纯函数：不读盘、不联网、不看时钟。测试按字段逐格断言，别处不要再写第二份过滤。
 */

#pragma once

#include "../diag/GameExitDiagnosis.hpp"
#include "../diagnose/StartupDiagnosis.hpp"
#include "../manage/VersionIntegrity.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace AgentDiagnosticProjection {

/// 锁定实例的元数据白名单。**没有** Java 路径、数据根、账户字段。
struct InstanceFacts {
    QString instanceId;
    QString instanceName;
    QString gameVersion;
    QString loaderName;
    QString loaderVersion;
    int requiredJavaMajor = -1;   ///< -1 = 不知道
    int usedJavaMajor = -1;       ///< -1 = 不知道
    int xmxMb = 0;
    int xmsMb = 0;
    bool autoMemory = true;
    int modCount = 0;
    int disabledModCount = 0;
    bool running = false;

    QJsonObject toJson() const;
};

/// 一条证据。`id` 由本层生成（`E1`、`E2`…），最终报告按它引用。
struct Evidence {
    QString id;
    QString kind;      ///< log | file | diagnostic | network | tool
    QString source;    ///< 不含绝对路径的来源标签（`startup-diagnosis` / `crash-report#1`）
    QString capturedAt;
    QString sha256;
    QString excerpt;   ///< 已脱敏的片段，可为空

    QJsonObject toJson() const;
};

/// 文本里有没有本机路径/账户痕迹（`X:\`、`/Users/`、`file://`、UNC）。证据片段用它兜底。
bool containsLocalPath(const QString &text);

/// 去掉含本机路径的句子，其余保留。空串表示整段都不适合外发。
QString sanitizeEvidence(const QString &text);

/// 把本机绝对路径就地替换成 `<path>`（保留行结构）。日志/崩溃报告结尾用它：整行丢掉会
/// 让证据对不上号，留着原路径又会带出用户名和目录结构。
QString scrubPaths(const QString &text);

/// 路径之外的账户与硬件痕迹，就地换成占位符：
/// - 启动参数里的账户字段（`--accessToken` / `--uuid` / `--username` / `--xuid` 等）→ `<redacted>`；
/// - MAC 地址 → `<mac>`；Windows 用户 SID → `<sid>`；
/// - `HWID=` / `MachineGuid=` / `serial=` 这类硬件标识字段 → `<redacted>`；
/// - @p knownNames 里点名的账户名（`` 整词，短于 4 个字符的不动）→ `<user>`。
QString scrubIdentifiers(const QString &text, const QStringList &knownNames = {});

/// 日志/崩溃报告进模型或 artifact 之前的统一出口：`redact`（凭据）之外的路径 + 账户 + 硬件痕迹。
QString scrubLogText(const QString &text);

/// 本机账户名候选（`USERNAME` / `USER` / 主目录最后一段），只用来做整词替换。
QStringList localAccountNames();

/// 启动诊断 → 允许进模型上下文的 JSON。`report.findings` 的 `detail` 只过 sanitize 后再给。
QJsonObject startup(const StartupDiagnosis::Report &report, const InstanceFacts &facts,
                    QList<Evidence> *evidence);

/// 崩溃/异常退出事实 → 白名单 JSON（不含 Java 路径、原始 stack trace）。
QJsonObject crash(const GameExitDiagnosis::Facts &facts, const QStringList &hints,
                  const InstanceFacts &instance, QList<Evidence> *evidence);

/// 完整性报告 → 只给计数与分类，不给路径。
QJsonObject integrity(const VersionIntegrity::Report &report, const InstanceFacts &facts,
                      QList<Evidence> *evidence);

/// 投影后还剩下什么可说的：空结果不许报成功（§9.4.1）。
bool hasContent(const QJsonObject &projected);

}  // namespace AgentDiagnosticProjection
