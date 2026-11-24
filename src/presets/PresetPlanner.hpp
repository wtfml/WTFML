/**
 * @file PresetPlanner.hpp
 * @brief 「把这一份预设落到那个实例上」这件事的**计划**那一层（
 * `PresetPlanner` 与「应用前可见精确计划」那张表）。
 *
 * 【这一层在干什么】
 *
 * 一份预设是**一串段**（`PresetSection`），每一段归一个适配器解释（`options.txt`、
 * `servers.dat` 和通用文件组、MaLiLib 那一族热键）。那张表是**一段一行**：
 *
 * ```text
 * 游戏设置 合并 18 项
 * 原版按键 合并 23 项，冲突 1 项
 * 服务器列表 追加 3 个
 * 配置文件 复制 8 个，覆盖 1 个
 * ```
 *
 * 所以这一层的活只有三件：**按段的种类分派**给对应适配器的计划函数、把五个适配器各自的
 * 报告**翻成同一张表**、以及把"哪些段这一趟做不了 / 哪些要用户先点头"标出来。
 *
 * 【为什么计划要单独走一趟，而不是"算完直接写"】
 *
 * 「所有写入先完成 ApplyPlan，再备份和原子替换」不是流程洁癖：用户在这张表上看
 * 见的每一行，都是**看得见后果**的一次改动。先给表、后动手，中间那一步才可能插进来
 * "确认"（`needsConfirm`）和"备份"（「覆盖前有回滚点」）。
 *
 * 【★ 这一层只读，一个字节都不写】
 *
 * 计划里会碰盘的只有两档的**干跑**：通用文件组那一档要真数一遍目标上缺哪几份
 * （`planFileGroup()`），MaLiLib 那一档要真读一遍 `config/<modId>.json` 才知道"这一趟会
 * 解绑几格"（`malilibReadTarget()`）—— 两趟都**只读、不写**。所以这一层是能拿着**任何**
 * 一份预设对**任何**一个实例算的：算完不满意就丢掉，盘上什么都没变。
 *
 * 【★ 「覆盖」要用户明确确认，这是**两趟**协议】
 *
 * 要求的「默认 Merge，Replace 必须明确确认」。落地就是这个形状：
 *
 * ```cpp
 * PlanRequest request{...}; // replaceConfirmed 默认 false
 * const ApplyPlan first = planPresetApplication(request);
 * if (first.needsConfirm()) { 问用户"这几段会覆盖，继续吗"… }
 * request.replaceConfirmed = true; // 用户点头之后
 * const ApplyPlan final = planPresetApplication(request);
 * ```
 *
 * 第一趟**照样把数给出来**（不然用户没法判断要不要点头：只告诉他"要确认"而不告诉他
 * "会拿掉 2 台服务器"，那个确认框等于没问）。所以这一层内部一律按"已确认"去算，
 * 再把"还没点头"标在 `needsConfirm` 上 —— 这是「生成并显示精确计划」的全部分量。
 *
 * 【★ 做不了的段不拦着别的段】
 *
 * 一份预设里可能有 `SectionKind::Custom` 那样**没有任何适配器接手**的段（用户自己塞进来的
 * 种类）。那种段落一行 `blocked`（理由写清楚，还带上盘上那个词），**其余照样算**。整份不认
 * 是 `PresetStore::loadManifest()` 那一层的事（读不回来的段在那儿就被挡住了），这一层拿到
 * 的已经是一份读得回来的预设。
 *
 * 【冲突报在哪一行】
 *
 * 那行「原版按键 合并 23 项，冲突 1 项」里的"冲突"是**按键抢同一个键位**（
 * 「检测标准按键的精确重复」）。MaLiLib 那一段报的是**同一件事**：两个热键抢
 * 同一组键位 —— 所以它进的是同一个 `conflictCount`。服务器那边的"同一个地址已经在列表里
 * 了"是 `skipped`（预设里那一条不动，目标上那条留着），通用文件组那边的"这一项只有复制
 * 和覆盖"是 `needsChoice`。三个数是三件事，别混着看。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "GenericFiles.hpp"
#include "MalilibKeys.hpp"
#include "PresetTypes.hpp"
#include "ServerListPreset.hpp"
#include "VanillaOptions.hpp"

namespace Presets {

// ---------------------------------------------------------------------------
// 输入：目标实例这一趟让我们看见的东西
// ---------------------------------------------------------------------------

/**
 * @brief 目标实例这一趟让我们看见的东西（**由调用方读好递进来**）。
 *
 * 这一层**不自己读用户的盘**：读盘那几口（`ServerListFile` / `QFile`）各自都有唯一实现，
 * 而"读的时候实例正在被游戏改"这件事得由调用方（它知道游戏在不在跑）来定夺。这一层
 * 拿到的是一份**快照**：算出来的计划对应的是快照那一瞬间的实例。
 *
 * 每一格都是"没有这一份"和"这一份是空的"分得开的那种形状（「区分缺失、`null` 与
 * 错误类型」）：`servers` 空 = 目标上一条服务器都没有（replace 那一档会拿掉 0 条），
 * 而 `serversProblem` 非空 = **那一份读不动**，是两件事。
 */
