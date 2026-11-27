/**
 * @file CoordConvert.cpp
 * @brief CoordConvert 的实现（那些规矩在 .hpp 开头那一段）。
 */
#include "CoordConvert.hpp"
#include <QCoreApplication>

#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <cstring>

namespace {

/// 区块 / 区域那一格有多少。
constexpr int kChunkSize = 16;
constexpr int kChunksPerRegion = 32;

/// 一个数看起来是不是坐标（`-3.5` / `1` / `0.0` / `~5` / `~-3.5` / `^1`）。
///
/// `~` 和 `^` 单独出现也是合法的写法（`~` = 当前位置）—— 所以这里连"光一个波浪号"都算。
bool looksLikeNumber(const QString &token) {
    static const QRegularExpression pattern(
        QStringLiteral("^[~^]?-?\\d*(\\.\\d+)?$"));
    return pattern.match(token.trimmed()).hasMatch();
}

/// 这一格是不是相对写法（`~` / `^` 开头）。
bool isRelative(const QString &token) {
    return token.startsWith(QChar('~')) || token.startsWith(QChar('^'));
}

/// 把一个数字格子读成 double。读不动回 false（`~` 那种由调用方先拦掉）。
bool toDouble(const QString &token, double *out) {
    bool ok = false;
    const double value = token.toDouble(&ok);
    if (ok) {
        *out = value;
    }
    return ok;
}

}  // namespace

