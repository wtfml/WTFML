/**
 * @file Banner.cpp
 * @brief Banner 的实现：图案表、十六色表、一条叠层计划（见头文件那一段）。
 */

#include "tools/Banner.hpp"
#include <QCoreApplication>

#include "tools/GameVersion.hpp"

#include <QHash>

namespace {

/// 图案表（id / 中文名 / 织布机里要摆的材料 / 从哪一版起）。**次序就是界面下拉框的次序**。
///
/// 材料一栏：原版里大多数图案用「旗帜图案」这件物品（按图案分种类），只有下面这几种
/// 用别的物品 —— 表里写死，界面不再猜。
///
/// 最后一栏（`since`）：空 = 1.14 织布机那批就有的。**只给 1.21 才加的那四种写死**
/// （涡流 / 狂风是试炼密室那批；砖墙纹 / 波纹边框是跟基岩版对齐时进 Java 的）——
/// 版本对不上时游戏里根本找不到这几样，计划里得提醒一句。
struct PatternRow {
    const char *id;
    const char *name;
    const char *item;
    const char *since;
};

const PatternRow kPatterns[] = {
    {"bordure", QT_TRANSLATE_NOOP("Banner", "方框边"), QT_TRANSLATE_NOOP("Banner", "砖块"), nullptr},
    // 1.21.2 added the pattern ITEMS, not these already-existing loom designs:
    // https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-2
    {"bordure_indented", QT_TRANSLATE_NOOP("Banner", "波纹边框"), QT_TRANSLATE_NOOP("Banner", "藤蔓"), nullptr},
    {"circle", QT_TRANSLATE_NOOP("Banner", "圆形"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"creeper_charge", QT_TRANSLATE_NOOP("Banner", "苦力怕盾徽"), QT_TRANSLATE_NOOP("Banner", "苦力怕头颅"), nullptr},
    {"cross", QT_TRANSLATE_NOOP("Banner", "斜十字"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"curly_border", QT_TRANSLATE_NOOP("Banner", "波纹边"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"field_masoned", QT_TRANSLATE_NOOP("Banner", "砖纹"), QT_TRANSLATE_NOOP("Banner", "砖块"), nullptr},
    {"flower_charge", QT_TRANSLATE_NOOP("Banner", "花朵盾徽"), QT_TRANSLATE_NOOP("Banner", "滨菊"), nullptr},
    {"flow", QT_TRANSLATE_NOOP("Banner", "涡流"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), "1.21"},
    {"globe", QT_TRANSLATE_NOOP("Banner", "地球"), QT_TRANSLATE_NOOP("Banner", "橡树树苗"), nullptr},
    {"gradient", QT_TRANSLATE_NOOP("Banner", "渐变"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"gradient_up", QT_TRANSLATE_NOOP("Banner", "上渐变"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"guster", QT_TRANSLATE_NOOP("Banner", "旋风"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), "1.21"},
    {"half_horizontal", QT_TRANSLATE_NOOP("Banner", "上半"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"half_horizontal_bottom", QT_TRANSLATE_NOOP("Banner", "下半"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"half_vertical", QT_TRANSLATE_NOOP("Banner", "左半"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"half_vertical_right", QT_TRANSLATE_NOOP("Banner", "右半"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"mojang", QT_TRANSLATE_NOOP("Banner", "Mojang徽标"), QT_TRANSLATE_NOOP("Banner", "附魔金苹果"), nullptr},
    {"piglin", QT_TRANSLATE_NOOP("Banner", "猪灵"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), "1.16"},
    {"rhombus", QT_TRANSLATE_NOOP("Banner", "菱形"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"skull_charge", QT_TRANSLATE_NOOP("Banner", "头颅盾徽"), QT_TRANSLATE_NOOP("Banner", "凋灵骷髅头颅"), nullptr},
    {"snout", QT_TRANSLATE_NOOP("Banner", "猪鼻"), QT_TRANSLATE_NOOP("Banner", "金胡萝卜"), "1.16"},
    {"straight_cross", QT_TRANSLATE_NOOP("Banner", "正十字"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_bottom", QT_TRANSLATE_NOOP("Banner", "底横条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_center", QT_TRANSLATE_NOOP("Banner", "中竖条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_downleft", QT_TRANSLATE_NOOP("Banner", "左下斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_downright", QT_TRANSLATE_NOOP("Banner", "右下斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_left", QT_TRANSLATE_NOOP("Banner", "左侧竖条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_middle", QT_TRANSLATE_NOOP("Banner", "中横条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_right", QT_TRANSLATE_NOOP("Banner", "右侧竖条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_small", QT_TRANSLATE_NOOP("Banner", "细横条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"stripe_top", QT_TRANSLATE_NOOP("Banner", "顶横条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"square_bottom_left", QT_TRANSLATE_NOOP("Banner", "左下角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"square_bottom_right", QT_TRANSLATE_NOOP("Banner", "右下角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"square_top_left", QT_TRANSLATE_NOOP("Banner", "左上角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"square_top_right", QT_TRANSLATE_NOOP("Banner", "右上角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"thing", QT_TRANSLATE_NOOP("Banner", "神秘图案"), QT_TRANSLATE_NOOP("Banner", "附魔金苹果"), nullptr},
    {"triangle_bottom", QT_TRANSLATE_NOOP("Banner", "底三角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"triangle_top", QT_TRANSLATE_NOOP("Banner", "顶三角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"triangles_bottom", QT_TRANSLATE_NOOP("Banner", "底双三角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"triangles_top", QT_TRANSLATE_NOOP("Banner", "顶双三角"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"diagonal_left", QT_TRANSLATE_NOOP("Banner", "左斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"diagonal_right", QT_TRANSLATE_NOOP("Banner", "右斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"diagonal_up_left", QT_TRANSLATE_NOOP("Banner", "左上斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
    {"diagonal_up_right", QT_TRANSLATE_NOOP("Banner", "右上斜条"), QT_TRANSLATE_NOOP("Banner", "旗帜图案"), nullptr},
};

/// 十六色（id / 中文名）。**次序和游戏里染料、羊毛的次序一致**（0 = 白 … 15 = 黑）。
struct ColorRow {
    const char *id;
    const char *name;
};

const ColorRow kColors[] = {
    {"white", QT_TRANSLATE_NOOP("Banner", "白色")},       {"orange", QT_TRANSLATE_NOOP("Banner", "橙色")},     {"magenta", QT_TRANSLATE_NOOP("Banner", "品红色")},
    {"light_blue", QT_TRANSLATE_NOOP("Banner", "淡蓝色")}, {"yellow", QT_TRANSLATE_NOOP("Banner", "黄色")},     {"lime", QT_TRANSLATE_NOOP("Banner", "黄绿色")},
    {"pink", QT_TRANSLATE_NOOP("Banner", "粉红色")},      {"gray", QT_TRANSLATE_NOOP("Banner", "灰色")},       {"light_gray", QT_TRANSLATE_NOOP("Banner", "淡灰色")},
    {"cyan", QT_TRANSLATE_NOOP("Banner", "青色")},        {"purple", QT_TRANSLATE_NOOP("Banner", "紫色")},     {"blue", QT_TRANSLATE_NOOP("Banner", "蓝色")},
    {"brown", QT_TRANSLATE_NOOP("Banner", "棕色")},       {"green", QT_TRANSLATE_NOOP("Banner", "绿色")},      {"red", QT_TRANSLATE_NOOP("Banner", "红色")},
    {"black", QT_TRANSLATE_NOOP("Banner", "黑色")},
};

constexpr int kPatternCount = int(sizeof(kPatterns) / sizeof(kPatterns[0]));
constexpr int kColorCount = int(sizeof(kColors) / sizeof(kColors[0]));

bool validColor(int index) { return index >= 0 && index < kColorCount; }
bool validPattern(int index) { return index >= 0 && index < kPatternCount; }

/// 材料清单按"第一次出现"的次序累计（同一件东西合并成一行 `× N`）。
class MaterialList {
public:
    void add(const QString &name, int count = 1) {
        if (count <= 0) {
            return;
        }
        const auto it = m_counts.find(name);
        if (it == m_counts.end()) {
            m_order.append(name);
            m_counts.insert(name, count);
        } else {
            it.value() += count;
        }
    }
    QStringList rows() const {
        QStringList list;
        for (const QString &name : m_order) {
            list.append(QStringLiteral("%1 × %2").arg(name).arg(m_counts.value(name)));
        }
        return list;
    }

private:
    QStringList m_order;
    QHash<QString, int> m_counts;
};

}  // namespace

namespace Banner {

int patternCount() { return kPatternCount; }

PatternInfo patternAt(int index) {
    if (!validPattern(index)) {
        return {};
    }
    return {QString::fromLatin1(kPatterns[index].id), QCoreApplication::translate("Banner", kPatterns[index].name),
            QCoreApplication::translate("Banner", kPatterns[index].item),
            kPatterns[index].since != nullptr ? QString::fromLatin1(kPatterns[index].since)
                                              : QStringLiteral("1.14")};
}

QStringList patternNames() {
    QStringList list;
    for (const PatternRow &row : kPatterns) {
        list.append(QCoreApplication::translate("Banner", row.name));
    }
    return list;
}

int patternIndexById(const QString &id) {
    for (int index = 0; index < kPatternCount; ++index) {
        if (id == QLatin1String(kPatterns[index].id)) {
            return index;
        }
    }
    return -1;
}

int colorCount() { return kColorCount; }

QString patternSince(int index) {
    return validPattern(index) ? patternAt(index).since : QString();
}

QString colorName(int index) {
    if (!validColor(index)) {
        return {};
    }
    return QCoreApplication::translate("Banner", kColors[index].name);
}

QString colorId(int index) {
    if (!validColor(index)) {
        return {};
    }
    return QString::fromLatin1(kColors[index].id);
}

QString dyeName(int index) {
    if (!validColor(index)) {
        return {};
    }
    return QCoreApplication::translate("Banner", "%1染料").arg(colorName(index));
}

Plan plan(int baseColorIndex, const QList<Layer> &layers, const QString &version) {
    Plan result;
    if (!validColor(baseColorIndex)) {
        result.error = QCoreApplication::translate("Banner", "底色编号无效（%1）。请从 16 种颜色中选择。")
                           .arg(baseColorIndex);
        return result;
    }
    if (layers.size() > kMaxLayers) {
        result.error = QCoreApplication::translate("Banner", "原版织布机最多支持 %1 层，当前有 %2 层。请移除多余图层。")
                           .arg(kMaxLayers)
                           .arg(layers.size());
        return result;
    }
    for (int index = 0; index < layers.size(); ++index) {
        if (!validPattern(layers[index].patternIndex)) {
            result.error = QCoreApplication::translate("Banner", "第 %1 层的图案编号无效（%2）。")
                               .arg(index + 1)
                               .arg(layers[index].patternIndex);
            return result;
        }
        if (!validColor(layers[index].colorIndex)) {
            result.error = QCoreApplication::translate("Banner", "第 %1 层的颜色编号无效（%2）。")
                               .arg(index + 1)
                               .arg(layers[index].colorIndex);
            return result;
        }
    }

    const QString baseName = colorName(baseColorIndex);
    MaterialList materials;

    QString previousKey;
    QStringList newerPatterns;  // 1.21 才加的那几种（同一图案只提醒一次）
    for (int index = 0; index < layers.size(); ++index) {
        const PatternInfo pattern = patternAt(layers[index].patternIndex);
        const QString dye = dyeName(layers[index].colorIndex);
        const QString banner = index == 0 ? QCoreApplication::translate("Banner", "%1空白旗帜").arg(baseName)
                                          : QCoreApplication::translate("Banner", "上一层的旗帜");
        result.steps.append(QCoreApplication::translate("Banner", "第 %1 层：织布机里放%2 + %3 + %4%5 → 叠出「%6」")
                                .arg(index + 1)
                                .arg(banner, dye, pattern.name, pattern.item)
                                .arg(pattern.name));
        materials.add(dye, 1);
        materials.add(pattern.item, 1);
        if (pattern.since != QStringLiteral("1.14") && !newerPatterns.contains(pattern.name)) {
            newerPatterns.append(pattern.name);
            result.notes.append(QCoreApplication::translate("Banner", "图案「%1」自 %2 版起可用。")
                                    .arg(pattern.name, pattern.since));
        }

        const QString key = QStringLiteral("%1|%2")
                                .arg(layers[index].patternIndex)
                                .arg(layers[index].colorIndex);
        if (index > 0 && key == previousKey) {
            result.notes.append(QCoreApplication::translate("Banner", "第 %1 层和第 %2 层图案及颜色相同，显示效果与单层相同。")
                                    .arg(index + 1)
                                    .arg(index));
        }
        previousKey = key;
    }

    // 指令那一行只在 6 层以内（这儿已经过闸）。
    //
    // 写法按目标版本分两套：1.20.5 起 NBT 那串不再被 `/give` 接受，图案改走物品组件
    // （`[minecraft:banner_patterns=[{pattern:"minecraft:cross",color:"white"}]]`）。
    // 「不限版本」（空串）照老写法给 —— 与这个参数加进来之前逐字相同。
    const bool componentSyntax =
        !version.isEmpty()
        && GameVersion::availableIn(QStringLiteral("1.20.5"), version);
    if (componentSyntax) {
        QStringList patternEntries;
        for (const Layer &layer : layers) {
            patternEntries.append(QStringLiteral("{pattern:\"minecraft:%1\",color:\"%2\"}")
                                      .arg(patternAt(layer.patternIndex).id,
                                           colorId(layer.colorIndex)));
        }
        result.command = QStringLiteral("/give @p minecraft:%1_banner%2")
                             .arg(colorId(baseColorIndex),
                                  patternEntries.isEmpty()
                                      ? QString()
                                      : QStringLiteral("[minecraft:banner_patterns=[%1]]")
                                            .arg(patternEntries.join(QChar(','))));
    } else {
        QStringList patternTags;
        for (const Layer &layer : layers) {
            patternTags.append(QStringLiteral("{Pattern:\"%1\",Color:%2}")
                                   .arg(patternAt(layer.patternIndex).id)
                                   .arg(layer.colorIndex));
        }
        result.command = QStringLiteral("/give @p minecraft:%1_banner%2")
                             .arg(colorId(baseColorIndex),
                                  patternTags.isEmpty()
                                      ? QString()
                                      : QStringLiteral("{BlockEntityTag:{Patterns:[%1]}}")
                                            .arg(patternTags.join(QChar(','))));
    }

    result.summary = QCoreApplication::translate("Banner", "底色 %1 · %2/%3 层").arg(baseName).arg(layers.size()).arg(kMaxLayers);
    result.materials = materials.rows();
    result.ok = true;
    return result;
}

QString Plan::detailText() const {
    if (!ok) {
        return error.isEmpty() ? QCoreApplication::translate("Banner", "排不出来。") : error;
    }
    QStringList lines;
    lines.append(summary);
    lines.append(QString());
    lines.append(QCoreApplication::translate("Banner", "一步一步："));
    for (const QString &step : steps) {
        lines.append(QStringLiteral("• %1").arg(step));
    }
    if (!materials.isEmpty()) {
        lines.append(QString());
        lines.append(QCoreApplication::translate("Banner", "材料清单："));
        for (const QString &row : materials) {
            lines.append(QStringLiteral("• %1").arg(row));
        }
    }
    if (!notes.isEmpty()) {
        lines.append(QString());
        for (const QString &note : notes) {
            lines.append(QCoreApplication::translate("Banner", "（%1）").arg(note));
        }
    }
    if (!command.isEmpty()) {
        lines.append(QString());
        lines.append(QCoreApplication::translate("Banner", "指令（1.13–1.20.4 的 NBT 写法；1.20.5 起换成了组件写法，" "这一版不出）："));
        lines.append(command);
    }
    return lines.join(QChar('\n'));
}

}  // namespace Banner
