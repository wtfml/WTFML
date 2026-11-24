/**
 * @file Banner.hpp
 * @brief 旗帜设计的判据层（工具箱里的「旗帜设计」）。
 *
 * 只做两件事：**一张图案表**（原版织布机能叠的那几十种图案、各自要什么材料）和
 * **一条叠层计划**（底色 + 若干层，回一段"先做什么、每层在织布机里放什么、一共要多少染料"）。
 * **不碰盘、不联网、不画图**。
 *
 * 【为什么这里不出一张"旗子长什么样"的预览】
 *
 * 预览要图案的遮罩贴图，而那些贴图是游戏自带的资源；要求**素材来源和分发许可
 * 没明确之前不伪造游戏内精确预览、也不复制官方贴图**。所以这一版只给**文字层序**：第几层
 * 是什么图案、什么颜色。用户拿去照着在织布机里点，一样能叠出来；以后要真预览，得先定
 * 贴图从哪儿来（自己按遮罩规则画，或者让用户指定游戏目录读本地的）。
 *
 * 【层的数与合法性】
 *
 * 原版织布机**最多叠 6 层**（第 6 层之后界面不再给图案）。这里按 6 卡死并报一句人话。
 * 同一图案可以叠两次（游戏允许），但**连续两层图案和颜色都一样**在画面上等于只叠了一层，
 * 所以那种情况给一条提醒（不是错误 —— 用户可能就是照别人的图叠的）。
 *
 * 【图案材料那一栏】
 *
 * `no_item_required` 里的图案在织布机里不需要额外物品（`item` 为空串）；其余图案要一件
 * 「旗帜图案」。1.21.2 起砖纹与波纹边改由各自的图案物品解锁，材料按目标版本判
 * （见 `Banner.cpp` 的 `materialFor()`）。界面不该自己再猜一份。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace Banner {

/// 原版织布机最多叠几层。
constexpr int kMaxLayers = 6;

/// 表里的一种图案。
struct PatternInfo {
    QString id;    ///< 图案 id（`stripe_bottom`；也用于 NBT 与指令）
    QString name;  ///< 中文名（`底横条`）
    QString item;  ///< 织布机里要额外摆的材料（`旗帜图案`）；空 = 该图案不需要图案物品
    /// 这一种图案从哪一版起才有（`1.16` / `1.21`；表里绝大多数是 `1.14` 织布机那批，回 `1.14`）。
    QString since;
};

int patternCount();
/// 第 @p index 种（越界回空的那个 —— `id` 为空）。
PatternInfo patternAt(int index);
/// 全部图案名（界面下拉框用；次序就是表的次序）。
QStringList patternNames();
/// 按 id 找下标；认不出来回 -1。
int patternIndexById(const QString &id);
/// 这一种图案从哪一版起才有（`1.14` / `1.21`）。越界回空串。
QString patternSince(int index);

// ================= 十六色 =================
//
// 次序和游戏里染料/羊毛的次序一致（0 = 白 … 15 = 黑），**下标就是界面下拉框的下标**。
int colorCount();
/// 中文颜色名（`红色`）；越界回空串。
QString colorName(int index);
/// 颜色的 id（`red`；拼 `red_banner` / `red_dye` 用）。
QString colorId(int index);
/// 染料物品名（`红色染料`）。
QString dyeName(int index);

/// 一层：什么图案、什么颜色。
struct Layer {
    int patternIndex = 0;
    int colorIndex = 0;
};

/// 一条叠层计划。
struct Plan {
    bool ok = false;
    QString error;        ///< 非空 = 排不出来（层数超了 / 下标不对）
    QStringList steps;    ///< 从空白底旗开始逐层叠加图案
    QString summary;      ///< 一句总述（`底色 红色 · 3/6 层`）
    QStringList materials;///< 图案材料清单（`白色染料 × 2`…）
    QStringList notes;    ///< 要提醒的（连续两层一模一样、材料有特殊物品…）
    /// `/give` 那一行。默认是 **1.13–1.20.4 的 NBT 写法**（`{BlockEntityTag:{Patterns:…}}`）；
    /// 目标版本 >= 1.20.5 时换成**物品组件**写法（`[minecraft:banner_patterns=[…]]`）——
    /// 那一版起 NBT 那串不再被接受（见 `plan()` 的 @p version）。
    QString command;
    /// 整份明细（总述 + 步骤 + 材料 + 指令）。界面上那个结果框直接摆它。
    QString detailText() const;
};

/**
 * @brief 算一条叠层计划。
 * @param baseColorIndex 底色（`colorName()` 的下标）。
 * @param layers 从下往上叠的层（第 0 项是先用织布机叠的那一层）。
 * @param version 目标版本（空串 = 不限版本）。决定图案材料（1.21.2 起砖纹 / 波纹边要图案物品）
 *                与 `command` 的写法（1.20.5 起是物品组件、更早是 NBT；空串按老写法给）。
 */
Plan plan(int baseColorIndex, const QList<Layer> &layers, const QString &version = QString());

}  // namespace Banner