namespace CoordConvert {

Dimension dimensionFromId(const QString &id) {
    QString name = id.trimmed().toLower();
    if (name.isEmpty()) {
        return Dimension::Unknown;
    }
    // `minecraft:` 那个前缀可有可无，别的地方（数据包、命令）两种写法都见得到。
    if (name.startsWith(QLatin1String("minecraft:"))) {
        name = name.mid(int(std::strlen("minecraft:")));
    }
    if (name == QLatin1String("overworld") || name == QLatin1String("world")) {
        return Dimension::Overworld;
    }
    if (name == QLatin1String("the_nether") || name == QLatin1String("nether")) {
        return Dimension::Nether;
    }
    if (name == QLatin1String("the_end") || name == QLatin1String("end")) {
        return Dimension::End;
    }
    return Dimension::Unknown;
}

QString dimensionId(Dimension dimension) {
    switch (dimension) {
    case Dimension::Overworld:
        return QStringLiteral("minecraft:overworld");
    case Dimension::Nether:
        return QStringLiteral("minecraft:the_nether");
    case Dimension::End:
        return QStringLiteral("minecraft:the_end");
    case Dimension::Unknown:
        break;
    }
    return QString();
}

QString dimensionText(Dimension dimension) {
    switch (dimension) {
    case Dimension::Overworld:
        return QCoreApplication::translate("CoordConvert", "主世界");
    case Dimension::Nether:
        return QCoreApplication::translate("CoordConvert", "下界");
    case Dimension::End:
        return QCoreApplication::translate("CoordConvert", "末地");
    case Dimension::Unknown:
        break;
    }
    return QCoreApplication::translate("CoordConvert", "未知维度");
}

bool convertible(Dimension from, Dimension to) {
    if (from == Dimension::Unknown || to == Dimension::Unknown) {
        return false;
    }
    if (from == Dimension::End || to == Dimension::End) {
        // 末地不与主世界成比例：那儿没有"8:1"这回事。
        return from == to;
    }
    return true;
}

int floorDiv(int value, int divisor) {
    if (divisor == 0) {
        return 0;
    }
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    // 余数不为 0 且两数异号 ⇒ C 的"/"是向零取整的，这里得再退一格才是向下取整。
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

int floorToInt(double value) {
    return int(std::floor(value));
}

Position convert(Dimension from, Dimension to, const Position &position) {
    if (!convertible(from, to) || from == to) {
        return position;
    }
    Position out = position;
    if (from == Dimension::Overworld && to == Dimension::Nether) {
        out.x = position.x / 8.0;
        out.z = position.z / 8.0;
    } else if (from == Dimension::Nether && to == Dimension::Overworld) {
        out.x = position.x * 8.0;
        out.z = position.z * 8.0;
    }
    return out;
}

int blockToChunk(int block) {
    return floorDiv(block, kChunkSize);
}

int chunkToRegion(int chunk) {
    return floorDiv(chunk, kChunksPerRegion);
}

QString regionFileName(int regionX, int regionZ) {
    return QStringLiteral("r.%1.%2.mca").arg(regionX).arg(regionZ);
}

QString blockRangeText(int chunk) {
    const int first = chunk * kChunkSize;
    return QStringLiteral("%1…%2").arg(first).arg(first + kChunkSize - 1);
}

ParsedTp parseTeleportCommand(const QString &text) {
    ParsedTp result;
    QString line = text.trimmed();
    if (line.isEmpty()) {
        result.error = QCoreApplication::translate("CoordConvert", "请粘贴在游戏中按 F3+C 复制的坐标命令。");
        return result;
    }
    if (line.startsWith(QChar('/'))) {
        line = line.mid(1).trimmed();
    }

    // ---------- 维度那一段 ----------
    // `execute in <维度> run …`；没有这一段就只是"没说维度"。
    QStringList tokens = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (!tokens.isEmpty() && tokens.first().compare(QLatin1String("execute"), Qt::CaseInsensitive) == 0) {
        if (tokens.size() < 4 || tokens.at(1).compare(QLatin1String("in"), Qt::CaseInsensitive) != 0) {
            result.error = QCoreApplication::translate("CoordConvert", "仅支持格式为 `execute in <维度> run <命令>` 的 `execute` 命令。");
            return result;
        }
        result.dimension = dimensionFromId(tokens.at(2));
        result.hasDimension = result.dimension != Dimension::Unknown;
        if (!result.hasDimension) {
            result.error = QCoreApplication::translate("CoordConvert", "无法识别维度：`%1`。").arg(tokens.at(2));
            return result;
        }
        // 后面必须跟着 `run`
        int index = 3;
        if (tokens.at(index).compare(QLatin1String("run"), Qt::CaseInsensitive) != 0) {
            result.error = QCoreApplication::translate("CoordConvert", "`execute in <维度>` 后缺少 `run`。");
            return result;
        }
        tokens = tokens.mid(index + 1);
    }

    // ---------- `tp` 那一段 ----------
    if (tokens.isEmpty() || tokens.first().compare(QLatin1String("tp"), Qt::CaseInsensitive) != 0) {
        result.error = QCoreApplication::translate("CoordConvert", "此命令不含 `tp`。仅支持传送命令，F3+C 复制的内容即为传送命令。");
        return result;
    }
    tokens.removeFirst();

    // 选择器那一格（`@s` / `@p` / 玩家名）**有就跳过** —— 判据是"这一格看起来不是坐标"。
    if (!tokens.isEmpty() && !looksLikeNumber(tokens.first())) {
        tokens.removeFirst();
    }

    const int coordinateCount = qMin(3, int(tokens.size()));
    if (coordinateCount < 3) {
        result.error = QCoreApplication::translate("CoordConvert", "坐标不足 3 个：仅读取到 %1 个数。").arg(tokens.size());
        return result;
    }

    double values[3] = {0.0, 0.0, 0.0};
    for (int index = 0; index < 3; ++index) {
        const QString token = tokens.at(index);
        if (isRelative(token)) {
            result.relative = true;
            continue;
        }
        if (!toDouble(token, &values[index])) {
            result.error = QCoreApplication::translate("CoordConvert", "`%1` 不是有效数字。").arg(token);
            return result;
        }
    }
    if (result.relative) {
        result.error = QCoreApplication::translate("CoordConvert", "输入包含 `~` 或 `^` 相对坐标。请站到目标位置后复制绝对坐标。");
        return result;
    }
    result.position = Position{values[0], values[1], values[2]};

    // 朝向角（最后两个数）：有就读上，**不影响位置**。
    if (tokens.size() >= 5) {
        double yaw = 0.0;
        double pitch = 0.0;
        if (toDouble(tokens.at(3), &yaw) && toDouble(tokens.at(4), &pitch)) {
            result.hasRotation = true;
            result.yaw = yaw;
            result.pitch = pitch;
        }
    }
    result.ok = true;
    return result;
}

QString toTeleportCommand(const Position &position, Dimension dimension) {
    QString command = QStringLiteral("/tp @s %1 %2 %3")
                          .arg(position.x, 0, 'f', 2)
                          .arg(position.y, 0, 'f', 2)
                          .arg(position.z, 0, 'f', 2);
    if (dimension != Dimension::Unknown) {
        command = QStringLiteral("/execute in %1 run ").arg(dimensionId(dimension)) + command.mid(1);
    }
    return command;
}

QString convertedTeleportCommand(Dimension from, Dimension to, const Position &position) {
    return toTeleportCommand(convert(from, to, position), to);
}

QString positionText(const Position &position) {
    return QStringLiteral("%1 %2 %3")
        .arg(position.x, 0, 'f', 2)
        .arg(position.y, 0, 'f', 2)
        .arg(position.z, 0, 'f', 2);
}

}  // namespace CoordConvert
