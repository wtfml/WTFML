/**
 */

#include "diagnose/StartupDiagnosis.hpp"
#include <QCoreApplication>

#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace StartupDiagnosis {

namespace {

constexpr qint64 kLogTailBytes = 512 * 1024;
constexpr qint64 kCrashHeadBytes = 64 * 1024;
constexpr int kDisabledNameLimit = 5;

QString readHead(const QString &path, qint64 maxBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(file.read(maxBytes));
}

QString readTail(const QString &path, qint64 maxBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    const qint64 size = file.size();
    if (size > maxBytes) file.seek(size - maxBytes);
    return QString::fromUtf8(file.readAll());
}

QString firstMeaningfulLine(const QString &text);

QString crashLine(const QString &text, const QString &prefix) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(prefix)) return line.trimmed();
    }
    // 兜底那一步（上面注释说的）：没有那两行的时候，第一句非空行就是这份报告在说什么。
    // 少了它，一份形状不太一样的报告会落成一句空证据（"崩溃了，原因不详"）。
    return firstMeaningfulLine(text);
}

QString firstMeaningfulLine(const QString &text) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith(QStringLiteral("----"))) return trimmed;
    }
    return QString();
}

int severityRank(Severity severity) {
    switch (severity) {
    case Severity::Problem: return 0;
    case Severity::Warning: return 1;
    case Severity::Info: return 2;
    case Severity::Ok: return 3;
    }
    return 4;
}

void appendFinding(Report *report, Severity severity, const QString &title,
                   const QString &detail = QString(), const QString &hint = QString()) {
    Finding finding;
    finding.severity = severity;
    finding.title = title;
    finding.detail = detail;
    finding.hint = hint;
    report->findings.append(finding);
    if (severity == Severity::Problem) ++report->problems;
    if (severity == Severity::Warning) ++report->warnings;
}

QString javaExecutableOf(const QString &path) {
    if (path.isEmpty()) return QString();
    const QFileInfo info(path);
    if (info.isDir()) {
        const QString inside = path + QStringLiteral("/bin/java.exe");
        return QFileInfo::exists(inside) ? inside : QString();
    }
    return path;
}

struct ModScan {
    QStringList enabled;
    QStringList disabled;
};

ModScan scanMods(const QString &modsDir) {
    ModScan scan;
    if (modsDir.isEmpty()) return scan;
    const QDir dir(modsDir);
    if (!dir.exists()) return scan;
    scan.enabled = dir.entryList({QStringLiteral("*.jar")}, QDir::Files, QDir::Name);
    scan.disabled = dir.entryList({QStringLiteral("*.jar.disabled")}, QDir::Files, QDir::Name);
    return scan;
}

struct CrashScan {
    QString path;
    QString name;
    QDateTime modified;
    QString text;
};

CrashScan newestCrash(const QString &crashDir) {
    CrashScan scan;
    if (crashDir.isEmpty()) return scan;
    const QDir dir(crashDir);
    if (!dir.exists()) return scan;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
    if (files.isEmpty()) return scan;
    const QFileInfo &newest = files.first();
    scan.path = newest.absoluteFilePath();
    scan.name = newest.fileName();
    scan.modified = newest.lastModified();
    scan.text = readHead(scan.path, kCrashHeadBytes);
    return scan;
}

struct LogScan {
    bool exists = false;
    bool hasExitCode = false;
    int exitCode = 0;
    QString lastGameError;
    bool hasOutOfMemory = false;
    bool hasUnsupportedClassVersion = false;
};

LogScan scanLog(const QString &logPath) {
    LogScan scan;
    if (logPath.isEmpty() || !QFileInfo::exists(logPath)) return scan;
    scan.exists = true;
    const QString tail = readTail(logPath, kLogTailBytes);
    // `qDebug() << "Exit code:" << exitCode` 写出来就是这一行（前面可能带时间戳）。
    static const QRegularExpression exitPattern(QStringLiteral("Exit code:\\s*(-?\\d+)"));
    QRegularExpressionMatchIterator matches = exitPattern.globalMatch(tail);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        bool ok = false;
        const int code = match.captured(1).toInt(&ok);
        if (ok) {
            scan.hasExitCode = true;
            scan.exitCode = code;
        }
    }
    const QStringList lines = tail.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.contains(QStringLiteral("[Game Error]"))) scan.lastGameError = line.trimmed();
    }
    scan.hasOutOfMemory = tail.contains(QStringLiteral("OutOfMemoryError"));
    scan.hasUnsupportedClassVersion =
        tail.contains(QStringLiteral("UnsupportedClassVersionError"));
    return scan;
}

}  // namespace

