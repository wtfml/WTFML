/**
 * @file Brewing.hpp
 * @brief 酿造助手的判据层（工具箱里的「酿造助手」）。
 *
 * 只做一张表和一条路径：用户选"要做什么药水、要不要延长 / II 级 / 喷溅 / 滞留"，
 * 这里回"从水瓶开始，按什么顺序放什么材料"。**不碰盘、不联网**。
 *
 * 【表怎么读】
 *
 * 每一种药水记四件事：它叫什么、要往**上一瓶**里加什么材料、上一瓶是哪一种（`parent`），
 * 以及它是不是从**水瓶**直接做（`fromWater`，只有虚弱和缓降这两种 —— 它们不需要下界疣）。
 * "伤害 / 隐身 / 迟缓"这几种是**发酵蜘蛛眼改出来的**：它们的前一步是另一种药水，
 * 所以 `plan()` 会先把那条链一步步补全（水瓶 → 粗制的药水 → 夜视 → 隐身）。
 *
 * 【红石、荧石粉、火药、龙息】
 *
 * * 红石粉延长**时长**、荧石粉提高**等级** —— 这两样在游戏里**二选一**（后加的会把先加的
 * 冲掉），所以两个勾同时打上时这里直接报错、不悄悄挑一个。
 * * 火药把**普通**变**喷溅**；龙息把**喷溅**变**滞留**（龙息只能加在喷溅药水上，所以选
 * 滞留时前面会先自动补一步火药）。
 * * 不是每一种药水都有延长 / II 级版本（抗火、夜视、隐身这些没有 II 级）—— 表里那两位
 * 标记说了算，界面不该自己再猜一份。
 *
 * 【时长和瞬间效果】
 *
 * 表里每种药水记着基础时长、红石延长后的时长、荧石升级后的时长；治疗和伤害是瞬间效果，
 * 只写说明、不写时长。喷溅型和普通型时长相同；滞留型给的是效果云的周期时长，只有普通型的
 * 四分之一（秒数向下取整），瞬间效果在效果云里改成一句「周期生效」的说明。
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Brewing {

/// 药水的三种形态。**下标就是界面上那个下拉框的下标**。
enum class Form {
    Normal = 0,   ///< 普通（药水）
    Splash = 1,   ///< 喷溅型（加火药）
    Lingering = 2,///< 滞留型（喷溅型再加龙息）
};

int formCount();
/// 形态的中文名（`普通` / `喷溅型` / `滞留型`）。越界回空串。
QString formName(Form form);

/// 表里的一种药水。
struct PotionInfo {
    QString name;       ///< 药水名（不带"药水"两个字，也不带形态前缀）
    QString ingredient; ///< 这一步要加的材料
    int parent = -1;    ///< -1 = 从粗制的药水（或水瓶）来；>= 0 = 先做出第 parent 种
    bool fromWater = false; ///< 直接从水瓶做（不需要下界疣）
    bool extendable = false;///< 能加红石粉延长
    bool upgradable = false;///< 能加荧石粉升一级
    /// 荧石粉之后是几级（2 = II；**迟缓是 IV**，游戏里荧石粉给的是迟缓 IV）。
    /// `upgradable == false` 时是 0。
    int upgradedLevel = 0;
    /// 这一种药水从哪一版起才有（`1.21`）。表里绝大多数是空串 = 各版本都有；
    /// 1.21 加的那四瓶（盘丝 / 蓄风 / 渗浆 / 虫蚀）写 `1.21`。
    QString since;
    /// 基础持续时间（秒）。瞬间药水（治疗 / 伤害）为 0，说明写在 `instantEffect` 里。
    int baseSeconds = 0;
    /// 红石粉延长之后持续几秒；0 = 没有延长版本。
    int extendedSeconds = 0;
    /// 荧石粉升级之后持续几秒；0 = 没有强化版本。
    int strongSeconds = 0;
    /// 瞬间效果的说明（I 级），如「恢复 4 点生命值」；空 = 不是瞬间药水。
    QString instantEffect;
    /// 瞬间效果升到 II 级时的说明，如「恢复 8 点生命值」。
    QString instantEffectStrong;
};

/// @p level 的中文写法：2 → 「II」、4 → 「IV」，其它照数字写（`levelText(0)` 空串）。
QString levelText(int level);

/// 表里有几种药水。
int potionCount();
/// 第 @p index 种（越界回空的那个 —— `name` 为空）。
PotionInfo potionAt(int index);
/// 全部药水名（界面下拉框用；次序就是表的次序）。
QStringList potionNames();
/// 这一种药水从哪一版起才有（`1.21`）；各版本都有时回空串。越界回空串。
QString potionSince(int index);
/**
 * @brief @p version 里有没有第 @p index 种药水（空串 = 不限版本）。
 *
 * 判据走 `GameVersion::availableIn()`，与旗帜那页的「目标版本」同一套。
 * 越界下标回 **false**（认不出来就不能说它有）。
 */
bool available(int index, const QString &version);

/// 一条酿造计划。
struct Plan {
    bool ok = false;
    QString error;       ///< 非空 = 做不了（这种药水没有延长版本…）
    QStringList steps;   ///< 每一步一行，写成「水瓶 + 地狱疣 → 粗制的药水」
    /// 一句总述（`产出：迅捷药水 × 6 瓶（酿 2 次）`）。
    QString summary;
    /// 效果摘要：药水名、等级与持续时间（`迅捷 I · 3:00`）；
    /// 瞬间药水写说明、不写持续时间（`治疗 II · 瞬间恢复 8 点生命值`）；
    /// 滞留型按普通型的四分之一写时长，瞬间药水写「瞬间，效果云内周期生效」。
    QString effect;
    /// 这瓶直接从水瓶酿（不经粗制的药水）。界面要把它单独标出来。
    bool directFromWater = false;
    int bottles = 1;     ///< 要几瓶（一次酿造最多 3 瓶）
    int operations = 1;  ///< 一共要酿几次 = `ceil(bottles / 3)`
    /// 材料合计（每种材料一行：`糖 × 2`）。同一步里的材料按**次数**累加；
    /// 第一行是水瓶（按瓶数），最后一行是烈焰粉燃料（1 个够酿 20 次）。
    QStringList materials;
    /// 整份明细（产出 + 效果 + 步骤 + 材料）。界面上那个结果框直接摆它。
    QString detailText() const;
};

/// 一次酿造最多放几瓶。
int maxBottlesPerOperation();
/// 一次「要几瓶」的取值范围上限（界面那个转盘的上限）。
int maxBottles();
/// @p bottles 瓶一共要酿几次（`ceil(bottles / 3)`；<= 0 回 0）。
int operationsFor(int bottles);
/// 酿 @p operations 次要几个烈焰粉（1 个烈焰粉够 20 次；<= 0 回 0）。
int blazePowderFor(int operations);

/**
 * @brief 算一条路线。
 * @param potionIndex 第几种药水（`potionAt()` 的下标）。
 * @param extended 要不要红石延长。
 * @param level2 要不要荧石粉升一级（和 @p extended 二选一）。
 * @param form 最终形态。
 * @param bottles 要几瓶（1 – `maxBottles()`；越界报错）。
 */
Plan plan(int potionIndex, bool extended, bool level2, Form form, int bottles = 1);

}  // namespace Brewing
