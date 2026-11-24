/**
 * @file VersionDisplay.cpp
 * @brief 实现版本显示名的归一化与拼装。
 * @note 见 VersionDisplay.hpp 的说明。
 */

#include "VersionDisplay.hpp"

#include <QDate>
#include <QDateTime>
#include <QStringList>

#include <algorithm>

namespace {

/// 单段的最大长度。手写的 wtfml.meta 里塞一整句话进来是很容易的事，
/// 这里封顶，免得它把「启动游戏」按钮和下拉框撑得没法看。
constexpr int kMaxComponentLength = 64;

/**
 * @brief 清洗一段用户可控的文本。
 * @param raw 原始文本。
 * @return 清掉首尾空白、把内部连续空白（含 \n \r \t）折成单个空格、并截断到长度上限的文本。
 *
 * QString::simplified() 正好是前两件事：它按 QChar::isSpace() 判空白，换行和制表符都算。
 * 必须清换行的原因很直接——「启动游戏」按钮的文字是"启动游戏\n<显示名>"，
 * 名字里带一个 '\n' 就变成三行，而不是两行了。
 */
QString sanitizeComponent(const QString &raw) {
    QString text = raw.simplified();
    if (text.size() > kMaxComponentLength) {
        int cut = kMaxComponentLength;
        // 截断点正好劈开一个代理对（R16）：第 cut-1 格是高位、第 cut 格是低位。
        // 直接 truncate(cut) 会留下**半个**字符（一个孤立的代理项），画出来是方块。
        // 退回一格，宁可少一个字符。
        if (text.at(cut - 1).isHighSurrogate() && text.at(cut).isLowSurrogate()) {
            --cut;
        }
        text.truncate(cut);
        // 截断点正好落在一个空格上时，会留下一个尾随空格。simplified() 只保证
        // **截断前**没有首尾空白，截断本身可能重新制造一个。而这里的返回值会被
        // 直接拿去用 '-' 拼接，尾随空格会变成 "xxx -1.20.1" 这种看着像 bug 的东西。
        while (text.endsWith(QLatin1Char(' '))) {
            text.chop(1);
        }
    }
    return text;
}

}  // namespace

QString formatVersionName(const VersionDisplayParts &parts) {
    const QString id = sanitizeComponent(parts.id);
    const QString customName = sanitizeComponent(parts.customName);
    const QString loaderName = sanitizeComponent(parts.loaderName);

    // 实际版本：元数据里写了 "realVersion" 就用它，否则退化成目录名 —— 老实例
    // （没有 wtfml.meta）就这样逐字节不变。
    // NOTE: 必须是 realVersion 而不是 id 参与后面两段，否则目录名里已经揉进去的加载器
    // （`1.18.2-Forge_40.3.12`）会和 loaderName 拼出重复段，见 .hpp 里的说明。
    // NOTE: 清洗**要在判空之前**。反过来（先判空、再清洗）的话，realVersion 写着
    // "只有空白 / 控制字符"那种（wtfml.meta 是能手改的文本文件）时，清洗完是空串 ——
    // 名称槽和版本段**两段都没人收**，而 chosen 里跳空串的那条规矩又把它悄悄吃掉，
    // 最后拼出来的名字是空的、界面上一块空白。
    // 清洗完还是空 = 当没有这一项，退回目录名（没有 wtfml.meta 的老实例也走这条）。
    QString realVersion = sanitizeComponent(parts.realVersion);
    if (realVersion.isEmpty()) {
        realVersion = id;
    }

    // 名称槽：没重命名过就退化成实际版本
    const QString label = customName.isEmpty() ? realVersion : customName;

    // 四段都走同一个收口，重复的文本只留第一份。对称是刻意的：只对某一段去重的话，
    // 加载器名恰好等于版本号（或者等于名称）时会拼出 `Forge-Forge` 这种重复段。
    QStringList chosen;
    const auto appendIfNew = [&chosen](const QString &text) {
        if (text.isEmpty() || chosen.contains(text)) {
            return;
        }
        chosen.append(text);
    };

    // 顺序与"出厂默认的那一档"一致：名称槽 → 实际版本 → 加载器名。
    // 加载器版本（`loaderVersion`）不进显示名 —— 出厂默认就把它关着（UI 问题 #14 之后
    // 这一档没有开关，拼法固定成这一份，没改过设置的用户看到的字一个都不变）。
    appendIfNew(label);
    appendIfNew(realVersion);
    appendIfNew(loaderName);

    return chosen.join(QLatin1Char('-'));
}

