/**
 * @file SchematicInfo.hpp
 * @brief 读一份结构 / 原理图的**元数据与方块统计**（Litematica / Sponge / 原版结构）。
 *
 * 【这是拿来干什么的】
 *
 * 实例设置的「原理图」那一页今天只是把 `schematics/` 和结构方块目录里的文件列出来 ——
 * 用户站在一行上，看不出这一份是多大、来自哪个版本、里面有些什么。
 * 那一条要的正是这两句：「元数据与方块统计」+「版本兼容提示」。
 *
 * 这一层**只读盘、只算**：进一个文件路径，出一份 `Info`。界面怎么摆（一行字、一个
 * 提示条）由 ui/ 那一层决定；这一层不 include 任何 ui/、也不 include src/controllers。
 *
 * 【三种格式，判据不是后缀】
 *
 * 后缀只是个提示（用户会把 `.schematic` 改名成 `.litematic`）。真正认的是根的字段：
 *
 * · Litematica `.litematic`：根里有 `Regions`（Compound）—— 一个投影一个文件，
 * 里面可以有好几个 region，各自一份调色板和方块表；
 * · Sponge `.schematic`：根里有 `Palette`（Compound）+ `BlockData`（ByteArray）；
 * · 原版结构 `.nbt`：根里有 `palette`（List）+ `blocks`（List）。
 *
 * 认不出来就如实说「认不出这是什么格式」，**不猜**。三种格式的字段名大小写不同
 * （`Palette` vs `palette`），按名字逐字认。
 *
 * 【压缩那一层】
 *
 * 三种文件**通常都是 gzip 包着的 NBT**（头两字节 `1f 8b`）。解压沿用仓库里唯一那条
 * gzip 路（`ZipUtil::gunzipTo()`，PowerShell + .NET）—— 和 `NbtUtil` / `level.dat`
 * 那边逐字同一口，不引 zlib（理由见 `ZipUtil.hpp` 开头）。**裸 NBT 也认**：测试与
 * 一些第三方工具写出来的就是没压缩的。
 *
 * 【方块统计的口径】
 *
 * - `paletteSize` 是**调色板条目数**（不同的方块状态数，`minecraft:stone[variant=granite]`
 * 和 `minecraft:stone` 算两个状态）；
 * - `blockCount` 是**非空气**方块的个数；调色板里的 `air` / `cave_air` / `void_air`
 * 不计入（投影的包络里绝大多数格子都是空气，把它们算进去这个数就没有意义了）；
 * - `topBlocks` 按**方块 id** 聚合（同一 id 的不同状态合在一起），取最多的前
 * `kTopBlockLimit` 个；
 * - 解不出来的时候（尺寸离谱、方块表长度对不上调色板、bits 推不出来）`countsComplete`
 * 为 false，`blockCount` / `topBlocks` 留空 —— **不编一个数**。
 *
 * 【数据版本】
 *
 * 三种格式里都有一格 DataVersion（Litematica 叫 `MinecraftDataVersion`）。它和游戏
 * 版本号的对应关系是游戏自己定的，表在 `.cpp` 里（只写有把握的正式版；认不出来就只报
 * 数字）。`targetGameVersion` 由调用方递进来（实例的显示版本，例如 `1.20.1`），
 * 两边都知道才谈得上「可能不兼容」。
 */

#pragma once

#include <QList>
#include <QJsonObject>
#include <QString>

