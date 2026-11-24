/**
 * @file Beacon.cpp
 * @brief Beacon.hpp 的实现（数值与那张效果表，见头文件）。
 */

#include "tools/Beacon.hpp"

#include <QCoreApplication>
#include <QList>

namespace Beacon {

namespace {

/// 效果的解锁表：第 i 项是"到第 i+1 级才出现的那一批"。
const char *const kUnlockedByLevel[][2] = {
    {QT_TRANSLATE_NOOP("Beacon", "迅捷"), QT_TRANSLATE_NOOP("Beacon", "急迫")},
    {QT_TRANSLATE_NOOP("Beacon", "抗性提升"), QT_TRANSLATE_NOOP("Beacon", "跳跃提升")},
    {QT_TRANSLATE_NOOP("Beacon", "力量"), nullptr},
    {QT_TRANSLATE_NOOP("Beacon", "生命恢复"), nullptr},
};

}  // namespace

int minLevel() { return 1; }
int maxLevel() { return 4; }

bool isValidLevel(int level) { return level >= minLevel() && level <= maxLevel(); }

bool isValidFromLevel(int fromLevel) { return fromLevel >= 0 && fromLevel < maxLevel(); }

int layerBlocks(int level) {
    if (!isValidLevel(level)) return 0;
    const int side = 2 * level + 1;
    return side * side;
}

int totalBlocks(int level) {
    if (!isValidLevel(level)) return 0;
    int total = 0;
    for (int k = minLevel(); k <= level; ++k) total += layerBlocks(k);
    return total;
}

int blocksToAdd(int fromLevel, int toLevel) {
    if (fromLevel < 0 || fromLevel > maxLevel() || !isValidLevel(toLevel) || toLevel <= fromLevel) {
        return 0;
    }
    return totalBlocks(toLevel) - totalBlocks(fromLevel);
}

int effectRange(int level) {
    if (!isValidLevel(level)) return 0;
    return 10 * level + 10;
}

QStringList effectNames() {
    QStringList names;
    for (const auto &tier : kUnlockedByLevel) {
        for (const char *name : tier) {
            if (name != nullptr) names.append(QCoreApplication::translate("Beacon", name));
        }
    }
    return names;
}

// ---------- 材质与矿物折算 ----------
//
// 信标底座只认这五种方块（铁 / 金 / 钻石 / 绿宝石 / 下界合金）。方块数、层数、效果与
// 材质无关 —— 这一节只回答"这些方块折成多少个锭 / 宝石"，不做"哪种更划算"的价值判断。

namespace {

struct MaterialInfo {
    const char *block;  ///< 「铁块」
    const char *unit;   ///< 「铁锭」
};

// 次序必须与 `Material` 的整数值一一对应。
const MaterialInfo kMaterials[] = {
    {QT_TRANSLATE_NOOP("Beacon", "铁块"), QT_TRANSLATE_NOOP("Beacon", "铁锭")},
    {QT_TRANSLATE_NOOP("Beacon", "金块"), QT_TRANSLATE_NOOP("Beacon", "金锭")},
    {QT_TRANSLATE_NOOP("Beacon", "钻石块"), QT_TRANSLATE_NOOP("Beacon", "钻石")},
    {QT_TRANSLATE_NOOP("Beacon", "绿宝石块"), QT_TRANSLATE_NOOP("Beacon", "绿宝石")},
    {QT_TRANSLATE_NOOP("Beacon", "下界合金块"), QT_TRANSLATE_NOOP("Beacon", "下界合金锭")},
};

constexpr int kMaterialCount = int(sizeof(kMaterials) / sizeof(kMaterials[0]));

}  // namespace

QStringList materialNames() {
    QStringList names;
    for (const MaterialInfo &info : kMaterials) names.append(QCoreApplication::translate("Beacon", info.block));
    return names;
}

bool isValidMaterial(int index) { return index >= 0 && index < kMaterialCount; }

QString blockName(int index) {
    return isValidMaterial(index) ? QCoreApplication::translate("Beacon", kMaterials[index].block) : QString();
}

QString unitName(int index) {
    return isValidMaterial(index) ? QCoreApplication::translate("Beacon", kMaterials[index].unit) : QString();
}

QString materialName(int index) { return blockName(index); }

int unitsPerBlock() { return 9; }

int unitsForBlocks(int index, int blocks) {
    if (!isValidMaterial(index) || blocks <= 0) return 0;
    return blocks * unitsPerBlock();
}

QString materialText(int index, int blocks) {
    if (!isValidMaterial(index)) {
        return QCoreApplication::translate("Beacon", "无效的信标底座方块（%1）。").arg(index);
    }
    if (blocks <= 0) return QString();
    return QCoreApplication::translate("Beacon", "%1 块%2 = %3 个%4（每块 %5 个）")
        .arg(blocks)
        .arg(blockName(index))
        .arg(unitsForBlocks(index, blocks))
        .arg(unitName(index))
        .arg(unitsPerBlock());
}

// ---------- 多颗信标共用底座 ----------

namespace {

/// 一颗信标在摆放网格里的位置（3 × 3 的脚印，下标 0 – 2）。
struct Cell {
    int x = 0;
    int z = 0;
};

/// 合成表（`data/minecraft/recipe/beacon.json`：`GGG / GSG / OOO`）：
/// 3 块黑曜石 + 1 颗下界之星 + 5 块玻璃。
constexpr int kObsidianPerBeacon = 3;
constexpr int kGlassPerBeacon = 5;
constexpr int kStarPerBeacon = 1;

/// 这一团信标在第 @p half 层（边长 `2 * half + 1`）的并集有多少格。
///
/// 照着 `BeaconBlockEntity.updateBase()` 那套判据：每一颗信标都要求"以自己为中心的
/// `(2 * half + 1)²` 全是底座方块"。所以挨着摆的几颗不是"各盖一座金字塔"，而是这几块
/// 方格的**并集** —— 重叠的部分只算一次。
int unionArea(const QList<Cell> &cells, int half) {
    const int side = 3 + 2 * half;  // 脚印 3 × 3 向外各扩 half
    QList<bool> covered;
    covered.fill(false, side * side);
    for (const Cell &cell : cells) {
        for (int dz = -half; dz <= half; ++dz) {
            for (int dx = -half; dx <= half; ++dx) {
                const int x = cell.x + dx + half;
                const int z = cell.z + dz + half;
                if (x >= 0 && x < side && z >= 0 && z < side) {
                    covered[z * side + x] = true;
                }
            }
        }
    }
    int count = 0;
    for (bool hit : covered) {
        if (hit) ++count;
    }
    return count;
}

/// 一颗信标要的层数摆出来长什么样（给界面说"摆成几 × 几"）。
struct Layout {
    int blocks = 0;  ///< 各层并集加起来一共多少块
    int width = 0;   ///< 这一团占几格宽（x 方向）
    int depth = 0;   ///< 这一团占几格深（z 方向）
};

/// 挑最省方块的紧凑摆法：在 3 × 3 的脚印里选 @p count 个格子，一层层算并集，取最小。
///
/// NOTE: 只搜 3 × 3 是因为 `count <= maxBeaconCount()`（9）—— 再多的摆法脚印会超过
/// 3 × 3，而这里本来也只算得出 9 颗（界面上那格的上限也是它）。
Layout compactLayout(int level, int count) {
    Layout best;
    best.blocks = -1;
    for (int mask = 0; mask < (1 << 9); ++mask) {
        if (__builtin_popcount(unsigned(mask)) != count) {
            continue;
        }
        QList<Cell> cells;
        int minX = 3;
        int maxX = -1;
        int minZ = 3;
        int maxZ = -1;
        for (int index = 0; index < 9; ++index) {
            if ((mask & (1 << index)) == 0) {
                continue;
            }
            const int x = index % 3;
            const int z = index / 3;
            cells.append(Cell{x, z});
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minZ = qMin(minZ, z);
            maxZ = qMax(maxZ, z);
        }
        int blocks = 0;
        for (int half = minLevel(); half <= level; ++half) {
            blocks += unionArea(cells, half);
        }
        if (best.blocks < 0 || blocks < best.blocks) {
            best.blocks = blocks;
            best.width = maxX - minX + 1;
            best.depth = maxZ - minZ + 1;
        }
    }
    return best;
}

}  // namespace

int maxBeaconCount() { return 9; }

int sharedBaseBlocks(int level, int beaconCount) {
    if (!isValidLevel(level) || beaconCount < 1 || beaconCount > maxBeaconCount()) return 0;
    return compactLayout(level, beaconCount).blocks;
}

QString sharedBaseText(int level, int beaconCount) {
    if (!isValidLevel(level)) {
        return QCoreApplication::translate("Beacon", "信标等级只有 1 – 4 级（%1 不是一个真的等级）。").arg(level);
    }
    if (beaconCount < 1 || beaconCount > maxBeaconCount()) {
        return QCoreApplication::translate("Beacon", "信标颗数只能算 1 – %1 颗（%2 颗超出范围）。")
            .arg(maxBeaconCount())
            .arg(beaconCount);
    }
    const QString each =
        QCoreApplication::translate("Beacon", "每颗信标另需 %1 块黑曜石 + %2 颗下界之星 + %3 块玻璃（合成表：底下 3 块黑曜石、中间一颗下界之星、四周 5 块玻璃）。")
            .arg(kObsidianPerBeacon)
            .arg(kStarPerBeacon)
            .arg(kGlassPerBeacon);
    if (beaconCount == 1) {
        return each;
    }
    const Layout layout = compactLayout(level, beaconCount);
    const int separate = beaconCount * totalBlocks(level);
    QString text = QCoreApplication::translate("Beacon", "%1 个信标按 %2 × %3 相邻排列时，可共用底座。%4 级底座共需 %5 块方块，分别建造则需 %6 块。")
                       .arg(beaconCount)
                       .arg(layout.width)
                       .arg(layout.depth)
                       .arg(level)
                       .arg(layout.blocks)
                       .arg(separate);
    text += each;
    text += QCoreApplication::translate("Beacon", "%1 颗一共 %2 块黑曜石、%1 颗下界之星、%3 块玻璃。")
                .arg(beaconCount)
                .arg(beaconCount * kObsidianPerBeacon)
                .arg(beaconCount * kGlassPerBeacon);
    return text;
}

QStringList effectsForLevel(int level) {
    QStringList names;
    if (!isValidLevel(level)) return names;
    for (int tier = 0; tier < level; ++tier) {
        for (const char *name : kUnlockedByLevel[tier]) {
            if (name != nullptr) names.append(QCoreApplication::translate("Beacon", name));
        }
    }
    return names;
}

QString summaryText(int level) {
    if (!isValidLevel(level)) {
        return QCoreApplication::translate("Beacon", "信标等级只有 1 – 4 级（%1 不是一个真的等级）。").arg(level);
    }
    return QCoreApplication::translate("Beacon", "%1 级信标：范围 %2 格，底座共 %3 块。")
        .arg(level)
        .arg(effectRange(level))
        .arg(totalBlocks(level));
}

QString detailText(int fromLevel, int level) {
    if (!isValidLevel(level)) {
        return QCoreApplication::translate("Beacon", "信标等级只有 1 – 4 级（%1 不是一个真的等级）。").arg(level);
    }
    if (fromLevel < 0 || fromLevel > maxLevel()) {
        return QCoreApplication::translate("Beacon", "「现在已有几级」只能是 0 – 4（%1 越界了）。").arg(fromLevel);
    }

    QStringList lines;
    lines << summaryText(level);
    // 说的是**从下面数第 level 层**（搭到这一级时新铺的那一层，也是最大的那一层）——
    // 不是"最上面那一层"：最上面永远是 3 × 3。
    lines << QCoreApplication::translate("Beacon", "• 第 %1 层：%2 × %2 = %3 块（最上面那层固定 3 × 3）")
                 .arg(level)
                 .arg(2 * level + 1)
                 .arg(layerBlocks(level));
    if (fromLevel > 0 && fromLevel < level) {
        lines << QCoreApplication::translate("Beacon", "• 当前为 %1 级，还需 %2 块方块")
                     .arg(fromLevel)
                     .arg(blocksToAdd(fromLevel, level));
    } else if (fromLevel == level) {
        lines << QCoreApplication::translate("Beacon", "• 当前为 %1 级，无需添加方块").arg(level);
    } else if (fromLevel > level) {
        lines << QCoreApplication::translate("Beacon", "• 当前为 %1 级，高于目标级别；降低级别需要拆除方块").arg(fromLevel);
    }
    lines << QCoreApplication::translate("Beacon", "• 效果：%1").arg(effectsForLevel(level).join(QCoreApplication::translate("Beacon", "、")));
    if (level < maxLevel()) {
        lines << QCoreApplication::translate("Beacon", "• 升到 %1 级：再加 %2 块，范围 %3 格")
                     .arg(level + 1)
                     .arg(layerBlocks(level + 1))
                     .arg(effectRange(level + 1));
    }
    lines << QCoreApplication::translate("Beacon", "• 信标放在金字塔正中间，上方留出天空（可用玻璃）");
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Beacon
