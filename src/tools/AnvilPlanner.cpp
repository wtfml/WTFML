/**
 * @file AnvilPlanner.cpp
 * @brief AnvilPlanner 的实现（这套规矩的出处与"不做什么"在 .hpp 开头那一段）。
 */
#include "AnvilPlanner.hpp"
#include <QCoreApplication>

#include <QSet>

#include <algorithm>
#include <cstring>
#include <map>

namespace {

using namespace Anvil;

/// 这张表核对的两处来源：
/// - 倍率（`anvil_cost`）与上限：1.21.10 客户端 `data/minecraft/enchantment` 那一批 json；
/// - 互斥那一列：同一份数据里的 `exclusive_set` 标签（`data/minecraft/tags/enchantment`
///   下面的 `exclusive_set` 目录）—— 同一条标签里的附魔**两两**互斥，等价于
///   `Enchantment.areCompatible()`。
///
/// NOTE: 互斥只按标签展开，不把"没能附在同一类物品上"当互斥：那是 `canEnchant` 那一档，
/// 这一层不管（见 .hpp 开头"不做什么"）。
/// NOTE: 1.21 的伤害那一组是**六条**（锋利 / 亡灵杀手 / 节肢杀手 / 穿刺 / 密度 / 破甲），
/// 不是早期那三条；`loyalty` 的 `anvil_cost` 是 2（书那一档减半后仍是 1）。
struct TableEntry {
    const char *id;
    const char *name;
    const char *english;
    int maxLevel;
    int itemMultiplier;
    int bookMultiplier;
    const char *incompatible;  ///< 逗号分开的 id 表；空串 = 没有
};

const TableEntry kTable[] = {
    {"protection", QT_TRANSLATE_NOOP("AnvilPlanner", "保护"), "Protection", 4, 1, 1,
     "fire_protection,blast_protection,projectile_protection"},
    {"fire_protection", QT_TRANSLATE_NOOP("AnvilPlanner", "火焰保护"), "Fire Protection", 4, 2, 1,
     "protection,blast_protection,projectile_protection"},
    {"feather_falling", QT_TRANSLATE_NOOP("AnvilPlanner", "摔落缓冲"), "Feather Falling", 4, 2, 1, ""},
    {"blast_protection", QT_TRANSLATE_NOOP("AnvilPlanner", "爆炸保护"), "Blast Protection", 4, 4, 2,
     "protection,fire_protection,projectile_protection"},
    {"projectile_protection", QT_TRANSLATE_NOOP("AnvilPlanner", "弹射物保护"), "Projectile Protection", 4, 2, 1,
     "protection,fire_protection,blast_protection"},
    {"thorns", QT_TRANSLATE_NOOP("AnvilPlanner", "荆棘"), "Thorns", 3, 8, 4, ""},
    {"respiration", QT_TRANSLATE_NOOP("AnvilPlanner", "水下呼吸"), "Respiration", 3, 4, 2, ""},
    {"depth_strider", QT_TRANSLATE_NOOP("AnvilPlanner", "深海探索者"), "Depth Strider", 3, 4, 2, "frost_walker"},
    {"aqua_affinity", QT_TRANSLATE_NOOP("AnvilPlanner", "水下速掘"), "Aqua Affinity", 1, 4, 2, ""},
    {"sharpness", QT_TRANSLATE_NOOP("AnvilPlanner", "锋利"), "Sharpness", 5, 1, 1,
     "smite,bane_of_arthropods,impaling,density,breach"},
    {"smite", QT_TRANSLATE_NOOP("AnvilPlanner", "亡灵杀手"), "Smite", 5, 2, 1,
     "sharpness,bane_of_arthropods,impaling,density,breach"},
    {"bane_of_arthropods", QT_TRANSLATE_NOOP("AnvilPlanner", "节肢杀手"), "Bane of Arthropods", 5, 2, 1,
     "sharpness,smite,impaling,density,breach"},
    {"knockback", QT_TRANSLATE_NOOP("AnvilPlanner", "击退"), "Knockback", 2, 2, 1, ""},
    {"fire_aspect", QT_TRANSLATE_NOOP("AnvilPlanner", "火焰附加"), "Fire Aspect", 2, 4, 2, ""},
    {"looting", QT_TRANSLATE_NOOP("AnvilPlanner", "抢夺"), "Looting", 3, 4, 2, ""},
    {"sweeping_edge", QT_TRANSLATE_NOOP("AnvilPlanner", "横扫之刃"), "Sweeping Edge", 3, 4, 2, ""},
    {"efficiency", QT_TRANSLATE_NOOP("AnvilPlanner", "效率"), "Efficiency", 5, 1, 1, ""},
    {"silk_touch", QT_TRANSLATE_NOOP("AnvilPlanner", "精准采集"), "Silk Touch", 1, 8, 4, "fortune"},
    {"unbreaking", QT_TRANSLATE_NOOP("AnvilPlanner", "耐久"), "Unbreaking", 3, 2, 1, ""},
    {"fortune", QT_TRANSLATE_NOOP("AnvilPlanner", "时运"), "Fortune", 3, 4, 2, "silk_touch"},
    {"power", QT_TRANSLATE_NOOP("AnvilPlanner", "力量"), "Power", 5, 1, 1, ""},
    {"punch", QT_TRANSLATE_NOOP("AnvilPlanner", "冲击"), "Punch", 2, 4, 2, ""},
    {"flame", QT_TRANSLATE_NOOP("AnvilPlanner", "火矢"), "Flame", 1, 4, 2, ""},
    {"infinity", QT_TRANSLATE_NOOP("AnvilPlanner", "无限"), "Infinity", 1, 8, 4, "mending"},
    {"luck_of_the_sea", QT_TRANSLATE_NOOP("AnvilPlanner", "海之眷顾"), "Luck of the Sea", 3, 4, 2, ""},
    {"lure", QT_TRANSLATE_NOOP("AnvilPlanner", "饵钓"), "Lure", 3, 4, 2, ""},
    {"frost_walker", QT_TRANSLATE_NOOP("AnvilPlanner", "冰霜行者"), "Frost Walker", 2, 4, 2, "depth_strider"},
    {"mending", QT_TRANSLATE_NOOP("AnvilPlanner", "经验修补"), "Mending", 1, 4, 2, "infinity"},
    {"binding_curse", QT_TRANSLATE_NOOP("AnvilPlanner", "绑定诅咒"), "Curse of Binding", 1, 8, 4, ""},
    {"vanishing_curse", QT_TRANSLATE_NOOP("AnvilPlanner", "消失诅咒"), "Curse of Vanishing", 1, 8, 4, ""},
    {"impaling", QT_TRANSLATE_NOOP("AnvilPlanner", "穿刺"), "Impaling", 5, 4, 2,
     "sharpness,smite,bane_of_arthropods,density,breach"},
    {"riptide", QT_TRANSLATE_NOOP("AnvilPlanner", "激流"), "Riptide", 3, 4, 2, "loyalty,channeling"},
    {"loyalty", QT_TRANSLATE_NOOP("AnvilPlanner", "忠诚"), "Loyalty", 3, 2, 1, "riptide"},
    {"channeling", QT_TRANSLATE_NOOP("AnvilPlanner", "引雷"), "Channeling", 1, 8, 4, "riptide"},
    {"multishot", QT_TRANSLATE_NOOP("AnvilPlanner", "多重射击"), "Multishot", 1, 4, 2, "piercing"},
    {"piercing", QT_TRANSLATE_NOOP("AnvilPlanner", "穿透"), "Piercing", 4, 1, 1, "multishot"},
    {"quick_charge", QT_TRANSLATE_NOOP("AnvilPlanner", "快速装填"), "Quick Charge", 3, 2, 1, ""},
    {"soul_speed", QT_TRANSLATE_NOOP("AnvilPlanner", "灵魂疾行"), "Soul Speed", 3, 8, 4, ""},
    {"swift_sneak", QT_TRANSLATE_NOOP("AnvilPlanner", "迅捷潜行"), "Swift Sneak", 3, 8, 4, ""},
    {"wind_burst", QT_TRANSLATE_NOOP("AnvilPlanner", "风爆"), "Wind Burst", 3, 4, 2, ""},
    {"density", QT_TRANSLATE_NOOP("AnvilPlanner", "致密"), "Density", 5, 2, 1,
     "sharpness,smite,bane_of_arthropods,impaling,breach"},
    {"breach", QT_TRANSLATE_NOOP("AnvilPlanner", "破甲"), "Breach", 4, 4, 2,
     "sharpness,smite,bane_of_arthropods,impaling,density"},
};

QList<EnchantInfo> buildTable() {
    QList<EnchantInfo> table;
    for (const TableEntry &entry : kTable) {
        EnchantInfo info;
        info.id = QString::fromLatin1(entry.id);
        info.name = QCoreApplication::translate("AnvilPlanner", entry.name);
        info.english = QString::fromLatin1(entry.english);
        info.maxLevel = entry.maxLevel;
        info.itemMultiplier = entry.itemMultiplier;
        info.bookMultiplier = entry.bookMultiplier;
        const QString incompatible = QString::fromLatin1(entry.incompatible);
        if (!incompatible.isEmpty()) {
            info.incompatible = incompatible.split(QChar(','), Qt::SkipEmptyParts);
        }
        table.append(info);
    }
    return table;
}

/// 这一条附魔的倍率（附魔书按书的那一列）。
int multiplierOf(const EnchantInfo *info, bool isBook) {
    if (info == nullptr) {
        // 表里没有的那一条：**别把整件事停下**（用户可能报了一个更新版本才有的附魔）。
        // 按最便宜那一档算，并在计划里由 plan() 事先拦掉——见那边的前置校验。
        return 1;
    }
    return isBook ? info->bookMultiplier : info->itemMultiplier;
}

/// 这一对附魔互斥吗（查表那一列）。
bool incompatiblePair(const QString &left, const QString &right) {
    const EnchantInfo *info = findEnchantment(left);
    return info != nullptr && info->incompatible.contains(right);
}

/// 两张附魔表合并（左边留下、右边被吃）：代价、结果、不兼容的条数。
struct MergeMaps {
    int value = 0;
    int incompatible = 0;
    QMap<QString, int> result;
    QMap<QString, int> dropped;
};

MergeMaps mergeMaps(const QMap<QString, int> &target, const QMap<QString, int> &sacrifice, bool sacrificeIsBook) {
    MergeMaps merged;
    merged.result = target;
    for (auto it = sacrifice.constBegin(); it != sacrifice.constEnd(); ++it) {
        const QString id = it.key();
        const int level = it.value();
        // 目标上有一条和它互斥的 ⇒ 目标保住自己那条，代价 +1（Java 那一档）。
        // NOTE: 比的是**已经合出来的那一份**（`merged.result`），不是最初的目标：铁砧那边
        // 一条一条塞进去，塞进去的那条立即参与后面几条的互斥判断（`AnvilMenu.createResult`
        // 里那个 `itemenchantments$mutable` 是边走边长的）。所以一件牺牲品上带着两条互斥的
        // 附魔时，只有先轮到的能用上，后一条 +1 被顶掉 —— 不是两条都合上来。
        // NOTE: 谁先轮到在这张表里按 id 的字典序（`QMap`），游戏里按附魔注册表的次序；
        // 这种"一本书上带着两条互斥"的输入本来就不是正常玩法能做出的，两种次序都可能不同。
        bool conflict = false;
        for (auto existing = merged.result.constBegin(); existing != merged.result.constEnd(); ++existing) {
            if (incompatiblePair(existing.key(), id)) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            ++merged.incompatible;
            merged.dropped.insert(id, level);
            continue;
        }
        const EnchantInfo *info = findEnchantment(id);
        const int existingLevel = target.value(id, 0);
        int level_ = level;
        if (existingLevel == level) {
            // 同级 ⇒ +1（这就是"两本锋利 III 合出锋利 IV"那条规矩）。
            ++level_;
        } else {
            level_ = qMax(existingLevel, level);
        }
        if (info != nullptr) {
            level_ = qMin(level_, info->maxLevel);
        }
        merged.result.insert(id, level_);
        merged.value += multiplierOf(info, sacrificeIsBook) * level_;
    }
    return merged;
}

/// "这一件上还没有步骤"那种状态的最小步长（叶子）。比任何真实代价都大。
constexpr int kNoStep = 0x3fffffff;

/// 计划搜索里的一个状态：手上那一件（前科 + 总代价 + 带着什么）+ 它是从哪两步来的。
struct State {
    int cost = 0;
    int penalty = 0;
    /// 走到这一件为止，**最大那一步**花了多少级（判"生存里做不做得出来"用它，
    /// 不看累计 —— 铁砧限制的是每一次，不是总和）。
    int maxStep = 0;
    /// 这一件是不是附魔书（决定它当牺牲品时按哪一列倍率算；合并出来的那件跟着左边那件）。
    bool isBook = true;
    /// 走到这一件为止**最小那一步**的代价（判"改名那 1 级能不能塞进某一步"用它：
    /// 改名只能加在某一步上，那一步原本就得 ≤ 38，加完才不超 39）。叶子上没有步骤，
    /// 给一个足够大的哨兵值。
    int minStep = kNoStep;
    QMap<QString, int> enchantments;
    /// 来源：子集 A、子集 B、以及"谁左谁右"（true = A 当牺牲品）。
    int leftMask = 0;
    int rightMask = 0;
    int leftState = -1;
    int rightState = -1;
    bool leftIsSacrifice = false;
    /// 这一次合并的账（重建步骤时要显示）。
    MergeCost merge;
    int node = -1;  ///< 这一件在第几步被合出来（叶子的 -1 表示"输入里那一件"）
};

}  // namespace