QString launchStatusName(const VersionDisplayParts &parts) {
    const QString customName = sanitizeComponent(parts.customName);
    // 有名字就**只用名字**（理由见 .hpp 里那两条）。
    if (!customName.isEmpty()) {
        return customName;
    }

    // 没有名字才轮到"版本号-加载器名"。实际版本那条兜底和 formatVersionName() 一模一样
    // （realVersion 为空就退化成目录名）—— 两处必须一致，否则同一个实例在"启动中"这一句
    // 和下拉框里会报出两个不同的版本号。
    //
    // 两处**都要先清洗再判空**：`parts.realVersion.isEmpty() ? id : sanitizeComponent(...)`
    // 那种写法在 realVersion 非空但清洗完为空（只有空白 / 控制字符，wtfml.meta
    // 是能手改的文本文件）时会和 formatVersionName() **当场分开** —— 列表里显示目录名
    // `1.20.1`，底栏"启动中"那一句显示加载器名（或者干脆空白）。
    const QString id = sanitizeComponent(parts.id);
    QString base = sanitizeComponent(parts.realVersion);
    if (base.isEmpty()) {
        base = id;
    }
    const QString loaderName = sanitizeComponent(parts.loaderName);
    if (base.isEmpty()) {
        // 目录名也是空的（理论上不会，id 的注释说它永远非空）—— 有什么给什么，
        // 别拼出 "-Forge" 这种以分隔符打头的东西。
        return loaderName;
    }
    return loaderName.isEmpty() ? base : QStringLiteral("%1-%2").arg(base, loaderName);
}

QString releaseDateText(const QString &releaseTime) {
    const QString trimmed = releaseTime.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    // 清单里是 ISO8601（`T11:23:02+00:00`，也有 `…Z` 那种写法）。fromString
    // 认这两种，而且取出来的**就是那个偏移量下的日期**（Qt 不会偷偷替谁换算一遍），
    // 所以 UTC 的清单不会因为本机在 UTC+8 就把当天下午发布的版本显示成第二天。
    const QDateTime parsed = QDateTime::fromString(trimmed, Qt::ISODate);
    if (parsed.isValid()) {
        return parsed.date().toString(QStringLiteral("yyyy-MM-dd"));
    }
    // 认不出来时还有一条兜底：前 10 个字符正好长得像 `YYYY-MM-DD` 就认它。
    // （哪天清单换成像 ` 11:23:02` 这种不带 T 的写法，fromString 会认不出来，
    // 但那 10 个字符仍然是对的。）
    const QString head = trimmed.left(10);
    if (head.size() == 10 && head.at(4) == QLatin1Char('-') && head.at(7) == QLatin1Char('-') &&
        QDate::fromString(head, QStringLiteral("yyyy-MM-dd")).isValid()) {
        return head;
    }
    return QString();
}

QString resolveCurrentInstance(const QStringList &ids, const QString &previousId, int previousIndex,
                               const QString &lastLaunched, const QString &lastSelected) {
    if (ids.isEmpty()) {
        return QString();
    }
    if (!previousId.isEmpty()) {
        if (ids.contains(previousId)) {
            return previousId;
        }
        return ids.at(std::clamp(previousIndex, 0, int(ids.size()) - 1));
    }
    for (const QString &remembered : {lastLaunched, lastSelected}) {
        if (!remembered.isEmpty() && ids.contains(remembered)) {
            return remembered;
        }
    }
    return ids.first();
}