QString severityName(Severity severity) {
    switch (severity) {
    case Severity::Problem: return QCoreApplication::translate("StartupDiagnosis", "问题");
    case Severity::Warning: return QCoreApplication::translate("StartupDiagnosis", "提醒");
    case Severity::Info: return QCoreApplication::translate("StartupDiagnosis", "信息");
    case Severity::Ok: return QCoreApplication::translate("StartupDiagnosis", "正常");
    }
    return QString();
}

QString Report::headline() const {
    if (problems > 0 || warnings > 0) {
        QStringList parts;
        if (problems > 0) parts << QCoreApplication::translate("StartupDiagnosis", "%1 个问题").arg(problems);
        if (warnings > 0) parts << QCoreApplication::translate("StartupDiagnosis", "%1 条提醒").arg(warnings);
        return QCoreApplication::translate("StartupDiagnosis", "启动诊断：发现 %1").arg(parts.join(QCoreApplication::translate("StartupDiagnosis", "、")));
    }
    return QCoreApplication::translate("StartupDiagnosis", "启动诊断：未发现明显问题");
}

QString Report::toText() const {
    QStringList lines;
    lines << headline();
    if (!instanceLine.isEmpty()) lines << instanceLine;
    for (const Finding &finding : findings) {
        lines << QString();
        lines << QCoreApplication::translate("StartupDiagnosis", "【%1】%2").arg(severityName(finding.severity), finding.title);
        if (!finding.detail.isEmpty()) {
            const QStringList detailLines = finding.detail.split(QLatin1Char('\n'));
            for (const QString &line : detailLines) {
                lines << QStringLiteral("  %1").arg(line);
            }
        }
        if (!finding.hint.isEmpty()) lines << QStringLiteral("  → %1").arg(finding.hint);
    }
    return lines.join(QLatin1Char('\n'));
}

