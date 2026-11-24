/**
 * @file TextFormat.cpp
 * @brief TextFormat 的实现（三份形态的规矩在 .hpp 开头那一段）。
 */
#include "TextFormat.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <functional>

namespace {

/// 那个十六色表（下标就是 `§` 的码位：0-9 a-f）。
///
/// 值取自游戏自己的那份（浅底正文色是 `§f` 白）。`Default` 不在表里 —— 它是"没写过
/// 颜色"，画成正文色才对；映射到某个具体的色上就等于替用户拿主意了。
struct ColorInfo {
    TextFormat::Color color;
    QChar code;
    const char *name;
    quint32 rgb;
    const char *text;  ///< 中文名（色板上那一格）
};

const ColorInfo kColors[] = {
    {TextFormat::Color::Black, QChar('0'), "black", 0x000000, "黑"},
    {TextFormat::Color::DarkBlue, QChar('1'), "dark_blue", 0x0000AA, "深蓝"},
    {TextFormat::Color::DarkGreen, QChar('2'), "dark_green", 0x00AA00, "深绿"},
    {TextFormat::Color::DarkAqua, QChar('3'), "dark_aqua", 0x00AAAA, "深青"},
    {TextFormat::Color::DarkRed, QChar('4'), "dark_red", 0xAA0000, "深红"},
    {TextFormat::Color::DarkPurple, QChar('5'), "dark_purple", 0xAA00AA, "深紫"},
    {TextFormat::Color::Gold, QChar('6'), "gold", 0xFFAA00, "金"},
    {TextFormat::Color::Gray, QChar('7'), "gray", 0xAAAAAA, "灰"},
    {TextFormat::Color::DarkGray, QChar('8'), "dark_gray", 0x555555, "深灰"},
    {TextFormat::Color::Blue, QChar('9'), "blue", 0x5555FF, "蓝"},
    {TextFormat::Color::Green, QChar('a'), "green", 0x55FF55, "绿"},
    {TextFormat::Color::Aqua, QChar('b'), "aqua", 0x55FFFF, "青"},
    {TextFormat::Color::Red, QChar('c'), "red", 0xFF5555, "红"},
    {TextFormat::Color::LightPurple, QChar('d'), "light_purple", 0xFF55FF, "浅紫"},
    {TextFormat::Color::Yellow, QChar('e'), "yellow", 0xFFFF55, "黄"},
    {TextFormat::Color::White, QChar('f'), "white", 0xFFFFFF, "白"},
};

/// 格式码那五个（复位的 `r` 不在里面：它不是一个"位"）。
struct FormatInfo {
    QChar code;
    const char *name;   ///< JSON 里那个字段名
    const char *text;   ///< 中文名
};

const FormatInfo kFormats[] = {
    {QChar('k'), "obfuscated", "随机"},
    {QChar('l'), "bold", "粗体"},
    {QChar('m'), "strikethrough", "删除线"},
    {QChar('n'), "underline", "下划线"},
    {QChar('o'), "italic", "斜体"},
};

/// 这一位是不是开着（`Style` 里那五个开关按 `kFormats` 的次序读）。
bool formatFlag(const TextFormat::Style &style, int index) {
    switch (index) {
    case 0:
        return style.obfuscated;
    case 1:
        return style.bold;
    case 2:
        return style.strikethrough;
    case 3:
        return style.underline;
    case 4:
        return style.italic;
    default:
        return false;
    }
}

/// 把第 @p index 位打开（写回 `Style`）。
void setFormatFlag(TextFormat::Style &style, int index, bool on) {
    switch (index) {
    case 0:
        style.obfuscated = on;
        break;
    case 1:
        style.bold = on;
        break;
    case 2:
        style.strikethrough = on;
        break;
    case 3:
        style.underline = on;
        break;
    case 4:
        style.italic = on;
        break;
    default:
        break;
    }
}

/// `§` 后面那个字符能不能当"一个码"认。
bool isAnyCode(QChar code) {
    const QChar lower = code.toLower();
    return TextFormat::isColorCode(lower) || TextFormat::isFormatCode(lower);
}

}  // namespace

