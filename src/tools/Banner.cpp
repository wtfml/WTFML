/**
 * @file Banner.cpp
 * @brief Banner 的实现：图案表、十六色表、一条叠层计划（见头文件那一段）。
 */

#include "tools/Banner.hpp"
#include <QCoreApplication>

#include "tools/GameVersion.hpp"

#include <QHash>

namespace {

/// 图案表（id / 中文名 / 图案物品 / 从哪一版起）。**次序就是界面下拉框的次序**，
/// id 与中文名以游戏 `data/minecraft/banner_pattern/` 和官方 `zh_cn.json` 为准。
///
/// 材料一栏：`no_item_required` 里的图案不需要额外物品（空串）；其余图案要一件
/// 「旗帜图案」。1.21.2 起砖纹与波纹边改由各自图案物品解锁，材料按目标版本在
/// `materialFor()` 里判，表里仍记公共档。
///
/// 最后一栏（`since`）：空 = 1.14 织布机那批就有的；猪鼻是 1.16，涡流 / 旋风是 1.21。
/// 版本对不上时计划里会提醒一句。
struct PatternRow {
    const char *id;
    const char *name;
    const char *item;
    const char *since;
};

const PatternRow kPatterns[] = {
    {"border", QT_TRANSLATE_NOOP("Banner", "方框边"), "", nullptr},
    {"bricks", QT_TRANSLATE_NOOP("Banner", "砖纹"), "", nullptr},
    {"circle", QT_TRANSLATE_NOOP("Banner", "圆形"), "", nullptr},
    {"creeper", QT_TRANSLATE_NOOP("Banner", "苦力怕盾徽"), QT_TRANSLATE_NOOP("Banner", "苦力怕盾徽旗帜图案"), nullptr},
    {"cross", QT_TRANSLATE_NOOP("Banner", "斜十字"), "", nullptr},
    {"curly_border", QT_TRANSLATE_NOOP("Banner", "波纹边"), "", nullptr},
    {"diagonal_left", QT_TRANSLATE_NOOP("Banner", "右上三角"), "", nullptr},
    {"diagonal_right", QT_TRANSLATE_NOOP("Banner", "左上三角"), "", nullptr},
    {"diagonal_up_left", QT_TRANSLATE_NOOP("Banner", "右下三角"), "", nullptr},
    {"diagonal_up_right", QT_TRANSLATE_NOOP("Banner", "左下三角"), "", nullptr},
    {"flow", QT_TRANSLATE_NOOP("Banner", "涡流"), QT_TRANSLATE_NOOP("Banner", "涡流旗帜图案"), "1.21"},
    {"flower", QT_TRANSLATE_NOOP("Banner", "花朵盾徽"), QT_TRANSLATE_NOOP("Banner", "花朵盾徽旗帜图案"), nullptr},
    {"globe", QT_TRANSLATE_NOOP("Banner", "地球"), QT_TRANSLATE_NOOP("Banner", "地球旗帜图案"), nullptr},
    {"gradient", QT_TRANSLATE_NOOP("Banner", "自上渐淡"), "", nullptr},
    {"gradient_up", QT_TRANSLATE_NOOP("Banner", "自下渐淡"), "", nullptr},
    {"guster", QT_TRANSLATE_NOOP("Banner", "旋风"), QT_TRANSLATE_NOOP("Banner", "旋风旗帜图案"), "1.21"},
    {"half_horizontal", QT_TRANSLATE_NOOP("Banner", "上半方形"), "", nullptr},
    {"half_horizontal_bottom", QT_TRANSLATE_NOOP("Banner", "下半方形"), "", nullptr},
    {"half_vertical", QT_TRANSLATE_NOOP("Banner", "右半方形"), "", nullptr},
    {"half_vertical_right", QT_TRANSLATE_NOOP("Banner", "左半方形"), "", nullptr},
    {"mojang", QT_TRANSLATE_NOOP("Banner", "Mojang徽标"), QT_TRANSLATE_NOOP("Banner", "Mojang徽标旗帜图案"), nullptr},
    {"piglin", QT_TRANSLATE_NOOP("Banner", "猪鼻"), QT_TRANSLATE_NOOP("Banner", "猪鼻旗帜图案"), "1.16"},
    {"rhombus", QT_TRANSLATE_NOOP("Banner", "菱形"), "", nullptr},
    {"skull", QT_TRANSLATE_NOOP("Banner", "头颅盾徽"), QT_TRANSLATE_NOOP("Banner", "头颅盾徽旗帜图案"), nullptr},
    {"small_stripes", QT_TRANSLATE_NOOP("Banner", "竖条纹"), "", nullptr},
    {"square_bottom_left", QT_TRANSLATE_NOOP("Banner", "右底方"), "", nullptr},
    {"square_bottom_right", QT_TRANSLATE_NOOP("Banner", "左底方"), "", nullptr},
    {"square_top_left", QT_TRANSLATE_NOOP("Banner", "右顶方"), "", nullptr},
    {"square_top_right", QT_TRANSLATE_NOOP("Banner", "左顶方"), "", nullptr},
    {"straight_cross", QT_TRANSLATE_NOOP("Banner", "正十字"), "", nullptr},
    {"stripe_bottom", QT_TRANSLATE_NOOP("Banner", "底横条"), "", nullptr},
    {"stripe_center", QT_TRANSLATE_NOOP("Banner", "中竖条"), "", nullptr},
    {"stripe_downleft", QT_TRANSLATE_NOOP("Banner", "左斜条"), "", nullptr},
    {"stripe_downright", QT_TRANSLATE_NOOP("Banner", "右斜条"), "", nullptr},
    {"stripe_left", QT_TRANSLATE_NOOP("Banner", "右竖条"), "", nullptr},
    {"stripe_middle", QT_TRANSLATE_NOOP("Banner", "中横条"), "", nullptr},
    {"stripe_right", QT_TRANSLATE_NOOP("Banner", "左竖条"), "", nullptr},
    {"stripe_top", QT_TRANSLATE_NOOP("Banner", "顶横条"), "", nullptr},
    {"triangle_bottom", QT_TRANSLATE_NOOP("Banner", "底三角"), "", nullptr},
    {"triangle_top", QT_TRANSLATE_NOOP("Banner", "顶三角"), "", nullptr},
    {"triangles_bottom", QT_TRANSLATE_NOOP("Banner", "底波纹"), "", nullptr},
    {"triangles_top", QT_TRANSLATE_NOOP("Banner", "顶波纹"), "", nullptr},
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

/// 这一层在织布机里要额外摆的图案物品；空串 = 不需要。
///
/// 1.21.2 起砖纹与波纹边改由「砖纹旗帜图案」「波纹边旗帜图案」解锁（`pattern_item/` 标签）；
/// 更早的版本里这两种是 `no_item_required`。其余图案的物品不随版本变。
QString materialFor(int index, const QString &version) {
    if (!validPattern(index)) {
        return {};
    }
    const QString id = QString::fromLatin1(kPatterns[index].id);
    if (!version.isEmpty() && GameVersion::availableIn(QStringLiteral("1.21.2"), version)) {
        if (id == QLatin1String("bricks")) {
            return QCoreApplication::translate("Banner", "砖纹旗帜图案");
        }
        if (id == QLatin1String("curly_border")) {
            return QCoreApplication::translate("Banner", "波纹边旗帜图案");
        }
    }
    return QCoreApplication::translate("Banner", kPatterns[index].item);
}

/// 1.20.4 及更早的 `/give` 里，`Pattern` 存的是游戏内部短码（`cross` → `cr`）。
/// `flow` / `guster` 是 1.21 才加的，没有短码（空串）。
QString legacyPatternCode(const QString &id) {
    static const QHash<QString, QString> codes{
        {QStringLiteral("square_bottom_left"), QStringLiteral("bl")},
        {QStringLiteral("square_bottom_right"), QStringLiteral("br")},
        {QStringLiteral("square_top_left"), QStringLiteral("tl")},
        {QStringLiteral("square_top_right"), QStringLiteral("tr")},
        {QStringLiteral("stripe_bottom"), QStringLiteral("bs")},
        {QStringLiteral("stripe_top"), QStringLiteral("ts")},
        {QStringLiteral("stripe_left"), QStringLiteral("ls")},
        {QStringLiteral("stripe_right"), QStringLiteral("rs")},
        {QStringLiteral("stripe_center"), QStringLiteral("cs")},
        {QStringLiteral("stripe_middle"), QStringLiteral("ms")},
        {QStringLiteral("stripe_downright"), QStringLiteral("drs")},
        {QStringLiteral("stripe_downleft"), QStringLiteral("dls")},
        {QStringLiteral("small_stripes"), QStringLiteral("ss")},
        {QStringLiteral("cross"), QStringLiteral("cr")},
        {QStringLiteral("straight_cross"), QStringLiteral("sc")},
        {QStringLiteral("triangle_bottom"), QStringLiteral("bt")},
        {QStringLiteral("triangle_top"), QStringLiteral("tt")},
        {QStringLiteral("triangles_bottom"), QStringLiteral("bts")},
        {QStringLiteral("triangles_top"), QStringLiteral("tts")},
        {QStringLiteral("diagonal_left"), QStringLiteral("ld")},
        {QStringLiteral("diagonal_up_right"), QStringLiteral("rd")},
        {QStringLiteral("diagonal_up_left"), QStringLiteral("lud")},
        {QStringLiteral("diagonal_right"), QStringLiteral("rud")},
        {QStringLiteral("circle"), QStringLiteral("mc")},
        {QStringLiteral("rhombus"), QStringLiteral("mr")},
        {QStringLiteral("half_vertical"), QStringLiteral("vh")},
        {QStringLiteral("half_horizontal"), QStringLiteral("hh")},
        {QStringLiteral("half_vertical_right"), QStringLiteral("vhr")},
        {QStringLiteral("half_horizontal_bottom"), QStringLiteral("hhb")},
        {QStringLiteral("border"), QStringLiteral("bo")},
        {QStringLiteral("curly_border"), QStringLiteral("cbo")},
        {QStringLiteral("gradient"), QStringLiteral("gra")},
        {QStringLiteral("gradient_up"), QStringLiteral("gru")},
        {QStringLiteral("bricks"), QStringLiteral("bri")},
        {QStringLiteral("globe"), QStringLiteral("glb")},
        {QStringLiteral("creeper"), QStringLiteral("cre")},
        {QStringLiteral("skull"), QStringLiteral("sku")},
        {QStringLiteral("flower"), QStringLiteral("flo")},
        {QStringLiteral("mojang"), QStringLiteral("moj")},
        {QStringLiteral("piglin"), QStringLiteral("pig")},
    };
    return codes.value(id);
}

/// 材料清单按"第一次出现"的次序累计（同一件东西合并成一行 `× N`）。
class MaterialList {
public:
    void add(const QString &name, int count = 1) {
        if (name.isEmpty() || count <= 0) {
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
    /// 图案物品在织布机里不消耗：同一种只记 1 件。
    void addOnce(const QString &name) {
        if (name.isEmpty() || m_counts.contains(name)) {
            return;
        }
        m_order.append(name);
        m_counts.insert(name, 1);
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
    // 旧版本存档里的 id（这一版改名前写出去的）按官方 id 认，不能丢图层。
    static const QHash<QString, QString> aliases{
        {QStringLiteral("bordure"), QStringLiteral("border")},
        {QStringLiteral("bordure_indented"), QStringLiteral("curly_border")},
        {QStringLiteral("creeper_charge"), QStringLiteral("creeper")},
        {QStringLiteral("field_masoned"), QStringLiteral("bricks")},
        {QStringLiteral("flower_charge"), QStringLiteral("flower")},
        {QStringLiteral("skull_charge"), QStringLiteral("skull")},
        {QStringLiteral("thing"), QStringLiteral("mojang")},
        {QStringLiteral("snout"), QStringLiteral("piglin")},
        {QStringLiteral("stripe_small"), QStringLiteral("small_stripes")},
    };
    const QString canonical = aliases.value(id, id);
    for (int index = 0; index < kPatternCount; ++index) {
        if (canonical == QLatin1String(kPatterns[index].id)) {
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
        const QString material = materialFor(layers[index].patternIndex, version);
        const QString step = material.isEmpty()
            ? QCoreApplication::translate("Banner", "第 %1 层：将%2与%3放入织布机，选择图案「%4」")
                  .arg(index + 1)
                  .arg(banner, dye, pattern.name)
            : QCoreApplication::translate("Banner", "第 %1 层：将%2、%3与%4放入织布机，选择图案「%5」")
                  .arg(index + 1)
                  .arg(banner, dye, material, pattern.name);
        result.steps.append(step);
        materials.add(dye, 1);
        materials.addOnce(material);
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
        QStringList missingLegacy;
        for (const Layer &layer : layers) {
            const PatternInfo pattern = patternAt(layer.patternIndex);
            const QString code = legacyPatternCode(pattern.id);
            if (code.isEmpty()) {
                missingLegacy.append(pattern.name);
                continue;
            }
            patternTags.append(QStringLiteral("{Pattern:\"%1\",Color:%2}")
                                   .arg(code)
                                   .arg(layer.colorIndex));
        }
        if (!missingLegacy.isEmpty()) {
            result.command.clear();
            for (const QString &name : missingLegacy) {
                result.notes.append(QCoreApplication::translate(
                    "Banner", "图案「%1」没有 1.20.4 及更早版本的指令写法。").arg(name));
            }
        } else {
            result.command = QStringLiteral("/give @p minecraft:%1_banner%2")
                                 .arg(colorId(baseColorIndex),
                                      patternTags.isEmpty()
                                          ? QString()
                                          : QStringLiteral("{BlockEntityTag:{Patterns:[%1]}}")
                                                .arg(patternTags.join(QChar(','))));
        }
    }

    result.summary = QCoreApplication::translate("Banner", "底色 %1 · %2/%3 层").arg(baseName).arg(layers.size()).arg(kMaxLayers);
    result.materials = materials.rows();
    result.ok = true;
    return result;
}

QString Plan::detailText() const {
    if (!ok) {
        return error.isEmpty() ? QCoreApplication::translate("Banner", "无法生成方案。") : error;
    }
    QStringList lines;
    lines.append(summary);
    lines.append(QString());
    lines.append(QCoreApplication::translate("Banner", "步骤："));
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
        lines.append(QCoreApplication::translate("Banner", "指令（写法随目标版本变化）："));
        lines.append(command);
    }
    return lines.join(QChar('\n'));
}

}  // namespace Banner
