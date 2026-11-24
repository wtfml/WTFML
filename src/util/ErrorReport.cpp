/**
 * @file ErrorReport.cpp
 * @brief 实现错误报告文本的组装。
 * @note 只生成文本，不写文件、不联网。
 */

#include "ErrorReport.hpp"
#include <QCoreApplication>
#include "SecretRedactor.hpp"

#include <QStringList>
#include <QSysInfo>

namespace {

QString field(const QString &name, const QString &value) {
    return name + QCoreApplication::translate("ErrorReport", "：") + value;
}

/// 文件名里不许出现的那些字符（Windows 那一套；换成一个下划线）。
QString sanitizeForFileName(const QString &text) {
    QString out = text;
    static const QString kForbidden = QStringLiteral("\\/:*?\"<>|");
    for (int i = 0; i < out.size(); ++i) {
        if (kForbidden.contains(out.at(i))) {
            out[i] = QLatin1Char('_');
        }
    }
    // 首尾的空白会让这个文件名在资源管理器里看着像"少了一截"，顺手收掉。
    return out.trimmed();
}

/// 时间那一格统一写成这个形状（两种文件名里也是它，**只有一处**）。
QString stampForName(const QDateTime &now) {
    return now.toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

/// 一段文本有多少行（和 `CrashReportController::lineCount()` 同一个口径：末尾那个换行
/// 不算"又多了一行"，末尾半行算一行）。
int lineCountOf(const QString &text) {
    if (text.isEmpty()) {
        return 0;
    }
    const int breaks = text.count(QLatin1Char('\n'));
    return breaks + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);
}

}  // namespace

namespace ErrorReport {

QString systemDescription() {
    // `prettyProductName()` 是"Windows 11 Home China"这种给人看的名字，内核版本号在
    // `kernelVersion()` 里 —— 两个都要：只看前一个不知道是哪一版，只看后一个不知道是哪个
    // 发行版（排查"只有 Win11 出的问题"时两样都用得上）。
    return QStringLiteral("%1 (%2) %3")
        .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
             QSysInfo::currentCpuArchitecture());
}

QString header(const Facts &facts, const QDateTime &now) {
    QStringList lines;
    lines.append(QCoreApplication::translate("ErrorReport", "WTFML 错误报告"));
    lines.append(field(QCoreApplication::translate("ErrorReport", "生成时间"),
                       now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    if (!facts.launcherVersion.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "启动器版本"), facts.launcherVersion));
    }
    if (!facts.systemDescription.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "系统"), facts.systemDescription));
    }
    if (!facts.instanceName.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "实例"), facts.instanceName));
    }
    if (!facts.instanceId.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "实例 id"), facts.instanceId));
    }
    if (!facts.accountName.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "账号"), facts.accountName));
    }
    // 加载器 / Java / 内存这三行只有「游戏异常退出」那一项会填（要求收的就是这几样）。
    // 空着一律不写 —— 那一路手上确实没有这些值，编一个填上去最误导人。
    if (!facts.loaderVersion.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "加载器"), facts.loaderVersion));
    }
    if (!facts.javaVersion.isEmpty()) {
        lines.append(field(QStringLiteral("Java"), facts.javaVersion));
    }
    if (!facts.memoryArgs.isEmpty()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "内存"), facts.memoryArgs));
    }
    if (facts.processId > 0) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "进程 ID"), QString::number(facts.processId)));
    }
    if (facts.startedAt.isValid()) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "启动时间"),
                           facts.startedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    }
    // 退出那一行只在**真的收到过**的时候写。游戏还在跑的时候编一个"退出码：0"出来
    // 是最坏的一种假话：看报告的人会照着它去排除"崩溃"这个方向。
    if (facts.finished) {
        lines.append(field(QCoreApplication::translate("ErrorReport", "退出码"),
                           facts.crashed
                               ? QCoreApplication::translate("ErrorReport", "%1（异常终止）").arg(facts.exitCode)
                               : QString::number(facts.exitCode)));
    } else {
        lines.append(field(QCoreApplication::translate("ErrorReport", "退出码"), QCoreApplication::translate("ErrorReport", "（导出时游戏还在运行）")));
    }
    return lines.join(QLatin1Char('\n'));
}

