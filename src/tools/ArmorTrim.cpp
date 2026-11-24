/**
 * @file ArmorTrim.cpp
 * @brief ArmorTrim 的实现：纹饰材料表 / 图案表 / 部位与盔甲材质表 / 一条计划。
 */

#include "tools/ArmorTrim.hpp"
#include <QCoreApplication>

#include "tools/GameVersion.hpp"

namespace {

struct TrimMaterialRow {
    const char *id;
    const char *name;
    const char *item;
    const char *since;  ///< 哪个 Java 版本开始有
};

// 名字照游戏 `zh_cn.json` 的 `trim_material.minecraft.*` 写（「紫水晶质」那一列），
// 不是自己另起一套简称；`item` 是锻造台里真正摆进去的那件东西。
const TrimMaterialRow kTrimMaterials[] = {
    {"amethyst", QT_TRANSLATE_NOOP("ArmorTrim", "紫水晶质"), QT_TRANSLATE_NOOP("ArmorTrim", "紫水晶碎片"), "1.20"},
    {"copper", QT_TRANSLATE_NOOP("ArmorTrim", "铜质"), QT_TRANSLATE_NOOP("ArmorTrim", "铜锭"), "1.20"},
    {"diamond", QT_TRANSLATE_NOOP("ArmorTrim", "钻石质"), QT_TRANSLATE_NOOP("ArmorTrim", "钻石"), "1.20"},
    {"emerald", QT_TRANSLATE_NOOP("ArmorTrim", "绿宝石质"), QT_TRANSLATE_NOOP("ArmorTrim", "绿宝石"), "1.20"},
    {"gold", QT_TRANSLATE_NOOP("ArmorTrim", "金质"), QT_TRANSLATE_NOOP("ArmorTrim", "金锭"), "1.20"},
    {"iron", QT_TRANSLATE_NOOP("ArmorTrim", "铁质"), QT_TRANSLATE_NOOP("ArmorTrim", "铁锭"), "1.20"},
    {"lapis", QT_TRANSLATE_NOOP("ArmorTrim", "青金石质"), QT_TRANSLATE_NOOP("ArmorTrim", "青金石"), "1.20"},
    {"netherite", QT_TRANSLATE_NOOP("ArmorTrim", "下界合金质"), QT_TRANSLATE_NOOP("ArmorTrim", "下界合金锭"), "1.20"},
    {"quartz", QT_TRANSLATE_NOOP("ArmorTrim", "石英质"), QT_TRANSLATE_NOOP("ArmorTrim", "石英"), "1.20"},
    {"redstone", QT_TRANSLATE_NOOP("ArmorTrim", "红石质"), QT_TRANSLATE_NOOP("ArmorTrim", "红石粉"), "1.20"},
    {"resin", QT_TRANSLATE_NOOP("ArmorTrim", "树脂质"), QT_TRANSLATE_NOOP("ArmorTrim", "树脂砖"), "1.21.4"},
};

struct TrimPatternRow {
    const char *id;
    const char *name;
    const char *source;    ///< 模板从哪儿来
    const char *duplicate; ///< 复制这枚模板要的那种方块
    const char *since;     ///< 哪个 Java 版本开始有
};

// 名字照游戏 `zh_cn.json` 的 `trim_pattern.minecraft.*` 写（「镶铆盔甲纹饰」那一列），
// 来源按 Java 1.20 / 1.21 的实际箱子写；不写"城堡里某处"这种含糊说法。
// `duplicate` 抄的是游戏自己的复制配方（`data/minecraft/recipe/*_armor_trim_smithing_template.json`
// 里那个 `C` 位），方块名照 `zh_cn.json`；四块陶瓦、四块圆石那几组是游戏原样，不是取整。
const TrimPatternRow kPatterns[] = {
    {"coast", QT_TRANSLATE_NOOP("ArmorTrim", "海岸"), QT_TRANSLATE_NOOP("ArmorTrim", "沉船（藏宝箱）"), QT_TRANSLATE_NOOP("ArmorTrim", "圆石"), "1.20"},
    {"sentry", QT_TRANSLATE_NOOP("ArmorTrim", "哨兵"), QT_TRANSLATE_NOOP("ArmorTrim", "掠夺者前哨站"), QT_TRANSLATE_NOOP("ArmorTrim", "圆石"), "1.20"},
    {"dune", QT_TRANSLATE_NOOP("ArmorTrim", "沙丘"), QT_TRANSLATE_NOOP("ArmorTrim", "沙漠神殿"), QT_TRANSLATE_NOOP("ArmorTrim", "砂岩"), "1.20"},
    {"wild", QT_TRANSLATE_NOOP("ArmorTrim", "荒野"), QT_TRANSLATE_NOOP("ArmorTrim", "丛林神庙"), QT_TRANSLATE_NOOP("ArmorTrim", "苔石"), "1.20"},
    {"ward", QT_TRANSLATE_NOOP("ArmorTrim", "监守"), QT_TRANSLATE_NOOP("ArmorTrim", "远古城市"), QT_TRANSLATE_NOOP("ArmorTrim", "深板岩圆石"), "1.20"},
    {"eye", QT_TRANSLATE_NOOP("ArmorTrim", "眼眸"), QT_TRANSLATE_NOOP("ArmorTrim", "要塞"), QT_TRANSLATE_NOOP("ArmorTrim", "末地石"), "1.20"},
    {"vex", QT_TRANSLATE_NOOP("ArmorTrim", "恼鬼"), QT_TRANSLATE_NOOP("ArmorTrim", "林地府邸"), QT_TRANSLATE_NOOP("ArmorTrim", "圆石"), "1.20"},
    {"tide", QT_TRANSLATE_NOOP("ArmorTrim", "潮汐"), QT_TRANSLATE_NOOP("ArmorTrim", "远古守卫者（击杀掉落）"), QT_TRANSLATE_NOOP("ArmorTrim", "海晶石"), "1.20"},
    {"snout", QT_TRANSLATE_NOOP("ArmorTrim", "猪鼻"), QT_TRANSLATE_NOOP("ArmorTrim", "堡垒遗迹"), QT_TRANSLATE_NOOP("ArmorTrim", "黑石"), "1.20"},
    {"rib", QT_TRANSLATE_NOOP("ArmorTrim", "肋骨"), QT_TRANSLATE_NOOP("ArmorTrim", "下界要塞"), QT_TRANSLATE_NOOP("ArmorTrim", "下界岩"), "1.20"},
    {"spire", QT_TRANSLATE_NOOP("ArmorTrim", "尖塔"), QT_TRANSLATE_NOOP("ArmorTrim", "末地城"), QT_TRANSLATE_NOOP("ArmorTrim", "紫珀块"), "1.20"},
    {"wayfinder", QT_TRANSLATE_NOOP("ArmorTrim", "向导"), QT_TRANSLATE_NOOP("ArmorTrim", "古迹废墟（可疑的沙砾）"), QT_TRANSLATE_NOOP("ArmorTrim", "陶瓦"), "1.20"},
    {"shaper", QT_TRANSLATE_NOOP("ArmorTrim", "塑造"), QT_TRANSLATE_NOOP("ArmorTrim", "古迹废墟（可疑的沙砾）"), QT_TRANSLATE_NOOP("ArmorTrim", "陶瓦"), "1.20"},
    {"silence", QT_TRANSLATE_NOOP("ArmorTrim", "幽静"), QT_TRANSLATE_NOOP("ArmorTrim", "远古城市"), QT_TRANSLATE_NOOP("ArmorTrim", "深板岩圆石"), "1.20"},
    {"raiser", QT_TRANSLATE_NOOP("ArmorTrim", "牧民"), QT_TRANSLATE_NOOP("ArmorTrim", "古迹废墟（可疑的沙砾）"), QT_TRANSLATE_NOOP("ArmorTrim", "陶瓦"), "1.20"},
    {"host", QT_TRANSLATE_NOOP("ArmorTrim", "雇主"), QT_TRANSLATE_NOOP("ArmorTrim", "古迹废墟（可疑的沙砾）"), QT_TRANSLATE_NOOP("ArmorTrim", "陶瓦"), "1.20"},
    {"flow", QT_TRANSLATE_NOOP("ArmorTrim", "涡流"), QT_TRANSLATE_NOOP("ArmorTrim", "试炼密室（不祥宝库）"), QT_TRANSLATE_NOOP("ArmorTrim", "旋风棒"), "1.21"},
    {"bolt", QT_TRANSLATE_NOOP("ArmorTrim", "镶铆"), QT_TRANSLATE_NOOP("ArmorTrim", "试炼密室（宝库）"), QT_TRANSLATE_NOOP("ArmorTrim", "铜块"), "1.21"},
};

const char *kParts[] = {QT_TRANSLATE_NOOP("ArmorTrim", "头盔"), QT_TRANSLATE_NOOP("ArmorTrim", "胸甲"), QT_TRANSLATE_NOOP("ArmorTrim", "护腿"), QT_TRANSLATE_NOOP("ArmorTrim", "靴子")};

/// 盔甲材质：id / 中文名 / 全名里那个QT_TRANSLATE_NOOP("ArmorTrim", "前缀")。
struct ArmorMaterialRow {
    const char *id;
    const char *name;
};

const ArmorMaterialRow kArmorMaterials[] = {
    {"leather", QT_TRANSLATE_NOOP("ArmorTrim", "皮革")},   {"chainmail", QT_TRANSLATE_NOOP("ArmorTrim", "锁链")}, {"iron", QT_TRANSLATE_NOOP("ArmorTrim", "铁")},
    {"gold", QT_TRANSLATE_NOOP("ArmorTrim", "金")},        {"diamond", QT_TRANSLATE_NOOP("ArmorTrim", "钻石")},   {"netherite", QT_TRANSLATE_NOOP("ArmorTrim", "下界合金")},
    {"turtle", QT_TRANSLATE_NOOP("ArmorTrim", "海龟壳")},
};

constexpr int kTrimMaterialCount = int(sizeof(kTrimMaterials) / sizeof(kTrimMaterials[0]));
constexpr int kPatternCount = int(sizeof(kPatterns) / sizeof(kPatterns[0]));
constexpr int kPartCount = int(sizeof(kParts) / sizeof(kParts[0]));
constexpr int kArmorMaterialCount =
    int(sizeof(kArmorMaterials) / sizeof(kArmorMaterials[0]));

/// 海龟壳那个材质的下标（**只认头盔**那一条判据就靠它）。
constexpr int kTurtleShell = kArmorMaterialCount - 1;

bool validTrimMaterial(int index) { return index >= 0 && index < kTrimMaterialCount; }
bool validPattern(int index) { return index >= 0 && index < kPatternCount; }
bool validPart(int index) { return index >= 0 && index < kPartCount; }
bool validArmorMaterial(int index) { return index >= 0 && index < kArmorMaterialCount; }

}  // namespace

