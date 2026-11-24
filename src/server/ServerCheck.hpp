/**
 * @file ServerCheck.hpp
 * @brief 服务端的「**体检**」那一层：把手上关于一个服务端的
 * 全部事实喂进来，回一张"哪几件事不对劲、各自该怎么办"的单子。
 *
 * 【它是干什么的（以及和「修复」那颗按钮的关系）】
 *
 * 服务端列表里那一行上就有一颗「修复」（= 按我们记的那一档**重装一遍**）。但"重装一遍"是个**能
 * 治很多病、也治不了很多病**的粗活：端口被别的程序占着它治不了，`server.properties` 里
 * `view-distance=abc` 它更治不了（那不是装出来的，是用户手写的）。所以在那颗按钮之前先来
 * 一遍**体检**：把"这一个服务端现在到底哪儿不对"一条一条列出来，每条后面跟着那一条自己的
 * 出路（重装 / 去把值改对 / 去同意 EULA / 开目录看一眼）。
 *
 * 【它为什么是纯函数】
 *
 * 这一层**自己不碰盘、不看时钟、不查端口**：`Facts` 里每一个 bool 都是调用方（`MainWindow`
 * 那一段）现查出来喂进来的。这么切的理由和 `ServerModel` / `ServerModel::Properties` 一样：
 * "哪几条该报、按什么次序报、每一条用哪句话"这件事是**规格**，规格要能被逐条念出来。真去
 * 读盘（`ServerRegistry`）、真去占端口（`ServerPortCheck`）都在外面。
 *
 * 【报的次序】
 *
 * 从**拦着它起不来**的往下排（没装好 → 记的那一份说不清 → EULA 没同意 → 值不合法 → 端口
 * 被占），然后是提醒（重复的键、备份一份都没有），最后是说明（世界还没生成、文件里有别的
 * 键）。界面上照着这个次序摆，颜色由 `Severity` 决定 —— 最要紧的那条永远在最上面。
 *
 * NOTE: 这一层**只报事实**，一句话都不重写界面上的措辞（`Severity` 那三个词在
 * `severityName()` 里，唯一出处）。
 */

#pragma once

#include <QList>
#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "server/ServerPortCheck.hpp"
#include "server/ServerPropertiesForm.hpp"