struct TargetSnapshot {
    /// 实例目录（`<数据目录>/versions/<id>`）。★ `CopyIfMissing` 那一档要的干跑得知道
    /// 目标在哪 —— 通用文件组那一段按它去数"目标上缺哪几份"。
    QString instanceRoot;

    /// `options.txt` 现在这一份（不在就是空的 `OptionsFile`，那是最正常的新实例）。
    OptionsFile options;

    /// `servers.dat` 现在这一份。
    QList<NbtUtil::ServerEntry> servers;
    /// 里面我们**认不出来**（没有 `ip` 那种）的条目数 —— 「覆盖」档不碰它们。
    int serversUntouchable = 0;
    /// 那一份读不动时的那句话（非空 ⇒ 服务器列表这一段这一趟做不了）。
    QString serversProblem;

    /// 这个实例的游戏版本有没有**数据包**（`VersionManager::instanceSupportsDatapacks()`）。
    /// 默认 true：验证工程 / 调用方不知道版本时按"有"算 —— 数据包从 17w43a 就有了，
    /// 把它默认成 false 会让绝大多数实例上的数据包段被无故判成做不了。
    bool supportsDataPacks = true;
};

// ---------------------------------------------------------------------------
// 输入：这一趟要算什么
// ---------------------------------------------------------------------------

/**
 * @brief 算这一趟计划要的输入。
 *
 * 段的 `mode` **在段自己身上**（`PresetSection::mode`）—— 那是用户在预设里选好的
 * "这一段怎么落"，不在这儿再给一遍（两处能给同一个东西，就会有两处说法）。
 */
struct PlanRequest {
    /// 预设的 `files/` 根（`PresetStore::filesDir(root, id)`）。★ 通用文件组那一段的干跑
    /// 要按它去数源那一侧有什么。预设里没有文件组那几段时留空也不要紧（那几段就不会用到
    /// 它），但只要有那样的段、这儿是空的，那一段就是 `blocked`（而不是悄悄当成"没有"）。
    QString presetFilesRoot;

    /// 这一份预设的段（`PresetManifest::sections`）。**次序 = 表上的次序**。
    QList<PresetSection> sections;

    /// 目标那一侧的快照。
    TargetSnapshot target;

    /// 用户已经明确确认过「覆盖」了（见文件头那个两趟协议）。
    bool replaceConfirmed = false;

    /// 那个实例现在**正在跑**。★ 真了的话这一趟**不许写**（`ApplyPlan::canApply()` 回
    /// false，理由在 `blockedReason` 里）：游戏自己会在退出时把 `options.txt` 整份重写一遍，
    /// 我们这一趟写进去的东西会被它盖掉（而用户看到的是"应用成功了，重启就没了"）。
    bool gameRunning = false;
};

// ---------------------------------------------------------------------------
// 输出：那张表
// ---------------------------------------------------------------------------

/**
 * @brief 计划表上的**一行**（= 预设里的一段）。
 *
 * 左栏（那一段叫什么）**不在这一层**：`title` 是段自己带的（可能空），空的时候界面按
 * `kind` 现翻一个中文名 —— 那是 `ui/` 那一层的活（见 `ui/` 与 `src/` 的分工）。
 * 这一层只给**右栏**（`applyPlanRowText()`）。
 */
struct ApplyPlanRow {
    QString sectionId;
    SectionKind kind = SectionKind::Custom;
    /// 段自己带的标题（可能空 ⇒ 界面按 `kind` 现翻）。
    QString title;
    ApplyMode mode = ApplyMode::Merge;

    /// 这一趟**真会动的项数**（按项算：`options.txt` 那两段数键、服务器数条目、
    /// 通用文件组数"项"）。★ 不含 `needsChoice` 那些 —— 它们这一趟一个字节都不动。
    int count = 0;
    /// 其中**被拿掉**的那些（「覆盖」档：认得出的键 / 对不上的服务器条目）。★ 单独一个数
    /// 是因为它和 `count` 回答的是不同的问题："换了几个" vs "删了几个"，而后者不可恢复。
    int removed = 0;
    /// 会和目标上已有的东西抢同一个键位的项数（今天只有 `options.txt` 那两段报得出）。
    int conflictCount = 0;
    /// 预设里有、这一趟不动的项数。
    int skipped = 0;
    /// 这一档我们**做不了主**、要用户选的项数（通用文件组那一段的 `Merge` / `Append`）。
    int needsChoice = 0;
    /**
     * @brief 目标上还没有对应世界、只能记为「待应用」的项数（数据包那一段）。
     *
     * 它和 `count` 是两件事：`count` 是"真落进目标世界里的"，这个是"先摆进
     * `<实例>/datapacks-pending/` 等那个世界建出来"。计划表上得分开说 —— "部署了 2 个"
     * 和"2 个在等世界"混成一句，用户会以为游戏里已经装上了。
     */
    int deferred = 0;

