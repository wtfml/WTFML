/**
 * @file TextFormat.hpp
 * @brief 彩色文本：`§` 码 ↔ 一段一段带样式的正文 ↔ JSON 文本组件 ↔ `/tellraw` 命令。
 * @note 用户点名的工具箱第二件：「Minecraft 文本
 * 生成器：首版只做 MOTD 与传统 `§` 格式文本，编辑与预览能力和服务端设置页共用」，
 * 外加他那一句「看看还有彩色文本的命令什么的」—— 所以这一层除了 `§` 那种写法，
 * 还要出得了**命令那一侧**的两种形态：JSON 文本组件（`/tellraw` 的正文）和拼好的
 * `/tellraw` 命令本身。
 *
 * 【三份形态，同一件事】
 *
 * | 形态 | 长什么样 | 哪儿用得上 |
 * |---|---|---|
 * | 传统 `§` | `§c红§l粗` | `server.properties` 的 MOTD、书、告示牌、名字 |
 * | JSON 组件 | `[{"text":"红","color":"red"},{"text":"粗","bold":true}]` | `/tellraw`、`/title`、对话框、数据包 |
 * | 命令 | `/tellraw @a [{"text":"红",…}]` | 聊天栏直接粘 |
 * | MOTD 那一行 | `motd=§c…` | `server.properties`（按 ISO-8859-1 读，非 ASCII 要转义） |
 *
 * 「同一件事」是这一层的**唯一理由**：三份形态里那份"一段一段的正文"只有一份模型
 * （`Segment`），三个方向都是它的译文。谁也别在界面里现拼字符串 —— 拼一遍就等于多一份
 * 转义规矩（`"` 和 `\` 在 JSON 里、`§` 在正文里，各是各的坑）。
 *
 * 【`§` 的规矩（照游戏来，不照"看着对"来）】
 *
 * - `0-9 a-f` 十六色，`k` 随机、`l` 粗、`m` 删除线、`n` 下划线、`o` 斜体，`r` 全部复位；
 * - **颜色码会把格式位一起清掉**（`§c` 之后那个粗体就不算数了）—— 游戏是这么干的，
 * `§a§lb` 里那个 `b` 是粗的、`§a§lb§cc` 里的 `c` 就不是；
 * - `§r` 把颜色和格式位一起复位（回"默认色 + 都不加"）；
 * - **两个 `§` 挨着**（`§§`）不是转义，游戏照样按"后一个是码"读；这一层照抄，不做聪明事；
 * - 收尾那个孤零零的 `§`（后面什么都没有）**原样留着**：它是正文的一部分，丢掉就等于
 * 改写了用户输入。
 *
 * NOTE: 这一层**不碰任何 Qt GUI**（`ui/` 不比 `src/` 高一级，方向不能倒过来）：那十六个
 * 颜色值以 `0xRRGGBB` 这种纯数字给出（`colorRgb()`），画成什么样是界面那一层的事。
 */

#pragma once

#include <QChar>
#include <QJsonArray>
#include <QList>
#include <QString>

namespace TextFormat {

/// `§` 本身。写成一个常量：全项目不许再出现第二个字面量 `§`（那是个非 ASCII 字符，
/// 编码没对上时它坏得很难看）。
inline constexpr QChar kSectionSign = QChar(0x00A7);

/// 十六色 + 默认色。顺序照 `§` 的码位排（0-9 a-f），`Default` 是"没写过颜色"那个状态。
enum class Color {
    Default,
    Black,        ///< 颜色码 0
    DarkBlue,     ///< 颜色码 1
    DarkGreen,    ///< 颜色码 2
    DarkAqua,     ///< 颜色码 3
    DarkRed,      ///< 颜色码 4
    DarkPurple,   ///< 颜色码 5
    Gold,         ///< 颜色码 6
    Gray,         ///< 颜色码 7
    DarkGray,     ///< 颜色码 8
    Blue,         ///< 颜色码 9
    Green,        ///< §a
    Aqua,         ///< §b
    Red,          ///< §c
    LightPurple,  ///< §d
    Yellow,       ///< §e
    White,        ///< §f
};

/// 一段正文的样式。**每一段都是自足的**（不是"相对上一段"的增量）—— 增量那种写法在
/// 中间插一段、删一段的时候必然出错。
struct Style {
    Color color = Color::Default;
    /// 1.16 起那份自定颜色（`#RRGGBB`；`§` 那边的写法是 `§x§R§R§G§G§B§B`）。
    /// 它一在位，`color` 就**不算数** —— 两个都写会让"到底哪个是色"得靠读者猜。
    bool hasCustomColor = false;
    quint32 customRgb = 0xFFFFFF;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool obfuscated = false;

    bool operator==(const Style &other) const;
    bool operator!=(const Style &other) const { return !(*this == other); }
    /// 一位都没加（默认色、五个格式位全假）。
    bool isPlain() const;
};

/// 一段正文 + 它的样式。
struct Segment {
    QString text;
    Style style;