namespace SchematicInfo {

/// 认出是哪一种格式。
enum class Format {
    Unknown,          ///< 认不出来（不是这三种之一 / 根字段不对）
    Litematic,        ///< Litematica 的 `.litematic`
    SpongeSchematic,  ///< Sponge 的 `.schematic`
    Structure,        ///< 原版结构方块存下来的 `.nbt`
};

/// 格式的中文名（界面那一行与验证工程共用这一处）。
QString formatName(Format format);

/// `paletteSize` 之外的"最多的剩下几种方块"——一次列这么多个。
constexpr int kTopBlockLimit = 8;

/// 一种方块的个数（按方块 id 聚合，不含状态属性）。
struct BlockCount {
    QString id;     ///< 例如 `minecraft:stone`
    qint64 count = 0;
};

/// 读出来的一份。
struct Info {
    bool ok = false;        ///< 读成功（元数据至少认出了格式与尺寸）
    QString problem;        ///< `ok == false` 时给人看的一句话（成功时恒为空）
    Format format = Format::Unknown;

    QString name;           ///< 投影自己的名字（Litematica / Sponge 才有；原版结构没有）
    QString author;         ///< 作者（同上）
    QString description;    ///< 描述（同上，可能带颜色码，原样留着）

    int width = 0;          ///< X（各取绝对值）
    int height = 0;         ///< Y
    int length = 0;         ///< Z
    qint64 volume = 0;      ///< 包络体积（多 region 时是各 region 之和；可能大于方块数）

    int paletteSize = 0;    ///< 不同的方块状态数
    qint64 blockCount = 0;  ///< 非空气方块数（`countsComplete` 为假时是 0）
    bool countsComplete = false;  ///< 方块表解出来了没有
    QList<BlockCount> blocks;     ///< 完整的非空气方块 ID 计数，按数量降序
    QList<BlockCount> topBlocks;  ///< 最多的前 `kTopBlockLimit` 种（降序）

    int dataVersion = 0;    ///< 0 = 文件里没写 / 读不出来
};

/// 读一份（不认识 / 读不动都返回 `ok == false`，原因在 `problem` 里）。**不抛异常。**
///
/// NOTE: 解析是**同步**的；Litematica 那种几百万方块的投影要几百毫秒。调用方在 UI
/// 线程上用的时候应该先显示一句「正在读取…」，并且只在用户选中一行时读一份
/// （`InstanceFilesPage` 那边就是这么接的）。
Info read(const QString &path);

/// 数据版本 → 正式版号（`3465` → `1.20.1`）。认不出来返回空串。
QString gameVersionOfDataVersion(int dataVersion);

/// 游戏版本号（`1.20.1`，可以从 `1.20.1-Forge_47.2.0` 这种串里抠出来）→ 数据版本。
/// 认不出来返回 0。
int dataVersionOfGameVersion(const QString &gameVersion);

/// 版本兼容提示（空串 = 没有要说的话）。两边都知道、而且不一样时才出一句
/// 「可能有方块兼容问题」—— 不一样**不等于**一定坏，措辞里不许把它说死。
QString compatibilityText(const Info &info, const QString &targetGameVersion);

/// 界面那一行（元数据 + 方块统计 + 版本提示，提示那一句另起一行）。
QString summaryText(const Info &info, const QString &targetGameVersion);

/// 悬停提示里的方块排行（"最多的是哪几种"）。空串 = 没有统计。
/// @p language 是 `loadBlockNames()` 的结果；查不到当前语言名称的方块显示 ID。
QString topBlocksText(const Info &info, const QJsonObject &language = {});

/// 从现有游戏资源查当前界面语言的语言文件；没有本地资源时返回空表，不下载。
/// @p language 是资源里的语言代号（`zh_cn` / `en_us`），只接受小写字母、数字和下划线。
QJsonObject loadBlockNames(const QString &assetsRoot, const QString &indexId,
                           const QString &language);

/// 解析一份语言 JSON（来自资源索引或版本 jar 的 `assets/minecraft/lang/*.json`）。
QJsonObject blockNamesFromJson(const QByteArray &json);

/// `loadBlockNames(..., "zh_cn")` 的旧入口。
QJsonObject loadChineseBlockNames(const QString &assetsRoot, const QString &indexId);
QString chineseBlockName(const QJsonObject &language, const QString &blockId);

}  // namespace SchematicInfo