QString sectionDivider(const QString &title) {
    return QStringLiteral("------------------------------ %1 ------------------------------").arg(title);
}

QString text(const Facts &facts, const QDateTime &now, const QString &errors, const QString &log) {
    QString out = header(facts, now);
    out += QStringLiteral("\n\n");

    // 「可能的原因」排在两段日志**前面**：拿到报告的人第一眼要知道的是"大概是什么事"，
    // 而不是几百行日志。一句都没有时这一节整个不写 —— "暂时无法判断"那种话写上去，
    // 看的人会以为我们判过一轮然后放弃了。
    if (!facts.hints.isEmpty()) {
        out += sectionDivider(QCoreApplication::translate("ErrorReport", "可能原因"));
        out += QLatin1Char('\n');
        for (const QString &hint : facts.hints) {
            out += QStringLiteral("• ") + hint + QLatin1Char('\n');
        }
        out += QLatin1Char('\n');
    }

    const QString errorTitle =
        facts.errorTruncated && !errors.isEmpty()
            ? QCoreApplication::translate("ErrorReport", "错误与致命（末尾 %1 行 / 原始共 %2 行）")
                  .arg(lineCountOf(errors))
                  .arg(facts.errorLines)
            : (facts.errorTruncated
                   // 被挤掉的那些**全是**错误行（再没有别的错误行留在窗里）：不写这一档，
                   // 标题就只能退回「共 0 行」，而下面那句"这一趟没有 ERROR 以上的输出"
                   // 和事实正好相反。
                   ? QCoreApplication::translate("ErrorReport", "错误与致命（原始共 %1 行；正文已经被日志行数上限挤掉）")
                         .arg(facts.errorLines)
                   : QCoreApplication::translate("ErrorReport", "错误与致命（共 %1 行）").arg(facts.errorLines));
    out += sectionDivider(errorTitle);
    out += QLatin1Char('\n');
    if (errors.isEmpty()) {
        // 照样写上那一段（理由见 .hpp）：空着的话看的人分不清"没出错"和"导出漏了"。
        out += facts.errorTruncated
                   ? QCoreApplication::translate("ErrorReport", "（这一段里现在一行都没有：较早的几行已被日志行数上限挤掉，原始共 %1 行。）")
                         .arg(facts.errorLines)
                   : QCoreApplication::translate("ErrorReport", "（这一段里一行都没有：本次没有 ERROR 及以上的输出。日志本身在下面。）");
        out += QLatin1Char('\n');
    } else {
        out += errors;
        // `logTextFrom()` 每行都带换行，只有末尾那半行可能不带 —— 缺了就补一个，
        // 免得两段的分隔线粘在最后一行后面。
        if (!out.endsWith(QLatin1Char('\n'))) {
            out += QLatin1Char('\n');
        }
    }

    out += QLatin1Char('\n');
    // 同上：只留了末尾一截时不能再叫「完整日志」—— 那份文件**不是**完整的。
    const QString logTitle =
        facts.logTruncated && !log.isEmpty()
            ? QCoreApplication::translate("ErrorReport", "日志末尾（最近 %1 行 / 原始共 %2 行；完整日志没有导出）")
                  .arg(lineCountOf(log))
                  .arg(facts.totalLines)
            : QCoreApplication::translate("ErrorReport", "完整日志（共 %1 行）").arg(facts.totalLines);
    out += sectionDivider(logTitle);
    out += QLatin1Char('\n');
    out += log;
    if (!out.endsWith(QLatin1Char('\n'))) {
        out += QLatin1Char('\n');
    }
    return SecretRedactor::redact(out);
}

QString suggestedFileName(const QString &instanceName, const QDateTime &now) {
    const QString name = sanitizeForFileName(instanceName);
    const QString middle = name.isEmpty() ? QString() : name + QLatin1Char('-');
    return QCoreApplication::translate("ErrorReport", "错误报告-%1%2.txt").arg(middle, stampForName(now));
}

QString suggestedLogFileName(const QString &instanceName, const QDateTime &now) {
    const QString name = sanitizeForFileName(instanceName);
    const QString middle = name.isEmpty() ? QString() : name + QLatin1Char('-');
    return QCoreApplication::translate("ErrorReport", "日志-%1%2.log").arg(middle, stampForName(now));
}

}  // namespace ErrorReport