    bool operator==(const Segment &other) const;
};

/// `§` 码那个字符对应的颜色 / 格式位。不是 `§` 码就回 `Default` / 什么都不加。
/// @param code `§` **后面**那个字符（大小写都认：`§A` 和 `§a` 是同一个色）。
Color colorFromCode(QChar code);
/// 反向：这个颜色是哪一个码（`Default` 回 `QChar()`）。
QChar codeForColor(Color color);
/// 这个字符是不是一个颜色码（`0-9a-f`）。
bool isColorCode(QChar code);
/// 这个字符是不是一个格式码（`k l m n o` 或复位的 `r`）。
bool isFormatCode(QChar code);

/// 颜色名（`red` / `dark_red`；`Default` 回空串）—— JSON 组件那一侧用的就是这一串。
QString colorName(Color color);
/// 反向。认不出来回 `Default`（**不猜**）。
Color colorFromName(const QString &name);
/// 颜色在游戏里那份十六进制值（`0xRRGGBB`）。`Default` 回 `0xFFFFFF`（浅底正文色）。
/// NOTE: 这是**画预览**用的近似值，不是"游戏渲染出来的那个像素"。
quint32 colorRgb(Color color);

/// 自定颜色那一套：`#RRGGBB` ↔ `0xRRGGBB`。
/// @param text 认带不带 `#` 都行、大小写都认，**只认六位**（三位那种缩写不猜）。
/// @param rgb 读出来写这儿（失败时不改）。
bool parseHexColor(const QString &text, quint32 *rgb);
/// `0xRRGGBB` → `#rrggbb`（JSON 组件里就是这一串）。
QString hexColorName(quint32 rgb);
/// `0xRRGGBB` → `§x§R§R§G§G§B§B`（1.16 起那份 `§` 写法；界面「插自定颜色」用它）。
QString legacyHexColor(quint32 rgb);
/// 这一段实际画出来是什么色：自定色优先，否则那张十六色表。
quint32 styleRgb(const Style &style);
/// 这一段颜色的说法（自定色给 `#rrggbb`，具名色给中文名，默认给「默认」）。
QString styleColorText(const Style &style);

/// 颜色 / 格式位的中文名（界面上那个色板与开关的悬停提示用）。
QString colorText(Color color);
QString styleText(Color color);
QString formatText(QChar code);

/**
 * @brief 把一段 `§` 文本拆成一段一段的正文。
 *
 * 空串回**空表**（不是"一段空正文"）：界面上一段都没有和"有一段空的"是两件事。
 */
QList<Segment> parseLegacy(const QString &text);

/**
 * @brief 把一段一段的正文拼回 `§` 文本。
 *
 * 每一段都重新写一遍颜色与格式位，然后收尾补一个 `§r` —— **只在有必要时补**（最后一段
 * 没有样式就不补）。每段重开一遍是刻意的：拼回去的这份文本会被粘到别处再被读一遍，
 * 谁也不用去猜"上一段的粗体还在不在"。
 */
QString toLegacy(const QList<Segment> &segments);

/**
 * @brief 转成 JSON 文本组件（`/tellraw` 那个正文）。
 *
 * - 颜色与格式位**只写非默认的那些**（`{"text":"x"}` 而不是把五位全写 false）——
 * 写全了读起来噪音太大，而且游戏那边默认值本来就是这些；
 * - 提交的正文里 `"` 和 `\` 交给 QJson 转义，这一层不手拼。
 */
QJsonArray toJson(const QList<Segment> &segments);

/// JSON 组件的那一串文本（紧凑写法，不带换行）。空表回 `[]`。
QString toJsonText(const QList<Segment> &segments);

/**
 * @brief 拼一条 `/tellraw` 命令。
 *
 * @param segments 正文。
 * @param target 收话的人（默认 `@a`）。**原样用**：这一层不认识选择器语法，也不去校验
 * —— 它是用户自己填的一格，填错了游戏那边会说话。
 * @param leadingSlash 开头要不要带 `/`（聊天栏里粘要带、命令方块里不带）。
 *
 * NOTE: 空表**照样出一条合法命令**（正文是空数组）：这是"我点了一下生成"的产物，不是
 * 一份内容；返回空串会让界面无从区分"没生成"和"生成了个空的"。
 */
QString toTellrawCommand(const QList<Segment> &segments, const QString &target = QStringLiteral("@a"),
                         bool leadingSlash = true);

/**
 * @brief `server.properties` 里那一整行 `motd=…`。
 *
 * 游戏读 properties 走的是 Java 那套 **ISO-8859-1** 的规矩：直接写中文 / `§`，在有些
 * 命令行参数、有些编辑器存盘之后会变成一堆问号。所以这一层照 Java 的转义规矩写：
 * `\` → `\\`、换行 → `\n`、**凡是 ASCII 可打印范围以外的字（含 `§` 和汉字）一律
 * `\uXXXX`**。粘进 properties 之后，游戏读回来还是原来那段。
 *
 * 值开头的空白也要转义（Java 会把 `=` 后面那串开头的空白吃掉）。
 */
QString toMotd(const QList<Segment> &segments);

/**
 * @brief 反着读：把一份 JSON 组件（`/tellraw` 的正文）读回一段一段的正文。
 *
 * 认 `{"text":…,"color":…,"bold":…}` 与**纯字符串元素**（`["a","b"]`），也认
 * `extra`（一个组件里嵌几个子组件）。认不出来的那几种（`translate` / `score` / `selector` /
 * `keybind` / `nbt`）**不猜**：整份回来时给一个失败值，界面上明说"这几种还没法读"。
 */
struct JsonParseResult {
    bool ok = false;
    /// 认不出来的那些组件的类型名（`translate` 等），按遇到它们的次序、去重。
    QStringList unsupported;
    QList<Segment> segments;
};
JsonParseResult parseJson(const QString &jsonText);

}  // namespace TextFormat
