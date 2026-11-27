/**
 * @file CoordConvert.hpp
 * @brief 坐标换算：主世界 ↔ 下界 8:1、区块与区域文件坐标、以及认得 F3+C 那条命令。
 * @note 用户点名的工具箱第三件：「坐标换算：主世界/
 * 下界 8:1、区块和区域文件坐标，负坐标必须按 Minecraft 的向下取整」，外加他那一句
 * 「允许基于 F3+C 复制来的 tp 命令来识别坐标和当前维度」—— 所以这一层还要能把游戏
 * 自己复制出来的那条命令读开。
 *
 * 【F3+C 复制出来的到底是什么】
 *
 * 游戏里按 F3+C（默认是"复制坐标"，长按会退出）往剪贴板里放的是**一条完整的命令**，
 * 不是三个数：
 *
 * ```
 * /execute in minecraft:the_nether run tp @s 123.450000 64.000000 -78.900000 90.000000 0.000000
 * ```
 *
 * 里面**带着维度**（`execute in <维度 id> run` 那一段）和两个朝向角（yaw / pitch，最后两
 * 个数）。所以"粘贴一条 F3+C"能一次性给出位置和维度这两件事 —— 这正是这个工具要的输入。
 * 手打的 `/tp @s 100 64 -200`、`/tp 100 64 -200` 这两种也认（那就是"没说维度"）。
 *
 * NOTE: `~` / `^` 开头的相对坐标**照样读得出来，但不算"一个位置"** —— 它说的是"从这里往前
 * 几格"，得先在游戏里站着才有意义。这一层把它标成 `relative`，界面上明说这一条不能
 * 换算，而不是当成 0 或者按上一次的值硬凑一个数出来。
 *
 * 【负坐标为什么值得单说】
 *
 * 区块和区域文件都是从 0 往两边排的，而 C++ 的 `/` 是**向零取整**：`-1 / 16 == 0`，于是
 * 区块 -1 会被算成区块 0；`>>` 对负数是向下取整，两者在负数那一侧**差一格**。整个 Minecraft
 * 的区块/区域坐标都按"向下取整"走（区块 -1 装的是方块 -16..-1），所以这一层只用
 * `floorDiv()`，不许在任何地方出现裸 `/` 或裸 `>>`。
 */

#pragma once

#include <QString>

namespace CoordConvert {

/// 三个维度。`Unknown` 是"没说出来 / 认不出来"——**不是**"主世界"。
enum class Dimension { Overworld, Nether, End, Unknown };

/// 一个位置（方块坐标可以带小数，游戏里玩家站的就是小数）。
struct Position {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// 维度 id（`minecraft:the_nether` / `the_nether` / `nether` 都认）→ 枚举。
/// 认不出来回 `Unknown`（**不猜**：`minecraft:overworld` 与一个拼错的 id 不能同等对待）。
Dimension dimensionFromId(const QString &id);
/// 反向：`minecraft:overworld` / `minecraft:the_nether` / `minecraft:the_end`。
QString dimensionId(Dimension dimension);
/// 中文名（界面上那一格）。`Unknown` 回「未知维度」。
QString dimensionText(Dimension dimension);
/// 能不能按 8:1 在这两个维度之间换算：主世界 ↔ 下界可以，两头一样可以（等于不动），
/// 末地**不在**这个关系里（末地坐标不与主世界成比例）。
bool convertible(Dimension from, Dimension to);

/// 向下取整的整数除法（负数那一侧也向下）。**区块/区域坐标只许走这一个函数**。
int floorDiv(int value, int divisor);
/// 向下取整地取整一个浮点坐标（`-0.3` → `-1`）。
int floorToInt(double value);

/// 换算 @p position 到 @p to 那个维度。
///
/// - 主世界 → 下界：x、z 各除以 8（y 不动）；
/// - 下界 → 主世界：x、z 各乘以 8；
/// - 同维度：原样回；
/// - 末地在任一头：**原样回**（`convertible()` 会先说是 false，界面上不许显示换算结果）。
///
/// NOTE: 这是**坐标本身**的比例关系。真要从这一头走进那一头，游戏找的是传送门（会在落点
/// 附近搜一圈、还要在顶上找地方放门），所以这个数只是"你该往那边哪一片去找"。
Position convert(Dimension from, Dimension to, const Position &position);

/// 方块坐标 → 区块坐标（16 格一个区块，向下取整）。
int blockToChunk(int block);
/// 区块坐标 → 区域文件坐标（32 个区块一个区域文件，向下取整）。
int chunkToRegion(int chunk);
/// 区域文件叫什么（`r.<x>.<z>.mca`）。
QString regionFileName(int regionX, int regionZ);
/// 区块的方块范围，写成 `-16…-1` 这种（界面上"这一格盖住哪一片"用）。
QString blockRangeText(int chunk);

/**
 * @brief F3+C 那条命令 / 手打的 `/tp` 读出来的东西。
 */
struct ParsedTp {
    /// 读出来了**一个能用的绝对位置**（三个数都在、都不是相对坐标）。
    bool ok = false;
    /// 命令里说了维度（`execute in <id> run` 那一段）。
    bool hasDimension = false;
    Dimension dimension = Dimension::Unknown;
    Position position;
    /// 有没有 `~` / `^` 这种相对写法（有的话 ok 一定是 false）。
    bool relative = false;
    /// 朝向角：有就给，没有就是 false（角度**不影响**位置，界面上单列一行）。
    bool hasRotation = false;
    double yaw = 0.0;
    double pitch = 0.0;
    /// 没读出来的原因（中文，直接显示给用户）。
    QString error;
};

/**
 * @brief 读一条 `/execute in <维度> run tp @s x y z [yaw pitch]` 或 `/tp …`。
 *
 * 认这些写法（前后空白、开头那个 `/`、大小写都不计较）：
 * ```
 * execute in minecraft:overworld run tp @s 123.45 67.00 890.12 90.0 0.0
 * /execute in minecraft:the_nether run tp @s 1 2 3
 * /tp @s 1 2 3
 * tp 1 2 3
 * /tp Steve 1 2 3 90 0
 * ```
 * 选择器那一格（`@s` / `@p` / `@a` / 玩家名）**有就跳过、没有也行** —— 这一层不解析选择器，
 * 只认"三个数从哪儿开始"。数字允许 `-` 和 `.`（`1`、`-3.5`、`0.0` 都认）。
 */
ParsedTp parseTeleportCommand(const QString &text);
/// 把坐标拼成一条能粘回聊天栏的 `/tp` 命令（三个数保留两位小数）。
QString toTeleportCommand(const Position &position, Dimension dimension);
/// 先把 @p position 从 @p from 换到 @p to，再拼成一条带 `execute in <to>` 的 `/tp`。
///
/// 同维度与末地那一头不按 8:1 换算（见 `convert()`），命令里的维度仍写 @p to ——
/// 用户选的是"要去哪个维度"，坐标该不该缩放由这两条规则自己决定。
QString convertedTeleportCommand(Dimension from, Dimension to, const Position &position);
/// 坐标怎么显示：`123.45 64.00 -78.90`（两位小数）。
QString positionText(const Position &position);

}  // namespace CoordConvert
