/**
 * @file Brewing.cpp
 * @brief Brewing.hpp 的实现（那张表与路线拼装，见头文件）。
 */

#include "tools/Brewing.hpp"
#include "tools/GameVersion.hpp"

#include <QCoreApplication>
#include <QHash>

namespace Brewing {

namespace {

/// 药的配方表。**次序就是界面上下拉框的次序**（把常做的几种摆前面）。
///
/// NOTE: `parent` 只指向**表里更靠前**的那一种 —— 这样"从上往下补全路线"永远不会绕
/// 出一只环，也就不需要额外做环检测。
struct Recipe {
    const char *name;
    const char *ingredient;
    const char *effectName;
    int parent;
    bool fromWater;
    bool extendable;
    bool upgradable;
    int upgradedLevel = 2;  ///< 荧石粉之后是几级（只有迟缓是 IV，见下面那一条）
    const char *since = nullptr;  ///< 从哪一版起才有（空 = 各版本都有）
    int baseSeconds = 0;      ///< 基础时长（秒）；瞬间药水为 0
    int extendedSeconds = 0;  ///< 红石延长之后（0 = 没有延长版本）
    int strongSeconds = 0;    ///< 荧石升级之后（0 = 没有强化版本）
    const char *instantEffect = nullptr;        ///< 瞬间效果（I 级）的说明
    const char *instantEffectStrong = nullptr;  ///< 瞬间效果（II 级）的说明
};

const Recipe kRecipes[] = {
    {QT_TRANSLATE_NOOP("Brewing", "迅捷药水"), QT_TRANSLATE_NOOP("Brewing", "糖"), QT_TRANSLATE_NOOP("Brewing", "迅捷"), -1, false, true, true, 2, nullptr, 180, 480, 90},
    {QT_TRANSLATE_NOOP("Brewing", "跳跃药水"), QT_TRANSLATE_NOOP("Brewing", "兔子脚"), QT_TRANSLATE_NOOP("Brewing", "跳跃"), -1, false, true, true, 2, nullptr, 180, 480, 90},
    // 治疗 / 伤害是瞬间效果：没有时长，只有一句说明。
    {QT_TRANSLATE_NOOP("Brewing", "治疗药水"), QT_TRANSLATE_NOOP("Brewing", "闪烁的西瓜片"), QT_TRANSLATE_NOOP("Brewing", "治疗"), -1, false, false, true, 2, nullptr, 0, 0, 0,
     QT_TRANSLATE_NOOP("Brewing", "恢复 4 点生命值"), QT_TRANSLATE_NOOP("Brewing", "恢复 8 点生命值")},
    {QT_TRANSLATE_NOOP("Brewing", "伤害药水"), QT_TRANSLATE_NOOP("Brewing", "发酵蜘蛛眼"), QT_TRANSLATE_NOOP("Brewing", "伤害"), 2, false, false, true, 2, nullptr, 0, 0, 0,
     QT_TRANSLATE_NOOP("Brewing", "造成 6 点伤害"), QT_TRANSLATE_NOOP("Brewing", "造成 12 点伤害")},
    {QT_TRANSLATE_NOOP("Brewing", "剧毒药水"), QT_TRANSLATE_NOOP("Brewing", "蜘蛛眼"), QT_TRANSLATE_NOOP("Brewing", "中毒"), -1, false, true, true, 2, nullptr, 45, 90, 21},
    {QT_TRANSLATE_NOOP("Brewing", "力量药水"), QT_TRANSLATE_NOOP("Brewing", "烈焰粉"), QT_TRANSLATE_NOOP("Brewing", "力量"), -1, false, true, true, 2, nullptr, 180, 480, 90},
    // 再生能加红石延长（1:30 → 3:00），荧石粉升 II —— 之前那张表把它标成了「不能延长」。
    {QT_TRANSLATE_NOOP("Brewing", "再生药水"), QT_TRANSLATE_NOOP("Brewing", "恶魂之泪"), QT_TRANSLATE_NOOP("Brewing", "再生"), -1, false, true, true, 2, nullptr, 45, 90, 22},
    {QT_TRANSLATE_NOOP("Brewing", "抗火药水"), QT_TRANSLATE_NOOP("Brewing", "岩浆膏"), QT_TRANSLATE_NOOP("Brewing", "抗火"), -1, false, true, false, 2, nullptr, 180, 480},
    {QT_TRANSLATE_NOOP("Brewing", "水肺药水"), QT_TRANSLATE_NOOP("Brewing", "河豚"), QT_TRANSLATE_NOOP("Brewing", "水肺"), -1, false, true, false, 2, nullptr, 180, 480},
    {QT_TRANSLATE_NOOP("Brewing", "夜视药水"), QT_TRANSLATE_NOOP("Brewing", "金胡萝卜"), QT_TRANSLATE_NOOP("Brewing", "夜视"), -1, false, true, false, 2, nullptr, 180, 480},
    {QT_TRANSLATE_NOOP("Brewing", "隐身药水"), QT_TRANSLATE_NOOP("Brewing", "发酵蜘蛛眼"), QT_TRANSLATE_NOOP("Brewing", "隐身"), 9, false, true, false, 2, nullptr, 180, 480},
    // 荧石粉给的是**迟缓 IV**（不是 II）—— 这是唯一一种升级跳两级的药水。
    {QT_TRANSLATE_NOOP("Brewing", "迟缓药水"), QT_TRANSLATE_NOOP("Brewing", "发酵蜘蛛眼"), QT_TRANSLATE_NOOP("Brewing", "迟缓"), 0, false, true, true, 4, nullptr, 90, 240, 20},
    {QT_TRANSLATE_NOOP("Brewing", "缓降药水"), QT_TRANSLATE_NOOP("Brewing", "幻翼膜"), QT_TRANSLATE_NOOP("Brewing", "缓降"), -1, true, true, false, 2, nullptr, 90, 240},
    {QT_TRANSLATE_NOOP("Brewing", "虚弱药水"), QT_TRANSLATE_NOOP("Brewing", "发酵蜘蛛眼"), QT_TRANSLATE_NOOP("Brewing", "虚弱"), -1, true, true, false, 2, nullptr, 90, 240},
    {QT_TRANSLATE_NOOP("Brewing", "神龟药水"), QT_TRANSLATE_NOOP("Brewing", "海龟壳"), QT_TRANSLATE_NOOP("Brewing", "海龟大师"), -1, false, true, true, 2, nullptr, 20, 40, 20},
    // 1.21 加的四瓶：都从**粗制的药水**来（不是水瓶直做），也没有延长 / II 级版本。
    // 材料名照 `zh_cn.json`：旋风棒 / 蜘蛛网 / 黏液块 / 石头。
    {QT_TRANSLATE_NOOP("Brewing", "盘丝药水"), QT_TRANSLATE_NOOP("Brewing", "蜘蛛网"), QT_TRANSLATE_NOOP("Brewing", "盘丝"), -1, false, false, false, 2, "1.21", 180},
    {QT_TRANSLATE_NOOP("Brewing", "蓄风药水"), QT_TRANSLATE_NOOP("Brewing", "旋风棒"), QT_TRANSLATE_NOOP("Brewing", "蓄风"), -1, false, false, false, 2, "1.21", 180},
    {QT_TRANSLATE_NOOP("Brewing", "渗浆药水"), QT_TRANSLATE_NOOP("Brewing", "黏液块"), QT_TRANSLATE_NOOP("Brewing", "渗浆"), -1, false, false, false, 2, "1.21", 180},
    {QT_TRANSLATE_NOOP("Brewing", "虫蚀药水"), QT_TRANSLATE_NOOP("Brewing", "石头"), QT_TRANSLATE_NOOP("Brewing", "虫蚀"), -1, false, false, false, 2, "1.21", 180},
};

int recipeCount() { return int(sizeof(kRecipes) / sizeof(kRecipes[0])); }

/// 秒数写成游戏里那种 `3:00` / `0:20`（<= 0 回空串）。
QString durationText(int seconds) {
    if (seconds <= 0) return QString();
    return QStringLiteral("%1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString nameWithModifiers(const QString &name, bool extended, bool level2, Form form,
                          int upgradedLevel) {
    QString text = name;
    switch (form) {
    case Form::Splash: text = QCoreApplication::translate("Brewing", "喷溅型%1").arg(name); break;
    case Form::Lingering: text = QCoreApplication::translate("Brewing", "滞留型%1").arg(name); break;
    case Form::Normal: break;
    }
    if (extended) text += QCoreApplication::translate("Brewing", "（延长）");
    if (level2) text += QLatin1Char(' ') + levelText(upgradedLevel > 0 ? upgradedLevel : 2);
    return text;
}

/// 效果那一行：药水名 + 等级 + 时长；瞬间药水改写说明、不带时长。
/// 滞留型的时长是普通 / 喷溅的四分之一（效果云每周期只给那么久），秒数向下取整。
QString effectText(const PotionInfo &info, const QString &effectName, bool extended, bool level2, Form form) {
    const int level = level2 ? info.upgradedLevel : 1;
    QString text = QStringLiteral("%1 %2").arg(effectName, levelText(level));
    if (!info.instantEffect.isEmpty()) {
        if (form == Form::Lingering) {
            return text + QCoreApplication::translate("Brewing", " · 瞬间，效果云内周期生效");
        }
        const QString note =
            level2 && !info.instantEffectStrong.isEmpty() ? info.instantEffectStrong
                                                          : info.instantEffect;
        return text + QCoreApplication::translate("Brewing", " · 瞬间") + note;
    }
    int seconds = level2 ? info.strongSeconds
                         : extended ? info.extendedSeconds : info.baseSeconds;
    if (form == Form::Lingering) seconds /= 4;
    return text + QStringLiteral(" · ") + durationText(seconds);
}

}  // namespace

int formCount() { return 3; }

QString formName(Form form) {
    switch (form) {
    case Form::Normal: return QCoreApplication::translate("Brewing", "普通");
    case Form::Splash: return QCoreApplication::translate("Brewing", "喷溅型");
    case Form::Lingering: return QCoreApplication::translate("Brewing", "滞留型");
    }
    return QString();
}

QString levelText(int level) {
    if (level <= 0) return QString();
    switch (level) {
    case 1: return QStringLiteral("I");
    case 2: return QStringLiteral("II");
    case 3: return QStringLiteral("III");
    case 4: return QStringLiteral("IV");
    default: return QString::number(level);
    }
}

int maxBottlesPerOperation() { return 3; }
int maxBottles() { return 64; }

int operationsFor(int bottles) {
    if (bottles <= 0) return 0;
    return (bottles + maxBottlesPerOperation() - 1) / maxBottlesPerOperation();
}

int blazePowderFor(int operations) {
    if (operations <= 0) return 0;
    return (operations + 19) / 20;  // 1 个烈焰粉够 20 次酿造
}

int potionCount() { return recipeCount(); }

PotionInfo potionAt(int index) {
    PotionInfo info;
    if (index < 0 || index >= recipeCount()) return info;
    const Recipe &recipe = kRecipes[index];
    info.name = QCoreApplication::translate("Brewing", recipe.name);
    info.ingredient = QCoreApplication::translate("Brewing", recipe.ingredient);
    info.parent = recipe.parent;
    info.fromWater = recipe.fromWater;
    info.extendable = recipe.extendable;
    info.upgradable = recipe.upgradable;
    info.upgradedLevel = recipe.upgradable ? recipe.upgradedLevel : 0;
    info.since = recipe.since != nullptr ? QString::fromLatin1(recipe.since) : QString();
    info.baseSeconds = recipe.baseSeconds;
    info.extendedSeconds = recipe.extendedSeconds;
    info.strongSeconds = recipe.strongSeconds;
    info.instantEffect =
        recipe.instantEffect != nullptr ? QCoreApplication::translate("Brewing", recipe.instantEffect) : QString();
    info.instantEffectStrong = recipe.instantEffectStrong != nullptr
                                   ? QCoreApplication::translate("Brewing", recipe.instantEffectStrong)
                                   : QString();
    return info;
}

QStringList potionNames() {
    QStringList names;
    for (const Recipe &recipe : kRecipes) names.append(QCoreApplication::translate("Brewing", recipe.name));
    return names;
}

QString potionSince(int index) {
    if (index < 0 || index >= recipeCount() || kRecipes[index].since == nullptr) {
        return QString();
    }
    return QString::fromLatin1(kRecipes[index].since);
}

bool available(int index, const QString &version) {
    if (index < 0 || index >= recipeCount()) {
        return false;
    }
    return GameVersion::availableIn(potionSince(index), version);
}

QString Plan::detailText() const {
    if (!ok) return error;
    QStringList lines;
    lines << summary;
    lines << QCoreApplication::translate("Brewing", "效果：%1").arg(effect);
    lines << QCoreApplication::translate("Brewing", "步骤（共 %1 步）：").arg(steps.size());
    for (int i = 0; i < steps.size(); ++i) {
        lines << QStringLiteral("%1. %2").arg(i + 1).arg(steps.at(i));
    }
    if (directFromWater) {
        lines << QCoreApplication::translate("Brewing", "特殊路径：这一瓶直接从水瓶酿，不需要先做粗制的药水。");
    }
    lines << QCoreApplication::translate("Brewing", "材料：%1").arg(materials.join(QCoreApplication::translate("Brewing", "、")));
    return lines.join(QLatin1Char('\n'));
}

Plan plan(int potionIndex, bool extended, bool level2, Form form, int bottles) {
    Plan result;
    if (potionIndex < 0 || potionIndex >= recipeCount()) {
        result.error = QCoreApplication::translate("Brewing", "药水类型编号超出范围。");
        return result;
    }
    if (extended && level2) {
        result.error = QCoreApplication::translate("Brewing", "红石粉仅延长时长，荧石粉仅提高等级。两种效果互斥，以最后添加的材料为准。");
        return result;
    }
    const PotionInfo target = potionAt(potionIndex);
    if (extended && !target.extendable) {
        result.error = QCoreApplication::translate("Brewing", "%1不支持用红石粉延长时长。")
                           .arg(target.name);
        return result;
    }
    if (level2 && !target.upgradable) {
        result.error = QCoreApplication::translate("Brewing", "%1不支持用荧石粉提高等级。")
                           .arg(target.name);
        return result;
    }
    if (int(form) < 0 || int(form) >= formCount()) {
        result.error = QCoreApplication::translate("Brewing", "药水形态编号超出范围。");
        return result;
    }
    if (bottles < 1 || bottles > maxBottles()) {
        result.error = QCoreApplication::translate("Brewing", "瓶数只能是 1 – %1（%2 不在这个范围里）。")
                           .arg(maxBottles())
                           .arg(bottles);
        return result;
    }

    result.bottles = bottles;
    result.operations = operationsFor(bottles);

    // 材料记账：每一种材料出现一次就算一份，最后统一乘"酿几次"。
    QStringList ingredientOrder;
    QHash<QString, int> ingredientCounts;
    const auto addIngredient = [&ingredientOrder, &ingredientCounts](const QString &name) {
        if (name.isEmpty()) return;
        if (!ingredientCounts.contains(name)) ingredientOrder << name;
        ingredientCounts[name] += 1;
    };

    // ---------- 把"从水瓶到目标"那一条链摆出来 ----------
    QList<int> chain;
    int cursor = potionIndex;
    int guard = 0;
    while (cursor >= 0 && guard++ <= recipeCount()) {
        chain.prepend(cursor);
        cursor = potionAt(cursor).parent;
    }

    bool first = true;
    for (int index : chain) {
        const PotionInfo info = potionAt(index);
        const QString produced = info.name;
        if (first && info.fromWater) {
            result.steps << QCoreApplication::translate("Brewing", "水瓶 + %1 → %2").arg(info.ingredient, produced);
            addIngredient(info.ingredient);
        } else {
            if (first) {
                result.steps << QCoreApplication::translate("Brewing", "水瓶 + 下界疣 → 粗制的药水");
                addIngredient(QCoreApplication::translate("Brewing", "下界疣"));
            }
            const QString source = first ? QCoreApplication::translate("Brewing", "粗制的药水")
                                         : potionAt(info.parent).name;
            result.steps << QStringLiteral("%1 + %2 → %3").arg(source, info.ingredient, produced);
            addIngredient(info.ingredient);
        }
        first = false;
    }

    // ---------- 后处理：每一步也写成「原料 + 材料 → 产物」 ----------
    QString modified = target.name;
    if (extended) {
        const QString next = modified + QCoreApplication::translate("Brewing", "（延长）");
        result.steps << QCoreApplication::translate("Brewing", "%1 + 红石粉 → %2").arg(modified, next);
        addIngredient(QCoreApplication::translate("Brewing", "红石粉"));
        modified = next;
    }
    if (level2) {
        const QString next = modified + QLatin1Char(' ') + levelText(target.upgradedLevel);
        result.steps << QCoreApplication::translate("Brewing", "%1 + 荧石粉 → %2").arg(modified, next);
        addIngredient(QCoreApplication::translate("Brewing", "荧石粉"));
        modified = next;
    }
    switch (form) {
    case Form::Normal: break;
    case Form::Splash: {
        const QString next = QCoreApplication::translate("Brewing", "喷溅型%1").arg(modified);
        result.steps << QCoreApplication::translate("Brewing", "%1 + 火药 → %2").arg(modified, next);
        addIngredient(QCoreApplication::translate("Brewing", "火药"));
        break;
    }
    case Form::Lingering: {
        // 龙息只能加在喷溅药水上：顺序里先把火药补上，再上龙息。
        const QString splashName = QCoreApplication::translate("Brewing", "喷溅型%1").arg(modified);
        result.steps << QCoreApplication::translate("Brewing", "%1 + 火药 → %2").arg(modified, splashName);
        result.steps << QCoreApplication::translate("Brewing", "%1 + 龙息 → %2")
                            .arg(splashName, QCoreApplication::translate("Brewing", "滞留型%1").arg(modified));
        addIngredient(QCoreApplication::translate("Brewing", "火药"));
        addIngredient(QCoreApplication::translate("Brewing", "龙息"));
        break;
    }
    }

    // 水瓶按瓶数备；材料按酿造次数备（一次最多 3 瓶，每次只放 1 个）；烈焰粉最后单列。
    result.materials << QCoreApplication::translate("Brewing", "水瓶 × %1").arg(bottles);
    for (const QString &name : ingredientOrder) {
        result.materials << QStringLiteral("%1 × %2")
                                .arg(name)
                                .arg(ingredientCounts.value(name) * result.operations);
    }
    result.materials << QCoreApplication::translate("Brewing", "烈焰粉（燃料） × %1（1 个烈焰粉可酿 20 次）")
                            .arg(blazePowderFor(result.operations));

    result.ok = true;
    result.directFromWater = target.fromWater;
    const QString effectName = QCoreApplication::translate("Brewing", kRecipes[potionIndex].effectName);
    result.effect = effectText(target, effectName, extended, level2, form);
    result.summary = QCoreApplication::translate("Brewing", "产出：%1 × %2 瓶（酿 %3 次）")
                         .arg(nameWithModifiers(target.name, extended, level2, form,
                                                target.upgradedLevel))
                         .arg(bottles)
                         .arg(result.operations);
    return result;
}

}  // namespace Brewing