    bool blocked = false;
    /// 这一段是「覆盖」而用户还没明确确认过。
    bool needsConfirm = false;
    /// 一句话（`blocked` / `needsConfirm` 那两档是给人看的原文，**原样能显示**）。
    QString problem;

    /// 这一行值不值得让用户多看一眼（做不了 / 要确认 / 会删东西 / 有冲突 / 要人拿主意 /
    /// 有只能等世界的东西）。
    bool risky() const {
        return blocked || needsConfirm || removed > 0 || conflictCount > 0 || needsChoice > 0 ||
               deferred > 0;
    }
};

/**
 * @brief 整份计划（段落表 + 几道总闸）。
 */
struct ApplyPlan {
    /// 算得出来吗。`false` 只有一条来路：没有指定目标实例（别的毛病都落在某一行上）。
    bool ok = false;
    QString problem;

    QList<ApplyPlanRow> rows;

    /// 这一趟**不许开始**的理由（游戏在跑那一条）。空 = 没有拦路虎。
    /// 和 `blocked` 那一行不是一回事：那些是"这一段做不了"，这一条是"整趟别动"。
    QString blockedReason;

    /**
     * @brief 这一趟**之后**整份键位里互相抢的那些（同一件事）。
     *
     * 为什么整份还要单独来一张：一段 = 一个模组 = 一份文件，所以**行上**那个
     * `conflictCount` 只看得见"同一份文件里两格绑了同一组键位"。用户最需要看见的那一种
     * 抢法是**两个模组各占一个**（`GenericHotkeys.xxx` 和 `litematica.Hotkeys.yyy` 都绑
     * `M,C`）—— 那一种只有在把各段算完**之后**的格子攒到一处才算得出来。
     * 两张单子问的是两个问题（"这一段自己有没有问题" / "整份有没有问题"），别去对加法：
     * 同一份文件里那两格在**这一张**里也在（它们确实在抢）—— 这是对的，不是算重了。
     * 只有 MaLiLib 那一族进这张单子（原版按键那一档的冲突留在它自己那一行的数里）。
     */
    QList<MalilibConflict> keyConflicts;

    bool isEmpty() const { return rows.isEmpty(); }
    int totalCount() const;
    int deferredCount() const;
    int riskyRowCount() const;
    int blockedRowCount() const;
    /// 有没有哪一行在等用户点头（「覆盖」那几段）。
    bool needsConfirm() const;
    /// 这一趟一个字节都不写吗（全是 `Skip` / `needsChoice` / 做不了的段）。
    /// 「待应用」那些也要算：它们会真往 `datapacks-pending/` 和清单里写。
    bool writesAnything() const { return totalCount() > 0 || deferredCount() > 0; }
    /// 可以开始动手了吗：算得出来 ∧ 没有拦路虎 ∧ 该点头的都点过了。
    bool canApply() const { return ok && blockedReason.isEmpty() && !needsConfirm(); }
};

// ---------------------------------------------------------------------------
// 算
// ---------------------------------------------------------------------------

/// 那一段右边的动作词（`ApplyMode` 那一档的话）。**唯一出处** —— 表上的字和报告里的字
/// 是同一份，别处不许再写一遍。
QString applyPlanVerb(ApplyMode mode);

/// 那一行的**量词**：「项」还是「个」（两行分别用"个"和"项"）。**唯一出处**。
QString applyPlanUnit(SectionKind kind);

/**
 * @brief 那一行的右栏（表上那半截）。
 *
 * 规则（都写在实现里，这里只列形状）：
 * · `blocked` ⇒ 「做不了」
 * · `needsChoice > 0` ⇒ 「待选择 4 个」（这一档一个字节都不动，别的数没有意义）
 * · 会动的数是 0 ⇒ 「跳过 4 个」/「不动」（有跳过就报跳过，否则就是不动）
 * · 其余 ⇒ 「合并 18 项」，后面按需接「，删掉 2 项」「，冲突 1 项」「，跳过 3 项」
 * · 「覆盖」但一个都不写（空段铺上去 = 把那一片清空）⇒ 「覆盖，删掉 12 项」
 */
QString applyPlanRowText(const ApplyPlanRow &row);

/**
 * @brief 算这一趟的计划（**只读**，见文件头）。
 *
 * 按段的种类分派给对应适配器：`optionsHandlesSection()` / `serverListHandlesSection()` /
 * `genericFilesHandlesSection()` / `malilibHandlesSection()` 四张表是**分派的唯一出处**
 * （那四口的语义就是"这一段归不归我管"）。都不认的种类落 `blocked`（不是"悄悄跳过"）。
 */
ApplyPlan planPresetApplication(const PlanRequest &request);

}  // namespace Presets
