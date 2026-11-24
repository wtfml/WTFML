/**
 * @file QuickPlay.hpp
 * @brief 「开游戏直接进某个存档 / 直接连某台服务器」那一对参数（`--quickPlaySingleplayer`
 * / `--quickPlayMultiplayer`）的判据层。
 * @note 要求：「世界和服务器都做一个临时占位（加入选中的世界（仅限有这个
 * 参数的版本）/服务器）先试试看能不能生效」。
 *
 * 做成 header-only：它只依赖 `QJsonObject`，两头都要用 —— 启动那一侧拼命令行
 * （`LauncherManager::generateJvmArguments`），界面那一侧决定那两颗按钮露不露、灰不灰
 * （`MainWindow` 算好递进实例设置页）。和 `VersionArguments` 是同一个形状、同一个理由。
 *
 * 【为什么「有这个参数」要问版本 json，而不是问版本号】
 *
 * quick play 是 **1.20 才有的**（1.20 之前 `Main` 的选项表里没有它），而那两条参数在
 * 版本 json 里是**带 feature 规则**的条目：
 * {"rules":[{"action":"allow","features":{"is_quick_play_singleplayer":true}}],
 * "value":["--quickPlaySingleplayer","${quickPlaySingleplayer}"]}
 * 也就是说，**json 里写了这一条 == 这个版本的 `Main` 认这个选项**（那份 json 是照着游戏
 * 自己的选项表生成的）。把版本号当判据（"1.20 及以上"）会在快照 / 模改版上立刻出错，而
 * 且 1.20 那条线以后还会移 —— 所以判据只有这一个：**json 里有没有写着这一条**。
 *
 * 真发了不认它的版本，游戏不是"忽略"，是**当场退出**：jopt-simple 遇到不认识的选项
 * 抛 `UnrecognizedOptionException`，`Main` 起不来。所以这两颗按钮在支持的版本上才露，
 * 别的版本上一颗都发不出去（`supports()` 就是那道闸）。
 *
 * NOTE: `--quickPlaySingleplayer` / `--quickPlayMultiplayer` 两位在
 * `VersionArguments::isVanillaGameArgument()` 的名单里 —— 那是"这一条由启动器
 * 自己发"的意思，json 自己声明的那一条（连同它的 `${quickPlaySingleplayer}`）
 * 会被整条丢掉，所以**必须**从这里发出去，和 `--width`/`--height` 同一档。
 */

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace QuickPlay {

/// 进一个存档，还是连一台服务器。两位一一对应原版那两条参数（名字在 `argumentName()`）。
enum class Kind {
    Singleplayer,
    Multiplayer,
};

/**
 * @brief 这一次要进哪儿。
 *
 * `value` 为空 = **没有这一发**（`isEmpty()` 恒真）—— 那是绝大多数启动的处境，也是这一层
 * 能被无脑接进 `launchArguments()` 的原因：没这一发时命令行上多不出任何东西。
 */
struct Target {
    Kind kind = Kind::Singleplayer;
    /// `Singleplayer` 给的是**存档目录名**（不是路径）—— 1.20 起的 `--quickPlaySingleplayer`
    /// 收的就是这个名字，游戏自己去 `saves/` 底下找。
    /// `Multiplayer` 给的是服务器地址（`host` 或 `host:port`，就是 `servers.dat` 里那一串）。
    QString value;

    bool isEmpty() const { return value.isEmpty(); }
};

/// 这一位对应的命令行选项名。**唯一出处**（判据、拼参数、提示语都从这儿取）。
inline QString argumentName(Kind kind) {
    return kind == Kind::Multiplayer ? QStringLiteral("--quickPlayMultiplayer")
                                     : QStringLiteral("--quickPlaySingleplayer");
}

/// 拼成命令行上的那两个 token（没这一发时是空表）。
inline QStringList arguments(const Target &target) {
    if (target.isEmpty()) {
        return QStringList();
    }
    return QStringList{argumentName(target.kind), target.value};
}

/**
 * @brief 这个版本认不认这一条 quick play 参数。
 * @param versionJson 版本 json —— **继承链合并过的那一份**（加载器版本自己的 profile 里
 * 一个 quick play 条目都没有，它们全在父那一份里；拿没合并的那份问，Forge /
 * NeoForge / Fabric 的实例会一个都答"不支持"）。
 *
 * 判据：`arguments.game` 里有没有写着这个名字（字符串条目直接比；对象条目看它的 `value`，
 * 字符串或字符串数组两种形态都认）。**不看 rules** —— 规则里那个 feature 名
 * （`is_quick_play_singleplayer`）只是"启动器要不要替它发"的条件，而这一条恰恰是启动器
 * 自己发；"json 里出现过这个名字"就已经说明这个版本的选项表里有它。
 *
 * 老格式（`minecraftArguments` 那个空格分隔的字符串）也扫一遍：1.12.2 那一代压根没有
 * quick play，这一支实际恒假；写在这儿只是为了"两种格式都认"这条规矩别只活在
 * `VersionArguments` 里（见那边 `extraGameArguments()` 的第 2 条）。
 */
inline bool supports(const QJsonObject &versionJson, Kind kind) {
    const QString flag = argumentName(kind);
    const QJsonObject arguments = versionJson.value(QStringLiteral("arguments")).toObject();
    for (const QJsonValue &entry : arguments.value(QStringLiteral("game")).toArray()) {
        if (entry.isString()) {
            if (entry.toString() == flag) {
                return true;
            }
            continue;
        }
        if (!entry.isObject()) {
            continue;
        }
        const QJsonValue value = entry.toObject().value(QStringLiteral("value"));
        if (value.isArray()) {
            for (const QJsonValue &item : value.toArray()) {
                if (item.isString() && item.toString() == flag) {
                    return true;
                }
            }
        } else if (value.isString() && value.toString() == flag) {
            return true;
        }
    }
    const QString legacy = versionJson.value(QStringLiteral("minecraftArguments")).toString();
    const QStringList tokens = legacy.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return tokens.contains(flag);
}

/**
 * @brief 这个版本用不上这一条时，界面上要说清为什么。
 *
 * 归 `pageUnavailableText()` 那一类（**一句话说清是哪件事、为什么**），两处读者：
 * 按钮的悬停提示、以及真点下去那一刻的兜底（按钮理论上露不出来，但判据和按钮的显隐
 * 不该各说各的 —— 那正是 `wtfml-comment-vs-code` 那个老毛病）。
 */
inline QString unsupportedText(Kind kind) {
    if (kind == Kind::Multiplayer) {
        return QStringLiteral("「加入服务器」要用游戏自己的 --quickPlayMultiplayer 参数，"
                              "这个版本（1.20 之前）没有它 —— 发了游戏会直接报错退出。");
    }
    return QStringLiteral("「加入选中的世界」要用游戏自己的 --quickPlaySingleplayer 参数，"
                          "这个版本（1.20 之前）没有它 —— 发了游戏会直接报错退出。");
}

}  // namespace QuickPlay