namespace Anvil {

const QList<EnchantInfo> &enchantmentTable() {
    static const QList<EnchantInfo> table = buildTable();
    return table;
}

QString normalizeId(const QString &id) {
    QString name = id.trimmed().toLower();
    if (name.startsWith(QLatin1String("minecraft:"))) {
        name = name.mid(int(std::strlen("minecraft:")));
    }
    return name;
}

const EnchantInfo *findEnchantment(const QString &id) {
    const QString name = normalizeId(id);
    for (const EnchantInfo &info : enchantmentTable()) {
        if (info.id == name) {
            return &info;
        }
    }
    return nullptr;
}

QString enchantmentName(const QString &id) {
    const EnchantInfo *info = findEnchantment(id);
    return info != nullptr ? info->name : normalizeId(id);
}

int penaltyForUses(int uses) {
    if (uses <= 0) {
        return 0;
    }
    if (uses >= 31) {
        // 2^31 会溢出 int；31 次以上早就超过铁砧那条 39 级的上限了，给一个够大的数就行。
        return 0x40000000;
    }
    return (1 << uses) - 1;
}

int usesForPenalty(int penalty) {
    if (penalty < 0) {
        return -1;
    }
    for (int uses = 0; uses < 31; ++uses) {
        if (penaltyForUses(uses) == penalty) {
            return uses;
        }
    }
    return -1;
}

MergeCost mergeCost(const Unit &target, const Unit &sacrifice) {
    MergeCost cost;
    const MergeMaps merged = mergeMaps(target.enchantments, sacrifice.enchantments, sacrifice.isBook);
    cost.enchantValue = merged.value;
    cost.incompatible = merged.incompatible;
    cost.dropped = merged.dropped;
    cost.penalties = target.penalty + sacrifice.penalty;
    cost.total = cost.enchantValue + cost.penalties + cost.incompatible;
    // 39 级是铁砧那条上限（wiki："beyond that, repairs are refused"）—— 代价**超过 39**
    // 就拒绝，所以判据是 `> 39` 而不是 `>= 39`。
    cost.tooExpensive = cost.total > 39;
    cost.result = merged.result;
    cost.resultPenalty = 2 * qMax(target.penalty, sacrifice.penalty) + 1;
    return cost;
}

Plan plan(const QList<Unit> &units, bool includeRename) {
    Plan plan;
    if (units.isEmpty()) {
        plan.error = QCoreApplication::translate("AnvilPlanner", "尚未添加任何物品，请先添加。");
        return plan;
    }
    if (units.size() > kMaxUnits) {
        plan.error = QCoreApplication::translate("AnvilPlanner", "最多支持计算 %1 件物品。").arg(kMaxUnits);
        return plan;
    }
    // ---------- 先把表里没有的那几条拦下来（宁可不算，也不给一条基于猜测的计划）----------
    QStringList unknown;
    for (const Unit &unit : units) {
        for (auto it = unit.enchantments.constBegin(); it != unit.enchantments.constEnd(); ++it) {
            if (findEnchantment(it.key()) == nullptr && !unknown.contains(normalizeId(it.key()))) {
                unknown.append(normalizeId(it.key()));
            }
        }
    }
    if (!unknown.isEmpty()) {
        plan.error = QCoreApplication::translate("AnvilPlanner", "这几条附魔不在表里，算不了：%1。").arg(unknown.join(QCoreApplication::translate("AnvilPlanner", "、")));
        return plan;
    }

    const int total = 1 << units.size();
    /// 这一趟有没有因为每层额度到顶而丢掉候选（见 `Plan::approximate`）。
    bool approximate = false;
    // 每个子集的候选状态（**帕累托**：代价和前科都不比它差的那个留着，其余丢掉）。
    // NOTE: 这里必须写成 `static_cast<size_t>(…)` 而不是 `size_t(total)` —— 后者会被当成
    // 一个名叫 `total` 的形参声明（最刁钻的解析），`best` 就变成函数声明了。
    std::vector<QList<State>> best(static_cast<size_t>(total));

    for (int index = 0; index < units.size(); ++index) {
        State state;
        state.cost = 0;
        state.penalty = units.at(index).penalty;
        state.isBook = units.at(index).isBook;
        state.enchantments = units.at(index).enchantments;
        best[size_t(1 << index)].append(state);
    }

    /// 剪枝：状态太多的话只留代价最低的那些（前科小的也留几个，免得把"以后更省"的那条剪掉）。
    constexpr int kMaxStatesPerMask = 12;

    for (int mask = 1; mask < total; ++mask) {
        if (__builtin_popcount(unsigned(mask)) < 2) {
            continue;
        }
        QList<State> &bucket = best[size_t(mask)];
        for (int left = (mask - 1) & mask; left > 0; left = (left - 1) & mask) {
            const int right = mask ^ left;
            if (right == 0) {
                continue;
            }
            // 每一对 (left, right) 只算一次（`left < right` 那一半）。
            if (left < right) {
                continue;
            }
            for (int li = 0; li < best[size_t(left)].size(); ++li) {
                for (int ri = 0; ri < best[size_t(right)].size(); ++ri) {
                    const State &leftState = best[size_t(left)].at(li);
                    const State &rightState = best[size_t(right)].at(ri);
                    // 左右不能对调着看（附魔价值只算牺牲品那一边），所以两种摆法都试。
                    for (int swapped = 0; swapped < 2; ++swapped) {
                        const State &targetState = swapped == 0 ? leftState : rightState;
                        const State &sacrificeState = swapped == 0 ? rightState : leftState;
                        Unit target;
                        target.enchantments = targetState.enchantments;
                        target.penalty = targetState.penalty;
                        Unit sacrifice;
                        sacrifice.enchantments = sacrificeState.enchantments;
                        sacrifice.penalty = sacrificeState.penalty;
                        // 附魔书的倍率那一列：牺牲品是**书**才按书算，是装备就按物品那一列。
                        // 合并出来的那一件跟着**左边**（留下）那一件 —— 铁砧的结果就是把
                        // 左边那件改一改，所以拿一件装备当底子合到最后还是那件装备。
                        sacrifice.isBook = sacrificeState.isBook;

                        const MergeCost merge = mergeCost(target, sacrifice);
                        // NOTE: 改名那 1 级**不塞进这里的代价**：铁砧那边 `AnvilMenu.
                        // createResult()` 里只有名字框填了个新名字才 `i += 1`，而且只加在
                        // 一次操作上。计划重建完再挑"最便宜的那一步"加（见 `renameStep`）。
                        // 能这么挑的前提是那一步原本 ≤ 38（加 1 才不超 39），所以这一层
                        // 要跟着记**最小那一步**。
                        State state;
                        state.cost = targetState.cost + sacrificeState.cost + merge.total;
                        state.penalty = merge.resultPenalty;
                        state.isBook = targetState.isBook;
                        state.maxStep = qMax(targetState.maxStep,
                                             qMax(sacrificeState.maxStep, merge.total));
                        state.minStep = qMin(targetState.minStep,
                                             qMin(sacrificeState.minStep, merge.total));
                        state.enchantments = merge.result;
                        state.leftMask = left;
                        state.rightMask = right;
                        state.leftState = li;
                        state.rightState = ri;
                        state.leftIsSacrifice = swapped != 0;
                        state.merge = merge;
                        bucket.append(state);
                    }
                }
            }
        }

        // ---------- 剪枝 ----------
        std::sort(bucket.begin(), bucket.end(), [](const State &a, const State &b) {
            if (a.cost != b.cost) {
                return a.cost < b.cost;
            }
            if (a.maxStep != b.maxStep) {
                return a.maxStep < b.maxStep;
            }
            return a.penalty < b.penalty;
        });
        QList<State> kept;
        bool cappedHere = false;
        for (int candidateIndex = 0; candidateIndex < bucket.size(); ++candidateIndex) {
            const State &candidate = bucket.at(candidateIndex);
            bool dominated = false;
            for (const State &keptState : kept) {
                // 要算改名时 `minStep` 越小越好（越小越有地方塞改名那 1 级），所以它也得
                // "不比候选差"；不算改名时它跟目标无关，别让它削弱剪枝。
                const bool minStepOk = !includeRename || keptState.minStep <= candidate.minStep;
                if (minStepOk && keptState.cost <= candidate.cost
                    && keptState.penalty <= candidate.penalty
                    && keptState.maxStep <= candidate.maxStep
                    && keptState.isBook == candidate.isBook
                    && keptState.enchantments == candidate.enchantments) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) {
                kept.append(candidate);
            }
            if (kept.size() >= kMaxStatesPerMask) {
                // 额度用完了、后面还有候选没看：这一层**丢过**没被支配的状态 ⇒ 结果只是
                // 近似最优。额度本身是为了"点一下别等几分钟"（见 Plan::approximate）。
                cappedHere = candidateIndex + 1 < bucket.size();
                break;
            }
        }
        approximate = approximate || cappedHere;
        bucket = kept;
    }
    plan.approximate = approximate;

    const QList<State> &finalStates = best[size_t(total - 1)];
    if (finalStates.isEmpty()) {
        plan.error = QCoreApplication::translate("AnvilPlanner", "无法生成合并方案。请检查附魔是否互斥。");
        return plan;
    }

    // ---------- 选哪一条 ----------
    // "合完是哪一件"要跟着用户抱来的那堆东西走：全是书 ⇒ 合出来还是书；里面有一件
    // 已经附过魔的装备 ⇒ 合出来必须是**那件装备**。不钉这一条的话，两件东西里"把装备
    // 当牺牲品喂给书"往往更便宜，算出来的计划就变成"把装备烧了换一本书"，不是用户要的。
    bool wantBook = true;
    for (const Unit &unit : units) {
        if (!unit.isBook) {
            wantBook = false;
            break;
        }
    }
    /// 改名那 1 级加在哪儿（`AnvilMenu.createResult()` 那条路）：
    /// - 塞进某一步合的时候 —— 只要那一步原本 ≤ 38（加 1 不超 39，而铁砧是 `>= 40` 才
    ///   拒绝）⇒ 一共多 1 级；
    /// - 否则合完之后**单独**改一次 —— 代价 = 这一件的前科 + 1、封顶 39（`k == i` 那一支
    ///   把 `cost >= 40` 压回 39，而且前科不变：单改名不算"在铁砧上用过一次"），所以这条
    ///   路永远不会被铁砧拒绝。
    const auto renameExtra = [includeRename](const State &state) {
        if (!includeRename) {
            return 0;
        }
        return state.minStep <= 38 ? 1 : qMin(state.penalty + 1, 39);
    };
    /// 同样地看"能不能做出来"：累计小但中间某一步超了 39 的那条在生存里做不出来。
    auto better = [&renameExtra, includeRename](const State &candidate, const State &chosen) {
        const bool candidateFeasible = candidate.maxStep <= 39;
        const bool chosenFeasible = chosen.maxStep <= 39;
        if (candidateFeasible != chosenFeasible) {
            return candidateFeasible;
        }
        const int candidateCost = candidate.cost + renameExtra(candidate);
        const int chosenCost = chosen.cost + renameExtra(chosen);
        if (candidateCost != chosenCost) {
            return candidateCost < chosenCost;
        }
        if (candidate.penalty != chosen.penalty) {
            return candidate.penalty < chosen.penalty;
        }
        // 要算改名、又一样便宜时，优先"能塞进某一步"的那条：少开一次铁砧。
        return includeRename && candidate.minStep <= 38 && chosen.minStep > 38;
    };

    // 目标附魔 = 全部用上时、按"合完还是那一件"这条规矩的最优结果（用户列了哪些书，
    // 就奔着那套结果去）。
    int targetMask = total - 1;
    int targetIndex = -1;
    for (int index = 0; index < finalStates.size(); ++index) {
        if (finalStates.at(index).isBook != wantBook) {
            continue;
        }
        if (targetIndex < 0 || better(finalStates.at(index), finalStates.at(targetIndex))) {
            targetIndex = index;
        }
    }
    if (targetIndex < 0) {  // 兜底：一套符合"合完还是那一件"的都没有，就照累计最小那条
        targetIndex = 0;
    }
    const QMap<QString, int> target = finalStates.at(targetIndex).enchantments;
    // 材料可以不全用：所以要在**所有子集**合出来的状态里找，不只是"全用上"那一档 ——
    // 合出来同一套附魔、少花几本的那条更好（剩下的那几本在 `unused` 里点名）。
    int chosenMask = targetMask;
    int chosenIndex = targetIndex;
    for (int mask = 1; mask < total; ++mask) {
        const QList<State> &bucket = best[size_t(mask)];
        for (int index = 0; index < bucket.size(); ++index) {
            const State &state = bucket.at(index);
            if (state.enchantments != target || state.isBook != wantBook) {
                continue;
            }
            const State &chosen = best[size_t(chosenMask)].at(chosenIndex);
            if (better(state, chosen)) {
                chosenMask = mask;
                chosenIndex = index;
            }
        }
    }
    const State *finalState = &best[size_t(chosenMask)].at(chosenIndex);

    // ---------- 把步骤重建出来 ----------
    struct Walk {
        const std::vector<QList<State>> *best = nullptr;
        const QList<Unit> *units = nullptr;
        QList<Step> steps;
        QList<int> consumed;
        int totalCost = 0;
        bool anyTooExpensive = false;

        /// 回一个"节点号"：叶子的节点号 = 输入下标，中间件的节点号 = 产出它的那一步在
        /// steps 里的序号（从输入个数开始数）。
        ///
        /// `identity`：这一件东西**是哪一件输入**（叶子下标）。中间件的那一件其实还是它左边
        /// 那一件（铁砧的结果就是把左边那件改一改，不是凭空多出来一件），所以它的身份要
        /// 一路传上来。`consumed` 收的是"被吃掉的那一件是**哪件输入**"—— 直接把
        /// `step.sacrificeIndex` 收进去的话，牺牲品是中间件的那一步就什么都不记（四件东西
        /// 按两两配对的次序合，最后一步被吃的是中间件 ⇒ 只会记下两件，而实际上你做掉了三件）。
        std::map<int, int> identity;

        int walk(int mask, int index) {
            const State &state = (*best)[size_t(mask)].at(index);
            if (state.leftState < 0) {
                // 叶子的"节点号"就是输入下标，而它的身份也就是它自己 —— 表里不用记。
                return __builtin_ctz(unsigned(mask));
            }
            const int leftNode = walk(state.leftMask, state.leftState);
            const int rightNode = walk(state.rightMask, state.rightState);
            Step step;
            const int targetNode = state.leftIsSacrifice ? rightNode : leftNode;
            const int sacrificeNode = state.leftIsSacrifice ? leftNode : rightNode;
            step.targetIndex = targetNode < units->size() ? targetNode : -1;
            step.sacrificeIndex = sacrificeNode < units->size() ? sacrificeNode : -1;
            step.targetNode = step.targetIndex < 0 ? targetNode : -1;
            step.sacrificeNode = step.sacrificeIndex < 0 ? sacrificeNode : -1;
            step.cost = state.merge;
            step.result = state.enchantments;
            step.resultIsBook = state.isBook;
            steps.append(step);
            const int node = units->size() + steps.size() - 1;
            // 中间件在表里、叶子不在表里（叶子的身份就是它的下标）。
            const int targetIdentity = identity.count(targetNode) > 0 ? identity.at(targetNode) : targetNode;
            const int sacrificeIdentity =
                identity.count(sacrificeNode) > 0 ? identity.at(sacrificeNode) : sacrificeNode;
            identity[node] = targetIdentity;  // 这一件还是左边那一件
            totalCost += state.merge.total;
            if (state.merge.tooExpensive) {
                anyTooExpensive = true;
            }
            consumed.append(sacrificeIdentity);
            return node;
        }
    };

    Walk walk;
    walk.best = &best;
    walk.units = &units;
    walk.walk(chosenMask, chosenIndex);

    plan.ok = true;
    plan.steps = walk.steps;
    plan.renameCost = renameExtra(*finalState);
    plan.totalCost = walk.totalCost + plan.renameCost;
    // 改名那 1 级落在**最便宜的那一步**上（那一步原本 ≤ 38 才塞得进去，所以挑最小的那步）。
    // 塞不进去（每一步都 39）就留到合完之后单独改一次 —— `renameStep` 留在 -1。
    if (includeRename && finalState->minStep <= 38) {
        int cheapest = -1;
        for (int index = 0; index < plan.steps.size(); ++index) {
            if (cheapest < 0
                || plan.steps.at(index).cost.total < plan.steps.at(cheapest).cost.total) {
                cheapest = index;
            }
        }
        if (cheapest >= 0) {
            plan.renameStep = cheapest;
            plan.steps[cheapest].renamedHere = true;
        }
    }
    plan.finalPenalty = finalState->penalty;
    plan.finalEnchantments = finalState->enchantments;
    plan.finalIsBook = finalState->isBook;
    plan.anyTooExpensive = walk.anyTooExpensive;
    plan.withinSurvival = !walk.anyTooExpensive;
    for (int index = 0; index < units.size(); ++index) {
        if ((chosenMask & (1 << index)) == 0) {
            plan.unused.append(index);
        }
    }
    plan.consumed = walk.consumed;
    return plan;
}

}  // namespace Anvil