namespace ArmorTrim {

int trimMaterialCount() { return kTrimMaterialCount; }

TrimMaterial trimMaterialAt(int index) {
    if (!validTrimMaterial(index)) {
        return {};
    }
    return {QString::fromLatin1(kTrimMaterials[index].id),
            QCoreApplication::translate("ArmorTrim", kTrimMaterials[index].name),
            QCoreApplication::translate("ArmorTrim", kTrimMaterials[index].item)};
}

QString trimMaterialSince(int index) {
    return validTrimMaterial(index) ? QString::fromLatin1(kTrimMaterials[index].since) : QString();
}

bool trimMaterialAvailable(int index, const QString &version) {
    return validTrimMaterial(index) &&
           GameVersion::availableIn(QString::fromLatin1(kTrimMaterials[index].since), version);
}

QStringList trimMaterialNames() {
    QStringList list;
    for (const TrimMaterialRow &row : kTrimMaterials) {
        list.append(QCoreApplication::translate("ArmorTrim", row.name));
    }
    return list;
}

int patternCount() { return kPatternCount; }

TrimPattern patternAt(int index) {
    if (!validPattern(index)) {
        return {};
    }
    return {QString::fromLatin1(kPatterns[index].id), QCoreApplication::translate("ArmorTrim", kPatterns[index].name),
            QCoreApplication::translate("ArmorTrim", kPatterns[index].source),
            QCoreApplication::translate("ArmorTrim", kPatterns[index].duplicate),
            QString::fromLatin1(kPatterns[index].since)};
}

QString patternSource(int index) {
    return validPattern(index) ? QCoreApplication::translate("ArmorTrim", kPatterns[index].source) : QString();
}

QString patternDuplicate(int index) {
    return validPattern(index) ? QCoreApplication::translate("ArmorTrim", kPatterns[index].duplicate) : QString();
}

QStringList patternNames() {
    QStringList list;
    for (const TrimPatternRow &row : kPatterns) {
        list.append(QCoreApplication::translate("ArmorTrim", row.name));
    }
    return list;
}

QString patternSince(int index) {
    return validPattern(index) ? QString::fromLatin1(kPatterns[index].since) : QString();
}

bool patternAvailable(int index, const QString &version) {
    return validPattern(index) &&
           GameVersion::availableIn(QString::fromLatin1(kPatterns[index].since), version);
}

int partCount() { return kPartCount; }

QString partName(int index) {
    if (!validPart(index)) {
        return {};
    }
    return QCoreApplication::translate("ArmorTrim", kParts[index]);
}

int armorMaterialCount() { return kArmorMaterialCount; }

QString armorMaterialId(int index) {
    return validArmorMaterial(index) ? QString::fromLatin1(kArmorMaterials[index].id) : QString();
}

QString armorMaterialName(int index) {
    if (!validArmorMaterial(index)) {
        return {};
    }
    return QCoreApplication::translate("ArmorTrim", kArmorMaterials[index].name);
}

QString armorPieceName(int armorMaterial, int part) {
    if (!validArmorMaterial(armorMaterial) || !validPart(part)) {
        return {};
    }
    if (armorMaterial == kTurtleShell) {
        return QCoreApplication::translate("ArmorTrim", "海龟壳");
    }
    return QStringLiteral("%1%2").arg(armorMaterialName(armorMaterial), partName(part));
}

bool armorMaterialFitsPart(int armorMaterial, int part) {
    if (!validArmorMaterial(armorMaterial) || !validPart(part)) {
        return false;
    }
    if (armorMaterial == kTurtleShell) {
        return part == 0;  // 只有头盔
    }
    return true;
}

Plan plan(int armorMaterial, int part, int patternIndex, int trimMaterial, int pieces,
          bool wholeSet) {
    Plan result;
    if (!validArmorMaterial(armorMaterial)) {
        result.error = QCoreApplication::translate("ArmorTrim", "盔甲材质编号无效（%1）。").arg(armorMaterial);
        return result;
    }
    if (!validPattern(patternIndex)) {
        result.error = QCoreApplication::translate("ArmorTrim", "纹饰图案编号无效（%1）。").arg(patternIndex);
        return result;
    }
    if (!validTrimMaterial(trimMaterial)) {
        result.error = QCoreApplication::translate("ArmorTrim", "纹饰材料编号无效（%1）。").arg(trimMaterial);
        return result;
    }
    if (wholeSet) {
        // 整套 = 四个部位各一件；海龟壳只有头盔，做不了整套。
        if (armorMaterial == kTurtleShell) {
            result.error = QCoreApplication::translate("ArmorTrim", "海龟壳只能用作头部装备。");
            return result;
        }
    } else {
        if (!validPart(part)) {
            result.error = QCoreApplication::translate("ArmorTrim", "装备部位编号无效（%1）。").arg(part);
            return result;
        }
        if (pieces < 1 || pieces > 4) {
            result.error = QCoreApplication::translate("ArmorTrim", "装备数量须在 1 至 4 之间，当前为 %1 件。").arg(pieces);
            return result;
        }
        if (!armorMaterialFitsPart(armorMaterial, part)) {
            result.error = QCoreApplication::translate("ArmorTrim", "海龟壳只能用作头部装备，无法用于%1。").arg(partName(part));
            return result;
        }
    }

    const TrimPattern pattern = patternAt(patternIndex);
    const TrimMaterial material = trimMaterialAt(trimMaterial);
    const QString templateName = QCoreApplication::translate("ArmorTrim", "%1纹饰模板").arg(pattern.name);

    // 一句话把"从哪来"和"再要一枚拿什么换"说完 —— 这两样是用户看这页真正要查的。
    // 模板本身和那七颗钻石不必写：前者就是这一枚，后者谁都备得住。
    result.notes.append(QCoreApplication::translate("ArmorTrim", "这枚%1来自：%2；复制用%3。")
                            .arg(templateName, pattern.source, pattern.duplicate));
    if (pattern.since != QStringLiteral("1.20")) {
        result.notes.append(QCoreApplication::translate("ArmorTrim", "%1 纹饰模板自 Java 版 %2 起可用。")
                                .arg(templateName, pattern.since));
    }
    const QString materialSince = trimMaterialSince(trimMaterial);
    if (!materialSince.isEmpty() && materialSince != QStringLiteral("1.20")) {
        result.notes.append(QCoreApplication::translate("ArmorTrim", "「%1」纹饰材料自 Java 版 %2 起可用。")
                                .arg(material.name, materialSince));
    }

    if (wholeSet) {
        // ---------- 整套四件 ----------
        QStringList pieceNames;
        for (int index = 0; index < kPartCount; ++index) {
            pieceNames.append(armorPieceName(armorMaterial, index));
        }
        const QString setLabel = QCoreApplication::translate("ArmorTrim", "%1四件套").arg(armorMaterialName(armorMaterial));

        result.steps.append(QCoreApplication::translate("ArmorTrim", "在锻造台中，用%1和%2分别处理四件盔甲。")
                                .arg(templateName, material.item));
        for (const QString &piece : pieceNames) {
            result.steps.append(
                QCoreApplication::translate("ArmorTrim", "%1 + %2 + %3 → 「%2 / %4纹饰的%3」")
                    .arg(templateName, material.item, piece, pattern.name));
        }
        result.materials.append(QStringLiteral("%1 × %2").arg(templateName).arg(kPartCount));
        result.materials.append(QStringLiteral("%1 × %2").arg(material.item).arg(kPartCount));
        for (const QString &piece : pieceNames) {
            result.materials.append(QStringLiteral("%1 × 1").arg(piece));
        }
        result.notes.append(QCoreApplication::translate("ArmorTrim", "整套需要 %1 次锻造，模板和材料各 %1 份。")
                                .arg(kPartCount));
        result.summary = QCoreApplication::translate("ArmorTrim", "%1 / %2纹饰的%3")
                             .arg(material.name, pattern.name, setLabel);
        result.ok = true;
        return result;
    }

    // ---------- 单件 / 同一件做几份 ----------
    const QString armor = armorPieceName(armorMaterial, part);

    result.steps.append(QCoreApplication::translate("ArmorTrim", "在锻造台中放入%1、%2和%3。")
                            .arg(templateName, material.item, armor));
    result.steps.append(QCoreApplication::translate("ArmorTrim", "一次锻造消耗 1 个模板、1 份材料、1 件%1 → 「%2 / %3纹饰的%1」")
                            .arg(armor, material.name, pattern.name));

    result.materials.append(QStringLiteral("%1 × %2").arg(templateName).arg(pieces));
    result.materials.append(QStringLiteral("%1 × %2").arg(material.item).arg(pieces));
    result.materials.append(QStringLiteral("%1 × %2").arg(armor).arg(pieces));

    if (pieces > 1) {
        result.notes.append(QCoreApplication::translate("ArmorTrim", "模板每打一件消耗一个：%1 件共需 %1 个。")
                                .arg(pieces));
    }
    result.summary = QCoreApplication::translate("ArmorTrim", "%1 / %2纹饰的%3 × %4")
                         .arg(material.name, pattern.name, armor)
                         .arg(pieces);
    result.ok = true;
    return result;
}

QString Plan::detailText() const {
    if (!ok) {
        return error.isEmpty() ? QCoreApplication::translate("ArmorTrim", "排不出来。") : error;
    }
    QStringList lines;
    lines.append(summary);
    lines.append(QString());
    lines.append(QCoreApplication::translate("ArmorTrim", "一步一步："));
    for (const QString &step : steps) {
        lines.append(QStringLiteral("• %1").arg(step));
    }
    if (!materials.isEmpty()) {
        lines.append(QString());
        lines.append(QCoreApplication::translate("ArmorTrim", "材料清单："));
        for (const QString &row : materials) {
            lines.append(QStringLiteral("• %1").arg(row));
        }
    }
    if (!notes.isEmpty()) {
        lines.append(QString());
        for (const QString &note : notes) {
            lines.append(QCoreApplication::translate("ArmorTrim", "（%1）").arg(note));
        }
    }
    return lines.join(QChar('\n'));
}

}  // namespace ArmorTrim