namespace ServerCheck {

/// 这一条有多要紧（决定界面上那一行的颜色）。
enum class Severity {
    Problem,  ///< 这样下去起不来 / 起起来是坏的（红）
    Warning,  ///< 能起，但多半不是你想要的（黄）
    Info,     ///< 只是告诉你一声（灰）
};

inline QString severityName(Severity severity) {
    switch (severity) {
    case Severity::Problem:
        return QCoreApplication::translate("ServerCheck", "要处理");
    case Severity::Warning:
        return QCoreApplication::translate("ServerCheck", "提醒");
    case Severity::Info:
        return QCoreApplication::translate("ServerCheck", "说明");
    }
    return QString();
}

/// 这一条对应的出路。`None` = 只能告诉你一声（这一层不编一个"按一下就好了"的动作出来）。
enum class Action {
    None = 0,
    Reinstall,     ///< 按我们记的那一档重装一遍（入口就在行尾那颗「修复」上）
    AcceptEula,    ///< 走一遍 EULA 那道闸（勾选 + 写 eula.txt）
    FixValues,     ///< 把不合法的值改回缺省（改完在「常用设置」那一页能看见）
    OpenSettings,  ///< 去「常用设置」那一页自己改
    OpenBackups,   ///< 去「世界备份」那一页
};

/// 出路摆在界面上的那个**记号**（`ui/` 看不见这个枚举，只认字符串）。
/// 认人认记号、不认按钮上那行字（同服务端行尾那颗按钮的规矩）。
inline QString actionToken(Action action) {
    switch (action) {
    case Action::None:
        return QString();
    case Action::Reinstall:
        return QStringLiteral("reinstall");
    case Action::AcceptEula:
        return QStringLiteral("accept-eula");
    case Action::FixValues:
        return QStringLiteral("fix-values");
    case Action::OpenSettings:
        return QStringLiteral("open-settings");
    case Action::OpenBackups:
        return QStringLiteral("open-backups");
    }
    return QString();
}

/// 体检单上的一条。
struct Finding {
    Severity severity = Severity::Info;
    QString title;   ///< 那一行的大字（一句话说清是什么事）
    QString detail;  ///< 下面那行小字（为什么、怎么办；可以多行）
    Action action = Action::None;
};

/**
 * @brief 体检的原料 —— **全是调用方现查出来的事实**（这一层一个都不自己查）。
 *
 * NOTE: 每一个字段都有缺省值，且缺省值一律是"最无害的那一档"（比如 `metaKnown = false` 谁也
 * 不冤枉、`portState = Unknown` 不报"被占"）。所以调用方**只填自己查得到的那几格**也
 * 是对的：查不到的，这一层就当"没这回事"，绝不猜。
 */
struct Facts {
    /// 装好了没有（判据是 `ServerRegistry::Entry::installed`，就是"入口文件在不在"）。
    bool installed = false;
    /// 没装好时**缺什么**（`Entry::note` 原话，照抄）。
    QString installedNote;
    /// 我们记的那一份（`.wtfml-server`）读得动吗。用户从外面拖进来的服务端就是 false。
    bool metaKnown = false;
    /// 记的那一份够不够拼出启动命令（`ServerModel::hasLaunchableInfo()`）。
    bool launchable = false;
    /// 不够时那句话（`serverLaunchPlan()` 的 `problem`，照抄）。
    QString launchProblem;
    /// `eula.txt` 里写着接受了吗（`ServerRegistry::readEula()`）。
    bool eulaAccepted = false;
    /// 现在在跑吗（在跑的时候有几条要跳过 —— 端口那一项尤其：它自己就占着）。
    bool running = false;
    /// `server.properties` 里读出来的端口；`-1` = 那一项不合法（已经会以 `valueProblems` 里
    /// 那一条报出来，这一层不再重复报一次"端口不对"）。
    int port = -1;
    /// 那个端口现在有人占着吗（`ServerPortCheck::stateOf()` 的结论）。
    ServerPortCheck::State portState = ServerPortCheck::State::Unknown;
    /// `level-name` 指的那个目录名（`ServerPropertiesForm::worldDirName()`；空 = 那一项不合法）。
    QString worldName;
    /// 那个目录在不在。
    bool worldExists = false;
    /// 那个目录有多大（说明用；0 = 空着或没查）。
    qint64 worldBytes = 0;
    /// 模组 / 插件目录里有多少个 jar（说明用）。
    int modCount = 0;
    /// 这个世界的备份有几份（空 = 一份都没有）。
    int worldBackupCount = 0;
    /// `server.properties` 里值不合法的那几项（`ServerPropertiesForm::problemsIn()`）。
    QList<ServerPropertiesForm::ValueProblem> valueProblems;
    /// 同一个键写了两遍、值还不一样（`duplicateKeys()`）。
    QStringList duplicateKeys;
    /// 文件里我们认不出来的键（`unknownKeys()`）。**不是错**，只是说一声。
    QStringList unknownKeys;
};

/// 体检。**次序就是界面上从上到下的次序**（见文件头那段）。
QList<Finding> inspect(const Facts &facts);

/// 一项都没查出来时摆什么（别摆一张空表 —— 用户会以为这一页坏了）。
QString allGoodText();

/// 顶上那句总括：`2 项要处理 · 1 项提醒 · 3 项说明`（没有的那几档不出现）。
QString summaryText(const QList<Finding> &findings);

// ---------------------------------------------------------------------------
// 落点（inline 实现；这一层不碰盘，所以整个头都是纯的）
// ---------------------------------------------------------------------------

inline QString allGoodText() {
    return QCoreApplication::translate("ServerCheck", "启动入口、配置和端口检查均通过。");
}

inline QString summaryText(const QList<Finding> &findings) {
    if (findings.isEmpty()) {
        return allGoodText();
    }
    int problems = 0;
    int warnings = 0;
    int infos = 0;
    for (const Finding &finding : findings) {
        switch (finding.severity) {
        case Severity::Problem:
            ++problems;
            break;
        case Severity::Warning:
            ++warnings;
            break;
        case Severity::Info:
            ++infos;
            break;
        }
    }
    QStringList parts;
    if (problems > 0) {
        parts.append(QCoreApplication::translate("ServerCheck", "%1 项要处理").arg(problems));
    }
    if (warnings > 0) {
        parts.append(QCoreApplication::translate("ServerCheck", "%1 项提醒").arg(warnings));
    }
    if (infos > 0) {
        parts.append(QCoreApplication::translate("ServerCheck", "%1 项说明").arg(infos));
    }
    return parts.join(QStringLiteral(" / "));
}

inline QList<Finding> inspect(const Facts &facts) {
    QList<Finding> findings;

    // ---------- ① 拦着它起不来的那几件（次序见文件头）----------
    if (!facts.installed) {
        QString detail = facts.installedNote;
        if (detail.isEmpty()) {
            detail = QCoreApplication::translate("ServerCheck", "入口文件不在当前目录中。");
        }
        detail += QCoreApplication::translate("ServerCheck", "\n点击「修复（重新安装）」下载缺失文件。");
        findings.append(Finding{Severity::Problem, QCoreApplication::translate("ServerCheck", "此服务端尚未安装完成"), detail,
                                Action::Reinstall});
    } else {
        // 装好了才有"记的那一份说不说得清"这一说（没装好那一条已经把话说了）。
        if (!facts.metaKnown) {
            findings.append(Finding{
                Severity::Warning, QCoreApplication::translate("ServerCheck", "服务端安装记录缺失"),
                QCoreApplication::translate("ServerCheck", "无法确定安装类型与重装参数。请检查启动文件，" "或通过启动器重新创建服务端。"),
                Action::None});
        } else if (!facts.launchable) {
            findings.append(Finding{
                Severity::Problem, QCoreApplication::translate("ServerCheck", "无法根据安装记录生成启动命令"),
                (facts.launchProblem.isEmpty()
                     ? QCoreApplication::translate("ServerCheck", "安装记录缺少启动所需的版本信息。")
                     : facts.launchProblem)
                    + QCoreApplication::translate("ServerCheck", "\n点击「修复（重新安装）」按记录的来源完整重装。"),
                Action::Reinstall});
        }
    }

    if (facts.installed && !facts.eulaAccepted) {
        findings.append(Finding{
            Severity::Problem, QCoreApplication::translate("ServerCheck", "尚未接受 Minecraft 最终用户许可协议"),
            QCoreApplication::translate("ServerCheck", "首次启动前需阅读并接受协议，" "确认后将更新 eula.txt。"),
            Action::AcceptEula});
    }

    for (const ServerPropertiesForm::ValueProblem &problem : facts.valueProblems) {
        findings.append(Finding{
            Severity::Problem,
            QCoreApplication::translate("ServerCheck", "「%1」的值无效").arg(problem.label),
            QCoreApplication::translate("ServerCheck", "当前值：%1。%2\n可恢复默认值，" "或在「常用设置」中修改。")
                .arg(problem.value.isEmpty() ? QCoreApplication::translate("ServerCheck", "（空）") : problem.value,
                     problem.problem),
            Action::FixValues});
    }

    // 端口那一条：**只在查得出端口、而且没在跑的时候说**。在跑的时候它自己就占着那个端口，
    // 报"被人占了"是一句吓人的废话（见 Facts::portState 那段）。
    if (facts.installed && !facts.running && facts.port > 0
        && facts.portState == ServerPortCheck::State::InUse) {
        findings.append(Finding{
            Severity::Problem, QCoreApplication::translate("ServerCheck", "端口 %1 已被占用").arg(facts.port),
            QCoreApplication::translate("ServerCheck", "端口冲突将导致服务端启动失败。\n" "请关闭占用端口的程序，或在「常用设置」中更换端口。"),
            Action::OpenSettings});
    }

    // ---------- ② 提醒 ----------
    if (!facts.duplicateKeys.isEmpty()) {
        findings.append(Finding{
            Severity::Warning,
            QCoreApplication::translate("ServerCheck", "配置包含重复键：%1")
                .arg(facts.duplicateKeys.join(QCoreApplication::translate("ServerCheck", "、"))),
            QCoreApplication::translate("ServerCheck", "重复键以最后一项为准。" "可在「原始文本」中移除重复项。"),
            Action::None});
    }
    if (facts.installed && facts.worldBackupCount <= 0 && facts.worldExists) {
        findings.append(Finding{
            Severity::Warning, QCoreApplication::translate("ServerCheck", "世界尚无备份"),
            QCoreApplication::translate("ServerCheck", "世界数据无法通过重新下载恢复。" "建议在「世界备份」中选择" "「立即备份」。"),
            Action::OpenBackups});
    }

    // ---------- ③ 说明 ----------
    if (facts.installed && !facts.worldName.isEmpty() && !facts.worldExists) {
        // 新装的、还没跑过的服务端就是这样：世界要等它第一次起来才生成。
        findings.append(Finding{
            Severity::Info,
            QCoreApplication::translate("ServerCheck", "世界「%1」尚未生成").arg(facts.worldName),
            QCoreApplication::translate("ServerCheck", "世界目录将在首次启动时生成。"),
            Action::None});
    } else if (facts.installed && facts.worldExists) {
        findings.append(Finding{
            Severity::Info, QCoreApplication::translate("ServerCheck", "世界「%1」已存在").arg(facts.worldName),
            QCoreApplication::translate("ServerCheck", "目录：%1/%2。")
                .arg(facts.worldName,
                     facts.worldBytes > 0
                         ? QCoreApplication::translate("ServerCheck", "，大小 %1 MB")
                               .arg(QString::number(double(facts.worldBytes) / (1024.0 * 1024.0),
                                                    'f', 1))
                         : QString()),
            Action::None});
    }
    if (facts.modCount > 0) {
        findings.append(Finding{
            Severity::Info,
            QCoreApplication::translate("ServerCheck", "已安装 %1 个模组或插件").arg(facts.modCount),
            QCoreApplication::translate("ServerCheck", "新增或更改模组后需重启服务端。"),
            Action::None});
    }
    if (!facts.unknownKeys.isEmpty()) {
        findings.append(Finding{
            Severity::Info,
            QCoreApplication::translate("ServerCheck", "配置包含 %1 项无法通过表单编辑的设置")
                .arg(facts.unknownKeys.size()),
            QCoreApplication::translate("ServerCheck", "以下设置（%1）可在「原始文本」中编辑。" "保存表单时将保留其原值。")
                .arg(facts.unknownKeys.mid(0, 6).join(QCoreApplication::translate("ServerCheck", "、"))),
            Action::None});
    }
    return findings;
}

}  // namespace ServerCheck