Report analyze(const Inputs &inputs) {
    Report report;
    QStringList where;
    if (!inputs.instanceName.isEmpty()) where << inputs.instanceName;
    QString version = inputs.gameVersion;
    if (version.isEmpty()) version = inputs.versionId;
    if (!version.isEmpty()) {
        if (!inputs.loaderName.isEmpty()) {
            version += inputs.loaderVersion.isEmpty()
                           ? QStringLiteral(" / %1").arg(inputs.loaderName)
                           : QStringLiteral(" / %1 %2").arg(inputs.loaderName, inputs.loaderVersion);
        }
        where << version;
    }
    if (!where.isEmpty()) {
        report.instanceLine = where.size() >= 2
                                  ? QCoreApplication::translate("StartupDiagnosis", "实例：%1（%2）").arg(where.at(0), where.at(1))
                                  : QCoreApplication::translate("StartupDiagnosis", "实例：%1").arg(where.at(0));
    }

    const ModScan mods = scanMods(inputs.modsDir);

    // ---------- 1. 版本与加载器 ----------
    if (inputs.versionId.isEmpty()) {
        appendFinding(&report, Severity::Problem,
                      QCoreApplication::translate("StartupDiagnosis", "此实例未指向任何游戏版本"),
                      QCoreApplication::translate("StartupDiagnosis", "实例目录中无法读取可启动的版本信息。"),
                      QCoreApplication::translate("StartupDiagnosis", "请在实例设置中重新确认版本，或重新安装此实例。"));
    } else if (inputs.gameVersion.isEmpty()) {
        appendFinding(&report, Severity::Warning,
                      QCoreApplication::translate("StartupDiagnosis", "无法识别实例的实际游戏版本"),
                      QCoreApplication::translate("StartupDiagnosis", "版本记录里没有 realVersion，界面上只能显示目录名。"),
                      QCoreApplication::translate("StartupDiagnosis", "请在「完整性检查」中检查版本 JSON 是否完整。"));
    }
    if (inputs.loaderName.isEmpty()) {
        if (inputs.versionId.isEmpty()) {
            // 上面那条已经说了，不重复。
        } else if (mods.enabled.size() > 0) {
            appendFinding(
                &report, Severity::Problem,
                QCoreApplication::translate("StartupDiagnosis", "未安装模组加载器，但启用了 %1 个模组").arg(mods.enabled.size()),
                QCoreApplication::translate("StartupDiagnosis", "mods/ 中有 %1 个 .jar，但此实例为原版（未安装 Fabric / Forge / " "NeoForge / Quilt），游戏不会加载这些文件。")
                    .arg(mods.enabled.size()),
                QCoreApplication::translate("StartupDiagnosis", "请安装模组加载器，或移除 mods/ 中的 jar 文件。"));
        } else {
            appendFinding(&report, Severity::Ok, QCoreApplication::translate("StartupDiagnosis", "原版实例（未安装模组加载器）"));
        }
    } else {
        appendFinding(&report, Severity::Ok,
                      QCoreApplication::translate("StartupDiagnosis", "加载器：%1%2").arg(
                          inputs.loaderName,
                          inputs.loaderVersion.isEmpty()
                              ? QString()
                              : QStringLiteral(" %1").arg(inputs.loaderVersion)));
    }

    // ---------- 2. Java ----------
    if (inputs.configuredJavaPath.isEmpty()) {
        if (inputs.autoSelectJava) {
            appendFinding(
                &report, Severity::Info, QCoreApplication::translate("StartupDiagnosis", "未预先指定 Java，启动时自动选择"),
                inputs.requiredJavaMajor > 0
                    ? QCoreApplication::translate("StartupDiagnosis", "此版本需要 Java %1。").arg(inputs.requiredJavaMajor)
                    : QCoreApplication::translate("StartupDiagnosis", "版本 json 未指定 Java 版本，启动时按游戏版本推断。"),
                QCoreApplication::translate("StartupDiagnosis", "本机无匹配 Java 时，首次启动将先下载运行时。"));
        } else {
            appendFinding(&report, Severity::Problem,
                          QCoreApplication::translate("StartupDiagnosis", "Java 已设为「手动指定」，但路径为空"),
                          QCoreApplication::translate("StartupDiagnosis", "手动模式下启动器不会自动选择 Java。"),
                          QCoreApplication::translate("StartupDiagnosis", "请在「启动设置」中指定 java.exe，或切换回「自动」。"));
        }
    } else {
        const QString executable = javaExecutableOf(inputs.configuredJavaPath);
        if (executable.isEmpty() || !QFileInfo::exists(executable)) {
            appendFinding(&report, Severity::Problem,
                          QCoreApplication::translate("StartupDiagnosis", "指定的 Java 不可用"),
                          QCoreApplication::translate("StartupDiagnosis", "路径：%1").arg(inputs.configuredJavaPath),
                          QCoreApplication::translate("StartupDiagnosis", "请将路径改为存在的 java.exe（或 JDK/JRE 目录），" "或切换回自动。"));
        } else {
            appendFinding(
                &report, Severity::Ok, QCoreApplication::translate("StartupDiagnosis", "已指定 Java"),
                QCoreApplication::translate("StartupDiagnosis", "路径：%1").arg(executable),
                inputs.requiredJavaMajor > 0
                    ? QCoreApplication::translate("StartupDiagnosis", "此版本需要 Java %1；诊断不启动 JVM，具体主版本以启动日志为准。")
                          .arg(inputs.requiredJavaMajor)
                    : QString());
        }
    }

    // ---------- 3. 内存 ----------
    {
        QStringList memoryParts;
        if (inputs.xmxMb > 0) memoryParts << QStringLiteral("Xmx %1 MB").arg(inputs.xmxMb);
        if (inputs.xmsMb > 0) memoryParts << QStringLiteral("Xms %1 MB").arg(inputs.xmsMb);
        if (inputs.autoMemory) memoryParts << QCoreApplication::translate("StartupDiagnosis", "自动");
        if (inputs.systemTotalMb > 0) {
            memoryParts << QCoreApplication::translate("StartupDiagnosis", "设备总内存 %1 MB").arg(inputs.systemTotalMb);
        }
        const QString memoryDetail = memoryParts.join(QStringLiteral(" / "));

        if (inputs.xmxMb > 0 && inputs.systemTotalMb > 0
            && inputs.xmxMb > inputs.systemTotalMb) {
            appendFinding(
                &report, Severity::Problem,
                QCoreApplication::translate("StartupDiagnosis", "分配给游戏的内存超过设备总内存"),
                memoryDetail,
                QCoreApplication::translate("StartupDiagnosis", "请将 Xmx 调低至物理内存容量以内，并为系统保留内存。"));
        } else if (inputs.xmxMb > 0 && inputs.systemTotalMb > 0
                   && inputs.xmxMb > inputs.systemTotalMb * 4 / 5) {
            appendFinding(&report, Severity::Warning,
                          QCoreApplication::translate("StartupDiagnosis", "分配给游戏的内存过多"),
                          memoryDetail,
                          QCoreApplication::translate("StartupDiagnosis", "请为系统和其他程序保留内存；模组整合包还需加载资源。"));
        } else if (inputs.xmxMb > 0) {
            appendFinding(&report, Severity::Info, QCoreApplication::translate("StartupDiagnosis", "内存分配"), memoryDetail);
        }
        if (inputs.xmxMb > 0 && inputs.xmxMb < 512) {
            appendFinding(&report, Severity::Warning,
                          QCoreApplication::translate("StartupDiagnosis", "分配给游戏的内存过小（%1 MB）").arg(inputs.xmxMb),
                          memoryDetail,
                          QCoreApplication::translate("StartupDiagnosis", "模组整合包通常需要更多内存。原版 1.20 及以上建议至少分配 1024 MB。"));
        }
        if (inputs.systemTotalMb <= 0) {
            appendFinding(&report, Severity::Info,
                          QCoreApplication::translate("StartupDiagnosis", "无法读取设备物理内存"),
                          QCoreApplication::translate("StartupDiagnosis", "仅获得配置的内存值，无法与设备容量比较。"));
        }
    }

    // ---------- 4. mods/ ----------
    if (!inputs.modsDir.isEmpty() && !QDir(inputs.modsDir).exists()) {
        appendFinding(&report, Severity::Info,
                      QCoreApplication::translate("StartupDiagnosis", "此实例无 mods 目录"),
                      QCoreApplication::translate("StartupDiagnosis", "路径：%1").arg(inputs.modsDir),
                      QCoreApplication::translate("StartupDiagnosis", "首次添加模组时自动创建该目录。"));
    } else if (mods.enabled.size() > 0 && !inputs.loaderName.isEmpty()) {
        appendFinding(&report, Severity::Ok,
                      QCoreApplication::translate("StartupDiagnosis", "已启用 %1 个模组").arg(mods.enabled.size()),
                      inputs.modsDir);
    } else if (!inputs.loaderName.isEmpty() && mods.enabled.isEmpty()) {
        appendFinding(&report, Severity::Info, QCoreApplication::translate("StartupDiagnosis", "mods/ 中无已启用的模组"),
                      inputs.modsDir.isEmpty() ? QString() : inputs.modsDir);
    }
    if (mods.disabled.size() > 0) {
        QStringList listed = mods.disabled.mid(0, kDisabledNameLimit);
        if (mods.disabled.size() > kDisabledNameLimit) {
            listed << QCoreApplication::translate("StartupDiagnosis", "…还有 %1 个").arg(mods.disabled.size() - kDisabledNameLimit);
        }
        appendFinding(&report, Severity::Info,
                      QCoreApplication::translate("StartupDiagnosis", "有 %1 个模组处于禁用状态").arg(mods.disabled.size()),
                      listed.join(QCoreApplication::translate("StartupDiagnosis", "、")),
                      QCoreApplication::translate("StartupDiagnosis", "这些模组以 `*.jar.disabled` 保存，游戏会跳过它们。"));
    }

    bool javaVersionProblemSeen = false;
    bool outOfMemorySeen = false;
    {
        const CrashScan crash = newestCrash(inputs.crashDir);
        if (crash.path.isEmpty()) {
            appendFinding(&report, Severity::Info, QCoreApplication::translate("StartupDiagnosis", "暂无崩溃报告"));
        } else {
            const QString description = crashLine(crash.text, QStringLiteral("Description:"));
            const QString time = crashLine(crash.text, QStringLiteral("Time:"));
            QStringList detail;
            detail << QCoreApplication::translate("StartupDiagnosis", "文件：%1").arg(crash.name);
            if (!time.isEmpty()) detail << time;
            if (!description.isEmpty()) detail << description;
            const QString where = detail.join(QLatin1Char('\n'));
            const QString hint = QCoreApplication::translate("StartupDiagnosis", "完整信息见「崩溃报告」页。");

            if (crash.text.contains(QStringLiteral("java.lang.OutOfMemoryError"))) {
                outOfMemorySeen = true;
                appendFinding(&report, Severity::Warning,
                              QCoreApplication::translate("StartupDiagnosis", "最近一份崩溃报告是内存不足（OutOfMemoryError）"),
                              where,
                              QCoreApplication::translate("StartupDiagnosis", "请调大 Xmx（确认设备有空闲内存），" "或减少同时加载的模组。"));
            } else if (crash.text.contains(QStringLiteral("UnsupportedClassVersionError"))) {
                javaVersionProblemSeen = true;
                appendFinding(
                    &report, Severity::Problem,
                    QCoreApplication::translate("StartupDiagnosis", "最近一份崩溃报告是 Java 版本不对（UnsupportedClassVersionError）"),
                    where,
                    QCoreApplication::translate("StartupDiagnosis", "此崩溃表明有内容使用比当前 Java 更新的版本编译。" "请更换更新的 Java 后重试。"));
            } else if (crash.text.contains(QStringLiteral("NoClassDefFoundError"))
                       || crash.text.contains(QStringLiteral("ClassNotFoundException"))) {
                appendFinding(&report, Severity::Warning,
                              QCoreApplication::translate("StartupDiagnosis", "最近一份崩溃报告里是缺类异常"),
                              where,
                              QCoreApplication::translate("StartupDiagnosis", "请检查日志中对应类所属的模组，确认依赖已安装且与当前游戏版本兼容。"));
            } else if (crash.text.contains(QStringLiteral("Mixin"))
                       && (crash.text.contains(QStringLiteral("InjectionError"))
                           || crash.text.contains(QStringLiteral("Apply"))
                           || crash.text.contains(QStringLiteral("apply")))) {
                appendFinding(&report, Severity::Warning,
                              QCoreApplication::translate("StartupDiagnosis", "最近一份崩溃报告发生在模组注入阶段（Mixin）"),
                              where,
                              QCoreApplication::translate("StartupDiagnosis", "报告通常会指明一个模组。请先单独移除该模组再启动，" "以确认原因。"));
            } else {
                appendFinding(&report, Severity::Info,
                              QCoreApplication::translate("StartupDiagnosis", "最近一份崩溃报告：%1").arg(crash.name),
                              where, hint);
            }
        }
    }

    // ---------- 6. 上一次启动的日志 ----------
    {
        const LogScan log = scanLog(inputs.logPath);
        if (!log.exists) {
            appendFinding(&report, Severity::Info,
                          QCoreApplication::translate("StartupDiagnosis", "暂无启动日志"),
                          QCoreApplication::translate("StartupDiagnosis", "此数据目录下没有使用该启动器运行游戏的记录。"));
        } else if (log.hasUnsupportedClassVersion && !javaVersionProblemSeen) {
            javaVersionProblemSeen = true;
            appendFinding(
                &report, Severity::Problem,
                QCoreApplication::translate("StartupDiagnosis", "启动日志里出现 UnsupportedClassVersionError（Java 太旧）"),
                log.lastGameError,
                QCoreApplication::translate("StartupDiagnosis", "换一个更新的 Java；具体要哪个版本查版本 json 或模组说明。"));
        }
        if (log.exists) {
            if (log.hasExitCode && log.exitCode != 0) {
                QString detail = log.lastGameError;
                if (detail.isEmpty()) detail = QCoreApplication::translate("StartupDiagnosis", "日志末尾未找到 [Game Error] 标记。");
                appendFinding(
                    &report, Severity::Warning,
                    QCoreApplication::translate("StartupDiagnosis", "上一次游戏以退出码 %1 结束").arg(log.exitCode), detail,
                    QCoreApplication::translate("StartupDiagnosis", "「日志」页提供完整输出。若有同一时间的崩溃报告，可在「崩溃报告」页查看。"));
            } else if (log.hasExitCode) {
                appendFinding(&report, Severity::Ok,
                              QCoreApplication::translate("StartupDiagnosis", "上一次游戏正常退出（退出码 0）"),
                              log.lastGameError.isEmpty() ? QString() : log.lastGameError);
            } else if (!log.lastGameError.isEmpty()) {
                appendFinding(&report, Severity::Warning,
                              QCoreApplication::translate("StartupDiagnosis", "启动日志里有错误，但没有看到退出码"),
                              log.lastGameError,
                              QCoreApplication::translate("StartupDiagnosis", "尚未获取完整的启动结果。可在「日志」页查看最新记录。"));
            } else {
                appendFinding(&report, Severity::Info,
                              QCoreApplication::translate("StartupDiagnosis", "启动日志里没有看到游戏退出记录"),
                              QCoreApplication::translate("StartupDiagnosis", "当前会话没有游戏启动记录。"));
            }
        }
        if (log.hasOutOfMemory && !outOfMemorySeen) {
            appendFinding(&report, Severity::Warning,
                          QCoreApplication::translate("StartupDiagnosis", "启动日志里出现 OutOfMemoryError"),
                          QCoreApplication::translate("StartupDiagnosis", "游戏运行期间内存不足（可能未生成崩溃报告）。"),
                          QCoreApplication::translate("StartupDiagnosis", "请调大 Xmx，或减少部分模组后重试。"));
        }
    }

    std::stable_sort(report.findings.begin(), report.findings.end(),
                     [](const Finding &a, const Finding &b) {
                         return severityRank(a.severity) < severityRank(b.severity);
                     });
    return report;
}

}  // namespace StartupDiagnosis