namespace TextFormat {

bool Style::operator==(const Style &other) const {
    return color == other.color && hasCustomColor == other.hasCustomColor
           && (!hasCustomColor || customRgb == other.customRgb) && bold == other.bold
           && italic == other.italic && underline == other.underline
           && strikethrough == other.strikethrough && obfuscated == other.obfuscated;
}

bool Style::isPlain() const {
    return color == Color::Default && !hasCustomColor && !bold && !italic && !underline
           && !strikethrough && !obfuscated;
}

bool Segment::operator==(const Segment &other) const {
    return text == other.text && style == other.style;
}

Color colorFromCode(QChar code) {
    const QChar lower = code.toLower();
    for (const ColorInfo &info : kColors) {
        if (info.code == lower) {
            return info.color;
        }
    }
    return Color::Default;
}

QChar codeForColor(Color color) {
    for (const ColorInfo &info : kColors) {
        if (info.color == color) {
            return info.code;
        }
    }
    return QChar();
}

bool isColorCode(QChar code) {
    const QChar lower = code.toLower();
    return (lower >= QChar('0') && lower <= QChar('9')) || (lower >= QChar('a') && lower <= QChar('f'));
}

bool isFormatCode(QChar code) {
    const QChar lower = code.toLower();
    for (const FormatInfo &info : kFormats) {
        if (info.code == lower) {
            return true;
        }
    }
    return lower == QChar('r');
}

QString colorName(Color color) {
    for (const ColorInfo &info : kColors) {
        if (info.color == color) {
            return QString::fromLatin1(info.name);
        }
    }
    return QString();
}

Color colorFromName(const QString &name) {
    const QString lower = name.trimmed().toLower();
    if (lower.isEmpty() || lower == QLatin1String("reset")) {
        return Color::Default;
    }
    for (const ColorInfo &info : kColors) {
        if (lower == QLatin1String(info.name)) {
            return info.color;
        }
    }
    return Color::Default;
}

quint32 colorRgb(Color color) {
    for (const ColorInfo &info : kColors) {
        if (info.color == color) {
            return info.rgb;
        }
    }
    return 0xFFFFFF;
}

namespace {

/// 一个十六进制字符值（不是就回 -1）。
int hexDigit(QChar ch) {
    const QChar lower = ch.toLower();
    if (lower >= QChar('0') && lower <= QChar('9')) {
        return lower.unicode() - QChar('0').unicode();
    }
    if (lower >= QChar('a') && lower <= QChar('f')) {
        return lower.unicode() - QChar('a').unicode() + 10;
    }
    return -1;
}

}  // namespace

bool parseHexColor(const QString &text, quint32 *rgb) {
    QString body = text.trimmed();
    if (body.startsWith(QChar('#'))) {
        body = body.mid(1);
    }
    // 只认六位：`#f00` 那种缩写游戏那边不认，这一层替它展开就等于猜。
    if (body.size() != 6) {
        return false;
    }
    quint32 value = 0;
    for (const QChar ch : body) {
        const int digit = hexDigit(ch);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<quint32>(digit);
    }
    if (rgb != nullptr) {
        *rgb = value;
    }
    return true;
}

QString hexColorName(quint32 rgb) {
    return QStringLiteral("#%1").arg(rgb & 0xFFFFFFu, 6, 16, QChar('0'));
}

QString legacyHexColor(quint32 rgb) {
    // `§x§R§R§G§G§B§B` —— 六个十六进制位各带一个 `§`，少一个游戏就不认。
    QString out;
    out.append(kSectionSign);
    out.append(QChar('x'));
    const quint32 masked = rgb & 0xFFFFFFu;
    for (int shift = 20; shift >= 0; shift -= 4) {
        const int digit = static_cast<int>((masked >> shift) & 0xFu);
        out.append(kSectionSign);
        out.append(QChar(QStringLiteral("0123456789abcdef").at(digit)));
    }
    return out;
}

quint32 styleRgb(const Style &style) {
    return style.hasCustomColor ? (style.customRgb & 0xFFFFFFu) : colorRgb(style.color);
}

QString styleColorText(const Style &style) {
    if (style.hasCustomColor) {
        return hexColorName(style.customRgb);
    }
    return colorText(style.color);
}

QString colorText(Color color) {
    for (const ColorInfo &info : kColors) {
        if (info.color == color) {
            return QString::fromUtf8(info.text);
        }
    }
    return QStringLiteral("默认");
}

QString styleText(Color color) {
    return colorText(color);
}

QString formatText(QChar code) {
    const QChar lower = code.toLower();
    for (const FormatInfo &info : kFormats) {
        if (info.code == lower) {
            return QString::fromUtf8(info.text);
        }
    }
    if (lower == QChar('r')) {
        return QStringLiteral("复位");
    }
    return QString();
}

QList<Segment> parseLegacy(const QString &text) {
    QList<Segment> segments;
    if (text.isEmpty()) {
        return segments;
    }
    Style style;  // 开始是"默认色 + 都不加"
    QString pending;
    const auto flush = [&segments, &pending, &style]() {
        if (pending.isEmpty()) {
            return;
        }
        // 连着几段同样式就并成一段：`§c红§c字` 本来就是一段，拆成两段只让模型变长。
        if (!segments.isEmpty() && segments.last().style == style) {
            segments.last().text += pending;
        } else {
            segments.append(Segment{pending, style});
        }
        pending.clear();
    };

    for (int index = 0; index < text.size(); ++index) {
        const QChar current = text.at(index);
        if (current != kSectionSign) {
            pending.append(current);
            continue;
        }
        // 收尾那个孤零零的 `§`、或者 `§x` 这种不是码的：**原样留着**。
        // NOTE: `x` 不是十六色也不是格式码，但它是 1.16 自定颜色那个 `§x§R§R§G§G§B§B`
        // 的开头 —— 所以它得放进这道门里，再由下面那段确认后面六个是不是真接得上。
        if (index + 1 >= text.size()
            || (!isAnyCode(text.at(index + 1)) && text.at(index + 1).toLower() != QChar('x'))) {
            pending.append(current);
            continue;
        }
        const QChar code = text.at(index + 1).toLower();
        ++index;  // 吃掉码位那个字符
        if (code == QChar('x')) {
            // `§x` 后面必须是六个 `§<hex>`；少一个就整串**原样留着**（不猜、不吃掉半截）。
            quint32 rgb = 0;
            bool complete = index + 12 < text.size();
            for (int pair = 0; complete && pair < 6; ++pair) {
                const int signAt = index + 1 + pair * 2;
                const QChar digit = text.at(signAt + 1);
                const int value = hexDigit(digit);
                if (text.at(signAt) != kSectionSign || value < 0) {
                    complete = false;
                    break;
                }
                rgb = (rgb << 4) | static_cast<quint32>(value);
            }
            if (!complete) {
                pending.append(current);
                pending.append(text.at(index));  // 原样那个 `x`（大小写照抄）
                continue;
            }
            flush();
            // 颜色码会把格式位一起清掉（和十六色那几条一个规矩）。
            style = Style{};
            style.hasCustomColor = true;
            style.customRgb = rgb;
            index += 12;
            continue;
        }
        if (isColorCode(code)) {
            flush();
            // 颜色码**把格式位一起清掉**（游戏就是这么读的）—— `§a§lb§cc` 里那个 c 不粗。
            style = Style{};
            style.color = colorFromCode(code);
            continue;
        }
        if (code == QChar('r')) {
            flush();
            style = Style{};
            continue;
        }
        flush();
        for (int bit = 0; bit < 5; ++bit) {
            if (kFormats[bit].code == code) {
                setFormatFlag(style, bit, true);
            }
        }
    }
    flush();
    return segments;
}

QString toLegacy(const QList<Segment> &segments) {
    QString out;
    Style running;  // 已经写出去的那些码拼出来的当前状态
    for (const Segment &segment : segments) {
        if (segment.text.isEmpty()) {
            continue;
        }
        if (segment.style.hasCustomColor) {
            out.append(legacyHexColor(segment.style.customRgb));
            running = Style{};
            running.hasCustomColor = true;
            running.customRgb = segment.style.customRgb & 0xFFFFFFu;
        } else if (segment.style.color != Color::Default) {
            // 每段重开一遍颜色：颜色码自己就把格式位清了，所以下面那几个位是从零开始加的。
            out.append(kSectionSign);
            out.append(codeForColor(segment.style.color));
            running = Style{};
            running.color = segment.style.color;
        } else if (running.color != Color::Default || !running.isPlain()) {
            // 回默认色没有"默认色那个码"，只有 `§r`。
            out.append(kSectionSign);
            out.append(QChar('r'));
            running = Style{};
        }
        for (int bit = 0; bit < 5; ++bit) {
            if (!formatFlag(segment.style, bit)) {
                continue;
            }
            out.append(kSectionSign);
            out.append(kFormats[bit].code);
            setFormatFlag(running, bit, true);
        }
        out.append(segment.text);
    }
    // 收尾：还有样式挂着就复位一次，免得后面粘上去的正文被顺带染色。
    if (running.color != Color::Default || !running.isPlain()) {
        out.append(kSectionSign);
        out.append(QChar('r'));
    }
    return out;
}

QJsonArray toJson(const QList<Segment> &segments) {
    QJsonArray array;
    for (const Segment &segment : segments) {
        QJsonObject object;
        object.insert(QStringLiteral("text"), segment.text);
        if (segment.style.hasCustomColor) {
            object.insert(QStringLiteral("color"), hexColorName(segment.style.customRgb));
        } else if (segment.style.color != Color::Default) {
            object.insert(QStringLiteral("color"), colorName(segment.style.color));
        }
        // 只写**开着**的那几位：全写出来读起来全是 false，噪音太大。
        for (int bit = 0; bit < 5; ++bit) {
            if (formatFlag(segment.style, bit)) {
                object.insert(QString::fromLatin1(kFormats[bit].name), true);
            }
        }
        array.append(object);
    }
    return array;
}

QString toJsonText(const QList<Segment> &segments) {
    const QJsonDocument document(toJson(segments));
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

QString toTellrawCommand(const QList<Segment> &segments, const QString &target, bool leadingSlash) {
    QString command;
    if (leadingSlash) {
        command.append(QChar('/'));
    }
    command.append(QStringLiteral("tellraw "));
    command.append(target);
    command.append(QChar(' '));
    command.append(toJsonText(segments));
    return command;
}

JsonParseResult parseJson(const QString &jsonText) {
    JsonParseResult result;

    // ---------- 先接住"顶上就是一段光字符串"那一档 ----------
    // 这一档**必须摆在那次 fromJson 前面**：`QJsonDocument` 只装得下对象和数组（它身上
    // 根本没有 isString/toString），标量直接报 IllegalValue —— 摆后面的话早就在
    // "读不动"那一句上返回了。套一层方括号就能读出来：对合法 JSON 来说这只是语法层多
    // 了一层，字符串里的 `]` 在引号里，不受影响。
    const QByteArray trimmed = jsonText.toUtf8().trimmed();
    if (!trimmed.isEmpty() && trimmed.at(0) == '"') {
        QJsonParseError inner{};
        const QJsonDocument wrapped = QJsonDocument::fromJson(QByteArray("[") + trimmed + "]", &inner);
        if (inner.error == QJsonParseError::NoError && wrapped.isArray() && wrapped.array().size() == 1) {
            result.segments.append(Segment{wrapped.array().at(0).toString(), Style{}});
            result.ok = true;
            return result;
        }
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(jsonText.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        result.unsupported.append(QStringLiteral("json"));
        return result;
    }

    // 三种开头都认：整个数组、单独一个组件对象、光一段字符串（`/tellraw @a "你好"`）。
    QJsonArray roots;
    if (document.isArray()) {
        roots = document.array();
    } else if (document.isObject()) {
        roots.append(document.object());
    } else {
        result.unsupported.append(QStringLiteral("json"));
        return result;
    }

    /// 读一个组件。@p base 是"从外面继承下来的样式"（`extra` 里的孩子继承父组件）——
    /// 平铺那一层传的是一份空样式。
    std::function<void(const QJsonValue &, const Style &)> read =
        [&](const QJsonValue &value, const Style &base) {
            if (value.isString()) {
                result.segments.append(Segment{value.toString(), base});
                return;
            }
            if (value.isArray()) {
                for (const QJsonValue &child : value.toArray()) {
                    read(child, base);
                }
                return;
            }
            if (!value.isObject()) {
                result.unsupported.append(QStringLiteral("json"));
                return;
            }
            const QJsonObject object = value.toObject();

            // 这一层认不出来的那几种组件：**不猜**，只把类型名记下来。
            static const char *kUnsupported[] = {"translate", "score", "selector", "keybind", "nbt"};
            for (const char *key : kUnsupported) {
                if (object.contains(QLatin1String(key))) {
                    const QString name = QString::fromLatin1(key);
                    if (!result.unsupported.contains(name)) {
                        result.unsupported.append(name);
                    }
                }
            }

            Style style = base;
            if (object.contains(QStringLiteral("color"))) {
                const QString name = object.value(QStringLiteral("color")).toString().trimmed();
                quint32 rgb = 0;
                if (name.startsWith(QChar('#'))) {
                    if (parseHexColor(name, &rgb)) {
                        style.hasCustomColor = true;
                        style.customRgb = rgb;
                        style.color = Color::Default;  // 两个都写会让"哪个是色"得靠猜
                    } else if (!result.unsupported.contains(QStringLiteral("hex_color"))) {
                        // `#f00` 那种缩写、或者后面跟了别的东西：**不猜**。
                        result.unsupported.append(QStringLiteral("hex_color"));
                    }
                } else {
                    style.color = colorFromName(name);
                }
            }
            for (int bit = 0; bit < 5; ++bit) {
                const QString name = QString::fromLatin1(kFormats[bit].name);
                if (object.value(name).isBool()) {
                    setFormatFlag(style, bit, object.value(name).toBool());
                }
            }
            if (object.contains(QStringLiteral("text"))) {
                result.segments.append(Segment{object.value(QStringLiteral("text")).toString(), style});
            }
            if (object.contains(QStringLiteral("extra"))) {
                read(object.value(QStringLiteral("extra")), style);
            }
        };

    for (const QJsonValue &root : roots) {
        read(root, Style{});
    }
    result.ok = result.unsupported.isEmpty();
    return result;
}

QString toMotd(const QList<Segment> &segments) {
    const QString legacy = toLegacy(segments);
    QString escaped;
    escaped.reserve(legacy.size() * 2);
    for (int index = 0; index < legacy.size(); ++index) {
        const QChar ch = legacy.at(index);
        if (ch == QChar('\\')) {
            escaped.append(QStringLiteral("\\\\"));
        } else if (ch == QChar('\n')) {
            escaped.append(QStringLiteral("\\n"));
        } else if (ch == QChar('\r')) {
            escaped.append(QStringLiteral("\\r"));
        } else if (ch == QChar('\t')) {
            escaped.append(QStringLiteral("\\t"));
        } else if (ch == QChar(' ')) {
            // 开头那串空白会被 Java 的 properties 读法吃掉 —— 只有第一个要转义，
            // 后面那些在值中间，照原样写就是空格。
            escaped.append(escaped.isEmpty() ? QStringLiteral("\\ ") : QStringLiteral(" "));
        } else if (ch.unicode() >= 0x20 && ch.unicode() < 0x7f) {
            escaped.append(ch);
        } else {
            // Java properties 用 ISO-8859-1 读：`§`、汉字都得写成 `\uXXXX` 才不坏。
            // 只把四位十六进制大写：前面那个 `u` 必须是**小写**（Java 只认 `\u`）。
            escaped.append(QStringLiteral("\\u")
                           + QStringLiteral("%1")
                                 .arg(static_cast<uint>(ch.unicode()), 4, 16, QChar('0'))
                                 .toUpper());
        }
    }
    return QStringLiteral("motd=") + escaped;
}

}  // namespace TextFormat
